#pragma once

// CQC holds and throws on Elden Ring enemies (ROADMAP 2.2 tier 1, protocol v13 kGEvCqc, docs/COMBAT.md §7a): the arithmetic,
// with no game access (tests/test_host_cqc.inc):
//  - who is grabbed: the nearest hostile whose capsule is within reach of Snake's chest and inside the arc in front of him;
//  - what each MGSV action does to him: a grab holds him in place for a moment (a small flinch, then ChrIns no-attack/no-move),
//    a throw knocks him down (big poise and a knockdown dmgLevel through the bullet route, only a token of damage: MGSV's own
//    strike that comes with the press is the damage), a choke is the sleep build-up of [Combat] choke_sleep / choke_share (the
//    sleep ladder), interrogate a corner line, release lets go; choke and interrogate only ever reach the one actually held;
//  - never a boss: Elden Ring's big ones (Bigness at or above boss_bigness, max HP at or above boss_hp, or the Tarnished's own
//    max HP not known yet) only stagger, and are never held;
//  - no hold chain: the one just let go can't be held again for hold_cooldown (a re-grab is only a flinch).

#include "CombatLogic.h"
#include "Math.h"

#include "phantomring_protocol.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace phantom::er::cqc
{
	struct Rules
	{
		float        range{ 1.6f };          // m from Snake's chest to the enemy's capsule
		float        arcDeg{ 70.0f };        // half angle in front of Snake
		float        bossBigness{ 0.5f };    // combat::Bigness at or above this: a boss (stagger only)
		float        holdSeconds{ 6.0f };    // a grab holds him at most this long (a release lets go earlier)
		float        leaveDistance{ 3.0f };  // ...or until Snake is this far from him (m, the puppet moved on)
		float        grabPoise{ 15.0f };
		std::uint8_t grabDmgLevel{ 1 };      // small flinch: he is caught
		float        throwPoise{ 120.0f };
		std::uint8_t throwDmgLevel{ 4 };     // AtkParam dmgLevel of the throw (UNVERIFIED which value is Elden Ring's knockdown)
		float        staggerPoise{ 40.0f };  // a boss's answer to any grab, throw or choke
		std::uint8_t staggerDmgLevel{ 2 };
		// An absolute floor: Bigness is relative to the Tarnished, so a levelled one (Vigor 60, ~1900 HP) made Margit (4174) and a
		// Tree Sentinel (~3000) "ordinary". 0 = off.
		std::int32_t bossHp{ 2000 };
		// After a hold ends, no new hold on him for this long (s): a release + re-grab every ~0.8 s kept him frozen ~6 s of 7.
		float        holdCooldown{ 8.0f };
		// AtkParam correction (%) of the flinch / knockdown / stagger bullets, which then carry a token 1 HP (non-lethal: capped
		// by combat::NonLethalCorrection). A correction-0 bullet is what a sleep dart flies with, and live that is "no damage,
		// no flinch" (COMBAT §2): the poise and dmgLevel likely need some of Elden Ring's own hit. 0 = damage-free as before.
		std::uint16_t hitCorrection{ 10 };
	};

	struct Candidate
	{
		combat::Capsule cap;
		std::int32_t    maxHp{ 0 };
		bool            hostile{ false };
	};

	// The candidate grabbed by Snake (chest a_snake, facing a_facing in XZ, need not be unit): hostile, within range, in the arc,
	// the nearest (capsule surface distance). nullopt: nobody.
	std::optional<std::size_t> SelectTarget(const Vec3& a_snake, const Vec3& a_facing, const std::vector<Candidate>& a_candidates, const Rules& a_rules);

	// A boss: big against the Tarnished (Bigness), big outright (bossHp), or unknown (the Tarnished's max HP or his own not read
	// yet: stagger only is the safe answer).
	bool IsBoss(std::int32_t a_maxHp, const combat::Scaling& a_scaling, const Rules& a_rules);

	enum class Response : std::uint8_t
	{
		kNone,
		kHold,       // flinch + held in place
		kKnockdown,  // thrown
		kSleep,      // choked: sleep build-up
		kGag,        // interrogated: a corner line
		kRelease,    // let go
		kStagger,    // a boss: a stagger and nothing else
		kFlinch,     // a re-grab inside hold_cooldown: the grab's flinch, no hold
		kCount
	};
	// Who the action lands on (er/Cqc.cpp knows): a_boss; a_held: he is the one Snake holds now; a_cooling: he was let go less than
	// hold_cooldown ago.
	struct Situation
	{
		bool boss{ false };
		bool held{ false };
		bool cooling{ false };
	};
	Response         Respond(proto::CqcAction a_action, const Situation& a_situation);
	// Choke, interrogate and MGSV's hold go on only to the one held (never to whoever walked up meanwhile); grab and throw take
	// the nearest in front when nobody is held.
	bool             NeedsHold(proto::CqcAction a_action);
	std::string_view Name(Response a_response);
	std::string_view Name(proto::CqcAction a_action);

	// The hit a response lands (empty for kNone/kGag/kRelease). Never real damage: the throw's and the hold's numbers ride on a
	// bullet with hitCorrection and a token 1 HP (non-lethal), or a damage-free one when hitCorrection is 0; the choke is MGSV's
	// choke as [Combat] already defines it (damage-free, the sleep darts' route).
	combat::HitEffect EffectOf(Response a_response, std::int32_t a_maxHp, const combat::MeleeValues& a_melee, const combat::HitRules& a_hit, const Rules& a_rules);

	// Per enemy: no new hold before a moment (milliseconds, any clock).
	class Cooldowns
	{
	public:
		void Start(std::uint64_t a_handle, std::uint64_t a_nowMs, float a_seconds);
		bool Cooling(std::uint64_t a_handle, std::uint64_t a_nowMs) const;
		void Expire(std::uint64_t a_nowMs);
		void Clear() { m_until.clear(); }
		std::size_t Size() const { return m_until.size(); }

	private:
		std::vector<std::pair<std::uint64_t, std::uint64_t>> m_until;  // handle, no hold before
	};

	// A CQC hit waits for Combat's bullet row: one in flight (MGSV's own punch from the same press, kGEvMelee before kGEvCqc)
	// makes any other key "busy", and a damage-free one then lands nothing. Not before a_settleMs after it was asked for (so the
	// punch of the same press takes the row first), then as soon as no bullet flies, or a_deadlineMs after the settle regardless.
	enum class Wait : std::uint8_t
	{
		kWait,
		kFire,
		kFireLate,  // the deadline: fired into whatever is in flight (it may land nothing)
	};
	Wait WhenToFire(std::uint64_t a_nowMs, std::uint64_t a_askedMs, std::uint32_t a_settleMs, std::uint32_t a_deadlineMs, bool a_rowBusy);

	// Interrogation lines (MGSV's interrogations, Elden Ring's people): the a_index-th, cycling.
	std::string_view GagLine(std::uint32_t a_index);
	std::size_t      GagCount();
}
