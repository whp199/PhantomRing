#include "QuickItem.h"

#include "Config.h"
#include "Game.h"
#include "Hud.h"
#include "InputBridge.h"
#include "Log.h"
#include "Mem.h"
#include "Params.h"
#include "Puppet.h"

#include <format>

#include <windows.h>

namespace phantom::er::QuickItem
{
	namespace
	{
		constexpr std::uint32_t kPlayerGameData = 0x580;  // PlayerIns: PlayerGameData* (er/Progression.cpp too)
		constexpr std::uint32_t kPgdEquip = 0x2B0;        // PlayerGameData: EquipGameData (inline)
		constexpr std::uint32_t kEgdInventory = 0x158;    // EquipGameData: EquipInventoryData (inline)
		constexpr std::uint32_t kEgdItemData = 0x288;     // EquipGameData: EquipItemData (inline)
		constexpr std::uint32_t kEidQuickSlots = 0x8;     // EquipItemData: {u32 handle, i32 index} x 10
		constexpr std::uint32_t kEidInventoryPtr = 0x98;  // EquipItemData: EquipInventoryData*
		constexpr std::uint32_t kEidSelected = 0xA0;      // EquipItemData: i32 selected quick slot
		constexpr int           kQuickSlots = 10;
		constexpr std::uint32_t kInvNormalCap = 0xC;      // EquipInventoryData (items data at +0x8)
		constexpr std::uint32_t kInvNormalHead = 0x10;
		constexpr std::uint32_t kInvKeyCap = 0x1C;
		constexpr std::uint32_t kInvKeyAccessor = 0x50;   // {entries*, u32* length}
		constexpr std::uint32_t kEntryBytes = 0x18;

		params::Table g_goods{ params::kEquipParamGoods };
		const void*   g_vt[4]{};  // PlayerGameData, EquipGameData, EquipInventoryData, EquipItemData (learned once by RTTI name)

		std::uint32_t         g_usesSeen{ 0 }, g_cyclesSeen{ 0 };
		bool                  g_countsInit{ false };
		quickitem::UseTracker g_tracker;
		std::string           g_trackName;
		// A cycle press went to Elden Ring then: ~kCycleCheckMs later say which item it is. Live 2026-10-31 the first D-pad down
		// after a while often switched nothing (also the Tarnished's own in Tarnished mode; the next ones did): an unchanged slot
		// with another item to go to gets one more press.
		constexpr std::uint64_t kCycleCheckMs = 400;
		std::uint64_t         g_cycleAt{ 0 };
		int                   g_cycleFrom{ -1 };
		int                   g_cycleRetries{ 0 };
		std::uint64_t         g_cycleRetried{ 0 };
		std::uint64_t         g_uses{ 0 }, g_used{ 0 }, g_notUsed{ 0 }, g_refused{ 0 }, g_cycles{ 0 };
		std::string           g_last;          // the last outcome (console)

		bool Is(const std::uint8_t* a_obj, int a_slot, std::string_view a_name, std::string& a_why, std::string_view a_where)
		{
			const auto vptr = a_obj ? SafeRead<const void*>(a_obj) : std::nullopt;
			if (!vptr || !*vptr) {
				a_why = std::format("no {} at {}", a_name, a_where);
				return false;
			}
			if (!g_vt[a_slot] && game::RttiNameOfVtable(*vptr) == a_name) {
				g_vt[a_slot] = *vptr;
			}
			if (*vptr != g_vt[a_slot]) {
				a_why = std::format("{} is a {}, not {}", a_where, game::RttiNameOfVtable(*vptr), a_name);
				return false;
			}
			return true;
		}

		quickitem::Slot SlotOf(const Readout& a_r)
		{
			quickitem::Slot s;
			s.goodsId = a_r.valid ? a_r.goods.id : -1;
			s.quantity = a_r.quantity;
			s.empty = a_r.valid && a_r.verdict == quickitem::Verdict::kEmpty;
			return s;
		}

		std::string Describe(const Readout& a_r)
		{
			if (!a_r.valid) {
				return "unreadable: " + a_r.why;
			}
			if (a_r.goods.id < 0) {
				return std::format("slot {}: empty", a_r.selected);
			}
			static constexpr const char* kVerdicts[] = { "usable", "empty", "Tarnished mode only", "no item" };
			return std::format("slot {}: {} (goods {}) x{}, anim {} menu {} consume {} -> {}", a_r.selected, quickitem::Shown(a_r.goods.id), a_r.goods.id,
				a_r.quantity, a_r.goods.useAnim, a_r.goods.menuType, a_r.goods.consume ? 1 : 0,
				a_r.goods.known ? kVerdicts[static_cast<int>(a_r.verdict)] : std::format("{} (no EquipParamGoods row: {})", kVerdicts[static_cast<int>(a_r.verdict)], g_goods.Describe()));
		}

		// Another quick slot than the selected one holds an item (a cycle has somewhere to go).
		bool OtherItems(const Frame& a_f)
		{
			auto* pgd = a_f.player ? SafePtr(a_f.player.chr + kPlayerGameData) : nullptr;
			if (!pgd) {
				return false;
			}
			auto*      eid = pgd + kPgdEquip + kEgdItemData;
			const auto sel = SafeRead<std::int32_t>(eid + kEidSelected).value_or(-1);
			for (int i = 0; i < kQuickSlots; ++i) {
				if (i != sel && SafeRead<std::uint32_t>(eid + kEidQuickSlots + 8 * static_cast<std::uint32_t>(i)).value_or(0) != 0) {
					return true;
				}
			}
			return false;
		}

		// Why Snake can't have the Tarnished use an item now (nullptr: he can).
		const char* Refusal(const Frame& a_f)
		{
			if (!Puppet::Active() || Host::Tarnished()) {
				return "Snake is not playing";
			}
			if (a_f.menuOpen || a_f.interacting || a_f.cutscene || a_f.loading) {
				return "Elden Ring has the pad";
			}
			if (a_f.dead || a_f.dying || Puppet::Dying()) {
				return "Snake is dying";
			}
			if (Puppet::Riding() && !Cfg().flaskWhileRiding) {
				return "riding ([Flask] while_riding = false)";
			}
			return nullptr;
		}

		void Use(const Frame& a_f, const char* a_how)
		{
			++g_uses;
			const auto& c = Cfg();
			if (const char* why = Refusal(a_f)) {
				++g_refused;
				Log::Info("flask: {} -- not now: {}", a_how, why);
				return;
			}
			const auto r = Read(a_f);
			if (!r.valid) {
				++g_refused;
				Hud::Notify("Quick item: can't read Elden Ring's quick slot (see the log)", Hud::Tone::kWarn);
				Log::Warn("flask: {} -- the quick slot can't be read ({}); nothing pressed", a_how, r.why);
				return;
			}
			const auto name = quickitem::Shown(r.goods.id);
			switch (r.verdict) {
			case quickitem::Verdict::kNoItem:
				++g_refused;
				Hud::Notify("No quick item in the slot", Hud::Tone::kWarn);
				g_last = "refused: no item";
				Log::Info("flask: {} -- {}: nothing to use", a_how, Describe(r));
				return;
			case quickitem::Verdict::kEmpty:
				++g_refused;
				Hud::Notify(quickitem::IsFlask(r.goods.id) ? std::format("{}: empty -- rest at a grace", name) : std::format("{}: none left", name), Hud::Tone::kWarn);
				g_last = std::format("refused: {} empty", name);
				Log::Info("flask: {} -- {}: refused (empty)", a_how, Describe(r));
				return;
			case quickitem::Verdict::kScreen:
				++g_refused;
				if (!r.goods.known) {
					static bool logged = false;
					if (!logged) {
						logged = true;
						Log::Warn("flask: no EquipParamGoods row for goods {}: {} -- only flasks are pressed for Snake", r.goods.id, g_goods.Describe());
					}
				}
				Hud::Notify(std::format("{}: Tarnished mode only (View+Start)", name), Hud::Tone::kWarn);
				g_last = std::format("refused: {} opens a screen", name);
				Log::Info("flask: {} -- {}: refused (it opens one of Elden Ring's screens, calls Torrent or takes the camera)", a_how, Describe(r));
				return;
			case quickitem::Verdict::kUse:
				break;
			}
			InputBridge::PressErButtons(c.flaskErUse, static_cast<unsigned>(c.flaskPressMs));
			g_tracker.Begin(SlotOf(r), r.goods.consume, ::GetTickCount64());
			g_trackName = name;
			Log::Info("flask: {} -> the hidden Tarnished presses his item button ({:#06x}, {} ms): {}; HP {}/{}", a_how, c.flaskErUse, c.flaskPressMs, Describe(r),
				game::Hp(a_f.player), game::MaxHp(a_f.player));
		}

		void Next(const Frame& a_f, const char* a_how)
		{
			const auto& c = Cfg();
			if (const char* why = Refusal(a_f)) {
				Log::Info("flask: {} (next quick item) -- not now: {}", a_how, why);
				return;
			}
			++g_cycles;
			InputBridge::PressErButtons(c.flaskErCycle, static_cast<unsigned>(c.flaskPressMs));
			g_cycleAt = ::GetTickCount64();
			g_cycleFrom = Read(a_f).selected;
			g_cycleRetries = 0;
			Log::Info("flask: {} -> the hidden Tarnished's next quick item ({:#06x})", a_how, c.flaskErCycle);
		}
	}

	Readout Read(const Frame& a_f)
	{
		Readout r;
		if (!a_f.player || !a_f.inGame) {
			r.why = "no player in the world";
			return r;
		}
		auto* pgd = SafePtr(a_f.player.chr + kPlayerGameData);
		if (!Is(pgd, 0, "CS::PlayerGameData", r.why, "PlayerIns+0x580")) {
			return r;
		}
		auto* egd = pgd + kPgdEquip;
		auto* inv = egd + kEgdInventory;
		auto* eid = egd + kEgdItemData;
		if (!Is(egd, 1, "CS::EquipGameData", r.why, "PlayerGameData+0x2B0") || !Is(inv, 2, "CS::EquipInventoryData", r.why, "EquipGameData+0x158") ||
			!Is(eid, 3, "CS::EquipItemData", r.why, "EquipGameData+0x288")) {
			return r;
		}
		if (SafePtr(eid + kEidInventoryPtr) != inv) {
			r.why = "EquipItemData+0x98 doesn't point at EquipGameData+0x158";
			return r;
		}
		r.selected = SafeRead<std::int32_t>(eid + kEidSelected).value_or(-1);
		if (r.selected < 0 || r.selected >= kQuickSlots) {
			r.why = std::format("selected quick slot {} is not 0..9", r.selected);
			return r;
		}
		auto* slot = eid + kEidQuickSlots + 8 * static_cast<std::uint32_t>(r.selected);
		r.handle = SafeRead<std::uint32_t>(slot).value_or(0);
		r.index = SafeRead<std::int32_t>(slot + 4).value_or(-1);
		r.valid = true;
		if (r.handle == 0 || r.index < 0) {
			r.verdict = quickitem::Verdict::kNoItem;
			return r;
		}
		// The inventory entry: key items below the key capacity, normal ones after.
		const auto    keyCap = SafeRead<std::uint32_t>(inv + kInvKeyCap).value_or(0);
		const auto    normalCap = SafeRead<std::uint32_t>(inv + kInvNormalCap).value_or(0);
		std::uint8_t* entry = nullptr;
		const auto    idx = static_cast<std::uint32_t>(r.index);
		if (idx < keyCap) {
			auto*      head = SafePtr(inv + kInvKeyAccessor);
			auto*      lenAt = SafePtr(inv + kInvKeyAccessor + 8);
			const auto len = lenAt ? SafeRead<std::uint32_t>(lenAt).value_or(0) : 0u;
			entry = head && idx < len ? head + kEntryBytes * idx : nullptr;
		} else if (idx - keyCap < normalCap) {
			auto* head = SafePtr(inv + kInvNormalHead);
			entry = head ? head + kEntryBytes * (idx - keyCap) : nullptr;
		}
		const auto entryHandle = entry ? SafeRead<std::uint32_t>(entry).value_or(0) : 0u;
		const auto itemId = entry ? SafeRead<std::uint32_t>(entry + 4).value_or(0) : 0u;
		// The slot keeps the full flask's handle when Elden Ring swaps its entry to the empty twin in place (live 2026-10-31: the
		// last Crimson Tears drunk, entry 397 became goods 1000 while the slot still said 1001); the entry is the truth then.
		const bool twin = quickitem::FlaskTwins(static_cast<int>(entryHandle & 0x0FFFFFFF), static_cast<int>(r.handle & 0x0FFFFFFF));
		if (!entry || (entryHandle != r.handle && !twin) || (itemId >> 28) != 4 || (itemId & 0x0FFFFFFF) != (entryHandle & 0x0FFFFFFF)) {
			r.valid = false;
			r.why = std::format("quick slot {} (handle {:#x}, index {}) doesn't match its inventory entry (handle {:#x}, item {:#x}; key capacity {}, normal {})",
				r.selected, r.handle, r.index, entryHandle, itemId, keyCap, normalCap);
			return r;
		}
		r.goods.id = static_cast<int>(itemId & 0x0FFFFFFF);
		r.quantity = static_cast<int>(SafeRead<std::uint32_t>(entry + 8).value_or(0));
		if (auto* row = g_goods.Row(static_cast<std::uint32_t>(r.goods.id))) {
			r.goods.known = true;
			r.goods.useAnim = SafeRead<std::uint8_t>(row + quickitem::kGoodsUseAnim).value_or(0);
			r.goods.menuType = SafeRead<std::uint8_t>(row + quickitem::kGoodsMenuType).value_or(0);
			r.goods.consume = (SafeRead<std::uint8_t>(row + quickitem::kGoodsBits48).value_or(0) & 0x80) != 0;
		} else {
			// No row: a flask is still counted (every flask is a consumable); everything else is refused (Classify).
			r.goods.consume = quickitem::IsFlask(r.goods.id);
		}
		r.verdict = quickitem::Classify(r.goods, r.quantity);
		return r;
	}

	void Tick(const Frame& a_f)
	{
		const auto& c = Cfg();
		const auto  uses = InputBridge::ItemUses();
		const auto  cycles = InputBridge::ItemCycles();
		if (!g_countsInit) {
			g_countsInit = true;
			g_usesSeen = uses;
			g_cyclesSeen = cycles;
		}
		if (uses != g_usesSeen) {
			g_usesSeen = uses;
			if (c.flask) {
				Use(a_f, std::format("View + {}", c.flaskButtonName).c_str());
			}
		}
		if (cycles != g_cyclesSeen) {
			g_cyclesSeen = cycles;
			if (c.flask) {
				Next(a_f, std::format("View + {}", c.flaskCycleName).c_str());
			}
		}
		const auto now = ::GetTickCount64();
		if (g_cycleAt && now - g_cycleAt >= kCycleCheckMs) {
			g_cycleAt = 0;
			const auto r = Read(a_f);
			// Not while a use is being followed (review of 0.6.0): the retry would switch the item under the use tracker, which then
			// says "couldn't use it now" for a flask that was drunk.
			if (r.selected >= 0 && r.selected == g_cycleFrom && g_cycleRetries == 0 && !g_tracker.Pending() && OtherItems(a_f) && !Refusal(a_f)) {
				++g_cycleRetries;
				++g_cycleRetried;
				InputBridge::PressErButtons(c.flaskErCycle, static_cast<unsigned>(c.flaskPressMs));
				g_cycleAt = now;
				Log::Info("flask: the quick slot is still {} -- one more press", r.selected);
				return;
			}
			if (r.valid) {
				const bool empty = r.verdict == quickitem::Verdict::kEmpty;
				Hud::Notify(quickitem::CycleText(r.goods.id, r.quantity, empty), empty ? Hud::Tone::kWarn : Hud::Tone::kInfo);
			}
			Log::Info("flask: the quick item is now {}", Describe(r));
		}
		if (!g_tracker.Pending()) {
			return;
		}
		const auto r = Read(a_f);
		switch (g_tracker.Update(SlotOf(r), now, static_cast<std::uint64_t>(c.flaskConfirmMs))) {
		case quickitem::UseTracker::Outcome::kUsed:
			++g_used;
			Hud::Notify(g_tracker.Left() > 0 ? std::format("{}: {} left", g_trackName, g_tracker.Left()) : std::format("{}: none left", g_trackName),
				g_tracker.Left() > 0 ? Hud::Tone::kGood : Hud::Tone::kWarn);
			g_last = std::format("{} used, {} left", g_trackName, g_tracker.Left());
			Log::Info("flask: {} used {} ms after the press, {} left; HP {}/{} (the heal goes to Snake through forward_heals)", g_trackName, now - g_tracker.Since(),
				g_tracker.Left(), game::Hp(a_f.player), game::MaxHp(a_f.player));
			break;
		case quickitem::UseTracker::Outcome::kAssumed:
			// Not a consumable (spirit ashes): nothing to count, and Elden Ring may still refuse it (no summoning area): "pressed",
			// never "used" (review of 0.6.0).
			++g_used;
			Hud::Notify(std::format("{}: pressed", g_trackName), Hud::Tone::kInfo);
			g_last = std::format("{} pressed (not counted)", g_trackName);
			Log::Info("flask: {} pressed (not a consumable: nothing to count, Elden Ring may still refuse it)", g_trackName);
			break;
		case quickitem::UseTracker::Outcome::kNotUsed:
			++g_notUsed;
			Hud::Notify(std::format("{}: the Tarnished couldn't use it now", g_trackName), Hud::Tone::kWarn);
			g_last = std::format("{} not used", g_trackName);
			Log::Info("flask: {} -- its count didn't drop within {} ms: Elden Ring didn't use it ({})", g_trackName, c.flaskConfirmMs, Describe(r));
			break;
		default:
			break;
		}
	}

	std::string Command(const Frame& a_f, std::string_view a_args)
	{
		if (a_args == "use") {
			Use(a_f, "console `flask use`");
			return "pressed (see the log for the outcome)";
		}
		if (a_args == "next") {
			Next(a_f, "console `flask next`");
			return "next quick item";
		}
		if (a_args == "slots") {
			std::string out;
			const auto  cur = Read(a_f);
			out += std::format("selected: {}\n", Describe(cur));
			if (!cur.valid || !a_f.player) {
				return out;
			}
			auto* eid = SafePtr(a_f.player.chr + kPlayerGameData) + kPgdEquip + kEgdItemData;
			for (int i = 0; i < kQuickSlots; ++i) {
				const auto h = SafeRead<std::uint32_t>(eid + kEidQuickSlots + 8 * i).value_or(0);
				const auto x = SafeRead<std::int32_t>(eid + kEidQuickSlots + 8 * i + 4).value_or(-1);
				if (h) {
					out += std::format("  slot {}: handle {:#x} index {} ({})\n", i, h, x, quickitem::Shown(static_cast<int>(h & 0x0FFFFFFF)));
				}
			}
			return out;
		}
		const auto& c = Cfg();
		return std::format("[Flask] {} (View + {} uses, View + {} next; Elden Ring's buttons {:#06x} / {:#06x}, {} ms); presses {} ({} used, {} not used, {} refused), "
		                   "cycles {} ({} pressed twice); last: {}; now {}",
			c.flask ? "on" : "off", c.flaskButtonName, c.flaskCycleName, c.flaskErUse, c.flaskErCycle, c.flaskPressMs, g_uses, g_used, g_notUsed, g_refused, g_cycles,
			g_cycleRetried,
			g_last.empty() ? "-" : g_last, Describe(Read(a_f)));
	}
}
