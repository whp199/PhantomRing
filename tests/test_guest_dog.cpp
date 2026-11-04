// Offline tests of D-Dog's pure half (mgsv/DogLogic.cpp, docs/research/ddog.md §3.5-3.7, §9): the style byte and attack ids, the attack
// job (Refused / Ordered / Connected / Released / Lost and its timeouts), the treadmill's choice, the leash, what Elden Ring is shown
// (DisplayLimiter) and the staged dog's spot.
#include "DogLogic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace phantom;
using namespace phantom::guest::dog::plan;
using guest::logic::Vec3;

static int g_failures = 0;
#define CHECK(x)                                                    \
	do {                                                            \
		if (!(x)) {                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			++g_failures;                                           \
		}                                                           \
	} while (0)

static void TestStyle()
{
	CHECK(StyleOf(0) == proto::kDogBusy);
	CHECK(StyleOf(1) == proto::kDogFetch);
	// Live 2026-11-04 with MGSV's own CHANGE EQUIPMENT: the byte is the equipment (vars.buddyDogEquipType) and the call menu's label
	// and command follow it -- STUN 2 = "Do it (knock out enemy)" / 13, BATTLE DRESS 3 = wound / 14, KNIFE 4 = "Do it (kill enemy)" / 12.
	CHECK(StyleOf(2) == proto::kDogKnockOut);
	CHECK(StyleOf(3) == proto::kDogWound);
	CHECK(StyleOf(4) == proto::kDogKill);
	CHECK(StyleOf(5) == proto::kDogBusy && StyleOf(7) == proto::kDogBusy);  // MGSV's attack state bites for modes 5-7
	CHECK(StyleOf(8) == proto::kDogStyleUnknown && StyleOf(-1) == proto::kDogStyleUnknown);
	CHECK(AttackIdOf(proto::kDogBusy) == 144 && AttackIdOf(proto::kDogKill) == 145 && AttackIdOf(proto::kDogWound) == 147 &&
	      AttackIdOf(proto::kDogKnockOut) == 146 && AttackIdOf(proto::kDogFetch) == 0 && AttackIdOf(proto::kDogStyleUnknown) == 0);
	// The knife message's +0xA byte is the stun knife's (set for byte 2): with it 146, without it the knife 145.
	CHECK(AttackIdOfMessage(0x887890EBE04A) == 144 && AttackIdOfMessage(0x53E70021556A, true) == 146 && AttackIdOfMessage(0x53E70021556A, false) == 145 &&
	      AttackIdOfMessage(0x48E4EDE3DAED) == 147 && AttackIdOfMessage(0x1234) == 0);
	CHECK(StyleOfCommand(11) == proto::kDogBusy && StyleOfCommand(12) == proto::kDogKill && StyleOfCommand(13) == proto::kDogKnockOut &&
	      StyleOfCommand(14) == proto::kDogWound && StyleOfCommand(15) == proto::kDogFetch && !StyleOfCommand(10) && !StyleOfCommand(16));
	for (int c = 11; c <= 15; ++c) {
		CHECK(CommandOfStyle(*StyleOfCommand(c)) == c);
	}
	CHECK(!CommandOfStyle(proto::kDogStyleUnknown));
	// Each equipment's byte and the command its menu entry gives agree (live 2026-11-04).
	CHECK(StyleOf(0) == StyleOfCommand(11) && StyleOf(1) == StyleOfCommand(15) && StyleOf(2) == StyleOfCommand(13) && StyleOf(3) == StyleOfCommand(14) &&
	      StyleOf(4) == StyleOfCommand(12));
	CHECK(IsAttackCommand(11) && IsAttackCommand(15) && !IsAttackCommand(10) && !IsAttackCommand(16));
	CHECK(IsDogCommand(8) && IsDogCommand(16) && !IsDogCommand(7) && !IsDogCommand(17) && !IsDogCommand(20));
}

static JobInput In(std::uint64_t a_now)
{
	JobInput in;
	in.nowMs = a_now;
	in.targetPresent = true;
	return in;
}

static void TestJob()
{
	const JobSettings s;  // 1500 / 12000 / 10000 ms, stuck 4 + 5 s
	// No target: Refused at once (handle 0), whistled back, idle.
	{
		Job        j;
		const auto st = j.Start(0, proto::kDogBusy, 1000);
		CHECK(st.event && *st.event == proto::kDogRefused && st.whistle && !st.dedicate && !j.Busy());
	}
	// A target: Realize -> Ordered (with the order) once its soldier stands there, within 1.5 s.
	{
		Job  j;
		auto st = j.Start(0x1000, proto::kDogKill, 1000);
		CHECK(!st.event && st.dedicate && j.GetStage() == Stage::kRealize && j.Handle() == 0x1000);
		auto in = In(1500);
		CHECK(!j.Update(in, s).event);  // not ready yet
		in = In(2200);
		in.proxyReady = true;
		in.dogDistance = 15.0f;
		st = j.Update(in, s);
		CHECK(st.event && *st.event == proto::kDogOrdered && st.order && j.GetStage() == Stage::kOrdered && j.OrderedMs() == 2200);
		// running in: nearer every 0.3 s
		for (std::uint64_t t = 2500; t <= 4000; t += 300) {
			in = In(t);
			in.dogDistance = 15.0f - static_cast<float>(t - 2200) * 0.006f;
			CHECK(!j.Update(in, s).event);
		}
		// accepted -> Connected (engaged)
		in = In(4100);
		in.accepted = true;
		st = j.Update(in, s);
		CHECK(st.event && *st.event == proto::kDogConnected && j.Engaged() && j.ConnectedMs() == 4100);
		// the dog's attack state ends at once (the same bite settling): not yet Released
		in = In(4200);
		in.contactOver = true;
		CHECK(!j.Update(in, s).event);
		// later: Released, the soldier given back, whistled back
		in = In(4600);
		in.contactOver = true;
		st = j.Update(in, s);
		CHECK(st.event && *st.event == proto::kDogReleased && st.release && st.whistle && !j.Busy() && !j.Engaged());
	}
	// Realize timeout -> Refused (soldier released).
	{
		Job j;
		j.Start(0x1000, proto::kDogBusy, 1000);
		auto in = In(2400);
		CHECK(!j.Update(in, s).event);
		in = In(2500);
		const auto st = j.Update(in, s);
		CHECK(st.event && *st.event == proto::kDogRefused && st.release && !j.Busy());
	}
	// The target leaves the table while realizing -> Refused; while ordered -> Lost; while connected -> Released.
	{
		Job j;
		j.Start(0x1000, proto::kDogBusy, 0);
		auto in = In(100);
		in.targetPresent = false;
		auto st = j.Update(in, s);
		CHECK(st.event && *st.event == proto::kDogRefused);
		j.Start(0x1000, proto::kDogBusy, 0);
		in = In(100);
		in.proxyReady = true;
		j.Update(in, s);
		in = In(200);
		in.targetPresent = false;
		st = j.Update(in, s);
		CHECK(st.event && *st.event == proto::kDogLost && st.release);
		j.Start(0x1000, proto::kDogBusy, 0);
		in = In(100);
		in.proxyReady = true;
		j.Update(in, s);
		in = In(200);
		in.accepted = true;
		j.Update(in, s);
		in = In(300);
		in.targetPresent = false;  // he died (the host killed him): let go
		st = j.Update(in, s);
		CHECK(st.event && *st.event == proto::kDogReleased && st.release);
	}
	// No accept within the reach timeout -> Lost.
	{
		Job j;
		j.Start(0x1000, proto::kDogKnockOut, 0);
		auto in = In(100);
		in.proxyReady = true;
		j.Update(in, s);
		in = In(12099);
		CHECK(!j.Update(in, s).event);
		in = In(12100);
		const auto st = j.Update(in, s);
		CHECK(st.event && *st.event == proto::kDogLost && st.whistle && st.release);
	}
	// Stuck (the backstop): MGSV's dog sets off 0.7-1.5 s after an order and stalks with the knife (live), so nothing before 4 s; then
	// far and not getting nearer for 5 s -> Lost; near (leaping distance) it may stand.
	{
		Job j;
		j.Start(0x1000, proto::kDogBusy, 0);
		auto in = In(100);
		in.proxyReady = true;
		in.dogDistance = 12.0f;
		j.Update(in, s);
		in = In(4000);
		in.dogDistance = 11.8f;  // still winding up / stalking: no verdict
		CHECK(!j.Update(in, s).event);
		in = In(8900);
		in.dogDistance = 11.9f;  // not yet 5 s since the last check inside the grace with no 0.5 m gained
		CHECK(!j.Update(in, s).event);
		in = In(9200);
		in.dogDistance = 11.9f;
		const auto st = j.Update(in, s);
		CHECK(st.event && *st.event == proto::kDogLost);
		// a dog that sets off late (1.5 s) and runs is never stuck
		j.Start(0x1000, proto::kDogBusy, 0);
		in = In(100);
		in.proxyReady = true;
		in.dogDistance = 15.0f;
		j.Update(in, s);
		for (std::uint64_t t = 200; t <= 4000; t += 100) {
			in = In(t);
			in.dogDistance = t < 1600 ? 15.0f : 15.0f - static_cast<float>(t - 1600) * 0.006f;
			CHECK(!j.Update(in, s).event);
		}
		j.Start(0x1000, proto::kDogBusy, 0);
		in = In(100);
		in.proxyReady = true;
		in.dogDistance = 2.0f;
		j.Update(in, s);
		in = In(3000);
		in.dogDistance = 2.0f;
		CHECK(!j.Update(in, s).event && j.GetStage() == Stage::kOrdered);
	}
	// MGSV gave the order up (its record lets go of the soldier) before a bite -> Lost.
	{
		Job j;
		j.Start(0x1000, proto::kDogKill, 0);
		auto in = In(100);
		in.proxyReady = true;
		j.Update(in, s);
		in = In(3000);
		in.orderDroppedMs = 300;  // not yet
		CHECK(!j.Update(in, s).event);
		in = In(3200);
		in.orderDroppedMs = 500;
		const auto st = j.Update(in, s);
		CHECK(st.event && *st.event == proto::kDogLost && st.release && st.whistle);
		// a fetch: MGSV lets go of the sleeper when the balloon is fixed; the yank 3 s later is the Connected
		j.Start(0x1001, proto::kDogFetch, 0);
		in = In(100);
		in.proxyReady = true;
		j.Update(in, s);
		in = In(6000);
		in.orderDroppedMs = 3000;
		CHECK(!j.Update(in, s).event);
		in = In(6100);
		in.orderDroppedMs = 3100;
		in.fultoned = true;
		const auto f = j.Update(in, s);
		CHECK(f.event && *f.event == proto::kDogConnected);
		// live 2026-11-03: it reached the sleeper (1.7 m), fixed the balloon and ran back to Snake (12 m) before the yank: not stuck
		j.Start(0x1001, proto::kDogFetch, 0);
		in = In(100);
		in.proxyReady = true;
		in.dogDistance = 14.0f;
		j.Update(in, s);
		for (std::uint64_t t = 200; t <= 15000; t += 100) {
			in = In(t);
			in.dogDistance = t < 3000 ? 14.0f : t < 7000 ? 1.7f : 12.0f;
			in.orderDroppedMs = t >= 11000 ? t - 11000 + 1 : 0;
			CHECK(!j.Update(in, s).event);
		}
		in = In(15100);
		in.fultoned = true;
		CHECK(j.Update(in, s).event == proto::kDogConnected);
		j.Start(0x1001, proto::kDogFetch, 0);
		in = In(100);
		in.proxyReady = true;
		j.Update(in, s);
		in = In(9000);
		in.orderDroppedMs = 8000;  // no balloon came
		const auto g = j.Update(in, s);
		CHECK(g.event && *g.event == proto::kDogLost);
	}
	// The player's whistle: before the bite -> Lost; after -> Released.
	{
		Job j;
		j.Start(0x1000, proto::kDogBusy, 0);
		auto in = In(100);
		in.proxyReady = true;
		j.Update(in, s);
		in = In(200);
		in.cancel = true;
		auto st = j.Update(in, s);
		CHECK(st.event && *st.event == proto::kDogLost);
		j.Start(0x1000, proto::kDogBusy, 0);
		in = In(100);
		in.proxyReady = true;
		j.Update(in, s);
		in = In(200);
		in.accepted = true;
		j.Update(in, s);
		in = In(300);
		in.cancel = true;
		st = j.Update(in, s);
		CHECK(st.event && *st.event == proto::kDogReleased);
	}
	// Review 0.8.0: the player's Wait or DD during an order ends it without a whistle of ours (it would undo the Wait at once); his
	// whistle (8) or an unknown cancel still whistles. In every stage.
	{
		const auto cancelAt = [&](int a_stage, int a_command) {
			Job j;
			j.Start(0x1000, proto::kDogBusy, 0);
			auto in = In(100);
			if (a_stage >= 1) {
				in.proxyReady = true;
				j.Update(in, s);  // Ordered
			}
			if (a_stage >= 2) {
				in = In(200);
				in.accepted = true;
				j.Update(in, s);  // Connected
			}
			in = In(300);
			in.cancel = true;
			in.cancelCommand = a_command;
			return j.Update(in, s);
		};
		for (int stage = 0; stage <= 2; ++stage) {
			const auto wait = cancelAt(stage, kCmdWait);
			const auto dd = cancelAt(stage, kCmdTalk);
			const auto whistle = cancelAt(stage, kCmdWhistle);
			const auto unknown = cancelAt(stage, 0);
			CHECK(wait.event && wait.release && !wait.whistle);
			CHECK(dd.event && dd.release && !dd.whistle);
			CHECK(whistle.event && whistle.release && whistle.whistle);
			CHECK(unknown.event && unknown.whistle);
		}
		CHECK(PlayerPlacesDog(9) && PlayerPlacesDog(16) && !PlayerPlacesDog(8) && !PlayerPlacesDog(10) && !PlayerPlacesDog(12));
	}
	// An order with no pick while the dog waits (MGSV never got it): Refused, and the dog keeps waiting -- no whistle.
	{
		Job        j;
		const auto st = j.Start(0, proto::kDogWound, 1000, true);
		CHECK(st.event && *st.event == proto::kDogRefused && !st.whistle && !j.Busy());
		const auto st2 = j.Start(0, proto::kDogWound, 1000, false);
		CHECK(st2.whistle);
	}
	// Busy max -> Released; a fetch's balloon -> Connected.
	{
		Job j;
		j.Start(0x1000, proto::kDogBusy, 0);
		auto in = In(100);
		in.proxyReady = true;
		j.Update(in, s);
		in = In(200);
		in.accepted = true;
		j.Update(in, s);
		in = In(10199);
		CHECK(!j.Update(in, s).event);
		in = In(10200);
		const auto st = j.Update(in, s);
		CHECK(st.event && *st.event == proto::kDogReleased);
		j.Start(0x1001, proto::kDogFetch, 0);
		in = In(100);
		in.proxyReady = true;
		in.dogDistance = 14.0f;
		j.Update(in, s);
		// it arrives and stands 3-4 m from the sleeper while MGSV's balloon goes up: never stuck, and past the attack's reach time
		for (std::uint64_t t = 1000; t <= 15000; t += 500) {
			in = In(t);
			in.dogDistance = t < 4000 ? 14.0f - static_cast<float>(t) * 0.0025f : 3.5f;
			CHECK(!j.Update(in, s).event);
		}
		in = In(16000);
		in.fultoned = true;
		const auto f = j.Update(in, s);
		CHECK(f.event && *f.event == proto::kDogConnected && j.Style() == proto::kDogFetch);
	}
	// Abort by stage.
	{
		Job j;
		CHECK(!j.Abort("x").event);
		j.Start(0x1000, proto::kDogBusy, 0);
		CHECK(j.Abort("x").event == proto::kDogRefused);
	}
}

static void TestTreadmill()
{
	TreadmillInput in;
	in.enabled = in.located = true;
	in.distance = 10.0f;
	CHECK(MoveAlong(in));
	in.distance = 45.0f;
	CHECK(!MoveAlong(in));  // a free dog that far is left to MGSV
	in.waiting = true;
	CHECK(MoveAlong(in));  // Wait keeps its place
	in.distance = 170.0f;
	CHECK(!MoveAlong(in));
	in.waiting = false;
	in.engaged = true;
	in.distance = 100.0f;
	CHECK(MoveAlong(in));  // on an order: always moved with its prey
	in.located = false;
	CHECK(!MoveAlong(in));
	in.located = true;
	in.enabled = false;
	CHECK(!MoveAlong(in));
}

static void TestLeash()
{
	// Snake at the origin facing +z, the camera 3.5 m behind him.
	const Vec3 snake{ 0.0f, 0.0f, 0.0f };
	const Vec3 cam{ 0.0f, 1.5f, -3.5f };
	const auto view = ViewOf(cam, snake);
	CHECK(view.valid && std::fabs(view.dir.z - 1.0f) < 1e-4f);
	CHECK(InView(view, { 0.0f, 0.0f, 20.0f }));    // ahead
	CHECK(!InView(view, { 0.0f, 0.0f, -30.0f }));  // behind the camera
	CHECK(!InView(view, { 40.0f, 0.0f, 0.0f }));   // far to the side
	// A dog 35 m behind the camera: put out of view, nearer to Snake.
	const auto spot = LeashSpot(snake, { 0.0f, 0.0f, 4.0f }, view, { 0.0f, 0.0f, -35.0f }, 30.0f);
	CHECK(spot.has_value());
	if (spot) {
		CHECK(!InView(view, *spot));
		CHECK((*spot - snake).LengthXZ() < 10.0f);
	}
	// In view, or near enough: left alone.
	CHECK(!LeashSpot(snake, {}, view, { 0.0f, 0.0f, 40.0f }, 30.0f));
	CHECK(!LeashSpot(snake, {}, view, { 0.0f, 0.0f, -20.0f }, 30.0f));
	CHECK(!LeashSpot(snake, {}, ViewOf(snake, snake), { 0.0f, 0.0f, -40.0f }, 30.0f));  // no view: nothing moved
}

// What Elden Ring is shown (0.8.0 polish): never faster than the dog runs; MGSV's lunge (0.56 m in one frame live) spread over frames,
// its every-other-frame running steps evened out, a warp of MGSV's taken at once, our own placements (reset) at once.
static void TestDisplay()
{
	DisplayLimiter  l;
	DisplaySettings s;  // 14 m/s, 15 m
	const float     dt = 1.0f / 55.0f;
	CHECK((l.Step({ 1.0f, 0.0f, 2.0f }, dt, false, s) - Vec3{ 1.0f, 0.0f, 2.0f }).Length() < 1e-6f);  // the first: as it is
	// The lunge: 0.56 m in one frame -> at most 14/55 = 0.255 m a frame, caught up within three frames, never more than that per frame.
	Vec3  real{ 1.56f, 0.3f, 2.0f };
	Vec3  last{ 1.0f, 0.0f, 2.0f };
	float most = 0.0f;
	for (int i = 0; i < 4; ++i) {
		const Vec3 shown = l.Step(real, dt, false, s);
		most = std::max(most, (shown - last).LengthXZ());
		CHECK(std::fabs(shown.y - real.y) < 1e-6f);  // height as MGSV has it (Elden Ring's ground decides)
		last = shown;
	}
	CHECK(most <= 14.0f * dt + 1e-4f && most > 0.2f);
	CHECK((last - real).LengthXZ() < 1e-5f && l.Offset().LengthXZ() < 1e-5f);
	CHECK(l.Limited() >= 2);
	// A dog running at 11.6 m/s whose place MGSV updates every other frame (0.42 m, then nothing): shown in steps no bigger than
	// 0.255 m, never more than one update behind.
	DisplayLimiter run;
	Vec3           r{ 0.0f, 0.0f, 0.0f };
	run.Step(r, dt, false, s);
	Vec3  prev = r;
	float big = 0.0f, lag = 0.0f;
	for (int i = 1; i <= 60; ++i) {
		if (i % 2) {
			r.x += 0.42f;
		}
		const Vec3 shown = run.Step(r, dt, false, s);
		big = std::max(big, (shown - prev).LengthXZ());
		lag = std::max(lag, (r - shown).LengthXZ());
		prev = shown;
	}
	CHECK(big <= 14.0f * dt + 1e-4f);
	CHECK(lag < 0.25f);
	// A one-frame blip (the 0.8.0 run's 6-8 m in its soldier's double move) never shows as one.
	DisplayLimiter b;
	b.Step({ 0.0f, 0.0f, 0.0f }, dt, false, s);
	const Vec3 blip = b.Step({ 7.0f, 0.0f, 0.0f }, dt, false, s);
	const Vec3 back = b.Step({ 0.0f, 0.0f, 0.0f }, dt, false, s);
	CHECK(blip.LengthXZ() <= 14.0f * dt + 1e-4f && back.LengthXZ() < 1e-5f);
	// MGSV's own warp of its dog (over 15 m), and our own placements: at once.
	CHECK((b.Step({ 40.0f, 0.0f, 0.0f }, dt, false, s) - Vec3{ 40.0f, 0.0f, 0.0f }).LengthXZ() < 1e-5f && b.Teleports() == 1);
	CHECK((b.Step({ 45.0f, 0.0f, 0.0f }, dt, true, s) - Vec3{ 45.0f, 0.0f, 0.0f }).LengthXZ() < 1e-5f);
	CHECK((b.Step({ 45.0f, 0.0f, 3.0f }, 0.0f, false, s) - Vec3{ 45.0f, 0.0f, 3.0f }).LengthXZ() < 1e-5f);  // no time: as it is
}

// After a Wait (review 0.8.0): the shown offset of a dog ordered from Wait goes with its way to the soldier, so the leap lands on him
// (at a fixed 4 m/s, 17 m of slip were still ~13 m when it got there); held: none; no soldier yet: it holds; a whistle: with the way to
// Snake (+0.5 m/s, so none is left behind a dog that follows a few metres off); no dog place: 4 m/s.
static void TestWaitFade()
{
	const float dt = 1.0f / 60.0f;
	WaitFade    f;
	const Vec3  off{ 0.0f, 0.0f, 12.0f };
	const Vec3  soldier{ 20.0f, 0.0f, 0.0f };
	Vec3        dog{ 0.0f, 0.0f, 0.0f };
	// The order is in, its soldier not yet there: the dog still sits, the offset holds.
	CHECK((f.Step(off, dog, { WaitFade::kNone, {} }, dt) - off).LengthXZ() < 1e-5f);
	// The soldier is there: the dog runs 19 m to arrive (1 m from him) in ~1.6 s at 12 m/s; the offset shrinks with the way, never back up.
	Vec3  o = off;
	float lastLen = o.LengthXZ();
	bool  monotonic = true;
	for (int i = 0; i < 200 && (dog - soldier).LengthXZ() > 1.0f; ++i) {
		dog.x += 0.2f;
		o = f.Step(o, dog, { WaitFade::kSoldier, soldier }, dt);
		monotonic = monotonic && o.LengthXZ() <= lastLen + 1e-5f;
		lastLen = o.LengthXZ();
	}
	CHECK(monotonic && o.LengthXZ() < 0.05f);  // at the leap: on him
	// Held: no offset at all.
	CHECK(f.Step({ 0.0f, 0.0f, 0.3f }, dog, { WaitFade::kHolding, soldier }, dt).LengthXZ() < 1e-6f);
	// Half-way and back up (stalking around him): the offset doesn't come back.
	WaitFade   g;
	Vec3       d2{ 0.0f, 0.0f, 0.0f };
	Vec3       o2 = g.Step(off, d2, { WaitFade::kSoldier, soldier }, dt);
	d2.x = 10.0f;
	o2 = g.Step(o2, d2, { WaitFade::kSoldier, soldier }, dt);
	const float half = o2.LengthXZ();
	CHECK(half < 7.0f && half > 5.0f);
	d2.x = 2.0f;
	CHECK(g.Step(o2, d2, { WaitFade::kSoldier, soldier }, dt).LengthXZ() <= half + 1e-5f);
	// A whistle: the dog stops 4 m behind Snake (never within 3 m): the slow 0.5 m/s still takes the rest within seconds.
	WaitFade   w;
	const Vec3 snake{ 0.0f, 0.0f, 30.0f };
	Vec3       d3{ 0.0f, 0.0f, 0.0f };
	Vec3       o3 = w.Step({ 2.0f, 0.0f, 0.0f }, d3, { WaitFade::kSnake, snake }, dt);
	d3.z = 26.0f;
	for (int i = 0; i < 60 * 5; ++i) {
		o3 = w.Step(o3, d3, { WaitFade::kSnake, snake }, dt);
	}
	CHECK(o3.LengthXZ() < 1e-4f);
	// No dog place, or the dog already at its goal: 4 m/s by the clock.
	WaitFade t;
	CHECK(std::fabs(t.Step({ 4.0f, 0.0f, 0.0f }, std::nullopt, { WaitFade::kSnake, snake }, 0.25f).LengthXZ() - 3.6f) < 1e-4f);
	WaitFade a;
	CHECK(std::fabs(a.Step({ 4.0f, 0.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 29.0f }, { WaitFade::kSnake, snake }, 0.25f).LengthXZ() - 3.6f) < 1e-4f);
}

// Snake staged at MGSV's ladder or ledge: the dog is put 2 m behind him there (his back), and he is at the site once 3 m from home.
static void TestStaged()
{
	const Vec3 site{ 854.15f, -12.0f, 1243.76f };
	const Vec3 spot = StagedSpot(site, 0.0f);  // facing +Z
	CHECK(std::fabs(spot.z - (site.z - 2.0f)) < 1e-4f && std::fabs(spot.x - site.x) < 1e-4f);
	const float yaw = 1.5707963f;  // facing +X: behind is -X
	CHECK(std::fabs(StagedSpot(site, yaw, 2.5f).x - (site.x - 2.5f)) < 1e-3f);
	const Vec3 home{ 820.6f, -11.95f, 1179.1f };
	CHECK(AtSite(site, home) && !AtSite(home + Vec3{ 1.0f, 0.0f, 2.0f }, home) && AtSite(home + Vec3{ 3.5f, 0.0f, 0.0f }, home));
}

// MGSV's markers while the dog carries out an order (live probe: green (0.4, 0.9, 0.7) the dog, red (1.0, 0.4, 0.3) its soldier, each
// also at 0.3x), and the HUD around them that must stay (white-cyan, its dark shades).
static void TestMarkerColor()
{
	CHECK(IsMgsvMarkerColor(0.4f, 0.9f, 0.7f) && IsMgsvMarkerColor(0.12f, 0.27f, 0.21f));
	CHECK(IsMgsvMarkerColor(1.0f, 0.4f, 0.3f) && IsMgsvMarkerColor(0.3f, 0.12f, 0.09f));
	CHECK(!IsMgsvMarkerColor(0.95f, 1.0f, 1.0f) && !IsMgsvMarkerColor(0.902f, 1.0f, 1.0f) && !IsMgsvMarkerColor(0.0598f, 0.12f, 0.12f));
	CHECK(!IsMgsvMarkerColor(0.161f, 0.2f, 0.2f) && !IsMgsvMarkerColor(0.475f, 0.95f, 0.95f) && !IsMgsvMarkerColor(0.855f, 1.0f, 1.0f));
	CHECK(!IsMgsvMarkerColor(0.0f, 0.0f, 0.0f) && !IsMgsvMarkerColor(1.0f, 1.0f, 1.0f) && !IsMgsvMarkerColor(1.0f, 0.0f, 0.0f));
}

int main()
{
	TestMarkerColor();
	TestStyle();
	TestJob();
	TestTreadmill();
	TestLeash();
	TestDisplay();
	TestWaitFade();
	TestStaged();
	if (g_failures) {
		std::printf("%d failures\n", g_failures);
		return 1;
	}
	std::printf("test_guest_dog: all passed\n");
	return 0;
}
