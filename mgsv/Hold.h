#pragma once
// Moving Snake natively, without Lua and without the side effects of the Warp command (re-backlog M1):
// Player2SystemImpl::Warp (0x1409CEAE0) ends in loc->vf[0x68/8](loc, pos, rot), the character's own
// "set position + rotation". We resolve `loc` the same way Warp does and call just that, on the game
// thread, from the camera frame (after the player update, before the camera is published).
// UNVERIFIED at runtime: does locomotion/animation keep playing when held every frame? (GUEST.md M1)

#include "Logic.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace phantom::guest::hold
{
	// Verifies Warp's bytes and the Player2System accessor; false = the native modes fall back to Lua.
	bool Init();
	[[nodiscard]] bool Available();
	// Puts the player character at a_pos with rotation a_rot (xyzw). False if any link failed validation.
	bool Place(const logic::Vec3& a_pos, const std::array<float, 4>& a_rot);

	// The same manager's location object for another GameObject id (EXPERIMENT 2026-10-13, `!loc <id>`): 0 if none.
	std::uintptr_t LocationOf(std::uint16_t a_id);
	// Its set-position (vf 0x68), as for Snake: D-Horse answers no Lua SetPosition (live), the treadmill moves it this way.
	bool PlaceObject(std::uint16_t a_id, const logic::Vec3& a_pos, const std::array<float, 4>& a_rot);
	std::string DescribeLocation(std::uint16_t a_id);
	// The set-position of a location object found otherwise (ActorProxy v1: a realized soldier's), if it still is one (vtable
	// addr::kLocationVtable, setter inside the exe). a_pos is the point 0.8 m above the feet, as for Snake.
	bool PlaceLocation(std::uintptr_t a_loc, const logic::Vec3& a_pos, const std::array<float, 4>& a_rot);
	// Snake's own location object (0 when it can't be resolved): ActorProxy never takes it for a soldier's.
	std::uintptr_t PlayerLocation();
	// The position a location object holds (loc+0x20, xyz of a Vec4): what MGSV's own readers take from it (e.g. 0x14103FAF0
	// copies loc+0x20..0x2F after the same lookup; static analysis, UNVERIFIED live). ActorProxy reads its placements back.
	std::optional<logic::Vec3> LocationPosition(std::uint16_t a_id);
	// Console (`!placeobj`): placed on the game thread at the next camera frame (ServicePending from Frame.cpp).
	void        RequestPlaceObject(std::uint16_t a_id, const logic::Vec3& a_pos, float a_yaw);
	void        ServicePending();
	std::string LastPlaceResult();
}
