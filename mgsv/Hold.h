#pragma once
// Moving Snake natively, without Lua and without the side effects of the Warp command (re-backlog M1):
// Player2SystemImpl::Warp (0x1409CEAE0) ends in loc->vf[0x68/8](loc, pos, rot), the character's own
// "set position + rotation". We resolve `loc` the same way Warp does and call just that, on the game
// thread, from the camera frame (after the player update, before the camera is published).
// UNVERIFIED at runtime: does locomotion/animation keep playing when held every frame? (GUEST.md M1)

#include "Logic.h"

#include <array>

namespace phantom::guest::hold
{
	// Verifies Warp's bytes and the Player2System accessor; false = the native modes fall back to Lua.
	bool Init();
	[[nodiscard]] bool Available();
	// Puts the player character at a_pos with rotation a_rot (xyzw). False if any link failed validation.
	bool Place(const logic::Vec3& a_pos, const std::array<float, 4>& a_rot);
}
