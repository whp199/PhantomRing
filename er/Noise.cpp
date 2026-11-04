#include "Noise.h"

#include "Addresses.h"
#include "Calibration.h"
#include "Combat.h"
#include "CombatLogic.h"
#include "Config.h"
#include "Cqc.h"
#include "Game.h"
#include "Hud.h"
#include "Link.h"
#include "Log.h"
#include "Mem.h"
#include "NoiseLogic.h"
#include "Params.h"
#include "Puppet.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <unordered_map>

#include <windows.h>

namespace phantom::er::Noise
{
	namespace
	{
		// The request builder (0x348760) and the system's add (0x34A1B0), read in the 2.7.1.0 Ghidra project (docs/HOST.md "Noise"):
		//   request +0x00 position (xyzw, 16-byte aligned), +0x10 the owner {u32 kind (0 = a character); u64 its handle; u64 -1},
		//   +0x28..+0x3B the owner's tail {i32 -1, i32 0, f32 1, f32 0, i32 0}, +0x40 the attack {i32 -1, i32 0}, +0x48 the
		//   AiSoundParam id, +0x4C the radius scale (radius = row radius x it), +0x50 / +0x54 the SpEffect rate / add (rows with
		//   bSpEffectEnable only).
		using RequestFn = void* (*)(void* a_req, const void* a_pos, const void* a_owner, const void* a_atk, const void* a_id);
		using AddFn = void (*)(void* a_system, void* a_req);
		using BoolGetter = bool (*)(void* a_owner);

		constexpr std::uint32_t kSoundSystem = 0x7628;  // CSWorldAiManager -> the sound system (AddAiSound's cmp [rcx+0x7628],0)
		constexpr std::uint32_t kReqOwnerHandle = 0x18;
		constexpr std::uint32_t kReqId = 0x48;
		constexpr std::uint32_t kReqScale = 0x4C;
		// An NPC's AI (0x1402BDA40 builds it, 0xF0D0 bytes): +0xC938 its CSAiTargetingSystemOwner (+0x10 the ChrIns), +0xC950 its
		// CSTargetingSystem* (0x480 bytes: +0x10 the CSTargetSearchSys, whose +0x90 holds {i32 the last AI sound id it heard, i8 its
		// rank} -- 0x140342D80 writes it, fromsoftware-rs `latest_ai_sound_id`; +0x10.. its 14 search slots, 2 = the sound).
		constexpr std::uint32_t kAiOwner = 0xC938;
		constexpr std::uint32_t kAiTargeting = 0xC950;
		constexpr std::uint32_t kOwnerChr = 0x10;
		constexpr std::uint32_t kTsSearch = 0x10;
		constexpr std::uint32_t kTsSlots = 0x20;
		constexpr std::uint32_t kTsLastSound = 0xA0;
		constexpr std::uint32_t kOwnerBattle = 0x70;     // vtable slot 14: is_battle_state (fromsoftware-rs CSTargetingSystemOwnerVmt)
		constexpr std::uint32_t kOwnerNoSearch = 0x78;   // slot 15: is_not_in_any_search_state

		params::Table      g_sounds{ params::kAiSoundParam };
		std::string        g_modeOverride;  // console: "" = ini
		noise::SuppressorWatch g_watch;
		std::uint64_t      g_emitted{ 0 }, g_quiet{ 0 }, g_failed{ 0 }, g_heard{ 0 }, g_worn{ 0 }, g_dropped{ 0 }, g_impacts{ 0 };
		bool               g_warned{ false };
		bool               g_broken{ false };  // the builder didn't fill the request as expected: never again this session
		std::string        g_last;
		double             g_tokens{ 0 };
		ULONGLONG          g_tokensAt{ 0 };
		// Where an NPC's AI hangs off its ChrIns (found by `noise who`, then kept): ChrIns + comOff -> the manipulator, + aiOff -> the AI.
		std::int32_t       g_comOff{ -1 }, g_aiOff{ -1 };
		bool               g_gettersWarned{ false };
		std::uint32_t      g_attachSeen{ 0 };
		bool               g_wasReady{ false };
		// (review 0.7.0) mode heard: a shot's token hits wait until the round's own hit has left Elden Ring's patched row (they used to
		// take it first: the real hit then lost its sleep or poise), at most kTokensPerShot foes each, and a foe once per kHeardAgainMs.
		struct HeardJob
		{
			Vec3          pos;
			float         radius{ 0 };
			std::uint64_t skip{ 0 };  // the shot's own target (its hit is Elden Ring's aggro already)
			ULONGLONG     at{ 0 };
			std::string   what;
		};
		std::deque<HeardJob>                         g_heardJobs;
		std::unordered_map<std::uint64_t, ULONGLONG> g_heardAt;  // handle -> its last token hit
		constexpr std::size_t kTokensPerShot = 4;
		constexpr std::size_t kMaxHeardJobs = 4;
		constexpr ULONGLONG   kHeardAgainMs = 10000;
		constexpr ULONGLONG   kHeardJobMaxAgeMs = 1500;
		std::uint64_t         g_heardSkipped{ 0 }, g_heardLate{ 0 };

		const Addresses& addr() { return Addr(); }

		std::string_view Mode() { return g_modeOverride.empty() ? std::string_view(Cfg().noise.mode) : std::string_view(g_modeOverride); }

		noise::Radii Radii() { return Cfg().noise.radii; }

		bool Allow()
		{
			// A token bucket: automatic fire makes one sound every few rounds (Elden Ring merges a sound of the same row nearby
			// into the last one anyway: 0x140349030).
			const auto  now = ::GetTickCount64();
			const float rate = std::max(0.5f, Cfg().noise.perSecond);
			if (!g_tokensAt) {
				g_tokens = rate;
			} else {
				g_tokens = std::min<double>(rate, g_tokens + static_cast<double>(now - g_tokensAt) / 1000.0 * rate);
			}
			g_tokensAt = now;
			if (g_tokens < 1.0) {
				++g_dropped;
				return false;
			}
			g_tokens -= 1.0;
			return true;
		}

		bool Usable(std::string* a_why)
		{
			const auto& a = addr();
			const char* why = nullptr;
			if (g_broken) {
				why = "the request builder returned an unexpected request (disabled for the session)";
			} else if (!a.HasAll(A::WorldAiManager, A::AiSoundEmit, A::AiSoundAdd, A::AiSoundRequest)) {
				why = "the AI sound addresses are not verified in this build";
			} else if (!params::Available()) {
				why = "the regulation params are not available";
			}
			if (why && a_why) {
				*a_why = why;
			}
			return why == nullptr;
		}

		// Enemies within a_radius that aren't fighting yet get a token hit: no damage, no flinch, Elden Ring's aggro on the Tarnished.
		std::size_t Heard(const Frame& a_f, const Vec3& a_pos, float a_radius, std::string_view a_what, std::uint64_t a_skip = 0);

		struct AiState
		{
			bool          found{ false };
			bool          stateKnown{ false };  // battle / searching come from the verified getters
			bool          battle{ false };
			bool          searching{ false };
			std::int32_t  lastSound{ -1 };
			std::int32_t  lastRank{ -1 };
			int           slots{ 0 };  // search slots holding a target
			bool          soundSlot{ false };
		};

		std::uint8_t* AiOf(std::uint8_t* a_chr)
		{
			const auto& a = addr();
			if (!a.HasAll(A::VtTargetingSystem, A::VtAiTargetingOwner)) {
				return nullptr;
			}
			auto proves = [&](std::uint8_t* a_ai) {
				return a_ai && HasVtable(a_ai + kAiOwner, a[A::VtAiTargetingOwner]) && SafePtr(a_ai + kAiOwner + kOwnerChr) == a_chr &&
				       HasVtable(SafePtr(a_ai + kAiTargeting), a[A::VtTargetingSystem]);
			};
			if (g_comOff >= 0) {
				auto* com = SafePtr(a_chr + g_comOff);
				auto* ai = com ? SafePtr(com + g_aiOff) : nullptr;
				return proves(ai) ? ai : nullptr;
			}
			// Find it once: EnemyIns' manipulators sit after its ChrIns (fromsoftware-rs: com_manipulator +0x580); the AI is one
			// of the first pointers in the right one, proven by its owner (the ChrIns) and the targeting system's vtable.
			for (std::int32_t comOff = 0x560; comOff <= 0x5A0; comOff += 8) {
				auto* com = SafePtr(a_chr + comOff);
				if (!com) {
					continue;
				}
				for (std::int32_t aiOff = 0; aiOff < 0x200; aiOff += 8) {
					auto* ai = SafePtr(com + aiOff);
					if (proves(ai)) {
						g_comOff = comOff;
						g_aiOff = aiOff;
						Log::Info("noise: an NPC's AI is ChrIns +{:#x} -> +{:#x} (its targeting owner +{:#x} names the ChrIns, its targeting system +{:#x})",
							comOff, aiOff, kAiOwner, kAiTargeting);
						return ai;
					}
				}
			}
			return nullptr;
		}

		AiState ReadAi(std::uint8_t* a_chr)
		{
			AiState s;
			auto*   ai = AiOf(a_chr);
			if (!ai) {
				return s;
			}
			auto* ts = SafePtr(ai + kAiTargeting);
			if (!ts || !HasVtable(ts + kTsSearch, addr()[A::VtTargetSearchSys])) {
				return s;
			}
			s.found = true;
			s.lastSound = SafeRead<std::int32_t>(ts + kTsLastSound).value_or(-1);
			s.lastRank = SafeRead<std::int8_t>(ts + kTsLastSound + 4).value_or(-1);
			for (int i = 0; i < 14; ++i) {
				if (SafePtr(ts + kTsSlots + 8 * i)) {
					++s.slots;
					s.soundSlot |= i == 2;
				}
			}
			// The owner's own getters (const reads of its AI's state; the listener's code calls them every update). Called only when its
			// vtable slots are the byte-verified functions of this build (review 0.7.0: they were called unverified).
			const auto& a = addr();
			auto*       owner = ai + kAiOwner;
			auto*       vt = SafePtr(owner);
			auto*       battle = vt ? SafePtr(vt + kOwnerBattle) : nullptr;
			auto*       noSearch = vt ? SafePtr(vt + kOwnerNoSearch) : nullptr;
			const bool  verified = a.HasAll(A::AiOwnerIsBattle, A::AiOwnerNoSearch);
			if (verified && battle && battle == reinterpret_cast<std::uint8_t*>(a[A::AiOwnerIsBattle]) && noSearch &&
				noSearch == reinterpret_cast<std::uint8_t*>(a[A::AiOwnerNoSearch])) {
				s.stateKnown = true;
				s.battle = reinterpret_cast<BoolGetter>(battle)(owner);
				s.searching = !reinterpret_cast<BoolGetter>(noSearch)(owner);
			} else if (!g_gettersWarned) {
				g_gettersWarned = true;
				Log::Warn("noise: an AI's battle / search state can't be read ({}) -- left unknown (mode heard then hears each foe once per {} s)",
					!verified ? "CSAiTargetingSystemOwner's getters are not verified in this build"
							  : std::format("its vtable slots {} / {} are not the verified getters", static_cast<void*>(battle), static_cast<void*>(noSearch)),
					kHeardAgainMs / 1000);
			}
			return s;
		}

		std::size_t Heard(const Frame& a_f, const Vec3& a_pos, float a_radius, std::string_view a_what, std::uint64_t a_skip)
		{
			std::size_t n = 0;
			combat::HitEffect e;
			e.poise = 0.01f;  // something to fly with: correction 0 (sleep_correction), dmgLevel 0 -- no damage, no flinch
			e.dmgLevel = 0;
			e.quiet = true;
			const auto now = ::GetTickCount64();
			std::erase_if(g_heardAt, [&](const auto& kv) { return now - kv.second >= kHeardAgainMs; });
			auto foes = Combat::HostilesNear(a_f, a_pos, a_radius);
			std::ranges::sort(foes, [&](const auto& x, const auto& y) { return Length((x.cap.a + x.cap.b) * 0.5f - a_pos) < Length((y.cap.a + y.cap.b) * 0.5f - a_pos); });
			for (const auto& foe : foes) {
				if (n >= kTokensPerShot) {
					break;
				}
				// Not the shot's own target, nobody asleep (an Elden Ring hit wakes him: a darted soldier would wake for every shot near
				// him), nobody CQC holds, nobody heard a moment ago (ReadAi can't always prove the battle state), nobody fighting already.
				if (foe.handle == a_skip || Combat::Asleep(foe.chr) || Cqc::IsHeld(foe.handle) || g_heardAt.contains(foe.handle)) {
					++g_heardSkipped;
					continue;
				}
				if (const auto s = ReadAi(foe.chr); s.stateKnown && s.battle) {
					continue;  // already after Snake
				}
				const Vec3 centre = (foe.cap.a + foe.cap.b) * 0.5f;
				if (Combat::HitCharacter(a_f, foe.handle, centre, foe.cap.r + 2.0f, e, std::format("noise ({}) heard", a_what))) {
					g_heardAt[foe.handle] = now;
					++n;
				}
			}
			return n;
		}
	}

	bool Emit(const Frame& a_f, const Vec3& a_pos, float a_radius, std::string_view a_what)
	{
		std::string why;
		if (!Usable(&why) || !a_f.player || !Finite(a_pos) || !(a_radius > 0.0f)) {
			if (!why.empty() && !g_warned) {
				g_warned = true;
				Log::Warn("noise: Elden Ring's AI sound can't be made ({}) -- [Noise] mode = heard is the fallback", why);
			}
			++g_failed;
			return false;
		}
		const auto& a = addr();
		const auto& c = Cfg().noise;
		auto*       manager = a.Instance(A::WorldAiManager);
		auto*       system = manager ? SafePtr(manager + kSoundSystem) : nullptr;
		auto*       row = g_sounds.Row(static_cast<std::uint32_t>(c.soundRow));
		const float rowRadius = row ? SafeRead<float>(row).value_or(0.0f) : 0.0f;
		const float scale = noise::ScaleFor(a_radius, rowRadius);
		if (!system || scale <= 0.0f) {
			if (!g_warned) {
				g_warned = true;
				Log::Warn("noise: no AI sound ({}) -- CSWorldAiManager {}, its sound system {}, AiSoundParam {} {} radius {:.1f} ({})",
					a_what, static_cast<void*>(manager), static_cast<void*>(system), c.soundRow, static_cast<void*>(row), rowRadius, g_sounds.Describe());
			}
			++g_failed;
			return false;
		}
		const auto handle = SafeRead<std::uint64_t>(a_f.player.chr + game::kChrHandle).value_or(~0ull);
		alignas(16) float        pos[4]{ a_pos.x, a_pos.y, a_pos.z, 1.0f };
		alignas(16) std::uint8_t owner[0x40]{};  // 0x140348CE0 + 0x140348D50: a character's owner record
		std::uint32_t            kind = 0;
		const std::uint64_t      none = ~0ull;
		const std::int32_t       minusOne = -1;
		const float              one = 1.0f;
		std::memcpy(owner + 0x00, &kind, 4);
		std::memcpy(owner + 0x08, &handle, 8);
		std::memcpy(owner + 0x10, &none, 8);
		std::memcpy(owner + 0x18, &minusOne, 4);
		std::memcpy(owner + 0x20, &one, 4);
		alignas(8) const std::int32_t atk[2]{ -1, 0 };  // 0x140348C80: no attack
		const std::int32_t           id = c.soundRow;
		alignas(16) std::uint8_t     req[0x80]{};
		a.Fn<RequestFn>(A::AiSoundRequest)(req, pos, owner, atk, &id);
		std::uint64_t reqHandle = 0;
		std::int32_t  reqId = 0;
		std::memcpy(&reqHandle, req + kReqOwnerHandle, 8);
		std::memcpy(&reqId, req + kReqId, 4);
		if (reqHandle != handle || reqId != id) {
			g_broken = true;
			Log::Error("noise: Elden Ring's request builder made an unexpected request (owner {:#x} id {}; wanted {:#x} {}) -- AI sounds off for the session",
				reqHandle, reqId, handle, id);
			++g_failed;
			return false;
		}
		std::memcpy(req + kReqScale, &scale, 4);
		a.Fn<AddFn>(A::AiSoundAdd)(system, req);
		++g_emitted;
		g_last = std::format("{} -- {:.0f} m at ({:.1f}, {:.1f}, {:.1f})", a_what, a_radius, a_pos.x, a_pos.y, a_pos.z);
		if (g_emitted <= 20) {
			Log::Info("noise: AI sound {} m ({}; AiSoundParam {} x {:.2f}) at ({:.2f}, {:.2f}, {:.2f})", a_radius, a_what, id, scale, a_pos.x, a_pos.y,
				a_pos.z);
		} else {
			Log::Debug("noise: AI sound {} m ({}) at ({:.2f}, {:.2f}, {:.2f})", a_radius, a_what, a_pos.x, a_pos.y, a_pos.z);
		}
		return true;
	}

	void OnShot(const Frame& a_f, const Vec3& a_muzzle, const Vec3& a_impact, bool a_struckWorld, std::uint32_t a_bulletId, bool a_explosive, bool a_flame,
		bool a_suppressed, bool a_buddy, std::uint64_t a_target, std::string_view a_what)
	{
		if (!a_buddy) {
			g_watch.OnShot(a_suppressed, ::GetTickCount64());
		}
		const auto mode = Mode();
		if (mode == "off" || !Puppet::Active() || !a_f.player) {
			return;
		}
		const auto  cls = noise::ClassOfShot(a_bulletId, a_explosive, a_flame);
		const float radius = noise::RadiusOf(cls, a_suppressed, Radii());
		if (radius <= 0.0f) {
			++g_quiet;
			Log::Debug("noise: {} ({}{}) -- no sound", a_what, noise::Name(cls), a_suppressed ? ", suppressed" : "");
			return;
		}
		if (!Allow()) {
			return;
		}
		const std::string what = std::format("{} {}{}", a_buddy ? "the buddy's" : "Snake's", noise::Name(cls), a_suppressed ? " (suppressed)" : "");
		if (mode == "heard") {
			// After the round's own hit has left the patched row (Tick): it goes first.
			if (g_heardJobs.size() >= kMaxHeardJobs) {
				g_heardJobs.pop_front();
				++g_heardLate;
			}
			g_heardJobs.push_back({ a_muzzle, radius, a_target, ::GetTickCount64(), what });
			return;
		}
		Emit(a_f, a_muzzle, radius, what);
		// The round's own strike on the world (not on a character: his hit bullet makes Elden Ring's own hit sound), a few metres.
		if (a_struckWorld && !a_suppressed && !a_explosive && Cfg().noise.impact > 0.0f && Finite(a_impact) && Length(a_impact - a_muzzle) > Cfg().noise.impact) {
			if (Emit(a_f, a_impact, Cfg().noise.impact, what + " strikes")) {
				++g_impacts;
			}
		}
	}

	void Tick(const Frame& a_f)
	{
		// mode heard: one waiting shot's token hits, once the row is free (its own hit first).
		while (!g_heardJobs.empty()) {
			auto& job = g_heardJobs.front();
			if (::GetTickCount64() - job.at > kHeardJobMaxAgeMs || !Puppet::Active() || !a_f.player || a_f.loading) {
				++g_heardLate;
				g_heardJobs.pop_front();
				continue;
			}
			if (Combat::RowBusy()) {
				break;
			}
			const auto n = Heard(a_f, job.pos, job.radius, job.what, job.skip);
			g_heard += n;
			Log::Debug("noise: {} heard by {} (mode heard, {:.0f} m)", job.what, n, job.radius);
			g_heardJobs.pop_front();
			break;  // one shot's tokens per frame: they hold the row too
		}
		// The worn-out watch starts over with each new guest (a re-attach) or when it stops publishing.
		const auto attach = Link::Get().AttachCount();
		if (attach != g_attachSeen || (g_wasReady && !a_f.guestReady)) {
			g_attachSeen = attach;
			g_watch.Reset();
		}
		g_wasReady = a_f.guestReady;
		if (!a_f.guestReady) {
			return;
		}
		g_watch.windowMs = static_cast<std::uint64_t>(std::max(100, Cfg().noise.wornWindowMs));
		const bool on = (a_f.guest.flags & proto::kGuestSuppressorOn) != 0;
		constexpr std::uint32_t kNotWear = proto::kGuestInMenu | proto::kGuestClimbing | proto::kGuestMantling | proto::kGuestCqc | proto::kGuestInBox |
		                                   proto::kGuestOnVehicle | proto::kGuestDead | proto::kGuestLoading | proto::kGuestCutscene;
		const bool blocked = (a_f.guest.flags & kNotWear) != 0 || a_f.menuOpen || a_f.loading || a_f.cutscene || Puppet::Dying();
		if (g_watch.Update(on, a_f.guest.weaponId, blocked, ::GetTickCount64())) {
			++g_worn;
			Log::Info("noise: the suppressor on Snake's weapon {} wore out (kGuestSuppressorOn dropped right after a suppressed shot)", a_f.guest.weaponId);
			if (Cfg().noise.wornNote && Puppet::Active()) {
				Hud::Notify("Suppressor worn out", Hud::Tone::kWarn);
			}
		}
	}

	std::string Describe()
	{
		const auto& c = Cfg().noise;
		const auto& r = c.radii;
		std::string why;
		const bool  usable = Usable(&why);
		return std::format("noise: mode {} ({}) | radii pistol {:.0f} rifle {:.0f} sniper {:.0f} shotgun {:.0f} launcher {:.0f} flame {:.0f} suppressed {:.0f} "
		                   "impact {:.0f} m | AiSoundParam {} ({}) | {} AI sounds made ({} impacts), {} quiet shots, {} over {:.0f}/s, {} failed, {} heard "
		                   "(fallback; {} skipped: the shot's target, asleep, held or heard < {} s ago; {} shots' tokens dropped late) | suppressors worn out {} | last: {}",
			Mode(), usable ? "Elden Ring's AI sound ready" : why, r.pistol, r.rifle, r.sniper, r.shotgun, r.launcher, r.flame, r.suppressed, c.impact,
			c.soundRow, g_sounds.Describe(), g_emitted, g_impacts, g_quiet, g_dropped, c.perSecond, g_failed, g_heard, g_heardSkipped, kHeardAgainMs / 1000,
			g_heardLate, g_worn, g_last.empty() ? "-" : g_last);
	}

	std::string Console(const std::vector<std::string>& w, const Frame& a_f)
	{
		auto& c = Cfg().noise;
		auto  num = [&](std::size_t i, double d) { return i < w.size() ? std::strtod(w[i].c_str(), nullptr) : d; };
		if (w.size() < 2 || w[1] == "status") {
			return Describe();
		}
		if (w[1] == "mode" && w.size() > 2) {
			if (w[2] != "sound" && w[2] != "heard" && w[2] != "off" && w[2] != "ini") {
				return "noise mode sound|heard|off|ini";
			}
			g_modeOverride = w[2] == "ini" ? "" : w[2];
			return Describe();
		}
		if (w[1] == "set" && w.size() > 3) {
			const float v = static_cast<float>(num(3, 0));
			const auto& k = w[2];
			if (k == "pistol") c.radii.pistol = v;
			else if (k == "rifle") c.radii.rifle = v;
			else if (k == "sniper") c.radii.sniper = v;
			else if (k == "shotgun") c.radii.shotgun = v;
			else if (k == "launcher") c.radii.launcher = v;
			else if (k == "flame") c.radii.flame = v;
			else if (k == "suppressed") c.radii.suppressed = v;
			else if (k == "impact") c.impact = v;
			else if (k == "row") c.soundRow = static_cast<int>(v);
			else if (k == "per_second") c.perSecond = v;
			else return "noise set <pistol|rifle|sniper|shotgun|launcher|flame|suppressed|impact|row|per_second> <value>";
			return Describe();
		}
		if (!a_f.player) {
			return "no player";
		}
		const Vec3 me = game::Position(a_f.player);
		if (w[1] == "emit") {
			// An AI sound in front of the Tarnished (no MGSV): `noise emit [radius] [m ahead]`.
			const float r = static_cast<float>(num(2, 20));
			const float ahead = static_cast<float>(num(3, 0));
			const Vec3  fwd = Rotate(game::Orientation(a_f.player), { 0, 0, 1 }) * static_cast<float>(Calibration::ErForward());
			const Vec3  at = me + Vec3{ fwd.x, 0, fwd.z } * ahead + Vec3{ 0, 1.2f, 0 };
			if (Mode() == "heard") {
				return std::format("heard by {} (mode heard)", Heard(a_f, at, r, "console"));
			}
			return Emit(a_f, at, r, "console") ? std::format("AI sound {:.0f} m at ({:.1f}, {:.1f}, {:.1f})", r, at.x, at.y, at.z) : "no AI sound: " + Describe();
		}
		if (w[1] == "who") {
			const float r = static_cast<float>(num(2, 40));
			std::string out;
			int         n = 0;
			for (const auto& ch : game::Characters(64)) {
				if (ch.isPlayer || ch.chr == a_f.player.chr || ch.dead || Length(ch.pos - me) > r) {
					continue;
				}
				const auto s = ReadAi(ch.chr);
				const auto yaw = std::atan2(2.0f * (ch.orientation.w * ch.orientation.y + ch.orientation.x * ch.orientation.z),
					1.0f - 2.0f * (ch.orientation.y * ch.orientation.y + ch.orientation.z * ch.orientation.z));
				out += std::format("\n  npc {} {:#x} team {} {:.1f} m hp {}/{} yaw {:.0f} at ({:.1f}, {:.1f}, {:.1f}): {}", ch.npcParam, ch.handle, ch.team,
					Length(ch.pos - me), ch.hp, ch.maxHp, yaw * 180.0f / 3.14159265f, ch.pos.x, ch.pos.y, ch.pos.z,
					s.found ? std::format("{}{}, last AI sound {} (rank {}), {} search slots{}", !s.stateKnown ? "state unknown" : s.battle ? "BATTLE" : "calm",
								  s.searching ? " searching" : "",
								  s.lastSound, s.lastRank, s.slots, s.soundSlot ? " (sound target)" : "")
							: "no AI found");
				++n;
			}
			return std::format("{} characters within {:.0f} m:{}", n, r, out);
		}
		return "noise [status | emit [r] [ahead] | mode sound|heard|off|ini | set <key> <v> | who [r]]";
	}
}
