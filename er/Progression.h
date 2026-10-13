#pragma once

// Elden Ring's progression for Snake ([Progression] damage_scaling, off by default; roadmap 2.12, docs/COMBAT.md "Progression").
// Reads the Tarnished's rune level and the right-hand armament in use from his PlayerGameData, and turns them into the factor
// er/Combat.cpp puts on every lethal hit (combat::ProgressionFactor).
//
// Layout (fromsoftware-rs cs/player_game_data.rs, cs/chr_ins.rs [V-src]; each read checked live, docs/COMBAT.md):
//   PlayerIns +0x580 PlayerGameData* (RTTI CS::PlayerGameData)
//   PlayerGameData +0x14 current max HP (== the data module's max HP: a cross-check), +0x3C u32 x 8 Vigor, Mind, Endurance,
//   Strength, Dexterity, Intelligence, Faith, Arcane, +0x68 u32 rune level (== their sum - 79: a cross-check),
//   +0x2B0 EquipGameData: +0x6C ChrAsm (= PlayerGameData +0x31C): +0x10 the right hand's slot in use (0..2), +0x7C i32[22]
//   equipment param ids (WeaponLeft1, WeaponRight1, WeaponLeft2, ...: the right hand's slot s at [1 + 2 s]).
// Every failed check turns the readout off (factor 1) and says why once.

#include "Host.h"

#include <string>

namespace phantom::er::Progression
{
	struct Readout
	{
		bool        valid{ false };
		std::string why;             // when not valid
		int         level{ 0 };
		int         attributes[8]{};
		int         maxHp{ 0 };
		int         rightSlot{ -1 };
		int         weaponId{ -1 };
		int         upgrade{ -1 };
		int         maxUpgrade{ 0 };
		int         reinforceType{ -1 };
		std::string weaponWhy;       // the weapon part's failure, when it failed alone
	};

	void          Tick(const Frame& a_frame);  // FrameBegin: re-reads twice a second while Snake plays
	[[nodiscard]] float Factor();              // x on Snake's lethal damage (1 when off or unknown)
	Readout       Read(const Frame& a_frame);  // a fresh readout (console)
	std::string   Command(const Frame& a_frame);  // console `progression`
}
