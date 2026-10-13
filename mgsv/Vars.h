#pragma once
// Native reads (and the one write) of MGSV's `vars` ScriptVar block: *(u8**)0x142C16920 + offset
// (research §7.1). Each var is enabled only if its registration site still reads that offset.
// Per-frame values come from here rather than from Lua (cheaper, and available on any thread).
// UNVERIFIED at runtime: how often the engine refreshes each var (IH reads playerPos* every update).

#include <array>
#include <cstdint>

namespace phantom::guest::vars
{
	struct Snapshot
	{
		bool          valid{ false };  // the block pointer is set (a mission is loaded)
		std::uint16_t missionCode{ 0 }, locationCode{ 0 };
		float         pos[3]{};
		float         rotYDeg{ 0 };
		std::uint16_t life{ 0 }, lifeMax{ 0 }, stamina{ 0 };
		std::uint16_t vehicleId{ 0xFFFF };
		std::array<std::uint16_t, 6> weapons{};
		std::array<std::uint16_t, 6> ammo{};
		std::uint16_t handEquip{ 0 };
		std::uint32_t inventorySlot{ 0 };
		std::array<std::uint16_t, 16> supportWeapons{};  // equip ids of the support items
		std::uint32_t supportIndex{ 0 };                  // the selected one (vars.currentSupportWeaponIndex)
		std::array<std::uint16_t, 23> stockCounts{};     // vars.ammoStockCounts: 0/2/4 weapon slots 0-2 (+1 their sub), 5 hand,
		                                                  // 6-13 support items, 14-21 items (Ammo.cpp)
		std::uint32_t itemIndex{ 0 };                     // vars.currentItemIndex: the item slot in use (0 none)
		std::uint8_t  buddyType{ 0 };
		std::uint16_t buddyGearAmmo[2]{};
		std::uint8_t  buddyGearMainWp{ 0 };
		std::uint8_t  phase{ 0 };
		std::int32_t  clock{ 0 };
		std::uint32_t buttons{ 0 };  // PlayerVars.scannedButtonsDirect
	};

	// Verifies every registration site; logs and disables the ones that differ.
	void Init();
	Snapshot Read();
	// The life bridge's write: queued (true) and applied by ApplyPendingLife on the link worker thread --
	// a write from the game thread during the player update is overwritten later in the same frame
	// (VERIFIED live), one between frames sticks. False if playerLife isn't verified.
	bool WriteLife(std::uint16_t a_life);
	int  PendingLife();       // -1: nothing queued
	void ApplyPendingLife();  // link worker thread
	// The ammo copies after Ammo.cpp refilled MGSV's ammo component: vars.ammoInWeapons[slot] (0xFFFF = leave) and
	// vars.ammoStockCounts[] -- MGSV writes them itself at Snake's next shot or throw; until then GuestState would show the old
	// counts. False if either var isn't verified.
	bool WriteAmmoCopies(const std::array<std::uint16_t, 3>& a_magazines, const std::array<std::uint16_t, 23>& a_counts);
}
