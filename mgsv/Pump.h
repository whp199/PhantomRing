#pragma once
// Everything that runs on MGSV's Lua thread (via the lua_pcall depth-1 hook): loading our script,
// the dev console, the per-update PhantomRing.Tick and the boot automation that steers MGSV from its
// title screen into the sandbox mission with the buddy deployed.

#include <cstdint>
#include <string>

namespace phantom::guest::pump
{
	void Run(void* a_L);
	std::string Describe();  // '!status'

#ifdef PHANTOM_GUEST_TESTING
	// tests/test_guest_pump.cpp: the pump's clock (GetTickCount64) jumps a_ms ahead, so the buddy's 10-30 s windows pass at once.
	void          SkipClockForTests(std::uint64_t a_ms);
	std::uint64_t NowForTests();  // the clock the pump runs on: GetTickCount64 + every skip so far
#endif
}
