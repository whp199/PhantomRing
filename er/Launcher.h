#pragma once
// Windows: Elden Ring starts MGSV (SkyCraft's Launcher.cpp model: Skyrim starts Minecraft). Under Linux
// `pr launch` does this (headless gamescope, SLR + Proton), so [Launch] StartMgsv = auto is off there.
//
// When the host plugin is up and no MGSV guest answers on the mapping, it
//  1. finds mgsvtpp.exe ([Launch] MgsvExe, else Steam's registry key -> libraryfolders.vdf ->
//     appmanifest_287700.acf, er/SteamLibrary.h);
//  2. installs/verifies the guest there (er/GuestInstall.h; never overwrites an unknown dinput8.dll);
//  3. starts it with PHANTOMRING=1 (the guest stays a plain dinput8 proxy without it), SteamAppId=287700
//     and the host's mapping, without taking the focus from Elden Ring (the guest then keeps its window
//     hidden, mgsv/WindowHidden.h), and watches it for an early exit (logged with the exit code).
// MGSV quits by itself a while after Elden Ring is gone ([Window] QuitWithoutHostSec), and a running
// guest is reused by the next Elden Ring.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace phantom::er::Launcher
{
	inline constexpr std::uint32_t kMgsvAppId = 287700;

	struct Settings
	{
		std::string  startMgsv{ "auto" };  // auto (Windows only) | true | false
		std::wstring mgsvExe;              // empty = through Steam
		std::wstring mgsvArgs;
		bool         installGuest{ true };
		int          waitGuestMs{ 3000 };  // a guest already running answers within this
		std::wstring exeName{ L"mgsvtpp.exe" };  // not in the ini: the offline test's stand-in has its own name

		bool operator==(const Settings&) const = default;
	};
	Settings Load(const std::wstring& a_iniPath);  // [Launch] of PhantomRingHost.ini

	// After Link::Open (a_mapping: the host's resolved mapping, passed on to MGSV). Works on its own thread.
	void Start(const Settings& a_settings, const std::wstring& a_mapping);
	std::string Describe();

	// ---- pieces (tests/test_windows.cpp) ----------------------------------------------------------
	// A CreateProcess environment block: a_current (a GetEnvironmentStringsW block) with a_set applied
	// (names case-insensitive, replacing existing ones), sorted case-insensitively, double-NUL terminated.
	std::wstring BuildEnvironment(const wchar_t* a_current, const std::vector<std::pair<std::wstring, std::wstring>>& a_set);
	// The variables MGSV gets.
	std::vector<std::pair<std::wstring, std::wstring>> GuestEnvironment(const std::wstring& a_mapping);
	// Starts a_exe in a_dir without activating it; a_pid/a_process receive its id/handle (the caller closes
	// the handle). Breaks away from Elden Ring's job when allowed, so MGSV can outlive a quick ER restart.
	bool StartProcess(const std::wstring& a_exe, const std::wstring& a_args, const std::wstring& a_dir, const std::wstring& a_environment,
		std::uint32_t* a_pid, void** a_process, std::string* a_why);
	// Process ids of running processes named a_exeName (case-insensitive).
	std::vector<std::uint32_t> FindProcesses(const std::wstring& a_exeName);
}
