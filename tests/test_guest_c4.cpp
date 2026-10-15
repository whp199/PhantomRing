// Unit tests for C4's detonator logic (mgsv/C4Logic.cpp): which C4 a press sets off, and when the guest answers a press
// MGSV's own detonator didn't. Run under wine (target check_guest).
#include "C4Logic.h"

#include <array>
#include <cstdio>

using namespace phantom::guest::c4;

static int g_failures = 0;
#define CHECK(x)                                                    \
	do {                                                            \
		if (!(x)) {                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			++g_failures;                                           \
		}                                                           \
	} while (0)

static void TestIsC4()
{
	CHECK(!IsC4(390));
	CHECK(IsC4(391) && IsC4(393) && IsC4(395));
	CHECK(!IsC4(396));
	CHECK(!IsC4(413));  // claymore
}

static void TestPickOldest()
{
	// Empty, mines only, nothing live.
	CHECK(PickOldest({}, 391) == -1);
	std::array<Candidate, 4> mines{ { { 3, 413, false }, { 0, 391, false }, { 5, 421, false }, { 6, 391, true } } };
	CHECK(PickOldest(mines, 391) == -1);

	// The oldest (lowest id) of the kind in hand, even when an older C4 of another grade lies there.
	std::array<Candidate, 5> all{ { { 9, 391, false }, { 4, 392, false }, { 7, 391, false }, { 2, 413, false }, { 3, 391, true } } };
	CHECK(PickOldest(all, 391) == 2);  // id 7 (id 3 already set off)
	CHECK(PickOldest(all, 392) == 1);  // id 4
	// None of that grade left: the oldest C4 of any grade.
	CHECK(PickOldest(all, 394) == 1);
	// Nothing in hand (0): any C4.
	CHECK(PickOldest(all, 0) == 1);
	// SetOffC4 skips a C4 whose set-off was refused only for that call (marked setOff in its local copy): the next one.
	all[2].setOff = true;
	CHECK(PickOldest(all, 391) == 0);  // id 9
}

static Detonator::Input In(std::uint64_t a_ms, std::uint32_t a_buttons, bool a_armed = true)
{
	return { .ms = a_ms, .buttons = a_buttons, .mask = 0x20, .armed = a_armed };
}

static void TestFallback()
{
	Detonator d(250);
	CHECK(d.Step(In(1000, 0)) == Detonator::Action::kNone);
	CHECK(d.Step(In(1016, 0x20)) == Detonator::Action::kNone);  // the press
	CHECK(d.Pending() && d.Presses() == 1);
	CHECK(d.Step(In(1100, 0x20)) == Detonator::Action::kNone);  // held: still the same press
	CHECK(d.Step(In(1265, 0)) == Detonator::Action::kNone);     // 249 ms
	CHECK(d.Step(In(1266, 0)) == Detonator::Action::kFallback); // 250 ms: MGSV never answered
	CHECK(!d.Pending() && d.Fallbacks() == 1);
	CHECK(d.Step(In(2000, 0)) == Detonator::Action::kNone);     // once
	// Holding the button never makes a second press (and a hold is no fallback: below).
	CHECK(d.Step(In(3000, 0x20)) == Detonator::Action::kNone);
	for (std::uint64_t t = 3016; t < 4000; t += 16) {
		d.Step(In(t, 0x20));
	}
	CHECK(d.Presses() == 2 && d.Fallbacks() == 1);
}

static void TestHold()
{
	// Review: Y held next to a sleeping enemy is MGSV's Fulton -- it never makes a detonate request, and the backup must not
	// take it for an unanswered detonator press.
	Detonator d(250);
	d.Step(In(1000, 0));
	d.Step(In(1016, 0x20));
	for (std::uint64_t t = 1032; t <= 1416; t += 16) {
		CHECK(d.Step(In(t, 0x20)) == Detonator::Action::kNone);  // still down at the grace time: waits for the release
	}
	CHECK(d.Pending());
	CHECK(d.Step(In(1432, 0x20)) == Detonator::Action::kNone);  // 416 ms > kHoldMs: a hold
	CHECK(!d.Pending() && d.Holds() == 1);
	for (std::uint64_t t = 1448; t < 3000; t += 16) {
		CHECK(d.Step(In(t, t < 2500 ? 0x20u : 0u)) == Detonator::Action::kNone);
	}
	CHECK(d.Fallbacks() == 0);

	// A slow tap (down 300 ms, past the grace time): the fallback comes at its release.
	d.Step(In(4000, 0x20));
	CHECK(d.Step(In(4260, 0x20)) == Detonator::Action::kNone && d.Pending());
	CHECK(d.Step(In(4300, 0)) == Detonator::Action::kFallback);
	CHECK(d.Fallbacks() == 1 && d.Holds() == 1);

	// MGSV answering a press that is still held: answered, no hold counted.
	d.Step(In(5000, 0x20));
	d.NoteAnswered(5020);
	for (std::uint64_t t = 5032; t < 6000; t += 16) {
		d.Step(In(t, 0x20));
	}
	CHECK(d.Answered() == 1 && d.Holds() == 1 && d.Fallbacks() == 1);
}

static void TestAnsweredByMgsv()
{
	Detonator d(250);
	d.Step(In(1000, 0));
	d.Step(In(1016, 0x20));
	d.NoteAnswered(1030);  // MGSV's SetOff from DetonateOwn
	CHECK(!d.Pending() && d.Answered() == 1);
	for (std::uint64_t t = 1032; t < 2000; t += 16) {
		CHECK(d.Step(In(t, t < 1200 ? 0x20u : 0u)) == Detonator::Action::kNone);
	}
	CHECK(d.Fallbacks() == 0);

	// MGSV answered earlier in the same frame than the guest saw the press.
	d.NoteAnswered(2000);
	CHECK(d.Step(In(2010, 0x20)) == Detonator::Action::kNone);
	CHECK(!d.Pending() && d.Answered() == 2);
	d.Step(In(2026, 0));
	CHECK(d.Step(In(2400, 0)) == Detonator::Action::kNone && d.Fallbacks() == 0);

	// An old set-off (another press's, a chain) does not answer a new press.
	d.NoteAnswered(3000);
	d.Step(In(3500, 0x20));
	CHECK(d.Pending());
	d.Step(In(3516, 0));
	CHECK(d.Step(In(3750, 0)) == Detonator::Action::kFallback);
	// ...and is used up once it answered one.
	d.NoteAnswered(4000);
	d.Step(In(4010, 0x20));
	d.Step(In(4026, 0));
	d.Step(In(4042, 0x20));  // a second press right after: MGSV's answer was for the first
	CHECK(d.Pending());
}

static void TestNotArmed()
{
	Detonator d(250);
	// No C4 in hand (or riding, a menu, none placed): the button is MGSV's other ACTION (climb, mount, pick up).
	CHECK(d.Step(In(1000, 0x20, false)) == Detonator::Action::kNone);
	CHECK(!d.Pending() && d.Presses() == 0);
	CHECK(d.Step(In(1500, 0, false)) == Detonator::Action::kNone);
	// Armed while already held: no edge, nothing.
	d.Step(In(2000, 0x20, false));
	CHECK(d.Step(In(2016, 0x20, true)) == Detonator::Action::kNone && !d.Pending());
	d.Step(In(2032, 0, true));
	// A press, then the C4 goes away (picked up, a menu) before the grace time: nothing to set off.
	d.Step(In(3000, 0x20, true));
	CHECK(d.Pending());
	CHECK(d.Step(In(3100, 0, false)) == Detonator::Action::kNone && !d.Pending());
	CHECK(d.Step(In(3400, 0, true)) == Detonator::Action::kNone && d.Fallbacks() == 0);
}

static void TestMask()
{
	// A two-button detonator: both held, the last one's press is the edge.
	Detonator d(0);
	auto in = [](std::uint64_t a_ms, std::uint32_t a_b) { return Detonator::Input{ .ms = a_ms, .buttons = a_b, .mask = 0x20 | 0x400, .armed = true }; };
	CHECK(d.Step(in(1000, 0x20)) == Detonator::Action::kNone);
	CHECK(d.Step(in(1016, 0x420)) == Detonator::Action::kNone && d.Pending());  // grace 0: at the release
	CHECK(d.Step(in(1032, 0x400)) == Detonator::Action::kFallback);
	CHECK(d.Step(in(1048, 0x420)) == Detonator::Action::kNone);
	CHECK(d.Step(in(1064, 0)) == Detonator::Action::kFallback);
	// No mask: never.
	Detonator e(0);
	CHECK(e.Step({ .ms = 1, .buttons = 0xFFFFFFFF, .mask = 0, .armed = true }) == Detonator::Action::kNone);
}

int main()
{
	TestIsC4();
	TestPickOldest();
	TestFallback();
	TestAnsweredByMgsv();
	TestHold();
	TestNotArmed();
	TestMask();
	if (g_failures) {
		std::printf("test_guest_c4: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("test_guest_c4: all passed\n");
	return 0;
}
