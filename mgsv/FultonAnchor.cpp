// Fulton of a sleeping Elden Ring enemy through an "anchor" soldier (FultonAnchor.h, docs/GUEST.md 20.4).
#include "FultonAnchor.h"

#include "ActorProxy.h"
#include "Config.h"
#include "Link.h"
#include "Log.h"
#include "State.h"
#include "phantomring_protocol.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>

namespace phantom::guest::fulton
{
	using logic::Vec3;

	namespace
	{
		enum class Phase
		{
			kIdle,
			kEnable,    // SetEnabled true sent; ChangeLifeState only takes a few updates later
			kSleep,     // asleep? settled where he lies?
			kPlace,     // Snake's placement beside him asked of the game thread
			kAnchored,  // MGSV offers its Fulton; watching him
			kLifted,    // MGSV's balloon took him: kGEvFulton sent; Snake's animation plays out
			kHome,      // Snake's placement back to the sandbox asked
		};
		const char* PhaseName(Phase a_p)
		{
			switch (a_p) {
			case Phase::kIdle:
				return "idle";
			case Phase::kEnable:
				return "waking a soldier";
			case Phase::kSleep:
				return "putting him to sleep";
			case Phase::kPlace:
				return "placing Snake";
			case Phase::kAnchored:
				return "anchored (MGSV's Fulton offered)";
			case Phase::kLifted:
				return "lifted";
			case Phase::kHome:
				return "going back to the sandbox";
			}
			return "?";
		}

		std::mutex                       g_lock;
		Phase                            g_phase{ Phase::kIdle };
		std::uint64_t                    g_since{ 0 };
		std::vector<plan::Candidate>     g_cands;
		std::uint64_t                    g_candsAt{ 0 };
		bool                             g_candsKnown{ false };
		std::vector<std::uint16_t>       g_spent;   // soldiers fultoned (gone) or that wouldn't sleep
		std::vector<std::uint64_t>       g_done;    // Elden Ring characters fultoned
		std::vector<proto::ActorRecord>  g_records;

		struct Anchor
		{
			std::uint16_t soldier{ 0xFFFF };
			std::uint64_t handle{ 0 };
			char          name[24]{};
			Vec3          stand;      // where he stood (his floor)
			Vec3          rest;       // where he lies asleep
			Vec3          spot;       // where Snake was placed
			Vec3          home;       // the sandbox centre to go back to
			std::uint64_t sleptAt{ 0 };
			std::uint64_t polledAt{ 0 };
			std::uint64_t awayAt{ 0 };  // the release condition first seen
			bool          fultoned{ false };
			int           liftSeen{ 0 };
		} g_a;

		std::optional<Placement> g_pending;
		int                      g_placeResult{ -1 };  // -1 waiting, 0 failed, 1 done
		Vec3                     g_lastShift;
		std::atomic<bool>        g_anchored{ false };
		std::mutex               g_exclLock;
		Vec3                     g_excl;

		std::uint32_t g_anchors{ 0 }, g_fultons{ 0 }, g_releases{ 0 }, g_failures{ 0 };
		std::string   g_last{ "nothing yet" };
		bool          g_noCandidateSaid{ false };

		void Enter(Phase a_p, std::uint64_t a_now)
		{
			g_phase = a_p;
			g_since = a_now;
		}

		void SetExclusion(const std::optional<Vec3>& a_at)
		{
			std::scoped_lock el(g_exclLock);
			if (a_at) {
				g_excl = *a_at;
			}
			g_anchored.store(a_at.has_value(), std::memory_order_release);
		}

		// "x,y,z,life,status,action" (PhantomRing.FultonSoldier); a field that isn't a number reads -1.
		bool ParseSoldier(const std::string& a_text, Vec3& a_pos, int& a_life, int* a_status = nullptr, int* a_action = nullptr)
		{
			char rest[64]{};
			if (std::sscanf(a_text.c_str(), "%f,%f,%f,%63s", &a_pos.x, &a_pos.y, &a_pos.z, rest) != 4 || !a_pos.Finite()) {
				return false;
			}
			int        v[3]{ -1, -1, -1 };
			const char* p = rest;
			for (int i = 0; i < 3 && *p; ++i) {
				v[i] = (*p >= '0' && *p <= '9') ? std::atoi(p) : -1;
				p = std::strchr(p, ',');
				if (!p) {
					break;
				}
				++p;
			}
			a_life = v[0];
			if (a_status) {
				*a_status = v[1];
			}
			if (a_action) {
				*a_action = v[2];
			}
			return true;
		}

		const proto::ActorRecord* Record(std::uint32_t a_count, std::uint64_t a_handle)
		{
			for (std::uint32_t i = 0; i < a_count; ++i) {
				if (g_records[i].handle == a_handle) {
					return &g_records[i];
				}
			}
			return nullptr;
		}

		// Back to the sandbox: the soldier disabled again (unless MGSV's balloon took him), Snake placed at the sandbox centre
		// with his offset from the spot kept (within the treadmill's radius), the centre restored.
		void GoHome(std::uint64_t a_now, const state::FrameView& a_fr, const std::function<std::string(const std::string&)>& a_call, const char* a_why)
		{
			if (!g_a.fultoned && g_a.soldier != 0xFFFF) {
				a_call(std::format("return PhantomRing.FultonDisable({})", g_a.soldier));
			}
			Vec3 off = a_fr.valid ? a_fr.root - g_a.spot : Vec3{};
			off.y = 0.0f;
			const float r = std::max(0.0f, Cfg().radius - 1.0f);
			if (off.LengthXZ() > r && off.LengthXZ() > 0.0f) {
				off = off * (r / off.LengthXZ());
			}
			g_pending = Placement{ g_a.home + off, g_a.home, a_why };
			g_placeResult = -1;
			SetExclusion(std::nullopt);
			Enter(Phase::kHome, a_now);
			g_last = std::format("{}: back to the sandbox", a_why);
			Log::Info("fulton: {} -- soldier {:#x} {}, Snake back to the sandbox", a_why, g_a.soldier, g_a.fultoned ? "gone with MGSV's balloon" : "disabled again");
		}
	}

	void Reset(const char* a_why)
	{
		std::scoped_lock lock(g_lock);
		if (g_phase != Phase::kIdle) {
			Log::Info("fulton: reset ({}) while {}", a_why, PhaseName(g_phase));
		}
		g_phase = Phase::kIdle;
		g_cands.clear();
		g_candsKnown = false;
		g_spent.clear();
		g_pending.reset();
		SetExclusion(std::nullopt);
	}

	void LuaStep(std::uint64_t a_nowMs, const std::function<std::string(const std::string&)>& a_call)
	{
		const auto& c = Cfg();
		if (!c.fultonAnchor || c.fultonMethod != "anchor" || (c.proxies && proxy::Active())) {
			return;  // [Fulton] Method = proxy, or ActorProxy runs: the sleeping enemy has a proxy soldier of his own (ActorProxyGame.cpp)
		}
		std::scoped_lock lock(g_lock);
		const auto fr = state::Frame();
		const auto lua = state::Lua();
		if (!fr.valid) {
			return;
		}
		const bool riding = lua.ridden != 0xFFFF || (lua.flags & state::kLVehicle);
		const bool busy = riding || (lua.flags & (state::kLDead | state::kLDemo)) || state::Parked() || !link::HostAlive();
		if (g_records.size() < proto::kMaxActors) {
			g_records.resize(proto::kMaxActors);
		}
		std::uint32_t n = 0, seq = 0;
		if (!link::ReadActors(g_records.data(), proto::kMaxActors, n, seq)) {
			n = 0;
		}
		const auto in = a_nowMs - g_since;

		switch (g_phase) {
		case Phase::kIdle: {
			if (busy || (lua.flags & state::kLMenu)) {
				return;
			}
			const int i = plan::PickSleeper(g_records.data(), n, fr.root, c.fultonReach, g_done);
			if (i < 0) {
				return;
			}
			const auto centre = state::Centre();
			if (!centre) {
				return;
			}
			if (!g_candsKnown || (g_cands.empty() && a_nowMs - g_candsAt > 30000)) {
				const auto text = a_call(std::format("return PhantomRing.FultonCandidates({}, {}, {}, {})", centre->x, centre->y, centre->z, c.fultonCandidates));
				g_cands = plan::ParseCandidates(text);
				g_candsKnown = true;
				g_candsAt = a_nowMs;
				Log::Info("fulton: {} anchor soldiers within {:.0f} m of the sandbox centre ({})", g_cands.size(), c.fultonCandidates, text.substr(0, 200));
			}
			const auto it = std::find_if(g_cands.begin(), g_cands.end(),
				[](const plan::Candidate& k) { return std::find(g_spent.begin(), g_spent.end(), k.id) == g_spent.end(); });
			if (it == g_cands.end()) {
				if (!g_noCandidateSaid) {
					g_noCandidateSaid = true;
					Log::Warn("fulton: a sleeping Elden Ring enemy is beside Snake, but no MGSV soldier is left to stand in for him");
				}
				return;
			}
			const auto& r = g_records[i];
			g_a = Anchor{};
			g_a.soldier = it->id;
			g_a.handle = r.handle;
			std::memcpy(g_a.name, r.name, sizeof(g_a.name) - 1);
			g_a.stand = it->pos;
			g_a.home = *centre;
			const auto ok = a_call(std::format("return PhantomRing.FultonEnable({})", g_a.soldier));
			Log::Info("fulton: Elden Ring character {:#x} '{}' asleep {:.1f} m from Snake -- waking soldier {:#x} (at {:.1f},{:.1f},{:.1f}) to stand in: {}",
				r.handle, g_a.name, (Vec3{ r.pos[0], r.pos[1], r.pos[2] } - fr.root).LengthXZ(), g_a.soldier, it->pos.x, it->pos.y, it->pos.z, ok);
			Enter(Phase::kEnable, a_nowMs);
			return;
		}
		case Phase::kEnable:
			if (in >= 300) {
				a_call(std::format("return PhantomRing.FultonSleep({})", g_a.soldier));
				g_a.sleptAt = a_nowMs;
				Enter(Phase::kSleep, a_nowMs);
			}
			return;
		case Phase::kSleep: {
			const auto* r = Record(n, g_a.handle);
			if (!r || !(r->flags & proto::kActorAsleep) || busy) {
				// Snake never left the sandbox: just the soldier back.
				a_call(std::format("return PhantomRing.FultonDisable({})", g_a.soldier));
				g_last = std::format("stopped before placing Snake: {}", !r ? "the enemy is gone" : busy ? "Snake is busy" : "the enemy woke up");
				Log::Info("fulton: {} -- soldier {:#x} disabled again", g_last, g_a.soldier);
				Enter(Phase::kIdle, a_nowMs);
				return;
			}
			if (a_nowMs - g_a.polledAt < 250) {
				return;
			}
			g_a.polledAt = a_nowMs;
			Vec3 pos;
			int  life = -1;
			const auto text = a_call(std::format("return PhantomRing.FultonSoldier({})", g_a.soldier));
			if (!ParseSoldier(text, pos, life)) {
				life = -1;
			}
			if (life != 3) {
				if (in > 1500 && a_nowMs - g_a.sleptAt > 700) {
					a_call(std::format("return PhantomRing.FultonSleep({})", g_a.soldier));  // once more
					g_a.sleptAt = a_nowMs;
				}
				if (in > 5000) {
					++g_failures;
					g_spent.push_back(g_a.soldier);
					a_call(std::format("return PhantomRing.FultonDisable({})", g_a.soldier));
					g_last = std::format("soldier {:#x} wouldn't sleep ({})", g_a.soldier, text);
					Log::Warn("fulton: soldier {:#x} didn't fall asleep ({}) -- trying another", g_a.soldier, text);
					Enter(Phase::kIdle, a_nowMs);
				}
				return;
			}
			// He falls, and TppSoldier2's position cache follows him slowly (live: 1.1 m in 30 s once): wait until two reads 250 ms
			// apart agree within 5 cm (at most 6 s).
			const bool settled = (pos - g_a.rest).Length() < 0.05f;
			g_a.rest = pos;
			if (in < 1500 || (!settled && in < 6000)) {
				return;
			}
			const Vec3 enemy{ r->pos[0], r->pos[1], r->pos[2] };
			g_a.spot = plan::SnakeSpot(fr.root, enemy, g_a.rest, g_a.stand.y);
			g_pending = Placement{ g_a.spot, g_a.spot, "beside the sleeping soldier" };
			g_placeResult = -1;
			SetExclusion(g_a.rest);  // from the placement's frame on (live: 20 extra 'Snake' draws for a frame or two when set after it)
			Enter(Phase::kPlace, a_nowMs);
			return;
		}
		case Phase::kPlace:
			if (g_placeResult < 0) {
				if (in > 3000) {
					g_pending.reset();
					SetExclusion(std::nullopt);
					++g_failures;
					a_call(std::format("return PhantomRing.FultonDisable({})", g_a.soldier));
					g_last = "Snake's placement never ran";
					Log::Warn("fulton: Snake's placement wasn't done within 3 s -- given up");
					Enter(Phase::kIdle, a_nowMs);
				}
				return;
			}
			if (g_placeResult == 0) {
				SetExclusion(std::nullopt);
				++g_failures;
				a_call(std::format("return PhantomRing.FultonDisable({})", g_a.soldier));
				g_last = "Snake's placement failed";
				Log::Warn("fulton: placing Snake beside the soldier failed (native placement unavailable?)");
				Enter(Phase::kIdle, a_nowMs);
				return;
			}
			++g_anchors;
			g_a.polledAt = 0;
			g_a.awayAt = 0;
			g_last = std::format("anchored: soldier {:#x} for '{}' {:#x}", g_a.soldier, g_a.name, g_a.handle);
			Log::Info("fulton: Snake placed at {:.2f},{:.2f},{:.2f} (shift {:.1f} m): soldier {:#x} asleep at {:.2f},{:.2f},{:.2f} where Elden Ring's '{}' lies -- "
			          "MGSV offers its Fulton (hold Y)",
				g_a.spot.x, g_a.spot.y, g_a.spot.z, g_lastShift.LengthXZ(), g_a.soldier, g_a.rest.x, g_a.rest.y, g_a.rest.z, g_a.name);
			Enter(Phase::kAnchored, a_nowMs);
			return;
		case Phase::kAnchored: {
			if (a_nowMs - g_a.polledAt >= 150) {
				g_a.polledAt = a_nowMs;
				Vec3       pos;
				int        life = -1, st = -1, act = -1;
				const auto text = a_call(std::format("return PhantomRing.FultonSoldier({})", g_a.soldier));
				const bool parsed = ParseSoldier(text, pos, life, &st, &act);
				// Taken by MGSV's balloon: GetStatus 1 -> 0 and GetActionStatus 0 -> 1 at once (live), seen twice in a row; or he is
				// Rise metres above where he lay (the position cache only creeps after him).
				g_a.liftSeen = parsed && st == 0 && act == 1 ? g_a.liftSeen + 1 : 0;
				if (parsed && (g_a.liftSeen >= 2 || pos.y > g_a.rest.y + c.fultonRise)) {
					g_a.fultoned = true;
					g_done.push_back(g_a.handle);
					g_spent.push_back(g_a.soldier);
					++g_fultons;
					proxy::Fulton(g_a.handle, true, "MGSV's own Fulton on the anchor soldier");
					g_last = std::format("fultoned '{}' {:#x} (soldier {:#x} rose to y {:.1f})", g_a.name, g_a.handle, g_a.soldier, pos.y);
					Log::Info("fulton: MGSV's balloon took soldier {:#x} ({}; y {:.1f}, lay at {:.1f}) -- kGEvFulton for '{}' {:#x}", g_a.soldier, text, pos.y,
						g_a.rest.y, g_a.name, g_a.handle);
					SetExclusion(std::nullopt);
					Enter(Phase::kLifted, a_nowMs);
					return;
				}
				if (life >= 0 && life != 3 && life != 4 && a_nowMs - g_a.sleptAt > 1000) {
					a_call(std::format("return PhantomRing.FultonSleep({})", g_a.soldier));  // keep him asleep
					g_a.sleptAt = a_nowMs;
				}
				// The cache crept on after the placement (or Snake's CQC / a bump moved him): Snake follows, so the soldier stays where
				// the enemy lies relative to him (Elden Ring sees nothing: the odometer is rebased).
				const Vec3 drift{ pos.x - g_a.rest.x, 0.0f, pos.z - g_a.rest.z };
				if (parsed && g_a.liftSeen == 0 && life == 3 && drift.LengthXZ() > 0.6f && !(lua.flags & (state::kLCqc | state::kLCarry)) && fr.valid) {
					g_a.rest = pos;
					g_a.spot = fr.root + drift;
					g_pending = Placement{ g_a.spot, g_a.spot, "after the soldier's drift" };
					g_placeResult = -1;
					SetExclusion(g_a.rest);
					Log::Info("fulton: soldier {:#x} reads {:.2f} m from where he lay -- Snake follows", g_a.soldier, drift.LengthXZ());
				}
			}
			if (a_nowMs - g_a.sleptAt > 20000) {
				a_call(std::format("return PhantomRing.FultonSleep({})", g_a.soldier));
				g_a.sleptAt = a_nowMs;
			}
			// Release: the enemy gone, awake or dead, Snake walked away, or he can't stay (riding, dead, Tarnished mode).
			const auto* r = Record(n, g_a.handle);
			const char* why = nullptr;
			if (!r) {
				why = "the Elden Ring enemy is gone";
			} else if (r->flags & proto::kActorDead) {
				why = "the Elden Ring enemy died";
			} else if (!(r->flags & proto::kActorAsleep)) {
				why = "the Elden Ring enemy woke up";
			} else if ((Vec3{ r->pos[0], r->pos[1], r->pos[2] } - fr.root).LengthXZ() > c.fultonLeave) {
				why = "Snake walked away";
			} else if (busy) {
				why = "Snake can't stay (riding, dead, Tarnished mode or no host)";
			}
			if (!why) {
				g_a.awayAt = 0;
				return;
			}
			if (!g_a.awayAt) {
				g_a.awayAt = a_nowMs;
			}
			if (a_nowMs - g_a.awayAt >= 500 && !(lua.flags & (state::kLCqc | state::kLCarry))) {
				++g_releases;
				GoHome(a_nowMs, fr, a_call, why);
			}
			return;
		}
		case Phase::kLifted:
			if (in >= 3500 && !(lua.flags & (state::kLCqc | state::kLMenu))) {
				GoHome(a_nowMs, fr, a_call, "fultoned");
			}
			return;
		case Phase::kHome:
			if (g_placeResult >= 0 || in > 3000) {
				if (g_placeResult != 1) {
					Log::Warn("fulton: Snake's placement back to the sandbox {} -- the treadmill brings him back", g_placeResult == 0 ? "failed" : "never ran");
					state::SetCentre(g_a.home);
					g_pending.reset();
				}
				Enter(Phase::kIdle, a_nowMs);
			}
			return;
		}
	}

	std::optional<Placement> TakePlacement()
	{
		std::scoped_lock lock(g_lock);
		auto p = g_pending;
		g_pending.reset();
		return p;
	}

	void PlacementDone(bool a_ok, const Vec3& a_shift)
	{
		std::scoped_lock lock(g_lock);
		g_placeResult = a_ok ? 1 : 0;
		g_lastShift = a_shift;
	}

	bool ExcludesDraw(const float a_drawPos[3], const float a_snakeRoot[3])
	{
		if (!g_anchored.load(std::memory_order_acquire)) {
			return false;
		}
		std::scoped_lock el(g_exclLock);
		const Vec3 at{ g_excl };
		return proxy::plan::NearProxy({ a_drawPos[0], a_drawPos[1], a_drawPos[2] }, { a_snakeRoot[0], a_snakeRoot[1], a_snakeRoot[2] }, &at, 1, 1.6f);
	}

	bool Anchored() { return g_anchored.load(std::memory_order_acquire); }

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		return std::format("fulton anchor {}: {} ({} s); {} anchors, {} fultons, {} released, {} failures; {} candidates{}, {} spent, {} fultoned; last: {}",
			Cfg().fultonAnchor ? "on" : "off", PhaseName(g_phase), (::GetTickCount64() - g_since) / 1000, g_anchors, g_fultons, g_releases, g_failures, g_cands.size(),
			g_candsKnown ? "" : " (not looked for yet)", g_spent.size(), g_done.size(), g_last);
	}
}
