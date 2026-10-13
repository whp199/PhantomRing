#include "Ladder.h"

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
