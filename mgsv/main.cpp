// dinput8.dll proxy: MGSV statically imports DINPUT8!DirectInput8Create, so the loader maps this DLL
// into the game before its entry point runs (the injection point IHHook, tpp-mod and MGS5VR use on this
// exe). Every export forwards to the real system dinput8, loaded by full path on first use. PhantomRing
// itself only activates under the launcher (PHANTOMRING=1) on the exact supported exe (Guest.cpp);
// otherwise this is a plain passthrough proxy.
#include "Config.h"
#include "DInputWrap.h"
#include "Guest.h"
#include "Log.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

namespace
{
	HMODULE g_self{ nullptr };

	HMODULE RealDinput()
	{
		static std::once_flag once;
		static HMODULE        real = nullptr;
		std::call_once(once, [] {
			wchar_t    dir[MAX_PATH];
			const auto n = ::GetSystemDirectoryW(dir, MAX_PATH);
			if (!n || n >= MAX_PATH) {
				return;
			}
			const std::wstring path = std::wstring(dir, n) + L"\\dinput8.dll";
			real = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			if (!real) {
				real = ::LoadLibraryW(path.c_str());
			}
			if (real == g_self) {
				// Would forward to ourselves forever (a misconfigured DLL search path).
				::FreeLibrary(real);
				real = nullptr;
			}
			phantom::Log::Info("proxy: real dinput8 {} ({})", phantom::Narrow(path), real ? "loaded" : "NOT FOUND");
		});
		return real;
	}

	template <class T>
	T Real(const char* a_name)
	{
		const auto mod = RealDinput();
		return mod ? reinterpret_cast<T>(reinterpret_cast<void*>(::GetProcAddress(mod, a_name))) : nullptr;
	}
}

extern "C"
{
	HRESULT WINAPI DirectInput8Create(HINSTANCE a_instance, DWORD a_version, REFIID a_iid, LPVOID* a_out, LPUNKNOWN a_outer)
	{
		using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
		const auto real = Real<Fn>("DirectInput8Create");
		if (!real) {
			if (a_out) {
				*a_out = nullptr;
			}
			return E_FAIL;
		}
		const HRESULT hr = real(a_instance, a_version, a_iid, a_out, a_outer);
		if (phantom::guest::Active()) {
			if (SUCCEEDED(hr) && a_out && *a_out && !a_outer && phantom::guest::Cfg().hideDirectInputControllers &&
				IsEqualGUID(a_iid, phantom::guest::dinput::kIID_IDirectInput8W)) {
				*a_out = phantom::guest::dinput::WrapW(static_cast<IUnknown*>(*a_out));
				phantom::Log::Info("proxy: IDirectInput8W wrapped (game controllers hidden from MGSV)");
			}
			// The first game import call happens outside the loader lock: finish what DllMain couldn't.
			phantom::guest::LateInit();
		}
		return hr;
	}

	HRESULT WINAPI DllCanUnloadNow()
	{
		return S_FALSE;  // detours live in this module: never unload
	}

	HRESULT WINAPI DllGetClassObject(REFCLSID a_clsid, REFIID a_iid, LPVOID* a_out)
	{
		using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
		const auto real = Real<Fn>("DllGetClassObject");
		if (!real) {
			if (a_out) {
				*a_out = nullptr;
			}
			return CLASS_E_CLASSNOTAVAILABLE;
		}
		return real(a_clsid, a_iid, a_out);
	}

	HRESULT WINAPI DllRegisterServer()
	{
		const auto real = Real<HRESULT(WINAPI*)()>("DllRegisterServer");
		return real ? real() : E_FAIL;
	}

	HRESULT WINAPI DllUnregisterServer()
	{
		const auto real = Real<HRESULT(WINAPI*)()>("DllUnregisterServer");
		return real ? real() : E_FAIL;
	}

	LPCDIDATAFORMAT WINAPI GetdfDIJoystick()
	{
		const auto real = Real<LPCDIDATAFORMAT(WINAPI*)()>("GetdfDIJoystick");
		return real ? real() : nullptr;
	}
}

BOOL WINAPI DllMain(HINSTANCE a_instance, DWORD a_reason, LPVOID)
{
	if (a_reason == DLL_PROCESS_ATTACH) {
		g_self = a_instance;
		::DisableThreadLibraryCalls(a_instance);
		// Pin: hooks and the worker threads live in this module for the rest of the process.
		HMODULE pinned = nullptr;
		::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
		phantom::guest::OnProcessAttach(a_instance);
	}
	return TRUE;
}
