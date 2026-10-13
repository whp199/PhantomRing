#include "Watch.h"

#include "Log.h"

#include <windows.h>
#include <tlhelp32.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <sstream>

namespace phantom::guest::watch
{
	namespace
	{
		constexpr std::uintptr_t kImageLo = 0x140000000;
		constexpr std::uintptr_t kImageHi = 0x14A01D000;  // SizeOfImage 0xA01D000

		struct Hit
		{
			std::atomic<std::uintptr_t> rip{ 0 };
			std::atomic<std::uint32_t>  count{ 0 };
			std::uintptr_t              ret[3]{};  // the first image addresses on the stack (callers)
		};

		std::array<Hit, 32>        g_hits;
		std::atomic<std::uint32_t> g_total{ 0 };
		PVOID                      g_veh{ nullptr };
		std::uintptr_t             g_addr{ 0 };

		// Page-guard mode (debug registers don't fire under Wine/Proton, VERIFIED): the whole page traps once, we
		// note the access if it touches the watched bytes, single-step the instruction and arm the guard again.
		std::atomic<bool>        g_pageMode{ false };
		std::atomic<bool>        g_pageArmed{ false };
		std::uintptr_t           g_len{ 4 };
		bool                     g_writesOnly{ true };
		thread_local bool        t_rearm = false;
		std::atomic<std::uint32_t> g_pageTraps{ 0 };
		// The guarded page and its own protection: the guard is added to (and later removed from) what the page
		// had, never a fixed PAGE_READWRITE (a .text page must stay executable, .rdata read-only).
		std::atomic<std::uintptr_t> g_pageBase{ 0 };
		DWORD                       g_origProt{ 0 };
		// Every page guarded this session: a guard trap that was already raised when the page was disarmed or
		// switched must still be swallowed (the trap consumed the guard; the access just retries). Passed on,
		// it reaches MGSV's handlers as an unhandled STATUS_GUARD_PAGE_VIOLATION and kills the game.
		std::array<std::atomic<std::uintptr_t>, 16> g_pagesSeen{};

		std::uintptr_t PageOf(std::uintptr_t a_addr) { return a_addr & ~std::uintptr_t(0xFFF); }

		bool SeenPage(std::uintptr_t a_page)
		{
			for (const auto& p : g_pagesSeen) {
				if (p.load(std::memory_order_acquire) == a_page) {
					return true;
				}
			}
			return false;
		}

		void RememberPage(std::uintptr_t a_page)
		{
			if (SeenPage(a_page)) {
				return;
			}
			for (auto& p : g_pagesSeen) {
				std::uintptr_t expected = 0;
				if (p.compare_exchange_strong(expected, a_page)) {
					return;
				}
			}
		}

		void GuardPage()
		{
			DWORD old = 0;
			::VirtualProtect(reinterpret_cast<void*>(g_pageBase.load()), 1, g_origProt | PAGE_GUARD, &old);
		}

		// Page mode off: back to the page's own protection. Before every new !watch and on !unwatch.
		bool DisarmPage()
		{
			if (!g_pageArmed.exchange(false)) {
				return false;
			}
			DWORD old = 0;
			::VirtualProtect(reinterpret_cast<void*>(g_pageBase.load()), 1, g_origProt, &old);
			return true;
		}

		void Note(CONTEXT* a_ctx, std::uintptr_t a_rip)
		{
			g_total.fetch_add(1, std::memory_order_relaxed);
			const std::uintptr_t rip = a_rip;
			auto* ctx = a_ctx;
			for (auto& h : g_hits) {
				std::uintptr_t expected = 0;
				if (h.rip.load(std::memory_order_acquire) == rip) {
					h.count.fetch_add(1, std::memory_order_relaxed);
					break;
				}
				if (h.rip.compare_exchange_strong(expected, rip)) {
					// Callers: image addresses among the first 64 stack slots (best effort, frame layout unknown).
					const auto* sp = reinterpret_cast<const std::uintptr_t*>(ctx->Rsp);
					int         n = 0;
					for (int i = 0; i < 64 && n < 3; ++i) {
						const auto v = sp[i];
						if (v > kImageLo && v < kImageHi) {
							h.ret[n++] = v;
						}
					}
					h.count.store(1, std::memory_order_release);
					break;
				}
			}
		}

		LONG CALLBACK OnException(EXCEPTION_POINTERS* a_ep)
		{
			const auto code = a_ep->ExceptionRecord->ExceptionCode;
			auto*      ctx = a_ep->ContextRecord;
			if (code == STATUS_GUARD_PAGE_VIOLATION && g_pageArmed.load(std::memory_order_acquire)) {
				const auto  rw = a_ep->ExceptionRecord->ExceptionInformation[0];  // 0 read, 1 write
				const auto  at = static_cast<std::uintptr_t>(a_ep->ExceptionRecord->ExceptionInformation[1]);
				if (PageOf(at) != g_pageBase.load(std::memory_order_acquire)) {
					// A page we guarded before (the trap raced a re-arm elsewhere): the guard is spent, retry.
					return SeenPage(PageOf(at)) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
				}
				g_pageTraps.fetch_add(1, std::memory_order_relaxed);
				if (at + 8 > g_addr && at < g_addr + g_len && (rw == 1 || !g_writesOnly)) {
					Note(ctx, ctx->Rip);  // the accessing instruction itself (it hasn't run yet)
				}
				t_rearm = true;
				ctx->EFlags |= 0x100;  // run it, then re-arm in the single-step trap
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			if (code == EXCEPTION_SINGLE_STEP && t_rearm) {
				t_rearm = false;
				if (g_pageArmed.load(std::memory_order_acquire)) {
					GuardPage();
					// !unwatch may have run between the check and the guard: undo ours, it would trap unhandled.
					if (!g_pageArmed.load(std::memory_order_acquire)) {
						DWORD old = 0;
						::VirtualProtect(reinterpret_cast<void*>(g_pageBase.load()), 1, g_origProt, &old);
					}
				}
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			if (code == STATUS_GUARD_PAGE_VIOLATION && SeenPage(PageOf(static_cast<std::uintptr_t>(a_ep->ExceptionRecord->ExceptionInformation[1])))) {
				return EXCEPTION_CONTINUE_EXECUTION;  // raised just before !unwatch: the guard is spent, retry
			}
			if (code != EXCEPTION_SINGLE_STEP || !(ctx->Dr6 & 1)) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			ctx->Dr6 = 0;
			Note(ctx, ctx->Rip);
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		// DR0 = a_addr with DR7 bits for it (a_rw: 1 write, 3 read/write; a_len: 0 = 1 byte, 1 = 2, 3 = 4, 2 = 8), or off.
		int Arm(std::uintptr_t a_addr, unsigned a_rw, unsigned a_len, bool a_on)
		{
			const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snap == INVALID_HANDLE_VALUE) {
				return -1;
			}
			const DWORD   self = ::GetCurrentThreadId();
			const DWORD   pid = ::GetCurrentProcessId();
			int           done = 0;
			THREADENTRY32 te{ .dwSize = sizeof(te) };
			for (BOOL ok = ::Thread32First(snap, &te); ok; ok = ::Thread32Next(snap, &te)) {
				if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
					continue;
				}
				const HANDLE t = ::OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
				if (!t) {
					continue;
				}
				if (::SuspendThread(t) != static_cast<DWORD>(-1)) {
					CONTEXT c{};
					c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
					if (::GetThreadContext(t, &c)) {
						c.Dr7 &= ~(0xFull << 16 | 3ull);  // DR0's enable + condition bits
						if (a_on) {
							c.Dr0 = a_addr;
							c.Dr7 |= 1ull | (static_cast<DWORD64>(a_rw) << 16) | (static_cast<DWORD64>(a_len) << 18);
						}
						c.Dr6 = 0;
						if (::SetThreadContext(t, &c)) {
							++done;
						}
					}
					::ResumeThread(t);
				}
				::CloseHandle(t);
			}
			::CloseHandle(snap);
			return done;
		}
	}

	bool Command(std::string_view a_cmd, std::string& a_out)
	{
		std::istringstream in{ std::string(a_cmd) };
		std::string        verb;
		in >> verb;
		if (verb == "watch") {
			std::string addrText, mode = "w";
			unsigned    len = 4;
			if (!(in >> addrText)) {
				a_out = "usage: !watch <addr> [w|rw|pw|prw] [1|2|4|8]   (p..: page-guard mode, works under Wine)";
				return true;
			}
			in >> mode >> len;
			DisarmPage();  // one watch at a time: the previous page gets its own protection back
			if (mode.starts_with("p")) {
				const auto                addr = static_cast<std::uintptr_t>(std::strtoull(addrText.c_str(), nullptr, 0));
				MEMORY_BASIC_INFORMATION mbi{};
				if (!::VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
					(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mbi.Protect == 0) {
					a_out = std::format("watch {:#x}: not a committed, accessible page (protect {:#x}) -- not armed", addr, mbi.Protect);
					return true;
				}
				if (!g_veh) {
					g_veh = ::AddVectoredExceptionHandler(1, &OnException);
				}
				for (auto& h : g_hits) {
					h.rip.store(0);
					h.count.store(0);
				}
				g_total.store(0);
				g_pageTraps.store(0);
				g_addr = addr;
				g_len = len;
				g_writesOnly = mode == "pw";
				g_origProt = mbi.Protect & ~static_cast<DWORD>(PAGE_GUARD);
				RememberPage(PageOf(addr));
				g_pageBase.store(PageOf(addr), std::memory_order_release);
				g_pageMode = true;
				g_pageArmed = true;
				GuardPage();
				Log::Info("watch: page guard on {:#x} ({}, {} bytes, page protect {:#x})", g_addr, mode, len, g_origProt);
				a_out = std::format("watch {:#x} {} {}: page guard armed (page protect {:#x})", g_addr, mode, len, g_origProt);
				return true;
			}
			const auto addr = static_cast<std::uintptr_t>(std::strtoull(addrText.c_str(), nullptr, 0));
			const unsigned lenBits = len == 1 ? 0 : len == 2 ? 1 : len == 8 ? 2 : 3;
			if (!g_veh) {
				g_veh = ::AddVectoredExceptionHandler(1, &OnException);
			}
			for (auto& h : g_hits) {
				h.rip.store(0);
				h.count.store(0);
			}
			g_total.store(0);
			g_addr = addr;
			const int n = Arm(addr, mode == "rw" ? 3u : 1u, lenBits, true);
			Log::Info("watch: {:#x} ({}, {} bytes) armed on {} threads", addr, mode, len, n);
			a_out = std::format("watch {:#x} {} {} armed on {} threads", addr, mode, len, n);
			return true;
		}
		if (verb == "unwatch") {
			if (DisarmPage()) {
				return (a_out = std::format("page guard off ({} hits, {} page traps)", g_total.load(), g_pageTraps.load())), true;
			}
			const int n = Arm(0, 0, 0, false);
			a_out = std::format("watch cleared on {} threads ({} hits seen)", n, g_total.load());
			return true;
		}
		if (verb == "watchlist") {
			a_out = std::format("watch {:#x}: {} hits ({} page traps)", g_addr, g_total.load(), g_pageTraps.load());
			for (const auto& h : g_hits) {
				if (const auto rip = h.rip.load()) {
					a_out += std::format("\n  after {:#x} x{}  stack {:#x} {:#x} {:#x}", rip, h.count.load(), h.ret[0], h.ret[1], h.ret[2]);
				}
			}
			return true;
		}
		return false;
	}
}
