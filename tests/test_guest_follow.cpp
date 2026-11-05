// Offline tests of auto-follow's player hold (mgsv/FollowLogic.h; docs/research/quiet.md §3.3, M0): which buddy a command belongs
// to, D-Dog's 8-16 and Quiet's 1-7 never setting or clearing the hold, the whistles resuming it, and a change of the active buddy
// clearing it.
#include "FollowLogic.h"

#include <cstdio>
#include <initializer_list>

using namespace phantom::guest::follow::plan;

static int g_failures = 0;
#define CHECK(x)                                                    \
	do {                                                            \
		if (!(x)) {                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			++g_failures;                                           \
		}                                                           \
	} while (0)

static void TestRanges()
{
	for (int c = 1; c <= 7; ++c) {
		CHECK(CommandBuddy(c) == 3);
	}
	for (int c = 8; c <= 16; ++c) {
		CHECK(CommandBuddy(c) == 2);
	}
	for (int c = 17; c <= 19; ++c) {
		CHECK(CommandBuddy(c) == 1);
	}
	for (int c = 20; c <= 26; ++c) {
		CHECK(CommandBuddy(c) == 4);
	}
	CHECK(CommandBuddy(0) == 0 && CommandBuddy(27) == 0 && CommandBuddy(-1) == 0);
}

static void TestCommands()
{
	// Quiet's 1-7 (Fire, Fire, Take aim, Cover me, Cease cover, Shoot this, talk) and D-Dog's 8-16: the hold as it was, either way.
	for (int c = 1; c <= 16; ++c) {
		CHECK(!HoldAfterCommand(false, c, true));
		CHECK(HoldAfterCommand(true, c, true));
		CHECK(!HoldAfterCommand(false, c, false));
	}
	// The live sequence of the Quiet study (07:14-07:34): talk 7, Take aim 3, Fire 1 x3, Cover me 4, Cease cover 5 -- no hold.
	bool held = false;
	for (const int c : { 7, 3, 1, 1, 1, 4, 5 }) {
		held = HoldAfterCommand(held, c, true);
	}
	CHECK(!held);
	// The whistles, Normal Mode and D-Horse's 19 resume; D-Walker's modes and D-Horse's "Stay back" (18) hold.
	CHECK(!HoldAfterCommand(true, kWhistle, true) && !HoldAfterCommand(true, kNormalMode, true) && !HoldAfterCommand(true, kHorseWhistle, true) &&
	      !HoldAfterCommand(true, kHorseCome, true));
	for (int c = 22; c <= 26; ++c) {
		CHECK(HoldAfterCommand(false, c, true));
		CHECK(!HoldAfterCommand(false, c, false));  // [Buddy] FollowRespectCommands off: never held
	}
	CHECK(HoldAfterCommand(false, 18, true));
	// A walker mode, then Quiet's orders: still held (they say nothing about the walker); then the whistle: free.
	held = HoldAfterCommand(false, 25, true);
	held = HoldAfterCommand(held, 1, true);
	held = HoldAfterCommand(held, 7, true);
	CHECK(held);
	CHECK(!HoldAfterCommand(held, kWhistle, true));
}

static void TestBuddyChange()
{
	HoldOwner o;
	CHECK(!o.OnBuddyType(-1) && !o.OnBuddyType(0));  // nothing known yet
	CHECK(!o.OnBuddyType(4) && o.Last() == 4);         // D-Walker first: nothing to clear
	CHECK(!o.OnBuddyType(4));
	CHECK(!o.OnBuddyType(0) && !o.OnBuddyType(-1));   // dismissed / a reload's moment: the hold stays the walker's
	CHECK(!o.OnBuddyType(4));                          // the same walker back: kept
	CHECK(o.OnBuddyType(3) && o.Last() == 3);          // Quiet deployed: cleared
	CHECK(!o.OnBuddyType(3));
	CHECK(!o.OnBuddyType(0));                          // Quiet dismissed from the iDroid...
	CHECK(o.OnBuddyType(4));                           // ...D-Walker deployed: cleared (the live case of the Quiet study)
	CHECK(o.OnBuddyType(2) && o.OnBuddyType(1) && o.OnBuddyType(4));
}

int main()
{
	TestRanges();
	TestCommands();
	TestBuddyChange();
	if (g_failures) {
		std::printf("test_guest_follow: %d FAILED\n", g_failures);
		return 1;
	}
	std::printf("test_guest_follow: all passed\n");
	return 0;
}
