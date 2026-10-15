#pragma once

// Dive = hop (roadmap 2.9): when the kinematic puppet hops for MGSV's dive (pure; er/Kinematic.h KinematicHop does the hop).
//
// The guest raises kGuestDiving when Snake dives (X). The hop goes the way the dive moves him, so it waits for the dive's
// first real move (at least kMinSpeed) -- at most a_waitMs: an X that never moved Snake (MGSV refused it) is no hop.

#include "Math.h"

#include <cstdint>

namespace phantom::er
{
	class DiveTrigger
	{
	public:
		enum class Out : std::uint8_t
		{
			kNone,
			kHop,     // hop now along *a_dir
			kNoMove,  // a dive that didn't move Snake within the wait: no hop
		};
		static constexpr float kMinSpeed = 0.5f;  // m/s: slower is no dive's move (a turn in place, odometer noise)

		// Each kinematic frame: kGuestDiving; a_allowed: a hop may happen now (on, on foot, kinematic); now (ms); this frame's
		// step in Elden Ring (horizontal used) and its dt.
		Out Update(bool a_diving, bool a_allowed, std::uint64_t a_nowMs, const Vec3& a_step, float a_dt, int a_waitMs, Vec3* a_dir);
		void Reset() { *this = DiveTrigger(); }
		[[nodiscard]] bool Armed() const { return armed_; }

	private:
		bool          wasDiving_{ false };
		bool          armed_{ false };
		std::uint64_t armedMs_{ 0 };
	};
}
