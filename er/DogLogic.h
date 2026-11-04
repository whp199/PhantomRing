#pragma once

// D-Dog in Elden Ring: the pure rules (docs/research/ddog.md §3; er/Dog.h is the game side; tests/test_host_dog.inc). No game
// access here: the game side gathers the characters, casts the rays and applies what these functions decide.
//  - spotting (§3.1): every hostile, living, rendered Elden Ring character within spot_range m of the dog (horizontally) and
//    spot_height m above or below it gets a provisional mark (er/Marking.h) that lingers spot_linger s after he leaves the range;
//    no line of sight (MGSV's dog smells them); "D-Dog: Found enemy" at most every spot_text_every s;
//  - the pick (§3.3): the one target D-Dog's next attack / fetch order takes (kActorDogTarget): hostile, alive, rendered, between
//    target_min and target_range m of Snake, within target_cone_deg of the view's centre, in plain sight of the camera, a clear
//    straight run from the dog (ClearRun), never a boss while [Dog] bosses = off, never one a CQC hold or a Fulton balloon has,
//    sleepers only for the fetch style; marked ones first, then the one nearest the centre. A non-hostile character (a merchant,
//    a quest character, a spirit summon, anyone Elden Ring's team relation doesn't make the Tarnished's enemy) is never a
//    candidate at all;
//  - the bite's effect (§3.6) per MGSV style: busy = held (CQC's hold) with a flinch now and then, kill = MGSV's lethal share of
//    the knife as Elden Ring damage with a knockdown, knock out = the sleep darts' build-up with a knockdown, wound = a hit down
//    to 1 HP at most with a knockdown, fetch = nothing (MGSV's Fulton arrives as kGEvFulton); a boss: nothing, or one stagger
//    with bosses = stagger;
//  - Bark (§3.2, command 10): the active decoy's rule around the dog (heard, no cone, no ray): awake, non-boss hostiles within
//    bark_range m that aren't busy with Snake stare at the dog.

#include "CombatLogic.h"
#include "CqcLogic.h"
#include "Math.h"

#include "phantomring_protocol.h"

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
	struct DogConfig;
}

namespace phantom::er::dog
{
	struct Rules
	{
		// spotting
		float spotRange{ 40.0f };      // m from the dog, horizontally
		float spotHeight{ 8.0f };      // m above or below it
		float spotLinger{ 10.0f };     // s a provisional mark stays after he leaves the range
		float spotTextEvery{ 8.0f };   // s between two "Found enemy" lines
		// the pick
		float targetRange{ 25.0f };    // m from Snake
		float targetMin{ 3.5f };       // m from Snake: nearer ones are CQC's
		float targetConeDeg{ 10.0f };  // off the view's centre
		bool  markedFirst{ true };
		bool  clearRun{ true };
		float runStepMax{ 0.8f };      // m between two ground samples of the run
		bool  bossStagger{ false };    // [Dog] bosses = stagger (else off: never a target)
		// the bite's effects
		float busyHold{ 6.0f };
		float busyFlinchEvery{ 1.5f };
		float killShareMult{ 1.0f };
		float killShareFallback{ 1.2f };
		float woundShare{ 1.0f };
		float knockoutSleep{ 1.1f };
		// bark
		float barkRange{ 20.0f };
		float barkStare{ 5.0f };
		float barkCooldown{ 15.0f };
		float barkSnakeClose{ 4.0f };  // m: one this near Snake (and nearer him than the dog) is busy with him
	};
	Rules MakeRules(const DogConfig& a_config);

	inline constexpr float kBossStaggerCooldown = 20.0f;  // s: bosses = stagger, per boss
	inline constexpr float kRunSpacing = 0.5f;            // m between two ground samples of the run
	inline constexpr float kRunAbove = 2.0f;              // m: each sample's down-ray starts this far over the line
	inline constexpr float kRunBelow = 4.0f;              // m: ...and finds ground at most this far under it (else a drop)
	inline constexpr float kRunKnee = 0.5f;               // m: the wall ray's height over the ground

	// ---- the pick ----------------------------------------------------------------------------------------------------------
	struct Candidate
	{
		std::uint64_t handle{ 0 };
		Vec3          feet;
		Vec3          aim;               // his chest (what the camera looks at)
		float         radius{ 0.5f };    // his body (the cone counts it)
		bool          hostile{ false };  // Elden Ring's team relation makes him the Tarnished's enemy (er/Dog.cpp: both team reads)
		bool          alive{ false };
		bool          rendered{ true };  // Elden Ring renders him (ChrIns+0x1C5 bit 3): not an event's hidden one, not a dummy
		bool          boss{ false };     // cqc::IsBoss (er/Cqc.h IsBossHp)
		bool          held{ false };     // a CQC hold (Snake's or the dog's) has him
		bool          lifting{ false };  // a Fulton balloon has him
		bool          asleep{ false };
		bool          marked{ false };   // binoculars, or the dog's provisional mark
	};

	struct View
	{
		Vec3 pos;
		Vec3 fwd;  // normalised or not
	};

	enum class Why : std::uint8_t
	{
		kOk,
		kNotHostile,  // a friend, a neutral, a merchant, a spirit summon...: never
		kDead,
		kHidden,      // not rendered
		kLifting,
		kBoss,
		kHeld,
		kAwake,       // the fetch style takes sleepers only
		kTooFar,
		kTooNear,
		kOffCentre,
		kNoSight,     // a wall between the camera and him
		kNoRun,       // the dog can't run to him straight on Elden Ring's ground
		kNobody,      // no character around at all
		kCount
	};
	const char* Name(Why a_why);

	// The rules without rays: kOk or why not. a_off gets his angle (rad) off the view's axis (pi behind the camera).
	Why Screen(const Candidate& a_c, const View& a_view, const Vec3& a_snake, bool a_fetch, const Rules& a_rules, float* a_off = nullptr);

	enum class Run : std::uint8_t
	{
		kClear,
		kStep,  // a step up or down between two samples larger than run_step_max
		kDrop,  // a sample with no ground within kRunBelow under the line
		kWall,  // the knee-high ray along the line hit something before his body
	};
	const char* Name(Run a_run);

	// The ground samples of the dog's straight run, a_dog -> a_target's feet, stopping a_stopShort m before his feet (his body):
	// every kRunSpacing m, the first at the dog, the last at the stop (at least these two).
	std::vector<Vec3> RunPoints(const Vec3& a_dog, const Vec3& a_target, float a_stopShort);
	// a_ground[i]: the ground height under sample i (nullopt: nothing within kRunBelow: a drop); a_wall: the knee-high ray hit.
	Run ClearRun(std::span<const std::optional<float>> a_ground, bool a_wall, float a_stepMax);

	struct PickResult
	{
		std::optional<std::size_t> index;      // into the candidates
		Why                        why{ Why::kNobody };  // kOk with a pick; else why the best-placed hostile one wasn't taken
		std::optional<std::size_t> nearest;    // that best-placed hostile one (the refused one), if any
		float                      nearestOff{ 3.1416f };  // his angle off the view's axis (rad)
		Run                        run{ Run::kClear };   // with kNoRun: what refused the run
		std::size_t                checked{ 0 };         // candidates the rays were cast for
	};
	using SightFn = std::function<bool(const Candidate&)>;  // true: the camera sees him (nothing in between)
	using RunFn = std::function<Run(const Candidate&)>;     // the dog's run to him
	// The eligible ones (Screen), marked ones first when a_rules.markedFirst, then nearest the view's centre; the first a_maxChecks
	// get the sight and run checks until one passes. Null functions pass.
	PickResult Pick(std::span<const Candidate> a_candidates, const View& a_view, const Vec3& a_snake, bool a_fetch, const Rules& a_rules,
		const SightFn& a_sight, const RunFn& a_run, std::size_t a_maxChecks = 3);

	// The corner line while LB is held and nothing is picked: why the enemy the player looks at isn't taken (he is within twice the
	// cone of the centre), else "look at an enemy". Pure (tests).
	std::string NoPickText(const PickResult& a_result, const Rules& a_rules);

	// MGSV's call menu points at the attack entry (right; Wait above it, DD below it, the whistle up near the top): the right stick
	// pushed over half way within 35 degrees of right. Only then does a hold with no pick say why (an LB hold for Wait says nothing).
	bool PointsAtAttack(std::int16_t a_rx, std::int16_t a_ry);

	// ---- the shown pick, kept through the click (0.8.0 polish) --------------------------------------------------------------
	// The player sees the paw over the pick while LB is held, pushes the right stick to the attack entry, lets it go and clicks it:
	// in the 0.8.0 integration run 2 of 16 orders were refused "out of sight" the instant the order came although the paw had shown
	// a moment before (the 4 Hz pick, re-run at the order, lost him for a few hundred ms). The pick the rules last chose stays the
	// published one (kActorDogTarget, the paw) for a_latchMs after they stop choosing him -- nobody, or someone else -- while he
	// still passes every hard rule (LatchFits: hostile, alive, rendered, not a boss, not held or lifted, asleep for Get it, within
	// range, at most kLatchConeMult x the cone off the centre) and the dog's run to him is still clear (LatchKeeps, with [Dog]
	// clear_run: a wall, a drop or a step the dog can't take ends the latch -- review 0.8.0); sight is not re-asked meanwhile.
	inline constexpr std::uint64_t kPickLatchMs = 500;
	inline constexpr std::uint64_t kClickGraceMs = 800;  // the paw shown this recently over one who still fits: the click takes him
	inline constexpr float         kLatchConeMult = 3.0f;
	// Screen() without the cone, sight and run: may the latched one stay the pick?
	bool LatchFits(const Candidate& a_c, const View& a_view, const Vec3& a_snake, bool a_fetch, const Rules& a_rules);
	// LatchFits and, with a_rules.clearRun, a clear run (a_run is asked only when the rest fits): what keeps (or revives) the latch.
	bool LatchKeeps(const Candidate& a_c, const View& a_view, const Vec3& a_snake, bool a_fetch, const Rules& a_rules, const RunFn& a_run);
	class PickLatch
	{
	public:
		// a_fresh: the rules' pick this time (0 = nobody); a_fits: LatchFits for the one latched now (ignored when he is a_fresh).
		// Returns the handle to publish.
		std::uint64_t Update(std::uint64_t a_fresh, bool a_fits, std::uint64_t a_nowMs, std::uint64_t a_latchMs = kPickLatchMs);
		// The click came while nobody was published, the paw having shown over a_handle a moment before (kClickGraceMs; he still fits):
		// he is the pick again, for a_latchMs from now -- the guest looks for the pick again for 350 ms after the click.
		void          Revive(std::uint64_t a_handle, std::uint64_t a_nowMs)
		{
			handle_ = a_handle;
			freshAt_ = a_nowMs;
			held_ = true;
		}
		std::uint64_t Handle() const { return handle_; }
		bool          Held() const { return held_; }      // the last Update kept him although the rules didn't choose him
		std::uint64_t FreshAt() const { return freshAt_; }  // ms: the rules last chose him
		void          Clear() { *this = {}; }

	private:
		std::uint64_t handle_{ 0 };
		std::uint64_t freshAt_{ 0 };
		bool          held_{ false };
	};

	// ---- spotting ----------------------------------------------------------------------------------------------------------
	struct Smelled
	{
		std::uint64_t handle{ 0 };
		Vec3          pos;
		bool          hostile{ false };
		bool          alive{ false };
		bool          rendered{ true };
	};

	class Spotter
	{
	public:
		// One update: who the dog smells now (a_dog: its feet in Elden Ring; nullopt = not placed: nobody new, the spotted linger).
		// Returns the newly spotted. A dead or non-hostile one is forgotten at once (his mark fades on Marking's own rule).
		std::vector<std::uint64_t> Update(std::span<const Smelled> a_seen, const std::optional<Vec3>& a_dog, std::uint64_t a_nowMs, const Rules& a_rules);
		bool          Spotted(std::uint64_t a_handle, std::uint64_t a_nowMs) const;
		std::uint64_t Until(std::uint64_t a_handle) const;  // ms; 0 = not spotted
		std::size_t   Count(std::uint64_t a_nowMs) const;
		std::span<const std::pair<std::uint64_t, std::uint64_t>> All() const { return until_; }
		void          Clear() { until_.clear(); }

	private:
		std::vector<std::pair<std::uint64_t, std::uint64_t>> until_;  // handle, spotted until (ms)
	};

	// "D-Dog: Found enemy" (with "(N nearby)" when several are spotted): when the spotted set gained someone, at most every a_everyS;
	// a gain inside the quiet time is said when it ends if someone is still spotted. "" = nothing to say now.
	class FoundText
	{
	public:
		std::string Update(std::size_t a_fresh, std::size_t a_total, std::uint64_t a_nowMs, float a_everyS);

	private:
		bool          said_{ false };
		bool          pending_{ false };
		std::uint64_t last_{ 0 };
	};

	// ---- the bite's effect -------------------------------------------------------------------------------------------------
	enum class Act : std::uint8_t
	{
		kNone,
		kHold,         // busy: held busy_hold s (Cqc::HoldFor) with a flinch now and every busy_flinch_every s
		kFlinch,       // busy, but the hold can't be taken (Snake holds him, hold cooldown): the flinch only
		kKill,
		kKnockOut,
		kWound,
		kFetch,        // nothing here: MGSV's Fulton follows (kGEvFulton)
		kStagger,      // a boss with bosses = stagger
		kBossRefused,  // a boss with bosses = off: nothing at all
		kCount
	};
	const char* Name(Act a_act);

	struct Effect
	{
		Act               act{ Act::kNone };
		combat::HitEffect hit;
		float             holdSeconds{ 0 };
	};
	// a_share: MGSV's damage share of the dog's attack (kGEvDogAttack b / 10000; 0 = unknown: kill_share_fallback). a_holdRefused:
	// busy can't hold him. a_bossCooling: bosses = stagger, but he was staggered less than kBossStaggerCooldown ago.
	Effect EffectOf(proto::DogStyle a_style, std::int32_t a_maxHp, float a_share, bool a_boss, bool a_holdRefused, bool a_bossCooling, const Rules& a_rules,
		const combat::HitRules& a_hit, const cqc::Rules& a_cqc, const combat::MeleeValues& a_melee);
	// The busy hold's flinch (the CQC grab's: a token 1 HP and its poise).
	combat::HitEffect Flinch(std::int32_t a_maxHp, const combat::HitRules& a_hit, const cqc::Rules& a_cqc, const combat::MeleeValues& a_melee);

	// The style the player's call-menu command names (11 busy, 12 kill, 13 knock out, 14 wound, 15 fetch; docs/research/ddog.md
	// §2.1), for a kGEvDogAttack whose style the guest couldn't read; nullopt for any other command.
	std::optional<proto::DogStyle> StyleOfCommand(std::uint32_t a_command);
	// A Refused / Lost that says nothing in the corner: the player's own whistle (8), Wait (9) or DD (16) came a_msSince ago (within
	// kQuietEndMs: it ended the order, nothing failed), or D-Dog isn't live (a load, Snake dying). Review 0.8.0.
	inline constexpr std::uint64_t kQuietEndMs = 2500;
	bool QuietEnd(std::uint32_t a_lastCommand, std::uint64_t a_msSince, bool a_live);
	const char*                    StyleName(std::uint32_t a_style);
	const char*                    PhaseName(std::uint32_t a_phase);

	// ---- Bark --------------------------------------------------------------------------------------------------------------
	struct Listener
	{
		Vec3  eye;
		Vec3  forward;
		float toSnake{ 1e9f };  // m to Snake (a large number when unknown)
		bool  hostile{ false };
		bool  asleep{ false };
		bool  boss{ false };
		bool  held{ false };
		bool  lifting{ false };
	};
	// Does he stop and stare at the dog (a_dogChest)? The active decoy's rule (decoy::Notices: heard within bark_range, no cone,
	// no ray; not when he is busy with Snake), never a sleeper, a boss, one held or lifted, or a non-hostile one.
	bool HearsBark(const Listener& a_l, const Vec3& a_dogChest, const Rules& a_rules);
}
