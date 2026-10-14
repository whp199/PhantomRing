#pragma once

// The host's combat arithmetic (DESIGN.md §8, docs/COMBAT.md), with no game access so
// tests/test_host_combat.cpp can exercise all of it offline:
//  - shapes: Elden Ring characters as vertical capsules (feet + hit height/radius from their physics
//    module); ray vs capsule for shots, sphere vs capsule for CQC/explosions, an arc test in front of
//    Snake for strikes whose geometry MGSV doesn't give us;
//  - damage: in MGSV soldiers' lives. A hit is worth a share of one standard MGSV soldier's life -- MGSV's own number
//    when the guest sends it (protocol v8: kGEvFire pad[1]), else a class value (by TppEquip bullet id) / units per
//    life -- times a reference HP for the target: its max HP for an ordinary enemy (so a soldier dies to as many hits
//    as in MGSV, wherever he stands in Elden Ring), a growing number of lives for big ones (by max HP against the
//    Tarnished's own: a field boss is worth ~70 soldiers, so a magazine takes 10-20% off it, not all of it);
//  - what a hit does in ER: damage, poise (stagger), sleep build-up (tranquilizer), headshots;
//  - pacing: a token-bucket rate limit, and the route monitor that decides whether ER's own bullet
//    pipeline actually lands our hits (falls back to direct HP changes when it doesn't);
//  - ER -> Snake: fall damage from the puppet's landing speed.

#include "Math.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace phantom::er::combat
{
	// ---- shapes ----------------------------------------------------------------------------------
	struct Capsule
	{
		Vec3  a;           // centre of the bottom sphere
		Vec3  b;           // centre of the top sphere (a.y <= b.y)
		float r{ 0.5f };
	};

	// An upright character capsule standing on a_feet: total height a_height (clamped >= 2r).
	Capsule ChrCapsule(const Vec3& a_feet, float a_height, float a_radius);

	// Closest point on segment [a,b] to p.
	Vec3 ClosestOnSegment(const Vec3& a_a, const Vec3& a_b, const Vec3& a_p);

	// First hit of the ray a_origin + t * a_dir (a_dir unit), 0 <= t <= a_maxLen, with the capsule:
	// t, or nullopt. A ray starting inside the capsule hits at t = 0.
	std::optional<float> RayCapsule(const Vec3& a_origin, const Vec3& a_dir, float a_maxLen, const Capsule& a_c);

	// Distance from a_p to the capsule's surface (negative inside).
	float CapsuleDistance(const Capsule& a_c, const Vec3& a_p);
	bool  SphereCapsule(const Vec3& a_centre, float a_radius, const Capsule& a_c);

	// A strike in front of Snake: the capsule's closest point to a_origin is within a_range and, seen
	// from above, within a_halfAngle of a_facing (XZ; a_facing need not be unit).
	bool InArc(const Vec3& a_origin, const Vec3& a_facing, float a_range, float a_halfAngle, const Capsule& a_c);

	// Hit height as a fraction of the capsule height, 0 at the feet, 1 at the top.
	float HeightFraction(const Capsule& a_c, const Vec3& a_hit);

	// ---- MGSV rounds -----------------------------------------------------------------------------
	enum class RoundClass : std::uint8_t
	{
		kUnknown,     // no bullet id (ammo-counter fallback shots): default lethal value
		kHandgun,
		kMagnum,
		kRifle,
		kBattleRifle,
		kMachineGun,
		kSniper,
		kShotgunPellet,
		kSlug,
		kHeavy,       // cannons, turrets, vehicle guns
		kExplosive,   // grenades, rockets, missiles (area)
		kTranq,
		kStun,
		kNone,        // smoke, flares, chaff, fulton: no damage
		kFlame,       // D-Walker's flamethrower: a burst of kGEvBuddyFire with no bullet and a range (fire damage)
	};

	RoundClass ClassOfBullet(std::uint32_t a_bulletId);  // TppEquip.BL_* (tools/re/data/tppequip_bullet_ids_1.0.15.4.txt)
	std::string_view Name(RoundClass a_class);

	struct RoundValue
	{
		float damage{ 0 };  // MGSV damage units per pellet (the DESIGN 13b.5 "mgsvDamage")
		float poise{ 0 };   // Elden Ring poise (AtkParam atkSuperArmor) per pellet
	};
	RoundValue ValueOf(RoundClass a_class);

	// ---- MGSV -> Elden Ring damage --------------------------------------------------------------
	// A hit's damage = soldier lives x the target's reference HP x multipliers (docs/COMBAT.md §2):
	//  - lives: MGSV's own damage share (v8, 1.0 = one hit kills a standard soldier), or class units / unitsPerLife;
	//  - reference HP: max HP / Lives(t), t = how "big" the target is: 0 while its max HP is at most bigFrom x the
	//    Tarnished's, 1 from bossFrom x on (log-interpolated between), Lives = bossLives^t (1 for an ordinary enemy).
	//    Measured against the Tarnished, not absolute: Elden Ring scales both with the area, so a late-game soldier is
	//    as "ordinary" as a Limgrave one and still dies to 3-5 rifle hits.
	struct Scaling
	{
		float unitsPerLife{ 40.0f };  // class units in one standard soldier's life (handgun 13 -> 0.33 life, rifle 17 -> 0.43)
		float bigFrom{ 1.5f };        // max HP / the Tarnished's max HP up to which an enemy is ordinary
		float bossFrom{ 5.0f };       // ... from which it counts as a boss
		float bossLives{ 45.0f };     // soldiers' lives a boss is worth (an assault-rifle magazine with MGSV's share: ~15%)
		float playerMaxHp{ 0.0f };    // the Tarnished's max HP; 0 = unknown: every enemy is ordinary
	};

	// 0 (ordinary) .. 1 (boss-like) for a target of max HP a_maxHp.
	float Bigness(std::int32_t a_maxHp, const Scaling& a_s);
	// The HP one soldier's life stands for on this target: max HP / bossLives^Bigness.
	float ReferenceHp(std::int32_t a_maxHp, const Scaling& a_s);
	// MGSV damage units (the class table) as soldier lives.
	float LivesOfUnits(float a_units, const Scaling& a_s);

	// What one landed hit does to an Elden Ring character.
	struct HitEffect
	{
		std::int32_t damage{ 0 };      // ER HP
		float        poise{ 0 };       // AtkParam atkSuperArmor
		std::int32_t sleep{ 0 };       // sleep build-up (SpEffect sleepAttackPower) when the target's resistance is unknown
		float        sleepShare{ 0 };  // else this share of the target's sleep resistance (SleepPower)
		std::uint8_t dmgLevel{ 1 };    // AtkParam dmgLevel: 1 small flinch .. 3 large
		bool         headshot{ false };
		bool         shared{ false };  // the damage came from MGSV's own share (v8), not the class table
		bool         fire{ false };    // the damage is Elden Ring fire damage (atkFire), not physical
		bool         nonLethal{ false };  // stun / sleep: never takes the target below 1 HP (our part of it)
		std::uint16_t correction{ 0 };    // the bullet's AtkParam correction (%); 0 = chosen per target (TopUp / sleep_correction)
	};

	struct HitRules
	{
		Scaling scaling;
		float   headFraction{ 0.82f };  // hit above this fraction of the capsule = head
		float   headshotMult{ 4.5f };   // on an ordinary enemy: MGSV's headshots kill a soldier (rifle 0.43 life x 4.5)
		float   headshotMultBoss{ 1.5f };  // on a boss (interpolated with Bigness)
		// A lethal headshot on an ordinary enemy takes at least this many soldiers' lives (MGSV: a headshot kills a soldier; an
		// assault rifle's 0.21 share x 4.5 = 0.95 left a Limgrave soldier at 11 HP), fading to none at a boss (Bigness).
		float   headshotMinLives{ 1.05f };
		float   shareMult{ 1.0f };      // x MGSV's own damage share (v8)
		float   buddyMult{ 1.0f };
		float   progression{ 1.0f };    // x Elden Ring's progression ([Progression], ProgressionFactor): every lethal hit's damage
		int     tranqSleep{ 80 };
		int     tranqSleepHead{ 200 };
		float   tranqShare{ 0.6f };      // of the target's sleep resistance (0 = use the absolute values)
		float   tranqShareHead{ 1.1f };
		float   stunPoiseMult{ 1.0f };
		int     maxDamage{ 99999 };
	};

	// The headshot multiplier on a target of this size.
	float HeadshotMult(std::int32_t a_maxHp, const HitRules& a_rules);

	// a_ammo: proto::AmmoKind of the event; a_class from the bullet id (kUnknown: use a_ammo). a_share: MGSV's lethal damage
	// of one body hit as a share of a standard soldier's life (protocol v8 kGEvFire pad[1] / 10000, per pellet); 0 =
	// unknown: the class table. Only for lethal rounds (tranquilizer, stun, flame and explosive rounds keep their rules).
	HitEffect ShotEffect(RoundClass a_class, std::uint32_t a_ammo, std::uint32_t a_pellets, float a_heightFraction, std::int32_t a_targetMaxHp,
		bool a_buddy, const HitRules& a_rules, float a_share = 0.0f);

	// CQC: a_kind is proto::MeleeKind (punch/kick/throw/choke).
	struct MeleeValues
	{
		float punch{ 5.0f };
		float kick{ 7.0f };
		float throwDamage{ 4.0f };
		float throwPoise{ 80.0f };
		float strikePoise{ 25.0f };
		int   chokeSleep{ 200 };
		float chokeShare{ 1.1f };
	};
	HitEffect MeleeEffect(std::uint32_t a_kind, std::int32_t a_targetMaxHp, const MeleeValues& a_values, const HitRules& a_rules);

	// Explosion at distance a_distance from the centre of an a_radius blast (linear falloff to 25%): the explosive class's
	// lives (70 units: 1.75 at the centre) x the target's reference HP.
	HitEffect ExplosionEffect(float a_distance, float a_radius, std::int32_t a_targetMaxHp, const HitRules& a_rules);

	// The non-lethal blasts give every target in them the SAME numbers. All our hit bullets share one patched row and
	// only bullets with identical numbers fly together (MayFly): with per-target numbers (poise by distance, the
	// correction learned per enemy kind, sleep as a share of each one's resistance) the first target took the row and
	// every other one went to the HP route -- which carries neither poise (no stagger) nor sleep (lost).
	//  - a stun grenade: everyone inside staggers as at its centre (MGSV's flash does not fall off), a token of damage,
	//    a fixed small correction (kStunBlastCorrection: Elden Ring's own part of the token stays ~1 HP);
	//  - a sleep-gas puff: one build-up for everyone, a_share of the largest sleep resistance among a_resists (immune
	//    999 and unknown 0 ignored; a_absolute when none is known), at most a_cap (the ladder's capacity). Immune targets
	//    still get nothing (SleepPower of an absolute value on a 999 resistance).
	inline constexpr std::uint16_t kStunBlastCorrection = 15;
	HitEffect StunBlastEffect(const HitRules& a_rules);
	HitEffect GasBlastEffect(std::int32_t a_absolute, float a_share, const std::vector<std::int32_t>& a_resists, std::int32_t a_cap);

	// The AtkParam correction (%) a non-lethal hit (stun, sleep) may fly with on a target at a_hp: Elden Ring's own part of
	// it (its weapon x the correction, ~a_reference HP at 100% when known, else ~100 HP; up to kNonLethalSpread x that: counter
	// hits) must not take the target to 0 (review: a stun grenade's fixed 15% killed a target at 1 HP: ER's hit is never
	// clamped). Nothing to deal (a_damage <= 0: a dart, the gas, a stun hit on a target already at 1 HP) -> a_sleepCorrection
	// (0: no damage, the poise, flinch level and sleep still ride on the bullet); a target at 2 HP or less -> 0.
	inline constexpr float kNonLethalSpread = 1.6f;
	std::uint16_t NonLethalCorrection(std::uint16_t a_correction, std::int32_t a_damage, std::int32_t a_hp, std::int32_t a_reference, std::uint16_t a_sleepCorrection);

	// SpEffect row that adds a_power sleep build-up, from a ladder of rows base, base+1, ... with
	// sleepAttackPower = step, 2*step, ... (ER's own 25000..25019 = 10..200). -1 when a_power <= 0.
	std::int32_t SleepSpEffect(std::int32_t a_power, std::int32_t a_base, std::int32_t a_step, std::int32_t a_count);

	// The ladder rows for a_power, one per BulletParam spEffectId slot (at most a_slots): a build-up larger than the
	// top rung takes several. Elden Ring adds a SpEffect's sleep build-up only when a hit carries it (a bullet's
	// spEffectId0..4, as its own sleep bullets do: Bullet 2270260 -> 25006); applied directly with ApplySpEffect it
	// builds nothing (VERIFIED live: 25019 x2 and 1885 x3 on a 226-resist soldier, no sleep), and the AtkParam's
	// spEffectId slots of our bullet didn't either.
	// The sleep build-up one hit gives a target of sleep resistance a_resist (CSChrResistModule; 0 = unknown):
	// a_share of it when known and a_share > 0, 0 for an immune one (Elden Ring writes 999), else a_absolute.
	// Elden Ring's resistance already carries its area scaling and grows after every sleep (resistCorrectId), so a
	// share puts a Limgrave soldier (226) and a late-game knight to sleep with the same number of darts.
	std::int32_t SleepPower(std::int32_t a_absolute, float a_share, std::int32_t a_resist);

	std::vector<std::int32_t> SleepRungs(std::int32_t a_power, std::int32_t a_base, std::int32_t a_step, std::int32_t a_count, std::size_t a_slots = 5);

	// ---- pacing ----------------------------------------------------------------------------------
	class RateLimiter
	{
	public:
		RateLimiter(float a_perSecond = 30.0f, float a_burst = 15.0f) :
			rate_(a_perSecond), burst_(a_burst), tokens_(a_burst) {}
		void Configure(float a_perSecond, float a_burst);
		bool Allow(double a_nowMs);
		[[nodiscard]] std::uint64_t Dropped() const { return dropped_; }

	private:
		float         rate_;
		float         burst_;
		float         tokens_;
		double        lastMs_{ -1.0 };
		std::uint64_t dropped_{ 0 };
	};

	// Does Elden Ring's bullet pipeline land our hits? Every bullet hit is checked a moment later (did
	// the target lose HP or die?). Until one lands the route is unconfirmed: each miss gets the direct
	// HP fallback, and a_failLimit misses in a row give the bullet route up for the session. Once one
	// lands it is confirmed and misses are just counted (a guard, an i-frame, a miss is ER's call).
	class RouteMonitor
	{
	public:
		enum class State
		{
			kUnknown,
			kConfirmed,
			kFailed,
		};
		explicit RouteMonitor(int a_failLimit = 3) :
			failLimit_(a_failLimit) {}
		void SetFailLimit(int a_limit) { failLimit_ = a_limit; }
		// Returns true if the caller should apply the HP fallback for this missed hit.
		bool Report(bool a_landed);
		[[nodiscard]] State         Current() const { return state_; }
		[[nodiscard]] std::uint64_t Landed() const { return landed_; }
		[[nodiscard]] std::uint64_t Missed() const { return missed_; }
		void                        Reset() { *this = RouteMonitor(failLimit_); }

	private:
		int           failLimit_;
		State         state_{ State::kUnknown };
		int           streak_{ 0 };
		std::uint64_t landed_{ 0 };
		std::uint64_t missed_{ 0 };
	};
	std::string_view Name(RouteMonitor::State a_state);

	// May one more hit bullet fly? All our hit bullets share one patched AtkParam_Pc row (Combat.cpp SpawnHitBullet), and
	// Elden Ring reads it when the bullet lands, a frame or more after the spawn (UNVERIFIED which; FromSoftware's usual
	// design): a bullet still in flight would hit with the next one's numbers. And the top-up attributes a target's HP
	// loss to its pending bullet: two in flight on one target, and the first one counts both. So a new bullet only flies
	// when every bullet in flight was patched with the same values (a_key) and, if it carries damage, none carrying
	// damage is in flight on the same target; otherwise its damage goes through the HP route.
	struct InFlight
	{
		const void*   chr{ nullptr };
		std::uint64_t key{ 0 };      // HitKey of its patched row
		bool          hp{ false };   // carries damage (its landing is measured in HP)
	};
	// A key no real row has (a correction above 100 can't be written): a hit with it flies only when nothing else does.
	inline constexpr std::uint64_t kUniqueKey = ~0ull;
	std::uint64_t HitKey(std::uint16_t a_amount, std::uint16_t a_correction, bool a_fire, float a_poise, std::uint8_t a_dmgLevel, std::int32_t a_sleep = 0);
	bool          MayFly(const std::vector<InFlight>& a_inFlight, const void* a_chr, std::uint64_t a_key, bool a_hp);

	// The bullet route's damage, completed. Elden Ring takes a player bullet's damage from the Tarnished's own weapon
	// (AtkParam atkBehaviorId 0 = the weapon's attack x the row's correction), not from the atkPhys we write: live, a
	// 38-, 190- and 714-attack bullet all dealt ~42 to a 219 HP Gatefront soldier, a blocked one 5. So our wanted
	// damage never arrived. The bullet stays (ER's flinch, poise, aggro, guard, runes) and the rest is topped up with
	// ChangeHp once ER has applied it -- scaled by how much of a normal hit got through, so a raised shield still
	// blocks: ER's damage of an unblocked bullet on this kind of enemy is the reference (the largest seen); a hit of
	// half of it or more counts as unblocked (ER's own variance), less as guarded (that share of the wanted damage).
	class TopUp
	{
	public:
		// The bullet's AtkParam correction (%) for a wanted damage: 100 (ER's normal hit, topped up) unless that alone
		// would already deal more than wanted -- a stun round (a handful of HP) must not land a full weapon hit -- then
		// the share of the reference it wants. Without a reference yet: 100 for a real hit, a small guess (15) for less
		// than kSmall.
		static constexpr std::int32_t kSmall = 20;
		[[nodiscard]] std::uint16_t Correction(std::int32_t a_npc, std::int32_t a_wanted) const;
		// ER dealt a_dealt (HP) for our bullet that wanted a_wanted on an enemy of NpcParam a_npc, fired with a_correction;
		// the HP to add. Only a full (100%) hit teaches the reference and is scaled by it (a reduced one is taken as
		// landed: whatever it wanted is small).
		std::int32_t Extra(std::int32_t a_npc, std::int32_t a_wanted, std::int32_t a_dealt, std::uint16_t a_correction = 100);
		[[nodiscard]] std::int32_t Reference(std::int32_t a_npc) const;

	private:
		std::vector<std::pair<std::int32_t, std::int32_t>> ref_;  // {npc, largest ER damage of one of our bullets}
	};

	// ---- Damage Elden Ring itself carries ([Combat] damage_route = speffect; off by default, UNVERIFIED live) ---------
	// The top-up (above) writes HP directly, which a networked session never sees: the session host owns its enemies' HP
	// (DESIGN §14c, ROADMAP §4 C1). The alternative puts the wanted damage on the hit bullet itself, as fixed-damage
	// SpEffect rows in its BulletParam spEffectId slots (the slots the sleep ladder already uses): Elden Ring applies them
	// with the hit, so whatever carries the hit to the enemy's owner carries them too.
	//
	// The ladder: 9 x a_digits rows from a_base, one per decimal digit of each place -- row base + 9 p + (d - 1) takes
	// d x 10^p HP (SpEffectParam changeHpPoint; positive = damage, as Elden Ring's own hemorrhage rows). One hit carries one
	// row per non-zero digit, so 4 digits deal any 0..9999 exactly in at most 4 of the 5 slots (MGSV's max_damage is 9999).
	// The rows are carriers: blank rows of this regulation (20010640.., unreferenced by any param) that every
	// PhantomRing install writes the same fixed values into while Snake plays, so both ends of a session agree on them.
	struct DamageLadder
	{
		std::int32_t base{ 20010640 };
		int          digits{ 4 };
	};
	inline constexpr int kDamageLadderMaxDigits = 5;
	int          DamageLadderRows(const DamageLadder& a_l);   // 9 x digits (digits clamped to 1..kDamageLadderMaxDigits)
	std::int32_t DamageLadderMax(const DamageLadder& a_l);    // 10^digits - 1
	std::int32_t DamageRungRow(int a_place, int a_digit, const DamageLadder& a_l);  // -1 out of range (digit 1..9)
	std::int32_t DamageRungValue(std::int32_t a_row, const DamageLadder& a_l);     // HP of a ladder row; 0 if not one
	// a_amount (clamped to 0..max) as the nearest value with at most a_slots non-zero digits (half rounds up; a round-up
	// past the ladder's top rounds down instead). Exact when it already fits.
	std::int32_t DamageRoundToSlots(std::int32_t a_amount, std::size_t a_slots, const DamageLadder& a_l);
	// The rows for a_amount (DamageRoundToSlots first), largest place first, at most a_slots.
	std::vector<std::int32_t> DamageRungs(std::int32_t a_amount, const DamageLadder& a_l, std::size_t a_slots = 4);
	std::int32_t              DamageOfRungs(const std::vector<std::int32_t>& a_rows, const DamageLadder& a_l);

	// How one hit is split between Elden Ring's own bullet damage and the ladder, and what its outcome teaches.
	//  - The bullet still flies with a small AtkParam correction: Elden Ring's own hit (its weapon x the correction) gives the
	//    flinch, poise, aggro and the guard reaction and is aimed at bulletShare of the wanted damage. Its 100% damage on
	//    this kind of enemy (the reference) is learned from landed hits (ER's part / correction); before one is known the
	//    bullet flies with firstCorrection and the ladder carries all of the wanted damage (a few HP over, once).
	//  - The ladder carries the rest: wanted - ER's expected part.
	//  - Guard: a raised shield cuts Elden Ring's part (a hit of less than half the expected part = guarded, as TopUp). Whether
	//    Elden Ring also drops a bullet's SpEffects on a guarded hit is UNKNOWN until seen live, so it is learned: a guarded
	//    hit whose ladder damage arrived anyway (kApplies) makes this target's next hits within guardMemoryMs carry only the
	//    share of a normal hit its shield let through; a guarded hit whose ladder damage did not arrive (kSkipped) shows
	//    Elden Ring blocks it itself, and nothing is scaled. Until a guard is seen: kUnknown, scaled as kApplies.
	struct NativePlan
	{
		std::uint16_t correction{ 100 };  // the bullet's AtkParam correction (%)
		std::int32_t  erExpected{ 0 };    // Elden Ring's own part we expect (0: no reference yet)
		std::int32_t  ladder{ 0 };        // HP the SpEffect rows should carry (before DamageRoundToSlots)
		float         guardFactor{ 1.0f };  // < 1: this target was seen guarding a moment ago
	};
	struct NativeOutcome
	{
		std::int32_t erPart{ 0 };         // Elden Ring's own damage of the bullet
		bool         ladderLanded{ false };  // the carried rows' damage arrived
		bool         guarded{ false };
		float        guardShare{ 1.0f };  // of a normal hit that got through (guarded only)
	};
	class NativeDamage
	{
	public:
		enum class GuardRule : std::uint8_t
		{
			kUnknown,
			kApplies,  // a guarded hit still delivers the ladder: we scale it by the shield's share
			kSkipped,  // Elden Ring drops a guarded hit's SpEffects itself
		};
		struct Rules
		{
			float         bulletShare{ 0.1f };      // Elden Ring's own part of the wanted damage
			std::uint16_t firstCorrection{ 10 };    // correction before the reference is known
			double        guardMemoryMs{ 1500.0 };  // a target seen guarding is treated as guarding this long
		};
		static constexpr double kMinGuardJudge = 4.0;  // ER's expected part (HP) below which no guard is judged
		[[nodiscard]] NativePlan Plan(std::int32_t a_npc, const void* a_chr, std::int32_t a_wanted, double a_nowMs, const Rules& a_rules) const;
		// A hit flown with a_correction carrying a_carried (the rows' sum) dealt a_dealt HP in all. a_ladderConfirmed: the
		// ladder's rows have arrived on some hit before (its RouteMonitor is kConfirmed). Only then does a guarded hit whose rows
		// did not arrive teach kSkipped -- with rows that never work, a guarded miss tells nothing about guards (review).
		NativeOutcome Learn(std::int32_t a_npc, const void* a_chr, std::uint16_t a_correction, std::int32_t a_carried, std::int32_t a_dealt, double a_nowMs,
			bool a_ladderConfirmed);
		[[nodiscard]] std::int32_t Reference(std::int32_t a_npc) const;  // ER's 100% hit on this kind of enemy (0 unknown)
		[[nodiscard]] GuardRule    Guard() const { return guard_; }
		void                       Reset() { *this = NativeDamage(); }

	private:
		struct Shield
		{
			const void* chr{ nullptr };
			double      atMs{ 0 };
			float       share{ 1.0f };
		};
		std::vector<std::pair<std::int32_t, float>> ref_;  // {npc, ER's 100% hit (moving average of unguarded hits)}
		std::vector<Shield>                         shields_;
		GuardRule                                   guard_{ GuardRule::kUnknown };
	};
	std::string_view Name(NativeDamage::GuardRule a_rule);

	// One landed ladder hit for the ladder's RouteMonitor (Combat.cpp Tick): true when it is to be topped up (the ladder still
	// unconfirmed and its rows missing). Only rows that arrived confirm the ladder (COMBAT.md "Did it arrive?"). A guarded hit
	// whose rows did not arrive is no evidence either way -- Elden Ring may drop a guarded hit's SpEffects, or the rows never
	// work at all -- so it is neither counted nor topped up (the shield blocked it). It used to confirm the ladder: with broken
	// rows one shield (or one of Elden Ring's 1 HP parts read as guarded) then left every later hit at ER's ~10% for the session.
	bool ReportLadder(RouteMonitor& a_monitor, const NativeOutcome& a_outcome, std::int32_t a_carried);

	// ---- Elden Ring's progression -> Snake's damage ([Progression] damage_scaling, off by default) -----------------
	// Snake's damage is counted in MGSV soldiers' lives against each target's own HP, so it never grows with the Tarnished.
	// The option makes Elden Ring's progression count: his rune level, the upgrade of the armament in his right hand, or
	// both, as a factor on every lethal hit (shots, CQC, blasts, D-Walker); sleep and stun build-up stay as they are. Vigor
	// needs no option: Elden Ring's hits already cost Snake the share of the Tarnished's max HP (HurtForGuest).
	enum class ProgressionMode : std::uint8_t
	{
		kOff,
		kLevel,
		kWeapon,
		kBoth,
	};
	ProgressionMode  ProgressionModeOf(std::string_view a_text);  // off|level|weapon|both; anything else off
	std::string_view Name(ProgressionMode a_mode);

	struct ProgressionRules
	{
		ProgressionMode mode{ ProgressionMode::kOff };
		float           levelReference{ 20.0f };  // rune level of x1
		float           levelExponent{ 0.5f };    // x (level / reference)^exponent
		float           weaponBonus{ 1.5f };      // x (1 + bonus x upgrade / max upgrade)
		float           minFactor{ 0.5f };
		float           maxFactor{ 3.0f };
	};

	struct ProgressionStats
	{
		int level{ 0 };        // rune level; 0 = unknown (that part of the factor is 1)
		int upgrade{ -1 };     // the right-hand armament's upgrade (+N); -1 = unknown
		int maxUpgrade{ 0 };   // its maximum (25, or 10 for somber ones); 0 = unknown
	};

	float LevelFactor(int a_level, const ProgressionRules& a_rules);
	float WeaponFactor(int a_upgrade, int a_maxUpgrade, const ProgressionRules& a_rules);
	// The factor for this mode, kept within [minFactor, maxFactor]; 1 when off.
	float ProgressionFactor(const ProgressionRules& a_rules, const ProgressionStats& a_stats);

	// Elden Ring's rune level is the sum of the eight attributes minus 79 (Vigor .. Arcane): the reads check themselves with it.
	bool LevelMatchesAttributes(int a_level, const int (&a_attributes)[8]);
	// An Elden Ring weapon id is its EquipParamWeapon row (base + infusion x 100) plus the upgrade: (id / 100) x 100 and id % 100.
	inline int WeaponRowOf(int a_weaponId) { return a_weaponId >= 0 ? (a_weaponId / 100) * 100 : -1; }
	inline int UpgradeOf(int a_weaponId) { return a_weaponId >= 0 ? a_weaponId % 100 : -1; }
	// The highest upgrade of a weapon of ReinforceParamWeapon type a_reinforceType: the rows reinforceType + 1, + 2, ... that
	// exist (25 for most weapons, 10 for somber ones; 0 when it has none).
	template <class RowExists>
	int MaxUpgrade(int a_reinforceType, RowExists&& a_rowExists)
	{
		int n = 0;
		while (n < 99 && a_rowExists(a_reinforceType + n + 1)) {
			++n;
		}
		return n;
	}

	// ---- Elden Ring -> Snake ---------------------------------------------------------------------
	// Fraction of Snake's life a landing at a_speed (m/s, downward) costs: 0 below a_hurtSpeed, 1 at or
	// above a_lethalSpeed (ER: ~8 m hurts, ~20 m kills), at least 10% once it hurts.
	float FallFraction(float a_speed, float a_hurtSpeed, float a_lethalSpeed);

	// The kHEvHurt amount (ER damage units, the guest multiplies by its life scale) that costs a_fraction
	// of a_lifeMax MGSV life points; a lethal fall rounds up so it really kills.
	std::uint32_t FallHurtAmount(float a_fraction, std::uint16_t a_lifeMax, float a_guestLifeScale);

	// An Elden Ring hit on the puppet as kHEvHurt's amount (the guest takes amount x its life scale off Snake's life).
	// a_share: Snake loses the share of his life the Tarnished would have lost (a_erDamage / a_erMaxHp of
	// a_guestLifeMax); a fixed ratio made a Limgrave soldier's 44 of a 522 HP Tarnished 2% of Snake's 7801 life,
	// regenerated in a fraction of a second (live). Else the raw Elden Ring damage. x a_mult; at least 1 for a hit.
	std::uint32_t HurtForGuest(std::int32_t a_erDamage, std::int32_t a_erMaxHp, std::uint32_t a_guestLifeMax, float a_guestLifeScale, bool a_share,
		float a_mult);

	// The puppet's HP that shows Snake's life on Elden Ring's HP bar: the same fraction of the Tarnished's max HP,
	// at least 1 while Snake lives (NoDead keeps the last point; 0 would be Elden Ring's own death).
	std::int32_t MirrorHp(std::uint16_t a_life, std::uint16_t a_lifeMax, std::int32_t a_erMaxHp);

	// An Elden Ring heal on the puppet (Crimson Tears, a grace rest, a regenerating talisman) as kHEvHeal's amount:
	// the same share of Snake's life, in kHEvHurt units (the guest multiplies by its life scale). Small heals (a
	// regeneration tick of 1-2 HP) would round to nothing: the remainder is carried to the next one in a_carry.
	std::uint32_t HealForGuest(std::int32_t a_erHealed, std::int32_t a_erMaxHp, std::uint32_t a_guestLifeMax, float a_guestLifeScale, double& a_carry);
}
