#include "Kbm.h"

#include "Config.h"
#include "Hook.h"
#include "InputBridge.h"
#include "KbmFilter.h"
#include "Log.h"

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>

namespace phantom::er::Kbm
{
	namespace
	{
		// dinput.h's GUIDs without dxguid (copied from it).
		constexpr GUID kIidDirectInput8W{ 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
		constexpr GUID kIidDirectInput8A{ 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
		constexpr GUID kGuidSysMouse{ 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
		constexpr GUID kGuidSysKeyboard{ 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

		constexpr std::size_t kSlotGetDeviceState = 9;  // IDirectInputDevice8: QI, AddRef, Release, GetCapabilities, EnumObjects,
		constexpr std::size_t kSlotGetDeviceData = 10;  // GetProperty, SetProperty, Acquire, Unacquire, GetDeviceState, GetDeviceData
		constexpr std::size_t kSlotGetCapabilities = 3;
		constexpr int         kMaxTargets = 4;          // W and A interfaces x keyboard and mouse, if none share code
		constexpr ULONGLONG   kTapMs = 120;             // the chord key's tap to Elden Ring: a few of its frames

		using StateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
		using DataFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
		using CapsFn = HRESULT(STDMETHODCALLTYPE*)(void*, LPDIDEVCAPS);
		using Create8Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

		bool             g_installed{ false };
		void*            g_stateTargets[kMaxTargets]{};
		StateFn          g_stateOrig[kMaxTargets]{};
		void*            g_dataTargets[kMaxTargets]{};
		DataFn           g_dataOrig[kMaxTargets]{};

		// Set by Tick (game thread), read by the DirectInput and XInput detours (Elden Ring's input threads).
		std::atomic<int>       g_mode{ 0 };             // Mode packed (PackMode): scope and MGSV's menu in one load, never torn apart
		std::atomic<bool>      g_useKeyboard{ false };  // kFull and the keyboard is the device in use: Apply puts its pad there
		std::atomic<bool>      g_chordHeld{ false };    // kChordOnly and the chord key held: Apply adds Start + View
		std::atomic<std::uint64_t> g_padA{ 0 };         // PadOut packed: buttons | lt << 16 | rt << 24 | lx << 32 | ly << 48
		std::atomic<std::uint32_t> g_padB{ 0 };         // rx | ry << 16
		std::atomic<ULONGLONG> g_tapUntil{ 0 };         // the chord key reaches Elden Ring's keyboard until then
		std::atomic<int>       g_tapPhase{ 0 };         // buffered reads: 1 = its key-down still to deliver, 2 = its key-up
		std::atomic<ULONGLONG> g_viewTapUntil{ 0 };     // chord_tap = view: the pad carries View until then
		std::atomic<ULONGLONG> g_padActiveAt{ 0 };      // the real pad was last in use then (Apply)

		// Elden Ring's DirectInput mouse, counted before it is masked; Tick takes them.
		std::atomic<long> g_mouseDx{ 0 }, g_mouseDy{ 0 }, g_mouseDz{ 0 };

		// DIK (scan code) tables, built from the bindings (KbmFilter DikTables). `input kbm on|off` rebuilds them on the game
		// thread while Elden Ring's input threads read them: built into the spare copy, then published by one index store,
		// so a reader (which takes the index once per read) never sees a half-built one or no chord key.
		std::array<DikTables, 2> g_tables{};
		std::atomic<int>         g_tablesIdx{ 0 };
		std::vector<int>         g_watched;             // every VK any binding uses (polled each frame; game thread only)

		// Diagnostics (`input kbm`).
		std::atomic<std::uint64_t> g_kbStateReads{ 0 }, g_mouseStateReads{ 0 }, g_kbDataReads{ 0 }, g_mouseDataReads{ 0 }, g_applied{ 0 };
		std::atomic<std::uint32_t> g_stateSizeSeen{ 0 };
		std::uint64_t              g_switches{ 0 }, g_taps{ 0 }, g_virtualPadOns{ 0 };

		// Game thread only.
		MouseStick    g_stick;
		Mapper        g_mapper;
		Arbiter       g_arbiter;
		ChordTap      g_chordTap;
		std::array<bool, kKeyCodes> g_prev{};
		std::array<bool, kKeyCodes> g_heldOver{};  // held when Snake took the keyboard (an Elden Ring menu's confirm): ignored until released
		Scope         g_lastScope{ Scope::kNone };
		long          g_wheelAcc{ 0 };
		LARGE_INTEGER g_lastTick{};
		ULONGLONG     g_lastTickMs{ 0 };
		bool          g_clipped{ false };
		RECT          g_clipRect{};
		float         g_lastSpeed{ 0.0f };

		int Dik(int a_vk)
		{
			if (a_vk <= 0x06 || a_vk > 0xFF) {
				return -1;  // mouse buttons and the wheel aren't keyboard keys
			}
			const UINT sc = ::MapVirtualKeyW(static_cast<UINT>(a_vk), MAPVK_VK_TO_VSC_EX);
			if ((sc & 0xFF) == 0) {
				return -1;
			}
			return static_cast<int>((sc & 0x7F) | ((sc & 0xFF00) ? 0x80 : 0));  // DIK_* = the scan code, 0x80 for an E0 (extended) one
		}

		const DikTables& Tables() { return g_tables[static_cast<std::size_t>(g_tablesIdx.load(std::memory_order_acquire))]; }

		int  PackMode(Mode a_m) { return static_cast<int>(a_m.scope) | (a_m.guestMenu ? 0x100 : 0); }
		Mode CurrentMode()
		{
			const int m = g_mode.load(std::memory_order_relaxed);
			return { static_cast<Scope>(m & 0xFF), (m & 0x100) != 0 };
		}

		void BuildTables(const Params& a_p)
		{
			const int spare = 1 - g_tablesIdx.load(std::memory_order_relaxed);
			g_tables[static_cast<std::size_t>(spare)] = DikTables::Build(a_p, &Dik);
			g_tablesIdx.store(spare, std::memory_order_release);
			g_watched.clear();
			for (const auto& list : a_p.keys) {
				for (const int code : list) {
					if (code > 0 && code <= 0xFF && std::ranges::find(g_watched, code) == g_watched.end()) {
						g_watched.push_back(code);
					}
				}
			}
		}

		// DirectInput threads only (Elden Ring reads each device from one thread). KbmFilter.h has the rules: a key hidden
		// while held stays hidden until let go; a buffered release is never lost.
		SnapshotFilter g_snapshot;
		EventFilter    g_kbEvents;     // GetDeviceData keyboard: DIKs
		EventFilter    g_mouseEvents;  // ...mouse: buttons 0..7

		void OnKeyboardState(std::uint8_t* a_keys)
		{
			g_kbStateReads.fetch_add(1, std::memory_order_relaxed);
			const auto& tables = Tables();
			g_snapshot.Keyboard(a_keys, tables, CurrentMode(), ::GetTickCount64() < g_tapUntil.load(std::memory_order_relaxed));
		}

		void OnMouseState(DWORD a_size, void* a_data)
		{
			g_mouseStateReads.fetch_add(1, std::memory_order_relaxed);
			auto* m = static_cast<DIMOUSESTATE*>(a_data);
			g_mouseDx.fetch_add(m->lX, std::memory_order_relaxed);
			g_mouseDy.fetch_add(m->lY, std::memory_order_relaxed);
			g_mouseDz.fetch_add(m->lZ, std::memory_order_relaxed);
			auto*       buttons = static_cast<std::uint8_t*>(a_data) + offsetof(DIMOUSESTATE, rgbButtons);
			const DWORD count = std::min<DWORD>(a_size - static_cast<DWORD>(offsetof(DIMOUSESTATE, rgbButtons)), 8);
			if (!g_snapshot.Mouse(buttons, count, CurrentMode().scope)) {
				// Snake's (or an interaction's): Elden Ring's camera and its attack/guard never see it (the hidden Tarnished
				// would swing). The filter zeroed the buttons.
				m->lX = m->lY = m->lZ = 0;
			}
		}

		template <int N>
		HRESULT STDMETHODCALLTYPE StateDetour(void* a_self, DWORD a_size, LPVOID a_data)
		{
			const HRESULT hr = g_stateOrig[N](a_self, a_size, a_data);
			if (SUCCEEDED(hr) && a_data) {
				g_stateSizeSeen.store(a_size, std::memory_order_relaxed);
				if (a_size == 256) {
					OnKeyboardState(static_cast<std::uint8_t*>(a_data));
				} else if (a_size == sizeof(DIMOUSESTATE) || a_size == sizeof(DIMOUSESTATE2)) {
					OnMouseState(a_size, a_data);
				}
			}
			return hr;
		}
		constexpr StateFn kStateDetours[kMaxTargets]{ &StateDetour<0>, &StateDetour<1>, &StateDetour<2>, &StateDetour<3> };

		// A buffered device's kind (GetDeviceData doesn't say), asked once per device.
		struct Known
		{
			void* device{ nullptr };
			BYTE  type{ 0 };
		};
		Known            g_known[8];
		std::atomic_flag g_knownLock = ATOMIC_FLAG_INIT;

		BYTE DeviceType(void* a_self)
		{
			while (g_knownLock.test_and_set(std::memory_order_acquire)) {
			}
			BYTE type = 0xFF;
			for (const auto& k : g_known) {
				if (k.device == a_self) {
					type = k.type;
				}
			}
			g_knownLock.clear(std::memory_order_release);
			if (type != 0xFF) {
				return type;
			}
			DIDEVCAPS caps{};
			caps.dwSize = sizeof(caps);
			auto** vtbl = *static_cast<void***>(a_self);
			type = SUCCEEDED(reinterpret_cast<CapsFn>(vtbl[kSlotGetCapabilities])(a_self, &caps)) ? static_cast<BYTE>(caps.dwDevType & 0xFF) : 0;
			while (g_knownLock.test_and_set(std::memory_order_acquire)) {
			}
			for (auto& k : g_known) {
				if (!k.device) {
					k = { a_self, type };
					break;
				}
			}
			g_knownLock.clear(std::memory_order_release);
			return type;
		}

		// One made-up event (a release, the chord key's tap) after the a_count delivered ones; false = no room.
		bool Append(std::uint8_t* a_items, DWORD a_stride, DWORD& a_count, DWORD a_capacity, DWORD a_ofs, DWORD a_data)
		{
			if (a_count >= a_capacity) {
				return false;
			}
			auto*       item = reinterpret_cast<DIDEVICEOBJECTDATA*>(a_items + a_count * a_stride);
			const DWORD sequence = a_count > 0 ? reinterpret_cast<DIDEVICEOBJECTDATA*>(a_items + (a_count - 1) * a_stride)->dwSequence : 0;
			std::memset(item, 0, a_stride);
			item->dwOfs = a_ofs;
			item->dwData = a_data;
			item->dwTimeStamp = static_cast<DWORD>(::GetTickCount64());
			item->dwSequence = sequence;
			++a_count;
			return true;
		}

		void OnKeyboardData(DWORD a_stride, std::uint8_t* a_items, DWORD& a_count, DWORD a_capacity, bool a_peek)
		{
			g_kbDataReads.fetch_add(1, std::memory_order_relaxed);
			const auto&  tables = Tables();
			const Mode   mode = CurrentMode();
			EventFilter  peeked = g_kbEvents;  // a peek must not change what Elden Ring was told
			EventFilter& filter = a_peek ? peeked : g_kbEvents;
			DWORD        kept = 0;
			for (DWORD i = 0; i < a_count; ++i) {
				auto*     item = reinterpret_cast<DIDEVICEOBJECTDATA*>(a_items + i * a_stride);
				const int dik = static_cast<int>(item->dwOfs & 0xFF);
				if (!filter.Pass(static_cast<std::size_t>(dik), (item->dwData & 0x80) != 0, ErKeeps(tables, dik, mode))) {
					continue;
				}
				if (kept != i) {
					std::memmove(a_items + kept * a_stride, item, a_stride);
				}
				++kept;
			}
			a_count = kept;
			// Keys Elden Ring has down that it may not have now (Snake took over under a held W): their release, room
			// allowing (the rest on the next read).
			while (a_count < a_capacity) {
				const int dik = filter.NextRelease([&](std::size_t a_dik) { return ErKeeps(tables, static_cast<int>(a_dik), mode); });
				if (dik < 0) {
					break;
				}
				Append(a_items, a_stride, a_count, a_capacity, static_cast<DWORD>(dik), 0);
			}
			// The chord key's tap: a key-down now and its key-up once the tap is over (one event each, if there is room).
			const int  phase = g_tapPhase.load(std::memory_order_relaxed);
			const bool due = phase == 1 || (phase == 2 && ::GetTickCount64() >= g_tapUntil.load(std::memory_order_relaxed));
			if (due && tables.tap >= 0 && Append(a_items, a_stride, a_count, a_capacity, static_cast<DWORD>(tables.tap), phase == 1 ? 0x80 : 0) && !a_peek) {
				g_tapPhase.store(phase == 1 ? 2 : 0, std::memory_order_relaxed);
			}
		}

		void OnMouseData(DWORD a_stride, std::uint8_t* a_items, DWORD& a_count, DWORD a_capacity, bool a_peek)
		{
			g_mouseDataReads.fetch_add(1, std::memory_order_relaxed);
			const bool   gets = ErGetsMouse(CurrentMode().scope);
			EventFilter  peeked = g_mouseEvents;
			EventFilter& filter = a_peek ? peeked : g_mouseEvents;
			DWORD        kept = 0;
			for (DWORD i = 0; i < a_count; ++i) {
				auto*      item = reinterpret_cast<DIDEVICEOBJECTDATA*>(a_items + i * a_stride);
				const auto delta = static_cast<long>(static_cast<LONG>(item->dwData));
				bool       pass = gets;  // movement and the wheel: only while Elden Ring has the mouse
				switch (item->dwOfs) {
				case DIMOFS_X:
					if (!a_peek) {
						g_mouseDx.fetch_add(delta, std::memory_order_relaxed);
					}
					break;
				case DIMOFS_Y:
					if (!a_peek) {
						g_mouseDy.fetch_add(delta, std::memory_order_relaxed);
					}
					break;
				case DIMOFS_Z:
					if (!a_peek) {
						g_mouseDz.fetch_add(delta, std::memory_order_relaxed);
					}
					break;
				default:
					// A button: its press only while Elden Ring has the mouse, its release whenever it had the press.
					if (item->dwOfs >= DIMOFS_BUTTON0 && item->dwOfs <= DIMOFS_BUTTON7) {
						pass = filter.Pass(item->dwOfs - DIMOFS_BUTTON0, (item->dwData & 0x80) != 0, gets);
					}
					break;
				}
				if (!pass) {
					continue;
				}
				if (kept != i) {
					std::memmove(a_items + kept * a_stride, item, a_stride);
				}
				++kept;
			}
			a_count = kept;
			while (a_count < a_capacity) {
				const int button = filter.NextRelease([&](std::size_t) { return gets; });
				if (button < 0) {
					break;
				}
				Append(a_items, a_stride, a_count, a_capacity, static_cast<DWORD>(DIMOFS_BUTTON0 + button), 0);
			}
		}

		template <int N>
		HRESULT STDMETHODCALLTYPE DataDetour(void* a_self, DWORD a_stride, LPDIDEVICEOBJECTDATA a_items, LPDWORD a_inOut, DWORD a_flags)
		{
			const DWORD   capacity = a_inOut ? *a_inOut : 0;
			const HRESULT hr = g_dataOrig[N](a_self, a_stride, a_items, a_inOut, a_flags);
			if (SUCCEEDED(hr) && a_items && a_inOut && a_stride >= sizeof(DIDEVICEOBJECTDATA_DX3)) {
				const bool peek = (a_flags & DIGDD_PEEK) != 0;
				switch (DeviceType(a_self)) {
				case DI8DEVTYPE_KEYBOARD:
					OnKeyboardData(a_stride, reinterpret_cast<std::uint8_t*>(a_items), *a_inOut, capacity, peek);
					break;
				case DI8DEVTYPE_MOUSE:
					OnMouseData(a_stride, reinterpret_cast<std::uint8_t*>(a_items), *a_inOut, capacity, peek);
					break;
				default:
					break;
				}
			}
			return hr;
		}
		constexpr DataFn kDataDetours[kMaxTargets]{ &DataDetour<0>, &DataDetour<1>, &DataDetour<2>, &DataDetour<3> };

		// One hook per distinct function: Windows' dinput8 shares one vtable between devices, older Wine has one per kind.
		template <class F>
		void HookSlot(void* a_target, void* (&a_targets)[kMaxTargets], F (&a_orig)[kMaxTargets], const F (&a_detours)[kMaxTargets], const char* a_what)
		{
			for (int i = 0; i < kMaxTargets; ++i) {
				if (a_targets[i] == a_target) {
					return;
				}
				if (!a_targets[i]) {
					if (Hooks::Create(std::format("dinput8!{}#{}", a_what, i), a_target, reinterpret_cast<void*>(a_detours[i]), reinterpret_cast<void**>(&a_orig[i]))) {
						a_targets[i] = a_target;
					}
					return;
				}
			}
		}

		bool OurWindowInFront(HWND& a_hwnd)
		{
			a_hwnd = ::GetForegroundWindow();
			DWORD pid = 0;
			if (a_hwnd) {
				::GetWindowThreadProcessId(a_hwnd, &pid);
			}
			return a_hwnd && pid == ::GetCurrentProcessId();
		}

		void ReleaseCursor()
		{
			if (!g_clipped) {
				return;
			}
			RECT now{};
			// Only our own clip is undone: Elden Ring may have set one of its own meanwhile.
			if (::GetClipCursor(&now) && ::EqualRect(&now, &g_clipRect)) {
				::ClipCursor(nullptr);
			}
			g_clipped = false;
		}

		void CaptureCursor(HWND a_hwnd)
		{
			RECT r{};
			if (!::GetClientRect(a_hwnd, &r)) {
				return;
			}
			::MapWindowPoints(a_hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
			RECT now{};
			if (!::GetClipCursor(&now) || !::EqualRect(&now, &r)) {
				::ClipCursor(&r);
			}
			g_clipRect = r;
			g_clipped = true;
		}

		const char* ScopeName(Scope a_scope)
		{
			switch (a_scope) {
			case Scope::kFull:
				return "Snake";
			case Scope::kButtonsOnly:
				return "Elden Ring interaction (its interaction keys only, no mouse)";
			case Scope::kChordOnly:
				return "Tarnished (chord key only)";
			default:
				return "Elden Ring's";
			}
		}
	}

	bool Install()
	{
		if (g_installed) {
			return true;
		}
		BuildTables(Cfg().keyboard);
		HMODULE di = ::GetModuleHandleW(L"dinput8.dll");
		if (!di) {
			di = ::LoadLibraryW(L"dinput8.dll");  // a system DLL Elden Ring imports anyway
		}
		auto* create = di ? reinterpret_cast<Create8Fn>(reinterpret_cast<void*>(::GetProcAddress(di, "DirectInput8Create"))) : nullptr;
		if (!create) {
			Log::Warn("keyboard: no dinput8!DirectInput8Create -- keyboard and mouse stay Elden Ring's");
			return false;
		}
		// Probe devices, only to find the functions every device of their kind runs (never acquired, released at once).
		for (const GUID* iid : { &kIidDirectInput8W, &kIidDirectInput8A }) {
			IDirectInput8W* dinput = nullptr;
			if (FAILED(create(::GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, *iid, reinterpret_cast<void**>(&dinput), nullptr)) || !dinput) {
				continue;
			}
			for (const GUID* kind : { &kGuidSysKeyboard, &kGuidSysMouse }) {
				IDirectInputDevice8W* device = nullptr;
				if (SUCCEEDED(dinput->CreateDevice(*kind, &device, nullptr)) && device) {
					auto** vtbl = *reinterpret_cast<void***>(device);
					HookSlot(vtbl[kSlotGetDeviceState], g_stateTargets, g_stateOrig, kStateDetours, "GetDeviceState");
					HookSlot(vtbl[kSlotGetDeviceData], g_dataTargets, g_dataOrig, kDataDetours, "GetDeviceData");
					device->Release();
				}
			}
			dinput->Release();
		}
		if (!g_stateTargets[0] || !Hooks::EnableAll()) {
			Log::Warn("keyboard: DirectInput hooks failed -- keyboard and mouse stay Elden Ring's");
			return false;
		}
		g_installed = true;
		const auto& p = Cfg().keyboard;
		Log::Info("keyboard: DirectInput hooked ({} GetDeviceState, {} GetDeviceData); [Keyboard] {} -- WASD {}, mouse sensitivity {} "
		          "acceleration {}, virtual pad {} (UNVERIFIED live)",
			std::ranges::count_if(g_stateTargets, [](void* a) { return a != nullptr; }), std::ranges::count_if(g_dataTargets, [](void* a) { return a != nullptr; }),
			p.enabled ? "ON: keyboard and mouse play Snake" : "off", p.runByDefault ? "run" : "walk", p.mouse.sensitivity, p.mouse.acceleration,
			p.virtualPad < 0 ? "auto" : (p.virtualPad ? "on" : "off"));
		return true;
	}

	bool Installed() { return g_installed; }

	void Tick()
	{
		const auto& p = Cfg().keyboard;
		if (!g_installed || !p.enabled) {
			g_mode.store(PackMode({}), std::memory_order_relaxed);
			g_useKeyboard.store(false, std::memory_order_relaxed);
			g_chordHeld.store(false, std::memory_order_relaxed);
			ReleaseCursor();
			return;
		}
		LARGE_INTEGER qpc{}, freq{};
		::QueryPerformanceCounter(&qpc);
		::QueryPerformanceFrequency(&freq);
		const float dtMs = g_lastTick.QuadPart ? static_cast<float>(static_cast<double>(qpc.QuadPart - g_lastTick.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)) : 16.7f;
		g_lastTick = qpc;
		const ULONGLONG now = ::GetTickCount64();

		HWND       hwnd = nullptr;
		const bool front = OurWindowInFront(hwnd);
		const auto modes = InputBridge::CurrentModes();
		Scope      scope = Scope::kNone;
		if (modes.buttonsOnly) {
			// An interaction before its HUD hides (the bridge's SetButtonsOnly; maskEr is off then): the keyboard's share of
			// what the pad path does there.
			scope = Scope::kButtonsOnly;
		} else if (modes.maskEr) {
			scope = Scope::kFull;
		} else if (modes.pad == PadGate::Mode::kTarnished && Cfg().chordMs > 0) {
			scope = Scope::kChordOnly;
		}

		Input in;
		in.nowMs = now;
		in.guestMenu = modes.guestMenu;
		bool edge = false;
		for (const int vk : g_watched) {
			const bool down = front && (::GetAsyncKeyState(vk) & 0x8000) != 0;
			const auto i = static_cast<std::size_t>(vk);
			if (scope == Scope::kFull && g_lastScope != Scope::kFull) {
				g_heldOver[i] = down;  // held as Snake takes the keyboard (Elden Ring's menu confirm): not Snake's until let go
			}
			g_heldOver[i] = g_heldOver[i] && down;
			edge = edge || (down && !g_prev[i]);
			g_prev[i] = down;
			in.down[i] = down && !g_heldOver[i];
		}

		const long dx = g_mouseDx.exchange(0, std::memory_order_relaxed);
		const long dy = g_mouseDy.exchange(0, std::memory_order_relaxed);
		g_wheelAcc += g_mouseDz.exchange(0, std::memory_order_relaxed);
		const int notches = static_cast<int>(g_wheelAcc / WHEEL_DELTA);
		g_wheelAcc -= static_cast<long>(notches) * WHEEL_DELTA;
		if (scope == Scope::kFull) {
			g_stick.Update(static_cast<float>(dx), static_cast<float>(dy), dtMs, p.mouse, in.mouseX, in.mouseY);
			in.wheel = notches;
		} else {
			g_stick.Reset();
			g_wheelAcc = 0;
		}
		g_lastSpeed = g_stick.Speed();

		const bool padActivity = g_padActiveAt.load(std::memory_order_relaxed) > g_lastTickMs;
		const bool kbmActivity = scope == Scope::kFull && (edge || notches != 0 || g_lastSpeed >= p.mouse.minSpeed);
		g_lastTickMs = now;
		const auto before = g_arbiter.Current();
		const auto device = g_arbiter.Update(padActivity, kbmActivity);
		if (device != before) {
			++g_switches;
			Log::Info("keyboard: Snake's pad now comes from the {}", device == Device::kKeyboard ? "keyboard and mouse" : "controller");
		}

		const auto out = g_mapper.Map(in, p);
		g_padB.store(static_cast<std::uint32_t>(static_cast<std::uint16_t>(out.rx)) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(out.ry)) << 16),
			std::memory_order_relaxed);
		g_padA.store(static_cast<std::uint64_t>(out.buttons) | (static_cast<std::uint64_t>(out.lt) << 16) | (static_cast<std::uint64_t>(out.rt) << 24) |
		                 (static_cast<std::uint64_t>(static_cast<std::uint16_t>(out.lx)) << 32) | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(out.ly)) << 48),
			std::memory_order_relaxed);
		const bool chordDown = g_mapper.Held(in, p, Action::kChord);
		g_useKeyboard.store(scope == Scope::kFull && device == Device::kKeyboard, std::memory_order_relaxed);
		g_chordHeld.store(scope == Scope::kChordOnly && chordDown, std::memory_order_relaxed);
		g_mode.store(PackMode({ scope, modes.guestMenu }), std::memory_order_relaxed);

		// Tarnished mode: the chord key let go early reaches Elden Ring as a tap ([Keyboard] chord_tap): by default the pad's
		// View (Apply below; the gate passes it on at its release and counts it for MenuWatch's latch) -- Elden Ring's map
		// whatever its own key bindings are --, or the chord key itself (counted here).
		if (g_chordTap.Update(chordDown, scope == Scope::kChordOnly, now, Cfg().chordMs)) {
			if (p.chordTap == Params::ChordTapTo::kView) {
				g_viewTapUntil.store(now + kTapMs, std::memory_order_relaxed);
				++g_taps;
				Log::Info("keyboard: chord key tapped in Tarnished mode -- passed to Elden Ring as the pad's View (its map)");
			} else if (p.chordTap == Params::ChordTapTo::kKey && Tables().tap >= 0) {
				g_tapUntil.store(now + kTapMs, std::memory_order_relaxed);
				g_tapPhase.store(1, std::memory_order_relaxed);
				InputBridge::NoteMenuPress();
				++g_taps;
				Log::Info("keyboard: chord key tapped in Tarnished mode -- passed to Elden Ring as that key (chord_tap = key)");
			}
		}

		// No controller: Elden Ring polls no XInput pad, so nothing would reach the bridge (or MGSV). The virtual pad makes it poll.
		if (p.virtualPad != 0 && !InputBridge::VirtualPadOn()) {
			const bool wanted = p.virtualPad == 1 || ((kbmActivity || chordDown) && scope != Scope::kNone && now - InputBridge::LastErPollMs() > 500);
			if (wanted) {
				++g_virtualPadOns;
				Log::Info("keyboard: {}", InputBridge::SetVirtualPad(true));
			}
		}

		if (p.captureCursor && front && g_useKeyboard.load(std::memory_order_relaxed)) {
			CaptureCursor(hwnd);
		} else {
			ReleaseCursor();
		}
		g_lastScope = scope;
	}

	unsigned long Apply(unsigned long a_index, void* a_xinputState, unsigned long a_result)
	{
		if (!g_installed || !a_xinputState || a_result != ERROR_SUCCESS) {
			return a_result;
		}
		const int pad = InputBridge::PadIndex();
		if (pad >= 0 ? static_cast<int>(a_index) != pad : a_index != 0) {
			return a_result;
		}
		auto* s = static_cast<XINPUT_STATE*>(a_xinputState);
		auto& g = s->Gamepad;
		if (PadActive(g.wButtons, g.bLeftTrigger, g.bRightTrigger, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY)) {
			g_padActiveAt.store(::GetTickCount64(), std::memory_order_relaxed);
		}
		if (g_useKeyboard.load(std::memory_order_relaxed)) {
			const auto a = g_padA.load(std::memory_order_relaxed);
			const auto b = g_padB.load(std::memory_order_relaxed);
			g.wButtons = static_cast<WORD>(a & 0xFFFF);
			g.bLeftTrigger = static_cast<BYTE>((a >> 16) & 0xFF);
			g.bRightTrigger = static_cast<BYTE>((a >> 24) & 0xFF);
			g.sThumbLX = static_cast<SHORT>((a >> 32) & 0xFFFF);
			g.sThumbLY = static_cast<SHORT>((a >> 48) & 0xFFFF);
			g.sThumbRX = static_cast<SHORT>(b & 0xFFFF);
			g.sThumbRY = static_cast<SHORT>((b >> 16) & 0xFFFF);
		} else if (g_chordHeld.load(std::memory_order_relaxed)) {
			g.wButtons |= XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK;
		} else if (::GetTickCount64() < g_viewTapUntil.load(std::memory_order_relaxed)) {
			g.wButtons |= XINPUT_GAMEPAD_BACK;  // the chord key's tap in Tarnished mode (chord_tap = view)
		} else {
			return a_result;
		}
		// Our own packet numbers (Elden Ring only re-reads a pad whose number moved), moved whenever what we hand on changes.
		static std::atomic<DWORD> s_packet{ 0x70000000 };
		static std::atomic<std::uint64_t> s_last{ 0 };
		const std::uint64_t shown = static_cast<std::uint64_t>(g.wButtons) ^ (static_cast<std::uint64_t>(g.bLeftTrigger) << 16) ^ (static_cast<std::uint64_t>(g.bRightTrigger) << 24) ^
		                            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(g.sThumbLX)) << 32) ^ (static_cast<std::uint64_t>(static_cast<std::uint16_t>(g.sThumbLY)) << 48) ^
		                            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(g.sThumbRX)) * 0x9E3779B1u) ^ (static_cast<std::uint64_t>(static_cast<std::uint16_t>(g.sThumbRY)) << 20);
		if (s_last.exchange(shown, std::memory_order_relaxed) != shown) {
			s_packet.fetch_add(1, std::memory_order_relaxed);
		}
		s->dwPacketNumber = s_packet.load(std::memory_order_relaxed);
		g_applied.fetch_add(1, std::memory_order_relaxed);
		return a_result;
	}

	bool KeyReachesEr(int a_vk)
	{
		if (!g_installed || !Cfg().keyboard.enabled) {
			return true;
		}
		return ErKeeps(Tables(), Dik(a_vk), CurrentMode());
	}

	bool KeyboardInUse() { return g_useKeyboard.load(std::memory_order_relaxed); }

	int GlyphDevice(int a_padDevice)
	{
		return (a_padDevice >= 0 && Cfg().keyboard.keyboardGlyphs && KeyboardInUse()) ? 1 : a_padDevice;
	}

	std::string Command(const std::string& a_sub, const std::string& a_value)
	{
		auto&       p = Cfg().keyboard;
		const float v = static_cast<float>(std::atof(a_value.c_str()));
		if (a_sub == "on" || a_sub == "off") {
			p.enabled = a_sub == "on";
			if (p.enabled && !g_installed) {
				Install();
			}
			BuildTables(p);
		} else if (a_sub == "sens" && v > 0.0f) {
			p.mouse.sensitivity = v;
		} else if (a_sub == "accel" && v >= 0.0f) {
			p.mouse.acceleration = v;
		} else if (a_sub == "antidz" && v >= 0.0f && v < 0.9f) {
			p.mouse.antiDeadzone = v;
		} else if (a_sub == "smooth" && v >= 0.0f) {
			p.mouse.smoothingMs = v;
		} else if (a_sub == "minspeed" && v >= 0.0f) {
			p.mouse.minSpeed = v;
		} else if (!a_sub.empty()) {
			return "input kbm [on|off | sens|accel|antidz|smooth|minspeed <value>]  (live only; the ini's [Keyboard] makes it stick)";
		}
		return Describe();
	}

	std::string Describe()
	{
		const auto& p = Cfg().keyboard;
		const auto  a = g_padA.load(std::memory_order_relaxed);
		const auto  b = g_padB.load(std::memory_order_relaxed);
		return std::format(
			"keyboard: {} ({}), scope {}, device {}, pad buttons {:#06x} lt {} rt {} L ({}, {}) R ({}, {}); mouse {:.0f} counts/s (sens {} accel {} antidz {} "
			"smooth {} ms); DirectInput reads: keyboard {} state / {} buffered, mouse {} state / {} buffered (last size {}); pads written {}; "
			"device switches {}, chord-key taps {}, virtual pad {} (turned on {}x), Elden Ring polled XInput {} ms ago, cursor {}",
			p.enabled ? "ON" : "off", g_installed ? "hooked" : "not hooked", ScopeName(CurrentMode().scope), g_arbiter.Current() == Device::kKeyboard ? "keyboard" : "pad",
			a & 0xFFFF, (a >> 16) & 0xFF, (a >> 24) & 0xFF, static_cast<std::int16_t>((a >> 32) & 0xFFFF), static_cast<std::int16_t>((a >> 48) & 0xFFFF),
			static_cast<std::int16_t>(b & 0xFFFF), static_cast<std::int16_t>(b >> 16), g_lastSpeed, p.mouse.sensitivity, p.mouse.acceleration, p.mouse.antiDeadzone,
			p.mouse.smoothingMs, g_kbStateReads.load(), g_kbDataReads.load(), g_mouseStateReads.load(), g_mouseDataReads.load(), g_stateSizeSeen.load(), g_applied.load(),
			g_switches, g_taps, InputBridge::VirtualPadOn() ? "on" : "off", g_virtualPadOns, ::GetTickCount64() - InputBridge::LastErPollMs(),
			g_clipped ? "captured" : "free");
	}
}
