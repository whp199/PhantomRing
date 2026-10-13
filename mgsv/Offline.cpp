#include "Offline.h"

#include "Addresses.h"
#include "Config.h"
#include "Log.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstring>
#include <cwctype>

namespace phantom::guest::offline
{
	namespace
	{
		using ConnectFn = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);

		ConnectFn             g_real{ nullptr };
		std::atomic<bool>     g_enabled{ false };
		std::atomic<unsigned> g_refused{ 0 };

		HINTERNET WINAPI Connect(HINTERNET a_session, LPCWSTR a_server, INTERNET_PORT a_port, DWORD a_reserved)
		{
			const std::wstring server = a_server ? a_server : L"";
			if (g_enabled.load(std::memory_order_acquire)) {
				const auto n = g_refused.fetch_add(1) + 1;
				if (n <= 20 || n % 100 == 0) {
					Log::Info("offline: refused MGSV connection to {}:{} (#{})", Narrow(server), a_port, n);
				}
				::SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
				return nullptr;
			}
			Log::Info("online: MGSV connects to {}:{}", Narrow(server), a_port);
			return g_real(a_session, a_server, a_port, a_reserved);
		}

		bool InModule(const void* a_fn, const wchar_t* a_name)
		{
			HMODULE mod = nullptr;
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a_fn), &mod)) {
				return false;
			}
			return mod == ::GetModuleHandleW(a_name);
		}
	}

	bool Install()
	{
		g_enabled.store(Cfg().offline, std::memory_order_release);
		auto*       base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		auto*       slot = reinterpret_cast<void**>(addr::kWinHttpConnectIat);
		bool        found = false;
		if (dir.VirtualAddress) {
			for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name && !found; ++d) {
				if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "WINHTTP.dll") != 0) {
					continue;
				}
				for (auto* t = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk); t->u1.Function; ++t) {
					found |= reinterpret_cast<void**>(&t->u1.Function) == slot;
				}
			}
		}
		if (!found || !InModule(*slot, L"winhttp.dll") || *slot != reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"winhttp.dll"), "WinHttpConnect"))) {
			Log::Error("offline: {:#x} is not MGSV's WinHttpConnect import -- MGSV may go online{}", addr::kWinHttpConnectIat,
				Cfg().offline ? " (and stop at Konami's region/terms screen)" : "");
			return false;
		}
		g_real = reinterpret_cast<ConnectFn>(*slot);
		DWORD old = 0;
		if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
			Log::Error("offline: can't unprotect the IAT ({})", ::GetLastError());
			return false;
		}
		const auto prior = ::InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(&Connect), reinterpret_cast<void*>(g_real));
		::VirtualProtect(slot, sizeof(void*), old, &old);
		if (prior != reinterpret_cast<void*>(g_real)) {
			Log::Error("offline: the IAT slot changed under us");
			return false;
		}
		Log::Info("offline: MGSV's WinHttpConnect import {} (Konami servers {})", "patched", Cfg().offline ? "unreachable: MGSV plays offline" : "reachable");
		return true;
	}

	bool     Enabled() { return g_enabled.load(); }
	unsigned Refused() { return g_refused.load(); }
}
