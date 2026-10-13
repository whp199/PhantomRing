#pragma once
// The player camera, the player root and (through them) the skeleton target, once per game frame.
//
// MGSV's camera setter 0x14044E960 is a leaf called for every camera; the player camera update calls
// it from 0x141118B06 with RSI = the camera owner (research §7.3, MGS5VR camera_observer.cpp). A small
// assembly stub hands us that RSI. On the player's call we snapshot the camera and root and run the
// guest's per-frame work (Guest.cpp) right there, on the game thread.

#include <cstdint>

namespace phantom::guest::camera
{
	struct Sample
	{
		std::uintptr_t owner{ 0 };
		std::uintptr_t cameraObject{ 0 };
		float          quat[4]{};      // camera orientation, xyzw, MGSV world
		float          pos[3]{};       // camera position, MGSV world
		float          focalMm{ 0 };   // camera+0xC: last frame's lerped focal length (UNVERIFIED unit)
		float          root[16]{};     // owner+0x30: player root, row-major, translation [12..14]
	};

	using FrameFn = void (*)(const Sample& a_sample);

	// Verifies the setter, the callsite and the owner layout bytes; creates (not enables) the hook.
	bool Install(FrameFn a_onPlayerFrame);
}
