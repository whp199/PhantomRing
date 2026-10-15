// Offline tests of ActorProxy v0's pure half (mgsv/ActorProxy.cpp, docs/GUEST.md §20): the treadmill offset correction, target
// selection, stable soldier assignment, clearance parking, the pool string, the capture filter and the auto-aim gate.
#include "ActorProxy.h"
#include "FultonAnchor.h"

#include "phantomring_protocol.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace phantom;
using namespace phantom::guest::proxy::plan;
using phantom::guest::logic::Vec3;

static int g_failures = 0;
#define CHECK(x)                                                    \
	do {                                                            \
		if (!(x)) {                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			++g_failures;                                           \
		}                                                           \
	} while (0)

static bool Near(const Vec3& a, const Vec3& b, float eps = 1e-3f) { return (a - b).Length() <= eps; }

static proto::ActorRecord Rec(std::uint64_t a_handle, float x, float y, float z, std::uint32_t a_flags = proto::kActorHostile, float a_yaw = 0.0f)
{
	proto::ActorRecord r{};
	r.handle = a_handle;
	r.flags = a_flags;
	r.pos[0] = x;
	r.pos[1] = y;
	r.pos[2] = z;
	r.yaw = a_yaw;
	r.radius = 0.5f;
	r.height = 1.8f;
	return r;
}

static void TestOffsetHistory()
{
	OffsetHistory h;
	CHECK(h.Empty() && Near(h.At(100), {}));
	h.Record(1000, { 1, 0, 0 });
	h.Record(1016, { 1, 0, 0 });
	h.Record(1033, { 7, 0, 0 });  // a treadmill warp: k jumped by 6 m
	h.Record(1050, { 7, 0, 0 });
	CHECK(Near(h.Latest(), { 7, 0, 0 }));
	CHECK(Near(h.At(1050), { 7, 0, 0 }));
	CHECK(Near(h.At(1040), { 7, 0, 0 }));
	CHECK(Near(h.At(1032), { 1, 0, 0 }));  // before the warp
	CHECK(Near(h.At(10), { 1, 0, 0 }));    // older than every record: the oldest
	// Wraps without losing the newest.
	for (std::uint64_t t = 0; t < 1000; ++t) {
		h.Record(2000 + t, { static_cast<float>(t), 0, 0 });
	}
	CHECK(Near(h.Latest(), { 999, 0, 0 }));
	CHECK(Near(h.At(2999), { 999, 0, 0 }));
	CHECK(Near(h.At(2950), { 950, 0, 0 }));
	h.Clear();
	CHECK(h.Empty());
}

static void TestTreadmillOffset()
{
	// The odometer runs to kilometres: k must come out to the centimetre.
	const double odo[3] = { 123456.25, -3.5, -98765.75 };
	const Vec3   k = TreadmillOffset({ 820.6f, -11.1f, 1179.1f }, odo);
	CHECK(std::fabs(k.x - static_cast<float>(820.6f - 123456.25)) < 0.01f);
	CHECK(std::fabs(k.y - (-11.1f + 3.5f)) < 0.01f);
	CHECK(std::fabs(k.z - static_cast<float>(1179.1f + 98765.75)) < 0.01f);
}

static void TestSelect()
{
	Settings s;
	s.maxDistance = 50.0f;
	const Vec3 snake{ 0, 0, 0 };
	std::vector<proto::ActorRecord> t;
	t.push_back(Rec(1, 30, 0, 0));                                          // hostile 30 m
	t.push_back(Rec(2, 10, 5, 0));                                          // hostile 10 m (5 m higher: horizontal distance)
	t.push_back(Rec(3, 5, 0, 0, 0));                                        // not hostile
	t.push_back(Rec(4, 6, 0, 0, proto::kActorHostile | proto::kActorDead));  // dead
	t.push_back(Rec(5, 60, 0, 0));                                          // too far
	t.push_back(Rec(6, NAN, 0, 0));                                         // not finite
	t.push_back(Rec(0, 3, 0, 0));                                           // no handle
	t.push_back(Rec(7, 0, 0, -20, proto::kActorHostile | proto::kActorBoss));
	const auto out = SelectTargets(t.data(), static_cast<std::uint32_t>(t.size()), snake, {}, {}, s);
	CHECK(out.size() == 3);
	CHECK(out.size() == 3 && out[0].handle == 2 && out[1].handle == 7 && out[2].handle == 1);
	CHECK(out.size() == 3 && std::fabs(out[0].distance - 10.0f) < 1e-4f && Near(out[0].pos, { 10, 5, 0 }));
	CHECK(SelectTargets(nullptr, 3, snake, {}, {}, s).empty());

	// The table was built before a treadmill warp that moved Snake (and k) by -6 m in x: the ER enemy is 6 m further along -x
	// in MGSV's world than the table says (Snake stays at the same odometer place, the world under him moved).
	const Vec3 kThen{ 100, 0, 0 }, kNow{ 94, 0, 0 };
	std::vector<proto::ActorRecord> one{ Rec(9, 20, 0, 0) };
	const auto shifted = SelectTargets(one.data(), 1, { -6, 0, 0 }, kThen, kNow, s);
	CHECK(shifted.size() == 1 && Near(shifted[0].pos, { 14, 0, 0 }) && std::fabs(shifted[0].distance - 20.0f) < 1e-4f);
}

static std::vector<Slot> Slots(std::size_t n)
{
	std::vector<Slot> v;
	for (std::size_t i = 0; i < n; ++i) {
		v.push_back({ .id = static_cast<std::uint16_t>(0x400 + i) });
	}
	return v;
}

static void TestAssign()
{
	Settings s;
	s.maxProxies = 2;
	const Vec3 snake{ 0, 0, 0 }, centre{ 0, 0, 0 };
	auto       slots = Slots(4);
	std::vector<Target> targets{ { 11, { 10, 0, 0 }, 0.5f, 10 }, { 12, { 20, 0, 0 }, 1.0f, 20 }, { 13, { 30, 0, 0 }, 0.0f, 30 } };
	Assign(slots, targets, snake, nullptr, centre, s);
	CHECK(slots[0].handle == 11 && slots[0].proxy && Near(slots[0].pos, { 10, 0, 0 }) && slots[0].yaw == 0.5f);
	CHECK(slots[1].handle == 12 && slots[1].proxy && Near(slots[1].pos, { 20, 0, 0 }));
	CHECK(!slots[2].proxy && !slots[2].handle && Near(slots[2].pos, ParkPosition(centre, 2, s)));
	CHECK(!slots[3].proxy && Near(slots[3].pos, { 2.0f * 3, -100, 0 }));
	CHECK(slots[0].id == 0x400 && slots[3].id == 0x403);  // ids are kept

	// 12 comes nearer than 11: nobody swaps soldiers.
	targets = { { 12, { 5, 0, 0 }, 0, 5 }, { 11, { 15, 0, 0 }, 0, 15 }, { 13, { 30, 0, 0 }, 0, 30 } };
	Assign(slots, targets, snake, nullptr, centre, s);
	CHECK(slots[0].handle == 11 && Near(slots[0].pos, { 15, 0, 0 }));
	CHECK(slots[1].handle == 12 && Near(slots[1].pos, { 5, 0, 0 }));

	// 11 dies (gone from the targets): its soldier takes 13, 12 stays put.
	targets = { { 12, { 5, 0, 0 }, 0, 5 }, { 13, { 30, 0, 0 }, 0, 30 } };
	Assign(slots, targets, snake, nullptr, centre, s);
	CHECK(slots[0].handle == 13 && slots[0].proxy && slots[1].handle == 12);

	// Too near Snake or the buddy: keeps its soldier, parked for now; back when it steps out.
	const Vec3 buddy{ 30, 0, 3 };
	targets = { { 12, { 2, 0, 0 }, 0, 2 }, { 13, { 30, 0, 0 }, 0, 30 } };
	Assign(slots, targets, snake, &buddy, centre, s);
	CHECK(slots[1].handle == 12 && !slots[1].proxy && Near(slots[1].pos, ParkPosition(centre, 1, s)));
	CHECK(slots[0].handle == 13 && !slots[0].proxy && Near(slots[0].pos, ParkPosition(centre, 0, s)));
	// An asleep character's proxy stays beside Snake (MGSV's Fulton wants the soldier right there; v1, live 2026-10-14).
	targets = { { 12, { 1, 0, 0 }, 0, 1, true }, { 13, { 30, 0, 0 }, 0, 30 } };
	Assign(slots, targets, snake, &buddy, centre, s);
	CHECK(slots[1].handle == 12 && slots[1].proxy && Near(slots[1].pos, { 1, 0, 0 }));
	// SelectTargets carries kActorAsleep.
	const proto::ActorRecord sleeper = Rec(21, 4, 0, 0, proto::kActorHostile | proto::kActorAsleep);
	const auto picked = SelectTargets(&sleeper, 1, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, s);
	CHECK(picked.size() == 1 && picked[0].asleep);
	targets = { { 12, { 4, 0, 0 }, 0, 4 }, { 13, { 30, 0, 0 }, 0, 30 } };
	Assign(slots, targets, snake, nullptr, centre, s);
	CHECK(slots[1].handle == 12 && slots[1].proxy && slots[0].proxy);

	// No targets: everything parked, no handles left.
	Assign(slots, {}, snake, nullptr, centre, s);
	bool allParked = true;
	for (const auto& sl : slots) {
		allParked = allParked && !sl.proxy && !sl.handle;
	}
	CHECK(allParked);

	// maxProxies 0 (or an empty pool): nothing stands in.
	s.maxProxies = 0;
	Assign(slots, { { 1, { 10, 0, 0 }, 0, 10 } }, snake, nullptr, centre, s);
	CHECK(!slots[0].proxy && !slots[0].handle);
	std::vector<Slot> none;
	Assign(none, { { 1, { 10, 0, 0 }, 0, 10 } }, snake, nullptr, centre, s);
	CHECK(none.empty());
}

static void TestNeedsPlace()
{
	Settings s;
	CHECK(!NeedsPlace({ 1, 2, 3 }, 0.1f, { 1, 2, 3.01f }, 0.1f, 1, s));
	CHECK(NeedsPlace({ 1, 2, 3 }, 0.1f, { 1, 2, 3.05f }, 0.1f, 1, s));
	CHECK(NeedsPlace({ 1, 2, 3 }, 0.1f, { 1, 2, 3 }, 0.2f, 1, s));
	CHECK(!NeedsPlace({ 1, 2, 3 }, 3.14f, { 1, 2, 3 }, -3.14f, 1, s));  // across the wrap: 0.18 deg
	CHECK(NeedsPlace({ 1, 2, 3 }, 0.1f, { 1, 2, 3 }, 0.1f, 30, s));      // the periodic refresh
}

static void TestParsePool()
{
	auto p = ParsePool("1024,1025, 1030;2048");
	CHECK(p.soldiers.size() == 3 && p.soldiers[0] == 1024 && p.soldiers[2] == 1030);
	CHECK(p.hostages.size() == 1 && p.hostages[0] == 2048);
	p = ParsePool(";");
	CHECK(p.soldiers.empty() && p.hostages.empty());
	p = ParsePool("7,7,x,65535,70000,,8");  // duplicates, junk, the null id and out-of-range values dropped
	CHECK(p.soldiers.size() == 2 && p.soldiers[0] == 7 && p.soldiers[1] == 8 && p.hostages.empty());
	p = ParsePool("");
	CHECK(p.soldiers.empty());
	// Review 2026-10-14: the pool is only taken from disabled soldiers ([Boot] DisableSoldiers = true): ProxyPool makes them
	// damage-immune, so with live AI they would be unkillable soldiers placed beside Snake.
	CHECK(PoolRefusal(true) == nullptr);
	CHECK(PoolRefusal(false) != nullptr && std::string_view(PoolRefusal(false)).find("DisableSoldiers") != std::string_view::npos);
}

static void TestNearProxy()
{
	const Vec3 proxies[2] = { { 5, 0, 0 }, { -10, 0, 4 } };
	const Vec3 snake{ 0, 0, 0 };
	CHECK(NearProxy({ 5.5f, 1.0f, 0 }, snake, proxies, 2, 1.5f));    // the soldier's torso
	CHECK(!NearProxy({ 0.3f, 1.0f, 0 }, snake, proxies, 2, 1.5f));   // Snake's own draw
	CHECK(!NearProxy({ 5, 4.0f, 0 }, snake, proxies, 2, 1.5f));      // 4 m above: not the soldier
	CHECK(NearProxy({ -10, 0, 5 }, snake, proxies, 2, 1.5f));
	CHECK(!NearProxy({ 5, 0, 0 }, snake, proxies, 0, 1.5f));
	CHECK(!NearProxy({ 5, 0, 0 }, snake, nullptr, 2, 1.5f));
	// A draw between Snake and a proxy 2 m away: nearer to Snake stays his.
	const Vec3 close[1] = { { 2, 0, 0 } };
	CHECK(!NearProxy({ 0.8f, 0, 0 }, snake, close, 1, 1.5f));
	CHECK(NearProxy({ 1.2f, 0, 0 }, snake, close, 1, 1.5f));
}

// Inert full proxies (docs/GUEST.md 20.6): flattened placement, the tolerant read-back, location ownership, the aim correction
// and the alert phase guard.
static void TestHeight()
{
	CHECK(ParseHeight("ground", Height::kEnemy) == Height::kGround);
	CHECK(ParseHeight("enemy", Height::kGround) == Height::kEnemy);
	CHECK(ParseHeight("sky", Height::kGround) == Height::kGround);
	CHECK(Near(ProxyFeet({ 5, -4.5f, 7 }, -12.0f, Height::kGround), { 5, -12.0f, 7 }));
	CHECK(Near(ProxyFeet({ 5, -4.5f, 7 }, -12.0f, Height::kEnemy), { 5, -4.5f, 7 }));
	CHECK(Near(ProxyFeet({ 5, -4.5f, 7 }, NAN, Height::kGround), { 5, -4.5f, 7 }));  // no ground known: as v1

	// Assign flattens with kGround and keeps the real place; the default (kEnemy, the Fulton-only path) is unchanged.
	Settings s;
	s.maxProxies = 2;
	const Vec3          snake{ 0, -12, 0 }, centre{ 0, -12, 0 };
	auto                slots = Slots(2);
	std::vector<Target> targets{ { 11, { 10, -9, 0 }, 0, 10 }, { 12, { 20, -17.5f, 0 }, 0, 20 } };
	Assign(slots, targets, snake, nullptr, centre, s);
	CHECK(Near(slots[0].pos, { 10, -9, 0 }) && Near(slots[0].real, { 10, -9, 0 }));
	s.height = Height::kGround;
	Assign(slots, targets, snake, nullptr, centre, s);
	CHECK(slots[0].proxy && Near(slots[0].pos, { 10, -12, 0 }) && Near(slots[0].real, { 10, -9, 0 }));
	CHECK(slots[1].proxy && Near(slots[1].pos, { 20, -12, 0 }) && Near(slots[1].real, { 20, -17.5f, 0 }));
	// Clearance still measured from where the character is (horizontal), and a parked slot's real place is its park place.
	targets = { { 11, { 2, -6, 0 }, 0, 2 }, { 12, { 20, -17.5f, 0 }, 0, 20 } };
	Assign(slots, targets, snake, nullptr, centre, s);
	CHECK(!slots[0].proxy && Near(slots[0].real, slots[0].pos));

	// Read-back: x/z within 1 m; y within its own tolerance (MGSV's ground under a far proxy is a kerb off the centre's).
	CHECK(ReadBackOk({ 10, -12, 0 }, { 10.3f, -11.2f, 0.2f }, 1.0f, 2.0f));
	CHECK(!ReadBackOk({ 10, -12, 0 }, { 10.3f, -9.5f, 0.2f }, 1.0f, 2.0f));   // pushed 2.5 m up: not where he was put
	CHECK(!ReadBackOk({ 10, -12, 0 }, { 11.2f, -12, 0 }, 1.0f, 2.0f));         // slid 1.2 m
	CHECK(!ReadBackOk({ 10, -12, 0 }, { NAN, -12, 0 }, 1.0f, 2.0f));

	// Location ownership: the cache lags far soldiers ~1 m; 5 m apart = another character in the pooled object.
	CHECK(OwnsLocation({ 30, -12, 0 }, { 31.1f, -12, 0.4f }, 5.0f));
	CHECK(!OwnsLocation({ 30, -12, 0 }, { 40, -12, 0 }, 5.0f));
	CHECK(OwnsLocation({ 30, 0, 0 }, { 30, -12, 0 }, 5.0f));  // horizontal only (the cache's y lags a flattened proxy)
}

static void TestCorrectShot()
{
	// Snake's muzzle at 1.4 m over the ground at the origin; a proxy on the ground 20 m ahead (+z) whose enemy stands 3 m higher.
	const Vec3 origin{ 0, 1.4f, 0 };
	const Body bodies[] = { { { 0, 0, 20 }, { 0, 3, 20 } }, { { 10, 0, 10 }, { 10, 0, 10 } } };
	// Aimed at the proxy's chest (1.3 m up): turned to the same point 3 m higher.
	Vec3       dir = Vec3{ 0, 1.3f - 1.4f, 20 } * (1.0f / Vec3{ 0, -0.1f, 20 }.Length());
	CHECK(CorrectShot(origin, dir, bodies, 2, 0.9f, 2.0f, 80.0f) == 0);
	const Vec3 want = Vec3{ 0, 4.3f - 1.4f, 20 } * (1.0f / Vec3{ 0, 2.9f, 20 }.Length());
	CHECK(Near(dir, want, 2e-3f));
	CHECK(std::fabs(dir.Length() - 1.0f) < 1e-4f);
	// A near miss (0.6 m to the side) keeps the player's sideways offset.
	dir = Vec3{ 0.6f, -0.1f, 20 } * (1.0f / Vec3{ 0.6f, -0.1f, 20 }.Length());
	CHECK(CorrectShot(origin, dir, bodies, 2, 0.9f, 2.0f, 80.0f) == 0 && dir.y > 0.1f && dir.x > 0.02f);
	// Wide of every body (3 m beside): unchanged.
	Vec3       wide = Vec3{ 3, -0.1f, 20 } * (1.0f / Vec3{ 3, -0.1f, 20 }.Length());
	const Vec3 wide0 = wide;
	CHECK(CorrectShot(origin, wide, bodies, 2, 0.9f, 2.0f, 80.0f) == -1 && Near(wide, wide0, 1e-6f));
	// Over his head (aimed 3 m above the proxy's feet, the body is 2 m): unchanged.
	Vec3 high = Vec3{ 0, 3.5f - 1.4f, 20 } * (1.0f / Vec3{ 0, 2.1f, 20 }.Length());
	CHECK(CorrectShot(origin, high, bodies, 2, 0.9f, 2.0f, 80.0f) == -1);
	// Behind Snake or beyond the range: unchanged.
	Vec3 back{ 0, 0, -1 };
	CHECK(CorrectShot(origin, back, bodies, 2, 0.9f, 2.0f, 80.0f) == -1);
	Vec3 ahead = Vec3{ 0, -0.1f, 20 } * (1.0f / Vec3{ 0, -0.1f, 20 }.Length());
	CHECK(CorrectShot(origin, ahead, bodies, 2, 0.9f, 2.0f, 15.0f) == -1);
	// A level character at 10 m in front of the raised one's stand-in: the round meets his real body, so it stays as aimed.
	const Body inLine[] = { { { 0, 0, 20 }, { 0, 3, 20 } }, { { 0, 0, 10 }, { 0, 0, 10 } } };
	Vec3       d2 = Vec3{ 0, -0.1f, 20 } * (1.0f / Vec3{ 0, -0.1f, 20 }.Length());
	const Vec3 d20 = d2;
	CHECK(CorrectShot(origin, d2, inLine, 2, 0.9f, 2.0f, 80.0f) == -1 && Near(d2, d20, 1e-6f));
	// The nearer of two stand-ins on the ray wins: one 3 m up at 10 m (its real body well above the ray) before the one at 20 m.
	const Body twoUp[] = { { { 0, 0, 20 }, { 0, 3, 20 } }, { { 0, 0, 10 }, { 0, 4, 10 } } };
	d2 = d20;
	CHECK(CorrectShot(origin, d2, twoUp, 2, 0.9f, 2.0f, 80.0f) == 1 && d2.y > 0.25f);
	// Aimed by hand at the real chest (1.2 m over his feet) of an enemy 1 m ABOVE Snake's ground, 20 m away: the ray also passes
	// through his flattened stand-in (2.2 m up its 2 m axis, within the radius) but must not be lifted another metre over his head.
	const Body ledge[] = { { { 0, 0, 20 }, { 0, 1, 20 } } };
	Vec3       chest = Vec3{ 0, 2.2f - 1.4f, 20 } * (1.0f / Vec3{ 0, 0.8f, 20 }.Length());
	const Vec3 chest0 = chest;
	CHECK(CorrectShot(origin, chest, ledge, 1, 0.9f, 2.0f, 80.0f) == -1 && Near(chest, chest0, 1e-6f));
	// ... nor one 1.5 m BELOW (chest at -0.3 m): not pushed into the ground.
	const Body dip[] = { { { 0, 0, 20 }, { 0, -1.5f, 20 } } };
	chest = Vec3{ 0, -0.3f - 1.4f, 20 } * (1.0f / Vec3{ 0, -1.7f, 20 }.Length());
	const Vec3 dip0 = chest;
	CHECK(CorrectShot(origin, chest, dip, 1, 0.9f, 2.0f, 80.0f) == -1 && Near(chest, dip0, 1e-6f));
	// Enemy A on a roof 3 m up at 15 m (stand-in on the ground there), enemy B level at 30 m: a round aimed at B's chest crosses
	// A's stand-in first but goes to B unchanged (not up to A).
	const Body roof[] = { { { 0, 0, 15 }, { 0, 3, 15 } }, { { 0, 0, 30 }, { 0, 0, 30 } } };
	Vec3       atB = Vec3{ 0, 1.2f - 1.4f, 30 } * (1.0f / Vec3{ 0, -0.2f, 30 }.Length());
	const Vec3 atB0 = atB;
	CHECK(CorrectShot(origin, atB, roof, 2, 0.9f, 2.0f, 80.0f) == -1 && Near(atB, atB0, 1e-6f));
	// Straight down / nothing / NaN: unchanged.
	Vec3 down{ 0, -1, 0 };
	CHECK(CorrectShot(origin, down, bodies, 2, 0.9f, 2.0f, 80.0f) == -1);
	CHECK(CorrectShot(origin, dir, nullptr, 0, 0.9f, 2.0f, 80.0f) == -1);
	Vec3 bad{ NAN, 0, 1 };
	CHECK(CorrectShot(origin, bad, bodies, 2, 0.9f, 2.0f, 80.0f) == -1);
}

static void TestPhaseGuard()
{
	PhaseGuard    g;
	PhaseSettings ps;
	ps.calmEveryMs = 1000;
	ps.giveUpMs = 20000;
	auto st = g.Update(0, 1000, ps);
	CHECK(!st.calm && !st.sweep && !st.giveUp && g.Alerts() == 0);
	// The first alert read: sweep + calm at once.
	st = g.Update(3, 5000, ps);
	CHECK(st.calm && st.sweep && !st.giveUp && g.Alerts() == 1);
	// Still up: calm again once a second; sweep again every 3 s (a reinforcement arrives some time into an alert).
	st = g.Update(3, 5500, ps);
	CHECK(!st.calm && !st.sweep);
	st = g.Update(2, 6000, ps);
	CHECK(st.calm && !st.sweep);
	st = g.Update(3, 7000, ps);
	CHECK(st.calm && !st.sweep);
	st = g.Update(3, 8000, ps);
	CHECK(st.calm && st.sweep);
	st = g.Update(3, 9000, ps);
	CHECK(st.calm && !st.sweep);
	// An unknown read changes nothing.
	st = g.Update(-1, 9500, ps);
	CHECK(!st.calm && !st.sweep && !st.giveUp);
	// Calmed: back to 0; a new alert later sweeps again at once.
	st = g.Update(0, 10000, ps);
	CHECK(!st.calm && !st.sweep);
	st = g.Update(1, 12000, ps);
	CHECK(st.sweep && st.calm && g.Alerts() == 2);
	// Up 20 s without a break: give up, once; sweeps keep coming every 3 s meanwhile.
	int sweeps = 0;
	for (std::uint64_t t = 12500; t < 32000; t += 500) {
		st = g.Update(3, t, ps);
		CHECK(!st.giveUp);
		sweeps += st.sweep;
	}
	CHECK(sweeps == 6);
	st = g.Update(3, 32000, ps);
	CHECK(st.giveUp && g.GaveUp());
	st = g.Update(3, 43000, ps);
	CHECK(!st.giveUp && st.calm && st.sweep);  // keeps calming after giving up
	// Full proxies turned on again (`!proxy on`): a fresh guard -- not "gave up", an alert still up counts as a new one (sweep
	// at once) and gives up again after another 20 s.
	g.Reset();
	CHECK(!g.GaveUp() && g.Alerts() == 0);
	st = g.Update(3, 50000, ps);
	CHECK(st.sweep && st.calm && g.Alerts() == 1);
	st = g.Update(3, 70000, ps);
	CHECK(st.giveUp && g.GaveUp());
}

static void TestAutoAim()
{
	CHECK(AutoAimWanted(true, true, true, 6, 6, 0));
	CHECK(!AutoAimWanted(false, true, true, 6, 6, 0));  // proxies off
	CHECK(!AutoAimWanted(true, false, true, 6, 6, 0));  // [Proxies] AutoAim = false
	CHECK(!AutoAimWanted(true, true, false, 6, 6, 0));  // saves not guarded
	CHECK(!AutoAimWanted(true, true, true, 6, 5, 1));   // one soldier didn't read back placed
	CHECK(!AutoAimWanted(true, true, true, 0, 0, 0));   // no pool
}

static void TestFultonAnchor()
{
	namespace fp = phantom::guest::fulton::plan;
	const std::uint32_t asleep = proto::kActorHostile | proto::kActorAsleep;
	const proto::ActorRecord recs[] = {
		Rec(1, 2.0f, 0.0f, 0.0f, asleep),                                // 2 m, asleep
		Rec(2, 1.0f, 0.0f, 0.0f),                                        // nearer, awake
		Rec(3, 0.5f, 0.0f, 0.5f, asleep | proto::kActorDead),            // dead
		Rec(4, 1.5f, 0.0f, 0.0f, proto::kActorAsleep),                   // not hostile
		Rec(5, 0.0f, 5.0f, 1.0f, asleep),                                // a floor above
		Rec(6, 2.5f, 0.0f, 0.0f, asleep),
	};
	CHECK(fp::PickSleeper(recs, 6, { 0, 0, 0 }, 3.0f, {}) == 0);
	CHECK(fp::PickSleeper(recs, 6, { 0, 0, 0 }, 3.0f, { 1 }) == 5);  // the first one fultoned already
	CHECK(fp::PickSleeper(recs, 6, { 0, 0, 0 }, 1.9f, {}) == -1);
	// Snake stands where the soldier lies as Snake does to the enemy: enemy 1.5 m +x of Snake -> Snake 1.5 m -x of the soldier.
	CHECK(Near(fp::SnakeSpot({ 10, 0, 10 }, { 11.5f, 0.2f, 10.5f }, { 800, -12, 1200 }, -11.9f), { 798.5f, -11.9f, 1199.5f }));
	const auto c = fp::ParseCandidates("1104:816.671,-11.954,1211.773;bad;12:1,2,3;1100:800.456,-11.729,1210.589;1105:x");
	CHECK(c.size() == 2 && c[0].id == 1104 && c[1].id == 1100 && Near(c[1].pos, { 800.456f, -11.729f, 1210.589f }));
	CHECK(fp::ParseCandidates("").empty());
}

int main()
{
	TestFultonAnchor();
	TestOffsetHistory();
	TestTreadmillOffset();
	TestSelect();
	TestAssign();
	TestNeedsPlace();
	TestParsePool();
	TestNearProxy();
	TestAutoAim();
	TestHeight();
	TestCorrectShot();
	TestPhaseGuard();
	if (g_failures) {
		std::printf("test_guest_actorproxy: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("test_guest_actorproxy: all passed\n");
	return 0;
}
