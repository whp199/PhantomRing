#pragma once
// Auto-follow's player hold, the pure half (Follow.cpp's command detour and Update; tests in tests/test_guest_follow.cpp).
//
// The player's own buddy commands (MGSV's call menu, the iDroid) pause our whistles until the player whistles again, so a mode or a
// "wait" the player chose for D-Walker (or D-Horse's "Stay back") sticks. Commands that belong to another buddy say nothing about the
// walker's or the horse's following: D-Dog's 8-16 (live 2026-11-04: a D-Walker deployed after the dog's orders stood 36 m away until
// the player whistled) and Quiet's 1-7 (live 2026-11-04, the Quiet study: after her Fire / Take aim / Cover me / talk orders a D-Walker
// deployed next stood 25.4 m away, "paused by the player's command", until the whistle). And a hold belongs to the buddy it was given
// to: a change of the active buddy type (Quiet dismissed, D-Walker deployed) clears it (docs/research/quiet.md §3.3, M0).

namespace phantom::guest::follow::plan
{
	// MGSV's buddy command ranges (docs/GUEST.md 18.3, the buddy command function 0x140A4E0B0): 1-7 Quiet, 8-16 D-Dog, 17-19 D-Horse,
	// 20-26 D-Walker (20 its whistle, 21-26 its modes). As BuddyType: 3, 2, 1, 4 (0 for anything else).
	inline constexpr int kWhistle = 20, kNormalMode = 21, kHorseWhistle = 17, kHorseCome = 19;
	inline int CommandBuddy(int a_command)
	{
		return a_command >= 20 && a_command <= 26 ? 4 : a_command >= 17 && a_command <= 19 ? 1 : a_command >= 8 && a_command <= 16 ? 2 : a_command >= 1 && a_command <= 7 ? 3 : 0;
	}

	// The hold after the player's command a_command (a_held = before it). D-Dog's and Quiet's commands leave it as it is; the whistles,
	// Normal Mode and D-Horse's 19 resume following; any other command (a walker mode, D-Horse's "Stay back") holds it -- only while
	// [Buddy] FollowRespectCommands (a_respect) is on.
	inline bool HoldAfterCommand(bool a_held, int a_command, bool a_respect)
	{
		const int buddy = CommandBuddy(a_command);
		if (buddy == 2 || buddy == 3) {
			return a_held;
		}
		return a_command != kWhistle && a_command != kNormalMode && a_command != kHorseWhistle && a_command != kHorseCome && a_respect;
	}

	// A hold is the buddy's it was given to: Update feeds MGSV's active buddy type (BuddyType, 0 NONE, -1 unknown) every time it reads it.
	// A change from one buddy to another (NONE in between or not: a dismissal, then a new deploy) clears the hold; NONE alone (a
	// dismissal, a reload's moment without a buddy) and the same buddy coming back keep it.
	class HoldOwner
	{
	public:
		// True = the hold must be cleared now (the active buddy is another one than the one last seen).
		bool OnBuddyType(int a_type)
		{
			if (a_type <= 0) {
				return false;
			}
			const bool changed = m_last > 0 && a_type != m_last;
			m_last = a_type;
			return changed;
		}
		[[nodiscard]] int Last() const { return m_last; }

	private:
		int m_last{ -1 };
	};
}
