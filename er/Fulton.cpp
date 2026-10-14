#include "Fulton.h"

#include "Combat.h"
#include "Game.h"
#include "Host.h"
#include "Hud.h"
#include "Log.h"
#include "Config.h"
#include "Mem.h"
#include "Params.h"
#include "Puppet.h"
#include "RenderMarkers.h"

#include "../protocol/phantomring_protocol.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace phantom::er::Fulton
{
	namespace
	{
		constexpr float        kPickRadius = 3.0f;     // m from where the proxy stood (handle not found)
		constexpr std::uint32_t kHoldBits = 0x30;      // ChrIns+0x538: bit4 no attack, bit5 no move (the console's flags)
		constexpr std::uint8_t kNoGravity = 0x20;      // ChrIns+0x1C4 bit5 no_gravity

		struct Lifting
		{
			std::uint8_t* chr{ nullptr };
			std::uint64_t handle{ 0 };
			Vec3          start;      // feet when the balloon went up
			float         t{ 0 };
			std::int32_t  npc{ 0 };
		};
		std::vector<Lifting>       g_lifts;
		std::vector<std::uint64_t> g_gone;   // fultoned, until Elden Ring brings its enemies back
		std::uint64_t              g_events{ 0 }, g_failed{ 0 }, g_unknown{ 0 }, g_done{ 0 }, g_again{ 0 };
		bool                       g_wasLoading{ false };
		std::uint64_t              g_ticks{ 0 };
		params::Table              g_npc{ params::kNpcParam };
		std::uint64_t              g_runesGiven{ 0 };
		bool                       g_runesOff{ false };
		constexpr std::uint32_t    kPlayerGameData = 0x580;  // PlayerIns: PlayerGameData* (er/Progression.cpp, RTTI-checked there too)
		constexpr std::uint32_t    kPgdLevel = 0x68, kPgdRunes = 0x6C, kPgdRuneMemory = 0x70;  // u32 level, rune_count, rune_memory (fromsoftware-rs)

		// The runes it would have dropped (NpcParam getSoul) onto the Tarnished's count, as a pickup does.
		void GiveRunes(const Frame& a_f, std::int32_t a_npc)
		{
			if (!Cfg().fultonRunes || g_runesOff || !a_f.player) {
				return;
			}
			auto* row = a_npc > 0 ? g_npc.Row(static_cast<std::uint32_t>(a_npc)) : nullptr;
			const auto soul = row ? SafeRead<std::uint32_t>(row + params::kNpcGetSoul) : std::nullopt;
			auto*      pgd = SafePtr(a_f.player.chr + kPlayerGameData);
			const auto vptr = pgd ? SafeRead<void*>(pgd) : std::nullopt;
			if (!soul || !vptr || game::RttiNameOfVtable(*vptr) != "CS::PlayerGameData") {
				g_runesOff = true;
				Log::Warn("fulton: no runes for fultoned enemies this session ({})", !soul ? std::format("NpcParam {} not readable ({})", a_npc, g_npc.Describe())
				                                                                       : "PlayerIns+0x580 is not a CS::PlayerGameData");
				return;
			}
			const auto level = SafeRead<std::uint32_t>(pgd + kPgdLevel).value_or(0);
			const auto runes = SafeRead<std::uint32_t>(pgd + kPgdRunes);
			const auto memory = SafeRead<std::uint32_t>(pgd + kPgdRuneMemory);
			if (!runes || !memory || level == 0 || level > 713 || *runes > 999999999u || *memory < *runes) {
				g_runesOff = true;
				Log::Warn("fulton: no runes for fultoned enemies this session (PlayerGameData level {} runes {} memory {}: not what a save holds)", level,
					runes.value_or(0), memory.value_or(0));
				return;
			}
			const std::uint32_t add = std::min<std::uint32_t>(*soul, 999999999u - *runes);
			if (add == 0) {
				return;
			}
			SafeWrite<std::uint32_t>(pgd + kPgdRunes, *runes + add);
			SafeWrite<std::uint32_t>(pgd + kPgdRuneMemory, *memory + std::min<std::uint32_t>(add, 0xFFFFFFFFu - *memory));
			g_runesGiven += add;
			Log::Info("fulton: +{} runes for npc {} (its NpcParam getSoul): {} -> {}", add, a_npc, *runes, *runes + add);
		}

		bool Showing(const Frame& a_f)
		{
			return Puppet::Active() && a_f.guestReady && a_f.player && !a_f.menuOpen && !a_f.cutscene && !a_f.loading && !a_f.dead && !a_f.dying &&
			       !(a_f.guest.flags & proto::kGuestInMenu);
		}

		void Color(Marker& a_m, float a_r, float a_g, float a_b, float a_a)
		{
			a_m.color[0] = a_r;
			a_m.color[1] = a_g;
			a_m.color[2] = a_b;
			a_m.color[3] = a_a;
		}

		// MGSV's balloon: an olive-grey ball over him on a dark line to the harness on his back.
		void Draw(const Lifting& a_l, const Lift& a_at, std::vector<Marker>& a_out)
		{
			const Vec3  harness = a_l.start + Vec3{ 0.0f, 1.3f + a_at.rise, 0.0f };
			const float r = 0.12f + 0.43f * a_at.balloon;
			const Vec3  centre = harness + Vec3{ 0.0f, 0.9f + 1.7f * a_at.balloon + r, 0.0f };
			Marker      ball;
			ball.shape = Marker::Shape::kBall;
			ball.pos = centre;
			ball.radius = r;
			ball.slack = 0.3f;
			Color(ball, 0.42f, 0.40f, 0.31f, 1.0f);
			a_out.push_back(ball);
			const Vec3  bottom = centre - Vec3{ 0.0f, r, 0.0f };
			const float len = Length(bottom - harness);
			const int   n = std::clamp(static_cast<int>(len / 0.09f), 2, 60);
			for (int i = 0; i <= n; ++i) {
				Marker dot;
				dot.shape = Marker::Shape::kDot;
				dot.pos = harness + (bottom - harness) * (static_cast<float>(i) / static_cast<float>(n));
				dot.radius = 0.014f;
				dot.slack = 0.2f;
				Color(dot, 0.07f, 0.07f, 0.06f, 1.0f);
				a_out.push_back(dot);
			}
		}

		std::uint64_t HandleOf(const std::uint8_t* a_chr) { return SafeRead<std::uint64_t>(a_chr + game::kChrHandle).value_or(0); }

		bool SetDisabled(std::uint8_t* a_chr)
		{
			auto*      flags = a_chr + game::kChrDebugFlags;
			const auto v = SafeRead<std::uint32_t>(flags);
			return v && SafeWrite<std::uint32_t>(flags, *v | game::kChrDebugDisabled | kHoldBits);
		}

		void Finish(const Frame& a_f, Lifting& a_l, const char* a_how, bool a_reward)
		{
			if (HandleOf(a_l.chr) != a_l.handle) {
				Log::Info("fulton: handle {:#x} is no longer that character ({}) -- left alone", a_l.handle, a_how);
				return;
			}
			if (SetDisabled(a_l.chr)) {
				++g_done;
				if (std::find(g_gone.begin(), g_gone.end(), a_l.handle) == g_gone.end()) {
					g_gone.push_back(a_l.handle);
				}
				Log::Info("fulton: npc {} (handle {:#x}) is out of Elden Ring's world ({}; ChrIns+0x538 |= 0x1000 character_disabled: no death, no drop)",
					a_l.npc, a_l.handle, a_how);
				if (a_reward) {
					GiveRunes(a_f, a_l.npc);
				}
			} else {
				Log::Warn("fulton: npc {} (handle {:#x}): its debug flags could not be written -- it stays", a_l.npc, a_l.handle);
			}
		}

		void Start(const Frame&, const game::Character& a_c)
		{
			for (const auto& l : g_lifts) {
				if (l.handle == a_c.handle) {
					return;  // already going up
				}
			}
			Lifting l;
			l.chr = a_c.chr;
			l.handle = a_c.handle;
			l.start = a_c.pos;
			l.npc = a_c.npcParam;
			// Held for the lift: no move, no attack (its AI), no gravity (we carry it up).
			if (const auto v = SafeRead<std::uint32_t>(a_c.chr + game::kChrDebugFlags)) {
				SafeWrite<std::uint32_t>(a_c.chr + game::kChrDebugFlags, *v | kHoldBits);
			}
			if (const auto f = SafeRead<std::uint8_t>(a_c.chr + game::kChrFlags1C4)) {
				SafeWrite<std::uint8_t>(a_c.chr + game::kChrFlags1C4, static_cast<std::uint8_t>(*f | kNoGravity));
			}
			g_lifts.push_back(l);
			Log::Info("fulton: the balloon goes up on npc {} (handle {:#x}, hp {}/{}) at ({:.2f}, {:.2f}, {:.2f}), {:.1f} m from Snake", a_c.npcParam, a_c.handle,
				a_c.hp, a_c.maxHp, a_c.pos.x, a_c.pos.y, a_c.pos.z, a_c.distance);
			Hud::Notify("Fulton: the enemy is on its way to Mother Base", Hud::Tone::kGood);
		}
	}

	Lift LiftAt(float a_t)
	{
		Lift l;
		if (a_t <= 0.0f) {
			return l;
		}
		const float s = std::clamp(a_t / 0.7f, 0.0f, 1.0f);
		l.balloon = s * s * (3.0f - 2.0f * s);
		if (a_t < 0.7f) {
			l.rise = 0.25f * s * s;
		} else if (a_t < 1.3f) {
			l.rise = 0.25f + (a_t - 0.7f) / 0.6f;
		} else {
			const float u = a_t - 1.3f;
			l.rise = 1.25f + 2.0f * u + 20.0f * u * u;
		}
		l.done = a_t >= kSeconds;
		return l;
	}

	int Pick(std::span<const Candidate> a_chrs, std::uint64_t a_handle, const Vec3& a_at, float a_radius)
	{
		if (a_handle) {
			for (std::size_t i = 0; i < a_chrs.size(); ++i) {
				if (a_chrs[i].handle == a_handle && a_chrs[i].alive) {
					return static_cast<int>(i);
				}
			}
		}
		int   best = -1;
		float bestD = a_radius;
		for (std::size_t i = 0; i < a_chrs.size(); ++i) {
			if (!a_chrs[i].alive) {
				continue;
			}
			const float d = Length(a_chrs[i].pos - a_at);
			if (d <= bestD) {
				bestD = d;
				best = static_cast<int>(i);
			}
		}
		return best;
	}

	void OnEvent(const Frame& a_f, const proto::GuestEvent& a_e)
	{
		++g_events;
		const std::uint64_t handle = std::uint64_t(a_e.pad[0]) | (std::uint64_t(a_e.pad[1]) << 32);
		if (a_e.a == 0) {
			++g_failed;
			Log::Info("fulton: MGSV's balloon never went up for handle {:#x} (a = 0) -- nothing happens in Elden Ring", handle);
			return;
		}
		if (!Puppet::Active() || !a_f.player) {
			Log::Info("fulton: handle {:#x} -- Snake is not playing, ignored", handle);
			return;
		}
		const Vec3 at = Puppet::CurrentAnchor().PointToEr({ a_e.v0[0], a_e.v0[1], a_e.v0[2] });
		const auto chrs = game::Characters(128);
		std::vector<Candidate> cand;
		std::vector<std::size_t> index;
		for (std::size_t i = 0; i < chrs.size(); ++i) {
			const auto& c = chrs[i];
			if (c.chr == a_f.player.chr || c.isPlayer) {
				continue;  // never the Tarnished (or another player)
			}
			// Only an enemy of the Tarnished's (the team relation every hit uses): never a friend or a neutral (live: "nearest"
			// took an invisible team-0 character standing at the player's grace and the grace's prompt went with it).
			cand.push_back({ c.handle, c.pos, !c.dead && c.hp > 0 && Combat::HostileToPlayer(a_f, c.team) });
			index.push_back(i);
		}
		const int k = Pick(cand, handle, at, kPickRadius);
		if (k < 0) {
			++g_unknown;
			Log::Info("fulton: no living Elden Ring character with handle {:#x} or within {:.0f} m of ({:.2f}, {:.2f}, {:.2f}) -- nothing to take", handle,
				kPickRadius, at.x, at.y, at.z);
			return;
		}
		const auto& c = chrs[index[static_cast<std::size_t>(k)]];
		if (c.handle != handle) {
			Log::Info("fulton: handle {:#x} not found; the nearest character to where the proxy stood is npc {} (handle {:#x}, {:.1f} m)", handle, c.npcParam,
				c.handle, Length(c.pos - at));
		}
		Start(a_f, c);
	}

	void Tick(const Frame& a_f)
	{
		++g_ticks;
		if (a_f.loading && !g_wasLoading) {
			OnEnemiesBack("a load");
		}
		g_wasLoading = a_f.loading;
		std::vector<Marker> markers;
		if (!g_lifts.empty()) {
			const float dt = std::clamp(a_f.dt, 0.0f, 0.1f);
			const bool  show = Showing(a_f);
			for (auto& l : g_lifts) {
				l.t += dt;
				if (HandleOf(l.chr) != l.handle) {
					l.t = 1e9f;  // gone from under us (unloaded): dropped below
					continue;
				}
				const auto at = LiftAt(l.t);
				if (at.done) {
					Finish(a_f, l, "after its lift", true);
					continue;
				}
				if (const auto p = game::ChrAsPlayer(l.chr)) {
					game::SetPosition(p, l.start + Vec3{ 0.0f, at.rise, 0.0f });
				}
				if (show) {
					Draw(l, at, markers);
				}
			}
			std::erase_if(g_lifts, [](const Lifting& a_l) { return LiftAt(a_l.t).done; });
		}
		RenderMarkers::Set(RenderMarkers::Owner::kFulton, std::move(markers));
		// One that turned up again (its block reloaded) before Elden Ring brings its enemies back: out again.
		if (!g_gone.empty() && g_ticks % 30 == 0 && a_f.inGame && !a_f.loading) {
			for (const auto& c : game::Characters(256, true)) {
				if (!c.disabled && std::find(g_gone.begin(), g_gone.end(), c.handle) != g_gone.end() && SetDisabled(c.chr)) {
					++g_again;
					Log::Info("fulton: npc {} (handle {:#x}) turned up again (its block reloaded) -- out of the world again until a rest", c.npcParam, c.handle);
				}
			}
		}
	}

	void OnEnemiesBack(const char* a_why)
	{
		for (auto& l : g_lifts) {
			Finish(Host::Current(), l, a_why, false);  // cut short by a rest/load/death: out of the world, no runes
		}
		g_lifts.clear();
		if (!g_gone.empty()) {
			Log::Info("fulton: {} -- Elden Ring brings its enemies back; {} fultoned one(s) may return", a_why, g_gone.size());
			g_gone.clear();
		}
	}

	void OnWorldShift(const Vec3& a_delta)
	{
		for (auto& l : g_lifts) {
			l.start += a_delta;
		}
	}

	std::string Describe()
	{
		return std::format("fulton: {} events ({} the balloon never went up, {} no character found), {} lifting, {} taken out of the world, {} gone until "
		                   "a rest ({} re-disabled after turning up again), {} runes given{}",
			g_events, g_failed, g_unknown, g_lifts.size(), g_done, g_gone.size(), g_again, g_runesGiven,
			!Cfg().fultonRunes ? " ([Fulton] runes = false)" : g_runesOff ? " (runes off this session: see the log)" : "");
	}

	std::string Command(const Frame& a_f, std::string_view a_args)
	{
		while (!a_args.empty() && a_args.front() == ' ') {
			a_args.remove_prefix(1);
		}
		if (a_args.empty() || a_args == "status") {
			return Describe();
		}
		if (!Puppet::Active() || !a_f.player) {
			return "fulton: Snake is not playing";
		}
		const bool    fail = a_args.find("fail") != std::string_view::npos;
		std::uint64_t handle = 0;
		Vec3          pos;
		if (a_args.starts_with("0x")) {
			handle = std::strtoull(std::string(a_args).c_str(), nullptr, 16);
			for (const auto& c : game::Characters(128)) {
				if (c.handle == handle) {
					pos = c.pos;
				}
			}
		} else if (a_args.starts_with("forget")) {
			OnEnemiesBack("the console");
			return Describe();
		} else if (a_args.starts_with("nearest")) {
			for (const auto& c : game::Characters(128)) {
				if (c.chr != a_f.player.chr && !c.isPlayer && Combat::HostileToPlayer(a_f, c.team) && !c.dead && c.hp > 0) {
					handle = c.handle;
					pos = c.pos;
					break;  // by distance
				}
			}
			if (!handle) {
				return "fulton: no living hostile character near";
			}
		} else {
			return "fulton [status | nearest [fail] | 0x<handle> [fail] | forget]";
		}
		proto::GuestEvent e{};
		e.type = proto::kGEvFulton;
		e.a = fail ? 0 : 1;
		e.pad[0] = static_cast<std::uint32_t>(handle);
		e.pad[1] = static_cast<std::uint32_t>(handle >> 32);
		const Vec3 m = Puppet::CurrentAnchor().PointToMgsv(pos);
		e.v0[0] = m.x;
		e.v0[1] = m.y;
		e.v0[2] = m.z;
		OnEvent(a_f, e);
		return std::format("kGEvFulton a={} handle {:#x} -- {}", e.a, handle, Describe());
	}
}
