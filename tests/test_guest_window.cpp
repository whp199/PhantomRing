// Hidden MGSV window (mgsv/WindowGuard + mgsv/WindowHidden) against a stand-in game window (run with wine):
//   PHANTOMRING_PLATFORM=windows wine test_guest_window.exe <mgsv/PhantomRingGuest.ini>
// This exe plays MGSV: it registers a window class from its own module and creates a top-level window
// with WS_VISIBLE, after hidden::Start installed the hooks. The window must never become visible,
// minimised or active, must stay off the taskbar, focus queries must answer it, and its window proc must
// never see focus-loss messages. The visibility-check detour must refuse itself here (the MGSV bytes
// are not at 0x14024A1D0 in this exe: linked at another image base).
#include "Hook.h"
#include "Log.h"
#include "Platform.h"
#include "WindowGuard.h"
#include "WindowHidden.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace phantom;
using namespace phantom::guest;

static int g_failures = 0;
#define CHECK(x)                                                              \
	do {                                                                      \
		if (!(x)) {                                                           \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);           \
			++g_failures;                                                     \
		}                                                                     \
	} while (0)

static std::vector<UINT> g_seen;       // what the game's own window proc received
static std::vector<UINT> g_seenOther;  // what a later subclass received

static LRESULT CALLBACK GameProc(HWND a_window, UINT a_msg, WPARAM a_w, LPARAM a_l)
{
	g_seen.push_back(a_msg);
	return ::DefWindowProcW(a_window, a_msg, a_w, a_l);
}

static WNDPROC g_afterUs{ nullptr };
static LRESULT CALLBACK OtherProc(HWND a_window, UINT a_msg, WPARAM a_w, LPARAM a_l)
{
	g_seenOther.push_back(a_msg);
	return ::CallWindowProcW(g_afterUs, a_window, a_msg, a_w, a_l);
}

static bool Saw(const std::vector<UINT>& a_list, UINT a_msg) { return std::ranges::find(a_list, a_msg) != a_list.end(); }

static bool HostAlive() { return true; }

static void TestPure()
{
	using namespace window;
	CHECK(SwallowFocusLoss(WM_ACTIVATEAPP, FALSE) && !SwallowFocusLoss(WM_ACTIVATEAPP, TRUE));
	CHECK(SwallowFocusLoss(WM_ACTIVATE, WA_INACTIVE) && !SwallowFocusLoss(WM_ACTIVATE, WA_ACTIVE) && !SwallowFocusLoss(WM_ACTIVATE, WA_CLICKACTIVE));
	CHECK(SwallowFocusLoss(WM_NCACTIVATE, FALSE) && !SwallowFocusLoss(WM_NCACTIVATE, TRUE));
	CHECK(SwallowFocusLoss(WM_KILLFOCUS, 0) && !SwallowFocusLoss(WM_SETFOCUS, 0) && !SwallowFocusLoss(WM_INPUT, 0) && !SwallowFocusLoss(WM_USER, 0));
	CHECK(HiddenStyle(WS_OVERLAPPEDWINDOW | WS_VISIBLE) == WS_OVERLAPPEDWINDOW);
	CHECK(HiddenExStyle(WS_EX_APPWINDOW | WS_EX_CLIENTEDGE) == (WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE));
	CHECK(IsMinimise(SW_MINIMIZE) && IsMinimise(SW_SHOWMINNOACTIVE) && IsMinimise(SW_FORCEMINIMIZE) && IsMinimise(SW_SHOWMINIMIZED));
	CHECK(!IsMinimise(SW_HIDE) && !IsMinimise(SW_SHOW) && !IsMinimise(SW_RESTORE) && !IsMinimise(SW_SHOWNOACTIVATE));
	CHECK(!hidden::ShouldQuit(200000, 100000, 0));       // 0 = never
	CHECK(!hidden::ShouldQuit(219999, 100000, 120));
	CHECK(hidden::ShouldQuit(220001, 100000, 120));
	// Saves blocked: kill at once. Saves live: WM_CLOSE first, the kill only after the grace period.
	using hidden::QuitStep;
	CHECK(hidden::NextQuitStep(true, 500000, 0, 30) == QuitStep::kKill);
	CHECK(hidden::NextQuitStep(false, 500000, 0, 30) == QuitStep::kAskClose);
	CHECK(hidden::NextQuitStep(false, 529000, 500000, 30) == QuitStep::kWait);
	CHECK(hidden::NextQuitStep(false, 530001, 500000, 30) == QuitStep::kKill);
}

int main(int argc, char** argv)
{
	Log::Init(L"test_guest_window.log", "test");
	if (argc < 2) {
		std::printf("usage: test_guest_window.exe <mgsv/PhantomRingGuest.ini>\n");
		return 2;
	}
	TestPure();
	const auto shipped = hidden::Load(Widen(argv[1]));
	CHECK(shipped == hidden::Settings{});  // the shipped [Window] states the defaults
	CHECK(platform::HideGuestWindow(shipped.hidden, platform::Detect()) == !platform::Detect().wine);
	if (platform::Detect().wine) {
		std::printf("test_guest_window: run with PHANTOMRING_PLATFORM=windows (hidden mode is auto-off under Wine)\n");
		return 1;
	}

	CHECK(Hooks::Init());
	hidden::Start(shipped, &HostAlive);
	Hooks::EnableAll();
	CHECK(hidden::Active() && window::Hidden() && hidden::ForceWindowed());
	CHECK(hidden::Describe().find("render check forced false") != std::string::npos);  // no MGSV code here

	// The game creates its window, visible, as a normal app window.
	const HINSTANCE exe = ::GetModuleHandleW(nullptr);
	WNDCLASSEXW     wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = &GameProc;
	wc.hInstance = exe;
	wc.lpszClassName = L"PhantomRingTestGame";
	CHECK(::RegisterClassExW(&wc) != 0);
	HWND game = ::CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"MGSV stand-in", WS_OVERLAPPEDWINDOW | WS_VISIBLE, -32000, -32000, 64, 64, nullptr, nullptr, exe,
		nullptr);
	CHECK(game != nullptr);
	CHECK(window::GameWindow() == game);
	CHECK(!::IsWindowVisible(game));
	const auto ex = static_cast<DWORD>(::GetWindowLongPtrW(game, GWL_EXSTYLE));
	CHECK((ex & WS_EX_TOOLWINDOW) && (ex & WS_EX_NOACTIVATE) && !(ex & WS_EX_APPWINDOW));

	// Everything that would show, activate or minimise it keeps it hidden.
	::ShowWindow(game, SW_SHOWNORMAL);
	CHECK(!::IsWindowVisible(game));
	::ShowWindow(game, SW_MINIMIZE);
	CHECK(!(::GetWindowLongPtrW(game, GWL_STYLE) & WS_MINIMIZE) && !::IsIconic(game) && !::IsWindowVisible(game));
	::SetWindowPos(game, HWND_TOP, 0, 0, 64, 64, SWP_SHOWWINDOW | SWP_NOMOVE);
	CHECK(!::IsWindowVisible(game));
	::SetWindowLongPtrW(game, GWL_STYLE, ::GetWindowLongPtrW(game, GWL_STYLE) | WS_VISIBLE);
	CHECK(!(::GetWindowLongPtrW(game, GWL_STYLE) & WS_VISIBLE));
	::SetWindowLongPtrW(game, GWL_EXSTYLE, WS_EX_APPWINDOW);
	CHECK((::GetWindowLongPtrW(game, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) && !(::GetWindowLongPtrW(game, GWL_EXSTYLE) & WS_EX_APPWINDOW));
	CHECK(::SetForegroundWindow(game));  // "succeeds", does nothing
	::SetActiveWindow(game);
	::SetFocus(game);
	CHECK(::AttachThreadInput(::GetCurrentThreadId(), ::GetCurrentThreadId() + 4, TRUE));  // no-op while hidden
	CHECK(!::IsWindowVisible(game));

	// MGSV believes it has the focus.
	CHECK(::GetForegroundWindow() == game && ::GetActiveWindow() == game && ::GetFocus() == game);

	// Focus loss never reaches its window proc; everything else does.
	g_seen.clear();
	::SendMessageW(game, WM_ACTIVATEAPP, FALSE, 0);
	::SendMessageW(game, WM_KILLFOCUS, 0, 0);
	::SendMessageW(game, WM_ACTIVATE, WA_INACTIVE, 0);
	CHECK(!Saw(g_seen, WM_ACTIVATEAPP) && !Saw(g_seen, WM_KILLFOCUS) && !Saw(g_seen, WM_ACTIVATE));
	::SendMessageW(game, WM_ACTIVATEAPP, TRUE, 0);
	::SendMessageW(game, WM_USER + 7, 0, 0);
	CHECK(Saw(g_seen, WM_ACTIVATEAPP) && Saw(g_seen, WM_USER + 7));

	// A later subclass (another mod, an overlay) sits behind our filter and still gets everything else.
	g_afterUs = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(game, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&OtherProc)));
	CHECK(g_afterUs == &GameProc);
	g_seen.clear();
	::SendMessageW(game, WM_KILLFOCUS, 0, 0);
	::SendMessageW(game, WM_USER + 8, 0, 0);
	CHECK(!Saw(g_seenOther, WM_KILLFOCUS) && Saw(g_seenOther, WM_USER + 8) && Saw(g_seen, WM_USER + 8));

	// Windows that aren't the game's (here a message-only one of the same class) are left alone.
	HWND other = ::CreateWindowExW(0, wc.lpszClassName, L"other", 0, 0, 0, 1, 1, HWND_MESSAGE, nullptr, exe, nullptr);
	CHECK(other && window::GameWindow() == game && (::GetWindowLongPtrW(other, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0);
	::DestroyWindow(other);

	std::printf("%s\n", hidden::Describe().c_str());
	CHECK(hidden::Describe().find("focus answers 0") == std::string::npos);

	// The game's window goes away: nothing answers for it any more.
	::DestroyWindow(game);
	CHECK(window::GameWindow() == nullptr);
	CHECK(::GetForegroundWindow() != game);

	if (g_failures) {
		std::printf("test_guest_window: %d FAILED\n", g_failures);
		return 1;
	}
	std::printf("test_guest_window: all passed\n");
	return 0;
}
