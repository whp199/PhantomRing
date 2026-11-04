// D-Dog in Elden Ring (er/DogLogic.h, docs/research/ddog.md §3, §7): the pick (cone, range, min, marked first, bosses, held,
// fetch -> sleepers only, never a non-hostile character), ClearRun (a step, a drop, a wall), spotting (range, height, linger,
// deaths), the "Found enemy" throttle, each style's effect (non-lethal at 1 HP, the boss rule), Bark (stare set, not near Snake,
// never a boss or a sleeper). Included by tests/test_host_combat.cpp.
#include "BuddyGuard.h"
#include "DogLogic.h"

namespace dogtest
{
	// A soldier standing at (x, 0, z), chest 1.2 m up: hostile, alive, rendered unless told otherwise.
	inline dog::Candidate At(std::uint64_t a_handle, float a_x, float a_z)
	{
		dog::Candidate c;
		c.handle = a_handle;
		c.feet = { a_x, 0, a_z };
		c.aim = { a_x, 1.2f, a_z };
		c.radius = 0.4f;
		c.hostile = true;
		c.alive = true;
		return c;
	}
}

static void TestDogPick()
{
	using dog::Why;
	const dog::Rules r;                                        // 3.5-25 m, cone 10 deg, marked first, clear run
	const dog::View  view{ { 0, 1.6f, -2.0f }, { 0, 0, 1 } };  // the camera 2 m behind Snake, looking +Z
	const Vec3       snake{ 0, 0, 0 };
	const auto       at = dogtest::At;

	// Screen: each rule on its own.
	CHECK(dog::Screen(at(1, 0, 10), view, snake, false, r) == Why::kOk);
	CHECK(dog::Screen(at(1, 0, 30), view, snake, false, r) == Why::kTooFar);
	CHECK(dog::Screen(at(1, 0, 2.5f), view, snake, false, r) == Why::kTooNear);  // CQC's
	CHECK(dog::Screen(at(1, 8, 10), view, snake, false, r) == Why::kOffCentre);   // ~35 deg off
	CHECK(dog::Screen(at(1, 0, -10), view, snake, false, r) == Why::kOffCentre);  // behind
	auto friendly = at(1, 0, 10);
	friendly.hostile = false;  // a merchant, a quest character, a spirit summon
	CHECK(dog::Screen(friendly, view, snake, false, r) == Why::kNotHostile);
	auto friendlyBoss = friendly;
	friendlyBoss.boss = true;
	friendlyBoss.asleep = true;
	CHECK(dog::Screen(friendlyBoss, view, snake, true, r) == Why::kNotHostile);  // never anything else first
	auto dead = at(1, 0, 10);
	dead.alive = false;
	CHECK(dog::Screen(dead, view, snake, false, r) == Why::kDead);
	auto hidden = at(1, 0, 10);
	hidden.rendered = false;
	CHECK(dog::Screen(hidden, view, snake, false, r) == Why::kHidden);
	auto boss = at(1, 0, 10);
	boss.boss = true;
	CHECK(dog::Screen(boss, view, snake, false, r) == Why::kBoss);
	dog::Rules stagger = r;
	stagger.bossStagger = true;
	CHECK(dog::Screen(boss, view, snake, false, stagger) == Why::kOk);  // bosses = stagger: a target (one stagger)
	auto held = at(1, 0, 10);
	held.held = true;
	CHECK(dog::Screen(held, view, snake, false, r) == Why::kHeld);
	auto lifting = at(1, 0, 10);
	lifting.lifting = true;
	CHECK(dog::Screen(lifting, view, snake, false, r) == Why::kLifting);
	// Get it (fetch): sleepers only; a sleeper is fine for the attacks too.
	auto sleeper = at(1, 0, 10);
	sleeper.asleep = true;
	CHECK(dog::Screen(at(1, 0, 10), view, snake, true, r) == Why::kAwake);
	CHECK(dog::Screen(sleeper, view, snake, true, r) == Why::kOk);
	CHECK(dog::Screen(sleeper, view, snake, false, r) == Why::kOk);
	// A big body counts: a troll 3 m wide beside the axis is in the cone.
	auto troll = at(1, 4.0f, 15);
	troll.radius = 2.0f;
	CHECK(dog::Screen(troll, view, snake, false, r) == Why::kOk);
	troll.radius = 0.4f;
	CHECK(dog::Screen(troll, view, snake, false, r) == Why::kOffCentre);

	// Pick: the one nearest the centre; marked ones first; non-hostile never, even dead centre.
	std::vector<dog::Candidate> cands{ at(1, 1.0f, 10), at(2, 0.2f, 12), at(3, -1.5f, 8) };
	auto res = dog::Pick(cands, view, snake, false, r, {}, {});
	CHECK(res.index == std::optional<std::size_t>(1) && res.why == Why::kOk);
	cands[0].marked = true;
	res = dog::Pick(cands, view, snake, false, r, {}, {});
	CHECK(res.index == std::optional<std::size_t>(0));
	dog::Rules unmarked = r;
	unmarked.markedFirst = false;
	CHECK(dog::Pick(cands, view, snake, false, unmarked, {}, {}).index == std::optional<std::size_t>(1));
	cands[0].marked = false;
	std::vector<dog::Candidate> npcs{ friendly, at(5, 0.6f, 9) };
	npcs[0].feet = { 0, 0, 10 };
	npcs[0].aim = { 0, 1.2f, 10 };
	res = dog::Pick(npcs, view, snake, false, r, {}, {});
	CHECK(res.index == std::optional<std::size_t>(1));
	npcs.pop_back();
	res = dog::Pick(npcs, view, snake, false, r, {}, {});
	CHECK(!res.index && res.why == Why::kNotHostile && !res.nearest);  // only a non-hostile one around: nobody, and he isn't named
	CHECK(dog::Pick({}, view, snake, false, r, {}, {}).why == Why::kNobody);
	// A boss in the centre: refused (bosses = off), the reason reported; the next one in the cone is taken.
	std::vector<dog::Candidate> withBoss{ boss, at(6, 1.2f, 12) };
	res = dog::Pick(withBoss, view, snake, false, r, {}, {});
	CHECK(res.index == std::optional<std::size_t>(1));
	withBoss.pop_back();
	res = dog::Pick(withBoss, view, snake, false, r, {}, {});
	CHECK(!res.index && res.why == Why::kBoss && res.nearest == std::optional<std::size_t>(0));
	// Sight and the run: refused ones are skipped, the first refused one is reported; at most a_maxChecks checked.
	const dog::SightFn blind2 = [](const dog::Candidate& a_c) { return a_c.handle != 2; };
	res = dog::Pick(cands, view, snake, false, r, blind2, {});
	CHECK(res.index == std::optional<std::size_t>(0) && res.checked == 2);
	const dog::RunFn noRun = [](const dog::Candidate&) { return dog::Run::kDrop; };
	res = dog::Pick(cands, view, snake, false, r, {}, noRun);
	CHECK(!res.index && res.why == Why::kNoRun && res.run == dog::Run::kDrop && res.nearest == std::optional<std::size_t>(1) && res.checked == 3);
	res = dog::Pick(cands, view, snake, false, r, {}, noRun, 1);
	CHECK(!res.index && res.checked == 1);
	dog::Rules noClear = r;
	noClear.clearRun = false;  // clear_run = false: the run isn't checked
	CHECK(dog::Pick(cands, view, snake, false, noClear, {}, noRun).index == std::optional<std::size_t>(1));
	// The corner line for no pick: the reason for the one in view, else "look at an enemy".
	res = dog::Pick(cands, view, snake, false, r, {}, noRun);
	CHECK(dog::NoPickText(res, r) == "D-Dog can't run to him straight (a drop in the way)");
	res = dog::Pick(withBoss, view, snake, false, r, {}, {});
	CHECK(dog::NoPickText(res, r) == "D-Dog won't take on a boss");
	std::vector<dog::Candidate> near{ at(9, 0, 2.0f) };
	CHECK(dog::NoPickText(dog::Pick(near, view, snake, false, r, {}, {}), r) == "D-Dog: he is too close -- CQC him");
	std::vector<dog::Candidate> aside{ at(9, 15, 10) };  // too far off the centre to be "the one in view"
	CHECK(dog::NoPickText(dog::Pick(aside, view, snake, false, r, {}, {}), r) == "D-Dog: look at an enemy (25 m)");
	CHECK(dog::NoPickText(dog::Pick(npcs, view, snake, false, r, {}, {}), r) == "D-Dog: look at an enemy (25 m)");  // a friend in view
	CHECK(dog::NoPickText(dog::Pick(cands, view, snake, false, r, {}, {}), r) == "D-Dog: look at an enemy (25 m)");  // picked: not used
	// Fetch: the sleeper even if another is nearer the centre.
	std::vector<dog::Candidate> fetch{ at(1, 0.1f, 10), sleeper };
	fetch[1].feet = { 1.0f, 0, 10 };
	fetch[1].aim = { 1.0f, 1.2f, 10 };
	CHECK(dog::Pick(fetch, view, snake, true, r, {}, {}).index == std::optional<std::size_t>(1));
	CHECK(dog::Pick(fetch, view, snake, false, r, {}, {}).index == std::optional<std::size_t>(0));
}

static void TestDogRun()
{
	using dog::Run;
	// The samples: from the dog, every 0.5 m, stopping 0.7 m short of his feet.
	const auto pts = dog::RunPoints({ 0, 0, 0 }, { 0, 0, 10 }, 0.7f);
	CHECK(pts.size() == 20 && Near(pts.front().z, 0) && Near(pts.back().z, 9.3f) && Near(pts[1].z - pts[0].z, 9.3f / 19.0f));
	CHECK(dog::RunPoints({ 0, 0, 0 }, { 0, 0, 0.5f }, 0.7f).size() == 1);  // already beside him
	CHECK(dog::RunPoints({ 0, 0, 0 }, { NAN, 0, 1 }, 0.7f).empty());
	using G = std::optional<float>;
	const std::vector<G> flat{ 0.0f, 0.05f, 0.1f, 0.12f, 0.1f };
	CHECK(dog::ClearRun(flat, false, 0.8f) == Run::kClear);
	CHECK(dog::ClearRun(flat, true, 0.8f) == Run::kWall);
	const std::vector<G> stairs{ 0.0f, 0.3f, 0.6f, 0.9f, 1.2f };  // 0.3 m steps: fine
	CHECK(dog::ClearRun(stairs, false, 0.8f) == Run::kClear);
	const std::vector<G> ledge{ 0.0f, 0.0f, 1.2f, 1.2f };  // a 1.2 m wall to jump up
	CHECK(dog::ClearRun(ledge, false, 0.8f) == Run::kStep);
	const std::vector<G> down{ 2.0f, 2.0f, 0.5f };  // a 1.5 m drop off a ledge
	CHECK(dog::ClearRun(down, false, 0.8f) == Run::kStep);
	const std::vector<G> gap{ 0.0f, 0.0f, std::nullopt, 0.0f };  // a chasm: no ground within 4 m
	CHECK(dog::ClearRun(gap, false, 0.8f) == Run::kDrop);
	CHECK(dog::ClearRun({}, false, 0.8f) == Run::kClear);
	CHECK(std::string(dog::Name(Run::kDrop)) == "a drop" && std::string(dog::Name(dog::Why::kNotHostile)) == "not hostile");
}

static void TestDogSpotting()
{
	dog::Rules r;  // 40 m, 8 m up/down, linger 10 s
	dog::Spotter s;
	const Vec3   dogAt{ 100, 10, 100 };
	const auto   smell = [](std::uint64_t h, float x, float y, float z, bool hostile = true, bool alive = true) {
        return dog::Smelled{ h, { x, y, z }, hostile, alive, true };
	};
	// In range and height: spotted (once); too far, too high, not hostile, dead: not.
	std::vector<dog::Smelled> seen{ smell(1, 120, 10, 100), smell(2, 145, 10, 100), smell(3, 110, 20, 100), smell(4, 105, 10, 100, false),
		smell(5, 100, 10, 110, true, false) };
	auto fresh = s.Update(seen, dogAt, 1000, r);
	CHECK(fresh == std::vector<std::uint64_t>{ 1 } && s.Count(1000) == 1 && s.Spotted(1, 1000) && !s.Spotted(4, 1000));
	CHECK(s.Until(1) == 11000);
	// Again: still spotted, not fresh; the linger renews.
	fresh = s.Update(seen, dogAt, 2000, r);
	CHECK(fresh.empty() && s.Until(1) == 12000);
	// He walks out of range: lingers 10 s, then gone; back in range later: fresh again.
	seen[0].pos = { 150, 10, 100 };
	CHECK(s.Update(seen, dogAt, 3000, r).empty() && s.Spotted(1, 11999));
	s.Update(seen, dogAt, 12001, r);
	CHECK(!s.Spotted(1, 12001) && s.Count(12001) == 0);
	seen[0].pos = { 120, 10, 100 };
	CHECK(s.Update(seen, dogAt, 13000, r) == std::vector<std::uint64_t>{ 1 });
	// The dog not placed: nobody new, the spotted linger.
	seen.push_back(smell(6, 101, 10, 101));
	CHECK(s.Update(seen, std::nullopt, 14000, r).empty() && s.Spotted(1, 14000) && !s.Spotted(6, 14000));
	// He dies: forgotten at once (Marking fades his mark).
	seen[0].alive = false;
	s.Update(seen, dogAt, 15000, r);
	CHECK(!s.Spotted(1, 15000) && s.Spotted(6, 15000));
	// A character who turns friendly is dropped too.
	seen[5].hostile = false;
	s.Update(seen, dogAt, 16000, r);
	CHECK(!s.Spotted(6, 16000));

	// "Found enemy": at once, then at most every 8 s; a gain inside the quiet time waits for it, if someone is still spotted.
	dog::FoundText t;
	CHECK(t.Update(0, 0, 0, 8.0f).empty());
	CHECK(t.Update(1, 1, 1000, 8.0f) == "D-Dog: Found enemy");
	CHECK(t.Update(2, 3, 2000, 8.0f).empty());
	CHECK(t.Update(0, 3, 8999, 8.0f).empty());
	CHECK(t.Update(0, 3, 9000, 8.0f) == "D-Dog: Found enemy (3 nearby)");
	CHECK(t.Update(0, 3, 20000, 8.0f).empty());  // nobody new
	CHECK(t.Update(1, 1, 21000, 8.0f) == "D-Dog: Found enemy");
	CHECK(t.Update(1, 1, 22000, 8.0f).empty());
	CHECK(t.Update(0, 0, 30000, 8.0f).empty());  // the one waiting left the range meanwhile: nothing
}

static void TestDogEffects()
{
	using dog::Act;
	const dog::Rules     r;
	cqc::Rules           q;  // throw poise 120 lv 4, grab poise 15 lv 1, hit correction 10
	combat::HitRules     h;
	h.scaling.playerMaxHp = 1000;
	const combat::MeleeValues m;
	// Busy: held 6 s with the grab's flinch (a token 1 HP); the hold refused: the flinch only.
	auto e = dog::EffectOf(proto::kDogBusy, 300, 0, false, false, false, r, h, q, m);
	CHECK(e.act == Act::kHold && Near(e.holdSeconds, 6.0f) && e.hit.damage == 1 && e.hit.nonLethal && Near(e.hit.poise, q.grabPoise) && e.hit.correction == 10);
	e = dog::EffectOf(proto::kDogBusy, 300, 0, false, true, false, r, h, q, m);
	CHECK(e.act == Act::kFlinch && e.holdSeconds == 0.0f && e.hit.damage == 1);
	// Kill: MGSV's share x his reference HP (an ordinary one: his max HP), lethal, the knockdown.
	e = dog::EffectOf(proto::kDogKill, 300, 1.5f, false, false, false, r, h, q, m);
	CHECK(e.act == Act::kKill && e.hit.damage == 450 && !e.hit.nonLethal && e.hit.shared && Near(e.hit.poise, q.throwPoise) && e.hit.dmgLevel == q.throwDmgLevel);
	e = dog::EffectOf(proto::kDogKill, 300, 0, false, false, false, r, h, q, m);  // share unknown: 1.2 lives
	CHECK(e.hit.damage == 360 && !e.hit.shared);
	// A big one (3x the Tarnished's HP): a share of a life of his, not all of it.
	e = dog::EffectOf(proto::kDogKill, 3000, 1.0f, false, false, false, r, h, q, m);
	CHECK(e.hit.damage > 0 && e.hit.damage < 3000 && e.hit.damage == std::lround(combat::ReferenceHp(3000, h.scaling)));
	// Knock out: the sleep darts' route (a share of his resistance, the choke's absolute one as the fallback), never lethal.
	e = dog::EffectOf(proto::kDogKnockOut, 300, 0, false, false, false, r, h, q, m);
	CHECK(e.act == Act::kKnockOut && Near(e.hit.sleepShare, 1.1f) && e.hit.sleep == m.chokeSleep && e.hit.nonLethal && e.hit.damage <= 1 &&
	      Near(e.hit.poise, q.throwPoise));
	// Wound: all of an ordinary one's HP, but never below 1 (nonLethal: Combat stops it at 1 HP), with the knockdown.
	e = dog::EffectOf(proto::kDogWound, 300, 0, false, false, false, r, h, q, m);
	CHECK(e.act == Act::kWound && e.hit.damage == 300 && e.hit.nonLethal && Near(e.hit.poise, q.throwPoise));
	// Live 2026-11-04: a kill bullet on a soldier charging Snake missed -- kill and wound deal their damage through the HP route then
	// (landOrHp); the knock out (its sleep rides on the bullet) and the busy flinch don't.
	CHECK(e.hit.landOrHp && dog::EffectOf(proto::kDogKill, 300, 0, false, false, false, r, h, q, m).hit.landOrHp);
	CHECK(!dog::EffectOf(proto::kDogKnockOut, 300, 0, false, false, false, r, h, q, m).hit.landOrHp &&
	      !dog::EffectOf(proto::kDogBusy, 300, 0, false, false, false, r, h, q, m).hit.landOrHp);
	dog::Rules half = r;
	half.woundShare = 0.5f;
	CHECK(dog::EffectOf(proto::kDogWound, 300, 0, false, false, false, half, h, q, m).hit.damage == 150);
	// Fetch: nothing here.
	e = dog::EffectOf(proto::kDogFetch, 300, 0, false, false, false, r, h, q, m);
	CHECK(e.act == Act::kFetch && e.hit.damage == 0 && e.hit.poise == 0.0f);
	// An unknown style: the gentlest (busy).
	CHECK(dog::EffectOf(proto::kDogStyleUnknown, 300, 0, false, false, false, r, h, q, m).act == Act::kHold);
	// A boss: nothing at all with bosses = off, whatever the style; one stagger with stagger, then nothing while cooling.
	for (const auto s : { proto::kDogBusy, proto::kDogKill, proto::kDogKnockOut, proto::kDogWound, proto::kDogFetch }) {
		e = dog::EffectOf(s, 5000, 2.0f, true, false, false, r, h, q, m);
		CHECK(e.act == Act::kBossRefused && e.hit.damage == 0 && e.hit.poise == 0.0f && e.hit.sleepShare == 0.0f && e.holdSeconds == 0.0f);
	}
	dog::Rules st = r;
	st.bossStagger = true;
	e = dog::EffectOf(proto::kDogKill, 5000, 2.0f, true, false, false, st, h, q, m);
	CHECK(e.act == Act::kStagger && Near(e.hit.poise, q.staggerPoise) && e.hit.damage <= 1 && e.hit.nonLethal && e.holdSeconds == 0.0f);
	CHECK(dog::EffectOf(proto::kDogKill, 5000, 2.0f, true, false, true, st, h, q, m).act == Act::kNone);
	// The order's label names the style when the guest can't read it.
	CHECK(dog::StyleOfCommand(11) == proto::kDogBusy && dog::StyleOfCommand(12) == proto::kDogKill && dog::StyleOfCommand(13) == proto::kDogKnockOut &&
	      dog::StyleOfCommand(14) == proto::kDogWound && dog::StyleOfCommand(15) == proto::kDogFetch && !dog::StyleOfCommand(10) && !dog::StyleOfCommand(25));
	CHECK(std::string(dog::StyleName(proto::kDogKnockOut)) == "knock out" && std::string(dog::StyleName(0xFF)) == "unknown" &&
	      std::string(dog::PhaseName(proto::kDogConnected)) == "connected");
}

static void TestDogBark()
{
	const dog::Rules r;  // 20 m, not within 4 m of Snake
	const Vec3       chest{ 0, 0.5f, 0 };
	dog::Listener    l;
	l.eye = { 10, 1.7f, 0 };
	l.forward = { 1, 0, 0 };  // facing away: heard anyway (no cone)
	l.hostile = true;
	CHECK(dog::HearsBark(l, chest, r));
	auto far = l;
	far.eye = { 25, 1.7f, 0 };
	CHECK(!dog::HearsBark(far, chest, r));
	auto busy = l;
	busy.toSnake = 2.0f;  // fighting Snake, nearer him than the dog
	CHECK(!dog::HearsBark(busy, chest, r));
	auto nearDog = l;
	nearDog.eye = { 1.5f, 1.7f, 0 };
	nearDog.toSnake = 3.0f;  // near Snake but nearer the dog: he turns to the bark
	CHECK(dog::HearsBark(nearDog, chest, r));
	for (const auto f : { &dog::Listener::asleep, &dog::Listener::boss, &dog::Listener::held, &dog::Listener::lifting }) {
		auto x = l;
		x.*f = true;
		CHECK(!dog::HearsBark(x, chest, r));
	}
	auto friendly = l;
	friendly.hostile = false;
	CHECK(!dog::HearsBark(friendly, chest, r));
}

// The floor-near-Snake rule (er/Buddy.h FloorNearSnake): a buddy over a drop is drawn on the nearest floor toward Snake.
static void TestDogFloor()
{
	// A tower top: floor for x <= 5 (at y 30), nothing beyond (the mapped buddy at x = 8 floats).
	const auto floorAt = [](const Vec3& a_p) -> std::optional<float> { return a_p.x <= 5.0f ? std::optional<float>(30.0f) : std::nullopt; };
	const auto f = Buddy::FloorNearSnake(Vec3{ 8, 30, 0 }, Vec3{ 0, 30, 0 }, floorAt);
	CHECK(f && Near(f->y, 30.0f) && f->shift.x <= -3.0f && f->shift.x > -3.6f && Near(f->shift.y, 0) && Near(f->shift.z, 0));
	CHECK(Buddy::FloorNearSnake(Vec3{ 4, 30, 0 }, Vec3{ 0, 30, 0 }, floorAt)->shift.x == 0.0f);  // a floor right under it: no shift
	const auto none = [](const Vec3&) -> std::optional<float> { return std::nullopt; };
	CHECK(!Buddy::FloorNearSnake(Vec3{ 8, 30, 0 }, Vec3{ 0, 30, 0 }, none));
	// Review 0.8.0: a buddy far from Snake (a horse left 150 m away) is never searched -- not one ray (it was ~300 a frame).
	int        rays = 0;
	const auto count = [&](const Vec3&) -> std::optional<float> {
		++rays;
		return std::nullopt;
	};
	CHECK(!Buddy::FloorNearSnake(Vec3{ 150, 30, 0 }, Vec3{ 0, 30, 0 }, count) && rays == 0);
	CHECK(!Buddy::FloorNearSnake(Vec3{ 29, 30, 0 }, Vec3{ 0, 30, 0 }, count) && rays > 50 && rays < 62);  // within 30 m: searched
}

// Review 0.8.0: a Refused / Lost after the player's own whistle, Wait or DD (or while D-Dog isn't live) says nothing in the corner.
static void TestDogQuietEnd()
{
	CHECK(dog::QuietEnd(8, 300, true) && dog::QuietEnd(9, 2000, true) && dog::QuietEnd(16, 10, true));
	CHECK(!dog::QuietEnd(8, 3000, true));                         // long ago: the order failed on its own
	CHECK(!dog::QuietEnd(14, 100, true) && !dog::QuietEnd(10, 100, true));  // an attack order or Bark ends nothing
	CHECK(dog::QuietEnd(14, 100, false) && dog::QuietEnd(0, 99999, false));  // not live: never a failure text
}

// Review 0.8.0: a boss is the HP rule's, or Elden Ring's own (NpcParam's isSoulGetByBoss), or -- the bit unreadable -- anything bigger
// than an ordinary enemy.
static void TestDogBossRule()
{
	cqc::Rules      r;
	combat::Scaling s;
	s.playerMaxHp = 600.0f;
	// A ~1000 HP dungeon boss of a 600 HP Tarnished: under boss_hp 2000 and Bigness 0.5 -- the HP rule alone misses him.
	CHECK(!cqc::IsBoss(1000, s, r));
	CHECK(cqc::IsBoss(1000, true, s, r));                     // his NpcParam row says so
	CHECK(!cqc::IsBoss(1000, false, s, r));                   // an ordinary big enemy (the row read, no bit)
	CHECK(cqc::IsBoss(1000, std::nullopt, s, r));             // row unreadable: bigger than 1.5 x the Tarnished -> a boss
	CHECK(!cqc::IsBoss(317, std::nullopt, s, r) && !cqc::IsBoss(317, false, s, r));  // a soldier stays a soldier either way
	CHECK(cqc::IsBoss(6080, false, s, r) && cqc::IsBoss(0, false, s, r));            // HP rule (Godrick), unknown HP
}

// (0.8.0 polish) The shown pick kept through the click: 2 of 16 orders of the 0.8.0 run were refused "out of sight" the instant the
// order came although the paw had shown. The rules' pick stays published 500 ms after they stop choosing him, while he still passes
// the hard rules; the attack entry of the call menu (only then the why-no-pick line).
static void TestDogLatch()
{
	using dog::PickLatch;
	PickLatch l;
	CHECK(l.Update(0, false, 1000) == 0 && !l.Held());
	CHECK(l.Update(7, false, 1000) == 7 && !l.Held());   // picked
	CHECK(l.Update(7, false, 1250) == 7 && !l.Held());   // again: fresh at 1250
	CHECK(l.Update(0, true, 1500) == 7 && l.Held());     // the rules lost him ("out of sight") 250 ms later: kept
	CHECK(l.Update(0, true, 1700) == 7 && l.Held());     // still within 500 ms of 1250
	CHECK(l.Update(7, false, 1720) == 7 && !l.Held());   // the rules have him again: fresh, no longer held
	CHECK(l.Update(0, true, 2219) == 7 && l.Held());
	CHECK(l.Update(0, true, 2220) == 0 && !l.Held());    // 500 ms after 1720: let go
	CHECK(l.Update(7, false, 3000) == 7);
	CHECK(l.Update(9, true, 3100) == 7 && l.Held());     // someone else in the centre for a moment: the shown one stays...
	CHECK(l.Update(9, true, 3500) == 9 && !l.Held());    // ...until 500 ms are up: then the new one
	CHECK(l.Update(0, false, 3600) == 0);                // the held one no longer fits (dead, a boss, held, gone): at once
	CHECK(l.Update(4, false, 5000, 0) == 4 && l.Update(0, true, 5001, 0) == 0);  // pick_latch_ms 0: no latch at all
	// The click 300 ms after the paw went (Dog.cpp: kClickGraceMs, he still fits): he is the pick again for 500 ms.
	l.Revive(7, 6000);
	CHECK(l.Handle() == 7 && l.Held() && l.Update(0, true, 6250) == 7 && l.Update(0, true, 6500) == 0);
	l.Clear();
	CHECK(l.Handle() == 0);

	// LatchFits: every hard rule, a wider cone, never sight or the run.
	const dog::Rules r;
	const dog::View  view{ { 0, 1.6f, -2.0f }, { 0, 0, 1 } };
	const Vec3       snake{ 0, 0, 0 };
	CHECK(dog::LatchFits(dogtest::At(1, 0, 10), view, snake, false, r));
	CHECK(dog::LatchFits(dogtest::At(1, 3.0f, 10), view, snake, false, r));    // ~14 deg off: out of the 10 deg cone, kept by the latch
	CHECK(!dog::LatchFits(dogtest::At(1, 8.0f, 10), view, snake, false, r));   // ~35 deg: the player looks elsewhere
	auto dead = dogtest::At(1, 0, 10);
	dead.alive = false;
	auto friendly = dogtest::At(1, 0, 10);
	friendly.hostile = false;
	auto boss = dogtest::At(1, 0, 10);
	boss.boss = true;
	auto held = dogtest::At(1, 0, 10);
	held.held = true;
	auto lifting = dogtest::At(1, 0, 10);
	lifting.lifting = true;
	for (const auto& c : { dead, friendly, boss, held, lifting }) {
		CHECK(!dog::LatchFits(c, view, snake, false, r));
	}
	CHECK(!dog::LatchFits(dogtest::At(1, 0, 10), view, snake, true, r));  // Get it: an awake one no longer fits
	CHECK(!dog::LatchFits(dogtest::At(1, 0, 30), view, snake, false, r) && !dog::LatchFits(dogtest::At(1, 0, 2), view, snake, false, r));

	// LatchKeeps (review 0.8.0): LatchFits, and with clear_run the dog's run must still be clear -- a wall or a drop that came up
	// since the pick ends the latch (it kept him through the click up to ~1.3 s); without clear_run the run isn't asked at all.
	{
		int        asked = 0;
		const auto wall = [&](const dog::Candidate&) {
			++asked;
			return dog::Run::kWall;
		};
		const auto clear = [&](const dog::Candidate&) {
			++asked;
			return dog::Run::kClear;
		};
		CHECK(dog::LatchKeeps(dogtest::At(1, 0, 10), view, snake, false, r, clear) && asked == 1);
		CHECK(!dog::LatchKeeps(dogtest::At(1, 0, 10), view, snake, false, r, wall) && asked == 2);
		CHECK(!dog::LatchKeeps(dead, view, snake, false, r, clear) && asked == 2);  // the hard rules first: no run asked
		dog::Rules noRun = r;
		noRun.clearRun = false;
		CHECK(dog::LatchKeeps(dogtest::At(1, 0, 10), view, snake, false, noRun, wall) && asked == 2);
	}

	// The call menu's attack entry is to the right.
	CHECK(dog::PointsAtAttack(32767, 0) && dog::PointsAtAttack(26000, 12000) && dog::PointsAtAttack(20000, -12000));
	CHECK(!dog::PointsAtAttack(0, 32767) && !dog::PointsAtAttack(-32767, 0) && !dog::PointsAtAttack(12000, 0) && !dog::PointsAtAttack(20000, 20000));
}

// (0.8.0 polish) D-Dog's place and drawn body guarded (er/BuddyGuard.h StepGuard): the 0.8.0 run's cases -- MGSV's leap snapping
// 0.59 m in one frame, a one-frame 7 m blip, ~77 m for 0.33 s after a climb-up, a 5.9 m snap that stays, the leash's return out of
// view -- and what must pass untouched (a run, a long frame).
static void TestBuddyGuard()
{
	using buddyguard::Act;
	const buddyguard::Rules r;
	constexpr float         dt = 1.0f / 60.0f;
	struct Run
	{
		buddyguard::StepGuard g;
		std::uint64_t         t{ 0 };
		float                 maxDrawn{ 0 };
		std::array<int, static_cast<std::size_t>(Act::kCount)> acts{};
		Vec3 last;
		buddyguard::Step Feed(const Vec3& a_p, bool a_in = true, float a_dt = 1.0f / 60.0f, const buddyguard::Rules& a_r = buddyguard::Rules{})
		{
			t += static_cast<std::uint64_t>(a_dt * 1000.0f + 0.5f);
			const auto s = g.Update(a_p, a_in, a_in, a_dt, t, a_r);
			++acts[static_cast<std::size_t>(s.act)];
			maxDrawn = std::max(maxDrawn, s.drawn);
			last = a_p + s.offset;
			return s;
		}
	};
	// A run at 8.4 m/s (0.14 m a frame): never touched.
	{
		Run x;
		for (int i = 0; i < 120; ++i) {
			const auto s = x.Feed({ 0.14f * i, 0, 0 });
			CHECK(Length(s.offset) < 1e-4f);
		}
		CHECK(x.acts[0] == 120 && Near(x.maxDrawn, 0.14f, 1e-3f));
	}
	// MGSV's leap: 0.59 m in one frame of a 0.14 m/frame run -- spread, never a 0.5 m step, back on the raw path within 0.5 s.
	{
		Run   x;
		float p = 0;
		for (int i = 0; i < 30; ++i) {
			x.Feed({ p += 0.14f, 0, 0 });
		}
		const auto snap = x.Feed({ p += 0.59f, 0, 0 });
		CHECK(snap.act == Act::kSnap && snap.drawn < 0.3f && Near(snap.jump, 0.45f, 0.01f));
		for (int i = 0; i < 30; ++i) {
			x.Feed({ p += 0.14f, 0, 0 });
		}
		CHECK(x.maxDrawn < 0.3f && Length(x.last - Vec3{ p, 0, 0 }) < 0.02f);
	}
	// A one-frame 7 m blip while it bites (standing): held, it comes back, never drawn there.
	{
		Run x;
		for (int i = 0; i < 10; ++i) {
			x.Feed({ 5, 0, 5 });
		}
		const auto hold = x.Feed({ 12, 0, 5 });
		CHECK(hold.act == Act::kHold && Near(hold.jump, 7.0f, 1e-3f) && Length(x.last - Vec3{ 5, 0, 5 }) < 1e-4f);
		const auto back = x.Feed({ 5, 0, 5 });
		CHECK(back.act == Act::kReturned && back.heldMs == 17 && Length(x.last - Vec3{ 5, 0, 5 }) < 1e-4f && x.maxDrawn < 1e-3f);
	}
	// ~77 m for 0.33 s right after a climb-up (in view): held all along, then it comes back.
	{
		Run x;
		for (int i = 0; i < 10; ++i) {
			x.Feed({ 1, 0, 1 });
		}
		for (int i = 0; i < 20; ++i) {
			const auto s = x.Feed({ 78, 0, 1 });
			CHECK(s.act == (i == 0 ? Act::kHold : Act::kHolding) && Length(x.last - Vec3{ 1, 0, 1 }) < 1e-4f);
		}
		CHECK(x.Feed({ 1.3f, 0, 1 }).act == Act::kReturned);
		for (int i = 0; i < 20; ++i) {
			x.Feed({ 1.3f, 0, 1 });
		}
		CHECK(x.maxDrawn < 0.1f && Length(x.last - Vec3{ 1.3f, 0, 1 }) < 0.01f);
	}
	// A 5.9 m snap that stays (in view): held 100 ms (a one-frame blip would have come back), then eased in at most 0.5 m a frame.
	{
		Run x;
		for (int i = 0; i < 10; ++i) {
			x.Feed({ 0, 0, 0 });
		}
		Act first = Act::kNone;
		int frames = 0;
		for (; frames < 120 && Length(x.last - Vec3{ 5.9f, 0, 0 }) > 0.01f; ++frames) {
			const auto s = x.Feed({ 5.9f, 0, 0 });
			first = frames == 0 ? s.act : first;
		}
		CHECK(first == Act::kHold && x.acts[static_cast<std::size_t>(Act::kEase)] == 1 && x.maxDrawn <= 0.5f + 1e-4f && frames > 15 && frames < 45);
	}
	// The guest's leash (40 m, out of view at both ends): taken at once.
	{
		Run x;
		for (int i = 0; i < 10; ++i) {
			x.Feed({ 0, 0, 40 }, false);
		}
		const auto s = x.Feed({ 1, 0, -2 }, false);
		CHECK(s.act == Act::kTakenOut && Length(s.offset) < 1e-4f && Length(x.last - Vec3{ 1, 0, -2 }) < 1e-4f);
	}
	// A real far move with one end in view: held until it leaves the view, or 2 s at the most.
	{
		Run x;
		for (int i = 0; i < 10; ++i) {
			x.Feed({ 0, 0, 0 });
		}
		CHECK(x.Feed({ 30, 0, 0 }).act == Act::kHold);
		for (int i = 0; i < 40; ++i) {
			CHECK(x.Feed({ 30, 0, 0 }).act == Act::kHolding);
		}
		CHECK(x.Feed({ 30, 0, 0 }, false).act == Act::kTakenOut);  // the camera turned away
		Run y;
		for (int i = 0; i < 10; ++i) {
			y.Feed({ 0, 0, 0 });
		}
		Act last = Act::kNone;
		for (int i = 0; i < 130 && last != Act::kTakenLate; ++i) {
			last = y.Feed({ 30, 0, 0 }).act;
		}
		CHECK(last == Act::kTakenLate && y.t >= 2000);
	}
	// A long Elden Ring frame (100 ms) while it runs at 10 m/s: 1 m in that frame is no jump.
	{
		Run x;
		for (int i = 0; i < 10; ++i) {
			x.Feed({ i / 6.0f, 0, 0 });
		}
		const auto s = x.Feed({ 9 / 6.0f + 1.0f, 0, 0 }, true, 0.1f);
		CHECK(s.act != Act::kHold && Length(s.offset) < 0.25f);
	}
	// The placement's rules: no view, a real move taken after 500 ms (never eased).
	{
		Run x;
		const auto pr = buddyguard::PlacementRules();
		for (int i = 0; i < 10; ++i) {
			x.Feed({ 0, 0, 0 }, true, dt, pr);
		}
		Act last = Act::kNone;
		int n = 0;
		for (; n < 60 && last != Act::kTakenLate; ++n) {
			last = x.Feed({ 8, 0, 0 }, true, dt, pr).act;
		}
		CHECK(last == Act::kTakenLate && n >= 30 && n <= 32);
	}
	// Review 0.8.0: a 60 m jump that settles 20 m from where it was held is no return (it was "came back" within half the jump and
	// glided there at 0.5 m a frame, in view): held, then taken in view after 2 s; with both ends out of view, taken at once.
	{
		Run x;
		for (int i = 0; i < 10; ++i) {
			x.Feed({ 0, 0, 0 });
		}
		CHECK(x.Feed({ 60, 0, 0 }).act == Act::kHold);
		Act  last = Act::kNone;
		bool returned = false, eased = false;
		for (int i = 0; i < 130 && last != Act::kTakenLate; ++i) {
			last = x.Feed({ 20, 0, 0 }).act;
			returned = returned || last == Act::kReturned;
			eased = eased || last == Act::kEase;
		}
		CHECK(last == Act::kTakenLate && !returned && !eased && x.t >= 2000);
		Run y;
		for (int i = 0; i < 10; ++i) {
			y.Feed({ 0, 0, 0 });
		}
		CHECK(y.Feed({ 60, 0, 0 }).act == Act::kHold);
		CHECK(y.Feed({ 20, 0, 0 }, false).act == Act::kTakenOut);
		// Within the cap (1.5 m from the held point after a 6 m jump) it still came back.
		Run z;
		for (int i = 0; i < 10; ++i) {
			z.Feed({ 0, 0, 0 });
		}
		CHECK(z.Feed({ 6, 0, 0 }).act == Act::kHold && z.Feed({ 1.5f, 0, 0 }).act == Act::kReturned);
	}
	CHECK(std::string(buddyguard::Name(Act::kReturned)) == "jump came back");
}

// (0.8.0 polish) A waiting D-Dog on its Elden Ring spot (er/BuddyGuard.h WaitPin): live 2026-11-04, Snake ran into the courtyard's
// barricades and the waiting dog's spot moved 3 m. Only what Snake's slip explains of the dog's drift is taken back, so a guest that
// moves MGSV's dog by the slip itself ([Dog] WaitKeepsPlace) is never corrected twice.
static void TestDogWaitPin()
{
	buddyguard::WaitPin w;
	constexpr float     dt = 1.0f / 60.0f;
	const Vec3          spot{ 2, 0, 3 };
	CHECK(LengthXZ(w.Update({ -0.1f, 0, 0 }, spot, false, dt)) == 0.0f);  // not waiting: nothing
	w.Begin(spot);
	// Snake stopped by a wall 0.5 s while MGSV's Snake runs on at 6 m/s: the slip is -3 m, and the dog as mapped drifts with it.
	Vec3 slip{};
	for (int i = 0; i < 30; ++i) {
		slip.x -= 0.1f;
		w.Update({ -0.1f, 0, 0 }, spot + slip, false, dt);
	}
	CHECK(w.Waiting() && Near(w.Shift().x, 3.0f, 1e-3f) && Near(LengthXZ(spot + slip + w.Shift() - spot), 0.0f, 1e-3f));
	// The guest moves MGSV's dog by the slip itself (WaitKeepsPlace): the mapped dog is back on its spot -- nothing left to correct.
	w.Update({}, spot, false, dt);
	CHECK(w.Waiting() && LengthXZ(w.Shift()) < 1e-4f);
	// Halfway there (its correction lags a few frames): the host fills the gap.
	w.Update({}, spot + Vec3{ -1.2f, 0, 0 }, false, dt);
	CHECK(Near(w.Shift().x, 1.2f, 1e-3f));
	// Its own small moves off the slip's line pass through.
	w.Update({}, spot + slip + Vec3{ 0, 0, 0.6f }, false, dt);
	CHECK(w.Waiting() && Near(w.Shift().x, 3.0f, 1e-3f) && Near(w.Shift().z, 0.0f, 1e-3f));
	// A staged move (a climb-up: MGSV's places mean nothing): the correction stays as it was, the slip still counts.
	w.Update({ 0, 0, -1.0f }, Vec3{ 70, 0, 3 }, true, dt);
	CHECK(w.Waiting() && Near(w.Shift().x, 3.0f, 1e-3f));
	w.Update({}, Vec3{ 70, 0, 3 }, false, 0.25f);  // still within 1 s after it
	CHECK(w.Waiting() && Near(w.Shift().x, 3.0f, 1e-3f));
	slip.z -= 1.0f;  // home again, the dog mapped where the whole slip (the climb-up's metre too) left it
	for (int i = 0; i < 60; ++i) {
		w.Update({}, spot + slip, false, dt);
	}
	CHECK(w.Waiting() && Near(w.Shift().x, 3.0f, 1e-3f) && Near(w.Shift().z, 1.0f, 1e-3f));
	// MGSV ends the wait (the dog runs to Snake, off the slip's line): over 1.5 m unexplained -> eased out at 2 m/s.
	w.Update({}, spot + slip + Vec3{ 2.0f, 0, -2.0f }, false, dt);
	CHECK(!w.Waiting() && std::string(w.EndedBy()) == "the dog moves in MGSV again");
	const float before = LengthXZ(w.Shift());
	w.Update({ -0.5f, 0, 0 }, spot, false, 0.25f);  // a slip after the wait doesn't count any more
	CHECK(Near(LengthXZ(w.Shift()), before - 0.5f, 1e-3f));
	for (int i = 0; i < 200; ++i) {
		w.Update({}, spot, false, dt);
	}
	CHECK(LengthXZ(w.Shift()) == 0.0f);
	// The whistle ends it too; a slip beyond 40 m is a teleport: no correction; Elden Ring's origin moving moves the spot.
	w.Begin(spot);
	w.Update({ -2.0f, 0, 0 }, spot + Vec3{ -2.0f, 0, 0 }, false, dt);
	w.Shift(Vec3{ 32, 0, -8 });
	w.Update({}, spot + Vec3{ 30, 0, -8 }, false, dt);
	CHECK(Near(w.Shift().x, 2.0f, 1e-3f) && Near(w.Shift().z, 0.0f, 1e-3f));
	w.End();
	CHECK(!w.Waiting() && Near(w.Shift().x, 2.0f, 1e-3f));
	w.Reset();
	w.Begin(spot);
	w.Update({ -41.0f, 0, 0 }, spot, false, dt);
	CHECK(!w.Waiting() && LengthXZ(w.Shift()) == 0.0f);
	// Review 0.8.0: Snake pushed against a wall for ~7 s (the slip builds up past 40 m frame by frame): the pin lets go and its
	// correction eases out -- never dropped in one frame.
	{
		buddyguard::WaitPin p;
		p.Begin(spot);
		Vec3 sl{};
		for (int i = 0; i < 400 && p.Waiting(); ++i) {
			sl.x -= 0.11f;
			p.Update({ -0.11f, 0, 0 }, spot + sl, false, dt);
		}
		CHECK(!p.Waiting() && std::string(p.EndedBy()) == "a long push against Elden Ring's wall (the pin let go)" && LengthXZ(p.Shift()) > 39.0f);
		const float s0 = LengthXZ(p.Shift());
		const float s1 = LengthXZ(p.Update({}, spot + sl, false, dt));
		CHECK(s1 < s0 && s0 - s1 < 0.1f);  // eased (2 m/s), not gone
	}
	// Review 0.8.0: an attack order MGSV took ends the pin (Buddy::OnDogAttack -> End); the correction goes with the dog's run to its
	// target (0.75 of its step a frame), not 2 m/s -- the bite lands on him: a 4 m correction is gone within ~0.5 s of a 12 m/s run.
	{
		buddyguard::WaitPin p;
		p.Begin(spot);
		Vec3 sl{};
		for (int i = 0; i < 40; ++i) {
			sl.x -= 0.1f;
			p.Update({ -0.1f, 0, 0 }, spot + sl, false, dt);
		}
		CHECK(p.Waiting() && Near(p.Shift().x, 4.0f, 1e-3f));
		p.End();
		Vec3 dogAt = spot + sl;
		int  frames = 0;
		for (; frames < 120 && LengthXZ(p.Shift()) > 0.0f; ++frames) {
			dogAt.z += 0.2f;  // 12 m/s toward the target
			p.Update({}, dogAt, false, dt);
		}
		CHECK(frames > 20 && frames < 32);
		// Standing still after the end: 2 m/s as before.
		buddyguard::WaitPin q;
		q.Begin(spot);
		q.Update({ -1.0f, 0, 0 }, spot + Vec3{ -1.0f, 0, 0 }, false, dt);
		q.End();
		CHECK(Near(LengthXZ(q.Update({}, spot + Vec3{ -1.0f, 0, 0 }, false, 0.25f)), 0.5f, 1e-3f));
	}
	// The step guard follows Elden Ring's origin too.
	buddyguard::StepGuard g;
	for (int i = 0; i < 5; ++i) {
		g.Update({ 1, 0, 1 }, true, true, dt, 1000 + 17 * i, buddyguard::Rules{});
	}
	g.Shift({ 32, 0, 0 });
	CHECK(g.Update({ 33, 0, 1 }, true, true, dt, 1100, buddyguard::Rules{}).act == buddyguard::Act::kNone);
}

// (0.8.0 polish) bosses = stagger, offline only (never fought live): a boss is pickable, the bite is one stagger -- no damage, no hold,
// no sleep -- then nothing for 20 s on that boss, then one stagger again; with bosses = off nothing at all, never picked.
static void TestDogBossStagger()
{
	dog::Rules st;
	st.bossStagger = true;
	const dog::Rules     off;
	cqc::Rules           q;
	combat::HitRules     h;
	h.scaling.playerMaxHp = 1000;
	const combat::MeleeValues m;
	const dog::View      view{ { 0, 1.6f, -2.0f }, { 0, 0, 1 } };
	auto                 boss = dogtest::At(77, 0, 12);
	boss.boss = true;
	std::vector<dog::Candidate> one{ boss };
	CHECK(dog::Pick(one, view, {}, false, st, {}, {}).index == std::optional<std::size_t>(0));
	CHECK(!dog::Pick(one, view, {}, false, off, {}, {}).index && !dog::LatchFits(boss, view, {}, false, off) && dog::LatchFits(boss, view, {}, false, st));
	cqc::Cooldowns cool;
	std::uint64_t  now = 100000;
	for (int round = 0; round < 2; ++round) {
		for (const auto s : { proto::kDogBusy, proto::kDogKill, proto::kDogKnockOut, proto::kDogWound }) {
			const auto e = dog::EffectOf(s, 6080, 25.0f, true, true, cool.Cooling(77, now), st, h, q, m);
			if (s == proto::kDogBusy) {
				CHECK(e.act == dog::Act::kStagger && e.hit.damage <= 1 && e.hit.nonLethal && e.hit.sleepShare == 0.0f && e.holdSeconds == 0.0f &&
				      Near(e.hit.poise, q.staggerPoise));
				cool.Start(77, now, dog::kBossStaggerCooldown);
			} else {
				CHECK(e.act == dog::Act::kNone && e.hit.damage == 0);  // the next bites within 20 s: nothing
			}
			now += 4000;
		}
		now += 20000;
		cool.Expire(now);
	}
	CHECK(dog::EffectOf(proto::kDogKill, 6080, 25.0f, true, false, false, off, h, q, m).act == dog::Act::kBossRefused);
}
