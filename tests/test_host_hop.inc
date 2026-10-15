// Included by tests/test_host.cpp: MGSV's dive as a hop of the kinematic puppet (roadmap 2.9, er/Kinematic.h KinematicHop).
// Snake can't jump; the hop carries him over short gaps and onto low ledges, never up a 1 m wall, never into a ceiling.

namespace
{
	// Lanes along +z, 10 m apart in x (each feature spans x in [lane - 2, lane + 2]); ground y = 0 unless a lane says otherwise.
	//   x = 0:  a 1 m gap (z in [1, 2], 5 m deep)
	//   x = 10: a 0.6 m ledge (z >= 1)
	//   x = 20: a 0.5 m ledge (z >= 1)
	//   x = 30: a 1 m wall (z in [1, 1.3], open ground behind it)
	//   x = 40: a ceiling slab y in [2.1, 2.3] over the whole lane
	//   x = 50: a ceiling slab y in [1.9, 2.1] over the whole lane
	//   x = 60: an overhang y in [2.0, 2.2] for z >= 0.8 (open sky above the take-off)
	//   x = 70: a 1 m gap (z in [1, 2]) under a ceiling y in [1.9, 2.1] (no hop: he falls in as he would walking)
	//   x = 80: solid stairs from z = 0.15, 0.3 m steps 0.4 m deep, up to a 1.8 m landing (the 0.2 m probe ahead is in a step)
	bool HopSolid(const Vec3& p)
	{
		const auto lane = [&](float a_x) { return p.x >= a_x - 2.0f && p.x <= a_x + 2.0f; };
		float ground = 0.0f;
		if ((lane(0.0f) || lane(70.0f)) && p.z >= 1.0f && p.z <= 2.0f) {
			ground = -5.0f;
		} else if (lane(10.0f) && p.z >= 1.0f) {
			ground = 0.6f;
		} else if (lane(20.0f) && p.z >= 1.0f) {
			ground = 0.5f;
		} else if (lane(30.0f) && p.z >= 1.0f && p.z <= 1.3f) {
			ground = 1.0f;
		} else if (lane(80.0f) && p.z >= 0.15f) {
			ground = 0.3f * static_cast<float>(std::min(5, static_cast<int>((p.z - 0.15f) / 0.4f)) + 1);
		}
		if (p.y < ground) {
			return true;
		}
		if (lane(40.0f) && p.y >= 2.1f && p.y <= 2.3f) {
			return true;
		}
		if ((lane(50.0f) || lane(70.0f)) && p.y >= 1.9f && p.y <= 2.1f) {
			return true;
		}
		return lane(60.0f) && p.z >= 0.8f && p.y >= 2.0f && p.y <= 2.2f;
	}

	bool HopRay(const Vec3& from, const Vec3& delta, Vec3& hit)
	{
		if (HopSolid(from)) {
			hit = from;
			return true;
		}
		constexpr int kSteps = 1000;
		Vec3          prev = from;
		for (int i = 1; i <= kSteps; ++i) {
			const Vec3 p = from + delta * (static_cast<float>(i) / kSteps);
			if (HopSolid(p)) {
				Vec3 a = prev, b = p;
				for (int k = 0; k < 24; ++k) {
					const Vec3 m = (a + b) * 0.5f;
					(HopSolid(m) ? b : a) = m;
				}
				hit = b;
				return true;
			}
			prev = p;
		}
		return false;
	}

	struct HopRun
	{
		KinematicState s;
		float          rise{ 0 };         // what KinematicHop granted (0: no hop)
		float          apex{ -INFINITY }; // highest feet
		float          landingSpeed{ 0 }; // at the first landing
		float          hopEndZ{ NAN };    // where the first landing was
		bool           hopCeiling{ false };
		float          overhangHead{ -INFINITY };  // x = 60: the highest head with the axis under the overhang
	};

	// Snake stands at a_from (standing, 1.8 m), dives along +z (a_hop: hop at frame 0; else he just walks on) and MGSV moves
	// him a_speed m/s along +z for a_frames at 60 fps. a_stance: the stance's height the host is told meanwhile (the frame that
	// confirms a dive may already say prone); the hop takes off at his standing height, as the puppet asks (HopParams::head).
	HopRun RunHop(const Vec3& a_from, bool a_hop, int a_frames, float a_stepUp = 0.5f, int a_hz = 60, float a_speed = 4.0f, float a_stance = 1.8f)
	{
		KinematicParams params;
		params.bodyHeight = a_stance;
		params.stepUp = a_stepUp;
		HopParams hop;
		hop.head = 1.8f;
		HopRun    run;
		run.s.pos = a_from;
		run.s.grounded = true;
		const float dt = 1.0f / static_cast<float>(a_hz);
		if (a_hop) {
			run.rise = KinematicHop(run.s, { 0, 0, 1 }, hop, params, &HopRay);
		}
		bool landed = false;
		for (int i = 0; i < a_frames * a_hz / 60; ++i) {
			const auto r = KinematicStep(run.s, { 0, 0, a_speed * dt }, dt, params, &HopRay);
			run.apex = std::max(run.apex, run.s.pos.y);
			run.hopCeiling |= r.ceiling && !landed;
			if (r.landed && !landed) {
				landed = true;
				run.landingSpeed = r.landingSpeed;
				run.hopEndZ = run.s.pos.z;
			}
			if (run.s.pos.z >= 0.8f) {
				run.overhangHead = std::max(run.overhangHead, run.s.pos.y + 1.8f);
			}
		}
		return run;
	}
}

// When the puppet hops (er/DiveHop.h): the flag's rising edge, then the dive's first move -- once per dive.
static void TestDiveTrigger()
{
	using Out = DiveTrigger::Out;
	const float dt = 1.0f / 60.0f;
	const Vec3  still{}, run{ 0.06f, 0, 0.06f };  // ~5 m/s
	Vec3        dir;
	{
		DiveTrigger t;
		CHECK(t.Update(false, true, 0, run, dt, 300, &dir) == Out::kNone);
		CHECK(t.Update(true, true, 16, still, dt, 300, &dir) == Out::kNone && t.Armed());  // the dive hasn't moved him yet
		CHECK(t.Update(true, true, 33, run, dt, 300, &dir) == Out::kHop && Near(dir.x, 0.7071f, 0.001f) && Near(dir.z, 0.7071f, 0.001f) && dir.y == 0.0f);
		CHECK(t.Update(true, true, 50, run, dt, 300, &dir) == Out::kNone);  // one hop per dive, however long the flag stays up
		CHECK(t.Update(false, true, 66, run, dt, 300, &dir) == Out::kNone);
		CHECK(t.Update(true, true, 83, run, dt, 300, &dir) == Out::kHop);    // the next dive
	}
	{
		DiveTrigger t;  // X that never moved him: no hop after the wait
		CHECK(t.Update(true, true, 0, still, dt, 300, &dir) == Out::kNone);
		CHECK(t.Update(true, true, 200, { 0.001f, 0, 0 }, dt, 300, &dir) == Out::kNone);  // 0.06 m/s: noise, not a dive
		CHECK(t.Update(true, true, 301, still, dt, 300, &dir) == Out::kNoMove && !t.Armed());
		CHECK(t.Update(true, true, 320, run, dt, 300, &dir) == Out::kNone);
	}
	{
		DiveTrigger t;  // off / mounted: no hop, and a dive that began so stays none
		CHECK(t.Update(true, false, 0, run, dt, 300, &dir) == Out::kNone && !t.Armed());
		CHECK(t.Update(true, true, 16, run, dt, 300, &dir) == Out::kNone);
		t.Reset();
		CHECK(t.Update(true, true, 0, still, dt, 300, &dir) == Out::kNone && t.Armed());
		CHECK(t.Update(true, false, 16, run, dt, 300, &dir) == Out::kNone && !t.Armed());
	}
}

static void TestHop()
{
	TestDiveTrigger();
	// Level ground (x = 100: nothing there): ~2.5 m long, ~0.6 m high, a soft landing.
	{
		const auto run = RunHop({ 100, 0, 0 }, true, 60);
		CHECK(Near(run.rise, 0.6f, 0.001f) && Near(run.apex, 0.6f, 0.03f));
		CHECK(Near(run.hopEndZ, 2.5f, 0.15f) && run.landingSpeed < 4.0f && run.s.grounded && !run.s.hopping && Near(run.s.pos.y, 0.0f, 0.01f));
		const auto fast = RunHop({ 100, 0, 0 }, true, 60, 0.5f, 240);  // 240 fps: the take-off ground is never a landing
		CHECK(Near(fast.hopEndZ, 2.5f, 0.15f) && Near(fast.apex, 0.6f, 0.03f));
		// One air with the jump (merge with the jump): a hop's landing counts as a fall only below its take-off -- a level one is none
		CHECK(run.landingSpeed < 0.01f && fast.landingSpeed < 0.01f);
	}
	// ... and they never stack: no hop in a jump, no jump in a hop.
	{
		KinematicParams params;
		params.bodyHeight = 1.8f;
		KinematicState j;
		j.pos = { 100, 0, 0 };
		j.grounded = true;
		CHECK(KinematicJump(j, {}, 1.2f, 0.15f, 12.0f, params, &HopRay).ok);
		KinematicStep(j, {}, 1.0f / 60.0f, params, &HopRay);
		CHECK(KinematicHop(j, { 0, 0, 1 }, HopParams{}, params, &HopRay) == 0.0f && j.jumping && !j.hopping);
		KinematicState h;
		h.pos = { 100, 0, 0 };
		h.grounded = true;
		CHECK(KinematicHop(h, { 0, 0, 1 }, HopParams{}, params, &HopRay) > 0.5f);
		CHECK(!KinematicJump(h, {}, 1.2f, 0.15f, 12.0f, params, &HopRay).ok && h.hopping && !h.jumping);
	}
	// A 1 m gap: walking in, he falls (both at 1.5 m/s: the hop ignores the asked move); the dive's hop carries him over, landing
	// well past the far rim. Running in at 4 m/s he falls in too: in the air a ledge is a wall until his feet are nearly at its
	// top (kFallStep; before the merge with the jump the controller let a falling body catch the far rim as a step).
	{
		CHECK(RunHop({ 0, 0, 0.5f }, false, 120, 0.5f, 60, 1.5f).s.pos.y < -1.0f);
		CHECK(RunHop({ 0, 0, 0.5f }, false, 120, 0.5f, 60, 4.0f).s.pos.y < -1.0f);
		const auto run = RunHop({ 0, 0, 0.5f }, true, 90, 0.5f, 60, 1.5f);
		CHECK(Near(run.rise, 0.6f, 0.001f) && Near(run.apex, 0.6f, 0.03f) && run.hopEndZ > 2.5f);
		CHECK(run.s.grounded && Near(run.s.pos.y, 0.0f, 0.01f));
	}
	// Off the rim (review of 2.9): the dive's flag reaches the host a few frames after his centre passed the rim, MGSV's dive
	// moving him on at 5 m/s meanwhile -- he still hops, from the rim's height, and lands on the far side. Later (0.27 s,
	// 0.35 m down; walking in at 1.5 m/s so he is still over the gap) it is an ordinary fall and nothing changes.
	for (const int late : { 0, 3, 6, 15 }) {
		KinematicParams params;
		params.bodyHeight = 1.8f;
		HopParams hop;
		hop.head = 1.8f;
		KinematicState s;
		s.pos = { 0, 0, 0 };
		s.grounded = true;
		const float dt = 1.0f / 60.0f, step = (late > 6 ? 1.5f : 5.0f) * dt;
		for (int i = 0; i < 60 && s.grounded; ++i) {
			KinematicStep(s, { 0, 0, step }, dt, params, &HopRay);
		}
		// Off the rim once fewer than two of the body's ground probes (er/Kinematic.h BodyGround) are on it: his centre at most
		// the probes' reach (0.8 x radius) past it.
		CHECK(!s.grounded && s.pos.z > 1.0f && s.pos.z < 1.0f + kGroundProbeRadius * params.radius + step + 0.01f);
		for (int i = 0; i < late; ++i) {
			KinematicStep(s, { 0, 0, step }, dt, params, &HopRay);
		}
		const KinematicState before = s;
		const float          rise = KinematicHop(s, { 0, 0, 1 }, hop, params, &HopRay);
		if (late > 6) {
			CHECK(rise == 0.0f && !s.hopping && s.pos.y == before.pos.y && s.vy == before.vy);
			continue;
		}
		CHECK(Near(rise, 0.6f, 0.001f) && s.hopping && s.pos.y == before.groundY && Near(s.pos.y, 0.0f, 0.001f) && s.pos.z == before.pos.z);
		float landedZ = NAN;
		for (int i = 0; i < 90 && std::isnan(landedZ); ++i) {
			if (KinematicStep(s, { 0, 0, step }, dt, params, &HopRay).landed) {
				landedZ = s.pos.z;
			}
		}
		CHECK(s.grounded && Near(s.pos.y, 0.0f, 0.01f) && landedZ > 3.0f);
	}
	// A 0.6 m ledge (above step_up 0.5): walking, the body stops at it; the hop lands on top.
	{
		const auto walk = RunHop({ 10, 0, 0 }, false, 60);
		CHECK(Near(walk.s.pos.y, 0.0f, 0.01f) && walk.s.pos.z < 0.7f);
		for (const float from : { 0.0f, 0.3f, -0.3f }) {
			const auto run = RunHop({ 10, 0, from }, true, 90);
			CHECK(run.s.grounded && Near(run.s.pos.y, 0.6f, 0.01f) && run.s.pos.z > 1.0f);
		}
	}
	// A 0.5 m ledge with step_up 0.3 (so it is the hop, not the step, that climbs it).
	{
		CHECK(RunHop({ 20, 0, 0 }, false, 60, 0.3f).s.pos.z < 0.7f);
		const auto run = RunHop({ 20, 0, 0 }, true, 90, 0.3f);
		CHECK(run.s.grounded && Near(run.s.pos.y, 0.5f, 0.01f) && run.s.pos.z > 1.0f);
	}
	// A 1 m wall: never over it, from any distance, hop after hop; he lands back at its foot.
	for (const float from : { 0.0f, -0.5f, -1.0f, 0.5f }) {
		HopRun run = RunHop({ 30, 0, from }, true, 90);
		CHECK(run.s.grounded && Near(run.s.pos.y, 0.0f, 0.01f) && run.s.pos.z < 0.7f);
		KinematicParams params;
		params.bodyHeight = 1.8f;
		for (int hop = 0; hop < 5; ++hop) {
			CHECK(KinematicHop(run.s, { 0, 0, 1 }, HopParams{}, params, &HopRay) > 0.5f);
			for (int i = 0; i < 90; ++i) {
				KinematicStep(run.s, { 0, 0, 4.0f / 60.0f }, 1.0f / 60.0f, params, &HopRay);
			}
		}
		CHECK(run.s.grounded && Near(run.s.pos.y, 0.0f, 0.01f) && run.s.pos.z < 0.7f);
	}
	// A ceiling 2.1 m up over a standing (1.8 m) Snake: a lower hop that never puts his head into it; 1.9 m: none at all.
	{
		const auto run = RunHop({ 40, 0, 0 }, true, 60);
		CHECK(run.rise > 0.15f && run.rise < 0.3f && run.apex + 1.8f <= 2.1f + 0.005f && run.s.grounded && run.hopEndZ > 0.5f);
		KinematicParams params;
		params.bodyHeight = 1.8f;
		KinematicState s;
		s.pos = { 50, 0, 0 };
		s.grounded = true;
		CHECK(KinematicHop(s, { 0, 0, 1 }, HopParams{}, params, &HopRay) == 0.0f && s.grounded && !s.hopping && s.vy == 0.0f);
		// The frame that confirmed the dive already says prone (0.5 m) or crouched (review of 2.9): the take-off height is his
		// standing one all the same (HopParams::head), so still no hop -- a prone head would have had room for the full hop.
		HopParams standing;
		standing.head = 1.8f;
		for (const float told : { 0.5f, 1.2f }) {
			params.bodyHeight = told;
			CHECK(KinematicHop(s, { 0, 0, 1 }, standing, params, &HopRay) == 0.0f && s.grounded && !s.hopping);
		}
		// Under 2.1 m, told prone: a lower hop that keeps his standing head out of it, all the way.
		const auto prone = RunHop({ 40, 0, 0 }, true, 60, 0.5f, 60, 4.0f, 0.5f);
		CHECK(prone.rise > 0.15f && prone.rise < 0.3f && prone.apex + 1.8f <= 2.1f + 0.005f && prone.s.grounded);
		// Crouched (1.2 m) at the take-off (no HopParams::head: the stance's height) there is room again.
		params.bodyHeight = 1.2f;
		CHECK(KinematicHop(s, { 0, 0, 1 }, HopParams{}, params, &HopRay) == 0.6f && s.hopping);
	}
	// An overhang 2.0 m up ahead (open sky at the take-off): the hop carries him toward it, but never with his head in it; he
	// goes under once he has come down.
	{
		const auto run = RunHop({ 60, 0, 0 }, true, 90);
		CHECK(Near(run.apex, 0.6f, 0.03f) && run.overhangHead <= 2.0f + 0.005f && run.hopCeiling && run.s.grounded && run.s.pos.z > 2.0f);
		const auto prone = RunHop({ 60, 0, 0 }, true, 90, 0.5f, 60, 4.0f, 0.5f);  // told prone meanwhile: the same
		CHECK(prone.overhangHead <= 2.0f + 0.005f && prone.hopCeiling);
	}
	// At the foot of stairs (merge with the head-room fix of 2026-10-14): the next step's top 0.2 m ahead is floor, not a
	// ceiling -- the full hop, and he goes on up the steps.
	{
		const auto run = RunHop({ 80, 0, 0 }, true, 60);
		CHECK(Near(run.rise, 0.6f, 0.001f) && run.s.grounded && run.s.pos.y > 1.7f);
	}
	// No hop under a low ceiling: over the gap below it he falls as he would walking (the landing rules are the fall's own).
	{
		const auto run = RunHop({ 70, 0, 0.5f }, true, 120, 0.5f, 60, 1.5f);
		CHECK(run.rise == 0.0f && run.s.pos.y < -1.0f);
	}
	// In the air already, or hopping: no second hop.
	{
		KinematicParams params;
		KinematicState  s;
		s.pos = { 100, 3, 0 };
		CHECK(KinematicHop(s, { 0, 0, 1 }, HopParams{}, params, &HopRay) == 0.0f);
		s.pos = { 100, 0, 0 };
		s.grounded = true;
		CHECK(KinematicHop(s, { 0, 0, 1 }, HopParams{}, params, &HopRay) > 0.0f && KinematicHop(s, { 0, 0, 1 }, HopParams{}, params, &HopRay) == 0.0f);
	}
	// The hop lands on the body's ground like a fall (merge with the ground probes, er/Kinematic.h BodyGround): along a gap between
	// the scaffold's planks (test_host_motion.inc WoodRay: 22 cm planks, 6 cm gaps, 3 m up) it comes down on the planks either
	// side of his centre; one ray under the centre (ground_probes = false) took him through the gap, 3 m down.
	for (const bool probes : { true, false }) {
		KinematicParams params;
		params.bodyHeight = 1.8f;
		params.groundProbes = probes;
		HopParams hop;
		hop.head = 1.8f;
		KinematicState s;
		s.pos = { 50.4f, 3, 50.25f };
		s.grounded = true;
		CHECK(Near(KinematicHop(s, { 1, 0, 0 }, hop, params, &WoodRay), 0.6f, 0.001f));
		bool  landed = false;
		float landedX = NAN;
		for (int i = 0; i < 150; ++i) {
			if (KinematicStep(s, {}, 1.0f / 60.0f, params, &WoodRay).landed && !landed) {
				landed = true;
				landedX = s.pos.x;
			}
		}
		CHECK(landed && s.grounded && landedX > 52.5f && landedX < 53.2f);
		CHECK(probes ? Near(s.pos.y, 3.0f, 0.01f) : Near(s.pos.y, 0.0f, 0.01f));
	}
}
