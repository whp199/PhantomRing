#include "Pad.h"

#include "Addresses.h"
#include "Config.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <mutex>

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
				}
			}
			const auto now = ::GetTickCount64();
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

	std::uint64_t Polls() { return g_polls.load(); }
}
