#pragma once

// "Is an Elden Ring menu open?" (RE item E4: no single verified flag exists). While one is, ER must
// get the whole controller and MGSV a neutral one.
//
// Signal: CSMenuMan's UI state table (0x46 entries, bit1 = visible). Which entries are the pause
// menu, map, inventory... isn't known statically, so this learns it: when the player presses Start
// or View (they always reach ER), entries that turn visible within the latch window are recorded as
// menu entries (logged; pin them in PhantomRingHost.ini menu_ui_states). Until something is learned,
// a press opens a short latch so the first menu is usable. The console can force the answer.
// A menu entry only counts when it turned visible right after a Start/View press or while a menu was already open:
// the same entries come up with ER's HUD by themselves (a boss's health bar), and those are not menus.

#include <cstdint>
#include <string>
#include <vector>

namespace phantom::er
{
	namespace MenuWatch
	{
		void Init();
		void Tick();  // FrameBegin
		[[nodiscard]] bool Open();

		enum class Force
		{
			kAuto,
			kOpen,
			kClosed
		};
		void        SetForce(Force a_force);
		std::string Describe();
		std::string DumpUi();

		// Pure rule (tests/test_host.cpp): is a group of entries armed together still open? Entries also seen coming up as
		// Elden Ring's HUD (a_hudToo: turned visible with no press and no menu open, or already up when the press came)
		// neither keep it open nor close it: it stays open while any OTHER entry is visible. Without that, a banner or an
		// item-pickup line that came up inside the latch and hid again closed a still-open menu (the pad went back to
		// MGSV inside Elden Ring's menu), and a boss bar (0x17) kept the map's group (0x1c, 0x17) open after the map
		// closed. A group of HUD-too entries only closes as soon as any of them hides.
		bool GroupOpen(const std::vector<int>& a_group, const std::vector<int>& a_hudToo, const std::vector<std::uint8_t>& a_states);

		// Cutscenes (review 2026-10-13): a Start/View press while one plays, or within kSkipTailMs after it ended (the skip's
		// fade), is a skip and never latches. Later presses latch as usual -- they used to be swallowed for 4 s, leaving MGSV
		// the pad inside a menu opened right after a cutscene.
		constexpr std::uint64_t kSkipTailMs = 500;
		constexpr std::uint64_t kAfterCutsceneMs = 4000;
		bool SwallowPress(bool a_cutscenePlaying, std::uint64_t a_now, std::uint64_t a_cutsceneEndedAt);
		// Entries that turned visible in the kAfterCutsceneMs after a cutscene (a_windowHud: a boss intro's health bar, 0x12/
		// 0x17 for Margit, 0x17 also being the map's) never arm a menu group or get learned, even inside a latch: what is
		// left of a_entries.
		std::vector<int> WindowArm(const std::vector<int>& a_entries, const std::vector<int>& a_windowHud);
		// A full-screen Elden Ring menu (inventory, equipment, status, the map): its in-game HUD (UI state 7) hid while a menu was
		// open (a_menuOpenBefore: open or latched last frame, a_hudVisibleBefore -> !a_hudVisibleNow); it stays one while the
		// HUD is hidden (a_was), never in a cutscene. The HUD state itself is never learned or armed as a menu entry.
		// A submenu replacing its menu in the same frame (a_cameUp: turned visible as a group closed): the group it arms, empty
		// when none. Entries of the after-cutscene window never arm; a group needs at least one entry that is not also Elden
		// Ring's HUD (a_hudToo), else a boss bar coming back as the map closed would hold a menu "open" for the whole fight.
		std::vector<int> SubmenuGroup(const std::vector<int>& a_cameUp, const std::vector<int>& a_hudToo, const std::vector<int>& a_windowHud);
		bool FullScreenMenu(bool a_was, bool a_menuOpenBefore, bool a_hudVisibleBefore, bool a_hudVisibleNow, bool a_cutscene);
		// "Open before" means open within this long: Start -> Inventory closed the main menu's group (0x1c hid) a frame or more
		// before the HUD hid (live).
		constexpr std::uint64_t kSubmenuGapMs = 500;
	}
}
