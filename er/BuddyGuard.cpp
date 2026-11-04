#include "BuddyGuard.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace phantom::er::buddyguard
{
	namespace
	{
		float Median3(float a_a, float a_b, float a_c) { return std::max(std::min(a_a, a_b), std::min(std::max(a_a, a_b), a_c)); }
	}

	const char* Name(Act a_act)
	{
		static constexpr std::array<const char*, static_cast<std::size_t>(Act::kCount)> kNames{ "none", "snap spread", "jump held", "holding",
			"jump came back", "real move eased in", "real move taken out of view", "real move taken in view", "leftover limited" };
		const auto i = static_cast<std::size_t>(a_act);
		return i < kNames.size() ? kNames[i] : "?";
	}

	Step StepGuard::Update(const Vec3& a_raw, bool a_rawInView, bool a_drawnInView, float a_dt, std::uint64_t a_nowMs, const Rules& a_r)
	{
		Step out;
		if (!Finite(a_raw)) {
			out.offset = offset_;
			return out;
		}
		const float dt = std::clamp(std::isfinite(a_dt) ? a_dt : 1.0f / 60.0f, 1.0f / 240.0f, 0.25f);
		const float frames = std::max(1.0f, dt * 60.0f);
		if (!have_) {
			have_ = true;
			prevRaw_ = drawn_ = a_raw;
			offset_ = {};
			return out;
		}
		const Vec3 step = a_raw - prevRaw_;
		prevRaw_ = a_raw;
		out.raw = Length(step);
		const Vec3 prevDrawn = drawn_;
		// The point's own motion: the median of its last three steps (one snap never pollutes it), scaled to this frame.
		Vec3 expected{};
		if (steps_ >= 3) {
			expected = Vec3{ Median3(hist_[0].x, hist_[1].x, hist_[2].x), Median3(hist_[0].y, hist_[1].y, hist_[2].y), Median3(hist_[0].z, hist_[1].z, hist_[2].z) } *
			           (dt / std::max(1e-3f, histDt_));
		}
		const float excess = Length(step - expected);
		const float jumpMin = a_r.jumpMin + a_r.speedMax * std::max(0.0f, dt - 1.0f / 60.0f);
		const float snapTol = a_r.snapTol * frames;
		const float maxStep = a_r.maxStep * frames;
		const auto  takeNow = [&](Act a_act, float a_jump, std::uint32_t a_heldMs) {
			holding_ = easing_ = false;
			offset_ = {};
			drawn_ = a_raw;
			steps_ = 0;
			out.act = a_act;
			out.jump = a_jump;
			out.heldMs = a_heldMs;
			out.offset = {};
			out.drawn = Length(drawn_ - prevDrawn);
			return out;
		};
		const bool outOfView = !a_rawInView && !a_drawnInView;
		if (holding_) {
			const auto  held = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_nowMs - std::min(a_nowMs, holdSince_), 0xFFFFFFFFu));
			const float off = Length(a_raw - heldAt_);
			if (off <= std::max(1.0f, std::min(0.5f * heldJump_, kReturnMax))) {
				// It came back (near where it was held, or at least nearer than half the jump, at most kReturnMax): it was never drawn
				// there. A far jump that settles further from the held point is a real move (taken out of view, eased in or taken late):
				// with half the jump a 60 m one settling 20 m off was glided there at 0.5 m a frame, in view (review 0.8.0).
				holding_ = false;
				steps_ = 0;
				offset_ = heldAt_ - a_raw;
				out.act = Act::kReturned;
				out.jump = heldJump_;
				out.heldMs = held;
			} else if (outOfView) {
				return takeNow(Act::kTakenOut, off, held);
			} else if (held >= a_r.holdShortMs && Length(a_raw - heldAt_) <= a_r.easeMax) {
				holding_ = false;
				easing_ = true;
				steps_ = 0;
				offset_ = heldAt_ - a_raw;
				out.act = Act::kEase;
				out.jump = Length(offset_);
				out.heldMs = held;
			} else if (held >= a_r.holdInViewMs) {
				return takeNow(Act::kTakenLate, off, held);
			} else {
				offset_ = heldAt_ - a_raw;
				drawn_ = heldAt_;
				out.act = Act::kHolding;
				out.offset = offset_;
				out.jump = off;
				out.heldMs = held;
				return out;
			}
		} else if (excess >= jumpMin) {
			if (outOfView) {
				return takeNow(Act::kTakenOut, excess, 0);
			}
			holding_ = true;
			holdSince_ = a_nowMs;
			heldAt_ = drawn_;
			heldJump_ = Length(a_raw - drawn_);
			offset_ = heldAt_ - a_raw;
			out.act = Act::kHold;
			out.jump = heldJump_;
			out.offset = offset_;
			return out;
		} else {
			if (excess >= snapTol) {
				// A snap: what it moved beyond its own motion is taken back now and given out over the next frames.
				offset_ = offset_ - (step - expected);
				out.act = Act::kSnap;
				out.jump = excess;
			}
			hist_[next_] = step;
			next_ = (next_ + 1) % 3;
			steps_ = std::min(steps_ + 1, 3);
			histDt_ = dt;
		}
		// The leftover decays; the drawn point never moves more than maxStep a frame meanwhile.
		offset_ = offset_ * std::exp(-dt / std::max(0.01f, easing_ ? 0.5f * a_r.settleS : a_r.settleS));
		if (Length(offset_) < 0.005f) {
			offset_ = {};
			easing_ = false;
		}
		Vec3        want = a_raw + offset_;
		const Vec3  d = want - prevDrawn;
		const float dl = Length(d);
		if (dl > maxStep) {
			want = prevDrawn + d * (maxStep / dl);
			offset_ = want - a_raw;
			if (out.act == Act::kNone) {
				out.act = Act::kLimited;
			}
		}
		drawn_ = want;
		out.offset = offset_;
		out.drawn = Length(drawn_ - prevDrawn);
		return out;
	}

	void WaitPin::Begin(const Vec3& a_dogEr)
	{
		waiting_ = true;
		at_ = a_dogEr;
		slip_ = {};
		moved_ = 0;
		afterStaged_ = 0;
		endedBy_ = nullptr;
		// (a correction still easing out from a wait before is kept: it is where the dog is drawn now)
		if (LengthXZ(shift_) > 0.0f) {
			at_ = at_ + shift_;
			slip_ = Vec3{ -shift_.x, 0, -shift_.z };
		}
	}

	void WaitPin::End() { waiting_ = false; }

	Vec3 WaitPin::Update(const Vec3& a_slip, const Vec3& a_dogEr, bool a_staged, float a_dt)
	{
		const float dt = std::clamp(std::isfinite(a_dt) ? a_dt : 0.0f, 0.0f, 0.25f);
		const Vec3  prevDog = lastDog_;
		const bool  hadDog = haveDog_;
		if (Finite(a_dogEr)) {
			lastDog_ = a_dogEr;
			haveDog_ = true;
		}
		if (waiting_) {
			if (Finite(a_slip)) {
				slip_ = slip_ + Vec3{ a_slip.x, 0, a_slip.z };
			}
			if (LengthXZ(slip_) > kMax) {
				// Snake held against a wall for many seconds (a teleport never gets here: Buddy's Slip() drops a frame over 5 m): the wait
				// ends and the correction eases out like any other -- it was dropped in one frame (review 0.8.0).
				waiting_ = false;
				endedBy_ = "a long push against Elden Ring's wall (the pin let go)";
				return shift_;
			}
			if (a_staged) {
				afterStaged_ = kAfterStagedS;
				return shift_;  // MGSV's places mean nothing now: the correction stays as it was
			}
			if (afterStaged_ > 0.0f) {
				afterStaged_ -= dt;
				return shift_;
			}
			if (!Finite(a_dogEr)) {
				return shift_;
			}
			const Vec3  drift{ a_dogEr.x - at_.x, 0, a_dogEr.z - at_.z };
			const float s2 = slip_.x * slip_.x + slip_.z * slip_.z;
			const float k = s2 > 0.0025f ? std::clamp((drift.x * slip_.x + drift.z * slip_.z) / s2, 0.0f, 1.0f) : 0.0f;
			const Vec3  explained = slip_ * k;
			moved_ = LengthXZ(drift - explained);
			if (moved_ > kMovedEnds) {
				waiting_ = false;
				endedBy_ = "the dog moves in MGSV again";
				return shift_;
			}
			shift_ = Vec3{ -explained.x, 0, -explained.z };
			return shift_;
		}
		// Eased out at kEaseSpeed, or faster while the dog runs (kFollowShare of its own step): an attack order's run from the Wait
		// lands its bite on the target rather than metres off it (review 0.8.0).
		const float l = LengthXZ(shift_);
		float       ran = 0.0f;
		if (hadDog && Finite(a_dogEr)) {
			ran = LengthXZ(a_dogEr - prevDog);
			ran = ran > kRunStepMax ? 0.0f : ran;  // a jump, not its run
		}
		const float step = std::max(kEaseSpeed * dt, kFollowShare * ran);
		shift_ = l <= step || !(l > 0.0f) ? Vec3{} : shift_ * ((l - step) / l);
		return shift_;
	}
}
