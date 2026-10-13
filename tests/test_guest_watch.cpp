// The !watch page-guard mode (mgsv/Watch.cpp) on pages of this process (run with wine):
//   wine test_guest_watch.exe
// A watch must give every page it leaves its own protection back (a .text page stays executable, .rdata
// read-only), switching pages or !unwatch must never leave a PAGE_GUARD nobody handles (MGSV would die of
// an unhandled STATUS_GUARD_PAGE_VIOLATION), and a guard trap on a page we already let go of is swallowed.
#include "Watch.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <format>
#include <string>

using namespace phantom::guest;

static int g_failures = 0;
#define CHECK(x)                                                              \
	do {                                                                      \
		if (!(x)) {                                                           \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);           \
			++g_failures;                                                     \
		}                                                                     \
	} while (0)

static std::string Run(const std::string& a_cmd)
{
	std::string out;
	CHECK(watch::Command(a_cmd, out));
	std::printf("  %s -> %s\n", a_cmd.c_str(), out.c_str());
	return out;
}

static DWORD Protect(const void* a_page)
{
	MEMORY_BASIC_INFORMATION mbi{};
	::VirtualQuery(a_page, &mbi, sizeof(mbi));
	return mbi.Protect;
}

static std::string Addr(const void* a_p) { return std::format("{:#x}", reinterpret_cast<std::uintptr_t>(a_p)); }

int main()
{
	auto* mem = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	auto* rw = mem;             // data page
	auto* ro = mem + 0x1000;    // .rdata-like
	auto* code = mem + 0x2000;  // .text-like: mov eax, 42; ret
	const std::uint8_t body[] = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };
	std::memcpy(code, body, sizeof(body));
	ro[8] = 7;
	DWORD old = 0;
	::VirtualProtect(ro, 0x1000, PAGE_READONLY, &old);
	::VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &old);
	auto* reserved = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 0x1000, MEM_RESERVE, PAGE_NOACCESS));

	// 1. Writes on a data page are seen; the guard is re-armed after each one.
	Run("watch " + Addr(rw + 16) + " pw 4");
	*reinterpret_cast<volatile std::uint32_t*>(rw + 16) = 1;
	*reinterpret_cast<volatile std::uint32_t*>(rw + 16) = 2;
	CHECK(Run("watchlist").find("2 hits") != std::string::npos);
	CHECK(Protect(rw) == (PAGE_READWRITE | PAGE_GUARD));

	// 2. A new page-mode watch gives the old page its protection back (no guard left behind on it).
	Run("watch " + Addr(ro + 8) + " prw 1");
	CHECK(Protect(rw) == PAGE_READWRITE);
	*reinterpret_cast<volatile std::uint32_t*>(rw + 16) = 3;  // must not trap
	CHECK(*reinterpret_cast<volatile std::uint8_t*>(ro + 8) == 7);
	CHECK(Run("watchlist").find("1 hits") != std::string::npos);
	CHECK(Protect(ro) == (PAGE_READONLY | PAGE_GUARD));

	// 3. !unwatch restores the page's own protection: read-only stays read-only (not PAGE_READWRITE).
	Run("unwatch");
	CHECK(Protect(ro) == PAGE_READONLY);

	// 4. A code page stays executable while guarded (the old guard was PAGE_READWRITE | PAGE_GUARD: the
	// retried fetch was an access violation), and is PAGE_EXECUTE_READ again after.
	Run("watch " + Addr(code) + " prw 1");
	CHECK(Protect(code) == (PAGE_EXECUTE_READ | PAGE_GUARD));
	const auto fn = reinterpret_cast<int (*)()>(code);
	CHECK(fn() == 42);
	CHECK(fn() == 42);
	Run("unwatch");
	CHECK(Protect(code) == PAGE_EXECUTE_READ);

	// 5. A DR-mode watch after a page-mode one also drops the page guard.
	Run("watch " + Addr(rw) + " pw 4");
	Run("watch " + Addr(rw) + " w 4");
	CHECK(Protect(rw) == PAGE_READWRITE);
	Run("unwatch");

	// 6. A guard trap raised on a page we already let go of (the trap raced !unwatch or a switch) is
	// swallowed and the access retried, instead of reaching the game's handlers.
	::VirtualProtect(rw, 1, PAGE_READWRITE | PAGE_GUARD, &old);
	*reinterpret_cast<volatile std::uint32_t*>(rw + 32) = 5;
	CHECK(*reinterpret_cast<volatile std::uint32_t*>(rw + 32) == 5);
	CHECK(Protect(rw) == PAGE_READWRITE);

	// 7. Pages that can't be guarded are refused, nothing armed.
	CHECK(Run("watch " + Addr(reserved) + " pw 4").find("not armed") != std::string::npos);
	CHECK(Run("unwatch").find("page guard off") == std::string::npos);

	std::printf("%s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
	return g_failures ? 1 : 0;
}
