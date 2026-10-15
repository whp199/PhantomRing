#include "DecoyLogic.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace phantom::er::decoy
{
	Variant VariantOf(std::uint16_t a_equip)
	{
		switch (a_equip) {
		case 401: case 402: case 403:
			return Variant::kActive;
		case 405: case 406: case 407: case 380: case 381:
			return Variant::kShock;
		default:
			return Variant::kPlain;  // 397-399, and anything the guest calls a decoy we don't know: it is at least seen
		}
	}

	const char* VariantName(Variant a_variant)
	{
		switch (a_variant) {
		case Variant::kActive:
			return "active decoy";
		case Variant::kShock:
			return "shock decoy";
		default:
			return "decoy";
		}
	}

	Notice Notices(const Vec3& a_eye, const Vec3& a_forward, const Vec3& a_decoy, Variant a_variant, float a_snakeDistance, const Rules& a_r)
	{
		const Vec3  to = a_decoy - a_eye;
		const float d = Length(to);
		if (!Finite(to) || d < 0.3f) {
			return Notice::kNo;  // on top of it: that one knocks it over (PopsOn), it doesn't stare
		}
		// Fighting Snake right next to him: a balloon doesn't pull him away (MGSV's alerted soldiers shoot decoys, they don't
		// stand and look; and a decoy that froze every enemy in a fight would be a pause button).
		if (a_snakeDistance <= a_r.snakeClose && a_snakeDistance < d) {
			return Notice::kNo;
		}
		if (a_variant == Variant::kActive && d <= a_r.activeRange) {
			return Notice::kHears;
		}
		if (d > a_r.range) {
			return Notice::kNo;
		}
		const Vec3 fwd{ a_forward.x, 0, a_forward.z }, flat{ to.x, 0, to.z };
		if (LengthXZ(fwd) < 1e-3f || LengthXZ(flat) < 1e-3f) {
			return Notice::kNo;
		}
		const float cone = std::cos(std::clamp(a_r.coneDeg, 0.0f, 180.0f) * std::numbers::pi_v<float> / 180.0f);
		return Dot(Normalize(fwd), Normalize(flat)) >= cone ? Notice::kSees : Notice::kNo;
	}

	float Scale(std::uint64_t a_ageMs, std::int64_t a_poppedAgeMs, const Rules& a_r)
	{
		if (a_poppedAgeMs >= 0) {
			if (a_r.popMs == 0) {
				return 0.0f;
			}
			return std::clamp(1.0f - static_cast<float>(a_poppedAgeMs) / static_cast<float>(a_r.popMs), 0.0f, 1.0f);
		}
		if (a_r.inflateMs == 0 || a_ageMs >= a_r.inflateMs) {
			return 1.0f;
		}
		return 0.15f + 0.85f * static_cast<float>(a_ageMs) / static_cast<float>(a_r.inflateMs);
	}

	std::vector<Ball> Figure(const Vec3& a_feet, const Vec3& a_facing, float a_height, float a_scale)
	{
		std::vector<Ball> out;
		const float s = std::clamp(a_scale, 0.0f, 1.0f);
		if (!(a_height > 0.0f) || s <= 0.0f || !Finite(a_feet)) {
			return out;
		}
		Vec3 f{ a_facing.x, 0, a_facing.z };
		f = LengthXZ(f) > 1e-3f ? Normalize(f) : Vec3{ 0, 0, 1 };
		const Vec3 side{ f.z, 0, -f.x };  // horizontal, square to the facing
		// Heights and radii as shares of a 1.75 m soldier; collapsing squashes it down onto its feet, the balls shrink less
		// (a deflating balloon slumps more than it shrinks).
		struct Part
		{
			float y, side, r;
		};
		static constexpr Part kParts[] = {
			{ 0.09f, -0.06f, 0.065f }, { 0.09f, 0.06f, 0.065f },  // lower legs
			{ 0.26f, -0.06f, 0.07f }, { 0.26f, 0.06f, 0.07f },    // upper legs
			{ 0.46f, 0.0f, 0.10f },                                // hips
			{ 0.60f, 0.0f, 0.12f }, { 0.72f, 0.0f, 0.125f },       // belly, chest
			{ 0.79f, -0.11f, 0.07f }, { 0.79f, 0.11f, 0.07f },     // shoulders
			{ 0.62f, -0.15f, 0.05f }, { 0.62f, 0.15f, 0.05f },     // hands at its sides
			{ 0.92f, 0.0f, 0.07f },                                // head
		};
		const float rs = 0.6f + 0.4f * s;
		for (const auto& p : kParts) {
			out.push_back({ a_feet + Vec3{ 0, p.y * a_height * s, 0 } + side * (p.side * a_height), p.r * a_height * rs });
		}
		return out;
	}
}
