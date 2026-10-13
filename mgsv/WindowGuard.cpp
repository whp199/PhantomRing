#include "WindowGuard.h"

#include "Hook.h"
#include "Log.h"

#include <atomic>
#include <format>

namespace phantom::guest::window
{
	namespace
	{
		using SetCursorPosFn = BOOL(WINAPI*)(int, int);
		using SetForegroundWindowFn = BOOL(WINAPI*)(HWND);
		using ShowWindowFn = BOOL(WINAPI*)(HWND, int);
		using SendMessageWFn = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
		using CreateWindowExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
		using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
		using SetWindowLongWFn = LONG(WINAPI*)(HWND, int, LONG);
		using SetWindowLongPtrWFn = LONG_PTR(WINAPI*)(HWND, int, LONG_PTR);
		using GetWindowFn = HWND(WINAPI*)();
		using SetWindowFn = HWND(WINAPI*)(HWND);
		using AttachThreadInputFn = BOOL(WINAPI*)(DWORD, DWORD, BOOL);
		using IsIconicFn = BOOL(WINAPI*)(HWND);

		SetCursorPosFn        g_setCursorPos{ nullptr };
		SetForegroundWindowFn g_setForeground{ nullptr };
		ShowWindowFn          g_showWindow{ nullptr };
		SendMessageWFn        g_sendMessageW{ nullptr };
		CreateWindowExWFn     g_createWindowExW{ nullptr };
		SetWindowPosFn        g_setWindowPos{ nullptr };
		SetWindowLongWFn      g_setWindowLongW{ nullptr };
		SetWindowLongPtrWFn   g_setWindowLongPtrW{ nullptr };
		GetWindowFn           g_getForeground{ nullptr };
		GetWindowFn           g_getActive{ nullptr };
		GetWindowFn           g_getFocus{ nullptr };
		SetWindowFn           g_setActive{ nullptr };
		SetWindowFn           g_setFocus{ nullptr };
		AttachThreadInputFn   g_attachThreadInput{ nullptr };
		IsIconicFn            g_isIconic{ nullptr };

		Options            g_options;
		std::atomic<HWND>  g_game{ nullptr };
		std::atomic<unsigned> g_blocked{ 0 };
		std::atomic<unsigned> g_focusAnswers{ 0 };
		std::atomic<unsigned> g_swallowed{ 0 };
		std::atomic<unsigned> g_showsHidden{ 0 };

		constexpr wchar_t kProcProp[] = L"PhantomRing.WndProc";

		HMODULE Exe() { return ::GetModuleHandleW(nullptr); }

		bool Ours(HWND a_window)
		{
			DWORD pid = 0;
			return a_window && ::GetWindowThreadProcessId(a_window, &pid) && pid == ::GetCurrentProcessId();
		}

		// A top-level window of a class the exe registered: MGSV's game window (research §8.3).
		bool IsGameClass(HWND a_window)
		{
			if (!Ours(a_window) || (::GetWindowLongPtrW(a_window, GWL_STYLE) & WS_CHILD) || ::GetAncestor(a_window, GA_PARENT) != ::GetDesktopWindow()) {
				return false;
			}
			return reinterpret_cast<HMODULE>(::GetClassLongPtrW(a_window, GCLP_HMODULE)) == Exe();
		}

		bool IsGame(HWND a_window)
		{
			return a_window && (a_window == g_game.load(std::memory_order_acquire) || ::GetPropW(a_window, kProcProp));
		}

		void Note(const char* a_what)
		{
			if (g_blocked.fetch_add(1) < 24) {
				Log::Info("window guard: ignored {}", a_what);
			}
		}

		LRESULT CALLBACK HiddenProc(HWND a_window, UINT a_msg, WPARAM a_w, LPARAM a_l)
		{
			const auto original = reinterpret_cast<WNDPROC>(::GetPropW(a_window, kProcProp));
			if (SwallowFocusLoss(a_msg, a_w)) {
				if (g_swallowed.fetch_add(1) < 8) {
					Log::Info("window: swallowed focus-loss message {:#x} for MGSV's hidden window", a_msg);
				}
				return a_msg == WM_NCACTIVATE ? TRUE : 0;
			}
			if (a_msg == WM_NCDESTROY) {
				::RemovePropW(a_window, kProcProp);
				HWND expected = a_window;
				g_game.compare_exchange_strong(expected, nullptr);
			}
			return original ? ::CallWindowProcW(original, a_window, a_msg, a_w, a_l) : ::DefWindowProcW(a_window, a_msg, a_w, a_l);
		}

		// Hide, restyle, subclass. Idempotent (a window already subclassed keeps its first original proc).
		void Adopt(HWND a_window, const char* a_how)
		{
			if (!a_window) {
				return;
			}
			const auto set = g_setWindowLongPtrW ? g_setWindowLongPtrW : &::SetWindowLongPtrW;
			const auto ex = static_cast<DWORD>(::GetWindowLongPtrW(a_window, GWL_EXSTYLE));
			set(a_window, GWL_EXSTYLE, static_cast<LONG_PTR>(HiddenExStyle(ex)));
			(g_showWindow ? g_showWindow : &::ShowWindow)(a_window, SW_HIDE);
			if (g_options.spoofFocus && !::GetPropW(a_window, kProcProp)) {
				const auto original = set(a_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HiddenProc));
				if (original) {
					::SetPropW(a_window, kProcProp, reinterpret_cast<HANDLE>(original));
				}
			}
			g_game.store(a_window, std::memory_order_release);
			Log::Info("window: MGSV's game window {} kept hidden ({}; ex-style {:#x} -> {:#x}, focus {})", static_cast<void*>(a_window), a_how, ex,
				HiddenExStyle(ex), g_options.spoofFocus ? "spoofed" : "real");
		}

		BOOL CALLBACK AdoptExisting(HWND a_window, LPARAM)
		{
			if (IsGameClass(a_window)) {
				Adopt(a_window, "already open");
			}
			return TRUE;
		}

		// ---- guard detours ------------------------------------------------------------------------
		BOOL WINAPI SetCursorPosDetour(int, int)
		{
			return TRUE;  // MGSV recentres the cursor for mouse look; the cursor belongs to Elden Ring
		}

		BOOL WINAPI SetForegroundWindowDetour(HWND a_window)
		{
			if (Ours(a_window)) {
				Note("SetForegroundWindow");
				return TRUE;
			}
			return g_setForeground(a_window);
		}

		BOOL WINAPI ShowWindowDetour(HWND a_window, int a_show)
		{
			if (g_options.hidden && IsGame(a_window)) {
				if (a_show != SW_HIDE && g_showsHidden.fetch_add(1) < 8) {
					Log::Info("window: ShowWindow({}) on MGSV's window kept it hidden", a_show);
				}
				return g_showWindow(a_window, SW_HIDE);
			}
			if (IsMinimise(a_show) && Ours(a_window)) {
				Note("ShowWindow(minimise)");
				return TRUE;
			}
			return g_showWindow(a_window, a_show);
		}

		LRESULT WINAPI SendMessageWDetour(HWND a_window, UINT a_msg, WPARAM a_w, LPARAM a_l)
		{
			// The exclusive-fullscreen focus-loss path minimises through WM_SYSCOMMAND/SC_MINIMIZE.
			if (a_msg == WM_SYSCOMMAND && (a_w & 0xFFF0) == SC_MINIMIZE && Ours(a_window)) {
				Note("WM_SYSCOMMAND SC_MINIMIZE");
				return 0;
			}
			return g_sendMessageW(a_window, a_msg, a_w, a_l);
		}

		// ---- hidden-mode detours ------------------------------------------------------------------
		HWND WINAPI CreateWindowExWDetour(DWORD a_ex, LPCWSTR a_class, LPCWSTR a_title, DWORD a_style, int a_x, int a_y, int a_w, int a_h, HWND a_parent,
			HMENU a_menu, HINSTANCE a_instance, LPVOID a_param)
		{
			// MGSV passes its own module and no parent for the game window (0x1400705C0). Decide before
			// creation so it never flashes onto the desktop or takes the activation from Elden Ring.
			WNDCLASSEXW wc{};
			wc.cbSize = sizeof(wc);
			const bool candidate = !(a_style & WS_CHILD) && !a_parent && ::GetClassInfoExW(Exe(), a_class, &wc) && wc.hInstance == Exe();
			if (candidate) {
				a_style = HiddenStyle(a_style);
				a_ex = HiddenExStyle(a_ex);
			}
			HWND window = g_createWindowExW(a_ex, a_class, a_title, a_style, a_x, a_y, a_w, a_h, a_parent, a_menu, a_instance, a_param);
			if (window && IsGameClass(window)) {
				Adopt(window, "created");
			}
			return window;
		}

		BOOL WINAPI SetWindowPosDetour(HWND a_window, HWND a_after, int a_x, int a_y, int a_w, int a_h, UINT a_flags)
		{
			if (IsGame(a_window)) {
				a_flags = (a_flags & ~SWP_SHOWWINDOW) | SWP_NOACTIVATE;
			}
			return g_setWindowPos(a_window, a_after, a_x, a_y, a_w, a_h, a_flags);
		}

		LONG_PTR Restyle(HWND a_window, int a_index, LONG_PTR a_value)
		{
			if (!IsGame(a_window)) {
				return a_value;
			}
			if (a_index == GWL_STYLE) {
				return static_cast<LONG_PTR>(HiddenStyle(static_cast<DWORD>(a_value)));
			}
			if (a_index == GWL_EXSTYLE) {
				return static_cast<LONG_PTR>(HiddenExStyle(static_cast<DWORD>(a_value)));
			}
			return a_value;
		}

		LONG WINAPI SetWindowLongWDetour(HWND a_window, int a_index, LONG a_value)
		{
			return g_setWindowLongW(a_window, a_index, static_cast<LONG>(Restyle(a_window, a_index, a_value)));
		}

		LONG_PTR WINAPI SetWindowLongPtrWDetour(HWND a_window, int a_index, LONG_PTR a_value)
		{
			// Somebody subclassing the window after us: their proc goes behind our filter (which keeps
			// calling it), and they get back the proc they would have replaced.
			if (a_index == GWLP_WNDPROC && a_value != reinterpret_cast<LONG_PTR>(&HiddenProc)) {
				if (const auto previous = ::GetPropW(a_window, kProcProp)) {
					::SetPropW(a_window, kProcProp, reinterpret_cast<HANDLE>(a_value));
					return reinterpret_cast<LONG_PTR>(previous);
				}
			}
			return g_setWindowLongPtrW(a_window, a_index, Restyle(a_window, a_index, a_value));
		}

		HWND FocusAnswer(GetWindowFn a_original)
		{
			if (const auto game = g_game.load(std::memory_order_acquire); game && g_options.spoofFocus) {
				g_focusAnswers.fetch_add(1, std::memory_order_relaxed);
				return game;
			}
			return a_original();
		}

		HWND WINAPI GetForegroundWindowDetour() { return FocusAnswer(g_getForeground); }
		HWND WINAPI GetActiveWindowDetour() { return FocusAnswer(g_getActive); }
		HWND WINAPI GetFocusDetour() { return FocusAnswer(g_getFocus); }

		HWND WINAPI SetActiveWindowDetour(HWND a_window)
		{
			if (IsGame(a_window)) {
				return a_window;  // activating it would take the activation from Elden Ring
			}
			return g_setActive(a_window);
		}

		HWND WINAPI SetFocusDetour(HWND a_window)
		{
			if (IsGame(a_window)) {
				return a_window;
			}
			return g_setFocus(a_window);
		}

		BOOL WINAPI AttachThreadInputDetour(DWORD a_from, DWORD a_to, BOOL a_attach)
		{
			// MGSV's force-foreground helper (0x14024B5B0) attaches to the foreground thread: Elden Ring's.
			// Sharing its input state could steal its keyboard focus; never do it while hidden.
			Note("AttachThreadInput");
			(void)a_from, (void)a_to, (void)a_attach;
			return TRUE;
		}

		BOOL WINAPI IsIconicDetour(HWND a_window)
		{
			if (IsGame(a_window)) {
				return FALSE;  // the iconic watcher (0x140001BC0) would post MGSV's suspend event
			}
			return g_isIconic(a_window);
		}

		template <class F>
		bool Hook(HMODULE a_user32, const char* a_name, F* a_detour, F** a_original)
		{
			auto* target = reinterpret_cast<void*>(::GetProcAddress(a_user32, a_name));
			if (!target) {
				Log::Warn("window: user32!{} not found", a_name);
				return false;
			}
			return Hooks::Create(a_name, target, reinterpret_cast<void*>(a_detour), reinterpret_cast<void**>(a_original));
		}
	}

	DWORD HiddenStyle(DWORD a_style) { return a_style & ~static_cast<DWORD>(WS_VISIBLE); }

	DWORD HiddenExStyle(DWORD a_exStyle) { return (a_exStyle & ~static_cast<DWORD>(WS_EX_APPWINDOW)) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE; }

	bool IsMinimise(int a_show) { return a_show == SW_MINIMIZE || a_show == SW_SHOWMINIMIZED || a_show == SW_SHOWMINNOACTIVE || a_show == SW_FORCEMINIMIZE; }

	bool SwallowFocusLoss(UINT a_msg, WPARAM a_w)
	{
		switch (a_msg) {
		case WM_ACTIVATEAPP:
			return a_w == FALSE;
		case WM_ACTIVATE:
			return LOWORD(a_w) == WA_INACTIVE;
		case WM_NCACTIVATE:
			return a_w == FALSE;
		case WM_KILLFOCUS:
			return true;
		default:
			return false;
		}
	}

	bool Install(const Options& a_options)
	{
		g_options = a_options;
		g_options.guard |= g_options.hidden;
		HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
		if (!user32) {
			Log::Warn("window guard: user32 not loaded");
			return false;
		}
		bool ok = true;
		if (g_options.guard) {
			ok &= Hook(user32, "SetCursorPos", &SetCursorPosDetour, &g_setCursorPos);
			ok &= Hook(user32, "SetForegroundWindow", &SetForegroundWindowDetour, &g_setForeground);
			ok &= Hook(user32, "ShowWindow", &ShowWindowDetour, &g_showWindow);
			ok &= Hook(user32, "SendMessageW", &SendMessageWDetour, &g_sendMessageW);
		}
		if (g_options.hidden) {
			ok &= Hook(user32, "CreateWindowExW", &CreateWindowExWDetour, &g_createWindowExW);
			ok &= Hook(user32, "SetWindowPos", &SetWindowPosDetour, &g_setWindowPos);
			ok &= Hook(user32, "SetWindowLongW", &SetWindowLongWDetour, &g_setWindowLongW);
			ok &= Hook(user32, "SetWindowLongPtrW", &SetWindowLongPtrWDetour, &g_setWindowLongPtrW);
			ok &= Hook(user32, "SetActiveWindow", &SetActiveWindowDetour, &g_setActive);
			ok &= Hook(user32, "SetFocus", &SetFocusDetour, &g_setFocus);
			ok &= Hook(user32, "AttachThreadInput", &AttachThreadInputDetour, &g_attachThreadInput);
			ok &= Hook(user32, "IsIconic", &IsIconicDetour, &g_isIconic);
			if (g_options.spoofFocus) {
				ok &= Hook(user32, "GetForegroundWindow", &GetForegroundWindowDetour, &g_getForeground);
				ok &= Hook(user32, "GetActiveWindow", &GetActiveWindowDetour, &g_getActive);
				ok &= Hook(user32, "GetFocus", &GetFocusDetour, &g_getFocus);
			}
		}
		if (!ok) {
			Log::Warn("window: some user32 hooks were refused (see above)");
		}
		if (g_options.hidden) {
			// Enable now: a window made before the hooks (late install) is adopted with the trampolines live.
			Hooks::EnableAll();
			::EnumWindows(&AdoptExisting, 0);
		}
		Log::Info("window: guard {}, hidden {}, focus spoof {}", g_options.guard, g_options.hidden, g_options.hidden && g_options.spoofFocus);
		return ok;
	}

	bool Hidden() { return g_options.hidden; }

	HWND GameWindow()
	{
		const auto game = g_game.load(std::memory_order_acquire);
		return game && ::IsWindow(game) ? game : nullptr;
	}

	std::string Describe()
	{
		const auto game = GameWindow();
		return std::format("window: guard {} hidden {} focus-spoof {}; game window {} visible {} iconic {} ex-style {:#x}; focus answers {}, focus-loss swallowed {}, shows kept hidden {}, blocked {}",
			g_options.guard, g_options.hidden, g_options.hidden && g_options.spoofFocus, static_cast<void*>(game), game ? ::IsWindowVisible(game) != FALSE : false,
			game ? (g_isIconic ? g_isIconic(game) : ::IsIconic(game)) != FALSE : false, game ? static_cast<DWORD>(::GetWindowLongPtrW(game, GWL_EXSTYLE)) : 0u,
			g_focusAnswers.load(), g_swallowed.load(), g_showsHidden.load(), g_blocked.load());
	}
}
