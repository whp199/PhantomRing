#pragma once
// Snake's ammunition refill (protocol v10 kHEvRested, docs/GUEST.md 18.11): Elden Ring refills the flasks when the player
// rests at a grace or respawns, and the guest refills Snake the same way -- every weapon's magazine, every ammo stock and
// every support item / item count up to its maximum -- through the setters of MGSV's own ammo component (the one
// Player.GetAmmoStockBySlot reads; the vars arrays are only copies of it).
//
// Two steps: the frame thread asks (Request), the next PhantomRing.Tick runs the Lua 'refill' command for the magazine sizes
// of the three weapon slots (TppEquip.GetAmmoInfo), and its result (OnResults, MGSV's Lua thread) applies everything.

#include <cstdint>
#include <string>
#include <string_view>

namespace phantom::guest::ammo
{
	// Verifies the component's setters (Addresses.h kAmmo*); a mismatch turns the refill off and logs why.
	bool Init();
	bool Available();

	// Frame thread (kHEvRested): refill at the next Lua update. a_why goes into the log ("rest", "respawn", "console").
	void Request(const char* a_why);

	// Lua thread, every Tick's command results (state::CommandResultSink): applies a pending refill from "refill=...".
	void OnResults(std::string_view a_results);

	// Console: `!refill` = Request("console"); `!ammo` = Describe(); `!ammo drain` = Drain() (dev: every ammo/item stock to half
	// through MGSV's own SetCount, so a refill can be seen on a save whose grenades never run out). Drain only queues it: it runs
	// on the Lua thread with the next Tick's results (OnResults), never on the console's worker thread; `!ammo` shows the outcome.
	std::string Describe();
	std::string Drain();
}
