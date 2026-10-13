// PhantomRingHost.dll: the Elden Ring side of PhantomRing, loaded by me3 as a [[natives]] DLL.
// DllMain only starts a thread: identity check, address verification, shared memory, the XInput
// bridge, then (after the game's system init) the per-frame tasks. Anything unexpected leaves the
// game untouched.

#include "Addresses.h"
#include "Config.h"
#include "Host.h"
#include "Ini.h"
#include "InputBridge.h"
#include "Launcher.h"
#include "Link.h"
#include "Log.h"
#include "MenuWatch.h"
#include "Renderer.h"
#include "Singletons.h"
#include "Tasks.h"

#include <windows.h>

namespace
{
	constexpr char kVersion[] = "0.1.0";

	DWORD WINAPI Init(LPVOID)
	{
		using namespace phantom;
		using namespace phantom::er;
		const auto dir = ModuleDir();
		Log::Init(dir + L"PhantomRingHost.log", "host");
		Log::Info("PhantomRingHost {} (protocol v{}) loading from {}", kVersion, proto::kVersion, Narrow(dir));

		auto& cfg = Cfg();
		if (!cfg.Load(dir + L"PhantomRingHost.ini")) {
			Log::Warn("PhantomRingHost.ini not found next to the DLL; using defaults");
		}
		Log::SetDebug(cfg.debug);

		if (!CheckExeIdentity(cfg.verifySha256)) {
			return 0;
		}
		if (const int failures = ResolveAddresses()) {
			Log::Warn("{} addresses failed verification; the features using them stay off", failures);
		}
		if (!Link::Get().Open(cfg.mapping)) {
			Log::Error("no shared memory at {}: PhantomRing disabled, Elden Ring runs normally", Narrow(cfg.mapping));
			return 0;
		}
		// Windows: start MGSV (hidden, with its guest) unless one is already there (er/Launcher.h).
		Launcher::Start(Launcher::Load(dir + L"PhantomRingHost.ini"), cfg.mapping);
		if (cfg.input) {
			InputBridge::Install();
		}
		// Before the game's graphics init if we can: the swap-chain creation hook then names the queue.
		Renderer::Install();
		MenuWatch::Init();
		if (!WaitForSystemInit(0)) {
			return 0;
		}
		FindSingletons();
		if (!Host::Start()) {
			Log::Error("could not register the frame tasks; PhantomRing idle");
			Link::Get().SetHostStatus(proto::Status::kFailed, "host tasks failed");
			return 0;
		}
		Log::Info("PhantomRingHost ready");
		return 0;
	}
}

BOOL WINAPI DllMain(HINSTANCE a_instance, DWORD a_reason, LPVOID)
{
	if (a_reason == DLL_PROCESS_ATTACH) {
		::DisableThreadLibraryCalls(a_instance);
		// No real work under the loader lock.
		if (HANDLE thread = ::CreateThread(nullptr, 0, &Init, nullptr, 0, nullptr)) {
			::CloseHandle(thread);
		}
	}
	return TRUE;
}
