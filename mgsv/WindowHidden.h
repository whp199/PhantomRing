#pragma once
// Hidden MGSV on Windows (docs/DESIGN.md §14b, docs/WINDOWS.md). Under Linux `pr launch` runs MGSV inside
// headless gamescope: it really has the focus there and nothing reaches the desktop. On Windows both games
// share one desktop, so the guest does it itself, all behind [Window] Hidden (auto = on when not under
// Wine/gamescope, common/Platform.h):
//  - the game window stays hidden (never minimised), off the taskbar, never active, and believes it has
//    the focus (mgsv/WindowGuard.h);
//  - MGSV's per-view visibility check (0x14024A1D0, IsWindow && !IsIconic && IsWindowVisible) answers
//    "visible" while the window exists: a hidden window would otherwise skip the scene draw, and the
//    capture (phase 2) would have nothing to send;
//  - the swap chain is created windowed (exclusive fullscreen on a hidden window would fight Elden Ring);
//  - MGSV quits by itself once no Elden Ring has been alive for QuitWithoutHostSec: a hidden process
//    has no window to close.

#include <cstdint>
#include <string>

namespace phantom::guest::hidden
{
	struct Settings
	{
		std::string hidden{ "auto" };      // auto | true | false
		bool        spoofFocus{ true };
		bool        forceRender{ true };
		bool        forceWindowed{ true };
		int         quitWithoutHostSec{ 120 };  // 0 = never

		bool operator==(const Settings&) const = default;
	};

	// [Window] of PhantomRingGuest.ini (missing keys keep the defaults above).
	Settings Load(const std::wstring& a_iniPath);

	// From DllMain, after the identity checks and Hooks::Init: decides hidden mode and, when on, installs
	// the user32 hooks (WindowGuard, guard included), the visibility-check detour and the quit watcher.
	// MGSV's window thread starts right after DllMain, so this can't wait for late init.
	// a_hostAlive: the link's view of Elden Ring (nullptr: never quit).
	void Start(const Settings& a_settings, bool (*a_hostAlive)());

	[[nodiscard]] bool Active();          // hidden mode is on
	[[nodiscard]] bool ForceWindowed();   // D3D11Hooks: make the swap chain windowed
	std::string        Describe();

	// Guest.cpp, after the save guards: true only when MGSV's saves really are blocked ([Saves] Block and
	// the file guards installed). Until then (and by default) the quit watcher treats saves as live.
	void SetSavesBlocked(bool a_blocked);

	// Pure: quit once the host has been gone (or never came) for longer than a_quitSec.
	bool ShouldQuit(std::int64_t a_nowMs, std::int64_t a_lastHostMs, int a_quitSec);

	// Pure: how to quit once ShouldQuit says so. Saves blocked: nothing MGSV holds is worth keeping, so a
	// hard kill. Saves live: ask MGSV to close itself (a_closeAskedMs 0 = not asked yet) so a queued save
	// can finish; kill only if it still runs a_graceSec after asking.
	enum class QuitStep
	{
		kKill,
		kAskClose,
		kWait,
	};
	QuitStep NextQuitStep(bool a_savesBlocked, std::int64_t a_nowMs, std::int64_t a_closeAskedMs, int a_graceSec);
}
