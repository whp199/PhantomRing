#pragma once

// The test virtual pad (console `input virtualpad on|exclusive|shared|off`, er/InputBridge.cpp): what one XInput poll returns.
// Pure: tests/test_host_chord.inc (TestVirtualPad).
//
//  - kShared (the old `virtualpad on`, and what [Keyboard] virtual_pad turns on for a player with no controller): user 0
//    reports a connected neutral gamepad only when no real one answers there; a real pad works as usual.
//  - kExclusive (`virtualpad on` / `exclusive` since 0.6.0: test automation): every real XInput pad is ignored completely --
//    its sticks, triggers and buttons reach neither Elden Ring nor MGSV. User 0 is the neutral virtual pad (console `press`
//    and the keyboard ([Keyboard]) still drive it), users 1-3 report no controller. Found 2026-10-31: the player's DualSense,
//    plugged in with its left stick resting pushed, walked Snake around, opened Elden Ring's menus and talked to Renna in the
//    middle of a scripted run, because a `press` only adds its buttons (and a non-zero stick) to the real pad.

#include <cstdint>

namespace phantom::er
{
	enum class VirtualPadMode : std::uint8_t
	{
		kOff,
		kShared,
		kExclusive,
	};

	enum class PadPoll : std::uint8_t
	{
		kReal,          // what the real XInput call returned, unchanged
		kVirtual,       // a connected, neutral gamepad (the real one's state, if any, is dropped)
		kDisconnected,  // ERROR_DEVICE_NOT_CONNECTED (a real pad there is hidden)
	};

	// XInput user a_index was polled; a_realConnected: the real call succeeded.
	constexpr PadPoll VirtualPadPoll(VirtualPadMode a_mode, std::uint32_t a_index, bool a_realConnected)
	{
		switch (a_mode) {
		case VirtualPadMode::kExclusive:
			return a_index == 0 ? PadPoll::kVirtual : PadPoll::kDisconnected;
		case VirtualPadMode::kShared:
			return a_index == 0 && !a_realConnected ? PadPoll::kVirtual : PadPoll::kReal;
		default:
			return PadPoll::kReal;
		}
	}

	constexpr const char* VirtualPadName(VirtualPadMode a_mode)
	{
		return a_mode == VirtualPadMode::kExclusive ? "exclusive" : a_mode == VirtualPadMode::kShared ? "shared" : "off";
	}
}
