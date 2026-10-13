#pragma once

// What the renderer draws at Present, captured on Elden Ring's task thread so one frame's pieces
// agree with each other: the camera Elden Ring renders from (CSCamera.pers_cam_1 as it stands at
// Draw_Pre (144), i.e. after CameraDriver wrote MGSV's camera at 107 and after anything Elden Ring
// itself did to it), the anchor (puppet feet + yaw delta, er/Anchor.h), and a private copy of the
// guest's DrawList (records + matrices; the shared slot may be recycled by the guest as soon as the
// next acquire): blended between MGSV's two lists around the moment this frame shows (er/Pacing.h),
// or the newest one with [Render] smooth_draws off.
//
// Snapshots go into a short history; Present picks the newest, or the one [Render] latency_frames
// older when Elden Ring presents a frame that was simulated earlier (learned live).

#include "Math.h"
#include "RenderMarkers.h"
#include "phantomring_protocol.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace phantom::er
{
	struct Frame;

	struct RenderCamera
	{
		Vec3  right, up, fwd, pos;  // ER world; fwd = view direction (sign of the +0x30 row applied)
		float fovY{ 0 };            // rad
		float aspect{ 0 };
		float nearZ{ 0 }, farZ{ 0 };
		bool  valid{ false };
	};

	struct RenderSnapshot
	{
		std::uint64_t                  erFrame{ 0 };
		std::uint64_t                  guestFrame{ 0 };
		RenderCamera                   camera;
		float                          anchor[12]{};  // anchor-relative MGSV world -> ER world, 3x4 rows (world = A * p)
		bool                           mirrored{ false };
		float                          hour{ -1 };     // Elden Ring's clock (0..24), -1 unknown
		bool                           draw{ false };  // something to draw this frame
		float                          dissolve{ 0 };  // death fade: 0 solid .. 1 gone ([Render] death_visual)
		bool                           riding{ false };  // Snake rides his buddy (kGuestOnVehicle): it never fades as an occluder
		// MGSV's first person / scope (kGuestFirstPerson): Snake's own draws are not faded near the camera -- they are his arms and
		// the gun's sights in front of it (session 4: "doesn't register hits when looking down scope" -- with the near fade the
		// iron sights vanished and MGSV draws no reticle there: nothing to aim with).
		bool                           firstPerson{ false };
		// The Tarnished's chest on screen (NDC, y up) while he is visible (no puppet): stage C learns the
		// light set Elden Ring gives its characters here from his G-buffer texels.
		bool                           tarnishedVisible{ false };
		float                          tarnishedNdc[2]{};
		std::vector<Marker>            markers;       // er/RenderMarkers.h: throw arc, grenades, mines (ER world)
		std::vector<proto::DrawRecord> draws;
		std::vector<float>             matrices;      // 12 floats each
		// The previous Elden Ring frame, for velocity (stage C): its camera and anchor, and each draw's
		// matrices of then at the same index as now (a draw without a match repeats its current ones,
		// prevValid 0). havePrev: the previous snapshot had a camera.
		bool                           havePrev{ false };
		RenderCamera                   prevCamera;
		float                          prevAnchor[12]{};
		std::vector<float>             prevMatrices;
		std::vector<std::uint8_t>      prevValid;     // per draw
	};

	namespace RenderScene
	{
		void Capture(const Frame& a_frame);  // task Draw_Pre (144)
		// The snapshot for the frame being presented (shared: stays valid while held).
		std::shared_ptr<const RenderSnapshot> ForPresent();
		// Cheap (no lock): the newest snapshot carries world markers (er/RenderMarkers.h). The renderer splits Elden
		// Ring's composite batch for them only when there are some.
		bool                                  HasMarkers();
		std::string                           Describe();
	}
}
