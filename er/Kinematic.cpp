#include "Kinematic.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

		// BodyGround's five probes for a body of radius a_radius: the centre, then four at kGroundProbeRadius x radius on a cross
		// turned 30 degrees -- planks laid along the world's axes (or any one direction) then see five different offsets across
		// their gaps, not three, and a regular gap pitch can't swallow them all. Opposite probes are 1/2 and 3/4.
		void ProbeOffsets(float a_radius, Vec3 (&a_out)[5])
		{
			const float r = kGroundProbeRadius * std::max(0.0f, a_radius);
			const float c = 0.8660254f * r, s = 0.5f * r;
			a_out[0] = Vec3{};
			a_out[1] = Vec3{ c, 0, s };
			a_out[2] = Vec3{ -c, 0, -s };
			a_out[3] = Vec3{ -s, 0, c };
			a_out[4] = Vec3{ s, 0, -c };
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

	GroundProbe BodyGround(const Vec3& a_feet, float a_from, float a_length, float a_radius, const RayFn& a_ray, bool a_probes)
	{
		const float r = kGroundProbeRadius * std::max(0.0f, a_radius);
		Vec3        offsets[5];
		ProbeOffsets(a_radius, offsets);
		const int   n = a_probes && r > 1e-3f ? 5 : 1;
		bool        found[5] = {};
		float       ys[5] = {};
		GroundProbe g;
		if (a_length <= 0.0f) {
			return g;
		}
		for (int i = 0; i < n; ++i) {
			const Vec3 from = a_feet + offsets[i] + Vec3{ 0.0f, a_from, 0.0f };
			Vec3       hit;
			if (!a_ray(from, Vec3{ 0.0f, -a_length, 0.0f }, hit) || !std::isfinite(hit.y)) {
				continue;
			}
			if (i > 0 && hit.y >= from.y - 1e-3f) {
				continue;  // a side ray from inside something (a wall it stands against): not ground
			}
			found[i] = true;
			ys[i] = hit.y;
			++g.hits;
		}
		g.centre = found[0];
		// A side probe is held when another probe (the centre too) found ground no more than the tolerance below it.
		bool  held[5] = {};
		float highest = NAN;
		for (int i = 1; i < n; ++i) {
			for (int j = 0; j < n && found[i] && !held[i]; ++j) {
				held[i] = j != i && found[j] && ys[j] >= ys[i] - kGroundSupportTolerance;
			}
			if (held[i] && !(ys[i] <= highest)) {
				highest = ys[i];
			}
		}
		// The side probes' plane under the axis: opposite probes (1/2, 3/4) are symmetric about it, so on any plane their mean is
		// its height there. Two pairs that disagree are no plane (a step inside the floor): the higher.
		const auto plane = [&](auto a_use) -> std::optional<float> {
			float m[2];
			int   k = 0;
			for (const auto& [a, b] : { std::pair{ 1, 2 }, std::pair{ 3, 4 } }) {
				if (b < n && a_use(a) && a_use(b)) {
					m[k++] = 0.5f * (ys[a] + ys[b]);
				}
			}
			if (k == 0) {
				return std::nullopt;
			}
			return k == 1 || std::fabs(m[0] - m[1]) <= kGroundPlaneTolerance ? 0.5f * (m[0] + m[k - 1]) : std::max(m[0], m[1]);
		};
		if (g.centre) {
			// The centre's ground, as one ray had it: on a slope the uphill probes are higher, and standing on them floats the body
			// (review 2026-10-14: 6-25 cm over 10-40 degree slopes). Three or more side probes on ground above it is a gap under the
			// centre instead (a slope or a step's edge lifts two of four at most: they are 90 degrees apart).
			g.supported = true;
			g.y = ys[0];
			int above = 0;
			for (int i = 1; i < n; ++i) {
				above += held[i] && ys[i] > ys[0] + kGroundSupportTolerance ? 1 : 0;
			}
			if (above >= 3) {
				const auto p = plane([&](int i) { return held[i] && ys[i] > ys[0] + kGroundSupportTolerance; });
				g.y = p ? *p : highest;
			}
		} else if (std::isfinite(highest)) {
			// Nothing under the centre (a gap down to a drop, or a rim): the side probes' plane, else the highest held probe. A
			// plane further than a 45-degree slope from that probe is something else (a pair across a step or a hole's two rims).
			g.supported = true;
			g.y = highest;
			if (const auto p = plane([&](int i) { return found[i]; }); p && std::fabs(*p - highest) <= r) {
				g.y = *p;
			}
		}
		for (int i = 0; i < n && g.supported; ++i) {
			g.level += found[i] && std::fabs(ys[i] - g.y) <= kGroundSupportTolerance ? 1 : 0;
		}
		return g;
	}

	RayFn BodyRays(RayFn a_ground, RayFn a_wall, RayFn a_wallAlso)
	{
		return [ground = std::move(a_ground), wall = std::move(a_wall), also = std::move(a_wallAlso)](const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit) {
			if (std::fabs(a_delta.y) > LengthXZ(a_delta)) {
				return ground(a_from, a_delta, a_hit);
			}
			Vec3       h1, h2;
			const bool b1 = wall(a_from, a_delta, h1);
			const bool b2 = also && also(a_from, a_delta, h2);
			if (!b1 && !b2) {
				return false;
			}
			a_hit = b1 && (!b2 || Dot(h1 - a_from, h1 - a_from) <= Dot(h2 - a_from, h2 - a_from)) ? h1 : h2;
			return true;
		};
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

	float SolidHeadroom(const Vec3& a_feet, float a_max, const RayFn& a_ray, float a_stepUp)
	{
		constexpr float kSeam = kSeamProbe;
		// The floor under a probe: a step up to a_stepUp higher is floor, and the room is measured from its top.
		const auto onGround = [&](Vec3 a_at) {
			if (a_stepUp > 0.0f) {
				const float rise = a_stepUp + 0.05f;
				Vec3        ground;
				if (a_ray(a_at + Vec3{ 0.0f, rise, 0.0f }, Vec3{ 0.0f, -rise, 0.0f }, ground) && ground.y > a_at.y) {
					a_at.y = ground.y;
				}
			}
			return a_at;
		};
		const auto roomAt = [&](const Vec3& a_at) {
			const Vec3 at = onGround(a_at);
			const float r = Headroom(at, a_max, a_ray);
			return std::isfinite(r) ? r : INFINITY;
		};
		float room = roomAt(a_feet);
		if (std::isfinite(room)) {
			return room;
		}
		for (const Vec3 off : { Vec3{ kSeam, 0, 0 }, Vec3{ -kSeam, 0, 0 }, Vec3{ 0, 0, kSeam }, Vec3{ 0, 0, -kSeam } }) {
			room = std::min(room, roomAt(a_feet + off));
		}
		return room;
	}

	namespace
	{
		// The ground a body in the air meets this frame (a fall, a hop, a jump): the whole body's (BodyGround) over the frame's
		// whole move (a_dy), from a_lift above the higher end to 0.05 m below the lower one -- consecutive frames' segments overlap,
		// so a plank or a thin platform is never stepped over between two frames however fast he falls (or rises: a ledge's top
		// is caught at any frame rate). Landed only where he can stand: on side
		// probes alone (nothing under his centre in the segment) the ground snap from there must keep him there. At a ledge's rim
		// with the floor below within snap_down the snap takes the centre's ground (he would step down at once, 0.6 m in a
		// frame): there he goes on falling past the rim (or, rising, on toward the top) instead.
		GroundProbe LandingGround(const Vec3& a_feet, float a_lift, float a_dy, const KinematicParams& a_params, const RayFn& a_ray)
		{
			GroundProbe g = BodyGround(a_feet, a_lift + std::max(a_dy, 0.0f), a_lift + 0.05f + std::fabs(a_dy), a_params.radius, a_ray, a_params.groundProbes);
			if (g.supported && !g.centre) {
				const float       top = a_params.stepUp + 0.05f;
				const GroundProbe stand = BodyGround(Vec3{ a_feet.x, g.y, a_feet.z }, top, top + a_params.snapDown, a_params.radius, a_ray, a_params.groundProbes);
				if (!stand.supported || std::fabs(stand.y - g.y) > 0.05f) {
					g.supported = false;
				}
			}
			return g;
		}

		// Rising in a hop or a jump: what is over his head (a_head m tall), as its height above a_feet (INFINITY: nothing within
		// a_reach). Rays up from half his height -- a ledge's top the feet are being lifted past is below that -- at his axis and,
		// when that finds nothing, the seam probes around it (Elden Ring's tread plates: SolidHeadroom).
		float CeilingOver(const Vec3& a_feet, float a_head, float a_reach, const RayFn& a_ray)
		{
			const float from = 0.5f * a_head;
			if (!(a_reach > from)) {
				return INFINITY;
			}
			const auto up = [&](const Vec3& a_at) {
				Vec3 hit;
				return a_ray(a_at + Vec3{ 0.0f, from, 0.0f }, Vec3{ 0.0f, a_reach - from, 0.0f }, hit) ? std::max(from, hit.y - a_feet.y) : INFINITY;
			};
			float room = up(a_feet);
			if (std::isfinite(room)) {
				return room;
			}
			for (const Vec3 off : { Vec3{ kSeamProbe, 0, 0 }, Vec3{ -kSeamProbe, 0, 0 }, Vec3{ 0, 0, kSeamProbe }, Vec3{ 0, 0, -kSeamProbe } }) {
				room = std::min(room, up(a_feet + off));
			}
			return room;
		}

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
			// Both look at the whole body's ground (BodyGround: its centre and four points at its radius), not one ray under its centre.
			const float top = a_params.stepUp + 0.05f;
			if (a_state.grounded) {
				const GroundProbe g = BodyGround(pos, top, top + a_params.snapDown, a_params.radius, a_ray, a_params.groundProbes);
				if (g.supported) {
					r.stepped = g.y > a_state.pos.y + 0.05f;
					pos.y = g.y;
					a_state.groundY = g.y;
					a_state.vy = 0.0f;
				} else {
					a_state.grounded = false;  // walked off a ledge: start falling next pass
					a_state.vy = 0.0f;
				}
			}
			if (!a_state.grounded) {
				// One air for a fall, a hop and a jump. A hop or a jump (aimed: AirStep passes its own a_params -- stepUp = its ledge
				// catch, bodyHeight = his height at the take-off) rises with its head checked and may land on a ledge above the feet.
				const bool  aimed = a_state.hopping || a_state.jumping;
				const float vy0 = a_state.vy;
				a_state.vy = std::max(vy0 - a_params.gravity * a_dt, -a_params.terminalSpeed);
				float dy = 0.5f * (vy0 + a_state.vy) * a_dt;  // the frame's mean speed: a hop's or a jump's apex is the one asked for
				if (aimed && dy > 0.0f) {
					// Rising: his head never goes into what is above it (bumped, he falls from there).
					const float head = a_params.bodyHeight;
					const float room = CeilingOver(pos, head, head + dy + 0.05f, a_ray);
					if (room < head + dy) {
						dy = std::max(0.0f, room - head - 0.02f);
						a_state.vy = 0.0f;
						r.headBump = true;
					}
				}
				// The landing looks from 0.1 m above the feet (kFallStep: a fall's knee ray), a hop's or a jump's from its ledge catch:
				// a ledge top the feet are just under is landed on (rising, only one above the take-off ground: the ground he left is no
				// landing). A plain fall going up lands on nothing.
				const float       lift = aimed ? a_params.stepUp + 0.05f : kFallStep + 0.05f;
				const GroundProbe g = dy <= 0.0f || aimed ? LandingGround(pos, lift, dy, a_params, a_ray) : GroundProbe{};
				if (g.supported && g.y <= pos.y + std::max(dy, 0.0f) + lift && (a_state.vy <= 0.0f || g.y > pos.y + 0.02f)) {
					r.landed = true;
					r.landingSpeed = std::max(0.0f, -a_state.vy);
					if (aimed) {
						// A jump or a hop is no fall: its landing counts only the height below its take-off (a level one: none).
						r.landingSpeed = std::min(r.landingSpeed, std::sqrt(2.0f * a_params.gravity * std::max(0.0f, a_state.jumpFromY - g.y)));
					}
					r.jumpLanded = a_state.jumping;
					pos.y = g.y;
					a_state.groundY = g.y;
					a_state.vy = 0.0f;
					a_state.grounded = true;
					a_state.hopping = a_state.jumping = false;
					a_state.airVel = {};
				} else {
					pos.y += dy;
				}
			}
			a_state.pos = pos;
			a_state.airS = a_state.grounded ? 0.0f : a_state.airS + a_dt;
			return r;
		}

		// The room for the body where it stands: at its axis and a little ahead of it (the head is not a point).
		float BodyHeadroom(const Vec3& a_feet, const Vec3& a_dir, const KinematicParams& a_params, const RayFn& a_ray)
		{
			const float probe = a_params.bodyHeight + 0.05f;
			float       room = SolidHeadroom(a_feet, probe, a_ray, a_params.stepUp);
			if (LengthXZ(a_dir) > 1e-4f) {
				// Measured from the ground there, not from his feet: on stairs the spot ahead is the next step, and from his feet the
				// step's own top read as a ceiling (live 2026-10-14, the broken stairs on Stormveil's side: "a ceiling 0.26 m above",
				// Snake couldn't climb them).
				Vec3        ahead = a_feet + a_dir * std::min(a_params.radius, 0.2f);
				const float rise = a_params.stepUp + 0.05f;
				Vec3        ground;
				if (a_ray(ahead + Vec3{ 0.0f, rise, 0.0f }, Vec3{ 0.0f, -rise, 0.0f }, ground) && ground.y > ahead.y) {
					ahead.y = ground.y;
				}
				room = std::min(room, SolidHeadroom(ahead, probe, a_ray, a_params.stepUp));
			}
			return room;
		}

		// A hop's or a jump's frame in the air (KinematicState::hopping / jumping). Its own horizontal velocity: a hop's fixed
		// (hopVel, along the dive), a jump's from the take-off, steered toward Snake's own motion at jumpAirControl m/s^2 at most.
		// The air's wall rays: the knee ray at its ledge catch (hopStep / jumpAirStep) above the feet -- in the air a ledge is a
		// wall until the feet are nearly over it (a 1 m wall is never hopped, a 1.7 m one never jumped). Never under anything
		// lower than his head where the move ends (at the take-off he had room; the air may carry him under a beam or a balcony):
		// that move is refused, he goes up or down where he is. The landing is Step's: the whole body's ground (LandingGround).
		KinematicResult AirStep(KinematicState& a_state, const Vec3& a_want, float a_dt, const KinematicParams& a_params, const RayFn& a_ray)
		{
			const bool      hop = a_state.hopping;
			KinematicParams air = a_params;
			air.stepUp = hop ? a_state.hopStep : a_params.jumpAirStep;
			air.bodyHeight = hop ? a_state.hopHead : JumpBodyHeight(a_params);
			const float dt = std::clamp(a_dt, 1.0f / 1000.0f, 0.1f);
			if (!hop) {
				// The take-off's velocity carries on; Snake's own motion steers it a little (none asked: it keeps going).
				const Vec3  wantVel = Horizontal(a_want) * (1.0f / dt);
				const float wantSpeed = LengthXZ(wantVel);
				if (wantSpeed > 0.5f && std::isfinite(wantSpeed)) {
					Vec3        dv = wantVel - a_state.airVel;
					const float most = a_params.jumpAirControl * dt;
					if (LengthXZ(dv) > most) {
						dv = dv * (most / LengthXZ(dv));
					}
					a_state.airVel += Horizontal(dv);
				}
			}
			const Vec3           vel = hop ? a_state.hopVel : a_state.airVel;
			const KinematicState before = a_state;
			KinematicResult      r = Step(a_state, vel * dt, a_dt, air, a_ray);
			if (LengthXZ(r.moved) > 1e-5f && !a_state.grounded) {
				const float room = BodyHeadroom(a_state.pos, Normalize(Horizontal(r.moved)), air, a_ray);
				r.headroom = room;
				if (room < air.bodyHeight) {
					a_state = before;
					r = Step(a_state, Vec3{}, a_dt, air, a_ray);  // up/down where he is; the air move goes on once there is room
					r.blocked = true;
					r.ceiling = true;
					r.headroom = room;
				}
			}
			if (hop) {
				a_state.hopLeftS -= dt;
				if (a_state.hopLeftS <= 0.0f) {
					a_state.hopping = false;  // long gone (off a cliff): an ordinary fall from here
				}
			}
			return r;
		}
	}

	float KinematicHop(KinematicState& a_state, const Vec3& a_dir, const HopParams& a_hop, const KinematicParams& a_params, const RayFn& a_ray)
	{
		const Vec3 flat = Horizontal(a_dir);
		if (a_state.hopping || a_state.jumping || a_hop.height <= 0.0f || a_hop.length <= 0.0f || LengthXZ(flat) < 1e-4f) {
			return 0.0f;
		}
		// The take-off: where he stands, or -- just off a rim (HopParams::graceS: the dive's flag came a few frames late) -- the
		// ground he left, at its height. Falling longer or further than that is an ordinary fall.
		Vec3 at = a_state.pos;
		if (!a_state.grounded) {
			const bool justLeft = a_state.airS <= a_hop.graceS && a_state.vy <= 0.0f && std::isfinite(a_state.groundY) &&
			                      at.y <= a_state.groundY + 0.01f && at.y >= a_state.groundY - a_hop.graceDrop;
			if (!justLeft) {
				return 0.0f;
			}
			at.y = a_state.groundY;
		}
		const Vec3 dir = flat * (1.0f / LengthXZ(flat));
		// His height at the take-off (the dive starts standing; mid-dive MGSV may already say prone, so it is kept for the
		// whole hop): the caller's (HopParams::head), else the stance's, else the head ray's height.
		const float head = a_hop.head > 0.0f ? a_hop.head : a_params.bodyHeight > 0.0f ? a_params.bodyHeight : a_params.headHeight;
		const float probe = head + a_hop.height + 0.05f;
		// Each probe from its own ground (a step ahead is floor, not a ceiling: SolidHeadroom's a_stepUp, as BodyHeadroom). From a
		// step's top the room is the smaller one, so the hop is never higher than the room from his feet.
		const float room = std::min(SolidHeadroom(at, probe, a_ray, a_params.stepUp),
		                            SolidHeadroom(at + dir * std::min(a_params.radius, 0.2f), probe, a_ray, a_params.stepUp));
		// Under a ceiling the hop is lower (never through it), and below minRise there is none: a doorway still gives a small
		// one, a low passage none.
		const float rise = std::isfinite(room) ? std::min(a_hop.height, room - head - 0.05f) : a_hop.height;
		if (!(rise >= a_hop.minRise)) {
			return 0.0f;
		}
		const float g = std::max(a_params.gravity, 0.1f);
		// The full hop's flight time: a lower one (a ceiling) is shorter, not slower.
		const float airtime = 2.0f * std::sqrt(2.0f * a_hop.height / g);
		a_state.pos = at;
		a_state.vy = std::sqrt(2.0f * g * rise);
		a_state.grounded = false;
		a_state.hopping = true;
		a_state.hopVel = dir * (a_hop.length / airtime);
		a_state.hopStep = std::clamp(a_hop.step, 0.0f, a_params.stepUp);
		a_state.hopHead = head;
		a_state.hopLeftS = airtime * 1.25f + 0.1f;
		a_state.jumpFromY = at.y;  // its landing counts as a fall only from here down (as a jump's)
		a_state.airS = INFINITY;  // a hop is no ground: once it ends (off a cliff), no grace from it
		return rise;
	}

	KinematicResult KinematicStep(KinematicState& a_state, const Vec3& a_want, float a_dt, const KinematicParams& a_params, const RayFn& a_ray)
	{
		if (a_state.hopping || a_state.jumping) {
			if (!a_state.grounded) {
				return AirStep(a_state, a_want, a_dt, a_params, a_ray);
			}
			a_state.hopping = a_state.jumping = false;  // put on the ground from outside (a warp, the unstick, Elden Ring moved him)
			a_state.airVel = {};
		}
		const KinematicState before = a_state;
		if (!a_state.grounded) {
			// A plain fall moves through the air as a hop or a jump does: a ledge is a wall until his feet are nearly at its top
			// (kFallStep; its top is landed on from there), never a step walked up in mid-air. With step_up's knee ray a body
			// falling past a ledge's face walked into it below its top and "landed" inside it (merge with the ground probes: a 6 m/s
			// walk crossed a 2 m gap 0.4 m down into its far side).
			KinematicParams fall = a_params;
			fall.stepUp = std::min(a_params.stepUp, kFallStep);
			return Step(a_state, a_want, a_dt, fall, a_ray);
		}
		KinematicResult r = Step(a_state, a_want, a_dt, a_params, a_ray);
		if (a_params.bodyHeight <= 0.0f || LengthXZ(r.moved) < 1e-5f) {
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
		const float hereAxis = SolidHeadroom(before.pos, a_params.bodyHeight + 0.05f, a_ray, a_params.stepUp);
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

	JumpStart KinematicJump(KinematicState& a_state, const Vec3& a_runVel, float a_apex, float a_minRoom, float a_maxSpeed, const KinematicParams& a_params,
		const RayFn& a_ray)
	{
		JumpStart j;
		if (!a_state.grounded || a_state.jumping || a_state.hopping) {
			j.why = "not on the ground";
			return j;
		}
		if (!(a_apex > 0.0f) || !(a_params.gravity > 0.0f)) {
			j.why = "no jump height";
			return j;
		}
		// The room above his head: rays up at his axis and at four points 0.7 radius around it, from just above a step (a
		// step's top is floor, not a ceiling), seams between Elden Ring's tread plates bridged by the side rays.
		const float body = JumpBodyHeight(a_params);
		const float from = std::min(a_params.stepUp + 0.05f, body * 0.5f);
		const float reach = body + a_apex + 0.2f;
		float       room = INFINITY;
		const float side = 0.7f * a_params.radius;
		for (const Vec3 off : { Vec3{}, Vec3{ side, 0, 0 }, Vec3{ -side, 0, 0 }, Vec3{ 0, 0, side }, Vec3{ 0, 0, -side } }) {
			Vec3 hit;
			if (a_ray(a_state.pos + off + Vec3{ 0.0f, from, 0.0f }, Vec3{ 0.0f, reach - from, 0.0f }, hit)) {
				room = std::min(room, hit.y - a_state.pos.y);
			}
		}
		j.room = room - body;
		if (j.room < a_minRoom) {
			j.why = j.room < 0.0f ? "a ceiling lower than him" : "a ceiling right over his head";
			return j;
		}
		j.apex = std::min(a_apex, j.room - 0.05f);
		Vec3        vel = Horizontal(a_runVel);
		const float speed = LengthXZ(vel);
		if (!std::isfinite(speed)) {
			vel = {};
		} else if (speed > a_maxSpeed && a_maxSpeed > 0.0f) {
			vel = vel * (a_maxSpeed / speed);
		}
		j.speed = LengthXZ(vel);
		a_state.vy = std::sqrt(2.0f * a_params.gravity * j.apex);
		a_state.grounded = false;
		a_state.jumping = true;
		a_state.airVel = vel;
		a_state.jumpFromY = a_state.pos.y;
		a_state.airS = INFINITY;  // a jump is no ground: no dive's grace from it (KinematicHop)
		j.ok = true;
		return j;
	}

	std::optional<Vec3> SupportedStart(const Vec3& a_feet, const KinematicParams& a_params, const RayFn& a_ray)
	{
		const float top = a_params.stepUp + 0.05f;
		auto        ground = [&](const Vec3& a_at, Vec3& a_hit) { return a_ray(a_at + Vec3{ 0.0f, top, 0.0f }, Vec3{ 0.0f, -(top + a_params.snapDown), 0.0f }, a_hit); };
		Vec3        hit;
		// The body stands here: the centre's ground, or four of five probes (planks with gaps under its centre). Not two probes on a
		// rim with the centre over a drop, though they hold a walking body (review 2026-10-14: a start 0.1 m past Fort Haight's
		// shaft rim stayed there, 4 cm from losing the second probe and falling 19 m).
		if (StartGround(BodyGround(a_feet, top, top + a_params.snapDown, a_params.radius, a_ray, a_params.groundProbes))) {
			return std::nullopt;
		}
		constexpr int kDirections = 16;
		// A plank floor seldom has the whole body (five points at its full radius) on wood: at each distance, failing that, a point
		// where BodyGround finds all five of its probes on ground at its level within step_up -- planks with gaps hold the body, a
		// rim doesn't. Five, not four: the cross is fixed in world axes, and with an arm square to an edge four probes stand with
		// the centre a centimetre inside it (two sit level with the centre); five keep the centre 0.2 m or more in.
		for (float r = 0.15f; r <= 1.2f + 1e-4f; r += 0.15f) {
			std::optional<Vec3> best;
			std::optional<Vec3> loose;
			for (int i = 0; i < kDirections; ++i) {
				const float a = static_cast<float>(i) * (6.2831853f / kDirections);
				const Vec3  dir{ std::sin(a), 0.0f, std::cos(a) };
				const Vec3  at = a_feet + dir * r;
				// The whole body on ground, not another rim (live: the first point found was the ladder's own top, 0.1 m down).
				bool body = ground(at, hit) && std::fabs(hit.y - a_feet.y) <= a_params.stepUp;
				for (int k = 0; k < 4 && body; ++k) {
					const float b = static_cast<float>(k) * 1.5707963f;
					Vec3        under;
					body = ground(at + Vec3{ std::sin(b), 0.0f, std::cos(b) } * a_params.radius, under) && std::fabs(under.y - hit.y) <= a_params.stepUp;
				}
				std::optional<Vec3> here;
				if (body) {
					here = Vec3{ at.x, hit.y, at.z };
				} else if (!best && a_params.groundProbes) {
					const GroundProbe g = BodyGround(at, top, top + a_params.snapDown, a_params.radius, a_ray);
					if (g.supported && g.level >= 5 && std::fabs(g.y - a_feet.y) <= a_params.stepUp &&
						(!loose || std::fabs(g.y - a_feet.y) < std::fabs(loose->y - a_feet.y)) &&
						WallClearance(a_feet, dir, r, a_params, a_ray, nullptr) >= r - 1e-3f) {
						loose = Vec3{ at.x, g.y, at.z };
					}
				}
				if (!here || WallClearance(a_feet, dir, r, a_params, a_ray, nullptr) < r - 1e-3f) {
					continue;  // no whole body on ground, or behind a wall
				}
				if (!best || std::fabs(here->y - a_feet.y) < std::fabs(best->y - a_feet.y)) {
					best = here;
				}
			}
			if (best) {
				return best;
			}
			if (loose) {
				return loose;
			}
		}
		return std::nullopt;
	}

	void GroundProbes(const Vec3& a_feet, float a_up, float a_down, float a_radius, const RayFn& a_ray, float (&a_out)[5])
	{
		Vec3 offsets[5];
		ProbeOffsets(a_radius, offsets);
		for (int i = 0; i < 5; ++i) {
			Vec3 hit;
			a_out[i] = a_ray(a_feet + offsets[i] + Vec3{ 0.0f, a_up, 0.0f }, Vec3{ 0.0f, -a_down, 0.0f }, hit) ? a_feet.y - hit.y : NAN;
		}
	}

	void KinematicShift(KinematicState& a_state, const Vec3& a_delta)
	{
		a_state.pos += a_delta;
		a_state.groundY += a_delta.y;
		a_state.jumpFromY += a_delta.y;
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
