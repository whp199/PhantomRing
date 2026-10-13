#pragma once

// In-process FD4 singleton finder. Technique and null-check byte pattern from The Grand Archives' fd4_singleton.h
// (Elden-Ring-CT-TGA), re-implemented (docs/research/elden-ring-runtime-re.md §2.2; THIRD-PARTY-NOTICES.md):
// every FD4Singleton<T>::instance() emits the same
// null-check, `mov reg,[slot]; test; jnz; lea rcx,[tag]; call get_singleton_name; ...
// "FD4Singleton.h"`. Scanning for it finds every singleton slot; the game's own
// get_singleton_name(tag) names it. Used to cross-check the AOB-resolved slots by name (a patch that
// moves things around shows up as a name mismatch) and to give the dev console every singleton.

#include <cstdint>
#include <string>
#include <vector>

namespace phantom::er
{
	struct SingletonInfo
	{
		std::string   name;  // e.g. "CS::WorldChrMan"
		std::uint8_t* slot;
	};

	// Needs system init done (calls into the game's name table). Returns the number found.
	// a_nameThem = false only scans (offline verification: returns the number of distinct slots).
	std::size_t FindSingletons(bool a_nameThem = true);
	const std::vector<SingletonInfo>& Singletons();
	std::uint8_t* SingletonSlot(std::string_view a_name);  // full or simple name
}
