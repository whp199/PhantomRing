#include "Dive.h"

#include <format>

namespace phantom::guest::dive
{
	bool DiveWatch::Update(const Facts& a_f, const Tunables& a_t, Event* a_event)
	{
		Event ev = Event::kNone;
		// A press is the counter moving: a tap shorter than a frame still counts, a held X is one press.
		const bool pressed = primed_ && a_f.xPresses != presses_;
		presses_ = a_f.xPresses;
		primed_ = true;
		const std::uint64_t in = a_f.nowMs - sinceMs_;
		switch (phase_) {
		case Phase::kIdle:
			if (pressed && a_f.eligible && a_f.standing) {
				phase_ = Phase::kArmed;
				sinceMs_ = a_f.nowMs;
				speedAtX_ = a_f.speed;
				normalAtX_ = a_f.normal;
				ev = Event::kArmed;
			}
			break;
		case Phase::kArmed:
			{
				if (!a_f.eligible) {
					phase_ = Phase::kIdle;
					ev = Event::kCancelled;
					break;
				}
				// Each sign is a change since X: NORMAL_ACTION already off at X (a reload, a weapon switch) says nothing about a dive.
				const char* sign = !a_f.standing                ? "left STAND"
				                 : a_f.crawl                    ? "CRAWL"
				                 : (normalAtX_ && !a_f.normal) ? "left NORMAL_ACTION"
				                 : (speedAtX_ < a_t.moveSpeed && a_f.speed >= a_t.moveSpeed) ? "sped up"
				                                                                              : nullptr;
				if (sign) {
					phase_ = Phase::kDiving;
					sign_ = sign;
					signMs_ = in;
					sinceMs_ = a_f.nowMs;
					++dives_;
					ev = Event::kConfirmed;
				} else if (in > a_t.confirmMs) {
					phase_ = Phase::kIdle;
					++unconfirmed_;
					ev = Event::kUnconfirmed;
				}
				break;
			}
		case Phase::kDiving:
			// Over: not eligible any more, long enough, or come to rest (prone at the end of it, or back on his feet).
			if (!a_f.eligible || in >= a_t.maxMs || (in >= a_t.minMs && a_f.speed < a_t.restSpeed)) {
				phase_ = Phase::kIdle;
				ev = Event::kEnded;
			}
			break;
		}
		if (a_event) {
			*a_event = ev;
		}
		return phase_ == Phase::kDiving;
	}

	std::string DiveWatch::Describe() const
	{
		return std::format("dive: {} -- {} dives{}, {} X presses with no dive", phase_ == Phase::kDiving ? "DIVING" : phase_ == Phase::kArmed ? "X pressed" : "idle",
			dives_, dives_ ? std::format(" (last confirmed by {} after {} ms)", sign_, signMs_) : std::string(), unconfirmed_);
	}
}
