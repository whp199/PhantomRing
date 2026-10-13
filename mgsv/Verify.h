#pragma once
// Fail-safe gates: is this the exe we were built for, and are the bytes at an address what we expect?

#include "Addresses.h"

#include <cstdint>
#include <string>

namespace phantom::guest
{
	struct ExeIdentity
	{
		std::uint32_t timeDateStamp{ 0 };
		std::uint32_t sizeOfImage{ 0 };
		std::uintptr_t base{ 0 };
		bool          matches{ false };  // stamp + size + base agree with addr::k*
		std::wstring  path;
	};

	// Reads the in-memory PE header of the main module (cheap: safe in DllMain).
	ExeIdentity CheckExe();
	// Hashes the exe file on disk (≈160 MB: seconds; never call it from DllMain). Lower-case hex.
	std::string Sha256File(const std::wstring& a_path);

	// True if the bytes at a_site.va are a_site.bytes (read through ReadProcessMemory, so an unmapped
	// address is a mismatch, not a crash). Logs mismatches with the bytes found.
	bool Matches(const addr::Site& a_site, const char* a_what);
	bool Matches(const addr::VarSite& a_site);
}
