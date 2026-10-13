#pragma once

// The per-frame anchor between MGSV's sandbox and Elden Ring (DESIGN.md §4):
//
//   ER_point  = Pe + Ry(delta) * M(MGSV_point - Pm)
//   MGSV_dir  = M(Ry(-delta) * ER_dir)
//
// Pe = the puppet's feet in ER (Havok space), Pm = Snake's root in MGSV world, delta = session-fixed
// yaw offset chosen when puppeting starts. M is an optional X mirror that absorbs a handedness
// difference between Fox and Havok (UNVERIFIED which one applies: confirmed live with the debug
// skeleton, then pinned in PhantomRingHost.ini [Axes]). Pure math, unit-tested.

#include "Math.h"

namespace phantom::er
{
	struct Axes
	{
		float mirrorX{ 1 };         // MGSV x is multiplied by this before mapping (+1 or -1)
		float mgsvYawSign{ 1 };     // guest rootYaw == mgsvYawSign * atan2(fwd.x, fwd.z) in MGSV world
		float erForward{ 1 };       // ER character local forward axis is (0, 0, erForward)
		float mgsvCamForward{ 1 };  // MGSV camera looks along R * (0, 0, mgsvCamForward)
	};

	struct Anchor
	{
		Vec3  pe;         // ER feet (Havok space)
		Vec3  pm;         // MGSV root (MGSV world)
		float delta{ 0 };  // rad
		Axes  axes;

		[[nodiscard]] Vec3 M(const Vec3& v) const { return { v.x * axes.mirrorX, v.y, v.z }; }
		[[nodiscard]] Vec3 DirToEr(const Vec3& v) const { return RotY(delta, M(v)); }
		[[nodiscard]] Vec3 PointToEr(const Vec3& p) const { return pe + DirToEr(p - pm); }
		[[nodiscard]] Vec3 DirToMgsv(const Vec3& v) const { return M(RotY(-delta, v)); }
		[[nodiscard]] Vec3 PointToMgsv(const Vec3& p) const { return pm + DirToMgsv(p - pe); }
	};

	// Snake's facing as a heading in the mirrored ("canonical") frame, so that ER heading = this + delta.
	inline float MgsvHeading(float a_rootYaw, const Axes& a_axes)
	{
		const float y = a_axes.mgsvYawSign * a_rootYaw;
		return Heading(Vec3{ std::sin(y) * a_axes.mirrorX, 0.0f, std::cos(y) });
	}

	// The ER character's facing (unit, horizontal part only matters) from its physics quaternion.
	inline Vec3 ErForward(const Quat& a_q, const Axes& a_axes) { return Rotate(a_q, { 0.0f, 0.0f, a_axes.erForward }); }

	// The yaw ChrIns::SetYaw needs for a wanted ER heading. SetYaw builds q = RotY(yaw) from the second of its Euler
	// angles (0x461F60: each times 0.5, about X, Y, Z; er/Game.cpp passes the yaw there), which maps +Z to Heading ==
	// yaw; if ER's forward is -Z the character must be turned by an extra pi.
	inline float ErYawForHeading(float a_heading, const Axes& a_axes)
	{
		return WrapPi(a_heading + (a_axes.erForward < 0 ? std::numbers::pi_v<float> : 0.0f));
	}

	// delta for a fresh session: makes the puppet keep the facing it has right now.
	inline float ChooseDelta(const Quat& a_erOrientation, float a_mgsvRootYaw, const Axes& a_axes)
	{
		return WrapPi(Heading(ErForward(a_erOrientation, a_axes)) - MgsvHeading(a_mgsvRootYaw, a_axes));
	}
}
