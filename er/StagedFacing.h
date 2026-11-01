#pragma once

// Which way Snake's body is drawn while MGSV plays a staged move (protocol v15 kGuestClimbing / kGuestMantling, docs/GUEST.md 22).
// The guest has put Snake at one of the airport's real ladders or obstacles, facing it, and his draw list is relative to his root
// there: drawn with the puppet's delta (er/Anchor.h: home facing -> Elden Ring heading) he would face (that obstacle's yaw + delta),
// an arbitrary way in Elden Ring (review 2026-10-31: a climb-up drawn sideways or backwards; after a ladder the puppet's delta taken
// from the ladder's yaw). So while staged he is drawn facing a heading of the host's own -- a climb-up / vault's direction, eased
// in over kEaseS from the way he faced, or the Tarnished's facing after a ladder -- and eased back to his own heading (MGSV facing
// + delta) once MGSV has him home. The camera and the motion keep the delta: the guest publishes the camera he had at home.
//
// Pure (tests/test_host_mantle.inc TestStagedFacing); er/Puppet.cpp drives it, er/RenderScene.cpp draws with it
// (Puppet::StagedDrawAnchor).

#include "Math.h"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace phantom::er
{
	class StagedFacing
	{
	public:
		static constexpr float kEaseS = 0.15f;

		// a_from: the heading he is drawn at now; a_to: the one to draw him at while staged (Elden Ring headings, rad).
		void Begin(float a_from, float a_to, std::uint64_t a_nowMs)
		{
			on_ = true;
			from_ = a_from;
			to_ = a_to;
			beginMs_ = a_nowMs;
			endMs_ = 0;
		}
		// MGSV has him home: ease back to his own heading.
		void End(std::uint64_t a_nowMs)
		{
			if (on_ && !endMs_) {
				endMs_ = a_nowMs ? a_nowMs : 1;
			}
		}
		void Reset() { *this = StagedFacing(); }
		[[nodiscard]] bool  On() const { return on_; }
		[[nodiscard]] bool  Ending() const { return on_ && endMs_ != 0; }
		[[nodiscard]] float Target() const { return to_; }

		// The Elden Ring heading to draw him at now. a_own: his own heading (MGSV facing + delta), which the way back eases to.
		// nullopt: not staged, or the way back is over (the caller resets it).
		[[nodiscard]] std::optional<float> Heading(std::uint64_t a_nowMs, float a_own) const
		{
			if (!on_) {
				return std::nullopt;
			}
			const float h = WrapPi(from_ + WrapPi(to_ - from_) * Ease(a_nowMs, beginMs_));
			if (!endMs_) {
				return h;
			}
			const float out = Ease(a_nowMs, endMs_);
			if (out >= 1.0f) {
				return std::nullopt;
			}
			return WrapPi(h + WrapPi(a_own - h) * out);
		}

	private:
		static float Ease(std::uint64_t a_nowMs, std::uint64_t a_sinceMs)
		{
			const float t = a_nowMs > a_sinceMs ? static_cast<float>(a_nowMs - a_sinceMs) / (1000.0f * kEaseS) : 0.0f;
			const float c = std::clamp(t, 0.0f, 1.0f);
			return c * c * (3.0f - 2.0f * c);
		}

		bool          on_{ false };
		float         from_{ 0 };
		float         to_{ 0 };
		std::uint64_t beginMs_{ 0 };
		std::uint64_t endMs_{ 0 };
	};
}
