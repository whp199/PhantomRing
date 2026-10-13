#pragma once
// PhantomRing's MGSV guest: activation and the three places it runs.
//
//  DllMain            OnProcessAttach: decide (PHANTOMRING=1 + exe identity), read the ini, create the
//                     code hooks (Lua pump, camera, skeleton, save guards) and start the link worker.
//  first DirectInput8Create (or 5 s later)
//                     LateInit: the XInput IAT patch and the window guards (needs the IAT bound and
//                     user32 loaded).
//  game threads       the Lua pump (script, console, per-update Tick, boot automation) and the player
//                     camera frame (motion, life, shots, GuestState) -- Guest.cpp.

#include <windows.h>

namespace phantom::guest
{
	void OnProcessAttach(HMODULE a_self);
	void LateInit();
	[[nodiscard]] bool Active();
}
