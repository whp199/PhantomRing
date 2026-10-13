#pragma once
// Once per MGSV frame (the player camera update, on the game thread): read Snake, run the odometer and
// the hold-in-place, apply Elden Ring's hits to his life, detect shots, publish GuestState.

#include "Camera.h"

#include <string>

namespace phantom::guest::frame
{
	void OnPlayerCamera(const camera::Sample& a_sample);
	std::string Describe();  // one line for the '!status' console command

	// MGSV's Lua thread after every Tick: while the player-camera frames stall (MGSV's pause menu stops them) and the
	// sandbox is ready, republish the last GuestState with this moment's menu flag and keep the heartbeat going, so
	// Elden Ring sees "MGSV menu open" (and routes the pad to it) instead of a dead or loading MGSV.
	void PublishStalled(std::uint64_t a_nowMs, bool a_ready);
}
