#pragma once

// Pure rules of the explosion visuals (er/Explosion.h): which Elden Ring effect a MGSV blast becomes, how
// many of them a blast's radius takes, how often a lingering cloud is refreshed, and how a BulletParam row
// becomes a harmless carrier of its own effect. No game access: tests/test_host_combat.cpp runs all of it,
// including on the real regulation rows.
//
// Why bullets: Elden Ring's pots are bullets whose burst is itself a short-lived bullet (Fire Pot 10030000
// hits -> 10030001: life 0.07 s, its flight effect 300122 *is* the fireball, and the effect outlives the
// bullet). Spawning such a burst row directly at a point plays the pot's own explosion there. Its attack and
// SpEffects are what hurt, so a copy without them (atk 0 and SpEffect 0, the way ER's own effect-only rows
// such as the Sleep Pot's mist 10064001 are written) is only the visual. The copy lives in a carrier row that
// nothing in the game spawns (cut content), so real pots never change.

#include "Math.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace phantom::er::explosion
{
	enum class Kind : std::uint8_t
	{
		kLethal,  // hand grenades, C4, rockets, grenade rounds: a fireball
		kStun,    // stun grenades: a flash
		kSleep,   // sleep gas: a lingering sleep mist
		kSmoke,   // smoke grenades, flares: a lingering cloud, no damage anywhere
		kFlame,   // D-Walker's flamethrower stream (flames flying along it; MGSV draws its own fire only in MGSV)
		kBreaker, // no look: a small object-only bullet that breaks Elden Ring's destructible assets it flies into (pots, crates)
		kShrapnel,// the same with the heavy object attack (atk_obj 100), flying out of a blast
		kMuzzle,  // a muzzle flash at a shot: a burst's effect cut short (MakeFlash)
	};
	inline constexpr std::size_t kKinds = 8;
	inline bool IsBreaker(Kind a_kind) { return a_kind == Kind::kBreaker || a_kind == Kind::kShrapnel; }

	std::string_view    Name(Kind a_kind);
	std::optional<Kind> FromAmmo(std::uint32_t a_ammo);      // proto::kAmmo* of a kGEvExplosion
	std::optional<Kind> FromName(std::string_view a_name);  // console: lethal|fire|stun|flash|sleep|smoke
	inline bool         IsCloud(Kind a_kind) { return a_kind == Kind::kSleep || a_kind == Kind::kSmoke; }

	// [Combat] explosion_fx
	enum class Mode : std::uint8_t
	{
		kAuto,    // bullet when it verifies, else off
		kBullet,  // ER pot bursts spawned as harmless bullets
		kSfx,     // a direct effect spawn: no verified call in this build, behaves as off
		kOff,
	};
	std::optional<Mode> ParseMode(std::string_view a_text);
	std::string_view    Name(Mode a_mode);

	// "<template bullet id> [sfx id]" (ini): the template row, and an effect to show instead of its own flight
	// effect (-1 = its own).
	struct Look
	{
		std::uint32_t templateId{ 0 };
		std::int32_t  sfx{ -1 };
	};
	std::optional<Look> ParseLook(std::string_view a_text);
	// "a, b, c, d[, e, f, g]" (ini): one carrier row per kind (the first ones; 4 = an ini from before the flame and breaker
	// kinds), all different.
	std::optional<std::vector<std::uint32_t>> ParseIds(std::string_view a_text);

	// How wide one burst of a template looks, from its hit radius (and the radius it grows to): the effect is
	// drawn about as big as the area it hits. Clamped to 1..6 m.
	float NativeRadius(float a_hitRadius, float a_hitRadiusMax);

	// Offsets (XZ plane) of the bursts that cover a blast of a_radius: one at the centre, and once the blast
	// is clearly bigger than one burst, a ring of up to a_maxBursts - 1 more, about one burst apart (pulled in
	// when there are too few to go round), rotated by a_phase (radians) so repeated clouds don't stack on the
	// same spots.
	std::vector<Vec3> Layout(float a_radius, float a_nativeRadius, int a_maxBursts, float a_phase);

	// MGSV reports a lingering cloud every ~0.5 s for as long as it lasts (sleep gas ~12 s, smoke up to a
	// minute). One burst of ER's mist lasts ~3 s, so a cloud is refreshed every a_repeatMs only: Allow() is
	// true for the first report near a spot and again once a_repeatMs have passed since the last allowed one.
	class CloudGate
	{
	public:
		bool Allow(Kind a_kind, const Vec3& a_pos, double a_nowMs, double a_repeatMs, float a_near = 2.5f);
		[[nodiscard]] std::size_t Size() const { return clouds_.size(); }
		void                      Clear() { clouds_.clear(); }
		// Elden Ring moved its world origin (er/WorldShift.h): the clouds' spots move with it.
		void Shift(const Vec3& a_delta)
		{
			for (auto& c : clouds_) {
				c.pos += a_delta;
			}
		}

	private:
		struct Cloud
		{
			Kind   kind;
			Vec3   pos;
			double lastMs;
			double repeatMs;
		};
		std::vector<Cloud> clouds_;
	};

	// ---- carrier rows (raw BulletParam bytes, params::kBullet.rowSize long) ---------------------------------
	// Does this row hurt or affect anything by itself? (atk row other than 0/-1, any SpEffect on hit or on the
	// shooter.)
	bool Harmful(const std::uint8_t* a_row);

	// The rows a template's burst goes on to spawn (hit_bullet_id / interval_create_bullet_id, recursively;
	// at most a_maxRows). a_row(id) returns a row's bytes or nullptr.
	struct Chain
	{
		std::vector<std::uint32_t> ids;
		bool                       harmless{ true };  // none of them is Harmful
		bool                       complete{ true };  // every child row exists and the walk ended within a_maxRows
	};
	Chain WalkChain(const std::uint8_t* a_template, const std::function<const std::uint8_t*(std::uint32_t)>& a_row, std::size_t a_maxRows = 32);

	// Turns a copy of a template row into its effect only: atk 0, no SpEffects (on hit or on the shooter), no
	// create-limit group (our own cap applies), the children kept only if a_keepChain (else none), and
	// a_sfx (> 0) as its flight effect.
	void MakeCarrier(std::uint8_t* a_row, std::int32_t a_sfx, bool a_keepChain);

	// The effect a row shows when spawned (its flight effect), -1 if none.
	std::int32_t FlightSfx(const std::uint8_t* a_row);

	// The breaker look: a copy of a bullet row (the flying Fire Pot's: Elden Ring breaks a pot or a crate with its attack, atk_obj
	// 10 and no damage, when the pot flies into it) that shows and sounds nothing (no flight, hit or flick effect, no sound ids),
	// spawns nothing, flies a_speed m/s for a_life seconds with a hit radius of a_radius, and carries AtkParam a_atk (the
	// object-only attack: er/Explosion.cpp patches it).
	void MakeBreaker(std::uint8_t* a_row, std::int32_t a_atk, float a_radius, float a_life, float a_speed = 0.0f);
	// The flame look: one flame per spawn (num_shoot 1, no shoot interval: a breath row is an emitter that keeps shooting as
	// long as it lives -- live, the Dragonfire copy went on shooting for minutes after one spawn), flying a_speed m/s
	// (0 = the row's own).
	void MakeSingleShot(std::uint8_t* a_row, float a_speed);
	// The muzzle-flash look: one bullet that stays where it is spawned (no speed), lives a_life seconds and takes its effect
	// with it when it dies (a pot's fireball cut after its first flash), so automatic fire never piles up lingering effects.
	void MakeFlash(std::uint8_t* a_row, float a_life);
	// A bullet row whose only attack is a_atk (and no SpEffects): an object-only carrier is not "harmful".
	bool OnlyAttack(const std::uint8_t* a_row, std::int32_t a_atk);

	// ---- the object-only attack (raw AtkParam bytes, params::kAtkParamPc.rowSize long) --------------------------------
	// No damage of any element, no stamina, poise or SpEffect, no flinch, no knock-back; atk_obj = a_objAttack; the target
	// bits (oppose / friendly / self) cleared unless a_hitsCharacters, so it can only touch map objects.
	void MakeObjectOnlyAttack(std::uint8_t* a_atkRow, std::uint16_t a_objAttack, bool a_hitsCharacters);
	// Is it one (nothing a character could feel)?
	bool ObjectOnly(const std::uint8_t* a_atkRow);
}
