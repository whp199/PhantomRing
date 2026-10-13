#pragma once
// MGSV's controller: its XInputGetState import (IAT slot 0x14208E070, MGS5VR gamepad_hook.cpp method)
// returns the player's pad from the shared PadState for user 0 and "not connected" for 1..3, so MGSV
// plays with exactly the controller Elden Ring sees and nothing else. Rumble goes back as kGEvRumble.

#include <cstddef>
#include <cstdint>
#include <span>

namespace phantom::guest::pad
{
	// Patches the IAT slot after checking it is the XINPUT1_3 import and points into an xinput module.
	bool Install();

	enum class Source
	{
		kNeutral,  // centred sticks, nothing pressed (boot, Elden Ring menus, host gone)
		kHost,     // the player's pad from PadState
	};
	// Whether the player's pad reaches MGSV right now (Guest.cpp decides from boot state and host flags).
	void SetSource(Source a_source);

	// Snake's death (Death.cpp): the player's pad is cut off (neutral) whatever the source; pulses still go through.
	void SetBlocked(bool a_blocked);
	// Tarnished mode (protocol v10 kHEvTarnished): the same, for as long as the player plays the Tarnished.
	void SetParked(bool a_parked);

	// Boot automation: hold these XInput buttons for a_ms, on top of whatever the source gives.
	void Pulse(std::uint16_t a_buttons, unsigned a_ms);

	// Boot automation: a short button sequence (each step's buttons and right trigger held for its ms, then the next), on
	// top of whatever the source gives -- e.g. MGSV's item menu at the sandbox setup (hold D-pad left, X to change the box's
	// poster: docs/GUEST.md 18.9). False when one is already playing or it is too long.
	struct Step
	{
		std::uint16_t buttons{ 0 };
		std::uint8_t  rightTrigger{ 0 };
		unsigned      ms{ 0 };
	};
	inline constexpr std::size_t kMaxSequence = 40;
	bool Sequence(std::span<const Step> a_steps);
	[[nodiscard]] bool SequenceRunning();

	[[nodiscard]] std::uint64_t Polls();
}
