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

		void Apply(const Step& a_s)
		{
			const Vec3 line = kSite.origin + Facing(kSite) * kGeo.lineOut;
			if (a_s.knockOff && onLadder) {
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
				if (onLadder) {
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
	p.Start(0.0f);
	CHECK(p.Push(1.0 / 60.0, 0.0f));  // unknown speed: MGSV's own pace
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
	l.Start(kSite, kGeo, false, 0.0f, kHome, 1.0f, sim.now);
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
		l.Start(kSite, kGeo, true, 0.0f, kHome, 1.0f, sim.now);
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
		l.Start(kSite, kGeo, false, 0.0f, kHome, 1.0f, sim.now);
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

// A ladder that never takes him: placed, faced and Y three times, then home; never any stick.
static void TestLadderRefused()
{
	Ladder l;
	Sim    sim;
	l.Start(kSite, kGeo, false, 0.0f, kHome, 1.0f, sim.now);
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
}

int main()
{
	TestGeometry();
	TestPacer();
	TestLadderUp();
	TestLadderDownAndEarlyStop();
	TestLadderRefused();
	TestMantle();
	std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ok", g_failures);
	return g_failures ? 1 : 0;
}
