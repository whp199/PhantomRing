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
		// Elden Ring kept the controller for the whole fight. Presses during a cutscene and its skip/fade tail never latch
		// (SwallowPress); for 4 s after it whatever turns visible is Elden Ring's HUD and never arms a menu group, even inside
		// a latch (WindowArm) -- a press there still latches, so a menu opened right after a cutscene has the pad for the
		// latch (it used to be swallowed for the whole 4 s, leaving the pad with MGSV inside Elden Ring's menu).
		std::uint64_t             g_cutsceneEndedAt{ 0 };
		bool                      g_cutsceneWas{ false };
		std::vector<int>          g_windowHud;     // turned visible inside the current after-cutscene window
		// Elden Ring's full-screen menus (inventory, equipment, status, the map): its in-game HUD ([Input] interaction_hud_state,
		// UI state 7) hides as one opens from the main menu, and the main menu's own entries (0x1c, 0x17) hide too -- live
		// 2026-10-13, Start -> Inventory: the main menu's group closed, the inventory's entries (0x1f, 0x20; equipment 0x1d)
		// came up a frame later "with no menu open" and were taken for HUD, and A/B/D-pad went to MGSV inside the inventory
		// (only Start got out). The HUD hiding while a menu is open keeps it open until the HUD is back (FullScreenMenu).
		bool                      g_fullScreen{ false };
		std::uint64_t             g_openSeenAt{ 0 };  // a menu was open (or latched) then: the main menu's group closes a frame or
		                                              // two before the HUD hides as the inventory opens (live)
		bool                      g_pressInWindow{ false };  // the latest latch began inside that window

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

		// Elden Ring's menus opened from the keyboard (roadmap 1.8: Esc went unnoticed, MGSV's HUD and the throw markers stayed
		// over the menu, and its UI entries came up "with no press" and were taken for HUD): [Input] menu_keys pressed in Elden
		// Ring's window count like the pad's Start. Elden Ring reads the keyboard itself; this only watches the key state
		// once a frame (down edge, or the "pressed since the last call" bit for a tap shorter than a frame).
		bool          g_keyDown[256]{};
		std::uint64_t g_keyPresses{ 0 };

		void PollMenuKeys()
		{
			const auto& keys = Cfg().menuKeys;
			if (keys.empty()) {
				return;
			}
			HWND  fg = ::GetForegroundWindow();
			DWORD pid = 0;
			if (fg) {
				::GetWindowThreadProcessId(fg, &pid);
			}
			const bool ours = !fg || pid == ::GetCurrentProcessId();  // never a key typed into another program
			for (const int vk : keys) {
				if (vk <= 0 || vk > 0xFF) {
					continue;
				}
				const SHORT state = ::GetAsyncKeyState(vk);
				const bool  down = (state & 0x8000) != 0;
				const bool  pressed = (down && !g_keyDown[vk]) || (!down && (state & 0x0001));
				g_keyDown[vk] = down;
				if (pressed && ours) {
					++g_keyPresses;
					InputBridge::NoteMenuPress();
					Log::Info("menu watch: key {:#04x} ({}) pressed in Elden Ring's window -- counted as a menu press", vk,
						vk == VK_ESCAPE ? "Esc: its menu" : (vk == 'G' ? "G: its map" : "a menu key"));
				}
			}
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
		PollMenuKeys();

		if (Log::DebugEnabled() && !g_last.empty() && states.size() == g_last.size()) {
			for (std::size_t i = 0; i < states.size(); ++i) {
				if (states[i] != g_last[i]) {
					Log::Debug("ui state {:#04x}: {:#04x} -> {:#04x}", i, g_last[i], states[i]);
				}
			}
		}

		const bool cutscene = game::CutscenePlaying().value_or(false);
		if (cutscene) {
			if (!g_cutsceneWas) {
				g_windowHud.clear();
			}
			g_cutsceneEndedAt = now;
			g_learning = false;
			g_latchUntil = 0;
		}
		g_cutsceneWas = cutscene;
		const bool aroundCutscene = g_cutsceneEndedAt && now < g_cutsceneEndedAt + kAfterCutsceneMs;
		if (aroundCutscene && g_last.size() == states.size()) {
			for (std::size_t i = 0; i < states.size(); ++i) {
				const int index = static_cast<int>(i);
				if (Visible(states, index) && !Visible(g_last, index) && std::ranges::find(g_windowHud, index) == g_windowHud.end()) {
					g_windowHud.push_back(index);
				}
			}
		}
		const auto presses = InputBridge::MenuPresses();
		if (presses != g_presses && SwallowPress(cutscene, now, g_cutsceneEndedAt)) {
			g_presses = presses;  // a cutscene skip, not a menu
			Log::Info("menu watch: a Start/View press during a cutscene or its {} ms tail -- a skip, not a menu", kSkipTailMs);
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
			g_pressInWindow = aroundCutscene;
			g_beforePress = g_last;
			g_candidates.clear();
			g_learning = Cfg().menuLearn && !states.empty();
		}
		const int hudIndex = Cfg().interactionHudState;  // Elden Ring's in-game HUD itself: never a menu entry
		if (g_learning && states.size() == g_beforePress.size()) {
			for (std::size_t i = 0; i < states.size(); ++i) {
				const int index = static_cast<int>(i);
				if (index != hudIndex && Visible(states, index) && !Visible(g_beforePress, index) &&
					std::ranges::find(g_candidates, index) == g_candidates.end()) {
					g_candidates.push_back(index);
				}
			}
		}
		if (g_learning && now >= g_latchUntil) {
			g_learning = false;
			// Inside the after-cutscene window what came up is a boss bar at least as likely as a menu (0x17 is both):
			// never learned, never armed.
			const auto kept = g_pressInWindow ? WindowArm(g_candidates, g_windowHud) : g_candidates;
			if (kept.size() != g_candidates.size()) {
				Log::Info("menu watch: {} UI state(s) came up right after a cutscene inside the latch -- Elden Ring's HUD (a boss "
					"bar), not learned", g_candidates.size() - kept.size());
			}
			g_candidates = kept;
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
		const auto groupsBefore = g_groups.size();
		std::erase_if(g_groups, [&](const std::vector<int>& a_group) { return !GroupOpen(a_group, g_hudToo, states); });
		// A submenu replacing its menu in the same frame (Start -> Inventory: the main menu's 0x1c hid as the inventory's
		// 0x1f/0x20 came up, the HUD a frame later): what came up arms a group of its own. Without it the menu read closed for
		// that frame, Elden Ring's pad lost the A still held and saw it pressed again a frame later -- live, one A opened the
		// inventory AND the first item's context menu (one A in the equipment equipped a weapon).
		if (wasOpen && g_groups.size() < groupsBefore && g_last.size() == states.size() && !aroundCutscene) {
			std::vector<int> replaced;
			for (std::size_t i = 0; i < states.size(); ++i) {
				const int index = static_cast<int>(i);
				if (index != hudIndex && Visible(states, index) && !Visible(g_last, index)) {
					replaced.push_back(index);
				}
			}
			// Only what is not also Elden Ring's HUD (review: closing the map during Margit, the boss bar 0x12 coming back in the
			// same frame armed a group {0x12} that GroupOpen kept open for the whole fight, and learned 0x12 as a menu).
			auto group = SubmenuGroup(replaced, g_hudToo, g_windowHud);
			for (const int index : group) {
				if (std::ranges::find(g_hudToo, index) == g_hudToo.end() &&
					std::ranges::find(g_menuStates, index) == g_menuStates.end()) {
					g_menuStates.push_back(index);
					Log::Info("menu watch: learned ER menu UI state {:#04x} (a submenu replacing its menu)", index);
				}
			}
			if (!group.empty()) {
				g_groups.push_back(std::move(group));
			} else if (!replaced.empty()) {
				Log::Info("menu watch: {} UI state(s) came up as a menu closed, all of them Elden Ring's HUD too (a boss bar) -- no "
					"submenu", replaced.size());
			}
		}
		std::vector<int> turned;
		std::vector<int> unlearn;
		std::erase(g_menuStates, hudIndex);
		for (const int index : g_menuStates) {
			const bool turnedVisible = Visible(states, index) && (g_last.size() != states.size() || !Visible(g_last, index));
			if (!turnedVisible) {
				continue;
			}
			if (latched || wasOpen) {
				turned.push_back(index);  // never un-learned inside a latch, cutscene or not
			} else {
				unlearn.push_back(index);
			}
		}
		if (aroundCutscene) {
			turned = WindowArm(turned, g_windowHud);
		}
		if (!turned.empty() && !g_learning) {
			g_groups.push_back(std::move(turned));  // while learning, the end of the latch arms what is still up
		}
		for (const int index : unlearn) {
			std::erase(g_menuStates, index);
			Log::Info("menu watch: UI state {:#04x} turned visible without a Start/View press: ER's HUD, not a menu -- un-learned", index);
		}
		const bool known = hudIndex >= 0 && static_cast<std::size_t>(hudIndex) < states.size() && g_last.size() == states.size();
		if (wasOpen || latched) {
			g_openSeenAt = now;
		}
		const bool recentlyOpen = g_openSeenAt && now < g_openSeenAt + kSubmenuGapMs;
		const bool fullScreen = known && FullScreenMenu(g_fullScreen, recentlyOpen, Visible(g_last, hudIndex), Visible(states, hudIndex),
		                                                game::CutscenePlaying().value_or(false));
		if (fullScreen != g_fullScreen) {
			Log::Info("menu watch: {}", fullScreen ? "Elden Ring's HUD hid inside its menu -- a full-screen menu (inventory, equipment, map): "
			                                         "open until the HUD is back" :
			                                         "Elden Ring's HUD is back -- its full-screen menu closed");
		}
		g_fullScreen = fullScreen;
		bool open = latched || !g_groups.empty() || g_fullScreen;
		if (g_force != Force::kAuto) {
			open = g_force == Force::kOpen;
		}
		if (open != g_open) {
			Log::Debug("menu watch: ER menu {}", open ? "open" : "closed");
		}
		g_open = open;
		g_last = std::move(states);
	}

	bool FullScreenMenu(bool a_was, bool a_menuOpenBefore, bool a_hudVisibleBefore, bool a_hudVisibleNow, bool a_cutscene)
	{
		if (a_cutscene || a_hudVisibleNow) {
			return false;
		}
		return a_was || (a_menuOpenBefore && a_hudVisibleBefore);
	}

	bool SwallowPress(bool a_cutscenePlaying, std::uint64_t a_now, std::uint64_t a_cutsceneEndedAt)
	{
		return a_cutscenePlaying || (a_cutsceneEndedAt && a_now < a_cutsceneEndedAt + kSkipTailMs);
	}

	std::vector<int> WindowArm(const std::vector<int>& a_entries, const std::vector<int>& a_windowHud)
	{
		std::vector<int> out;
		for (const int index : a_entries) {
			if (std::ranges::find(a_windowHud, index) == a_windowHud.end()) {
				out.push_back(index);
			}
		}
		return out;
	}

	std::vector<int> SubmenuGroup(const std::vector<int>& a_cameUp, const std::vector<int>& a_hudToo, const std::vector<int>& a_windowHud)
	{
		auto group = WindowArm(a_cameUp, a_windowHud);
		const bool own = std::ranges::any_of(group, [&](int a_index) { return std::ranges::find(a_hudToo, a_index) == a_hudToo.end(); });
		if (!own) {
			group.clear();
		}
		return group;
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
		return std::format("menu {} (force {}, states [{}], open [{}], HUD too [{}], latch {} ms left, menu keys pressed {})", g_open ? "OPEN" : "closed",
			g_force == Force::kAuto ? "auto" : (g_force == Force::kOpen ? "open" : "closed"), list, armed, hud,
			g_latchUntil > ::GetTickCount64() ? g_latchUntil - ::GetTickCount64() : 0, g_keyPresses);
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
