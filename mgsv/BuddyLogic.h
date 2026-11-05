#pragma once
// The iDroid's DISMISS <buddy>, told apart from a buddy MGSV lost by itself -- the pure half (Pump.cpp AdoptBuddy / ReviveBuddy; the
// leave order is seen by the buddies' message-handler detours in Quiet.cpp, Dog.cpp and Follow.cpp; tests in tests/test_guest_logic.cpp).
// No game, Windows or mapping access here.
//
// MGSV's active buddy type going NONE is all the Lua side sees of a dismissal, and it comes only once the buddy has gone: 8.6-9.3 s after
// the iDroid closed for D-Walker (DISMISS, fultoned out), D-Horse and D-Dog (swapped for another buddy) (live 2026-10-14 and 2026-11-04);
// Quiet walks off cloaked and is NONE 12.2-12.8 s after it (three DISMISS QUIETs from her sniping post 70 m above Snake, the player's
// session 2026-11-04). 0.9.0 took a NONE within 15 s of the last MGSV menu as the dismissal (2 s of it to confirm), 13 s at NONE's start:
// at 15:11:02 it had just run out (NONE 14.85 s after the close at its decision) and the guest called her back.
//
// The precise signal (offline RE of mgsvtpp.exe 1.0.15.4, live in the logs): MGSV's buddy system sends the buddy that is out the message
// `leave` (StrCode64 0x6626CE35F43D) the moment the iDroid sends it away -- DISMISS, CHANGE EQUIPMENT or another buddy. Its only sender is
// the buddy system's request state machine (0x140A439C0, state 2: `mov rax,0x6626CE35F43D` at 0x140A43CAA, sent to the outgoing buddy's
// GameObject type from 0x140A46130: HORSE 0x0D, DOG 0x13, QUIET 0x12, WALKER_GEAR 0x2C); all four buddies' message handlers take it
// (`cmp` at 0x140CE38DD Quiet 0x140CE3330, 0x140C56B8C D-Dog 0x140C56B10, 0x141706D6E D-Walker 0x141706CF0, 0x140E4D9D7 0x140E4D960).
// Seen live with the iDroid open, 17-535 ms before the guest saw it close: Quiet's handler at the DISMISS QUIET of 14:48:57 (2026-11-04),
// D-Dog's at three CHANGE EQUIPMENTs and at D-Dog -> D-Horse (integ8 runs 1-3, integ9, 2026-11-04). The guest hooks the handlers of
// Quiet, D-Dog and (with auto-follow on) D-Walker; D-Horse's isn't hooked.
//
// So: a leave order to the buddy that is out while an MGSV menu is open is remembered (one at a time) until that buddy is gone; a NONE
// (2 s of it) that follows within kLeaveMaxMs is the player's dismissal, however long the buddy takes to go. A re-equip (the same buddy
// gone for a while, then back) or another buddy coming out forgets it. Without a leave (D-Horse, a hook missing) the old rule stays as a
// fallback, per buddy and measured from the menu's close: MenuWindowMs. A NONE with neither is a buddy MGSV lost: it is called back.

#include <cstdint>
#include <string_view>

namespace phantom::guest::buddy::plan
{
	// MGSV's `leave` order to a buddy (StrCode64; the same id in Quiet.cpp's kMsgLeave).
	inline constexpr std::uint64_t kMsgLeave = 0x6626CE35F43D;

	// BuddyType (State.h kBuddyType*).
	inline constexpr int kNone = 0, kHorse = 1, kDog = 2, kQuiet = 3, kWalker = 4;

	// [Buddy] Type's words and BuddyType.
	inline int TypeOfKind(std::string_view a_kind)
	{
		return a_kind == "walker" ? kWalker : a_kind == "horse" ? kHorse : a_kind == "dog" ? kDog : a_kind == "quiet" ? kQuiet : kNone;
	}
	inline const char* KindOfType(int a_type)
	{
		switch (a_type) {
		case kWalker:
			return "walker";
		case kHorse:
			return "horse";
		case kDog:
			return "dog";
		case kQuiet:
			return "quiet";
		default:
			return "none";
		}
	}

	// A leave order is the menu's when a menu was open as it came (the detour reads state::MenuOpen()) or one was seen open at most this
	// long before it (the Lua thread samples the menu every 250 ms; live the leave came 17-535 ms before the close was seen).
	inline constexpr std::uint64_t kMenuSlackMs = 2000;
	inline bool LeaveFromMenu(bool a_menuOpenAtLeave, std::uint64_t a_leaveMs, std::uint64_t a_menuSeenAt)
	{
		return a_menuOpenAtLeave || (a_menuSeenAt && a_leaveMs <= a_menuSeenAt + kMenuSlackMs);
	}

	// The buddy must be gone (NONE) within this long of its leave order: ~9x Quiet's slowest exit seen (12.8 s). The order is kept
	// kJudgeSlackMs longer, so a NONE that began in time is still judged by it (2 s to confirm, then again every 2 s).
	inline constexpr std::uint64_t kLeaveMaxMs = 120000;
	inline constexpr std::uint64_t kJudgeSlackMs = 10000;
	// The leave's buddy counts as gone (a re-equip's first half) once its object hasn't answered, or NONE was out, for this long.
	inline constexpr std::uint64_t kGoneMs = 1000;

	// The fallback without a leave order: NONE began at most this long after the last MGSV menu (or while one was open). D-Walker /
	// D-Horse gone (fultoned) 8.6-9.3 s after the iDroid closed: 20 s. Quiet NONE 12.2-12.8 s after it from her far post, D-Dog gone
	// 8.8 s after it (on foot, it can be far off): 30 s (Quiet's slowest x2.3). 0.9.0's 15 s at the decision was 13 s at NONE's start
	// for every buddy.
	inline std::uint64_t MenuWindowMs(int a_type) { return a_type == kQuiet || a_type == kDog ? 30000 : 20000; }

	enum class Verdict : std::uint8_t
	{
		kLost,             // no dismissal seen: MGSV lost it -- called back
		kDismissedLeave,   // MGSV's leave order from the iDroid, then NONE
		kDismissedWindow,  // NONE soon after an MGSV menu (the fallback)
	};

	inline const char* VerdictName(Verdict a_verdict)
	{
		switch (a_verdict) {
		case Verdict::kDismissedLeave:
			return "leave order";
		case Verdict::kDismissedWindow:
			return "menu window";
		default:
			return "lost";
		}
	}

	// Why OnActive forgot a leave order.
	enum class Forgot : std::uint8_t
	{
		kNo,       // still remembered (or none was)
		kBack,     // the same buddy gone for a while, then back: a re-equip
		kAnother,  // another buddy came out (the switch's own adoption follows it)
		kExpired,  // kLeaveMaxMs (+ kJudgeSlackMs) passed without a NONE
	};

	class DismissWatch
	{
	public:
		// MGSV ordered the buddy of type a_type to leave at a_ms (any thread saw it; the Lua thread hands it over). Only an order given
		// with an MGSV menu open (LeaveFromMenu) is the player's; another replaces the one remembered. True = remembered.
		bool OnLeave(int a_type, std::uint64_t a_ms, bool a_fromMenu)
		{
			if (!a_fromMenu || a_type <= kNone) {
				return false;
			}
			m_type = a_type;
			m_ms = a_ms;
			m_goneSince = 0;
			m_sawGone = false;
			return true;
		}

		// Every step: what MGSV has out (a_active: BuddyType, -1 unknown; a_present: its object id answers). Forgets the order when
		// another buddy came out (the switch's own adoption follows it) or the same one came back after being gone (CHANGE EQUIPMENT),
		// and once kLeaveMaxMs (+ kJudgeSlackMs) passed.
		Forgot OnActive(int a_active, bool a_present, std::uint64_t a_ms)
		{
			if (m_type <= kNone) {
				return Forgot::kNo;
			}
			if (a_ms > m_ms + kLeaveMaxMs + kJudgeSlackMs) {
				Reset();
				return Forgot::kExpired;
			}
			if (a_active > kNone && a_active != m_type) {
				Reset();
				return Forgot::kAnother;
			}
			if (a_active < kNone) {
				return Forgot::kNo;
			}
			if (a_active == kNone || !a_present) {
				if (!m_goneSince) {
					m_goneSince = a_ms;
				}
				m_sawGone |= a_ms - m_goneSince >= kGoneMs;
				return Forgot::kNo;
			}
			m_goneSince = 0;
			if (m_sawGone) {
				Reset();  // back again: a re-equip, not a dismissal
				return Forgot::kBack;
			}
			return Forgot::kNo;
		}

		// The buddy of type a_type is leaving by the player's order (ReviveBuddy leaves it alone meanwhile).
		[[nodiscard]] bool Leaving(int a_type, std::uint64_t a_ms) const { return a_type > kNone && m_type == a_type && a_ms <= m_ms + kLeaveMaxMs; }
		[[nodiscard]] int           Type() const { return m_type; }
		[[nodiscard]] std::uint64_t LeaveMs() const { return m_ms; }

		// MGSV's active type has been NONE since a_noneSince (2 s of it confirmed) while a_type was the guest's buddy: the player's
		// dismissal, or lost? a_menuSeenAt = the last time an MGSV menu was seen open (0 = never).
		[[nodiscard]] Verdict Judge(int a_type, std::uint64_t a_noneSince, std::uint64_t a_menuSeenAt) const
		{
			if (a_type <= kNone || !a_noneSince) {
				return Verdict::kLost;
			}
			if (m_type == a_type && m_ms <= a_noneSince && a_noneSince <= m_ms + kLeaveMaxMs) {
				return Verdict::kDismissedLeave;
			}
			if (a_menuSeenAt && a_noneSince <= a_menuSeenAt + MenuWindowMs(a_type)) {
				return Verdict::kDismissedWindow;
			}
			return Verdict::kLost;
		}

		void Reset()
		{
			m_type = -1;
			m_ms = 0;
			m_goneSince = 0;
			m_sawGone = false;
		}

	private:
		int           m_type{ -1 };  // the buddy ordered to leave (BuddyType), -1 none
		std::uint64_t m_ms{ 0 };
		std::uint64_t m_goneSince{ 0 };
		bool          m_sawGone{ false };
	};
}
