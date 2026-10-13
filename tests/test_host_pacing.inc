// Included by tests/test_host.cpp: er/PacingLogic (frame pacing: MGSV's frames blended into Elden Ring's) -- the
// quaternion and 3x4 blends, the step timeline, and a simulated MGSV at 52 Hz (one fixed game step per frame, stamps
// jittering like the live measurement) seen by a 60 Hz Elden Ring: the shown motion must advance evenly.

#include "CameraCollision.h"
#include "PacingLogic.h"

#include <random>

namespace pacing_test
{
	using namespace phantom::er::pacing;

	Quat AxisAngle(const Vec3& a_axis, float a_rad)
	{
		const Vec3 n = Normalize(a_axis);
		const float s = std::sin(a_rad * 0.5f);
		return { n.x * s, n.y * s, n.z * s, std::cos(a_rad * 0.5f) };
	}

	float AngleBetween(const Quat& a_a, const Quat& a_b)
	{
		const float d = std::fabs(a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z + a_a.w * a_b.w);
		return 2.0f * std::acos(std::min(1.0f, d));
	}

	// 3x4 rows of a rotation about y by a_rad, uniform scale a_s, translation a_t.
	void MatY(float a_rad, float a_s, const Vec3& a_t, float* a_m)
	{
		const float c = std::cos(a_rad) * a_s, s = std::sin(a_rad) * a_s;
		const float m[12] = { c, 0, s, a_t.x, 0, a_s, 0, a_t.y, -s, 0, c, a_t.z };
		std::copy_n(m, 12, a_m);
	}

	Sample At(std::uint64_t a_frame, double a_t, float a_yaw, double a_odoZ)
	{
		Sample s;
		s.frame = a_frame;
		s.t = a_t;
		s.epoch = 1;
		s.odo[2] = a_odoZ;
		s.rootYaw = a_yaw;
		s.camRel = { 0, 1.5f, -3.0f };
		s.camQuat = AxisAngle({ 0, 1, 0 }, a_yaw);
		s.fovY = 0.62f;
		return s;
	}

	struct Spread
	{
		double mean{ 0 }, cv{ 0 }, worst{ 0 };  // worst = max |x / mean - 1|
	};

	Spread SpreadOf(const std::vector<double>& a_x)
	{
		Spread r;
		if (a_x.empty()) {
			return r;
		}
		for (double x : a_x) {
			r.mean += x;
		}
		r.mean /= static_cast<double>(a_x.size());
		double var = 0;
		for (double x : a_x) {
			var += (x - r.mean) * (x - r.mean);
			r.worst = std::max(r.worst, std::fabs(x / r.mean - 1.0));
		}
		r.cv = std::sqrt(var / static_cast<double>(a_x.size())) / std::fabs(r.mean);
		return r;
	}
}

static void TestPacing()
{
	using namespace pacing_test;

	// Quaternions: halfway between identity and 90 deg about y is 45 deg; a negated end takes the short way.
	{
		const Quat a{}, b = AxisAngle({ 0, 1, 0 }, 1.5707963f);
		CHECK(Near(AngleBetween(Slerp(a, b, 0.5f), AxisAngle({ 0, 1, 0 }, 0.7853982f)), 0.0f, 1e-3f));
		const Quat nb{ -b.x, -b.y, -b.z, -b.w };
		CHECK(Near(AngleBetween(Slerp(a, nb, 0.5f), AxisAngle({ 0, 1, 0 }, 0.7853982f)), 0.0f, 1e-3f));
		// Past 36 deg the real arc: a third of 120 deg is 40.
		const Quat c = AxisAngle({ 1, 0, 0 }, 2.0943951f);
		CHECK(Near(AngleBetween(Slerp(a, c, 1.0f / 3.0f), AxisAngle({ 1, 0, 0 }, 0.6981317f)), 0.0f, 2e-3f));
		CHECK(Near(LerpAngle(3.0f, -3.0f, 0.5f), 3.1415927f, 1e-3f) || Near(LerpAngle(3.0f, -3.0f, 0.5f), -3.1415927f, 1e-3f));
	}

	// 3x4 blend: rotation, scale and translation each halfway; a mirrored part stays mirrored.
	{
		float a[12], b[12], m[12], want[12];
		MatY(0.0f, 1.2f, { 0, 0, 0 }, a);
		MatY(0.7f, 1.2f, { 1, 2, 3 }, b);
		Blend34(a, b, 0.5f, m);
		MatY(0.35f, 1.2f, { 0.5f, 1, 1.5f }, want);
		bool same = true;
		for (int i = 0; i < 12; ++i) {
			same = same && Near(m[i], want[i], 1e-4f);
		}
		CHECK(same);
		for (int r = 0; r < 3; ++r) {  // mirror x in both
			a[r * 4] = -a[r * 4];
			b[r * 4] = -b[r * 4];
			want[r * 4] = -want[r * 4];
		}
		Blend34(a, b, 0.5f, m);
		same = true;
		for (int i = 0; i < 12; ++i) {
			same = same && Near(m[i], want[i], 1e-4f);
		}
		CHECK(same);
		Vec3 s;
		const Quat q = RotationOf(want, s);
		CHECK(Near(s.x, -1.2f, 1e-4f) && Near(s.y, 1.2f, 1e-4f));
		CHECK(Near(AngleBetween(q, AxisAngle({ 0, 1, 0 }, 0.35f)), 0.0f, 1e-3f));
	}

	// The step timeline: one step per frame, two for a frame that took two steps' time, the pace in ms per step.
	{
		History h;
		CHECK(h.Add(At(10, 1000.0, 0, 0)));
		CHECK(!h.Add(At(10, 1001.0, 0, 0)));  // the same frame republished (MGSV's menu stall)
		for (std::uint64_t f = 11; f <= 20; ++f) {
			CHECK(h.Add(At(f, 1000.0 + 19.0 * static_cast<double>(f - 10), 0, 0)));
		}
		CHECK(Near(static_cast<float>(h.Newest()->step), 10.0f));
		CHECK(h.MsPerStep() > 18.9 && h.MsPerStep() < 19.1);
		h.Add(At(21, 1190.0 + 49.0, 0, 0));  // a 49 ms frame: two steps (live)
		CHECK(Near(static_cast<float>(h.Newest()->step), 12.0f));
		h.Add(At(23, 1239.0 + 38.0, 0, 0));  // a frame the host missed: still at least one step each
		CHECK(Near(static_cast<float>(h.Newest()->step), 14.0f));
		CHECK(Near(static_cast<float>(*h.StepOf(15)), 5.0f));
		CHECK(Near(static_cast<float>(*h.StepOf(22)), 13.0f));
		CHECK(!h.StepOf(5));
		const auto mid = h.At(4.25, 0.3);
		CHECK(mid.mode == History::Mode::kBlend && mid.a->frame == 14 && mid.b->frame == 15 && Near(mid.t, 0.25f));
		const auto ex = h.At(14.2, 0.3);
		CHECK(ex.mode == History::Mode::kExtrapolate && Near(ex.t, 1.1f));
		CHECK(h.At(15.0, 0.3).mode == History::Mode::kHoldNewest);
		CHECK(h.At(-1.0, 0.3).mode == History::Mode::kHoldOldest);
		Sample other = At(24, 1300.0, 0, 0);
		other.epoch = 2;  // a new odometer: nothing before it
		CHECK(h.Add(other) && h.Size() == 1);
	}

	// A cut is never blended: a teleport, a camera switch.
	{
		const Sample a = At(1, 0, 0, 0);
		Sample       b = At(2, 19, 0, 5.0);
		CHECK(IsCut(a, b) && Near(static_cast<float>(Blend(a, b, 0.4f).odo[2]), 0.0f) && Near(static_cast<float>(Blend(a, b, 0.6f).odo[2]), 5.0f));
		b = At(2, 19, 0, 0.08);
		b.camRel = { 0.3f, 1.6f, 0.1f };  // first person: the camera jumps into the head
		CHECK(IsCut(a, b));
		b = At(2, 19, 0.005f, 0.08);
		CHECK(!IsCut(a, b) && Near(static_cast<float>(Blend(a, b, 0.5f).odo[2]), 0.04f));
	}

	// Live-like run: MGSV 19 ms frames (sd ~1.6 ms), one fixed step each (0.288 deg of camera, 7.8 cm of odometer), a
	// 49 ms frame every ~5 s (two steps); Elden Ring at 16.67 ms looks at FrameBegin. Shown per Elden Ring frame:
	// newest sample (what Snake did before) vs the paced blend.
	{
		std::mt19937                           rng(7);
		std::normal_distribution<double>       jitter(0.0, 1.6);
		std::vector<std::pair<double, Sample>> frames;  // (time it is visible to Elden Ring, sample)
		double                                 t = 0, step = 0;
		for (std::uint64_t f = 1; f < 900; ++f) {
			const bool hitch = f % 260 == 0;
			t += hitch ? 49.0 : std::clamp(19.0 + jitter(rng), 16.0, 23.0);
			step += hitch ? 2 : 1;
			Sample s = At(f, t, static_cast<float>(step * 0.288 * 3.14159265 / 180.0), step * 0.078);
			frames.emplace_back(t + 0.2, s);  // published right after it is stamped
		}
		History              h;
		StepClock            clock;
		StepClock::Params    p;
		std::vector<double>  naive, paced;
		double               lastNaive = -1, lastPaced = -1;
		std::size_t          next = 0, extrapolated = 0;
		for (double now = 100.0; now < t - 50.0; now += 1000.0 / 60.0) {
			while (next < frames.size() && frames[next].first <= now) {
				h.Add(frames[next++].second);
			}
			const Sample* newest = h.Newest();
			const double  target = h.StepAt(now);
			const double  shown = clock.Update(now, h.MsPerStep(), target, newest ? target - newest->step : -1.0, p);
			const auto    pick = h.At(shown, 1.5);
			if (now < 2000.0 || !pick.a) {
				continue;  // warm-up: the delay settles
			}
			extrapolated += pick.mode == History::Mode::kExtrapolate ? 1 : 0;
			const double v = Blend(*pick.a, *pick.b, pick.t).odo[2];
			if (lastPaced >= 0) {
				paced.push_back(v - lastPaced);
			}
			lastPaced = v;
			if (lastNaive >= 0) {
				naive.push_back(newest->odo[2] - lastNaive);
			}
			lastNaive = newest->odo[2];
		}
		const auto n = SpreadOf(naive);
		const auto s = SpreadOf(paced);
		std::printf("pacing sim: per-frame odometer step: newest sample cv %.3f worst %.2f | paced cv %.4f worst %.3f, delay %.2f steps, %zu extrapolated, %llu snaps\n",
			n.cv, n.worst, s.cv, s.worst, clock.Delay(), extrapolated, static_cast<unsigned long long>(clock.Snaps()));
		CHECK(n.cv > 0.3);
		CHECK(s.cv < 0.06);              // the hitch's double step stays (MGSV's game really jumped two steps), spread
		CHECK(Near(static_cast<float>(s.mean), static_cast<float>(0.078 * 16.667 / 19.0), 0.004f));  // MGSV's pace, not 60 Hz
		CHECK(extrapolated < paced.size() / 20);
		CHECK(clock.Snaps() == 0);
		CHECK(clock.Delay() > 0.5 && clock.Delay() < 1.8);
	}

	// Running: MGSV's root advances 2,2,2,2,2,3,3 animation keys (3.9 cm each) per frame. Blending the frames shows that
	// pattern; the line fit over 7 frames about halves it, and settles on the spot where he stops.
	{
		History              h;
		double               keys = 0;
		const int            pattern[7] = { 2, 2, 2, 2, 2, 3, 3 };
		std::vector<double>  blended, fitted;
		double               lastB = -1, lastF = -1;
		for (std::uint64_t f = 1; f <= 140; ++f) {
			keys += f <= 100 ? pattern[f % 7] : 0;  // stops after frame 100
			h.Add(At(f, 19.0 * static_cast<double>(f), 0, keys * 0.039));
			if (f < 20) {
				continue;
			}
			const double step = h.Newest()->step - 1.14;
			const auto   pick = h.At(step, 1.5);
			const double b = Blend(*pick.a, *pick.b, pick.t).odo[2];
			double       fit[3]{};
			const double blendedOdo[3] = { 0, 0, b };
			const double v = FitOdometer(h, step, 7, blendedOdo, 0.15, fit) ? fit[2] : b;
			if (f < 95) {
				if (lastB >= 0) {
					blended.push_back(b - lastB);
					fitted.push_back(v - lastF);
				}
			}
			lastB = b;
			lastF = v;
		}
		const auto sb = SpreadOf(blended), sf = SpreadOf(fitted);
		std::printf("pacing: running key steps per frame: blended cv %.3f, fitted cv %.4f; stopped at %.3f m (MGSV %.3f)\n", sb.cv, sf.cv, lastF, keys * 0.039);
		CHECK(sb.cv > 0.15);
		CHECK(sf.cv < 0.6 * sb.cv);  // about half (as on the recorded runs)
		CHECK(Near(static_cast<float>(lastF), static_cast<float>(keys * 0.039), 0.002f));  // at rest exactly where MGSV stopped
	}

	// Camera collision: in at once, out eased, never closer than the minimum, let go once back.
	{
		using phantom::er::camcol::Params;
		using phantom::er::camcol::Smoother;
		Smoother     c;
		const Params p;
		const float  inf = std::numeric_limits<float>::infinity();
		CHECK(c.Update(inf, 5.0f, 1.0f / 60, p) >= 5.0f && !c.Pulling());
		CHECK(Near(c.Update(2.0f, 5.0f, 1.0f / 60, p), 1.75f));             // a trunk on the line: in this frame
		const float out1 = c.Update(inf, 5.0f, 1.0f / 60, p);                // clear again: not straight back
		CHECK(out1 > 1.75f && out1 < 1.75f + 8.0f / 60.0f + 1e-4f);
		CHECK(Near(c.Update(2.0f, 5.0f, 1.0f / 60, p), 1.75f));             // in again at once
		float d = 0;
		for (int i = 0; i < 60; ++i) {
			d = c.Update(inf, 5.0f, 1.0f / 60, p);
		}
		CHECK(d >= 5.0f && !c.Pulling());                                     // back out within a second
		CHECK(Near(c.Update(0.2f, 5.0f, 1.0f / 60, p), 0.1f));              // a wall at the eye: the minimum
	}
}
