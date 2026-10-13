#pragma once

// Debug lines in Elden Ring's world through RendMan's CSEzDraw (fromsoftware-rs examples/debug-line
// draws the same way from a task). Phase 1 uses it to confirm the axis conventions live:
//  - green: MGSV's skeleton (PoseBlock) mapped through the anchor; it must stand on the puppet's
//    feet, face where Snake faces and move his limbs the right way round;
//  - yellow: 1 m along the puppet's mapped facing;
//  - cyan: the buddy (D-Walker) estimate from the draw list, ground-snapped;
//  - red: shot traces (kGEvFire / kGEvBuddyFire) to their ER hit point.
// UNVERIFIED: that retail renders CSEzDraw output at all (if nothing shows, check `ez` in the dev
// console: it reports whether the drawer exists).

#include "Math.h"

namespace phantom::er
{
	struct Frame;

	namespace DebugDraw
	{
		void Draw(const Frame& a_frame);  // WorldChrMan_PostPhysics
		void AddTracer(const Vec3& a_from, const Vec3& a_to, bool a_hit, bool a_buddy);
		[[nodiscard]] bool Available();
	}
}
