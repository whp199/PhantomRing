#include "CameraCollision.h"

#include <algorithm>
#include <cmath>

namespace phantom::er::camcol
{
	float Smoother::Update(float a_hit, float a_wanted, float a_dt, const Params& a_p)
	{
		if (!(a_wanted > 0.0f) || !std::isfinite(a_wanted)) {
			allowed_ = kFree;
			return a_wanted;
		}
		// Where the camera may be by this frame's rays: in front of the hit, else just past MGSV's own distance.
		const float target = std::isfinite(a_hit) ? std::max(a_p.minDistance, a_hit - a_p.margin) : a_wanted + 0.5f;
		if (allowed_ == kFree) {
			if (target < a_wanted) {
				allowed_ = target;  // in at once
			}
		} else if (target < allowed_) {
			allowed_ = target;
		} else {
			const float dt = std::clamp(a_dt, 0.0f, 0.1f);
			const float k = a_p.easeOutSeconds > 0.0f ? 1.0f - std::exp(-dt / a_p.easeOutSeconds) : 1.0f;
			allowed_ += std::min((target - allowed_) * k, a_p.maxOutSpeed * dt);
			if (allowed_ >= a_wanted && target >= a_wanted) {
				allowed_ = kFree;  // back where MGSV wants it: let go
			}
		}
		return allowed_;
	}
}
