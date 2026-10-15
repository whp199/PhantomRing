// The Phantom Cigar = Elden Ring time skip (common/TimeSkipLogic.h, er/TimeSkip.h): the dial arithmetic both games' clocks
// share, the guest's minute tracker, the host's pacer. Included by test_host.cpp.
#include "TimeSkipLogic.h"
#include "TimeSkip.h"

static void TestTimeSkip()
{
	namespace ts = phantom::timeskip;

	// ---- the dial: minute arithmetic and the day wrap
	CHECK(ts::Wrap(0) == 0 && ts::Wrap(ts::kDay) == 0 && ts::Wrap(-1) == ts::kDay - 1 && ts::Wrap(3 * ts::kDay + 5) == 5);
	CHECK(ts::Forward(12 * 3600, 18 * 3600) == 6 * 3600);
	CHECK(ts::Forward(23 * 3600 + 50 * 60, 10 * 60) == 20 * 60);  // 23:50 -> 00:10 is 20 min, round midnight
	CHECK(ts::Forward(100, 100) == 0);
	CHECK(ts::Forward(101, 100) == ts::kDay - 1);  // a clock that went back reads as almost a day: callers bound the step
	CHECK((ts::ToHms(0) == ts::Hms{ 0, 0, 0 }) && (ts::ToHms(ts::kDay - 1) == ts::Hms{ 23, 59, 59 }) && (ts::ToHms(45296) == ts::Hms{ 12, 34, 56 }));
	CHECK(ts::FromHms({ 12, 34, 56 }) == 45296 && ts::FromHms({ 24, 0, 0 }) == 0);
	CHECK((ts::Advance({ 12, 0, 0 }, 6 * 3600 + 30 * 60) == ts::Hms{ 18, 30, 0 }));  // noon + 6 h 30 min
	CHECK((ts::Advance({ 22, 45, 30 }, 2 * 3600) == ts::Hms{ 0, 45, 30 }));        // over midnight
	CHECK((ts::Advance({ 23, 59, 59 }, 1) == ts::Hms{ 0, 0, 0 }));
	CHECK((ts::Advance({ 6, 0, 0 }, 3 * ts::kDay + 60) == ts::Hms{ 6, 1, 0 }));     // whole days change nothing
	CHECK(ts::SpanOf(200).hours == 3 && ts::SpanOf(200).minutes == 20 && ts::SpanOf(59).hours == 0);

	// ---- guest: the tracker counts only what passes while Snake smokes, in whole minutes
	{
		ts::Tracker t;
		const ts::Tracker::Params p;  // tail 1500 ms, emit every 250 ms, steps > 6 h ignored
		std::uint64_t now = 1000;
		std::int32_t  clock = 12 * 3600;
		std::uint64_t sent = 0;
		auto tick = [&](std::int32_t a_stepSec, bool a_smoking) {
			now += 33;
			clock = ts::Wrap(static_cast<std::int64_t>(clock) + a_stepSec);
			const auto o = t.Update(now, clock, a_smoking, p);
			sent += o.minutes;
			return o;
		};
		// A running clock while he doesn't smoke ([Boot] FreezeClock = false): never counted.
		for (int i = 0; i < 100; ++i) {
			CHECK(tick(1, false).minutes == 0);
		}
		CHECK(!t.Active() && sent == 0);
		// He lights it: the time-lapse races 6 h in 3 s (90 ticks x 240 s).
		const auto first = tick(240, true);
		CHECK(first.started && t.Active());
		for (int i = 1; i < 90; ++i) {
			tick(240, true);
		}
		CHECK(sent > 0 && sent < 6 * 60);  // streamed while he smokes...
		// ...he stops; the clock settles one more step, then the tail runs out and the rest is flushed.
		tick(30, false);
		bool ended = false;
		for (int i = 0; i < 60 && !ended; ++i) {
			ended = tick(0, false).ended;
		}
		CHECK(ended && !t.Active());
		CHECK(sent == 6 * 60 + 1);  // 6 h + the 30 s settle (+ the 1 s step before the flag rose) rounds up to a minute
		CHECK(t.SessionSeconds() == 6 * 3600 + 30 + 1 && t.SentMinutes() == sent);
		// Afterwards (frozen clock re-set by TppClock.SetTime, a big jump): nothing more.
		tick(-3 * 3600, false);
		tick(0, false);
		CHECK(sent == 6 * 60 + 1);
	}
	{
		// Smoking over midnight, the flag a tick late (the step before it rose counts), and a junk jump left out.
		ts::Tracker t;
		std::uint64_t now = 0, sent = 0;
		std::int32_t  clock = 22 * 3600;
		auto tick = [&](std::int32_t a_stepSec, bool a_smoking) {
			now += 100;
			clock = ts::Wrap(static_cast<std::int64_t>(clock) + a_stepSec);
			const auto o = t.Update(now, clock, a_smoking);
			sent += o.minutes;
			return o;
		};
		tick(0, false);
		tick(600, false);  // the clock already moved 10 min, the flag comes next tick
		CHECK(tick(600, true).started);
		CHECK(ts::ToHms(t.StartClock()).h == 22 && ts::ToHms(t.StartClock()).m == 0);
		for (int i = 0; i < 10; ++i) {
			tick(600, true);  // to 22:00 + 120 min = 00:00
		}
		tick(10 * 3600, true);  // implausible (> 6 h in a tick): not the cigar
		for (int i = 0; i < 30; ++i) {
			tick(0, false);
		}
		CHECK(!t.Active() && sent == 120 && t.Ignored() == 1);
	}
	{
		// Emission is paced: whole minutes at most every emitEveryMs; Reset drops a smoke cut by a load.
		ts::Tracker t;
		ts::Tracker::Params p;
		p.emitEveryMs = 1000;
		std::int32_t clock = 0;
		int          events = 0;
		for (std::uint64_t ms = 0; ms <= 3000; ms += 50) {
			clock += 120;
			if (t.Update(ms, clock, true, p).minutes) {
				++events;
			}
		}
		CHECK(events == 3);
		t.Reset();
		CHECK(!t.Active());
		CHECK(t.Update(4000, clock + 3600, true, p).minutes == 0);  // after a reset the first sample only primes
	}

	// ---- guest: the governor -- MGSV's own time-lapse is never overridden, the clock let run only if it stands still, the
	// stuck guard in milliseconds (PhantomRing.Tick's ~60 Hz is no clock: review), every smoke ends frozen
	{
		using Cmd = ts::ClockGovernor::Cmd;
		const ts::ClockGovernor::Params p;  // probe 500 ms, stuck after 120 s
		std::int32_t clock = 12 * 3600;
		// MGSV runs its own time-lapse: its clock moves within the probe, so no speed is set over it.
		{
			ts::ClockGovernor g;
			CHECK(g.Update(1000, false, clock, p).cmd == Cmd::kNone);
			auto o = g.Update(1016, true, clock, p);
			CHECK(o.cmd == Cmd::kLit && o.smoking && g.On());
			CHECK(g.Update(1300, true, clock + 200, p).cmd == Cmd::kNone);   // probing
			CHECK(g.Update(1516, true, clock + 400, p).cmd == Cmd::kNone);   // moved by itself: left to MGSV
			CHECK(g.Update(1600, true, clock + 500, p).cmd == Cmd::kNone);   // and decided once
			CHECK(g.Runs() == 0 && g.SelfRuns() == 1);
			o = g.Update(21000, false, clock + 6 * 3600, p);
			CHECK(o.cmd == Cmd::kOut && !o.smoking && !g.On());
			CHECK(g.Update(22000, false, clock, p).cmd == Cmd::kNone);
		}
		// The frozen clock holds it still: run at ClockScale after the probe, once.
		{
			ts::ClockGovernor g;
			CHECK(g.Update(0, true, clock, p).cmd == Cmd::kLit);
			CHECK(g.Update(499, true, clock, p).cmd == Cmd::kNone);
			CHECK(g.Update(500, true, clock, p).cmd == Cmd::kRun && g.Runs() == 1);
			CHECK(g.Update(600, true, clock, p).cmd == Cmd::kNone && g.Runs() == 1);  // still not moving: asked once only
			CHECK(g.Update(700, false, clock + 60, p).cmd == Cmd::kOut);
			// A smoke shorter than the probe: lit and out, never run.
			CHECK(g.Update(1000, true, clock, p).cmd == Cmd::kLit && g.Update(1200, false, clock, p).cmd == Cmd::kOut && g.Runs() == 1);
		}
		// The stuck guard is time, not ticks: at ~60 Hz a 90 s smoke is ~5400 Tick calls and still a smoke.
		{
			ts::ClockGovernor g;
			std::uint64_t now = 0;
			bool          stuck = false, out = false;
			g.Update(now, true, clock, p);
			for (; now <= 90000; now += 16) {
				const auto o = g.Update(now, true, clock += 3, p);
				stuck = stuck || o.stuck;
				out = out || o.cmd == Cmd::kOut;
			}
			CHECK(!stuck && !out && g.On() && g.Stucks() == 0);
			// Over 120 s: stuck -> frozen again (kOut), ignored while it stays on...
			for (; now <= 121000 && !stuck; now += 16) {
				const auto o = g.Update(now, true, clock, p);
				stuck = o.stuck;
				out = o.cmd == Cmd::kOut;
				CHECK(!stuck || (!o.smoking && out));
			}
			CHECK(stuck && out && g.Stuck() && g.Stucks() == 1 && !g.On());
			CHECK(g.Update(now + 5000, true, clock, p).cmd == Cmd::kNone && !g.Update(now + 6000, true, clock, p).smoking);
			// ...until it drops; then a new smoke counts again.
			CHECK(g.Update(now + 7000, false, clock, p).cmd == Cmd::kNone && !g.Stuck());
			CHECK(g.Update(now + 8000, true, clock, p).cmd == Cmd::kLit && g.Stucks() == 1);
			// A load mid-smoke: over (the caller sends kOut); next time it starts afresh.
			CHECK(g.Reset() && !g.On() && !g.Reset());
			CHECK(g.Update(now + 9000, true, clock, p).cmd == Cmd::kLit);
		}
	}

	// ---- host: the pacer spreads a skip, eases in, never overshoots
	{
		ts::Pacer pacer;
		ts::Pacer::Params p;  // 1000 ms smooth, 100 ms steps, >= 60 s per step
		pacer.Add(6 * 3600);
		std::int64_t total = 0, firstStep = 0;
		int          steps = 0;
		std::uint64_t now = 0;
		for (; pacer.Pending() > 0 && steps < 1000; now += 16) {
			const auto s = pacer.Step(now, p);
			if (s > 0) {
				firstStep = firstStep ? firstStep : s;
				total += s;
				++steps;
			}
		}
		CHECK(total == 6 * 3600);
		CHECK(firstStep == 6 * 3600 / 10);  // a tenth of it in the first 100 ms step
		CHECK(steps > 10 && steps < 80);    // spread over a few seconds, not one jump
		CHECK(pacer.Step(now + 1000, p) == 0);
		// Small streamed skips go out at once (the minimum chunk), steps stay step_ms apart.
		pacer.Add(90);
		CHECK(pacer.Step(now + 2000, p) == 60 && pacer.Step(now + 2050, p) == 0 && pacer.Step(now + 2100, p) == 30 && pacer.Pending() == 0);
		// Whole days drop out; Clear drops everything; "at once" (smooth_ms 0 -> min chunk a day) is one write.
		pacer.Add(2 * ts::kDay + 120);
		CHECK(pacer.Pending() == 120);
		pacer.Clear();
		CHECK(pacer.Pending() == 0);
		ts::Pacer::Params once;
		once.smoothMs = 1;
		once.minChunkSec = ts::kDay;
		pacer.Add(5 * 3600);
		CHECK(pacer.Step(now + 5000, once) == 5 * 3600 && pacer.Pending() == 0);
	}
	// The cursor takes Elden Ring's reading only when it runs a little ahead (its own clock), never back to a stale one.
	CHECK(ts::Reconcile(12 * 3600, 12 * 3600 + 30) == 12 * 3600 + 30);
	CHECK(ts::Reconcile(12 * 3600, 11 * 3600) == 12 * 3600);                // the write hasn't landed: keep ours
	CHECK(ts::Reconcile(23 * 3600 + 59 * 60, 60) == 60);                    // ahead across midnight
	CHECK(ts::Reconcile(12 * 3600, 15 * 3600) == 12 * 3600);                // far ahead: not its own clock, keep ours

	// ---- the console command (no game: it queues, nothing is written outside a world)
	using namespace phantom::er;
	CHECK(TimeSkip::Command("abc").starts_with("timeskip ["));
	CHECK(TimeSkip::Command("0").starts_with("timeskip ["));
	CHECK(TimeSkip::Command("2000").starts_with("timeskip ["));
	CHECK(TimeSkip::Command("150").find("+2 h 30 min queued") != std::string::npos);
	CHECK(TimeSkip::Command("status").find("9000 s pending") != std::string::npos);
	Frame outside{};  // not in a world: the skip waits
	TimeSkip::Tick(outside);
	CHECK(TimeSkip::Describe().find("9000 s pending") != std::string::npos);
	CHECK(TimeSkip::Command("clear").find("dropped") != std::string::npos);
	CHECK(TimeSkip::Describe().find(" 0 s pending") != std::string::npos);
}
