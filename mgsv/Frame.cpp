#include "Frame.h"

#include "ActorProxy.h"
#include "Addresses.h"
#include "Ammo.h"
#include "Capture.h"
#include "Hud.h"
#include "Combat.h"
#include "Cqc.h"
#include "Config.h"
#include "Death.h"
#include "Dive.h"
#include "Follow.h"
#include "FultonAnchor.h"
#include "FramePace.h"
#include "Hold.h"
#include "Horse.h"
#include "Blast.h"
#include "C4.h"
#include "Link.h"
#include "Menu.h"
#include "Log.h"
#include "Logic.h"
#include "Pad.h"
#include "Placed.h"
#include "Skeleton.h"
#include "State.h"
#include "Throw.h"
#include "Vars.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <numbers>

namespace phantom::guest::frame
{
	namespace
	{
		using logic::Vec3;
		using state::LuaFlags;

		constexpr std::uint16_t kNullId = 0xFFFF;

		// Frame-thread state (the camera hook never runs concurrently with itself).
		std::uint64_t     g_frame{ 0 };
		std::int64_t      g_lastQpc{ 0 };
		std::int64_t      g_qpcFreq{ 0 };
		logic::Odometer   g_odo;
		logic::AmmoWatch  g_ammo;
		std::uint32_t     g_attachGen{ ~0u };
		std::uint16_t     g_lastBuddyAmmo{ 0 };
		bool              g_buddyAmmoPrimed{ false };
		ULONGLONG         g_warpAt{ 0 };        // when we queued the last treadmill warp
		bool              g_warpPending{ false };
		float             g_settleY{ 0 };        // height the odometer sees while a landed warp settles
		float             g_warpTargetY{ 0 };    // root height the queued warp aims at (the centre's)
		std::uint64_t     g_settleUntil{ 0 };    // frame: the odometer ignores vertical motion until then
		Vec3              g_lastRoot;             // last frame's root (landing detection)
		std::uint64_t     g_landings{ 0 };
		logic::FlameBurst g_flameBurst;
		std::uint64_t     g_flames{ 0 };
		std::uint64_t     g_warps{ 0 };
		bool              g_diedLatched{ false };
		bool              g_diedNow{ false };      // this frame: the life clamp latched (Death.cpp)
		bool              g_respawnedNow{ false };  // this frame: kHEvRespawn
		std::int32_t      g_lifeWritten{ -1 };  // value of our last native write, for the stickiness check
		std::int32_t      g_lifeBefore{ -1 };   // ... and the life it replaced
		std::uint64_t     g_lifeWrittenFrame{ 0 };
		bool              g_lifeWarned{ false };
		// The regeneration gate (logic::RegenCap): after an Elden Ring hit, life is capped by MGSV's own post-hit curve.
		bool              g_regenGate{ false };
		ULONGLONG         g_regenHitAt{ 0 };
		float             g_regenBase{ 0 };
		std::uint64_t     g_regenCapped{ 0 };  // frames the cap wrote life back down (!status)
		std::uint8_t      g_lastPhase{ 0 };    // MGSV's alert phase last published (logged on change)
		bool              g_nativeHoldWarned{ false };
		bool              g_wasReady{ false };
		Vec3              g_buddyLast;        // buddy heading from its motion (no per-frame GetRotationY)
		bool              g_buddyLastValid{ false };
		float             g_buddyYaw{ 0 };
		// A ridden vehicle no warp of ours could move (D-Horse answers no Lua SetPosition: live 2026-10-13, 15 warps in 45 s
		// never landed): after 3 misses in a row the treadmill leaves it alone until Snake rides something else.
		std::uint16_t     g_warpVehicle{ 0xFFFF };
		int               g_vehicleMisses{ 0 };
		std::uint16_t     g_unwarpable{ 0xFFFF };
		proto::GuestState g_last{};
		std::mutex        g_publishLock;  // GuestState has two writers: the camera frame, and the Lua thread while frames stall
		std::uint64_t     g_lastFrameMs{ 0 };
		std::uint64_t     g_stalledPublished{ 0 };
		std::mutex        g_describeLock;
		std::string       g_describe{ "no player frame yet" };
		// Snake's jumps ([Jump], protocol v14 kGuestJumping): a press lifts him in MGSV, the flag stays up until he lands there.
		logic::JumpTracker g_jump;
		std::uint64_t     g_jumpsLifted{ 0 };
		std::uint64_t     g_jumpsRefused{ 0 };
		std::uint64_t     g_jumpsTimedOut{ 0 };

		std::int64_t Qpc()
		{
			LARGE_INTEGER t;
			::QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		proto::AmmoKind KindOf(std::uint16_t a_weapon)
		{
			const auto& c = Cfg();
			if (std::ranges::find(c.tranqWeapons, a_weapon) != c.tranqWeapons.end()) {
				return proto::kAmmoTranq;
			}
			if (std::ranges::find(c.stunWeapons, a_weapon) != c.stunWeapons.end()) {
				return proto::kAmmoStun;
			}
			return proto::kAmmoLethal;
		}

		void SetV(float a_out[3], const Vec3& a_v)
		{
			a_out[0] = a_v.x;
			a_out[1] = a_v.y;
			a_out[2] = a_v.z;
		}

		// Riding with the camera in the cockpit: aiming (LT) on D-Walker always puts MGSV's camera there, ~0.6 m in front of
		// the walker's root and ~2.3 m up; in third person it is ~5 m behind (VERIFIED live 2026-10-12: (820.74, -9.79,
		// 1180.94) aiming vs (826.36, -10.47, 1181.92) not, walker root (821.35, -12.05, 1180.88)). MGSV sets no status for it.
		bool RidingFirstPerson(const camera::Sample& a_cam, const Vec3& a_root)
		{
			const Vec3 d = Vec3{ a_cam.pos[0], a_cam.pos[1], a_cam.pos[2] } - a_root;
			return d.Finite() && d.LengthXZ() < 1.8f && d.y > 1.0f;
		}

		// D-Walker's flamethrower nozzle (its right arm) relative to the walker's root and facing: estimated from MGSV's
		// first-person view (the nozzle at the lower right, ~1.6 m beyond the cockpit camera), m.
		constexpr float kNozzleAhead = 1.8f, kNozzleRight = 0.45f, kNozzleUp = 1.9f;
		constexpr float kFlameReach = 12.0f;  // the stream's reach (kGEvBuddyFire pad[1] in mm)

		// After an Elden Ring hit MGSV regenerates as after its own (Logic.h RegenCap): life MGSV regenerated above the curve is
		// written back down to it, until the curve reaches full life (or a respawn/rest ends it).
		void RegenGate(const vars::Snapshot& a_vars)
		{
			if (!g_regenGate) {
				return;
			}
			const auto& c = Cfg();
			if (!c.regenGate || !a_vars.valid || !a_vars.lifeMax) {
				g_regenGate = false;
				return;
			}
			const double since = static_cast<double>(::GetTickCount64() - g_regenHitAt) / 1000.0;
			const float  cap = logic::RegenCap(g_regenBase, since, c.regenDelaySec, c.regenRampSec, c.regenRate);
			if (cap >= static_cast<float>(a_vars.lifeMax)) {
				g_regenGate = false;
				Log::Debug("regen: MGSV's curve reached full life {:.1f} s after the last hit (from {:.0f})", since, g_regenBase);
				return;
			}
			if (vars::PendingLife() >= 0 || static_cast<float>(a_vars.life) <= cap + 1.0f) {
				return;
			}
			if (vars::WriteLife(static_cast<std::uint16_t>(std::max(1.0f, cap)))) {
				++g_regenCapped;
			}
		}

		// ---- Elden Ring -> Snake ------------------------------------------------------------------
		void HostEvents(const vars::Snapshot& a_vars, const Vec3& a_root)
		{
			proto::HostEvent e;
			std::uint16_t    life = a_vars.life;
			if (const int pending = vars::PendingLife(); pending >= 0 && pending < life) {
				life = static_cast<std::uint16_t>(pending);  // a hit queued a moment ago, not applied yet
			}
			while (link::PopHostEvent(e)) {
				switch (e.type) {
				case proto::kHEvHurt:
					{
						const auto r = logic::ApplyHurt(life, static_cast<float>(e.a), Cfg().lifeScale);
						Log::Debug("hurt: er {} kind {} -> life {} -> {}", e.a, e.b, life, r.life);
						if (Cfg().nativeLifeWrite && r.life != life && vars::WriteLife(r.life)) {
							g_lifeBefore = life;
							g_lifeWritten = r.life;
							g_lifeWrittenFrame = g_frame;
						}
						if (Cfg().hurtFeedback && r.life < life) {
							// MGSV's own reaction: its camera (= Elden Ring's) shakes, Snake gets blood where it hit.
							const Vec3 from{ e.v0[0], e.v0[1], e.v0[2] };
							const auto fb = logic::FeedbackFor(static_cast<std::uint16_t>(life - r.life), a_vars.lifeMax, e.b, a_root, from);
							if (fb.shake) {
								std::string cmd = std::format("hurt {:.2f} {:.2f} {:.2f}", fb.level, fb.time, fb.decay);
								if (fb.blood && Cfg().hurtBlood) {
									cmd += std::format(" {:.2f} {:.2f} {:.2f} {:.2f}", fb.bloodPos.x, fb.bloodPos.y, fb.bloodPos.z, fb.bloodRadius);
								}
								state::QueueCommand(cmd);
							}
						}
						life = r.life;
						if (Cfg().regenGate && r.life < a_vars.lifeMax) {
							g_regenGate = true;
							g_regenHitAt = ::GetTickCount64();
							g_regenBase = r.life;
						}
						if (r.died && !g_diedLatched) {
							g_diedLatched = true;
							g_diedNow = true;
							proto::GuestEvent d{};
							d.type = proto::kGEvDied;
							d.frame = static_cast<std::uint32_t>(g_frame);
							link::PushEvent(d);
							Log::Info("life: Snake would have died -- kept at 1, kGEvDied sent");
						}
						break;
					}
				case proto::kHEvRespawn:
					g_diedLatched = false;
					g_respawnedNow = true;
					if (a_vars.lifeMax && vars::WriteLife(a_vars.lifeMax)) {
						g_lifeBefore = life;
						g_lifeWritten = a_vars.lifeMax;
						g_lifeWrittenFrame = g_frame;
						life = a_vars.lifeMax;
					}
					g_regenGate = false;
					Log::Info("life: respawn -- refilled to {}", a_vars.lifeMax);
					break;
				case proto::kHEvHeal:
					{
						// Elden Ring healed the puppet (Crimson Tears, a grace): the same share of Snake's life back, in kHEvHurt
						// units (x LifeScale). Never while he lies dead: the respawn refills him.
						if (g_diedLatched || !a_vars.lifeMax) {
							break;
						}
						const float add = static_cast<float>(e.a) * Cfg().lifeScale;
						const auto  healed = static_cast<std::uint16_t>(std::min<float>(a_vars.lifeMax, static_cast<float>(life) + std::max(0.0f, add)));
						if (healed > life && Cfg().nativeLifeWrite && vars::WriteLife(healed)) {
							g_lifeBefore = life;
							g_lifeWritten = healed;
							g_lifeWrittenFrame = g_frame;
							Log::Debug("heal: er {} -> life {} -> {}", e.a, life, healed);
							g_regenBase += static_cast<float>(healed - life);  // a flask is no regeneration: it isn't capped
							life = healed;
						}
						break;
					}
				case proto::kHEvRested:
					// v10: a grace rest (a = 0) or a respawn (a = 1, grace / Stake of Marika / death): Elden Ring refills the
					// flasks, Snake gets his life and every magazine, ammo stock and item back (Ammo.cpp, docs/GUEST.md 18.11).
					// A respawn's death is still kHEvRespawn's to end (it clears the died latch and stands him up).
					if (!g_diedLatched && a_vars.lifeMax && a_vars.life < a_vars.lifeMax && vars::WriteLife(a_vars.lifeMax)) {
						g_lifeBefore = life;
						g_lifeWritten = a_vars.lifeMax;
						g_lifeWrittenFrame = g_frame;
						life = a_vars.lifeMax;
					}
					g_regenGate = false;
					ammo::Request(e.a ? "respawn" : "rest");
					Log::Info("rested: {} -- life {}/{}, ammo refill requested", e.a ? "respawn" : "grace rest", life, a_vars.lifeMax);
					break;
				case proto::kHEvTarnished:
					// v10: Tarnished mode -- the player takes the Tarnished (a = 1) and Snake parks: neutral pad, no treadmill
					// warps, no follow whistles; a = 0 gives him back the pad where he stands.
					if ((e.a != 0) != state::Parked()) {
						state::SetParked(e.a != 0);
						pad::SetParked(e.a != 0);
						Log::Info("tarnished mode: {}", e.a ? "on -- Snake parked (neutral pad, no warps, no whistles)" : "off -- Snake has the pad again");
					}
					break;
				case proto::kHEvWarped:
					Log::Debug("host: the Elden Ring player was moved by the game (warp/elevator)");
					break;
				case proto::kHEvStatus:
					Log::Debug("host: status proc {}", e.a);
					break;
				case proto::kHEvDetonate:
					placed::Detonate(e.b);  // an Elden Ring enemy walked into one of Snake's mines
					break;
				default:
					break;
				}
			}
			// Does the engine keep a native write? 8 frames on, once the worker has applied it (vars::ApplyPendingLife runs
			// every ~33 ms), life back nearer the value it replaced than ours means it didn't. (It used to warn on any rise of
			// 300: MGSV regenerates right after our write -- it never saw a hit, so its regeneration delay never starts --
			// ~100 in 8 frames and more, and the warning fired on writes that had stuck: live, Margit's hits 2026-10-13.)
			if (g_lifeWritten >= 0 && g_frame >= g_lifeWrittenFrame + 8 && (vars::PendingLife() < 0 || g_frame >= g_lifeWrittenFrame + 30)) {
				const int now = a_vars.life;
				if (!g_lifeWarned && vars::PendingLife() < 0 && logic::LifeWriteLost(g_lifeBefore, g_lifeWritten, now)) {
					g_lifeWarned = true;
					Log::Warn("life: native playerLife write did not stick ({} -> {} written, {} now) -- need the Lua/native damage path (RE M6)",
						g_lifeBefore, g_lifeWritten, now);
				}
				g_lifeWritten = -1;
			}
		}

		// ---- shots ---------------------------------------------------------------------------------
		void Shots(const vars::Snapshot& a_vars, const camera::Sample& a_cam, const state::LuaView& a_lua)
		{
			const Vec3 forward = logic::RotateByQuat(a_cam.quat, { 0.0f, 0.0f, 1.0f });
			const Vec3 camPos{ a_cam.pos[0], a_cam.pos[1], a_cam.pos[2] };
			// Ammo counters are the fallback for when the ShotSolver hook (Combat.cpp) is refused.
			if (Cfg().fireFromAmmo && !combat::NativeShots()) {
				if (const auto shot = g_ammo.Update(a_vars.weapons, a_vars.ammo)) {
					float      muzzle[3];
					const bool fromBone = skeleton::BoneWorld(Cfg().muzzleBone, muzzle);
					if (!fromBone) {
						SetV(muzzle, camPos);
					}
					for (int i = 0; i < shot->rounds; ++i) {
						proto::GuestEvent e{};
						e.type = proto::kGEvFire;
						e.a = shot->weapon | (static_cast<std::uint32_t>(KindOf(shot->weapon)) << 16);
						e.b = 1;
						e.frame = static_cast<std::uint32_t>(g_frame);
						std::copy_n(muzzle, 3, e.v0);
						SetV(e.v1, forward);
						link::PushEvent(e);
					}
					Log::Debug("fire: slot {} weapon {:#x} rounds {} muzzle {}", shot->slot, shot->weapon, shot->rounds, fromBone ? "bone" : "camera");
				}
			}
			// D-Walker's gun (vars.buddyGearAmmoCount[0]); UNVERIFIED that it counts down live. Only until
			// the ShotSolver hook has seen a buddy round itself (re-backlog M5: its fire path is unknown).
			const auto buddyAmmo = a_vars.buddyGearAmmo[0];
			if (g_buddyAmmoPrimed && a_lua.buddyActive >= 0 && combat::BuddyRounds() == 0) {
				const int rounds = logic::CounterDrop(g_lastBuddyAmmo, buddyAmmo);
				for (int i = 0; i < rounds; ++i) {
					proto::GuestEvent e{};
					e.type = proto::kGEvBuddyFire;
					e.a = a_vars.buddyGearMainWp;
					e.b = 1;
					e.frame = static_cast<std::uint32_t>(g_frame);
					const bool ridden = a_lua.ridden != kNullId && a_lua.ridden == a_lua.buddyId;
					if (a_lua.buddyPosValid && !ridden) {
						SetV(e.v0, a_lua.buddyPos + Vec3{ 0.0f, 1.5f, 0.0f });
						const float yaw = a_lua.buddyYawValid ? a_lua.buddyYaw : 0.0f;
						SetV(e.v1, { std::sin(yaw), 0.0f, std::cos(yaw) });
					} else {
						SetV(e.v0, camPos);
						SetV(e.v1, forward);
					}
					link::PushEvent(e);
				}
			}
			g_lastBuddyAmmo = buddyAmmo;
			g_buddyAmmoPrimed = true;

			// D-Walker weapons that fire no rounds (its flamethrower: no ShotSolver call, no ammo counter;
			// VERIFIED live 2026-10-12): while Snake rides it and MGSV reports GUN_FIRE, a burst of short-range
			// kGEvBuddyFire along the aim, ~8 per second. pad[1] = range in mm (guest extension, like pad[0]).
			// Only with the ShotSolver hook: without it no round is ever seen and every gun would look like
			// a flamethrower (logic::FlameBurst). And only with the flamethrower as main weapon: a walker gun
			// whose rounds bypass the counted ShotSolver call would otherwise burn as well.
			const bool ridingBuddy = a_lua.buddyId != kNullId && a_lua.ridden == a_lua.buddyId;
			const bool firing = logic::FlameBurst::Firing(combat::NativeShots(), ridingBuddy, (a_lua.flags & state::kLGunFire) != 0, a_vars.buddyGearMainWp);
			if (g_flameBurst.Update(firing, combat::BuddyRounds(), ::GetTickCount64())) {
				proto::GuestEvent e{};
				e.type = proto::kGEvBuddyFire;
				e.a = (a_vars.buddyGearMainWp & 0xFFFF) | (static_cast<std::uint32_t>(proto::kAmmoLethal) << 16);
				e.b = 1;
				e.frame = static_cast<std::uint32_t>(g_frame);
				// From the nozzle on the walker's right arm. Aiming (LT: the cockpit camera, first person) MGSV burns where the
				// reticle points: at the camera's centre line, the stream's reach away. Not aiming, along the walker's facing
				// at the camera's pitch.
				const Vec3  root = logic::MatrixTranslation(a_cam.root);
				const float walkerYaw = logic::MatrixYaw(a_cam.root);
				const Vec3  nozzle = logic::FlameNozzle(root, walkerYaw, kNozzleAhead, kNozzleRight, kNozzleUp);
				const bool  aiming = (a_vars.buttons & addr::kPadHold) != 0;
				const float flat = std::sqrt(std::max(0.0f, 1.0f - forward.y * forward.y));
				const Vec3  facing{ std::sin(walkerYaw) * flat, forward.y, std::cos(walkerYaw) * flat };
				SetV(e.v0, nozzle);
				SetV(e.v1, aiming ? logic::FlameDirection(nozzle, camPos, forward, kFlameReach) : facing);
				e.pad[1] = static_cast<std::uint32_t>(kFlameReach * 1000.0f);
				link::PushEvent(e);
				if (g_flames++ == 0) {
					Log::Info("shots: D-Walker fires without rounds (main weapon type {}) -- flame bursts as kGEvBuddyFire", a_vars.buddyGearMainWp);
				}
			}
		}

		// The riding camera's orientation now, asked back right after a warp of what Snake rides (MGSV's own request: rotY = world
		// yaw, rotX = -pitch, degrees; an interpTime of 0 is ignored, 0.02 s works). Empty if it can't be computed.
		std::string RidingCameraKeep(const camera::Sample& a_cam)
		{
			const auto  fwd = logic::RotateByQuat(a_cam.quat, { 0.0f, 0.0f, 1.0f });
			const float camYaw = std::atan2(fwd.x, fwd.z) * 180.0f / std::numbers::pi_v<float>;
			const float camPitch = std::asin(std::clamp(fwd.y, -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>;
			return std::isfinite(camYaw) && std::isfinite(camPitch) ? std::format("camrot {:.3f} {:.3f} 0.02", -camPitch, camYaw) : std::string();
		}

		// ---- jump -----------------------------------------------------------------------------------
		// Why Snake can't jump right now (nullptr: he can): on foot, standing (walking and running are standing), not aiming,
		// not in a menu, CQC, a fall, a box or any other action, not parked, and with the native placement to lift him.
		const char* JumpRefusal(const vars::Snapshot& a_vars, const state::LuaView& a_lua, bool a_ready)
		{
			const std::uint32_t lf = a_lua.flags;
			if (!a_ready) {
				return "MGSV isn't ready";
			}
			if (state::Parked()) {
				return "Tarnished mode";
			}
			if (fulton::Anchored()) {
				return "the Fulton anchor holds him";
			}
			if ((lf & state::kLVehicle) || a_vars.vehicleId != kNullId || a_lua.ridden != kNullId) {
				return "riding";
			}
			if ((lf & state::kLMenu) || menu::PauseOpen() || menu::Open()) {
				return "a menu is open";
			}
			if (a_vars.buttons & addr::kPadHold) {
				return "aiming";
			}
			if (lf & state::kLCqc) {
				return "CQC";
			}
			if (lf & state::kLFall) {
				return "falling";
			}
			if ((lf & (state::kLSquat | state::kLCrawl)) || !(lf & state::kLStand)) {
				return "not standing (crouched or prone)";
			}
			if (lf & (state::kLDemo | state::kLDead | state::kLCarry | state::kLCbox | state::kLSubject | state::kLBinocle | state::kLHeli)) {
				return "busy (a box, carrying, the binoculars, first person ...)";
			}
			if (!(lf & state::kLNormal)) {
				return "in an action (a dive, a climb ...)";
			}
			if (g_warpPending) {
				return "a treadmill warp is landing";
			}
			if (!(hold::Available() || hold::Init()) || !hold::Available()) {
				return "the native placement is unavailable";
			}
			return nullptr;
		}

		// A [Jump] press: Snake lifted [Jump] Lift m in MGSV's own world through the treadmill's native placement (MGSV keeps his
		// running and plays its own fall and landing; the odometer rebased: the lift is no motion), kGuestJumping until he lands.
		void Jump(const camera::Sample& a_cam, const Vec3& a_root, const vars::Snapshot& a_vars, const state::LuaView& a_lua, bool a_ready)
		{
			const auto& c = Cfg();
			// R3 is MGSV's own while he aims (MGSV's HOLD, whatever LT reads), rides, is in a menu or the binoculars: the pad thread
			// passes it to MGSV then (the shoulder change, the call menu's select, the zoom ...), not taken for a jump it can't make.
			const std::uint32_t lf = a_lua.flags;
			pad::SetMgsvOwnsR3((a_vars.buttons & addr::kPadHold) || (lf & (state::kLVehicle | state::kLMenu | state::kLBinocle)) || a_vars.vehicleId != kNullId ||
				a_lua.ridden != kNullId || menu::Open());
			const bool  press = pad::TakeJumpPress();
			const char* why = press && c.jump ? JumpRefusal(a_vars, a_lua, a_ready) : nullptr;
			const auto  now = ::GetTickCount64();
			using E = logic::JumpTracker::Event;
			const auto ev = g_jump.Update(press && c.jump, why == nullptr, now, a_root.y, (a_lua.flags & state::kLFall) != 0, static_cast<std::uint64_t>(c.jumpMaxAirMs));
			switch (ev) {
			case E::kLift:
				{
					const Vec3 lifted = a_root + Vec3{ 0.0f, c.jumpLift, 0.0f };
					if (hold::Place(lifted + Vec3{ 0.0f, c.warpLift, 0.0f }, logic::MatrixToQuat(a_cam.root))) {
						g_odo.Rebase(lifted);
						++g_jumpsLifted;
						if (g_jumpsLifted <= 10 || g_jumpsLifted % 50 == 0) {
							Log::Info("jump: Snake lifted {:.2f} m at {:.2f},{:.2f},{:.2f} (running {:.1f} m/s) -- kGuestJumping until he lands (jump #{})", c.jumpLift,
								a_root.x, a_root.y, a_root.z, g_odo.Velocity().LengthXZ(), g_jumpsLifted);
						}
					} else {
						g_jump.Abort();
						Log::Warn("jump: the native placement failed -- no jump");
					}
					break;
				}
			case E::kRefused:
				if (++g_jumpsRefused <= 10 || g_jumpsRefused % 50 == 0) {
					Log::Info("jump: not now -- {} (refused #{})", why ? why : (g_jump.Active() ? "a jump is under way" : "just landed"), g_jumpsRefused);
				}
				break;
			case E::kLanded:
				Log::Debug("jump: landed in MGSV (rose {:.2f} m)", g_jump.Rose());
				break;
			case E::kTimedOut:
				++g_jumpsTimedOut;
				Log::Info("jump: no landing seen in MGSV within {} ms (rose {:.2f} m) -- kGuestJumping down anyway", c.jumpMaxAirMs, g_jump.Rose());
				break;
			case E::kNoLift:
				++g_jumpsTimedOut;
				Log::Warn("jump: the lift never showed in Snake's root (rose {:.2f} m in {} ms) -- over", g_jump.Rose(), logic::JumpTracker::kRiseMs);
				break;
			default:
				break;
			}
		}

		// ---- hold in place -------------------------------------------------------------------------
		// Treadmill (Lua Warp, or the native set-position call) past the radius, or the native call every
		// frame (re-backlog M1). The odometer has already taken this frame's motion; a native placement
		// rebases it, a Lua warp is recognised as a teleport when it lands.
		void Hold(const camera::Sample& a_cam, const Vec3& a_root, float a_yaw, const vars::Snapshot& a_vars, const state::LuaView& a_lua, bool a_ready)
		{
			if (const auto req = state::TakeHoldRequest()) {
				auto& m = MutableCfg();
				m.holdMode = static_cast<HoldMode>(req->mode);
				if ((m.holdMode == HoldMode::kNative || m.holdMode == HoldMode::kNativeTreadmill) && !hold::Available()) {
					hold::Init();  // byte checks only: fine on the game thread, so !hold can switch to it live
					g_nativeHoldWarned = false;
				}
				if (req->radius > 0.0f) {
					m.radius = req->radius;
				}
				Log::Info("motion: hold mode {} radius {:.1f} m", req->mode, m.radius);
			}
			const auto& c = Cfg();
			// Fulton anchor (FultonAnchor.cpp, docs/GUEST.md 20.4): Snake beside a sleeping MGSV soldier, or back to the sandbox. The native
			// placement as the treadmill's (camera kept, odometer rebased: Elden Ring sees no jump), the walker moved along with him.
			if (const auto p = fulton::TakePlacement()) {
				bool ok = a_ready && (hold::Available() || hold::Init()) && hold::Available();
				const Vec3 shift = p->pos - a_root;
				ok = ok && hold::Place(p->pos + Vec3{ 0.0f, c.warpLift, 0.0f }, logic::MatrixToQuat(a_cam.root));
				if (ok) {
					g_odo.Rebase(p->pos);
					state::SetCentre(p->centre);
					g_warpAt = ::GetTickCount64();
					++g_warps;
					if (a_lua.buddyId != kNullId && a_lua.buddyPosValid && a_lua.ridden != a_lua.buddyId && a_lua.buddyActive == state::kBuddyTypeWalker) {
						const Vec3 b = a_lua.buddyPos + Vec3{ shift.x, 0.0f, shift.z };
						state::QueueCommand(std::format("owarp {} {} {} {} {}", a_lua.buddyId, b.x, b.y, b.z, a_lua.buddyYawValid ? a_lua.buddyYaw : g_buddyYaw));
						follow::OnShift({ shift.x, 0.0f, shift.z });
					} else if (a_lua.buddyActive == state::kBuddyTypeHorse) {
						// [Buddy] HorseTreadmill: moved along (Horse.cpp); else whistled back
						if (horse::Decide(a_lua, false) != horse::plan::Action::kMoveAlong ||
							!horse::Shift({ shift.x, 0.0f, shift.z }, g_buddyYaw, "fulton anchor")) {
							follow::OnShift({ shift.x, 0.0f, shift.z });
						}
					}
				}
				Log::Info("fulton: Snake {} {:.2f},{:.2f},{:.2f} ({}; {:.1f} m)", ok ? "placed at" : "NOT placed at", p->pos.x, p->pos.y, p->pos.z, p->why,
					shift.LengthXZ());
				fulton::PlacementDone(ok, shift);
				return;
			}
			const auto  centre = state::Centre();
			if (!a_ready || !centre || c.holdMode == HoldMode::kOff || !state::TreadmillEnabled() || state::Parked() || fulton::Anchored()) {
				return;
			}
			if (g_jump.Active()) {
				return;  // in the air from a jump: no treadmill warp, no native hold until he has landed
			}
			const bool onVehicle = (a_lua.flags & state::kLVehicle) || a_vars.vehicleId != kNullId || a_lua.ridden != kNullId;
			const bool busy = (a_lua.flags & (state::kLCqc | state::kLFall | state::kLDemo | state::kLMenu | state::kLDead | state::kLCarry));
			const bool normal = (a_lua.flags & state::kLNormal) || (onVehicle && c.treadmillOnVehicle);
			const bool wantNative = c.holdMode == HoldMode::kNative || c.holdMode == HoldMode::kNativeTreadmill;
			if (wantNative && !hold::Available() && !g_nativeHoldWarned) {
				g_nativeHoldWarned = true;
				Log::Warn("motion: native hold unavailable -- using the Lua treadmill");
			}
			const bool native = wantNative && hold::Available() && !onVehicle;
			const auto riddenId = onVehicle ? (a_vars.vehicleId != kNullId ? a_vars.vehicleId : a_lua.ridden) : kNullId;
			if (riddenId != g_warpVehicle) {
				g_warpVehicle = riddenId;
				g_vehicleMisses = 0;
				if (g_unwarpable != kNullId && riddenId != g_unwarpable) {
					g_unwarpable = kNullId;
				}
			}
			const bool horseRidden = onVehicle && riddenId != kNullId && riddenId == a_lua.buddyId && a_lua.buddyActive == state::kBuddyTypeHorse;
			const auto horseAct = horse::Decide(a_lua, horseRidden);  // [Buddy] HorseTreadmill (Horse.cpp, GUEST.md 8.1a)
			if (horseRidden && horseAct == horse::plan::Action::kNoTreadmill && g_unwarpable != riddenId) {
				// D-Horse answers no SetPosition (Lua, native player placement and Warp while riding: all tried live 2026-10-13).
				g_unwarpable = riddenId;
				Log::Info("treadmill: D-Horse can't be moved -- no treadmill while Snake rides it");
			} else if (horseRidden && horseAct == horse::plan::Action::kMoveRidden && g_unwarpable == riddenId && g_vehicleMisses < 3 &&
			           horse::plan::RiddenJumpOk(a_root, *centre, c.radius)) {
				// Its location object was confirmed during the ride (not 3 missed landings), and Snake is near enough for one jump
				// (a ride that went on far without the treadmill is not thrown back in one go: the horse would land metres off).
				g_unwarpable = kNullId;
				Log::Info("treadmill: D-Horse can be moved now -- treadmill while Snake rides it");
			}
			if (onVehicle && riddenId == g_unwarpable) {
				return;
			}

			if (c.holdMode == HoldMode::kNative && native) {
				if (!normal || busy) {
					return;  // CQC, dives, falls: let MGSV move him; the next held frame pulls him back
				}
				const Vec3 anchor{ centre->x, a_root.y, centre->z };
				if (hold::Place(anchor + Vec3{ 0.0f, c.warpLift, 0.0f }, logic::MatrixToQuat(a_cam.root))) {
					g_odo.Rebase(anchor);
					++g_warps;
				}
				return;
			}

			logic::TreadmillInput in;
			in.root = a_root;
			in.centre = *centre;
			in.velocity = g_odo.Velocity();
			in.radius = c.radius;
			in.allowed = normal && !busy && !g_warpPending;
			in.nowMs = static_cast<double>(::GetTickCount64());
			in.lastWarpMs = static_cast<double>(g_warpAt);
			in.cooldownMs = c.warpCooldownMs;
			const auto d = logic::Treadmill(in);
			if (!d.warp) {
				return;
			}
			// The Warp command takes MGSV's own yaw convention: vars.playerRotY (degrees) in radians, as IH and
			// TppPlayer.Warp do. The root-matrix yaw is the fallback (same convention unless the live test says otherwise).
			const float warpYaw = a_vars.valid && a_vars.rotYDeg != 0.0f ? a_vars.rotYDeg * std::numbers::pi_v<float> / 180.0f : a_yaw;
			std::string cmd;
			bool        placed = false;
			g_warpTargetY = d.target.y;
			// Warp and the native set-position place the character ~0.6-0.8 m *below* the root height they are
			// given: he then climbs out of the ground over ~7 frames and drops into a crouch (VERIFIED live
			// 2026-10-12, both paths). [Motion] WarpLift (0.8 m) lands him exactly on the ground, standing.
			const Vec3 lifted = d.target + Vec3{ 0.0f, c.warpLift, 0.0f };
			if (native && hold::Place(lifted, logic::MatrixToQuat(a_cam.root))) {
				g_odo.Rebase(d.target);
				placed = true;
			} else if (horseRidden && horseAct == horse::plan::Action::kMoveRidden) {
				// D-Horse moved natively *as* the jump (Horse.cpp), its feet onto the centre's ground; Snake comes along in the
				// saddle, and his root's jump is taken as the warp's landing like a vehicle's -- within a few frames, or the horse is
				// put back under him (horse::RiddenMissed). HorseMoveRider also places him by the same jump in this frame.
				if (!horse::plan::RiddenJumpOk(a_root, *centre, c.radius)) {
					g_unwarpable = riddenId;  // re-allowed above once he rides back near the centre
					Log::Info("treadmill: Snake rides D-Horse {:.0f} m from the centre -- no ridden jump that long (until he is back within {:.0f} m)",
						(a_root - *centre).LengthXZ(), horse::plan::RiddenJumpLimit(c.radius));
					return;
				}
				const auto moved = horse::Shift(d.shift, warpYaw, "treadmill (ridden)", d.target.y);
				if (!moved) {
					return;  // nothing moved; repeated failures fall back to no treadmill while riding
				}
				const Vec3 rider = a_root + *moved;  // the saddle went where the horse did
				g_warpTargetY = rider.y;
				// The native placement lands ~0.8 m below the height it is given (WarpLift, as every other placement here).
				if (c.buddyHorseMoveRider && hold::Place(rider + Vec3{ 0.0f, c.warpLift, 0.0f }, logic::MatrixToQuat(a_cam.root))) {
					g_odo.Rebase(rider);
					placed = true;
				} else {
					horse::NoteRiddenJump(g_frame, ::GetTickCount64(), a_root);
				}
				if (c.ridingCameraKeep) {
					cmd = RidingCameraKeep(a_cam);
				}
			} else if (onVehicle) {
				// Move what he rides; he comes along (IH's walker/vehicle SetPosition). By the horizontal shift
				// from where *it* is: Snake's root is on its seat, a metre or more above the walker's feet.
				const auto vehicle = a_vars.vehicleId != kNullId ? a_vars.vehicleId : a_lua.ridden;
				Vec3       at = d.target;
				if (vehicle == a_lua.buddyId && a_lua.buddyPosValid) {
					at = a_lua.buddyPos + Vec3{ d.shift.x, 0.0f, d.shift.z };
				}
				cmd = std::format("owarp {} {} {} {} {}", vehicle, at.x, at.y, at.z, warpYaw);
				// The riding camera stays where it was in the world and turns to look at the vehicle's new place: every
				// warp swung it (VERIFIED live: 6 m -> 166 deg, 75-100 deg once a second while riding; the walker kept
				// its own heading). Its orientation before the warp is asked back right after it (MGSV's own request:
				// rotY = world yaw, rotX = -pitch, degrees; an interpTime of 0 is ignored, 0.02 s works).
				if (c.ridingCameraKeep) {
					if (const auto keep = RidingCameraKeep(a_cam); !keep.empty()) {
						cmd += ";" + keep;
					}
				}
			} else {
				// Warp, not WarpAndWaitBlock: no block wait in the already-streamed sandbox (re-backlog M1).
				cmd = std::format("warp {} {} {} {}", lifted.x, lifted.y, lifted.z, warpYaw);
			}
			// Keep the buddy where it was relative to Snake, or Elden Ring would see it jump by the radius.
			if (c.treadmillMovesBuddy && a_lua.buddyId != kNullId && a_lua.buddyPosValid && a_lua.ridden != a_lua.buddyId &&
				a_lua.buddyActive == state::kBuddyTypeHorse) {
				// D-Horse answers no SetPosition: moved by its location object in this same frame ([Buddy] HorseTreadmill, Horse.cpp),
				// else whistled back to Snake at once.
				if (horseAct != horse::plan::Action::kMoveAlong || !horse::Shift(d.shift, a_lua.buddyYawValid ? a_lua.buddyYaw : g_buddyYaw, "treadmill")) {
					follow::OnShift(d.shift);
				}
			} else if (c.treadmillMovesBuddy && a_lua.buddyId != kNullId && a_lua.buddyPosValid && a_lua.ridden != a_lua.buddyId &&
			           a_lua.buddyActive != state::kBuddyTypeDog && a_lua.buddyActive != state::kBuddyTypeQuiet) {
				// (D-Dog and Quiet ignore SetPosition -- live 2026-10-14, D-Dog stayed put -- and its Vector3 form sends D-Walker to the
				// world origin: they are left to MGSV, which brings them back to Snake by itself.)
				const Vec3 b = a_lua.buddyPos + d.shift;
				cmd += std::format("{}owarp {} {} {} {} {}", cmd.empty() ? "" : ";", a_lua.buddyId, b.x, b.y, b.z, a_lua.buddyYawValid ? a_lua.buddyYaw : g_buddyYaw);
				follow::OnShift(d.shift);  // and where it is walking to
			}
			if (!cmd.empty()) {
				state::QueueCommand(cmd);
			}
			g_warpAt = ::GetTickCount64();
			g_warpPending = !placed;
			++g_warps;
			Log::Debug("treadmill: {}{} (shift {:.2f},{:.2f})", placed ? "native placement; " : "", cmd, d.shift.x, d.shift.z);
		}

		std::uint16_t Stance(std::uint32_t a_flags)
		{
			if (a_flags & state::kLCrawl) {
				return 2;
			}
			return (a_flags & state::kLSquat) ? 1 : 0;
		}
	}

	void OnPlayerCamera(const camera::Sample& a_sample)
	{
		++g_frame;
		link::NoteGameTick();
		hold::ServicePending();
		skeleton::ObserveOwner(a_sample.owner, g_frame);
		if (!g_qpcFreq) {
			LARGE_INTEGER f;
			::QueryPerformanceFrequency(&f);
			g_qpcFreq = f.QuadPart;
		}
		const auto now = Qpc();
		framepace::OnGameFrame(now);
		const double dt = g_lastQpc ? static_cast<double>(now - g_lastQpc) / static_cast<double>(g_qpcFreq) : 0.0;
		g_lastQpc = now;

		const auto& c = Cfg();
		const auto  v = vars::Read();
		const auto  lua = state::Lua();
		const Vec3  root = logic::MatrixTranslation(a_sample.root);
		const float yaw = logic::MatrixYaw(a_sample.root);
		const bool  bootReady = state::CurrentBoot() == state::Boot::kReady;
		const bool  luaFresh = lua.valid && ::GetTickCount64() - lua.tickMs < 1000;
		const bool  ready = bootReady && luaFresh && lua.InGame();

		state::SetFrame({ true, g_frame, ::GetTickCount64(), root, yaw, { a_sample.pos[0], a_sample.pos[1], a_sample.pos[2] }, g_odo.Velocity() });
		{
			const float at[3] = { root.x, root.y, root.z };
			capture::NoteFrame(g_frame, at, yaw, a_sample.pos);  // the draw anchor (Capture.cpp)
		}

		// A new host session (or our first frame) starts a new odometer epoch; so does the boot sequence
		// reaching Ready (its warps are not motion the host should replay). Brief not-ready spells (MGSV's
		// pause menu stops Lua updates) keep the epoch.
		const auto gen = link::AttachGeneration();
		if (gen != g_attachGen || (bootReady && !g_wasReady)) {
			const bool newSession = gen != g_attachGen;
			g_attachGen = gen;
			g_odo.Reset(root);
			g_ammo.Reset();
			g_buddyAmmoPrimed = false;
			g_warpPending = false;
			Log::Info("motion: odometer epoch {} at {:.2f},{:.2f},{:.2f}", g_odo.Epoch(), root.x, root.y, root.z);
			if (newSession && state::Parked()) {
				state::SetParked(false);  // a new host session starts with Snake in control (it says so if not)
				pad::SetParked(false);
				Log::Info("tarnished mode: off (new host session)");
			}
		}
		g_wasReady = bootReady;

		const bool warpWindow = g_warpPending && ::GetTickCount64() - g_warpAt < 3000;
		if (g_warpPending && !warpWindow) {
			g_warpPending = false;
			if (g_warpVehicle != kNullId && ++g_vehicleMisses >= 3 && g_unwarpable != g_warpVehicle) {
				g_unwarpable = g_warpVehicle;
				Log::Warn("treadmill: what Snake rides ({:#x}) can't be moved -- no treadmill while he rides it (Snake can leave the sandbox)",
					g_warpVehicle);
			} else if (g_unwarpable != g_warpVehicle || g_warpVehicle == kNullId) {
				Log::Warn("treadmill: warp didn't land within 3 s");
			}
		}
		// A warp lands where we sent him (WarpLift puts him on his feet), but MGSV's root still settles a few cm
		// over the next frames, and the pre-warp root bobbed with his stride: vertical is held at the warp's
		// target height for 20 frames after the landing, so neither shows up as odometer height.
		Vec3 odoRoot = root;
		if (g_frame < g_settleUntil) {
			odoRoot.y = g_settleY;
		}
		if (warpWindow && g_odo.Primed() && (root - g_lastRoot).Length() > std::min(0.75f, c.teleportJump)) {
			// The landing frame: the jump stays out, but this frame's own running motion counts (else the
			// puppet stalls one frame per warp). MGSV's smoothed velocity is the best guess of it.
			g_settleY = odoRoot.y = g_warpTargetY;
			g_odo.Rebase(odoRoot - g_odo.Velocity() * static_cast<float>(std::clamp(dt, 0.0, 0.1)));
			g_warpPending = false;
			g_settleUntil = g_frame + 20;
			++g_landings;
			g_vehicleMisses = 0;
			horse::RiddenLanded();
		}
		if (horse::RiddenMissed(g_frame, ::GetTickCount64(), root)) {
			g_warpPending = false;  // a ridden D-Horse jump Snake didn't follow: the horse is back under him (Horse.cpp), nothing landed
		}
		g_odo.Update(odoRoot, dt, warpWindow && g_warpPending, c.teleportJump, c.velocitySmoothing);
		g_lastRoot = root;

		// Riding a buddy whose GameObject gives no position (D-Horse): its skin is the one under Snake (Skeleton.cpp).
		if (lua.buddyId != kNullId && (lua.ridden == lua.buddyId || v.vehicleId == lua.buddyId) && lua.buddyActive != state::kBuddyTypeWalker) {
			skeleton::HintBuddy(root, 1.5f, 500);
		}

		pad::SetSource(bootReady || c.padDuringBoot ? pad::Source::kHost : pad::Source::kNeutral);
		g_diedNow = g_respawnedNow = false;
		HostEvents(v, root);
		RegenGate(v);
		{
			proto::HostState hs{};
			const bool       haveHost = link::ReadHostState(hs);
			death::Update({ .ready = ready,
				.died = g_diedNow,
				.hostDead = haveHost && (hs.flags & proto::kHostPlayerDead),
				.respawned = g_respawnedNow,
				.hostAlive = link::HostAlive(),
				.luaFlags = lua.flags,
				.riding = (lua.flags & state::kLVehicle) || v.vehicleId != kNullId || lua.ridden != kNullId });
		}
		if (ready) {
			Shots(v, a_sample, lua);
		} else {
			g_ammo.Reset();  // counters change across loads/menus without shots
			g_buddyAmmoPrimed = false;
		}
		combat::ObserveOwner(a_sample.owner, g_frame, lua.buddyActive >= 0 && lua.buddyId != kNullId);
		{
			const float at[3] = { root.x, root.y, root.z };
			blast::Observe(ready, g_frame, at);
		}
		// Throws and placed explosives (docs/GUEST.md 18.8): the projectiles' lifetimes, the arc Snake aims, his mines.
		const auto support = v.supportIndex < v.supportWeapons.size() ? v.supportWeapons[v.supportIndex] : std::uint16_t(0);
		const auto aim = throwing::Observe(ready, g_frame, (v.buttons & addr::kPadHold) != 0, v.inventorySlot, support);
		placed::Update(ready, g_frame, root);
		// C4's detonator (C4.cpp): MGSV's own button with C4 in hand, on foot, no menu.
		c4::Update(ready, v.buttons, v.inventorySlot, support,
			!(lua.flags & (state::kLVehicle | state::kLMenu | state::kLDead)) && v.vehicleId == kNullId && lua.ridden == kNullId && !menu::PauseOpen());
		if (ready) {
			const float at[3] = { root.x, root.y, root.z };
			combat::PollMelee(at, yaw);
		}
		// CQC holds and throws (ROADMAP 2.2, Cqc.h): after PollMelee, which reads the CQC state it reports with.
		cqc::Update({ .ready = ready,
			.riding = (lua.flags & state::kLVehicle) || v.vehicleId != kNullId || lua.ridden != kNullId,
			.luaFlags = lua.flags,
			.buttons = v.buttons,
			.frame = g_frame,
			.root = { root.x, root.y, root.z },
			.yaw = yaw });
		horse::Update(lua, ::GetTickCount64());  // [Buddy] HorseTreadmill: D-Horse's location object, before the treadmill may move it
		Jump(a_sample, root, v, lua, ready);
		Hold(a_sample, root, yaw, v, lua, ready);

		proto::GuestState s{};
		s.frame = g_frame;
		s.qpc = now;
		std::uint32_t f = 0;
		const std::uint32_t lf = lua.flags;
		if (ready) {
			f |= proto::kGuestReady;
		}
		if (!lua.InGame() || !(lf & state::kLPrepared)) {
			f |= proto::kGuestLoading;
		}
		if (lf & state::kLDead) {
			f |= proto::kGuestDead;
		}
		if ((lf & state::kLMenu) || menu::PauseOpen()) {
			f |= proto::kGuestInMenu;
		}
		if (lf & state::kLDemo) {
			f |= proto::kGuestCutscene;
		}
		const bool riding = (lf & state::kLVehicle) || v.vehicleId != kNullId || lua.ridden != kNullId;
		if (riding) {
			f |= proto::kGuestOnVehicle;
		}
		if (v.buttons & addr::kPadHold) {
			f |= proto::kGuestAiming;
		}
		if (lf & state::kLDash) {
			f |= proto::kGuestSprint;
		}
		if (lf & state::kLStand) {
			f |= proto::kGuestStanding;
		}
		if (lf & state::kLSquat) {
			f |= proto::kGuestCrouching;
		}
		if (lf & state::kLCrawl) {
			f |= proto::kGuestProne;
		}
		if (lf & state::kLCqc) {
			f |= proto::kGuestCqc;
		}
		if (g_jump.Active()) {
			f |= proto::kGuestJumping;  // v14: from the [Jump] press until he lands in MGSV (the host jumps the puppet on its rising edge)
		}
		// First person: MGSV's own SUBJECT/BINOCLE status, or riding with the camera in the cockpit (RB while riding D-Walker:
		// MGSV sets no status for it; its camera sits ~0.6 m in front of the walker's root, ~5 m behind in third person).
		if ((lf & (state::kLSubject | state::kLBinocle)) || (riding && RidingFirstPerson(a_sample, root))) {
			f |= proto::kGuestFirstPerson;
		}
		// v8: inside a cardboard box (PlayerStatus CBOX), and one with a poster (docs/GUEST.md 18.9). Keeping still or moving
		// in it shows in velocity (MGSV's box shuffle ~1.3 m/s, still 0).
		if (lf & state::kLCbox) {
			f |= proto::kGuestInBox;
			if (lf & state::kLPoster) {
				f |= proto::kGuestBoxPoster;
			}
		}
		// v10: night-vision goggles on (MGSV's own effect only darkens Elden Ring's frame: the host recreates it)
		if (lf & state::kLNvg) {
			f |= proto::kGuestNvg;
		}
		capture::hud::SetNightVision((lf & state::kLNvg) != 0);
		// v13: Snake looks through his binoculars (RB held): MGSV's PlayerStatus BINOCLE, the status kGuestFirstPerson already
		// reads (GuestScript PR.Flags). The host marks the Elden Ring characters he looks at (er/Marking.h). UNVERIFIED live.
		if (lf & state::kLBinocle) {
			f |= proto::kGuestBinoculars;
		}
		// v13: Snake dives (X): the host may hop its puppet over a gap or onto a ledge (Dive.h, roadmap 2.9)
		if (dive::Observe(ready, riding, lf, g_odo.Velocity().LengthXZ())) {
			f |= proto::kGuestDiving;
		}
		if (aim.valid) {
			f |= proto::kGuestThrowAim;  // v6: the arc MGSV draws, for the host's own
			std::copy_n(aim.pos, 3, s.aimThrowPos);
			std::copy_n(aim.vel, 3, s.aimThrowVel);
			s.aimThrowKind = aim.kind;
		}
		s.flags = f;
		s.missionCode = v.missionCode;
		s.locationCode = v.locationCode;
		s.odoEpoch = g_odo.Epoch();
		const auto& odo = g_odo.Odo();
		std::copy(odo.begin(), odo.end(), s.odo);
		SetV(s.rootPos, root);
		s.rootYaw = yaw;
		SetV(s.velocity, g_odo.Velocity());
		std::copy_n(a_sample.pos, 3, s.camPos);
		std::copy_n(a_sample.quat, 4, s.camQuat);
		// MGSV's own projection when capture has seen it (exact); else the focal-length estimate.
		const float projFov = capture::ProjectionFovY();
		float fov = c.fovOverrideDeg > 0.0f ? c.fovOverrideDeg * std::numbers::pi_v<float> / 180.0f
		          : projFov > 0.0f        ? projFov
		                                  : logic::FocalToFovY(a_sample.focalMm, c.filmHeightMm);
		if (!(fov > 0.0f)) {
			fov = 60.0f * std::numbers::pi_v<float> / 180.0f;
		}
		s.camFovY = fov;
		s.camNear = c.nearPlane;
		s.life = v.life;
		s.lifeMax = v.lifeMax;
		s.stamina = v.stamina;
		s.stance = Stance(lf);
		// The weapon in hand: slots 0-2 index weapons[] / ammoInWeapons[], 3 is the selected support item (VERIFIED live). The
		// stock is MGSV's count for it: all the rounds left, the magazine's included (it drops with every shot; full = 210 for
		// the SVG-76: 30 + 180), a support item's count (grenades left).
		const auto slot = v.inventorySlot;
		if (slot == addr::kSupportSlotInHand) {
			s.weaponId = v.supportIndex < v.supportWeapons.size() ? v.supportWeapons[v.supportIndex] : 0;
			s.ammoInClip = 0;
			s.ammoStock = v.supportIndex < 8 ? v.stockCounts[6 + v.supportIndex] : 0;
		} else {
			s.weaponId = slot < 3 ? v.weapons[slot] : 0;
			s.ammoInClip = slot < 3 ? v.ammo[slot] : 0;
			s.ammoStock = slot < 3 ? v.stockCounts[slot * 2] : 0;
		}
		s.buddyType = v.buddyType;
		s.vehicleId = riding ? (v.vehicleId != kNullId ? v.vehicleId : lua.ridden) : kNullId;
		s.phase = v.phase;
		if (v.phase != g_lastPhase && ready) {
			// MGSV's alert phase (vars.playerPhase: 0 sneak .. 3 combat) belongs at 0 in the sandbox: anything else means an MGSV
			// soldier saw Snake (live 2026-10-14 with `!proxy on` and the real Elden Ring: phase 3 and a riot-shield reinforcement
			// shooting Snake on the apron). Logged so it is found.
			if (v.phase > 0 && g_lastPhase == 0) {
				Log::Warn("alert: MGSV's alert phase {} -> {} -- an MGSV soldier saw Snake{}", g_lastPhase, v.phase,
					Cfg().proxies ? " ([Proxies] are on: their slot soldiers are the likely ones)" : "");
			} else {
				Log::Info("alert: MGSV's alert phase {} -> {}", g_lastPhase, v.phase);
			}
			g_lastPhase = v.phase;
		}

		// Buddy (D-Walker first).
		std::uint32_t bf = 0;
		if (lua.buddyActive >= 0 && lua.buddyId != kNullId) {
			bf |= proto::kBuddyPresent;
		}
		if (lua.buddyPosValid && luaFresh) {
			bf |= proto::kBuddyPosValid;
			SetV(s.buddyPos, lua.buddyPos);
		}
		if (lua.buddyId != kNullId && (lua.ridden == lua.buddyId || (riding && v.vehicleId == lua.buddyId))) {
			bf |= proto::kBuddyRidden;
		}
		if (state::CurrentBoot() == state::Boot::kBuddy) {
			bf |= proto::kBuddyLoading;
		}
		if (state::BuddyMissing()) {
			bf |= proto::kBuddyMissing;
		}
		if (lua.buddyPosValid) {
			const Vec3 d = lua.buddyPos - g_buddyLast;
			if (!g_buddyLastValid) {
				g_buddyYaw = yaw;  // it was called facing Snake's way
			} else if (d.LengthXZ() > 0.02f && d.LengthXZ() < 2.0f) {
				g_buddyYaw = std::atan2(d.x, d.z);
			}
			g_buddyLast = lua.buddyPos;
			g_buddyLastValid = true;
		} else {
			g_buddyLastValid = false;
		}
		if (bf & proto::kBuddyRidden) {
			g_buddyYaw = yaw;  // ridden: it faces where Snake's root faces
		}
		s.buddyFlags = bf;
		capture::NoteBuddy((bf & proto::kBuddyPosValid) != 0, s.buddyPos);
		s.buddyObjectId = lua.buddyId;
		s.buddyAmmo = v.buddyGearAmmo[0];
		s.buddyYaw = lua.buddyYawValid ? lua.buddyYaw : g_buddyYaw;
		{
			std::scoped_lock lock(g_publishLock);
			link::PublishState(s);
			g_last = s;
			g_lastFrameMs = ::GetTickCount64();
		}
		// ActorProxy v0 ([Proxies] Enabled, docs/GUEST.md 20): the pool soldiers to the nearest hostile Elden Ring characters, with
		// the treadmill offset of exactly what was just published (rootPos - odo).
		if (c.ProxiesRun()) {
			const auto centre = ready ? state::Centre() : std::nullopt;
			if (centre) {
				const Vec3 buddy{ s.buddyPos[0], s.buddyPos[1], s.buddyPos[2] };
				proxy::OnFrame(root, s.odo, (bf & proto::kBuddyPosValid) ? &buddy : nullptr, *centre, ::GetTickCount64());
			} else {
				proxy::Suspend();  // no OnFrame: nothing may act on the last placements (review 2026-10-14)
			}
		}

		if (g_frame % 30 == 0) {
			std::scoped_lock lock(g_describeLock);
			g_describe = std::format("frame {} ready {} flags {:#x} lua {:#x} root {:.2f},{:.2f},{:.2f} yaw {:.1f} (vars {:.1f}) odo {:.2f},{:.2f},{:.2f} epoch {} "
			                         "teleports {} warps {} landed {} life {}/{} (regen gate {}, {} capped) fov {:.1f} focal {:.1f} pose {} pad polls {} buddy {:#x} id {} ammo {} "
			                         "jumps {} (refused {}, no landing seen {}){}",
				g_frame, ready, f, lf, root.x, root.y, root.z, yaw * 57.2958f, v.rotYDeg, odo[0], odo[1], odo[2], g_odo.Epoch(), g_odo.Teleports(), g_warps, g_landings, v.life,
				v.lifeMax, g_regenGate ? std::format("from {:.0f}, {:.1f} s", g_regenBase, static_cast<double>(::GetTickCount64() - g_regenHitAt) / 1000.0) : std::string("off"),
				g_regenCapped, fov * 57.2958f, a_sample.focalMm, skeleton::PublishedFrames(), pad::Polls(), bf, lua.buddyId, s.buddyAmmo, g_jumpsLifted, g_jumpsRefused,
				g_jumpsTimedOut, g_jump.Active() ? " -- in the air" : "");
		}
	}

	void PublishStalled(std::uint64_t a_nowMs, bool a_ready)
	{
		std::scoped_lock lock(g_publishLock);
		// Only a ready sandbox whose frames stopped a moment ago (not a load: the boot sequence tells those apart and
		// says "loading" itself), and only while one of MGSV's menus is open -- that is what stops them.
		if (!a_ready || !g_lastFrameMs || a_nowMs - g_lastFrameMs < 100 || !(g_last.flags & proto::kGuestReady)) {
			return;
		}
		const bool open = menu::Open();
		if (!open && !(g_last.flags & proto::kGuestInMenu)) {
			return;  // stalled without a menu: leave the last state as it is (the boot sequence reports the stall)
		}
		proto::GuestState s = g_last;
		s.flags = open ? (s.flags | proto::kGuestInMenu) : (s.flags & ~proto::kGuestInMenu);
		s.flags &= ~proto::kGuestLoading;
		s.qpc = Qpc();
		link::PublishState(s);
		g_last = s;
		link::NoteGameTick();  // MGSV is alive (its Lua runs): keep the heartbeat while no frame comes
		++g_stalledPublished;
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_describeLock);
		return g_describe;
	}
}
