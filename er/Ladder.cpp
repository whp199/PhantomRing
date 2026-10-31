#include "Ladder.h"

#include <algorithm>
#include <cmath>

namespace phantom::er
{
	LadderHandOver::Event LadderHandOver::Update(const Input& a_in)
	{
		if (!a_in.inWorld) {
			if (state_ != State::kIdle) {
				*this = LadderHandOver();
				return Event::kAborted;
			}
			return Event::kNone;
		}
		switch (state_) {
		case State::kIdle:
			// Snake's press on a ladder prompt, or a ladder state while Snake plays (a climb started by a press we didn't see:
			// Elden Ring's keyboard): either way the Tarnished must have the controller for it.
			if (a_in.snake && (a_in.pressed || a_in.onLadder)) {
				state_ = a_in.onLadder ? State::kClimbing : State::kWaiting;
				since_ = a_in.nowMs;
				return Event::kStarted;
			}
			return Event::kNone;
		case State::kWaiting:
			if (a_in.onLadder) {
				state_ = State::kClimbing;
				return Event::kClimbing;
			}
			if (a_in.nowMs - since_ >= a_in.waitMs) {
				*this = LadderHandOver();
				return Event::kNoClimb;
			}
			return Event::kNone;
		case State::kClimbing:
			if (!a_in.onLadder) {
				state_ = State::kSettling;
				offSince_ = a_in.nowMs;
			}
			return Event::kNone;
		case State::kSettling:
			if (a_in.onLadder) {  // a frame of "none" between two of its states: still on it
				state_ = State::kClimbing;
				return Event::kNone;
			}
			if (a_in.nowMs - offSince_ >= a_in.settleMs) {
				*this = LadderHandOver();
				return Event::kEnded;
			}
			return Event::kNone;
		}
		return Event::kNone;
	}

	bool LadderHandOver::PressDue(std::uint64_t a_nowMs, std::uint64_t a_delayMs)
	{
		if (state_ != State::kWaiting || pressed_ || a_nowMs - since_ < a_delayMs) {
			return false;
		}
		pressed_ = true;
		return true;
	}

	std::string_view Name(LadderHandOver::State a_state)
	{
		switch (a_state) {
		case LadderHandOver::State::kIdle: return "idle";
		case LadderHandOver::State::kWaiting: return "waiting for the climb";
		case LadderHandOver::State::kClimbing: return "climbing";
		case LadderHandOver::State::kSettling: return "stepping off";
		}
		return "?";
	}

	std::string_view Name(LadderHandOver::Event a_event)
	{
		switch (a_event) {
		case LadderHandOver::Event::kNone: return "none";
		case LadderHandOver::Event::kStarted: return "started";
		case LadderHandOver::Event::kClimbing: return "climbing";
		case LadderHandOver::Event::kEnded: return "ended";
		case LadderHandOver::Event::kNoClimb: return "no climb";
		case LadderHandOver::Event::kAborted: return "aborted";
		}
		return "?";
	}

	int LadderMotion(int a_state)
	{
		switch (a_state) {
		case 0:   // StartBottom: onto it at the bottom, going up
		case 2:   // UpRight
		case 3:   // UpLeft
		case 6:   // EndTop: off it at the top
			return 1;
		case 1:   // StartTop: onto it at the top, going down
		case 4:   // DownRight
		case 5:   // DownLeft
		case 7:   // EndBottom
		case 14:  // CoastStart .. CoastLanding: the slide down
		case 15:
		case 16:
		case 18:
		case 20:
			return -1;
		default:
			return 0;
		}
	}

	ClimbPace::Out ClimbPace::Update(std::uint64_t a_nowMs, int a_state, float a_y, float a_defaultSpeed)
	{
		Out out;
		// The measured pace: the Tarnished's vertical speed, eased (Elden Ring's climb moves him rung by rung).
		if (haveY_ && a_nowMs > lastMs_ && std::isfinite(a_y)) {
			const float dt = static_cast<float>(a_nowMs - lastMs_) / 1000.0f;
			if (dt < 0.5f) {
				const float v = std::fabs(a_y - lastY_) / dt;
				const float k = 1.0f - std::exp(-dt / kSpeedTauS);
				speed_ += (std::min(v, 20.0f) - speed_) * k;
			}
		}
		if (std::isfinite(a_y)) {
			haveY_ = true;
			lastY_ = a_y;
			lastMs_ = a_nowMs;
		}
		if (a_state == -1 && started_) {
			return out;  // a frame of "none" between two of its states: nothing changes
		}
		int motion = LadderMotion(a_state);
		movingSince_ = motion == 0 ? 0 : (movingSince_ ? movingSince_ : a_nowMs);
		stillSince_ = motion != 0 ? 0 : (stillSince_ ? stillSince_ : a_nowMs);
		if (started_ && motion == 0 && motionSent_ != 0 && a_nowMs - stillSince_ < kStillMs) {
			motion = motionSent_;  // a moment's idle between two pushes: still climbing as far as MGSV's climb goes
		}
		const bool  warm = movingSince_ && a_nowMs - movingSince_ >= kWarmupMs;
		const float speed = motion == 0 ? 0.0f : warm && speed_ > 0.05f ? speed_ : (moveSent_ > 0.0f ? moveSent_ : std::max(a_defaultSpeed, 0.0f));
		auto        fill = [&] {
			out.send = true;
			out.on = true;
			if (motion != 0) {
				down_ = motion < 0;
			}
			out.down = down_;
			out.speed = speed;
			out.motion = motion;
			motionSent_ = motion;
			speedSent_ = speed;
			moveSent_ = speed > 0.0f ? speed : moveSent_;
			sentMs_ = a_nowMs;
			pending_ = false;
			++sent_;
		};
		if (!started_) {
			started_ = true;
			fill();
			return out;
		}
		const bool turned = motion != motionSent_;
		const bool paced = motion != 0 && speedSent_ > 0.0f && std::fabs(speed - speedSent_) > kPaceChange * speedSent_;
		if (turned || pending_) {
			if (!turned) {
				pending_ = false;  // turned back within the gap: nothing to tell
				return out;
			}
			if (a_nowMs - sentMs_ >= kMinGapMs) {
				fill();
			} else {
				pending_ = true;
			}
			return out;
		}
		if (paced && a_nowMs - sentMs_ >= kPaceGapMs) {
			fill();
		}
		return out;
	}

	ClimbPace::Out ClimbPace::End()
	{
		Out out;
		if (started_) {
			out.send = true;
			out.on = false;
			out.down = down_;
			++sent_;
		}
		const auto sent = sent_;
		*this = ClimbPace();
		sent_ = sent;
		return out;
	}

	std::string_view LadderStateName(int a_state)
	{
		switch (a_state) {
		case -1: return "none";
		case 0: return "StartBottom";
		case 1: return "StartTop";
		case 2: return "UpRight";
		case 3: return "UpLeft";
		case 4: return "DownRight";
		case 5: return "DownLeft";
		case 6: return "EndTop";
		case 7: return "EndBottom";
		case 8: return "IdleRight";
		case 9: return "IdleLeft";
		case 10: return "AttackUpRight";
		case 11: return "AttackUpLeft";
		case 12: return "AttackDownRight";
		case 13: return "AttackDownLeft";
		case 14: return "CoastStart";
		case 15: return "CoastRight";
		case 16: return "CoastStop";
		case 18: return "CoastLeft";
		case 20: return "CoastLanding";
		case 21: return "DamageSmall";
		case 22: return "DamageLarge";
		default: return "?";
		}
	}
}
