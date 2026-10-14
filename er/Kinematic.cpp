#include "Kinematic.h"

#include <algorithm>
#include <cmath>

namespace phantom::er
{
	namespace
	{
		Vec3 Horizontal(const Vec3& v) { return { v.x, 0.0f, v.z }; }

		// The wall-ray heights for the body's stance: knee (just above a step: anything lower is a step the ground pass
		// climbs), waist, chest and head -- those above bodyHeight left out, the knee ray always kept.
		int WallHeights(const KinematicParams& a_p, float (&a_out)[4])
		{
			const float knee = a_p.stepUp + 0.05f;
			const float all[4] = { knee, std::max(a_p.stepUp + 0.1f, a_p.waistHeight), a_p.chestHeight, a_p.headHeight };
			int         n = 0;
			for (const float h : all) {
				if (n == 0 || a_p.bodyHeight <= 0.0f || h < a_p.bodyHeight) {
					a_out[n++] = h;
				}
			}
			return n;
		}

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
		float     heights[4];
		const int nh = WallHeights(a_params, heights);
		for (int i = 0; i < nh; ++i) {
			const float h = heights[i];
			Vec3        hit;
			const Vec3  origin = a_feet + Vec3{ 0.0f, h, 0.0f };
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

	float Headroom(const Vec3& a_feet, float a_max, const RayFn& a_ray)
	{
		constexpr float kFrom = 0.25f;
		Vec3            hit;
		if (a_max <= kFrom || !a_ray(a_feet + Vec3{ 0.0f, kFrom, 0.0f }, Vec3{ 0.0f, a_max - kFrom, 0.0f }, hit)) {
			return INFINITY;
		}
		return std::max(0.0f, hit.y - a_feet.y);
	}

	float SolidHeadroom(const Vec3& a_feet, float a_max, const RayFn& a_ray)
	{
		constexpr float kSeam = 0.07f;  // wider than the widest seam seen (10 cm across both sides), well inside the body
		float           room = Headroom(a_feet, a_max, a_ray);
		if (std::isfinite(room)) {
			return room;
		}
		for (const Vec3 off : { Vec3{ kSeam, 0, 0 }, Vec3{ -kSeam, 0, 0 }, Vec3{ 0, 0, kSeam }, Vec3{ 0, 0, -kSeam } }) {
			room = std::min(room, Headroom(a_feet + off, a_max, a_ray));
		}
		return room;
	}

	namespace
	{
		KinematicResult Step(KinematicState& a_state, const Vec3& a_want, float a_dt, const KinematicParams& a_params, const RayFn& a_ray);

		// The room for the body where it stands: at its axis and a little ahead of it (the head is not a point).
		float BodyHeadroom(const Vec3& a_feet, const Vec3& a_dir, const KinematicParams& a_params, const RayFn& a_ray)
		{
			const float probe = a_params.bodyHeight + 0.05f;
			float       room = SolidHeadroom(a_feet, probe, a_ray);
			if (LengthXZ(a_dir) > 1e-4f) {
				room = std::min(room, SolidHeadroom(a_feet + a_dir * std::min(a_params.radius, 0.2f), probe, a_ray));
			}
			return room;
		}
	}

	KinematicResult KinematicStep(KinematicState& a_state, const Vec3& a_want, float a_dt, const KinematicParams& a_params, const RayFn& a_ray)
	{
		const KinematicState before = a_state;
		KinematicResult      r = Step(a_state, a_want, a_dt, a_params, a_ray);
		if (a_params.bodyHeight <= 0.0f || LengthXZ(r.moved) < 1e-5f || !before.grounded) {
			return r;
		}
		// A ceiling lower than his height where the move ended (the underside of a staircase, a table): refused, unless it is no
		// lower than where he is (under one already -- he stood up beneath it, or Elden Ring put him there: walking out must work).
		const Vec3  dir = Normalize(Horizontal(r.moved));
		const float after = BodyHeadroom(a_state.pos, dir, a_params, a_ray);
		r.headroom = after;
		if (after >= a_params.bodyHeight) {
			a_state.underRefusedS = 0;
			a_state.ceilingEscape = false;  // free again
			return r;
		}
		// Where he stands, at his axis: free (nothing lower than him above him) or already under something.
		const float hereAxis = SolidHeadroom(before.pos, a_params.bodyHeight + 0.05f, a_ray);
		const bool  under = hereAxis < a_params.bodyHeight;
		if (under && before.ceilingEscape) {
			return r;  // under a ceiling, refused long enough: he walks (out) until he stands free
		}
		// Free, he never goes under anything lower than him (live 2026-10-14: comparing with the room "here" measured 0.2 m ahead
		// let a free Snake creep 1 cm under a 1.79 m underside, and from there the way-out rule took him in). Already under one
		// (he stood up beneath it, Elden Ring put him there), a move that is no lower than here is fine: walking out works.
		const float here = under ? BodyHeadroom(before.pos, dir, a_params, a_ray) : INFINITY;
		if (under && after + 0.02f >= here) {
			a_state.underRefusedS = 0;
			return r;
		}
		a_state = before;
		KinematicResult refused = Step(a_state, Vec3{}, a_dt, a_params, a_ray);  // stand where he was (ground snap, no move)
		refused.blocked = true;
		refused.ceiling = true;
		refused.headroom = after;  // the room where the move would have ended
		refused.wallPoint = before.pos + dir * a_params.radius + Vec3{ 0.0f, std::min(after, a_params.bodyHeight), 0.0f };
		// Already under one (here lower than him: Elden Ring put him there, he stood up beneath it) and every try refused: after
		// kCeilingEscapeS the check stands aside until he is free. Never while he is free (here >= his height): walking into the
		// low space stays refused however long he pushes.
		a_state.underRefusedS = under ? before.underRefusedS + a_dt : 0.0f;
		if (a_state.underRefusedS >= kCeilingEscapeS) {
			a_state.ceilingEscape = true;
			refused.ceilingEscape = true;
		}
		return refused;
	}

	namespace
	{
	KinematicResult Step(KinematicState& a_state, const Vec3& a_want, float a_dt, const KinematicParams& a_params, const RayFn& a_ray)
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
			float     heights[4];
			const int nh = WallHeights(a_params, heights);
			for (int k = 0; k < nh; ++k) {
				Vec3 hit;
				if (a_ray(a_feet + Vec3{ 0.0f, heights[k], 0.0f }, dir * a_reach, hit)) {
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

	std::optional<Vec3> Unstick::Update(const Vec3& a_feet, int a_enclosed, bool a_grounded, double a_nowS, float a_seconds, bool a_headroom)
	{
		if (a_enclosed <= 4 && a_grounded && a_headroom && stalledS_ <= 0.0f) {
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
		boxedSinceS_ = -1.0;
		++count_;
		return Back(a_feet);
	}

	std::optional<Vec3> Unstick::Back(const Vec3& a_feet) const
	{
		if (freeCount_ == 0) {
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
		return back;
	}

	std::optional<Vec3> Unstick::Stall(const Vec3& a_feet, const Vec3& a_want, const Vec3& a_moved, bool a_grounded, double a_nowS, float a_seconds,
		bool* a_noPlace)
	{
		if (a_noPlace) {
			*a_noPlace = false;
		}
		// Not asking (or in the air) is a pause in a stall; one longer than 4 s ends it.
		if (lastStallS_ >= 0.0 && a_nowS - lastStallS_ > 4.0) {
			stalledS_ = 0;
			sectors_ = 0;
			freeRunS_ = 0;
		}
		const float dt = lastStallS_ < 0.0 ? 0.0f : static_cast<float>(std::clamp(a_nowS - lastStallS_, 0.0, 0.1));
		const float want = LengthXZ(a_want);
		if (want < 1e-4f || !a_grounded) {
			return std::nullopt;
		}
		const bool refused = LengthXZ(a_moved) < 0.25f * want;
		if (!refused) {
			// Moving: a quarter second of it ends a stall.
			if (stalledS_ > 0.0f) {
				freeRunS_ += dt > 0.0f ? dt : 1.0f / 60.0f;
				if (freeRunS_ >= 0.25f) {
					stalledS_ = 0;
					sectors_ = 0;
					freeRunS_ = 0;
				}
			}
			lastStallS_ = a_nowS;
			return std::nullopt;
		}
		freeRunS_ = 0;
		if (stalledS_ <= 0.0f) {
			stalledS_ = 1e-4f;  // a stall starts (its first frame counts no time)
		} else {
			stalledS_ += dt;
		}
		lastStallS_ = a_nowS;
		const float a = std::atan2(a_want.x, a_want.z);  // -pi..pi
		const int   sector = static_cast<int>(std::floor((a + 3.14159265f) / 0.78539816f + 0.5f)) & 7;
		sectors_ = static_cast<std::uint8_t>(sectors_ | (1u << sector));
		int tried = 0;
		for (int i = 0; i < 8; ++i) {
			tried += (sectors_ >> i) & 1;
		}
		if (stalledS_ < a_seconds || tried < 3) {
			return std::nullopt;
		}
		stalledS_ = 0;
		sectors_ = 0;
		++stalls_;
		const auto back = Back(a_feet);
		if (!back || LengthXZ(*back - a_feet) < 0.05f) {
			if (a_noPlace) {
				*a_noPlace = true;
			}
			return std::nullopt;
		}
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
