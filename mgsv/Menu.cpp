#include "Menu.h"

#include "Addresses.h"
#include "Hook.h"
#include "Log.h"
#include "State.h"
#include "Verify.h"

#include <windows.h>

#include <atomic>
#include <format>
#include <optional>

namespace phantom::guest::menu
{
	namespace
	{
		std::atomic<bool>          g_pauseOk{ false };
		std::atomic<bool>          g_last{ false };
		std::atomic<std::uint64_t> g_opens{ 0 }, g_pauses{ 0 }, g_badObject{ 0 };
		std::atomic<std::uint64_t> g_openedMs{ 0 };
		std::atomic<int>           g_lastWhy{ 0 };  // 0 none, 1 pause, 2 iDroid/popup

		template <class T>
		std::optional<T> Get(std::uintptr_t a_at)
		{
			return SafeRead<T>(reinterpret_cast<const void*>(a_at));
		}

		bool LuaMenu()
		{
			// The Tick's MENU bit, while that Tick is fresh (the Lua thread keeps ticking through every menu).
			const auto lua = state::Lua();
			return lua.valid && (lua.flags & state::kLMenu) && ::GetTickCount64() - lua.tickMs < 500;
		}
	}

	void Install()
	{
		g_pauseOk = Matches(addr::kGetPauseMenu, "TppGameSequence.GetPauseMenu");
		Log::Info("menu: pause menu {}", g_pauseOk ? "probe ready" : "probe disabled (bytes differ) -- only the iDroid/popups are seen");
	}

	bool PauseOpen()
	{
		if (!g_pauseOk.load(std::memory_order_relaxed)) {
			return false;
		}
		const auto holder = Get<std::uintptr_t>(addr::kPauseMenuHolder);
		const auto pm = holder && *holder ? Get<std::uintptr_t>(*holder + 8) : std::nullopt;
		if (!pm || !*pm) {
			return false;  // not built yet (title, loading)
		}
		if (Get<std::uintptr_t>(*pm) != addr::kPauseMenuVtable) {
			if (g_badObject.fetch_add(1) == 0) {
				Log::Warn("menu: the pause menu object at {:#x} has another vtable -- pause menu not seen", *pm);
			}
			return false;
		}
		return Get<std::uint32_t>(*pm + addr::kPauseMenuOpen).value_or(0) == 1;
	}

	bool Open()
	{
		return PauseOpen() || LuaMenu();
	}

	void Poll(std::uint64_t a_nowMs)
	{
		const bool pause = PauseOpen();
		const bool open = pause || LuaMenu();
		state::SetMenuOpen(open);
		if (open != g_last.exchange(open)) {
			if (open) {
				g_opens.fetch_add(1);
				g_pauses.fetch_add(pause ? 1 : 0);
				g_openedMs = a_nowMs;
				g_lastWhy = pause ? 1 : 2;
				Log::Info("menu: MGSV {} opened -- kGuestInMenu", pause ? "pause menu" : "iDroid/popup");
			} else {
				Log::Info("menu: closed after {} ms", a_nowMs - g_openedMs.load());
			}
		}
	}

	std::string Describe()
	{
		const int why = g_lastWhy.load();
		return std::format("menu: {} (pause probe {}, iDroid/popup via Lua), {} opened ({} pause){}", g_last.load() ? (why == 1 ? "PAUSE MENU OPEN" : "IDROID/POPUP OPEN") : "closed",
			g_pauseOk.load() ? "on" : "off", g_opens.load(), g_pauses.load(), g_badObject.load() ? " (pause object vtable mismatch seen)" : "");
	}
}
