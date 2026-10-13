#pragma once
// Windows or Wine? PhantomRing's two plugins are the same DLLs on both; only the glue differs
// (docs/DESIGN.md §14b, docs/WINDOWS.md):
//
//   | piece          | Wine / Proton (Linux)                         | Windows                                  |
//   |----------------|-----------------------------------------------|------------------------------------------|
//   | shared memory  | file on /dev/shm (each game has its own        | named mapping Local\PhantomRing_v1       |
//   |                | prefix and wineserver: names don't cross)      | (one kernel, one session)                |
//   | hiding MGSV    | headless gamescope (`pr launch`)               | the guest hides its own window           |
//   | starting MGSV  | `pr launch`                                    | the host plugin (er/Launcher)            |
//
// The decisions are pure functions of a few facts (unit-tested in tests/test_windows.cpp); Detect()
// gathers the facts once per process.

#include <string>
#include <string_view>

namespace phantom::platform
{
	inline constexpr wchar_t kWineMapping[] = L"file:Z:\\dev\\shm\\phantomring_v1";  // == proto::kDefaultMapping
	inline constexpr wchar_t kWindowsMapping[] = L"name:Local\\PhantomRing_v1";

	struct Facts
	{
		bool wine{ false };       // ntdll exports wine_get_version
		bool gamescope{ false };  // started inside gamescope (GAMESCOPE_WAYLAND_DISPLAY in the environment)
	};

	// This process (cached). PHANTOMRING_PLATFORM=windows|wine overrides `wine` for offline tests only:
	// it lets the Windows paths run under system wine without a Windows machine.
	const Facts& Detect();

	// The mapping both plugins name when the ini leaves it empty (or says "auto").
	std::wstring DefaultMapping(bool a_wine);
	// Precedence: the launcher's PHANTOMRING_MAPPING (a_env, may be empty) > the ini value > the platform
	// default. Values without a "file:"/"name:" prefix are taken as a kernel object name, as Mapping does.
	std::wstring ResolveMapping(std::wstring_view a_ini, std::wstring_view a_env, bool a_wine);

	// "auto" | true-ish | false-ish (Ini::GetBool spellings). auto = a_autoValue.
	bool Switch(std::string_view a_setting, bool a_autoValue);

	// [Window] Hidden: auto = the guest hides MGSV itself only where nothing else does (not under Wine,
	// where `pr launch` puts MGSV in headless gamescope, and never inside gamescope).
	bool HideGuestWindow(std::string_view a_setting, const Facts& a_facts);
	// [Launch] StartMgsv: auto = the host starts MGSV only on Windows (`pr launch` does it under Wine).
	bool HostStartsGuest(std::string_view a_setting, const Facts& a_facts);
}
