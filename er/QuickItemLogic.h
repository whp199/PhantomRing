#pragma once

// Elden Ring's quick item without leaving Snake (roadmap 2.10, [Flask]; er/QuickItem.h does the game side): what the item in the
// Tarnished's selected quick slot is, whether the hidden Tarnished may use it while Snake plays, and whether a press was used.
// Pure: tests/test_host_quickitem.inc.

#include <cstdint>
#include <string>

namespace phantom::er::quickitem
{
	// One goods row's facts (EquipParamGoods, tools/re/er_params.py fields EQUIP_PARAM_GOODS_ST; regulation 1.17.2 rows checked
	// offline: 1001 Flask of Crimson Tears anim 10 menu 0 consume 1, its empty twin 1000 anim 254, 130 Spectral Steed Whistle menu 9,
	// 115 Memory of Grace menu 1, 2040 Telescope anim 4, 200000 spirit ashes menu 13 consume 0, 2140 Margit's Shackle consume 0).
	inline constexpr std::uint32_t kGoodsUseAnim = 0x42;   // u8 goods_use_anim (254: nothing to use, an empty flask)
	inline constexpr std::uint32_t kGoodsMenuType = 0x43;  // u8 opme_menu_type (0: none; 13: summon spirit ashes; others open a screen)
	inline constexpr std::uint32_t kGoodsBits48 = 0x48;    // u8: bit 7 is_consume
	inline constexpr std::uint8_t  kAnimEmpty = 254;
	inline constexpr std::uint8_t  kAnimTelescope = 4;
	inline constexpr std::uint8_t  kMenuSpiritAshes = 13;

	struct Goods
	{
		int          id{ -1 };          // goods id (the inventory's item id & 0x0FFFFFFF), -1 = no item in the slot
		bool         known{ false };    // its EquipParamGoods row was read
		std::uint8_t useAnim{ 0 };
		std::uint8_t menuType{ 0 };
		bool         consume{ false };  // is_consume: using it takes one
	};

	enum class Verdict : std::uint8_t
	{
		kUse,      // the hidden Tarnished uses it
		kEmpty,    // an empty flask (or none left of a consumable): nothing to use
		kScreen,   // it opens one of Elden Ring's screens, calls Torrent or takes the camera: Tarnished mode only
		kNoItem,   // the slot is empty
	};

	// a_quantity: how many the inventory holds (consumables: 0 = none left).
	Verdict Classify(const Goods& a_goods, int a_quantity);

	// "Flask of Crimson Tears +3", "Spirit ashes", ...; empty when this table doesn't know it.
	std::string Name(int a_goodsId);
	// Name(), or "Quick item" when it isn't known.
	std::string Shown(int a_goodsId);
	bool        IsFlask(int a_goodsId);  // Crimson / Cerulean Tears, Wondrous Physick (full or empty)
	// The corner line after a switch (View + D-pad down): "Quick item: Flask of Crimson Tears (3)", "(empty -- rest at a grace)" for a
	// drunk-out flask (its empty twin's entry counts 1: live 2026-10-31 the corner said "(1)" for an empty Cerulean flask), "(none
	// left)" for another empty consumable, "Quick item: (empty slot)" with nothing in it.
	std::string CycleText(int a_goodsId, int a_quantity, bool a_empty);
	// The full and the empty goods of one flask (1000 / 1001 Crimson Tears, 1006 / 1007 its +3, 250 / 251 Physick): Elden Ring swaps
	// the inventory entry between them in place while the quick slot keeps the other's handle.
	bool        FlaskTwins(int a_a, int a_b);

	// What the quick slot holds right now (er/QuickItem.cpp reads it each frame while a press is pending).
	struct Slot
	{
		int  goodsId{ -1 };
		int  quantity{ 0 };
		bool empty{ false };  // Classify said kEmpty
	};

	// One press, followed to its outcome.
	class UseTracker
	{
	public:
		enum class Outcome : std::uint8_t
		{
			kIdle,
			kPending,
			kUsed,     // a consumable's count dropped (left() is what's left), or the slot moved on to its empty twin (0 left)
			kAssumed,  // not a consumable: nothing to count, taken as used at the press
			kNotUsed,  // a consumable's count didn't drop within a_confirmMs: Elden Ring refused it (in the air, staggered, ...)
		};

		void Begin(const Slot& a_slot, bool a_consume, std::uint64_t a_nowMs);
		// Each frame while pending; returns the outcome once (then kIdle).
		Outcome Update(const Slot& a_now, std::uint64_t a_nowMs, std::uint64_t a_confirmMs);
		[[nodiscard]] bool Pending() const { return pending_; }
		[[nodiscard]] int  Left() const { return left_; }
		[[nodiscard]] int  GoodsId() const { return before_.goodsId; }
		[[nodiscard]] std::uint64_t Since() const { return since_; }

	private:
		Slot          before_;
		bool          consume_{ false };
		bool          pending_{ false };
		std::uint64_t since_{ 0 };
		int           left_{ 0 };
	};
}
