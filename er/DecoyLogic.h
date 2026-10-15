#pragma once

// Pure rules for Snake's decoys in Elden Ring (er/Decoys.h, roadmap 2.8): which MGSV decoy it is, whether an Elden Ring
// enemy notices one, when one pops, how it inflates / deflates and the figure it is drawn as. No game access: unit-tested
// in tests/test_host_decoy.inc.

#include "Math.h"

#include <cstdint>
#include <vector>

namespace phantom::er::decoy
{
	// MGSV's three decoys (TppEquip ids on 1.0.15.4, tools/re/data/tppequip_ids_1.0.15.4.txt).
	enum class Variant : std::uint8_t
	{
		kPlain,   // EQP_SWP_Decoy 397-399: an inflatable soldier, seen
		kActive,  // EQP_SWP_ActiveDecoy 401-403: it also calls out, heard without a line of sight
		kShock,   // EQP_SWP_ShockDecoy 405-407, 380-381: who walks into it gets a shock
	};
	Variant     VariantOf(std::uint16_t a_equip);
	const char* VariantName(Variant a_variant);

	struct Rules
	{
		float    range{ 15.0f };        // m: seen from this far (in front of the enemy, a clear line)
		float    activeRange{ 25.0f };  // m: an active decoy is heard from this far, anywhere around
		float    coneDeg{ 70.0f };      // half angle of an enemy's sight
		float    popDistance{ 1.0f };   // m from its body: an enemy this close knocks it over
		float    snakeClose{ 4.0f };    // m: an enemy nearer to Snake than this (and than to the decoy) is busy with him
		std::uint64_t inflateMs{ 800 };  // it blows up after being set; nobody notices it before
		std::uint64_t popMs{ 500 };      // it collapses when popped
	};

	enum class Notice : std::uint8_t
	{
		kNo,
		kSees,   // in front of the enemy within range: the caller still casts the sight line
		kHears,  // an active decoy within activeRange: no sight line needed
	};
	// a_eye: the enemy's eyes; a_forward: its facing (only the horizontal part counts); a_decoy: the decoy's chest;
	// a_snakeDistance: the enemy's distance to Snake (m, a large number when unknown).
	Notice Notices(const Vec3& a_eye, const Vec3& a_forward, const Vec3& a_decoy, Variant a_variant, float a_snakeDistance, const Rules& a_rules);

	// An enemy a_contact m from the decoy's body knocks it over.
	inline bool PopsOn(float a_contact, const Rules& a_rules) { return a_contact <= a_rules.popDistance; }

	// Its size share now: inflating from 0.15 to 1 over inflateMs after it was set, then 1; a_poppedAgeMs (>= 0 once popped)
	// collapses it to 0 over popMs. a_ageMs: since it was set.
	float Scale(std::uint64_t a_ageMs, std::int64_t a_poppedAgeMs, const Rules& a_rules);
	// Up (inflated) and noticeable: an enemy only stares at a standing decoy.
	inline bool Standing(std::uint64_t a_ageMs, std::int64_t a_poppedAgeMs, const Rules& a_rules)
	{
		return a_poppedAgeMs < 0 && a_ageMs >= a_rules.inflateMs;
	}

	// The figure it is drawn as: lit balls for legs, hips, chest, shoulders and head of a soldier a_height m tall standing
	// at a_feet facing a_facing (horizontal), squashed toward the ground by a_scale (0..1).
	struct Ball
	{
		Vec3  pos;
		float radius{ 0 };
	};
	std::vector<Ball> Figure(const Vec3& a_feet, const Vec3& a_facing, float a_height, float a_scale);
	// The point enemies look at and walk into: its chest.
	inline Vec3 Chest(const Vec3& a_feet, float a_height) { return a_feet + Vec3{ 0, a_height * 0.65f, 0 }; }
}
