#pragma once
// ActorProxy v0 (docs/ROADMAP.md 2.1, docs/GUEST.md §20): the sandbox's disabled MGSV soldiers become inert position proxies
// of the nearest hostile Elden Ring characters, so MGSV's own aim assist, marking and buddies have something to target.
// The host keeps damage authority: Snake's rounds still reach Elden Ring only through ShotSolver (Combat.cpp); nothing the
// soldiers suffer in MGSV is ever reported.
//
// [Proxies] Enabled = false by default. UNVERIFIED live: the whole runtime half (does the shared location manager's set-position
// move a disabled TppSoldier2, does aim assist follow it, does a parked soldier stay parked). The pure half (this header's
// `plan` namespace, ActorProxy.cpp) is unit-tested offline (tests/test_guest_actorproxy.cpp); the game glue is in
// ActorProxyGame.cpp.
//
// Coordinates: the host maps each Elden Ring character through the same anchor it uses for Snake (er/Actors.cpp:
// pos = Pm + DirToMgsv(erPos - Pe), Pm = the GuestState rootPos it consumed), so ActorRecord.pos is MGSV world *as of that
// GuestState*. The treadmill moves Snake (and with him MGSV's world under ER's enemies) by whole metres between two tables;
// the guest takes its own warps back out with the treadmill offset k = root - odometer (OffsetHistory).

#include "Logic.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace phantom::proto
{
	struct ActorRecord;
}

namespace phantom::guest::proxy
{
	namespace plan
	{
		using logic::Vec3;

		// Where a proxy stands vertically (docs/GUEST.md 20.6). kEnemy: at the Elden Ring character's height relative to Snake
		// (v0/v1). kGround: on the sandbox's ground (x/z still the character's) -- with the real Elden Ring its enemies stand 1-5 m
		// above and below Snake, MGSV's flat apron can't hold a soldier there and the read-back flapped (live 2026-10-14).
		enum class Height : std::uint8_t
		{
			kEnemy,
			kGround,
		};
		// "ground" / "enemy" (anything else: a_fallback).
		Height ParseHeight(std::string_view a_text, Height a_fallback);
		// A proxy's feet for a target at a_target: its x/z, and its y (kEnemy) or a_groundY (kGround).
		Vec3 ProxyFeet(const Vec3& a_target, float a_groundY, Height a_mode);

		struct Settings
		{
			int   maxProxies{ 4 };            // soldiers used as proxies (the nearest hostiles); the rest of the pool is parked
			float maxDistance{ 60.0f };       // only hostiles this close to Snake (m, horizontal) get a proxy
			float snakeClearance{ 3.5f };     // a proxy closer than this to Snake's root is parked instead (capture owner: 2.5 m)
			float buddyClearance{ 5.5f };     // ... or than this to the buddy (capture owner: 4 m)
			float parkDepth{ 100.0f };        // parked soldiers: this far below the sandbox centre
			float parkSpacing{ 2.0f };        // ... spread on a line along +X so they don't overlap
			float minMove{ 0.02f };           // re-place a proxy only when its target moved this much (m) or turned 1 deg ...
			int   refreshFrames{ 30 };        // ... or every this many frames anyway
			int   tableLagMs{ 33 };           // a new ActorTable was built from the GuestState about this long before it arrived
			Height height{ Height::kEnemy };  // [Proxies] Height (kGround only with full proxies; the Fulton-only path keeps kEnemy)
			// The dedicated soldier's height: D-Dog's on the sandbox ground (the dog has to reach him), Quiet's stand-in at the enemy's
			// ([Quiet] StandInHeight: her binoculars look up at him, docs/research/quiet.md §3.4).
			Height dedicatedHeight{ Height::kGround };
		};

		// One hostile Elden Ring character, already corrected into MGSV world as of now.
		struct Target
		{
			std::uint64_t handle{ 0 };
			Vec3          pos;
			float         yaw{ 0 };
			float         distance{ 0 };  // horizontal, from Snake's root
			bool          asleep{ false }; // kActorAsleep: its proxy sleeps and stays even beside Snake (MGSV's Fulton wants him there)
			bool          dedicated{ false };  // D-Dog's order target (Dedicate): first to get a soldier, on the ground, never parked for the
			                                   // buddy's clearance (the dog has to reach him) nor for Snake's (review 0.8.0: Snake walking up
			                                   // to the enemy the dog holds -- to CQC or fulton him -- disabled the soldier, ended MGSV's
			                                   // bite and so the host's hold; the pick keeps CQC-range enemies out by target_min instead)
		};

		// D-Dog's target first (stable otherwise: nearest first, as SelectTargets sorted them); marks it dedicated. Nothing if absent.
		void PutDedicatedFirst(std::vector<Target>& a_targets, std::uint64_t a_handle);

		// The treadmill offset k = root - odometer over the last ~2 s. It changes only when the guest moves Snake (a
		// treadmill warp, a native placement, a teleport left out of the odometer); between those it is constant.
		class OffsetHistory
		{
		public:
			void Clear() { m_count = 0; }
			// Records k at a_nowMs (call once per frame, times increasing).
			void Record(std::uint64_t a_nowMs, const Vec3& a_k);
			// k at a_tMs: the last record at or before it (the oldest one if a_tMs is older still); zero if none.
			[[nodiscard]] Vec3 At(std::uint64_t a_tMs) const;
			[[nodiscard]] Vec3 Latest() const;
			[[nodiscard]] bool Empty() const { return m_count == 0; }

		private:
			struct Entry
			{
				std::uint64_t t;
				Vec3          k;
			};
			static constexpr std::size_t kSize = 128;
			std::array<Entry, kSize> m_ring{};
			std::size_t              m_head{ 0 };   // next write
			std::size_t              m_count{ 0 };
		};

		// k = root - odometer, in doubles first (the odometer runs to kilometres; floats would lose centimetres).
		Vec3 TreadmillOffset(const Vec3& a_root, const double a_odo[3]);

		// Elden Ring's walls stop the puppet where MGSV's flat sandbox lets Snake run on (the "slip": what the odometer asked for
		// minus what Elden Ring did, in MGSV's axes). The guest isn't told -- but the host maps every Elden Ring character into
		// MGSV's world through Snake's place in both games, so a character standing still in Elden Ring moves in odometer space
		// (his MGSV place minus k) by exactly the slip. From Begin() on, each new table: every character's move since then; the
		// slip so far is their median when most of them agree (within a_agree), at least 2 do, and it is no longer than Snake's own
		// path meanwhile (a wall takes at most what he ran) -- and each change of it no longer than his path since the last level
		// taken, never against his motion: characters walking together (a patrol) while Snake stands or runs another way are not
		// the slip. One character, a table torn or around a treadmill jump add nothing (the last level holds; the baseline waits for
		// a settled table); a character first seen later counts from then on as still; everyone jumping at once by more than Snake's
		// MGSV motion between two tables is Elden Ring teleporting Snake (counted anew from there, the level kept). Used to keep
		// a waiting D-Dog where Elden Ring saw it wait (Dog.cpp; docs/research/ddog.md §9).
		struct SlipSample
		{
			std::uint64_t handle{ 0 };
			Vec3          q;  // the character's MGSV place minus k at the table's time
		};
		class SlipObserver
		{
		public:
			void Begin() { m_pending = true; }
			void Stop() { m_begun = m_pending = false; }
			// One new table. a_settled = false (k changed lately: a treadmill jump, a placement) keeps the level as it is, and the
			// baseline waits for the first settled table. A new level is taken only when it changed by no more than Snake's path since
			// the last one taken (+0.1 m) and not against his motion: walls only take back what he just ran.
			bool Update(const std::vector<SlipSample>& a_samples, const Vec3& a_odo, bool a_settled, float a_agree = 0.15f);
			[[nodiscard]] Vec3          Level() const { return m_level; }  // the slip since Begin (horizontal)
			[[nodiscard]] bool          Active() const { return m_begun; }
			[[nodiscard]] float         Path() const { return m_path; }
			[[nodiscard]] std::uint64_t Taken() const { return m_taken; }
			[[nodiscard]] std::uint64_t Refused() const { return m_refused; }
			[[nodiscard]] std::uint64_t Rebased() const { return m_rebased; }  // teleports seen (the characters counted anew)

		private:
			std::vector<SlipSample> m_base;  // each character's q when first seen (minus the level then)
			bool                    m_pending{ false }, m_begun{ false };
			Vec3                    m_level, m_lastOdo;
			float                   m_path{ 0.0f };
			float                   m_levelPath{ 0.0f };  // m_path when the level was last taken
			Vec3                    m_levelOdo;           // the odometer then
			Vec3                    m_prevM;              // the last settled table's median move (a teleport: it jumps)
			float                   m_prevPath{ 0.0f };   // m_path at that table
			bool                    m_havePrev{ false };
			std::uint64_t           m_rebased{ 0 };
			std::uint64_t           m_taken{ 0 }, m_refused{ 0 };
		};

		// The hostile, living, finite records of a table, corrected for the treadmill warps since the table was built
		// (pos + kNow - kThen), within maxDistance of Snake, nearest first.
		std::vector<Target> SelectTargets(const proto::ActorRecord* a_records, std::uint32_t a_count, const Vec3& a_snakeRoot, const Vec3& a_kThen,
			const Vec3& a_kNow, const Settings& a_s);

		// What one pool soldier should do this frame.
		struct Slot
		{
			std::uint16_t id{ 0xFFFF };   // MGSV GameObject id (TppSoldier2)
			std::uint64_t handle{ 0 };    // the Elden Ring character it stands for; 0 = parked
			Vec3          pos;            // where it should be (MGSV world; on the sandbox ground with Height kGround)
			Vec3          real;           // where its character is (MGSV world, unflattened): aim correction, kGEvFulton's v0
			float         yaw{ 0 };
			bool          proxy{ false }; // true: standing in for `handle` at pos; false: parked
		};

		// Keeps each Elden Ring character on the soldier it already had (no swaps: markers and aim assist would jump), hands
		// free soldiers to new characters nearest first, parks the rest. A target inside snakeClearance / buddyClearance keeps
		// its soldier but is parked for the frame. a_slots: one per pool soldier, updated in place (ids are kept).
		void Assign(std::vector<Slot>& a_slots, const std::vector<Target>& a_targets, const Vec3& a_snakeRoot, const Vec3* a_buddy, const Vec3& a_centre,
			const Settings& a_s);

		// Where pool soldier a_index waits while it isn't a proxy.
		Vec3 ParkPosition(const Vec3& a_centre, std::size_t a_index, const Settings& a_s);

		// Whether a placed soldier must be written again (moved/turned enough, or the periodic refresh).
		bool NeedsPlace(const Vec3& a_last, float a_lastYaw, const Vec3& a_want, float a_wantYaw, std::uint64_t a_framesSince, const Settings& a_s);

		// The pool as PhantomRing.ProxyPool returns it: "soldiers;hostages", comma lists of GameObject ids ("1024,1025;2048").
		struct Pool
		{
			std::vector<std::uint16_t> soldiers;
			std::vector<std::uint16_t> hostages;
		};
		Pool ParsePool(std::string_view a_text);

		// Whether the pool may be taken at all: nullptr if so, else why not (logged; no pool = OnFrame stays inactive).
		// PhantomRing.ProxyPool makes every soldier damage-immune, and only PR.DisableSoldiers ([Boot] DisableSoldiers = true)
		// takes their AI out: with it false the slot soldiers would be live, unkillable AI placed beside Snake every frame
		// (review 2026-10-14).
		const char* PoolRefusal(bool a_disableSoldiers);

		// Capture (Capture.cpp): a G-buffer draw at a_drawPos belongs to a proxy (and never to Snake or the buddy) when it is
		// within a_radius (horizontal, and 2.5 m vertically) of a placed proxy and nearer to it than to Snake's root.
		bool NearProxy(const Vec3& a_drawPos, const Vec3& a_snakeRoot, const Vec3* a_proxies, std::size_t a_count, float a_radius);

		// MGSV's Auto-Aim Correction may be on only while every soldier and hostage the pool knows is placed (a proxy or
		// parked, read back where it was put): a disabled soldier left standing pulls the aim off the Elden Ring target
		// (docs/GUEST.md 18.13). a_savesGuarded: the option is never written while MGSV could save it.
		bool AutoAimWanted(bool a_enabled, bool a_autoAimForProxies, bool a_savesGuarded, std::size_t a_pool, std::size_t a_placed, std::size_t a_unplaced);

		// ---- inert full proxies (docs/GUEST.md 20.6) ----------------------------------------------------------------------
		// The read-back: a placed proxy stands where it was put when its feet are within a_xzTol horizontally and a_yTol vertically.
		// On the ground (kGround) MGSV's own ground under a far proxy may differ from the sandbox centre's by a step or a kerb: y
		// gets its own, looser tolerance so a soldier settling on MGSV's ground doesn't flap auto-aim off and on.
		bool ReadBackOk(const Vec3& a_put, const Vec3& a_at, float a_xzTol, float a_yTol);

		// The location object a proxy holds is still his: TppSoldier2's position cache (refreshed slower for far soldiers: ~1 m off
		// at 33 m, live) is within a_tol (horizontal) of the object's feet. The ~30 location objects are pooled per realized
		// character: after an unrealize the same object may carry another soldier (a reinforcement) that must not be moved.
		bool OwnsLocation(const Vec3& a_cache, const Vec3& a_feet, float a_tol);

		// Aim correction for flattened proxies: a round whose ray (a_origin + t * a_dir, t in (0, a_range]) passes within a_radius of
		// a placed proxy's body (its feet to a_bodyHeight above) is turned to the same point of the real character (+ real - placed),
		// so MGSV's aim assist onto a proxy on the ground still hits an Elden Ring enemy standing 3 m higher. The nearest such body
		// along the ray wins. A ray that meets any character's REAL body (same radius and height) is left alone: the camera is MGSV's,
		// so a round aimed by hand at the enemy (or at an enemy behind another's stand-in) already goes where Elden Ring has him.
		// a_dir stays unit length. Returns the index of the proxy it was turned to, -1 = unchanged.
		struct Body
		{
			Vec3 placed;  // feet where MGSV has the proxy
			Vec3 real;    // feet where the character really is (MGSV world)
		};
		int CorrectShot(const Vec3& a_origin, Vec3& a_dir, const Body* a_bodies, std::size_t a_count, float a_radius, float a_bodyHeight, float a_range);

		// MGSV's alert phase (vars.playerPhase, 0 sneak .. 3 combat) while full proxies are in the world. It belongs at 0: at the
		// first non-zero read every soldier outside the slots is disabled again (sweep: a reinforcement or a soldier the mission
		// re-enabled; repeated every sweepEveryMs while it stays up, since MGSV's reinforcements arrive some time into an alert) and
		// the command posts are set back to sneak (calm, repeated every calmEveryMs); if it is still up after giveUpMs the full
		// proxies turn themselves off (the Fulton-only proxies stay). Reset() when full proxies are turned on again.
		struct PhaseSettings
		{
			std::uint64_t calmEveryMs{ 1000 };
			std::uint64_t sweepEveryMs{ 3000 };
			std::uint64_t giveUpMs{ 20000 };
		};
		struct PhaseStep
		{
			bool calm{ false };
			bool sweep{ false };
			bool giveUp{ false };
		};
		class PhaseGuard
		{
		public:
			PhaseStep Update(int a_phase, std::uint64_t a_nowMs, const PhaseSettings& a_s);
			void      Reset() { *this = {}; }
			[[nodiscard]] std::uint32_t Alerts() const { return m_alerts; }
			[[nodiscard]] bool          GaveUp() const { return m_gaveUp; }

		private:
			std::uint64_t m_since{ 0 };   // first non-zero read of this alert (0 = phase 0)
			std::uint64_t m_calmAt{ 0 };
			std::uint64_t m_sweepAt{ 0 };
			std::uint32_t m_alerts{ 0 };  // alerts seen (0 -> non-zero)
			bool          m_gaveUp{ false };
		};
	}

	// ---- game glue (ActorProxyGame.cpp; game thread unless noted) -------------------------------------------------------
	// `!proxy on`: Pump asks MGSV for the pool at its next Tick (TakePoolRequest), as the sandbox setup does.
	void RequestPool();
	[[nodiscard]] bool TakePoolRequest();
	// The pool from PhantomRing.ProxyPool (Lua thread, after the sandbox setup). Clears it with an empty one.
	void SetPool(const plan::Pool& a_pool);
	// MGSV's Lua thread, every Tick while ready (Pump.cpp): enables/puppets/disables the slot soldiers, finds their location
	// objects, keeps asleep proxies asleep and sees MGSV's Fulton take one. a_call runs a Lua chunk (its first result).
	void LuaStep(std::uint64_t a_nowMs, const std::function<std::string(const std::string&)>& a_call);
	// Once per player-camera frame after the hold (Frame.cpp): reads the ActorTable, assigns, places, parks.
	void OnFrame(const logic::Vec3& a_root, const double a_odo[3], const logic::Vec3* a_buddy, const logic::Vec3& a_centre, std::uint64_t a_nowMs);
	// Frame thread: Frame.cpp skips OnFrame (not ready: a load, a reload, no sandbox centre) -- the proxies count as inactive
	// until the next OnFrame, so Capture/Skeleton stop excluding draws near stale places and Pump's auto-aim check (AllPlaced,
	// AutoAimWanted) never reads the last pool's counts after a mission reload (review 2026-10-14).
	void Suspend();
	// Any thread: Capture's owner filter (false when proxies are off).
	bool ExcludesDraw(const float a_drawPos[3], const float a_snakeRoot[3]);
	// Any thread: a skinned model rooted within a_radius of a placed proxy is a soldier's (Skeleton.cpp's buddy-skin learning
	// skips it: a proxy beside the whistle spot must never be learned as D-Horse).
	bool NearAnyProxy(const float a_pos[3], float a_radius);
	// Lua thread (Pump): whether MGSV's auto-aim should be on now (plan::AutoAimWanted with the live counts).
	bool AutoAimWanted(bool a_savesGuarded);
	[[nodiscard]] bool Active();
	// Every pool soldier and hostage read back where it was put (proxies and parked alike): nothing disabled stands around.
	[[nodiscard]] bool AllPlaced();
	std::string Describe();  // `!proxy`

	// D-Dog's attack orders (Dog.cpp, docs/research/ddog.md §3.5): one Elden Ring character gets a slot soldier of his own for the
	// order's length -- awake or asleep, [Proxies] on or off (the pool exists whenever ProxiesRun) -- enabled inert (restrict-notice,
	// and puppet unless a_puppet is false: [Dog] TargetPuppet) before his first update, standing on the sandbox ground at the
	// character's x/z, never parked for the buddy's clearance (the dog has to reach him); MGSV's alert phase is held at 0 while he is
	// out (the phase lock; if it can't be held, dog::DisableAttack). Release() ends it (the soldier goes back to what the slot is
	// without it: disabled, or a sleeping / full proxy). Lua thread or console.
	// (Quiet, docs/research/quiet.md §3.4-3.5) a_buddyType = the buddy the soldier is for (BuddyType: 2 D-Dog, 3 Quiet) and a_height his
	// height (D-Dog's on the ground; Quiet's stand-in at the enemy's). The phase lock's guard is the buddy's own: a fresh one on each
	// buddy's first dedicated soldier and on every change of the active buddy type, and a give-up turns off the active buddy's orders
	// (D-Dog's Attack, Quiet's orders) -- never another's.
	void Dedicate(std::uint64_t a_handle, bool a_puppet, int a_buddyType = 2, plan::Height a_height = plan::Height::kGround);
	void Release(const char* a_why);
	// The buddy (BuddyType) the dedicated soldier is for, 0 when none.
	int DedicatedBuddy();
	// D-Dog's attack has its soldier (Dog.cpp, game thread, every frame before OnFrame): from the bite's start to its end MGSV's paired
	// animation moves him and the dog together, so he is no longer put back onto the character's place -- only moved along with Snake's
	// treadmill jumps (the dog is moved by the same jump in the same frame).
	void HoldDedicated(bool a_held);
	// The slip (plan::SlipObserver; horizontal, MGSV axes) since SlipBegin: what Elden Ring's walls kept Snake from doing of his MGSV
	// motion, as seen in the characters around him. SlipBegin / SlipStop: game thread (D-Dog's Wait). Slip: any thread.
	void        SlipBegin();
	void        SlipStop();
	logic::Vec3 Slip();
	// A fresh alert guard (the phase lock's give-up is latched once per guard): `!dog attack on` after a give-up turned Attack off,
	// so that a later alert MGSV can't calm turns it off again. Lua thread or console.
	void RearmPhaseGuard(const char* a_why);
	struct Dedicated
	{
		std::uint64_t handle{ 0 };       // 0: none
		std::uint16_t soldier{ 0xFFFF }; // his slot soldier (0xFFFF: no slot yet)
		bool          ready{ false };    // realized, placed where he should be (the order may go)
		bool          asleep{ false };
		logic::Vec3   pos;               // where the soldier is put (MGSV world, on the sandbox ground)
		logic::Vec3   real;              // where the character is (MGSV world)
		logic::Vec3   at;                // where his location object's feet were at the last OnFrame (atValid)
		bool          atValid{ false };
		bool          placed{ false };   // the last OnFrame put him there (his setter called)
	};
	Dedicated DedicatedState();
	// The Elden Ring character a slot soldier stands in for (0: none). Any thread, no game call under its lock (the dog-attack detour).
	std::uint64_t HandleOfSoldier(std::uint16_t a_soldier);
	// MGSV's balloon took this character's proxy (kGEvFulton a=1 sent) this session.
	bool Fultoned(std::uint64_t a_handle);

	// Fulton (protocol v12 kGEvFulton): Snake's fulton of the proxy soldier that stands in for Elden Ring character a_handle
	// succeeded (a_ok) or failed. Sends the event (v0 = where the proxy stood, else the character's ActorTable position, else
	// Snake's root) and gives the soldier back to the pool (parked) -- a fultoned character is never assigned again. Any thread.
	// `!fulton <handle> [fail]` calls it directly: the event path without MGSV's own fulton (live 2026-10-14: proxies can't move
	// MGSV's soldiers yet, docs/GUEST.md 20.4).
	std::string Fulton(std::uint64_t a_handle, bool a_ok, const char* a_why);

	// Combat.cpp's ShotSolver detour (game thread): turns a round aimed at a flattened proxy ([Proxies] Height = ground) to the
	// Elden Ring character's real height (plan::CorrectShot). a_dir: MGSV world, unit length; changed in place. No-op unless full
	// proxies run with [Proxies] AimHeightFix.
	void CorrectShot(const float a_origin[3], float a_dir[3]);
}
