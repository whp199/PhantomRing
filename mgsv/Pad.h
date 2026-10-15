#pragma once
// MGSV's controller: its XInputGetState import (IAT slot 0x14208E070, MGS5VR gamepad_hook.cpp method)
// returns the player's pad from the shared PadState for user 0 and "not connected" for 1..3, so MGSV
// plays with exactly the controller Elden Ring sees and nothing else. Rumble goes back as kGEvRumble.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

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

	// The player's own presses of a_button (one XINPUT_GAMEPAD_* bit) so far: rising edges of the host's pad as MGSV polled it,
	// never Pulse/Sequence buttons (Death.cpp's own dive is no player's dive). Dive.h counts X.
	[[nodiscard]] std::uint32_t Presses(std::uint16_t a_button);
	// Snake's [Jump] button (protocol v14; logic::JumpButton): its presses since the last call (MGSV never saw them), for the
	// game thread (Frame.cpp). RequestJump: one as if pressed (`!jump`).
	[[nodiscard]] bool TakeJumpPress();
	void               RequestJump();
	// The game thread, every frame: R3 is MGSV's own now (Snake aims by MGSV's HOLD, rides, is in a menu or the binoculars):
	// the pad thread passes it to MGSV instead of taking it for a jump (logic::MgsvOwnsR3).
	void               SetMgsvOwnsR3(bool a_owns);

	[[nodiscard]] std::uint64_t Polls();
	// `!pad` / `!menu`: the iDroid requests ([Menu] IDroidRetryMs) so far.
	std::string DescribeIDroid();
}
