// Offline tests of the D-Horse treadmill's pure half (mgsv/HorseLogic.cpp, docs/GUEST.md 8.1a): picking the horse's location
// object near its skin, confirming and losing it, the setter's arguments for a jump, the treadmill's choice, and a ridden gallop
// through the treadmill with the frame glue copied in (the odometer -- what Elden Ring sees -- stays continuous, the tracker
// confirmed, the horse on the ground under Snake), under the assumption that the saddle carries him, and the fallback when it doesn't.
#include "HorseLogic.h"
#include "Logic.h"

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

int main()
{
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
