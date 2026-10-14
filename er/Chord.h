#pragma once

// Start and View on the pad -- Elden Ring's menu and map, MGSV's iDroid -- and the "Tarnished mode" chord (roadmap 1.1:
// View + Start held together ~1 s), decided on every poll of the published pad (er/InputBridge.cpp, Elden Ring's input
// thread). Pure: tests/test_host_chord.inc.
//
// Modes (the game thread picks one each frame, InputBridge::SetPadMode):
//  - kSnake: Snake plays and no menu owns the pad. View tapped = Elden Ring's map (passed on at the release as a 120 ms
//    press), View held [Input] view_hold_ms = MGSV's iDroid (MGSV gets Start held guest_idroid_start_ms, Elden Ring
//    nothing); Start = Elden Ring's menu, passed on [Input] start_wait_ms after it went down, or at its release if that
//    comes first (a tap).
//  - kTarnished: the player plays the Tarnished (Tarnished mode) and no Elden Ring menu, cutscene or hidden HUD is up:
//    Start and View both work like Start above (no iDroid: MGSV is parked).
//  - kGuestMenu: MGSV's own menu has the pad: Elden Ring gets neither, nothing is counted.
//  - kOff: an Elden Ring menu, a load, a death, the grace menu, Snake not playing: Elden Ring gets both as they are.
// In kSnake and kTarnished, Start and View down together -- the second going down before the first reached anyone -- is
// the chord: neither reaches Elden Ring or MGSV (an iDroid hold is cancelled), and held [Input] chord_ms together it
// fires once. Forgiving (player report 2026-10-14: "the chord failed when View went down a bit before Start -- the View
// hold opened MGSV's iDroid first"): a Start within [Input] chord_window_ms after View still makes the chord when View's
// iDroid hold already went to MGSV (and MGSV opened its menu: it does so on the Start at once) -- the hold's Start is cut,
// the real Start stays out of MGSV's pad, and MGSV's menu, if it opened, gets a Start tap (kCloseGapMs after the cut, once
// more kCloseRetryMs later if it is still open) that closes it; the chord fires as usual.
// Start first, View within [Input] start_wait_ms (400) is the chord the other way round. Let go early, nothing happens.
// Either way both stay out of every pad until they are released, whatever the mode is by then: Elden Ring's menu never
// opens or closes from the chord. A press still undecided when the mode leaves
// kSnake/kTarnished (a menu came up) is dropped the same way; one that went down in kOff passes as it is until released.

#include <cstdint>

namespace phantom::er
{
	class PadGate
	{
	public:
		enum class Mode : std::uint8_t
		{
			kOff,
			kSnake,
			kTarnished,
			kGuestMenu,
		};

		struct Params
		{
			int viewHoldMs{ 400 };      // kSnake: View held this long = MGSV's iDroid; 0 = View works like Start
			int idroidStartMs{ 1200 };  // ...sent to MGSV as Start held this long
			int startWaitMs{ 400 };     // a press is passed to Elden Ring this long after it went down (room for the chord)
			int chordMs{ 1000 };        // Start + View held together this long = the chord; 0 = no chord (and no wait)
			int chordWindowMs{ 700 };   // Start this soon after View is the chord even after View's iDroid hold went to MGSV
		};

		struct Input
		{
			std::uint64_t nowMs{ 0 };
			bool          start{ false };
			bool          view{ false };
			Mode          mode{ Mode::kOff };
		};

		struct Output
		{
			bool erStart{ false };      // Elden Ring's pad carries Start this poll
			bool erView{ false };       // ... and View (Back)
			bool guestStart{ false };   // kSnake: MGSV's pad carries the synthesized Start of an iDroid hold
			int  menuPresses{ 0 };      // presses that reached Elden Ring this poll (MenuWatch's latch)
			bool viewToGuest{ false };  // a View hold went to MGSV this poll
			bool chord{ false };        // the chord fired this poll
			bool idroidCancelled{ false };  // a View hold already sent to MGSV became part of the chord: its Start was cut
			bool guestMenuClosed{ false };  // ...and MGSV opened its menu anyway: it gets a Start tap to close it (this poll)
			bool guestMaskStart{ false };   // MGSV's pad must not carry the real Start / View (held for the chord)
			bool guestMaskView{ false };
		};

		static constexpr std::uint64_t kTapMs = 120;  // a synthesized press: a few of Elden Ring's polls
		static constexpr std::uint64_t kCloseWatchMs = 2500;  // after an iDroid cancel: MGSV's menu open meanwhile is closed
		static constexpr std::uint64_t kCloseGapMs = 100;     // ...with a Start tap this long after the hold's Start was cut
		static constexpr std::uint64_t kCloseRetryMs = 700;   // ...and once more this much later if it is still open

		Output Update(const Input& a_in, const Params& a_params);

		// A button chord-consumed or dropped, still held (not passed to anyone): tests and the console.
		[[nodiscard]] bool Holding() const { return start_.held() || view_.held(); }

	private:
		struct Button
		{
			bool          down{ false };
			std::uint64_t downAt{ 0 };
			bool          tracked{ false };   // went down while armed: its fate is decided here
			bool          raw{ false };       // went down in kOff: Elden Ring sees it as it is
			bool          passed{ false };    // reached Elden Ring as held (after the wait)
			bool          consumed{ false };  // part of a chord, or dropped: reaches no one until released
			bool          chord{ false };     // part of the chord now forming or fired (both went down together)
			bool          toGuest{ false };   // View: became MGSV's iDroid
			std::uint64_t tapUntil{ 0 };      // a synthesized press reaches Elden Ring until then

			[[nodiscard]] bool held() const { return down && consumed; }
			[[nodiscard]] bool undecided() const { return down && tracked && !passed && !consumed && !toGuest; }
		};

		Button        start_, view_;
		bool          fired_{ false };
		std::uint64_t chordSince_{ 0 };
		std::uint64_t guestStartUntil_{ 0 };
		std::uint64_t cancelAt_{ 0 };         // when an iDroid hold was cut for the chord (0 = none)
		int           closeTaps_{ 0 };         // Start taps sent to close MGSV's menu since
		std::uint64_t lastCloseAt_{ 0 };
		std::uint64_t guestCloseUntil_{ 0 };   // the Start tap that closes it
	};
}
