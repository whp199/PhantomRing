// Runs the host's own address verification (er/Addresses.cpp) against eldenring.exe on disk, mapped
// as an image without executing anything (LoadLibraryEx DONT_RESOLVE_DLL_REFERENCES). Under wine:
//   wine verify_er_addresses.exe 'Z:\path\to\ELDEN RING\Game\eldenring.exe'
// Exit 0 when every entry verifies. The game file is only read. Linked at a low image base so the exe
// maps at its preferred 0x140000000 (the loader doesn't relocate a DONT_RESOLVE image under wine, and
// the RTTI checks follow absolute COL pointers).
#include "Addresses.h"
#include "Log.h"
#include "Singletons.h"

#include <windows.h>

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
	using namespace phantom;
	if (argc < 2) {
		std::printf("usage: verify_er_addresses <eldenring.exe>\n");
		return 2;
	}
	Log::Init(L"verify_er_addresses.log", "verify");
	Log::SetDebug(true);
	const std::wstring path = Widen(argv[1]);
	HMODULE image = ::LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
	if (!image) {
		std::printf("FAIL: can't map %s (%lu)\n", argv[1], ::GetLastError());
		return 1;
	}
	if (!er::UseImageForVerification(image)) {
		std::printf("FAIL: not eldenring.exe 2.7.1.0\n");
		return 1;
	}
	const int failures = er::ResolveAddresses();
	// The singleton finder's scan (naming needs the running game). The static port in
	// tools/er_aob_scan.py finds 216 slots in this exe.
	const auto slots = er::FindSingletons(false);
	std::printf("%d failures, %zu singleton slots (details in verify_er_addresses.log)\n", failures, slots);
	return failures || slots < 200 ? 1 : 0;
}
