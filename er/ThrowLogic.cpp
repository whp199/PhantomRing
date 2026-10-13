#include "ThrowLogic.h"

#include <algorithm>
#include <cmath>

namespace phantom::er::throwing
{
	namespace
	{
		const Vec3 kDown{ 0, -1, 0 };

		// Rolling: the part of gravity along the surface pulls it downhill, the rest is the surface's; it slows by
		// rollDecel; a ray along -normal keeps it on the surface (none: it rolled off an edge and flies again).
		void Roll(Projectile& a_p, float a_h, const Params& a_params, const RayFn& a_ray)
		{
			const Vec3 n = a_p.normal;
			const Vec3 g = kDown * a_params.gravity;
			const Vec3 along = g - n * Dot(g, n);
			Vec3       v = a_p.vel - n * Dot(a_p.vel, n) + along * a_h;
			const float speed = Length(v);
			const float slowed = std::max(0.0f, speed - a_params.rollDecel * a_h);
			v = speed > 1e-4f ? v * (slowed / speed) : Vec3{};
			const Vec3 move = v * a_h;
			// Into a wall along the way: stop there.
			Vec3 to = a_p.pos + move;
			if (Length(move) > 1e-4f) {
				if (const auto hit = a_ray(a_p.pos, move + Normalize(move) * a_params.radius)) {
					to = hit->point + hit->normal * a_params.radius;
					v = {};
				}
			}
			// Back onto the surface below.
			const float probe = 0.25f + a_params.radius;
			if (const auto ground = a_ray(to + n * probe, n * -(probe * 2.0f))) {
				a_p.pos = ground->point + ground->normal * a_params.radius;
				a_p.normal = ground->normal;
			} else {
				a_p.pos = to;
				a_p.rolling = false;  // over an edge: flying again
			}
			a_p.vel = v;
			if (a_p.rolling && Length(v) < a_params.restSpeed && a_p.normal.y > 0.75f) {
				a_p.resting = true;
				a_p.vel = {};
			}
		}
	}

	void Step(Projectile& a_p, float a_dt, const Params& a_params, const RayFn& a_ray, bool a_stick)
	{
		if (a_p.resting || a_p.lost || !(a_dt > 0.0f)) {
			a_p.age += std::max(0.0f, a_dt);
			return;
		}
		float remaining = std::min(a_dt, 0.25f);  // a long hitch is not a teleport
		while (remaining > 1e-6f && !a_p.resting && !a_p.lost) {
			const float h = std::min(remaining, std::max(1e-3f, a_params.maxStep));
			remaining -= h;
			a_p.age += h;
			if (a_p.age > a_params.maxTime || !Finite(a_p.pos) || !Finite(a_p.vel)) {
				a_p.lost = true;
				return;
			}
			if (a_p.rolling) {
				Roll(a_p, h, a_params, a_ray);
				continue;
			}
			const Vec3 move = a_p.vel * h + kDown * (0.5f * a_params.gravity * h * h);
			const Vec3 velEnd = a_p.vel + kDown * (a_params.gravity * h);
			const float len = Length(move);
			const auto hit = len > 1e-5f ? a_ray(a_p.pos, move + move * (a_params.radius / len)) : std::nullopt;
			if (!hit) {
				a_p.pos = a_p.pos + move;
				a_p.vel = velEnd;
				continue;
			}
			// Contact: the velocity at the hit, reflected off the surface (restitution into it, friction along it).
			const Vec3  n = hit->normal;
			const float into = Dot(velEnd, n);
			a_p.pos = hit->point + n * a_params.radius;
			a_p.normal = n;
			a_p.touched = true;
			++a_p.bounces;
			if (a_stick) {
				a_p.vel = {};
				a_p.resting = true;
				return;
			}
			const Vec3 tangent = velEnd - n * into;
			const Vec3 out = n * (-into * a_params.restitution) + tangent * a_params.friction;
			if (std::abs(into * a_params.restitution) < a_params.restSpeed && n.y > 0.5f) {
				a_p.rolling = true;  // too slow to bounce off ground-like surfaces: it rolls on
				a_p.vel = tangent * a_params.friction;
			} else {
				a_p.vel = out;
			}
		}
	}

	Arc TraceArc(const Vec3& a_pos, const Vec3& a_vel, const Params& a_params, const RayFn& a_ray, float a_spacing, float a_maxTime)
	{
		Arc        arc;
		const float spacing = std::max(0.05f, a_spacing);
		Vec3       pos = a_pos;
		Vec3       vel = a_vel;
		float      sinceDot = 0.0f;
		arc.dots.push_back(pos);
		const float h = std::max(1.0f / 120.0f, a_params.maxStep);
		for (float t = 0.0f; t < a_maxTime && Finite(pos); t += h) {
			const Vec3  move = vel * h + kDown * (0.5f * a_params.gravity * h * h);
			const float len = Length(move);
			Vec3        end = pos + move;
			const auto  hit = len > 1e-5f ? a_ray(pos, move) : std::nullopt;
			if (hit) {
				end = hit->point;
			}
			// Dots along this piece, a_spacing apart along the path.
			const float piece = Length(end - pos);
			float       at = spacing - sinceDot;
			while (at <= piece && piece > 1e-6f) {
				arc.dots.push_back(pos + (end - pos) * (at / piece));
				at += spacing;
			}
			sinceDot = piece - (at - spacing);
			if (hit) {
				arc.landing = hit;
				return arc;
			}
			pos = end;
			vel = vel + kDown * (a_params.gravity * h);
		}
		return arc;
	}

	Vec3 PlaneNormal(const Vec3& a_a, const Vec3& a_b, const Vec3& a_c, const Vec3& a_toward, const Vec3& a_fallback)
	{
		Vec3        n = Cross(a_b - a_a, a_c - a_a);
		const float l = Length(n);
		if (!(l > 1e-6f) || !Finite(n)) {
			return a_fallback;
		}
		n = n * (1.0f / l);
		return Dot(n, a_toward) < 0.0f ? -n : n;
	}

	Sphere Enclosing(const std::vector<Sphere>& a_spheres)
	{
		if (a_spheres.empty()) {
			return {};
		}
		Vec3 lo = a_spheres.front().centre, hi = lo;
		for (const auto& s : a_spheres) {
			lo = { std::min(lo.x, s.centre.x), std::min(lo.y, s.centre.y), std::min(lo.z, s.centre.z) };
			hi = { std::max(hi.x, s.centre.x), std::max(hi.y, s.centre.y), std::max(hi.z, s.centre.z) };
		}
		Sphere out{ (lo + hi) * 0.5f, 0.0f };
		for (const auto& s : a_spheres) {
			out.radius = std::max(out.radius, Length(s.centre - out.centre) + std::max(0.0f, s.radius));
		}
		return out;
	}
}
