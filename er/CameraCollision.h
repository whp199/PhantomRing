#pragma once

// Pulling MGSV's camera in front of Elden Ring's walls (er/CameraDriver.cpp), without the pops: measured live
// (2026-10-12, panning next to a tree and a wall) the eye-to-camera distance jumped by up to 5 m in one frame on 56 of
// 900 frames -- the camera snapped into Snake's head as a trunk passed the line and back out the next frame. Now the
// camera still comes in at once (a wall must never show through) but goes back out eased (er/PhantomRingHost.ini
// [Camera] collision_*). Pure; tests/test_host_pacing.inc.

#include <limits>

namespace phantom::er::camcol
{
	struct Params
	{
		float margin{ 0.25f };       // kept between the camera and what the ray hit (m)
		float minDistance{ 0.1f };   // never closer to the pivot than this (m)
		float easeOutSeconds{ 0.2f };  // time constant going back out once the way is clear
		float maxOutSpeed{ 8.0f };   // m/s at most going back out
	};

	class Smoother
	{
	public:
		// a_hit: distance from the pivot to the nearest hit along the camera's way (infinity: nothing); a_wanted: where MGSV
		// puts the camera. Returns how far from the pivot the camera may be this frame (>= a_wanted: not pulled in).
		float Update(float a_hit, float a_wanted, float a_dt, const Params& a_p);
		void  Reset() { allowed_ = kFree; }
		[[nodiscard]] bool Pulling() const { return allowed_ < kFree; }

	private:
		static constexpr float kFree = std::numeric_limits<float>::infinity();
		float                  allowed_{ kFree };
	};
}
