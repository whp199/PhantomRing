#pragma once
// Projectile meshes (roadmap 5.4, protocol v13 kOwnerProjectile), MGSV side: the grenade Snake throws and the rocket he
// fires are MGSV meshes drawn near him for a moment after the release. Capture.cpp asks Tag() for each rigid G-buffer
// draw; a draw where a fresh projectile must be (mgsv/ProjectileLogic.h) is published as kOwnerProjectile instead of being
// Snake's (within 2.5 m) or dropped (further out), so Elden Ring gets the projectile's own mesh and draws it at the copy
// it flies (er/Projectiles.h). [Capture] ProjectileMeshes = false: nothing is tagged (the draws stay as they were).

#include <cstdint>
#include <string>

namespace phantom::guest::projectile
{
	// Game thread: a thrown explosive left Snake's hand (Throw.cpp, with its kGEvThrow) / an explosive round left his gun
	// (Combat.cpp, with its kGEvFire). Positions MGSV world; a_vel m/s, a_dir unit.
	void NoteThrow(std::uint64_t a_frame, const float a_p0[3], const float a_vel[3]);
	void NoteRound(std::uint64_t a_frame, const float a_muzzle[3], const float a_dir[3]);

	// Recording threads (Capture.cpp): is a rigid draw at a_at (MGSV world) in game frame a_frame a projectile in flight?
	bool Tag(std::uint64_t a_frame, const float a_at[3]);

	std::string Describe();
}
