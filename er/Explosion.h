#pragma once

// Explosion visuals: Elden Ring shows only Snake's and D-Walker's meshes, never MGSV's effects, so a blast
// that Snake causes (a rocket's impact, a thrown grenade, sleep gas, smoke) is drawn with Elden Ring's own
// pot effects at the Elden Ring point: a Fire Pot fireball for lethal blasts, the Holy Water Pot's flash for
// stun grenades, the Sleep Pot's mist for sleep gas (renewed while MGSV's cloud lasts), the Fetid Pot's cloud
// for smoke. A big blast gets a centre burst and a ring around it (er/ExplosionLogic.h). D-Walker's flamethrower
// gets the Dragonfire incantation's flames along its stream.
//
// How: [Combat] explosion_fx = bullet (auto picks it). Each look's BulletParam burst row is copied into a
// carrier row that nothing in the game uses (cut content: the Roped Freezing Pot's and Miranda's Prayer's
// bullets), without its attack and SpEffects, and that carrier is spawned with CSBulletManager::SpawnBullet
// (the address the combat route already uses live). Damage stays where it was (er/Combat.cpp Blast()): these
// bullets carry no damage and no SpEffect, like ER's own effect-only rows, so they cannot hurt or affect anyone,
// the player included. The carrier bytes go back once the puppet is off and the last burst has played out.
//
// Destructible assets: Elden Ring breaks a pot or a crate when its flying Fire Pot (Bullet 10030000) flies into it -- that
// bullet's own attack is object-only (no damage, atk_obj 10). The breaker and shrapnel looks are copies of it with no effect
// and no sound, carrying an object-only attack of ours (the cut Roped Freezing Pot's AtkParam_Pc rows patched to no damage of
// any kind and no flinch): light (atk_obj 10: pots, crates, bushes) through the point where Snake's round hit the world, his
// CQC strike met no one or (kinematic motion) his body bumped into something (Break()); heavy (atk_obj 100) flying out of every
// lethal blast (Shrapnel()). Live: carts and crates (defence 0) break by shot, strike and blast; a defence-90 fence did not,
// even at atk_obj 999 (docs/HOST.md "Breakables").
//
// Elden Ring's bullet pool is shared with everything in the world (one 128 + 128 list): our bullets alive at once are
// counted in its live list by carrier id and capped ([Combat] explosion_max_live); a lingering cloud is refreshed only
// once its last bursts are gone. (Session 4: a minute of smoke at 7 bursts every 2.5 s ran the pool dry --
// "SpawnBullet(10302001) refused (error 4)" -- and every later hit bullet with it.)

#include "ExplosionLogic.h"
#include "Host.h"

#include <string>
#include <string_view>
#include <vector>

namespace phantom::er::Explosion
{
	// A blast of a_kind and a_radius (m) at a_centre (Elden Ring world). a_onGround: the point came from MGSV's
	// world (the anchor) and may float or sink: drop it onto Elden Ring's ground (explosive rounds' impact points
	// are already on a surface; their ring bursts are dropped either way).
	void Show(const Frame& a_frame, explosion::Kind a_kind, const Vec3& a_centre, float a_radius, bool a_onGround, std::string_view a_what);

	// D-Walker's flamethrower: one flame flying from a_from along a_dir (rate-limited, [Combat] flame_per_second).
	void Flame(const Frame& a_frame, const Vec3& a_from, const Vec3& a_dir);

	// The object-only attack at a_at (a shot's impact, a strike that hit no one, a bump): breaks a destructible asset
	// there, nothing else. a_dir: where it came from (the sphere is put a little back along it, on the surface).
	void Break(const Frame& a_frame, const Vec3& a_at, const Vec3& a_dir, std::string_view a_what);
	// A lethal blast: heavy breakers flying out of a_centre to a_radius (everything a pot's blast would break around it).
	void Shrapnel(const Frame& a_frame, const Vec3& a_centre, float a_radius, std::string_view a_what);

	void Tick(const Frame& a_frame);  // FrameBegin (from Combat::Tick): carrier rows back once they're no longer needed
	void OnPuppetStop();
	void OnWorldShift(const Vec3& a_delta);  // er/WorldShift.h: the clouds' spots move with Elden Ring's origin

	// Dev console: `boom [status] | boom <lethal|stun|sleep|smoke|flame|breaker> [radius] | boom pool | boom mode
	// auto|bullet|sfx|off|ini | boom restore`.
	std::string Console(const std::vector<std::string>& a_words, const Frame& a_frame);
	std::string Describe();
}
