#pragma once

// Quiet as a sniper buddy in Elden Ring: the pure rules (docs/research/quiet.md §3.2-3.8; er/Quiet.h is the game side;
// tests/test_host_quiet.inc). No game access here: the game side gathers the characters, casts the rays (an injected ray oracle,
// so recorded rays drive the same rules offline) and applies what these decide.
//  - her post (§3.2): Cover (a hostile within 60 m of Snake): rings of 25 / 35 / 45 / 55 m round Snake x 24 bearings, the camera's
//    forward half first; standable ground (the ground filter from 70 m above, normal y >= 0.8, a footprint of 4 rays at +-0.5 m
//    within 0.3 m, 1.6 m of headroom), -3..+25 m over Snake's feet, 60 m at most in 3D, drawable (|dh| <= d tan(fovY / 2) - 2:
//    MGSV draws her body on its apron, so a post the camera centres must keep her mirrored ground point in its frustum), no hostile
//    within 15 m and no character within 3 m, and from her sight point (feet + 0.9 m) a clear line to Snake's chest and to at
//    least one hostile's chest. Score: 3 x enemies seen - 0.05 |d - 35| + 0.1 x min(height, the drawable bound). Near (calm):
//    rings 8 / 12 / 16 m, -2..+8 m, the same rules, sight to the camera's aim point, higher ground preferred within the bound.
//    On the move (Snake over 3 m/s for 2 s): no post, she is not drawn, until he slows under 1 m/s for 1.5 s or a hostile comes
//    within 60 m. A new post must score 20 % better than the current one;
//  - her target (§3.4): hostile by both of Elden Ring's team reads, alive, rendered, never a boss, never one CQC or a Fulton
//    balloon has, within the Actors radius of Snake and 100 m of her post, seen from her sight point (the shot's own ray) and from
//    the camera, within 6 deg of the view's centre through the binoculars (10 otherwise); with the Guilty Butterfly never a
//    sleeper; marked ones first. A non-hostile character -- a merchant, a quest character, a spirit summon, the hidden team-0
//    one under a grace -- is never a candidate;
//  - her shot (§3.6): on kGEvQuietShot Hit the handle must still be a living hostile non-boss, and the line from her sight point
//    to his chest clear (Elden Ring decides whether the round lands): Wicked Butterfly (ATK 290) lethal by MGSV's share, Sinful
//    (292) lethal with a knockdown, Guilty (291) the sleep darts' build-up; no sound by default;
//  - spotting (§3.7): the pick's hard rules and a clear line from her sight point, within 60 m of her: provisional marks.

#include "CombatLogic.h"
#include "CqcLogic.h"
#include "Math.h"

#include "phantomring_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace phantom::er
{
	struct QuietConfig;
}

namespace phantom::er::quiet
{
	struct Rules
	{
		std::vector<float> coverRings{ 25, 35, 45, 55 };
		std::vector<float> nearRings{ 8, 12, 16 };
		int   dirs{ 24 };
		float coverHeightMin{ -3.0f }, coverHeightMax{ 25.0f };
		float nearHeightMin{ -2.0f }, nearHeightMax{ 8.0f };
		bool  drawable{ true };
		float heightScore{ 0.1f };
		float sight{ 0.9f };          // m over her feet: the post's, the pick's and the shot's one point
		float max3d{ 60.0f };
		float enemyClear{ 15.0f };
		float charClear{ 3.0f };
		int   raysPerFrame{ 30 };
		float researchCover{ 25.0f };
		float researchNear{ 20.0f };
		float moveSpeed{ 3.0f };      // m/s for moveSeconds: on the move
		float moveSeconds{ 2.0f };
		float stopSpeed{ 1.0f };      // m/s for stopSeconds: stopped
		float stopSeconds{ 1.5f };
		int   fadeMs{ 300 };
		float farUp{ 20.0f }, farDown{ 40.0f };
		float liftMax{ 30.0f };
		float targetRange{ 100.0f };
		float coneDeg{ 10.0f }, coneBinocularsDeg{ 6.0f };
		bool  markedFirst{ true };
		int   latchMs{ 500 };
		float spotRange{ 60.0f };
		int   spotEveryMs{ 500 };
		float spotLinger{ 10.0f };
		float spotTextEvery{ 8.0f };
		float lethalShareMult{ 1.0f };
		float lethalFallback{ 1.0f };
		bool  heavyPoise{ true };
		float tranqSleep{ 1.1f };
		float coverThreatRange{ 20.0f };
		// geometry (the design's, quiet.md §3.2)
		float hostileRange{ 60.0f };  // m from Snake: a hostile this near makes it Cover
		float groundAbove{ 70.0f };   // m over Snake's feet: the ground ray starts here
		float footprint{ 0.5f };      // m: the 4 footprint rays' offsets ...
		float footTol{ 0.3f };        // ... each within this of the ground found
		float headroom{ 1.6f };
		float slopeNy{ 0.8f };
		float lineSlack{ 0.6f };      // m: a sight ray stopping this near its end still sees
		float better{ 0.2f };         // a new post must score this much better (share of max(1, |current|))
		int   maxSeen{ 8 };           // hostiles a candidate's sight is tested against (the nearest to Snake)
		float threatNear{ 8.0f };     // m: a hostile this near her post makes her move
		int   echoWaitMs{ 1000 };     // a post not echoed this long is drawn anyway (the translation absorbs where MGSV has her)
	};
	Rules MakeRules(const QuietConfig& a_config);

	// ---- rays --------------------------------------------------------------------------------------------------------------
	enum class Ray : std::uint8_t
	{
		kGround,  // the ground filter: what characters stand on
		kWorld,   // the ray filter: anything in the way
	};
	struct Hit
	{
		Vec3 pos;
		Vec3 normal{ NAN, NAN, NAN };
	};
	using RayFn = std::function<std::optional<Hit>(const Vec3& a_from, const Vec3& a_delta, Ray a_kind)>;
	// Nothing in between (a hit within a_slack of a_to still sees him).
	bool LineClear(const Vec3& a_from, const Vec3& a_to, const RayFn& a_ray, float a_slack, int* a_rays = nullptr);

	// ---- her post ----------------------------------------------------------------------------------------------------------
	enum class Mode : std::uint8_t
	{
		kNone,
		kCover,
		kNear,
		kMoving,
	};
	const char* Name(Mode a_mode);

	struct Foe
	{
		std::uint64_t handle{ 0 };
		Vec3          feet;
		Vec3          chest;
	};
	struct PostScene
	{
		Vec3              snake;       // feet
		Vec3              snakeChest;
		Vec3              camPos;
		Vec3              camFwd;      // the view direction (its horizontal part orders the candidates)
		float             fovY{ 0.8f };
		Vec3              aimPoint{ NAN, NAN, NAN };  // what the camera looks at (Near's sight target), NAN: none
		std::vector<Foe>  hostiles;    // hostile, alive, not bosses, within hostileRange of Snake
		std::vector<Vec3> characters;  // every other character's feet near the candidates (any team)
	};

	enum class PostWhy : std::uint8_t
	{
		kOk,
		kNoGround,
		kSlope,
		kTooLow,
		kTooHigh,
		kTooFar,
		kNotDrawable,
		kEnemyNear,
		kCharNear,
		kFootprint,
		kHeadroom,
		kNoSnake,
		kNoEnemy,
		kNoAim,
		kCount
	};
	const char* Name(PostWhy a_why);

	struct Post
	{
		Mode    mode{ Mode::kNone };
		float   ring{ 0 };
		float   deg{ 0 };       // bearing round Snake, from +X toward +Z
		Vec3    feet;           // y = the ground once evaluated
		float   heading{ 0 };   // Elden Ring heading she faces (toward the hostiles she covers, or along the view)
		float   height{ 0 };    // over Snake's feet
		int     seen{ 0 };      // hostiles seen from her sight point
		float   score{ -1e9f };
		bool    forward{ true }; // in the camera's forward half
		PostWhy why{ PostWhy::kOk };
	};
	// The drawable bound: how high over Snake a post this far away may be (|dh| <= d tan(fovY / 2) - 2 m).
	float DrawableBound(float a_horizontal, float a_fovY);
	float PostScore(Mode a_mode, int a_seen, float a_horizontal, float a_height, float a_fovY, const Rules& a_rules);
	// The candidates in search order: the camera's forward half first, each half by ring (nearest the preferred distance first).
	std::vector<Post> PostCandidates(const PostScene& a_scene, Mode a_mode, const Rules& a_rules);
	Post EvaluatePost(Post a_post, const PostScene& a_scene, const RayFn& a_ray, const Rules& a_rules, int* a_rays = nullptr);
	// Is a_candidate worth moving her for (20 % better)?
	bool Better(const Post& a_candidate, const Post& a_current, const Rules& a_rules);

	// The search, a few candidates a frame: the forward half first; it stops after the forward half when anything passed there.
	class PostSearch
	{
	public:
		void Start(PostScene a_scene, Mode a_mode, const Rules& a_rules);
		bool Step(int a_rayBudget, const RayFn& a_ray);  // true = done
		bool Running() const { return running_; }
		bool Done() const { return done_; }
		Mode CurrentMode() const { return mode_; }
		const std::optional<Post>& Best() const { return best_; }
		const PostScene& Scene() const { return scene_; }
		int  Rays() const { return rays_; }
		int  Evaluated() const { return evaluated_; }
		int  Total() const { return static_cast<int>(order_.size()); }
		int  Passed() const { return passed_; }
		std::size_t Count(PostWhy a_why) const { return counts_[static_cast<std::size_t>(a_why)]; }
		std::string Summary() const;
		void Shift(const Vec3& a_delta);
		void Clear() { *this = {}; }

	private:
		PostScene           scene_;
		Rules               rules_;
		Mode                mode_{ Mode::kNone };
		std::vector<Post>   order_;
		std::size_t         next_{ 0 };
		std::optional<Post> best_;
		bool                running_{ false }, done_{ false };
		int                 rays_{ 0 }, evaluated_{ 0 }, passed_{ 0 };
		std::array<std::size_t, static_cast<std::size_t>(PostWhy::kCount)> counts_{};
	};

	// Cover, Near or on the move, from Snake's speed (m/s) and whether a hostile is within hostileRange.
	class MoveWatch
	{
	public:
		Mode Update(float a_speed, float a_dt, bool a_hostileNear, const Rules& a_rules);
		bool Moving() const { return moving_; }
		void Reset() { *this = {}; }

	private:
		bool  moving_{ false };
		float fast_{ 0 }, slow_{ 0 };
	};

	// Whether a new post search is due (§3.2's triggers).
	struct Need
	{
		bool  havePost{ false };
		Mode  postMode{ Mode::kNone };
		Mode  mode{ Mode::kNone };
		float snakeFromCentre{ 0 };   // m, horizontal: Snake from the last search's centre
		bool  lostSnake{ false };     // the post has had no line to Snake for 2 s
		bool  threatNear{ false };    // a hostile within threatNear of her post
		bool  targetUnseen{ false };  // the enemy at the view's centre can't be seen from her post
	};
	const char* NeedSearch(const Need& a_need, const Rules& a_rules);  // nullptr = not due; else why

	// A change of post in view is her cloak: fade out, publish, wait for the guest's echo, fade in. Out of view: at once.
	class Fader
	{
	public:
		enum class Step : std::uint8_t
		{
			kSteady,
			kOut,
			kWaitEcho,
			kIn,
		};
		// A new post (or none) is wanted. a_inView: she is drawn and in view now.
		void  Want(std::optional<Post> a_post, bool a_inView, std::uint64_t a_nowMs);
		// Returns true on the frame the wanted post must be published (seq + 1).
		bool  Update(std::uint64_t a_nowMs, bool a_echo, bool a_inView, const Rules& a_rules);
		float Alpha() const { return alpha_; }
		Step  Current() const { return step_; }
		const std::optional<Post>& Shown() const { return shown_; }      // the post published (drawn once echoed or waited out)
		const std::optional<Post>& Pending() const { return pending_; }  // waiting for its fade-out
		bool  Drawn() const { return shown_.has_value() && alpha_ > 0.0f; }
		bool  EchoLate() const { return echoLate_; }
		// She is at a_post now, drawn (the grace seat she gets up from becomes her post): published at once, no fade.
		void  Adopt(const Post& a_post)
		{
			shown_ = a_post;
			pending_.reset();
			havePending_ = false;
			step_ = Step::kSteady;
			alpha_ = 1.0f;
		}
		void  Shift(const Vec3& a_delta);
		void  Reset() { *this = {}; }

	private:
		Step                step_{ Step::kSteady };
		std::optional<Post> shown_, pending_;
		bool                havePending_{ false };
		std::uint64_t       since_{ 0 };
		float               alpha_{ 0 };
		float               from_{ 0 };
		bool                echoLate_{ false };
	};

	// Her ground when she isn't at a post (just deployed, moving after an order): a ray from farUp m above the mapped point to
	// farDown m below it; the first walkable hit (normal y >= 0.7 or unknown). nullopt: no floor -- she is hidden, never floating.
	std::optional<float> FarGround(std::span<const Hit> a_hitsTopDown);

	// ---- her target --------------------------------------------------------------------------------------------------------
	struct Target
	{
		std::uint64_t handle{ 0 };
		Vec3          feet;
		Vec3          chest;
		float         radius{ 0.5f };
		bool          hostile{ false };
		bool          alive{ false };
		bool          rendered{ true };
		bool          boss{ false };
		bool          held{ false };
		bool          lifting{ false };
		bool          asleep{ false };
		bool          marked{ false };
	};
	struct View
	{
		Vec3 pos;
		Vec3 fwd;
	};
	enum class PickWhy : std::uint8_t
	{
		kOk,
		kNotHostile,
		kDead,
		kHidden,
		kBoss,
		kHeld,
		kLifting,
		kAsleep,      // the Guilty Butterfly: never a sleeper
		kNoPost,
		kTooFar,      // from her post
		kOutside,     // past the Actors radius of Snake
		kOffCentre,
		kNoSightPost,
		kNoSightView,
		kNobody,
		kCount
	};
	const char* Name(PickWhy a_why);
	// The rules without rays. a_sight: her sight point (nullopt: no post). a_off gets his angle off the view's axis (rad).
	PickWhy Screen(const Target& a_t, const View& a_view, const std::optional<Vec3>& a_sight, const Vec3& a_snake, float a_actorRadius, bool a_tranq,
		bool a_binoculars, const Rules& a_rules, float* a_off = nullptr);
	using SightFn = std::function<bool(const Target&)>;
	struct PickResult
	{
		std::optional<std::size_t> index;
		PickWhy                    why{ PickWhy::kNobody };
		std::optional<std::size_t> nearest;
		float                      nearestOff{ 3.1416f };
		std::size_t                checked{ 0 };
	};
	// The eligible ones, marked first (markedFirst), then nearest the view's centre; the first a_maxChecks get the two sight tests
	// (her post's first: the shot's own ray) until one passes. Null functions pass.
	PickResult Pick(std::span<const Target> a_targets, const View& a_view, const std::optional<Vec3>& a_sight, const Vec3& a_snake, float a_actorRadius,
		bool a_tranq, bool a_binoculars, const Rules& a_rules, const SightFn& a_fromPost, const SightFn& a_fromView, std::size_t a_maxChecks = 3);
	// The pick's hard rules again (no cone, no rays): may the latched pick stay?
	bool LatchFits(const Target& a_t, const std::optional<Vec3>& a_sight, const Vec3& a_snake, float a_actorRadius, bool a_tranq, const Rules& a_rules);

	// ---- spotting ----------------------------------------------------------------------------------------------------------
	class Spotter
	{
	public:
		// a_seen: who passes the pick's hard rules and is seen from her sight point within spot_range now. Returns the new ones.
		std::vector<std::uint64_t> Update(std::span<const std::uint64_t> a_seen, std::uint64_t a_nowMs, const Rules& a_rules);
		void          Forget(std::uint64_t a_handle);  // dead, gone, no longer hostile: at once
		std::uint64_t Until(std::uint64_t a_handle) const;
		std::size_t   Count(std::uint64_t a_nowMs) const;
		void          Clear() { until_.clear(); }

	private:
		std::vector<std::pair<std::uint64_t, std::uint64_t>> until_;
	};
	// "Quiet: Enemy spotted" ("(N nearby)" with several), at most every a_everyS; "" = nothing now.
	class SpotText
	{
	public:
		std::string Update(std::size_t a_fresh, std::size_t a_total, std::uint64_t a_nowMs, float a_everyS);

	private:
		bool          said_{ false };
		bool          pending_{ false };
		std::uint64_t last_{ 0 };
	};

	// ---- her shot ----------------------------------------------------------------------------------------------------------
	proto::QuietWeapon WeaponOfAttack(std::uint32_t a_attackId);  // 290 Wicked (lethal), 291 Guilty (tranq), 292 Sinful (heavy)
	const char*        WeaponName(proto::QuietWeapon a_weapon);
	enum class ShotAct : std::uint8_t
	{
		kNone,
		kLethal,       // Wicked Butterfly: MGSV's share of his life
		kHeavy,        // Sinful Butterfly: lethal with a knockdown
		kSleep,        // Guilty Butterfly: the sleep darts' build-up
		kNotHostile,   // never: a friend, a neutral, a merchant, a summon
		kBoss,         // never a boss
		kDead,
		kGone,
		kNoPost,       // she has no post (on the move) or sits at a grace: the job shouldn't have run
		kBlocked,      // Elden Ring's line from her sight point is blocked: a miss
		kUnknownRifle, // her rifle unknown (an attack id outside 290-292 and no weapon from the guest): refused, never lethal (review)
		kCount
	};
	const char* Name(ShotAct a_act);
	struct Shot
	{
		ShotAct           act{ ShotAct::kNone };
		combat::HitEffect hit;
	};
	struct ShotTarget
	{
		bool         present{ false };
		bool         hostile{ false };
		bool         alive{ false };
		bool         boss{ false };
		std::int32_t maxHp{ 0 };
	};
	// a_share: MGSV's share of his life (kGEvQuietShot b / 10000; 0 = unknown). a_weapon from the attack id (else the event's).
	Shot ShotEffect(proto::QuietWeapon a_weapon, const ShotTarget& a_t, float a_share, bool a_hasPost, bool a_clear, const Rules& a_rules,
		const combat::HitRules& a_hit, const cqc::Rules& a_cqc, const combat::MeleeValues& a_melee);

	// ---- texts -------------------------------------------------------------------------------------------------------------
	std::string OrderText(std::uint32_t a_command, proto::QuietWeapon a_weapon);
	// The corner for a kGEvQuietShot phase ("" = nothing): Refused, Lost (by its reason), and the host's own miss reasons.
	std::string ShotText(std::uint32_t a_phase, std::uint32_t a_lostReason);
	inline constexpr const char* kMarkHint = "Quiet: mark an enemy with your binoculars to order Fire";
	inline constexpr const char* kNoClearShot = "Quiet: no clear shot";
	inline constexpr const char* kNoPostShot = "Quiet: no clear shot from her post";
	inline constexpr const char* kMovingForShot = "Quiet: moving to get a shot";
}
