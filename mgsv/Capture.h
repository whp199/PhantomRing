#pragma once
// Phase 2, MGSV side: publish exactly what MGSV draws for Snake (body, head, bionic arm, gear, weapons)
// and his buddy (D-Walker), every frame, per the DRAW CONTRACT in protocol/phantomring_protocol.h:
// meshes and textures once on the render ring (content-hash keys), and one DrawList per MGSV frame
// (mesh key, textures by role, index range, anchor-relative skinning matrices) in the draw slots.
// Elden Ring draws them at the puppet. The D3D11 observation is D3D11Hooks.*; this file decides.

#include <cstdint>
#include <string>
#include <string_view>

namespace phantom::guest::capture
{
	// DllMain, before the game creates its device ([Capture] Enabled).
	void InstallEarly();
	// Maintenance thread (~1 Hz): device/swap-chain fallbacks, periodic stats.
	void Poll();
	// "!capture ..." console lines (Guest.cpp).
	std::string Command(std::string_view a_args);

	// Game thread, once per player-camera frame (Frame.cpp): Snake's root this frame, the anchor his
	// draws are published relative to, and the camera MGSV renders this frame with.
	void NoteFrame(std::uint64_t a_frame, const float a_root[3], float a_yaw, const float a_camPos[3]);
	// Snake's deployed buddy (Frame.cpp): where to look for its draws.
	void NoteBuddy(bool a_present, const float a_pos[3]);

	// MGSV's real vertical field of view (rad), from the projection matrix its G-buffer draws use
	// (cVSScene.m_projection); 0 until one has been seen. Exact, unlike the focal-length estimate.
	float ProjectionFovY();
}
