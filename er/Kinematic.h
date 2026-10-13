#pragma once

// Motion v1 (DESIGN.md §5.1): a host-side kinematic character controller. Snake's intended
// displacement (odometer delta mapped into ER) is resolved against Elden Ring's world with ER's own
// ray cast: wall stop + slide, step-up, ground snap, gravity fall. The result is applied to the
// puppet with ChrIns::SetPosition. Pure: the ray cast is a callback, so tests/test_host.cpp can run
// it against a synthetic world.
//
// Target (RE item E1) is to feed the motion into ER's Havok character proxy instead; this stays as
// the fallback.

#include "Math.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace phantom::er
{
	// Cast from a_from along a_delta (the whole segment). On a hit, a_hit receives the hit point.
	using RayFn = std::function<bool(const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit)>;

	struct KinematicParams
	{
		float radius{ 0.35f };       // body radius kept from walls (m)
		float stepUp{ 0.5f };        // highest ledge walked onto without a jump (m)
		float snapDown{ 0.6f };      // how far down the feet follow the ground while grounded (m)
		float chestHeight{ 1.3f };   // a wall ray (m above the feet)
		float waistHeight{ 0.9f };   // ... and two more: a cart's bed or a table between knee and chest stops the body too
		float headHeight{ 1.6f };    // (session 4: Snake walked into a wooden cart between the knee and chest rays and got stuck in it)
		float gravity{ 9.81f };      // m/s^2
		float terminalSpeed{ 50.0f };
		float slideProbe{ 0.2f };    // lateral offset of the two rays that estimate a wall's normal (m)
	};

	struct KinematicState
	{
		Vec3  pos;                // feet (Havok space)
		float vy{ 0 };            // vertical velocity (m/s, up positive)
		bool  grounded{ false };
		float groundY{ NAN };     // last ground height found under the feet
	};

	struct KinematicResult
	{
		Vec3  moved;               // horizontal displacement actually applied
		bool  blocked{ false };    // a wall clipped the wanted move
		bool  slid{ false };       // the remainder was redirected along the wall
		bool  stepped{ false };    // the feet went up a ledge this frame
		bool  landed{ false };     // touched ground after falling
		float landingSpeed{ 0 };   // m/s downward at landing
		Vec3  wallPoint;           // blocked: where the body met the wall (knee height), e.g. a crate to break
	};

	// One frame. a_want: wanted horizontal displacement (y ignored). a_dt: seconds.
	KinematicResult KinematicStep(KinematicState& a_state, const Vec3& a_want, float a_dt, const KinematicParams& a_params, const RayFn& a_ray);

	// How far (<= a_maxDist) the body can go along a_dir (unit, horizontal) from a_feet before its
	// radius touches a wall, probing at knee (just above a step), waist, chest and head height. a_normal receives
	// the wall's horizontal normal when one was hit (estimated from two side rays).
	float WallClearance(const Vec3& a_feet, const Vec3& a_dir, float a_maxDist, const KinematicParams& a_params, const RayFn& a_ray, Vec3* a_normal);

	// How boxed in the body is: of 8 horizontal directions, how many meet geometry within a_reach of the body's axis at knee
	// or chest height (a flat wall: ~3, a corner ~4-5, inside a cart or a rock: 7-8).
	int Enclosed(const Vec3& a_feet, float a_reach, const KinematicParams& a_params, const RayFn& a_ray);

	// Unsticking (pure): a puppet boxed in (Enclosed >= 7) for a_seconds goes back to the last place it stood free.
	class Unstick
	{
	public:
		// Each frame: where the feet are, how boxed in, grounded, now (s). Returns the place to go back to, or nothing.
		std::optional<Vec3> Update(const Vec3& a_feet, int a_enclosed, bool a_grounded, double a_nowS, float a_seconds);
		void                Reset() { *this = Unstick(); }
		void                Shift(const Vec3& a_delta);  // Elden Ring moved its world origin
		[[nodiscard]] std::uint64_t Count() const { return count_; }

	private:
		Vec3          free_[8];
		int           freeCount_{ 0 };
		double        lastFreeS_{ -1.0 };
		double        boxedSinceS_{ -1.0 };
		std::uint64_t count_{ 0 };
	};
}
