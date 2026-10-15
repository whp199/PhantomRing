#pragma once

// The Phantom Cigar = Elden Ring time skip (roadmap 2.7, protocol v13 kGEvTimeSkip). Pure arithmetic shared by both plugins,
// header-only so the tests (tests/test_host_timeskip.inc) see exactly what the games run:
//  * the guest (mgsv/Cigar.cpp) watches MGSV's clock while Snake smokes and turns how far it moved into whole minutes
//    (Tracker), streamed in small events while the time-lapse runs so Elden Ring's sky can follow it, and decides when the
//    sandbox's frozen clock is let run (ClockGovernor);
//  * the host (er/TimeSkip.cpp) spreads every skip over a short time (Pacer) and writes Elden Ring's clock a step at a time,
//    so its sky moves rather than jumps.
// Both clocks are a 24 h dial in seconds (MGSV vars.clock; Elden Ring's WorldAreaTime h/m/s): nothing here knows a date,
// a skip past midnight just goes round.

#include <algorithm>
#include <cstdint>
#include <utility>

namespace phantom::timeskip
{
	inline constexpr std::int32_t kDay = 24 * 60 * 60;

	// Any second count onto the dial, [0, kDay).
	constexpr std::int32_t Wrap(std::int64_t a_seconds)
	{
		const auto r = a_seconds % kDay;
		return static_cast<std::int32_t>(r < 0 ? r + kDay : r);
	}

	// Seconds forward from a_from to a_to on the dial: 23:50 -> 00:10 is 20 minutes, never negative (a clock that went back a
	// little reads as almost a day forward: callers bound a plausible step, Tracker::Params::maxStepSec).
	constexpr std::int32_t Forward(std::int32_t a_from, std::int32_t a_to) { return Wrap(static_cast<std::int64_t>(a_to) - a_from); }

	struct Hms
	{
		std::uint32_t h{ 0 }, m{ 0 }, s{ 0 };
		constexpr bool operator==(const Hms&) const = default;
	};

	constexpr Hms ToHms(std::int64_t a_seconds)
	{
		const auto w = static_cast<std::uint32_t>(Wrap(a_seconds));
		return { w / 3600, (w / 60) % 60, w % 60 };
	}

	constexpr std::int32_t FromHms(const Hms& a_t) { return Wrap(static_cast<std::int64_t>(a_t.h) * 3600 + a_t.m * 60 + a_t.s); }

	// a_t moved a_seconds forward round the dial.
	constexpr Hms Advance(const Hms& a_t, std::int64_t a_seconds) { return ToHms(static_cast<std::int64_t>(FromHms(a_t)) + a_seconds); }

	// "+3 h 20 min" for the log and the corner message.
	struct Span
	{
		std::uint32_t hours{ 0 }, minutes{ 0 };
	};
	constexpr Span SpanOf(std::uint64_t a_minutes) { return { static_cast<std::uint32_t>(a_minutes / 60), static_cast<std::uint32_t>(a_minutes % 60) }; }

	// ---- guest: MGSV's clock while Snake smokes -> whole minutes ------------------------------------------------------
	//
	// Fed MGSV's clock (seconds of the day) and whether Snake smokes, a few times a second. Only time that passes while he
	// smokes counts -- plus a short tail after the smoking flag drops (the clock may settle a tick later) and the step just
	// before it rose (the flag may come a tick after the clock starts running). Steps longer than maxStepSec are not the
	// cigar (a TppClock.SetTime, a mission load, a clock that went back) and are left out. Whole minutes go out at most
	// every emitEveryMs; at the end the rest is rounded to the nearest minute.
	struct TrackerParams  // (outside the class: GCC can't take a nested struct's member initializers as a default argument)
	{
		std::uint64_t tailMs{ 1500 };
		std::uint64_t emitEveryMs{ 250 };
		std::int32_t  maxStepSec{ 6 * 3600 };
	};

	class Tracker
	{
	public:
		using Params = TrackerParams;

		struct Out
		{
			std::uint32_t minutes{ 0 };  // to send now (kGEvTimeSkip a)
			bool          started{ false };
			bool          ended{ false };
		};

		Out Update(std::uint64_t a_nowMs, std::int32_t a_clock, bool a_smoking, const Params& a_p = {})
		{
			Out out;
			const std::int32_t clock = Wrap(a_clock);
			std::int32_t       step = 0;
			if (primed_) {
				step = Forward(last_, clock);
				if (step > a_p.maxStepSec) {
					++ignored_;
					step = 0;
				}
			}
			const std::int32_t before = last_;
			last_ = clock;
			primed_ = true;

			if (a_smoking) {
				if (!active_) {
					active_ = true;
					out.started = true;
					acc_ = prevStep_;  // the step before the flag rose
					sessionSec_ = prevStep_;
					startClock_ = Wrap(static_cast<std::int64_t>(before) - prevStep_);
					lastEmitMs_ = a_nowMs;
				}
				lastSmokeMs_ = a_nowMs;
			}
			prevStep_ = step;
			if (!active_) {
				return out;
			}
			acc_ += step;
			sessionSec_ += step;
			if (acc_ >= 60 && a_nowMs - lastEmitMs_ >= a_p.emitEveryMs) {
				out.minutes = static_cast<std::uint32_t>(acc_ / 60);
				acc_ %= 60;
				lastEmitMs_ = a_nowMs;
			}
			if (!a_smoking && a_nowMs - lastSmokeMs_ >= a_p.tailMs) {
				out.minutes += static_cast<std::uint32_t>(acc_ / 60) + (acc_ % 60 >= 30 ? 1u : 0u);
				acc_ = 0;
				active_ = false;
				out.ended = true;
			}
			sentMin_ += out.minutes;
			return out;
		}

		// A load, a lost sample: the next clock starts afresh (nothing is counted across the gap).
		void Reset()
		{
			primed_ = false;
			prevStep_ = 0;
			if (!active_) {
				return;
			}
			active_ = false;
			acc_ = 0;
		}

		[[nodiscard]] bool          Active() const { return active_; }
		[[nodiscard]] std::int32_t  StartClock() const { return startClock_; }  // MGSV's clock when he lit it
		[[nodiscard]] std::int64_t  SessionSeconds() const { return sessionSec_; }  // MGSV time this smoke (so far)
		[[nodiscard]] std::uint64_t SentMinutes() const { return sentMin_; }        // every minute sent (all smokes)
		[[nodiscard]] std::uint64_t Ignored() const { return ignored_; }            // implausible steps left out

	private:
		bool          primed_{ false };
		bool          active_{ false };
		std::int32_t  last_{ 0 };
		std::int32_t  prevStep_{ 0 };
		std::int64_t  acc_{ 0 };
		std::int64_t  sessionSec_{ 0 };
		std::int32_t  startClock_{ 0 };
		std::uint64_t lastSmokeMs_{ 0 };
		std::uint64_t lastEmitMs_{ 0 };
		std::uint64_t sentMin_{ 0 };
		std::uint64_t ignored_{ 0 };
	};

	// ---- guest: the sandbox's frozen clock while Snake smokes -----------------------------------------------------------
	//
	// The sandbox freezes MGSV's clock ([Boot] FreezeClock: time scale 0). How MGSV's cigar moves the clock is UNVERIFIED live:
	// if its time-lapse sets the time scale itself, setting ours on top would override it (a 20 s smoke at x20 is ~7 min, not
	// hours); if the frozen scale is what stops it, the clock must be let run. So the governor waits probeMs after he lights it
	// and asks for ClockScale (kRun) only if MGSV's clock has not moved by itself by then. kLit/kOut tell the Lua when a smoke
	// starts and ends (it freezes the clock at kOut and keeps re-freezing it while he doesn't smoke, whenever MGSV restores its
	// own speed). A smoke longer than maxSmokeMs is a signal that stuck (or one that means something else, UNVERIFIED): it ends
	// the smoke and is ignored until it drops. Milliseconds, not Lua ticks: PhantomRing.Tick runs at MGSV's frame rate (~60 Hz,
	// slower in a load), so a tick count is no clock.
	struct GovernorParams
	{
		std::uint64_t probeMs{ 500 };        // MGSV's own time-lapse (x20 or more) moves the clock >= 10 s in this
		std::uint64_t maxSmokeMs{ 120000 };  // ~2 min: far longer than a smoke
	};

	class ClockGovernor
	{
	public:
		using Params = GovernorParams;

		enum class Cmd : std::uint8_t
		{
			kNone,
			kLit,  // he lit it: stop holding the clock frozen
			kRun,  // MGSV's clock stood still probeMs in: run it at [Cigar] ClockScale
			kOut,  // the smoke is over (or the signal stuck, or a load): frozen again
		};

		struct Out
		{
			Cmd  cmd{ Cmd::kNone };
			bool smoking{ false };  // the signal, unless it stuck
			bool stuck{ false };    // it stuck just now
		};

		Out Update(std::uint64_t a_nowMs, bool a_signal, std::int32_t a_clock, const Params& a_p = {})
		{
			Out out;
			if (!a_signal) {
				stuck_ = false;
			} else if (on_ && !stuck_ && a_nowMs - since_ > a_p.maxSmokeMs) {
				stuck_ = true;
				++stucks_;
				out.stuck = true;
			}
			out.smoking = a_signal && !stuck_;
			if (out.smoking && !on_) {
				on_ = true;
				probed_ = false;
				since_ = a_nowMs;
				start_ = Wrap(a_clock);
				out.cmd = Cmd::kLit;
			} else if (out.smoking && !probed_ && a_nowMs - since_ >= a_p.probeMs) {
				probed_ = true;
				if (Wrap(a_clock) == start_) {
					++runs_;
					out.cmd = Cmd::kRun;
				} else {
					++selfRuns_;
				}
			} else if (!out.smoking && on_) {
				on_ = false;
				out.cmd = Cmd::kOut;
			}
			return out;
		}

		// A load: a smoke in progress is over. True if one was (the caller sends kOut); a stuck signal stays stuck.
		bool Reset() { return std::exchange(on_, false); }

		[[nodiscard]] bool          On() const { return on_; }
		[[nodiscard]] bool          Stuck() const { return stuck_; }
		[[nodiscard]] std::uint64_t Stucks() const { return stucks_; }      // signals that stuck (all smokes)
		[[nodiscard]] std::uint64_t Runs() const { return runs_; }          // smokes the guest ran the clock for
		[[nodiscard]] std::uint64_t SelfRuns() const { return selfRuns_; }  // smokes MGSV's clock moved by itself

	private:
		bool          on_{ false };
		bool          stuck_{ false };
		bool          probed_{ false };
		std::uint64_t since_{ 0 };
		std::int32_t  start_{ 0 };
		std::uint64_t stucks_{ 0 }, runs_{ 0 }, selfRuns_{ 0 };
	};

	// ---- host: a skip spread over time -----------------------------------------------------------------------------------
	//
	// Skips add up; each Step (every stepMs) hands out a share of what is left -- pending * stepMs / smoothMs, at least
	// minChunkSec -- so a big skip starts fast and eases in, and the small ones the guest streams while MGSV's time-lapse runs
	// follow it closely. Whole days are dropped (the dial looks the same).
	struct PacerParams
	{
		std::uint32_t smoothMs{ 1000 };
		std::uint32_t stepMs{ 100 };
		std::int32_t  minChunkSec{ 60 };
	};

	class Pacer
	{
	public:
		using Params = PacerParams;

		void Add(std::int64_t a_seconds)
		{
			if (a_seconds <= 0) {
				return;
			}
			pending_ += a_seconds;
			if (pending_ >= kDay) {
				pending_ %= kDay;
			}
		}

		// Seconds to move Elden Ring's clock by now (0: nothing, or not yet: steps are stepMs apart).
		std::int64_t Step(std::uint64_t a_nowMs, const Params& a_p = {})
		{
			if (pending_ <= 0 || (stepped_ && a_nowMs - lastStepMs_ < a_p.stepMs)) {
				return 0;
			}
			stepped_ = true;
			lastStepMs_ = a_nowMs;
			const auto smooth = std::max<std::uint32_t>(a_p.smoothMs, 1);
			std::int64_t chunk = pending_ * a_p.stepMs / smooth;
			chunk = std::min<std::int64_t>(std::max<std::int64_t>(chunk, std::max<std::int32_t>(a_p.minChunkSec, 1)), pending_);  // the last one: what is left
			pending_ -= chunk;
			return chunk;
		}

		void Clear()
		{
			pending_ = 0;
			stepped_ = false;
		}

		[[nodiscard]] std::int64_t Pending() const { return pending_; }

	private:
		std::int64_t  pending_{ 0 };
		bool          stepped_{ false };
		std::uint64_t lastStepMs_{ 0 };
	};

	// The time to write next: the host's own cursor (its last write may land a frame later, so the clock can still read the old
	// time), unless Elden Ring's clock reads at most a_slackSec ahead of it -- its own clock ran on meanwhile, take that.
	constexpr std::int32_t Reconcile(std::int32_t a_cursor, std::int32_t a_read, std::int32_t a_slackSec = 600)
	{
		return Forward(a_cursor, a_read) <= a_slackSec ? Wrap(a_read) : Wrap(a_cursor);
	}
}
