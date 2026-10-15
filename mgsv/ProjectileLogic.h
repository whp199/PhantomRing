#pragma once
// Which of MGSV's G-buffer draws is a projectile Snake just released (protocol v13 kOwnerProjectile): pure rules, no game
// access -- tests/test_guest_logic.cpp runs them offline (mgsv/ProjectileTag.h keeps the releases and asks these).
//
// Why a prediction: MGSV draws its thrown grenade and its fired rocket as ordinary rigid G-buffer draws with nothing that
// names them. What the guest does know is the release (kGEvThrow: release point + velocity; kGEvFire with the explosive bit:
// muzzle + direction) and the game frame of it. For a short window after that, a rigid draw that sits where the projectile
// must be by now -- on the throw's parabola, or ahead of the muzzle on the round's line -- is the projectile. The window is
// short (the host needs two lists a few frames apart that show it flying, er/Projectiles.h) and the tolerance grows with the
// projectile's speed, because the draw's frame and the release's frame can differ by a frame or two (MGSV records draws a
// frame late). Being wrong costs Snake a draw in Elden Ring's frame, so the rules would rather tag nothing for a few frames
// than tag his gear: nothing near the hand, nothing behind the release, nothing further down a line than a round can be.

#include <cmath>
#include <cstdint>

namespace phantom::guest::projectile
{
	struct Release
	{
		std::uint64_t frame{ 0 };  // game frame of the release (0 = empty slot)
		float         p0[3]{};     // release point / muzzle (MGSV world)
		float         v0[3]{};     // thrown: initial velocity (m/s); round: unit direction
		bool          round{ false };  // a fired round (rocket, grenade-launcher round): along its line, speed unknown
	};

	struct TagParams
	{
		float         gravity{ 9.8f };       // m/s^2 (MGSV's grenades fall like Elden Ring's copy: [Throw] gravity on the host)
		float         fps{ 60.0f };          // MGSV's game frames per second
		std::uint32_t windowFrames{ 30 };    // frames after the release a draw may be tagged
		float         tolerance{ 0.45f };    // m around the predicted point (plus two frames of the projectile's travel)
		float         minOut{ 0.6f };        // m it must be out of the hand / ahead of the muzzle (Snake's hand, the launcher tube)
		float         lineRadius{ 0.5f };    // m off a round's line
		float         roundMaxSpeed{ 120.0f };  // m/s: no explosive round of Snake's is faster (the host flies them at 16-80)
		float         roundMaxDrop{ 0.4f };  // m: a grenade-launcher round's drop allowed off the line (a rocket's line doesn't widen)
	};

	inline float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

	// Is a rigid draw whose origin is a_at (MGSV world), drawn in game frame a_frame, the projectile a_r released?
	inline bool Matches(const Release& a_r, std::uint64_t a_frame, const float a_at[3], const TagParams& a_p)
	{
		if (!a_r.frame || a_frame <= a_r.frame || a_frame - a_r.frame > a_p.windowFrames || !(a_p.fps > 1.0f)) {
			return false;
		}
		const float t = static_cast<float>(a_frame - a_r.frame) / a_p.fps;
		const float d[3] = { a_at[0] - a_r.p0[0], a_at[1] - a_r.p0[1], a_at[2] - a_r.p0[2] };
		if (!std::isfinite(d[0]) || !std::isfinite(d[1]) || !std::isfinite(d[2])) {
			return false;
		}
		const float drop = 0.5f * a_p.gravity * t * t;
		if (a_r.round) {
			const float len = std::sqrt(Dot3(a_r.v0, a_r.v0));
			if (!(len > 0.5f)) {
				return false;
			}
			const float u[3] = { a_r.v0[0] / len, a_r.v0[1] / len, a_r.v0[2] / len };
			const float along = Dot3(d, u);
			if (along < a_p.minOut) {
				return false;  // the launcher itself (behind the muzzle) and Snake's hands
			}
			// Not further than the round can have gone by now: a prop, the walker or a soldier's gun down range is on the line
			// too (review: a draw 40 m out was tagged one frame after the shot -- before the rocket itself had cleared 0.6 m).
			if (along > a_p.minOut + a_p.tolerance + a_p.roundMaxSpeed * (t + 2.0f / a_p.fps)) {
				return false;
			}
			const float off[3] = { d[0] - u[0] * along, d[1] - u[1] * along, d[2] - u[2] * along };
			// A grenade-launcher round drops off the line, a rocket doesn't: the drop widens it a little, never by metres.
			return std::sqrt(Dot3(off, off)) <= a_p.lineRadius + std::fmin(drop, a_p.roundMaxDrop);
		}
		const float speed = std::sqrt(Dot3(a_r.v0, a_r.v0));
		const float pred[3] = { a_r.v0[0] * t, a_r.v0[1] * t - drop, a_r.v0[2] * t };
		const float e[3] = { d[0] - pred[0], d[1] - pred[1], d[2] - pred[2] };
		const float slack = a_p.tolerance + speed * 2.0f / a_p.fps;
		// Nothing while the sphere the grenade may be in still reaches the hand: in the first frames it is a shell around the
		// hand that takes in Snake's own rigid gear (a holstered sidearm 0.7 m below, the gun in his other hand, a slung rifle
		// behind) while the grenade itself is still inside minOut -- and the host would learn that gear as the grenade.
		if (std::sqrt(Dot3(pred, pred)) - slack < a_p.minOut) {
			return false;
		}
		// The draw itself well out of the hand (his gear is within ~0.7 m of it), and ahead of the release, not behind it.
		if (std::sqrt(Dot3(d, d)) < a_p.minOut + a_p.tolerance || Dot3(d, a_r.v0) <= 0.0f) {
			return false;
		}
		return std::sqrt(Dot3(e, e)) <= slack;
	}
}
