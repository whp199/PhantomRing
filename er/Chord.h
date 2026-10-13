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
// fires once. Let go early, nothing happens. Either way both stay out of every pad until they are released, whatever the
// mode is by then: Elden Ring's menu never opens or closes from the chord. A press still undecided when the mode leaves
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
			int startWaitMs{ 150 };     // a press is passed to Elden Ring this long after it went down (room for the chord)
			int chordMs{ 1000 };        // Start + View held together this long = the chord; 0 = no chord (and no wait)
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
		};

		static constexpr std::uint64_t kTapMs = 120;  // a synthesized press: a few of Elden Ring's polls

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
	};
}
