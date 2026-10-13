// Windows players: both plugins' real link code talking through the named mapping, in two processes of
// one Wine prefix (the way they share one kernel on Windows). Run with PHANTOMRING_PLATFORM=windows:
//   PHANTOMRING_PLATFORM=windows wine test_windows_link.exe
// The parent is the host (er/Link) and starts the guest (this exe, "guest") through er/Launcher's
// StartProcess with the guest environment, *before* it creates the mapping (the guest must wait for it).
// Then: attach, GuestState, pad -> guest -> event back, host event -> guest -> event back, a host restart
// (new session, re-attach), and a clean exit of the guest on request.
// Second, er/Launcher end to end: a fake Steam (registry SteamPath of this scratch prefix -> a library with
// appmanifest_287700.acf and MGS_TPP\<stand-in>.exe = a copy of this exe; never named mgsvtpp.exe, so the Linux
// tools' process scans can't mistake it for the real MGSV) and a fake package (host.exe = this
// exe, mgsv\dinput8.dll + ini). host.exe runs Launcher::Start as PhantomRingHost does: it must find the
// "MGSV", install the guest files, start it with the guest environment and see its guest answer; a second
// Start (Elden Ring restarted) must reuse the running guest.
#include "Launcher.h"
#include "Link.h"
#include "Log.h"
#include "Mapping.h"
#include "Platform.h"

#include "../mgsv/Link.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

using namespace phantom;
using namespace phantom::proto;

static int g_failures = 0;
#define CHECK(x)                                                              \
	do {                                                                      \
		if (!(x)) {                                                           \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);           \
			++g_failures;                                                     \
		}                                                                     \
	} while (0)

static constexpr std::uint32_t kQuit = 999;
static constexpr wchar_t       kStandIn[] = L"phantomring_test_mgsv.exe";  // plays mgsvtpp.exe

static std::wstring Env(const wchar_t* a_name)
{
	wchar_t    buf[512];
	const auto n = ::GetEnvironmentVariableW(a_name, buf, 512);
	return n > 0 && n < 512 ? std::wstring(buf, n) : std::wstring();
}

// ---- the guest process --------------------------------------------------------------------------
static int Guest()
{
	Log::Init(L"test_windows_link_guest.log", "guest");
	if (Env(L"PHANTOMRING") != L"1") {
		return 20;
	}
	// As mgsv/Config + Guest.cpp resolve it: the launcher's PHANTOMRING_MAPPING wins.
	const auto mapping = platform::ResolveMapping(L"", Env(L"PHANTOMRING_MAPPING"), platform::Detect().wine);
	if (!mapping.starts_with(L"name:")) {
		return 21;
	}
	guest::link::SetStatus(Status::kReady, "test guest ready");
	guest::link::Start(mapping);
	std::uint64_t frame = 0;
	bool          padEchoed = false;
	const auto    start = ::GetTickCount64();
	while (::GetTickCount64() - start < 60000) {
		guest::link::NoteGameTick();
		if (guest::link::Attached()) {
			GuestState s{};
			s.flags = kGuestReady;
			s.frame = ++frame;
			guest::link::PublishState(s);
			PadState pad{};
			if (guest::link::ReadPad(pad) && pad.buttons == 0x1000 && !padEchoed) {
				GuestEvent e{};
				e.type = kGEvFire;
				e.a = pad.buttons;
				padEchoed = guest::link::PushEvent(e);
			}
			HostEvent he{};
			while (guest::link::PopHostEvent(he)) {
				if (he.type == kHEvStatus && he.a == kQuit) {
					return guest::link::AttachGeneration() >= he.b ? 0 : 22;  // b: the attaches the host saw
				}
				GuestEvent e{};
				e.type = kGEvRumble;
				e.a = he.a;
				guest::link::PushEvent(e);
			}
		}
		::Sleep(16);
	}
	return 23;  // never told to quit
}

// ---- the host process ---------------------------------------------------------------------------
static bool Until(const std::function<bool()>& a_done, int a_ms = 15000)
{
	const auto start = ::GetTickCount64();
	while (::GetTickCount64() - start < static_cast<std::uint64_t>(a_ms)) {
		er::Link::Get().Tick();
		if (a_done()) {
			return true;
		}
		::Sleep(16);
	}
	return false;
}

namespace fs = std::filesystem;

static void Write(const fs::path& a_path, std::string_view a_text)
{
	fs::create_directories(a_path.parent_path());
	std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
	out << a_text;
}

// host.exe in the fake package: what PhantomRingHost's Init does around the launcher.
static int LauncherHost()
{
	Log::Init(L"test_windows_link_launcher.log", "host");
	const auto mapping = platform::ResolveMapping(L"", L"", platform::Detect().wine);
	auto&      link = er::Link::Get();
	if (!link.Open(mapping)) {
		return 30;
	}
	er::Launcher::Settings settings;
	settings.waitGuestMs = 500;
	settings.exeName = kStandIn;
	er::Launcher::Start(settings, mapping);
	if (!Until([&] { return er::Launcher::Describe().find("guest answered") != std::string::npos; }, 30000)) {
		std::printf("launcher host: %s\n", er::Launcher::Describe().c_str());
		return 31;
	}
	if (er::Launcher::FindProcesses(kStandIn).empty()) {
		return 32;
	}
	er::Launcher::Start(settings, mapping);  // Elden Ring restarted: the running guest is reused
	if (!Until([&] { return er::Launcher::Describe().find("already running") != std::string::npos; }, 10000)) {
		std::printf("launcher host (2nd): %s\n", er::Launcher::Describe().c_str());
		return 33;
	}
	HostEvent quit{};
	quit.type = kHEvStatus;
	quit.a = kQuit;
	quit.b = 1;
	link.PushHostEvent(quit);
	if (!Until([&] { return er::Launcher::FindProcesses(kStandIn).empty(); }, 15000)) {
		return 34;
	}
	return 0;
}

static void TestLauncherEndToEnd(const wchar_t* a_self)
{
	const auto root = fs::current_path() / "test_windows_launch";
	fs::remove_all(root);
	const auto steam = root / "Steam";
	const auto mgsv = steam / "steamapps" / "common" / "MGS_TPP";
	const auto pkg = root / "PhantomRing";
	Write(steam / "steamapps" / "libraryfolders.vdf", "\"libraryfolders\"\n{\n}\n");
	Write(steam / "steamapps" / "appmanifest_287700.acf", "\"AppState\"\n{\n\t\"appid\"\t\t\"287700\"\n\t\"installdir\"\t\t\"MGS_TPP\"\n}\n");
	Write(pkg / "mgsv" / "dinput8.dll", "stand-in guest dll");
	Write(pkg / "mgsv" / "PhantomRingGuest.ini", "[Window]\nHidden = auto\n");
	fs::create_directories(mgsv);
	fs::copy_file(a_self, mgsv / kStandIn);
	fs::copy_file(a_self, pkg / "host.exe");

	// This scratch prefix's Steam key points at the fake Steam (restored afterwards).
	HKEY key = nullptr;
	CHECK(::RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS);
	wchar_t    previous[1024]{};
	DWORD      previousBytes = sizeof(previous);
	const bool hadPrevious = key && ::RegQueryValueExW(key, L"SteamPath", nullptr, nullptr, reinterpret_cast<BYTE*>(previous), &previousBytes) == ERROR_SUCCESS;
	auto       steamPath = steam.wstring();
	std::ranges::replace(steamPath, L'\\', L'/');  // Steam writes it with forward slashes
	if (key) {
		::RegSetValueExW(key, L"SteamPath", 0, REG_SZ, reinterpret_cast<const BYTE*>(steamPath.c_str()), static_cast<DWORD>((steamPath.size() + 1) * sizeof(wchar_t)));
	}

	std::uint32_t pid = 0;
	void*         process = nullptr;
	std::string   why;
	CHECK(er::Launcher::StartProcess((pkg / "host.exe").wstring(), L"", pkg.wstring(), er::Launcher::BuildEnvironment(nullptr, { { L"PHANTOMRING_ROLE", L"launcher-host" } }),
		&pid, &process, &why));
	DWORD code = 99;
	if (process) {
		CHECK(::WaitForSingleObject(static_cast<HANDLE>(process), 90000) == WAIT_OBJECT_0);
		::GetExitCodeProcess(static_cast<HANDLE>(process), &code);
		::CloseHandle(static_cast<HANDLE>(process));
	}
	CHECK(code == 0);
	if (code != 0) {
		std::printf("launcher host exit code %lu (log: %s)\n", code, (pkg / "test_windows_link_launcher.log").string().c_str());
	}
	// The guest files went into "MGSV's folder", recorded as ours.
	{
		std::ifstream dll(mgsv / "dinput8.dll", std::ios::binary);
		CHECK(std::string(std::istreambuf_iterator<char>(dll), {}) == "stand-in guest dll");
	}
	CHECK(fs::exists(mgsv / "PhantomRingGuest.ini") && fs::exists(mgsv / "PhantomRingGuest.installed"));

	if (key) {
		if (hadPrevious) {
			::RegSetValueExW(key, L"SteamPath", 0, REG_SZ, reinterpret_cast<const BYTE*>(previous), previousBytes);
		} else {
			::RegDeleteValueW(key, L"SteamPath");
		}
		::RegCloseKey(key);
	}
	// Keep both sides' logs next to the test, then clean up.
	std::error_code ec;
	fs::copy_file(pkg / "test_windows_link_launcher.log", "test_windows_link_launcher.log", fs::copy_options::overwrite_existing, ec);
	fs::copy_file(mgsv / "test_windows_link_guest.log", "test_windows_link_launched_guest.log", fs::copy_options::overwrite_existing, ec);
	if (code == 0) {
		fs::remove_all(root, ec);
	}
}

int main()
{
	if (Env(L"PHANTOMRING_ROLE") == L"guest") {
		return Guest();
	}
	if (Env(L"PHANTOMRING_ROLE") == L"launcher-host") {
		return LauncherHost();
	}
	Log::Init(L"test_windows_link_host.log", "host");
	CHECK(!platform::Detect().wine);  // PHANTOMRING_PLATFORM=windows
	const auto mapping = platform::ResolveMapping(L"", L"", platform::Detect().wine);
	CHECK(mapping == L"name:Local\\PhantomRing_v1");

	// The guest first: on Windows MGSV may be started (or still run) before Elden Ring creates the mapping.
	wchar_t self[MAX_PATH];
	::GetModuleFileNameW(nullptr, self, MAX_PATH);
	std::uint32_t pid = 0;
	void*         process = nullptr;
	std::string   why;
	CHECK(er::Launcher::StartProcess(self, L"", std::filesystem::current_path().wstring(),
		er::Launcher::BuildEnvironment(nullptr, er::Launcher::GuestEnvironment(mapping)), &pid, &process, &why));
	if (!process) {
		std::printf("test_windows_link: could not start the guest: %s\n", why.c_str());
		return 1;
	}
	::Sleep(1500);

	auto& link = er::Link::Get();
	CHECK(link.Open(mapping));
	CHECK(Until([&] { return link.GuestAlive() && link.AttachCount() == 1; }));
	GuestState gs{};
	CHECK(Until([&] { return link.ReadGuestState(gs) && gs.frame > 0 && (gs.flags & kGuestReady); }));
	CHECK(link.GuestStatus() == Status::kReady && link.GuestStatusText() == "test guest ready");

	// Pad and events, both ways.
	PadState pad{};
	pad.flags = kPadConnected;
	pad.packet = 1;
	pad.buttons = 0x1000;
	link.WritePad(pad);
	HostEvent hurt{};
	hurt.type = kHEvHurt;
	hurt.a = 77;
	CHECK(link.PushHostEvent(hurt));
	bool sawFire = false, sawHurtEcho = false;
	CHECK(Until([&] {
		GuestEvent e{};
		while (link.PopGuestEvent(e)) {
			sawFire |= e.type == kGEvFire && e.a == 0x1000;
			sawHurtEcho |= e.type == kGEvRumble && e.a == 77;
		}
		return sawFire && sawHurtEcho;
	}));

	// Elden Ring restarts (new session on the same named mapping): the guest re-attaches.
	CHECK(link.Open(mapping));
	CHECK(Until([&] { return link.GuestAlive() && link.AttachCount() == 2; }));
	HostEvent quit{};
	quit.type = kHEvStatus;
	quit.a = kQuit;
	quit.b = 2;
	CHECK(link.PushHostEvent(quit));
	CHECK(::WaitForSingleObject(static_cast<HANDLE>(process), 15000) == WAIT_OBJECT_0);
	DWORD code = 99;
	::GetExitCodeProcess(static_cast<HANDLE>(process), &code);
	CHECK(code == 0);
	::CloseHandle(static_cast<HANDLE>(process));
	if (code != 0) {
		std::printf("guest exit code %lu (log test_windows_link_guest.log)\n", code);
	}

	TestLauncherEndToEnd(self);
	if (g_failures) {
		std::printf("test_windows_link: %d FAILED (logs test_windows_link_*.log)\n", g_failures);
		return 1;
	}
	std::printf("test_windows_link: all passed (named mapping %s, guest pid %u; launcher end to end)\n", Narrow(mapping).c_str(), pid);
	return 0;
}
