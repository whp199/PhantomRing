#pragma once

// The rules that keep Capture.cpp's pointer-keyed caches honest (docs/GUEST.md 15.3), pure so tests/test_guest_logic.cpp
// can run them against a fake copy table: our reference to one of MGSV's resources lasts only as long as our copy of it
// (CaptureAssets.cpp), after which MGSV may create another resource at the same address. Every copy carries a serial that
// is never reused (assets::Serial): an entry remembers the serial it was made from and is redone when that copy is gone.

#include <cstddef>
#include <cstdint>

namespace phantom::guest::capture::keys
{
	// The copy table (CaptureAssets.cpp Evict): a settled copy (ready or failed) nobody used for a minute is let go. Every
	// use counts (Buffer/Texture, and Serial with a_touch): Capture touches an entry's copies on each use, so a resource in
	// use is never let go (before 2026-10-14 only the first request counted: everything went after a minute).
	inline constexpr std::uint64_t kEvictAfterPresents = 60 * 60;
	inline bool Evictable(bool a_settled, std::uint64_t a_lastUse, std::uint64_t a_now) { return a_settled && a_now - a_lastUse > kEvictAfterPresents; }

	// Something keyed by a resource's pointer while its copy a_kept was current is stale when the current copy (a_now: Serial
	// now, 0 = none) is another one: nothing kept yet (0) is never stale.
	inline bool Stale(std::uint64_t a_kept, std::uint64_t a_now) { return a_kept != 0 && a_now != a_kept; }

	// Each use of an entry (EnsureTexture / EnsureMesh): a_staleTouch(entry) touches the entry's copies and says whether any is
	// another one than the entry was made from; then the entry starts over as Entry{} (copied, keyed and sent afresh: its
	// sentGeneration is reset with it). True when it did.
	template <class Entry, class StaleTouch>
	bool Revalidate(Entry& a_entry, StaleTouch&& a_staleTouch)
	{
		if (!a_staleTouch(a_entry)) {
			return false;
		}
		a_entry = Entry{};
		return true;
	}

	// After an eviction: erase the entries whose copies are gone (a_stale(key, entry), without touching), with their bytes
	// kept for resends. Returns how many.
	template <class Map, class StaleOf>
	std::size_t Prune(Map& a_map, StaleOf&& a_stale)
	{
		std::size_t n = 0;
		for (auto it = a_map.begin(); it != a_map.end();) {
			if (a_stale(it->first, it->second)) {
				it = a_map.erase(it);
				++n;
			} else {
				++it;
			}
		}
		return n;
	}
}
