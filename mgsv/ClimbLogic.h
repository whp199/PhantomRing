#pragma once
// The pure half of Snake's ladder climbs and climb-ups / vaults for Elden Ring (protocol v15 kHEvClimb / kHEvMantle,
// kGuestClimbing / kGuestMantling; docs/GUEST.md 22). Climb.cpp drives it on the game thread; tests/test_guest_climb.cpp
// drives it with made-up frames.
//
// MGSV plays its ladder climb, climb-up and vault only at real geometry: they live in additional motion archives
// (player2_ladder.mtar, player2_jump.mtar) that its direct-motion request can't reach (live 2026-10-31: a raw request with
// any of their 23 + 37 gani hashes is taken and plays nothing; one from the resident archive plays). So Snake is *staged*:
// placed (the treadmill's native placement) at a real ladder or barrier of the airport, MGSV's own action button (Y) starts
// the move, and he is placed back on the sandbox when it is over. Elden Ring never sees the trip: the odometer ignores it
// (rebased every frame), the host draws his pose where its own climb happens.
//
// Ladders: all of the airport's are the same model (mafr_lddr001, an 8 m wall ladder; 11 of them load with the apron, their
// TppLadder entities found in memory, docs/GUEST.md 22.1). Measured live on mafr_lddr001_0005 (origin (916.35, -11.52,
// 1072.30), entity yaw 60 deg) and the same on _0011: Snake stands BottomOut m out along the entity's facing to mount it
// (facing the ladder: entity yaw + 180), hangs on it with his root LineOut m out, HangLow m above the foot after the
// bottom mount, HangHigh m after the top mount; the top stand point is TopIn m behind the foot on the roof, TopHeight m up.
// MGSV climbs 1.25-1.30 m/s up and 1.22 m/s down with the stick held (a half stick, L3: the same), steps off the bottom
// below ~0.4 m and climbs over the top above ~6.8 m. Nothing moves him along MGSV's ladder (a native placement never; its
// Warp did in one session and never again), so a climb of any length loops through MGSV's own ends: he climbs over the top
// (steps off the foot) and is mounted again from the foot (the roof), every ~5.5 m. A direct-motion request ends the ladder
// action at once (he lets go and drops: Elden Ring's climb ended), and he is placed home.

#include "Logic.h"

#include <cstdint>
#include <optional>
#include <string>

namespace phantom::guest::climb::plan
{
	using logic::Vec3;

	// One of MGSV's real ladders: its TppLadder entity's translation (the foot of the ladder) and yaw (degrees).
	struct LadderSite
	{
		Vec3  origin;
		float yawDeg{ 0 };
	};

	struct LadderGeometry
	{
		float bottomOut{ 1.0f };   // the bottom stand point, out from the foot along the entity's facing (m)
		float lineOut{ 0.65f };    // Snake's root while he hangs on it
		float topIn{ 0.2f };       // the top stand point, behind the foot (on the roof)
		float topHeight{ 7.92f };  // the roof above the foot
		float hangLow{ 0.5f };     // root above the foot right after the bottom mount (the lowest he hangs)
		float hangHigh{ 6.0f };    // root above the foot right after the top mount
	};

	// The ladder's facing (unit, horizontal): the side Snake climbs on.
	Vec3  Facing(const LadderSite& a_site);
	Vec3  BottomStand(const LadderSite& a_site, const LadderGeometry& a_geo);  // feet
	Vec3  TopStand(const LadderSite& a_site, const LadderGeometry& a_geo);     // feet
	// How far (horizontally) a root is from the climbing line: he hangs on the ladder when it is ~0.
	float LineDistance(const LadderSite& a_site, const LadderGeometry& a_geo, const Vec3& a_root);
	float BottomFacing(const LadderSite& a_site);  // radians (MGSV yaw)
	float TopFacing(const LadderSite& a_site);

	// Elden Ring's speed against MGSV's fixed one: the stick is held while MGSV is behind what Elden Ring climbed. Speeds at or
	// below 0 (or not finite) are "unknown": MGSV's own pace (the stick always held).
	class Pacer
	{
	public:
		void Start(float a_speed);
		void SetSpeed(float a_speed);
		// a_dt: seconds since the last frame; a_climbed: metres MGSV climbed this frame toward the climb's end (placements out).
		bool Push(double a_dt, float a_climbed);
		[[nodiscard]] float Owed() const { return owed_; }
		[[nodiscard]] float Speed() const { return speed_; }

		static constexpr float kAhead = 0.05f;   // pushed while owing more than this
		static constexpr float kMaxOwed = 0.6f;  // Elden Ring faster than MGSV: the debt stops growing here
		static constexpr float kMaxAhead = 0.5f;

	private:
		float speed_{ 0 };
		float owed_{ 0 };
	};

	// What the game thread does for a move this frame.
	struct Step
	{
		std::optional<Vec3> place;       // put Snake's feet here (the treadmill's native placement)...
		float               placeYaw{ 0 };  // ... facing this way (radians)
		// After a native placement: MGSV's Warp to the same spot as well, for his facing (the native placement keeps whatever way
		// he faces; live: yaw 90 asked, -90 kept).
		bool                face{ false };
		// A direct-motion request, played and stopped (`dmoff`): ends MGSV's ladder action (he lets go and drops), and right after
		// the placement home cancels the landing that drop would play (live 2026-10-31).
		bool                knockOff{ false };
		bool                pressAction{ false };  // pulse MGSV's action button (Y)
		std::int16_t        stickY{ 0 };           // the left stick's Y MGSV gets this frame (the guest owns the pad)
		bool                active{ false };       // the flag (kGuestClimbing / kGuestMantling) is up
		const char*         note{ nullptr };       // something worth a log line happened (static text)
	};

	// What MGSV shows of Snake this frame (the game thread reads it).
	struct Facts
	{
		std::uint64_t nowMs{ 0 };
		Vec3          root;
		bool          normalAction{ false };  // PlayerStatus NORMAL_ACTION: on his feet, no special action (off on a ladder, in a vault)
		bool          standing{ false };      // STAND and not SQUAT / CRAWL (a stance change is no NORMAL_ACTION either)
		float         yaw{ 0 };               // the way he faces (radians, MGSV yaw)
		bool          ready{ false };         // the sandbox plays (else the move is dropped where it is)
	};

	enum class Phase
	{
		kIdle,
		kPlace,   // placed at the stand point, waiting for him to stand there
		kMount,   // Y pressed, waiting for him to leave NORMAL_ACTION (on the ladder / in the move)
		kMove,    // on the ladder (climbing, paced) / in the climb-up or vault
		kLeave,   // ladder: placed below its foot, waiting for him to stand
		kReturn,  // placed back on the sandbox, waiting for him to stand there
	};
	const char* PhaseName(Phase a_phase);

	// How far a_yaw is from a_want (radians, 0..pi).
	float YawError(float a_yaw, float a_want);
	// The stand point's ready check, shared by both moves: once he has stood (NORMAL_ACTION, STAND) a_settleMs, true when he faces
	// a_want (within 25 deg); otherwise a_s gets another facing Warp at a_stand (up to 3 a stand: MGSV turns him back to its camera
	// when a stance request stands him up, live 2026-10-31).
	bool ReadyAtStand(const Facts& a_f, std::uint64_t a_settleMs, const Vec3& a_stand, float a_want, std::uint64_t& a_standSince, int& a_retries,
		Step& a_s);

	// A ladder climb: Start (kHEvClimb a = 1) .. Stop (a = 0). Update once per game frame.
	class Ladder
	{
	public:
		struct Timing
		{
			std::uint64_t settleMs{ 150 };   // after the stand placement, before Y
			std::uint64_t mountMs{ 1800 };   // Y unanswered this long: Y again (once), then give up
			std::uint64_t leaveMs{ 600 };    // still on the ladder this long after the foot placement: climbed down instead
			std::uint64_t leaveMaxMs{ 4000 };
			std::uint64_t returnMs{ 1500 };  // back on the sandbox: up to this long for NORMAL_ACTION
		};

		// a_home: Snake's feet on the sandbox and his facing (radians) to come back to.
		void Start(const LadderSite& a_site, const LadderGeometry& a_geo, bool a_down, float a_speed, const Vec3& a_home, float a_homeYaw,
			std::uint64_t a_nowMs);
		// a = 1 again while climbing: a new direction or speed (no new mount).
		void Change(bool a_down, float a_speed);
		void Stop(std::uint64_t a_nowMs);
		Step Update(const Facts& a_facts, double a_dt);
		// The placement of the last Step was refused (the native call failed): the move ends where he is.
		void PlacementFailed();

		[[nodiscard]] Phase Current() const { return phase_; }
		[[nodiscard]] bool  Active() const { return phase_ != Phase::kIdle; }
		[[nodiscard]] bool  Down() const { return down_; }
		[[nodiscard]] int   Loops() const { return loops_; }
		[[nodiscard]] float Climbed() const { return climbed_; }
		[[nodiscard]] const Pacer& Pace() const { return pacer_; }
		[[nodiscard]] std::uint64_t MountedMs() const { return mountedMs_; }  // Y -> on the ladder (last mount)
		Timing timing;

	private:
		Step Enter(Phase a_phase, std::uint64_t a_nowMs);

		LadderSite     site_;
		LadderGeometry geo_;
		Phase          phase_{ Phase::kIdle };
		bool           down_{ false };
		bool           stopAsked_{ false };
		bool           retried_{ false };
		Vec3           home_;
		float          homeYaw_{ 0 };
		std::uint64_t  since_{ 0 };
		std::uint64_t  mountedMs_{ 0 };
		bool           placed_{ false };  // the phase's placement was handed out (its first frame is the placement's own)
		Pacer          pacer_;
		std::optional<float> lastY_;
		float          climbed_{ 0 };
		int            loops_{ 0 };
		int            mounts_{ 0 };
		int            mountTries_{ 0 };
		int            faceRetries_{ 0 };
		std::uint64_t  offSince_{ 0 };
		float          leaveY_{ 0 };
		bool           droppedOff_{ false };
		bool           leaveTried_{ false };  // the way home went through kLeave once (he was still on the ladder)  // a = 0 dropped him off the ladder: his landing is cancelled after the home placement
		std::uint64_t  standSince_{ 0 };
	};

	// A real obstacle Snake is staged at for a climb-up or a vault: where he stands (feet) and which way he faces (degrees), and
	// how high it is (cm, informational: the site is picked by kind and height).
	struct MantleSite
	{
		Vec3  stand;
		float yawDeg{ 0 };
		int   heightCm{ 0 };
		bool  valid{ false };
		bool  cutAtTop{ false };  // a climb-up played on a vault site: over once he is on top (before he drops down its far side)
	};

	// Which site a kHEvMantle gets: a vault (b = 1) the vault site; a climb-up the low one up to a_lowMaxCm, the high one above
	// (either when only one is set), else the vault site cut at its top (cutAtTop). Invalid = none fits.
	MantleSite PickMantle(int a_heightCm, bool a_vault, const MantleSite& a_vaultSite, const MantleSite& a_low, const MantleSite& a_high,
		int a_lowMaxCm);

	// A climb-up or vault: Start .. done (it ends by itself).
	class Mantle
	{
	public:
		struct Timing
		{
			std::uint64_t settleMs{ 150 };
			std::uint64_t startMs{ 700 };    // Y unanswered (still NORMAL_ACTION) this long: Y again once, then give up
			std::uint64_t maxMoveMs{ 4000 }; // the move hasn't ended by then: over anyway
			std::uint64_t returnMs{ 1500 };
		};

		void Start(const MantleSite& a_site, const Vec3& a_home, float a_homeYaw, std::uint64_t a_nowMs);
		Step Update(const Facts& a_facts);
		void PlacementFailed();

		[[nodiscard]] Phase Current() const { return phase_; }
		[[nodiscard]] bool  Active() const { return phase_ != Phase::kIdle; }
		[[nodiscard]] std::uint64_t MoveMs() const { return moveMs_; }  // the last move: Y -> back in NORMAL_ACTION
		[[nodiscard]] float Rose() const { return rose_; }              // ... and how high his root rose
		Timing timing;

	private:
		Step Enter(Phase a_phase, std::uint64_t a_nowMs);

		MantleSite    site_;
		Phase         phase_{ Phase::kIdle };
		Vec3          home_;
		float         homeYaw_{ 0 };
		std::uint64_t since_{ 0 };
		std::uint64_t pressedAt_{ 0 };
		std::uint64_t moveMs_{ 0 };
		bool          placed_{ false };
		bool          retried_{ false };
		bool          started_{ false };
		float         startY_{ 0 };
		float         rose_{ 0 };
		std::uint64_t standSince_{ 0 };
		int           faceRetries_{ 0 };
		bool          cut_{ false };  // the move was cut short (dmoff): once more after the placement home
	};
}
