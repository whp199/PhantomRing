#pragma once
// Snake's suppressor (docs/GUEST.md 18.18; protocol v16 kGuestSuppressorOn / kFireSuppressed).
//
// MGSV keeps it in the ammo component's slot record next to the magazine (Addresses.h "Snake's suppressor"): its life (rounds
// it still takes, 0xFF = never wears) and whether it is on. FireWeapon asks MGSV's own reader (0x141050870) before its pellet
// loop whether the round goes through it -- quiet, no muzzle flash -- and wears it by the pellets afterwards. The guest:
//  - reads it the same way (a replica over safe reads, exact while MGSV's bytes match) at each of Snake's shots, before the
//    wear (kFireSuppressed), and every frame for the weapon in hand (kGuestSuppressorOn);
//  - gives the [Loadout] guns their suppressor before the sandbox loads (vars.isUseSuppressor / suppressorLives, which the
//    mission load builds the slot records from; GuestScript PR.PreLoadout), only while MGSV's saves are blocked;
//  - puts it back to full at a rest / respawn (kHEvRested) through the component's own setters, as Ammo.cpp does the rounds.

#include <array>
#include <cstdint>
#include <string>

namespace phantom::guest::suppressor
{
	// Byte checks (DllMain-safe): the reader and FireWeapon's use of it, the setters. A mismatch logs why and turns that part off.
	bool Init();
	[[nodiscard]] bool Available();      // the reading (shots, the flag)
	[[nodiscard]] bool SettersAvailable();  // the refill

	struct Reading
	{
		bool          valid{ false };    // everything read (a firearm or not)
		bool          firearm{ false };
		bool          on{ false };
		std::uint8_t  life{ 0 };
		std::uint8_t  slot{ 0xFF };      // the ammo component's slot ([w+0x3DA])
		std::uint16_t equip{ 0 };
		[[nodiscard]] bool Working() const { return valid && on && life > 0; }
	};

	// MGSV's reading for WeaponState a_state of WeaponComponent a_component, player a_player (any thread: safe reads only).
	Reading Read(std::uintptr_t a_component, std::uintptr_t a_state, std::uint32_t a_player);
	// Snake's weapon in hand: his character's WeaponComponent and his WeaponState (Combat's character and index).
	Reading InHand(std::uintptr_t a_character, std::uint32_t a_player);

	// Lua thread (Ammo.cpp's refill, the console): the slot's suppressor on and at a_life through MGSV's setters (vf 6, vf 5).
	// a_component = the ammo component (Ammo.cpp Find). Returns what it did ("slot 2: 0 -> 16 on"), or why not.
	std::string Set(std::uintptr_t a_component, std::uint32_t a_player, int a_slot, std::uint8_t a_life, bool a_on);

	// Lua thread, the boot's setup (Pump.cpp), after the sandbox loaded the [Loadout] guns (a_slots: bit per slot 0-2 the loadout
	// set): [Loadout] Suppressors ("sleep" / "all" / "none") applied with MGSV's full life for each gun that takes one (its gun
	// data's durability; PR.PreLoadout wrote a provisional one before the load), off on the others. Only while saves are blocked
	// (the caller checks). Returns the log text.
	std::string ApplyLoadout(const std::string& a_mode, unsigned a_slots);
	// A refill of every gun that has a suppressor this session (the loadout's, or one seen working) to MGSV's full life: asked
	// from any thread (a_why: "rest", "respawn", "pickup"), run on the Lua thread by LuaTick; or run there directly.
	void RequestRefill(const char* a_why);
	void RefillOnLuaThread(const char* a_why);
	// Frame thread, every frame: the vars' ammo stocks (vars.ammoStockCounts; 0/2/4 = weapon slots 0-2) -- a slot's stock that
	// rises outside a rest (a supply drop's ammo picked up) refills that gun's suppressor too.
	void NoteStocks(const std::array<std::uint16_t, 23>& a_counts, bool a_valid);
	// A rest / respawn refill was asked (Frame.cpp, the console): its stock rises are not pickups.
	void NoteRest();

	// Console `!suppressor <slot> <life> [on|off]`: queued for MGSV's Lua thread (LuaTick runs it after the next Tick).
	std::string Queue(int a_slot, int a_life, bool a_on);
	void        LuaTick();
	[[nodiscard]] std::string LastSet();

	// Frame thread: a change of the weapon in hand's reading, for the log (once per change).
	void NoteInHand(const Reading& a_r);

	// Console `!suppressor` (pure reads, any thread): the three weapon slots' records and vars copies, the weapon in hand.
	std::string Describe(std::uintptr_t a_character, std::uint32_t a_player);
}
