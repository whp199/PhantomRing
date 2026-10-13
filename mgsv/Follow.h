#pragma once
// D-Walker follows Snake on foot (docs/GUEST.md 18.3, with MGSV's buddy command table). MGSV's walker doesn't follow by itself (Normal Mode waits
// where it is); MGSV's own call menu command "Whistle (call D-Walker)" makes it come to where Snake is (walking,
// or placed nearer first when far). The guest issues that same command natively (the buddy system's command vf,
// command 20) whenever the walker is more than [Buddy] FollowDistance from its follow spot and Snake isn't riding it,
// so MGSV's AI does the walking.
//
// The follow spot is beside Snake ([Buddy] FollowAhead / FollowSide along the camera's view of him), not Snake
// himself: MGSV's whistle stores Snake's own position as the walker's destination, and parked there it stands in
// front of his muzzle (MGSV then refuses to fire: the walker blocks the barrel) or between him and the camera. A
// detour on the walker AI's message handler moves that stored destination to the spot right after the handler wrote
// it, for the whistles we issue.
//
// The whistle also puts D-Walker back into Normal Mode, and a mode change makes it announce the mode: auto-follow only
// acts while the walker is in a [Buddy] FollowModes mode (Normal by default), so a Stealth (wait), Search or Intercept
// Mode the player chose stays. [Buddy] FollowMethod = record (default: no whistle, the walker's destination record written
// directly: no message, no mode change, no voice), command (the whistle as is) or restore (the whistle, mode kept by the
// handler detour). Only for the buddy walker.

#include "Logic.h"

#include <cstdint>
#include <string>

namespace phantom::guest::follow
{
	// DllMain: the message handler detour (byte-checked; refused = whistles keep MGSV's destination, logged).
	void Install();

	// MGSV's Lua thread (Pump.cpp), a few times a second; a_ready = the sandbox is ready.
	void Update(std::uint64_t a_nowMs, bool a_ready);

	// The treadmill moved Snake and the walker by a_shift (MGSV world): move the walker's destination with them, and
	// whistle again soon so it is fresh.
	void OnShift(const logic::Vec3& a_shift);

	// Console: whistle once on the next Update (`!whistle`), or switch following (`!follow on|off`).
	void RequestWhistle();
	void SetEnabled(bool a_on);
	// Console: `!follow method command|restore|record` ([Buddy] FollowMethod; false = unknown name) and `!follow modes
	// <list>` ([Buddy] FollowModes: normal stealth search intercept mode1 mode2 | all).
	bool SetMethod(const std::string& a_name);
	void SetModes(const std::string& a_list);

	std::string Describe();
}
