#include "CqcLogic.h"

#include <algorithm>
#include <cmath>

namespace phantom::guest::cqc
{
	Actions Tracker::Update(const Input& a_in, const Rules& a_r)
	{
		Actions    out;
		const auto down = [&](std::uint32_t a_buttons) { return (a_buttons & a_r.cqcMask) != 0 && (a_buttons & a_r.aimMask) == 0; };
		const bool cqcDown = down(a_in.buttons);
		// Edges only against a frame we saw: a button already held when the guest started is no press.
		const bool pressed = primed_ && cqcDown && !down(lastButtons_);
		const auto edge = [&](std::uint32_t a_mask) { return primed_ && a_mask != 0 && (a_in.buttons & a_mask) != 0 && (lastButtons_ & a_mask) == 0; };
		const auto now = a_in.nowMs;
		const auto grab = [&] {
			out.Add(proto::kCqcGrab);
			state_ = State::kHolding;
			holdAt_ = now;
			choked_ = interrogated_ = false;
		};
		const auto cool = [&] {
			state_ = State::kCooldown;
			until_ = now + a_r.cooldownMs;
		};

		switch (state_) {
		case State::kIdle:
			if (!a_in.allowed) {
				break;
			}
			if (pressed) {
				if (a_in.stick >= a_r.throwStick) {
					// MGSV: CQC while moving toward a soldier throws him (no hold first).
					out.Add(proto::kCqcThrow);
					cool();
				} else {
					state_ = State::kPressed;
					pressAt_ = now;
				}
			} else if (a_in.mgsvHold) {
				grab();  // MGSV holds someone (an ActorProxy soldier) without a press we saw
			}
			break;
		case State::kPressed:
			if (!a_in.allowed || (!cqcDown && !a_in.mgsvHold)) {
				state_ = State::kIdle;  // let go before holdMs: it was a strike (kGEvMelee covers it)
			} else if (a_in.mgsvHold || now - pressAt_ >= a_r.holdMs) {
				grab();
			}
			break;
		case State::kHolding:
			if (!a_in.allowed || (!cqcDown && !a_in.mgsvHold) || now - holdAt_ >= a_r.maxHoldMs) {
				out.Add(proto::kCqcRelease);
				cool();
				break;
			}
			if (!interrogated_ && edge(a_r.interrogateMask)) {
				out.Add(proto::kCqcInterrogate);
				interrogated_ = true;
			}
			if (!choked_ && (edge(a_r.chokeMask) || (a_r.chokeMs != 0 && now - holdAt_ >= a_r.chokeMs))) {
				out.Add(proto::kCqcChoke);
				choked_ = true;
			}
			break;
		case State::kCooldown:
			if (!cqcDown && now >= until_) {
				state_ = State::kIdle;
			}
			break;
		}
		lastButtons_ = a_in.buttons;
		primed_ = true;
		return out;
	}

	const char* Name(proto::CqcAction a_action)
	{
		switch (a_action) {
		case proto::kCqcGrab:
			return "grab";
		case proto::kCqcHold:
			return "hold";
		case proto::kCqcThrow:
			return "throw";
		case proto::kCqcChoke:
			return "choke";
		case proto::kCqcInterrogate:
			return "interrogate";
		case proto::kCqcRelease:
			return "release";
		}
		return "?";
	}

	const char* Name(Tracker::State a_state)
	{
		switch (a_state) {
		case Tracker::State::kIdle:
			return "idle";
		case Tracker::State::kPressed:
			return "pressed";
		case Tracker::State::kHolding:
			return "holding";
		case Tracker::State::kCooldown:
			return "cooldown";
		}
		return "?";
	}

	float StickDeflection(std::int16_t a_x, std::int16_t a_y)
	{
		constexpr float kDead = 7849.0f;  // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
		const float     m = std::hypot(static_cast<float>(a_x), static_cast<float>(a_y));
		if (m <= kDead) {
			return 0.0f;
		}
		return std::clamp((m - kDead) / (32767.0f - kDead), 0.0f, 1.0f);
	}
}
