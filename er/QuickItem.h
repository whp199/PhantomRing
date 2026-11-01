#pragma once

// Elden Ring's quick item without leaving Snake (roadmap 2.10, [Flask]): View held + X makes the hidden Tarnished use the item
// in his selected quick slot (a Flask of Crimson Tears, a Physick, a Fire Pot, spirit ashes...) while Snake keeps the controller;
// View held + D-pad down switches to his next quick item. The press is Elden Ring's own (InputBridge::PressErButtons: its "use
// item" button on the masked pad, MGSV never sees it), so Elden Ring plays the drink on the hidden Tarnished, counts the flask
// and heals him; the heal reaches Snake through forward_heals (er/Puppet.cpp: HP above the mirror -> kHEvHeal). The binding is
// er/Chord.h's (View as a shift; X and D-pad down stay out of MGSV's pad while they are the shift's).
//
// Before the press the slot is read (refused cleanly, with a corner line and a log line): an empty slot, an empty flask ("rest
// at a grace"), an item that opens one of Elden Ring's screens, calls Torrent or takes the camera (Memory of Grace, the
// Spectral Steed Whistle, fingers, the Telescope: Tarnished mode only). After it the count is watched: "Flask of Crimson Tears:
// 3 left", or "the Tarnished couldn't use it now" when it didn't drop within [Flask] confirm_ms.
//
// Layout (fromsoftware-rs cs/player_game_data.rs; each object's RTTI and the inventory pointer checked on every read, no code
// address, so no per-build table; VERIFIED live 2026-10-31 on 1.17.2):
//   PlayerIns +0x580 PlayerGameData* (CS::PlayerGameData) +0x2B0 EquipGameData (CS::EquipGameData):
//     +0x158 EquipInventoryData (CS::EquipInventoryData), +0x288 EquipItemData (CS::EquipItemData):
//       +0x8 quick slots [10] {u32 gaitem handle (0xB0000000 | goods id), i32 inventory index}, +0x98 EquipInventoryData*
//       (== EquipGameData +0x158), +0xA0 i32 the selected quick slot (0..9)
//   EquipInventoryData +0x8 InventoryItemsData: +0xC u32 normal capacity, +0x10 normal entries*, +0x1C u32 key capacity,
//     +0x50 the key entries accessor {entries*, u32* length}; an index below the key capacity is a key entry, else a normal one
//     at index - key capacity. Entry (0x18): {u32 gaitem handle, u32 item id (0x40000000 | goods id), u32 quantity, ...}.

#include "Host.h"
#include "QuickItemLogic.h"

#include <string>
#include <string_view>

namespace phantom::er::QuickItem
{
	struct Readout
	{
		bool             valid{ false };
		std::string      why;           // when not valid
		int              selected{ -1 };
		int              index{ -1 };   // inventory index of the slot's entry
		std::uint32_t    handle{ 0 };
		quickitem::Goods goods;
		int              quantity{ 0 };
		quickitem::Verdict verdict{ quickitem::Verdict::kNoItem };
	};

	Readout     Read(const Frame& a_frame);
	void        Tick(const Frame& a_frame);  // FrameBegin, after the puppet decided: the shift's presses, a press's outcome
	std::string Command(const Frame& a_frame, std::string_view a_args);  // console `flask [use|next|slots]`
}
