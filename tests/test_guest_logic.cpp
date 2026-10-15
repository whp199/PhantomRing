// Unit tests for the MGSV guest's pure arithmetic (mgsv/Logic.cpp): odometer, treadmill, life bridge,
// shot detection, Fox transform helpers, hurt feedback, bullet kinds. Run under wine (target check_guest).
#include "CaptureKeys.h"
#include "Logic.h"
#include "ProjectileLogic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <numbers>

using namespace phantom::guest::logic;

static int g_failures = 0;
#define CHECK(x)                                                    \
	do {                                                            \
		if (!(x)) {                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			++g_failures;                                           \
		}                                                           \
	} while (0)

static bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

static void TestOdometer()
{
	Odometer o;
	CHECK(!o.Primed());
	// First sample only primes.
	CHECK(!o.Update({ 10, 0, 10 }, 1.0 / 60, false, 3.0f, 0.5f));
	CHECK(o.Primed());
	CHECK(o.Epoch() == 1);
	CHECK(Near(o.Odo()[0], 0) && Near(o.Odo()[2], 0));

	// Walk +X at 6 m/s for 60 frames: odometer adds 6 m, velocity converges to 6 m/s.
	Vec3 p{ 10, 0, 10 };
	for (int i = 0; i < 60; ++i) {
		p.x += 0.1f;
		CHECK(!o.Update(p, 1.0 / 60, false, 3.0f, 0.5f));
	}
	CHECK(Near(o.Odo()[0], 6.0, 1e-3));
	CHECK(Near(o.Velocity().x, 6.0f, 0.05));

	// A treadmill warp back 15 m is a teleport: odometer unchanged.
	p.x -= 15.0f;
	CHECK(o.Update(p, 1.0 / 60, true, 3.0f, 0.5f));
	CHECK(Near(o.Odo()[0], 6.0, 1e-3));
	CHECK(o.Teleports() == 1);
	// ... and walking continues from the new spot.
	p.x += 0.1f;
	CHECK(!o.Update(p, 1.0 / 60, false, 3.0f, 0.5f));
	CHECK(Near(o.Odo()[0], 6.1, 1e-3));

	// While one of our warps is pending, even a 1 m jump is a warp (not motion) ...
	p.z += 1.0f;
	CHECK(o.Update(p, 1.0 / 60, true, 3.0f, 0.5f));
	CHECK(Near(o.Odo()[2], 0.0, 1e-3));
	// ... but without one it is ordinary (fast) motion.
	p.z += 1.0f;
	CHECK(!o.Update(p, 1.0 / 60, false, 3.0f, 0.5f));
	CHECK(Near(o.Odo()[2], 1.0, 1e-3));

	// Vertical motion counts (stairs, falls).
	p.y -= 0.5f;
	CHECK(!o.Update(p, 1.0 / 60, false, 3.0f, 0.5f));
	CHECK(Near(o.Odo()[1], -0.5, 1e-3));

	// NaN samples are ignored entirely.
	CHECK(!o.Update({ NAN, 0, 0 }, 1.0 / 60, false, 3.0f, 0.5f));
	CHECK(Near(o.Odo()[0], 6.1, 1e-3));

	// Rebase: we moved him ourselves (native hold); the jump never counts, motion continues from there.
	o.Rebase({ 0, 0, 0 });
	CHECK(!o.Update({ 0.1f, 0, 0 }, 1.0 / 60, false, 3.0f, 0.5f));
	CHECK(Near(o.Odo()[0], 6.2, 1e-3));
	p = { 0.1f, 0, 0 };

	// Reset starts a new epoch at zero.
	o.Reset(p);
	CHECK(o.Epoch() == 2);
	CHECK(Near(o.Odo()[0], 0) && Near(o.Odo()[1], 0) && Near(o.Odo()[2], 0));

	// Many small steps don't drift (double accumulator): 100k frames of 1 cm.
	Odometer d;
	Vec3     q{ 1000, 0, 1000 };
	d.Update(q, 1.0 / 60, false, 3.0f, 0.2f);
	for (int i = 0; i < 100000; ++i) {
		q.z += 0.01f;
		if (q.z > 1010.0f) {  // treadmill-style wrap so float positions stay precise
			q.z -= 10.0f;
			d.Update(q, 1.0 / 60, true, 3.0f, 0.2f);
			continue;
		}
		d.Update(q, 1.0 / 60, false, 3.0f, 0.2f);
	}
	CHECK(Near(d.Odo()[2], 1000.0, 2.0));  // a handful of wrap frames carry their 1 cm with them
}

static void TestTreadmill()
{
	TreadmillInput in;
	in.centre = { 0, 5, 0 };
	in.radius = 15.0f;
	in.nowMs = 10000;
	in.lastWarpMs = 0;
	in.cooldownMs = 1000;

	// Inside the radius: nothing.
	in.root = { 10, 5, 0 };
	in.velocity = { 5, 0, 0 };
	CHECK(!Treadmill(in).warp);

	// Outside and moving away: warp to the centre, shift = centre - root.
	in.root = { 16, 5.2f, 3 };
	auto d = Treadmill(in);
	CHECK(d.warp);
	CHECK(Near(d.target.x, 0) && Near(d.target.y, 5) && Near(d.target.z, 0));
	CHECK(Near(d.shift.x, -16) && Near(d.shift.y, -0.2, 1e-3) && Near(d.shift.z, -3));

	// Outside but walking back in: leave him alone ...
	in.velocity = { -5, 0, 0 };
	CHECK(!Treadmill(in).warp);
	// ... unless he is past twice the radius.
	in.root = { 31, 5, 0 };
	CHECK(Treadmill(in).warp);

	// Not allowed (CQC, dive, vehicle, menu...): never.
	in.root = { 20, 5, 0 };
	in.velocity = { 5, 0, 0 };
	in.allowed = false;
	CHECK(!Treadmill(in).warp);
	in.allowed = true;

	// Cooldown after the last warp.
	in.lastWarpMs = 9500;
	CHECK(!Treadmill(in).warp);
	in.lastWarpMs = 8000;
	CHECK(Treadmill(in).warp);

	// Height doesn't count toward the radius (stairs/ramps on the sandbox).
	in.root = { 1, 50, 1 };
	CHECK(!Treadmill(in).warp);

	// Odometer + treadmill together: walk 100 m in a straight line with warps; the odometer sees
	// exactly 100 m and Snake never strays past radius + one frame of motion.
	Odometer       o;
	Vec3           p{ 0, 5, 0 };
	TreadmillInput t;
	t.centre = p;
	t.radius = 15.0f;
	t.cooldownMs = 0;
	o.Update(p, 1.0 / 30, false, 3.0f, 1.0f);
	double maxDist = 0;
	bool   pending = false;
	for (int i = 1; i <= 1000; ++i) {
		p.z += 0.1f;  // 3 m/s at 30 fps
		const bool teleport = o.Update(p, 1.0 / 30, pending, 3.0f, 1.0f);
		CHECK(!teleport);
		maxDist = std::max<double>(maxDist, (p - t.centre).LengthXZ());
		t.root = p;
		t.velocity = o.Velocity();
		t.nowMs = i * 33.0;
		const auto w = Treadmill(t);
		if (w.warp) {
			// the warp lands next frame (WarpAndWaitBlock through the Lua pump)
			p = w.target;
			CHECK(o.Update(p, 1.0 / 30, true, 3.0f, 1.0f));
			t.lastWarpMs = t.nowMs;
		}
	}
	CHECK(Near(o.Odo()[2], 100.0, 1e-2));
	CHECK(maxDist < 15.2);
}

static void TestLife()
{
	auto r = ApplyHurt(6000, 100.0f, 4.0f);
	CHECK(r.life == 5600 && !r.died);
	r = ApplyHurt(500, 100.0f, 4.0f);
	CHECK(r.life == 100 && !r.died);
	r = ApplyHurt(400, 100.0f, 4.0f);  // exactly zero: clamp to 1, died
	CHECK(r.life == 1 && r.died);
	r = ApplyHurt(100, 1000.0f, 4.0f);
	CHECK(r.life == 1 && r.died);
	r = ApplyHurt(1, 1.0f, 4.0f);  // already at 1
	CHECK(r.life == 1 && r.died);
	r = ApplyHurt(3000, 0.0f, 4.0f);  // no damage
	CHECK(r.life == 3000 && !r.died);
	// MGSV's post-hit regeneration curve (live: 2651 held ~2.4 s, +339 2.3 s into the ramp, +1560 at 5 s, full 7801 ~12.5 s on)
	CHECK(RegenCap(2651.0f, 1.0, 2.4f, 6.25f, 750.0f) == 2651.0f);
	CHECK(RegenCap(2651.0f, 2.4, 2.4f, 6.25f, 750.0f) == 2651.0f);
	CHECK(std::fabs(RegenCap(2651.0f, 4.7, 2.4f, 6.25f, 750.0f) - (2651.0f + 60.0f * 2.3f * 2.3f)) < 0.5f);
	CHECK(std::fabs(RegenCap(2651.0f, 8.65, 2.4f, 6.25f, 750.0f) - (2651.0f + 2343.75f)) < 0.5f);
	CHECK(std::fabs(RegenCap(2651.0f, 10.65, 2.4f, 6.25f, 750.0f) - (2651.0f + 2343.75f + 1500.0f)) < 0.5f);
	CHECK(RegenCap(2651.0f, 13.0, 2.4f, 6.25f, 750.0f) >= 7801.0f && RegenCap(2651.0f, 11.9, 2.4f, 6.25f, 750.0f) < 7801.0f);
	CHECK(RegenCap(100.0f, 50.0, 2.4f, 0.0f, 0.0f) == 100.0f);  // no rate: held
	// The stickiness check (live 2026-10-13: a Margit hit 6942 -> 6641 regenerated to 6942 within 8 frames and was called lost).
	CHECK(!LifeWriteLost(7801, 4609, 4720));   // 8 frames of MGSV's regeneration: kept
	CHECK(LifeWriteLost(7801, 4609, 7801));    // never applied: lost
	CHECK(!LifeWriteLost(6942, 6641, 6942));   // too small to judge (regeneration alone brings it back)
	CHECK(LifeWriteLost(2000, 7801, 2050));    // a refill that fell back
	CHECK(!LifeWriteLost(2000, 7801, 7801));
	CHECK(!LifeWriteLost(5000, 4900, 5000));   // too small to judge
	CHECK(!LifeWriteLost(-1, 4900, 5000));
	r = ApplyHurt(3000, -5.0f, 4.0f);
	CHECK(r.life == 3000 && !r.died);
	r = ApplyHurt(3000, NAN, 4.0f);
	CHECK(r.life == 3000 && !r.died);
	r = ApplyHurt(65535, 1.5f, 4.0f);  // fractions truncate toward keeping life
	CHECK(r.life == 65529);
}

// Elden Ring hit -> MGSV feedback (camera noise + blood) and the bullet-kind table (re-backlog M3).
static void TestCombat()
{
	const Vec3 root{ 100, 0, 50 };
	// A small hit: a light shake, blood at the chest on the side it came from.
	auto f = FeedbackFor(400, 6000, 1, root, { 1, 0, 0 });
	CHECK(f.shake && f.blood && f.level > 0.15f && f.level < 0.4f && f.time >= 0.25f);
	CHECK(Near(f.bloodPos.x, 100.2f) && Near(f.bloodPos.y, 1.2f) && Near(f.bloodPos.z, 50.0f));
	CHECK(f.bloodRadius >= 0.15f && f.bloodRadius <= 0.4f);
	// A big hit shakes harder and longer, capped.
	const auto big = FeedbackFor(6000, 6000, 0, root, { 0, 0, -3 });
	CHECK(big.level == 1.0f && big.time <= 1.2f && big.time > f.time && Near(big.bloodPos.z, 49.8f) && big.bloodRadius == 0.4f);
	// A fall: shake only. Unknown direction: blood at the chest. No loss: nothing.
	const auto fall = FeedbackFor(3000, 6000, 3, root, { 0, -1, 0 });
	CHECK(fall.shake && !fall.blood);
	const auto other = FeedbackFor(100, 6000, 4, root, {});
	CHECK(other.blood && Near(other.bloodPos.x, 100.0f) && Near(other.bloodPos.z, 50.0f));
	CHECK(!FeedbackFor(0, 6000, 0, root, { 1, 0, 0 }).shake);
	CHECK(FeedbackFor(10, 0, 0, root, { 1, 0, 0 }).shake);  // lifeMax unknown: still a reaction

	CHECK(KindOfBullet(10) == BulletKind::kTranq && KindOfBullet(40) == BulletKind::kTranq && KindOfBullet(49) == BulletKind::kTranq);
	CHECK(KindOfBullet(7) == BulletKind::kStun && KindOfBullet(17) == BulletKind::kStun && KindOfBullet(244) == BulletKind::kStun);
	CHECK(KindOfBullet(24) == BulletKind::kNone && KindOfBullet(105) == BulletKind::kNone);
	// v8 damage share: MGSV's table vs a 2600-life soldier (AR 550, grenade round 2600, sniper 3100, stun 0)
	CHECK(DamageShare(550, 2600) == 2115 && DamageShare(2600, 2600) == 10000 && DamageShare(3100, 2600) == 11923 && DamageShare(0, 2600) == 0 && DamageShare(550, 0) == 0);
	CHECK(std::abs(FlameDirection({ 0, 1, 0 }, { 0, 1, -1 }, { 0, 0, 1 }, 11.0f).z - 1.0f) < 1e-4f && FlameNozzle({ 0, 0, 0 }, 0.0f, 2.0f, 1.0f, 1.5f).x == -1.0f);
	CHECK(KindOfBullet(23) == BulletKind::kExplosive && KindOfBullet(45) == BulletKind::kExplosive && KindOfBullet(103) == BulletKind::kExplosive);
	CHECK(KindOfBullet(15) == BulletKind::kLethal && KindOfBullet(38) == BulletKind::kLethal && KindOfBullet(0) == BulletKind::kLethal);
	// Blasts by TppDamage.ATK_* (live ids): grenade / sleep gas / stun / smoke go to Elden Ring, rockets never twice, the rest never.
	CHECK(KindOfAttack(178) == BlastKind::kLethal && KindOfAttack(197) == BlastKind::kLethal && KindOfAttack(192) == BlastKind::kLethal);
	CHECK(KindOfAttack(188) == BlastKind::kTranq && KindOfAttack(286) == BlastKind::kTranq && KindOfAttack(182) == BlastKind::kStun);
	CHECK(KindOfAttack(181) == BlastKind::kNone && KindOfAttack(170) == BlastKind::kRound && KindOfAttack(174) == BlastKind::kRound);
	CHECK(KindOfAttack(347) == BlastKind::kRound && KindOfAttack(447) == BlastKind::kRound && KindOfAttack(450) == BlastKind::kLethal);
	CHECK(KindOfAttack(123) == BlastKind::kIgnore && KindOfAttack(80) == BlastKind::kIgnore && KindOfAttack(0) == BlastKind::kIgnore);
	// Explosives by equip id (GUEST.md 18.8) and the release velocity of MGSV's arc (live: 0.075 s steps, 11.67/4.70 m/s).
	CHECK(ExplosiveOf(354).use == ExplosiveUse::kThrown && ExplosiveOf(378).kind == BlastKind::kTranq && ExplosiveOf(382).impact);
	CHECK(ExplosiveOf(418).use == ExplosiveUse::kPlaced && ExplosiveOf(391).triggerMm == 0 && !ExplosiveOf(425).ownBlast);
	CHECK(ExplosiveOf(349).use == ExplosiveUse::kNone && ExplosiveOf(875).use == ExplosiveUse::kNone);
	// Decoys (roadmap 2.8): plain, active, shock (incl. the out-of-order 380/381); the stun dark matter between them is thrown.
	for (const std::uint16_t d : { 397, 398, 399, 401, 402, 403, 405, 406, 407, 380, 381 }) {
		CHECK(ExplosiveOf(d).use == ExplosiveUse::kDecoy && ExplosiveOf(d).triggerMm == 0 && ExplosiveOf(d).kind == BlastKind::kIgnore);
	}
	CHECK(ExplosiveOf(400).use == ExplosiveUse::kThrown && ExplosiveOf(404).use == ExplosiveUse::kThrown && ExplosiveOf(396).use == ExplosiveUse::kNone);
	const auto arc = ArcVelocity({ 822.002f, -11.094f, 1179.52f }, { 822.877f, -10.769f, 1179.52f }, { 823.752f, -10.499f, 1179.52f });
	CHECK(arc && Near(arc->x, 11.67f, 0.02) && Near(arc->y, 4.70f, 0.03) && Near(arc->z, 0.0f, 1e-3));
	CHECK(!ArcVelocity({ 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 }) && !ArcVelocity({ 0, 0, 0 }, { 1, 1, 0 }, { 1.2f, 1.9f, 0 }));
}

static void TestAmmo()
{
	std::array<SlotMagazine, 3> mags{};
	CHECK(ParseMagazines("36/30/0,54/1/0,7/7/2", mags) && mags[0].equip == 36 && mags[0].rounds == 30 && mags[1].rounds == 1 && mags[2].sub == 2);
	CHECK(!ParseMagazines("36/30/0,54/1/0", mags) && !ParseMagazines("36/30/0,54/1/0,7/7/0 x", mags) && !ParseMagazines("error: x", mags));

	AmmoWatch                      w;
	std::array<std::uint16_t, 6> weapons{ 0x1001, 0x2002, 0x3003, 0, 0, 0 };
	std::array<std::uint16_t, 6> ammo{ 30, 15, 8, 0, 0, 0 };
	CHECK(!w.Update(weapons, ammo));  // primes
	ammo[0] = 29;
	auto s = w.Update(weapons, ammo);
	CHECK(s && s->slot == 0 && s->weapon == 0x1001 && s->rounds == 1);
	ammo[0] = 26;  // a 3-round burst within one frame
	s = w.Update(weapons, ammo);
	CHECK(s && s->rounds == 3);
	ammo[0] = 30;  // reload
	CHECK(!w.Update(weapons, ammo));
	weapons[1] = 0x2fff;  // swapped the secondary: its counter belongs to another gun
	ammo[1] = 3;
	CHECK(!w.Update(weapons, ammo));
	ammo[1] = 2;
	s = w.Update(weapons, ammo);
	CHECK(s && s->slot == 1 && s->weapon == 0x2fff);
	ammo[2] = 0;  // 8 -> 0 in one frame is plausible (shotgun dump? no: > 16 only is rejected)
	s = w.Update(weapons, ammo);
	CHECK(s && s->slot == 2 && s->rounds == 8);
	ammo[0] = 0;  // 30 -> 0 in one frame: an unload/reset, not 30 shots
	CHECK(!w.Update(weapons, ammo));

	CHECK(CounterDrop(100, 99) == 1);
	CHECK(CounterDrop(100, 100) == 0);
	CHECK(CounterDrop(10, 200) == 0);
	CHECK(CounterDrop(500, 0) == 0);
}

// The HUD's main UI target size decays when nothing draws to it any more.
static void TestMainSizeDecay()
{
	MainSizeDecay d;
	for (int i = 0; i < 59; ++i) {
		CHECK(!d.Present(false, true));
	}
	CHECK(!d.Present(true, true));  // one matching draw: still the HUD's target
	for (int i = 0; i < 59; ++i) {
		CHECK(!d.Present(false, true));
		CHECK(!d.Present(false, false));  // presents without UI draws (a menu) change nothing
	}
	CHECK(d.Present(false, true));
	CHECK(!d.Present(false, true));  // counts afresh
}

// D-Walker flamethrower vs gun: decided per GUN_FIRE burst at 60 fps.
static void TestFlameBurst()
{
	// A gun at 10 rounds/s: most frames see no new round, yet not one flame event in the burst.
	FlameBurst    f;
	std::uint64_t rounds = 0, now = 100000;
	int           flames = 0;
	for (int frame = 0; frame < 120; ++frame, now += 16) {
		if (frame % 6 == 5) {
			++rounds;
		}
		flames += f.Update(true, rounds, now);
	}
	CHECK(flames == 0);
	// A flamethrower: no round in 2 s of fire -> nothing for the first 300 ms, then ~8 per second.
	flames = 0;
	f.Update(false, rounds, now);
	const std::uint64_t start = now;
	std::uint64_t       first = 0;
	for (int frame = 0; frame < 125; ++frame, now += 16) {
		if (f.Update(true, rounds, now)) {
			first = first ? first : now;
			++flames;
		}
	}
	CHECK(first - start >= FlameBurst::kSettleMs && first - start < FlameBurst::kSettleMs + 32);
	CHECK(flames >= 12 && flames <= 15);
	// A round late in the burst ends the flame for the rest of it; the next burst decides afresh.
	CHECK(!f.Update(true, rounds + 1, now += 200));
	CHECK(!f.Update(true, rounds + 1, now += 200));
	CHECK(!f.Update(false, rounds + 1, now += 16));
	CHECK(!f.Update(true, rounds + 1, now += 16));
	CHECK(f.Update(true, rounds + 1, now += FlameBurst::kSettleMs));
	// Only the flamethrower burns: a walker gun (any other main weapon type) never feeds a burst, even
	// when none of its rounds reach the counted ShotSolver call.
	CHECK(FlameBurst::Firing(true, true, true, FlameBurst::kFlamethrowerWp));
	CHECK(!FlameBurst::Firing(true, true, true, 1));
	CHECK(!FlameBurst::Firing(true, true, true, 0));
	CHECK(!FlameBurst::Firing(false, true, true, FlameBurst::kFlamethrowerWp));
	CHECK(!FlameBurst::Firing(true, false, true, FlameBurst::kFlamethrowerWp));
	CHECK(!FlameBurst::Firing(true, true, false, FlameBurst::kFlamethrowerWp));
}

static void TestTransforms()
{
	// Identity root facing +Z: yaw 0. Rotated 90 deg about Y (forward = +X): yaw = +pi/2.
	float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 4, 5, 1 };
	CHECK(Near(MatrixYaw(m), 0));
	CHECK(Near(MatrixTranslation(m).x, 3) && Near(MatrixTranslation(m).z, 5));
	float r[16] = { 0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1 };
	CHECK(Near(MatrixYaw(r), std::numbers::pi / 2));
	const auto p = TransformPoint(r, { 0, 0, 1 });  // model forward -> world +X
	CHECK(Near(p.x, 1) && Near(p.z, 0));

	// Quaternion rotation: 90 deg about Y takes +Z to +X.
	const float s = std::sin(std::numbers::pi_v<float> / 4), c = std::cos(std::numbers::pi_v<float> / 4);
	const float q[4] = { 0, s, 0, c };
	const auto  f = RotateByQuat(q, { 0, 0, 1 });
	CHECK(Near(f.x, 1) && Near(f.y, 0) && Near(f.z, 0));
	const float id[4] = { 0, 0, 0, 1 };
	const auto  g = RotateByQuat(id, { 1, 2, 3 });
	CHECK(Near(g.x, 1) && Near(g.y, 2) && Near(g.z, 3));

	// Matrix -> quaternion agrees with RotateByQuat on every basis vector (yaw, pitch+yaw, scaled).
	{
		const float yaw = 0.7f, pitch = -0.3f;
		const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
		// rows = images of x, y, z under R = Ry(yaw) * Rx(pitch), scaled by 1.02 like a Fox root may be
		const float k = 1.02f;
		float m2[16] = { k * cy, 0, -k * sy, 0, k * sy * sp, k * cp, k * cy * sp, 0, k * sy * cp, -k * sp, k * cy * cp, 0, 1, 2, 3, 1 };
		const auto  q2 = MatrixToQuat(m2);
		const Vec3  axes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
		for (int i = 0; i < 3; ++i) {
			const auto a = RotateByQuat(q2.data(), axes[i]);
			CHECK(Near(a.x, m2[i * 4] / k, 1e-4) && Near(a.y, m2[i * 4 + 1] / k, 1e-4) && Near(a.z, m2[i * 4 + 2] / k, 1e-4));
		}
		const auto qi = MatrixToQuat(m);
		CHECK(Near(qi[3], 1) && Near(qi[0], 0) && Near(qi[1], 0) && Near(qi[2], 0));
		const auto qr = MatrixToQuat(r);  // 90 deg about Y
		CHECK(Near(qr[1], std::sin(std::numbers::pi / 4)) && Near(qr[3], std::cos(std::numbers::pi / 4)));
	}

	// The follow spot: facing +X (yaw pi/2, the sandbox's RotY 90) his right is +Z (VERIFIED live: the buddy call spot), so
	// 3 m to his left is -Z; ahead is +X. A quaternion turning +Z to +X has yaw pi/2.
	{
		const auto l = BesideOf({ 820.6f, -11.95f, 1179.1f }, std::numbers::pi_v<float> / 2, 0.0f, -3.0f);
		CHECK(Near(l.x, 820.6, 1e-3) && Near(l.y, -11.95, 1e-3) && Near(l.z, 1176.1, 1e-3));
		const auto r = BesideOf({ 820.6f, -11.95f, 1179.1f }, std::numbers::pi_v<float> / 2, 3.0f, 3.5f);  // the boot call spot
		CHECK(Near(r.x, 823.6, 1e-3) && Near(r.z, 1182.6, 1e-3));
		const auto z = BesideOf({ 0, 0, 0 }, 0.0f, 0.0f, 2.0f);  // facing +Z, the right is -X
		CHECK(Near(z.x, -2) && Near(z.z, 0));
		CHECK(Near(QuatYaw(q), std::numbers::pi / 2));
		CHECK(Near(QuatYaw(id), 0));
		const float bad[4] = { 0, 0, 0, 0 };
		CHECK(QuatYaw(bad) == 0.0f);
		for (const float y : { 0.0f, 1.0f, -2.5f, 3.0f }) {
			CHECK(Near(QuatYaw(YawQuat(y).data()), y, 1e-5));
		}
		// Leading a runner (4.7 m/s along +X, 1.2 s, at most 8 m): 5.64 m on, height kept; standing still: no lead; 8 m cap.
		const auto lead = LeadPoint({ 1, -11, 2 }, { 4.7f, 0.3f, 0 }, 1.2f, 8.0f, 1.0f);
		CHECK(Near(lead.x, 6.64, 1e-3) && Near(lead.y, -11) && Near(lead.z, 2));
		const auto still = LeadPoint({ 1, 0, 2 }, { 0.5f, 0, 0.5f }, 1.2f, 8.0f, 1.0f);
		CHECK(Near(still.x, 1) && Near(still.z, 2));
		const auto capped = LeadPoint({ 0, 0, 0 }, { 0, 0, 9.0f }, 1.2f, 8.0f, 1.0f);
		CHECK(Near(capped.z, 8.0, 1e-4));
	}

	// The host's puppet starts (Placed.cpp re-announces Snake's mines to each one): off, on, on, off, on = 2 starts.
	{
		RisingEdges starts;
		CHECK(starts.Update(false) == 0);
		CHECK(starts.Update(true) == 1);
		CHECK(starts.Update(true) == 1);
		CHECK(starts.Update(false) == 1);
		CHECK(starts.Update(true) == 2);
	}

	// 21 mm (MGSV's normal focal target) on 24 mm film: ~59.5 deg vertical.
	CHECK(Near(FocalToFovY(21.0f, 24.0f) * 180.0 / std::numbers::pi, 59.49, 0.05));
	CHECK(FocalToFovY(0.0f, 24.0f) == 0.0f);
	CHECK(FocalToFovY(NAN, 24.0f) == 0.0f);
}

static void TestIDroidRequest()
{
	using R = IDroidRequest;
	R::Event ev{};
	{  // accepted at once: opened, no override
		R r;
		CHECK(r.Update(0, true, false, true, 3000, ev) == R::Pad::kPass && ev == R::Event::kArmed);
		CHECK(r.Update(60, true, true, true, 3000, ev) == R::Pad::kPass && ev == R::Event::kOpened && r.OpenedAfterMs() == 60 && r.Retries() == 0);
		CHECK(r.Update(100, true, true, true, 3000, ev) == R::Pad::kPass && ev == R::Event::kNone);
		CHECK(r.Update(200, false, true, true, 3000, ev) == R::Pad::kPass && !r.Active());
	}
	{  // refused (aiming): neutral, then Start alone, until it opens
		R r;
		r.Update(0, true, false, true, 3000, ev);
		CHECK(r.Update(R::kAcceptMs - 1, true, false, true, 3000, ev) == R::Pad::kPass);
		CHECK(r.Update(R::kAcceptMs, true, false, true, 3000, ev) == R::Pad::kNeutral);
		CHECK(r.Update(R::kAcceptMs + R::kReleaseMs, true, false, true, 3000, ev) == R::Pad::kNeutralStart && ev == R::Event::kRetry && r.Retries() == 1);
		CHECK(r.Update(R::kAcceptMs + R::kReleaseMs + R::kPressMs, true, false, true, 3000, ev) == R::Pad::kNeutral);
		CHECK(r.Update(R::kAcceptMs + R::kReleaseMs + R::kPressMs + 10, true, true, true, 3000, ev) == R::Pad::kPass && ev == R::Event::kOpened);
		CHECK(r.Update(2000, false, false, true, 3000, ev) == R::Pad::kPass && ev == R::Event::kNone);  // Start up: idle again
		CHECK(r.Update(2100, true, false, true, 3000, ev) == R::Pad::kPass && ev == R::Event::kArmed);
	}
	{  // never opens: given up within the window, then waits for Start to go up
		R r;
		r.Update(0, true, false, true, 1000, ev);
		int retries = 0, expired = 0;
		for (std::uint64_t t = 10; t <= 1500; t += 10) {
			r.Update(t, true, false, true, 1000, ev);
			retries += ev == R::Event::kRetry;
			expired += ev == R::Event::kExpired;
		}
		CHECK(retries >= 1 && expired == 1 && !r.Active());
		CHECK(r.Update(1600, true, false, true, 1000, ev) == R::Pad::kPass && ev == R::Event::kNone);  // still held: no new request
	}
	{  // the host's 1200 ms hold ends while a retry still waits: the retries go on (only an early release is a cut)
		R r;
		r.Update(0, true, false, true, 3000, ev);
		int retries = 0;
		for (std::uint64_t t = 10; t <= 1190; t += 10) {
			r.Update(t, true, false, true, 3000, ev);
			retries += ev == R::Event::kRetry;
		}
		CHECK(r.Update(1200, false, false, true, 3000, ev) != R::Pad::kPass || r.Active());
		CHECK(retries >= 1 && r.Active() && ev != R::Event::kCut);
	}
	{  // ER agent 2026-10-14: the host cut its hold for the View+Start chord (Start up 150 ms into it) while Snake aimed:
	   // no Start of ours afterwards (it opened the iDroid as Tarnished mode began)
		R r;
		r.Update(0, true, false, true, 3000, ev);
		CHECK(r.Update(150, false, false, true, 3000, ev) == R::Pad::kPass && ev == R::Event::kCut && !r.Active());
		bool start = false;
		for (std::uint64_t t = 160; t <= 3500; t += 10) {
			start = start || r.Update(t, false, false, true, 3000, ev) == R::Pad::kNeutralStart;
		}
		CHECK(!start && !r.Active());
		// ... also when it is cut during one of our retries (after the first refusal)
		R q;
		q.Update(0, true, false, true, 3000, ev);
		q.Update(R::kAcceptMs, true, false, true, 3000, ev);
		CHECK(q.Update(R::kAcceptMs + R::kReleaseMs, true, false, true, 3000, ev) == R::Pad::kNeutralStart && q.Retries() == 1);
		CHECK(q.Update(R::kAcceptMs + R::kReleaseMs + 20, false, false, true, 3000, ev) == R::Pad::kPass && ev == R::Event::kCut);
		// The host's own Start tap that closes a menu opened anyway is no request (the menu is open).
		CHECK(q.Update(1000, true, true, true, 3000, ev) == R::Pad::kPass && ev == R::Event::kNone && !q.Active());
	}
	{  // a Start inside a menu, or while not eligible, is no request; losing eligibility drops one
		R r;
		CHECK(r.Update(0, true, true, true, 3000, ev) == R::Pad::kPass && ev == R::Event::kNone && !r.Active());
		R q;
		q.Update(0, true, false, true, 3000, ev);
		CHECK(q.Update(300, true, false, false, 3000, ev) == R::Pad::kPass && ev == R::Event::kDropped);
		R off;
		CHECK(off.Update(0, true, false, true, 0, ev) == R::Pad::kPass && ev == R::Event::kNone && !off.Active());
	}
}

// Which rigid draws are the grenade / rocket Snake just released (mgsv/ProjectileLogic.h, protocol v13 kOwnerProjectile).
static void TestProjectileTag()
{
	using namespace phantom::guest::projectile;
	const TagParams p;
	// A grenade thrown from the hand at 1.5 m, 12 m/s forward and 4 up, in frame 100.
	Release g;
	g.frame = 100;
	g.p0[0] = 0, g.p0[1] = 1.5f, g.p0[2] = 0;
	g.v0[0] = 0, g.v0[1] = 4.0f, g.v0[2] = 12.0f;
	auto at = [&](const Release& a_r, float a_t, float a_dx = 0, float a_dy = 0) {
		static float out[3];
		out[0] = a_r.p0[0] + a_r.v0[0] * a_t + a_dx;
		out[1] = a_r.p0[1] + a_r.v0[1] * a_t - 0.5f * p.gravity * a_t * a_t + a_dy;
		out[2] = a_r.p0[2] + a_r.v0[2] * a_t;
		return out;
	};
	CHECK(Matches(g, 110, at(g, 10.0f / 60), p));           // on its arc 10 frames later
	CHECK(!Matches(g, 106, at(g, 0.1f), p));                // 6 frames: the sphere it may be in still reaches the hand
	CHECK(Matches(g, 112, at(g, 0.2f - 1.0f / 60), p));     // a frame of lag either way
	CHECK(Matches(g, 112, at(g, 0.2f, 0.4f), p));           // within the tolerance
	CHECK(!Matches(g, 112, at(g, 0.2f, 1.5f), p));          // a soldier's rifle 1.5 m to the side
	CHECK(!Matches(g, 101, at(g, 0.0f), p));                // still in the hand (the next grenade, the glove)
	CHECK(!Matches(g, 100, at(g, 0.0f), p) && !Matches(g, 99, at(g, 0.0f), p));  // not before the release
	CHECK(!Matches(g, 100 + p.windowFrames + 1, at(g, (p.windowFrames + 1) / 60.0f), p));  // the window is over
	// Review: in the first frames the "on its arc" sphere around the predicted point is a shell around the hand, and Snake's
	// own rigid gear sat in it (the host learned his pistol as the grenade). Never tagged, at any frame of the window ...
	const float below[3] = { 0, 0.8f, 0 }, side[3] = { 0.65f, 1.5f, 0 }, behind[3] = { 0, 1.5f, -0.65f }, sideAhead[3] = { 0.5f, 1.3f, 0.45f };
	bool gear = false, grenade = false;
	for (std::uint64_t f = 101; f <= 100 + p.windowFrames; ++f) {
		gear = gear || Matches(g, f, below, p) || Matches(g, f, side, p) || Matches(g, f, behind, p) || Matches(g, f, sideAhead, p);
		grenade = grenade || Matches(g, f, at(g, static_cast<float>(f - 100) / 60.0f), p);
	}
	CHECK(!gear && grenade);
	// ... while the grenade itself is tagged from frame 8 on (12.6 m/s: the hand is out of reach of its sphere by then).
	CHECK(!Matches(g, 102, at(g, 2.0f / 60), p) && Matches(g, 108, at(g, 8.0f / 60), p));
	CHECK(!Matches(g, 110, behind, p));  // behind the release: never, however far
	Release empty;
	CHECK(!Matches(empty, 110, at(g, 10.0f / 60), p));
	const float nan[3] = { NAN, 0, 0 };
	CHECK(!Matches(g, 110, nan, p));
	// A rocket from a muzzle at 1.4 m along +x: ahead on its line at any speed, never the tube behind or beside the muzzle.
	Release r;
	r.frame = 500;
	r.round = true;
	r.p0[0] = 0, r.p0[1] = 1.4f, r.p0[2] = 0;
	r.v0[0] = 1, r.v0[1] = 0, r.v0[2] = 0;
	const float ahead[3] = { 3.0f, 1.45f, 0.1f }, far[3] = { 25.0f, 1.4f, 0.0f }, tube[3] = { -0.4f, 1.4f, 0.0f }, aside[3] = { 2.0f, 1.4f, 1.2f };
	CHECK(Matches(r, 503, ahead, p) && Matches(r, 515, far, p));
	CHECK(!Matches(r, 503, tube, p) && !Matches(r, 503, aside, p));
	// Review: not further down the line than the round can have gone (a prop / the walker / a soldier's gun 40 m out was
	// tagged one frame after the shot, before the rocket itself had left the tube).
	const float downRange[3] = { 40.0f, 1.4f, 0.0f };
	CHECK(!Matches(r, 501, downRange, p) && !Matches(r, 503, far, p));
	// A grenade-launcher round falls off its line: allowed by the drop so far -- a little, never the 1.2 m of half a second.
	const float dropped[3] = { 20.0f, 1.4f - 0.5f * p.gravity * 0.25f * 0.25f, 0.0f };
	CHECK(Matches(r, 515, dropped, p));
	const float wide[3] = { 30.0f, 1.4f - 1.2f, 0.0f };
	CHECK(!Matches(r, 530, wide, p));
	r.v0[0] = 0;
	CHECK(!Matches(r, 503, ahead, p));  // no direction: nothing
}

// Capture.cpp's pointer-keyed caches (docs/GUEST.md 15.3): an entry is redone only when the copy it was made from is gone.
namespace copy_test
{
	namespace keys = phantom::guest::capture::keys;

	// The copy table as CaptureAssets.cpp keeps it, by "address": a serial per copy, every request and touching Serial counts
	// as a use, settled copies nobody used for a minute go (keys::Evictable, the rule Evict runs).
	struct FakeCopies
	{
		struct Copy
		{
			std::uint64_t serial{ 0 }, lastUse{ 0 };
			bool          settled{ false };
		};
		std::map<int, Copy>          copies;
		std::map<int, std::uint64_t> content;  // what MGSV has at each address now (the key a copy hashes to)
		std::uint64_t                present{ 0 }, serials{ 0 };

		bool Request(int a_at)  // assets::Texture: true once the bytes are in
		{
			auto [it, fresh] = copies.try_emplace(a_at);
			if (fresh) {
				it->second.serial = ++serials;
			}
			it->second.lastUse = present;
			return it->second.settled;
		}
		std::uint64_t Serial(int a_at, bool a_touch)
		{
			const auto it = copies.find(a_at);
			if (it == copies.end()) {
				return 0;
			}
			if (a_touch) {
				it->second.lastUse = present;
			}
			return it->second.serial;
		}
		void Evict()
		{
			for (auto it = copies.begin(); it != copies.end();) {
				it = keys::Evictable(it->second.settled, it->second.lastUse, present) ? copies.erase(it) : std::next(it);
			}
		}
	};

	// Capture.cpp's TexEntry and EnsureTexture's bookkeeping (minus hashing and the ring).
	struct Tex
	{
		std::uint64_t key{ 0 }, serial{ 0 };
		std::uint32_t sentGeneration{ ~0u };
		bool          bytes{ false };
	};
	struct Capture
	{
		FakeCopies&         c;
		std::map<int, Tex> textures;
		int                 restarted{ 0 }, sent{ 0 };
		std::size_t         pruned{ 0 };

		std::uint64_t Ensure(int a_at, std::uint32_t a_gen)
		{
			auto& t = textures[a_at];
			restarted += keys::Revalidate(t, [&](const Tex& a_t) { return keys::Stale(a_t.serial, c.Serial(a_at, true)); });
			if (!t.bytes) {
				t.bytes = c.Request(a_at);
				t.serial = c.Serial(a_at, false);
				if (!t.bytes) {
					return 0;
				}
				t.key = c.content[a_at];
			}
			if (t.sentGeneration != a_gen) {
				t.sentGeneration = a_gen;
				++sent;
			}
			return t.key;
		}
		// OnPresent: every 600 presents an eviction, then Prune.
		void Frame()
		{
			if (++c.present % 600 == 0) {
				c.Evict();
				pruned += keys::Prune(textures, [&](int a_at, const Tex& a_t) { return keys::Stale(a_t.serial, c.Serial(a_at, false)); });
			}
		}
	};
}

// Capture.cpp's pointer-keyed caches (docs/GUEST.md 15.3): a texture in use keeps its key for good; one let go and replaced by
// another texture at the same address is copied, keyed and sent afresh; entries whose copies went are pruned.
static void TestCopyStale()
{
	using namespace copy_test;
	CHECK(!keys::Stale(0, 0) && !keys::Stale(0, 7));  // nothing kept yet
	CHECK(!keys::Stale(7, 7));                        // the same copy: keep the key
	CHECK(keys::Stale(7, 0) && keys::Stale(7, 9));    // evicted (MGSV may have put another texture there), or evicted and copied again
	CHECK(!keys::Evictable(false, 0, 100000) && !keys::Evictable(true, 100, 100 + keys::kEvictAfterPresents) &&
		  keys::Evictable(true, 100, 101 + keys::kEvictAfterPresents));  // copies in flight never go

	FakeCopies c;
	Capture    cap{ c };
	c.content[1] = 0xD06;  // D-Dog's coat at "address" 1
	c.content[2] = 0x5A1;  // something drawn once, at 2
	CHECK(cap.Ensure(1, 1) == 0 && cap.Ensure(2, 1) == 0);  // copies queued
	c.copies[1].settled = c.copies[2].settled = true;
	CHECK(cap.Ensure(1, 1) == 0xD06 && cap.Ensure(2, 1) == 0x5A1 && cap.sent == 2);
	const auto first = c.Serial(1, false);
	// Three minutes with the coat drawn every frame: never let go (each use touches its copy), never re-keyed or re-sent.
	for (int f = 0; f < 3 * 3600; ++f) {
		cap.Frame();
		CHECK(cap.Ensure(1, 1) == 0xD06);
	}
	CHECK(c.Serial(1, false) == first && cap.restarted == 0 && cap.sent == 2);
	// ... while address 2, unused since, went with its copy and its entry (pruned, not sent again).
	CHECK(c.copies.count(2) == 0 && cap.textures.count(2) == 0 && cap.pruned == 1);

	// The coat not drawn for over a minute: its copy goes. Without a Prune in between, MGSV puts D-Walker's SRM at the same
	// address and the next use restarts the entry: copied anew, keyed by its own content, sent again (sentGeneration reset).
	for (int f = 0; f < 3700; ++f) {
		++c.present;
	}
	c.Evict();
	CHECK(c.copies.count(1) == 0 && cap.textures.count(1) == 1);
	c.content[1] = 0x0A7;
	CHECK(cap.Ensure(1, 1) == 0 && cap.restarted == 1);  // never the old key: nothing until the new copy is in
	CHECK(c.Serial(1, false) != first);
	c.copies[1].settled = true;
	CHECK(cap.Ensure(1, 1) == 0x0A7 && cap.sent == 3);
	CHECK(cap.Ensure(1, 1) == 0x0A7 && cap.restarted == 1 && cap.sent == 3);  // and kept from then on

	// Unused again for over a minute, then pruned at the next eviction round: memory and key gone with the copy.
	for (int f = 0; f < 5000; ++f) {
		cap.Frame();
	}
	CHECK(cap.textures.empty() && cap.pruned == 2 && c.copies.empty());
}

// Snake jumps (protocol v14 kGuestJumping; Pad.cpp and Frame.cpp): the [Jump] button kept from MGSV, and a jump's life in MGSV.
static void TestJump()
{
	constexpr std::uint16_t kR3 = 0x0080, kLb = 0x0100, kRb = 0x0200, kA = 0x1000, kPass = kLb | kRb;
	{  // R3 alone: swallowed for its whole hold, one jump at the press; the other buttons pass
		JumpButton b;
		bool       jump = false;
		CHECK(b.Filter(kA, false, kR3, kPass, true, jump) == kA && !jump);
		CHECK(b.Filter(kA | kR3, false, kR3, kPass, true, jump) == kA && jump);
		CHECK(b.Filter(kA | kR3, false, kR3, kPass, true, jump) == kA && !jump);  // held: no second jump
		CHECK(b.Filter(kR3 | kLb, false, kR3, kPass, true, jump) == kLb && !jump);  // LB later: still the jump's hold
		CHECK(b.Filter(0, false, kR3, kPass, true, jump) == 0 && !jump);
		CHECK(b.Filter(kR3, false, kR3, kPass, true, jump) == 0 && jump);  // a new press
	}
	{  // with LB held (MGSV's call menu: R3 selects), RB (binoculars) or aiming (LT: the shoulder) it is MGSV's, the whole hold
		JumpButton b;
		bool       jump = false;
		CHECK(b.Filter(kLb | kR3, false, kR3, kPass, true, jump) == (kLb | kR3) && !jump);
		CHECK(b.Filter(kR3, false, kR3, kPass, true, jump) == kR3 && !jump);  // LB let go first: still MGSV's
		b.Filter(0, false, kR3, kPass, true, jump);
		CHECK(b.Filter(kRb | kR3, false, kR3, kPass, true, jump) == (kRb | kR3) && !jump);
		b.Filter(0, false, kR3, kPass, true, jump);
		CHECK(b.Filter(kR3, true, kR3, kPass, true, jump) == kR3 && !jump);
		b.Filter(0, false, kR3, kPass, true, jump);
		CHECK(b.Filter(kR3, true, kR3, kPass, false, jump) == 0 && jump);  // PassWith without lt: aiming doesn't matter
		b.Filter(0, false, kR3, kPass, true, jump);
		CHECK(b.Filter(kR3, false, 0, kPass, true, jump) == kR3 && !jump);  // no button configured: nothing kept
	}
	{  // review 2026-10-14: LT lightly pulled (31-64 of 255) is aiming (XInput's threshold, 30), so R3 is MGSV's shoulder change
		CHECK(!AimingTrigger(0) && !AimingTrigger(30) && AimingTrigger(31) && AimingTrigger(40) && AimingTrigger(64) && AimingTrigger(255));
		JumpButton b;
		bool       jump = false;
		CHECK(b.Filter(kR3, AimingTrigger(40), kR3, kPass, true, jump) == kR3 && !jump);
		b.Filter(0, false, kR3, kPass, true, jump);
		// the game thread's word (MGSV's own HOLD, riding, a menu, the binoculars): MGSV's, whatever the pad shows, even with
		// PassWith without lt
		CHECK(b.Filter(kR3, false, kR3, kPass, false, jump, true) == kR3 && !jump);
		CHECK(b.Filter(kR3, false, kR3, kPass, false, jump, false) == kR3 && !jump);  // still that hold
		b.Filter(0, false, kR3, kPass, true, jump, true);
		CHECK(b.Filter(kR3, false, kR3, kPass, true, jump, false) == 0 && jump);  // the word gone: a jump
		// ... fresh for kMgsvOwnsFreshMs only (a game thread that stopped saying it: the pad decides)
		CHECK(MgsvOwnsR3(true, 1000, 1000) && MgsvOwnsR3(true, 1000, 1000 + kMgsvOwnsFreshMs));
		CHECK(!MgsvOwnsR3(true, 1000, 1001 + kMgsvOwnsFreshMs) && !MgsvOwnsR3(false, 1000, 1000) && !MgsvOwnsR3(true, 2000, 1000));
	}
	using E = JumpTracker::Event;
	const auto lift = [](JumpTracker& t, std::uint64_t& now, float y) {
		const auto ev = t.Update(true, true, now, y, false, 2000);
		now += 16;
		return ev;
	};
	{  // lifted 1 m, falls back (MGSV's 1 m drop: ~0.45 s), stands: landed; the flag was up from the press until then
		JumpTracker   t;
		std::uint64_t now = 1000;
		CHECK(t.Update(false, true, now, 5.0f, false, 2000) == E::kNone && !t.Active());
		CHECK(lift(t, now, 5.0f) == E::kLift && t.Active());
		float y = 6.0f, vy = 0.0f;
		int   frames = 0;
		E     ev = E::kNone;
		for (; frames < 200 && ev == E::kNone; ++frames) {
			ev = t.Update(false, true, now, y, false, 2000);
			now += 16;
			vy -= 9.81f * 0.016f;
			y = std::max(5.0f, y + vy * 0.016f);
		}
		CHECK(ev == E::kLanded && !t.Active() && frames > 25 && frames < 40 && Near(t.Rose(), 1.0, 1e-3));
		CHECK(t.Update(true, true, now, 5.0f, false, 2000) == E::kRefused);  // cooling down (200 ms)
		now += 250;
		CHECK(t.Update(true, true, now, 5.0f, false, 2000) == E::kLift);
	}
	{  // MGSV's FALL status came and went after he came down: landed then, even if his root still settles
		JumpTracker   t;
		std::uint64_t now = 1000;
		CHECK(lift(t, now, 0.0f) == E::kLift);
		CHECK(t.Update(false, true, now, 1.0f, true, 2000) == E::kNone);
		now += 16;
		CHECK(t.Update(true, true, now, 0.5f, true, 2000) == E::kRefused && t.Active());  // a press in the air: no second lift
		now += 16;
		CHECK(t.Update(false, true, now, 0.02f, false, 2000) == E::kLanded);
	}
	{  // refused while he can't jump; the lift that never shows ends it at once; no landing seen: the flag goes down anyway
		JumpTracker   t;
		std::uint64_t now = 1000;
		CHECK(t.Update(true, false, now, 0.0f, false, 2000) == E::kRefused && !t.Active());
		CHECK(lift(t, now, 0.0f) == E::kLift);
		E ev = E::kNone;
		for (int i = 0; i < 30 && ev == E::kNone; ++i, now += 16) {
			ev = t.Update(false, true, now, 0.0f, false, 2000);
		}
		CHECK(ev == E::kNoLift && !t.Active());
		now += 500;
		CHECK(lift(t, now, 0.0f) == E::kLift);
		ev = E::kNone;
		for (int i = 0; i < 200 && ev == E::kNone; ++i, now += 16) {
			ev = t.Update(false, true, now, 1.0f, false, 2000);  // up there for good (a ledge in MGSV)
		}
		CHECK(ev == E::kTimedOut && !t.Active());
		now += 500;
		CHECK(lift(t, now, 0.0f) == E::kLift);
		t.Abort();  // the placement failed
		CHECK(!t.Active() && lift(t, now, 0.0f) == E::kLift);  // no cool-down after an abort
	}
}

int main()
{
	TestCopyStale();
	TestIDroidRequest();
	TestJump();
	TestOdometer();
	TestTreadmill();
	TestLife();
	TestAmmo();
	TestFlameBurst();
	TestMainSizeDecay();
	TestTransforms();
	TestCombat();
	TestProjectileTag();
	if (g_failures) {
		std::printf("test_guest_logic: %d FAILED\n", g_failures);
		return 1;
	}
	std::printf("test_guest_logic: all passed\n");
	return 0;
}
