#include "MenuWatch.h"

#include "Config.h"
#include "Game.h"
#include "InputBridge.h"
#include "Log.h"

#include <algorithm>
#include <format>

#include <windows.h>

namespace phantom::er::MenuWatch
{
	namespace
	{
		constexpr std::uint8_t kVisible = 0x02;

		std::vector<int>          g_menuStates;    // learned or configured
		std::vector<std::uint8_t> g_last;          // last frame's UI states
		std::vector<std::uint8_t> g_beforePress;   // UI states right before the latest Start/View press
		std::vector<int>          g_candidates;    // turned visible since that press
		std::uint32_t             g_presses{ 0 };
		std::uint64_t             g_latchUntil{ 0 };
		bool                      g_learning{ false };
		bool                      g_open{ false };
		// Menu entries count only while "armed": they turned visible right after a Start/View press (inside the latch) or
		// while a menu was already open (a submenu, the map from the menu). The same UI entries also come up on their own
		// with ER's HUD (live: the map shows entries 0x1c and 0x17, and 0x17 also comes up with a boss's health bar:
		// Elden Ring kept the controller for the whole fight), so:
		//  - entries armed together form a group, and the group is open only while ALL of them are still visible (closing
		//    the map hides 0x1c: the group closes even though the boss bar keeps 0x17 up);
		//  - an entry that turns visible with no press and no menu open is ER's HUD: it is un-learned.
		std::vector<std::vector<int>> g_groups;
		// Entries ever seen as Elden Ring's HUD: turned visible with no press and no menu open, or already visible when a
		// Start/View press came with no menu open (GroupOpen).
		std::vector<int>          g_hudToo;
		Force                     g_force{ Force::kAuto };
		// Cutscenes: Start skips them (ER owns the pad during one anyway), and a boss intro ends with the boss's health bar
		// coming up -- live (Margit): a skip press opened the latch, the bar's entries 0x12/0x17 were learned as a menu and
		// Elden Ring kept the controller for the whole fight. Presses during a cutscene and shortly after it never latch or
		// learn, and whatever turns visible in that window is Elden Ring's HUD.
		std::uint64_t             g_afterCutsceneUntil{ 0 };
		constexpr std::uint64_t   kAfterCutsceneMs = 4000;

		void AddHud(int a_index)
		{
			if (std::ranges::find(g_hudToo, a_index) == g_hudToo.end()) {
				g_hudToo.push_back(a_index);
			}
		}

		bool Visible(const std::vector<std::uint8_t>& a_states, int a_index)
		{
			return a_index >= 0 && static_cast<std::size_t>(a_index) < a_states.size() && (a_states[a_index] & kVisible);
		}
	}

	void Init()
	{
		g_menuStates = Cfg().menuUiStates;
		g_presses = InputBridge::MenuPresses();
		if (!g_menuStates.empty()) {
			Log::Info("menu watch: {} configured menu UI states", g_menuStates.size());
		}
	}

	void Tick()
	{
		const auto now = ::GetTickCount64();
		auto       states = game::UiStates();

		if (Log::DebugEnabled() && !g_last.empty() && states.size() == g_last.size()) {
			for (std::size_t i = 0; i < states.size(); ++i) {
				if (states[i] != g_last[i]) {
					Log::Debug("ui state {:#04x}: {:#04x} -> {:#04x}", i, g_last[i], states[i]);
				}
			}
		}

		if (game::CutscenePlaying().value_or(false)) {
			g_afterCutsceneUntil = now + kAfterCutsceneMs;
			g_learning = false;
			g_latchUntil = 0;
		}
		const bool aroundCutscene = now < g_afterCutsceneUntil;
		const auto presses = InputBridge::MenuPresses();
		if (presses != g_presses && aroundCutscene) {
			g_presses = presses;  // a cutscene skip, not a menu
		}
		if (presses != g_presses) {
			g_presses = presses;
			// What is up when a press comes with no menu open is Elden Ring's HUD (a boss bar), not the menu it opens.
			if (!g_open) {
				for (std::size_t i = 0; i < g_last.size(); ++i) {
					if (Visible(g_last, static_cast<int>(i))) {
						AddHud(static_cast<int>(i));
					}
				}
			}
			g_latchUntil = now + static_cast<std::uint64_t>(std::max(0, Cfg().menuLatchMs));
			g_beforePress = g_last;
			g_candidates.clear();
			g_learning = Cfg().menuLearn && !states.empty();
		}
		if (g_learning && states.size() == g_beforePress.size()) {
			for (std::size_t i = 0; i < states.size(); ++i) {
				const int index = static_cast<int>(i);
				if (Visible(states, index) && !Visible(g_beforePress, index) && std::ranges::find(g_candidates, index) == g_candidates.end()) {
					g_candidates.push_back(index);
				}
			}
		}
		if (g_learning && now >= g_latchUntil) {
			g_learning = false;
			std::vector<int> opened;
			for (const int index : g_candidates) {
				// Only keep entries that are still up at the end of the latch (a menu stays open; a
				// flash of HUD doesn't).
				if (Visible(states, index) && std::ranges::find(g_menuStates, index) == g_menuStates.end()) {
					g_menuStates.push_back(index);
					Log::Info("menu watch: learned ER menu UI state {:#04x} (pin with menu_ui_states)", index);
				}
				if (Visible(states, index)) {
					opened.push_back(index);  // it opened after the press: the menu that press opened
				}
			}
			if (!opened.empty()) {
				g_groups.push_back(std::move(opened));
			}
		}

		const bool latched = now < g_latchUntil;
		const bool wasOpen = g_open;
		// Any entry turning visible with no press and no menu open is (also) Elden Ring's HUD; so is anything that comes up
		// around a cutscene (a boss intro's health bar).
		if (((!latched && !wasOpen) || aroundCutscene) && g_last.size() == states.size()) {
			for (std::size_t i = 0; i < states.size(); ++i) {
				if (Visible(states, static_cast<int>(i)) && !Visible(g_last, static_cast<int>(i))) {
					AddHud(static_cast<int>(i));
				}
			}
		}
		// A group closes when its menu's own entries have (and never comes back by itself): GroupOpen.
		std::erase_if(g_groups, [&](const std::vector<int>& a_group) { return !GroupOpen(a_group, g_hudToo, states); });
		std::vector<int> turned;
		std::vector<int> unlearn;
		for (const int index : g_menuStates) {
			const bool turnedVisible = Visible(states, index) && (g_last.size() != states.size() || !Visible(g_last, index));
			if (!turnedVisible) {
				continue;
			}
			if ((latched || wasOpen) && !aroundCutscene) {
				turned.push_back(index);
			} else {
				unlearn.push_back(index);
			}
		}
		if (!turned.empty() && !g_learning) {
			g_groups.push_back(std::move(turned));  // while learning, the end of the latch arms what is still up
		}
		for (const int index : unlearn) {
			std::erase(g_menuStates, index);
			Log::Info("menu watch: UI state {:#04x} turned visible without a Start/View press: ER's HUD, not a menu -- un-learned", index);
		}
		bool open = latched || !g_groups.empty();
		if (g_force != Force::kAuto) {
			open = g_force == Force::kOpen;
		}
		if (open != g_open) {
			Log::Debug("menu watch: ER menu {}", open ? "open" : "closed");
		}
		g_open = open;
		g_last = std::move(states);
	}

	bool GroupOpen(const std::vector<int>& a_group, const std::vector<int>& a_hudToo, const std::vector<std::uint8_t>& a_states)
	{
		bool own = false, ownVisible = false, allVisible = true;
		for (const int index : a_group) {
			const bool visible = Visible(a_states, index);
			allVisible = allVisible && visible;
			if (std::ranges::find(a_hudToo, index) == a_hudToo.end()) {
				own = true;
				ownVisible = ownVisible || visible;
			}
		}
		return own ? ownVisible : allVisible;
	}

	bool Open() { return g_open; }

	void SetForce(Force a_force) { g_force = a_force; }

	std::string Describe()
	{
		std::string list;
		for (const int i : g_menuStates) {
			list += std::format("{}{:#04x}", list.empty() ? "" : ",", i);
		}
		std::string armed;
		for (const auto& group : g_groups) {
			armed += armed.empty() ? "(" : " (";
			for (std::size_t k = 0; k < group.size(); ++k) {
				armed += std::format("{}{:#04x}", k ? "," : "", group[k]);
			}
			armed += ")";
		}
		std::string hud;
		for (const int i : g_hudToo) {
			hud += std::format("{}{:#04x}", hud.empty() ? "" : ",", i);
		}
		return std::format("menu {} (force {}, states [{}], open [{}], HUD too [{}], latch {} ms left)", g_open ? "OPEN" : "closed",
			g_force == Force::kAuto ? "auto" : (g_force == Force::kOpen ? "open" : "closed"), list, armed, hud,
			g_latchUntil > ::GetTickCount64() ? g_latchUntil - ::GetTickCount64() : 0);
	}

	std::string DumpUi()
	{
		const auto  states = game::UiStates();
		std::string out;
		for (std::size_t i = 0; i < states.size(); ++i) {
			out += std::format("{:02x}{}", states[i], (i % 16 == 15) ? "\n" : " ");
		}
		return states.empty() ? std::string("CSMenuMan unavailable") : out;
	}
}
