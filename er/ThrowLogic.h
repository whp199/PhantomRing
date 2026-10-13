#pragma once

// Pure rules of Snake's thrown explosives in Elden Ring (er/Throw.h): MGSV simulates its grenade over its
// flat airport apron, which is not Elden Ring's ground, so the host flies its own copy through Elden Ring's
// collision from the release point and velocity MGSV reports (kGEvThrow), with MGSV's gravity, bouncing and
// rolling like MGSV's grenades, and draws the throw arc MGSV shows while Snake aims. No game access: the ray
// cast is a callback (tests/test_host_combat.cpp flies it over synthetic ground).

#include "Math.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace phantom::er::throwing
{
	struct RayHit
	{
		Vec3 point;
		Vec3 normal{ 0, 1, 0 };  // unit, facing the ray's origin side
	};
	// The first surface on the segment a_from -> a_from + a_delta, if any.
	using RayFn = std::function<std::optional<RayHit>(const Vec3& a_from, const Vec3& a_delta)>;

	struct Params
	{
		float gravity{ 9.8f };       // m/s^2 down (MGSV's: protocol v6, GuestState aimThrowVel)
		float restitution{ 0.3f };   // share of the speed into a surface that comes back out of it
		float friction{ 0.65f };     // share of the speed along a surface kept by a bounce
		float rollDecel{ 3.0f };     // m/s^2 a rolling grenade slows by
		float restSpeed{ 0.6f };     // below this after a bounce it rolls; rolling below it, it lies still
		float radius{ 0.05f };       // kept this far off a surface
		float maxStep{ 1.0f / 60.0f };  // s, one ray per substep
		float maxTime{ 30.0f };      // s in the air or rolling before it is given up
	};

	struct Projectile
	{
		Vec3  pos;
		Vec3  vel;
		Vec3  normal{ 0, 1, 0 };  // of the surface it last touched
		float age{ 0 };           // s since the release
		int   bounces{ 0 };
		bool  touched{ false };   // hit something at least once (its first impact: impact grenades go off here)
		bool  rolling{ false };   // on a surface, sliding/rolling along it
		bool  resting{ false };   // lies still
		bool  lost{ false };      // fell out of the world / timed out
	};

	// Advances a_p by a_dt through a_ray's world. a_stick: impact explosives stop where they first touch.
	void Step(Projectile& a_p, float a_dt, const Params& a_params, const RayFn& a_ray, bool a_stick);

	// MGSV's aiming arc from a release point and velocity: the path until it first touches a surface (or
	// a_maxTime), as points every a_spacing metres along it, the first at the release point. a_landing: where it
	// lands, with the surface's normal (nullopt if it never does within a_maxTime).
	struct Arc
	{
		std::vector<Vec3>     dots;
		std::optional<RayHit> landing;
	};
	Arc TraceArc(const Vec3& a_pos, const Vec3& a_vel, const Params& a_params, const RayFn& a_ray, float a_spacing, float a_maxTime = 4.0f);

	// The normal of a surface from three hit points around a ray's hit (two extra parallel rays), facing a_toward
	// (the side the ray came from); a_fallback when they don't make a plane.
	Vec3 PlaneNormal(const Vec3& a_a, const Vec3& a_b, const Vec3& a_c, const Vec3& a_toward, const Vec3& a_fallback);

	// The pad[0] of a kGEvThrow: MGSV equip id (low 16 bits) and the fuse in ms (high 16; 0 = goes off on impact).
	inline std::uint16_t EquipOf(std::uint32_t a_pad0) { return static_cast<std::uint16_t>(a_pad0 & 0xFFFF); }
	inline std::uint32_t FuseMsOf(std::uint32_t a_pad0) { return a_pad0 >> 16; }

	// ---- placed explosives (er/Mines.cpp) ------------------------------------------------------
	// kHEvDetonate is sent once per enemy approach. MGSV may refuse it (its Placed System not resolvable that moment, the
	// object index past its count, the equip check: only logged on the guest), and then the mine would stay "set off"
	// with no blast for good. So a set-off with no blast a_retryMs later is cleared (the next enemy in range sends it
	// again), at most a_maxRetries times; the guest ignores a duplicate of one it did set off.
	inline constexpr std::uint64_t kDetonateRetryMs = 3000;
	inline constexpr int           kDetonateRetries = 3;
	inline bool DetonateRetryDue(std::uint64_t a_nowMs, std::uint64_t a_sentMs, int a_retries, bool a_blasted)
	{
		return a_sentMs != 0 && !a_blasted && a_nowMs >= a_sentMs + kDetonateRetryMs && a_retries < kDetonateRetries;
	}

	// One sphere around every armed mine plus its trigger radius: one scan of Elden Ring's characters per frame for all of
	// them (each mine then tests the list), not one per mine. Empty input: radius 0.
	struct Sphere
	{
		Vec3  centre;
		float radius{ 0 };
	};
	Sphere Enclosing(const std::vector<Sphere>& a_spheres);
}
