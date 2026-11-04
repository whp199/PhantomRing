// Offline tests of the D-Horse treadmill's pure half (mgsv/HorseLogic.cpp, docs/GUEST.md 8.1a): picking the horse's location
// object near its skin, confirming and losing it, the setter's arguments for a jump, the treadmill's choice, and a ridden gallop
// through the treadmill with the frame glue copied in (the odometer -- what Elden Ring sees -- stays continuous, the tracker
// confirmed, the horse on the ground under Snake), under the assumption that the saddle carries him, and the fallback when it doesn't.
// Also D-Walker's lead alongside a moving Snake (mgsv/WalkerLogic.h, GUEST.md 18.3 "Led alongside"): the same location objects.
#include "HorseLogic.h"
#include "Logic.h"
#include "WalkerLogic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace phantom::guest;
using namespace phantom::guest::horse::plan;
using logic::Vec3;

static int g_failures = 0;
#define CHECK(x)                                                    \
	do {                                                            \
		if (!(x)) {                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			++g_failures;                                           \
		}                                                           \
	} while (0)

static bool Near(const Vec3& a, const Vec3& b, float eps = 1e-3f) { return (a - b).Length() <= eps; }

static void TestPick()
{
	const Vec3                   horse{ 820.0f, -12.0f, 1180.0f };
	const std::vector<LocSample> s = {
		{ 0x1000, { 0.0f, 0.0f, 0.0f } },               // a pooled object at the origin
		{ 0x2000, { 820.5f, -12.1f, 1180.2f } },        // the horse's, 0.54 m off
		{ 0x3000, { 820.1f, -10.6f, 1180.0f } },        // Snake in the saddle, 1.4 m above
		{ 0x4000, { 825.0f, -12.0f, 1180.0f } },        // a soldier 5 m away
		{ 0x5000, { 820.2f, -12.0f, 1180.1f } },        // nearer still, but excluded (Snake's own / a proxy)
	};
	CHECK(PickLocation(s, horse, {}) == 0x5000);
	CHECK(PickLocation(s, horse, { 0x5000 }) == 0x2000);
	CHECK(PickLocation(s, horse, { 0x5000, 0x2000 }) == 0);  // Snake's 1.4 m up never qualifies
	CHECK(PickLocation({ { 0x3000, { 820.1f, -10.6f, 1180.0f } } }, horse, {}) == 0);
	CHECK(PickLocation({ { 0x1000, { 0.3f, 0.0f, -0.2f } } }, { 0.0f, 0.0f, 0.0f }, {}) == 0);  // origin: never
	CHECK(PickLocation(s, { NAN, 0.0f, 0.0f }, {}) == 0);
	CHECK(PickLocation({ { 0x6000, { NAN, 0.0f, 0.0f } } }, horse, {}) == 0);
	CHECK(PickLocation({}, horse, {}) == 0);
	// A wider window finds the soldier.
	CHECK(PickLocation({ s[3] }, horse, {}, { 6.0f, 1.0f }) == 0x4000);
}

static void TestTracker()
{
	const Vec3 skin{ 820.0f, -12.0f, 1180.0f };
	Tracker    t;
	CHECK(t.Observe(skin, skin, 0) == Tracker::Verdict::kNone);
	t.Offer(0x2000, 0);
	// 10 agreeing checks within 160 ms: still a candidate (kConfirmMs).
	std::uint64_t now = 0;
	for (int i = 0; i < 10; ++i, now += 16) {
		CHECK(t.Observe(skin, skin, now) == Tracker::Verdict::kCandidate);
	}
	CHECK(!t.Confirmed());
	// The skin briefly unknown: no change.
	CHECK(t.Observe(skin, std::nullopt, now) == Tracker::Verdict::kCandidate);
	while (now < 600) {
		t.Observe(skin + Vec3{ 0.2f, 0.0f, 0.0f }, skin, now);
		now += 16;
	}
	CHECK(t.Confirmed() && t.Loc() == 0x2000);
	// Galloping: object and skin move together.
	for (int i = 0; i < 60; ++i, now += 16) {
		const Vec3 p = skin + Vec3{ 0.25f * i, 0.0f, 0.0f };
		CHECK(t.Observe(p, p + Vec3{ 0.1f, 0.0f, 0.0f }, now) == Tracker::Verdict::kConfirmed);
	}
	// A jump of ours: the skin follows a few checks later -- no loss.
	t.NoteShift();
	const Vec3 jumped = skin + Vec3{ -6.0f, 0.0f, 0.0f };
	for (int i = 0; i < 5; ++i, now += 16) {
		CHECK(t.Observe(jumped, skin, now) == Tracker::Verdict::kConfirmed);
	}
	CHECK(t.Observe(jumped, jumped, now) == Tracker::Verdict::kConfirmed);
	// Without a jump of ours the skin leaving it is counted: lost after kLoseSamples checks in a row.
	int lostAt = -1;
	for (int i = 0; i < 40 && lostAt < 0; ++i, now += 16) {
		if (t.Observe(jumped, skin, now) == Tracker::Verdict::kLost) {
			lostAt = i;
		}
	}
	CHECK(lostAt == Tracker::kLoseSamples - 1);
	CHECK(!t.Confirmed() && t.Loc() == 0);
	// ... but one stray check doesn't lose it.
	t.Offer(0x2000, now);
	for (int i = 0; i < 40; ++i, now += 16) {
		t.Observe(skin, skin, now);
	}
	CHECK(t.Confirmed());
	CHECK(t.Observe(jumped, skin, now) == Tracker::Verdict::kConfirmed);
	CHECK(t.Observe(skin, skin, now) == Tracker::Verdict::kConfirmed);
	// Not a location object any more (freed, reused): lost at once.
	CHECK(t.Observe(std::nullopt, skin, now) == Tracker::Verdict::kLost);
	CHECK(t.Loc() == 0);
	// A candidate that leaves the skin even once was never the horse's (a soldier's object walking past).
	t.Offer(0x4000, now);
	t.Observe(skin, skin, now);
	CHECK(t.Observe(skin + Vec3{ 2.0f, 0.0f, 0.0f }, skin, now + 16) == Tracker::Verdict::kLost);
	// The jump's settle window is used up by disagreements, then counting starts.
	t.Offer(0x2000, now);
	for (int i = 0; i < 40; ++i, now += 16) {
		t.Observe(skin, skin, now);
	}
	t.NoteShift();
	int lost2 = -1;
	for (int i = 0; i < 60 && lost2 < 0; ++i, now += 16) {
		if (t.Observe(jumped, skin, now) == Tracker::Verdict::kLost) {
			lost2 = i;
		}
	}
	CHECK(lost2 == 10 + Tracker::kLoseSamples - 1);  // kSettleChecks, then kLoseSamples
}

static void TestShifted()
{
	const std::array<float, 4> q = logic::YawQuat(1.0f);
	const auto                 p = Shifted({ 1.0f, 2.0f, 3.0f }, q, { -6.0f, 0.7f, 2.0f }, 0.0f);
	CHECK(p && Near(p->pos, { -5.0f, 2.7f, 5.0f }));  // all three axes (on foot: the horse keeps its height relative to Snake)
	CHECK(p && p->rot == q);
	// A ridden jump: horizontal as asked, and the feet put on the centre's ground -- not the far point's altitude, nor Snake's
	// root's (a saddle above them). Downhill 4 m below the centre: up by 4; uphill: down.
	CHECK(Near(GroundedShift({ -12.0f, -2.6f, 3.0f }, { 830.0f, -16.0f, 1170.0f }, -12.0f), { -12.0f, 4.0f, 3.0f }));
	CHECK(Near(GroundedShift({ -12.0f, -6.0f, 3.0f }, { 830.0f, -7.4f, 1170.0f }, -12.0f), { -12.0f, -4.6f, 3.0f }));
	const auto g = Shifted({ 830.0f, -15.2f, 1170.0f }, q, GroundedShift({ -10.0f, 0.0f, 10.0f }, { 830.0f, -16.0f, 1170.0f }, -12.0f), 0.0f);
	CHECK(g && Near(g->pos, { 820.0f, -11.2f, 1180.0f }));  // +0x20 = the new feet + the object's own 0.8 m lift
	// Not a unit quaternion (unread, garbage): the fallback heading.
	const auto f = Shifted({ 1.0f, 2.0f, 3.0f }, { 0.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, 0.5f);
	CHECK(f && std::fabs(logic::QuatYaw(f->rot.data()) - 0.5f) < 1e-4f);
	CHECK(!Shifted({ 1.0f, 2.0f, 3.0f }, { 0.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, NAN));
	CHECK(!Shifted({ NAN, 2.0f, 3.0f }, q, { 1.0f, 0.0f, 0.0f }, 0.0f));
	CHECK(!Shifted({ 1.0f, 2.0f, 3.0f }, q, { INFINITY, 0.0f, 0.0f }, 0.0f));
}

static void TestFollowed()
{
	CHECK(Followed({ 10.0f, 0.0f, 10.0f }, { 4.1f, 0.3f, 10.0f }, { -6.0f, 0.0f, 0.0f }, 0.3f));
	CHECK(!Followed({ 10.0f, 0.0f, 10.0f }, { 10.0f, 0.0f, 10.0f }, { -6.0f, 0.0f, 0.0f }, 0.3f));  // didn't move
	CHECK(!Followed({ 10.0f, 0.0f, 10.0f }, { 4.0f, 0.0f, 11.0f }, { -6.0f, 0.0f, 0.0f }, 0.3f));
	CHECK(!Followed({ NAN, 0.0f, 10.0f }, { 4.0f, 0.0f, 10.0f }, { -6.0f, 0.0f, 0.0f }, 0.3f));
}

static void TestDecide()
{
	ActionInput in;
	CHECK(Decide(in) == Action::kLeave);  // no horse
	in.horse = true;
	CHECK(Decide(in) == Action::kLeave);  // its place unknown
	in.posValid = true;
	CHECK(Decide(in) == Action::kWhistle);  // feature off: today's way
	in.ridden = true;
	CHECK(Decide(in) == Action::kNoTreadmill);
	in.enabled = true;
	CHECK(Decide(in) == Action::kNoTreadmill);  // on, but its object not confirmed yet
	in.located = true;
	CHECK(Decide(in) == Action::kMoveRidden);
	in.posValid = false;
	CHECK(Decide(in) == Action::kMoveRidden);  // ridden: its skin under Snake may lag, the object is what moves
	in.ridden = false;
	CHECK(Decide(in) == Action::kLeave);
	in.posValid = true;
	CHECK(Decide(in) == Action::kMoveAlong);
	in.unmovable = true;
	CHECK(Decide(in) == Action::kWhistle);
	in.ridden = true;
	CHECK(Decide(in) == Action::kNoTreadmill);
	// A ridden jump left Snake behind once: no more ridden jumps, but on foot the horse is still moved along.
	in.unmovable = false;
	in.riderLeft = true;
	CHECK(Decide(in) == Action::kNoTreadmill);
	in.ridden = false;
	CHECK(Decide(in) == Action::kMoveAlong);
}

static void TestRiddenLimits()
{
	const Vec3 centre{ 820.0f, -12.0f, 1180.0f };
	CHECK(RiddenJumpLimit(6.0f) == 32.0f);
	CHECK(RiddenJumpOk(centre + Vec3{ 15.0f, 1.4f, 0.0f }, centre, 6.0f));          // a gallop's jump
	CHECK(RiddenJumpOk(centre + Vec3{ 0.0f, 9.0f, -31.9f }, centre, 6.0f));
	CHECK(!RiddenJumpOk(centre + Vec3{ 0.0f, 1.4f, -90.0f }, centre, 6.0f));        // ridden far without the treadmill
	CHECK(!RiddenJumpOk(centre + Vec3{ 30.0f, 1.4f, 30.0f }, centre, 6.0f));
	CHECK(!RiddenJumpOk({ NAN, 0.0f, 0.0f }, centre, 6.0f));

	RiddenWatch w;
	CHECK(!w.Open() && !w.Missed(100, 100000));
	w.Start(100, 10000);
	CHECK(w.Open());
	CHECK(!w.Missed(105, 10400));  // 5 frames: not yet, however long they took
	CHECK(!w.Missed(130, 10200));  // 30 fast frames: not before 300 ms either
	CHECK(w.Missed(110, 10300));
	w.Clear();  // his landing
	CHECK(!w.Missed(500, 99999));

	// Back under him: the old point moved as Snake has since.
	CHECK(Near(BackUnder({ 835.0f, -11.2f, 1180.0f }, { 835.0f, -10.6f, 1180.0f }, { 838.0f, -10.4f, 1181.0f }), { 838.0f, -11.0f, 1181.0f }));
}

// A minute of gallop at 15 m/s up a 15 % slope on the treadmill (radius 6), with Frame.cpp's and Horse.cpp's glue copied in: the
// tracker confirmed before the ride and observed every frame (the skin a frame behind its object), the decision, the jump limit,
// GroundedShift for the height, the landing taken out of the odometer (g_warpTargetY / g_settleY) and RiddenWatch.
// a_carried = the saddle carries Snake with the horse's object (a frame late) -- the ASSUMPTION the live test must confirm;
// false = it doesn't: the first jump must be undone within the watch's window and no other tried.
// This is an odometer/treadmill simulation under that assumption, not evidence that MGSV behaves so.
struct RideResult
{
	int    jumps{ 0 };
	double ridden{ 0 };          // odometer, horizontal (m)
	float  maxOut{ 0 };          // furthest Snake got from the centre (m)
	int    offFrames{ 0 };       // frames Snake was not over the horse
	float  maxGroundErr{ 0 };    // the horse's feet vs the slope after a jump (m)
	bool   alwaysConfirmed{ true };
	bool   riderLeft{ false };
	int    undoneAt{ -1 };       // frames after the jump that the horse was put back
};

static RideResult Ride(bool a_carried)
{
	const Vec3   centre{ 820.0f, -12.0f, 1180.0f };
	const Vec3   saddle{ 0.0f, 1.4f, 0.0f };
	const Vec3   lift{ 0.0f, 0.8f, 0.0f };  // the object's +0x20 over its feet
	const float  heading = 0.3f, grade = 0.15f, speed = 15.0f;
	const Vec3   dir{ std::sin(heading), 0.0f, std::cos(heading) };
	const auto   ground = [&](const Vec3& p) { return centre.y + grade * ((p.x - centre.x) * dir.x + (p.z - centre.z) * dir.z); };
	const Vec3   vel{ speed * dir.x, speed * grade, speed * dir.z };  // along the slope
	const double dt = 1.0 / 60.0;
	const auto   q = logic::YawQuat(heading);

	RideResult  r;
	Vec3        feet = centre;           // the horse's object (+0x10); its body follows it
	Vec3        root = feet + saddle;    // Snake's root
	Vec3        skinLate = feet;         // the skin as the tracker sees it: the body as MGSV moved it last frame (before our jump)
	Vec3        skinPublished = feet;
	Tracker     tracker;
	RiddenWatch watch;
	bool        riderLeft = false;
	Vec3        beforeAt, beforeRoot;
	std::uint64_t now = 0;
	// D-Horse out, standing: the scan's pick is confirmed.
	tracker.Offer(0x2000, now);
	for (int i = 0; i < 60; ++i, now += 16) {
		tracker.Observe(feet, feet, now);
	}
	CHECK(tracker.Confirmed());

	logic::Odometer odo;
	odo.Reset(root);
	double        lastWarp = -1e12;
	bool          pending = false, riderLags = false;
	Vec3          lastRoot = root;
	float         warpTargetY = 0, settleY = 0;
	int           settleUntil = 0, jumpFrame = -1;
	for (int f = 0; f < 3600; ++f) {
		now = static_cast<std::uint64_t>((f + 1) * dt * 1000.0) + 1000;
		// MGSV's frame: the horse gallops up the slope; Snake in the saddle (one frame late after a jump of ours), or -- not
		// carried -- on along his own line.
		const Vec3 step = vel * static_cast<float>(dt);
		skinLate = skinPublished;
		feet = feet + step;
		skinPublished = feet;
		root = (!a_carried || riderLags) ? root + step : feet + saddle;
		riderLags = false;
		// Frame.cpp: the landing, then RiddenWatch, then the odometer.
		Vec3 odoRoot = root;
		if (f < settleUntil) {
			odoRoot.y = settleY;
		}
		if (pending && odo.Primed() && (root - lastRoot).Length() > 0.75f) {
			settleY = odoRoot.y = warpTargetY;
			odo.Rebase(odoRoot - odo.Velocity() * static_cast<float>(dt));
			pending = false;
			settleUntil = f + 20;
			watch.Clear();
		}
		if (watch.Missed(static_cast<std::uint64_t>(f), now)) {
			// Horse.cpp RiddenMissed: back under him.
			watch.Clear();
			riderLeft = true;
			feet = BackUnder(beforeAt, beforeRoot, root) - lift;
			tracker.NoteShift();
			pending = false;
			r.undoneAt = f - jumpFrame;
		}
		odo.Update(odoRoot, dt, pending, 3.0f, 0.2f);
		lastRoot = root;
		// Horse.cpp Update: the tracker on the object's feet and the late skin.
		r.alwaysConfirmed = r.alwaysConfirmed && tracker.Observe(feet, skinLate, now) == Tracker::Verdict::kConfirmed;
		r.maxOut = std::max(r.maxOut, (root - centre).LengthXZ());
		r.offFrames += (root - saddle - feet).LengthXZ() > 0.01f;
		// Frame.cpp Hold: the decision, the treadmill, the ridden jump.
		ActionInput ai;
		ai.enabled = ai.horse = ai.posValid = ai.ridden = true;
		ai.located = tracker.Confirmed();
		ai.riderLeft = riderLeft;
		if (Decide(ai) != Action::kMoveRidden) {
			continue;  // g_unwarpable: no treadmill while he rides it
		}
		logic::TreadmillInput in;
		in.root = root;
		in.centre = centre;
		in.velocity = odo.Velocity();
		in.radius = 6.0f;
		in.allowed = !pending;
		in.nowMs = static_cast<double>(now);
		in.lastWarpMs = lastWarp;
		in.cooldownMs = 1000;
		const auto d = logic::Treadmill(in);
		if (!d.warp) {
			continue;
		}
		CHECK(RiddenJumpOk(root, centre, 6.0f));
		const Vec3 at = feet + lift;
		const auto p = Shifted(at, q, GroundedShift(d.shift, feet, d.target.y), heading);
		CHECK(p.has_value());
		if (!p) {
			break;
		}
		const Vec3 moved = p->pos - at;
		CHECK(Followed(at, p->pos, d.shift, 0.01f));
		beforeAt = at;
		beforeRoot = root;
		feet = p->pos - lift;
		tracker.NoteShift();
		r.maxGroundErr = std::max(r.maxGroundErr, std::fabs(feet.y - ground(feet)));
		warpTargetY = (root + moved).y;
		watch.Start(static_cast<std::uint64_t>(f), now);
		riderLags = true;
		pending = true;
		lastWarp = static_cast<double>(now);
		jumpFrame = f;
		++r.jumps;
	}
	const auto& o = odo.Odo();
	r.ridden = std::sqrt(o[0] * o[0] + o[2] * o[2]);
	r.riderLeft = riderLeft;
	return r;
}

static void TestRiddenGallop()
{
	const double want = 15.0 * 60.0;
	// The saddle carries Snake.
	const auto a = Ride(true);
	std::printf("ridden gallop (saddle carries Snake, ASSUMED): %d jumps, odometer %.1f m of %.1f m, furthest from the centre %.1f m, "
	            "horse off the slope by %.3f m after a jump\n",
		a.jumps, a.ridden, want, a.maxOut, a.maxGroundErr);
	CHECK(a.jumps >= 50);
	CHECK(std::fabs(a.ridden - want) < want * 0.01);  // no jump ever reached the odometer
	CHECK(a.maxOut < 16.0f);                          // 15 m/s for the 1 s cooldown past the centre at most
	CHECK(a.offFrames == a.jumps || a.offFrames == a.jumps - 1);  // only the one frame Snake lags after each jump
	CHECK(a.maxGroundErr < 1e-3f);                    // feet on the centre's ground, not the far point's 2.3 m up the slope
	CHECK(a.alwaysConfirmed);                         // the tracker survives every jump (NoteShift covers the skin's lag)
	CHECK(!a.riderLeft && a.undoneAt < 0);
	// It doesn't: one jump, undone at the end of the watch's window, the horse under him again, no other jump.
	const auto b = Ride(false);
	std::printf("ridden gallop (saddle does NOT carry Snake): %d jump, horse put back %d frames later, odometer %.1f m, %d frames off the horse\n",
		b.jumps, b.undoneAt, b.ridden, b.offFrames);
	CHECK(b.jumps == 1);
	CHECK(b.riderLeft);
	CHECK(b.undoneAt >= static_cast<int>(RiddenWatch::kFrames) && b.undoneAt <= 20);  // 300 ms at 60 fps: 18 frames
	CHECK(b.offFrames == b.undoneAt - 1);             // off the horse only until it was put back
	CHECK(b.alwaysConfirmed);
	CHECK(std::fabs(b.ridden - want) < want * 0.01);  // Snake never jumped: the odometer is his ride
}


// D-Walker led alongside a running Snake (mgsv/WalkerLogic.h): a minute of runs, turns, walks and stops at 60 fps with the treadmill
// moving both every time he passes its radius, the walker's own AI taking a step of its own each frame (toward an order ahead of
// him, as Follow.cpp's), and Elden Ring's view of it -- the odometer plus where the walker stands relative to Snake.
static void TestWalkerLead()
{
	namespace wp = walker::plan;
	const wp::LeadSettings s;
	const float            dt = 1.0f / 60.0f;
	// Not allowed / no motion / too far: nothing placed.
	{
		wp::Lead       lead;
		wp::LeadInput  in;
		in.allowed = true, in.dt = dt, in.nowMs = 1000, in.snake = { 0, 0, 0 }, in.velocity = { 0, 0, 5.0f }, in.viewYaw = 0.0f;
		in.walker = { 2.8f, 0, 0 };  // BesideOf: facing +z, -x is his right, so side -2.8 (left) is +x
		CHECK(Near(wp::LeadSpot(in, s), { 2.8f, 0, 0 }));
		in.allowed = false;
		CHECK(!lead.Update(in, s).place && !lead.Active());
		in.allowed = true, in.velocity = { 0, 0, 1.0f };
		CHECK(!lead.Update(in, s).place);  // walking slowly: MGSV's own
		in.velocity = { 0, 0, 5.0f }, in.walker = { 10.0f, 0, 0 };
		CHECK(!lead.Update(in, s).place);  // 7.2 m from its spot: MGSV brings it (acquire 6 m)
		in.walker = { 2.8f, 0, 0 }, in.dt = 0.0f;
		CHECK(!lead.Update(in, s).place);  // no frame time
		in.dt = dt;
		auto o = lead.Update(in, s);
		CHECK(o.place && o.started && lead.Active());
		CHECK(std::fabs(o.to.z - 5.0f * dt) < 0.02f && std::fabs(o.to.x - 2.8f) < 0.01f);  // carried along with him, on its spot
		// Someone else moved it 4 m (a whistle's placement, a Lua warp): lost, MGSV's again.
		in.walker = { 2.8f, 0, 4.0f };
		o = lead.Update(in, s);
		CHECK(!o.place && o.lost && o.stopped && !lead.Active());
		// Allowed no more (the player chose Search Mode, Snake mounted it): stopped.
		in.walker = { 2.8f, 0, 0 };
		CHECK(lead.Update(in, s).started);
		in.allowed = false;
		o = lead.Update(in, s);
		CHECK(!o.place && o.stopped && !o.lost);
	}
	// The minute. Snake: 12 s run, 3 s stop, 8 s walk (2 m/s: MGSV's), 3 s stop, 10 s run with two 90 degree turns, 4 s stop, 8 s run.
	struct Leg
	{
		float secs, speed, turnDeg;
	};
	const Leg plan[] = { { 12, 5.5f, 0 }, { 3, 0, 0 }, { 8, 2.0f, 0 }, { 3, 0, 0 }, { 4, 5.5f, 0 }, { 3, 5.5f, 90 }, { 3, 5.5f, -90 }, { 4, 0, 0 },
		{ 8, 5.5f, 0 }, { 3, 0, 0 } };
	wp::Lead      lead;
	const Vec3    centre{ 0, 0, 0 };
	const float   radius = 6.0f;
	Vec3          snake = centre, walker{ 2.8f, 0, -0.5f }, vel{ 0, 0, 0 };
	Vec3          odo{ 0, 0, 0 };  // Elden Ring's puppet
	float         yaw = 0.0f, walkerYaw = 0.0f, worstStep = 0.0f, worstSeen = 0.0f;
	std::uint64_t now = 1000;
	std::vector<float> running, standing;
	int           frames = 0, placed = 0, jumps = 0, lost = 0, leads = 0;
	Vec3          seenLast = odo + (walker - snake);
	for (const auto& leg : plan) {
		const int n = static_cast<int>(leg.secs * 60.0f);
		for (int i = 0; i < n; ++i, ++frames, now += 16) {
			yaw += (leg.turnDeg * 3.14159265f / 180.0f) / (leg.secs * 60.0f);
			const Vec3 want{ std::sin(yaw) * leg.speed, 0, std::cos(yaw) * leg.speed };
			vel = vel + (want - vel) * 0.2f;  // the odometer's smoothing (VelocitySmoothing 0.2)
			snake = snake + vel * dt;
			odo = odo + vel * dt;
			// MGSV's walker AI: a step toward its order ~4 m ahead of Snake on the lead's lane, stopping within 3 m of him (live).
			const Vec3 order = logic::BesideOf(snake + vel * 0.8f, yaw, 0.0f, s.side);
			const Vec3 toOrder = order - walker;
			if ((walker - snake).LengthXZ() > 3.0f && toOrder.LengthXZ() > 0.5f) {
				walker = walker + toOrder * (6.5f * dt / toOrder.LengthXZ());
			}
			// Camera ~5 m behind him (the view's yaw = his heading).
			wp::LeadInput in;
			in.allowed = true, in.dt = dt, in.nowMs = now, in.snake = snake, in.velocity = vel, in.viewYaw = yaw, in.walker = walker, in.walkerYaw = walkerYaw;
			const auto o = lead.Update(in, s);
			leads += o.started;
			lost += o.lost;
			if (o.place) {
				worstStep = std::max(worstStep, (o.to - walker).LengthXZ());
				walker = { o.to.x, walker.y, o.to.z };
				walkerYaw = o.yaw;
				++placed;
			}
			// The treadmill: past the radius, Snake and the walker back by the same shift (the walker's own object, same frame).
			if ((snake - centre).LengthXZ() > radius) {
				const Vec3 shift = centre - snake;
				snake = snake + shift;
				walker = walker + shift;
				lead.Shift(shift);
				++jumps;
			}
			const float d = (walker - snake).LengthXZ();
			(vel.LengthXZ() > 3.0f ? running : standing).push_back(d);
			// What Elden Ring draws: the puppet (odometer) + the walker relative to Snake -- never a jump.
			const Vec3 seen = odo + (walker - snake);
			worstSeen = std::max(worstSeen, (seen - seenLast).LengthXZ());
			seenLast = seen;
		}
	}
	std::sort(running.begin(), running.end());
	const float median = running[running.size() / 2], p90 = running[running.size() * 9 / 10];
	std::printf("walker lead: %d frames, %d placed, %d leads, %d lost, %d treadmill jumps; running: median %.2f m, p90 %.2f m, max %.2f m; "
	            "largest step %.3f m, largest jump as Elden Ring sees it %.3f m\n",
		frames, placed, leads, lost, jumps, median, p90, running.back(), worstStep, worstSeen);
	CHECK(jumps >= 20);                       // the treadmill ran
	CHECK(lost == 0);                         // its own jumps never count as someone else's
	CHECK(median < 3.5f && p90 < 3.5f);       // beside him while he runs
	CHECK(running.back() < 4.5f);             // even through the turns
	CHECK(worstStep <= s.maxSpeed * dt + 1e-3f);
	CHECK(worstSeen < 0.5f);                  // no frame shows it jump
	CHECK(leads >= 4 && leads <= 6);          // one per run (and the walk, faster than 1.5 m/s), none re-started mid-run
	CHECK(!lead.Active());                    // let go once he stood
	CHECK((walker - snake).LengthXZ() < 3.5f);
}

// The lead's leg gate (2026-11-03): never faster than its legs -- standing legs: not moved (idleSpeed 0); walking: at most
// legSpeed, reached at most accel m/s per second; unknown legs (-1): as before.
static void TestWalkerLegs()
{
	namespace wp = walker::plan;
	wp::LeadSettings s;
	s.legGate = true;
	const float   dt = 1.0f / 60.0f;
	wp::Lead      lead;
	wp::LeadInput in;
	in.allowed = true, in.dt = dt, in.nowMs = 1000, in.snake = { 0, 0, 0 }, in.velocity = { 0, 0, 5.0f }, in.viewYaw = 0.0f;
	in.walker = { 2.8f, 0, 0 }, in.legs = 0x05;
	auto o = lead.Update(in, s);
	CHECK(o.place && o.started && o.speed == 0.0f && !o.glide && Near(o.to, { 2.8f, 0, 0 }));  // legs idle: it stays
	float glided = 0.0f, maxSpeed = 0.0f, maxJerk = 0.0f, last = 0.0f;
	Vec3  snake{ 0, 0, 0 };
	for (int i = 0; i < 180; ++i) {  // Snake runs 3 s; its legs walk from 1 s
		snake = snake + in.velocity * dt;
		in.snake = snake, in.nowMs += 16, in.walker = { o.to.x, 0, o.to.z };
		in.legs = i < 60 ? 0x05 : 0x0A;
		const bool behind = (in.walker - wp::LeadSpot(in, s)).LengthXZ() > s.acquire;  // review 0.7.0: far behind, it may go maxSpeed
		o = lead.Update(in, s);
		if (!behind) {
			maxSpeed = std::max(maxSpeed, o.speed);
		}
		CHECK(o.place);
		if (i < 60) {
			CHECK(o.speed == 0.0f);
		}
		glided += o.glide ? dt : 0.0f;
		CHECK(o.speed <= s.maxSpeed + 1e-3f);
		maxJerk = std::max(maxJerk, (o.speed - last) / dt);
		last = o.speed;
	}
	CHECK(glided == 0.0f);
	CHECK(maxSpeed <= s.legSpeed + 1e-3f && maxJerk <= s.accel + 0.5f);
	// 2 s of walking at up to 6.5 m/s (8 m/s per s to get there) after 1 s still: it is catching up with the spot beside him
	const float gap = (Vec3{ o.to.x, 0, o.to.z } - wp::LeadSpot(in, s)).LengthXZ();
	CHECK(gap < 5.0f && gap > 0.5f);
	// Unknown legs (Follow hasn't read them): the old lead (maxSpeed alone; glides are counted only with known legs)
	wp::Lead old;
	in.legs = -1, in.walker = { 2.8f, 0, 0 }, in.snake = { 0, 0, 0 };
	o = old.Update(in, s);
	o = old.Update(in, s);
	CHECK(o.place && o.speed > 4.0f && !o.glide);
	// Review 0.7.0: a dashing Snake (~7 m/s, faster than legSpeed) with the legs standing 2.2 s at the start: far behind, its legs
	// walking, it catches up at maxSpeed (it used to fall ever further back), never faster than maxSpeed and never gliding.
	{
		wp::Lead dash;
		wp::LeadInput d = in;
		d.legs = 0x05, d.snake = { 0, 0, 0 }, d.walker = { 2.8f, 0, 0 }, d.velocity = { 0, 0, 7.0f }, d.nowMs = 1000;
		auto  od = dash.Update(d, s);
		float worst = 0.0f, fastest = 0.0f, glide2 = 0.0f, gapAt12 = 0.0f;
		Vec3  sn{ 0, 0, 0 };
		for (int i = 0; i < 12 * 60; ++i) {
			sn = sn + d.velocity * dt;
			d.snake = sn, d.nowMs += 16, d.walker = { od.to.x, 0, od.to.z };
			d.legs = i < 132 ? 0x05 : 0x0A;
			od = dash.Update(d, s);
			CHECK(od.place && !od.stopped);
			const float gap = (Vec3{ od.to.x, 0, od.to.z } - wp::LeadSpot(d, s)).LengthXZ();
			worst = std::max(worst, gap);
			fastest = std::max(fastest, od.speed);
			glide2 += od.glide ? dt : 0.0f;
			gapAt12 = gap;
		}
		CHECK(worst > s.acquire && fastest <= s.maxSpeed + 1e-3f && glide2 == 0.0f);
		CHECK(gapAt12 < s.acquire + 1.5f);  // caught up to about acquire, and holds there (6.5 <-> 9 m/s around it)
		// Its legs never walk (MGSV dropped the order): far behind for farReleaseMs, the lead lets it go (Follow's catch-up).
		wp::Lead stuck;
		d.legs = 0x05, d.snake = { 0, 0, 0 }, d.walker = { 2.8f, 0, 0 }, d.nowMs = 1000;
		auto os = stuck.Update(d, s);
		bool released = false;
		sn = { 0, 0, 0 };
		for (int i = 0; i < 10 * 60 && !released; ++i) {
			sn = sn + d.velocity * dt;
			d.snake = sn, d.nowMs += 16, d.walker = { os.to.x, 0, os.to.z };
			os = stuck.Update(d, s);
			released = os.stopped && os.farBehind;
		}
		CHECK(released && !stuck.Active());
	}
	// Gate off: the 0.6.0 glide, now measured
	s.legGate = false;
	wp::Lead glide;
	in.legs = 0x05;
	o = glide.Update(in, s);
	o = glide.Update(in, s);
	CHECK(o.place && o.speed > 4.0f && o.glide);
}

int main()
{
	TestWalkerLead();
	TestWalkerLegs();
	TestPick();
	TestTracker();
	TestShifted();
	TestFollowed();
	TestDecide();
	TestRiddenLimits();
	TestRiddenGallop();
	if (g_failures) {
		std::printf("%d failures\n", g_failures);
		return 1;
	}
	std::printf("test_guest_horse: all passed\n");
	return 0;
}
