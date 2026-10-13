#pragma once

// Snake in MGSV's cardboard box (protocol v8 GuestState kGuestInBox / kGuestBoxPoster), hidden from Elden Ring's enemies the
// MGSV way (docs/HOST.md "Box"):
//  - while he keeps still in the box (and for a short grace while he only creeps), the Tarnished carries one SpEffect of
//    Elden Ring's own that zeroes its enemies' sight and hearing of him ([Stealth] speffect, 20004999: sight_search_enemy_rate
//    and hearing_search_enemy_rate 0, no visual, no end): enemies that haven't noticed him walk past, and those already
//    after him lose him; a hostile within reveal_distance sees through it (the effect is taken off), and so does moving
//    faster. Elden Ring's own stealth system does the rest (it is how Unseen Form and Assassin's Approach work). The effect is
//    removed the moment he leaves the box, the puppet stops or Snake dies.
//  - a box with a poster on it: enemies within poster_range that can see the box (a clear line from their eyes, the box in
//    front of them) stop and stare: their ChrIns debug flags no-attack / no-move (+0x538 bits 4 and 5, the flags the console
//    `flag` command uses) for poster_stare seconds, then they go on (cooldown before the same one stares again). Every flag is
//    put back as it was.

#include "Host.h"
#include "Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phantom::er::Stealth
{
	// Pure: is Snake hidden this frame? (tests/test_host_combat.cpp)
	struct BoxRules
	{
		float stillSpeed{ 0.4f };   // m/s
		float slowSpeed{ 1.8f };    // m/s
		float slowGraceMs{ 1500.0f };
		float revealDistance{ 2.0f };
	};
	struct BoxState
	{
		std::uint64_t stillMs{ 0 };  // the last time he kept still in the box (0 = not since he got in)
	};
	// a_speed: Snake's horizontal speed (m/s); a_nearest: the nearest hostile's distance (m, or a large number).
	bool BoxHidden(BoxState& a_state, bool a_inBox, float a_speed, float a_nearest, std::uint64_t a_nowMs, const BoxRules& a_rules);

	void Tick(const Frame& a_frame);  // FrameBegin, after the events
	void OnPuppetStop(const Frame& a_frame);
	std::string Console(const std::vector<std::string>& a_words, const Frame& a_frame);
	std::string Describe();
}
