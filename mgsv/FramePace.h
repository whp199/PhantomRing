#pragma once
// MGSV's frame pacing (docs/GUEST.md 19). MGSV paces itself to 60 frames/s and advances its game by one fixed step per
// frame (a constant pan turns the camera exactly 0.288 deg per frame whatever the frame took). Hidden in gamescope next to
// Elden Ring it ran 51-58 frames/s (VERIFIED live 2026-10-12): not vsync (presenting with interval 0 changed nothing), not
// the GPU (Elden Ring stopped: 19.7 -> 18.3 ms), not a busy thread (none above 34% of a core) -- its job system's ~25 threads
// poll for work with Sleep(1), 1.06 ms under Wine, and the hand-offs add up. So Snake ran 85-95% of real speed.
//
// - MGSV's own Sleep(1) (its kernel32 import) becomes a delay of [Pacing] JobSleepUs microseconds (live, 500 us: idle
//   57.7 -> 60.0 frames/s, panning 50.5 -> 53.7; 250 was faster still but brought 77-168 ms hitches).
// - `!pace start [all]` / `!pace`: the game thread's frame interval, the time in Present and in our own Present work, and
//   with [Pacing] WaitProbe where MGSV's threads wait (ntdll's delay, object and condition-variable waits, by call chain;
//   only while recording); the full list goes to PhantomRingGuest.pace.txt.
// - Experiments: `!pace sync <n|game>` overrides the present sync interval; `!pace limit <hz|off> [grid <phase ms>]` waits
//   in Present for a 60 Hz slot, optionally on Elden Ring's frame grid (its HostState stamps). Live: one new MGSV frame on
//   99.5% of Elden Ring's frames, but MGSV's render then trails its game by 2-3 frames (Snake's poses 2-3 frames behind
//   his camera), and a frame that misses its slot waits a whole period: off by default.

#include <cstdint>
#include <string>
#include <string_view>

namespace phantom::guest::framepace
{
	// Late init: MGSV's Sleep import through us (import-checked; refused = MGSV's own); with [Pacing] WaitProbe detours on
	// ntdll's NtDelayExecution / NtWaitForSingleObject / NtWaitForMultipleObjects / NtWaitForAlertByThreadId too.
	bool Install();
	// The player-camera frame (game thread): marks the game thread and its frame boundary.
	void OnGameFrame(std::int64_t a_qpc);
	// Right before the original IDXGISwapChain::Present (render thread): returns the sync interval to present with
	// (MGSV's own unless overridden) and, when the limiter is on, waits for this frame's slot first.
	unsigned BeforePresent(unsigned a_syncInterval);
	// Around the original IDXGISwapChain::Present (render thread); a_ours = QPC our own Present work took before it.
	void OnPresent(std::int64_t a_begin, std::int64_t a_end, std::int64_t a_ours, unsigned a_syncInterval);
	std::string Command(std::string_view a_args);
}
