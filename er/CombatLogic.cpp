#include "CombatLogic.h"

#include "phantomring_protocol.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace phantom::er::combat
{
	// ---- shapes ----------------------------------------------------------------------------------
	Capsule ChrCapsule(const Vec3& a_feet, float a_height, float a_radius)
	{
		const float r = std::max(0.05f, a_radius);
		const float h = std::max(a_height, 2.0f * r);
		return { a_feet + Vec3{ 0, r, 0 }, a_feet + Vec3{ 0, h - r, 0 }, r };
	}

	Vec3 ClosestOnSegment(const Vec3& a_a, const Vec3& a_b, const Vec3& a_p)
	{
		const Vec3  ab = a_b - a_a;
		const float len2 = Dot(ab, ab);
		if (len2 < 1e-12f) {
			return a_a;
		}
		const float t = std::clamp(Dot(a_p - a_a, ab) / len2, 0.0f, 1.0f);
		return a_a + ab * t;
	}

	float CapsuleDistance(const Capsule& a_c, const Vec3& a_p) { return Length(a_p - ClosestOnSegment(a_c.a, a_c.b, a_p)) - a_c.r; }

	bool SphereCapsule(const Vec3& a_centre, float a_radius, const Capsule& a_c) { return CapsuleDistance(a_c, a_centre) <= std::max(0.0f, a_radius); }

	namespace
	{
		// Ray vs sphere: smallest t >= 0, or +inf.
		float RaySphere(const Vec3& a_o, const Vec3& a_d, const Vec3& a_centre, float a_r)
		{
			const Vec3  oc = a_o - a_centre;
			const float b = Dot(oc, a_d);
			const float c = Dot(oc, oc) - a_r * a_r;
			const float h = b * b - c;
			if (h < 0.0f) {
				return std::numeric_limits<float>::infinity();
			}
			const float s = std::sqrt(h);
			const float t0 = -b - s;
			if (t0 >= 0.0f) {
				return t0;
			}
			const float t1 = -b + s;
			return t1 >= 0.0f ? 0.0f : std::numeric_limits<float>::infinity();  // inside: 0; behind: none
		}
	}

	std::optional<float> RayCapsule(const Vec3& a_origin, const Vec3& a_dir, float a_maxLen, const Capsule& a_c)
	{
		if (!Finite(a_origin) || !Finite(a_dir) || !(a_maxLen > 0.0f)) {
			return std::nullopt;
		}
		if (CapsuleDistance(a_c, a_origin) <= 0.0f) {
			return 0.0f;
		}
		float best = std::numeric_limits<float>::infinity();
		// The cylinder part (Inigo Quilez's capsule intersection, side only).
		const Vec3  ba = a_c.b - a_c.a;
		const Vec3  oa = a_origin - a_c.a;
		const float baba = Dot(ba, ba);
		if (baba > 1e-10f) {
			const float bard = Dot(ba, a_dir);
			const float baoa = Dot(ba, oa);
			const float rdoa = Dot(a_dir, oa);
			const float oaoa = Dot(oa, oa);
			const float qa = baba - bard * bard;
			const float qb = baba * rdoa - baoa * bard;
			const float qc = baba * oaoa - baoa * baoa - a_c.r * a_c.r * baba;
			const float h = qb * qb - qa * qc;
			if (qa > 1e-10f && h >= 0.0f) {
				const float t = (-qb - std::sqrt(h)) / qa;
				const float y = baoa + t * bard;
				if (t >= 0.0f && y > 0.0f && y < baba) {
					best = t;
				}
			}
		}
		best = std::min({ best, RaySphere(a_origin, a_dir, a_c.a, a_c.r), RaySphere(a_origin, a_dir, a_c.b, a_c.r) });
		if (best <= a_maxLen) {
			return best;
		}
		return std::nullopt;
	}

	bool InArc(const Vec3& a_origin, const Vec3& a_facing, float a_range, float a_halfAngle, const Capsule& a_c)
	{
		const Vec3  axis = ClosestOnSegment(a_c.a, a_c.b, a_origin);
		const Vec3  to = axis - a_origin;
		const float surface = Length(to) - a_c.r;
		if (surface > a_range) {
			return false;
		}
		const Vec3  flatTo{ to.x, 0, to.z };
		const Vec3  flatFacing{ a_facing.x, 0, a_facing.z };
		const float lenTo = Length(flatTo);
		if (lenTo <= a_c.r + 0.05f) {
			return true;  // we're standing in it
		}
		const float lenFacing = Length(flatFacing);
		if (lenFacing < 1e-6f) {
			return true;  // no facing known: a sphere test
		}
		// Widen the cone by the capsule's angular radius: a big enemy off to the side still counts.
		const float cosAngle = std::clamp(Dot(flatTo, flatFacing) / (lenTo * lenFacing), -1.0f, 1.0f);
		const float angle = std::acos(cosAngle);
		const float halo = std::asin(std::clamp(a_c.r / lenTo, 0.0f, 1.0f));
		return angle <= a_halfAngle + halo;
	}

	float HeightFraction(const Capsule& a_c, const Vec3& a_hit)
	{
		const float bottom = a_c.a.y - a_c.r;
		const float top = a_c.b.y + a_c.r;
		return top > bottom ? std::clamp((a_hit.y - bottom) / (top - bottom), 0.0f, 1.0f) : 0.5f;
	}

	// ---- MGSV rounds -----------------------------------------------------------------------------
	RoundClass ClassOfBullet(std::uint32_t a_id)
	{
		// TppEquip.BL_* (enum builder 0x140A2C6D0, tools/re/data/tppequip_bullet_ids_1.0.15.4.txt); the
		// non-damage kinds match the guest's KindOfBullet (re-backlog M3).
		switch (a_id) {
		case 0:
			return RoundClass::kUnknown;
		case 2: case 3: case 4: case 5: case 6: case 102: case 106: case 107:
			return RoundClass::kHandgun;
		case 8: case 9:
			return RoundClass::kMagnum;
		case 15:
			return RoundClass::kRifle;
		case 16:
			return RoundClass::kBattleRifle;
		case 41: case 42: case 99:
			return RoundClass::kMachineGun;
		case 36: case 37: case 38: case 39: case 104:
			return RoundClass::kSniper;
		case 18: case 19:
			return RoundClass::kShotgunPellet;
		case 20: case 21:
			return RoundClass::kSlug;
		case 10: case 11: case 26: case 30: case 34: case 40: case 49: case 67: case 98:
			return RoundClass::kTranq;
		case 7: case 12: case 13: case 14: case 17: case 22: case 25: case 29: case 33: case 101: case 242: case 244:
			return RoundClass::kStun;
		case 24: case 28: case 32: case 66: case 68: case 69: case 97: case 105:
			return RoundClass::kNone;
		case 23: case 27: case 31: case 35: case 63: case 64: case 65: case 96: case 103: case 245:
			return RoundClass::kExplosive;
		case 1: case 61: case 62: case 86: case 87: case 88: case 89: case 90: case 91: case 92: case 93: case 94: case 95: case 100: case 243:
			return RoundClass::kHeavy;
		default:
			break;
		}
		if ((a_id >= 43 && a_id <= 60) || (a_id >= 70 && a_id <= 85)) {
			return RoundClass::kExplosive;
		}
		return RoundClass::kUnknown;
	}

	std::string_view Name(RoundClass a_class)
	{
		switch (a_class) {
		case RoundClass::kUnknown: return "unknown";
		case RoundClass::kHandgun: return "handgun";
		case RoundClass::kMagnum: return "magnum";
		case RoundClass::kRifle: return "rifle";
		case RoundClass::kBattleRifle: return "battle rifle";
		case RoundClass::kMachineGun: return "machine gun";
		case RoundClass::kSniper: return "sniper";
		case RoundClass::kShotgunPellet: return "buckshot";
		case RoundClass::kSlug: return "slug";
		case RoundClass::kHeavy: return "heavy";
		case RoundClass::kExplosive: return "explosive";
		case RoundClass::kTranq: return "tranquilizer";
		case RoundClass::kStun: return "stun";
		case RoundClass::kNone: return "no damage";
		case RoundClass::kFlame: return "flame";
		}
		return "?";
	}

	RoundValue ValueOf(RoundClass a_class)
	{
		// Tuned so a starting-area soldier (~300 HP, scale ~5.9) drops to ~4 handgun / ~3 rifle rounds,
		// 1 sniper round or 1 close buckshot, like MGSV's own body shots on a soldier.
		switch (a_class) {
		case RoundClass::kHandgun: return { 13.0f, 8.0f };
		case RoundClass::kMagnum: return { 30.0f, 25.0f };
		case RoundClass::kRifle: return { 17.0f, 10.0f };
		case RoundClass::kBattleRifle: return { 20.0f, 14.0f };
		case RoundClass::kMachineGun: return { 15.0f, 10.0f };
		case RoundClass::kSniper: return { 55.0f, 40.0f };
		case RoundClass::kShotgunPellet: return { 6.0f, 4.0f };
		case RoundClass::kSlug: return { 40.0f, 30.0f };
		case RoundClass::kHeavy: return { 60.0f, 50.0f };
		case RoundClass::kExplosive: return { 70.0f, 80.0f };
		case RoundClass::kTranq: return { 0.0f, 0.0f };
		case RoundClass::kStun: return { 1.0f, 40.0f };
		case RoundClass::kNone: return { 0.0f, 0.0f };
		// Per flame event (the guest sends ~8 a second while it burns): a soldier in the stream burns down in ~2 s.
		case RoundClass::kFlame: return { 2.5f, 2.0f };
		case RoundClass::kUnknown: break;
		}
		return { 15.0f, 10.0f };
	}

	// ---- MGSV -> Elden Ring damage --------------------------------------------------------------
	float Bigness(std::int32_t a_maxHp, const Scaling& a_s)
	{
		if (a_maxHp <= 0 || !(a_s.playerMaxHp > 0.0f) || !(a_s.bigFrom > 0.0f) || !(a_s.bossFrom > a_s.bigFrom)) {
			return 0.0f;
		}
		const float r = static_cast<float>(a_maxHp) / a_s.playerMaxHp;
		const float t = (std::log(std::max(r, 1e-6f)) - std::log(a_s.bigFrom)) / (std::log(a_s.bossFrom) - std::log(a_s.bigFrom));
		return std::clamp(t, 0.0f, 1.0f);
	}

	float ReferenceHp(std::int32_t a_maxHp, const Scaling& a_s)
	{
		if (a_maxHp <= 0) {
			return 0.0f;
		}
		const float lives = std::pow(std::max(1.0f, a_s.bossLives), Bigness(a_maxHp, a_s));
		return static_cast<float>(a_maxHp) / lives;
	}

	float LivesOfUnits(float a_units, const Scaling& a_s) { return a_s.unitsPerLife > 0.0f ? std::max(0.0f, a_units) / a_s.unitsPerLife : 0.0f; }

	float HeadshotMult(std::int32_t a_maxHp, const HitRules& a_rules)
	{
		const float t = Bigness(a_maxHp, a_rules.scaling);
		return a_rules.headshotMult + (a_rules.headshotMultBoss - a_rules.headshotMult) * t;
	}

	namespace
	{
		std::int32_t FromLives(float a_lives, std::int32_t a_maxHp, float a_mult, const HitRules& a_r)
		{
			if (!(a_lives > 0.0f) || a_maxHp <= 0) {
				return 0;
			}
			const float progression = std::isfinite(a_r.progression) && a_r.progression > 0.0f ? a_r.progression : 1.0f;
			const float v = a_lives * ReferenceHp(a_maxHp, a_r.scaling) * a_mult * progression;
			return std::clamp(static_cast<std::int32_t>(std::lround(v)), 1, std::max(1, a_r.maxDamage));
		}

		std::int32_t Scaled(float a_units, std::int32_t a_maxHp, float a_mult, const HitRules& a_r)
		{
			return FromLives(LivesOfUnits(a_units, a_r.scaling), a_maxHp, a_mult, a_r);
		}

		std::uint8_t LevelFor(float a_poise, bool a_heavy)
		{
			if (a_heavy || a_poise >= 60.0f) {
				return 3;
			}
			return a_poise >= 30.0f ? 2 : 1;
		}
	}

	HitEffect ShotEffect(RoundClass a_class, std::uint32_t a_ammo, std::uint32_t a_pellets, float a_heightFraction, std::int32_t a_targetMaxHp, bool a_buddy,
		const HitRules& a_rules, float a_share)
	{
		// The guest's ammo kind wins over our class when they disagree on tranq/stun (its table is the
		// one tested live; ours only refines lethal rounds).
		if (a_ammo == proto::kAmmoTranq) {
			a_class = RoundClass::kTranq;
		} else if (a_ammo == proto::kAmmoStun) {
			a_class = RoundClass::kStun;
		}
		HitEffect out;
		if (a_class == RoundClass::kNone) {
			return out;
		}
		out.headshot = a_class != RoundClass::kFlame && a_heightFraction >= a_rules.headFraction;  // a stream has no aimed head hit
		if (a_class == RoundClass::kTranq) {
			out.sleep = out.headshot ? a_rules.tranqSleepHead : a_rules.tranqSleep;
			out.sleepShare = out.headshot ? a_rules.tranqShareHead : a_rules.tranqShare;
			out.dmgLevel = 0;  // a dart doesn't make anyone flinch
			out.nonLethal = true;
			return out;
		}
		const float pellets = static_cast<float>(std::clamp<std::uint32_t>(a_pellets, 1, 32));
		const auto  v = ValueOf(a_class);
		float       mult = pellets * (a_buddy ? a_rules.buddyMult : 1.0f);
		if (out.headshot && a_class != RoundClass::kStun) {
			mult *= HeadshotMult(a_targetMaxHp, a_rules);
		}
		// MGSV's own number for this round (v8) where it applies; the class table otherwise.
		const bool shared = a_share > 0.0f && std::isfinite(a_share) && a_class != RoundClass::kStun && a_class != RoundClass::kFlame &&
		                    a_class != RoundClass::kExplosive;
		out.damage = shared ? FromLives(std::min(a_share, 20.0f) * a_rules.shareMult, a_targetMaxHp, mult, a_rules) : Scaled(v.damage, a_targetMaxHp, mult, a_rules);
		if (out.headshot && a_class != RoundClass::kStun && a_class != RoundClass::kFlame && out.damage > 0) {
			const float atLeast = a_rules.headshotMinLives * (1.0f - Bigness(a_targetMaxHp, a_rules.scaling));
			out.damage = std::max(out.damage, FromLives(atLeast, a_targetMaxHp, 1.0f, a_rules));
		}
		out.shared = shared;
		out.poise = v.poise * pellets * (a_class == RoundClass::kStun ? a_rules.stunPoiseMult : 1.0f);
		out.fire = a_class == RoundClass::kFlame;
		out.dmgLevel = LevelFor(out.poise, a_class == RoundClass::kStun || a_class == RoundClass::kExplosive || a_class == RoundClass::kHeavy);
		if (out.headshot && out.dmgLevel < 2) {
			out.dmgLevel = 2;
		}
		out.nonLethal = a_class == RoundClass::kStun;  // MGSV's stun rounds knock out, they never kill
		return out;
	}

	HitEffect MeleeEffect(std::uint32_t a_kind, std::int32_t a_targetMaxHp, const MeleeValues& a_v, const HitRules& a_rules)
	{
		HitEffect out;
		switch (a_kind) {
		case proto::kMeleeChoke:
			out.sleep = a_v.chokeSleep;
			out.sleepShare = a_v.chokeShare;
			out.dmgLevel = 0;
			out.nonLethal = true;
			return out;
		case proto::kMeleeThrow:
			out.damage = Scaled(a_v.throwDamage, a_targetMaxHp, 1.0f, a_rules);
			out.poise = a_v.throwPoise;
			out.dmgLevel = 3;
			return out;
		case proto::kMeleeKick:
			out.damage = Scaled(a_v.kick, a_targetMaxHp, 1.0f, a_rules);
			out.poise = a_v.strikePoise * 1.5f;
			break;
		default:
			out.damage = Scaled(a_v.punch, a_targetMaxHp, 1.0f, a_rules);
			out.poise = a_v.strikePoise;
			break;
		}
		out.dmgLevel = LevelFor(out.poise, false);
		return out;
	}

	HitEffect ExplosionEffect(float a_distance, float a_radius, std::int32_t a_targetMaxHp, const HitRules& a_rules)
	{
		HitEffect out;
		if (!(a_radius > 0.0f) || !(a_distance <= a_radius)) {
			return out;
		}
		const float falloff = 1.0f - 0.75f * std::clamp(a_distance / a_radius, 0.0f, 1.0f);
		const auto  v = ValueOf(RoundClass::kExplosive);
		out.damage = Scaled(v.damage, a_targetMaxHp, falloff, a_rules);
		out.poise = v.poise * falloff;
		out.dmgLevel = 3;
		return out;
	}

	HitEffect StunBlastEffect(const HitRules& a_rules)
	{
		HitEffect out;
		out.damage = 1;
		out.poise = ValueOf(RoundClass::kExplosive).poise * a_rules.stunPoiseMult;  // the centre's (falloff 1)
		out.dmgLevel = 3;
		out.nonLethal = true;
		out.correction = kStunBlastCorrection;
		return out;
	}

	std::uint16_t NonLethalCorrection(std::uint16_t a_correction, std::int32_t a_damage, std::int32_t a_hp, std::int32_t a_reference, std::uint16_t a_sleepCorrection)
	{
		if (a_damage <= 0) {
			return std::min<std::uint16_t>(a_sleepCorrection, 100);
		}
		if (a_hp <= 2) {
			return 0;
		}
		// The largest hit Elden Ring may make of it at 100%: what we measured on this kind of enemy, else a weapon hit's ~100 HP.
		const double full = kNonLethalSpread * (a_reference > 0 ? a_reference : 100);
		const auto   allowed = static_cast<long>(std::floor((a_hp - 1) * 100.0 / full));
		return static_cast<std::uint16_t>(std::clamp<long>(allowed, 0, std::min<long>(a_correction, 100)));
	}

	HitEffect GasBlastEffect(std::int32_t a_absolute, float a_share, const std::vector<std::int32_t>& a_resists, std::int32_t a_cap)
	{
		std::int32_t top = 0;
		for (const auto r : a_resists) {
			if (r > 0 && r < 999) {
				top = std::max(top, r);
			}
		}
		HitEffect out;
		out.sleep = std::clamp(top > 0 && a_share > 0.0f ? SleepPower(0, a_share, top) : a_absolute, 0, std::max(0, a_cap));
		out.dmgLevel = 0;
		out.nonLethal = true;
		return out;
	}

	std::int32_t SleepSpEffect(std::int32_t a_power, std::int32_t a_base, std::int32_t a_step, std::int32_t a_count)
	{
		if (a_power <= 0 || a_step <= 0 || a_count <= 0 || a_base <= 0) {
			return -1;
		}
		// Round to the nearest rung; the ladder's first rung (1 step) is the smallest build-up we give.
		const std::int32_t rung = std::clamp((a_power + a_step / 2) / a_step, 1, a_count);
		return a_base + rung - 1;
	}

	std::int32_t SleepPower(std::int32_t a_absolute, float a_share, std::int32_t a_resist)
	{
		if (a_resist >= 999) {
			return 0;  // immune (Elden Ring's "999" resistance)
		}
		if (a_share > 0.0f && a_resist > 0) {
			return std::max(1, static_cast<std::int32_t>(std::lround(a_share * static_cast<float>(a_resist))));
		}
		return std::max(0, a_absolute);
	}

	std::vector<std::int32_t> SleepRungs(std::int32_t a_power, std::int32_t a_base, std::int32_t a_step, std::int32_t a_count, std::size_t a_slots)
	{
		std::vector<std::int32_t> out;
		if (a_power <= 0 || a_step <= 0 || a_count <= 0 || a_base <= 0) {
			return out;
		}
		const std::int32_t top = a_step * a_count;
		while (a_power > 0 && out.size() < a_slots) {
			const std::int32_t part = std::min(a_power, top);
			out.push_back(SleepSpEffect(part, a_base, a_step, a_count));
			a_power -= part;
		}
		return out;
	}

	// ---- pacing ----------------------------------------------------------------------------------
	void RateLimiter::Configure(float a_perSecond, float a_burst)
	{
		rate_ = std::max(0.1f, a_perSecond);
		burst_ = std::max(1.0f, a_burst);
		tokens_ = std::min(tokens_, burst_);
	}

	bool RateLimiter::Allow(double a_nowMs)
	{
		if (lastMs_ >= 0.0 && a_nowMs > lastMs_) {
			tokens_ = std::min(burst_, tokens_ + static_cast<float>((a_nowMs - lastMs_) / 1000.0) * rate_);
		}
		lastMs_ = std::max(lastMs_, a_nowMs);
		if (tokens_ >= 1.0f) {
			tokens_ -= 1.0f;
			return true;
		}
		++dropped_;
		return false;
	}

	bool RouteMonitor::Report(bool a_landed)
	{
		if (a_landed) {
			++landed_;
			streak_ = 0;
			if (state_ == State::kUnknown) {
				state_ = State::kConfirmed;
			}
			return false;
		}
		++missed_;
		if (state_ != State::kUnknown) {
			return false;
		}
		if (++streak_ >= std::max(1, failLimit_)) {
			state_ = State::kFailed;
		}
		return true;
	}

	std::string_view Name(RouteMonitor::State a_state)
	{
		switch (a_state) {
		case RouteMonitor::State::kUnknown: return "unconfirmed";
		case RouteMonitor::State::kConfirmed: return "confirmed";
		case RouteMonitor::State::kFailed: return "failed";
		}
		return "?";
	}

	// ---- Elden Ring -> Snake ---------------------------------------------------------------------
	float FallFraction(float a_speed, float a_hurtSpeed, float a_lethalSpeed)
	{
		if (!(a_speed > a_hurtSpeed) || !(a_lethalSpeed > a_hurtSpeed)) {
			return 0.0f;
		}
		if (a_speed >= a_lethalSpeed) {
			return 1.0f;
		}
		return std::clamp((a_speed - a_hurtSpeed) / (a_lethalSpeed - a_hurtSpeed), 0.1f, 1.0f);
	}

	std::uint32_t FallHurtAmount(float a_fraction, std::uint16_t a_lifeMax, float a_guestLifeScale)
	{
		if (!(a_fraction > 0.0f) || a_lifeMax == 0 || !(a_guestLifeScale > 0.0f)) {
			return 0;
		}
		const double life = static_cast<double>(std::min(a_fraction, 1.0f)) * a_lifeMax;
		const double amount = a_fraction >= 1.0f ? std::ceil((a_lifeMax + 1.0) / a_guestLifeScale) : std::ceil(life / a_guestLifeScale);
		return static_cast<std::uint32_t>(std::max(1.0, amount));
	}

	ProgressionMode ProgressionModeOf(std::string_view a_text)
	{
		if (a_text == "level") {
			return ProgressionMode::kLevel;
		}
		if (a_text == "weapon") {
			return ProgressionMode::kWeapon;
		}
		if (a_text == "both") {
			return ProgressionMode::kBoth;
		}
		return ProgressionMode::kOff;
	}

	std::string_view Name(ProgressionMode a_mode)
	{
		switch (a_mode) {
		case ProgressionMode::kOff: return "off";
		case ProgressionMode::kLevel: return "level";
		case ProgressionMode::kWeapon: return "weapon";
		case ProgressionMode::kBoth: return "both";
		}
		return "?";
	}

	float LevelFactor(int a_level, const ProgressionRules& a_r)
	{
		if (a_level <= 0 || !(a_r.levelReference > 0.0f) || !std::isfinite(a_r.levelExponent)) {
			return 1.0f;
		}
		return std::pow(static_cast<float>(a_level) / a_r.levelReference, a_r.levelExponent);
	}

	float WeaponFactor(int a_upgrade, int a_maxUpgrade, const ProgressionRules& a_r)
	{
		if (a_upgrade < 0 || a_maxUpgrade <= 0 || !std::isfinite(a_r.weaponBonus)) {
			return 1.0f;
		}
		return 1.0f + a_r.weaponBonus * static_cast<float>(std::min(a_upgrade, a_maxUpgrade)) / static_cast<float>(a_maxUpgrade);
	}

	float ProgressionFactor(const ProgressionRules& a_r, const ProgressionStats& a_s)
	{
		float f = 1.0f;
		switch (a_r.mode) {
		case ProgressionMode::kOff: return 1.0f;
		case ProgressionMode::kLevel: f = LevelFactor(a_s.level, a_r); break;
		case ProgressionMode::kWeapon: f = WeaponFactor(a_s.upgrade, a_s.maxUpgrade, a_r); break;
		case ProgressionMode::kBoth: f = LevelFactor(a_s.level, a_r) * WeaponFactor(a_s.upgrade, a_s.maxUpgrade, a_r); break;
		}
		const float lo = a_r.minFactor > 0.0f ? a_r.minFactor : 0.0f;
		const float hi = a_r.maxFactor >= lo ? a_r.maxFactor : lo;
		return std::isfinite(f) ? std::clamp(f, lo, hi) : 1.0f;
	}

	bool LevelMatchesAttributes(int a_level, const int (&a_attributes)[8])
	{
		int sum = 0;
		for (const int a : a_attributes) {
			if (a < 1 || a > 99) {
				return false;
			}
			sum += a;
		}
		return a_level >= 1 && a_level <= 713 && sum - 79 == a_level;
	}

	std::uint32_t HurtForGuest(std::int32_t a_erDamage, std::int32_t a_erMaxHp, std::uint32_t a_guestLifeMax, float a_guestLifeScale, bool a_share,
		float a_mult)
	{
		if (a_erDamage <= 0 || !(a_mult > 0.0f)) {
			return 0;
		}
		double amount = static_cast<double>(a_erDamage) * a_mult;
		if (a_share && a_erMaxHp > 0 && a_guestLifeMax > 0 && a_guestLifeScale > 0.0f) {
			amount = amount / a_erMaxHp * a_guestLifeMax / a_guestLifeScale;
		}
		return static_cast<std::uint32_t>(std::clamp(std::lround(amount), 1l, 1000000l));
	}

	std::int32_t MirrorHp(std::uint16_t a_life, std::uint16_t a_lifeMax, std::int32_t a_erMaxHp)
	{
		if (a_erMaxHp <= 0) {
			return 1;
		}
		if (a_lifeMax == 0) {
			return a_erMaxHp;
		}
		const double frac = std::clamp(static_cast<double>(a_life) / a_lifeMax, 0.0, 1.0);
		return std::clamp(static_cast<std::int32_t>(std::lround(frac * a_erMaxHp)), 1, a_erMaxHp);
	}

	std::uint32_t HealForGuest(std::int32_t a_erHealed, std::int32_t a_erMaxHp, std::uint32_t a_guestLifeMax, float a_guestLifeScale, double& a_carry)
	{
		if (a_erHealed <= 0 || a_erMaxHp <= 0 || a_guestLifeMax == 0 || !(a_guestLifeScale > 0.0f)) {
			return 0;
		}
		const double amount = static_cast<double>(a_erHealed) / a_erMaxHp * a_guestLifeMax / a_guestLifeScale + a_carry;
		const double whole = std::floor(amount);
		a_carry = amount - whole;
		return static_cast<std::uint32_t>(std::min(whole, 1000000.0));
	}

	std::uint64_t HitKey(std::uint16_t a_amount, std::uint16_t a_correction, bool a_fire, float a_poise, std::uint8_t a_dmgLevel, std::int32_t a_sleep)
	{
		// Exactly the values SpawnHitBullet writes: amount, correction, element, poise (as atkStam and atkSuperArmor),
		// dmgLevel and the sleep build-up (its spEffectId rungs).
		const auto poise = static_cast<std::uint64_t>(std::clamp(a_poise, 0.0f, 999.0f) * 16.0f) & 0x3FFF;
		// The sleep sum has the top 10 bits (54..63). The rungs are a greedy decomposition, so the sum names the set; the
		// default ladder tops out at 5 x 200 = 1000. A larger sum (a ladder configured bigger) can't be told apart in 10
		// bits: such a hit never shares the row (kUniqueKey).
		if (a_sleep > 1023) {
			return kUniqueKey;
		}
		const auto sleep = static_cast<std::uint64_t>(std::max(a_sleep, 0));
		return static_cast<std::uint64_t>(a_amount) | static_cast<std::uint64_t>(a_correction) << 16 | static_cast<std::uint64_t>(a_fire) << 32 |
		       poise << 33 | static_cast<std::uint64_t>(a_dmgLevel & 0x7F) << 47 | sleep << 54;
	}

	bool MayFly(const std::vector<InFlight>& a_inFlight, const void* a_chr, std::uint64_t a_key, bool a_hp)
	{
		if (a_key == kUniqueKey && !a_inFlight.empty()) {
			return false;
		}
		for (const auto& f : a_inFlight) {
			if (f.key != a_key || (a_hp && f.hp && f.chr == a_chr)) {
				return false;
			}
		}
		return true;
	}

	std::uint16_t TopUp::Correction(std::int32_t a_npc, std::int32_t a_wanted) const
	{
		const auto ref = Reference(a_npc);
		if (!ref) {
			return a_wanted >= kSmall ? 100 : 15;
		}
		if (a_wanted >= ref) {
			return 100;
		}
		return static_cast<std::uint16_t>(std::clamp<long>(std::lround(100.0 * a_wanted / ref), 1, 100));
	}

	std::int32_t TopUp::Extra(std::int32_t a_npc, std::int32_t a_wanted, std::int32_t a_dealt, std::uint16_t a_correction)
	{
		if (a_dealt <= 0 || a_wanted <= 0) {
			return 0;
		}
		if (a_correction < 100) {
			return std::max(0, a_wanted - a_dealt);
		}
		auto it = std::ranges::find_if(ref_, [&](const auto& a_r) { return a_r.first == a_npc; });
		if (it == ref_.end()) {
			ref_.emplace_back(a_npc, a_dealt);
			it = ref_.end() - 1;
			if (ref_.size() > 256) {
				ref_.erase(ref_.begin());
				it = ref_.end() - 1;
			}
		}
		it->second = std::max(it->second, a_dealt);
		// ER's unblocked hits vary (live: 42..63 on the same soldier: counter hits, the part hit); a guard leaves a sliver
		// (5 of 42 through a soldier's shield). Half a normal hit or more counts as unblocked; less lets that share through.
		const double share = static_cast<double>(a_dealt) / it->second;
		const double through = share >= 0.5 ? 1.0 : share;
		const auto   total = static_cast<std::int32_t>(std::lround(a_wanted * through));
		return std::max(0, total - a_dealt);
	}

	std::int32_t TopUp::Reference(std::int32_t a_npc) const
	{
		const auto it = std::ranges::find_if(ref_, [&](const auto& a_r) { return a_r.first == a_npc; });
		return it == ref_.end() ? 0 : it->second;
	}

	// ---- damage Elden Ring carries: the SpEffect ladder ([Combat] damage_route = speffect) -----------------------
	namespace
	{
		int LadderDigits(const DamageLadder& a_l) { return std::clamp(a_l.digits, 1, kDamageLadderMaxDigits); }

		std::int32_t Pow10(int a_p)
		{
			std::int32_t v = 1;
			while (a_p-- > 0) {
				v *= 10;
			}
			return v;
		}

		std::size_t NonZeroDigits(std::int64_t a_v)
		{
			std::size_t n = 0;
			for (; a_v > 0; a_v /= 10) {
				n += (a_v % 10) != 0;
			}
			return n;
		}
	}

	int DamageLadderRows(const DamageLadder& a_l) { return 9 * LadderDigits(a_l); }

	std::int32_t DamageLadderMax(const DamageLadder& a_l) { return Pow10(LadderDigits(a_l)) - 1; }

	std::int32_t DamageRungRow(int a_place, int a_digit, const DamageLadder& a_l)
	{
		if (a_l.base <= 0 || a_place < 0 || a_place >= LadderDigits(a_l) || a_digit < 1 || a_digit > 9) {
			return -1;
		}
		return a_l.base + 9 * a_place + (a_digit - 1);
	}

	std::int32_t DamageRungValue(std::int32_t a_row, const DamageLadder& a_l)
	{
		if (a_l.base <= 0 || a_row < a_l.base || a_row >= a_l.base + DamageLadderRows(a_l)) {
			return 0;
		}
		const int k = a_row - a_l.base;
		return (k % 9 + 1) * Pow10(k / 9);
	}

	std::int32_t DamageRoundToSlots(std::int32_t a_amount, std::size_t a_slots, const DamageLadder& a_l)
	{
		const int          digits = LadderDigits(a_l);
		const std::int32_t top = DamageLadderMax(a_l);
		const std::int32_t amount = std::clamp(a_amount, 0, top);
		if (a_slots == 0) {
			return 0;
		}
		// Drop the low places one by one (rounding half up) until the rest fits the slots.
		for (int p = 0; p <= digits; ++p) {
			const std::int64_t unit = Pow10(p);
			std::int64_t       r = (amount + unit / 2) / unit * unit;
			if (r > top) {
				r = amount / unit * unit;  // 9960 in one slot: 9000, not 10000 (no such rung)
			}
			if (NonZeroDigits(r) <= a_slots) {
				return static_cast<std::int32_t>(r);
			}
		}
		return 0;
	}

	std::vector<std::int32_t> DamageRungs(std::int32_t a_amount, const DamageLadder& a_l, std::size_t a_slots)
	{
		std::vector<std::int32_t> out;
		if (a_l.base <= 0) {
			return out;
		}
		const std::int32_t r = DamageRoundToSlots(a_amount, a_slots, a_l);
		for (int p = LadderDigits(a_l) - 1; p >= 0; --p) {
			const int d = (r / Pow10(p)) % 10;
			if (d) {
				out.push_back(DamageRungRow(p, d, a_l));
			}
		}
		return out;
	}

	std::int32_t DamageOfRungs(const std::vector<std::int32_t>& a_rows, const DamageLadder& a_l)
	{
		std::int32_t sum = 0;
		for (const auto id : a_rows) {
			sum += DamageRungValue(id, a_l);
		}
		return sum;
	}

	NativePlan NativeDamage::Plan(std::int32_t a_npc, const void* a_chr, std::int32_t a_wanted, double a_nowMs, const Rules& a_rules) const
	{
		NativePlan p;
		p.correction = std::clamp<std::uint16_t>(a_rules.firstCorrection, 1, 100);
		if (a_wanted <= 0) {
			return p;
		}
		if (const auto ref = Reference(a_npc); ref > 0) {
			const double want = std::max(0.0, static_cast<double>(a_rules.bulletShare)) * a_wanted;
			p.correction = static_cast<std::uint16_t>(std::clamp<long>(std::lround(100.0 * want / ref), 1, 100));
			p.erExpected = static_cast<std::int32_t>(std::lround(ref * p.correction / 100.0));
		}
		p.ladder = std::max(0, a_wanted - p.erExpected);
		if (guard_ != GuardRule::kSkipped) {
			for (const auto& s : shields_) {
				if (s.chr == a_chr && a_nowMs - s.atMs <= a_rules.guardMemoryMs) {
					p.guardFactor = std::clamp(s.share, 0.0f, 1.0f);
					p.ladder = static_cast<std::int32_t>(std::lround(p.ladder * p.guardFactor));
					break;
				}
			}
		}
		return p;
	}

	NativeOutcome NativeDamage::Learn(std::int32_t a_npc, const void* a_chr, std::uint16_t a_correction, std::int32_t a_carried, std::int32_t a_dealt,
		double a_nowMs, bool a_ladderConfirmed)
	{
		NativeOutcome o;
		const std::int32_t dealt = std::max(0, a_dealt);
		// The rows are exact and ER's own part is >= 0, so a total short of what was carried means the rows did not apply
		// (a little slack: a regeneration tick may land in between).
		const std::int32_t slack = std::max(1, a_carried / 20);
		o.ladderLanded = a_carried > 0 && dealt >= a_carried - slack;
		o.erPart = o.ladderLanded ? std::max(0, dealt - a_carried) : dealt;
		const auto   ref = Reference(a_npc);
		const double expected = ref > 0 ? ref * a_correction / 100.0 : 0.0;
		// A guard is judged only when ER's part is big enough to tell it from its rounding (a 4 HP part dealing 1-2 is guarded;
		// a 2 HP part dealing 1 may be ER's variance).
		if (expected >= NativeDamage::kMinGuardJudge && o.erPart < expected / 2.0) {
			o.guarded = true;
			o.guardShare = static_cast<float>(std::clamp(o.erPart / expected, 0.0, 1.0));
			if (a_carried > 0 && o.ladderLanded) {
				guard_ = GuardRule::kApplies;
			} else if (a_carried > 0 && a_ladderConfirmed) {
				guard_ = GuardRule::kSkipped;  // the rows work (they arrived before): this shield dropped them
			}
			auto it = std::ranges::find_if(shields_, [&](const Shield& a_s) { return a_s.chr == a_chr; });
			if (it == shields_.end()) {
				shields_.push_back({ a_chr, a_nowMs, o.guardShare });
				if (shields_.size() > 64) {
					shields_.erase(shields_.begin());
				}
			} else {
				*it = { a_chr, a_nowMs, o.guardShare };
			}
			return o;
		}
		std::erase_if(shields_, [&](const Shield& a_s) { return a_s.chr == a_chr; });  // its shield is down
		if (a_correction > 0 && o.erPart > 0) {
			const float sample = static_cast<float>(o.erPart) * 100.0f / a_correction;
			auto        it = std::ranges::find_if(ref_, [&](const auto& a_r) { return a_r.first == a_npc; });
			if (it == ref_.end()) {
				ref_.emplace_back(a_npc, sample);
				if (ref_.size() > 256) {
					ref_.erase(ref_.begin());
				}
			} else {
				// ER's hits vary (counter hits, the part hit, its rounding of a small part): a moving average.
				it->second = 0.75f * it->second + 0.25f * sample;
			}
		}
		return o;
	}

	std::int32_t NativeDamage::Reference(std::int32_t a_npc) const
	{
		const auto it = std::ranges::find_if(ref_, [&](const auto& a_r) { return a_r.first == a_npc; });
		return it == ref_.end() ? 0 : static_cast<std::int32_t>(std::lround(it->second));
	}

	bool ReportLadder(RouteMonitor& a_monitor, const NativeOutcome& a_outcome, std::int32_t a_carried)
	{
		if (a_carried <= 0 || (a_outcome.guarded && !a_outcome.ladderLanded)) {
			return false;  // nothing carried tests nothing; a guarded miss is no evidence (and Elden Ring's block stands)
		}
		return a_monitor.Report(a_outcome.ladderLanded);
	}

	std::string_view Name(NativeDamage::GuardRule a_rule)
	{
		switch (a_rule) {
		case NativeDamage::GuardRule::kApplies:
			return "a guarded hit still delivers it (scaled by the shield)";
		case NativeDamage::GuardRule::kSkipped:
			return "Elden Ring drops it on a guarded hit";
		default:
			return "not seen on a guarded hit yet";
		}
	}
}
