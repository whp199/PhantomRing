#pragma once

// Elden Ring moves its world origin in the open world (task groups BeginShiftWorldPosition 160 at the end of a frame,
// EndShiftWorldPosition 11 early in the next; CSWorldShiftFragmentProxy classes). Almost nothing PhantomRing keeps
// is an absolute position across frames (the anchor is re-read every frame), but thrown grenades and placed mines
// are: they must move with the world. The player's physics position is read at 160 and again at 11; nothing else
// moves him in between, so a difference there is a shift (not during loads).

#include "Math.h"

#include <optional>
#include <string>

namespace phantom::er::WorldShift
{
	void Start();  // registers the two tasks
	// FrameBegin (0), before the events and the grenade/mine ticks: FrameBegin runs between 160 and 11, so a shift Elden
	// Ring made after 160 must move our copies before they fly or scan this frame; 11 then applies only what is left.
	void Check();
	// ChrIns_PrePhysics (78), before the puppet moves: live, every shift (32 / 40 m, multiples of 8 m on x/z while riding
	// through Limgrave) happened after our task in group 11 -- 160, 0 and 11 all saw nothing, the puppet's next move saw
	// "Elden Ring moved the player 32 m by itself". Nothing moves the player between 160 and here but a shift.
	void CheckBeforePhysics();
	std::string Describe();

	// Elden Ring shifts its origin on an 8 m grid (live: 32.00 / 32.98 / 35.78 / 40.00 m long, every axis a multiple of 8 m, y
	// too: (48, -8, -56), (-32, -104, 16)).
	inline constexpr float kShiftGrid = 8.0f;
	// What the player himself may have moved in the frame of the shift (live: 0.03-0.09 m; riding through a 50 ms hitch
	// ~0.5 m): more off the grid on any axis is no shift.
	inline constexpr float kShiftSlack = 0.75f;

	// Pure: the player's position before -> after with nothing else moving him is a shift of the world origin when the move
	// is a whole number of grid steps (not all zero) on every axis, give or take his own motion of that frame, and at most
	// 20 km (not garbage). The shift is the grid's: his own motion never leaks into the mines, grenades and the puppet that
	// follow it, and a console `call setpos` or any other off-grid move is no shift (the puppet's resync handles that).
	inline std::optional<Vec3> ShiftOf(const Vec3& a_before, const Vec3& a_after)
	{
		const Vec3 d = a_after - a_before;
		if (!Finite(d) || Length(d) > 20000.0f) {
			return std::nullopt;
		}
		auto       snap = [](float a_v) { return std::round(a_v / kShiftGrid) * kShiftGrid; };
		const Vec3 s{ snap(d.x), snap(d.y), snap(d.z) };
		if ((s.x == 0.0f && s.y == 0.0f && s.z == 0.0f) || std::fabs(d.x - s.x) > kShiftSlack || std::fabs(d.y - s.y) > kShiftSlack ||
			std::fabs(d.z - s.z) > kShiftSlack) {
			return std::nullopt;
		}
		return s;
	}
}
