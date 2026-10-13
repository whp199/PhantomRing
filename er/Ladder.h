#pragma once

// Ladders (roadmap 1.7). Snake can't climb: Elden Ring climbs a ladder for the Tarnished, from its "Climb" / "Descend"
// prompt (ActionButtonParam 5000 / 5010, [Input] ladder_prompts) until he is off it at the top or the bottom. A ladder
// needs the left stick for the whole climb, and the puppet's kinematic SetPosition (or the root-motion hook) fights
// Elden Ring's ladder animation, which moves him itself. So a ladder is an automatic, temporary Tarnished mode: like the
// mist's step-aside (er/Host.cpp), the puppet stops (Puppet::Hold::kLadder: the Tarnished shows, Elden Ring has the whole
// pad and its own camera), and it lasts while Elden Ring's ladder state says he is on one (CSPlayerLadderModule +0x18,
// fromsoftware-rs LadderState: -1 none, 0 StartBottom, 1 StartTop, 2-5 up/down, 6 EndTop, 7 EndBottom, 8/9 idle, ...).
// Snake takes over where the Tarnished got off (the puppet's start anchors him there).
//
// Pure rules (tests/test_host_chord.inc TestLadder): the caller reads the ladder state and the prompt press, and turns the
// events into the puppet hold, the corner message and the HP carried over.

#include <cstdint>
#include <string_view>

namespace phantom::er
{
	class LadderHandOver
	{
	public:
		enum class State : std::uint8_t
		{
			kIdle,      // Snake plays (or nothing to do)
			kWaiting,   // Snake's press on a ladder prompt reached Elden Ring: the puppet steps aside until the climb starts
			kClimbing,  // Elden Ring's ladder state says the Tarnished is on a ladder
			kSettling,  // ... and it just went back to none: Snake waits a moment (the step off), then takes over
		};
		enum class Event : std::uint8_t
		{
			kNone,
			kStarted,   // hold the puppet: a ladder press (kWaiting) or a ladder state seen while Snake played (kClimbing)
			kClimbing,  // the press led to a climb
			kEnded,     // off the ladder: Snake takes over
			kNoClimb,   // a press that never led to a climb within waitMs (Elden Ring refused it): Snake takes over again
			kAborted,   // a load or a death on the way: let go (the puppet's own rules restart it)
		};
		struct Input
		{
			std::uint64_t nowMs{ 0 };
			bool          pressed{ false };   // this frame: Snake's prompt press on a ladder prompt went to Elden Ring
			bool          onLadder{ false };  // Elden Ring's ladder state != none
			bool          snake{ false };     // Snake plays right now (the puppet runs)
			bool          inWorld{ true };    // in the world, alive (not loading, not dead or dying)
			std::uint64_t waitMs{ 1500 };     // a press waits this long for the climb to start
			std::uint64_t settleMs{ 300 };    // off the ladder this long (the step off it) before Snake takes over
		};

		Event Update(const Input& a_in);
		// Live (Fort Haight's tower, 2026-10-13): Snake's Y on "Descend" at the top reached Elden Ring while the puppet still held
		// the Tarnished, and Elden Ring never started the climb (alone, the Tarnished starts it 0.5 s after his Y). So the press
		// is kept from Elden Ring (InputBridge::SetPrompt deferred) and given to it a_delayMs later, once the puppet has stepped
		// aside: true once, while waiting for a climb that a press started.
		bool PressDue(std::uint64_t a_nowMs, std::uint64_t a_delayMs);
		[[nodiscard]] bool          Holding() const { return state_ != State::kIdle; }
		[[nodiscard]] State         Current() const { return state_; }
		[[nodiscard]] std::uint64_t Since() const { return since_; }  // when the hand-over started
		void                        Reset() { *this = LadderHandOver(); }

	private:
		State         state_{ State::kIdle };
		std::uint64_t since_{ 0 };
		std::uint64_t offSince_{ 0 };
		bool          pressed_{ false };  // PressDue fired
	};

	std::string_view Name(LadderHandOver::State a_state);
	std::string_view Name(LadderHandOver::Event a_event);
	// fromsoftware-rs LadderState names (common_define.hks), for the log; "?" for others.
	std::string_view LadderStateName(int a_state);
}
