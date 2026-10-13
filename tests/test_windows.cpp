// Windows players (docs/WINDOWS.md), the host half and the shared decisions (run with wine):
//   wine test_windows.exe <er/PhantomRingHost.ini>
// - common/Platform: mapping defaults and precedence, the auto switches for Windows / Wine / gamescope;
// - er/SteamLibrary: KeyValues parsing, libraryfolders.vdf (both formats), appmanifest, a fake Steam tree;
// - er/GuestInstall: the dll/ini decisions, and real installs into a scratch "MGSV folder" (fresh, same,
//   our older build, an unknown dinput8.dll, a player-edited ini);
// - er/Launcher: [Launch] defaults = the shipped ini, the environment block, starting a child process
//   (this exe again) that checks what it was given, and finding it by name.
#include "GuestInstall.h"
#include "Ini.h"
#include "Launcher.h"
#include "Log.h"
#include "Mapping.h"
#include "Platform.h"
#include "SteamLibrary.h"
#include "phantomring_protocol.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace phantom;
using namespace phantom::er;

static int g_failures = 0;
#define CHECK(x)                                                              \
	do {                                                                      \
		if (!(x)) {                                                           \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);           \
			++g_failures;                                                     \
		}                                                                     \
	} while (0)

namespace fs = std::filesystem;

static void Write(const fs::path& a_path, std::string_view a_text)
{
	fs::create_directories(a_path.parent_path());
	std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
	out << a_text;
}

static std::string Read(const fs::path& a_path)
{
	std::ifstream in(a_path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), {});
}

static std::wstring Env(const wchar_t* a_name)
{
	wchar_t    buf[512];
	const auto n = ::GetEnvironmentVariableW(a_name, buf, 512);
	return n > 0 && n < 512 ? std::wstring(buf, n) : std::wstring();
}

// ---- common/Platform ------------------------------------------------------------------------------
static void TestPlatform()
{
	using namespace platform;
	CHECK(DefaultMapping(true) == L"file:Z:\\dev\\shm\\phantomring_v1");
	CHECK(DefaultMapping(true) == proto::kDefaultMapping);  // what tools/phantomring (pr launch) creates
	CHECK(DefaultMapping(false) == L"name:Local\\PhantomRing_v1");
	// env (the launcher's) > ini > platform default; "auto" = default.
	CHECK(ResolveMapping(L"", L"", false) == kWindowsMapping);
	CHECK(ResolveMapping(L"auto", L"", true) == kWineMapping);
	CHECK(ResolveMapping(L"name:Local\\Other", L"", true) == L"name:Local\\Other");
	CHECK(ResolveMapping(L"name:Local\\Other", L"file:Z:\\dev\\shm\\x", false) == L"file:Z:\\dev\\shm\\x");
	CHECK(ResolveMapping(L"", L"file:Z:\\tmp\\y", false) == L"file:Z:\\tmp\\y");

	CHECK(Switch("auto", true) && !Switch("auto", false) && !Switch("", false) && Switch("nonsense", true));
	CHECK(Switch("true", false) && Switch(" On ", false) && Switch("1", false) && Switch("YES", false));
	CHECK(!Switch("false", true) && !Switch("off", true) && !Switch("0", true) && !Switch("No", true));

	const Facts windows{ false, false }, wine{ true, false }, wineGamescope{ true, true }, gamescopeOnly{ false, true };
	CHECK(HideGuestWindow("auto", windows));
	CHECK(!HideGuestWindow("auto", wine));
	CHECK(!HideGuestWindow("auto", wineGamescope));
	CHECK(!HideGuestWindow("auto", gamescopeOnly));
	CHECK(HideGuestWindow("true", wineGamescope));  // forced: e.g. the same-prefix Wine test mode
	CHECK(!HideGuestWindow("false", windows));
	CHECK(HostStartsGuest("auto", windows) && !HostStartsGuest("auto", wine) && HostStartsGuest("on", wine) && !HostStartsGuest("off", windows));

	// Under wine for real, but the test override makes this process take the Windows paths.
	CHECK(RunningUnderWine());
	CHECK(Detect().wine == (Env(L"PHANTOMRING_PLATFORM") != L"windows"));
}

// ---- er/SteamLibrary ------------------------------------------------------------------------------
static void TestSteam()
{
	using namespace steam;
	const char* modern = R"("libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"contentid"		"123"
		"apps"
		{
			"1245620"		"60000000000"
		}
	}
	// a comment
	"1"
	{
		"path"		"D:\\SteamLibrary"
		"apps"
		{
			"287700"		"25000000000"
		}
	}
	"contentstatsid"		"-4242"
}
)";
	const auto libs = LibraryFolders(modern);
	CHECK(libs.size() == 2 && libs[0] == "C:\\Program Files (x86)\\Steam" && libs[1] == "D:\\SteamLibrary");
	const char* old = "\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\t\"1\"\n\t\"1\"\t\t\"E:\\\\Games\\\\Steam\"\n}\n";
	const auto  oldLibs = LibraryFolders(old);
	CHECK(oldLibs.size() == 1 && oldLibs[0] == "E:\\Games\\Steam");
	CHECK(LibraryFolders("garbage { \"unterminated").empty());
	CHECK(LibraryFolders("").empty());

	const char* manifest = "\"AppState\"\n{\n\t\"appid\"\t\t\"287700\"\n\t\"name\"\t\t\"METAL GEAR SOLID V: THE PHANTOM PAIN\"\n\t\"installdir\"\t\t\"MGS_TPP\"\n"
						   "\t\"UserConfig\"\n\t{\n\t\t\"language\"\t\t\"english\"\n\t}\n}\n";
	CHECK(InstallDir(manifest) == std::optional<std::string>("MGS_TPP"));
	CHECK(!InstallDir("\"AppState\" { \"appid\" \"1\" }"));
	const auto kv = Parse("\"a\" { \"B\" \"x\\\"y\" }");
	CHECK(kv && kv->Child("a") && kv->Child("a")->Child("b") && kv->Child("a")->Child("b")->value == "x\"y");

	// A fake Steam: the game in a second library, named by its manifest; Steam's own library has no copy.
	const auto root = fs::absolute("test_windows_steam");
	fs::remove_all(root);
	const auto steamDir = root / "Steam";
	const auto lib2 = root / "Library Two";
	std::string lib2Escaped;
	for (const char c : lib2.string()) {
		lib2Escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
	}
	Write(steamDir / "steamapps" / "libraryfolders.vdf", "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + lib2Escaped + "\"\n\t}\n}\n");
	Write(lib2 / "steamapps" / "appmanifest_287700.acf", manifest);
	Write(lib2 / "steamapps" / "common" / "MGS_TPP" / "mgsvtpp.exe", "MZ");
	std::string why;
	const auto  exe = FindAppExe(287700, L"mgsvtpp.exe", L"MGS_TPP", steamDir.wstring(), &why);
	CHECK(fs::path(exe) == lib2 / "steamapps" / "common" / "MGS_TPP" / "mgsvtpp.exe");
	// No manifest: the fallback folder name still finds it.
	fs::remove(lib2 / "steamapps" / "appmanifest_287700.acf");
	CHECK(fs::path(FindAppExe(287700, L"mgsvtpp.exe", L"MGS_TPP", steamDir.wstring(), &why)) == lib2 / "steamapps" / "common" / "MGS_TPP" / "mgsvtpp.exe");
	why.clear();
	CHECK(FindAppExe(287700, L"mgsvtpp.exe", L"Elsewhere", steamDir.wstring(), &why).empty());
	CHECK(why.find("2 Steam libraries") != std::string::npos);
	fs::remove_all(root);
}

// ---- er/GuestInstall ------------------------------------------------------------------------------
static void TestGuestInstall()
{
	using namespace guestinstall;
	const std::vector<std::string> ours{ "old1", "old2" };
	CHECK(DecideDll("new", "", ours) == Action::kCopy);
	CHECK(DecideDll("new", "new", ours) == Action::kNone);
	CHECK(DecideDll("new", "old2", ours) == Action::kReplace);
	CHECK(DecideDll("new", "ihhook", ours) == Action::kRefuse);
	CHECK(DecideIni("new", "", ours) == Action::kCopy);
	CHECK(DecideIni("new", "new", ours) == Action::kNone);
	CHECK(DecideIni("new", "old1", ours) == Action::kReplace);
	CHECK(DecideIni("new", "edited", ours) == Action::kKeepUser);

	const auto root = fs::absolute("test_windows_install");
	fs::remove_all(root);
	const auto pkg = root / "pkg" / "mgsv";
	const auto mgsv = root / "MGS_TPP";
	fs::create_directories(mgsv);
	Write(pkg / "dinput8.dll", "guest build 1");
	Write(pkg / "PhantomRingGuest.ini", "[Guest]\nDebug = false\n");
	CHECK(Sha256File((pkg / "dinput8.dll").wstring()).size() == 64);
	CHECK(Sha256File((root / "missing").wstring()).empty());

	auto r = Install(pkg.wstring(), mgsv.wstring());
	CHECK(r.ok && r.dll == Action::kCopy && r.ini == Action::kCopy);
	CHECK(Read(mgsv / "dinput8.dll") == "guest build 1" && fs::exists(mgsv / kRecordName) && !fs::exists(mgsv / "dinput8.dll.new"));
	r = Install(pkg.wstring(), mgsv.wstring());
	CHECK(r.ok && r.dll == Action::kNone && r.ini == Action::kNone);

	// A newer build replaces ours; the player's edited ini stays.
	Write(pkg / "dinput8.dll", "guest build 2");
	Write(pkg / "PhantomRingGuest.ini", "[Guest]\nDebug = false\n[Window]\nHidden = auto\n");
	Write(mgsv / "PhantomRingGuest.ini", "[Guest]\nDebug = true\n");
	r = Install(pkg.wstring(), mgsv.wstring());
	CHECK(r.ok && r.dll == Action::kReplace && r.ini == Action::kKeepUser);
	CHECK(Read(mgsv / "dinput8.dll") == "guest build 2" && Read(mgsv / "PhantomRingGuest.ini") == "[Guest]\nDebug = true\n");

	// Our unedited older ini is refreshed.
	Write(mgsv / "PhantomRingGuest.ini", Read(pkg / "PhantomRingGuest.ini"));
	r = Install(pkg.wstring(), mgsv.wstring());
	CHECK(r.ini == Action::kNone);
	Write(pkg / "PhantomRingGuest.ini", "[Guest]\nDebug = false\n; v3\n");
	r = Install(pkg.wstring(), mgsv.wstring());
	CHECK(r.ok && r.ini == Action::kReplace && Read(mgsv / "PhantomRingGuest.ini") == "[Guest]\nDebug = false\n; v3\n");

	// Another mod's dinput8.dll (IHHook...): never touched, and MGSV must not start.
	Write(mgsv / "dinput8.dll", "IHHook");
	r = Install(pkg.wstring(), mgsv.wstring());
	CHECK(!r.ok && r.dll == Action::kRefuse && Read(mgsv / "dinput8.dll") == "IHHook");
	CHECK(r.text.find("not PhantomRing's") != std::string::npos);
	// ... nor when the record file is gone.
	fs::remove(mgsv / kRecordName);
	Write(mgsv / "dinput8.dll", "guest build 1");  // ours once, but nothing remembers it
	r = Install(pkg.wstring(), mgsv.wstring());
	CHECK(!r.ok && r.dll == Action::kRefuse);

	// A dinput8.dll / ini that exists but can't be read (held open without sharing by another process)
	// is not "absent": never copied over.
	{
		Write(mgsv / "dinput8.dll", "someone else's, locked");
		HANDLE lock = ::CreateFileW((mgsv / "dinput8.dll").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		CHECK(lock != INVALID_HANDLE_VALUE);
		r = Install(pkg.wstring(), mgsv.wstring());
		CHECK(!r.ok && r.dll == Action::kRefuse && r.text.find("cannot be read") != std::string::npos);
		::CloseHandle(lock);
		CHECK(Read(mgsv / "dinput8.dll") == "someone else's, locked");

		Write(mgsv / "dinput8.dll", Read(pkg / "dinput8.dll"));
		Write(mgsv / "PhantomRingGuest.ini", "[Guest]\nDebug = true ; the player's\n");
		lock = ::CreateFileW((mgsv / "PhantomRingGuest.ini").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		CHECK(lock != INVALID_HANDLE_VALUE);
		r = Install(pkg.wstring(), mgsv.wstring());
		CHECK(r.ok && r.dll == Action::kNone && r.ini == Action::kKeepUser);
		::CloseHandle(lock);
		CHECK(Read(mgsv / "PhantomRingGuest.ini") == "[Guest]\nDebug = true ; the player's\n");
	}

	// The package without its mgsv folder.
	r = Install((root / "nowhere").wstring(), mgsv.wstring());
	CHECK(!r.ok && r.text.find("missing") != std::string::npos);
	fs::remove_all(root);
}

// ---- er/Launcher ----------------------------------------------------------------------------------
static int Child()
{
	// Started by TestLauncher through Launcher::StartProcess with the guest's environment.
	int bad = 0;
	bad += Env(L"PHANTOMRING") != L"1";
	bad += Env(L"PHANTOMRING_ROLE") != L"guest";
	bad += Env(L"PHANTOMRING_MAPPING") != L"name:Local\\PhantomRing_v1";
	bad += Env(L"SteamAppId") != L"287700";
	bad += Env(L"SteamGameId") != L"287700";
	bad += Env(L"PHANTOM_TEST_KEEP") != L"kept";  // the rest of the parent's environment is inherited
	wchar_t dir[MAX_PATH];
	::GetCurrentDirectoryW(MAX_PATH, dir);
	bad += !std::wstring(dir).ends_with(L"test_windows_cwd");
	::Sleep(1500);  // long enough for the parent to find us by name
	return bad ? 10 + bad : 42;
}

static void TestLauncher(const wchar_t* a_ini, const wchar_t* a_self)
{
	using namespace Launcher;
	CHECK(Load(a_ini) == Settings{});  // the shipped [Launch] states the defaults
	CHECK(Load(L"no_such.ini") == Settings{});

	// Environment block: replaced case-insensitively, sorted, drive-cwd entries kept, double NUL.
	const wchar_t current[] = L"=C:=C:\\games\0Path=C:\\Windows\0steamappid=1245620\0Zeta=1\0ALPHA=2\0";
	const auto    block = BuildEnvironment(current, { { L"SteamAppId", L"287700" }, { L"PHANTOMRING", L"1" } });
	std::vector<std::wstring> entries;
	for (const wchar_t* p = block.c_str(); *p; p += std::wcslen(p) + 1) {
		entries.emplace_back(p);
	}
	CHECK(block.size() >= 2 && block[block.size() - 1] == L'\0' && block[block.size() - 2] == L'\0');
	CHECK((entries == std::vector<std::wstring>{ L"=C:=C:\\games", L"ALPHA=2", L"Path=C:\\Windows", L"PHANTOMRING=1", L"SteamAppId=287700", L"Zeta=1" }));

	const auto vars = GuestEnvironment(L"name:Local\\PhantomRing_v1");
	auto       has = [&](const wchar_t* a_name, const wchar_t* a_value) {
        return std::ranges::any_of(vars, [&](const auto& kv) { return kv.first == a_name && kv.second == a_value; });
	};
	CHECK(has(L"PHANTOMRING", L"1") && has(L"SteamAppId", L"287700") && has(L"SteamGameId", L"287700") &&
		  has(L"PHANTOMRING_MAPPING", L"name:Local\\PhantomRing_v1"));

	// The real start: this exe as "MGSV", in its own folder, with the guest environment.
	::SetEnvironmentVariableW(L"PHANTOM_TEST_KEEP", L"kept");
	::SetEnvironmentVariableW(L"SteamAppId", L"1245620");
	const auto cwd = fs::absolute("test_windows_cwd");
	fs::create_directories(cwd);
	std::uint32_t pid = 0;
	void*         process = nullptr;
	std::string   why;
	CHECK(StartProcess(a_self, L"child", cwd.wstring(), BuildEnvironment(nullptr, vars), &pid, &process, &why));
	if (process) {
		::Sleep(500);
		const auto self = fs::path(a_self).filename().wstring();
		const auto found = FindProcesses(self);
		CHECK(std::ranges::find(found, pid) != found.end());
		CHECK(FindProcesses(L"no_such_process.exe").empty());
		CHECK(::WaitForSingleObject(static_cast<HANDLE>(process), 20000) == WAIT_OBJECT_0);
		DWORD code = 0;
		::GetExitCodeProcess(static_cast<HANDLE>(process), &code);
		CHECK(code == 42);
		if (code != 42) {
			std::printf("child exit code %lu\n", code);
		}
		::CloseHandle(static_cast<HANDLE>(process));
	}
	CHECK(!StartProcess(L"C:\\no\\such.exe", L"", L"", L"", &pid, &process, &why) && why.find("CreateProcess") != std::string::npos);
	fs::remove_all(cwd);
}

int wmain(int argc, wchar_t** argv)
{
	if (argc > 1 && std::wstring(argv[1]) == L"child") {
		return Child();
	}
	Log::Init(L"test_windows.log", "test");
	if (argc < 2) {
		std::printf("usage: test_windows.exe <er/PhantomRingHost.ini>\n");
		return 2;
	}
	TestPlatform();
	TestSteam();
	TestGuestInstall();
	wchar_t self[MAX_PATH];
	::GetModuleFileNameW(nullptr, self, MAX_PATH);
	TestLauncher(argv[1], self);
	if (g_failures) {
		std::printf("test_windows: %d FAILED\n", g_failures);
		return 1;
	}
	std::printf("test_windows: all passed\n");
	return 0;
}
