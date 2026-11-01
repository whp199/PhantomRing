#include "QuickItemLogic.h"

#include <format>

namespace phantom::er::quickitem
{
	Verdict Classify(const Goods& a_goods, int a_quantity)
	{
		if (a_goods.id < 0) {
			return Verdict::kNoItem;
		}
		if (!a_goods.known) {
			// No row to judge by (EquipParamGoods not resolved: another regulation or build): only a flask, known by its id, is
			// pressed for Snake (the odd/even twins of 1.17.2 are not trusted here: an empty one simply does nothing in Elden Ring).
			// Anything else may open a screen (Memory of Grace's question), call Torrent (the whistle) or take the camera (the
			// telescope) under the hidden Tarnished: Tarnished mode only. Before the 0.6.0 review it was pressed as it was.
			if (!IsFlask(a_goods.id)) {
				return Verdict::kScreen;
			}
			return a_quantity <= 0 ? Verdict::kEmpty : Verdict::kUse;
		}
		if (a_goods.useAnim == kAnimEmpty || (a_goods.consume && a_quantity <= 0)) {
			return Verdict::kEmpty;
		}
		if ((a_goods.menuType != 0 && a_goods.menuType != kMenuSpiritAshes) || a_goods.useAnim == kAnimTelescope) {
			return Verdict::kScreen;
		}
		return Verdict::kUse;
	}

	std::string CycleText(int a_goodsId, int a_quantity, bool a_empty)
	{
		if (a_goodsId < 0) {
			return "Quick item: (empty slot)";
		}
		if (a_empty) {
			return std::format("Quick item: {} ({})", Shown(a_goodsId), IsFlask(a_goodsId) ? "empty -- rest at a grace" : "none left");
		}
		return std::format("Quick item: {} ({})", Shown(a_goodsId), a_quantity);
	}

	bool IsFlask(int a_id)
	{
		return (a_id >= 1000 && a_id <= 1025) || (a_id >= 1050 && a_id <= 1075) || a_id == 250 || a_id == 251;
	}

	bool FlaskTwins(int a_a, int a_b)
	{
		if (a_a == a_b || !IsFlask(a_a) || !IsFlask(a_b)) {
			return a_a == a_b;
		}
		auto key = [](int a_id) { return a_id >= 1050 ? 1000 + (a_id - 1050) / 2 : a_id >= 1000 ? (a_id - 1000) / 2 : 500; };
		return key(a_a) == key(a_b);
	}

	std::string Name(int a_id)
	{
		auto flask = [](const char* a_kind, int a_level) {
			return a_level > 0 ? std::format("Flask of {} Tears +{}", a_kind, a_level) : std::format("Flask of {} Tears", a_kind);
		};
		if (a_id >= 1000 && a_id <= 1025) {
			return flask("Crimson", (a_id - 1000) / 2);
		}
		if (a_id >= 1050 && a_id <= 1075) {
			return flask("Cerulean", (a_id - 1050) / 2);
		}
		if (a_id >= 200000 && a_id < 300000) {
			return "Spirit ashes";
		}
		if (a_id >= 2900 && a_id <= 2912) {
			return std::format("Golden Rune [{}]", a_id - 2899);
		}
		struct Known
		{
			int         id;
			const char* name;
		};
		static constexpr Known kKnown[] = {
			{ 115, "Memory of Grace" }, { 130, "Spectral Steed Whistle" }, { 181, "Spectral Steed Whistle" },
			{ 250, "Flask of Wondrous Physick" }, { 251, "Flask of Wondrous Physick" }, { 300, "Fire Pot" }, { 320, "Lightning Pot" },
			{ 330, "Fetid Pot" }, { 350, "Holy Water Pot" }, { 360, "Freezing Pot" }, { 370, "Poison Pot" }, { 640, "Sleep Pot" },
			{ 660, "Magic Pot" }, { 670, "Rot Pot" }, { 900, "Neutralizing Boluses" }, { 910, "Stanching Boluses" },
			{ 920, "Thawfrost Boluses" }, { 930, "Stimulating Boluses" }, { 940, "Preserving Boluses" }, { 950, "Rejuvenating Boluses" },
			{ 960, "Clarifying Boluses" }, { 1700, "Throwing Dagger" }, { 2040, "Telescope" }, { 2050, "Grace Mimic" }, { 2070, "Lantern" },
			{ 2140, "Margit's Shackle" }, { 2150, "Mohg's Shackle" }, { 3080, "Wraith Calling Bell" },
		};
		for (const auto& k : kKnown) {
			if (k.id == a_id) {
				return k.name;
			}
		}
		return {};
	}

	std::string Shown(int a_id)
	{
		auto n = Name(a_id);
		return n.empty() ? std::string("Quick item") : n;
	}

	void UseTracker::Begin(const Slot& a_slot, bool a_consume, std::uint64_t a_nowMs)
	{
		before_ = a_slot;
		consume_ = a_consume;
		pending_ = true;
		since_ = a_nowMs;
		left_ = a_slot.quantity;
	}

	UseTracker::Outcome UseTracker::Update(const Slot& a_now, std::uint64_t a_nowMs, std::uint64_t a_confirmMs)
	{
		if (!pending_) {
			return Outcome::kIdle;
		}
		if (!consume_) {
			pending_ = false;
			return Outcome::kAssumed;
		}
		if (a_now.goodsId == before_.goodsId && a_now.quantity < before_.quantity) {
			pending_ = false;
			left_ = a_now.quantity;
			return Outcome::kUsed;
		}
		if (a_now.goodsId != before_.goodsId && a_now.empty && before_.quantity <= 1) {
			// The last one: the slot moved on to the flask's empty twin.
			pending_ = false;
			left_ = 0;
			return Outcome::kUsed;
		}
		if (a_nowMs - since_ >= a_confirmMs) {
			pending_ = false;
			return Outcome::kNotUsed;
		}
		return Outcome::kPending;
	}
}
