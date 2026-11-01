#pragma once
// D-Walker by its own location object (docs/GUEST.md 18.3 "Led alongside"; the pure half in WalkerLogic.h). Like D-Horse's
// (Horse.h), the walker's object is found by a scan of MGSV's memory -- here the one standing on its GetPosition -- and moved
// through the class's own setter (vf 0x68, what MGSV's SetPosition for the walker calls itself, 0x14170D410):
//   - the treadmill moves it by Snake's jump in the same frame (was: Lua SetPosition a frame or two later, a 6 m flash in Elden Ring);
//   - while Snake moves on foot it is led alongside him every frame (WalkerLogic.h), its AI still walking toward Follow.cpp's order.
// Game thread (the camera frame, Frame.cpp), as Hold's and Horse's placements.

#include "Logic.h"
#include "State.h"

#include <cstdint>
#include <optional>
#include <string>

namespace phantom::guest::walker
{
	struct FrameIn
	{
		bool          ready{ false };     // MGSV ready, Snake on foot (not riding), not staged / parked / anchored
		float         dt{ 0.0f };
		std::uint64_t nowMs{ 0 };
		logic::Vec3   root;               // Snake's root (MGSV world)
		logic::Vec3   velocity;           // the odometer's smoothed velocity
		logic::Vec3   camPos;             // MGSV's camera
		float         yaw{ 0.0f };        // Snake's facing (fallback for the view)
	};

	// Every camera frame (game thread), before the treadmill: finds and tracks the walker's object, leads it.
	void Update(const state::LuaView& a_lua, const FrameIn& a_in);
	// The treadmill / the Fulton anchor moved Snake by a_shift this frame: the walker too, natively. False = not moved (no
	// confirmed object, [Buddy] WalkerTreadmill off, the setter failed): the caller does what it did before (Lua SetPosition).
	bool Shift(const logic::Vec3& a_shift, const char* a_why);
	// The walker's feet from its object (fresh this frame) while it is confirmed; nothing otherwise (use GetPosition's).
	std::optional<logic::Vec3> Position();
	// Any thread: the lead placed it this frame or the last (Follow.cpp leaves it alone then: no catch-up warp).
	bool Leading();

	// Console: `!walker`, `!walker scan`, `!walker shift <dx> <dz>`, `!walker lead on|off`, `!walker lead <side> <ahead> <tau>
	// <maxSpeed> [turnRate] [acquire]`, `!walker treadmill on|off`.
	std::string Command(const std::string& a_args);
	std::string Describe();
}
