#pragma once

// Frame pacing (docs/HOST.md "Frame pacing"; the math is er/PacingLogic.h): MGSV's motion shown evenly in Elden Ring's
// frames. Each Elden Ring frame picks one point on MGSV's step timeline, a little behind its newest frame, and blends
// the two MGSV frames around it: the puppet moves by the blended odometer and turns to the blended yaw
// (er/Puppet.cpp), the camera is the blended camera relative to Snake's root (er/CameraDriver.cpp), and Snake's and
// D-Walker's draw matrices are blended between the two draw lists around a point one draw-list latency further back
// (er/RenderScene.cpp; MGSV publishes a frame's draws about one frame after its state). [Camera] smooth and [Render]
// smooth_draws turn it off (the newest sample / list, as before).

#include "PacingLogic.h"
#include "phantomring_protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phantom::er
{
	struct Frame;

	namespace Pacing
	{
		struct Motion
		{
			bool          valid{ false };  // smoothing on and a blend (or hold) available this frame
			std::uint64_t frame{ 0 };      // the nearer MGSV frame
			std::uint32_t epoch{ 0 };
			double        odo[3]{};
			float         rootYaw{ 0 };
			Vec3          camRel;          // camera - root, MGSV world
			Quat          camQuat;
			float         fovY{ 0 };
		};

		void OnFrameBegin(const Frame& a_frame);  // after the guest state read: the sample, this frame's step
		void Resample(const Frame& a_frame);      // later in the frame (camera task): newer MGSV frames to blend toward
		[[nodiscard]] const Motion& Current();     // this Elden Ring frame's motion

		// Draw_Pre: this frame's draw list (records + matrices relative to Snake's root, DRAW CONTRACT), blended. False:
		// smoothing off or nothing to blend; the caller takes the newest list as before.
		bool Draws(const Frame& a_frame, std::vector<proto::DrawRecord>& a_records, std::vector<float>& a_matrices, std::uint64_t& a_frameOut);

		std::string Command(const std::vector<std::string>& a_words);
		std::string Describe();
	}
}
