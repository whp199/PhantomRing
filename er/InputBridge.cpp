#include "InputBridge.h"

#include "Chord.h"
#include "Config.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cstdlib>
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
		std::atomic<bool>          g_promptDeferred{ false };  // SetPrompt(.., deferred): the press goes to neither game
		std::atomic<bool>          g_deferLatch{ false };      // ... and stays out of both until released
		std::atomic<ULONGLONG>     g_erPressUntil{ 0 };        // PressForEr: Elden Ring alone sees the prompt button until then
		DWORD                      g_erPressPacket{ 0x60000000 };
		DWORD                      g_erPressRaw{ 0 };
		WORD                       g_erPressShown{ 0 };
		std::atomic<int>           g_padIndex{ -1 };
		std::atomic<std::uint16_t> g_lastButtons{ 0 };
		std::atomic<std::int16_t>  g_lastRx{ 0 };
		std::atomic<std::uint32_t> g_menuPresses{ 0 };
		std::atomic<std::uint32_t> g_promptPresses{ 0 };  // prompt-button presses handed to ER (interactions started)
		std::atomic<std::uint64_t> g_calls{ 0 };
		std::atomic<bool>          g_guestMenu{ false };  // MGSV's iDroid/pause is open: MGSV gets the whole pad, ER nothing
		std::atomic<bool>          g_buttonsOnly{ false };  // ER's interaction before its HUD hides: buttons only
		std::atomic<std::uint32_t> g_viewToGuest{ 0 };    // View holds handed to MGSV (its iDroid)

		// Start/View and the Tarnished-mode chord (er/Chord.h): one gate, run on every poll of the published pad, one poll at a
		// time (the others keep its last result). View held [Input] view_hold_ms while Snake plays is MGSV's iDroid, sent as a
		// synthesized Start of [Input] guest_idroid_start_ms (MGSV's own Back is its pause menu and its iDroid opens on Start
		// held ~1 s, VERIFIED live), and the View itself never reaches MGSV.
		PadGate                    g_gate;
		std::atomic_flag           g_gateLock = ATOMIC_FLAG_INIT;
		std::atomic<int>           g_padMode{ static_cast<int>(PadGate::Mode::kOff) };  // set by the game thread each frame
		std::atomic<bool>          g_guestStart{ false };  // this poll: MGSV's pad carries the synthesized Start
		std::atomic<bool>          g_erStart{ false };     // this poll: Elden Ring's pad carries Start ...
		std::atomic<bool>          g_erBack{ false };      // ... and Back (View)
		std::atomic<std::uint32_t> g_chords{ 0 };          // the chord fired (Host toggles Tarnished mode)
		std::atomic<bool>          g_gateHolding{ false };
		std::atomic<std::uint32_t> g_idroidCancels{ 0 };   // View holds sent to MGSV that became the chord (Start within chord_window_ms)
		std::atomic<std::uint32_t> g_guestMenuCloses{ 0 };  // ...after which MGSV opened its menu anyway and got a Start tap to close it
		std::atomic<std::uint16_t> g_guestMask{ 0 };        // Start/View the chord holds: out of MGSV's pad, its own menu included
		// The View that opened MGSV's iDroid stays out of MGSV's pad until released: once the iDroid is up MGSV gets the
		// whole pad, and a still-held View would be its Back (pause menu / back out of the iDroid).
		std::atomic<bool> g_backHeldOver{ false };

		void UpdateGate(std::uint16_t a_buttons)
		{
			if (g_gateLock.test_and_set(std::memory_order_acquire)) {
				return;
			}
			const auto&     c = Cfg();
			PadGate::Params p;
			p.viewHoldMs = c.viewHoldMs;
			p.idroidStartMs = c.guestIdroidStartMs;
			p.startWaitMs = c.startWaitMs;
			p.chordMs = c.chordMs;
			p.chordWindowMs = c.chordWindowMs;
			PadGate::Input in;
			in.nowMs = ::GetTickCount64();
			in.start = (a_buttons & XINPUT_GAMEPAD_START) != 0;
			in.view = (a_buttons & XINPUT_GAMEPAD_BACK) != 0;
			in.mode = static_cast<PadGate::Mode>(g_padMode.load(std::memory_order_relaxed));
			const auto o = g_gate.Update(in, p);
			if (!in.view) {
				g_backHeldOver.store(false, std::memory_order_relaxed);
			}
			if (o.viewToGuest) {
				g_backHeldOver.store(true, std::memory_order_relaxed);
				g_viewToGuest.fetch_add(1, std::memory_order_relaxed);
			}
			if (o.menuPresses) {
				g_menuPresses.fetch_add(static_cast<std::uint32_t>(o.menuPresses), std::memory_order_relaxed);
			}
			if (o.chord) {
				g_chords.fetch_add(1, std::memory_order_relaxed);
			}
			if (o.idroidCancelled) {
				g_idroidCancels.fetch_add(1, std::memory_order_relaxed);
			}
			if (o.guestMenuClosed) {
				g_guestMenuCloses.fetch_add(1, std::memory_order_relaxed);
			}
			g_guestMask.store(static_cast<std::uint16_t>((o.guestMaskStart ? XINPUT_GAMEPAD_START : 0) | (o.guestMaskView ? XINPUT_GAMEPAD_BACK : 0)),
				std::memory_order_relaxed);
			g_erStart.store(o.erStart, std::memory_order_relaxed);
			g_erBack.store(o.erView, std::memory_order_relaxed);
			g_guestStart.store(o.guestStart, std::memory_order_relaxed);
			g_gateHolding.store(g_gate.Holding(), std::memory_order_relaxed);
			g_gateLock.clear(std::memory_order_release);
		}

		// Elden Ring's Start/View bits as the gate says, the rest as given.
		WORD GatedButtons(WORD a_buttons)
		{
			constexpr WORD kGated = XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK;
			return static_cast<WORD>((a_buttons & ~kGated) | (g_erStart.load(std::memory_order_relaxed) ? XINPUT_GAMEPAD_START : 0) |
									 (g_erBack.load(std::memory_order_relaxed) ? XINPUT_GAMEPAD_BACK : 0));
		}

		// The pad slot has one writer at a time (seqlock); ER may poll from more than one thread.
		std::atomic_flag g_publishLock = ATOMIC_FLAG_INIT;
		proto::PadState  g_published{};
		bool             g_wasConnected{ false };
		DWORD            g_maskedPacket{ 0 };
		std::uint16_t    g_maskedButtons{ 0 };
		DWORD            g_gatePacket{ 0x20000000 };  // unmasked pad with Start/View changed by the gate (its own range)
		DWORD            g_gateRawPacket{ 0 };
		WORD             g_gateShown{ 0 };
		// Buttons (and triggers) already held while MGSV had a neutral pad stay out of MGSV's pad until released: the A
		// that confirms an Elden Ring menu choice (or the B that closes one) is still down when the menu is gone, and
		// MGSV used to take it as its own (VERIFIED live: Snake crouched after choosing a revival location).
		std::uint16_t    g_heldOver{ 0 };
		bool             g_ltHeldOver{ false }, g_rtHeldOver{ false };
		std::atomic<bool> g_lstickHoldWanted{ false };  // HoldLeftStickUntilReleased(): latch on the next publish
		bool             g_lstickHeldOver{ false };
		constexpr int    kStickDeadZone = 7849;          // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE

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
					// and to close it; Elden Ring gets a neutral pad meanwhile. Not the Start/View held for the chord; and
					// a menu MGSV opened from an iDroid hold the chord cut short gets the gate's Start tap that closes it.
					pad.buttons &= static_cast<std::uint16_t>(~g_guestMask.load(std::memory_order_relaxed));
					if (g_guestStart.load(std::memory_order_relaxed)) {
						pad.buttons |= XINPUT_GAMEPAD_START;
					}
				} else {
					if (g_prompt.load(std::memory_order_relaxed) || g_promptLatch.load(std::memory_order_relaxed) ||
						g_deferLatch.load(std::memory_order_relaxed)) {
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
				const bool stickOut = std::abs(static_cast<int>(g.sThumbLX)) > kStickDeadZone || std::abs(static_cast<int>(g.sThumbLY)) > kStickDeadZone;
				if (g_lstickHoldWanted.exchange(false, std::memory_order_relaxed)) {
					g_lstickHeldOver = stickOut;
				}
				g_lstickHeldOver = g_lstickHeldOver && stickOut;
				pad.thumbLX = g_lstickHeldOver ? 0 : g.sThumbLX;
				pad.thumbLY = g_lstickHeldOver ? 0 : g.sThumbLY;
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
					// Start/View: Elden Ring's menu and map, MGSV's iDroid, the Tarnished-mode chord; the presses that reach Elden
					// Ring are counted for MenuWatch's latch (er/Chord.h).
					UpdateGate(now);
					// Only a press that starts while the prompt is up goes to ER (a button already held for MGSV
					// must not turn into an ER interaction); it stays ER's until released.
					const auto button = Cfg().promptButton;
					if (button && (now & ~before & button) && g_prompt.load(std::memory_order_relaxed)) {
						(g_promptDeferred.load(std::memory_order_relaxed) ? g_deferLatch : g_promptLatch).store(true, std::memory_order_relaxed);
						g_promptPresses.fetch_add(1, std::memory_order_relaxed);
					} else if (!(now & button)) {
						g_promptLatch.store(false, std::memory_order_relaxed);
						g_deferLatch.store(false, std::memory_order_relaxed);
					}
				} else {
					// The pad went away (or a test press ended): nothing is held any more.
					g_lastButtons.store(0, std::memory_order_relaxed);
					g_lastRx.store(0, std::memory_order_relaxed);
					UpdateGate(0);
				}
				Publish(a_state, ok);
			}
			const bool published = static_cast<DWORD>(pad) == a_index;

			if (ok && g_maskEr.load(std::memory_order_relaxed)) {
				auto&      g = a_state->Gamepad;
				constexpr WORD kGated = XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK;
				auto           allow = static_cast<WORD>((Cfg().allowButtons & ~kGated) | (g_promptLatch.load(std::memory_order_relaxed) ? Cfg().promptButton : 0));
				WORD           extra = 0;
				if (g_guestMenu.load(std::memory_order_relaxed)) {
					allow = 0;  // MGSV's menu: Elden Ring gets nothing, not even Start/View
				} else if (published) {
					// Start/View only as the gate passes them on (a View tap at its release, Start after start_wait_ms, never
					// the chord's).
					extra = static_cast<WORD>(GatedButtons(0) & Cfg().allowButtons);
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
				const auto allowed = static_cast<WORD>((published ? GatedButtons(g.wButtons) : g.wButtons) & kInteractionButtons);
				g.wButtons = allowed;
				g.bLeftTrigger = g.bRightTrigger = 0;
				g.sThumbLX = g.sThumbLY = g.sThumbRX = g.sThumbRY = 0;
				if (allowed != g_maskedButtons) {
					g_maskedButtons = allowed;
					++g_maskedPacket;
				}
				a_state->dwPacketNumber = g_maskedPacket;
			} else if (ok && published) {
				// Unmasked (the Tarnished plays, an Elden Ring menu is open): Start/View as the gate says -- delayed for the chord
				// in Tarnished mode, a chord's presses kept out until released. When that differs from the real pad, Elden Ring
				// gets our own packet number, moved whenever the real one or what it may see changes (it only re-reads a pad
				// whose packet number changed).
				auto&      g = a_state->Gamepad;
				const auto shown = GatedButtons(g.wButtons);
				if (shown != g.wButtons) {
					if (a_state->dwPacketNumber != g_gateRawPacket || shown != g_gateShown) {
						g_gateRawPacket = a_state->dwPacketNumber;
						g_gateShown = shown;
						++g_gatePacket;
					}
					g.wButtons = shown;
					a_state->dwPacketNumber = g_gatePacket;
				}
			}
			if (ok && published) {
				// A deferred prompt press (a ladder's): the real button stays out of Elden Ring's pad until released, and the host's
				// own press (PressForEr, once the puppet has stepped aside) reaches it alone -- Elden Ring sees exactly one press,
				// made while it has the Tarnished. Our own packet numbers whenever that differs from the real pad.
				const WORD button = Cfg().promptButton;
				const bool give = ::GetTickCount64() < g_erPressUntil.load(std::memory_order_acquire);
				if (button && (give || g_deferLatch.load(std::memory_order_relaxed))) {
					auto&      g = a_state->Gamepad;
					const WORD shown = give ? static_cast<WORD>(g.wButtons | button) : static_cast<WORD>(g.wButtons & ~button);
					if (shown != g.wButtons) {
						if (a_state->dwPacketNumber != g_erPressRaw || shown != g_erPressShown) {
							g_erPressRaw = a_state->dwPacketNumber;
							g_erPressShown = shown;
							++g_erPressPacket;
						}
						g.wButtons = shown;
						a_state->dwPacketNumber = g_erPressPacket;
					}
				}
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

	void SetPrompt(bool a_shown, bool a_deferred)
	{
		g_promptDeferred.store(a_deferred, std::memory_order_relaxed);
		g_prompt.store(a_shown && Cfg().promptButton != 0, std::memory_order_relaxed);
	}

	void SetButtonsOnly(bool a_on) { g_buttonsOnly.store(a_on && g_installed, std::memory_order_relaxed); }

	void SetGuestMenu(bool a_open) { g_guestMenu.store(a_open && g_installed, std::memory_order_relaxed); }
	void PressForEr(unsigned a_ms) { g_erPressUntil.store(::GetTickCount64() + a_ms, std::memory_order_release); }
	void HoldLeftStickUntilReleased() { g_lstickHoldWanted.store(true, std::memory_order_relaxed); }
	void SetPadMode(PadGate::Mode a_mode) { g_padMode.store(static_cast<int>(a_mode), std::memory_order_relaxed); }
	std::uint32_t Chords() { return g_chords.load(std::memory_order_relaxed); }
	bool          ChordHolding() { return g_gateHolding.load(std::memory_order_relaxed); }
	void          NoteMenuPress() { g_menuPresses.fetch_add(1, std::memory_order_relaxed); }
	bool GuestMenu() { return g_guestMenu.load(std::memory_order_relaxed); }
	std::uint32_t ViewHoldsToGuest() { return g_viewToGuest.load(std::memory_order_relaxed); }
	std::uint32_t IdroidCancels() { return g_idroidCancels.load(std::memory_order_relaxed); }
	std::uint32_t GuestMenuCloses() { return g_guestMenuCloses.load(std::memory_order_relaxed); }

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
