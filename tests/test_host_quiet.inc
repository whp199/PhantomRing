// Quiet in Elden Ring (er/QuietLogic.h, docs/research/quiet.md §3.2-3.8, §7): her post (the footprint, the height cap, the drawable
// rule, no hostile within 15 m, sight to Snake and to a hostile from the one sight point, the score with no bonus past the drawable
// bound, 20 % hysteresis, the forward half first, the search triggers, on the move), the cloak fade's sequence, the far-ground rule,
// the pick (the cone, the binoculars, never a boss / a non-hostile / the dead / one CQC holds, never a sleeper with the Guilty
// Butterfly, marked first, and never a target the shot's own ray refuses from the same post), spotting, each rifle's effect, the
// texts, a world shift; the study's post surveys recounted with the design's rule (tests/fixtures/quiet/perch_*.json) and the raw rays
// recorded live (quiet_rays_*.json) replayed. Included by tests/test_host_combat.cpp.
#include "QuietLogic.h"

#include <random>

namespace quiettest
{
	using namespace phantom::er::quiet;

	// Flat ground at y = 0 everywhere, plus boxes (walls) that stop world rays and stand on the ground filter.
	struct World
	{
		struct Box
		{
			Vec3 lo, hi;
		};
		std::vector<Box> boxes;
		float            groundY{ 0 };
		int              rays{ 0 };

		static bool Slab(float a_o, float a_d, float a_lo, float a_hi, float& a_t0, float& a_t1)
		{
			if (std::fabs(a_d) < 1e-9f) {
				return a_o >= a_lo && a_o <= a_hi;
			}
			float t0 = (a_lo - a_o) / a_d, t1 = (a_hi - a_o) / a_d;
			if (t0 > t1) {
				std::swap(t0, t1);
			}
			a_t0 = std::max(a_t0, t0);
			a_t1 = std::min(a_t1, t1);
			return a_t0 <= a_t1;
		}

		std::optional<Hit> operator()(const Vec3& a_f, const Vec3& a_d, Ray)
		{
			++rays;
			float best = 2.0f;
			Vec3  n{ 0, 1, 0 };
			for (const auto& b : boxes) {
				float t0 = 0, t1 = 1;
				if (Slab(a_f.x, a_d.x, b.lo.x, b.hi.x, t0, t1) && Slab(a_f.y, a_d.y, b.lo.y, b.hi.y, t0, t1) && Slab(a_f.z, a_d.z, b.lo.z, b.hi.z, t0, t1) &&
					t0 < best) {
					best = t0;
				}
			}
			if (a_d.y < 0 && a_f.y >= groundY && a_f.y + a_d.y <= groundY) {
				const float t = (groundY - a_f.y) / a_d.y;
				if (t < best) {
					best = t;
				}
			}
			if (best > 1.0f) {
				return std::nullopt;
			}
			return Hit{ a_f + a_d * best, n };
		}
	};

	inline PostScene Scene(std::vector<Foe> a_foes = {})
	{
		PostScene s;
		s.snake = { 0, 0, 0 };
		s.snakeChest = { 0, 1.25f, 0 };
		s.camPos = { 0, 1.6f, -5.0f };
		s.camFwd = { 0, 0, 1 };
		s.fovY = 0.8f;
		s.hostiles = std::move(a_foes);
		return s;
	}

	inline Foe At(std::uint64_t a_h, float a_x, float a_z, float a_y = 0)
	{
		return { a_h, { a_x, a_y, a_z }, { a_x, a_y + 1.2f, a_z } };
	}

	inline Target T(std::uint64_t a_h, float a_x, float a_z)
	{
		Target t;
		t.handle = a_h;
		t.feet = { a_x, 0, a_z };
		t.chest = { a_x, 1.2f, a_z };
		t.radius = 0.4f;
		t.hostile = true;
		t.alive = true;
		return t;
	}
}

static void TestQuietPostRules()
{
	using namespace quiettest;
	const Rules r;
	quiettest::World world;
	auto             ray = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) { return world(a_f, a_d, a_k); };
	auto             scene = quiettest::Scene({ quiettest::At(1, 0, 40) });  // one hostile 40 m ahead
	// A post 35 m to the right on flat ground: standable, sees Snake and him, scored.
	Post p;
	p.mode = Mode::kCover;
	p.ring = 35;
	p.feet = { 35, 0, 0 };
	auto e = EvaluatePost(p, scene, ray, r);
	CHECK(e.why == PostWhy::kOk && e.seen == 1 && Near(e.height, 0.0f));
	CHECK(Near(e.score, PostScore(Mode::kCover, 1, 35.0f, 0.0f, 0.8f, r)));
	// She faces the hostiles she covers.
	CHECK(std::fabs(WrapPi(e.heading - Heading(Vec3{ -35, 0, 40 }))) < 0.01f);

	// The drawable rule (MGSV's body on its apron, the camera centring the post): 25 m up at 35 m fails, 10 m up passes.
	CHECK(!(25.0f <= DrawableBound(35.0f, 0.8f)) && 10.0f <= DrawableBound(35.0f, 0.8f));
	quiettest::World tower = world;
	tower.boxes.push_back({ { 34, -1, -2 }, { 36, 25, 2 } });  // a 25 m tower top at 35 m
	auto onTower = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) { return tower(a_f, a_d, a_k); };
	CHECK(EvaluatePost(p, scene, onTower, r).why == PostWhy::kNotDrawable);
	Rules noDraw = r;
	noDraw.drawable = false;
	const auto high = EvaluatePost(p, scene, onTower, noDraw);
	CHECK(high.why == PostWhy::kOk && Near(high.height, 25.0f));
	// ... and its height scores no more than the drawable bound would allow (the draft's x0.2 up to 15 m steered her to exactly these).
	CHECK(PostScore(Mode::kCover, 1, 35.0f, 25.0f, 0.8f, r) == PostScore(Mode::kCover, 1, 35.0f, DrawableBound(35.0f, 0.8f), 0.8f, r));
	quiettest::World tooHigh = world;
	tooHigh.boxes.push_back({ { 34, -1, -2 }, { 36, 30, 2 } });
	auto onTooHigh = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) { return tooHigh(a_f, a_d, a_k); };
	CHECK(EvaluatePost(p, scene, onTooHigh, noDraw).why == PostWhy::kTooHigh);  // the 25 m cap (a castle wall top 59.6 m up, Gatefront)
	quiettest::World pit = world;
	pit.groundY = -5;  // 5 m under Snake
	auto inPit = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) { return pit(a_f, a_d, a_k); };
	CHECK(EvaluatePost(p, scene, inPit, r).why == PostWhy::kTooLow);
	// The footprint: a 0.8 m-wide cornice 8 m up passes a single down-ray but not the 4 rays at +-0.5 m (The First Step's chapel).
	quiettest::World ledge = world;
	ledge.boxes.push_back({ { 34.6f, -1, -10 }, { 35.4f, 8, 10 } });
	auto onLedge = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) { return ledge(a_f, a_d, a_k); };
	CHECK(EvaluatePost(p, scene, onLedge, r).why == PostWhy::kFootprint);
	// No headroom (under a balcony).
	quiettest::World roof = world;
	roof.boxes.push_back({ { 30, 1.2f, -5 }, { 40, 1.5f, 5 } });
	auto underRoof = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) -> std::optional<Hit> {
		if (a_k == Ray::kGround) {
			return world(a_f, a_d, a_k);  // the floor under it, not its top
		}
		return roof(a_f, a_d, a_k);
	};
	CHECK(EvaluatePost(p, scene, underRoof, r).why == PostWhy::kHeadroom);
	// No hostile within 15 m of her, no character within 3 m.
	auto near = quiettest::Scene({ quiettest::At(1, 30, 5) });
	CHECK(EvaluatePost(p, near, ray, r).why == PostWhy::kEnemyNear);
	auto crowd = scene;
	crowd.characters.push_back({ 36, 0, 1 });
	CHECK(EvaluatePost(p, crowd, ray, r).why == PostWhy::kCharNear);
	// No line from her sight point to Snake (a wall between), or to any hostile.
	quiettest::World wall = world;
	wall.boxes.push_back({ { 17, -1, -1 }, { 18, 5, 1 } });
	auto behindWall = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) { return wall(a_f, a_d, a_k); };
	CHECK(EvaluatePost(p, scene, behindWall, r).why == PostWhy::kNoSnake);
	quiettest::World hide = world;
	hide.boxes.push_back({ { -2, -1, 37 }, { 2, 5, 38 } });  // a wall in front of the hostile
	auto hidden = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) { return hide(a_f, a_d, a_k); };
	const auto blind = EvaluatePost(p, scene, hidden, r);
	CHECK(blind.why == PostWhy::kNoEnemy || blind.why == PostWhy::kNoSnake);
	// No ground at all (over a chasm).
	auto chasm = [&](const Vec3&, const Vec3&, Ray) -> std::optional<Hit> { return std::nullopt; };
	CHECK(EvaluatePost(p, scene, chasm, r).why == PostWhy::kNoGround);
	// Near mode: calm, sight to the camera's aim point instead of a hostile.
	auto calm = quiettest::Scene();
	calm.aimPoint = { 0, 1.0f, 30.0f };
	Post np;
	np.mode = Mode::kNear;
	np.ring = 12;
	np.feet = { 12, 0, 0 };
	const auto ne = EvaluatePost(np, calm, ray, r);
	CHECK(ne.why == PostWhy::kOk && ne.seen == 0);
	CHECK(std::fabs(WrapPi(ne.heading - Heading(calm.camFwd))) < 0.01f);  // along the view

	// 20 % hysteresis; a change of mode always moves her.
	Post a = e, b = e;
	b.score = a.score + 0.1f * std::max(1.0f, std::fabs(a.score));
	CHECK(!Better(b, a, r));
	b.score = a.score + 0.3f * std::max(1.0f, std::fabs(a.score));
	CHECK(Better(b, a, r));
	b.mode = Mode::kNear;
	b.score = -100;
	CHECK(Better(b, a, r));

	// The candidates: the camera's forward half first, nearest the preferred ring first.
	const auto cands = PostCandidates(scene, Mode::kCover, r);
	CHECK(cands.size() == 4u * 24u && cands.front().forward && Near(cands.front().ring, 35.0f) && !cands.back().forward);
	CHECK(cands.front().feet.z > 0);  // straight ahead of the camera first
	// The search: on flat ground with a hostile ahead, the forward half alone (it passed there), the best kept; rays spread over frames.
	PostSearch search;
	search.Start(scene, Mode::kCover, r);
	int frames = 0;
	while (!search.Step(r.raysPerFrame, ray)) {
		++frames;
	}
	CHECK(search.Best() && search.Best()->forward && search.Evaluated() <= 48 && frames > 3);
	CHECK(search.Best() && search.Best()->seen == 1 && Near(LengthXZ(search.Best()->feet), 35.0f));
	// Nothing passes anywhere (a corridor: no ground): no post, every candidate refused, logged by the game side.
	PostSearch none;
	none.Start(scene, Mode::kCover, r);
	while (!none.Step(30, chasm)) {
	}
	CHECK(!none.Best() && none.Count(PostWhy::kNoGround) == 96 && none.Evaluated() == 96);

	// A 32 m world shift moves what a search in flight keeps.
	PostSearch moving;
	moving.Start(scene, Mode::kCover, r);
	moving.Step(5, ray);
	moving.Shift({ 32, 0, -8 });
	CHECK(Near(moving.Scene().snake.x, 32.0f) && Near(moving.Scene().hostiles[0].feet.z, 32.0f));
}

static void TestQuietModes()
{
	using namespace quiettest;
	const Rules r;
	// On the move: over 3 m/s for 2 s; stopped again under 1 m/s for 1.5 s, or a hostile within 60 m.
	MoveWatch m;
	Mode      mode = Mode::kNone;
	for (int i = 0; i < 60; ++i) {
		mode = m.Update(5.0f, 1.0f / 60.0f, false, r);
	}
	CHECK(mode == Mode::kNear && !m.Moving());  // 1 s fast: not yet
	for (int i = 0; i < 61; ++i) {
		mode = m.Update(5.0f, 1.0f / 60.0f, false, r);
	}
	CHECK(mode == Mode::kMoving);
	for (int i = 0; i < 60; ++i) {
		mode = m.Update(0.5f, 1.0f / 60.0f, false, r);
	}
	CHECK(mode == Mode::kMoving);  // 1 s slow: still on the move
	for (int i = 0; i < 31; ++i) {
		mode = m.Update(0.5f, 1.0f / 60.0f, false, r);
	}
	CHECK(mode == Mode::kNear);
	for (int i = 0; i < 130; ++i) {
		mode = m.Update(5.0f, 1.0f / 60.0f, false, r);
	}
	CHECK(mode == Mode::kMoving && m.Update(5.0f, 1.0f / 60.0f, true, r) == Mode::kCover);  // a hostile near: she posts

	// The triggers.
	Need n;
	n.mode = Mode::kCover;
	CHECK(NeedSearch(n, r) != nullptr);  // no post
	n.havePost = true;
	n.postMode = Mode::kCover;
	CHECK(NeedSearch(n, r) == nullptr);
	n.snakeFromCentre = 26;
	CHECK(NeedSearch(n, r) != nullptr);
	n.snakeFromCentre = 0;
	n.postMode = Mode::kNear;
	CHECK(NeedSearch(n, r) != nullptr);  // hostiles came: cover
	n.postMode = Mode::kCover;
	n.lostSnake = true;
	CHECK(NeedSearch(n, r) != nullptr);
	n.lostSnake = false;
	n.threatNear = true;
	CHECK(NeedSearch(n, r) != nullptr);
	n.threatNear = false;
	n.targetUnseen = true;
	CHECK(NeedSearch(n, r) != nullptr);
	n.mode = Mode::kMoving;
	CHECK(NeedSearch(n, r) == nullptr);  // never while on the move

	// The cloak fade: a change in view fades out (300 ms), publishes, waits for the guest's echo, fades in; out of view at once.
	Fader f;
	Post  p1, p2;
	p1.feet = { 10, 0, 0 };
	p2.feet = { 40, 5, 0 };
	f.Want(p1, false, 1000);
	CHECK(f.Update(1000, false, false, r) && f.Shown() && Near(f.Shown()->feet.x, 10.0f) && f.Alpha() == 0.0f);  // published at once
	CHECK(!f.Update(1016, true, false, r) && f.Alpha() == 1.0f);  // echoed out of view: drawn at once
	f.Want(p2, true, 2000);                                        // in view: her cloak
	CHECK(!f.Update(2150, false, true, r) && f.Alpha() > 0.4f && f.Alpha() < 0.6f && Near(f.Shown()->feet.x, 10.0f));
	CHECK(f.Update(2300, false, true, r) && Near(f.Shown()->feet.x, 40.0f) && f.Alpha() == 0.0f);  // faded out: the new post published
	CHECK(!f.Update(2400, false, true, r) && f.Alpha() == 0.0f && f.Current() == Fader::Step::kWaitEcho);  // not shown before the echo
	CHECK(!f.Update(2450, true, true, r) && f.Current() == Fader::Step::kIn);
	f.Update(2600, true, true, r);
	CHECK(f.Alpha() > 0.4f && f.Alpha() < 0.6f);
	f.Update(2760, true, true, r);
	CHECK(f.Alpha() == 1.0f && f.Current() == Fader::Step::kSteady);
	// No echo at all (a guest that keeps her beside Snake): drawn at the post anyway after echo_wait_ms (the translation absorbs it).
	f.Want(p1, false, 3000);
	f.Update(3000, false, false, r);
	CHECK(!f.Update(3500, false, false, r) && f.Alpha() == 0.0f);
	f.Update(4001, false, false, r);
	CHECK(f.Alpha() == 1.0f && f.EchoLate());
	// On the move: faded out, none published.
	f.Want(std::nullopt, true, 5000);
	CHECK(!f.Update(5100, false, true, r));
	CHECK(f.Update(5301, false, true, r) && !f.Shown() && f.Alpha() == 0.0f);
	// A world shift moves the kept posts.
	f.Want(p1, false, 6000);
	f.Update(6000, false, false, r);
	f.Shift({ 32, -8, 0 });
	CHECK(Near(f.Shown()->feet.x, 42.0f) && Near(f.Shown()->feet.y, -8.0f));
	// Adopt: the grace seat she gets up from becomes her post, drawn at once.
	Post seat;
	seat.feet = { 1, 0, 2 };
	f.Adopt(seat);
	CHECK(f.Shown() && Near(f.Shown()->feet.z, 2.0f) && f.Alpha() == 1.0f && f.Current() == Fader::Step::kSteady);

	// The far-ground rule: the first walkable hit from 20 m above; a steep face skipped; none: hidden.
	std::vector<Hit> hits{ { { 0, 15.16f, 0 }, { 0, 0.27f, 0.96f } }, { { 0, 8.9f, 0 }, { 0, 0.99f, 0 } } };
	CHECK(FarGround(hits) && Near(*FarGround(hits), 8.9f));
	std::vector<Hit> plain{ { { 0, 15.16f, 0 } } };  // no normal known: taken
	CHECK(FarGround(plain) && Near(*FarGround(plain), 15.16f));
	CHECK(!FarGround(std::span<const Hit>{}));
}

static void TestQuietPick()
{
	using namespace quiettest;
	const Rules r;
	const View  view{ { 0, 1.6f, -2.0f }, { 0, 0, 1 } };
	const Vec3  snake{ 0, 0, 0 };
	const std::optional<Vec3> post = Vec3{ 30, 0.9f, 0 };
	const float actors = 60.0f;
	const auto  at = quiettest::T;
	CHECK(Screen(at(1, 0, 30), view, post, snake, actors, false, false, r) == PickWhy::kOk);
	CHECK(Screen(at(1, 4, 30), view, post, snake, actors, false, false, r) == PickWhy::kOk);        // ~7 deg off: within 10
	CHECK(Screen(at(1, 4, 30), view, post, snake, actors, false, true, r) == PickWhy::kOffCentre);  // ...not within the binoculars' 6
	auto friendly = at(1, 0, 30);
	friendly.hostile = false;  // Varre, a merchant, a spirit summon, the grace's hidden team-0 character
	friendly.boss = true;
	friendly.asleep = true;
	CHECK(Screen(friendly, view, post, snake, actors, true, false, r) == PickWhy::kNotHostile);
	auto boss = at(1, 0, 30);
	boss.boss = true;
	CHECK(Screen(boss, view, post, snake, actors, false, false, r) == PickWhy::kBoss);
	auto dead = at(1, 0, 30);
	dead.alive = false;
	CHECK(Screen(dead, view, post, snake, actors, false, false, r) == PickWhy::kDead);
	auto held = at(1, 0, 30);
	held.held = true;
	CHECK(Screen(held, view, post, snake, actors, false, false, r) == PickWhy::kHeld);
	auto lifting = at(1, 0, 30);
	lifting.lifting = true;
	CHECK(Screen(lifting, view, post, snake, actors, false, false, r) == PickWhy::kLifting);
	auto hiddenOne = at(1, 0, 30);
	hiddenOne.rendered = false;
	CHECK(Screen(hiddenOne, view, post, snake, actors, false, false, r) == PickWhy::kHidden);
	auto sleeper = at(1, 0, 30);
	sleeper.asleep = true;
	CHECK(Screen(sleeper, view, post, snake, actors, true, false, r) == PickWhy::kAsleep);   // the Guilty Butterfly: never a sleeper
	CHECK(Screen(sleeper, view, post, snake, actors, false, false, r) == PickWhy::kOk);      // the lethal rifles: yes
	CHECK(Screen(at(1, 0, 30), view, std::nullopt, snake, actors, false, false, r) == PickWhy::kNoPost);
	CHECK(Screen(at(1, 0, 70), view, post, snake, actors, false, false, r) == PickWhy::kOutside);  // past the Actors radius
	Rules shortRange = r;
	shortRange.targetRange = 20;
	CHECK(Screen(at(1, 0, 30), view, post, snake, actors, false, false, shortRange) == PickWhy::kTooFar);

	// Marked first, then the one nearest the centre; sight from her post refused -> the next; the camera's sight too.
	std::vector<Target> ts{ at(1, 0, 30), at(2, 3, 30), at(3, -3, 25) };
	ts[2].marked = true;
	auto res = Pick(ts, view, post, snake, actors, false, false, r, nullptr, nullptr);
	CHECK(res.index && *res.index == 2);
	ts[2].marked = false;
	res = Pick(ts, view, post, snake, actors, false, false, r, nullptr, nullptr);
	CHECK(res.index && *res.index == 0);
	const SightFn notFirst = [](const Target& a_t) { return a_t.handle != 1; };
	res = Pick(ts, view, post, snake, actors, false, false, r, notFirst, nullptr);
	CHECK(res.index && *res.index != 0);
	const SightFn none = [](const Target&) { return false; };
	res = Pick(ts, view, post, snake, actors, false, false, r, none, nullptr);
	CHECK(!res.index && res.why == PickWhy::kNoSightPost);
	res = Pick(ts, view, post, snake, actors, false, false, r, nullptr, none);
	CHECK(!res.index && res.why == PickWhy::kNoSightView);
	// Nobody hostile at all (The First Step's grace: Varre and the hidden team-0 character): never a pick.
	std::vector<Target> neutrals{ friendly, friendly };
	neutrals[1].handle = 9;
	res = Pick(neutrals, view, post, snake, actors, false, false, r, nullptr, nullptr);
	CHECK(!res.index);
	CHECK(LatchFits(at(1, 0, 30), post, snake, actors, false, r) && !LatchFits(boss, post, snake, actors, false, r) &&
	      !LatchFits(friendly, post, snake, actors, false, r) && !LatchFits(dead, post, snake, actors, false, r));

	// The pick never selects a target the shot's own ray refuses from the same post: random walls, posts and targets, one oracle.
	std::mt19937                          rng(18);
	std::uniform_real_distribution<float> u(-1.0f, 1.0f);
	int                                   picks = 0;
	for (int trial = 0; trial < 300; ++trial) {
		quiettest::World w;
		for (int k = 0; k < 6; ++k) {
			const Vec3 c{ u(rng) * 25.0f, 0, 20.0f + u(rng) * 15.0f };
			w.boxes.push_back({ c - Vec3{ 1.5f, 0, 0.3f }, c + Vec3{ 1.5f, 3.0f, 0.3f } });
		}
		const Vec3 sight{ u(rng) * 40.0f, 0.9f + std::fabs(u(rng)) * 6.0f, u(rng) * 10.0f };
		std::vector<Target> targets;
		for (int k = 0; k < 5; ++k) {
			targets.push_back(at(static_cast<std::uint64_t>(k + 1), u(rng) * 4.0f, 30.0f + u(rng) * 10.0f));
		}
		auto          ray = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) { return w(a_f, a_d, a_k); };
		const SightFn fromPost = [&](const Target& a_t) { return LineClear(sight, a_t.chest, ray, r.lineSlack); };
		const auto    p = Pick(targets, view, sight, snake, actors, false, false, r, fromPost, nullptr);
		if (p.index) {
			++picks;
			CHECK(LineClear(sight, targets[*p.index].chest, ray, r.lineSlack));  // the shot's ray (ApplyHit casts the same line)
		}
	}
	CHECK(picks > 50);
}

static void TestQuietShot()
{
	using namespace quiettest;
	const Rules               r;
	const combat::HitRules    hit;
	const cqc::Rules          cqcr;
	const combat::MeleeValues melee;
	CHECK(WeaponOfAttack(290) == proto::kQuietLethal && WeaponOfAttack(291) == proto::kQuietTranq && WeaponOfAttack(292) == proto::kQuietHeavy &&
	      WeaponOfAttack(141) == proto::kQuietWeaponUnknown);
	ShotTarget soldier{ true, true, true, false, 221 };
	// Wicked Butterfly: MGSV's share 0.9615 of his reference life, lands whatever the bullet does, no sound.
	const auto lethal = ShotEffect(proto::kQuietLethal, soldier, 0.9615f, true, true, r, hit, cqcr, melee);
	CHECK(lethal.act == ShotAct::kLethal && lethal.hit.damage > 0 && lethal.hit.landOrHp && lethal.hit.shared && lethal.hit.quiet && !lethal.hit.nonLethal);
	const auto fallback = ShotEffect(proto::kQuietLethal, soldier, 0.0f, true, true, r, hit, cqcr, melee);  // no share: lethal_fallback lives
	CHECK(fallback.act == ShotAct::kLethal && !fallback.hit.shared && fallback.hit.damage >= lethal.hit.damage);
	// Sinful Butterfly: heavier, with the knockdown.
	const auto heavy = ShotEffect(proto::kQuietHeavy, soldier, 1.4615f, true, true, r, hit, cqcr, melee);
	CHECK(heavy.act == ShotAct::kHeavy && heavy.hit.damage > lethal.hit.damage && Near(heavy.hit.poise, cqcr.throwPoise) && heavy.hit.dmgLevel == cqcr.throwDmgLevel);
	// Guilty Butterfly: the sleep darts' build-up, never lethal.
	const auto tranq = ShotEffect(proto::kQuietTranq, soldier, 0.4615f, true, true, r, hit, cqcr, melee);
	CHECK(tranq.act == ShotAct::kSleep && tranq.hit.nonLethal && Near(tranq.hit.sleepShare, 1.1f) && tranq.hit.sleep > 0 && tranq.hit.damage <= 1);
	// Refusals, checked again when the round lands.
	ShotTarget friendly = soldier;
	friendly.hostile = false;
	CHECK(ShotEffect(proto::kQuietHeavy, friendly, 1.46f, true, true, r, hit, cqcr, melee).act == ShotAct::kNotHostile);
	ShotTarget boss = soldier;
	boss.boss = true;
	CHECK(ShotEffect(proto::kQuietHeavy, boss, 1.46f, true, true, r, hit, cqcr, melee).act == ShotAct::kBoss);
	ShotTarget dead = soldier;
	dead.alive = false;
	CHECK(ShotEffect(proto::kQuietLethal, dead, 0.96f, true, true, r, hit, cqcr, melee).act == ShotAct::kDead);
	CHECK(ShotEffect(proto::kQuietLethal, ShotTarget{}, 0.96f, true, true, r, hit, cqcr, melee).act == ShotAct::kGone);
	CHECK(ShotEffect(proto::kQuietLethal, soldier, 0.96f, false, true, r, hit, cqcr, melee).act == ShotAct::kNoPost);
	const auto blocked = ShotEffect(proto::kQuietLethal, soldier, 0.96f, true, false, r, hit, cqcr, melee);
	CHECK(blocked.act == ShotAct::kBlocked && blocked.hit.damage == 0);  // Elden Ring's line decides: a miss, no damage
	// Review (integration 0.9.0): an unknown rifle (an attack id outside 290-292 with no weapon from the guest, her dash's 141/142) is never
	// lethal, whatever share comes with it -- refused, no damage.
	for (const float share : { 0.0f, 0.46f, 25.2f }) {
		const auto unknown = ShotEffect(proto::kQuietWeaponUnknown, soldier, share, true, true, r, hit, cqcr, melee);
		CHECK(unknown.act == ShotAct::kUnknownRifle && unknown.hit.damage <= 1 && unknown.act != ShotAct::kLethal);
	}
	CHECK(std::string(Name(ShotAct::kUnknownRifle)).find("unknown") != std::string::npos);

	// The texts.
	CHECK(OrderText(1, proto::kQuietLethal).find("kill") != std::string::npos && OrderText(2, proto::kQuietTranq).find("tranq") != std::string::npos);
	CHECK(OrderText(4, proto::kQuietLethal).find("not in Elden Ring yet") != std::string::npos && OrderText(7, proto::kQuietLethal) == "Quiet");
	CHECK(ShotText(proto::kQuietRefused, 0).find("binoculars") != std::string::npos);
	CHECK(ShotText(proto::kQuietLost, proto::kQuietLostMgsv) == "Quiet: no shot (MGSV)" && ShotText(proto::kQuietLost, proto::kQuietLostCancelled).empty() &&
	      ShotText(proto::kQuietHit, 0).empty());

	// Spotting: newly spotted once, again after the mark lapsed; forgotten at once (dead / not hostile).
	Spotter s;
	const std::uint64_t one[]{ 7 };
	CHECK(s.Update(one, 1000, r).size() == 1 && s.Update(one, 1500, r).empty() && s.Count(1500) == 1);
	CHECK(s.Update({}, 12000, r).empty() && s.Count(12000) == 0 && s.Update(one, 12500, r).size() == 1);
	s.Forget(7);
	CHECK(s.Count(12500) == 0);
	SpotText st;
	CHECK(st.Update(1, 1, 1000, 8) == "Quiet: Enemy spotted" && st.Update(1, 2, 2000, 8).empty() && st.Update(0, 2, 9100, 8) == "Quiet: Enemy spotted (2 nearby)");
}

// The study's post surveys (perch_gatefront.json, perch_firststep.json; rings 20-60 m, the eye 0.8 m up, no footprint cast) recounted
// with the design's rule (quiet.md §2.9): height -3..+25 m, <= 60 m in 3D, ny >= 0.8, headroom, sees Snake, sees >= 1 hostile, none
// within 15 m -- Gatefront 15, The First Step 16 -- and then with the drawable rule (Elden Ring's default vertical FOV, read live).
static void TestQuietPerches()
{
	using namespace quiettest;
	const Rules r;
	struct Want
	{
		const char* file;
		int         recount;
	};
	for (const Want w : { Want{ "perch_gatefront.json", 15 }, Want{ "perch_firststep.json", 16 } }) {
		const auto j = jsontest::Load(jsontest::g_fixtures / w.file);
		CHECK(j.has_value());
		if (!j) {
			continue;
		}
		int design = 0, drawable = 0;
		std::map<int, int> byRing;
		for (const auto& s : j->Get("spots")->arr) {
			if (s.Has("fail")) {
				continue;
			}
			const float ring = static_cast<float>(s.Num("r"));
			const float dy = static_cast<float>(s.Num("dy"));
			const bool  ok = dy >= r.coverHeightMin && dy <= r.coverHeightMax && std::sqrt(ring * ring + dy * dy) <= r.max3d && s.Num("ny") >= r.slopeNy &&
			                s.Bool("headroom") && s.Bool("sees_snake") && s.Num("sees_enemies") >= 1 && s.Num("nearest_enemy") > r.enemyClear;
			if (!ok) {
				continue;
			}
			++design;
			++byRing[static_cast<int>(ring)];
			drawable += std::fabs(dy) <= DrawableBound(ring, 0.8f) ? 1 : 0;
		}
		CHECK(design == w.recount);
		std::string rings;
		for (const auto& [ring, n] : byRing) {
			rings += std::format("{}{} m: {}", rings.empty() ? "" : ", ", ring, n);
		}
		std::printf("  quiet perches %s: the design's rule %d (%s), drawable at fovY 0.8 %d\n", w.file, design, rings.c_str(), drawable);
	}
}

// The raw rays the host recorded live (`quiet survey`, `quiet record on` -> PhantomRingQuietRays-N.json, copied to
// tests/fixtures/quiet/quiet_rays_*.json): every candidate re-evaluated on the recorded rays must reach the recorded verdict.
static void TestQuietRecorded()
{
	using namespace quiettest;
	int files = 0;
	std::error_code ec;
	for (const auto& e : std::filesystem::directory_iterator(jsontest::g_fixtures, ec)) {
		const auto name = e.path().filename().string();
		if (!name.starts_with("quiet_rays_") || e.path().extension() != ".json") {
			continue;
		}
		const auto j = jsontest::Load(e.path());
		CHECK(j.has_value());
		if (!j || !j->Has("spots")) {
			continue;
		}
		++files;
		PostScene s;
		s.snake = jsontest::V3(j->Get("snake"));
		s.snakeChest = jsontest::V3(j->Get("snakeChest"));
		s.camPos = jsontest::V3(j->Get("camPos"));
		s.camFwd = jsontest::V3(j->Get("camFwd"));
		s.fovY = static_cast<float>(j->Num("fovY", 0.8));
		s.aimPoint = jsontest::V3(j->Get("aimPoint"));
		for (const auto& h : j->Get("hostiles")->arr) {
			s.hostiles.push_back({ 0, jsontest::V3(h.Get("feet")), jsontest::V3(h.Get("chest")) });
		}
		for (const auto& c : j->Get("characters")->arr) {
			s.characters.push_back(jsontest::V3(&c));
		}
		gracetest::RecordedOracle oracle;
		for (const auto& ry : j->Get("rays")->arr) {
			gracetest::RecordedOracle::R x;
			x.f = jsontest::V3(ry.Get("f"));
			x.d = jsontest::V3(ry.Get("d"));
			x.k = static_cast<int>(ry.Num("k"));
			x.hit = !ry.Get("h")->IsNull();
			if (x.hit) {
				x.h = jsontest::V3(ry.Get("h"));
			}
			oracle.rays.push_back(x);
		}
		const auto* m = j->Get("mode");
		const Mode  mode = m && m->str == "near" ? Mode::kNear : Mode::kCover;
		auto        ray = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) -> std::optional<Hit> {
			const auto h = oracle(a_f, a_d, a_k == Ray::kGround ? phantom::er::grace::Ray::kGround : phantom::er::grace::Ray::kWorld);
			return h ? std::optional<Hit>(Hit{ h->pos }) : std::nullopt;
		};
		const Rules r;
		int         same = 0, total = 0, passed = 0;
		for (const auto& sp : j->Get("spots")->arr) {
			Post p;
			p.mode = mode;
			p.ring = static_cast<float>(sp.Num("ring"));
			p.deg = static_cast<float>(sp.Num("deg"));
			const float a = p.deg * 3.14159265f / 180.0f;
			p.feet = { s.snake.x + p.ring * std::cos(a), s.snake.y, s.snake.z + p.ring * std::sin(a) };
			const auto got = EvaluatePost(p, s, ray, r);
			++total;
			same += Name(got.why) == sp.Get("why")->str ? 1 : 0;
			passed += got.why == PostWhy::kOk ? 1 : 0;
		}
		CHECK(same == total && oracle.misses == 0);
		// U13 with the real MGSV's camera (quiet.md M1 Q8: MGSV's vertical FOV 35.6 deg -- Elden Ring's camera follows it; the fake's
		// camera of this recording was 60 deg): the same rays, the drawable rule at 35.6 deg.
		PostScene real = s;
		real.fovY = 35.6f * 3.14159265f / 180.0f;
		std::map<int, int> rings;
		int                realPassed = 0;
		for (const auto& sp : j->Get("spots")->arr) {
			Post p;
			p.mode = mode;
			p.ring = static_cast<float>(sp.Num("ring"));
			p.deg = static_cast<float>(sp.Num("deg"));
			const float a = p.deg * 3.14159265f / 180.0f;
			p.feet = { real.snake.x + p.ring * std::cos(a), real.snake.y, real.snake.z + p.ring * std::sin(a) };
			if (EvaluatePost(p, real, ray, r).why == PostWhy::kOk) {
				++realPassed;
				++rings[static_cast<int>(p.ring)];
			}
		}
		std::string ringText;
		for (const auto& [ring, n] : rings) {
			ringText += std::format("{}{} m: {}", ringText.empty() ? "" : ", ", ring, n);
		}
		CHECK(realPassed > 0);  // a post at both places with MGSV's own field of view
		std::printf("  quiet rays %s (%s): %d of %d candidates re-evaluated to the recorded verdict, %d passed (%d unmatched rays); at MGSV's 35.6 deg: %d "
		            "passed (%s)\n",
			name.c_str(), Name(mode), same, total, passed, oracle.misses, realPassed, ringText.c_str());
	}
	std::printf("  quiet: %d recorded ray file(s) replayed\n", files);
}
