#pragma once

// Pure parts of frame pacing (er/Pacing.h; docs/HOST.md "Frame pacing"). No game access; tests/test_host_pacing.inc.
//
// What was measured (2026-10-12, both games, Limgrave): Elden Ring runs a steady 60 Hz (FrameBegin every 16.63-16.70
// ms); MGSV, hidden in gamescope, runs 51-57 Hz (17-21 ms, sd 1.7 ms) and advances its game by one FIXED step per frame
// (a constant camera pan turned exactly 0.288 deg per MGSV frame whether the frame took 16.5 or 21 ms; a 49 ms frame
// took two steps). Shown as they came, 12-14% of Elden Ring's frames repeated the last MGSV frame and the next one
// jumped: the panning camera's per-frame step had a residual of 56% rms, a running Snake's 48%.
//
// So the clock that matters is MGSV's step count, not its frame stamps: each MGSV frame is one step on a uniform
// timeline (a frame that took 2+ nominal steps of wall time is counted as that many), and Elden Ring's frames walk
// along that timeline at MGSV's measured pace (ms of wall time per step), a little behind its newest frame. Every
// Elden Ring frame then blends the two MGSV frames around its point: the odometer (warp-free), the camera relative to
// Snake's root, his yaw, the draw matrices relative to his root -- all continuous across the guest's treadmill warps.

#include "Math.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace phantom::er::pacing
{
	inline constexpr double kNominalStepMs = 1000.0 / 60.0;  // MGSV's fixed game step

	// One MGSV frame (GuestState) as far as motion goes.
	struct Sample
	{
		std::uint64_t frame{ 0 };
		double        t{ 0 };       // ms on the shared QPC clock: when MGSV sampled this frame
		double        step{ 0 };    // position on MGSV's step timeline (History::Add)
		std::uint32_t epoch{ 0 };   // GuestState.odoEpoch: never blended across
		double        odo[3]{};     // Snake's warp-free root (MGSV world)
		float         rootYaw{ 0 };
		Vec3          camRel;       // camera position - root (MGSV world): continuous across warps
		Quat          camQuat;      // unit
		float         fovY{ 0 };
	};

	Quat   Normalized(const Quat& a_q);
	Quat   Slerp(const Quat& a_a, Quat a_b, float a_t);  // shortest arc; t may leave [0, 1] a little (extrapolation)
	float  LerpAngle(float a_a, float a_b, float a_t);  // radians, the short way round
	// a and b blended at t. A cut between them (the odometer or the camera jumping, the camera turning more than ~40 deg:
	// a teleport, a camera switch) is not blended: the nearer one is taken whole.
	Sample Blend(const Sample& a_a, const Sample& a_b, float a_t);
	[[nodiscard]] bool IsCut(const Sample& a_a, const Sample& a_b);

	// Rotation of a 3x4 row-major affine matrix (x' = R S x + t; columns carry the per-axis scale, a mirror in x), and back.
	Quat RotationOf(const float* a_m, Vec3& a_scale);
	// out = a and b blended: translation lerped, rotation slerped, per-axis scale lerped (element-wise if degenerate).
	void Blend34(const float* a_a, const float* a_b, float a_t, float* a_out);

	// The last MGSV samples, oldest first, one per frame, each with its place on the step timeline.
	class History
	{
	public:
		static constexpr std::size_t kSize = 64;

		// Adds a frame newer than the newest (false otherwise). Its step: the previous one's + the frames between them, or
		// more when the wall time between them held more nominal steps (MGSV catching up after a hitch; a_inferSteps).
		// A new epoch, or a frame or stamp going backwards (a restarted guest), starts over.
		bool Add(Sample a_s, bool a_inferSteps = true);
		void Clear();
		[[nodiscard]] std::size_t   Size() const { return n_; }
		[[nodiscard]] const Sample& FromNewest(std::size_t a_i) const { return buf_[(head_ + kSize - 1 - a_i) % kSize]; }
		[[nodiscard]] const Sample* Newest() const { return n_ ? &FromNewest(0) : nullptr; }
		// Where guest frame a_frame is on the step timeline: exact if seen, else from the frames around it; nothing before
		// the oldest kept frame or more than a few frames past the newest.
		[[nodiscard]] std::optional<double> StepOf(std::uint64_t a_frame) const;
		// Wall-clock ms per MGSV step, smoothed (~1 s); 0 until two frames were seen.
		[[nodiscard]] double MsPerStep() const { return msPerStep_; }
		// The step MGSV would be at at a_nowMs if it went on at its pace: the average over the last few frames (their stamps
		// jitter by ~1.7 ms), -inf without samples.
		[[nodiscard]] double StepAt(double a_nowMs) const;

		enum class Mode : std::uint8_t
		{
			kNone,         // no sample
			kBlend,        // a.step <= s <= b.step
			kExtrapolate,  // past the newest: the last step carried on (at most the caller's limit)
			kHoldNewest,   // past the newest by more than that, a single sample, or a cut / gap at the newest
			kHoldOldest,   // before the oldest kept sample
		};
		struct Pick
		{
			const Sample* a{ nullptr };
			const Sample* b{ nullptr };
			float         t{ 0 };
			Mode          mode{ Mode::kNone };
		};
		[[nodiscard]] Pick At(double a_step, double a_maxExtrapolateSteps) const;

	private:
		std::array<Sample, kSize> buf_{};
		std::size_t               head_{ 0 };  // next write
		std::size_t               n_{ 0 };
		double                    msPerStep_{ 0 };
	};

	// Elden Ring's walk along MGSV's step timeline: each frame it advances by the frame's wall time at MGSV's pace and is
	// pulled gently (a capped correction, so the pace never visibly changes) toward "MGSV now" minus a delay. The delay
	// follows how far behind "now" the newest sample usually is (a quantile over ~2 s), so a frame after the point is
	// almost always there already; it moves slowly too. A big disagreement (a pause, a hitch, a load) is a snap.
	class StepClock
	{
	public:
		struct Params
		{
			double minDelay{ 0.25 };        // steps
			double maxDelay{ 3.0 };
			double quantile{ 0.97 };
			double margin{ 0.12 };
			double delaySlew{ 0.01 };       // steps per frame
			double fixedDelay{ -1 };        // >= 0: this delay (steps), no adaptation
			double gain{ 0.08 };            // share of the error corrected per frame
			double maxCorrection{ 0.03 };   // steps per frame (a ~3% change of pace at most)
			double snap{ 2.5 };             // steps of disagreement that jump instead
		};

		// a_nowMs: this Elden Ring frame's time; a_lag: how many steps behind "MGSV now" the newest sample is (>= 0; < 0
		// unknown); a_target: "MGSV now" (History::StepAt). Returns the step this frame shows.
		double Update(double a_nowMs, double a_msPerStep, double a_target, double a_lag, const Params& a_p);
		void   Reset();
		[[nodiscard]] double Delay() const { return delay_; }
		[[nodiscard]] double Step() const { return step_; }
		[[nodiscard]] std::uint64_t Snaps() const { return snaps_; }

	private:
		bool                started_{ false };
		double              step_{ 0 };
		double              lastNow_{ 0 };
		double              delay_{ 0 };
		std::vector<double> lags_;
		std::size_t         lagsNext_{ 0 };
		std::uint64_t       snaps_{ 0 };
	};

	// Snake's root motion comes from his animation, in whole 120 Hz keys: running, MGSV's frames alternate 2 and 3 keys
	// (live: 7.8 / 11.7 cm, a 2,2,2,2,2,3,3 pattern), so even blended evenly the world stepped +-1.8 cm around his true
	// speed (MGSV's own view does the same). The odometer at a_step from a straight-line fit over the last a_window MGSV
	// frames instead (step domain, per axis): measured on recorded runs, 1.2-1.4 cm rms -> 0.5-0.7, 2-6 cm off MGSV's
	// path at most (starts, stops, turns), no overshoot past a stop. False (a_out untouched) when the window holds fewer
	// frames, spans an epoch change, a cut or a pause, or the fit lands more than a_maxOff (m) from a_blended.
	bool FitOdometer(const History& a_history, double a_step, std::size_t a_window, const double a_blended[3], double a_maxOff, double a_out[3]);

	// A quantile of a small window (copy + nth_element), for the delay estimates.
	double Quantile(std::vector<double> a_values, double a_q);
}
