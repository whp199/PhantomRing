#pragma once
// The Phantom Cigar moves Elden Ring's clock (roadmap 2.7, protocol v13 kGEvTimeSkip; [Cigar] TimeSkip, OFF by default until
// the live test shows which signal MGSV raises while he smokes: it changes Elden Ring's saved time of day; UNVERIFIED live).
// In MGSV the cigar is a time-lapse: Snake smokes, the day races by. The embedded Lua (GuestScript.cpp PR.Flags) reports the
// smoking signal (state::kLCigar); here it is debounced against a signal that sticks (ClockGovernor, in milliseconds), the
// sandbox's frozen clock ([Boot] FreezeClock) is let run only if MGSV's own time-lapse doesn't move it (Lua PR.CigarClock acts),
// and MGSV's clock (vars.clock, seconds of the day, read natively) is watched meanwhile: the game minutes that pass go to Elden
// Ring as kGEvTimeSkip -- a few each quarter second while the time-lapse runs, so Elden Ring's sky can follow it (the host
// spreads each one, er/TimeSkip.h). The arithmetic is common/TimeSkipLogic.h (Tracker, ClockGovernor).

#include <cstdint>
#include <string>

namespace phantom::guest::cigar
{
	// MGSV's Lua thread, after every PhantomRing.Tick (state::RunLuaTickWork: MGSV's frame rate, ~60 Hz, or ~30 Hz without frames).
	void        Tick(std::uint64_t a_nowMs, bool a_ready);
	std::string Describe();  // `!cigar` and `!status`
}
