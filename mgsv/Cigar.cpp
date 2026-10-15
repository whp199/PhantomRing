#include "Cigar.h"

#include "Config.h"
#include "Link.h"
#include "Log.h"
#include "State.h"
#include "TimeSkipLogic.h"
#include "Vars.h"

#include "../protocol/phantomring_protocol.h"

#include <format>
#include <mutex>

namespace phantom::guest::cigar
{
	namespace
	{
		namespace ts = phantom::timeskip;

		std::mutex        g_lock;  // Tick (Lua thread) vs Describe (the console worker)
		ts::Tracker       g_tracker;
		ts::ClockGovernor g_governor;  // when the sandbox's frozen clock runs (the Lua acts: PR.CigarClock)
		std::int32_t  g_clock{ -1 };  // MGSV's clock at the last tick (seconds of the day), -1 unknown
		bool          g_smoking{ false };
		std::uint64_t g_smokes{ 0 }, g_events{ 0 }, g_lost{ 0 };

		std::string Clock(std::int32_t a_sec)
		{
			const auto t = ts::ToHms(a_sec);
			return std::format("{:02}:{:02}:{:02}", t.h, t.m, t.s);
		}

		void Send(std::uint32_t a_minutes)
		{
			proto::GuestEvent e{};
			e.type = proto::kGEvTimeSkip;
			e.a = a_minutes;
			e.frame = static_cast<std::uint32_t>(state::Frame().frame);
			if (link::PushEvent(e)) {
				++g_events;
				Log::Debug("cigar: kGEvTimeSkip +{} min (MGSV {})", a_minutes, Clock(g_clock));
			} else if (g_lost++ < 5) {
				// No host attached (or its ring is full): Elden Ring can't follow these minutes; MGSV's clock has moved anyway.
				Log::Warn("cigar: +{} min not sent (no host listening)", a_minutes);
			}
		}

		// The governor's word to the Lua (GuestScript.cpp PR.CigarClock), run at the next PhantomRing.Tick.
		void Command(ts::ClockGovernor::Cmd a_cmd)
		{
			using Cmd = ts::ClockGovernor::Cmd;
			switch (a_cmd) {
			case Cmd::kLit:
				state::QueueCommand("cigar lit");
				break;
			case Cmd::kRun:
				Log::Info("cigar: MGSV's clock stood still {} ms into the smoke: it runs at [Cigar] ClockScale x{}", ts::GovernorParams{}.probeMs,
					Cfg().cigarClockScale);
				state::QueueCommand("cigar run");
				break;
			case Cmd::kOut:
				state::QueueCommand("cigar out");
				break;
			case Cmd::kNone:
				break;
			}
		}
	}

	void Tick(std::uint64_t a_nowMs, bool a_ready)
	{
		if (!Cfg().cigarTimeSkip) {
			return;
		}
		const auto lua = state::Lua();
		const auto v = vars::Read();
		std::scoped_lock lock(g_lock);
		// Outside a loaded sandbox (a load, the title, a mission change) nothing counts: MGSV's clock jumps there by itself.
		if (!v.valid || !lua.InGame()) {
			if (g_tracker.Active()) {
				Log::Info("cigar: the smoke ended with a load (+{} s of MGSV time counted)", g_tracker.SessionSeconds());
			}
			g_tracker.Reset();
			if (g_governor.Reset()) {
				Command(ts::ClockGovernor::Cmd::kOut);
			}
			g_clock = -1;
			g_smoking = false;
			return;
		}
		g_clock = ts::Wrap(v.clock);
		const auto gov = g_governor.Update(a_nowMs, (lua.flags & state::kLCigar) != 0, v.clock);
		if (gov.stuck) {
			Log::Warn("cigar: the smoking signal has been on for over {} s: taken as stuck (or not a smoke), MGSV's clock frozen again, "
				"the signal ignored until it drops", ts::GovernorParams{}.maxSmokeMs / 1000);
		}
		Command(gov.cmd);
		g_smoking = gov.smoking;
		const auto out = g_tracker.Update(a_nowMs, v.clock, g_smoking);
		if (out.started) {
			++g_smokes;
			Log::Info("cigar: Snake lights the Phantom Cigar (MGSV's clock {}, vars.clock {}){}", Clock(g_tracker.StartClock()), v.clock,
				a_ready ? "" : " -- the sandbox isn't ready: Elden Ring may not follow");
		}
		if (out.minutes) {
			Send(out.minutes);
		}
		if (out.ended) {
			const auto sec = g_tracker.SessionSeconds();
			Log::Info("cigar: put out -- MGSV's clock {} -> {}: +{} min of game time for Elden Ring{}", Clock(g_tracker.StartClock()), Clock(g_clock),
				(sec + 30) / 60, sec == 0 ? " (MGSV's clock never moved, even let run: see [Cigar] ClockScale)" : "");
		}
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		if (!Cfg().cigarTimeSkip) {
			return "phantom cigar: off ([Cigar] TimeSkip = false)";
		}
		// "clock moved by MGSV itself" vs "run by the guest": which of the two the cigar needs (ClockGovernor, UNVERIFIED live)
		return std::format("phantom cigar: {} | MGSV's clock {} | {} smokes (clock moved by MGSV itself {}, run by the guest at x{} {}, "
			"stuck signals {}{}), {} events, {} min sent, {} lost, {} implausible clock steps ignored{}",
			g_smoking ? "SMOKING" : (g_governor.Stuck() ? "signal STUCK (ignored)" : (g_tracker.Active() ? "just put out" : "not smoking")),
			g_clock < 0 ? std::string("unknown") : Clock(g_clock), g_smokes, g_governor.SelfRuns(), Cfg().cigarClockScale, g_governor.Runs(),
			g_governor.Stucks(), g_governor.Stuck() ? ", now" : "", g_events, g_tracker.SentMinutes(), g_lost, g_tracker.Ignored(),
			g_tracker.Active() ? std::format(" | this smoke: from {}, +{} s", Clock(g_tracker.StartClock()), g_tracker.SessionSeconds()) : std::string());
	}
}
