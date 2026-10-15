#include "Pad.h"

#include "Addresses.h"
#include "Config.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"
#include "Logic.h"
#include "Menu.h"
#include "State.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <format>
#include <mutex>
#include <string>

namespace phantom::guest::pad
{
	namespace
	{
		using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
		using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

		GetStateFn g_realGet{ nullptr };
		SetStateFn g_realSet{ nullptr };

		std::atomic<Source>        g_source{ Source::kNeutral };
		std::atomic<bool>          g_blocked{ false };
		std::atomic<bool>          g_parked{ false };
		std::atomic<std::uint16_t> g_pulseButtons{ 0 };
		std::atomic<ULONGLONG>     g_pulseUntil{ 0 };
		std::atomic<std::uint64_t> g_polls{ 0 };
		std::atomic<std::uint16_t> g_ownLast{ 0 };       // the player's own buttons at the last poll (Presses)
		std::atomic<std::uint32_t> g_presses[16]{};      // ... their rising edges, per bit
		// Snake's [Jump] button (MGSV's poll thread only) and its presses for the game thread.
		logic::JumpButton          g_jumpButton;
		std::atomic<std::uint32_t> g_jumpPresses{ 0 };
		std::atomic<bool>          g_mgsvOwnsR3{ false };  // the game thread's word (SetMgsvOwnsR3) and when it last said it
		std::atomic<std::uint64_t> g_mgsvOwnsR3At{ 0 };

		// Boot automation's button sequence (Sequence()): step i holds its buttons/RT for its ms, one after the other.
		std::mutex                       g_seqLock;
		std::array<Step, kMaxSequence>   g_seq{};
		std::size_t                      g_seqCount{ 0 };
		ULONGLONG                        g_seqStart{ 0 };
		std::atomic<bool>                g_seqActive{ false };

		// The step playing at a_now (false when the sequence is over).
		bool SequenceStep(ULONGLONG a_now, Step& a_out)
		{
			if (!g_seqActive.load(std::memory_order_acquire)) {
				return false;
			}
			std::scoped_lock lock(g_seqLock);
			ULONGLONG at = g_seqStart;
			for (std::size_t i = 0; i < g_seqCount; ++i) {
				at += g_seq[i].ms;
				if (a_now < at) {
					a_out = g_seq[i];
					return true;
				}
			}
			g_seqActive.store(false, std::memory_order_release);
			return false;
		}

		// The iDroid request ([Menu] IDroidRetryMs, logic::IDroidRequest): MGSV's poll thread only.
		logic::IDroidRequest       g_idroid;
		std::atomic<std::uint32_t> g_idroidOpened{ 0 }, g_idroidRetried{ 0 }, g_idroidExpired{ 0 };
		std::uint32_t              g_idroidRefusal{ 0 };  // the host pad's buttons/triggers when MGSV didn't take the Start

		const char* RefusalName(std::uint32_t a_pad)
		{
			if (a_pad & 0x10000) {
				return "aiming (LT)";
			}
			if (a_pad & XINPUT_GAMEPAD_RIGHT_SHOULDER) {
				return "binoculars (RB)";
			}
			return "an action (dive, CQC, climb, a weapon change ...)";
		}

		void IDroid(ULONGLONG a_now, XINPUT_GAMEPAD& a_pad, bool a_fromHost)
		{
			const int retryMs = Cfg().idroidRetryMs;
			if (retryMs <= 0) {
				return;
			}
			const bool hostStart = a_fromHost && (a_pad.wButtons & XINPUT_GAMEPAD_START) != 0;
			const auto boot = state::CurrentBoot();
			const auto lua = state::Lua();
			const bool eligible = a_fromHost && boot == state::Boot::kReady && lua.InGame() && !(lua.flags & state::kLDead) && !state::Parked();
			logic::IDroidRequest::Event ev{};
			const auto out = g_idroid.Update(a_now, hostStart, menu::Open(), eligible, static_cast<std::uint64_t>(retryMs), ev);
			using E = logic::IDroidRequest::Event;
			switch (ev) {
			case E::kArmed:
				break;
			case E::kRetry:
				if (g_idroid.Retries() == 1) {
					g_idroidRefusal = a_pad.wButtons | (a_pad.bLeftTrigger > 64 ? 0x10000u : 0u);
					g_idroidRetried.fetch_add(1);
					Log::Info("pad: MGSV didn't open the iDroid on Start -- Snake is busy ({}); pressing it again with the pad neutral",
						RefusalName(g_idroidRefusal));
				}
				break;
			case E::kOpened:
				g_idroidOpened.fetch_add(1);
				if (g_idroid.Retries() > 0) {
					Log::Info("pad: the iDroid opened {} ms after Start ({} presses again; Snake was {})", g_idroid.OpenedAfterMs(), g_idroid.Retries(),
						RefusalName(g_idroidRefusal));
				}
				break;
			case E::kExpired:
				g_idroidExpired.fetch_add(1);
				Log::Warn("pad: the iDroid didn't open within {} ms of Start ({} presses again; Snake was {}) -- given up", retryMs, g_idroid.Retries(),
					RefusalName(g_idroidRefusal));
				break;
			case E::kDropped:
				Log::Info("pad: iDroid request dropped (Snake isn't playing any more)");
				break;
			case E::kCut:
				if (g_idroid.Retries() > 0) {
					Log::Info("pad: the host cut its iDroid hold (View+Start chord) -- no more Start presses");
				}
				break;
			default:
				break;
			}
			if (out == logic::IDroidRequest::Pad::kNeutral || out == logic::IDroidRequest::Pad::kNeutralStart) {
				a_pad = {};
				if (out == logic::IDroidRequest::Pad::kNeutralStart) {
					a_pad.wButtons = XINPUT_GAMEPAD_START;
				}
			}
		}

		std::mutex      g_lock;
		XINPUT_GAMEPAD  g_last{};
		DWORD           g_packet{ 0 };
		std::uint16_t   g_lastRumble[2]{};

		DWORD WINAPI GetState(DWORD a_index, XINPUT_STATE* a_state)
		{
			if (!a_state) {
				return ERROR_BAD_ARGUMENTS;
			}
			if (a_index != 0) {
				return ERROR_DEVICE_NOT_CONNECTED;
			}
			g_polls.fetch_add(1, std::memory_order_relaxed);
			XINPUT_GAMEPAD pad{};
			bool           fromHost = false;
			if (g_source.load(std::memory_order_acquire) == Source::kHost && !g_blocked.load(std::memory_order_acquire) &&
			    !g_parked.load(std::memory_order_acquire)) {
				proto::PadState slot{};
				if (link::HostAlive() && link::ReadPad(slot) && (slot.flags & proto::kPadConnected) && !(slot.flags & proto::kPadNeutral)) {
					pad.wButtons = slot.buttons;
					pad.bLeftTrigger = slot.leftTrigger;
					pad.bRightTrigger = slot.rightTrigger;
					pad.sThumbLX = slot.thumbLX;
					pad.sThumbLY = slot.thumbLY;
					pad.sThumbRX = slot.thumbRX;
					pad.sThumbRY = slot.thumbRY;
					fromHost = true;
				}
			}
			const auto& cfg = Cfg();
			if (fromHost && cfg.jump) {
				bool jump = false;
				const bool owns = logic::MgsvOwnsR3(g_mgsvOwnsR3.load(std::memory_order_acquire), g_mgsvOwnsR3At.load(std::memory_order_acquire), ::GetTickCount64());
				pad.wButtons = g_jumpButton.Filter(pad.wButtons, logic::AimingTrigger(pad.bLeftTrigger), cfg.jumpButton, cfg.jumpPassWith, cfg.jumpPassAiming, jump,
					owns);
				if (jump) {
					g_jumpPresses.fetch_add(1, std::memory_order_acq_rel);
				}
			} else {
				g_jumpButton.Reset();
			}
			const auto now = ::GetTickCount64();
			IDroid(now, pad, fromHost);
			{
				// The player's own presses (before our pulses go on top): Presses(), e.g. the dive (Dive.h).
				const std::uint16_t own = fromHost ? pad.wButtons : 0;
				const std::uint16_t rose = static_cast<std::uint16_t>(own & ~g_ownLast.exchange(own, std::memory_order_relaxed));
				for (int i = 0; i < 16; ++i) {
					if (rose & (1u << i)) {
						g_presses[i].fetch_add(1, std::memory_order_relaxed);
					}
				}
			}
			if (now < g_pulseUntil.load(std::memory_order_acquire)) {
				pad.wButtons |= g_pulseButtons.load(std::memory_order_relaxed);
			}
			if (Step step; SequenceStep(now, step)) {
				pad.wButtons |= step.buttons;
				pad.bRightTrigger = std::max(pad.bRightTrigger, step.rightTrigger);
			}
			std::scoped_lock lock(g_lock);
			// Our own packet counter: it must change whenever what we return changes, including the
			// neutral/pulse overrides the host's packet number knows nothing about.
			if (std::memcmp(&pad, &g_last, sizeof(pad)) != 0) {
				g_last = pad;
				++g_packet;
			}
			a_state->dwPacketNumber = g_packet;
			a_state->Gamepad = pad;
			return ERROR_SUCCESS;
		}

		DWORD WINAPI SetState(DWORD a_index, XINPUT_VIBRATION* a_vibration)
		{
			if (!a_vibration) {
				return ERROR_BAD_ARGUMENTS;
			}
			if (a_index != 0) {
				return ERROR_DEVICE_NOT_CONNECTED;
			}
			std::scoped_lock lock(g_lock);
			if (a_vibration->wLeftMotorSpeed != g_lastRumble[0] || a_vibration->wRightMotorSpeed != g_lastRumble[1]) {
				g_lastRumble[0] = a_vibration->wLeftMotorSpeed;
				g_lastRumble[1] = a_vibration->wRightMotorSpeed;
				proto::GuestEvent e{};
				e.type = proto::kGEvRumble;
				e.a = a_vibration->wLeftMotorSpeed;
				e.b = a_vibration->wRightMotorSpeed;
				link::PushEvent(e);
			}
			return ERROR_SUCCESS;  // the real pad belongs to Elden Ring; the host plays the rumble
		}

		bool IsXInputModule(const void* a_fn)
		{
			HMODULE mod = nullptr;
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a_fn), &mod)) {
				return false;
			}
			wchar_t    path[MAX_PATH];
			const auto n = ::GetModuleFileNameW(mod, path, MAX_PATH);
			std::wstring name(path, n);
			name = name.substr(name.find_last_of(L"\\/") + 1);
			for (auto& c : name) {
				c = static_cast<wchar_t>(std::towlower(c));
			}
			return name.starts_with(L"xinput");
		}
	}

	bool Install()
	{
		auto*       base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		auto*       slot = reinterpret_cast<void**>(addr::kXInputGetStateIat);
		bool        inXInputThunks = false;
		HMODULE     xinput = nullptr;
		if (dir.VirtualAddress) {
			for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
				const char* dll = reinterpret_cast<const char*>(base + d->Name);
				if (_stricmp(dll, "XINPUT1_3.dll") != 0) {
					continue;
				}
				for (auto* t = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk); t->u1.Function; ++t) {
					if (reinterpret_cast<void**>(&t->u1.Function) == slot) {
						inXInputThunks = true;
					}
				}
				xinput = ::GetModuleHandleA(dll);
			}
		}
		if (!inXInputThunks || !IsXInputModule(*slot)) {
			Log::Error("pad: {:#x} is not MGSV's XINPUT1_3 XInputGetState import -- the player's pad will not reach MGSV", addr::kXInputGetStateIat);
			return false;
		}
		g_realGet = reinterpret_cast<GetStateFn>(*slot);
		DWORD old = 0;
		if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
			Log::Error("pad: can't unprotect the IAT ({})", ::GetLastError());
			return false;
		}
		const auto prior = ::InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(&GetState), reinterpret_cast<void*>(g_realGet));
		::VirtualProtect(slot, sizeof(void*), old, &old);
		if (prior != reinterpret_cast<void*>(g_realGet)) {
			Log::Error("pad: the IAT slot changed under us");
			return false;
		}
		Log::Info("pad: XInputGetState import now returns the shared pad (user 0), users 1-3 disconnected");

		// MGSV doesn't import XInputSetState; if it resolves it dynamically, this catches its rumble.
		if (Cfg().rumble && xinput) {
			if (auto* set = reinterpret_cast<void*>(::GetProcAddress(xinput, "XInputSetState"))) {
				Hooks::Create("XInputSetState", set, reinterpret_cast<void*>(&SetState), reinterpret_cast<void**>(&g_realSet));
			}
		}
		return true;
	}

	void SetSource(Source a_source) { g_source.store(a_source, std::memory_order_release); }

	void SetBlocked(bool a_blocked) { g_blocked.store(a_blocked, std::memory_order_release); }
	void SetParked(bool a_parked) { g_parked.store(a_parked, std::memory_order_release); }

	void Pulse(std::uint16_t a_buttons, unsigned a_ms)
	{
		g_pulseButtons.store(a_buttons, std::memory_order_relaxed);
		g_pulseUntil.store(::GetTickCount64() + a_ms, std::memory_order_release);
	}

	bool Sequence(std::span<const Step> a_steps)
	{
		if (a_steps.empty() || a_steps.size() > kMaxSequence || g_seqActive.load(std::memory_order_acquire)) {
			return false;
		}
		std::scoped_lock lock(g_seqLock);
		std::copy(a_steps.begin(), a_steps.end(), g_seq.begin());
		g_seqCount = a_steps.size();
		g_seqStart = ::GetTickCount64();
		g_seqActive.store(true, std::memory_order_release);
		return true;
	}

	bool SequenceRunning()
	{
		Step step;
		return SequenceStep(::GetTickCount64(), step);
	}

	bool TakeJumpPress() { return g_jumpPresses.exchange(0, std::memory_order_acq_rel) > 0; }
	void RequestJump() { g_jumpPresses.fetch_add(1, std::memory_order_acq_rel); }
	void SetMgsvOwnsR3(bool a_owns)
	{
		g_mgsvOwnsR3At.store(::GetTickCount64(), std::memory_order_release);
		g_mgsvOwnsR3.store(a_owns, std::memory_order_release);
	}

	std::uint64_t Polls() { return g_polls.load(); }

	std::uint32_t Presses(std::uint16_t a_button)
	{
		for (int i = 0; i < 16; ++i) {
			if (a_button == (1u << i)) {
				return g_presses[i].load(std::memory_order_relaxed);
			}
		}
		return 0;
	}

	std::string DescribeIDroid()
	{
		return std::format("iDroid requests: {} opened, {} needed Start again, {} given up (IDroidRetryMs {}){}", g_idroidOpened.load(), g_idroidRetried.load(),
			g_idroidExpired.load(), Cfg().idroidRetryMs, g_idroid.Active() ? " -- one under way" : "");
	}
}
