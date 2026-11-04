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
			// (0.7.0) ... unless Elden Ring's climb ended for good (none right after EndTop / EndBottom: LadderEndedForGood -- no
			// frame of none between two of its states, the step off is done) and MGSV has Snake home (kGuestClimbing down): then
			// homeSettleMs is enough. Live 2026-11-01 (0.6.0) Snake walked 0.44-0.52 s after the climb: the 300 ms settle, then
			// MGSV's first step; MGSV had him home after 0.20 s.
			bool          endedForGood{ false };
			bool          guestHome{ false };
			std::uint64_t homeSettleMs{ 50 };
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

	// Elden Ring's climb is over for good: none right after EndTop (6) / EndBottom (7) -- not a frame of none between two of its
	// states. MGSV is told then (kHEvClimb a = 0), not after the settle, so that it has Snake home when the puppet starts (0.6.0:
	// the stick did nothing for 0.2-0.5 s after a ladder while MGSV took him back from its own ladder).
	constexpr bool LadderEndedForGood(int a_prevState, int a_state) { return a_state == -1 && (a_prevState == 6 || a_prevState == 7); }

	// How a ladder state moves the climber: +1 up (StartBottom, Up*, EndTop), -1 down (StartTop, Down*, EndBottom, the coast =
	// the slide down), 0 still on it (Idle*, Attack*, Damage*, unknown), and none (-1 state) = 0 too.
	int LadderMotion(int a_state);

	// Snake climbs the ladder himself (protocol v15 kHEvClimb, [Ladder] snake): Elden Ring still moves the hidden Tarnished up or
	// down it, and MGSV plays Snake's own ladder climb at the climb's pace. This says when to tell the guest: a = 1 when the climb
	// starts and again whenever it turns (up / down / still on it for kStillMs) or its pace changes by more than kPaceChange
	// (never more often than kMinGapMs); a = 0 when it ends. b = 0 up / 1 down (a still climber keeps the last direction); v0[0] = the vertical
	// speed (m/s): the Tarnished's own, measured (eased over kSpeedTauS), or a_defaultSpeed until a measure exists (its first
	// kWarmupMs of moving); exactly 0 = still on the rungs (the guest holds MGSV's climber still), and a moving climb never sends 0:
	// with no speed to tell (a_defaultSpeed <= 0) it sends -1, "unknown" (MGSV's own pace). Pure (tests/test_host_chord.inc
	// TestClimbPace).
	class ClimbPace
	{
	public:
		static constexpr std::uint64_t kMinGapMs = 200;
		static constexpr std::uint64_t kPaceGapMs = 1000;   // a pace change alone waits this long after the last event
		static constexpr std::uint64_t kWarmupMs = 500;
		static constexpr std::uint64_t kStillMs = 300;      // a stop on the rungs counts once it lasted this long (Elden Ring's climb
		                                                    // passes through its idle states between two pushes of the stick)
		static constexpr float         kSpeedTauS = 0.8f;   // Elden Ring climbs rung by rung: 0.76-1.72 m/s over 0.3 s windows (live)
		static constexpr float         kPaceChange = 0.5f;  // share of the speed last sent
		struct Out
		{
			bool  send{ false };   // push a kHEvClimb now
			bool  on{ false };     // a
			bool  down{ false };   // b
			float speed{ 0 };      // v0[0]
			int   motion{ 0 };     // +1 up, -1 down, 0 still (what changed)
		};
		// Each frame while Elden Ring has the Tarnished on a ladder (its state != -1; a single -1 frame between two states may
		// come too): the state, his feet's height, now.
		Out Update(std::uint64_t a_nowMs, int a_state, float a_y, float a_defaultSpeed);
		// The hand-over ended (off the ladder, a load, a death): a = 0 if a = 1 went out.
		Out End();
		// Elden Ring moved its world origin (er/WorldShift.h; live: 32 m at Fort Haight's tower mid-climb): the last height moves with
		// it, so that frame's speed isn't a 32 m step.
		void Shift(float a_dy)
		{
			if (haveY_) {
				lastY_ += a_dy;
			}
		}
		[[nodiscard]] bool  Started() const { return started_; }
		[[nodiscard]] float Speed() const { return speed_; }  // measured |vy| (m/s), eased
		[[nodiscard]] std::uint32_t Sent() const { return sent_; }
		void Reset() { *this = ClimbPace(); }

	private:
		bool          started_{ false };
		bool          down_{ false };
		int           motionSent_{ 0 };
		float         speedSent_{ 0 };
		float         moveSent_{ 0 };   // the last moving speed sent (a climb resumed goes on at it until a measure is warm)
		std::uint64_t sentMs_{ 0 };
		bool          haveY_{ false };
		float         lastY_{ 0 };
		std::uint64_t lastMs_{ 0 };
		float         speed_{ 0 };
		std::uint64_t movingSince_{ 0 };
		std::uint64_t stillSince_{ 0 };
		bool          pending_{ false };  // a turn within kMinGapMs of the last event: sent once the gap is over
		std::uint32_t sent_{ 0 };
	};
}
