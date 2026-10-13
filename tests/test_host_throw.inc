// Included by tests/test_host_combat.cpp: er/ThrowLogic (Snake's grenades flown through Elden Ring's world, MGSV's aiming
// arc) over synthetic ground, and the sleep route's pure rules (rungs on the bullet, shares of the target's resistance).

#include "ThrowLogic.h"
#include "WorldShift.h"

namespace throw_test
{
	using namespace phantom::er::throwing;

	// Flat ground at y = 0, plus (a_wall) a wall x = 10 facing -x.
	RayFn World(bool a_wall)
	{
		return [a_wall](const Vec3& a_from, const Vec3& a_d) -> std::optional<RayHit> {
			std::optional<RayHit> best;
			float                 bestT = 2.0f;
			if (a_d.y < 0 && a_from.y >= 0) {
				const float t = -a_from.y / a_d.y;
				if (t <= 1.0f) {
					bestT = t;
					best = RayHit{ a_from + a_d * t, { 0, 1, 0 } };
				}
			}
			if (a_wall && a_d.x > 0 && a_from.x <= 10.0f) {
				const float t = (10.0f - a_from.x) / a_d.x;
				if (t <= 1.0f && t < bestT) {
					best = RayHit{ a_from + a_d * t, { -1, 0, 0 } };
				}
			}
			return best;
		};
	}
}

static void TestThrowLogic()
{
	using namespace throw_test;
	Params p;
	// MGSV's frag throw from the recording: released 1.5 m up at (11.5, 4.5, 0) m/s. No bounce: lands where the parabola does.
	{
		Projectile g;
		g.pos = { 0, 1.5f, 0 };
		g.vel = { 11.5f, 4.5f, 0 };
		for (int i = 0; i < 300 && !g.touched; ++i) {
			Step(g, 1.0f / 60.0f, p, World(false), true);
		}
		const float t = (4.5f + std::sqrt(4.5f * 4.5f + 2 * 9.8f * 1.5f)) / 9.8f;  // flight time to y = 0
		CHECK(g.touched && g.resting);
		CHECK(std::abs(g.pos.x - 11.5f * t) < 0.15f);
		CHECK(std::abs(g.pos.y - p.radius) < 0.02f);
	}
	// A fused grenade bounces and rolls on, slower each time, then lies still beyond the first impact.
	{
		Projectile g;
		g.pos = { 0, 1.5f, 0 };
		g.vel = { 8.0f, 4.0f, 0 };
		float firstX = -1;
		for (int i = 0; i < 1200 && !g.resting; ++i) {
			Step(g, 1.0f / 60.0f, p, World(false), false);
			if (g.touched && firstX < 0) {
				firstX = g.pos.x;
			}
		}
		CHECK(g.resting && g.bounces >= 1);
		CHECK(g.pos.x > firstX + 0.5f);
		CHECK(std::abs(g.pos.y - p.radius) < 0.05f && Length(g.vel) < 1e-3f);
	}
	// A wall 10 m away sends it back: it ends up in front of the wall, not through it.
	{
		Projectile g;
		g.pos = { 0, 1.5f, 0 };
		g.vel = { 20.0f, 3.0f, 0 };
		for (int i = 0; i < 1200 && !g.resting; ++i) {
			Step(g, 1.0f / 60.0f, p, World(true), false);
		}
		CHECK(g.resting && g.pos.x < 10.0f && g.bounces >= 2);
	}
	// Falling out of the world (no ground) is given up, not flown forever.
	{
		Projectile g;
		g.pos = { 0, 1.5f, 0 };
		g.vel = { 1, 1, 0 };
		for (int i = 0; i < 60 * 35; ++i) {
			Step(g, 1.0f / 60.0f, p, [](const Vec3&, const Vec3&) { return std::optional<RayHit>{}; }, false);
		}
		CHECK(g.lost);
	}
	// The aiming arc: dots a_spacing apart along the path, a landing where the parabola meets the ground.
	{
		const auto arc = TraceArc({ 0, 1.5f, 0 }, { 11.5f, 4.5f, 0 }, p, World(false), 0.3f);
		CHECK(arc.landing && std::abs(arc.landing->point.y) < 1e-3f && arc.landing->normal.y > 0.99f);
		CHECK(arc.dots.size() > 30);
		bool spaced = true;
		for (std::size_t i = 1; i + 1 < arc.dots.size(); ++i) {
			spaced &= std::abs(Length(arc.dots[i] - arc.dots[i - 1]) - 0.3f) < 0.03f;  // chords of a 0.3 m spacing
		}
		CHECK(spaced);
		const auto none = TraceArc({ 0, 1.5f, 0 }, { 11.5f, 4.5f, 0 }, p, [](const Vec3&, const Vec3&) { return std::optional<RayHit>{}; }, 0.3f, 1.0f);
		CHECK(!none.landing);
	}
	// Surface normals from three hits, facing the side the ray came from.
	CHECK(PlaneNormal({ 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 0, 0 }).y > 0.99f);
	CHECK(PlaneNormal({ 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, -1, 0 }, { 0, 0, 0 }).y < -0.99f);
	CHECK(PlaneNormal({ 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 }, { 0, 1, 0 }, { 0, 7, 0 }).y == 7.0f);  // no plane: fallback
	// kGEvThrow pad[0]: equip | fuse ms << 16 (the recording: 354 | 3000 << 16).
	CHECK(EquipOf(354 | (3000u << 16)) == 354 && FuseMsOf(354 | (3000u << 16)) == 3000);

	// Sleep: a share of the target's own resistance (226 Gatefront soldier), immune at 999, absolute when unknown; rungs on
	// the bullet's 5 SpEffect slots; the bullet row key changes with the build-up.
	CHECK(SleepPower(80, 0.6f, 226) == 136 && SleepPower(80, 0.6f, 0) == 80 && SleepPower(80, 0.6f, 999) == 0 && SleepPower(80, 0.0f, 226) == 80);
	CHECK((SleepRungs(136, 25000, 10, 20) == std::vector<std::int32_t>{ 25013 }));
	CHECK((SleepRungs(450, 25000, 10, 20) == std::vector<std::int32_t>{ 25019, 25019, 25004 }));
	CHECK(SleepRungs(5000, 25000, 10, 20).size() == 5 && SleepRungs(0, 25000, 10, 20).empty());
	CHECK(HitKey(0, 0, false, 0, 0, 136) != HitKey(0, 0, false, 0, 0, 45));
	CHECK(HitKey(376, 100, false, 30.0f, 1) == HitKey(376, 100, false, 30.0f, 1, 0));
	// Large build-ups keep their own keys up to the default ladder's 5 x 200 = 1000 (a body dart 540 vs a head dart 1023
	// rounded to 1000 on a ~930-resist enemy used to share 511); past 10 bits a hit never shares the row.
	CHECK(HitKey(0, 0, false, 0, 0, 540) != HitKey(0, 0, false, 0, 0, 1000));
	CHECK(HitKey(0, 0, false, 0, 0, 1000) != HitKey(0, 0, false, 0, 0, 1023));
	CHECK(HitKey(0, 0, false, 0, 0, 1024) == kUniqueKey && HitKey(0, 0, false, 0, 0, 1023) != kUniqueKey);
	{
		int                    a = 0, b = 0;
		std::vector<InFlight> none, one{ { &a, HitKey(0, 0, false, 0, 0, 100), false } };
		CHECK(MayFly(none, &b, kUniqueKey, false));
		CHECK(!MayFly(one, &b, kUniqueKey, false));
		CHECK(!MayFly({ { &a, kUniqueKey, false } }, &b, kUniqueKey, false));
	}

	// Mines (review): a set-off with no blast is sent again after 3 s, at most 3 times; one scan sphere covers every mine.
	{
		CHECK(!DetonateRetryDue(1000, 0, 0, false));             // never sent
		CHECK(!DetonateRetryDue(3999, 1000, 0, false));          // too early
		CHECK(DetonateRetryDue(4000, 1000, 0, false));
		CHECK(!DetonateRetryDue(9000, 1000, 0, true));           // it went off
		CHECK(DetonateRetryDue(9000, 1000, 2, false) && !DetonateRetryDue(9000, 1000, 3, false));
		const auto one = Enclosing({ { { 1, 0, 2 }, 3.0f } });
		CHECK(Near(one.centre.x, 1) && Near(one.centre.z, 2) && Near(one.radius, 3));
		const auto two = Enclosing({ { { 0, 0, 0 }, 3.0f }, { { 10, 0, 0 }, 1.5f } });
		CHECK(Near(two.centre.x, 5) && Near(two.radius, 8));  // 5 + the first one's 3 m
		CHECK(Enclosing({}).radius == 0.0f);
		// Every point within a mine's trigger radius is inside the sphere.
		const std::vector<Sphere> mines{ { { -4, 1, 7 }, 3.0f }, { { 6, -2, 1 }, 1.5f }, { { 0, 0, -9 }, 3.0f } };
		const auto                all = Enclosing(mines);
		for (const auto& m : mines) {
			for (const Vec3 off : { Vec3{ 1, 0, 0 }, Vec3{ -1, 0, 0 }, Vec3{ 0, 1, 0 }, Vec3{ 0, 0, -1 } }) {
				CHECK(Length(m.centre + off * m.radius - all.centre) <= all.radius + 1e-4f);
			}
		}
	}
	// Elden Ring's world origin shifts: whole 8 m grid steps, up to 20 km; the player's own motion of that frame never leaks
	// in (review: every live shift carried 0.03-0.09 m of it into the mines), and an off-grid move (a console setpos) is none.
	{
		using phantom::er::WorldShift::ShiftOf;
		CHECK(!ShiftOf({ 0, 0, 0 }, { 0.2f, 0, 0.2f }));
		const auto d = ShiftOf({ 100, 5, -50 }, { -900, 5, 454 });
		CHECK(d && Near(d->x, -1000) && Near(d->y, 0) && Near(d->z, 504));
		CHECK(!ShiftOf({ 0, 0, 0 }, { 30000, 0, 0 }) && !ShiftOf({ 0, 0, 0 }, { NAN, 0, 0 }));
		// The live shifts, with the player's motion in them: the grid step comes back exactly.
		for (const Vec3 live : { Vec3{ -31.94f, 0.01f, -0.03f }, Vec3{ -7.97f, 0.01f, 32.07f }, Vec3{ 32.06f, 0.02f, 23.97f } }) {
			const Vec3 from{ 812.3f, 41.7f, -1203.9f };
			const auto s = ShiftOf(from, from + live);
			CHECK(s && Near(s->x, std::round(live.x / 8.0f) * 8.0f) && s->y == 0.0f && Near(s->z, std::round(live.z / 8.0f) * 8.0f));
		}
		// Riding through a hitch: half a metre of his own on top of a 32 m step is still that step.
		const auto ride = ShiftOf({ 0, 0, 0 }, { 32.5f, -0.4f, 0.45f });
		CHECK(ride && Near(ride->x, 32.0f) && ride->y == 0.0f && ride->z == 0.0f);
		// Not a shift: a setpos 25 m up, a 5 m teleport, a 12 m move (half-way between steps), a fall of 3 m in one frame.
		CHECK(!ShiftOf({ 0, 0, 0 }, { 0, 25, 0 }) && !ShiftOf({ 0, 0, 0 }, { 5, 0, 0 }) && !ShiftOf({ 0, 0, 0 }, { 12, 0, 0 }));
		CHECK(!ShiftOf({ 0, 0, 0 }, { 32, -3, 0 }));
	}

	// One blast, one set of numbers (review: a stun grenade over three enemies staggered only the first one; sleep gas
	// over two kinds of enemy put only one to sleep, the other's build-up was "LOST (another bullet holds the row)").
	{
		HitRules rules;
		const auto stun = StunBlastEffect(rules);
		CHECK(stun.damage == 1 && stun.nonLethal && stun.dmgLevel == 3 && stun.correction == kStunBlastCorrection);
		CHECK(Near(stun.poise, ExplosionEffect(0.0f, 10.0f, 300, rules).poise));  // the centre's stagger for everyone
		rules.stunPoiseMult = 2.0f;
		CHECK(Near(StunBlastEffect(rules).poise, 2.0f * stun.poise));
		// The same key for every target (what MayFly compares), whatever its distance or kind.
		CHECK(HitKey(1, stun.correction, false, stun.poise, stun.dmgLevel) == HitKey(1, stun.correction, false, stun.poise, stun.dmgLevel));
		// Gas: share of the largest known resistance (226 soldier, 480 knight -> 0.2 x 480 = 96), immune/unknown ignored,
		// capped; absolute when nothing is known.
		const auto gas = GasBlastEffect(40, 0.2f, { 226, 480, 999, 0 }, 1000);
		CHECK(gas.sleep == 96 && gas.sleepShare == 0.0f && gas.damage == 0 && gas.nonLethal);
		CHECK(GasBlastEffect(40, 0.2f, { 999, 0 }, 1000).sleep == 40);
		CHECK(GasBlastEffect(40, 0.2f, {}, 1000).sleep == 40);
		CHECK(GasBlastEffect(40, 2.0f, { 900 }, 1000).sleep == 1000);
		// Per target: the same power (the same rungs, the same key), nothing for an immune one.
		CHECK(SleepPower(gas.sleep, gas.sleepShare, 226) == 96 && SleepPower(gas.sleep, gas.sleepShare, 480) == 96);
		CHECK(SleepPower(gas.sleep, gas.sleepShare, 999) == 0);
	}

	// Non-lethal hits can't kill through Elden Ring's own part either (review: the stun grenade's fixed 15% landed a weapon
	// hit x 15% on a target at 1 HP; Apply clamps only our damage).
	{
		// A full-HP soldier (219, ER's hit 42 at 100%): the blast's 15% stays.
		CHECK(NonLethalCorrection(kStunBlastCorrection, 1, 219, 42, 0) == kStunBlastCorrection);
		// Nothing left to deal (our damage clamped to 0 at 1 HP): the sleep correction (0), whatever the blast brought.
		CHECK(NonLethalCorrection(kStunBlastCorrection, 0, 1, 42, 0) == 0);
		CHECK(NonLethalCorrection(100, 0, 50, 42, 0) == 0);
		// 2 HP or less: nothing of Elden Ring's.
		CHECK(NonLethalCorrection(kStunBlastCorrection, 1, 2, 42, 0) == 0);
		// Low HP: lowered so even a counter hit (1.6 x the reference) leaves 1 HP.
		for (const int hp : { 3, 5, 8, 12, 20 }) {
			for (const int ref : { 0, 30, 42, 63, 200 }) {
				const auto c = NonLethalCorrection(kStunBlastCorrection, 1, hp, ref, 0);
				CHECK(c <= kStunBlastCorrection);
				CHECK(kNonLethalSpread * (ref > 0 ? ref : 100) * c / 100.0f <= hp - 1 + 1e-3f);
			}
		}
		// A stun round's reduced correction (TopUp: 12% for 5 of a 42 hit) on 6 HP: 7%, ~3 HP of Elden Ring's.
		CHECK(NonLethalCorrection(12, 5, 6, 42, 0) == 7);
	}
}
