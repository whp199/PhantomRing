// Unit tests for MGSV's dive as kGuestDiving (mgsv/Dive.h, roadmap 2.9). Run under wine (target check_guest).
#include "Dive.h"

#include <cstdio>

using namespace phantom::guest::dive;

static int g_failures = 0;
#define CHECK(x)                                                    \
	do {                                                            \
		if (!(x)) {                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			++g_failures;                                           \
		}                                                           \
	} while (0)

namespace
{
	// Snake standing in NORMAL_ACTION, eligible, still.
	Facts Stand(std::uint64_t a_ms, std::uint32_t a_presses, float a_speed = 0.0f)
	{
		Facts f;
		f.nowMs = a_ms;
		f.xPresses = a_presses;
		f.eligible = true;
		f.standing = true;
		f.normal = true;
		f.speed = a_speed;
		return f;
	}
}

// Each sign confirms a dive; the flag stays up through it and drops when he comes to rest (or after maxMs).
static void TestSigns()
{
	const Tunables t;
	struct Case
	{
		const char* sign;
		void (*apply)(Facts&);
	};
	const Case cases[] = {
		{ "left STAND", [](Facts& f) { f.standing = false; } },
		{ "CRAWL", [](Facts& f) { f.crawl = true; } },
		{ "left NORMAL_ACTION", [](Facts& f) { f.normal = false; } },
		{ "sped up", [](Facts& f) { f.speed = 3.0f; } },
	};
	for (const auto& c : cases) {
		DiveWatch           w;
		DiveWatch::Event    ev{};
		CHECK(!w.Update(Stand(0, 7), t, &ev) && ev == DiveWatch::Event::kNone);  // the first frame only primes the counter
		CHECK(!w.Update(Stand(16, 8), t, &ev) && ev == DiveWatch::Event::kArmed);
		CHECK(!w.Update(Stand(33, 8), t, &ev) && ev == DiveWatch::Event::kNone);
		Facts f = Stand(100, 8, 0.0f);
		c.apply(f);
		CHECK(w.Update(f, t, &ev) && ev == DiveWatch::Event::kConfirmed && w.Sign() == c.sign && w.SignMs() == 84 && w.Dives() == 1);
		// Mid-dive, moving: still diving (it lands prone, sliding).
		Facts mid = f;
		mid.nowMs = 500;
		mid.speed = 5.0f;
		mid.standing = false;
		mid.crawl = true;
		CHECK(w.Update(mid, t, &ev) && ev == DiveWatch::Event::kNone);
		// Prone and at rest after minMs: over.
		mid.nowMs = 900;
		mid.speed = 0.2f;
		CHECK(!w.Update(mid, t, &ev) && ev == DiveWatch::Event::kEnded);
	}
}

static void TestNoDive()
{
	const Tunables t;
	DiveWatch      w;
	DiveWatch::Event ev{};
	w.Update(Stand(0, 0), t);
	// X while crouched (not standing): not his dive.
	Facts crouch = Stand(16, 1);
	crouch.standing = false;
	CHECK(!w.Update(crouch, t, &ev) && ev == DiveWatch::Event::kNone);
	// X while standing but nothing changes within confirmMs: no dive.
	CHECK(!w.Update(Stand(100, 2), t, &ev) && ev == DiveWatch::Event::kArmed);
	CHECK(!w.Update(Stand(600, 2), t, &ev) && ev == DiveWatch::Event::kNone);
	CHECK(!w.Update(Stand(701, 2), t, &ev) && ev == DiveWatch::Event::kUnconfirmed && w.Unconfirmed() == 1 && w.Dives() == 0);
	// Already running (4.7 m/s) at X: speed alone is no sign (he was fast already), the stance must change.
	CHECK(!w.Update(Stand(1000, 3, 4.7f), t, &ev) && ev == DiveWatch::Event::kArmed);
	CHECK(!w.Update(Stand(1100, 3, 5.5f), t, &ev) && ev == DiveWatch::Event::kNone);
	Facts dive = Stand(1150, 3, 5.5f);
	dive.normal = false;
	CHECK(w.Update(dive, t, &ev) && ev == DiveWatch::Event::kConfirmed && w.Sign() == "left NORMAL_ACTION");
	// The flag ends by maxMs at the latest, even still moving.
	dive.nowMs = 1150 + t.maxMs;
	CHECK(!w.Update(dive, t, &ev) && ev == DiveWatch::Event::kEnded);
	// Not eligible (CQC, a fall, a menu, riding ...): X arms nothing, and an armed X is cancelled.
	Facts busy = Stand(3000, 4);
	busy.eligible = false;
	CHECK(!w.Update(busy, t, &ev) && ev == DiveWatch::Event::kNone);
	CHECK(!w.Update(Stand(3100, 5), t, &ev) && ev == DiveWatch::Event::kArmed);
	busy.nowMs = 3150;
	busy.xPresses = 5;
	busy.standing = false;
	CHECK(!w.Update(busy, t, &ev) && ev == DiveWatch::Event::kCancelled);
	// A press during a dive starts nothing new (one dive, one flag).
	CHECK(!w.Update(Stand(4000, 6), t, &ev) && ev == DiveWatch::Event::kArmed);
	Facts d2 = Stand(4050, 6);
	d2.standing = false;
	CHECK(w.Update(d2, t, &ev) && ev == DiveWatch::Event::kConfirmed && w.Dives() == 2);
	d2.nowMs = 4100;
	d2.xPresses = 7;
	d2.speed = 5.0f;
	CHECK(w.Update(d2, t, &ev) && ev == DiveWatch::Event::kNone && w.Dives() == 2);
}

// Each sign is a change since X (review of 2.9): NORMAL_ACTION already off at X (a reload, a weapon switch) is no sign; the
// stance changing afterwards still is.
static void TestNormalOffAtX()
{
	const Tunables t;
	DiveWatch      w;
	DiveWatch::Event ev{};
	const auto busy = [](std::uint64_t a_ms, std::uint32_t a_presses) {
		Facts f = Stand(a_ms, a_presses);
		f.normal = false;
		return f;
	};
	w.Update(busy(0, 0), t);
	CHECK(!w.Update(busy(16, 1), t, &ev) && ev == DiveWatch::Event::kArmed);
	CHECK(!w.Update(busy(33, 1), t, &ev) && ev == DiveWatch::Event::kNone);
	CHECK(!w.Update(busy(600, 1), t, &ev) && ev == DiveWatch::Event::kNone);
	CHECK(!w.Update(busy(617, 1), t, &ev) && ev == DiveWatch::Event::kUnconfirmed && w.Dives() == 0 && w.Unconfirmed() == 1);
	CHECK(!w.Update(busy(1000, 2), t, &ev) && ev == DiveWatch::Event::kArmed);
	Facts f = busy(1050, 2);
	f.standing = false;
	CHECK(w.Update(f, t, &ev) && ev == DiveWatch::Event::kConfirmed && w.Sign() == "left STAND");
}

int main()
{
	TestSigns();
	TestNoDive();
	TestNormalOffAtX();
	if (g_failures) {
		std::printf("test_guest_dive: %d failures\n", g_failures);
		return 1;
	}
	std::printf("test_guest_dive: all passed\n");
	return 0;
}
