#include "Mantle.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace phantom::er
{
	namespace
	{
		constexpr Vec3  kUp{ 0.0f, 1.0f, 0.0f };
		// The face is looked for from the knees up -- every 0.2 m: a Limgrave rail fence's two rails slipped between 0.3, 0.7 and 1.1 m
		// (live 2026-10-31: "nothing in front to climb" at a fence the walk was stopped by; running into it broke it).
		constexpr float kFaceHeights[] = { 0.3f, 0.5f, 0.7f, 0.9f, 1.1f };
		constexpr float kTopOffsets[] = { 0.06f, 0.15f, 0.25f };  // the top is looked for this far past the face
		constexpr float kStep = 0.3f;  // a hit at least this high past the face is the obstacle's top, not the ground
		constexpr float kSquareMinDeg = 5.0f;    // a face this slanted or more may be planned square to it ...
		constexpr float kSquareFirstDeg = 25.0f; // ... and from this slant on, square first
		constexpr int   kFarLandings = 4;        // a climb's landing looked for this many steps farther past the first ...
		constexpr float kFarStep = 0.25f;        // ... this far apart (m) ...
		constexpr float kFarRise = 0.7f;         // ... each up to this much higher than the highest ground before it

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

		// The last blocking hit of Clear() / RoomToRise() / ClearOver() (the plan's diagnostics).
		thread_local Vec3 t_block;
		thread_local bool t_blocked{ false };

		bool Clear(const Vec3& a_from, const Vec3& a_to, const RayFn& a_ray)
		{
			Vec3 hit;
			if (a_ray(a_from, a_to - a_from, hit)) {
				t_block = hit;
				t_blocked = true;
				return false;
			}
			return true;
		}

		// Room for the body (radius a_r) rising from a_feet to a_height above them: up its axis, and at its sides, back and front
		// above the ledge's top a_top (the face itself is there below it; a beam over the face is not).
		bool RoomToRise(const Vec3& a_feet, const Vec3& a_dir, float a_r, float a_height, float a_top, float a_stepUp, const RayFn& a_ray)
		{
			(void)a_stepUp;  // one ray up his axis: SolidHeadroom's seam probes start inside rough rock beside him (live, Gatefront)
			if (const float room = Headroom(a_feet, a_height + 0.05f, a_ray); room < a_height) {
				t_block = a_feet + kUp * room;
				t_blocked = true;
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
			// Every 0.08 m at most: a beam or a rail is ~0.1-0.3 m (every 0.2 m let a 0.12 m beam over a ledge slip between two rays
			// once rays from inside a shape stopped counting, as Elden Ring's don't: tests/test_host_mantle.inc MantleRayEr).
			const int  steps = std::max(2, static_cast<int>(std::ceil(a_rise / 0.08f)));
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

	namespace
	{
	MantlePlan PlanAlong(const Vec3& a_feet, const Vec3& a_dir, float a_body, const MantleParams& a_m, const KinematicParams& a_params, const RayFn& a_ray,
		bool a_vault)
	{
		t_blocked = false;
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
		// under anything higher there (a ceiling over the top, a beam over the face) until the start is free. The highest of the
		// three columns: a rounded rock's lip is lower than its crown a hand further on (live 2026-10-31, Gatefront: a boulder's
		// 0.40 m lip read "low enough to step onto" while its crown stopped the walk -- Snake stood stuck against it).
		const float probeTop = a_m.maxHeight + 0.35f;
		float       top = NAN;
		bool        wall = false, overMax = false;
		for (const float o : kTopOffsets) {
			const Vec3 col = a_feet + dir * (face + o);
			float      t = NAN;
			bool       colWall = false, colOver = false;
			for (float y0 = probeTop; y0 >= a_m.minHeight && !std::isfinite(t);) {
				bool        inside = false;
				const float y = Down(col + kUp * y0, y0 - (kStep - 0.05f), a_ray, &inside);
				if (inside) {
					colWall = true;
					y0 -= 0.1f;
				} else if (std::isfinite(y) && y - a_feet.y > a_m.maxHeight) {
					colOver = true;
					y0 = y - a_feet.y - 0.1f;  // something up there above his reach: look under it
				} else {
					if (std::isfinite(y)) {
						t = y;
					}
					break;
				}
			}
			if (std::isfinite(t)) {
				top = std::isfinite(top) ? std::max(top, t) : t;
			} else if (!std::isfinite(top)) {
				wall = wall || colWall;
				overMax = overMax || colOver;
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
		t_blocked = false;  // the refusal's block (diagnostics) is the climb's from here on
		const char* climbWhy = "nothing to stand on up there";
		bool        climbOk = false;
		Vec3        L = a_feet + dir * (face + r + 0.1f);
		float       climbTop = top;  // the highest ground his body passes over on the way (a farther landing: the stack's)
		{
			// From well above the top (live 2026-10-31, Gatefront's rocks: a top rising past its lip had the old start 0.3 m over the
			// lip inside the rock -- Elden Ring's rays don't hit from inside -- and read "nothing to stand on up there").
			const float from = std::min(top + 0.65f, a_feet.y + a_m.maxHeight + 0.05f);
			const float yL = Down(Vec3{ L.x, from, L.z }, from - (top - 0.3f), a_ray);
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
		// Rubble stacked in steps, a rock whose top rises past its lip (live 2026-10-31: the Church of Elleh's rubble, Gatefront's
		// rocks -- "no room to stand up there", "not enough top to stand on", "its top slopes too much"): a landing farther on,
		// up to kFarLandings x kFarStep past the first, on the highest ground met so far, still no higher than maxHeight above
		// his feet, with room for his height and nothing in the way over everything he passes (ClearOver above the highest of
		// it). Never past a drop, a gap or a wall higher than he climbs.
		// (A rock rising past a low lip read "something in the way up there" too, live: its crown is in the way of the nearest landing,
		// not of one on top of it. A beam over the face stays in the way of every landing: ClearOver checks each.)
		if (!climbOk && climbWhy != std::string_view("nothing to stand on up there")) {
			const float maxTop = a_feet.y + a_m.maxHeight;
			float       highest = std::max(top, L.y);
			float       prev = L.y;
			const char* farWhy = climbWhy;
			for (int k = 1; k <= kFarLandings && !climbOk; ++k) {
				Vec3        C = a_feet + dir * (face + r + 0.1f + kFarStep * static_cast<float>(k));
				const float from = std::min(maxTop + 0.05f, highest + kFarRise);
				bool        inside = false;
				const float yC = Down(Vec3{ C.x, from, C.z }, from - (prev - 0.35f), a_ray, &inside);
				if (inside) {
					farWhy = "a wall past it, higher than he climbs";
					break;
				}
				if (!std::isfinite(yC)) {
					break;  // it drops away past there: the first refusal stands
				}
				prev = yC;
				highest = std::max(highest, yC);
				C.y = yC;
				const auto g = BodyGround(C, 0.3f, 0.6f, r, a_ray, a_params.groundProbes);
				if (!g.supported || std::fabs(g.y - yC) >= 0.15f) {
					continue;
				}
				C.y = g.y;
				highest = std::max(highest, C.y);
				if (Headroom(C, body + 0.05f, a_ray) < body) {
					continue;
				}
				// Room to rise to the highest top along the face, then over everything to C.
				if (highest - a_feet.y > a_m.maxHeight || !RoomToRise(a_feet, dir, r, highest - a_feet.y + a_m.climbRoom, highest - a_feet.y, a_params.stepUp, a_ray)) {
					farWhy = "no room above him to climb";
					break;
				}
				if (!ClearOver(a_feet, C, highest + 0.3f, std::min(body, 1.2f) - 0.3f, dir, r, a_ray)) {
					farWhy = "something in the way up there";
					continue;
				}
				climbOk = true;
				L = C;
				climbTop = highest;
				p.farther = k;
			}
			climbWhy = farWhy;
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
			p.height = std::max(h, climbTop - a_feet.y);  // a farther landing: the rise over the highest of the stack
			p.duration = a_m.climbBaseS + a_m.climbPerM * p.height;
			p.keys = { a_feet, Vec3{ a_feet.x, climbTop + 0.05f, a_feet.z }, L, L };
			p.keyT = { 0.0f, 0.55f * p.duration, p.duration, p.duration };
			p.keyCount = 3;
			return p;
		}
		p.why = h <= a_m.vaultMaxHeight && std::isfinite(farEdge) ? vaultWhy : climbWhy;
		return p;
	}
	}

	std::optional<Vec3> FaceNormal(const Vec3& a_feet, const Vec3& a_dir, float a_max, float a_spread, const RayFn& a_ray)
	{
		const Vec3 flat{ a_dir.x, 0.0f, a_dir.z };
		if (!(LengthXZ(flat) > 1e-4f)) {
			return std::nullopt;
		}
		const Vec3 dir = Normalize(flat);
		const Vec3 side{ dir.z, 0.0f, -dir.x };
		// The height whose ray along a_dir hit nearest: the face there.
		float best = INFINITY, bestH = 0.0f;
		for (const float h : kFaceHeights) {
			Vec3 hit;
			if (a_ray(a_feet + kUp * h, dir * a_max, hit)) {
				const float d = LengthXZ(hit - (a_feet + kUp * h));
				if (d < best) {
					best = d;
					bestH = h;
				}
			}
		}
		if (!std::isfinite(best)) {
			return std::nullopt;
		}
		// Three parallel rays across it: the normal from the outer two (or the centre and one of them), facing back toward him.
		Vec3 pts[3];
		bool hit[3]{};
		for (int i = 0; i < 3; ++i) {
			const Vec3 from = a_feet + kUp * bestH + side * (a_spread * static_cast<float>(i - 1));
			hit[i] = a_ray(from, dir * (a_max + a_spread * 2.0f), pts[i]);
		}
		Vec3 along;
		if (hit[0] && hit[2]) {
			along = pts[2] - pts[0];
		} else if (hit[1] && hit[2]) {
			along = pts[2] - pts[1];
		} else if (hit[0] && hit[1]) {
			along = pts[1] - pts[0];
		} else {
			return std::nullopt;
		}
		along.y = 0.0f;
		if (!(LengthXZ(along) > 1e-4f)) {
			return std::nullopt;
		}
		Vec3 n = Normalize(Vec3{ along.z, 0.0f, -along.x });
		if (Dot(n, dir) > 0.0f) {
			n = n * -1.0f;
		}
		return n;
	}

	MantlePlan PlanMantle(const Vec3& a_feet, const Vec3& a_dir, float a_body, const MantleParams& a_m, const KinematicParams& a_params, const RayFn& a_ray,
		bool a_vault, float a_maxSlantDeg)
	{
		auto plan = [&](const Vec3& a_along) {
			MantlePlan p = PlanAlong(a_feet, a_along, a_body, a_m, a_params, a_ray, a_vault);
			if (p.kind == MantleKind::kNone && t_blocked) {
				p.hasBlock = true;
				p.block = t_block;
			}
			return p;
		};
		// The face's normal: how slanted the approach is, and the square direction to plan along.
		const float reach = a_params.radius + a_m.reach;
		const auto  n = FaceNormal(a_feet, a_dir, reach + 0.6f, 0.2f, a_ray);
		float       slant = -1.0f;
		if (n) {
			const Vec3 dir = Normalize(Vec3{ a_dir.x, 0.0f, a_dir.z });
			slant = std::acos(std::clamp(-Dot(*n, dir), -1.0f, 1.0f)) * 57.29578f;
		}
		const bool squareable = n && a_maxSlantDeg > 0.0f && slant >= kSquareMinDeg && slant <= a_maxSlantDeg;
		// Well off the normal: square up first (MGSV climbs and vaults straight on; a diagonal vault would cross the fence
		// lengthwise), the asked direction only if that fails. Nearly straight on: as asked, square only if that fails.
		MantlePlan first = plan(squareable && slant >= kSquareFirstDeg ? *n * -1.0f : a_dir);
		first.squared = squareable && slant >= kSquareFirstDeg;
		first.slant = slant;
		if (first.kind != MantleKind::kNone || !squareable) {
			return first;
		}
		MantlePlan second = plan(first.squared ? a_dir : *n * -1.0f);
		second.squared = !first.squared;
		second.slant = slant;
		if (second.kind != MantleKind::kNone) {
			return second;
		}
		return first.squared ? second : first;  // the refusal of what he asked says why
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
