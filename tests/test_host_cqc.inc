// CQC holds and throws on Elden Ring enemies (er/CqcLogic.h, ROADMAP 2.2 tier 1): who is grabbed, what each MGSV action does,
// bosses, and the shipped ini's [Cqc]. Included by tests/test_host_combat.cpp.
#include "CqcLogic.h"

static void TestCqcSelection()
{
	const cqc::Rules r;  // range 1.6 m, half arc 70 deg
	const Vec3       snake{ 0, 1.0f, 0 };
	const Vec3       facing{ 0, 0, 1 };  // +Z
	const auto       at = [](float a_x, float a_z, bool a_hostile = true) {
		return cqc::Candidate{ ChrCapsule({ a_x, 0, a_z }, 1.8f, 0.4f), 500, a_hostile };
	};
	// Nobody: nothing.
	CHECK(!cqc::SelectTarget(snake, facing, {}, r));
	// One in front within reach (capsule surface 1.1 m away).
	CHECK(cqc::SelectTarget(snake, facing, { at(0, 1.5f) }, r) == std::optional<std::size_t>(0));
	// Too far (2.5 m to the surface), behind him, beside him past the arc: nobody.
	CHECK(!cqc::SelectTarget(snake, facing, { at(0, 2.9f), at(0, -1.2f), at(1.4f, -0.2f) }, r));
	// The nearest of two in reach, not the first.
	CHECK(cqc::SelectTarget(snake, facing, { at(0.3f, 1.6f), at(0, 1.0f) }, r) == std::optional<std::size_t>(1));
	// Not hostile (a spirit ash, an NPC): never, even nearer.
	CHECK(cqc::SelectTarget(snake, facing, { at(0, 0.9f, false), at(0, 1.5f) }, r) == std::optional<std::size_t>(1));
	// 60 deg to the side is inside a 70 deg half arc; the facing need not be unit.
	CHECK(cqc::SelectTarget(snake, Vec3{ 0, 0, 5 }, { at(1.2f * std::sin(1.047f), 1.2f * std::cos(1.047f)) }, r) == std::optional<std::size_t>(0));
	cqc::Rules narrow = r;
	narrow.arcDeg = 30;
	CHECK(!cqc::SelectTarget(snake, facing, { at(1.2f * std::sin(1.047f), 1.2f * std::cos(1.047f)) }, narrow));
}

static void TestCqcResponses()
{
	using R = cqc::Response;
	const auto at = [](bool a_boss, bool a_held, bool a_cooling = false) { return cqc::Situation{ a_boss, a_held, a_cooling }; };
	CHECK(cqc::Respond(proto::kCqcGrab, at(false, false)) == R::kHold);
	CHECK(cqc::Respond(proto::kCqcHold, at(false, true)) == R::kNone);
	CHECK(cqc::Respond(proto::kCqcThrow, at(false, false)) == R::kKnockdown);
	CHECK(cqc::Respond(proto::kCqcThrow, at(false, true)) == R::kKnockdown);
	CHECK(cqc::Respond(proto::kCqcChoke, at(false, true)) == R::kSleep);
	CHECK(cqc::Respond(proto::kCqcInterrogate, at(false, true)) == R::kGag);
	CHECK(cqc::Respond(proto::kCqcRelease, at(false, false)) == R::kRelease);
	// Review: a choke or an interrogation on someone who is not held (the grab caught nobody, he walked up while RT stayed
	// down) does nothing -- it used to put him to sleep at once. Only grab and throw pick whoever is in front.
	CHECK(cqc::Respond(proto::kCqcChoke, at(false, false)) == R::kNone);
	CHECK(cqc::Respond(proto::kCqcInterrogate, at(false, false)) == R::kNone);
	CHECK(cqc::Respond(proto::kCqcChoke, at(true, false)) == R::kNone);  // nor a boss's 40-poise stagger
	CHECK(cqc::NeedsHold(proto::kCqcChoke) && cqc::NeedsHold(proto::kCqcInterrogate) && cqc::NeedsHold(proto::kCqcHold));
	CHECK(!cqc::NeedsHold(proto::kCqcGrab) && !cqc::NeedsHold(proto::kCqcThrow) && !cqc::NeedsHold(proto::kCqcRelease));
	// A boss is never held, thrown or choked: a stagger.
	CHECK(cqc::Respond(proto::kCqcGrab, at(true, false)) == R::kStagger);
	CHECK(cqc::Respond(proto::kCqcThrow, at(true, false)) == R::kStagger);
	CHECK(cqc::Respond(proto::kCqcChoke, at(true, true)) == R::kStagger);
	CHECK(cqc::Respond(proto::kCqcInterrogate, at(true, true)) == R::kGag);
	CHECK(cqc::Respond(proto::kCqcRelease, at(true, false)) == R::kRelease);
	CHECK(cqc::Respond(static_cast<proto::CqcAction>(9), at(false, true)) == R::kNone);
	CHECK(cqc::Name(proto::kCqcChoke) == "choke" && cqc::Name(R::kKnockdown) == "knockdown");
	// Review: no hold chain. Let go less than hold_cooldown ago: a re-grab is a flinch, not a hold; the one held now (a second
	// grab of him) and a throw are unchanged; a boss still only staggers.
	CHECK(cqc::Respond(proto::kCqcGrab, at(false, false, true)) == R::kFlinch);
	CHECK(cqc::Respond(proto::kCqcGrab, at(false, true, true)) == R::kHold);
	CHECK(cqc::Respond(proto::kCqcThrow, at(false, false, true)) == R::kKnockdown);
	CHECK(cqc::Respond(proto::kCqcGrab, at(true, false, true)) == R::kStagger);
	CHECK(!cqc::Name(R::kFlinch).empty() && cqc::Name(R::kFlinch) != "?");
	{
		cqc::Cooldowns cd;
		CHECK(!cd.Cooling(7, 1000));
		cd.Start(7, 1000, 8.0f);
		CHECK(cd.Cooling(7, 1000) && cd.Cooling(7, 8999) && !cd.Cooling(7, 9000) && !cd.Cooling(8, 2000));
		cd.Start(7, 5000, 8.0f);  // a second let-go restarts it
		CHECK(cd.Cooling(7, 12000) && cd.Size() == 1);
		cd.Start(8, 5000, 1.0f);
		cd.Expire(6000);
		CHECK(cd.Size() == 1 && cd.Cooling(7, 6000) && !cd.Cooling(8, 6000));
		cd.Expire(13000);
		CHECK(cd.Size() == 0);
		cd.Start(9, 100, -3.0f);  // a negative cooldown is none
		CHECK(!cd.Cooling(9, 100));
		// The loop the review found: hold 6 s, auto-release, RT up 0.4 s + 0.35 s to grab again -> 0.75 s later: still cooling.
		const cqc::Rules r8;
		cqc::Cooldowns   loop;
		loop.Start(42, 6000, r8.holdCooldown);
		CHECK(loop.Cooling(42, 6750));
		CHECK(cqc::Respond(proto::kCqcGrab, at(false, false, loop.Cooling(42, 6750))) == R::kFlinch);
	}

	// Review: a CQC hit waits for the bullet row. MGSV's punch of the same press (kGEvMelee first) holds it up to verify_ms;
	// fired at once the throw found it busy and landed nothing.
	using W = cqc::Wait;
	CHECK(cqc::WhenToFire(1000, 1000, 150, 450, false) == W::kWait);  // settling: the punch may still be on its way
	CHECK(cqc::WhenToFire(1149, 1000, 150, 450, false) == W::kWait);
	CHECK(cqc::WhenToFire(1150, 1000, 150, 450, false) == W::kFire);
	CHECK(cqc::WhenToFire(1150, 1000, 150, 450, true) == W::kWait);  // the punch's bullet flies: wait for it
	CHECK(cqc::WhenToFire(1450, 1000, 150, 450, true) == W::kWait);  // (its verify_ms, 300)
	CHECK(cqc::WhenToFire(1460, 1000, 150, 450, false) == W::kFire);  // landed: the throw goes
	CHECK(cqc::WhenToFire(1600, 1000, 150, 450, true) == W::kFireLate);  // a stream of bullets: at the deadline regardless
	CHECK(cqc::WhenToFire(1000, 1000, 0, 0, false) == W::kFire);
	CHECK(cqc::WhenToFire(1000, 1000, 0, 0, true) == W::kFireLate);
	CHECK(cqc::WhenToFire(900, 1000, 150, 450, false) == W::kWait);  // a clock that went back: still settling

	// Bosses by size against the Tarnished's max HP (bigFrom 1.5, bossFrom 5): a soldier no, a field boss yes.
	const cqc::Rules r;
	Scaling          s;
	s.playerMaxHp = 500;
	CHECK(!cqc::IsBoss(300, s, r));     // a Limgrave soldier
	CHECK(!cqc::IsBoss(700, s, r));     // a knight (1.4x)
	CHECK(cqc::IsBoss(2500, s, r));     // 5x: a boss
	CHECK(cqc::IsBoss(10000, s, r));
	// Review: before Combat's first tick the Tarnished's max HP is 0 and Bigness 0 for everyone -- that made every boss
	// ordinary. Unknown is a boss now (stagger only), and so is an enemy whose own max HP is unknown.
	s.playerMaxHp = 0;
	CHECK(cqc::IsBoss(10000, s, r));
	CHECK(cqc::IsBoss(300, s, r));
	s.playerMaxHp = 500;
	CHECK(cqc::IsBoss(0, s, r));
	// Review: a levelled Tarnished (Vigor 60, ~1900 HP). Bigness alone: a Tree Sentinel (~3000, 1.6x) and Margit (4174, 2.2x)
	// were ordinary and could be held, thrown and choked. boss_hp (2000) catches them whatever the Tarnished's level.
	s.playerMaxHp = 1900;
	CHECK(Bigness(3000, s) < r.bossBigness && Bigness(4174, s) < r.bossBigness);  // what went wrong
	CHECK(cqc::IsBoss(3000, s, r));
	CHECK(cqc::IsBoss(4174, s, r));
	CHECK(!cqc::IsBoss(1500, s, r));  // a late knight is still ordinary against him
	CHECK(!cqc::IsBoss(400, s, r));
	cqc::Rules noFloor = r;
	noFloor.bossHp = 0;  // boss_hp = 0: Bigness alone, as before
	CHECK(!cqc::IsBoss(4174, s, noFloor));
	s.playerMaxHp = 1500;
	CHECK(!cqc::IsBoss(1999, s, r) && cqc::IsBoss(2000, s, r));  // 1.3x: Bigness says no, the floor says yes

	// What lands: never damage; the hold a small flinch, the throw a knockdown, the choke MGSV's choke sleep.
	const MeleeValues m;
	const HitRules    hr;
	// Review: a damage-free non-lethal hit flies with sleep_correction 0, which live gives no flinch (COMBAT §2). The poise
	// hits carry hit_correction (10%) and a token 1 HP so Combat keeps that correction (NonLethalCorrection caps it by HP).
	CHECK(r.hitCorrection == 10);
	const auto hold = cqc::EffectOf(R::kHold, 300, m, hr, r);
	CHECK(hold.damage == 1 && hold.correction == 10 && hold.poise == r.grabPoise && hold.dmgLevel == 1 && hold.nonLethal && hold.sleep == 0);
	const auto flinch = cqc::EffectOf(R::kFlinch, 300, m, hr, r);
	CHECK(flinch.damage == 1 && flinch.poise == r.grabPoise && flinch.dmgLevel == r.grabDmgLevel && flinch.nonLethal);
	const auto down = cqc::EffectOf(R::kKnockdown, 300, m, hr, r);
	CHECK(down.damage == 1 && down.correction == 10 && down.poise == 120.0f && down.dmgLevel == 4 && down.nonLethal);
	const auto sleep = cqc::EffectOf(R::kSleep, 300, m, hr, r);
	CHECK(sleep.damage == 0 && sleep.correction == 0 && sleep.sleep == m.chokeSleep && sleep.sleepShare == m.chokeShare && sleep.dmgLevel == 0 &&
	      sleep.nonLethal);  // the choke stays on the sleep darts' verified route
	// ...enough to put a Limgrave soldier (sleep resistance 226) to sleep at once: share 1.1 of it.
	CHECK(SleepPower(sleep.sleep, sleep.sleepShare, 226) >= 226);
	const auto stagger = cqc::EffectOf(R::kStagger, 5000, m, hr, r);
	CHECK(stagger.damage == 1 && stagger.correction == 10 && stagger.poise == r.staggerPoise && stagger.dmgLevel == 2 && stagger.nonLethal);
	// The token never kills: Elden Ring's part is capped by NonLethalCorrection (0% at 2 HP), ours by nonLethal.
	CHECK(NonLethalCorrection(down.correction, down.damage, 2, 100, 0) == 0);
	CHECK(NonLethalCorrection(down.correction, down.damage, 300, 100, 0) == 10);
	// hit_correction 0: damage-free as before.
	cqc::Rules free0 = r;
	free0.hitCorrection = 0;
	const auto down0 = cqc::EffectOf(R::kKnockdown, 300, m, hr, free0);
	CHECK(down0.damage == 0 && down0.correction == 0 && down0.poise == 120.0f);
	// hit_correction above 100 is capped.
	cqc::Rules big = r;
	big.hitCorrection = 400;
	CHECK(cqc::EffectOf(R::kKnockdown, 300, m, hr, big).correction == 100);
	for (const auto none : { R::kNone, R::kGag, R::kRelease }) {
		const auto e = cqc::EffectOf(none, 300, m, hr, r);
		CHECK(e.damage == 0 && e.poise == 0 && e.sleep == 0 && e.sleepShare == 0);
	}
	// The interrogation lines cycle and are never empty.
	CHECK(cqc::GagCount() >= 4);
	for (std::uint32_t i = 0; i < 2 * cqc::GagCount(); ++i) {
		CHECK(!cqc::GagLine(i).empty());
	}
	CHECK(cqc::GagLine(0) == cqc::GagLine(static_cast<std::uint32_t>(cqc::GagCount())));
}

// The shipped ini's [Cqc] states the built-in defaults, and those are CqcLogic's own.
static void TestCqcIni(const wchar_t* a_path)
{
	Config s;
	CHECK(s.Load(a_path));
	const Config d;
	CHECK(d.cqc && s.cqc == d.cqc);  // live pass B 2026-10-15: on
	CHECK(s.cqcRange == d.cqcRange && s.cqcArcDeg == d.cqcArcDeg && s.cqcBossBigness == d.cqcBossBigness && s.cqcHoldSeconds == d.cqcHoldSeconds &&
	      s.cqcLeaveDistance == d.cqcLeaveDistance && s.cqcGrabPoise == d.cqcGrabPoise && s.cqcThrowPoise == d.cqcThrowPoise &&
	      s.cqcThrowDmgLevel == d.cqcThrowDmgLevel && s.cqcStaggerPoise == d.cqcStaggerPoise && s.cqcInterrogate == d.cqcInterrogate);
	CHECK(s.cqcBossHp == d.cqcBossHp && s.cqcHoldCooldown == d.cqcHoldCooldown && s.cqcHitCorrection == d.cqcHitCorrection &&
	      s.cqcSettleMs == d.cqcSettleMs && s.cqcRetryMs == d.cqcRetryMs);
	const cqc::Rules r;
	CHECK(r.range == d.cqcRange && r.arcDeg == d.cqcArcDeg && r.bossBigness == d.cqcBossBigness && r.holdSeconds == d.cqcHoldSeconds &&
	      r.leaveDistance == d.cqcLeaveDistance && r.grabPoise == d.cqcGrabPoise && r.throwPoise == d.cqcThrowPoise && r.throwDmgLevel == d.cqcThrowDmgLevel &&
	      r.staggerPoise == d.cqcStaggerPoise && r.bossHp == d.cqcBossHp && r.holdCooldown == d.cqcHoldCooldown && r.hitCorrection == d.cqcHitCorrection);
	// The wait covers MGSV's punch bullet: verify_ms (300) is its longest stay in the row.
	CHECK(d.cqcSettleMs + d.cqcRetryMs > d.verifyMs);
}
