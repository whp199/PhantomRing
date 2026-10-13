// Offline test of the MGSV guest's dinput8 proxy (run under wine with WINEDLLOVERRIDES=dinput8=n,b,
// target check_guest):
//
//   test_guest_proxy <path to built dinput8.dll>
//
// 1. A dummy exe (this one) loads the proxy and calls DirectInput8Create: it must get a working
//    IDirectInput8W from the real system dinput8, with PhantomRing NOT activating -- once without
//    PHANTOMRING and once with PHANTOMRING=1 (the exe identity check must refuse this exe). Each case
//    runs in a child process because the DLL decides once, in DllMain.
// 2. The IDirectInput8W wrapper (mgsv/DInputWrap.cpp, linked in directly) hides game controllers and
//    passes everything else through, checked against a fake DirectInput that reports a gamepad, a
//    joystick and a keyboard.
#include "DInputWrap.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(x)                                                    \
	do {                                                            \
		if (!(x)) {                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			++g_failures;                                           \
		}                                                           \
	} while (0)

// ---- fake IDirectInput8W ------------------------------------------------------------------------
struct FakeDI final : IDirectInput8W
{
	ULONG refs{ 1 };
	int   enumCalls{ 0 };

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** a_out) override
	{
		*a_out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
	ULONG STDMETHODCALLTYPE Release() override { return --refs; }
	HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID, LPDIRECTINPUTDEVICE8W*, LPUNKNOWN) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE EnumDevices(DWORD a_type, LPDIENUMDEVICESCALLBACKW a_cb, LPVOID a_ref, DWORD) override
	{
		++enumCalls;
		const DWORD types[] = { DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8), DI8DEVTYPE_KEYBOARD, DI8DEVTYPE_JOYSTICK, DI8DEVTYPE_MOUSE };
		for (const auto t : types) {
			DIDEVICEINSTANCEW d{};
			d.dwSize = sizeof(d);
			d.dwDevType = t;
			if (a_cb(&d, a_ref) == DIENUM_STOP) {
				break;
			}
		}
		return DI_OK;
	}
	HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID) override { return DI_OK; }
	HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return DI_OK; }
	HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD) override { return DI_OK; }
	HRESULT STDMETHODCALLTYPE FindDevice(REFGUID, LPCWSTR, LPGUID) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCWSTR, LPDIACTIONFORMATW, LPDIENUMDEVICESBYSEMANTICSCBW, LPVOID, DWORD) override { return DI_OK; }
	HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK, LPDICONFIGUREDEVICESPARAMSW, DWORD, LPVOID) override { return DI_OK; }
};

static BOOL CALLBACK Collect(LPCDIDEVICEINSTANCEW a_device, LPVOID a_ref)
{
	static_cast<std::vector<DWORD>*>(a_ref)->push_back(GET_DIDEVICE_TYPE(a_device->dwDevType));
	return DIENUM_CONTINUE;
}

static void TestWrapper()
{
	FakeDI fake;
	auto*  wrapped = static_cast<IDirectInput8W*>(phantom::guest::dinput::WrapW(&fake));
	CHECK(wrapped != nullptr);

	std::vector<DWORD> seen;
	CHECK(wrapped->EnumDevices(DI8DEVCLASS_ALL, &Collect, &seen, DIEDFL_ATTACHEDONLY) == DI_OK);
	CHECK(seen.size() == 2);  // keyboard + mouse; the gamepad and joystick are hidden
	for (auto t : seen) {
		CHECK(t == DI8DEVTYPE_KEYBOARD || t == DI8DEVTYPE_MOUSE);
	}

	seen.clear();
	const int before = fake.enumCalls;
	CHECK(wrapped->EnumDevices(DI8DEVCLASS_GAMECTRL, &Collect, &seen, DIEDFL_ATTACHEDONLY) == DI_OK);
	CHECK(seen.empty());
	CHECK(fake.enumCalls == before);  // answered without asking the real one

	seen.clear();
	CHECK(wrapped->EnumDevices(DI8DEVTYPE_GAMEPAD, &Collect, &seen, 0) == DI_OK);
	CHECK(seen.empty());
	CHECK(phantom::guest::dinput::HiddenDevices() == 2);

	// QueryInterface for IDirectInput8W hands back the wrapper, not the real object.
	void* again = nullptr;
	CHECK(wrapped->QueryInterface(phantom::guest::dinput::kIID_IDirectInput8W, &again) == S_OK && again == wrapped);
	CHECK(wrapped->Release() == 1);
	CHECK(fake.refs == 1);  // the wrapper still holds the real reference
	CHECK(wrapped->Release() == 0);
	CHECK(fake.refs == 0);  // ... and gave it back when it died
}

// ---- the real proxy -------------------------------------------------------------------------------
static int Child(const wchar_t* a_dll)
{
	HMODULE dll = ::LoadLibraryW(a_dll);
	if (!dll) {
		std::printf("child: LoadLibrary failed (%lu)\n", ::GetLastError());
		return 2;
	}
	wchar_t loaded[MAX_PATH];
	::GetModuleFileNameW(dll, loaded, MAX_PATH);
	if (_wcsicmp(loaded, a_dll) != 0) {
		std::printf("child: loaded %ls instead of the proxy (WINEDLLOVERRIDES=dinput8=n,b missing?)\n", loaded);
		return 3;
	}
	using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
	auto create = reinterpret_cast<Create>(reinterpret_cast<void*>(::GetProcAddress(dll, "DirectInput8Create")));
	if (!create) {
		return 4;
	}
	IDirectInput8W* di = nullptr;
	const HRESULT   hr = create(::GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, phantom::guest::dinput::kIID_IDirectInput8W, reinterpret_cast<void**>(&di), nullptr);
	if (FAILED(hr) || !di) {
		std::printf("child: DirectInput8Create failed (%08lx)\n", static_cast<unsigned long>(hr));
		return 5;
	}
	std::vector<DWORD> seen;
	const HRESULT      e = di->EnumDevices(DI8DEVCLASS_ALL, &Collect, &seen, DIEDFL_ATTACHEDONLY);
	std::printf("child: real DirectInput8 via proxy ok, EnumDevices %08lx, %zu devices\n", static_cast<unsigned long>(e), seen.size());
	di->Release();
	// The other exports forward too.
	using Unload = HRESULT(WINAPI*)();
	auto canUnload = reinterpret_cast<Unload>(reinterpret_cast<void*>(::GetProcAddress(dll, "DllCanUnloadNow")));
	if (!canUnload || canUnload() != S_FALSE) {
		return 6;
	}
	if (!::GetProcAddress(dll, "GetdfDIJoystick") || !::GetProcAddress(dll, "DllGetClassObject")) {
		return 7;
	}
	return SUCCEEDED(e) ? 0 : 8;
}

static std::string ReadLog(const std::wstring& a_dll)
{
	const auto     dir = a_dll.substr(0, a_dll.find_last_of(L"\\/") + 1);
	std::ifstream  in(std::filesystem::path(dir + L"PhantomRingGuest.log"));
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

static int RunChild(const std::wstring& a_self, const std::wstring& a_dll, bool a_env)
{
	if (a_env) {
		::SetEnvironmentVariableW(L"PHANTOMRING", L"1");
	} else {
		::SetEnvironmentVariableW(L"PHANTOMRING", nullptr);
	}
	std::wstring        cmd = L"\"" + a_self + L"\" --child \"" + a_dll + L"\"";
	STARTUPINFOW        si{ sizeof(si) };
	PROCESS_INFORMATION pi{};
	if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
		return -1;
	}
	::WaitForSingleObject(pi.hProcess, 60000);
	DWORD code = 0;
	::GetExitCodeProcess(pi.hProcess, &code);
	::CloseHandle(pi.hProcess);
	::CloseHandle(pi.hThread);
	return static_cast<int>(code);
}

int wmain(int argc, wchar_t** argv)
{
	if (argc == 3 && std::wcscmp(argv[1], L"--child") == 0) {
		return Child(argv[2]);
	}
	if (argc != 2) {
		std::printf("usage: test_guest_proxy <full path to dinput8.dll>\n");
		return 2;
	}
	wchar_t self[MAX_PATH];
	::GetModuleFileNameW(nullptr, self, MAX_PATH);
	wchar_t dll[MAX_PATH];
	::GetFullPathNameW(argv[1], MAX_PATH, dll, nullptr);

	TestWrapper();

	CHECK(RunChild(self, dll, false) == 0);
	auto log = ReadLog(dll);
	CHECK(log.find("PHANTOMRING is not 1: plain dinput8 proxy") != std::string::npos);
	CHECK(log.find("activating") == std::string::npos);

	CHECK(RunChild(self, dll, true) == 0);
	log = ReadLog(dll);
	CHECK(log.find("is not mgsvtpp.exe 1.0.15.4") != std::string::npos);
	CHECK(log.find("plain dinput8 proxy") != std::string::npos);
	CHECK(log.find("activating") == std::string::npos);
	CHECK(log.find("real dinput8") != std::string::npos);

	if (g_failures) {
		std::printf("test_guest_proxy: %d FAILED\n--- last PhantomRingGuest.log ---\n%s\n", g_failures, log.c_str());
		return 1;
	}
	std::printf("test_guest_proxy: all passed\n");
	return 0;
}
