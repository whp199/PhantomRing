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
		};

		// One hostile Elden Ring character, already corrected into MGSV world as of now.
		struct Target
		{
			std::uint64_t handle{ 0 };
			Vec3          pos;
			float         yaw{ 0 };
			float         distance{ 0 };  // horizontal, from Snake's root
			bool          asleep{ false }; // kActorAsleep: its proxy sleeps and stays even beside Snake (MGSV's Fulton wants him there)
		};

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

		// The hostile, living, finite records of a table, corrected for the treadmill warps since the table was built
		// (pos + kNow - kThen), within maxDistance of Snake, nearest first.
		std::vector<Target> SelectTargets(const proto::ActorRecord* a_records, std::uint32_t a_count, const Vec3& a_snakeRoot, const Vec3& a_kThen,
			const Vec3& a_kNow, const Settings& a_s);

		// What one pool soldier should do this frame.
		struct Slot
		{
			std::uint16_t id{ 0xFFFF };   // MGSV GameObject id (TppSoldier2)
			std::uint64_t handle{ 0 };    // the Elden Ring character it stands for; 0 = parked
			Vec3          pos;            // where it should be (MGSV world)
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

	// Fulton (protocol v12 kGEvFulton): Snake's fulton of the proxy soldier that stands in for Elden Ring character a_handle
	// succeeded (a_ok) or failed. Sends the event (v0 = where the proxy stood, else the character's ActorTable position, else
	// Snake's root) and gives the soldier back to the pool (parked) -- a fultoned character is never assigned again. Any thread.
	// `!fulton <handle> [fail]` calls it directly: the event path without MGSV's own fulton (live 2026-10-14: proxies can't move
	// MGSV's soldiers yet, docs/GUEST.md 20.4).
	std::string Fulton(std::uint64_t a_handle, bool a_ok, const char* a_why);
}
