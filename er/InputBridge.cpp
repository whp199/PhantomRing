#include "InputBridge.h"

#include "Config.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace phantom::er::InputBridge
{
	namespace
	{
		using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
		using GetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
		using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

		GetStateFn g_getState{ nullptr };  // trampoline
		GetCapsFn  g_getCaps{ nullptr };   // trampoline (installed only for the test virtual pad)
		// Test automation (console `input virtualpad on`): with no controller connected Elden Ring enumerates none and never
		// polls; this reports a neutral gamepad on user 0 to both its capability query and its polls, so `press` reaches
		// Elden Ring itself too (its menus, the grace, Y prompts), not only MGSV. A player with a real pad never needs it.
		std::atomic<bool> g_virtualPad{ false };
		SetStateFn g_setState{ nullptr };
		bool       g_installed{ false };

		std::atomic<bool>          g_maskEr{ false };
		std::atomic<bool>          g_neutral{ true };
		std::atomic<bool>          g_prompt{ false };
		std::atomic<bool>          g_promptLatch{ false };  // the prompt button belongs to ER until released
		std::atomic<int>           g_padIndex{ -1 };
		std::atomic<std::uint16_t> g_lastButtons{ 0 };
		std::atomic<std::int16_t>  g_lastRx{ 0 };
		std::atomic<std::uint32_t> g_menuPresses{ 0 };
		std::atomic<std::uint32_t> g_promptPresses{ 0 };  // prompt-button presses handed to ER (interactions started)
		std::atomic<std::uint64_t> g_calls{ 0 };
		std::atomic<bool>          g_guestMenu{ false };  // MGSV's iDroid/pause is open: MGSV gets the whole pad, ER nothing
		std::atomic<bool>          g_buttonsOnly{ false };  // ER's interaction before its HUD hides: buttons only
		std::atomic<std::uint32_t> g_viewToGuest{ 0 };    // View holds handed to MGSV (its iDroid)

		// View (Back) while Snake plays: a tap is Elden Ring's map, a hold of [Input] view_hold_ms is MGSV's iDroid. ER
		// only gets the tap once it is released (a short synthesized press), so a hold never opens ER's map first.
		// MGSV's own Back is its pause menu and its iDroid opens on Start held ~1 s (VERIFIED live), so the hold reaches
		// MGSV as a synthesized Start of [Input] guest_idroid_start_ms, and the View itself never does.
		struct ViewHold
		{
			ULONGLONG downAt{ 0 };          // 0 = not held
			bool      toGuest{ false };      // this hold reached the threshold
			ULONGLONG erBackUntil{ 0 };      // ER sees Back until then (the tap)
			ULONGLONG guestStartUntil{ 0 };  // MGSV sees Start until then (its iDroid)
		};
		ViewHold         g_view;
		std::atomic_flag g_viewLock = ATOMIC_FLAG_INIT;
		std::atomic<bool> g_guestStart{ false };  // this poll: MGSV's pad carries the synthesized Start
		std::atomic<bool> g_erBack{ false };      // this poll: ER's masked pad carries Back
		// The View that opened MGSV's iDroid stays out of MGSV's pad until released: once the iDroid is up MGSV gets the
		// whole pad, and a still-held View would be its Back (pause menu / back out of the iDroid).
		std::atomic<bool> g_backHeldOver{ false };

		// Runs on every poll of the published pad (any ER input thread); one at a time, the others keep the last result.
		void UpdateViewHold(std::uint16_t a_buttons, std::uint16_t a_before)
		{
			if (g_viewLock.test_and_set(std::memory_order_acquire)) {
				return;
			}
			const auto holdMs = Cfg().viewHoldMs;
			const bool active = holdMs > 0 && g_maskEr.load(std::memory_order_relaxed) && !g_guestMenu.load(std::memory_order_relaxed);
			const ULONGLONG now = ::GetTickCount64();
			const bool      down = (a_buttons & XINPUT_GAMEPAD_BACK) != 0;
			if (!down) {
				g_backHeldOver.store(false, std::memory_order_relaxed);
			}
			if (!active) {
				g_view = {};
				g_guestStart.store(false, std::memory_order_relaxed);
				g_erBack.store(false, std::memory_order_relaxed);
				g_viewLock.clear(std::memory_order_release);
				return;
			}
			if (down && !(a_before & XINPUT_GAMEPAD_BACK)) {
				g_view.downAt = now;
				g_view.toGuest = false;
			}
			if (down && g_view.downAt && !g_view.toGuest && now - g_view.downAt >= static_cast<ULONGLONG>(holdMs)) {
				g_view.toGuest = true;
				g_view.guestStartUntil = now + static_cast<ULONGLONG>(std::max(0, Cfg().guestIdroidStartMs));
				g_backHeldOver.store(true, std::memory_order_relaxed);
				g_viewToGuest.fetch_add(1, std::memory_order_relaxed);
			}
			if (!down && g_view.downAt) {
				if (!g_view.toGuest) {
					g_view.erBackUntil = now + 120;  // a few of ER's polls: its map opens on the press
					g_menuPresses.fetch_add(1, std::memory_order_relaxed);
				}
				g_view.downAt = 0;
				g_view.toGuest = false;
			}
			g_guestStart.store(now < g_view.guestStartUntil, std::memory_order_relaxed);
			g_erBack.store(now < g_view.erBackUntil, std::memory_order_relaxed);
			g_viewLock.clear(std::memory_order_release);
		}

		bool ViewHoldActive() { return Cfg().viewHoldMs > 0 && g_maskEr.load(std::memory_order_relaxed) && !g_guestMenu.load(std::memory_order_relaxed); }

		// The pad slot has one writer at a time (seqlock); ER may poll from more than one thread.
		std::atomic_flag g_publishLock = ATOMIC_FLAG_INIT;
		proto::PadState  g_published{};
		bool             g_wasConnected{ false };
		DWORD            g_maskedPacket{ 0 };
		std::uint16_t    g_maskedButtons{ 0 };
		// Buttons (and triggers) already held while MGSV had a neutral pad stay out of MGSV's pad until released: the A
		// that confirms an Elden Ring menu choice (or the B that closes one) is still down when the menu is gone, and
		// MGSV used to take it as its own (VERIFIED live: Snake crouched after choosing a revival location).
		std::uint16_t    g_heldOver{ 0 };
		bool             g_ltHeldOver{ false }, g_rtHeldOver{ false };

		void Publish(const XINPUT_STATE* a_state, bool a_connected)
		{
			if (g_publishLock.test_and_set(std::memory_order_acquire)) {
				return;  // another thread is publishing this very moment; its copy is as fresh
			}
			proto::PadState pad = g_published;
			pad.flags = (a_connected ? std::uint32_t(proto::kPadConnected) : 0u) | (g_neutral.load(std::memory_order_relaxed) ? std::uint32_t(proto::kPadNeutral) : 0u);
			if (a_connected) {
				const auto& g = a_state->Gamepad;
				constexpr BYTE kTriggerDown = 30;
				if (g_neutral.load(std::memory_order_relaxed)) {
					g_heldOver = g.wButtons;
					g_ltHeldOver = g.bLeftTrigger >= kTriggerDown;
					g_rtHeldOver = g.bRightTrigger >= kTriggerDown;
				} else {
					g_heldOver &= g.wButtons;
					g_ltHeldOver = g_ltHeldOver && g.bLeftTrigger >= kTriggerDown;
					g_rtHeldOver = g_rtHeldOver && g.bRightTrigger >= kTriggerDown;
				}
				pad.buttons = static_cast<std::uint16_t>(g.wButtons & ~g_heldOver);
				if (g_backHeldOver.load(std::memory_order_relaxed)) {
					pad.buttons &= static_cast<std::uint16_t>(~XINPUT_GAMEPAD_BACK);
				}
				if (g_guestMenu.load(std::memory_order_relaxed)) {
					// MGSV's own menu (iDroid, pause) is open: it gets the whole pad, Start/View and Y included, to work it
					// and to close it; Elden Ring gets a neutral pad meanwhile.
				} else {
					if (g_prompt.load(std::memory_order_relaxed) || g_promptLatch.load(std::memory_order_relaxed)) {
						pad.buttons &= static_cast<std::uint16_t>(~Cfg().promptButton);  // ER's press, not MGSV's
					}
					if (g_maskEr.load(std::memory_order_relaxed)) {
						// Start/View are Elden Ring's while Snake plays (its menu and map): MGSV opening its iDroid or pause
						// menu under ER's (VERIFIED live: one Start press opened both) would leave it stuck in a menu. A held
						// View is the exception: held, it is MGSV's iDroid (view_hold_ms -> Start held, see ViewHold).
						pad.buttons &= static_cast<std::uint16_t>(~Cfg().allowButtons);
						if (g_guestStart.load(std::memory_order_relaxed)) {
							pad.buttons |= XINPUT_GAMEPAD_START;
						}
					}
				}
				pad.leftTrigger = g_ltHeldOver ? 0 : g.bLeftTrigger;
				pad.rightTrigger = g_rtHeldOver ? 0 : g.bRightTrigger;
				pad.thumbLX = g.sThumbLX;
				pad.thumbLY = g.sThumbLY;
				pad.thumbRX = g.sThumbRX;
				pad.thumbRY = g.sThumbRY;
			} else {
				pad.buttons = 0;
				pad.leftTrigger = pad.rightTrigger = 0;
				pad.thumbLX = pad.thumbLY = pad.thumbRX = pad.thumbRY = 0;
			}
			if (std::memcmp(reinterpret_cast<const std::uint8_t*>(&pad) + 4, reinterpret_cast<const std::uint8_t*>(&g_published) + 4, sizeof(pad) - 4) != 0 ||
				a_connected != g_wasConnected) {
				++pad.packet;
				Link::Get().WritePad(pad);
				g_published = pad;
				g_wasConnected = a_connected;
			}
			g_publishLock.clear(std::memory_order_release);
		}

		std::atomic<std::uint16_t> g_injButtons{ 0 };
		std::atomic<std::int32_t>  g_injStick{ 0 };  // lx | ly << 16
		std::atomic<std::int32_t>  g_injStickR{ 0 };  // rx | ry << 16
		std::atomic<std::uint16_t> g_injTriggers{ 0 };  // lt | rt << 8
		std::atomic<ULONGLONG>     g_injUntil{ 0 };
		std::atomic<DWORD>         g_injPacket{ 0x40000000 };

		// Console 'press': applied last, so it reaches Elden Ring whatever the mask says.
		DWORD ApplyInjection(DWORD a_index, XINPUT_STATE* a_state, DWORD a_result)
		{
			if (a_index != 0 || !a_state || ::GetTickCount64() >= g_injUntil.load(std::memory_order_acquire)) {
				return a_result;
			}
			if (a_result != ERROR_SUCCESS) {
				*a_state = {};
			}
			const auto stick = g_injStick.load(std::memory_order_relaxed);
			a_state->Gamepad.wButtons |= g_injButtons.load(std::memory_order_relaxed);
			if (stick) {
				a_state->Gamepad.sThumbLX = static_cast<SHORT>(stick & 0xFFFF);
				a_state->Gamepad.sThumbLY = static_cast<SHORT>((stick >> 16) & 0xFFFF);
			}
			if (const auto r = g_injStickR.load(std::memory_order_relaxed)) {
				a_state->Gamepad.sThumbRX = static_cast<SHORT>(r & 0xFFFF);
				a_state->Gamepad.sThumbRY = static_cast<SHORT>((r >> 16) & 0xFFFF);
			}
			if (const auto t = g_injTriggers.load(std::memory_order_relaxed)) {
				a_state->Gamepad.bLeftTrigger = std::max<BYTE>(a_state->Gamepad.bLeftTrigger, static_cast<BYTE>(t & 0xFF));
				a_state->Gamepad.bRightTrigger = std::max<BYTE>(a_state->Gamepad.bRightTrigger, static_cast<BYTE>(t >> 8));
			}
			a_state->dwPacketNumber = g_injPacket.fetch_add(1, std::memory_order_relaxed);
			return ERROR_SUCCESS;
		}

		// Test automation without a controller: Elden Ring stops polling XInput when none is connected, so `press` never
		// reached the bridge. FrameBegin then polls for it (PollIfIdle); these calls are not Elden Ring's.
		thread_local bool      t_ourPoll{ false };
		std::atomic<ULONGLONG> g_lastErCall{ 0 };
		std::atomic<bool>      g_ourPollLogged{ false };

		DWORD WINAPI GetStateDetour(DWORD a_index, XINPUT_STATE* a_state)
		{
			if (!t_ourPoll) {
				g_lastErCall.store(::GetTickCount64(), std::memory_order_relaxed);
			}
			// Injected test input behaves exactly like the real pad: published to MGSV, masked for ER.
			DWORD raw = g_getState(a_index, a_state);
			if (raw != ERROR_SUCCESS && a_index == 0 && a_state && g_virtualPad.load(std::memory_order_relaxed)) {
				*a_state = {};  // the test virtual pad: connected, neutral
				a_state->dwPacketNumber = 1;
				raw = ERROR_SUCCESS;
			}
			const DWORD result = ApplyInjection(a_index, a_state, raw);
			g_calls.fetch_add(1, std::memory_order_relaxed);
			const bool ok = result == ERROR_SUCCESS && a_state;

			int pad = g_padIndex.load(std::memory_order_relaxed);
			if (pad < 0 && ok) {
				const int want = Cfg().userIndex;
				if (want < 0 || static_cast<DWORD>(want) == a_index) {
					int expected = -1;
					if (g_padIndex.compare_exchange_strong(expected, static_cast<int>(a_index))) {
						Log::Info("input: publishing XInput pad {} to MGSV", a_index);
					}
					pad = g_padIndex.load(std::memory_order_relaxed);
				}
			}
			if (static_cast<DWORD>(pad) == a_index) {
				if (ok) {
					const auto now = a_state->Gamepad.wButtons;
					const auto before = g_lastButtons.exchange(now, std::memory_order_relaxed);
					g_lastRx.store(a_state->Gamepad.sThumbRX, std::memory_order_relaxed);
					// Presses that reach Elden Ring's menus (MenuWatch's latch): none while MGSV's menu has the pad; a held
					// View goes to MGSV and its tap is counted when ER gets it (UpdateViewHold).
					const std::uint16_t menuButtons = g_guestMenu.load(std::memory_order_relaxed) ? 0 :
						(ViewHoldActive() ? XINPUT_GAMEPAD_START : (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK));
					if ((now & ~before) & menuButtons) {
						g_menuPresses.fetch_add(1, std::memory_order_relaxed);
					}
					UpdateViewHold(now, before);
					// Only a press that starts while the prompt is up goes to ER (a button already held for MGSV
					// must not turn into an ER interaction); it stays ER's until released.
					const auto button = Cfg().promptButton;
					if (button && (now & ~before & button) && g_prompt.load(std::memory_order_relaxed)) {
						g_promptLatch.store(true, std::memory_order_relaxed);
						g_promptPresses.fetch_add(1, std::memory_order_relaxed);
					} else if (!(now & button)) {
						g_promptLatch.store(false, std::memory_order_relaxed);
					}
				} else {
					// The pad went away (or a test press ended): nothing is held any more.
					g_lastButtons.store(0, std::memory_order_relaxed);
					g_lastRx.store(0, std::memory_order_relaxed);
				}
				Publish(a_state, ok);
			}

			if (ok && g_maskEr.load(std::memory_order_relaxed)) {
				auto&      g = a_state->Gamepad;
				auto       allow = static_cast<WORD>(Cfg().allowButtons | (g_promptLatch.load(std::memory_order_relaxed) ? Cfg().promptButton : 0));
				WORD       extra = 0;
				if (g_guestMenu.load(std::memory_order_relaxed)) {
					allow = 0;  // MGSV's menu: Elden Ring gets nothing, not even Start/View
				} else if (ViewHoldActive()) {
					allow = static_cast<WORD>(allow & ~XINPUT_GAMEPAD_BACK);  // only the released tap (UpdateViewHold)
					extra = g_erBack.load(std::memory_order_relaxed) ? XINPUT_GAMEPAD_BACK : 0;
				}
				const auto allowed = static_cast<WORD>((g.wButtons & allow) | extra);
				g.wButtons = allowed;
				g.bLeftTrigger = g.bRightTrigger = 0;
				g.sThumbLX = g.sThumbLY = g.sThumbRX = g.sThumbRY = 0;
				// ER only re-reads a pad whose packet number changed: give it our own, which moves
				// exactly when what it may see changes.
				if (allowed != g_maskedButtons) {
					g_maskedButtons = allowed;
					++g_maskedPacket;
				}
				a_state->dwPacketNumber = g_maskedPacket;
			} else if (ok && g_buttonsOnly.load(std::memory_order_relaxed)) {
				auto&      g = a_state->Gamepad;
				const auto allowed = static_cast<WORD>(g.wButtons & kInteractionButtons);
				g.wButtons = allowed;
				g.bLeftTrigger = g.bRightTrigger = 0;
				g.sThumbLX = g.sThumbLY = g.sThumbRX = g.sThumbRY = 0;
				if (allowed != g_maskedButtons) {
					g_maskedButtons = allowed;
					++g_maskedPacket;
				}
				a_state->dwPacketNumber = g_maskedPacket;
			}
			return result;
		}
	}

	void Inject(std::uint16_t a_buttons, unsigned a_ms, std::int16_t a_lx, std::int16_t a_ly, std::uint8_t a_lt, std::uint8_t a_rt, std::int16_t a_rx, std::int16_t a_ry)
	{
		g_injButtons.store(a_buttons, std::memory_order_relaxed);
		g_injTriggers.store(static_cast<std::uint16_t>(a_lt | (a_rt << 8)), std::memory_order_relaxed);
		g_injStickR.store(static_cast<std::int32_t>(static_cast<std::uint16_t>(a_rx)) | (static_cast<std::int32_t>(static_cast<std::uint16_t>(a_ry)) << 16),
			std::memory_order_relaxed);
		g_injStick.store(static_cast<std::int32_t>(static_cast<std::uint16_t>(a_lx)) | (static_cast<std::int32_t>(static_cast<std::uint16_t>(a_ly)) << 16),
			std::memory_order_relaxed);
		g_injUntil.store(::GetTickCount64() + a_ms, std::memory_order_release);
	}

	namespace
	{
		DWORD WINAPI GetCapsDetour(DWORD a_index, DWORD a_flags, XINPUT_CAPABILITIES* a_caps)
		{
			const DWORD r = g_getCaps(a_index, a_flags, a_caps);
			if (r != ERROR_SUCCESS && a_index == 0 && a_caps && g_virtualPad.load(std::memory_order_relaxed)) {
				*a_caps = {};
				a_caps->Type = XINPUT_DEVTYPE_GAMEPAD;
				a_caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
				a_caps->Gamepad.wButtons = 0xF3FF;
				a_caps->Gamepad.bLeftTrigger = a_caps->Gamepad.bRightTrigger = 0xFF;
				a_caps->Gamepad.sThumbLX = a_caps->Gamepad.sThumbLY = a_caps->Gamepad.sThumbRX = a_caps->Gamepad.sThumbRY = static_cast<SHORT>(0xFFC0);
				a_caps->Vibration.wLeftMotorSpeed = a_caps->Vibration.wRightMotorSpeed = 0xFF;
				return ERROR_SUCCESS;
			}
			return r;
		}

		BOOL CALLBACK NotifyWindow(HWND a_hwnd, LPARAM)
		{
			DWORD pid = 0;
			::GetWindowThreadProcessId(a_hwnd, &pid);
			if (pid == ::GetCurrentProcessId()) {
				// What Windows sends when a device comes or goes: Elden Ring re-enumerates its controllers on it.
				::PostMessageW(a_hwnd, WM_DEVICECHANGE, 0x0007 /* DBT_DEVNODES_CHANGED */, 0);
			}
			return TRUE;
		}
	}

	std::string SetVirtualPad(bool a_on)
	{
		if (a_on && !g_getCaps) {
			HMODULE xinput = ::GetModuleHandleW(L"xinput1_4.dll");
			auto*   caps = xinput ? reinterpret_cast<void*>(::GetProcAddress(xinput, "XInputGetCapabilities")) : nullptr;
			if (!caps || !Hooks::Create("xinput1_4!XInputGetCapabilities", caps, reinterpret_cast<void*>(&GetCapsDetour), reinterpret_cast<void**>(&g_getCaps)) ||
				!Hooks::EnableAll()) {
				return "virtual pad: XInputGetCapabilities hook failed";
			}
		}
		g_virtualPad.store(a_on, std::memory_order_relaxed);
		::EnumWindows(&NotifyWindow, 0);
		Log::Info("input: test virtual pad {} (Elden Ring was told devices changed)", a_on ? "ON: user 0 reports a neutral gamepad" : "off");
		return std::format("virtual pad {}; Elden Ring polls: {} calls so far", a_on ? "on" : "off", g_calls.load());
	}

	void PollIfIdle()
	{
		const auto now = ::GetTickCount64();
		const auto until = g_injUntil.load(std::memory_order_acquire);
		// While a `press` lasts (and 0.5 s after, so its release is published too), and only if Elden Ring isn't polling.
		if (!g_installed || until == 0 || now >= until + 500 || now - g_lastErCall.load(std::memory_order_relaxed) < 200) {
			return;
		}
		if (!g_ourPollLogged.exchange(true)) {
			Log::Info("input: Elden Ring polls no controller -- console `press` drives a virtual pad through the bridge (test automation; Elden Ring "
			          "itself doesn't see it)");
		}
		t_ourPoll = true;
		XINPUT_STATE state{};
		GetStateDetour(0, &state);
		t_ourPoll = false;
	}

	bool Install()
	{
		HMODULE xinput = ::GetModuleHandleW(L"xinput1_4.dll");
		if (!xinput) {
			Log::Warn("input: xinput1_4.dll is not loaded in this process; controller bridge off");
			return false;
		}
		auto* byOrdinal = reinterpret_cast<void*>(::GetProcAddress(xinput, MAKEINTRESOURCEA(2)));
		auto* byName = reinterpret_cast<void*>(::GetProcAddress(xinput, "XInputGetState"));
		if (!byOrdinal) {
			Log::Error("input: xinput1_4 has no ordinal 2; controller bridge off");
			return false;
		}
		if (byName && byName != byOrdinal) {
			// ER imports #2; that is what we must hook even if this build numbers things differently.
			Log::Warn("input: xinput1_4 #2 ({}) is not XInputGetState ({}); hooking #2 as ER imports it", byOrdinal, byName);
		}
		g_setState = reinterpret_cast<SetStateFn>(reinterpret_cast<void*>(::GetProcAddress(xinput, MAKEINTRESOURCEA(3))));
		if (!Hooks::Create("xinput1_4!XInputGetState", byOrdinal, reinterpret_cast<void*>(&GetStateDetour), reinterpret_cast<void**>(&g_getState)) ||
			!Hooks::EnableAll()) {
			return false;
		}
		g_installed = true;
		Log::Info("input: XInputGetState hooked (ER pad masked while puppeting; allow-list {:#06x})", Cfg().allowButtons);
		return true;
	}

	bool Installed() { return g_installed; }

	void SetMode(bool a_maskEr, bool a_neutralForGuest)
	{
		g_maskEr.store(a_maskEr && g_installed, std::memory_order_relaxed);
		g_neutral.store(a_neutralForGuest, std::memory_order_relaxed);
	}

	void SetPrompt(bool a_shown) { g_prompt.store(a_shown && Cfg().promptButton != 0, std::memory_order_relaxed); }

	void SetButtonsOnly(bool a_on) { g_buttonsOnly.store(a_on && g_installed, std::memory_order_relaxed); }

	void SetGuestMenu(bool a_open) { g_guestMenu.store(a_open && g_installed, std::memory_order_relaxed); }
	bool GuestMenu() { return g_guestMenu.load(std::memory_order_relaxed); }
	std::uint32_t ViewHoldsToGuest() { return g_viewToGuest.load(std::memory_order_relaxed); }

	std::uint16_t LastButtons() { return g_lastButtons.load(std::memory_order_relaxed); }
	std::int16_t  LastRightStickX() { return g_lastRx.load(std::memory_order_relaxed); }
	std::uint32_t MenuPresses() { return g_menuPresses.load(std::memory_order_relaxed); }
	std::uint32_t PromptPresses() { return g_promptPresses.load(std::memory_order_relaxed); }
	int           PadIndex() { return g_padIndex.load(std::memory_order_relaxed); }
	std::uint64_t Calls() { return g_calls.load(std::memory_order_relaxed); }

	void Rumble(std::uint16_t a_low, std::uint16_t a_high)
	{
		const int pad = PadIndex();
		if (g_setState && pad >= 0) {
			XINPUT_VIBRATION v{ a_low, a_high };
			g_setState(static_cast<DWORD>(pad), &v);
		}
	}
}
