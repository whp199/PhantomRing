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
		// Snake's height in his stance (MGSV's: standing ~1.8 m, crouched ~1.2, prone ~0.5; riding: the mount's): the wall rays
		// above it are left out (a crouched Snake goes under a 1.3 m beam) and a move never takes him under a ceiling lower than
		// it (player report 2026-10-14: Snake walked into the low space under the Roundtable Hold's basement stairs and was stuck
		// there). 0 = no ceiling check, every wall ray.
		float bodyHeight{ 0.0f };
	};

	struct KinematicState
	{
		Vec3  pos;                // feet (Havok space)
		float vy{ 0 };            // vertical velocity (m/s, up positive)
		bool  grounded{ false };
		float groundY{ NAN };     // last ground height found under the feet
		// The ceiling check's way out (kCeilingEscapeS): already under a ceiling lower than him and refused that long in a row,
		// the check stands aside until he stands free again -- the way out from under a staircase can dip (a stringer lower
		// than the treads around it; live 2026-10-14 a crouched Snake stuck under the Roundtable Hold's basement stairs: every
		// move out read "lower than here").
		float underRefusedS{ 0 };
		bool  ceilingEscape{ false };
	};
	inline constexpr float kCeilingEscapeS = 0.75f;

	struct KinematicResult
	{
		Vec3  moved;               // horizontal displacement actually applied
		bool  blocked{ false };    // a wall clipped the wanted move
		bool  slid{ false };       // the remainder was redirected along the wall
		bool  stepped{ false };    // the feet went up a ledge this frame
		bool  landed{ false };     // touched ground after falling
		float landingSpeed{ 0 };   // m/s downward at landing
		Vec3  wallPoint;           // blocked: where the body met the wall (knee height), e.g. a crate to break
		bool  ceiling{ false };    // the move was refused: it went under a ceiling lower than bodyHeight (and lower than here)
		bool  ceilingEscape{ false };  // this frame the check stood aside for good (KinematicState::ceilingEscape began)
		float headroom{ INFINITY };  // with bodyHeight: the room above the feet where the move ended, or would have (refused) (m)
	};

	// One frame. a_want: wanted horizontal displacement (y ignored). a_dt: seconds.
	KinematicResult KinematicStep(KinematicState& a_state, const Vec3& a_want, float a_dt, const KinematicParams& a_params, const RayFn& a_ray);

	// How far (<= a_maxDist) the body can go along a_dir (unit, horizontal) from a_feet before its
	// radius touches a wall, probing at knee (just above a step), waist, chest and head height. a_normal receives
	// the wall's horizontal normal when one was hit (estimated from two side rays).
	float WallClearance(const Vec3& a_feet, const Vec3& a_dir, float a_maxDist, const KinematicParams& a_params, const RayFn& a_ray, Vec3* a_normal);

	// The room above a_feet (m, from the feet to the first thing above them within a_max; INFINITY when nothing is): one ray up
	// from 0.25 m above the feet (on a step's edge or a slope it never meets the ground he stands on).
	float Headroom(const Vec3& a_feet, float a_max, const RayFn& a_ray);
	// Headroom that a seam can't fool: when the ray up at a_feet finds nothing, four more 7 cm around it (along x and z) decide.
	// Elden Ring's stairs are tread plates with 4-10 cm seams between them (live 2026-10-14, the Roundtable Hold's basement
	// stairs): a ray through a seam read "no ceiling" and a crouched Snake crept under them seam by seam to a 0.57 m ceiling.
	float SolidHeadroom(const Vec3& a_feet, float a_max, const RayFn& a_ray);

	// Where Snake can start standing (the puppet's start, kinematic): nullopt when the ground is under a_feet (within snap_down),
	// else the nearest point within 1.2 m where the body stands on ground (its centre and four points at its radius, within
	// step_up of the feet) and that it can reach without a wall -- nullopt again when there is none (a real drop: he falls). Live: off a ladder at the top of Fort
	// Haight's tower Elden Ring's capsule stood on the shaft's rim with its centre over the hole, and Snake fell 19 m down it.
	std::optional<Vec3> SupportedStart(const Vec3& a_feet, const KinematicParams& a_params, const RayFn& a_ray);

	// How boxed in the body is: of 8 horizontal directions, how many meet geometry within a_reach of the body's axis at knee
	// or chest height (a flat wall: ~3, a corner ~4-5, inside a cart or a rock: 7-8).
	int Enclosed(const Vec3& a_feet, float a_reach, const KinematicParams& a_params, const RayFn& a_ray);

	// A fall that never lands (roadmap 1.3, pure): with NoDead set, a chasm whose death is Elden Ring's kill volume (not a
	// landing) can't kill the puppet, and kinematic gravity would let Snake fall for ever. Update() is true once, the frame
	// a fall has lasted a_seconds or gone a_drop metres below where it began (0 = that check off); a landing re-arms it.
	class FallWatch
	{
	public:
		bool                Update(bool a_grounded, float a_feetY, float a_dt, float a_seconds, float a_drop);
		void                Reset() { *this = FallWatch(); }
		void                Shift(float a_dy) { startY_ += a_dy; }  // Elden Ring moved its world origin
		[[nodiscard]] float Seconds() const { return seconds_; }
		[[nodiscard]] float Drop() const { return drop_; }

	private:
		bool  falling_{ false };
		bool  fired_{ false };
		float seconds_{ 0 };
		float startY_{ 0 };
		float drop_{ 0 };
	};

	// Unsticking (pure): a puppet boxed in (Enclosed >= 7) for a_seconds goes back to the last place it stood free. And a stall
	// (Stall): Snake asked to move and was refused (a wall, a ceiling) in every direction he tried -- at least three of eight
	// sectors -- for a_seconds of trying (pauses don't count, up to 4 s each) also goes back. Live 2026-10-14 (the player's
	// session, under the Roundtable Hold's basement stairs): 59 wall contacts over 1353 refused frames and never boxed in 7 of 8:
	// the old check alone never fired.
	class Unstick
	{
	public:
		// Each frame: where the feet are, how boxed in, grounded, now (s); a_headroom: there is room for his height here (a
		// place under a low ceiling is never a "free" place to go back to). Returns the place to go back to, or nothing.
		std::optional<Vec3> Update(const Vec3& a_feet, int a_enclosed, bool a_grounded, double a_nowS, float a_seconds, bool a_headroom = true);
		// Each kinematic frame: what Snake asked (a_want, horizontal) and what the controller moved. A free place to go back to
		// (or nothing: a_noPlace then says the stall fired with none -- the caller hints at Tarnished mode).
		std::optional<Vec3> Stall(const Vec3& a_feet, const Vec3& a_want, const Vec3& a_moved, bool a_grounded, double a_nowS, float a_seconds,
			bool* a_noPlace = nullptr);
		void                Reset() { *this = Unstick(); }
		void                Shift(const Vec3& a_delta);  // Elden Ring moved its world origin
		[[nodiscard]] std::uint64_t Count() const { return count_; }
		[[nodiscard]] std::uint64_t Stalls() const { return stalls_; }   // stalls that fired (moved back or hinted)
		[[nodiscard]] float         StalledSeconds() const { return stalledS_; }

	private:
		std::optional<Vec3> Back(const Vec3& a_feet) const;
		Vec3          free_[8];
		int           freeCount_{ 0 };
		double        lastFreeS_{ -1.0 };
		double        boxedSinceS_{ -1.0 };
		std::uint64_t count_{ 0 };
		// stall
		float         stalledS_{ 0 };     // seconds of refused asking in this stall
		float         freeRunS_{ 0 };     // seconds of real moving since the stall's last refusal
		double        lastStallS_{ -1.0 };  // the last frame of this stall (refused or idle)
		std::uint8_t  sectors_{ 0 };      // directions tried (8 sectors)
		std::uint64_t stalls_{ 0 };
	};
}
