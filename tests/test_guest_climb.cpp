// Unit tests for Snake's ladder climbs and climb-ups / vaults (mgsv/ClimbLogic.h, protocol v15). Run under wine (check_guest).
#include "ClimbLogic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace phantom::guest::climb::plan;
using phantom::guest::logic::Vec3;

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
	// mafr_lddr001_0005, as measured live (docs/GUEST.md 22.1).
	const LadderSite     kSite{ { 916.35f, -11.52f, 1072.30f }, 60.0f };
	const LadderGeometry kGeo{};
	const Vec3           kHome{ 820.6f, -11.95f, 1179.1f };

	bool Near(float a, float b, float e = 0.02f) { return std::fabs(a - b) <= e; }

	// A tiny MGSV: where Snake is and what he does, moved by the placements, the action button, the stick and the direct-motion
	// request the plan hands out -- as measured live: no placement reaches him on the ladder, Y mounts him only standing at a
	// stand point facing it, the ladder ends in a climb-over (top) or a step-off (foot), a direct motion drops him off it.
	struct Sim
	{
		Vec3          root{ kHome };
		bool          normal{ true };
		bool          onLadder{ false };
		std::uint64_t now{ 1000 };
		std::uint64_t busyUntil{ 0 };  // a landing / climb-over: no NORMAL_ACTION until then
		int           actions{ 0 }, placements{ 0 }, ignored{ 0 }, overs{ 0 }, knockOffs{ 0 }, landingsCancelled{ 0 };
		float         yaw{ 0 };
		float         maxY{ -100.0f }, minY{ 100.0f };
		bool          falling{ false };
		bool          rungs{ false };  // MGSV's ladder record is readable (ladder_rung::Read): the plan may move him along it
		int           shifts{ 0 };
		bool          knockOffWorks{ true };  // false: the direct-motion request never takes him off MGSV's ladder (the review's failure path)
		bool          placementsWork{ true }; // false: the placement home lands nowhere (as if MGSV held him where he is)

		void Apply(const Step& a_s)
		{
			const Vec3 line = kSite.origin + Facing(kSite) * kGeo.lineOut;
			if (a_s.knockOff && onLadder && !knockOffWorks) {
				++knockOffs;  // taken, and nothing happens: he still hangs on it
			} else if (a_s.knockOff && onLadder) {
				++knockOffs;
				onLadder = false;
				falling = true;
				normal = false;
			} else if (a_s.knockOff) {
				busyUntil = 0;  // a landing cancelled
				++landingsCancelled;
			}
			if (a_s.place) {
				++placements;
				if (onLadder || !placementsWork) {
					++ignored;  // MGSV's ladder holds him
				} else {
					root = *a_s.place;
					if (a_s.face) {
						yaw = a_s.placeYaw;  // MGSV's Warp turns him
					}
					if (falling) {
						falling = false;
						busyUntil = now + 1000;  // the landing after the drop
					}
				}
			}
			if (a_s.pressAction && normal) {
				++actions;
				const Vec3 bottom = BottomStand(kSite, kGeo), top = TopStand(kSite, kGeo);
				if ((root - bottom).LengthXZ() < 0.3f && std::fabs(root.y - bottom.y) < 0.3f && YawError(yaw, BottomFacing(kSite)) < 0.4f) {
					onLadder = true;
					root = { line.x, kSite.origin.y + kGeo.hangLow, line.z };
				} else if ((root - top).LengthXZ() < 0.3f && std::fabs(root.y - top.y) < 0.3f && YawError(yaw, TopFacing(kSite)) < 0.4f) {
					onLadder = true;
					root = { line.x, kSite.origin.y + kGeo.hangHigh, line.z };
				}
			}
			if (a_s.rungShift && onLadder) {
				++shifts;
				root.y += static_cast<float>(a_s.rungShift) * 0.5f;  // MGSV's ladder plays on from the new rung pair
			}
			if (onLadder && a_s.stickY != 0) {
				root.y += (a_s.stickY > 0 ? 1.28f : -1.22f) / 60.0f;
				if (root.y > kSite.origin.y + 6.8f) {  // over the top, onto the roof
					onLadder = false;
					root = TopStand(kSite, kGeo);
					busyUntil = now + 1000;
					++overs;
				} else if (root.y < kSite.origin.y + 0.4f) {  // off the foot
					onLadder = false;
					root = BottomStand(kSite, kGeo);
					busyUntil = now + 750;
					++overs;
				}
			}
			if (falling) {
				root.y -= 0.1f;
			}
			normal = !onLadder && !falling && now >= busyUntil;
			if (onLadder) {
				maxY = std::max(maxY, root.y);
				minY = std::min(minY, root.y);
			}
		}

		Facts F() const
		{
			Facts f;
			f.nowMs = now;
			f.root = root;
			f.normalAction = normal;
			f.standing = true;
			f.yaw = yaw;
			f.ready = true;
			if (rungs && onLadder) {
				// The bottom mount's rung pair is 5 (root hangLow above the foot), 0.5 m a pair; the airport's ladders top out at 16.
				f.rung = 5 + static_cast<int>(std::floor((root.y - (kSite.origin.y + kGeo.hangLow)) / 0.5f + 1e-3f));
				f.rungTarget = f.rung;
				f.rungTop = 16;
			}
			return f;
		}
	};
}

static void TestGeometry()
{
	// The stand points measured at 0005: (917.2, 1072.8) facing 240 at the foot, (916.2, 1072.2) on the roof facing 60.
	const Vec3 b = BottomStand(kSite, kGeo), t = TopStand(kSite, kGeo);
	CHECK(Near(b.x, 917.22f, 0.05f) && Near(b.z, 1072.80f, 0.05f) && Near(b.y, -11.52f));
	CHECK(Near(t.x, 916.18f, 0.05f) && Near(t.z, 1072.20f, 0.05f) && Near(t.y, -3.60f, 0.05f));
	CHECK(Near(BottomFacing(kSite), 240.0f * 3.14159265f / 180.0f, 1e-3f) && Near(TopFacing(kSite), 60.0f * 3.14159265f / 180.0f, 1e-3f));
	// Hanging on 0005 his root was (916.91, 1072.63): on the line; at the foot's stand point 0.35 m off it.
	CHECK(LineDistance(kSite, kGeo, { 916.91f, -8.0f, 1072.63f }) < 0.05f);
	CHECK(Near(LineDistance(kSite, kGeo, b), 0.35f, 0.02f));
	CHECK(Near(YawError(0.1f, 6.2f), 0.183f, 0.01f) && Near(YawError(3.0f, -3.0f), 0.283f, 0.01f));
}

static void TestPacer()
{
	Pacer p;
	p.Start(-1.0f);
	CHECK(p.Push(1.0 / 60.0, 0.0f));  // unknown speed: MGSV's own pace
	p.Start(NAN);
	CHECK(p.Push(1.0 / 60.0, 0.0f));
	// Exactly 0: Elden Ring's climber is still on the rungs (er/Ladder.cpp ClimbPace sends 0 then) -- MGSV holds still too.
	p.Start(0.0f);
	for (int i = 0; i < 60; ++i) {
		CHECK(!p.Push(1.0 / 60.0, 0.0f));
	}
	CHECK(p.Owed() == 0.0f);
	// Elden Ring at 0.6 m/s, MGSV at 1.28: the stick is held about half the time and the climb keeps up.
	float mgsv = 0.0f, er = 0.0f;
	int   pushed = 0;
	p.Start(0.6f);
	float climbedLast = 0.0f;
	for (int i = 0; i < 600; ++i) {
		const bool push = p.Push(1.0 / 60.0, climbedLast);
		climbedLast = push ? 1.28f / 60.0f : 0.0f;
		mgsv += climbedLast;
		er += 0.6f / 60.0f;
		pushed += push;
	}
	CHECK(std::fabs(mgsv - er) < 0.2f);
	CHECK(pushed > 220 && pushed < 340);
	// Elden Ring faster than MGSV: always pushed, the debt capped.
	p.Start(3.0f);
	for (int i = 0; i < 600; ++i) {
		CHECK(p.Push(1.0 / 60.0, 1.28f / 60.0f));
	}
	CHECK(p.Owed() <= Pacer::kMaxOwed + 1e-4f);
}

// Up a long Elden Ring ladder: staged, faced, mounted with Y, climbed; at MGSV's top he climbs over and is mounted again from
// the foot; a = 0 drops him off (a direct-motion request) and he is placed home on his feet.
static void TestLadderUp()
{
	Ladder l;
	Sim    sim;
	sim.yaw = 1.0f;  // facing elsewhere: the stand placement turns him
	l.Start(kSite, kGeo, false, -1.0f, kHome, 1.0f, sim.now);
	CHECK(l.Active());
	bool sawActive = false, flagDown = false;
	for (int frame = 0; frame < 60 * 30 && l.Active(); ++frame) {
		sim.now += 16;
		if (frame == 60 * 20) {
			l.Stop(sim.now);
		}
		const auto s = l.Update(sim.F(), 1.0 / 60.0);
		sawActive |= s.active;
		sim.Apply(s);
		flagDown = !s.active;
	}
	CHECK(sawActive && flagDown && !l.Active());
	CHECK(l.Loops() >= 2 && sim.overs == l.Loops());  // ~20 s: 5.5 m climbs, each ended by MGSV, each mounted again
	CHECK(l.Climbed() > 12.0f);
	CHECK(sim.ignored == 0);                            // nothing was placed while he hung on the ladder
	CHECK(sim.knockOffs == 1 && sim.landingsCancelled == 1);  // a = 0: dropped off once, home, the landing cancelled
	CHECK(Near(sim.root.x, kHome.x) && Near(sim.root.z, kHome.z) && Near(sim.yaw, 1.0f));  // home, facing as before
}

// Down: mounted from the roof; a = 0 right after the start (before the mount) is a clean way back too.
static void TestLadderDownAndEarlyStop()
{
	{
		Ladder l;
		Sim    sim;
		l.Start(kSite, kGeo, true, -1.0f, kHome, 1.0f, sim.now);
		for (int i = 0; i < 60 * 14 && l.Active(); ++i) {
			sim.now += 16;
			if (i == 60 * 12) {
				l.Stop(sim.now);
			}
			sim.Apply(l.Update(sim.F(), 1.0 / 60.0));
		}
		CHECK(!l.Active() && l.Loops() >= 1 && sim.ignored == 0 && sim.minY >= kSite.origin.y + 0.4f - 0.05f);
		CHECK(Near(sim.root.x, kHome.x) && sim.normal);
	}
	{
		Ladder l;
		Sim    sim;
		l.Start(kSite, kGeo, false, -1.0f, kHome, 1.0f, sim.now);
		sim.now += 16;
		sim.Apply(l.Update(sim.F(), 1.0 / 60.0));  // the stand placement
		l.Stop(sim.now);
		for (int i = 0; i < 300 && l.Active(); ++i) {
			sim.now += 16;
			sim.Apply(l.Update(sim.F(), 1.0 / 60.0));
		}
		CHECK(!l.Active() && sim.actions == 0 && Near(sim.root.x, kHome.x));
	}
}

// The rule: up, near the top (two pairs below it) 3 m down; down, near the bottom mount's pair 3 m up; nothing on short or
// unknown ladders.
static void TestRecycleShift()
{
	CHECK(RecycleShift(false, 13, 13, 16) == 0 && RecycleShift(false, 13, 14, 16) == -kRecycleSpan && RecycleShift(false, 14, 14, 16) == -kRecycleSpan);
	CHECK(RecycleShift(true, 8, 8, 16) == 0 && RecycleShift(true, 8, 7, 16) == kRecycleSpan && RecycleShift(true, 7, 7, 16) == kRecycleSpan);
	CHECK(RecycleShift(false, 14, 14, -1) == 0 && RecycleShift(false, -1, 14, 16) == 0 && RecycleShift(true, 7, -1, 16) == 0);
	CHECK(RecycleShift(false, 10, 10, 12) == 0 && RecycleShift(true, 7, 7, 12) == 0);  // a 4 m ladder: no room to move along
	// After a shift neither threshold holds: up lands on 8 (> 7), down on 13 (< 14) -- no ping-pong.
	CHECK(RecycleShift(true, 14 - kRecycleSpan, 14 - kRecycleSpan, 16) == 0 && RecycleShift(false, 7 + kRecycleSpan, 7 + kRecycleSpan, 16) == 0);
}

// Seamless (MGSV's ladder record readable): 20 s up and 14 s down a long Elden Ring ladder without one climb-over or step-off;
// he is moved along MGSV's ladder instead, never placed while on it.
static void TestLadderSeamless()
{
	for (const bool down : { false, true }) {
		Ladder l;
		Sim    sim;
		sim.rungs = true;
		l.Start(kSite, kGeo, down, -1.0f, kHome, 1.0f, sim.now);
		for (int frame = 0; frame < 60 * 24 && l.Active(); ++frame) {
			sim.now += 16;
			if (frame == 60 * 21) {
				l.Stop(sim.now);
			}
			sim.Apply(l.Update(sim.F(), 1.0 / 60.0));
		}
		CHECK(!l.Active() && l.Loops() == 0 && sim.overs == 0 && sim.ignored == 0);
		CHECK(l.Recycles() >= 4 && sim.shifts == l.Recycles());
		CHECK(l.Climbed() > 20.0f);  // MGSV climbed all the way (the shifts not counted)
		CHECK(sim.maxY < kSite.origin.y + 6.8f && sim.minY > kSite.origin.y + 0.4f);
		CHECK(Near(sim.root.x, kHome.x) && sim.knockOffs == 1);
	}
	// A refused shift (the record moved under it): no more this climb, MGSV's ends again.
	Ladder l;
	Sim    sim;
	sim.rungs = true;
	l.Start(kSite, kGeo, false, -1.0f, kHome, 1.0f, sim.now);
	bool failed = false;
	for (int frame = 0; frame < 60 * 20 && l.Active(); ++frame) {
		sim.now += 16;
		auto s = l.Update(sim.F(), 1.0 / 60.0);
		if (s.rungShift && !failed) {
			failed = true;
			l.ShiftFailed();
			s.rungShift = 0;
		}
		CHECK(!(failed && s.rungShift));
		sim.Apply(s);
	}
	CHECK(failed && l.Loops() >= 1 && sim.overs >= 1);
}

// A ladder that never takes him: placed, faced and Y three times, then home; never any stick.
static void TestLadderRefused()
{
	Ladder l;
	Sim    sim;
	l.Start(kSite, kGeo, false, -1.0f, kHome, 1.0f, sim.now);
	int presses = 0, stick = 0;
	for (int i = 0; i < 60 * 20 && l.Active(); ++i) {
		sim.now += 16;
		auto s = l.Update(sim.F(), 1.0 / 60.0);
		presses += s.pressAction;
		stick += s.stickY != 0;
		s.pressAction = false;  // MGSV ignores it
		sim.Apply(s);
	}
	CHECK(!l.Active() && presses == 3 && stick == 0 && Near(sim.root.x, kHome.x));
}

// Still on Elden Ring's rungs (a = 1 with speed 0): mounted, then no stick; a = 1 at a speed again climbs.
static void TestLadderStill()
{
	Ladder l;
	Sim    sim;
	l.Start(kSite, kGeo, false, 0.0f, kHome, 1.0f, sim.now);
	int stillFrames = 0, stickWhileStill = 0;
	float hungAt = 0.0f;
	for (int frame = 0; frame < 60 * 12 && l.Active(); ++frame) {
		sim.now += 16;
		if (frame == 60 * 6) {
			hungAt = sim.root.y;
			l.Change(false, 1.2f);  // Elden Ring climbs on
		}
		if (frame == 60 * 10) {
			l.Stop(sim.now);
		}
		const auto s = l.Update(sim.F(), 1.0 / 60.0);
		if (frame > 60 * 2 && frame < 60 * 6 && l.Current() == Phase::kMove) {
			++stillFrames;
			stickWhileStill += s.stickY != 0;
		}
		sim.Apply(s);
	}
	CHECK(stillFrames > 200 && stickWhileStill == 0);
	CHECK(Near(hungAt, kSite.origin.y + kGeo.hangLow, 0.05f));  // he hung where the mount put him
	CHECK(!l.Active() && l.Climbed() > 2.0f && Near(sim.root.x, kHome.x));
}

// Ending(): a = 0 came (or he is on his way off / home) -- a new a = 1 is a new climb then (Climb.cpp keeps it for after).
static void TestLadderEnding()
{
	Ladder l;
	Sim    sim;
	l.Start(kSite, kGeo, false, -1.0f, kHome, 1.0f, sim.now);
	CHECK(!l.Ending());
	for (int i = 0; i < 120; ++i) {
		sim.now += 16;
		sim.Apply(l.Update(sim.F(), 1.0 / 60.0));
	}
	CHECK(l.Current() == Phase::kMove && !l.Ending());
	l.Stop(sim.now);
	CHECK(l.Ending());
	bool sawReturnEnding = false;
	for (int i = 0; i < 300 && l.Active(); ++i) {
		sim.now += 16;
		sim.Apply(l.Update(sim.F(), 1.0 / 60.0));
		sawReturnEnding |= l.Current() == Phase::kReturn && l.Ending();
	}
	CHECK(sawReturnEnding && !l.Active() && !l.Ending());
}

// The way off MGSV's ladder never takes (the direct-motion request is ignored, review 2026-10-31): the plan never says "back on the
// sandbox" with him still on the ladder -- it tries again (off it, then home), and after kReturnTries asks for MGSV's Warp home.
static void TestLadderNeverHome()
{
	Ladder l;
	Sim    sim;
	sim.knockOffWorks = false;
	l.Start(kSite, kGeo, false, -1.0f, kHome, 1.0f, sim.now);
	bool warped = false, falseHome = false;
	int  knockOffsAsked = 0;
	for (int frame = 0; frame < 60 * 40 && l.Active(); ++frame) {
		sim.now += 16;
		if (frame == 60 * 3) {
			l.Stop(sim.now);
		}
		const auto s = l.Update(sim.F(), 1.0 / 60.0);
		warped |= s.warpHome;
		knockOffsAsked += s.knockOff;
		falseHome |= !s.active && !s.warpHome && (sim.root - kHome).LengthXZ() > kHomeRadius;
		sim.Apply(s);
	}
	CHECK(!l.Active() && warped && !falseHome);
	CHECK(knockOffsAsked >= kReturnTries);  // off the ladder asked again on each try
}

// The native placement home refused (PlacementFailed on the way home): tried again, kReturnTries in all, then the Warp; a refused
// first placement (he is still home) just ends the move.
static void TestPlacementRefused()
{
	{
		Ladder l;
		Sim    sim;
		l.Start(kSite, kGeo, false, -1.0f, kHome, 1.0f, sim.now);
		bool warped = false;
		int  fails = 0;
		for (int frame = 0; frame < 60 * 20 && l.Active(); ++frame) {
			sim.now += 16;
			if (frame == 60 * 3) {
				l.Stop(sim.now);
			}
			auto s = l.Update(sim.F(), 1.0 / 60.0);
			warped |= s.warpHome;
			if (s.place && l.Current() == Phase::kReturn) {
				l.PlacementFailed(sim.now);  // the native call refused it
				++fails;
				s.place.reset();
			}
			sim.Apply(s);
		}
		CHECK(!l.Active() && warped && fails == kReturnTries);
	}
	{
		Ladder l;
		Sim    sim;
		l.Start(kSite, kGeo, false, -1.0f, kHome, 1.0f, sim.now);
		sim.now += 16;
		auto s = l.Update(sim.F(), 1.0 / 60.0);
		CHECK(s.place.has_value() && l.Current() == Phase::kPlace);
		l.PlacementFailed(sim.now);  // the stand placement at the ladder refused: he never left home
		bool warped = false;
		for (int i = 0; i < 60 && l.Active(); ++i) {
			sim.now += 16;
			s = l.Update(sim.F(), 1.0 / 60.0);
			warped |= s.warpHome;
			sim.Apply(s);
		}
		CHECK(!l.Active() && !warped && sim.actions == 0 && Near(sim.root.x, kHome.x));
	}
}

static void TestMantle()
{
	const MantleSite vault{ { 826.39f, -11.95f, 1218.62f }, 90.0f, 100, true };
	const MantleSite low{ { 1.0f, 0.0f, 1.0f }, 0.0f, 80, true };
	const MantleSite high{ { 2.0f, 0.0f, 2.0f }, 0.0f, 160, true };
	const MantleSite none{};
	CHECK(PickMantle(100, true, vault, low, high, 110).stand.x == vault.stand.x);
	CHECK(PickMantle(90, false, vault, low, high, 110).stand.x == low.stand.x);
	CHECK(PickMantle(150, false, vault, low, high, 110).stand.x == high.stand.x);
	CHECK(PickMantle(150, false, vault, low, none, 110).stand.x == low.stand.x);
	CHECK(PickMantle(60, false, vault, none, high, 110).stand.x == high.stand.x);
	CHECK(PickMantle(60, false, vault, none, none, 110).cutAtTop && PickMantle(60, false, vault, none, none, 110).stand.x == vault.stand.x);
	CHECK(!PickMantle(60, false, none, none, none, 110).valid);
	CHECK(!PickMantle(90, false, vault, low, high, 110).cutAtTop && !PickMantle(100, true, vault, low, high, 110).cutAtTop);
	CHECK(!PickMantle(60, true, none, low, high, 110).valid);
	// The step site (the 75 cm sacks) takes low ledges up to its cm, the low / high ones the rest; unset = as before; never a vault.
	{
		MantleSite step = low;
		step.stand.x += 1.0f;
		step.heightCm = 75;
		CHECK(PickMantle(57, false, vault, low, high, 110, step, 89).stand.x == step.stand.x);
		CHECK(PickMantle(89, false, vault, low, high, 110, step, 89).stand.x == step.stand.x);
		CHECK(PickMantle(90, false, vault, low, high, 110, step, 89).stand.x == low.stand.x);
		CHECK(PickMantle(150, false, vault, low, high, 110, step, 89).stand.x == high.stand.x);
		CHECK(PickMantle(57, false, vault, low, high, 110, none, 89).stand.x == low.stand.x);
		CHECK(PickMantle(57, true, vault, low, high, 110, step, 89).stand.x == vault.stand.x);
		CHECK(PickMantle(57, false, none, none, none, 110, step, 89).stand.x == step.stand.x);
	}

	// The move: Y, out of NORMAL_ACTION ~1.1 s (his root up 1 m), back; then home.
	Mantle m;
	Sim    sim;
	m.Start(vault, kHome, 1.0f, sim.now);
	std::uint64_t pressedAt = 0;
	for (int i = 0; i < 600 && m.Active(); ++i) {
		sim.now += 16;
		const auto s = m.Update(sim.F());
		if (s.place) {
			sim.root = *s.place;
		}
		if (s.pressAction) {
			pressedAt = sim.now;
			sim.normal = false;
		}
		if (pressedAt && !sim.normal) {
			const auto t = sim.now - pressedAt;
			sim.root.y = vault.stand.y + (t < 500 ? 1.0f * static_cast<float>(t) / 500.0f : t < 900 ? 1.0f : 0.0f);
			if (t >= 1100) {
				sim.normal = true;
			}
		}
	}
	CHECK(!m.Active() && m.MoveMs() >= 880 && m.MoveMs() < 1000 && Near(m.Rose(), 1.0f, 0.05f));  // over once down on the far side
	CHECK(Near(sim.root.x, kHome.x) && Near(sim.root.z, kHome.z));

	// A climb-up on the vault site: over at the top (~0.5-0.9 s in), not after the drop down the far side.
	{
		Mantle c;
		Sim    cs;
		MantleSite cut = vault;
		cut.cutAtTop = true;
		c.Start(cut, kHome, 1.0f, cs.now);
		std::uint64_t at = 0;
		for (int i = 0; i < 600 && c.Active(); ++i) {
			cs.now += 16;
			const auto st = c.Update(cs.F());
			if (st.place) {
				cs.root = *st.place;
			}
			if (st.pressAction) {
				at = cs.now;
				cs.normal = false;
			}
			if (at && !cs.normal) {
				const auto t = cs.now - at;
				cs.root.y = vault.stand.y + (t < 500 ? static_cast<float>(t) / 500.0f : t < 900 ? 1.0f - static_cast<float>(t - 500) / 400.0f : 0.0f);
				if (t >= 1100) {
					cs.normal = true;
				}
			}
		}
		CHECK(!c.Active() && c.MoveMs() >= 500 && c.MoveMs() < 700 && Near(cs.root.x, kHome.x));
	}
	// The placement home lands nowhere (MGSV held him at the obstacle): placed again; never "back on the sandbox" away from it; the
	// Warp after kReturnTries.
	for (const bool landsLater : { true, false }) {
		Mantle c;
		Sim    cs;
		c.Start(vault, kHome, 1.0f, cs.now);
		std::uint64_t at = 0;
		int           homePlacements = 0;
		bool          warped = false, falseHome = false;
		for (int i = 0; i < 900 && c.Active(); ++i) {
			cs.now += 16;
			const auto st = c.Update(cs.F());
			warped |= st.warpHome;
			falseHome |= !st.active && !st.warpHome && (cs.root - kHome).LengthXZ() > kHomeRadius;
			if (st.place) {
				const bool home = (*st.place - kHome).LengthXZ() < 0.01f;
				homePlacements += home;
				if (!home || (landsLater && homePlacements >= 2)) {
					cs.root = *st.place;
				}
			}
			if (st.pressAction) {
				at = cs.now;
				cs.normal = false;
			}
			if (at && !cs.normal) {
				const auto t = cs.now - at;
				cs.root.y = vault.stand.y + (t < 500 ? 1.0f * static_cast<float>(t) / 500.0f : t < 900 ? 1.0f : 0.0f);
				if (t >= 1100) {
					cs.normal = true;
				}
			}
		}
		CHECK(!c.Active() && !falseHome);
		CHECK(landsLater ? (homePlacements == 2 && !warped && Near(cs.root.x, kHome.x)) : (homePlacements == kReturnTries && warped));
	}
}

// After a move the odometer stays still for the settle (300 ms), unless the player pushes the stick (review of 0.6.0: the dead
// stick after a ladder).
static void TestOdoThaw()
{
	using W = OdoThaw::Why;
	OdoThaw t;
	CHECK(!t.Active());
	t.Start(1000);
	CHECK(t.Active());
	CHECK(t.Update(1016, 0, 0) == W::kStill);
	CHECK(t.Update(1033, 3000, -5000) == W::kStill);  // inside the dead zone: the settle, not the player
	CHECK(t.Update(1050, 0, 20000) == W::kStick && !t.Active() && t.EndedAfterMs() == 50);
	t.Start(2000);
	CHECK(t.Update(2299, 0, 0) == W::kStill);
	CHECK(t.Update(2300, 0, 0) == W::kCap && !t.Active() && t.EndedAfterMs() == 300);
	t.Start(3000);
	CHECK(t.Update(3016, -9000, 0) == W::kStick);  // any direction
	t.Start(4000);
	t.Cancel();  // a new move began
	CHECK(!t.Active());
}

int main()
{
	TestGeometry();
	TestPacer();
	TestLadderUp();
	TestLadderDownAndEarlyStop();
	TestLadderRefused();
	TestRecycleShift();
	TestLadderSeamless();
	TestLadderStill();
	TestLadderEnding();
	TestLadderNeverHome();
	TestPlacementRefused();
	TestMantle();
	TestOdoThaw();
	std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ok", g_failures);
	return g_failures ? 1 : 0;
}
