// Offline tests for the host's combat (run with wine): capsule shapes, damage scaling, hit effects,
// sleep ladder, pacing, route monitor, fall damage, the param file parser and, when the extracted
// regulation params are around (tools/re/er_params.py writes them to build/er_params), every param
// offset the combat code patches, against this game's real data. Also: the shipped ini's [Combat]
// section states the built-in defaults.
#include "CombatLogic.h"
#include "ExplosionLogic.h"
#include "Config.h"
#include "Log.h"
#include "Params.h"
#include "Rounds.h"
#include "Stealth.h"

#include "phantomring_protocol.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

using namespace phantom::er;
using namespace phantom::er::combat;
namespace proto = phantom::proto;

static int g_failures = 0;
#define CHECK(x)                                                    \
	do {                                                            \
		if (!(x)) {                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			++g_failures;                                           \
		}                                                           \
	} while (0)

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

#include "test_host_throw.inc"

static void TestShapes()
{
	// A 1.8 m tall, 0.5 m radius character standing at (0, 0, 10).
	const Capsule c = ChrCapsule({ 0, 0, 10 }, 1.8f, 0.5f);
	CHECK(Near(c.a.y, 0.5f) && Near(c.b.y, 1.3f) && Near(c.r, 0.5f));

	// Straight at the chest from the origin: hits the surface at z = 9.5.
	auto t = RayCapsule({ 0, 1, 0 }, { 0, 0, 1 }, 100, c);
	CHECK(t && Near(*t, 9.5f));
	// Too short a ray.
	CHECK(!RayCapsule({ 0, 1, 0 }, { 0, 0, 1 }, 9.0f, c));
	// Over the head: misses; into the head cap: hits the top sphere.
	CHECK(!RayCapsule({ 0, 1.9f, 0 }, { 0, 0, 1 }, 100, c));
	t = RayCapsule({ 0, 1.6f, 0 }, { 0, 0, 1 }, 100, c);
	CHECK(t && *t > 9.5f && *t < 10.0f);
	// Beside it: misses; grazing within the radius: hits.
	CHECK(!RayCapsule({ 0.6f, 1, 0 }, { 0, 0, 1 }, 100, c));
	CHECK(RayCapsule({ 0.45f, 1, 0 }, { 0, 0, 1 }, 100, c).has_value());
	// Pointing away: misses; starting inside: t = 0.
	CHECK(!RayCapsule({ 0, 1, 0 }, { 0, 0, -1 }, 100, c));
	t = RayCapsule({ 0, 1, 10 }, { 1, 0, 0 }, 100, c);
	CHECK(t && Near(*t, 0.0f));
	// From above, straight down onto the head: top of the capsule at y = 1.8.
	t = RayCapsule({ 0, 5, 10 }, { 0, -1, 0 }, 100, c);
	CHECK(t && Near(*t, 3.2f));
	// A diagonal shot: the hit point lies on the surface.
	const Vec3 o{ -3, 2.5f, 7 };
	const Vec3 d = Normalize(Vec3{ 3, -1.2f, 3 });
	t = RayCapsule(o, d, 100, c);
	CHECK(t && Near(CapsuleDistance(c, o + d * *t), 0.0f, 2e-3f));
	// Bad input never hits.
	CHECK(!RayCapsule({ NAN, 0, 0 }, { 0, 0, 1 }, 100, c));

	CHECK(Near(CapsuleDistance(c, { 0, 1, 12 }), 1.5f));
	CHECK(Near(CapsuleDistance(c, { 0, 3, 10 }), 1.2f));
	CHECK(SphereCapsule({ 0, 1, 11.2f }, 0.8f, c));
	CHECK(!SphereCapsule({ 0, 1, 11.4f }, 0.8f, c));

	// Arc in front of Snake at the origin facing +Z.
	const float half = 60.0f * std::numbers::pi_v<float> / 180.0f;
	const Capsule front = ChrCapsule({ 0, 0, 1.5f }, 1.8f, 0.4f);
	const Capsule behind = ChrCapsule({ 0, 0, -1.5f }, 1.8f, 0.4f);
	const Capsule side = ChrCapsule({ 1.5f, 0, 0.2f }, 1.8f, 0.4f);
	const Capsule far = ChrCapsule({ 0, 0, 3.5f }, 1.8f, 0.4f);
	const Vec3    chest{ 0, 1, 0 };
	CHECK(InArc(chest, { 0, 0, 1 }, 1.6f, half, front));
	CHECK(!InArc(chest, { 0, 0, 1 }, 1.6f, half, behind));
	CHECK(!InArc(chest, { 0, 0, 1 }, 1.6f, half, side));
	CHECK(InArc(chest, { 1, 0, 0 }, 1.6f, half, side));
	CHECK(!InArc(chest, { 0, 0, 1 }, 1.6f, half, far));
	CHECK(InArc(chest, { 0, 0, 0 }, 1.6f, half, behind));  // no facing: a sphere test
	CHECK(InArc(chest, { 0, 0, 1 }, 1.6f, half, ChrCapsule({ 0, 0, 0.2f }, 1.8f, 0.4f)));  // standing in it

	// Height fraction: feet 0, top 1.
	CHECK(Near(HeightFraction(c, { 0, 0, 10 }), 0.0f) && Near(HeightFraction(c, { 0, 1.8f, 10 }), 1.0f) && Near(HeightFraction(c, { 0, 0.9f, 10 }), 0.5f));
}

static void TestDamage()
{
	HitRules r;  // defaults = the ini's; the Tarnished's max HP unknown: every enemy is ordinary (reference = its max HP)
	// How big a target is, against the Tarnished's max HP (522): ordinary up to 1.5x, a boss from 5x, log in between.
	HitRules big = r;
	big.scaling.playerMaxHp = 522.0f;
	const auto& s = big.scaling;
	CHECK(Near(Bigness(219, s), 0.0f) && Near(Bigness(783, s), 0.0f) && Near(Bigness(2610, s), 1.0f) && Near(Bigness(50000, s), 1.0f));
	CHECK(Near(Bigness(static_cast<std::int32_t>(std::sqrt(1.5f * 5.0f) * 522.0f), s), 0.5f, 0.01f));
	CHECK(Near(Bigness(5000, r.scaling), 0.0f) && Near(Bigness(0, s), 0.0f));
	// Relative, not absolute: a late-game soldier against a late-game Tarnished is still ordinary.
	HitRules late = r;
	late.scaling.playerMaxHp = 1800.0f;
	CHECK(Near(Bigness(1500, late.scaling), 0.0f));
	// One soldier's life on a target: its max HP when ordinary, max HP / 45 on a boss (a 3000 HP field boss: 67 HP).
	CHECK(Near(ReferenceHp(219, s), 219.0f) && Near(ReferenceHp(3000, s), 3000.0f / 45.0f, 0.01f) && ReferenceHp(0, s) == 0.0f);
	CHECK(Near(LivesOfUnits(17.0f, s), 0.425f) && Near(LivesOfUnits(-5.0f, s), 0.0f));
	CHECK(Near(HeadshotMult(219, big), 4.5f) && Near(HeadshotMult(3000, big), 1.5f));

	// The class table on a 400 HP soldier: a handgun 13 units = 0.325 of a life (130 HP: 4 shots), a rifle 0.425 (3),
	// a sniper round one.
	auto e = ShotEffect(RoundClass::kHandgun, proto::kAmmoLethal, 1, 0.5f, 400, false, r);
	CHECK(e.damage == 130 && !e.headshot && !e.shared && e.sleep == 0 && e.dmgLevel == 1 && Near(e.poise, 8.0f));
	CHECK(e.damage * 4 >= 400 && e.damage * 3 < 400);
	const auto rifle = ShotEffect(RoundClass::kRifle, proto::kAmmoLethal, 1, 0.5f, 400, false, r);
	CHECK(rifle.damage == 170 && rifle.damage * 3 >= 400 && rifle.damage * 2 < 400);
	CHECK(ShotEffect(RoundClass::kSniper, proto::kAmmoLethal, 1, 0.5f, 400, false, r).damage >= 400);
	// Wherever he stands: the same number of shots on a 4000 HP soldier.
	const auto strong4k = ShotEffect(RoundClass::kRifle, proto::kAmmoLethal, 1, 0.5f, 4000, false, r);
	CHECK(strong4k.damage == 1700);
	// Headshot: x4.5 on an ordinary enemy (one rifle round kills) and a stronger flinch.
	const auto head = ShotEffect(RoundClass::kHandgun, proto::kAmmoLethal, 1, 0.9f, 400, false, r);
	CHECK(head.headshot && head.damage == 585 && head.dmgLevel >= 2);
	// MGSV's own share (v8) wins over the table: 0.25 of a life = 100 of 400, x4.5 to the head.
	const auto shared = ShotEffect(RoundClass::kRifle, proto::kAmmoLethal, 1, 0.5f, 400, false, r, 0.25f);
	CHECK(shared.shared && shared.damage == 100);
	CHECK(ShotEffect(RoundClass::kRifle, proto::kAmmoLethal, 1, 0.9f, 400, false, r, 0.25f).damage == 450);
	CHECK(ShotEffect(RoundClass::kRifle, proto::kAmmoLethal, 1, 0.5f, 400, false, r, NAN).damage == 170);  // garbage: the table
	CHECK(!ShotEffect(RoundClass::kExplosive, proto::kAmmoLethal, 1, 0.5f, 400, false, r, 0.25f).shared);
	// A field boss (3000 HP against a 522 HP Tarnished): an assault-rifle magazine (30) with MGSV's own share (0.2115) takes
	// 10-20% off it (live, Margit 4174 HP: 9.8% at the old boss_lives 70, so 45 now), headshots x1.5; the class table's
	// rifle (0.425 of a life, for a guest that sends no share) twice that.
	const auto bossShare = ShotEffect(RoundClass::kRifle, proto::kAmmoLethal, 1, 0.5f, 3000, false, big, 0.2115f);
	CHECK(bossShare.damage == 14 && bossShare.damage * 30 >= 300 && bossShare.damage * 30 <= 600);
	const auto boss = ShotEffect(RoundClass::kRifle, proto::kAmmoLethal, 1, 0.5f, 3000, false, big);
	CHECK(boss.damage == 28);
	CHECK(ShotEffect(RoundClass::kRifle, proto::kAmmoLethal, 1, 0.9f, 3000, false, big).damage == 43);
	// MGSV's headshots kill a soldier: the assault rifle's own share (0.2115) x 4.5 = 0.95 of 219 HP left him at 11 (live, session 5);
	// at least 1.05 lives now -- on an ordinary enemy only (the boss above keeps its x1.5: 43).
	const auto arHead = ShotEffect(RoundClass::kRifle, proto::kAmmoLethal, 1, 0.9f, 219, false, big, 0.2115f);
	CHECK(arHead.headshot && arHead.damage >= 219 && arHead.damage == 230);
	CHECK(ShotEffect(RoundClass::kRifle, proto::kAmmoLethal, 1, 0.5f, 219, false, big, 0.2115f).damage == 46);  // body: 5 hits
	CHECK(ShotEffect(RoundClass::kStun, proto::kAmmoStun, 1, 0.9f, 219, false, big).damage < 219);  // stun rounds never kill
	// Buckshot: per pellet times pellets (6 units x 8 = 1.2 lives).
	const auto buck = ShotEffect(RoundClass::kShotgunPellet, proto::kAmmoLethal, 8, 0.5f, 400, false, r);
	CHECK(buck.damage == 480 && Near(buck.poise, 32.0f) && buck.dmgLevel == 2);
	// Tranquilizer: sleep only (body / head), whatever the class table says.
	auto tq = ShotEffect(RoundClass::kHandgun, proto::kAmmoTranq, 1, 0.5f, 300, false, r);
	CHECK(tq.damage == 0 && tq.poise == 0 && tq.sleep == 80);
	tq = ShotEffect(RoundClass::kTranq, proto::kAmmoLethal, 1, 0.95f, 300, false, r, 0.5f);
	CHECK(tq.sleep == 200 && tq.damage == 0);
	// Stun: a token of damage (1 unit), heavy poise, the big flinch, no headshot multiplier, no share.
	const auto stun = ShotEffect(RoundClass::kUnknown, proto::kAmmoStun, 1, 0.95f, 400, false, r, 0.5f);
	CHECK(stun.damage == 10 && Near(stun.poise, 40.0f) && stun.dmgLevel == 3 && !stun.shared);
	// Smoke & co: nothing.
	const auto none = ShotEffect(RoundClass::kNone, proto::kAmmoLethal, 1, 0.5f, 300, false, r);
	CHECK(none.damage == 0 && none.poise == 0 && none.sleep == 0);
	// Unknown rounds (ammo-counter fallback) use the default lethal value; buddy multiplier; cap.
	CHECK(ShotEffect(RoundClass::kUnknown, proto::kAmmoLethal, 1, 0.5f, 400, false, r).damage == 150);
	HitRules strong = r;
	strong.buddyMult = 2.0f;
	strong.maxDamage = 100;
	CHECK(ShotEffect(RoundClass::kUnknown, proto::kAmmoLethal, 1, 0.5f, 400, true, strong).damage == 100);
	CHECK(ShotEffect(RoundClass::kHandgun, proto::kAmmoLethal, 1, 0.5f, 400, true, strong).damage == 100);
	strong.maxDamage = 9999;
	CHECK(ShotEffect(RoundClass::kHandgun, proto::kAmmoLethal, 1, 0.5f, 400, true, strong).damage == 260);
	// Silly pellet counts are clamped.
	CHECK(ShotEffect(RoundClass::kShotgunPellet, proto::kAmmoLethal, 1000, 0.5f, 300, false, r).damage ==
	      ShotEffect(RoundClass::kShotgunPellet, proto::kAmmoLethal, 32, 0.5f, 300, false, r).damage);

	// CQC.
	const MeleeValues mv;
	const auto        punch = MeleeEffect(proto::kMeleePunch, 400, mv, r);
	const auto        kick = MeleeEffect(proto::kMeleeKick, 400, mv, r);
	const auto        thr = MeleeEffect(proto::kMeleeThrow, 400, mv, r);
	const auto        choke = MeleeEffect(proto::kMeleeChoke, 400, mv, r);
	CHECK(punch.damage == 50 && kick.damage == 70 && kick.poise > punch.poise);  // 5 and 7 units of a 40-unit life
	CHECK(thr.dmgLevel == 3 && Near(thr.poise, 80.0f) && thr.damage > 0);
	CHECK(choke.damage == 0 && choke.sleep == 200);

	// Explosions: full at the centre, 25% at the edge, nothing outside.
	const auto e0 = ExplosionEffect(0.0f, 4.0f, 400, r);
	const auto e4 = ExplosionEffect(4.0f, 4.0f, 400, r);
	CHECK(e0.damage == 700 && e0.dmgLevel == 3);  // 70 units = 1.75 lives at the centre
	CHECK(e4.damage == 175);
	CHECK(ExplosionEffect(4.1f, 4.0f, 300, r).damage == 0 && ExplosionEffect(1.0f, 0.0f, 300, r).damage == 0);

	// Bullet classes (re-backlog M3 table) agree with the guest's kinds for the non-lethal ones.
	CHECK(ClassOfBullet(10) == RoundClass::kTranq && ClassOfBullet(49) == RoundClass::kTranq && ClassOfBullet(98) == RoundClass::kTranq);
	CHECK(ClassOfBullet(7) == RoundClass::kStun && ClassOfBullet(22) == RoundClass::kStun && ClassOfBullet(244) == RoundClass::kStun);
	CHECK(ClassOfBullet(24) == RoundClass::kNone && ClassOfBullet(105) == RoundClass::kNone);
	CHECK(ClassOfBullet(23) == RoundClass::kExplosive && ClassOfBullet(50) == RoundClass::kExplosive && ClassOfBullet(80) == RoundClass::kExplosive);
	CHECK(ClassOfBullet(5) == RoundClass::kHandgun && ClassOfBullet(8) == RoundClass::kMagnum && ClassOfBullet(15) == RoundClass::kRifle);
	CHECK(ClassOfBullet(16) == RoundClass::kBattleRifle && ClassOfBullet(38) == RoundClass::kSniper && ClassOfBullet(18) == RoundClass::kShotgunPellet);
	CHECK(ClassOfBullet(20) == RoundClass::kSlug && ClassOfBullet(41) == RoundClass::kMachineGun && ClassOfBullet(0) == RoundClass::kUnknown);
	CHECK(ClassOfBullet(9999) == RoundClass::kUnknown && Name(RoundClass::kSniper) == "sniper");
}

// Elden Ring hits -> Snake, and D-Walker's flame.
static void TestHurtAndFlame()
{
	// Share: a Limgrave soldier's 44 on a 522 HP Tarnished costs Snake 8.4% of 7801 (life scale 4 on the guest's side).
	const auto share = combat::HurtForGuest(44, 522, 7801, 4.0f, true, 1.0f);
	CHECK(share >= 164 && share <= 165);  // x 4 = ~657 life
	CHECK(combat::HurtForGuest(44, 522, 7801, 4.0f, false, 1.0f) == 44);
	CHECK(combat::HurtForGuest(44, 522, 7801, 4.0f, true, 2.0f) >= 328);
	CHECK(combat::HurtForGuest(0, 522, 7801, 4.0f, true, 1.0f) == 0);
	CHECK(combat::HurtForGuest(1, 5000, 100, 4.0f, true, 1.0f) == 1);  // any hit is at least 1
	// HP bar mirror and heals (er/Puppet.cpp)
	CHECK(combat::MirrorHp(7801, 7801, 522) == 522);
	CHECK(combat::MirrorHp(3900, 7801, 522) == 261);
	CHECK(combat::MirrorHp(1, 7801, 522) == 1);   // alive: never 0 (that is Elden Ring's death)
	CHECK(combat::MirrorHp(0, 7801, 522) == 1);
	CHECK(combat::MirrorHp(5, 0, 522) == 522);    // no life known: full
	{
		double carry = 0;
		const auto flask = combat::HealForGuest(261, 522, 7801, 4.0f, carry);  // half the bar = half Snake's life
		CHECK(flask >= 975 && flask <= 976);
		carry = 0;
		std::uint32_t ticks = 0;
		for (int i = 0; i < 100; ++i) {
			ticks += combat::HealForGuest(1, 522, 7801, 4.0f, carry);  // regeneration: 1 HP at a time adds up
		}
		CHECK(ticks >= 373 && ticks <= 374);
		CHECK(combat::HealForGuest(0, 522, 7801, 4.0f, carry) == 0);
	}
	CHECK(combat::HurtForGuest(44, 0, 7801, 4.0f, true, 1.0f) == 44);   // no max HP: raw
	// A full-HP hit kills: 522 of 522 -> all of Snake's life.
	CHECK(combat::HurtForGuest(522, 522, 7801, 4.0f, true, 1.0f) * 4 >= 7800);

	// Flame: fire damage, no headshots, small poise, ~2-3 s to burn a 219 HP soldier at 8 events a second.
	const HitRules r;
	const auto     f = combat::ShotEffect(RoundClass::kFlame, phantom::proto::kAmmoLethal, 1, 0.95f, 219, true, r);
	CHECK(f.fire && !f.headshot && f.damage > 0 && f.poise > 0);
	CHECK(219 / f.damage >= 12 && 219 / f.damage <= 30);
	CHECK(combat::Name(RoundClass::kFlame) == "flame");
	const auto b = combat::ShotEffect(RoundClass::kRifle, phantom::proto::kAmmoLethal, 1, 0.5f, 219, false, r);
	CHECK(!b.fire);

	// Top-up (live numbers: ER deals ~42 of any bullet on a Gatefront soldier, 5 through its shield).
	combat::TopUp t;
	CHECK(t.Extra(43114010, 376, 42) == 334);  // first hit: the reference, all the rest added
	CHECK(t.Reference(43114010) == 42);
	CHECK(t.Extra(43114010, 376, 5) == 40);    // blocked: 5/42 of 376 in all
	CHECK(t.Extra(43114010, 30, 42) == 0);     // ER already dealt more than we wanted
	CHECK(t.Extra(43114010, 376, 0) == 0);     // nothing landed: nothing added
	CHECK(t.Extra(43114010, 376, 48) == 328);  // a stronger hit raises the reference
	CHECK(t.Reference(43114010) == 48);
	CHECK(t.Extra(43114010, 376, 30) == 346);  // 30 of 48: ER's own variance, not a guard -- all through
	CHECK(t.Extra(60100010, 100, 10) == 90);   // per kind of enemy
	// Small wants (stun rounds) fly with a reduced correction, never a full weapon hit.
	CHECK(t.Correction(43114010, 376) == 100);
	CHECK(t.Correction(43114010, 6) == 13);    // 6 of 48
	CHECK(t.Correction(99999999, 6) == 15);    // no reference yet: a guess
	CHECK(t.Correction(99999999, 60) == 100);
	CHECK(t.Extra(43114010, 6, 2, 13) == 4);   // a reduced hit is taken as landed, the reference untouched
	CHECK(t.Reference(43114010) == 48);

	// One patched row for every hit bullet: a bullet flies only next to bullets with the same numbers, and only one
	// with damage per target (else two overlapping bullets inflate the reference and later hits look guarded).
	int        soldier = 0, knight = 0;
	const auto rifle = combat::HitKey(376, 100, false, 30.0f, 1);
	const auto flame = combat::HitKey(376, 100, true, 30.0f, 1);
	CHECK(rifle != flame);
	CHECK(rifle != combat::HitKey(376, 13, false, 30.0f, 1));
	CHECK(rifle != combat::HitKey(375, 100, false, 30.0f, 1));
	CHECK(rifle != combat::HitKey(376, 100, false, 31.0f, 1));
	CHECK(rifle != combat::HitKey(376, 100, false, 30.0f, 2));
	std::vector<combat::InFlight> flying;
	CHECK(combat::MayFly(flying, &soldier, rifle, true));
	flying.push_back({ &soldier, rifle, true });
	CHECK(!combat::MayFly(flying, &soldier, rifle, true));    // same target: wait (HP route)
	CHECK(combat::MayFly(flying, &knight, rifle, true));      // another target, same numbers: fine
	CHECK(!combat::MayFly(flying, &knight, flame, true));     // the flame would re-patch the rifle's row
	CHECK(combat::MayFly(flying, &soldier, rifle, false));    // poise only on the same target: no HP to misattribute
	flying.push_back({ &knight, rifle, false });
	CHECK(!combat::MayFly(flying, &knight, flame, false));
}

static void TestSleepAndPacing()
{
	// ER's ladder: 25000 + n - 1 adds n * 10 sleep build-up (n = 1..20).
	CHECK(SleepSpEffect(80, 25000, 10, 20) == 25007);
	CHECK(SleepSpEffect(200, 25000, 10, 20) == 25019);
	CHECK(SleepSpEffect(15, 25000, 10, 20) == 25001);
	CHECK(SleepSpEffect(1, 25000, 10, 20) == 25000);
	CHECK(SleepSpEffect(5000, 25000, 10, 20) == 25019);
	CHECK(SleepSpEffect(0, 25000, 10, 20) == -1 && SleepSpEffect(50, 25000, 0, 20) == -1);

	// Token bucket: a burst of 3, then 2 per second.
	RateLimiter limiter(2.0f, 3.0f);
	CHECK(limiter.Allow(0) && limiter.Allow(0) && limiter.Allow(0));
	CHECK(!limiter.Allow(10) && limiter.Dropped() == 1);
	CHECK(limiter.Allow(510));   // +1 token after 0.5 s
	CHECK(!limiter.Allow(520));
	CHECK(limiter.Allow(5000) && limiter.Allow(5000) && limiter.Allow(5000) && !limiter.Allow(5000));  // refills to the burst only

	// Route monitor: misses get the fallback until 3 in a row fail the route; a landed hit confirms it.
	RouteMonitor m(3);
	CHECK(m.Current() == RouteMonitor::State::kUnknown);
	CHECK(m.Report(false) && m.Report(false));
	CHECK(!m.Report(true) && m.Current() == RouteMonitor::State::kConfirmed);
	CHECK(!m.Report(false) && !m.Report(false) && !m.Report(false) && !m.Report(false));
	CHECK(m.Current() == RouteMonitor::State::kConfirmed && m.Landed() == 1 && m.Missed() == 6);
	RouteMonitor f(3);
	CHECK(f.Report(false) && f.Report(false) && f.Report(false));
	CHECK(f.Current() == RouteMonitor::State::kFailed && !f.Report(false));
	f.Reset();
	CHECK(f.Current() == RouteMonitor::State::kUnknown && f.Missed() == 0);
	RouteMonitor g(3);
	CHECK(g.Report(false) && g.Report(false) && !g.Report(true) && g.Current() == RouteMonitor::State::kConfirmed);  // streak resets

	// Fall damage: nothing below 12.5 m/s, at least 10% once it hurts, lethal at 19.8 m/s.
	CHECK(FallFraction(12.0f, 12.5f, 19.8f) == 0.0f);
	CHECK(Near(FallFraction(12.6f, 12.5f, 19.8f), 0.1f));
	CHECK(Near(FallFraction(16.15f, 12.5f, 19.8f), 0.5f));
	CHECK(FallFraction(25.0f, 12.5f, 19.8f) == 1.0f);
	// The amount is in ER units: x guest life scale (4) it costs that share of lifeMax; lethal kills.
	CHECK(FallHurtAmount(0.5f, 6000, 4.0f) == 750);
	CHECK(FallHurtAmount(1.0f, 6000, 4.0f) * 4 > 6000);
	CHECK(FallHurtAmount(0.0f, 6000, 4.0f) == 0 && FallHurtAmount(0.5f, 0, 4.0f) == 0);
}

// A synthetic PARAM file: header, 3 row descriptors, rows of 0x20 bytes, struct name by offset.
static std::vector<std::uint8_t> FakeParam()
{
	std::vector<std::uint8_t> f(0x200, 0);
	const std::uint16_t       rows = 3;
	std::memcpy(f.data() + 0xA, &rows, 2);
	f[0x2D] = 0x85;  // name by offset | 64-bit | ...
	const std::uint32_t nameOffset = 0x180;
	std::memcpy(f.data() + 0x10, &nameOffset, 4);
	std::memcpy(f.data() + nameOffset, "TEST_PARAM_ST", 14);
	const std::uint32_t ids[] = { 10, 20, 999 };
	for (int i = 0; i < 3; ++i) {
		const std::uint64_t data = 0xA0 + 0x20 * i;
		std::memcpy(f.data() + 0x40 + 0x18 * i, &ids[i], 4);
		std::memcpy(f.data() + 0x40 + 0x18 * i + 8, &data, 8);
		f[data] = static_cast<std::uint8_t>(i + 1);
	}
	return f;
}

static std::vector<std::uint8_t> ReadFile(const std::string& a_path)
{
	std::ifstream in(a_path, std::ios::binary);
	return in ? std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) : std::vector<std::uint8_t>{};
}

static const std::uint8_t* RowOf(const std::vector<std::uint8_t>& a_file, const params::ParsedFile& a_p, std::uint32_t a_id)
{
	for (const auto& [id, offset] : a_p.rows) {
		if (id == a_id && offset < a_file.size()) {
			return a_file.data() + offset;
		}
	}
	return nullptr;
}

template <class T>
static T At(const std::uint8_t* a_row, std::uint32_t a_offset)
{
	T v{};
	std::memcpy(&v, a_row + a_offset, sizeof(T));
	return v;
}

// ---- explosion visuals (er/ExplosionLogic) --------------------------------------------------------------
static std::array<std::uint8_t, params::kBullet.rowSize> BulletRow(std::int32_t a_atk, std::int32_t a_sfx, std::int32_t a_hit = -1)
{
	std::array<std::uint8_t, params::kBullet.rowSize> r{};
	const std::int32_t                                minus1 = -1;
	std::memcpy(r.data() + params::bullet::kAtkId, &a_atk, 4);
	std::memcpy(r.data() + params::bullet::kSfxIdBullet, &a_sfx, 4);
	std::memcpy(r.data() + params::bullet::kHitBulletId, &a_hit, 4);
	std::memcpy(r.data() + params::bullet::kIntervalCreateBulletId, &minus1, 4);
	return r;
}

static void TestExplosionLogic()
{
	using namespace phantom::er::explosion;
	// Kinds: MGSV's ammo kinds of a kGEvExplosion, console names.
	CHECK(FromAmmo(proto::kAmmoLethal) == Kind::kLethal && FromAmmo(proto::kAmmoTranq) == Kind::kSleep && FromAmmo(proto::kAmmoStun) == Kind::kStun &&
	      FromAmmo(proto::kAmmoSmoke) == Kind::kSmoke && !FromAmmo(99));
	CHECK(FromName("fire") == Kind::kLethal && FromName("flash") == Kind::kStun && FromName("sleep") == Kind::kSleep && FromName("smoke") == Kind::kSmoke &&
	      !FromName("nuke"));
	for (std::size_t k = 0; k < kKinds; ++k) {
		CHECK(FromName(Name(static_cast<Kind>(k))) == static_cast<Kind>(k));
	}
	CHECK(IsCloud(Kind::kSleep) && IsCloud(Kind::kSmoke) && !IsCloud(Kind::kLethal) && !IsCloud(Kind::kStun));
	CHECK(ParseMode("auto") == Mode::kAuto && ParseMode("bullet") == Mode::kBullet && ParseMode("sfx") == Mode::kSfx && ParseMode("off") == Mode::kOff &&
	      !ParseMode("on"));

	// Ini values.
	auto l = ParseLook("10030001");
	CHECK(l && l->templateId == 10030001 && l->sfx == -1);
	l = ParseLook("10033001 300192");
	CHECK(l && l->templateId == 10033001 && l->sfx == 300192);
	l = ParseLook(" 10033001, 300192 ");
	CHECK(l && l->sfx == 300192);
	CHECK(!ParseLook("") && !ParseLook("abc") && !ParseLook("1 2 3") && !ParseLook("-5") && !ParseLook("10030001 x"));
	auto ids = ParseIds("10048000, 10048001, 10302000, 10302001");  // an ini from before the flame and breaker kinds
	CHECK(ids && ids->size() == 4 && (*ids)[3] == 10302001);
	ids = ParseIds("10048000, 10048001, 10302000, 10302001, 10302002, 10302003, 10302004");
	CHECK(ids && ids->size() == kKinds && (*ids)[6] == 10302004);
	CHECK(!ParseIds("1, 2, 3") && !ParseIds("1, 2, 3, 3") && !ParseIds("1 2 3 4 5 6 7 8") && !ParseIds("1, 2, 0, 4"));
	CHECK(IsBreaker(Kind::kBreaker) && IsBreaker(Kind::kShrapnel) && !IsBreaker(Kind::kFlame) && !IsCloud(Kind::kFlame) && !IsCloud(Kind::kBreaker));

	// Burst size from the hit radius; layout.
	CHECK(Near(NativeRadius(1.6f, 0.0f), 1.6f) && Near(NativeRadius(1.0f, 4.0f), 4.0f) && Near(NativeRadius(0.05f, 15.0f), 6.0f) &&
	      Near(NativeRadius(0.08f, -1.0f), 1.0f) && Near(NativeRadius(NAN, NAN), 1.0f));
	CHECK(Layout(1.5f, 1.6f, 7, 0).size() == 1);   // a pot-sized blast: one burst
	CHECK(Layout(2.0f, 1.6f, 7, 0).size() == 1);   // within 1.25 x one burst
	CHECK(Layout(4.0f, 1.6f, 1, 0).size() == 1 && Layout(4.0f, 1.6f, 3, 0).size() == 1);  // no room for a ring
	CHECK(Layout(NAN, 1.6f, 7, 0).size() == 1 && Layout(-3.0f, 1.6f, 7, 0).size() == 1);
	auto ring = Layout(4.0f, 1.6f, 7, 0.3f);       // a rocket (explosion_radius 4): centre + 6
	CHECK(ring.size() == 7 && Near(Length(ring[0]), 0.0f));
	for (std::size_t i = 1; i < ring.size(); ++i) {
		CHECK(Near(ring[i].y, 0.0f) && Length(ring[i]) <= 2.5f && Length(ring[i]) >= 4.0f - 1.6f);  // 6 can't go round 3 m: pulled in
	}
	for (const auto& o : Layout(8.0f, 1.4f, 7, 0)) {  // MGSV smoke (8 m) with the Fetid Pot's 1.4 m cloud: a tight cluster
		CHECK(Length(o) <= 2.2f);
	}
	CHECK(Layout(3.0f, 1.6f, 16, 0).size() == 1 + 5);  // ring at 2.04 m, one burst per 2.56 m of it: 5
	const auto big = Layout(9.0f, 4.0f, 7, 0);     // MGSV sleep gas (9 m) with the 4 m mist: a ring of 6
	CHECK(big.size() == 7);
	CHECK(Layout(4.5f, 4.0f, 7, 0).size() == 1);   // 4.5 <= 5: one mist

	// Clouds: one burst per repeat while MGSV keeps reporting, per spot and kind.
	CloudGate g;
	CHECK(g.Allow(Kind::kSleep, { 0, 0, 0 }, 1000, 2500));
	CHECK(!g.Allow(Kind::kSleep, { 0.5f, 0, 0.5f }, 1500, 2500));  // MGSV's next puff, same cloud
	CHECK(g.Allow(Kind::kSmoke, { 0, 0, 0 }, 1500, 2500));        // another kind
	CHECK(g.Allow(Kind::kSleep, { 10, 0, 0 }, 1500, 2500));       // another cloud
	CHECK(!g.Allow(Kind::kSleep, { 0, 0, 0 }, 3400, 2500));
	CHECK(g.Allow(Kind::kSleep, { 0, 0, 0 }, 3500, 2500));        // refreshed
	CHECK(g.Allow(Kind::kLethal, { 0, 0, 0 }, 3500, 0) && g.Allow(Kind::kLethal, { 0, 0, 0 }, 3501, 0));  // no repeat: always
	CHECK(g.Allow(Kind::kSleep, { 0, 0, 0 }, 20000, 2500) && g.Size() == 1);  // the others were forgotten

	// Rows: what hurts, a carrier, a chain.
	auto fire = BulletRow(10030001, 300122, 10030002);
	CHECK(Harmful(fire.data()) && FlightSfx(fire.data()) == 300122);
	auto mist = BulletRow(0, 300212);
	CHECK(!Harmful(mist.data()));
	const std::int32_t sleepSp = 500640, shooterSp = 3121;
	std::memcpy(mist.data() + params::bullet::kSpEffectId0, &sleepSp, 4);
	CHECK(Harmful(mist.data()));
	mist[params::bullet::kCreateLimitGroupId] = 37;
	MakeCarrier(mist.data(), -1, true);
	CHECK(!Harmful(mist.data()) && FlightSfx(mist.data()) == 300212 && mist[params::bullet::kCreateLimitGroupId] == 0);
	auto fetid = BulletRow(10033001, -1);
	std::memcpy(fetid.data() + params::bullet::kSpEffectIdForShooter, &shooterSp, 4);
	CHECK(Harmful(fetid.data()) && FlightSfx(fetid.data()) == -1);
	MakeCarrier(fetid.data(), 300192, false);
	CHECK(!Harmful(fetid.data()) && FlightSfx(fetid.data()) == 300192);
	MakeCarrier(fire.data(), -1, false);
	std::int32_t hit = 0;
	std::memcpy(&hit, fire.data() + params::bullet::kHitBulletId, 4);
	CHECK(!Harmful(fire.data()) && hit == -1 && FlightSfx(fire.data()) == 300122);

	auto a = BulletRow(1, 1, 2), b = BulletRow(0, 2, 3), c = BulletRow(0, 3, 2), bad = BulletRow(77, 4);
	auto rows = [&](std::uint32_t id) -> const std::uint8_t* { return id == 2 ? b.data() : id == 3 ? c.data() : id == 4 ? bad.data() : nullptr; };
	auto chain = WalkChain(a.data(), rows);
	CHECK(chain.harmless && chain.complete && chain.ids.size() == 2);  // 2 -> 3 -> 2 (a loop) walked once
	auto d = BulletRow(1, 1, 4);
	chain = WalkChain(d.data(), rows);
	CHECK(!chain.harmless && chain.ids.size() == 1);
	auto e = BulletRow(1, 1, 9);
	chain = WalkChain(e.data(), rows);
	CHECK(!chain.complete);

	// The breaker: the flying pot's row without a look, a sound or children, flying 25 m/s with our attack only.
	auto pot = BulletRow(10030000, 300100, 10030001);
	const std::int32_t potSp = 1234;
	std::memcpy(pot.data() + params::bullet::kSpEffectId0, &potSp, 4);
	MakeBreaker(pot.data(), 10048000, 0.15f, 0.048f, 25.0f);
	std::memcpy(&hit, pot.data() + params::bullet::kHitBulletId, 4);
	CHECK(OnlyAttack(pot.data(), 10048000) && !OnlyAttack(pot.data(), 10030000) && FlightSfx(pot.data()) == -1 && hit == -1);
	CHECK(At<std::int32_t>(pot.data(), params::bullet::kSfxIdHit) == -1 && At<std::int32_t>(pot.data(), params::bullet::kSeIdHit) == -1);
	CHECK(Near(At<float>(pot.data(), params::bullet::kHitRadius), 0.15f) && Near(At<float>(pot.data(), params::bullet::kLife), 0.048f) &&
	      Near(At<float>(pot.data(), params::bullet::kInitVelocity), 25.0f));
	MakeBreaker(pot.data(), 10048001, 100.0f, 9.0f, NAN);  // clamped
	CHECK(OnlyAttack(pot.data(), 10048001) && Near(At<float>(pot.data(), params::bullet::kHitRadius), 8.0f) &&
	      Near(At<float>(pot.data(), params::bullet::kLife), 1.0f) && Near(At<float>(pot.data(), params::bullet::kInitVelocity), 0.0f));
	// The flame: one flame per spawn (a breath row is an emitter: live, it went on shooting for minutes).
	auto breath = BulletRow(10700000, 600000);
	const std::uint16_t five = 5;
	const float         interval = 0.2f;
	std::memcpy(breath.data() + params::bullet::kNumShoot, &five, 2);
	std::memcpy(breath.data() + params::bullet::kShootInterval, &interval, 4);
	MakeSingleShot(breath.data(), 14.0f);
	CHECK(At<std::uint16_t>(breath.data(), params::bullet::kNumShoot) == 1 && At<float>(breath.data(), params::bullet::kShootInterval) == 0.0f &&
	      Near(At<float>(breath.data(), params::bullet::kInitVelocity), 14.0f));
	MakeSingleShot(breath.data(), 0.0f);  // 0: the row's own speed
	CHECK(Near(At<float>(breath.data(), params::bullet::kInitVelocity), 14.0f));

	// The object-only attack: nothing a character could feel, atk_obj ours, the oppose bit as asked.
	std::array<std::uint8_t, params::kAtkParamPc.rowSize> atk{};
	const std::uint16_t phys = 120, stam = 30;
	const std::int32_t  sp = 6400;
	const float         armor = 25.0f;
	std::memcpy(atk.data() + params::atk::kAtkPhys, &phys, 2);
	std::memcpy(atk.data() + params::atk::kAtkStam, &stam, 2);
	std::memcpy(atk.data() + params::atk::kSpEffectId0, &sp, 4);
	std::memcpy(atk.data() + params::atk::kAtkSuperArmor, &armor, 4);
	atk[params::atk::kDmgLevel] = 3;
	atk[params::atk::kTargetBits] = 0x07;
	CHECK(!ObjectOnly(atk.data()));
	MakeObjectOnlyAttack(atk.data(), 100, false);
	CHECK(ObjectOnly(atk.data()) && At<std::uint16_t>(atk.data(), params::atk::kAtkObj) == 100 && (atk[params::atk::kTargetBits] & 0x07) == 0);
	MakeObjectOnlyAttack(atk.data(), 10, true);
	CHECK(ObjectOnly(atk.data()) && At<std::uint16_t>(atk.data(), params::atk::kAtkObj) == 10 && (atk[params::atk::kTargetBits] & 0x01) == 1);
}

// The default looks and carriers against the real regulation (build/er_params).
static void TestExplosionRows(const std::string& a_dir, const std::vector<std::uint8_t>& a_bulletFile, const params::ParsedFile& a_bullet,
	const std::vector<std::uint8_t>& a_atkFile, const params::ParsedFile& a_atk)
{
	using namespace phantom::er::explosion;
	const Config d;
	auto         row = [&](std::uint32_t a_id) { return RowOf(a_bulletFile, a_bullet, a_id); };
	auto         atkRow = [&](std::uint32_t a_id) { return RowOf(a_atkFile, a_atk, a_id); };
	// Each look: the row exists, it is harmful as shipped (that's why it is copied), its effect-only copy is not, and it
	// shows the expected effect: Fire Pot fireball, Holy Water Pot flash, Sleep Pot mist, Fetid Pot cloud.
	const std::int32_t effect[] = { 300122, 300182, 300212, 300192 };
	const std::size_t  children[] = { 2, 0, 0, 0 };
	for (std::size_t k = 0; k < 4; ++k) {  // the blasts (flame and breakers below)
		const auto* t = row(d.explosionTemplate[k]);
		CHECK(t != nullptr);
		if (!t) {
			continue;
		}
		CHECK(Harmful(t));
		const auto chain = WalkChain(t, row);
		CHECK(chain.complete && chain.harmless && chain.ids.size() == children[k]);
		std::array<std::uint8_t, params::kBullet.rowSize> copy;
		std::memcpy(copy.data(), t, copy.size());
		MakeCarrier(copy.data(), d.explosionSfx[k], chain.harmless);
		CHECK(!Harmful(copy.data()) && FlightSfx(copy.data()) == effect[k]);
		// The effect outlives a short burst bullet only if ER leaves it to finish (delete type 0, as on every pot burst).
		CHECK(copy[0x9E] == 0 && copy[0x9F] == 0);
		float r = 0, rMax = 0;
		std::memcpy(&r, copy.data() + params::bullet::kHitRadius, 4);
		std::memcpy(&rMax, copy.data() + params::bullet::kHitRadiusMax, 4);
		CHECK(NativeRadius(r, rMax) >= 1.0f && NativeRadius(r, rMax) <= 4.0f);
	}
	// The flame: Dragonfire's breath row, an emitter that hurts; one harmless flame per spawn once copied.
	if (const auto* t = row(d.explosionTemplate[4])) {
		CHECK(Harmful(t) && At<float>(t, params::bullet::kShootInterval) > 0.0f);
		std::array<std::uint8_t, params::kBullet.rowSize> copy;
		std::memcpy(copy.data(), t, copy.size());
		MakeCarrier(copy.data(), d.explosionSfx[4], false);
		MakeSingleShot(copy.data(), d.flameSpeed);
		CHECK(!Harmful(copy.data()) && FlightSfx(copy.data()) > 0 && At<std::uint16_t>(copy.data(), params::bullet::kNumShoot) == 1 &&
		      At<float>(copy.data(), params::bullet::kShootInterval) == 0.0f);
	} else {
		CHECK(!"flame template missing");
	}
	// The breakers: the flying Fire Pot, whose own attack is object-only (atk_obj 10, no damage): Elden Ring breaks a pot or a
	// crate it flies into. Our copies carry the cut Roped Freezing Pot's attack rows, patched the same way.
	for (const std::size_t k : { std::size_t{ 5 }, std::size_t{ 6 } }) {
		const auto* t = row(d.explosionTemplate[k]);
		CHECK(t != nullptr);
		if (!t) {
			continue;
		}
		const auto  ownAtk = At<std::int32_t>(t, params::bullet::kAtkId);
		const auto* own = atkRow(static_cast<std::uint32_t>(ownAtk));
		CHECK(ownAtk == 10030000 && own && At<std::uint16_t>(own, params::atk::kAtkObj) == 10 && At<std::uint16_t>(own, params::atk::kAtkPhys) == 0);
		const auto ourAtk = k == 5 ? d.breakerAtk : d.explosionAtk;
		std::array<std::uint8_t, params::kBullet.rowSize> copy;
		std::memcpy(copy.data(), t, copy.size());
		MakeBreaker(copy.data(), static_cast<std::int32_t>(ourAtk), d.breakerRadius, 0.05f, d.breakerSpeed);
		CHECK(OnlyAttack(copy.data(), static_cast<std::int32_t>(ourAtk)) && FlightSfx(copy.data()) == -1);
		const auto* a = atkRow(ourAtk);
		CHECK(a != nullptr);
		if (a) {
			std::array<std::uint8_t, params::kAtkParamPc.rowSize> patched;
			std::memcpy(patched.data(), a, patched.size());
			MakeObjectOnlyAttack(patched.data(), static_cast<std::uint16_t>(k == 5 ? d.breakerObjAttack : d.explosionObjAttack), d.breakerHitsCharacters);
			CHECK(ObjectOnly(patched.data()));
		}
	}
	// The fire pot's children (18 sparks, then small ground flames) carry no attack: they stay.
	const auto* fire = row(10030001);
	if (fire) {
		const auto chain = WalkChain(fire, row);
		CHECK(chain.ids.size() == 2 && chain.ids[0] == 10030002 && chain.ids[1] == 10030003);
	}
	// The Lightning Pot burst's children do hurt: its chain would be cut.
	if (const auto* bolt = row(10032001)) {
		CHECK(!WalkChain(bolt, row).harmless);
	}
	// The carriers exist and nothing in any param refers to them (as a bullet, goods, behaviour or magic id: any i32 in
	// any row), except the cut items they belong to (Roped Freezing Pot 480, Miranda's Prayer 3020) and their own chains.
	for (const auto id : d.explosionCarrier) {
		CHECK(row(id) != nullptr);
	}
	auto allowed = [](const std::string& a_file, std::uint32_t a_row) {
		if (a_file == "EquipParamGoods.param") {
			return a_row == 480 || a_row == 3020;
		}
		return a_file == "Bullet.param" && ((a_row >= 10048000 && a_row <= 10048001) || (a_row >= 10302000 && a_row <= 10302006));
	};
	int scanned = 0, found = 0;
	for (const auto& entry : std::filesystem::directory_iterator(a_dir)) {
		if (entry.path().extension() != ".param") {
			continue;
		}
		const auto file = ReadFile(entry.path().string());
		const auto p = params::Parse(file.data(), file.size());
		if (!p || p->rowStride < 4) {
			continue;
		}
		++scanned;
		const auto name = entry.path().filename().string();
		for (const auto& [rowId, offset] : p->rows) {
			for (std::uint32_t o = 0; o + 4 <= p->rowStride && offset + o + 4 <= file.size(); o += 4) {
				std::int32_t v;
				std::memcpy(&v, file.data() + offset + o, 4);
				if (std::ranges::find(d.explosionCarrier, static_cast<std::uint32_t>(v)) != d.explosionCarrier.end() && !allowed(name, rowId)) {
					std::printf("FAIL carrier %d referenced by %s row %u +0x%x\n", v, name.c_str(), rowId, o);
					++found;
				}
			}
		}
	}
	CHECK(found == 0 && scanned > 100);
	std::printf("explosion looks and carriers checked (%d param files scanned)\n", scanned);
}

static void TestParams(const std::string& a_dir)
{
	auto fake = FakeParam();
	auto p = params::Parse(fake.data(), fake.size());
	CHECK(p && p->structName == "TEST_PARAM_ST" && p->rowCount == 3 && p->rowStride == 0x20 && p->rows.size() == 3);
	CHECK(p && p->rows[2].first == 999 && p->rows[2].second == 0xE0);
	fake[0x2D] = 0x81;  // 32-bit offsets: not ER's format
	CHECK(!params::Parse(fake.data(), fake.size()));
	CHECK(!params::Parse(fake.data(), 0x20));

	// Patch/undo on plain memory (the same guarded writes as in the game).
	std::uint8_t row[16] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	{
		params::Patch patch;
		CHECK(patch.Set<std::uint16_t>(row, 2, 0xBEEF) && patch.Set<std::uint16_t>(row, 2, 0x1234) && patch.Set<std::uint8_t>(row, 7, 9));
		CHECK(At<std::uint16_t>(row, 2) == 0x1234 && row[7] == 9 && !patch.Empty());
	}
	CHECK(row[2] == 3 && row[3] == 4 && row[7] == 8);  // the destructor restored the first backup

	// The real regulation params (tools/re/er_params.py ids Bullet extracts them into build/er_params).
	auto atkFile = ReadFile(a_dir + "/AtkParam_Pc.param");
	auto bulletFile = ReadFile(a_dir + "/Bullet.param");
	auto spFile = ReadFile(a_dir + "/SpEffectParam.param");
	if (atkFile.empty() || bulletFile.empty() || spFile.empty()) {
		std::printf("note: no extracted regulation params in %s (run tools/re/er_params.py ids Bullet): real-data checks skipped\n", a_dir.c_str());
		return;
	}
	const auto atk = params::Parse(atkFile.data(), atkFile.size());
	const auto bullet = params::Parse(bulletFile.data(), bulletFile.size());
	const auto sp = params::Parse(spFile.data(), spFile.size());
	CHECK(atk && atk->structName == params::kAtkParamPc.structName && atk->rowStride == params::kAtkParamPc.rowSize);
	CHECK(bullet && bullet->structName == params::kBullet.structName && bullet->rowStride == params::kBullet.rowSize);
	CHECK(sp && sp->structName == params::kSpEffect.structName && sp->rowStride == params::kSpEffect.rowSize);
	if (!atk || !bullet || !sp) {
		return;
	}
	// The default bullet row (Throwing Dagger) and its attack row, at the offsets we patch.
	const Config d;
	const auto*  b = RowOf(bulletFile, *bullet, d.bulletId);
	CHECK(b != nullptr);
	if (b) {
		const auto atkId = At<std::int32_t>(b, params::bullet::kAtkId);
		CHECK(atkId == 10170000 && At<std::int32_t>(b, params::bullet::kSfxIdBullet) == 300001);
		const auto* a = RowOf(atkFile, *atk, static_cast<std::uint32_t>(atkId));
		CHECK(a != nullptr);
		if (a) {
			CHECK(At<std::uint16_t>(a, params::atk::kAtkPhys) == 67 && At<std::uint16_t>(a, params::atk::kAtkPhysCorrection) == 120);
			CHECK(At<std::uint16_t>(a, params::atk::kAtkFire) == 0 && At<std::uint16_t>(a, params::atk::kAtkFireCorrection) == 100);
			CHECK(At<std::uint16_t>(a, params::atk::kAtkStam) == 5 && At<std::uint8_t>(a, params::atk::kDmgLevel) == 8);
			CHECK(At<float>(a, params::atk::kAtkSuperArmor) == 1.0f && At<float>(a, params::atk::kAtkSuperArmorCorrection) == 0.0f);
		}
	}
	// The sleep ladder: every rung is a sleep build-up row with n * step power.
	for (int n = 1; n <= d.sleepSpEffectCount; ++n) {
		const auto* s = RowOf(spFile, *sp, static_cast<std::uint32_t>(d.sleepSpEffectBase + n - 1));
		CHECK(s != nullptr);
		if (s) {
			CHECK(At<std::uint16_t>(s, params::speffect::kStateInfo) == 436);
			CHECK(At<std::int32_t>(s, params::speffect::kSleepAttackPower) == n * d.sleepSpEffectStep);
		}
	}
	std::printf("regulation params checked (%u AtkParam_Pc, %u Bullet, %u SpEffectParam rows)\n", atk->rowCount, bullet->rowCount, sp->rowCount);
	TestExplosionRows(a_dir, bulletFile, *bullet, atkFile, *atk);
}

// Snake's cardboard box (er/Stealth.h) and his rounds in flight (er/Rounds.h).
static void TestStealthAndRounds()
{
	using namespace phantom::er;
	const Stealth::BoxRules r;
	Stealth::BoxState       s;
	CHECK(!Stealth::BoxHidden(s, false, 0.0f, 50.0f, 1000, r) && s.stillMs == 0);
	CHECK(!Stealth::BoxHidden(s, true, 1.0f, 50.0f, 1000, r));  // got in creeping: not yet
	CHECK(Stealth::BoxHidden(s, true, 0.0f, 50.0f, 2000, r) && s.stillMs == 2000);  // keeps still: hidden
	CHECK(Stealth::BoxHidden(s, true, 1.0f, 50.0f, 3000, r));    // creeps on: a short grace...
	CHECK(!Stealth::BoxHidden(s, true, 1.0f, 50.0f, 3600, r));   // ...then seen
	CHECK(!Stealth::BoxHidden(s, true, 3.0f, 50.0f, 3700, r));   // runs: seen
	CHECK(Stealth::BoxHidden(s, true, 0.2f, 50.0f, 3800, r));
	CHECK(!Stealth::BoxHidden(s, true, 0.0f, 1.5f, 3900, r));    // an enemy right next to the box sees through it
	CHECK(Stealth::BoxHidden(s, true, 0.0f, 2.5f, 4000, r));
	CHECK(!Stealth::BoxHidden(s, false, 0.0f, 50.0f, 4100, r) && s.stillMs == 0);  // out of the box
	CHECK(!Stealth::BoxHidden(s, true, 1.0f, 50.0f, 4200, r));   // back in creeping: the old stillness doesn't count

	// Rockets and grenade rounds fly; bullets don't.
	using Rounds::Flight;
	CHECK(Rounds::FlightOf(59) == Flight::kRocket && Rounds::FlightOf(43) == Flight::kRocket && Rounds::FlightOf(35) == Flight::kRocket &&
	      Rounds::FlightOf(245) == Flight::kRocket);
	CHECK(Rounds::FlightOf(23) == Flight::kGrenade && Rounds::FlightOf(27) == Flight::kGrenade && Rounds::FlightOf(64) == Flight::kGrenade);
	CHECK(Rounds::FlightOf(15) == Flight::kNone && Rounds::FlightOf(5) == Flight::kNone && Rounds::FlightOf(0) == Flight::kNone);
	// A rocket's motor: 16 m/s for 1 s, then 60 (MGSV: ~1.1 s to 19 m, ~3.3 s to 150 m). Flown the way Tick does (1/60 s steps).
	auto flight = [](float a_metres) {
		float d = 0, t = 0;
		while (d < a_metres && t < 30.0f) {
			d += Rounds::SpeedAt(Flight::kRocket, t, 16.0f, 60.0f, 1.0f) / 60.0f;
			t += 1.0f / 60.0f;
		}
		return t;
	};
	CHECK(flight(19.0f) > 1.0f && flight(19.0f) < 1.25f);
	CHECK(flight(150.0f) > 3.1f && flight(150.0f) < 3.6f);
	CHECK(Rounds::SpeedAt(Flight::kGrenade, 5.0f, 80.0f, 60.0f, 1.0f) == 80.0f && Rounds::SpeedAt(Flight::kRocket, 0.5f, 16.0f, 60.0f, 1.0f) == 16.0f);
}

// The shipped er/PhantomRingHost.ini must state the built-in [Combat] defaults (it is the documentation).
static void TestShippedIni(const wchar_t* a_path)
{
	Config s;
	CHECK(s.Load(a_path));
	const Config d;
	CHECK(s.traceRange == d.traceRange && s.damage == d.damage && s.route == d.route && s.targets == d.targets && s.bulletId == d.bulletId);
	CHECK(s.bulletHideSfx == d.bulletHideSfx && s.bulletOffset == d.bulletOffset && s.bulletDamageMult == d.bulletDamageMult && s.verifyMs == d.verifyMs &&
	      s.verifyFailures == d.verifyFailures);
	CHECK(s.unitsPerLife == d.unitsPerLife && s.bigFrom == d.bigFrom && s.bossFrom == d.bossFrom && s.bossLives == d.bossLives &&
	      s.shareMult == d.shareMult && s.headFraction == d.headFraction && s.headshotMult == d.headshotMult && s.headshotMultBoss == d.headshotMultBoss &&
	      s.headshotMinLives == d.headshotMinLives && s.buddyMult == d.buddyMult && s.maxDamage == d.maxDamage);
	CHECK(s.tranqSleep == d.tranqSleep && s.tranqSleepHead == d.tranqSleepHead && s.chokeSleep == d.chokeSleep && s.stunPoiseMult == d.stunPoiseMult &&
	      s.sleepSpEffectBase == d.sleepSpEffectBase && s.sleepSpEffectStep == d.sleepSpEffectStep && s.sleepSpEffectCount == d.sleepSpEffectCount);
	CHECK(s.meleePunch == d.meleePunch && s.meleeKick == d.meleeKick && s.meleeThrow == d.meleeThrow && s.meleeThrowPoise == d.meleeThrowPoise &&
	      s.meleeStrikePoise == d.meleeStrikePoise && s.meleeRange == d.meleeRange && s.meleeArcDeg == d.meleeArcDeg);
	CHECK(s.explosionRadius == d.explosionRadius && s.maxHitsPerSecond == d.maxHitsPerSecond && s.capsuleRadius == d.capsuleRadius &&
	      s.capsuleHeight == d.capsuleHeight);
	CHECK(s.fallDamage == d.fallDamage && s.fallHurtSpeed == d.fallHurtSpeed && s.fallLethalSpeed == d.fallLethalSpeed && s.guestLifeScale == d.guestLifeScale);
	CHECK(s.hurtShare == d.hurtShare && s.hurtMult == d.hurtMult && s.topUp == d.topUp);
	CHECK(s.explosionFx == d.explosionFx && s.explosionTemplate == d.explosionTemplate && s.explosionSfx == d.explosionSfx &&
	      s.explosionCarrier == d.explosionCarrier && s.explosionMaxBursts == d.explosionMaxBursts && s.explosionPerSecond == d.explosionPerSecond &&
	      s.explosionCloudRepeat == d.explosionCloudRepeat && s.explosionGround == d.explosionGround);
	CHECK(s.hitHook == d.hitHook && s.killOnGuestDeath == d.killOnGuestDeath && s.mirrorLife == d.mirrorLife);
	// The defaults the rules code assumes.
	const HitRules r;
	CHECK(r.scaling.unitsPerLife == d.unitsPerLife && r.scaling.bigFrom == d.bigFrom && r.scaling.bossFrom == d.bossFrom &&
	      r.scaling.bossLives == d.bossLives && r.shareMult == d.shareMult && r.headFraction == d.headFraction && r.headshotMult == d.headshotMult &&
	      r.headshotMultBoss == d.headshotMultBoss && r.headshotMinLives == d.headshotMinLives && r.tranqSleep == d.tranqSleep &&
	      r.tranqSleepHead == d.tranqSleepHead);
	const MeleeValues m;
	CHECK(m.punch == d.meleePunch && m.kick == d.meleeKick && m.throwDamage == d.meleeThrow && m.throwPoise == d.meleeThrowPoise &&
	      m.strikePoise == d.meleeStrikePoise && m.chokeSleep == d.chokeSleep);
}

// [Progression] (roadmap 2.12): Elden Ring's level / right-hand armament scale Snake's lethal damage; off by default.
static void TestProgression(const std::string& a_dir)
{
	ProgressionRules r;  // defaults: off
	ProgressionStats st{ 40, 12, 25 };
	CHECK(r.mode == ProgressionMode::kOff && Near(ProgressionFactor(r, st), 1.0f));
	CHECK(ProgressionModeOf("level") == ProgressionMode::kLevel && ProgressionModeOf("weapon") == ProgressionMode::kWeapon &&
	      ProgressionModeOf("both") == ProgressionMode::kBoth && ProgressionModeOf("off") == ProgressionMode::kOff && ProgressionModeOf("x") == ProgressionMode::kOff);
	// level: (level / 20)^0.5 -- 10 x0.71, 20 x1, 40 x1.41, 80 x2, 150 x2.74; level 1 clamps to min 0.5, 713 to max 3
	r.mode = ProgressionMode::kLevel;
	auto lv = [&](int a_level) { return ProgressionFactor(r, ProgressionStats{ a_level, -1, 0 }); };
	CHECK(Near(lv(10), 0.7071f) && Near(lv(20), 1.0f) && Near(lv(40), 1.4142f) && Near(lv(80), 2.0f) && Near(lv(150), 2.7386f));
	CHECK(Near(lv(1), 0.5f) && Near(lv(713), 3.0f) && Near(lv(0), 1.0f));  // 0 = unknown: x1
	// weapon: 1 + 1.5 x upgrade / max -- +0 x1, +12 of 25 x1.72, +25 x2.5, somber +10 of 10 x2.5, +5 of 10 x1.75; unknown x1
	r.mode = ProgressionMode::kWeapon;
	auto wp = [&](int a_up, int a_max) { return ProgressionFactor(r, ProgressionStats{ 40, a_up, a_max }); };
	CHECK(Near(wp(0, 25), 1.0f) && Near(wp(12, 25), 1.72f) && Near(wp(25, 25), 2.5f) && Near(wp(10, 10), 2.5f) && Near(wp(5, 10), 1.75f));
	CHECK(Near(wp(-1, 25), 1.0f) && Near(wp(3, 0), 1.0f) && Near(wp(30, 25), 2.5f));
	// both: the product, clamped
	r.mode = ProgressionMode::kBoth;
	CHECK(Near(ProgressionFactor(r, ProgressionStats{ 40, 12, 25 }), 1.4142f * 1.72f) && Near(ProgressionFactor(r, ProgressionStats{ 150, 25, 25 }), 3.0f));
	// The factor multiplies every lethal hit's damage; sleep and the fixed stun-blast token stay.
	HitRules h;
	const auto base = ShotEffect(RoundClass::kRifle, proto::kAmmoLethal, 1, 0.5f, 400, false, h);
	h.progression = 2.0f;
	const auto x2 = ShotEffect(RoundClass::kRifle, proto::kAmmoLethal, 1, 0.5f, 400, false, h);
	CHECK(x2.damage == 2 * base.damage);
	CHECK(ShotEffect(RoundClass::kTranq, proto::kAmmoTranq, 1, 0.5f, 400, false, h).damage == 0);
	CHECK(MeleeEffect(proto::kMeleePunch, 400, MeleeValues{}, h).damage == 2 * MeleeEffect(proto::kMeleePunch, 400, MeleeValues{}, HitRules{}).damage);
	CHECK(ExplosionEffect(0.0f, 4.0f, 400, h).damage == 2 * ExplosionEffect(0.0f, 4.0f, 400, HitRules{}).damage);
	CHECK(StunBlastEffect(h).damage == StunBlastEffect(HitRules{}).damage);
	h.progression = std::nanf("");
	CHECK(ShotEffect(RoundClass::kRifle, proto::kAmmoLethal, 1, 0.5f, 400, false, h).damage == base.damage);  // a bad factor counts as 1
	// The reads check themselves: rune level = the eight attributes' sum - 79 (a Vagabond's starting 15 10 11 14 13 9 9 7 = level 9).
	const int vagabond[8]{ 15, 10, 11, 14, 13, 9, 9, 7 };
	CHECK(LevelMatchesAttributes(9, vagabond) && !LevelMatchesAttributes(10, vagabond));
	const int bad[8]{ 0, 10, 11, 14, 13, 9, 9, 7 };
	CHECK(!LevelMatchesAttributes(-1, bad));
	CHECK(WeaponRowOf(2000003) == 2000000 && UpgradeOf(2000003) == 3 && WeaponRowOf(2000112) == 2000100 && UpgradeOf(2000112) == 12 && WeaponRowOf(-1) == -1);
	// The highest upgrade from the real ReinforceParamWeapon (25 for a Longsword, 10 for a somber reinforce type 2200).
	auto wFile = ReadFile(a_dir + "/EquipParamWeapon.param");
	auto rFile = ReadFile(a_dir + "/ReinforceParamWeapon.param");
	if (wFile.empty() || rFile.empty()) {
		std::printf("note: no EquipParamWeapon/ReinforceParamWeapon in %s: the weapon levels are not checked against the regulation\n", a_dir.c_str());
		return;
	}
	const auto w = params::Parse(wFile.data(), wFile.size());
	const auto rf = params::Parse(rFile.data(), rFile.size());
	CHECK(w && w->structName == params::kEquipParamWeapon.structName && w->rowStride == params::kEquipParamWeapon.rowSize);
	CHECK(rf && rf->structName == params::kReinforceParamWeapon.structName && rf->rowStride == params::kReinforceParamWeapon.rowSize);
	if (!w || !rf) {
		return;
	}
	auto exists = [&](int a_id) { return RowOf(rFile, *rf, static_cast<std::uint32_t>(a_id)) != nullptr; };
	const auto* longsword = RowOf(wFile, *w, 2000000);
	CHECK(longsword != nullptr);
	if (longsword) {
		const auto type = At<std::int16_t>(longsword, params::weapon::kReinforceTypeId);
		CHECK(type == 0 && MaxUpgrade(type, exists) == 25);
	}
	CHECK(MaxUpgrade(2200, exists) == 10 && MaxUpgrade(100, exists) == 25);
	int somber = 0, regular = 0;
	for (const auto& [id, off] : w->rows) {
		if (id % 10000 == 0 && id >= 1000000) {
			const int m = MaxUpgrade(At<std::int16_t>(wFile.data() + off, params::weapon::kReinforceTypeId), exists);
			somber += m == 10;
			regular += m == 25;
		}
	}
	CHECK(somber > 50 && regular > 300);
	std::printf("weapon levels: %d regular (+25) and %d somber (+10) base weapons\n", regular, somber);
}

int main(int argc, char** argv)
{
	phantom::Log::Init(L"test_host_combat.log", "test");
	TestShapes();
	TestDamage();
	TestSleepAndPacing();
	TestHurtAndFlame();
	TestExplosionLogic();
	TestThrowLogic();
	TestStealthAndRounds();
	TestParams(argc >= 3 ? argv[2] : "er_params");
	TestProgression(argc >= 3 ? argv[2] : "er_params");
	if (argc >= 2) {
		TestShippedIni(phantom::Widen(argv[1]).c_str());
	}
	std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
	return g_failures ? 1 : 0;
}
