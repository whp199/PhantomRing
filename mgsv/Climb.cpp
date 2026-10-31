#include "Climb.h"

#include "Camera.h"
#include "ClimbLogic.h"
#include "Config.h"
#include "FultonAnchor.h"
#include "Hold.h"
#include "Hud.h"
#include "Log.h"
#include "Menu.h"
#include "Pad.h"
#include "State.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <deque>
#include <format>
#include <mutex>
#include <numbers>
#include <sstream>

namespace phantom::guest::climb
{
	namespace
	{
		using logic::Vec3;
		constexpr std::uint16_t kNullId = 0xFFFF;
		constexpr std::uint16_t kActionButton = 0x8000;  // XInput Y: MGSV's ACTION (ladders, climb-ups, vaults)
		constexpr unsigned      kActionPulseMs = 150;
		// After the move the HUD layer stays frozen this long (its copies lag a few frames) and the odometer still (live: he settled
		// 0.5-0.9 m forward right after the placement home, which Elden Ring's puppet would have walked)
		constexpr std::uint64_t kHudThawMs = 300;

		struct Request
		{
			enum class Kind
			{
				kClimb,
				kClimbStop,
				kMantle,
			} kind{ Kind::kClimb };
			bool        down{ false };
			float       speed{ 0 };
			int         cm{ 0 };
			bool        vault{ false };
			float       secs{ 0 };
			const char* from{ "host" };
		};

		std::mutex          g_queueLock;
		std::deque<Request> g_queue;
		std::atomic<bool>   g_active{ false };

		// Game thread only.
		enum class Kind
		{
			kNone,
			kLadder,
			kMantle,
		};
		Kind          g_kind{ Kind::kNone };
		plan::Ladder  g_ladder;
		plan::Mantle  g_mantle;
		plan::Phase   g_lastPhase{ plan::Phase::kIdle };
		std::string   g_camKeep;       // the "camrot" command that turns MGSV's camera back after the move
		float         g_camQuat[4]{};  // the camera published while staged: the one Snake had when the move began ...
		Vec3          g_camRel;        // ... relative to his root
		Vec3          g_home;          // where Snake stood on the sandbox
		std::uint64_t g_startedMs{ 0 };
		std::uint64_t g_thawAt{ 0 };
		bool          g_frozen{ false };
		Request       g_req;

		std::mutex  g_describeLock;
		std::string g_lastLine{ "no climb yet" };
		std::atomic<std::uint64_t> g_climbs{ 0 }, g_mantles{ 0 }, g_refused{ 0 }, g_placeFailed{ 0 }, g_loops{ 0 };

		void SetLast(std::string a_line)
		{
			std::scoped_lock lock(g_describeLock);
			g_lastLine = std::move(a_line);
		}

		void Push(const Request& a_r)
		{
			std::scoped_lock lock(g_queueLock);
			if (g_queue.size() < 32) {
				g_queue.push_back(a_r);
			}
		}

		std::string CamKeep(const float a_q[4])
		{
			const auto  fwd = logic::RotateByQuat(a_q, { 0.0f, 0.0f, 1.0f });
			const float yaw = std::atan2(fwd.x, fwd.z) * 180.0f / std::numbers::pi_v<float>;
			const float pitch = std::asin(std::clamp(fwd.y, -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>;
			return std::isfinite(yaw) && std::isfinite(pitch) ? std::format("camrot {:.3f} {:.3f} 0.02", -pitch, yaw) : std::string();
		}

		// Why a move can't start now (nullptr: it can).
		const char* Refusal(const state::LuaView& a_lua, bool a_ready, bool a_mantle)
		{
			const auto& c = Cfg();
			if (a_mantle ? !c.mantle : !c.climb) {
				return a_mantle ? "[Mantle] Enabled = false" : "[Climb] Enabled = false";
			}
			if (!a_ready) {
				return "MGSV isn't ready";
			}
			if (state::Parked()) {
				return "Tarnished mode";
			}
			if (fulton::Anchored()) {
				return "the Fulton anchor holds him";
			}
			if ((a_lua.flags & state::kLVehicle) || a_lua.ridden != kNullId) {
				return "riding";
			}
			if ((a_lua.flags & (state::kLDead | state::kLDemo | state::kLMenu | state::kLCbox)) || menu::PauseOpen() || menu::Open()) {
				return "busy (dead, a cutscene, a menu, the box)";
			}
			if (!(hold::Available() || hold::Init()) || !hold::Available()) {
				return "the native placement is unavailable";
			}
			return nullptr;
		}

		plan::MantleSite Site(const float a_v[5])
		{
			plan::MantleSite s;
			s.stand = { a_v[0], a_v[1], a_v[2] };
			s.yawDeg = a_v[3];
			s.heightCm = static_cast<int>(a_v[4]);
			s.valid = !(a_v[0] == 0.0f && a_v[1] == 0.0f && a_v[2] == 0.0f) && s.stand.Finite();
			return s;
		}

		void Begin(Kind a_kind, const camera::Sample& a_cam, const Vec3& a_root, const state::LuaView& a_lua, std::uint64_t a_now)
		{
			g_kind = a_kind;
			g_home = a_root;
			g_startedMs = a_now;
			g_lastPhase = plan::Phase::kPlace;
			std::copy_n(a_cam.quat, 4, g_camQuat);
			g_camRel = Vec3{ a_cam.pos[0], a_cam.pos[1], a_cam.pos[2] } - a_root;
			g_camKeep = CamKeep(a_cam.quat);
			g_active.store(true, std::memory_order_release);
			state::SetStaged(true);
			capture::hud::SetFrozen(true);
			g_frozen = true;
			if (a_lua.flags & (state::kLSquat | state::kLCrawl)) {
				state::QueueCommand("stance 1");  // on his feet first: MGSV's action at a ladder or a wall is a standing one
			}
		}

		void Finish(const char* a_why, std::uint64_t a_now)
		{
			const auto ms = a_now - g_startedMs;
			if (g_kind == Kind::kLadder) {
				g_loops += static_cast<std::uint64_t>(g_ladder.Loops());
				const auto line = std::format("climb: over ({}) after {:.1f} s -- {:.1f} m climbed in MGSV, {} loops (mounted again), the first mount {} ms after Y",
					a_why, static_cast<double>(ms) / 1000.0, g_ladder.Climbed(), g_ladder.Loops(), g_ladder.MountedMs());
				Log::Info("{}", line);
				SetLast(line);
			} else if (g_kind == Kind::kMantle) {
				const auto line = std::format("mantle: over ({}) after {:.1f} s -- MGSV's move took {} ms (Y to back on his feet), his root rose {:.2f} m",
					a_why, static_cast<double>(ms) / 1000.0, g_mantle.MoveMs(), g_mantle.Rose());
				Log::Info("{}", line);
				SetLast(line);
			}
			g_kind = Kind::kNone;
			g_lastPhase = plan::Phase::kIdle;
			g_active.store(false, std::memory_order_release);
			pad::SetOwned(false);  // the player's pad again; state::Staged() (the odometer still) until kHudThawMs: he settles there
			if (!g_camKeep.empty()) {
				state::QueueCommand(g_camKeep);
			}
			g_thawAt = a_now + kHudThawMs;
		}

		void StartClimb(const Request& a_r, const camera::Sample& a_cam, const Vec3& a_root, float a_yaw, const state::LuaView& a_lua, bool a_ready,
			std::uint64_t a_now)
		{
			if (g_kind == Kind::kLadder) {
				g_ladder.Change(a_r.down, a_r.speed);
				Log::Info("climb: now {} at {:.2f} m/s ({})", a_r.down ? "down" : "up", a_r.speed, a_r.from);
				return;
			}
			if (g_kind == Kind::kMantle) {
				++g_refused;
				Log::Info("climb: refused -- a climb-up/vault is still playing ({})", a_r.from);
				return;
			}
			if (const char* why = Refusal(a_lua, a_ready, false)) {
				if (++g_refused <= 20) {
					Log::Info("climb: refused -- {} ({})", why, a_r.from);
				}
				SetLast(std::format("climb refused: {}", why));
				return;
			}
			const auto&      c = Cfg();
			plan::LadderSite site{ { c.climbLadder[0], c.climbLadder[1], c.climbLadder[2] }, c.climbLadder[3] };
			plan::LadderGeometry geo;
			Begin(Kind::kLadder, a_cam, a_root, a_lua, a_now);
			g_ladder.Start(site, geo, a_r.down, a_r.speed, a_root, a_yaw, a_now);
			++g_climbs;
			Log::Info("climb #{}: {} at {} -- Snake staged at MGSV's ladder ({:.2f},{:.2f},{:.2f} yaw {:.0f}) from {:.2f},{:.2f},{:.2f} ({})", g_climbs.load(),
				a_r.down ? "down" : "up", a_r.speed > 0.0f ? std::format("{:.2f} m/s", a_r.speed) : std::string("MGSV's pace"), site.origin.x, site.origin.y,
				site.origin.z, site.yawDeg, a_root.x, a_root.y, a_root.z, a_r.from);
		}

		void StartMantle(const Request& a_r, const camera::Sample& a_cam, const Vec3& a_root, float a_yaw, const state::LuaView& a_lua, bool a_ready,
			std::uint64_t a_now)
		{
			if (g_kind != Kind::kNone) {
				++g_refused;
				Log::Info("mantle: refused -- a {} is still playing ({})", g_kind == Kind::kLadder ? "ladder climb" : "climb-up/vault", a_r.from);
				return;
			}
			if (const char* why = Refusal(a_lua, a_ready, true)) {
				if (++g_refused <= 20) {
					Log::Info("mantle: refused -- {} ({})", why, a_r.from);
				}
				SetLast(std::format("mantle refused: {}", why));
				return;
			}
			const auto& c = Cfg();
			const auto  site = plan::PickMantle(a_r.cm, a_r.vault, Site(c.mantleVault), Site(c.mantleLow), Site(c.mantleHigh), c.mantleLowMaxCm);
			if (!site.valid) {
				++g_refused;
				Log::Info("mantle: refused -- no [Mantle] site for a {} cm {} ({})", a_r.cm, a_r.vault ? "vault" : "climb-up", a_r.from);
				SetLast(std::format("mantle refused: no site for a {} cm {}", a_r.cm, a_r.vault ? "vault" : "climb-up"));
				return;
			}
			Begin(Kind::kMantle, a_cam, a_root, a_lua, a_now);
			g_mantle.Start(site, a_root, a_yaw, a_now);
			++g_mantles;
			Log::Info("mantle #{}: {} cm {} (Elden Ring's {:.2f} s) -- Snake staged at MGSV's {} cm obstacle ({:.2f},{:.2f},{:.2f} yaw {:.0f}) ({})", g_mantles.load(),
				a_r.cm, a_r.vault ? "vault" : "climb-up", a_r.secs, site.heightCm, site.stand.x, site.stand.y, site.stand.z, site.yawDeg, a_r.from);
		}
	}

	void OnHostEvent(const proto::HostEvent& a_e)
	{
		Request r;
		if (a_e.type == proto::kHEvClimb) {
			r.kind = a_e.a ? Request::Kind::kClimb : Request::Kind::kClimbStop;
			r.down = a_e.b != 0;
			r.speed = std::isfinite(a_e.v0[0]) ? a_e.v0[0] : 0.0f;
		} else if (a_e.type == proto::kHEvMantle) {
			r.kind = Request::Kind::kMantle;
			r.cm = static_cast<int>(a_e.a);
			r.vault = a_e.b != 0;
			r.secs = std::isfinite(a_e.v0[0]) ? a_e.v0[0] : 0.0f;
		} else {
			return;
		}
		Push(r);
	}

	Result Update(const camera::Sample& a_cam, const Vec3& a_root, float a_yaw, const state::LuaView& a_lua, bool a_ready, double a_dt)
	{
		Result     out;
		const auto now = ::GetTickCount64();
		std::deque<Request> requests;
		{
			std::scoped_lock lock(g_queueLock);
			requests.swap(g_queue);
		}
		for (const auto& r : requests) {
			switch (r.kind) {
			case Request::Kind::kClimb:
				StartClimb(r, a_cam, a_root, a_yaw, a_lua, a_ready, now);
				break;
			case Request::Kind::kClimbStop:
				if (g_kind == Kind::kLadder) {
					g_ladder.Stop(now);
					Log::Info("climb: Elden Ring's climb ended ({}) -- Snake off MGSV's ladder and back on the sandbox", r.from);
				} else {
					Log::Debug("climb: an end with no climb under way ({})", r.from);
				}
				break;
			case Request::Kind::kMantle:
				StartMantle(r, a_cam, a_root, a_yaw, a_lua, a_ready, now);
				break;
			}
		}
		if (g_frozen && g_kind == Kind::kNone && now >= g_thawAt) {
			capture::hud::SetFrozen(false);
			g_frozen = false;
			state::SetStaged(false);  // the odometer counts again (he settled for kHudThawMs after the placement home)
		}
		if (g_kind == Kind::kNone) {
			return out;
		}

		plan::Facts f;
		f.nowMs = now;
		f.root = a_root;
		f.normalAction = (a_lua.flags & state::kLNormal) != 0;
		f.standing = (a_lua.flags & state::kLStand) && !(a_lua.flags & (state::kLSquat | state::kLCrawl));
		f.yaw = a_yaw;
		f.ready = a_ready;
		plan::Step  step = g_kind == Kind::kLadder ? g_ladder.Update(f, a_dt) : g_mantle.Update(f);
		const auto  phase = g_kind == Kind::kLadder ? g_ladder.Current() : g_mantle.Current();
		const char* what = g_kind == Kind::kLadder ? "climb" : "mantle";
		if (step.knockOff) {
			state::QueueCommand("dmoff");  // MGSV's ladder action ends: a direct-motion request played and stopped (GuestScript)
		}
		if (step.place) {
			const Vec3 at = *step.place + Vec3{ 0.0f, Cfg().warpLift, 0.0f };
			if (hold::Place(at, logic::YawQuat(step.placeYaw))) {
				if (step.face) {
					// The native placement keeps whatever way he faces: MGSV's Warp to the same spot turns him (no streaming: he is
					// there already).
					state::QueueCommand(std::format("warp {:.3f} {:.3f} {:.3f} {:.4f}", at.x, at.y, at.z, step.placeYaw));
				}
				out.placed = true;
				Log::Debug("{}: placed at {:.2f},{:.2f},{:.2f} yaw {:.0f} ({})", what, step.place->x, step.place->y, step.place->z,
					step.placeYaw * 180.0f / std::numbers::pi_v<float>, plan::PhaseName(phase));
			} else {
				++g_placeFailed;
				Log::Warn("{}: the native placement failed ({}) -- the move ends where Snake is", what, plan::PhaseName(phase));
				if (g_kind == Kind::kLadder) {
					g_ladder.PlacementFailed();
				} else {
					g_mantle.PlacementFailed();
				}
				step.active = false;
			}
		}
		if (step.pressAction) {
			pad::Pulse(kActionButton, kActionPulseMs);
		}
		if (step.note) {
			Log::Info("{}: {} ({} at {:.2f},{:.2f},{:.2f}{})", what, step.note, plan::PhaseName(phase), a_root.x, a_root.y, a_root.z,
				g_kind == Kind::kLadder ? std::format(", {:.1f} m climbed, {} loops", g_ladder.Climbed(), g_ladder.Loops()) : std::string());
		}
		if (phase != g_lastPhase) {
			Log::Debug("{}: {} -> {}", what, plan::PhaseName(g_lastPhase), plan::PhaseName(phase));
			g_lastPhase = phase;
		}
		if (!step.active) {
			pad::SetOwned(false);
			Finish(step.note ? step.note : "done", now);
			return out;
		}
		pad::SetOwned(true, step.stickY);
		out.climbing = g_kind == Kind::kLadder;
		out.mantling = g_kind == Kind::kMantle;
		return out;
	}

	bool Active() { return g_active.load(std::memory_order_acquire); }

	bool PublishedCamera(const Vec3& a_root, float a_pos[3], float a_quat[4])
	{
		if (!Active()) {
			return false;
		}
		const Vec3 p = a_root + g_camRel;
		a_pos[0] = p.x;
		a_pos[1] = p.y;
		a_pos[2] = p.z;
		std::copy_n(g_camQuat, 4, a_quat);
		return true;
	}

	Vec3 Home() { return g_home; }

	std::string Command(std::string_view a_args, bool a_mantle)
	{
		std::string        args(a_args);
		std::istringstream ss(args);
		std::string        w;
		ss >> w;
		Request r;
		r.from = "console";
		if (a_mantle) {
			// !mantle <cm> [vault] [secs]
			if (w.empty()) {
				return Describe();
			}
			r.kind = Request::Kind::kMantle;
			r.cm = std::atoi(w.c_str());
			std::string x;
			while (ss >> x) {
				if (x == "vault") {
					r.vault = true;
				} else {
					r.secs = static_cast<float>(std::atof(x.c_str()));
				}
			}
			Push(r);
			return std::format("mantle {} cm {} queued", r.cm, r.vault ? "vault" : "climb-up");
		}
		// !climb up|down [speed] | off
		if (w.empty()) {
			return Describe();
		}
		if (w == "off" || w == "end" || w == "stop" || w == "0") {
			r.kind = Request::Kind::kClimbStop;
			Push(r);
			return "climb end queued";
		}
		if (w != "up" && w != "down" && w != "1") {
			return "usage: !climb up|down [speed m/s] | off   (!mantle <cm> [vault] [secs])";
		}
		r.kind = Request::Kind::kClimb;
		r.down = w == "down";
		std::string sp;
		if (ss >> sp) {
			r.speed = static_cast<float>(std::atof(sp.c_str()));
		}
		Push(r);
		return std::format("climb {} {} queued", r.down ? "down" : "up", r.speed > 0.0f ? std::format("at {:.2f} m/s", r.speed) : std::string("at MGSV's pace"));
	}

	std::string Describe()
	{
		const auto& c = Cfg();
		std::string last;
		{
			std::scoped_lock lock(g_describeLock);
			last = g_lastLine;
		}
		return std::format("climb: {} (ladder {:.2f},{:.2f},{:.2f} yaw {:.0f}; mantle {}: vault {}, low {}, high {}); {} climbs, {} mantles, "
		                   "{} refused, {} loops (mounted again), {} placements failed{}; last: {}",
			c.climb ? "on" : "off", c.climbLadder[0], c.climbLadder[1], c.climbLadder[2], c.climbLadder[3], c.mantle ? "on" : "off",
			c.mantleVault[0] != 0.0f ? std::format("{:.0f} cm", c.mantleVault[4]) : std::string("unset"),
			c.mantleLow[0] != 0.0f ? std::format("{:.0f} cm", c.mantleLow[4]) : std::string("unset"),
			c.mantleHigh[0] != 0.0f ? std::format("{:.0f} cm", c.mantleHigh[4]) : std::string("unset"), g_climbs.load(), g_mantles.load(), g_refused.load(),
			g_loops.load(), g_placeFailed.load(), Active() ? " -- one under way" : "", last);
	}
}
