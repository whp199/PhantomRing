#pragma once

// Elden Ring's camera = MGSV's camera mapped through the anchor (DESIGN.md §7). Written into
// CSCamera.pers_cam_1 (the cam ApplyCameraMask fills for the renderer) from a task in
// DrawParamUpdate (107). UNVERIFIED (RE item E3) that the renderer reads pers_cam_1 after that
// group; the live test checks it, [Camera] task_group can move it (e.g. 144 Draw_Pre).
//
// Conventions: MGSV quat is xyzw, its view direction R*(0,0,mgsv_cam_forward) (Fox forward is +Z per
// MGS5VR's controller_rig.cpp, UNVERIFIED for the camera). ER's matrix rows are right, up, forward,
// position; the sign of the forward row and the row handedness are learned from ER's own camera
// (er/Calibration.cpp) unless pinned in the ini.
//
// Collision: MGSV's boom doesn't know ER walls, so a ray from the puppet's eye to the camera pulls
// the camera in; the distance goes out as HostState.cameraMaxDistance (measured from the eye).

namespace phantom::er
{
	struct Frame;

	namespace CameraDriver
	{
		void  Apply(const Frame& a_frame);
		// DmgMan_Post (122): MGSV's camera (as last written) into chr_cam again for Elden Ring's HUD compass. ER computes
		// the compass at MoveMap_PostPhysics_1_Core0 (123: MoveMapStep+0x378 -> 0xB00B40 -> 0xB01FC0 -> 0x7774A0) from
		// GameRend's first camera, which 0xB03AA0 copies from chr_cam just before; but ER refreshes chr_cam from its own
		// follow camera between MoveMap_PostPhysics_0 (118) and DmgMan_Pre (120), after our DrawParamUpdate write.
		// VERIFIED live (fake_mgsv orbiting at 0.3 rad/s): CSFeMan+0x1074 (the heading the compass meter shows) stayed 270
		// with chr_cam / GameRend / pers_cam_2 written at 107, at 10/16/18/20 or at 113/118; it followed the orbit
		// (73.8 -> 80.4 -> 86.7 deg in 0.3 s steps) with chr_cam written at 120 or 122.
		void  ApplyCompass();
		float MaxDistance();  // 0 = no clamp
		[[nodiscard]] bool Driving();
	}
}
