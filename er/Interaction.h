#pragma once

// An Elden Ring interaction that Snake started with the prompt button (re-backlog E6): resting at a grace, picking
// up an item, a lever, a door, talking. Elden Ring runs it with its own player, so for as long as it lasts Elden Ring
// gets the whole controller (its grace menu needs the stick and B), MGSV a neutral pad and Snake holds still --
// exactly what an Elden Ring menu gets (er/Host.cpp folds this into menuOpen).
//
// How long: the grace menu is not a "blocking" menu (CSMenuMan+0x798 stays 0, VERIFIED live) and its UI state is not
// one Start/View learning finds, but Elden Ring hides its in-game HUD while you rest, talk or read: CSMenuMan UI state
// 7 loses its visible bit (VERIFIED live at the Gatefront grace: 03 -> 01, back on leaving), 2.3-2.9 s after the press
// (the sit-down). So: at least [Input] interaction_min_ms (3.5 s: covers the sit-down; a pickup is over in about a
// second), then until the HUD is visible again and no Elden Ring menu is open (er/Host.cpp only passes the HUD on when
// it was up at the press: ER's "auto" HUD may hide by itself); a load, the player's death or interaction_max_ms end
// it too.
// Until the HUD has hidden, Elden Ring only gets the buttons a prompt, a pickup or a dialog needs (ButtonsOnly ->
// InputBridge::SetButtonsOnly): the sticks, bumpers and triggers would walk, roll and swing the hidden Tarnished for
// the rest of the 3.5 s after a 1 s pickup while Snake stands still, and Snake would jump to wherever it went.
// Pure logic (unit-tested in tests/test_host.cpp).

#include <cstdint>

namespace phantom::er
{
	class Interaction
	{
	public:
		struct Input
		{
			bool          pressed{ false };    // a prompt-button press went to Elden Ring this frame
			bool          hudHidden{ false };  // the HUD UI state is hidden (false when unknown)
			bool          menuOpen{ false };   // any other Elden Ring menu
			bool          inWorld{ true };     // false while loading
			bool          dead{ false };
			std::uint64_t nowMs{ 0 };
			std::uint64_t minMs{ 1500 };
			std::uint64_t maxMs{ 0 };          // 0 = no cap
			bool          abort{ false };      // ends it now (a press-less start whose Tarnished walked away: no screen)
		};

		// True while the interaction lasts.
		bool Update(const Input& a_in);

		[[nodiscard]] bool          Active() const { return active_; }
		[[nodiscard]] std::uint64_t Since() const { return start_; }
		[[nodiscard]] bool          SawHudHidden() const { return sawHidden_; }
		// Active, and the HUD has not hidden yet (a pickup, a lever, the sit-down before the grace menu).
		[[nodiscard]] bool ButtonsOnly() const { return active_ && !sawHidden_; }

	private:
		bool          active_{ false };
		bool          sawHidden_{ false };
		std::uint64_t start_{ 0 };
	};

	// Elden Ring's HUD on "Auto" (review 2026-10-13; UNVERIFIED whether that clears UI state 7 at all): the press-less starts in
	// er/Host.cpp (an NPC's dialog, the next screen, the grace's next screen) trust any HUD hide, and the grace's has no time
	// window -- an auto HUD fading at the grace after a rest (HP/FP full) would start an "interaction" that lasts until the HUD
	// comes back, Snake on a neutral pad meanwhile. A hide nothing explains (Snake playing in the world: no press, menu,
	// cutscene, load or death, no prompt or interaction end within the last second, not at a grace, no rest prompt) is an
	// auto HUD's fade; the kAutoHudHides-th such hide (a one-off stays possible) turns the press-less starts off for the session.
	class AutoHud
	{
	public:
		static constexpr int kAutoHudHides = 2;
		struct Input
		{
			bool hid{ false };        // the HUD state turned hidden this frame
			bool explained{ false };  // something above explains it
		};
		// True on the frame it is detected.
		bool Update(const Input& a_in);
		[[nodiscard]] bool Detected() const { return detected_; }
		[[nodiscard]] int  Unexplained() const { return unexplained_; }

	private:
		int  unexplained_{ 0 };
		bool detected_{ false };
	};
}
