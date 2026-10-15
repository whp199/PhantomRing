#pragma once
// The pure part of C4's detonator (C4.cpp, docs/GUEST.md 18.8a): which C4 a press sets off, and when the guest has to set
// one off itself because MGSV's own detonator didn't answer the press. No game access: unit-tested (tests/test_guest_c4.cpp).
//
// MGSV's detonator is the ACTION button (PlayerPad bit 5, Y by default) with C4 in hand: one press sets off the player's
// oldest C4 of the kind in hand (DetonateOwn, Addresses.h). Live it never went off in the sandbox (GUEST.md 18.8), and the
// static RE can't tell which of its conditions failed there, so the guest watches the same press: answered by MGSV (its
// SetOff seen) -> nothing to do; refused by MGSV (PlayerDetonate saw the request and set nothing off) -> the guest sets the
// oldest off at once; never seen by MGSV within a grace time -> the guest's backup may set it off then ([Combat] C4Backup,
// off by default: MGSV's Y is also Fulton, supply boxes and D-Walker, which never make a request). Only a tap counts for
// the backup: a held Y is Fulton's (or carrying), never the detonator's.

#include <cstdint>
#include <span>

namespace phantom::guest::c4
{
	// EQP_SWP_C4 and its grades _G01.._G04 (tools/re/data/tppequip_ids_1.0.15.4.txt).
	[[nodiscard]] constexpr bool IsC4(std::uint16_t a_equip) { return a_equip >= 391 && a_equip <= 395; }

	struct Candidate
	{
		std::uint32_t id{ 0 };      // kGEvPlaced id: they count up, so the lowest is the oldest
		std::uint16_t equip{ 0 };
		bool          setOff{ false };  // already on its way (kHEvDetonate, an earlier press)
	};

	// The C4 one press sets off, as MGSV's DetonateOwn picks it: the oldest live one of the kind in hand; any C4 when none of
	// that kind is left (the guest's own press may come with another grade in hand). -1 = none.
	[[nodiscard]] int PickOldest(std::span<const Candidate> a_all, std::uint16_t a_inHand);

	// One player's detonator presses. Single-threaded: C4.cpp holds its lock around every call.
	class Detonator
	{
	public:
		struct Input
		{
			std::uint64_t ms{ 0 };
			std::uint32_t buttons{ 0 };  // PlayerVars.scannedButtonsDirect (PlayerPad bits)
			std::uint32_t mask{ 0 };     // the detonator: all of these down (rising edge of the last one)
			bool          armed{ false };  // C4 in hand, on foot, no menu, the sandbox ready, a live C4 of ours exists
		};
		enum class Action
		{
			kNone,
			kFallback,  // MGSV didn't answer this tap within the grace time: the guest's backup may set one off
		};

		explicit Detonator(std::uint32_t a_graceMs = 250) :
			m_graceMs(a_graceMs) {}

		Action Step(const Input& a_in);
		// A set-off that answers a press: MGSV's own (its SetOff from DetonateOwn) or the guest's at once (PlayerDetonate
		// refused). Before the press is seen (MGSV's player update can run before ours in the same frame) it still answers it.
		void NoteAnswered(std::uint64_t a_ms);

		[[nodiscard]] bool          Pending() const { return m_pending; }
		[[nodiscard]] std::uint32_t Presses() const { return m_presses; }
		[[nodiscard]] std::uint32_t Answered() const { return m_answered; }
		[[nodiscard]] std::uint32_t Fallbacks() const { return m_fallbacks; }
		[[nodiscard]] std::uint32_t Holds() const { return m_holds; }

		// A set-off this recent (ms) answers a press seen now.
		static constexpr std::uint64_t kRecentMs = 150;
		// A press held down longer than this (ms) is a hold -- MGSV's Fulton of a sleeping enemy, carrying one -- not the
		// detonator's tap: it never becomes a fallback. A tap still down at the grace time waits for its release.
		static constexpr std::uint64_t kHoldMs = 400;

	private:
		std::uint32_t m_graceMs;
		bool          m_down{ false };
		bool          m_pending{ false };
		std::uint64_t m_pressMs{ 0 };
		std::uint64_t m_lastAnswerMs{ 0 };
		bool          m_haveAnswer{ false };
		std::uint32_t m_presses{ 0 }, m_answered{ 0 }, m_fallbacks{ 0 }, m_holds{ 0 };
	};
}
