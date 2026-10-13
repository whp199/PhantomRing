#pragma once

// Frame-pacing measurements (console `pacing probe ...`): one row per Elden Ring frame with what each stage of the
// frame saw -- the guest state it read at FrameBegin (MGSV's frame number and QPC stamp, odometer, root, camera), the
// draw list it acquired, where the puppet went, the camera written at DrawParamUpdate (and how far collision pulled it
// in), the camera and draw list the render snapshot took at Draw_Pre, and when the render thread used that snapshot
// (G-buffer submission, Present, the view-constant check). Written as CSV next to the DLL for offline analysis
// (docs/HOST.md "Frame pacing"). Costs nothing while off.

#include "Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phantom::er
{
	struct Frame;
	struct RenderSnapshot;

	namespace PacingProbe
	{
		[[nodiscard]] bool Active();

		// Task thread (in frame order).
		void OnFrameBegin(const Frame& a_frame);  // right after the guest state read: the row
		void OnDrawAcquired(std::uint64_t a_erFrame, std::uint64_t a_drawFrame);
		void OnMoved(const Frame& a_frame, const Vec3& a_pe, const Vec3& a_step);
		void OnCamera(const Frame& a_frame, const Vec3& a_pe, const Vec3& a_camPos, const Vec3& a_camFwd, float a_rayLen, float a_pulledTo);
		void OnSnapshot(const RenderSnapshot& a_snap);
		// Frame pacing (er/Pacing.h): the step on MGSV's timeline this frame shows, the blend factor and mode; the same
		// for the draw lists at Draw_Pre.
		void OnPaced(std::uint64_t a_erFrame, double a_step, float a_alpha, std::uint32_t a_mode);
		void OnPacedDraws(std::uint64_t a_erFrame, double a_step, float a_alpha, std::uint32_t a_mode);

		// Render thread: a snapshot used for the G-buffer (kind 0), at Present (1); a view-constant check's error (2).
		void OnRender(int a_kind, std::uint64_t a_snapFrame, float a_value = 0.0f);

		std::string Command(const std::vector<std::string>& a_words);
	}
}
