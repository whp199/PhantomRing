#include "Mantle.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace phantom::er
{
	namespace
	{
		constexpr Vec3  kUp{ 0.0f, 1.0f, 0.0f };
		constexpr float kFaceHeights[] = { 0.3f, 0.7f, 1.1f };  // the face is looked for from the knees up
		constexpr float kTopOffsets[] = { 0.06f, 0.15f, 0.25f };  // the top is looked for this far past the face
		constexpr float kStep = 0.3f;  // a hit at least this high past the face is the obstacle's top, not the ground

		// The nearest wall straight ahead (horizontal distance from the axis), INFINITY when none within a_max.
		float Face(const Vec3& a_feet, const Vec3& a_dir, float a_max, const RayFn& a_ray)
		{
			float best = INFINITY;
			for (const float h : kFaceHeights) {
				const Vec3 from = a_feet + kUp * h;
				Vec3       hit;
				if (a_ray(from, a_dir * a_max, hit)) {
					best = std::min(best, LengthXZ(hit - from));
				}
			}
			return best;
		}

		// The first thing straight below a_from within a_len: its height (NAN none); a_inside: the ray began inside something.
		float Down(const Vec3& a_from, float a_len, const RayFn& a_ray, bool* a_inside = nullptr)
		{
			Vec3 hit;
			if (!a_ray(a_from, Vec3{ 0.0f, -a_len, 0.0f }, hit)) {
				return NAN;
			}
			if (a_inside && a_from.y - hit.y < 1e-3f) {
				*a_inside = true;
			}
			return hit.y;
		}

		bool Clear(const Vec3& a_from, const Vec3& a_to, const RayFn& a_ray)
		{
			Vec3 hit;
			return !a_ray(a_from, a_to - a_from, hit);
		}

		// Room for the body (radius a_r) rising from a_feet to a_height above them: up its axis, and at its sides, back and front
		// above the ledge's top a_top (the face itself is there below it; a beam over the face is not).
		bool RoomToRise(const Vec3& a_feet, const Vec3& a_dir, float a_r, float a_height, float a_top, float a_stepUp, const RayFn& a_ray)
		{
			(void)a_stepUp;  // one ray up his axis: SolidHeadroom's seam probes start inside rough rock beside him (live, Gatefront)
			if (Headroom(a_feet, a_height + 0.05f, a_ray) < a_height) {
				return false;
			}
			// Natural faces are rough and lean: the columns start a little above the ledge's own top (live 2026-10-31, Gatefront's
			// rocks: a face leaning 0.3 m toward him and a top 0.1 m higher at his side read "no room" from knee height).
			const Vec3  side{ a_dir.z, 0.0f, -a_dir.x };
			const float from = std::max(0.6f, a_top + 0.25f);
			for (const Vec3 off : { side * (0.8f * a_r), side * (-0.8f * a_r), a_dir * (-0.8f * a_r), a_dir * (0.8f * a_r) }) {
				if (from < a_height && !Clear(a_feet + off + kUp * from, a_feet + off + kUp * a_height, a_ray)) {
					return false;
				}
			}
			return true;
		}

		// The body moving at heights a_y0 (+ a_rise) from above a_from to above a_to (both feet points, horizontal travel):
		// nothing in the way along its axis, its sides (0.8 r) and its front (r beyond a_to).
		bool ClearOver(const Vec3& a_from, const Vec3& a_to, float a_y0, float a_rise, const Vec3& a_dir, float a_r, const RayFn& a_ray)
		{
			const Vec3 side{ a_dir.z, 0.0f, -a_dir.x };
			const Vec3 a{ a_from.x, a_y0, a_from.z }, b{ a_to.x, a_y0, a_to.z };
			const int  steps = std::max(2, static_cast<int>(std::ceil(a_rise / 0.2f)));  // every 0.2 m at most: a beam is ~0.1-0.3 m
			for (int i = 0; i <= steps; ++i) {
				const float h = a_rise * static_cast<float>(i) / static_cast<float>(steps);
				if (!Clear(a + kUp * h, b + kUp * h + a_dir * a_r, a_ray)) {
					return false;
				}
			}
			for (const float s : { 0.8f, -0.8f }) {
				const Vec3 o = side * (s * a_r) + kUp * (0.5f * a_rise);
				if (!Clear(a + o, b + o, a_ray)) {
					return false;
				}
			}
			return true;
		}

		float Ease(float a_u)
		{
			const float u = std::clamp(a_u, 0.0f, 1.0f);
			return u * u * (3.0f - 2.0f * u);
		}
	}

	MantlePlan PlanMantle(const Vec3& a_feet, const Vec3& a_dir, float a_body, const MantleParams& a_m, const KinematicParams& a_params, const RayFn& a_ray,
		bool a_vault)
	{
		MantlePlan p;
		const Vec3 flat{ a_dir.x, 0.0f, a_dir.z };
		if (!(LengthXZ(flat) > 1e-4f) || !Finite(a_feet)) {
			p.why = "no direction";
			return p;
		}
		p.dir = Normalize(flat);
		const Vec3& dir = p.dir;
		const float r = a_params.radius;
		const float body = a_body > 0.0f ? a_body : JumpBodyHeight(a_params);

		// 1. A face in reach.
		const float face = Face(a_feet, dir, r + a_m.reach, a_ray);
		if (!std::isfinite(face)) {
			p.why = "nothing in front to climb";
			return p;
		}
		p.face = face;
		p.ledge = true;

		// 2. Its top, just past the face: the first surface above step height, looked for from above maxHeight down -- and from
		// under anything higher there (a ceiling over the top, a beam over the face) until the start is free.
		const float probeTop = a_m.maxHeight + 0.35f;
		float       top = NAN;
		bool        wall = false, overMax = false;
		for (const float o : kTopOffsets) {
			const Vec3 col = a_feet + dir * (face + o);
			for (float y0 = probeTop; y0 >= a_m.minHeight && !std::isfinite(top);) {
				bool        inside = false;
				const float y = Down(col + kUp * y0, y0 - (kStep - 0.05f), a_ray, &inside);
				if (inside) {
					wall = true;
					y0 -= 0.1f;
				} else if (std::isfinite(y) && y - a_feet.y > a_m.maxHeight) {
					overMax = true;
					y0 = y - a_feet.y - 0.1f;  // something up there above his reach: look under it
				} else {
					if (std::isfinite(y)) {
						top = y;
						wall = overMax = false;
					}
					break;
				}
			}
			if (std::isfinite(top)) {
				break;
			}
		}
		if (!std::isfinite(top)) {
			p.height = probeTop;
			p.why = wall ? "too high to climb (a wall)" : overMax ? "too high to climb" : "no top to climb onto (an opening or a gap past it)";
			return p;
		}
		const float h = top - a_feet.y;
		p.height = h;
		if (h > a_m.maxHeight) {
			p.why = "too high to climb";
			return p;
		}
		if (h < a_m.minHeight) {
			p.why = "low enough to step onto";
			return p;
		}

		// 3. Room to rise along the face (his body bent over the top).
		if (!RoomToRise(a_feet, dir, r, h + a_m.climbRoom, h, a_params.stepUp, a_ray)) {
			p.why = "no room above him to climb";
			return p;
		}

		// 4a. Over it: a thin obstacle (its top at most vaultMaxDepth deep), ground behind it, room there.
		const char* vaultWhy = "too high to vault";
		bool        vaultOk = false;
		float       farEdge = NAN, yB = NAN;
		float       crest = top;  // the highest point of its top on the way over (a rubble wall's stones, a railing's post)
		Vec3        B;
		if (h <= a_m.vaultMaxHeight) {
			vaultWhy = "too deep to vault over";
			for (float o = 0.1f; o <= a_m.vaultMaxDepth + 1e-3f; o += 0.1f) {
				bool        inside = false;
				const float y = Down(a_feet + dir * (face + o) + kUp * (h + 0.45f), h + 0.45f + a_m.vaultMaxDrop + 0.5f, a_ray, &inside);
				if (inside || (std::isfinite(y) && y > top + 0.35f)) {
					vaultWhy = "a wall behind it";
					break;
				}
				if (!std::isfinite(y) || y < top - 0.3f) {
					farEdge = o;
					break;
				}
				crest = std::max(crest, y);
			}
			if (crest - a_feet.y > a_m.vaultMaxHeight) {
				farEdge = NAN;
				vaultWhy = "too high to vault";
			}
			if (std::isfinite(farEdge)) {
				B = a_feet + dir * (face + farEdge + r + 0.15f);
				yB = Down(Vec3{ B.x, crest + 0.3f, B.z }, crest + 0.3f - (a_feet.y - a_m.vaultMaxDrop), a_ray);
				vaultWhy = "a drop behind it";
				if (std::isfinite(yB)) {
					B.y = yB;
					const auto g = BodyGround(B, 0.3f, 0.6f, r, a_ray, a_params.groundProbes);
					vaultWhy = "no ground to land on behind it";
					if (g.supported && std::fabs(g.y - yB) < 0.25f) {
						B.y = g.y;
						vaultWhy = "no room to land behind it";
						if (Headroom(B, body + 0.05f, a_ray) >= body) {
							vaultWhy = "something in the way over it";
							const Vec3 mid = a_feet + dir * (face + 0.5f * farEdge);
							if (ClearOver(a_feet, B, crest + 0.3f, 0.6f, dir, r, a_ray) &&
								Headroom(Vec3{ mid.x, crest, mid.z }, 1.0f, a_ray) >= 0.9f) {
								vaultOk = true;
								p.depth = std::max(0.05f, farEdge - 0.05f);
								p.drop = a_feet.y - B.y;
							}
						}
					}
				}
			}
		}

		// 4b. Onto it: a top to stand on, flat enough, with room for his height.
		const char* climbWhy = "nothing to stand on up there";
		bool        climbOk = false;
		Vec3        L = a_feet + dir * (face + r + 0.1f);
		{
			const float yL = Down(Vec3{ L.x, top + 0.3f, L.z }, 0.6f, a_ray);
			if (std::isfinite(yL)) {
				L.y = yL;
				climbWhy = "its top slopes too much";
				if (std::fabs(yL - top) <= a_m.maxSlope * (r + 0.1f) + 0.05f) {
					const auto g = BodyGround(L, 0.3f, 0.6f, r, a_ray, a_params.groundProbes);
					climbWhy = "not enough top to stand on";
					if (g.supported && std::fabs(g.y - yL) < 0.15f) {
						L.y = g.y;
						climbWhy = "no room to stand up there";
						if (Headroom(L, body + 0.05f, a_ray) >= body) {
							climbWhy = "something in the way up there";
							if (ClearOver(a_feet, L, std::max(top, L.y) + 0.3f, std::min(body, 1.2f) - 0.3f, dir, r, a_ray)) {
								climbOk = true;
							}
						}
					}
				}
			}
		}

		if (vaultOk && (a_vault || !climbOk)) {
			p.kind = MantleKind::kVault;
			p.duration = a_m.vaultBaseS + a_m.vaultPerM * h + 0.2f * std::max(0.0f, p.drop);
			const Vec3 over = a_feet + dir * (face + farEdge + r);
			p.keys = { a_feet, Vec3{ a_feet.x, crest + 0.25f, a_feet.z }, Vec3{ over.x, crest + 0.25f, over.z }, B };
			p.keyT = { 0.0f, 0.35f * p.duration, 0.7f * p.duration, p.duration };
			p.keyCount = 4;
			return p;
		}
		if (climbOk) {
			p.kind = MantleKind::kClimb;
			p.duration = a_m.climbBaseS + a_m.climbPerM * h;
			p.keys = { a_feet, Vec3{ a_feet.x, top + 0.05f, a_feet.z }, L, L };
			p.keyT = { 0.0f, 0.55f * p.duration, p.duration, p.duration };
			p.keyCount = 3;
			return p;
		}
		p.why = h <= a_m.vaultMaxHeight && std::isfinite(farEdge) ? vaultWhy : climbWhy;
		return p;
	}

	Vec3 MantlePoint(const MantlePlan& a_plan, float a_t)
	{
		const int n = a_plan.keyCount;
		if (n <= 0) {
			return {};
		}
		if (a_t <= 0.0f || n == 1) {
			return a_plan.keys[0];
		}
		for (int i = 0; i + 1 < n; ++i) {
			const auto  k = static_cast<std::size_t>(i);
			const float t0 = a_plan.keyT[k], t1 = a_plan.keyT[k + 1];
			if (a_t <= t1) {
				const float s = t1 > t0 ? Ease((a_t - t0) / (t1 - t0)) : 1.0f;
				return a_plan.keys[k] + (a_plan.keys[k + 1] - a_plan.keys[k]) * s;
			}
		}
		return a_plan.End();
	}

	std::string_view Name(MantleKind a_kind)
	{
		switch (a_kind) {
		case MantleKind::kNone: return "none";
		case MantleKind::kClimb: return "climb up";
		case MantleKind::kVault: return "vault over";
		}
		return "?";
	}

	std::optional<Vec3> MantleRun::Update(std::uint64_t a_nowMs, bool a_guestMantling, std::uint64_t a_waitMs, bool* a_done)
	{
		if (a_done) {
			*a_done = false;
		}
		switch (phase_) {
		case Phase::kIdle:
			return std::nullopt;
		case Phase::kWaiting:
			if (!a_guestMantling && a_nowMs - askedMs_ < a_waitMs) {
				return plan_.keys[0];
			}
			phase_ = Phase::kMoving;
			startMs_ = a_nowMs;
			guest_ = a_guestMantling;
			[[fallthrough]];
		case Phase::kMoving: {
			const float t = static_cast<float>(a_nowMs - startMs_) / 1000.0f;
			if (t >= plan_.duration) {
				phase_ = Phase::kIdle;
				if (a_done) {
					*a_done = true;
				}
				return plan_.End();
			}
			return MantlePoint(plan_, t);
		}
		}
		return std::nullopt;
	}

	void MantleRun::Shift(const Vec3& a_delta)
	{
		for (auto& k : plan_.keys) {
			k += a_delta;
		}
	}
}
