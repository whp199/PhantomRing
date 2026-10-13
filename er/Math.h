#pragma once

// Small vector math for the host: Elden Ring Havok space and MGSV Fox world are both Y-up meters.
// Pure (no game access) so tests/test_host.cpp can exercise it.

#include <cmath>
#include <numbers>

namespace phantom::er
{
	struct Vec3
	{
		float x{ 0 }, y{ 0 }, z{ 0 };

		constexpr Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
		constexpr Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
		constexpr Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
		constexpr Vec3 operator-() const { return { -x, -y, -z }; }
		Vec3&          operator+=(const Vec3& o) { return *this = *this + o; }
	};

	struct Quat
	{
		float x{ 0 }, y{ 0 }, z{ 0 }, w{ 1 };  // xyzw, same layout as hkQuaternion and MGSV's quats
	};

	// hkVector4 / F32Vector4: 16 bytes, 16-aligned (the game's SSE code uses movaps on these).
	struct alignas(16) Vec4
	{
		float x{ 0 }, y{ 0 }, z{ 0 }, w{ 0 };
		Vec4() = default;
		Vec4(float a_x, float a_y, float a_z, float a_w) : x(a_x), y(a_y), z(a_z), w(a_w) {}
		Vec4(const Vec3& v, float a_w) : x(v.x), y(v.y), z(v.z), w(a_w) {}
		[[nodiscard]] Vec3 xyz() const { return { x, y, z }; }
	};
	static_assert(sizeof(Vec4) == 16);

	inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline Vec3  Cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
	inline float LengthXZ(const Vec3& v) { return std::sqrt(v.x * v.x + v.z * v.z); }
	inline Vec3  Normalize(const Vec3& v)
	{
		const float l = Length(v);
		return l > 1e-12f ? v * (1.0f / l) : Vec3{};
	}
	inline bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

	inline Vec3 Rotate(const Quat& q, const Vec3& v)
	{
		// v' = v + 2w(u x v) + 2(u x (u x v)), u = q.xyz
		const Vec3 u{ q.x, q.y, q.z };
		const Vec3 t = Cross(u, v) * 2.0f;
		return v + t * q.w + Cross(u, t);
	}

	// Rotation about +Y by a: maps +Z to (sin a, 0, cos a). Heading() is its inverse, so
	// Heading(RotY(a, v)) == Heading(v) + a. Plain math, no handedness assumed: both games' yaw
	// conventions are reduced to this one in Axes (er/Anchor.h).
	inline Vec3 RotY(float a, const Vec3& v)
	{
		const float c = std::cos(a), s = std::sin(a);
		return { v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
	}
	inline float Heading(const Vec3& v) { return std::atan2(v.x, v.z); }

	inline float WrapPi(float a)
	{
		constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
		a = std::fmod(a + std::numbers::pi_v<float>, kTwoPi);
		if (a < 0) {
			a += kTwoPi;
		}
		return a - std::numbers::pi_v<float>;
	}
}
