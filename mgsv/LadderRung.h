#pragma once
// MGSV's own ladder rung index, the native way to keep Snake's ladder climb seamless on an Elden Ring ladder of any length
// (docs/GUEST.md 22.2a). MGSV's ladder action keeps Snake's place on a ladder as a rung pair (u16, 0.5 m each) in the player's
// ladder record and plays the climbing animation from it; MGSV's airport ladders are 8 m (top rung pair 16), so a long climb
// used to end in MGSV's climb-over every ~5.5 m. Moving the rung pair (and the one he climbs to) by the same count puts him
// that many 0.5 m steps along the same ladder in the middle of his climb: the animation goes on from the new rung, his pose
// is the same, only his root jumps -- and Elden Ring draws the pose at its own ladder (live 2026-10-31: 14 -> 8 moved him 3 m
// down his ladder, still climbing; 20 m up and down in 17 s each with no climb-over, every frame on the rungs).
//
// The ladder system object holding the records isn't reachable from a static: it is found once by its vtables (a worker
// thread's scan of the private heap, ~1 s, started when the sandbox first plays) and checked every use (vtables, the player
// context it belongs to, the record's ladder at the foot we staged at).

#include "Logic.h"

#include <optional>
#include <string>

namespace phantom::guest::ladder_rung
{
	// DllMain: byte checks only (no scan). False = never used (logged): ladders loop through MGSV's ends as before.
	bool Init();
	[[nodiscard]] bool Available();

	// Any thread: start the one-time search for MGSV's ladder system (a worker thread; again only after a failed check).
	void Prepare();

	struct Rungs
	{
		int rung{ -1 };    // the rung pair he is on
		int target{ -1 };  // the one he climbs to
		int top{ -1 };     // the ladder's top rung pair (past it he climbs over)
	};
	// Game thread: Snake's ladder record when it is on the MGSV ladder whose foot is within 1.5 m of a_foot; nullopt otherwise
	// (not found yet, another ladder, off it).
	std::optional<Rungs> Read(const logic::Vec3& a_foot);
	// Game thread: moves him a_delta rung pairs along that ladder (both the rung and the target, one atomic store, only if they
	// still are a_seen's). False = not done.
	bool Shift(const logic::Vec3& a_foot, const Rungs& a_seen, int a_delta);

	std::string Describe();
}
