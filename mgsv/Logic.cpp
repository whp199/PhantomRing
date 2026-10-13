#include "Logic.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>

namespace phantom::guest::logic
{
	Vec3 MatrixTranslation(const float a_m[16]) { return { a_m[12], a_m[13], a_m[14] }; }

	float MatrixYaw(const float a_m[16])
	{
		const float fx = a_m[8];
		const float fz = a_m[10];
		if (!std::isfinite(fx) || !std::isfinite(fz) || fx * fx + fz * fz < 1e-8f) {
			return 0.0f;
		}
		return std::atan2(fx, fz);
	}

	Vec3 RotateByQuat(const float a_q[4], const Vec3& a_v)
	{
		// v' = v + 2w(q x v) + 2 q x (q x v)
		const Vec3  q{ a_q[0], a_q[1], a_q[2] };
		const float w = a_q[3];
		const Vec3  t{ 2.0f * (q.y * a_v.z - q.z * a_v.y), 2.0f * (q.z * a_v.x - q.x * a_v.z), 2.0f * (q.x * a_v.y - q.y * a_v.x) };
		return { a_v.x + w * t.x + (q.y * t.z - q.z * t.y), a_v.y + w * t.y + (q.z * t.x - q.x * t.z), a_v.z + w * t.z + (q.x * t.y - q.y * t.x) };
	}

	float QuatYaw(const float a_q[4])
	{
		const Vec3 f = RotateByQuat(a_q, { 0.0f, 0.0f, 1.0f });
		if (!f.Finite() || f.x * f.x + f.z * f.z < 1e-8f) {
			return 0.0f;
		}
		return std::atan2(f.x, f.z);
	}

	std::array<float, 4> YawQuat(float a_yaw) { return { 0.0f, std::sin(a_yaw * 0.5f), 0.0f, std::cos(a_yaw * 0.5f) }; }

	Vec3 BesideOf(const Vec3& a_at, float a_yaw, float a_ahead, float a_side)
	{
		const float s = std::sin(a_yaw), c = std::cos(a_yaw);
		return { a_at.x + s * a_ahead - c * a_side, a_at.y, a_at.z + c * a_ahead + s * a_side };
	}

	Vec3 LeadPoint(const Vec3& a_at, const Vec3& a_velocity, float a_seconds, float a_maxAhead, float a_minSpeed)
	{
		const Vec3  flat{ a_velocity.x, 0.0f, a_velocity.z };
		const float speed = flat.LengthXZ();
		if (!a_velocity.Finite() || !(speed >= a_minSpeed) || !(a_seconds > 0.0f)) {
			return a_at;
		}
		const float ahead = std::min(speed * a_seconds, a_maxAhead);
		return a_at + flat * (ahead / speed);
	}

	Vec3 TransformPoint(const float a_m[16], const Vec3& a_p)
	{
		// row vector convention: p' = p * M (rows 0..2 are the basis, row 3 the translation)
		return { a_p.x * a_m[0] + a_p.y * a_m[4] + a_p.z * a_m[8] + a_m[12], a_p.x * a_m[1] + a_p.y * a_m[5] + a_p.z * a_m[9] + a_m[13],
			a_p.x * a_m[2] + a_p.y * a_m[6] + a_p.z * a_m[10] + a_m[14] };
	}

	std::array<float, 4> MatrixToQuat(const float a_m[16])
	{
		// Normalised basis rows: r0 = x axis, r1 = y axis, r2 = z axis (row-vector convention), so the
		// column-vector rotation matrix R has these rows as its columns: R[i][j] = row_j[i].
		float r[3][3];
		for (int row = 0; row < 3; ++row) {
			const float x = a_m[row * 4], y = a_m[row * 4 + 1], z = a_m[row * 4 + 2];
			const float len = std::sqrt(x * x + y * y + z * z);
			const float k = len > 1e-6f ? 1.0f / len : 0.0f;
			r[0][row] = x * k;
			r[1][row] = y * k;
			r[2][row] = z * k;
		}
		std::array<float, 4> q{};
		const float          trace = r[0][0] + r[1][1] + r[2][2];
		if (trace > 0.0f) {
			const float s = std::sqrt(trace + 1.0f) * 2.0f;
			q = { (r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s, 0.25f * s };
		} else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
			const float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
			q = { 0.25f * s, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s, (r[2][1] - r[1][2]) / s };
		} else if (r[1][1] > r[2][2]) {
			const float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
			q = { (r[0][1] + r[1][0]) / s, 0.25f * s, (r[1][2] + r[2][1]) / s, (r[0][2] - r[2][0]) / s };
		} else {
			const float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
			q = { (r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, 0.25f * s, (r[1][0] - r[0][1]) / s };
		}
		const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
		if (!(n > 1e-6f)) {
			return { 0.0f, 0.0f, 0.0f, 1.0f };
		}
		for (auto& c : q) {
			c /= n;
		}
		return q;
	}

	float FocalToFovY(float a_focalMm, float a_filmHeightMm)
	{
		if (!(a_focalMm > 1.0f) || !(a_focalMm < 2000.0f) || !(a_filmHeightMm > 0.0f)) {
			return 0.0f;
		}
		return 2.0f * std::atan(0.5f * a_filmHeightMm / a_focalMm);
	}

	void Odometer::Reset(const Vec3& a_root)
	{
		odo_ = {};
		last_ = a_root;
		velocity_ = {};
		primed_ = a_root.Finite();
		++epoch_;
	}

	bool Odometer::Update(const Vec3& a_root, double a_dtSeconds, bool a_warpPending, float a_teleportJump, float a_smoothing)
	{
		if (!a_root.Finite()) {
			return false;
		}
		if (!primed_) {
			Reset(a_root);
			return false;
		}
		const Vec3  delta = a_root - last_;
		const float jump = delta.Length();
		last_ = a_root;
		const float limit = a_warpPending ? std::min(0.75f, a_teleportJump) : a_teleportJump;
		if (jump > limit) {
			++teleports_;
			return true;
		}
		odo_[0] += delta.x;
		odo_[1] += delta.y;
		odo_[2] += delta.z;
		if (a_dtSeconds > 1e-4 && a_dtSeconds < 0.5) {
			const Vec3  v = delta * static_cast<float>(1.0 / a_dtSeconds);
			const float k = std::clamp(a_smoothing, 0.0f, 1.0f);
			velocity_ = velocity_ + (v - velocity_) * k;
		}
		return false;
	}

	TreadmillDecision Treadmill(const TreadmillInput& a_in)
	{
		TreadmillDecision out;
		if (!a_in.allowed || !a_in.root.Finite() || !a_in.centre.Finite() || a_in.radius <= 0.0f) {
			return out;
		}
		if (a_in.nowMs - a_in.lastWarpMs < a_in.cooldownMs) {
			return out;
		}
		const Vec3  away{ a_in.root.x - a_in.centre.x, 0.0f, a_in.root.z - a_in.centre.z };
		const float distance = away.LengthXZ();
		if (distance <= a_in.radius) {
			return out;
		}
		const bool movingAway = away.x * a_in.velocity.x + away.z * a_in.velocity.z > 0.0f;
		if (!movingAway && distance <= 2.0f * a_in.radius) {
			return out;
		}
		out.warp = true;
		out.target = a_in.centre;
		out.shift = out.target - a_in.root;
		return out;
	}

	bool LifeWriteLost(int a_before, int a_written, int a_now)
	{
		if (a_before < 0 || a_written < 0 || std::abs(a_written - a_before) < 400) {
			return false;
		}
		return std::abs(a_now - a_before) < std::abs(a_now - a_written);
	}

	LifeResult ApplyHurt(std::uint16_t a_life, float a_erDamage, float a_scale)
	{
		LifeResult out{ a_life, false };
		if (!(a_erDamage > 0.0f) || !(a_scale > 0.0f)) {
			return out;
		}
		const double loss = static_cast<double>(a_erDamage) * a_scale;
		const double left = static_cast<double>(a_life) - loss;
		if (left < 1.0) {
			out.life = 1;
			out.died = true;
		} else {
			out.life = static_cast<std::uint16_t>(left);
		}
		return out;
	}

	float RegenCap(float a_base, double a_since, float a_delay, float a_ramp, float a_rate)
	{
		const double s = a_since - std::max(0.0f, a_delay);
		if (!(s > 0.0) || !(a_rate > 0.0f)) {
			return a_base;
		}
		const double ramp = std::max(0.0f, a_ramp);
		if (s < ramp) {
			return static_cast<float>(a_base + 0.5 * a_rate / ramp * s * s);
		}
		return static_cast<float>(a_base + 0.5 * a_rate * ramp + a_rate * (s - ramp));
	}

	HurtFeedback FeedbackFor(std::uint16_t a_lifeLost, std::uint16_t a_lifeMax, std::uint32_t a_kind, const Vec3& a_root, const Vec3& a_from)
	{
		HurtFeedback out;
		if (a_lifeLost == 0 || !a_root.Finite()) {
			return out;
		}
		const float share = a_lifeMax ? std::clamp(static_cast<float>(a_lifeLost) / static_cast<float>(a_lifeMax), 0.0f, 1.0f) : 0.1f;
		out.shake = true;
		out.level = std::clamp(0.15f + 2.5f * share, 0.15f, 1.0f);
		out.time = std::clamp(0.25f + 1.5f * share, 0.25f, 1.2f);
		out.decay = 0.1f;
		constexpr std::uint32_t kHurtFall = 3;  // proto::kHurtFall (Logic stays protocol-free)
		if (a_kind == kHurtFall) {
			return out;  // a landing: no wound
		}
		Vec3        flat{ a_from.x, 0.0f, a_from.z };
		const float len = flat.Length();
		flat = len > 1e-3f && std::isfinite(len) ? flat * (0.2f / len) : Vec3{};
		out.blood = true;
		out.bloodPos = a_root + Vec3{ 0.0f, 1.2f, 0.0f } + flat;
		out.bloodRadius = std::clamp(0.15f + share, 0.15f, 0.4f);
		return out;
	}

	BulletKind KindOfBullet(std::uint32_t a_id)
	{
		// re-backlog M3 (names of TppEquip.BL_*, enum builder 0x140A2C6D0). UNVERIFIED in-game damage.
		switch (a_id) {
		case 10: case 11: case 26: case 30: case 34: case 40: case 49: case 67: case 98:
			return BulletKind::kTranq;
		case 7: case 12: case 13: case 14: case 17: case 22: case 25: case 29: case 33: case 101: case 242: case 244:
			return BulletKind::kStun;
		case 24: case 28: case 32: case 66: case 68: case 69: case 97: case 105:
			return BulletKind::kNone;
		case 23: case 27: case 31: case 35: case 63: case 64: case 65: case 96: case 103: case 245:
			return BulletKind::kExplosive;
		default:
			break;
		}
		if ((a_id >= 43 && a_id <= 60) || (a_id >= 70 && a_id <= 85)) {
			return BulletKind::kExplosive;
		}
		return BulletKind::kLethal;
	}

	std::uint32_t DamageShare(std::uint32_t a_lethal, std::uint32_t a_soldierLife)
	{
		if (a_lethal == 0 || a_soldierLife == 0) {
			return 0;
		}
		// Rounded, at least 1 (a lethal round is never "unknown"), at most 100 soldiers' life (a corrupt row).
		const std::uint64_t share = (std::uint64_t(a_lethal) * 10000 + a_soldierLife / 2) / a_soldierLife;
		return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(share, 1, 1000000));
	}

	Vec3 FlameNozzle(const Vec3& a_root, float a_yaw, float a_ahead, float a_right, float a_up)
	{
		const Vec3 p = BesideOf(a_root, a_yaw, a_ahead, a_right);
		return { p.x, a_root.y + a_up, p.z };
	}

	Vec3 FlameDirection(const Vec3& a_nozzle, const Vec3& a_camPos, const Vec3& a_camForward, float a_reach)
	{
		const Vec3  target = a_camPos + a_camForward * a_reach;
		const Vec3  d = target - a_nozzle;
		const float len = d.Length();
		if (!d.Finite() || !(len > 0.5f)) {
			return a_camForward;
		}
		return d * (1.0f / len);
	}

	BlastKind KindOfAttack(std::uint32_t a_id)
	{
		// Ids from TppDamage.ATK_* on 1.0.15.4 (all 492 names queried live, 2026-10-12). Grades share a kind.
		switch (a_id) {
		// ATK_Grenade(_G1,_G2) 178-180, _G5 450, _G6.._G8 457-459; molotov (and its fire) 191-196; C4 197-199;
		// claymore = D-mine 202-204, D-mine G3 451; anti-tank mine 205-207; ATK_BombAssist 212
		case 178: case 179: case 180: case 450: case 457: case 458: case 459:
		case 191: case 192: case 193: case 194: case 195: case 196:
		case 197: case 198: case 199: case 202: case 203: case 204: case 451: case 205: case 206: case 207: case 212:
			return BlastKind::kLethal;
		// ATK_SleepingGusGrenade 188-190, ATK_SleepingGusMine 208-210, ATK_SleepGusAssist 24, the gas cloud ATK_SleepGus 286 /
		// ATK_SleepGusOccurred 287
		case 188: case 189: case 190: case 208: case 209: case 210: case 24: case 286: case 287:
			return BlastKind::kTranq;
		// ATK_StunGrenade 182-184, _G3.._G6 452/460/462/464 and their "Grazed" (edge) ids 185-187/453/461/463/465;
		// ATK_ElectromagneticNetMine 211; ATK_StunDarkMatter 483-486
		case 182: case 183: case 184: case 452: case 460: case 462: case 464:
		case 185: case 186: case 187: case 453: case 461: case 463: case 465:
		case 211: case 483: case 484: case 485: case 486:
			return BlastKind::kStun;
		// ATK_SmokeGrenade 181, ATK_Smoke 284 / ATK_SmokeOccurred 285, ATK_SmokeAssist 23, flares 26/27
		case 181: case 284: case 285: case 23: case 26: case 27:
			return BlastKind::kNone;
		// gun-fired: ATK_CannonShell..ATK_WavRocket 11-14, ATK_WalkerGear_Missile 17, ATK_StunRocket 167-169, ATK_KillRocket
		// 170-172, ATK_GrenadeHit 174, ATK_Mortar(Sleep) 282/283, ATK_Tankgun_* 301-306, ATK_Grenader_* 315-318
		case 11: case 12: case 13: case 14: case 17: case 167: case 168: case 169: case 170: case 171: case 172: case 174:
		case 282: case 283: case 301: case 302: case 303: case 304: case 305: case 306: case 315: case 316: case 317: case 318:
			return BlastKind::kRound;
		default:
			break;
		}
		// The per-weapon attacks (ATK_10307..ATK_20xxx, named by number: the rounds of developed/customised weapons, e.g. the
		// ZORN-KP's grenade blast is ATK_10603 = 347, VERIFIED live) and the EX weapons (ATK_EX_hg/gl/sr_000 446-448).
		if ((a_id >= 319 && a_id <= 448) || (a_id >= 454 && a_id <= 456) || (a_id >= 466 && a_id <= 477) || (a_id >= 487 && a_id <= 488)) {
			return BlastKind::kRound;
		}
		return BlastKind::kIgnore;
	}

	Explosive ExplosiveOf(std::uint16_t a_id)
	{
		// TppEquip ids on 1.0.15.4 (tools/re/data/tppequip_ids_1.0.15.4.txt). Kinds as KindOfAttack of their blasts.
		const auto thrown = [](BlastKind a_kind, bool a_impact = false) { return Explosive{ ExplosiveUse::kThrown, a_kind, a_impact }; };
		const auto placed = [](BlastKind a_kind, std::uint32_t a_triggerMm, bool a_ownBlast = true, float a_radius = 0.0f) {
			return Explosive{ ExplosiveUse::kPlaced, a_kind, false, a_triggerMm, a_ownBlast, a_radius };
		};
		switch (a_id) {
		case 350: case 351: case 352: case 353: case 354: case 355: case 1536: case 1537: case 1538:  // EQP_SWP_Grenade(_G01.._G08)
			return thrown(BlastKind::kLethal);
		case 382: case 383: case 384:  // EQP_SWP_MolotovCocktail(_G01, _G02)
			return thrown(BlastKind::kLethal, true);
		case 377: case 378: case 379:  // EQP_SWP_SleepingGusGrenade(_G01, _G02)
			return thrown(BlastKind::kTranq);
		case 372: case 373: case 374: case 375: case 1539: case 1540: case 1541:  // EQP_SWP_StunGrenade(_G01.._G06)
		case 400: case 404: case 408: case 420:                                     // EQP_SWP_StunDarkMatter(_G01.._G03)
			return thrown(BlastKind::kStun);
		case 357: case 358: case 359: case 360: case 361:  // EQP_SWP_SmokeGrenade(_G01.._G04)
		case 364: case 365: case 366: case 368: case 369: case 370:  // support-heli / supply flare grenades
			return thrown(BlastKind::kNone);
		case 391: case 392: case 393: case 394: case 395:  // EQP_SWP_C4(_G01.._G04): set off by the player (remote)
			return placed(BlastKind::kLethal, 0);
		case 413: case 414: case 415: case 416:  // EQP_SWP_DMine(_G01.._G03): the claymore (directional, MGSV's blast along its +Z)
			return placed(BlastKind::kLethal, 3000);
		case 417: case 418: case 419:  // EQP_SWP_SleepingGusMine(_G01, _G02)
			return placed(BlastKind::kTranq, 3000);
		case 421: case 422: case 423:  // EQP_SWP_AntitankMine(_G01, _G02): MGSV sets it off under vehicles; big enemies here
			return placed(BlastKind::kLethal, 1500);
		case 425: case 426: case 427:  // EQP_SWP_ElectromagneticNetMine(_G01, _G02): a net, no blast (its BLA row 0x17: 5.0 m)
			return placed(BlastKind::kStun, 3000, false, 5.0f);
		default:
			return {};
		}
	}

	std::optional<Vec3> ArcVelocity(const Vec3& a_p0, const Vec3& a_p1, const Vec3& a_p2, float a_gravity)
	{
		const Vec3  d1 = a_p1 - a_p0, d2 = a_p2 - a_p1;
		const float second = d2.y - d1.y;  // = -g * dt^2 in free flight
		if (!a_p0.Finite() || !a_p1.Finite() || !a_p2.Finite() || !(a_gravity > 0.0f) || !(second < 0.0f)) {
			return std::nullopt;
		}
		const float dt = std::sqrt(-second / a_gravity);
		// Free flight keeps the horizontal step: a point already deflected by a wall doesn't.
		if (!(dt > 0.005f && dt < 0.5f) || (Vec3{ d2.x - d1.x, 0.0f, d2.z - d1.z }).LengthXZ() > 0.05f * std::max(0.05f, d1.LengthXZ())) {
			return std::nullopt;
		}
		return Vec3{ d1.x / dt, d1.y / dt + 0.5f * a_gravity * dt, d1.z / dt };
	}

	const char* BlastKindName(BlastKind a_kind)
	{
		switch (a_kind) {
		case BlastKind::kLethal:
			return "lethal";
		case BlastKind::kTranq:
			return "sleep";
		case BlastKind::kStun:
			return "stun";
		case BlastKind::kNone:
			return "smoke";
		case BlastKind::kRound:
			return "round";
		default:
			return "ignored";
		}
	}

	int CounterDrop(std::uint16_t a_prev, std::uint16_t a_now, int a_maxPerFrame)
	{
		if (a_now >= a_prev) {
			return 0;
		}
		const int drop = a_prev - a_now;
		return drop <= a_maxPerFrame ? drop : 0;  // a huge drop is a reset/unload, not shooting
	}

	bool MainSizeDecay::Present(bool a_matched, bool a_skippedForSize, int a_presents)
	{
		if (a_matched) {
			misses_ = 0;
		} else if (a_skippedForSize && ++misses_ >= a_presents) {
			misses_ = 0;
			return true;
		}
		return false;  // a present without UI draws changes nothing
	}

	bool FlameBurst::Update(bool a_firing, std::uint64_t a_rounds, std::uint64_t a_nowMs)
	{
		if (!a_firing) {
			firing_ = false;
			return false;
		}
		if (!firing_) {
			firing_ = true;
			gun_ = false;
			emitted_ = false;
			start_ = a_nowMs;
			rounds_ = a_rounds;
		}
		gun_ |= a_rounds != rounds_;  // a round in this burst: a gun, for the rest of it
		if (gun_ || a_nowMs - start_ < kSettleMs || (emitted_ && a_nowMs - last_ < kEveryMs)) {
			return false;
		}
		emitted_ = true;
		last_ = a_nowMs;
		return true;
	}

	std::optional<AmmoWatch::Shot> AmmoWatch::Update(const std::array<std::uint16_t, kSlots>& a_weapons, const std::array<std::uint16_t, kSlots>& a_ammo)
	{
		std::optional<Shot> best;
		if (primed_) {
			for (int s = 0; s < kSlots; ++s) {
				if (a_weapons[s] != weapons_[s] || a_weapons[s] == 0) {
					continue;  // switched/dropped/picked up: the counter belongs to a different gun now
				}
				const int rounds = CounterDrop(ammo_[s], a_ammo[s], 16);
				if (rounds > 0 && (!best || rounds > best->rounds)) {
					best = Shot{ s, a_weapons[s], rounds };
				}
			}
		}
		weapons_ = a_weapons;
		ammo_ = a_ammo;
		primed_ = true;
		return best;
	}

	bool ParseMagazines(std::string_view a_text, std::array<SlotMagazine, 3>& a_out)
	{
		for (std::size_t i = 0; i < a_out.size(); ++i) {
			std::uint32_t v[3]{};
			for (int k = 0; k < 3; ++k) {
				const auto [p, ec] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), v[k]);
				if (ec != std::errc{}) {
					return false;
				}
				a_text.remove_prefix(static_cast<std::size_t>(p - a_text.data()));
				const char want = k < 2 ? '/' : (i + 1 < a_out.size() ? ',' : '\0');
				if (want) {
					if (a_text.empty() || a_text.front() != want) {
						return false;
					}
					a_text.remove_prefix(1);
				}
			}
			a_out[i] = { v[0], v[1], v[2] };
		}
		return a_text.empty();
	}
}
