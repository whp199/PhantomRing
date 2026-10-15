#pragma once
// MGSV's dive (X) as protocol v13 kGuestDiving (roadmap 2.9: the host hops its kinematic puppet over a gap or onto a ledge).
//
// MGSV has no PlayerStatus for the dive (the enum's names in mgsvtpp.exe 1.0.15.4, 0x1421961D0..0x142196830: STAND ... CBOX,
// CBOX_EVADE (the box's own dive) ... VOLGIN_CHASE -- no DIVE), so it is read from what it does: the player's own X while Snake stands
// (his dive, README "How to play"), then within a short window a sign that a dive is playing -- he left STAND, went CRAWL
// (a dive ends prone), left NORMAL_ACTION (an action is playing), or sped up from a walk. Which of these MGSV shows during
// a dive is UNVERIFIED live: the log names the sign that confirmed each dive and how long it took (docs/GUEST.md asks "Is
// a dive NORMAL_ACTION?"). Pure: tests/test_guest_dive.cpp.

#include <cstdint>
#include <string>

namespace phantom::guest::dive
{
	struct Facts
	{
		std::uint64_t nowMs{ 0 };
		std::uint32_t xPresses{ 0 };  // the player's own X presses so far (pad::Presses: rising edges, never our pulses)
		bool          eligible{ false };  // ready, on foot, not in CQC/a fall/a menu/a cutscene/death/the box/carrying
		bool          standing{ false };  // PlayerStatus STAND
		bool          crawl{ false };     // ... CRAWL
		bool          normal{ false };    // ... NORMAL_ACTION
		float         speed{ 0 };         // Snake's horizontal speed (m/s, the odometer's smoothed velocity)
	};

	struct Tunables
	{
		std::uint32_t confirmMs{ 600 };  // a sign of the dive must come this soon after X
		std::uint32_t minMs{ 300 };      // the flag stays up at least this long once confirmed ...
		std::uint32_t maxMs{ 1500 };     // ... and at most this long (a dive lands in ~1 s: Death.cpp kFallMs)
		float         moveSpeed{ 2.0f }; // from below this, reaching it after X is a sign (a dive from a stand or a walk)
		float         restSpeed{ 0.8f }; // below this after minMs the dive is over
	};

	class DiveWatch
	{
	public:
		enum class Event : std::uint8_t
		{
			kNone,
			kArmed,        // X while standing: waiting for a sign
			kConfirmed,    // diving (kGuestDiving up)
			kUnconfirmed,  // no sign within confirmMs: no dive (MGSV refused X, or X was something else)
			kCancelled,    // not eligible any more before a sign (CQC, a fall, a menu ...)
			kEnded,        // the dive is over (kGuestDiving down)
		};

		// Each frame; true while Snake dives.
		bool Update(const Facts& a_f, const Tunables& a_t, Event* a_event = nullptr);
		[[nodiscard]] bool               Diving() const { return phase_ == Phase::kDiving; }
		[[nodiscard]] const std::string& Sign() const { return sign_; }      // what confirmed the last dive
		[[nodiscard]] std::uint64_t      SignMs() const { return signMs_; }  // ... this long after X
		[[nodiscard]] std::uint64_t      Dives() const { return dives_; }
		[[nodiscard]] std::uint64_t      Unconfirmed() const { return unconfirmed_; }
		std::string                      Describe() const;

	private:
		enum class Phase : std::uint8_t
		{
			kIdle,
			kArmed,
			kDiving
		};
		Phase         phase_{ Phase::kIdle };
		bool          primed_{ false };
		std::uint32_t presses_{ 0 };
		std::uint64_t sinceMs_{ 0 };
		float         speedAtX_{ 0 };
		bool          normalAtX_{ false };
		std::string   sign_;
		std::uint64_t signMs_{ 0 };
		std::uint64_t dives_{ 0 };
		std::uint64_t unconfirmed_{ 0 };
	};

	// The guest's own (DiveGuest.cpp, not pure): each player-camera frame (Frame.cpp), from MGSV's PlayerStatus bits (PR.Flags)
	// and Snake's speed; true = kGuestDiving. [Motion] DiveFlag = false: never.
	bool        Observe(bool a_ready, bool a_riding, std::uint32_t a_luaFlags, float a_speed);
	std::string Describe();  // console `dive`
}
