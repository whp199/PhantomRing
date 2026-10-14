#pragma once

// The real controller goes to MGSV; Elden Ring gets a masked copy while it is a puppet (DESIGN.md §6).
//
// Elden Ring imports XInputGetState by ordinal (xinput1_4 #2). We detour the export inside
// xinput1_4.dll (a system DLL: Arxan never checks it, nothing in eldenring.exe is patched). Every
// call for the published pad writes PadState (seqlock, our own packet counter). What ER gets back:
//  - puppeting and no ER menu open: only the allow-list (Start = ER menu, Back/View = map), neutral
//    sticks and triggers -- plus the interact button while ER shows a prompt (SetPrompt); MGSV gets
//    the rest (not the allow-list, and not the interact button while the prompt is up). Start and View
//    pass through a gate (er/Chord.h): View held [Input] view_hold_ms is MGSV's iDroid instead, sent as
//    Start held [Input] guest_idroid_start_ms (ER gets View taps when they are released), Start reaches
//    ER [Input] start_wait_ms after it went down, and View + Start held together [Input] chord_ms is
//    the Tarnished-mode chord (Chords(); its presses reach no one);
//  - MGSV's own menu open (SetGuestMenu): MGSV gets everything, Elden Ring a neutral pad;
//  - an ER interaction before its HUD hides (SetButtonsOnly: a pickup, a lever, the grace sit-down): Elden Ring
//    gets the face buttons, D-pad, Start and View only (UNVERIFIED live), MGSV a neutral pad;
//  - otherwise (ER menu open, guest not alive, not puppeting): the real state, and MGSV is told to
//    use a neutral pad (kPadNeutral) while an ER menu or loading owns the controller.
// Keyboard and mouse are left to Elden Ring for now (phase 5 forwards them to MGSV's raw input).

#include "Chord.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace phantom::er
{
	namespace InputBridge
	{
		bool Install();
		[[nodiscard]] bool Installed();

		// Game thread -> hook: what to do with the next calls.
		void SetMode(bool a_maskEr, bool a_neutralForGuest);
		// Elden Ring shows an interaction prompt (re-backlog E6): a fresh press of [Input] prompt_button
		// goes to Elden Ring (until released) and MGSV never sees that button while the prompt is up.
		void SetPrompt(bool a_shown, bool a_deferred = false);
		// a_deferred (a ladder's "Climb" / "Descend", er/Ladder.h): the press is counted but kept from Elden Ring as well as from
		// MGSV until released -- the host steps the puppet aside first and gives Elden Ring the press itself (Inject), as the
		// Tarnished's own: pressed while the puppet held him, Elden Ring never started the descent (live).
		// MGSV's own menu is open (GuestState kGuestInMenu, [Input] guest_menu_owns_pad): MGSV gets the whole pad (Start,
		// View, the prompt button too), Elden Ring a neutral one. Needs SetMode(true, false) as well.
		void SetGuestMenu(bool a_open);
		// The left stick, if it is pushed now, stays out of MGSV's pad until it is let go (back inside the dead zone): at the top of
		// a ladder the player still pushes it up to climb, and Snake taking over must not run on with it (live: he ran off the
		// ladder's landing and fell 19 m down its shaft).
		void HoldLeftStickUntilReleased();
		// Elden Ring alone sees the prompt button pressed for a_ms (the deferred press of SetPrompt; MGSV never does).
		void PressForEr(unsigned a_ms);
		// Elden Ring has the pad for an interaction that has not hidden its HUD yet (er/Interaction.h ButtonsOnly):
		// it gets only kInteractionButtons, neutral sticks and triggers. Used with SetMode(false, true).
		inline constexpr std::uint16_t kInteractionButtons = 0xF000 /* A B X Y */ | 0x000F /* D-pad */ | 0x0030 /* Start, View */;
		void SetButtonsOnly(bool a_on);
		[[nodiscard]] bool GuestMenu();
		// Game thread, every frame: what Start and View do now (er/Chord.h PadGate::Mode).
		void SetPadMode(PadGate::Mode a_mode);
		// Running count of the Tarnished-mode chord (View + Start held together [Input] chord_ms).
		std::uint32_t Chords();
		bool          ChordHolding();  // a chord's (or a dropped) Start/View is still held: it reaches no one until released
		// A menu key of Elden Ring's other than the pad's (keyboard Esc: er/MenuWatch.cpp) counts like a Start press.
		void NoteMenuPress();
		// While Snake plays, View held for [Input] view_hold_ms opens MGSV's iDroid (as Start held) and never reaches Elden Ring; a tap
		// still opens Elden Ring's map (passed on when released). Count of holds handed to MGSV.
		std::uint32_t ViewHoldsToGuest();
		std::uint32_t IdroidCancels();     // View holds sent to MGSV that the chord took back (Start within chord_window_ms)
		std::uint32_t GuestMenuCloses();   // ...and MGSV's menu, opened anyway, closed with a Start tap

		// Hook -> game thread: the last real pad seen (for menu learning / console).
		std::uint16_t LastButtons();
		std::int16_t  LastRightStickX();  // the real pad's (er/BuddyOrders: where the call menu points)
		std::uint32_t MenuPresses();  // running count of Start/Back presses (rising edges)
		std::uint32_t PromptPresses();  // running count of prompt-button presses that went to ER (interactions)
		int           PadIndex();
		std::uint64_t Calls();

		// Dev/test automation (console 'press'): Elden Ring's XInput user 0 reports these buttons and
		// left stick for a_ms on top of whatever is real (a virtual pad if none is connected).
		void Inject(std::uint16_t a_buttons, unsigned a_ms, std::int16_t a_lx = 0, std::int16_t a_ly = 0, std::uint8_t a_lt = 0, std::uint8_t a_rt = 0,
			std::int16_t a_rx = 0, std::int16_t a_ry = 0);
		// FrameBegin: with no controller connected Elden Ring doesn't poll XInput at all; while an injection lasts the bridge
		// then polls itself once a frame, so MGSV gets the injected pad exactly as from a real one (Elden Ring sees nothing).
		void PollIfIdle();
		// Test automation (console `input virtualpad on|off`): user 0 reports a connected neutral gamepad to Elden Ring's
		// capability query and polls, and Elden Ring is told devices changed, so it enumerates and polls it.
		std::string SetVirtualPad(bool a_on);

		// Forward an MGSV rumble to the real controller (kGEvRumble).
		void Rumble(std::uint16_t a_low, std::uint16_t a_high);
	}
}
