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
	}
}
