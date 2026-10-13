#include "WindowHidden.h"

#include "Addresses.h"
#include "Hook.h"
#include "Ini.h"
#include "Log.h"
#include "Platform.h"
#include "Verify.h"
#include "WindowGuard.h"

#include <windows.h>

#include <atomic>
#include <format>

namespace phantom::guest::hidden
{
	namespace
	{
		std::atomic<bool>          g_active{ false };
		std::atomic<bool>          g_forceWindowed{ false };
		std::atomic<bool>          g_renderForced{ false };
		std::atomic<std::uint64_t> g_viewChecks{ 0 };
		std::atomic<std::uint64_t> g_viewForced{ 0 };
		bool (*g_hostAlive)(){ nullptr };
		int g_quitSec{ 0 };
		std::atomic<bool> g_savesBlocked{ false };
		constexpr int     kCloseGraceSec = 30;

		using ViewVisibleFn = bool (*)(void*, std::uint8_t*);
		ViewVisibleFn g_viewVisible{ nullptr };

		// 0x14024A1D0(renderer, view): the scene of a view is drawn only if its window is visible. The
		// window is hidden on purpose, so "exists" is the test that matters (a destroyed window still
		// skips, as before).
		bool ViewVisibleDetour(void* a_self, std::uint8_t* a_view)
		{
			g_viewChecks.fetch_add(1, std::memory_order_relaxed);
			if (!a_view) {
				return false;
			}
			const HWND window = *reinterpret_cast<HWND*>(a_view + 0x40);
			if (!::IsWindow(window)) {
				return false;
			}
			if (!::IsWindowVisible(window)) {
				g_viewForced.fetch_add(1, std::memory_order_relaxed);
			}
			(void)a_self;
			return true;
		}

		bool InstallRender()
		{
			if (!Matches(addr::kViewVisible, "view visibility check")) {
				Log::Warn("hidden: the visibility check is not the expected code: MGSV may stop drawing while hidden (no capture)");
				return false;
			}
			return Hooks::Create("view visibility check", reinterpret_cast<void*>(addr::kViewVisible.va), &ViewVisibleDetour, &g_viewVisible);
		}

		DWORD WINAPI QuitWatcher(void*)
		{
			std::int64_t lastHost = static_cast<std::int64_t>(::GetTickCount64());  // the boot counts as "seen": ER starts us
			std::int64_t closeAsked = 0;
			for (;;) {
				::Sleep(1000);
				const auto now = static_cast<std::int64_t>(::GetTickCount64());
				if (g_hostAlive && g_hostAlive()) {
					lastHost = now;
					closeAsked = 0;
					continue;
				}
				if (!ShouldQuit(now, lastHost, g_quitSec)) {
					continue;
				}
				switch (NextQuitStep(g_savesBlocked.load(), now, closeAsked, kCloseGraceSec)) {
				case QuitStep::kKill:
					if (closeAsked) {
						Log::Warn("hidden: MGSV did not close {} s after WM_CLOSE -- terminating it", kCloseGraceSec);
					} else {
						Log::Info("hidden: no Elden Ring for {} s -- MGSV quits (nothing is saved: [Saves] Block)", g_quitSec);
					}
					::TerminateProcess(::GetCurrentProcess(), 0);
					break;
				case QuitStep::kAskClose: {
					// MGSV's own shutdown, so a save it has queued is written whole (WM_CLOSE handling UNVERIFIED:
					// the kill after the grace period covers a game that ignores it).
					const HWND w = window::GameWindow();
					Log::Info("hidden: no Elden Ring for {} s -- saves are live: asking MGSV to close (window {})", g_quitSec, static_cast<void*>(w));
					if (w) {
						::PostMessageW(w, WM_CLOSE, 0, 0);
					}
					closeAsked = now;
					break;
				}
				case QuitStep::kWait:
					break;
				}
			}
		}
	}

	Settings Load(const std::wstring& a_iniPath)
	{
		Settings s;
		Ini      ini;
		ini.Load(a_iniPath);
		s.hidden = ini.Get("Window", "Hidden", s.hidden);
		s.spoofFocus = ini.GetBool("Window", "SpoofFocus", s.spoofFocus);
		s.forceRender = ini.GetBool("Window", "ForceRender", s.forceRender);
		s.forceWindowed = ini.GetBool("Window", "ForceWindowed", s.forceWindowed);
		s.quitWithoutHostSec = static_cast<int>(ini.GetInt("Window", "QuitWithoutHostSec", s.quitWithoutHostSec));
		return s;
	}

	void SetSavesBlocked(bool a_blocked) { g_savesBlocked.store(a_blocked); }
	bool SavesBlocked() { return g_savesBlocked.load(); }

	QuitStep NextQuitStep(bool a_savesBlocked, std::int64_t a_nowMs, std::int64_t a_closeAskedMs, int a_graceSec)
	{
		if (a_savesBlocked) {
			return QuitStep::kKill;
		}
		if (!a_closeAskedMs) {
			return QuitStep::kAskClose;
		}
		return a_nowMs - a_closeAskedMs > std::int64_t(a_graceSec) * 1000 ? QuitStep::kKill : QuitStep::kWait;
	}

	bool ShouldQuit(std::int64_t a_nowMs, std::int64_t a_lastHostMs, int a_quitSec) { return a_quitSec > 0 && a_nowMs - a_lastHostMs > std::int64_t(a_quitSec) * 1000; }

	void Start(const Settings& a_settings, bool (*a_hostAlive)())
	{
		const auto& facts = platform::Detect();
		const bool  hide = platform::HideGuestWindow(a_settings.hidden, facts);
		Log::Info("hidden: [Window] Hidden = {} -> {} (wine {}, gamescope {})", a_settings.hidden, hide ? "hide MGSV's window" : "leave it", facts.wine,
			facts.gamescope);
		g_active.store(hide);
		if (!hide) {
			return;  // the plain guard (if any) is installed at late init, as before (Guest.cpp)
		}
		window::Options options;
		options.guard = true;
		options.hidden = true;
		options.spoofFocus = a_settings.spoofFocus;
		window::Install(options);
		g_forceWindowed.store(a_settings.forceWindowed);
		if (a_settings.forceRender) {
			g_renderForced.store(InstallRender());
		}
		g_hostAlive = a_hostAlive;
		g_quitSec = a_settings.quitWithoutHostSec;
		if (g_quitSec > 0 && a_hostAlive) {
			if (HANDLE t = ::CreateThread(nullptr, 0, &QuitWatcher, nullptr, 0, nullptr)) {
				::CloseHandle(t);
			}
		}
	}

	bool Active() { return g_active.load(); }

	bool ForceWindowed() { return g_active.load() && g_forceWindowed.load(); }

	std::string Describe()
	{
		return std::format("{}\nhidden: {}, render check forced {} ({} checks, {} answered visible for the hidden window), windowed swap chain {}, quit after {} s without Elden Ring",
			window::Describe(), Active(), g_renderForced.load(), g_viewChecks.load(), g_viewForced.load(), ForceWindowed(), g_quitSec);
	}
}
