#pragma once

// MGSV's Phantom Cigar moves Elden Ring's clock (roadmap 2.7, protocol v13 kGEvTimeSkip; [TimeSkip], on by default; UNVERIFIED
// live). In MGSV the cigar is a time-lapse: Snake smokes, the sky races. The guest (mgsv/Cigar.cpp) lets MGSV's frozen sandbox
// clock run while he smokes and streams the game minutes that pass; here the same game time passes in Elden Ring.
//
// Elden Ring's clock is WorldAreaTime: writing its minute (+0x2C) and second (+0x30), then its hour (+0x28) sets the time (the
// hour write applies all three; VERIFIED live 2026-10-12 with `poke`, docs/HOST.md "Lighting from Elden Ring"). A skip is not
// written at once: phantom::timeskip::Pacer (common/TimeSkipLogic.h) spreads it over [TimeSkip] smooth_ms in step_ms steps,
// each step a write of the clock a little further on, so Elden Ring's sky moves with MGSV's time-lapse instead of jumping. Our
// own cursor carries the time between writes (a write may land a frame later), and takes Elden Ring's reading when that runs
// a little ahead.
// Nothing is written during a load or before the world is in (the skip waits).

#include <string>
#include <string_view>

namespace phantom::proto
{
	struct GuestEvent;
}

namespace phantom::er
{
	struct Frame;

	namespace TimeSkip
	{
		void        OnEvent(const proto::GuestEvent& a_event);  // kGEvTimeSkip: a = MGSV game minutes
		void        Tick(const Frame& a_frame);                 // FrameBegin, after the events (the game thread)
		std::string Describe();
		// `timeskip [status | <minutes> | clear]`: a skip as the guest would send it (tests without MGSV), or drop what is pending.
		std::string Command(std::string_view a_args);
	}
}
