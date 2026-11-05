#pragma once

// Resting at a Site of Grace with the buddy: the pure rules (docs/research/quiet.md §3.9; er/GraceRest.h is the game side;
// tests/test_host_grace.inc). No game access here: the game side reads the fade, finds the grace, casts the rays (an injected
// ray oracle, so recorded rays can drive the same search offline) and publishes what these decide.
//  - the observed black (U12, 2026-11-04 live): Elden Ring's "InGame" fade plate (CSFadeImp's CSFD4FadePlate named InGame) is
//    black when its colour's alpha, interpolated from its "from" to its "to" by the time left, reaches 1 (FadeAlpha). Live at
//    Rampart Tower the frame's mean luminance was 0.0 exactly while it was >= 0.99, and 2.0 / 7.1 / 15.3 at 0.7 / 0.5 / 0.27;
//  - the gate (Gate): armed by a rest (kHEvRested 0), the seat work starts only on the first black frame, and only if that comes
//    within seat_black_wait_ms of the press (a rest that turns into a talk has no black: no seat, nothing moves); the seat is
//    published only while black and before black start + seat_deadline_ms; the host shows the buddy there only once the guest
//    echoes it while still black (an echo after the black: hidden through this rest, never a pop-in); an Elden Ring dialog
//    while seated or seating cancels the seat;
//  - the seat (Search): rings of 2.0 / 2.5 / 1.5 / 3.0 m round the grace x 16 bearings, evaluated in the order of their expected
//    score (screen x nearest 0.62 first); the floor (the ground filter: indoors the ray filter hits loose planks) within
//    seat_step of the floor under the grace, its normal y >= seat_slope_ny, a footprint spread <= seat_spread, seat_headroom m
//    clear, knee rays at 0.5 m AND 0.25 m clear (Gatefront's 0.3-0.5 m stone border escaped the 0.5 m ones), the line from the
//    grace at 0.5 m clear, >= seat_snake_clear from Snake, >= seat_char_clear from any other character (the grace's own hidden
//    team-0 one excluded by the caller), and in the rest's view: in front, on screen, right of the grace menu (screen x >=
//    seat_menu_x), the camera's line clear, neither behind Snake's silhouette nor in front of it. Score |r - 2| + 2 |x - 0.62|
//    + 0.5 |dh|; the search stops at the first that passes within 0.2 of the best possible. Fallback: 2 m from the grace
//    opposite Snake with the floor, headroom and knee rules only; else no seat.

#include "Math.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace phantom::er
{
	struct GraceConfig;
}

namespace phantom::er::grace
{
	struct Rules
	{
		std::vector<float> rings{ 2.0f, 2.5f, 1.5f, 3.0f };
		int                dirs{ 16 };
		float              screenX{ 0.62f };
		float              menuX{ 0.33f };
		float              step{ 0.3f };
		float              slopeNy{ 0.9f };
		float              spread{ 0.15f };
		float              headroom{ 1.3f };
		float              snakeClear{ 1.0f };
		float              charClear{ 2.0f };
		int                blackWaitMs{ 2000 };
		int                deadlineMs{ 750 };
		// (0.9.0 integration) The real MGSV's Quiet settles into her rest ~3 s after her order (a turn, a hop, a crouch: live), longer than
		// the black: the guest echoes the seat only once she has settled, and an echo after the black fades her in over lateFadeMs (her
		// cloak) instead of hiding her through the rest. No echo within lateEchoMs of the publish: hidden through this rest.
		int                lateEchoMs{ 6000 };
		int                lateFadeMs{ 300 };
		int                reloadReadyMs{ 100 };
		// geometry of the tests (the study's survey, quiet.md §2.9)
		float footprint{ 0.35f };    // m: the 4 footprint rays' offsets
		float kneeOut{ 0.45f };      // m: the knee rays' length
		float kneeLow{ 0.25f };      // m over the floor: the second knee ring (the stone border)
		float kneeHigh{ 0.5f };
		float reachHeight{ 0.5f };   // m: the grace -> spot line's height
		float floorAbove{ 1.5f };    // m: the floor ray starts this far over the grace's floor ...
		float floorBelow{ 2.5f };    // ... and looks this far under it
		float bodyHeight{ 0.6f };    // m: the point the camera test projects (a kneeling body's middle)
		float snakeRadius{ 0.45f };  // m: Snake's silhouette for the "behind / in front of him" test
		float snakeHeight{ 0.9f };   // m: ... projected at this height
		float goodEnough{ 0.2f };    // stop at the first pass within this of the best possible score
		// The renderer fades whatever stands within [Render] occluder_radius m of the camera-to-Snake line in front of him (MGSV's
		// rule; er/RenderShared.cpp Fades): a seat there would be drawn at 15 % -- live 2026-11-04 (Rampart Tower, the camera
		// turned 90 deg) the seat 3.1 m in front of the camera was invisible. occluder_radius + her body: refused.
		float occluderClear{ 1.3f };
		float fallbackR{ 2.0f };
	};
	Rules MakeRules(const GraceConfig& a_config);

	// ---- the observed black ------------------------------------------------------------------------------------------------
	// One CSFD4FadePlate (live 2026-11-04, Elden Ring 1.17.2): +0x20 colour "from" (RGBA), +0x30 colour "to", +0x48 the time left
	// (s), +0x58 the fade's length (s). The colour shown = to + (from - to) * left / length (left 0: "to"). InGame's: black (0, 0, 0)
	// with the alpha 0 -> 1 over 0.75 s, held, then 1 -> 0 over 0.5 s.
	struct Plate
	{
		float from[4]{};
		float to[4]{};
		float left{ 0 };
		float length{ 0 };
	};
	// The alpha (and colour) shown; NAN when the plate's numbers are not sane (the layout is not the verified one).
	float FadeAlpha(const Plate& a_p, float* a_rgb = nullptr);
	inline constexpr float kDarkAlpha = 0.99f;
	// Black: alpha >= kDarkAlpha and a dark colour (each channel <= 0.05).
	bool PlateDark(const Plate& a_p);

	// One rest's black, measured (U12): when it started and ended after the press (ms), and the HUD's hide. Logged for every rest
	// while the fade plate is readable, whatever the buddy (the U12 record).
	class BlackWatch
	{
	public:
		void          Press(std::uint64_t a_nowMs);
		// Returns true once, when a black that started after the press ended (or the window closed).
		bool          Update(std::uint64_t a_nowMs, bool a_dark);
		bool          Active() const { return pressMs_ != 0 && !reported_; }  // watching this rest's black (until it was reported)
		std::uint64_t PressMs() const { return pressMs_; }
		std::int64_t  StartMs() const { return startMs_; }  // after the press; -1 none
		std::int64_t  EndMs() const { return endMs_; }      // after the press; -1 none
		std::int64_t  LastMs() const { return lastMs_; }    // the last black of a chain (Pass time) ended
		int           Blacks() const { return blacks_; }
		void          Clear() { *this = {}; }
		static constexpr std::uint64_t kWindowMs = 8000;    // a black more than this after the press is not the rest's

	private:
		std::uint64_t pressMs_{ 0 };
		std::int64_t  startMs_{ -1 }, endMs_{ -1 }, lastMs_{ -1 };
		bool          dark_{ false };
		int           blacks_{ 0 };
		bool          reported_{ false };
	};

	// ---- the gate ----------------------------------------------------------------------------------------------------------
	class Gate
	{
	public:
		enum class Phase : std::uint8_t
		{
			kIdle,       // no rest (or not armed)
			kArmed,      // rested: waiting for the observed black
			kSearching,  // black seen: the centre and the seat are being found
			kPublished,  // the seat is published: hidden until the guest echoes it
			kShown,      // the guest echoed it (inside the black, or after it: faded in): drawn at the seat
			kHidden,     // published, but no echo within lateEchoMs: hidden through this rest
			kNoSeat,     // no seat this rest (no black, too late, none found): she stays where she was
			kCancelled,  // an Elden Ring dialog came while seated or seating: hidden while it lasts, then her post rule
		};
		enum class Event : std::uint8_t
		{
			kNone,
			kBlack,      // the first black frame: start the centre and the seat now
			kNoBlack,    // no black within seat_black_wait_ms of the press (a talk): no seat
			kPublish,    // the seat is ready inside the black, before the deadline: publish it (graceSeq + 1)
			kTooLate,    // the seat (or the search) ran past the deadline: none this rest
			kLight,      // the black ended before the seat was ready: none this rest
			kNoSpot,     // the search found no seat (fallback included)
			kShown,      // the echo came inside the black, or after it (then faded in: ShownLate)
			kLateEcho,   // no echo within lateEchoMs of the publish: hidden through this rest
			kCancelled,  // an Elden Ring dialog
			kEnded,      // left the grace (2 m, a load, a death): back to her post rule
		};
		struct In
		{
			std::uint64_t nowMs{ 0 };
			bool          dark{ false };
			bool          atGrace{ false };
			bool          echo{ false };    // kBuddySeated with graceSeqSeated == this rest's seq
			bool          dialog{ false };  // an Elden Ring dialog started (this frame) or runs
		};

		// kHEvRested 0 with everything the rest needs on (the caller decides "armed"; a respawn never calls it).
		void   Rest(std::uint64_t a_nowMs);
		Event  Update(const In& a_in, const Rules& a_rules);
		// The search finished at a_nowMs (a_found: a seat). Returns kPublish, kTooLate, kLight or kNoSpot.
		Event  SearchDone(std::uint64_t a_nowMs, bool a_found, bool a_dark, const Rules& a_rules);
		Phase  Current() const { return phase_; }
		bool   Searching() const { return phase_ == Phase::kSearching; }
		// What the host publishes: a seat only while published or shown.
		bool   SeatPublished() const { return phase_ == Phase::kPublished || phase_ == Phase::kShown; }
		bool   AtSeat() const { return phase_ == Phase::kShown; }  // drawn at the seat
		// Her alpha at the seat: 1, or the fade-in after an echo that came once the screen was back.
		float  SeatAlpha(std::uint64_t a_nowMs, const Rules& a_rules) const
		{
			if (phase_ != Phase::kShown || !shownLate_) {
				return 1.0f;
			}
			const float t = static_cast<float>(a_nowMs - shownMs_) / static_cast<float>(std::max(1, a_rules.lateFadeMs));
			return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
		}
		bool   ShownLate() const { return shownLate_; }
		bool   Hide() const { return phase_ == Phase::kPublished || phase_ == Phase::kHidden || (phase_ == Phase::kCancelled && dialog_); }
		std::uint64_t PressMs() const { return pressMs_; }
		std::uint64_t BlackMs() const { return blackMs_; }
		std::uint64_t PublishMs() const { return publishMs_; }
		// The seat was ready within seat_reload_ready_ms of the black's start (a guest that places by a reload could still fit).
		bool   ReadyForReload(const Rules& a_rules) const
		{
			return publishMs_ && blackMs_ && publishMs_ - blackMs_ <= static_cast<std::uint64_t>(std::max(0, a_rules.reloadReadyMs));
		}
		void   Clear() { *this = {}; }

	private:
		Phase         phase_{ Phase::kIdle };
		std::uint64_t pressMs_{ 0 }, blackMs_{ 0 }, publishMs_{ 0 }, shownMs_{ 0 };
		bool          shownLate_{ false };
		bool          dialog_{ false };
	};
	const char* Name(Gate::Phase a_phase);
	const char* Name(Gate::Event a_event);

	// ---- the grace's centre ------------------------------------------------------------------------------------------------
	inline constexpr float kCentreMax = 2.5f;     // m: the AEG099_060 asset is this near the player at the rest (live 1.15-1.42 m)
	inline constexpr float kFacingCentre = 1.25f; // m along his facing (he turns to the grace within ~0.8 s)
	std::optional<Vec3> NearestCentre(std::span<const Vec3> a_assets, const Vec3& a_player, float a_max = kCentreMax);
	Vec3                FacingCentre(const Vec3& a_player, const Vec3& a_forward, float a_dist = kFacingCentre);

	// ---- the seat ----------------------------------------------------------------------------------------------------------
	enum class Ray : std::uint8_t
	{
		kGround,  // the ground filter: what characters stand on
		kWorld,   // the ray filter: anything in the way
	};
	struct Hit
	{
		Vec3 pos;
		Vec3 normal{ NAN, NAN, NAN };  // NAN: unknown (a plain cast)
	};
	using RayFn = std::function<std::optional<Hit>(const Vec3& a_from, const Vec3& a_delta, Ray a_kind)>;

	struct View
	{
		Vec3  pos, fwd, right, up;  // normalised
		float fovY{ 1.0472f };
		float aspect{ 16.0f / 9.0f };
		bool  valid{ false };
	};
	struct ScreenPoint
	{
		bool  front{ false };
		float x{ 0 }, y{ 0 };  // 0..1 (left -> right, top -> bottom)
		float depth{ 0 };
	};
	ScreenPoint Project(const View& a_view, const Vec3& a_p);

	struct Scene
	{
		Vec3              centre;
		float             floorY{ 0 };   // the floor under the grace
		Vec3              snake;         // Snake's feet (where he is drawn: the Tarnished's place)
		View              view;          // the rest's view (the frozen camera of the press)
		std::vector<Vec3> others;        // other characters' feet near the grace (the grace's own hidden one left out)
	};

	enum class Why : std::uint8_t
	{
		kOk,
		kNoFloor,
		kStep,
		kSlope,
		kUneven,
		kHeadroom,
		kKnee,
		kReach,
		kOnSnake,
		kCharacter,
		kBehindView,
		kOffScreen,
		kUnderMenu,
		kViewBlocked,
		kBehindSnake,
		kCoversSnake,
		kCount
	};
	const char* Name(Why a_why);

	struct Spot
	{
		float r{ 0 };
		float deg{ 0 };     // bearing round the centre, from +X toward +Z
		Vec3  feet;         // y = the floor once evaluated (else the grace's floor)
		float heading{ 0 }; // Elden Ring heading (atan2(x, z)) from the seat toward the grace
		float screenX{ NAN };
		float score{ 1e9f };
		float expected{ 1e9f };  // the order's estimate (before the rays)
		bool  fallback{ false };
		Why   why{ Why::kOk };
	};
	float Score(float a_r, float a_screenX, float a_dh, const Rules& a_rules);
	// The candidates in search order: by the score they would have on the grace's floor, the view's screen x included.
	std::vector<Spot> Candidates(const Scene& a_scene, const Rules& a_rules);
	// One candidate's hard rules with the rays (a_rays counts them).
	Spot Evaluate(Spot a_spot, const Scene& a_scene, const RayFn& a_ray, const Rules& a_rules, int* a_rays = nullptr);
	// The fallback: 2 m from the grace opposite Snake, floor + headroom + knees only.
	Spot Fallback(const Scene& a_scene, const RayFn& a_ray, const Rules& a_rules, int* a_rays = nullptr);

	// The whole search, a few candidates a frame (a ray budget), stopping at the first good-enough pass.
	class Search
	{
	public:
		void Start(Scene a_scene, const Rules& a_rules);
		// Evaluates candidates until about a_rayBudget rays were cast this call (at least one candidate). true = done.
		bool Step(int a_rayBudget, const RayFn& a_ray);
		bool Running() const { return running_; }
		bool Done() const { return done_; }
		const std::optional<Spot>& Result() const { return result_; }
		int  Rays() const { return rays_; }
		int  Evaluated() const { return evaluated_; }
		int  Passed() const { return passed_; }
		std::size_t Count(Why a_why) const { return counts_[static_cast<std::size_t>(a_why)]; }
		const Scene& CurrentScene() const { return scene_; }
		void Shift(const Vec3& a_delta);
		void Clear() { *this = {}; }
		std::string Summary() const;

	private:
		Scene                 scene_;
		Rules                 rules_;
		std::vector<Spot>     order_;
		std::size_t           next_{ 0 };
		std::optional<Spot>   best_;
		std::optional<Spot>   result_;
		bool                  running_{ false }, done_{ false };
		int                   rays_{ 0 }, evaluated_{ 0 }, passed_{ 0 };
		float                 bestPossible_{ 0 };
		std::array<std::size_t, static_cast<std::size_t>(Why::kCount)> counts_{};
	};
}
