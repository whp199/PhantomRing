#pragma once
// When does Snake grab, hold, throw, choke, interrogate or let go? (ROADMAP 2.2 tier 1, protocol v13 kGEvCqc)
//
// MGSV runs its holds and throws only on its own soldiers: in the sandbox there is no one to grab, so an RT pressed next to
// an Elden Ring enemy is only ever a punch (kGEvMelee, attack 80). Tier 1 reads the player's intent from MGSV's own pad bits
// (PlayerVars.scannedButtonsDirect: what MGSV itself saw this frame, so an Elden Ring menu's neutral pad never counts) and
// MGSV's controls for CQC on a soldier, and lets the host decide whether an Elden Ring enemy is there to take it:
//  - RT (unaimed) with the left stick pushed: a throw (MGSV: CQC while moving toward the soldier throws him);
//  - RT held: a grab (MGSV: hold RT next to a soldier to restrain him), and while it stays held:
//      the interrogate button (MGSV's CALL/INTERROGATE pad bit) -> interrogate, once per hold;
//      the choke button (none by default), or RT still held chokeMs after the grab -> choke, once per hold;
//    RT let go (or Snake can't do CQC any more: a menu, a fall, a vehicle...) -> release;
//  - MGSV's own hold (PlayerStatus CQC_HOLD/CONTINUOUS or the CQC state's action callback: a real grab on an ActorProxy
//    soldier) starts the grab at once rather than after holdMs.
// Pure (no game access): tests/test_guest_cqc.cpp. Every threshold is an ini value ([Cqc], docs/GUEST.md 21); the mapping of
// MGSV's controls is UNVERIFIED live (the guest logs every button change during a hold to settle it).

#include "phantomring_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace phantom::guest::cqc
{
	// PlayerPad bits (docs/research/mgsv-runtime-re.md 5.5, VERIFIED against the exe's PlayerPad enum).
	inline constexpr std::uint32_t kPadHold = 1u << 3;    // LT: aim
	inline constexpr std::uint32_t kPadFire = 1u << 4;    // RT
	inline constexpr std::uint32_t kPadCall = 1u << 9;    // CALL / INTERROGATE (the same pad bit)
	inline constexpr std::uint32_t kPadCqc = 1u << 15;    // CQC

	struct Rules
	{
		std::uint32_t cqcMask{ kPadFire | kPadCqc };  // any of these = the CQC button
		std::uint32_t aimMask{ kPadHold };             // ...unless one of these is held (RT aimed = a shot)
		std::uint32_t interrogateMask{ kPadCall };
		std::uint32_t chokeMask{ 0 };                  // 0: only the long hold chokes
		std::uint32_t holdMs{ 350 };                   // RT held this long = a grab
		std::uint32_t chokeMs{ 2500 };                 // ...and this long after the grab = a choke (0 = never)
		std::uint32_t maxHoldMs{ 10000 };              // a hold this long ends by itself (a stuck button)
		std::uint32_t cooldownMs{ 400 };               // after a throw or a release, before the next press counts
		float         throwStick{ 0.6f };              // left stick deflection (0..1) that makes a press a throw
	};

	struct Input
	{
		bool          allowed{ false };  // on foot, playing, standing or crouched: MGSV would do CQC now
		std::uint32_t buttons{ 0 };      // PlayerVars.scannedButtonsDirect
		float         stick{ 0 };        // left stick deflection 0..1
		bool          mgsvHold{ false }; // MGSV itself holds someone (CQC_HOLD / CQC_CONTINUOUS / the action callback)
		std::uint64_t nowMs{ 0 };
	};

	// What this frame started (at most two: e.g. a choke and the release that ends the hold in the same frame).
	struct Actions
	{
		std::array<proto::CqcAction, 2> list{};
		std::size_t                     count{ 0 };
		void                            Add(proto::CqcAction a_action)
		{
			if (count < list.size()) {
				list[count++] = a_action;
			}
		}
		[[nodiscard]] bool Has(proto::CqcAction a_action) const
		{
			for (std::size_t i = 0; i < count; ++i) {
				if (list[i] == a_action) {
					return true;
				}
			}
			return false;
		}
	};

	class Tracker
	{
	public:
		enum class State : std::uint8_t
		{
			kIdle,
			kPressed,   // RT down, not yet a grab (a strike if it is let go now)
			kHolding,   // a grab is on
			kCooldown,  // after a throw/release: waits for RT up and cooldownMs
		};

		Actions Update(const Input& a_in, const Rules& a_rules);
		[[nodiscard]] State         Current() const { return state_; }
		[[nodiscard]] bool          Choked() const { return choked_; }
		[[nodiscard]] bool          Interrogated() const { return interrogated_; }
		[[nodiscard]] std::uint64_t HoldStart() const { return holdAt_; }
		void                        Reset() { *this = Tracker(); }

	private:
		State         state_{ State::kIdle };
		std::uint32_t lastButtons_{ 0 };
		bool          primed_{ false };     // lastButtons_ holds a real frame (no edge from before the first one)
		std::uint64_t pressAt_{ 0 };
		std::uint64_t holdAt_{ 0 };
		std::uint64_t until_{ 0 };
		bool          choked_{ false };
		bool          interrogated_{ false };
	};

	const char* Name(proto::CqcAction a_action);
	const char* Name(Tracker::State a_state);

	// Left stick deflection 0..1 from XInput thumbs, with XInput's own dead zone (7849) taken out.
	float StickDeflection(std::int16_t a_x, std::int16_t a_y);
}
