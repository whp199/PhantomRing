#include "Kinematic.h"

#include <algorithm>
#include <cmath>

namespace phantom::er
{
	namespace
	{
		Vec3 Horizontal(const Vec3& v) { return { v.x, 0.0f, v.z }; }

		// Wall normal from two parallel rays offset sideways: the line through both hits is the wall's
		// trace, its horizontal perpendicular facing back along the move is the normal. cast_ray only
		// returns a hit point, so this is how we get a slide direction.
		bool WallNormal(const Vec3& a_origin, const Vec3& a_dir, float a_reach, const KinematicParams& a_params, const RayFn& a_ray, Vec3& a_normal)
		{
			const Vec3 side{ -a_dir.z, 0.0f, a_dir.x };
			Vec3       h1, h2;
			if (!a_ray(a_origin + side * a_params.slideProbe, a_dir * a_reach, h1) || !a_ray(a_origin - side * a_params.slideProbe, a_dir * a_reach, h2)) {
				return false;
			}
			const Vec3 trace = Horizontal(h1 - h2);
			if (LengthXZ(trace) < 1e-4f) {
				return false;
			}
			Vec3 n = Normalize(Vec3{ -trace.z, 0.0f, trace.x });
			if (Dot(n, a_dir) > 0) {
				n = -n;
			}
			a_normal = n;
			return true;
		}
	}

	float WallClearance(const Vec3& a_feet, const Vec3& a_dir, float a_maxDist, const KinematicParams& a_params, const RayFn& a_ray, Vec3* a_normal)
	{
		// Reach a few radii past the move so an oblique wall is seen before the body touches it.
		const float reach = a_maxDist + 3.0f * a_params.radius;
		float       nearest = INFINITY;
		// Knee ray just above the step height (anything lower is a step the ground pass climbs) and
		// a chest ray (overhangs, railings).
		for (const float h : { a_params.stepUp + 0.05f, std::max(a_params.stepUp + 0.1f, a_params.waistHeight), a_params.chestHeight, a_params.headHeight }) {
			Vec3       hit;
			const Vec3 origin = a_feet + Vec3{ 0.0f, h, 0.0f };
			if (a_ray(origin, a_dir * reach, hit)) {
				nearest = std::min(nearest, LengthXZ(hit - origin));
			}
		}
		if (!std::isfinite(nearest)) {
			return a_maxDist;
		}
		// Keep the body radius *perpendicular* to the wall: along the move that is radius / cos.
		Vec3 n;
		if (WallNormal(a_feet + Vec3{ 0.0f, a_params.stepUp + 0.05f, 0.0f }, a_dir, reach + 0.5f, a_params, a_ray, n)) {
			if (a_normal) {
				*a_normal = n;
			}
			const float cosA = -Dot(a_dir, n);
			if (cosA < 0.05f) {
				return a_maxDist;  // moving parallel to it
			}
			return std::clamp((nearest * cosA - a_params.radius) / cosA, 0.0f, a_maxDist);
		}
		if (a_normal) {
			*a_normal = -a_dir;
		}
		return std::clamp(nearest - a_params.radius, 0.0f, a_maxDist);
	}

	KinematicResult KinematicStep(KinematicState& a_state, const Vec3& a_want, float a_dt, const KinematicParams& a_params, const RayFn& a_ray)
	{
		KinematicResult r;
		a_dt = std::clamp(a_dt, 1.0f / 1000.0f, 0.1f);

		// ---- horizontal: wall stop, then one slide along the wall --------------------------------
		Vec3        want = Horizontal(a_want);
		const float len = LengthXZ(want);
		Vec3        pos = a_state.pos;
		if (len > 1e-5f) {
			const Vec3  dir = want * (1.0f / len);
			Vec3        n = -dir;
			const float clear = WallClearance(pos, dir, len, a_params, a_ray, &n);
			pos += dir * clear;
			r.moved = dir * clear;
			if (clear + 1e-4f < len) {
				r.blocked = true;
				r.wallPoint = pos + dir * a_params.radius + Vec3{ 0.0f, a_params.stepUp + 0.05f, 0.0f };
				const float remain = len - clear;
				const Vec3  slide = (dir - n * Dot(dir, n)) * remain;
				const float slideLen = LengthXZ(slide);
				if (slideLen > 1e-5f) {
					const Vec3  sdir = Horizontal(slide) * (1.0f / slideLen);
					const float sclear = WallClearance(pos, sdir, slideLen, a_params, a_ray, nullptr);
					if (sclear > 1e-5f) {
						pos += sdir * sclear;
						r.moved += sdir * sclear;
						r.slid = true;
					}
				}
			}
		}

		// ---- vertical: step up / ground snap while grounded, gravity while falling ---------------
		// The probe starts stepUp above the feet so a ledge up to that height becomes the new ground.
		const float top = a_params.stepUp + 0.05f;
		Vec3        hit;
		if (a_state.grounded) {
			const Vec3 from = pos + Vec3{ 0.0f, top, 0.0f };
			if (a_ray(from, Vec3{ 0.0f, -(top + a_params.snapDown), 0.0f }, hit)) {
				r.stepped = hit.y > a_state.pos.y + 0.05f;
				pos.y = hit.y;
				a_state.groundY = hit.y;
				a_state.vy = 0.0f;
			} else {
				a_state.grounded = false;  // walked off a ledge: start falling next pass
				a_state.vy = 0.0f;
			}
		}
		if (!a_state.grounded) {
			a_state.vy = std::max(a_state.vy - a_params.gravity * a_dt, -a_params.terminalSpeed);
			const float drop = a_state.vy * a_dt;  // <= 0
			const Vec3  from = pos + Vec3{ 0.0f, 0.1f, 0.0f };
			if (a_ray(from, Vec3{ 0.0f, drop - 0.15f, 0.0f }, hit) && hit.y <= pos.y + 0.1f) {
				r.landed = true;
				r.landingSpeed = -a_state.vy;
				pos.y = hit.y;
				a_state.groundY = hit.y;
				a_state.vy = 0.0f;
				a_state.grounded = true;
			} else {
				pos.y += drop;
			}
		}
		a_state.pos = pos;
		return r;
	}

	std::optional<Vec3> SupportedStart(const Vec3& a_feet, const KinematicParams& a_params, const RayFn& a_ray)
	{
		const float top = a_params.stepUp + 0.05f;
		auto        ground = [&](const Vec3& a_at, Vec3& a_hit) { return a_ray(a_at + Vec3{ 0.0f, top, 0.0f }, Vec3{ 0.0f, -(top + a_params.snapDown), 0.0f }, a_hit); };
		Vec3        hit;
		if (ground(a_feet, hit)) {
			return std::nullopt;
		}
		constexpr int kDirections = 16;
		for (float r = 0.15f; r <= 1.2f + 1e-4f; r += 0.15f) {
			std::optional<Vec3> best;
			for (int i = 0; i < kDirections; ++i) {
				const float a = static_cast<float>(i) * (6.2831853f / kDirections);
				const Vec3  dir{ std::sin(a), 0.0f, std::cos(a) };
				const Vec3  at = a_feet + dir * r;
				if (!ground(at, hit) || std::fabs(hit.y - a_feet.y) > a_params.stepUp) {
					continue;
				}
				// The whole body on ground, not another rim (live: the first point found was the ladder's own top, 0.1 m down).
				bool body = true;
				for (int k = 0; k < 4 && body; ++k) {
					const float b = static_cast<float>(k) * 1.5707963f;
					Vec3        under;
					body = ground(at + Vec3{ std::sin(b), 0.0f, std::cos(b) } * a_params.radius, under) && std::fabs(under.y - hit.y) <= a_params.stepUp;
				}
				if (!body) {
					continue;
				}
				if (WallClearance(a_feet, dir, r, a_params, a_ray, nullptr) < r - 1e-3f) {
					continue;  // behind a wall
				}
				if (!best || std::fabs(hit.y - a_feet.y) < std::fabs(best->y - a_feet.y)) {
					best = Vec3{ at.x, hit.y, at.z };
				}
			}
			if (best) {
				return best;
			}
		}
		return std::nullopt;
	}

	int Enclosed(const Vec3& a_feet, float a_reach, const KinematicParams& a_params, const RayFn& a_ray)
	{
		int blocked = 0;
		for (int i = 0; i < 8; ++i) {
			const float a = static_cast<float>(i) * 0.7853982f;
			const Vec3  dir{ std::sin(a), 0.0f, std::cos(a) };
			// The same heights the move's wall probes use (WallClearance): live 2026-10-13, Snake ended up with only his waist ray
			// blocked all round (inside a table-high plank at the Stormhill Shack: knee 2 of 8, chest 2 of 8, waist 8 of 8): every
			// move was refused and, counting knee and chest only, he never read as boxed in -- stuck for good.
			for (const float h : { a_params.stepUp + 0.05f, std::max(a_params.stepUp + 0.1f, a_params.waistHeight), a_params.chestHeight, a_params.headHeight }) {
				Vec3 hit;
				if (a_ray(a_feet + Vec3{ 0.0f, h, 0.0f }, dir * a_reach, hit)) {
					++blocked;
					break;
				}
			}
		}
		return blocked;
	}

	bool FallWatch::Update(bool a_grounded, float a_feetY, float a_dt, float a_seconds, float a_drop)
	{
		if (a_grounded || !std::isfinite(a_feetY)) {
			*this = FallWatch();
			return false;
		}
		if (!falling_) {
			falling_ = true;
			startY_ = a_feetY;
		}
		seconds_ += std::max(0.0f, a_dt);
		drop_ = std::max(drop_, startY_ - a_feetY);
		if (!fired_ && ((a_seconds > 0.0f && seconds_ >= a_seconds) || (a_drop > 0.0f && drop_ >= a_drop))) {
			fired_ = true;
			return true;
		}
		return false;
	}

	std::optional<Vec3> Unstick::Update(const Vec3& a_feet, int a_enclosed, bool a_grounded, double a_nowS, float a_seconds)
	{
		if (a_enclosed <= 4 && a_grounded) {
			boxedSinceS_ = -1.0;
			// Remember where it stood free, every quarter second (the last 2 s).
			if (lastFreeS_ < 0 || a_nowS - lastFreeS_ >= 0.25) {
				lastFreeS_ = a_nowS;
				if (freeCount_ < 8) {
					free_[freeCount_++] = a_feet;
				} else {
					std::copy(free_ + 1, free_ + 8, free_);
					free_[7] = a_feet;
				}
			}
			return std::nullopt;
		}
		if (a_enclosed < 7) {
			boxedSinceS_ = -1.0;
			return std::nullopt;
		}
		if (boxedSinceS_ < 0) {
			boxedSinceS_ = a_nowS;
		}
		if (a_nowS - boxedSinceS_ < a_seconds || freeCount_ == 0) {
			return std::nullopt;
		}
		// The newest free place at least 0.5 m away (the latest ones may be at the edge of what boxed it in).
		Vec3 back = free_[0];
		for (int i = freeCount_ - 1; i >= 0; --i) {
			if (LengthXZ(free_[i] - a_feet) >= 0.5f) {
				back = free_[i];
				break;
			}
		}
		boxedSinceS_ = -1.0;
		++count_;
		return back;
	}

	void Unstick::Shift(const Vec3& a_delta)
	{
		for (int i = 0; i < freeCount_; ++i) {
			free_[i] += a_delta;
		}
	}
}
