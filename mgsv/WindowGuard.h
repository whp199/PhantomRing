#pragma once
// Every user32 detour of the guest lives here (MinHook allows one detour per function).
//
// Guard (both platforms, [Input] GuardWindow): MGSV must never minimise (its renderer skips iconic
// windows and a watcher thread posts suspend events, research §8.5), warp the cursor, or pull itself to
// the foreground. Under Linux it runs inside headless gamescope with real focus, so that is all it needs.
//
// Hidden (Windows, [Window] Hidden, mgsv/WindowHidden.h): MGSV shares the desktop with Elden Ring, so the
// guest keeps its game window hidden (SW_HIDE, never minimised: hidden still renders once the visibility
// check is forced, minimised suspends), off the taskbar and Alt-Tab (WS_EX_TOOLWINDOW), never active, and
// tells MGSV it has the focus (GetForegroundWindow/GetActiveWindow/GetFocus answer its own window; the
// window proc never sees WM_ACTIVATEAPP false / WM_KILLFOCUS). "The game window" = the first top-level
// window whose class belongs to the exe module (CreateWindowExW at 0x1400706D6, research §8.3).

#include <windows.h>

#include <string>

namespace phantom::guest::window
{
	struct Options
	{
		bool guard{ true };       // no minimise / foreground grab / cursor warp
		bool hidden{ false };     // hidden mode (implies guard)
		bool spoofFocus{ true };  // hidden mode: focus queries answer MGSV's own window
	};

	// Creates the hooks (enabled with the others by Hooks::EnableAll). Safe from DllMain: user32 is one of
	// this DLL's own imports, so it is loaded and initialised before us. An already existing game window
	// is adopted (hidden, restyled, subclassed) at once.
	bool Install(const Options& a_options);

	[[nodiscard]] bool Hidden();
	[[nodiscard]] HWND GameWindow();  // nullptr until MGSV created it
	std::string        Describe();    // for !status / !window

	// Pure decisions, unit-tested (tests/test_guest_window.cpp).
	DWORD HiddenStyle(DWORD a_style);      // WS_VISIBLE off
	DWORD HiddenExStyle(DWORD a_exStyle);  // WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE on, WS_EX_APPWINDOW off
	bool  IsMinimise(int a_show);
	// The window proc message the hidden window must not see (focus loss); true = swallow.
	bool SwallowFocusLoss(UINT a_msg, WPARAM a_w);
}
