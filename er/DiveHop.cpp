#include "DiveHop.h"

#include <algorithm>

namespace phantom::er
{
	DiveTrigger::Out DiveTrigger::Update(bool a_diving, bool a_allowed, std::uint64_t a_nowMs, const Vec3& a_step, float a_dt, int a_waitMs, Vec3* a_dir)
	{
		// The rising edge arms it: a dive is one hop however long MGSV keeps the flag up.
		if (a_diving && !wasDiving_ && a_allowed) {
			armed_ = true;
			armedMs_ = a_nowMs;
		}
		wasDiving_ = a_diving;
		if (!armed_) {
			return Out::kNone;
		}
		if (!a_allowed) {
			armed_ = false;  // mounted, switched off, an interaction: this dive is no hop
			return Out::kNone;
		}
		const Vec3  flat{ a_step.x, 0.0f, a_step.z };
		const float len = LengthXZ(flat);
		if (len >= kMinSpeed * std::max(a_dt, 1.0f / 1000.0f)) {
			armed_ = false;
			if (a_dir) {
				*a_dir = flat * (1.0f / len);
			}
			return Out::kHop;
		}
		if (a_nowMs - armedMs_ > static_cast<std::uint64_t>(std::max(a_waitMs, 0))) {
			armed_ = false;
			return Out::kNoMove;
		}
		return Out::kNone;
	}
}
