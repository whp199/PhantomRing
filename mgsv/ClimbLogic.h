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

	// Elden Ring's speed against MGSV's fixed one: the stick is held while MGSV is behind what Elden Ring climbed. Exactly 0 = Elden
	// Ring's climber stopped on the rungs: MGSV holds still too (no stick). Below 0 (or not finite) = "unknown": MGSV's own pace (the
	// stick always held). (Before 0.6.0 0 was "unknown" too, and a climber resting on Elden Ring's rungs kept climbing in MGSV.)
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
		// The placements home never got him back on the sandbox (still on MGSV's ladder, or they failed): MGSV's own Warp home as
		// the last resort (Climb.cpp; it logs a warning). The move is over with it.
		bool                warpHome{ false };
		std::int16_t        stickY{ 0 };           // the left stick's Y MGSV gets this frame (the guest owns the pad)
		// Move him this many rung pairs (0.5 m each) along MGSV's ladder in the middle of his climb (ladder_rung::Shift): a long
		// Elden Ring ladder never reaches MGSV's ends (RecycleShift). 0 = none.
		int                 rungShift{ 0 };
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
		// MGSV's ladder record while he is on the staged ladder (ladder_rung::Read; -1 = unknown: no seamless climb, he loops
		// through MGSV's ends as before): the rung pair he is on, the one he climbs to, the ladder's top one.
		int           rung{ -1 }, rungTarget{ -1 }, rungTop{ -1 };
	};

	// Seamless long ladders: MGSV's ladder is 8 m (top rung pair 16; the bottom mount puts him on 5, a rung pair is 0.5 m). Climbing
	// up, once he is on (or climbs to) the pair two below the top he is moved kRecycleSpan pairs (3 m) down the same ladder; climbing
	// down, once on (or to) the pair two above the bottom mount's, kRecycleSpan up. MGSV's climb goes on from there with the same
	// pose, so it never reaches its climb-over or step-off. 0 = nothing to do (unknown record, a ladder under 6 m, or not there).
	inline constexpr int kRecycleSpan = 6;
	inline constexpr int kRecycleLow = 7;  // ... the lowest pair a down climb is let reach (the step-off comes below 5)
	int RecycleShift(bool a_down, int a_rung, int a_target, int a_top);

	// The way home ends only with Snake back on the sandbox: within kHomeRadius of where he stood (horizontally). A placement home
	// that didn't take him there (he was still on MGSV's ladder: no placement reaches him there; or it failed) is tried again, up to
	// kReturnTries in all, waiting kHomeWaitMs for each to land; then MGSV's Warp (Step::warpHome).
	inline constexpr float         kHomeRadius = 2.0f;
	inline constexpr int           kReturnTries = 3;
	inline constexpr std::uint64_t kHomeWaitMs = 250;

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
		// a = 1 again while climbing: a new direction or speed (no new mount). Not while Ending(): that climb is over (Climb.cpp
		// keeps the new one for after it).
		void Change(bool a_down, float a_speed);
		void Stop(std::uint64_t a_nowMs);
		Step Update(const Facts& a_facts, double a_dt);
		// The placement of the last Step was refused (the native call failed): on the way home it is tried again next frame (up to
		// kReturnTries, then Step::warpHome); anywhere else he goes home from where he is.
		void PlacementFailed(std::uint64_t a_nowMs);
		// The climb is on its way out (a = 0 came, or he is leaving MGSV's ladder / going home): a new a = 1 is a new climb.
		[[nodiscard]] bool Ending() const { return phase_ != Phase::kIdle && (stopAsked_ || phase_ == Phase::kLeave || phase_ == Phase::kReturn); }

		[[nodiscard]] Phase Current() const { return phase_; }
		[[nodiscard]] bool  Active() const { return phase_ != Phase::kIdle; }
		[[nodiscard]] bool  Down() const { return down_; }
		[[nodiscard]] int   Loops() const { return loops_; }
		[[nodiscard]] int   Recycles() const { return recycles_; }  // rung shifts handed out (seamless: no climb-over)
		// The last rung shift was refused (the record moved under it or isn't there): no more this climb -- MGSV's ends again.
		void ShiftFailed() { noRecycle_ = true; }
		[[nodiscard]] float Climbed() const { return climbed_; }
		[[nodiscard]] const Pacer& Pace() const { return pacer_; }
		[[nodiscard]] std::uint64_t MountedMs() const { return mountedMs_; }  // Y -> on the ladder (last mount)
		[[nodiscard]] float HomeYaw() const { return homeYaw_; }
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
		int            recycles_{ 0 };
		bool           noRecycle_{ false };
		int            mounts_{ 0 };
		int            mountTries_{ 0 };
		int            faceRetries_{ 0 };
		std::uint64_t  offSince_{ 0 };
		float          leaveY_{ 0 };
		bool           droppedOff_{ false };
		bool           leaveTried_{ false };  // the way home went through kLeave once (he was still on the ladder)
		std::uint64_t  standSince_{ 0 };
		int            returnTries_{ 0 };     // placements home handed out (kReturnTries)
		int            placeFails_{ 0 };      // ... and refused by the native call
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

	// Which site a kHEvMantle gets: a vault (b = 1) the vault site; a climb-up the step one up to a_stepMaxCm (when set), the low one
	// up to a_lowMaxCm, the high one above (either when only one is set), else the vault site cut at its top (cutAtTop). Invalid =
	// none fits.
	MantleSite PickMantle(int a_heightCm, bool a_vault, const MantleSite& a_vaultSite, const MantleSite& a_low, const MantleSite& a_high,
		int a_lowMaxCm, const MantleSite& a_step = {}, int a_stepMaxCm = 0);

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
		void PlacementFailed(std::uint64_t a_nowMs);  // as Ladder::PlacementFailed

		[[nodiscard]] Phase Current() const { return phase_; }
		[[nodiscard]] bool  Active() const { return phase_ != Phase::kIdle; }
		[[nodiscard]] std::uint64_t MoveMs() const { return moveMs_; }  // the last move: Y -> back in NORMAL_ACTION
		[[nodiscard]] float Rose() const { return rose_; }              // ... and how high his root rose
		[[nodiscard]] float HomeYaw() const { return homeYaw_; }
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
		int           returnTries_{ 0 };
		int           placeFails_{ 0 };
	};
	// After a move is over (Climb.cpp Finish) MGSV's root settles 0.5-0.9 m forward over a few frames (live, 0.5.0), which Elden
	// Ring's puppet would walk: the odometer stays still until kCapMs. Before the 0.6.0 review it stayed still for the whole
	// 300 ms whatever the player did -- with the time MGSV takes Snake home that was the 0.2-0.5 s of a dead stick after a ladder.
	// Now a push of the player's own left stick ends it at once (MGSV moves Snake with it from that frame; the settle's first
	// centimetres may count with it). A stick held through the ladder's end never reaches MGSV's pad (the host holds it neutral
	// until released: er/InputBridge.cpp HoldLeftStickUntilReleased), so only a fresh push counts.
	class OdoThaw
	{
	public:
		static constexpr std::uint64_t kCapMs = 300;
		static constexpr int           kStickDeadZone = 7849;  // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE

		enum class Why : std::uint8_t
		{
			kStill,  // keep it still this frame
			kStick,  // the player pushed the stick
			kCap,    // kCapMs over
		};
		void Start(std::uint64_t a_nowMs)
		{
			since_ = a_nowMs;
			active_ = true;
		}
		void Cancel() { active_ = false; }
		// Each frame while Active(): kStill keeps the odometer still; anything else ends it (Active() false from then).
		Why Update(std::uint64_t a_nowMs, int a_stickX, int a_stickY)
		{
			if (!active_) {
				return Why::kCap;
			}
			const bool pushed = a_stickX > kStickDeadZone || a_stickX < -kStickDeadZone || a_stickY > kStickDeadZone || a_stickY < -kStickDeadZone;
			const Why  why = pushed ? Why::kStick : a_nowMs - since_ >= kCapMs ? Why::kCap : Why::kStill;
			if (why != Why::kStill) {
				active_ = false;
				endedAfter_ = a_nowMs - since_;
			}
			return why;
		}
		[[nodiscard]] bool          Active() const { return active_; }
		[[nodiscard]] std::uint64_t EndedAfterMs() const { return endedAfter_; }

	private:
		std::uint64_t since_{ 0 };
		std::uint64_t endedAfter_{ 0 };
		bool          active_{ false };
	};
}
