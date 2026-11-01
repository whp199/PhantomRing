#include "Climb.h"

#include "Camera.h"
#include "ClimbLogic.h"
#include "Config.h"
#include "FultonAnchor.h"
#include "Hold.h"
#include "Hud.h"
#include "LadderRung.h"
#include "Link.h"
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
#include <optional>
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
		// Elden Ring gone (no heartbeat) this long, or a new host session, while a ladder climb plays: it ends here (no a = 0 will
		// ever come: Elden Ring crashed or was restarted; MGSV runs on for [Link] QuitWithoutHostSec). And no ladder climb runs
		// longer than kMaxClimbMs whatever happens (Elden Ring's longest ladders take well under a minute).
		constexpr std::uint64_t kHostLostMs = 1000;
		constexpr std::uint64_t kMaxClimbMs = 300000;
		// A new climb asked for while the last one is on its way out starts this long after it is over: the host sees
		// kGuestClimbing go down between the two (er/Host.cpp: MGSV played *this* climb).
		constexpr std::uint64_t kPendingGapMs = 80;
		// After a move, a rigid draw near Snake far from where he stood on the sandbox is a prop of the staging site, not his
		// (Capture.cpp): for this long after it.
		constexpr std::uint64_t kAwayWindowMs = 5000;
		constexpr float         kAwayDistance = 10.0f;

		struct Request
		{
			enum class Kind
			{
				kClimb,
				kClimbStop,
				kMantle,
			} kind{ Kind::kClimb };
			bool        down{ false };
			float       speed{ -1 };  // m/s; 0 = still on the rungs, < 0 = unknown (MGSV's own pace)
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
		// The odometer still (Frame.cpp): from the move's start until plan::OdoThaw says so after it -- the player's stick, or the
		// settle's kHudThawMs. MGSV's HUD layer stays frozen the full kHudThawMs either way.
		std::atomic<bool> g_odoStill{ false };
		plan::OdoThaw     g_odoThaw;
		std::atomic<std::uint64_t> g_odoStickThaws{ 0 };
		Request       g_req;
		std::uint32_t g_beginGen{ 0 };           // the host session the move began in
		std::uint64_t g_hostLostSince{ 0 };      // ... and since when its heartbeat is gone (0: alive)
		std::optional<Request> g_pendingClimb;   // a = 1 that came while the last climb was ending: started after it
		std::uint64_t g_pendingAt{ 0 };
		// Capture.cpp's render thread: where he stood on the sandbox and when the last move ended (AwayFromHome).
		std::atomic<float>         g_homeX{ 0 }, g_homeY{ 0 }, g_homeZ{ 0 };
		std::atomic<std::uint64_t> g_endedMs{ 0 };

		std::mutex  g_describeLock;
		std::string g_lastLine{ "no climb yet" };
		std::atomic<std::uint64_t> g_climbs{ 0 }, g_mantles{ 0 }, g_refused{ 0 }, g_placeFailed{ 0 }, g_loops{ 0 }, g_recycles{ 0 };

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
			g_homeX.store(a_root.x, std::memory_order_relaxed);
			g_homeY.store(a_root.y, std::memory_order_relaxed);
			g_homeZ.store(a_root.z, std::memory_order_relaxed);
			g_beginGen = link::AttachGeneration();
			g_hostLostSince = 0;
			g_startedMs = a_now;
			g_lastPhase = plan::Phase::kPlace;
			std::copy_n(a_cam.quat, 4, g_camQuat);
			g_camRel = Vec3{ a_cam.pos[0], a_cam.pos[1], a_cam.pos[2] } - a_root;
			g_camKeep = CamKeep(a_cam.quat);
			g_active.store(true, std::memory_order_release);
			state::SetStaged(true);
			g_odoStill.store(true, std::memory_order_release);
			g_odoThaw.Cancel();
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
				g_recycles += static_cast<std::uint64_t>(g_ladder.Recycles());
				const auto line = std::format("climb: over ({}) after {:.1f} s -- {:.1f} m climbed in MGSV, {} moved along MGSV's ladder (seamless), {} loops "
				                              "(mounted again), the first mount {} ms after Y",
					a_why, static_cast<double>(ms) / 1000.0, g_ladder.Climbed(), g_ladder.Recycles(), g_ladder.Loops(), g_ladder.MountedMs());
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
			g_endedMs.store(a_now, std::memory_order_relaxed);
			g_active.store(false, std::memory_order_release);
			pad::SetOwned(false);  // the player's pad again; state::Staged() (the odometer still) until kHudThawMs: he settles there
			if (!g_camKeep.empty()) {
				state::QueueCommand(g_camKeep);
			}
			g_thawAt = a_now + kHudThawMs;
			g_odoThaw.Start(a_now);
		}

		std::string PaceText(float a_speed)
		{
			return a_speed > 0.0f ? std::format("{:.2f} m/s", a_speed) : a_speed == 0.0f ? std::string("still (on the rungs)") : std::string("MGSV's pace");
		}

		void StartClimb(const Request& a_r, const camera::Sample& a_cam, const Vec3& a_root, float a_yaw, const state::LuaView& a_lua, bool a_ready,
			std::uint64_t a_now)
		{
			if (g_kind == Kind::kLadder) {
				if (g_ladder.Ending()) {
					// The last climb is on its way out (its a = 0 came, he leaves MGSV's ladder or goes home): this one is a new climb,
					// started once that one is over (a turn of Change() here would be lost with it).
					g_pendingClimb = a_r;
					Log::Info("climb: a new climb ({}) while the last one ends -- it starts once Snake is back on the sandbox ({})", a_r.down ? "down" : "up",
						a_r.from);
					return;
				}
				g_ladder.Change(a_r.down, a_r.speed);
				Log::Info("climb: now {} at {} ({})", a_r.down ? "down" : "up", PaceText(a_r.speed), a_r.from);
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
				a_r.down ? "down" : "up", PaceText(a_r.speed), site.origin.x, site.origin.y, site.origin.z, site.yawDeg, a_root.x, a_root.y, a_root.z, a_r.from);
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
			const auto  site = plan::PickMantle(a_r.cm, a_r.vault, Site(c.mantleVault), Site(c.mantleLow), Site(c.mantleHigh), c.mantleLowMaxCm,
				 Site(c.mantleStep), c.mantleStepMaxCm);
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
			r.speed = std::isfinite(a_e.v0[0]) ? a_e.v0[0] : -1.0f;  // exactly 0: Elden Ring's climber is still on the rungs
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
				if (g_pendingClimb) {
					g_pendingClimb.reset();  // the climb that was to follow is over before it began
					Log::Info("climb: the next climb ended before it began ({})", r.from);
				} else if (g_kind == Kind::kLadder) {
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
		if (g_pendingClimb && g_kind == Kind::kNone && now >= g_pendingAt) {
			const Request r = *g_pendingClimb;
			g_pendingClimb.reset();
			StartClimb(r, a_cam, a_root, a_yaw, a_lua, a_ready, now);
		}
		if (g_kind == Kind::kLadder && !g_ladder.Ending()) {
			// Elden Ring can't end this climb any more (it crashed or was restarted: no a = 0 comes) -- or it has run far too long.
			const char* why = nullptr;
			if (link::AttachGeneration() != g_beginGen) {
				why = "a new Elden Ring session";
			} else if (!link::HostAlive()) {
				g_hostLostSince = g_hostLostSince ? g_hostLostSince : now;
				why = now - g_hostLostSince >= kHostLostMs ? "Elden Ring is gone (no heartbeat)" : nullptr;
			} else {
				g_hostLostSince = 0;
			}
			if (!why && now - g_startedMs >= kMaxClimbMs) {
				why = "it has played for 5 minutes";
			}
			if (why) {
				g_ladder.Stop(now);
				Log::Warn("climb: ended here -- {}: Snake off MGSV's ladder and back on the sandbox", why);
			}
		}
		if (a_ready && Cfg().climb && Cfg().climbSeamless) {
			ladder_rung::Prepare();  // once: the search for MGSV's ladder system (a worker thread)
		}
		if (g_kind == Kind::kNone && g_odoThaw.Active()) {
			// The player's own stick ends the odometer's stillness at once (plan::OdoThaw); else the settle's kHudThawMs.
			int lx = 0, ly = 0;
			proto::PadState pad{};
			if (link::HostAlive() && link::ReadPad(pad) && (pad.flags & proto::kPadConnected) && !(pad.flags & proto::kPadNeutral)) {
				lx = pad.thumbLX;
				ly = pad.thumbLY;
			}
			const auto why = g_odoThaw.Update(now, lx, ly);
			if (why != plan::OdoThaw::Why::kStill) {
				g_odoStill.store(false, std::memory_order_release);
				if (why == plan::OdoThaw::Why::kStick) {
					++g_odoStickThaws;
					Log::Info("climb: the player's stick {} ms after the move -- Snake's motion counts again (the HUD stays frozen to {} ms)",
						g_odoThaw.EndedAfterMs(), kHudThawMs);
				}
			}
		}
		if (g_frozen && g_kind == Kind::kNone && now >= g_thawAt) {
			capture::hud::SetFrozen(false);
			g_frozen = false;
			g_odoStill.store(false, std::memory_order_release);
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
		const auto& cfg = Cfg();
		const Vec3  foot{ cfg.climbLadder[0], cfg.climbLadder[1], cfg.climbLadder[2] };
		std::optional<ladder_rung::Rungs> rungs;
		if (g_kind == Kind::kLadder && cfg.climbSeamless && !f.normalAction && (rungs = ladder_rung::Read(foot))) {
			f.rung = rungs->rung;
			f.rungTarget = rungs->target;
			f.rungTop = rungs->top;
		}
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
				Log::Warn("{}: the native placement failed ({}) -- {}", what, plan::PhaseName(phase),
					phase == plan::Phase::kReturn ? "home again next frame" : "Snake goes home from where he is");
				if (g_kind == Kind::kLadder) {
					g_ladder.PlacementFailed(now);
					step.active = g_ladder.Active();
				} else {
					g_mantle.PlacementFailed(now);
					step.active = g_mantle.Active();
				}
			}
		}
		if (step.rungShift && g_kind == Kind::kLadder) {
			if (rungs && ladder_rung::Shift(foot, *rungs, step.rungShift)) {
				out.placed = true;  // his root jumps along MGSV's ladder: the odometer stays still (it does while staged anyway)
				Log::Debug("climb: moved {} rung pairs along MGSV's ladder ({} -> {}, top {})", step.rungShift, rungs->rung, rungs->rung + step.rungShift,
					rungs->top);
			} else {
				g_ladder.ShiftFailed();
				Log::Info("climb: the rung shift ({:+d}) was refused -- this climb loops through MGSV's ladder ends", step.rungShift);
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
		if (step.warpHome) {
			// The last resort: MGSV's own Warp (Lua) to where he stood on the sandbox, facing as he did.
			const Vec3  at = g_home + Vec3{ 0.0f, Cfg().warpLift, 0.0f };
			const float yaw = g_kind == Kind::kLadder ? g_ladder.HomeYaw() : g_mantle.HomeYaw();
			state::QueueCommand(std::format("warp {:.3f} {:.3f} {:.3f} {:.4f}", at.x, at.y, at.z, yaw));
			Log::Warn("{}: Snake never got back on the sandbox ({:.2f},{:.2f},{:.2f}, {:.1f} m from it) -- MGSV's Warp home", what, a_root.x, a_root.y,
				a_root.z, (a_root - g_home).LengthXZ());
		}
		if (!step.active) {
			pad::SetOwned(false);
			Finish(step.note ? step.note : "done", now);
			if (g_pendingClimb) {
				g_pendingAt = now + kPendingGapMs;
			}
			return out;
		}
		pad::SetOwned(true, step.stickY);
		out.climbing = g_kind == Kind::kLadder;
		out.mantling = g_kind == Kind::kMantle;
		return out;
	}

	bool Active() { return g_active.load(std::memory_order_acquire); }
	bool OdometerStill() { return g_odoStill.load(std::memory_order_acquire); }

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

	bool AwayFromHome(const float a_root[3])
	{
		const auto ended = g_endedMs.load(std::memory_order_relaxed);
		if (!Active() && (!ended || ::GetTickCount64() - ended > kAwayWindowMs)) {
			return false;
		}
		const float dx = a_root[0] - g_homeX.load(std::memory_order_relaxed), dz = a_root[2] - g_homeZ.load(std::memory_order_relaxed);
		return dx * dx + dz * dz > kAwayDistance * kAwayDistance;
	}

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
			if (w == "site") {
				// !mantle site low|high|vault <x> <y> <z> <yaw deg> <cm> (or 'none'): a [Mantle] site for this session
				std::string which, first;
				ss >> which;
				float* dst = which == "low" ? MutableCfg().mantleLow : which == "high" ? MutableCfg().mantleHigh : which == "vault" ? MutableCfg().mantleVault :
				             which == "step" ? MutableCfg().mantleStep : nullptr;
				if (!dst) {
					return "usage: !mantle site step|low|high|vault <x> <y> <z> <yaw> <cm> | none";
				}
				float v[5]{};
				if ((ss >> first) && first == "none") {
					std::fill_n(dst, 5, 0.0f);
					return std::format("mantle: {} site cleared", which);
				}
				v[0] = static_cast<float>(std::atof(first.c_str()));
				if (!(ss >> v[1] >> v[2] >> v[3] >> v[4])) {
					return "usage: !mantle site low|high|vault <x> <y> <z> <yaw> <cm> | none";
				}
				std::copy_n(v, 5, dst);
				return std::format("mantle: {} site {:.2f},{:.2f},{:.2f} facing {:.0f}, {:.0f} cm", which, v[0], v[1], v[2], v[3], v[4]);
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
			r.speed = static_cast<float>(std::atof(sp.c_str()));  // 0 = still on the rungs, as Elden Ring's a = 1 with v0 0
		}
		Push(r);
		return std::format("climb {} at {} queued", r.down ? "down" : "up", PaceText(r.speed));
	}

	std::string Describe()
	{
		const auto& c = Cfg();
		std::string last;
		{
			std::scoped_lock lock(g_describeLock);
			last = g_lastLine;
		}
		return std::format("climb: {} (ladder {:.2f},{:.2f},{:.2f} yaw {:.0f}, seamless {}; mantle {}: vault {}, step {} (to {} cm), low {}, high {}); {} climbs, {} mantles, "
		                   "{} refused, {} moved along MGSV's ladder, {} loops (mounted again), {} placements failed, {} ended early by the stick{}; last: {}\n{}",
			c.climb ? "on" : "off", c.climbLadder[0], c.climbLadder[1], c.climbLadder[2], c.climbLadder[3], c.climbSeamless ? "on" : "off", c.mantle ? "on" : "off",
			c.mantleVault[0] != 0.0f ? std::format("{:.0f} cm", c.mantleVault[4]) : std::string("unset"),
			c.mantleStep[0] != 0.0f ? std::format("{:.0f} cm", c.mantleStep[4]) : std::string("unset"), c.mantleStepMaxCm,
			c.mantleLow[0] != 0.0f ? std::format("{:.0f} cm", c.mantleLow[4]) : std::string("unset"),
			c.mantleHigh[0] != 0.0f ? std::format("{:.0f} cm", c.mantleHigh[4]) : std::string("unset"), g_climbs.load(), g_mantles.load(), g_refused.load(),
			g_recycles.load(), g_loops.load(), g_placeFailed.load(), g_odoStickThaws.load(), Active() ? " -- one under way" : "", last, ladder_rung::Describe());
	}
}
