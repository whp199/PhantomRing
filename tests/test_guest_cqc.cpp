// Unit tests for the guest's CQC intent tracker (mgsv/CqcLogic.cpp, protocol v13 kGEvCqc): grab, hold, throw, choke,
// interrogate, release from MGSV's pad bits. Run under wine (target check_guest).
#include "CqcLogic.h"

#include <cstdio>
#include <string_view>

using namespace phantom::guest::cqc;
namespace proto = phantom::proto;

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
	struct Driver
	{
		Tracker       t;
		Rules         r;
		std::uint64_t now{ 1000 };
		bool          allowed{ true };
		bool          mgsvHold{ false };

		Actions Step(std::uint32_t a_buttons, std::uint64_t a_dtMs = 16, float a_stick = 0.0f)
		{
			now += a_dtMs;
			return t.Update({ .allowed = allowed, .buttons = a_buttons, .stick = a_stick, .mgsvHold = mgsvHold, .nowMs = now }, r);
		}
		// Holds the buttons for a_ms in 16 ms frames; every action seen.
		Actions Hold(std::uint32_t a_buttons, std::uint64_t a_ms, float a_stick = 0.0f)
		{
			Actions all;
			for (std::uint64_t t0 = 0; t0 < a_ms; t0 += 16) {
				const auto a = Step(a_buttons, 16, a_stick);
				for (std::size_t i = 0; i < a.count; ++i) {
					all.Add(a.list[i]);
				}
			}
			return all;
		}
	};
}

static void TestStrikeIsNoGrab()
{
	Driver d;
	CHECK(d.Step(0).count == 0);
	CHECK(d.Step(kPadFire).count == 0);
	CHECK(d.t.Current() == Tracker::State::kPressed);
	CHECK(d.Hold(kPadFire, 200).count == 0);  // a tap: under holdMs
	CHECK(d.Step(0).count == 0);
	CHECK(d.t.Current() == Tracker::State::kIdle);
}

static void TestGrabChokeRelease()
{
	Driver d;
	d.Step(0);
	auto a = d.Hold(kPadFire, 400);
	CHECK(a.count == 1 && a.list[0] == proto::kCqcGrab);
	CHECK(d.t.Current() == Tracker::State::kHolding);
	// Held on: the choke comes chokeMs after the grab, once.
	a = d.Hold(kPadFire, 2600);
	CHECK(a.count == 1 && a.list[0] == proto::kCqcChoke);
	CHECK(d.t.Choked());
	CHECK(d.Hold(kPadFire, 1000).count == 0);
	a = d.Step(0);
	CHECK(a.count == 1 && a.list[0] == proto::kCqcRelease);
	CHECK(d.t.Current() == Tracker::State::kCooldown);
	// A press during the cooldown is nothing; after it (RT up), a new grab works.
	CHECK(d.Step(kPadFire).count == 0);
	d.Step(0, 500);
	d.Step(0);
	CHECK(d.t.Current() == Tracker::State::kIdle);
	a = d.Hold(kPadCqc, 400);  // the CQC bit counts as the button too
	CHECK(a.Has(proto::kCqcGrab));
}

static void TestInterrogateOnce()
{
	Driver d;
	d.Step(0);
	d.Hold(kPadFire, 400);
	auto a = d.Step(kPadFire | kPadCall);
	CHECK(a.count == 1 && a.list[0] == proto::kCqcInterrogate);
	CHECK(d.Step(kPadFire | kPadCall).count == 0);  // held: no repeat
	d.Step(kPadFire);
	CHECK(d.Step(kPadFire | kPadCall).count == 0);  // once per hold
	CHECK(d.t.Interrogated());
	// Interrogate without RT held is not in a hold: nothing.
	Driver e;
	e.Step(0);
	CHECK(e.Step(kPadCall).count == 0);
}

static void TestThrow()
{
	Driver d;
	d.Step(0);
	auto a = d.Step(kPadFire, 16, 0.9f);  // RT with the stick pushed: a throw at the press
	CHECK(a.count == 1 && a.list[0] == proto::kCqcThrow);
	CHECK(d.Hold(kPadFire, 3000, 0.9f).count == 0);  // held on: no grab, no choke
	// A small stick deflection is no throw.
	Driver e;
	e.Step(0);
	CHECK(e.Step(kPadFire, 16, 0.3f).count == 0);
	CHECK(e.t.Current() == Tracker::State::kPressed);
}

static void TestAimAndNotAllowed()
{
	Driver d;
	d.Step(0);
	CHECK(d.Hold(kPadFire | kPadHold, 3000).count == 0);  // aimed RT: shots
	Driver e;
	e.allowed = false;
	e.Step(0);
	CHECK(e.Hold(kPadFire, 3000).count == 0);
	// A hold ends with a release as soon as CQC isn't possible any more (a menu, a fall).
	Driver f;
	f.Step(0);
	f.Hold(kPadFire, 400);
	f.allowed = false;
	auto a = f.Step(kPadFire);
	CHECK(a.count == 1 && a.list[0] == proto::kCqcRelease);
	// RT held from before the first frame is no press.
	Driver g;
	CHECK(g.Hold(kPadFire, 1000).count == 0);
	CHECK(g.t.Current() == Tracker::State::kIdle);
}

static void TestMgsvHoldAndLimits()
{
	// MGSV's own hold (an ActorProxy soldier) starts the grab at once and keeps it while MGSV holds.
	Driver d;
	d.Step(0);
	d.Step(kPadFire);
	d.mgsvHold = true;
	auto a = d.Step(kPadFire);
	CHECK(a.count == 1 && a.list[0] == proto::kCqcGrab);
	CHECK(d.Step(0).count == 0);  // RT up but MGSV still holds
	d.mgsvHold = false;
	a = d.Step(0);
	CHECK(a.count == 1 && a.list[0] == proto::kCqcRelease);
	// A stuck button ends at maxHoldMs; chokeMs 0 never chokes; the choke button chokes at once.
	Driver e;
	e.r.chokeMs = 0;
	e.r.chokeMask = kPadCall;
	e.r.interrogateMask = 0;
	e.Step(0);
	e.Hold(kPadFire, 400);
	a = e.Step(kPadFire | kPadCall);
	CHECK(a.count == 1 && a.list[0] == proto::kCqcChoke);
	a = e.Hold(kPadFire | kPadCall, 11000);
	CHECK(a.count == 1 && a.list[0] == proto::kCqcRelease);
}

static void TestStick()
{
	CHECK(StickDeflection(0, 0) == 0.0f);
	CHECK(StickDeflection(7000, 0) == 0.0f);
	CHECK(StickDeflection(32767, 0) == 1.0f);
	CHECK(StickDeflection(-32768, -32768) == 1.0f);
	const float half = StickDeflection(0, 20000);
	CHECK(half > 0.45f && half < 0.55f);
	CHECK(std::string_view(Name(proto::kCqcInterrogate)) == "interrogate");
}

int main()
{
	TestStrikeIsNoGrab();
	TestGrabChokeRelease();
	TestInterrogateOnce();
	TestThrow();
	TestAimAndNotAllowed();
	TestMgsvHoldAndLimits();
	TestStick();
	std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
	return g_failures ? 1 : 0;
}
