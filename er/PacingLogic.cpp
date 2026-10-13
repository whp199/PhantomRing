#include "PacingLogic.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace phantom::er::pacing
{
	Quat Normalized(const Quat& a_q)
	{
		const float n = std::sqrt(a_q.x * a_q.x + a_q.y * a_q.y + a_q.z * a_q.z + a_q.w * a_q.w);
		return n > 1e-12f ? Quat{ a_q.x / n, a_q.y / n, a_q.z / n, a_q.w / n } : Quat{};
	}

	Quat Slerp(const Quat& a_a, Quat a_b, float a_t)
	{
		float d = a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z + a_a.w * a_b.w;
		if (d < 0) {
			a_b = { -a_b.x, -a_b.y, -a_b.z, -a_b.w };
			d = -d;
		}
		float wa = 1.0f - a_t, wb = a_t;
		// Within ~36 deg a normalized lerp stays within ~0.1 deg of the arc (one MGSV frame of motion is far less); past
		// that the real arc.
		if (d < 0.95f) {
			const float th = std::acos(std::min(1.0f, d));
			const float s = std::sin(th);
			wa = std::sin((1.0f - a_t) * th) / s;
			wb = std::sin(a_t * th) / s;
		}
		return Normalized({ a_a.x * wa + a_b.x * wb, a_a.y * wa + a_b.y * wb, a_a.z * wa + a_b.z * wb, a_a.w * wa + a_b.w * wb });
	}

	float LerpAngle(float a_a, float a_b, float a_t) { return WrapPi(a_a + WrapPi(a_b - a_a) * a_t); }

	bool IsCut(const Sample& a_a, const Sample& a_b)
	{
		if (a_a.epoch != a_b.epoch) {
			return true;
		}
		const double dx = a_b.odo[0] - a_a.odo[0], dy = a_b.odo[1] - a_a.odo[1], dz = a_b.odo[2] - a_a.odo[2];
		if (dx * dx + dy * dy + dz * dz > 3.0 * 3.0) {
			return true;  // a teleport the guest's odometer let through (the host drops those steps anyway)
		}
		if (Length(a_b.camRel - a_a.camRel) > 1.5f) {
			return true;  // a camera switch (first person, a cut)
		}
		const float d = std::fabs(a_a.camQuat.x * a_b.camQuat.x + a_a.camQuat.y * a_b.camQuat.y + a_a.camQuat.z * a_b.camQuat.z + a_a.camQuat.w * a_b.camQuat.w);
		return d < 0.94f;  // > ~40 deg in one MGSV frame: no camera pans that fast
	}

	Sample Blend(const Sample& a_a, const Sample& a_b, float a_t)
	{
		if (IsCut(a_a, a_b)) {
			return a_t < 0.5f ? a_a : a_b;
		}
		Sample s = a_t < 0.5f ? a_a : a_b;  // frame and epoch of the nearer one
		s.t = a_a.t + (a_b.t - a_a.t) * a_t;
		s.step = a_a.step + (a_b.step - a_a.step) * a_t;
		for (int i = 0; i < 3; ++i) {
			s.odo[i] = a_a.odo[i] + (a_b.odo[i] - a_a.odo[i]) * static_cast<double>(a_t);
		}
		s.rootYaw = LerpAngle(a_a.rootYaw, a_b.rootYaw, a_t);
		s.camRel = a_a.camRel + (a_b.camRel - a_a.camRel) * a_t;
		s.camQuat = Slerp(a_a.camQuat, a_b.camQuat, a_t);
		s.fovY = a_a.fovY + (a_b.fovY - a_a.fovY) * a_t;
		return s;
	}

	Quat RotationOf(const float* m, Vec3& a_scale)
	{
		// Rows x' = m0 x + m1 y + m2 z + m3: column i is where local axis i goes (scaled).
		Vec3 c0{ m[0], m[4], m[8] }, c1{ m[1], m[5], m[9] }, c2{ m[2], m[6], m[10] };
		a_scale = { Length(c0), Length(c1), Length(c2) };
		if (!(a_scale.x > 1e-8f && a_scale.y > 1e-8f && a_scale.z > 1e-8f)) {
			return Quat{ 0, 0, 0, 0 };  // degenerate: the caller blends element-wise
		}
		c0 = c0 * (1.0f / a_scale.x);
		c1 = c1 * (1.0f / a_scale.y);
		c2 = c2 * (1.0f / a_scale.z);
		if (Dot(Cross(c0, c1), c2) < 0) {  // a mirrored part: keep the rotation proper, the mirror in the x scale
			a_scale.x = -a_scale.x;
			c0 = -c0;
		}
		// Shepperd's method on R = [c0 c1 c2].
		const float r00 = c0.x, r10 = c0.y, r20 = c0.z, r01 = c1.x, r11 = c1.y, r21 = c1.z, r02 = c2.x, r12 = c2.y, r22 = c2.z;
		const float tr = r00 + r11 + r22;
		Quat        q;
		if (tr > 0) {
			const float s = std::sqrt(tr + 1.0f) * 2.0f;
			q = { (r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s };
		} else if (r00 > r11 && r00 > r22) {
			const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
			q = { 0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s };
		} else if (r11 > r22) {
			const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
			q = { (r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s };
		} else {
			const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
			q = { (r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s };
		}
		return Normalized(q);
	}

	void Blend34(const float* a_a, const float* a_b, float a_t, float* a_out)
	{
		Vec3       sa, sb;
		const Quat qa = RotationOf(a_a, sa);
		const Quat qb = RotationOf(a_b, sb);
		const bool ok = (qa.w != 0 || qa.x != 0 || qa.y != 0 || qa.z != 0) && (qb.w != 0 || qb.x != 0 || qb.y != 0 || qb.z != 0) && (sa.x < 0) == (sb.x < 0);
		if (!ok) {
			for (int i = 0; i < 12; ++i) {
				a_out[i] = a_a[i] + (a_b[i] - a_a[i]) * a_t;
			}
			return;
		}
		const Quat  q = Slerp(qa, qb, a_t);
		const Vec3  s = sa + (sb - sa) * a_t;
		const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
		const float r[3][3] = {
			{ 1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy) },
			{ 2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx) },
			{ 2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy) },
		};
		const float sc[3] = { s.x, s.y, s.z };
		for (int row = 0; row < 3; ++row) {
			for (int col = 0; col < 3; ++col) {
				a_out[row * 4 + col] = r[row][col] * sc[col];
			}
			a_out[row * 4 + 3] = a_a[row * 4 + 3] + (a_b[row * 4 + 3] - a_a[row * 4 + 3]) * a_t;
		}
	}

	void History::Clear()
	{
		n_ = 0;
		msPerStep_ = 0;
	}

	bool History::Add(Sample a_s, bool a_inferSteps)
	{
		if (n_) {
			const Sample last = FromNewest(0);
			if (a_s.epoch != last.epoch || a_s.frame + 600 < last.frame || (a_s.frame > last.frame && a_s.t < last.t)) {
				Clear();  // a restarted odometer or guest: nothing before it blends with what follows
			} else if (a_s.frame <= last.frame) {
				return false;  // seen (the guest republishes a stalled frame with a new stamp while its menu is open)
			} else {
				const double frames = static_cast<double>(a_s.frame - last.frame);
				const double dt = a_s.t - last.t;
				double       steps = frames;
				if (a_inferSteps && dt > 0) {
					// MGSV runs one fixed step per frame, two after a frame that took two steps' time (live: 49 ms -> 2 steps;
					// 17-25 ms -> 1). More than a few extra is a pause: the clock snaps then anyway.
					steps = std::clamp(std::floor(dt / kNominalStepMs), frames, frames + 4.0);
				}
				a_s.step = last.step + steps;
				const double per = dt / steps;
				if (per > 8.0 && per < 50.0) {
					msPerStep_ = msPerStep_ > 0 ? msPerStep_ + (per - msPerStep_) * 0.03 : per;
				}
			}
		}
		if (!n_) {
			a_s.step = 0;
		}
		buf_[head_] = a_s;
		head_ = (head_ + 1) % kSize;
		n_ = std::min(n_ + 1, kSize);
		return true;
	}

	std::optional<double> History::StepOf(std::uint64_t a_frame) const
	{
		if (!n_) {
			return std::nullopt;
		}
		const Sample& newest = FromNewest(0);
		if (a_frame >= newest.frame) {
			if (a_frame - newest.frame > 6) {
				return std::nullopt;
			}
			return newest.step + static_cast<double>(a_frame - newest.frame);
		}
		for (std::size_t i = 1; i < n_; ++i) {
			const Sample& s = FromNewest(i);
			if (s.frame <= a_frame) {
				if (s.frame == a_frame) {
					return s.step;
				}
				const Sample& after = FromNewest(i - 1);
				const double  k = static_cast<double>(a_frame - s.frame) / static_cast<double>(after.frame - s.frame);
				return s.step + (after.step - s.step) * k;
			}
		}
		return std::nullopt;
	}

	double History::StepAt(double a_nowMs) const
	{
		if (!n_) {
			return -std::numeric_limits<double>::infinity();
		}
		const Sample& newest = FromNewest(0);
		if (msPerStep_ <= 0) {
			return newest.step;
		}
		// Each recent frame says where MGSV would be now; their stamps jitter (sd ~1.7 ms live), the mean much less.
		constexpr std::size_t kN = 8;
		double                sum = 0;
		std::size_t           k = 0;
		for (std::size_t i = 0; i < std::min(n_, kN); ++i) {
			const Sample& s = FromNewest(i);
			if (newest.step - s.step > 3.0 * kN) {
				break;  // across a pause: only the frames after it
			}
			sum += s.step + (a_nowMs - s.t) / msPerStep_;
			++k;
		}
		return sum / static_cast<double>(k);
	}

	History::Pick History::At(double a_step, double a_maxExtrapolateSteps) const
	{
		Pick p;
		if (!n_) {
			return p;
		}
		const Sample& newest = FromNewest(0);
		if (n_ == 1) {
			p.a = p.b = &newest;
			p.t = 1;
			p.mode = Mode::kHoldNewest;
			return p;
		}
		if (a_step >= newest.step) {
			const Sample& prev = FromNewest(1);
			const double  span = newest.step - prev.step;
			p.a = &prev;
			p.b = &newest;
			if (a_step - newest.step <= a_maxExtrapolateSteps && span > 0 && span <= 4.0 && !IsCut(prev, newest)) {
				p.t = static_cast<float>(1.0 + (a_step - newest.step) / span);
				p.mode = Mode::kExtrapolate;
			} else {
				p.a = &newest;
				p.t = 1;
				p.mode = Mode::kHoldNewest;
			}
			return p;
		}
		const Sample& oldest = FromNewest(n_ - 1);
		if (a_step < oldest.step) {
			p.a = p.b = &oldest;
			p.mode = Mode::kHoldOldest;
			return p;
		}
		for (std::size_t i = 1; i < n_; ++i) {
			const Sample& a = FromNewest(i);
			if (a.step <= a_step) {
				const Sample& b = FromNewest(i - 1);
				const double  span = b.step - a.step;
				p.a = &a;
				p.b = &b;
				p.mode = Mode::kBlend;
				p.t = span > 1e-9 ? static_cast<float>((a_step - a.step) / span) : 1.0f;
				return p;
			}
		}
		p.a = p.b = &oldest;
		p.mode = Mode::kHoldOldest;
		return p;
	}

	bool FitOdometer(const History& a_h, double a_step, std::size_t a_window, const double a_blended[3], double a_maxOff, double a_out[3])
	{
		if (a_window < 3 || a_h.Size() < a_window) {
			return false;
		}
		const Sample& newest = a_h.FromNewest(0);
		const Sample& oldest = a_h.FromNewest(a_window - 1);
		if (newest.step - oldest.step > 2.0 * static_cast<double>(a_window) || a_step < oldest.step || a_step > newest.step + 2.0) {
			return false;  // a pause or hitches inside the window, or a moment it doesn't cover
		}
		double ms = 0;
		for (std::size_t i = 0; i < a_window; ++i) {
			const Sample& s = a_h.FromNewest(i);
			if (s.epoch != newest.epoch || (i > 0 && IsCut(s, a_h.FromNewest(i - 1)))) {
				return false;
			}
			ms += s.step;
		}
		ms /= static_cast<double>(a_window);
		double sss = 0;
		double out[3]{};
		for (int axis = 0; axis < 3; ++axis) {
			double mx = 0, ssx = 0;
			for (std::size_t i = 0; i < a_window; ++i) {
				mx += a_h.FromNewest(i).odo[axis];
			}
			mx /= static_cast<double>(a_window);
			sss = 0;
			for (std::size_t i = 0; i < a_window; ++i) {
				const Sample& s = a_h.FromNewest(i);
				ssx += (s.step - ms) * (s.odo[axis] - mx);
				sss += (s.step - ms) * (s.step - ms);
			}
			out[axis] = mx + (sss > 1e-12 ? ssx / sss : 0.0) * (a_step - ms);
		}
		const double dx = out[0] - a_blended[0], dy = out[1] - a_blended[1], dz = out[2] - a_blended[2];
		if (!(dx * dx + dy * dy + dz * dz <= a_maxOff * a_maxOff)) {
			return false;
		}
		std::copy(std::begin(out), std::end(out), a_out);
		return true;
	}

	double Quantile(std::vector<double> a_values, double a_q)
	{
		if (a_values.empty()) {
			return 0;
		}
		const auto k = static_cast<std::size_t>(std::clamp(a_q, 0.0, 1.0) * static_cast<double>(a_values.size() - 1) + 0.5);
		std::nth_element(a_values.begin(), a_values.begin() + static_cast<std::ptrdiff_t>(k), a_values.end());
		return a_values[k];
	}

	void StepClock::Reset()
	{
		started_ = false;
		lags_.clear();
		lagsNext_ = 0;
	}

	double StepClock::Update(double a_nowMs, double a_msPerStep, double a_target, double a_lag, const Params& a_p)
	{
		constexpr std::size_t kLags = 120;  // 2 s of Elden Ring frames
		if (a_lag >= 0 && a_lag < 10) {
			if (lags_.size() < kLags) {
				lags_.push_back(a_lag);
			} else {
				lags_[lagsNext_] = a_lag;
			}
			lagsNext_ = (lagsNext_ + 1) % kLags;
		}
		double want = a_p.fixedDelay;
		if (want < 0) {
			want = std::clamp(lags_.empty() ? a_p.maxDelay : Quantile(lags_, a_p.quantile) + a_p.margin, a_p.minDelay, a_p.maxDelay);
		}
		if (!started_ || a_p.fixedDelay >= 0) {
			delay_ = want;
		} else {
			delay_ += std::clamp(want - delay_, -a_p.delaySlew, a_p.delaySlew);
		}
		const double goal = a_target - delay_;
		if (!started_ || !(a_msPerStep > 0) || !std::isfinite(goal) || std::fabs(goal - (step_ + (a_nowMs - lastNow_) / a_msPerStep)) > a_p.snap) {
			snaps_ += started_ ? 1 : 0;
			step_ = goal;
			started_ = std::isfinite(goal);
		} else {
			step_ += (a_nowMs - lastNow_) / a_msPerStep;
			step_ += std::clamp((goal - step_) * a_p.gain, -a_p.maxCorrection, a_p.maxCorrection);
		}
		lastNow_ = a_nowMs;
		return step_;
	}
}
