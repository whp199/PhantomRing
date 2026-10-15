// Included by tests/test_host.cpp: the kinematic controller under low ceilings (er/Kinematic.h bodyHeight, Headroom) and the
// stall unstick (Unstick::Stall), 2026-10-14 (player report: Snake walked into the low space under the Roundtable Hold's basement
// stairs and was stuck there: 59 wall contacts over 1353 refused frames, 0 unsticks).

namespace
{
	// A world with things to get under: ground y = 0 everywhere; a staircase slab (0.3 m thick, rising 0.75 m per metre along +z)
	// for x in [0, 2], z in [0, 4]; a table top y in [1.0, 1.1] for x in [6, 8], z in [0, 2]; a beam y in [1.25, 1.45] for x in
	// [10, 10.3], all z.
	bool LowSolid(const Vec3& p)
	{
		if (p.y < 0.0f) {
			return true;
		}
		if (p.x >= 0.0f && p.x <= 2.0f && p.z >= 0.0f && p.z <= 4.0f && p.y <= 0.75f * p.z && p.y >= 0.75f * p.z - 0.3f) {
			return true;
		}
		if (p.x >= 6.0f && p.x <= 8.0f && p.z >= 0.0f && p.z <= 2.0f && p.y >= 1.0f && p.y <= 1.1f) {
			return true;
		}
		if (p.x >= 10.0f && p.x <= 10.3f && p.y >= 1.25f && p.y <= 1.45f) {
			return true;
		}
		// A staircase as Elden Ring's collision has it (the Roundtable Hold's basement stairs, live 2026-10-14): thin tread plates,
		// no risers, 6 cm seams between them through which a vertical ray passes. x in [20, 22], rising 0.75 m per metre along +z
		// from z = 20: tread k covers z in [20 + 0.3k + 0.06, 20 + 0.3(k + 1)] at y = 0.225 (k + 1).
		if (p.x >= 20.0f && p.x <= 22.0f && p.z >= 20.0f && p.z <= 24.0f) {
			const float local = p.z - 20.0f;
			const int   k = static_cast<int>(local / 0.3f);
			const float top = 0.225f * static_cast<float>(k + 1);
			if (local - 0.3f * static_cast<float>(k) >= 0.06f && p.y <= top && p.y >= top - 0.02f) {
				return true;
			}
		}
		// A low passage with a dip on the way out: a thin ceiling 1.0 m up for x in [30, 32], z in [30, 33], and a beam 0.7 m up
		// across it at z in [31.4, 31.6] (the stringer under the Roundtable Hold's stairs that held a crouched Snake).
		if (p.x >= 30.0f && p.x <= 32.0f && p.z >= 30.0f && p.z <= 33.0f && p.y >= 0.98f && p.y <= 1.0f) {
			return true;
		}
		if (p.x >= 30.0f && p.x <= 32.0f && p.z >= 31.4f && p.z <= 31.6f && p.y >= 0.7f && p.y <= 0.75f) {
			return true;
		}
		// Solid steps to climb (Stormveil's broken side stairs, live 2026-10-14): x in [40, 42], step k covers z in
		// [40 + 0.4k, 40 + 0.4(k + 1)] up to y = 0.3 (k + 1), 6 steps (1.8 m), then a landing at 1.8 m to z 46.
		if (p.x >= 40.0f && p.x <= 42.0f && p.z >= 40.0f && p.z <= 46.0f) {
			const int k = std::min(5, static_cast<int>((p.z - 40.0f) / 0.4f));
			return p.y <= 0.3f * static_cast<float>(k + 1);
		}
		return false;
	}

	bool LowRay(const Vec3& from, const Vec3& delta, Vec3& hit)
	{
		if (LowSolid(from)) {
			hit = from;
			return true;
		}
		constexpr int kSteps = 1000;
		Vec3          prev = from;
		for (int i = 1; i <= kSteps; ++i) {
			const Vec3 p = from + delta * (static_cast<float>(i) / kSteps);
			if (LowSolid(p)) {
				Vec3 a = prev, b = p;
				for (int k = 0; k < 24; ++k) {
					const Vec3 m = (a + b) * 0.5f;
					(LowSolid(m) ? b : a) = m;
				}
				hit = b;
				return true;
			}
			prev = p;
		}
		return false;
	}

	// Walks a_frames steps of a_step from a_from; returns the end and whether a ceiling refused a move.
	struct Walk
	{
		Vec3 end;
		bool ceiling{ false };
	};
	Walk WalkLow(const Vec3& a_from, const Vec3& a_step, int a_frames, float a_height)
	{
		KinematicParams params;
		params.bodyHeight = a_height;
		KinematicState s;
		s.pos = a_from;
		s.grounded = true;
		Walk w;
		for (int i = 0; i < a_frames; ++i) {
			w.ceiling |= KinematicStep(s, a_step, 1.0f / 60.0f, params, &LowRay).ceiling;
		}
		w.end = s.pos;
		return w;
	}
}

static void TestCeiling()
{
	{  // climbing solid 0.3 m steps standing: the next step's top is not a ceiling (live 2026-10-14: "a ceiling 0.26 m above")
		const Walk up = WalkLow({ 41, 0, 38 }, { 0, 0, 0.05f }, 130, 1.8f);  // 6.5 m: up the steps onto the landing
		CHECK(up.end.z > 43.0f && up.end.y > 1.7f && !up.ceiling);
	}
	CHECK(!std::isfinite(Headroom({ -3, 0, 2 }, 2.0f, &LowRay)));
	CHECK(Near(Headroom({ 7, 0, 1 }, 2.0f, &LowRay), 1.0f, 0.01f));
	{  // under the stairs from the side, where the underside is 1.72 m up: the head ray (1.6) passes beneath it, so the old
	   // controller walked him under (and through); standing (1.8 m) he now stops at its edge, crouched (1.2 m) he goes under
		const Walk old = WalkLow({ -2, 0, 2.7f }, { 0.05f, 0, 0 }, 120, 0.0f);
		CHECK(old.end.x > 3.0f && !old.ceiling);
		const Walk stand = WalkLow({ -2, 0, 2.7f }, { 0.05f, 0, 0 }, 120, 1.8f);
		CHECK(stand.end.x < 0.05f && stand.end.x > -0.6f && stand.ceiling && Near(stand.end.y, 0.0f, 0.01f));
		const Walk crouch = WalkLow({ -2, 0, 2.7f }, { 0.05f, 0, 0 }, 120, 1.2f);
		CHECK(crouch.end.x > 3.0f && !crouch.ceiling);
	}
	{  // into the low end under the stairs (along -z, beneath them): standing stops where the underside comes down to 1.8 m (z 2.8),
	   // crouched at 1.2 m (z 2.0; his waist ray meets it at 1.95), prone further in
		const Walk stand = WalkLow({ 1, 0, 6 }, { 0, 0, -0.05f }, 200, 1.8f);
		CHECK(stand.end.z > 2.75f && stand.end.z < 3.2f && Near(stand.end.y, 0.0f, 0.01f));
		const Walk crouch = WalkLow({ 1, 0, 6 }, { 0, 0, -0.05f }, 200, 1.2f);
		CHECK(crouch.end.z > 1.9f && crouch.end.z < 2.4f && Near(crouch.end.y, 0.0f, 0.01f));
		const Walk prone = WalkLow({ 1, 0, 6 }, { 0, 0, -0.05f }, 200, 0.5f);
		CHECK(prone.end.z > 1.05f && prone.end.z < 1.9f && Near(prone.end.y, 0.0f, 0.01f));
	}
	{  // a table 1.0 m high: no wall ray meets its thin top, the old controller walked under it; standing and crouched he stops
	   // at its edge, prone he crawls under
		CHECK(WalkLow({ 4, 0, 1 }, { 0.05f, 0, 0 }, 120, 0.0f).end.x > 8.5f);
		const Walk stand = WalkLow({ 4, 0, 1 }, { 0.05f, 0, 0 }, 120, 1.8f);
		CHECK(stand.end.x < 6.05f && stand.end.x > 5.4f && stand.ceiling);
		CHECK(WalkLow({ 4, 0, 1 }, { 0.05f, 0, 0 }, 120, 1.2f).end.x < 6.05f);
		CHECK(WalkLow({ 4, 0, 1 }, { 0.05f, 0, 0 }, 120, 0.5f).end.x > 8.5f);
	}
	{  // already under it (he stood up beneath the table): walking out goes, either way
		CHECK(WalkLow({ 7, 0, 1 }, { 0.05f, 0, 0 }, 60, 1.8f).end.x > 8.5f);
		CHECK(WalkLow({ 7, 0, 1 }, { -0.05f, 0, 0 }, 60, 1.8f).end.x < 5.5f);
		CHECK(WalkLow({ 7, 0, 1.5f }, { 0, 0, 0.05f }, 60, 1.8f).end.z > 2.5f);
	}
	{  // a beam at 1.25-1.45 m: the chest ray stops him standing; crouched (1.2 m) the rays above his height are left out: under it
		CHECK(WalkLow({ 8.5f, 0, 5 }, { 0.05f, 0, 0 }, 100, 1.8f).end.x < 9.7f);
		CHECK(WalkLow({ 8.5f, 0, 5 }, { 0.05f, 0, 0 }, 100, 1.2f).end.x > 11.0f);
		CHECK(WalkLow({ 8.5f, 0, 5 }, { 0.05f, 0, 0 }, 100, 0.0f).end.x < 9.7f);  // no stance: every ray, as before
	}
	{  // live 2026-10-14 under the Roundtable Hold's basement stairs: a crouched Snake crept in seam by seam (a ray up between two
	   // treads finds nothing, so the move "under no ceiling" passed) to a 0.57 m ceiling. With the seams bridged he stops where the
	   // treads come down to his 1.2 m (tread 4, 1.125 m, at z 21.26-21.5; his ahead probe 0.2 m), standing at 1.8 m earlier.
		CHECK(!std::isfinite(Headroom({ 21, 0, 21.22f }, 2.0f, &LowRay)));  // the seam itself: a plain ray finds nothing
		CHECK(Near(SolidHeadroom({ 21, 0, 21.22f }, 2.0f, &LowRay), 0.9f, 0.03f));  // the lower tread beside the seam
		const Walk crouch = WalkLow({ 21, 0, 26 }, { 0, 0, -0.05f }, 200, 1.2f);
		CHECK(crouch.end.z > 21.55f && crouch.end.z < 22.1f && crouch.ceiling && Near(crouch.end.y, 0.0f, 0.01f));
		const Walk stand = WalkLow({ 21, 0, 26 }, { 0, 0, -0.05f }, 200, 1.8f);
		CHECK(stand.end.z > 22.2f && stand.end.z < 22.9f && stand.ceiling);
		// ... and back out the way he came, from as deep as he got
		CHECK(WalkLow(crouch.end, { 0, 0, 0.05f }, 80, 1.2f).end.z > 25.0f);
		// from the side, along the treads (x): the same
		const Walk side = WalkLow({ 24, 0, 21.4f }, { -0.05f, 0, 0 }, 120, 1.2f);
		CHECK(side.end.x > 21.9f && side.ceiling);
	}
	{  // already under a low ceiling (crouched under 1.0 m) with a dip on the way out (a beam at 0.7 m): every move out reads
	   // "lower than here" -- live, a crouched Snake stuck under the stairs. After kCeilingEscapeS of refusals the check stands
	   // aside until he is free; then it is back (walking in again is refused), and a free Snake pushing in never escapes.
		KinematicParams params;
		params.bodyHeight = 1.2f;
		KinematicState s;
		s.pos = { 31, 0, 30.5f };
		s.grounded = true;
		bool refused = false, escaped = false;
		int  frames = 0;
		for (; frames < 240 && s.pos.z < 33.6f; ++frames) {
			const auto r = KinematicStep(s, { 0, 0, 0.04f }, 1.0f / 60.0f, params, &LowRay);
			refused |= r.ceiling;
			escaped |= r.ceilingEscape;
		}
		CHECK(refused && escaped && s.pos.z >= 33.6f && frames < 200);
		CHECK(!s.ceilingEscape);  // free again: the check is back
		const Walk back = WalkLow(s.pos, { 0, 0, -0.04f }, 120, 1.2f);
		CHECK(back.ceiling && back.end.z > 33.0f);
		// free at the passage's mouth, pushing in for 4 s: refused all along
		KinematicState f;
		f.pos = { 31, 0, 33.6f };
		f.grounded = true;
		bool fEscaped = false;
		for (int i = 0; i < 240; ++i) {
			fEscaped |= KinematicStep(f, { 0, 0, -0.04f }, 1.0f / 60.0f, params, &LowRay).ceilingEscape;
		}
		CHECK(!fEscaped && f.pos.z > 33.0f);
	}
	{  // up the stairs on top of them: nothing above, the ceiling check never refuses a step
		const Walk up = WalkLow({ 1, 0, -1 }, { 0, 0, 0.04f }, 90, 1.8f);
		CHECK(up.end.z > 2.4f && up.end.y > 1.7f && !up.ceiling);
	}
}

namespace
{
	// Wooden things to stand on (live 2026-10-14: the Tarnished climbed Stormveil's side scaffolding, Snake took over at the top and
	// fell through it -- its floor is planks with gaps, and one ray under his centre went through a gap). Ground y = 0 everywhere.
	//  - a scaffold floor 3 m up, x in [50, 54], z in [50, 54]: planks along x, 5 cm thick, 22 cm wide with 6 cm gaps (plank k
	//    covers z in [50 + 0.28k, 50 + 0.28k + 0.22]), and a 0.8 m hole where planks are missing (z in [52.24, 53.04]);
	//  - a thin platform 2 cm thick at y 2.0, x and z in [60, 62];
	//  - a boardwalk 0.3 m above the ground: the same planks, x in [70, 74], z in [50, 54] (a ray through its gaps finds the ground
	//    0.3 m below, inside the step window);
	//  - 25-degree ramps up along x, x in [80, 88] (surface y = (x - 80) tan 25): solid below it for z in [50, 56]; planks across
	//    it (22 cm wide along x, 6 cm gaps, 5 cm thick, on the slope) for z in [60, 66]; solid with a ceiling 1.9 m above the slope
	//    (a sloped tunnel just taller than Snake standing, 1.8 m) for z in [70, 76].
	// RailSolid: a railing along the scaffold's edge, x in [53.9, 53.95], y 3.0-4.1 -- only in the characters' layers (CharRay).
	constexpr float kRampTan = 0.46630766f;  // tan 25 degrees
	float RampY(float a_x) { return (a_x - 80.0f) * kRampTan; }
	bool PlankFloor(const Vec3& p, float a_x0, float a_top)
	{
		if (p.x < a_x0 || p.x > a_x0 + 4.0f || p.z < 50.0f || p.z > 54.0f || p.y > a_top || p.y < a_top - 0.05f) {
			return false;
		}
		const float local = p.z - 50.0f;
		const int   k = static_cast<int>(local / 0.28f);
		return local - 0.28f * static_cast<float>(k) <= 0.22f;
	}

	bool WoodSolid(const Vec3& p)
	{
		if (p.y < 0.0f) {
			return true;
		}
		if (PlankFloor(p, 50.0f, 3.0f) && !(p.z >= 52.24f && p.z <= 53.04f)) {
			return true;
		}
		if (p.x >= 60.0f && p.x <= 62.0f && p.z >= 60.0f && p.z <= 62.0f && p.y <= 2.0f && p.y >= 1.98f) {
			return true;
		}
		if (p.x >= 80.0f && p.x <= 88.0f) {
			const float s = RampY(p.x);
			if (p.z >= 50.0f && p.z <= 56.0f && p.y <= s) {
				return true;
			}
			if (p.z >= 60.0f && p.z <= 66.0f && p.y <= s && p.y >= s - 0.05f) {
				const float local = p.x - 80.0f;
				return local - 0.28f * std::floor(local / 0.28f) <= 0.22f;
			}
			if (p.z >= 70.0f && p.z <= 76.0f && (p.y <= s || (p.y >= s + 1.9f && p.y <= s + 2.2f))) {
				return true;
			}
		}
		return PlankFloor(p, 70.0f, 0.3f);
	}

	bool RailSolid(const Vec3& p) { return p.x >= 53.9f && p.x <= 53.95f && p.z >= 50.0f && p.z <= 54.0f && p.y >= 3.0f && p.y <= 4.1f; }

	// Marched 1 mm along a ray of up to 4 m (the 2 cm platform is never stepped over), the crossing refined by bisection.
	template <class Solid>
	bool MarchRay(const Vec3& from, const Vec3& delta, Vec3& hit, Solid a_solid)
	{
		if (a_solid(from)) {
			hit = from;
			return true;
		}
		constexpr int kSteps = 4000;
		Vec3          prev = from;
		for (int i = 1; i <= kSteps; ++i) {
			const Vec3 p = from + delta * (static_cast<float>(i) / kSteps);
			if (a_solid(p)) {
				Vec3 a = prev, b = p;
				for (int k = 0; k < 24; ++k) {
					const Vec3 m = (a + b) * 0.5f;
					(a_solid(m) ? b : a) = m;
				}
				hit = b;
				return true;
			}
			prev = p;
		}
		return false;
	}

	// What Snake's old ray filter (0x58) sees: the wood, not the railing.
	bool WoodRay(const Vec3& from, const Vec3& delta, Vec3& hit) { return MarchRay(from, delta, hit, WoodSolid); }
	// The characters' own layers (ground_filter 0x16): the wood and the railing (a ray from inside the railing doesn't meet it,
	// as Havok's don't: a ground probe beside it finds the planks).
	bool CharRay(const Vec3& from, const Vec3& delta, Vec3& hit)
	{
		Vec3       h1, h2;
		const bool b1 = WoodRay(from, delta, h1);
		const bool b2 = !RailSolid(from) && MarchRay(from, delta, h2, RailSolid);
		if (b1 || b2) {
			hit = b1 && (!b2 || Dot(h1 - from, h1 - from) <= Dot(h2 - from, h2 - from)) ? h1 : h2;
		}
		return b1 || b2;
	}

	struct WoodWalk
	{
		Vec3  end;
		bool  leftGround{ false };  // any frame airborne
		float lowest{ INFINITY };   // lowest feet height on the way
		bool  landed{ false };
		float landingSpeed{ 0 };
	};
	WoodWalk WalkWood(const Vec3& a_from, const Vec3& a_step, int a_frames, bool a_probes = true, bool a_grounded = true, const RayFn& a_ray = RayFn(&WoodRay))
	{
		KinematicParams params;
		params.bodyHeight = 1.8f;
		params.groundProbes = a_probes;
		KinematicState s;
		s.pos = a_from;
		s.grounded = a_grounded;
		WoodWalk w;
		for (int i = 0; i < a_frames; ++i) {
			const auto r = KinematicStep(s, a_step, 1.0f / 60.0f, params, a_ray);
			w.leftGround |= !s.grounded;
			w.lowest = std::min(w.lowest, s.pos.y);
			if (r.landed && !w.landed) {
				w.landed = true;
				w.landingSpeed = r.landingSpeed;
			}
		}
		w.end = s.pos;
		return w;
	}
}

static void TestGround()
{
	const KinematicParams params;
	const float           top = params.stepUp + 0.05f;
	{  // BodyGround: centre over a gap between two planks (z 50.25 is in the gap 50.22-50.28): the side probes hold him
		const GroundProbe g = BodyGround({ 51, 3, 50.25f }, top, top + params.snapDown, params.radius, &WoodRay);
		CHECK(g.supported && !g.centre && g.hits >= 2 && Near(g.y, 3.0f, 0.01f));
		CHECK(!BodyGround({ 51, 3, 50.25f }, top, top + params.snapDown, params.radius, &WoodRay, false).supported);  // the old way: nothing
		// on a plank, every probe on one too
		const GroundProbe on = BodyGround({ 51, 3, 51.0f }, top, top + params.snapDown, params.radius, &WoodRay);
		CHECK(on.supported && on.centre && on.hits == 5 && Near(on.y, 3.0f, 0.01f));
		// the boardwalk: through a gap the centre finds the ground 0.3 m below, the planks beside it are the floor
		const GroundProbe walk = BodyGround({ 71, 0.3f, 50.25f }, top, top + params.snapDown, params.radius, &WoodRay);
		CHECK(walk.supported && walk.centre && Near(walk.y, 0.3f, 0.01f));
		// the scaffold's edge: centre 0.2 m past it, one probe on the last plank -- a rim, no support (he falls off)
		const GroundProbe rim = BodyGround({ 54.2f, 3, 51.0f }, top, top + params.snapDown, params.radius, &WoodRay);
		CHECK(!rim.supported && rim.hits == 1);
		// centre 0.1 m inside the edge: the centre's own plank
		CHECK(BodyGround({ 53.9f, 3, 51.0f }, top, top + params.snapDown, params.radius, &WoodRay).supported);
		// in the middle of the 0.8 m hole: every probe falls through
		CHECK(!BodyGround({ 52, 3, 52.64f }, top, top + params.snapDown, params.radius, &WoodRay).supported);
	}
	{  // the player's case: Snake takes over standing on the planks with his centre over a gap. He stays; the old single ray fell
	   // through to the ground 3 m below
		const WoodWalk stay = WalkWood({ 52, 3, 50.25f }, {}, 60);
		CHECK(!stay.leftGround && Near(stay.end.y, 3.0f, 0.01f));
		const WoodWalk old = WalkWood({ 52, 3, 50.25f }, {}, 60, false);
		CHECK(old.leftGround && old.landed && Near(old.end.y, 0.0f, 0.01f));
		// across the planks (along z, every 0.28 m a 6 cm gap; 5 cm a frame): never off the scaffold, never a dip
		const WoodWalk across = WalkWood({ 52, 3, 50.1f }, { 0, 0, 0.05f }, 40);  // 2 m: z 50.1 -> 52.1, short of the hole
		CHECK(!across.leftGround && Near(across.end.y, 3.0f, 0.01f) && across.lowest > 2.99f && across.end.z > 52.0f);
		const WoodWalk oldAcross = WalkWood({ 52, 3, 50.1f }, { 0, 0, 0.05f }, 40, false);
		CHECK(oldAcross.leftGround);  // the old way: off the floor at the first gap (and back on the next plank)
		// along a gap (x, centre in the gap at z 50.25): the planks either side
		const WoodWalk along = WalkWood({ 50.5f, 3, 50.25f }, { 0.05f, 0, 0 }, 60);
		CHECK(!along.leftGround && Near(along.end.y, 3.0f, 0.01f) && along.end.x > 53.4f);
		// diagonally
		const WoodWalk diag = WalkWood({ 50.5f, 3, 50.1f }, { 0.035f, 0, 0.035f }, 50);
		CHECK(!diag.leftGround && Near(diag.end.y, 3.0f, 0.01f));
		// the boardwalk: no dips into its gaps
		const WoodWalk board = WalkWood({ 72, 0.3f, 50.1f }, { 0, 0, 0.05f }, 60);
		CHECK(!board.leftGround && board.lowest > 0.29f && Near(board.end.y, 0.3f, 0.01f));
	}
	{  // real holes still drop him: into the 0.8 m hole, and off the scaffold's edge (both down to the ground 3 m below)
		const WoodWalk hole = WalkWood({ 52, 3, 51.8f }, { 0, 0, 0.04f }, 120);
		CHECK(hole.leftGround && hole.landed && Near(hole.end.y, 0.0f, 0.01f));
		const WoodWalk edge = WalkWood({ 53.5f, 3, 51.0f }, { 0.04f, 0, 0 }, 120);
		CHECK(edge.leftGround && edge.landed && Near(edge.end.y, 0.0f, 0.01f) && edge.end.x > 54.0f);
	}
	{  // falling onto the planks (5 m, centre over a gap): lands on them at ~9.9 m/s, not through them
		const WoodWalk fall = WalkWood({ 51, 8, 50.25f }, {}, 120, true, false);
		CHECK(fall.landed && Near(fall.end.y, 3.0f, 0.01f) && fall.landingSpeed > 9.0f && fall.landingSpeed < 10.8f);
		const WoodWalk old = WalkWood({ 51, 8, 50.25f }, {}, 120, false, false);
		CHECK(old.landed && Near(old.end.y, 0.0f, 0.01f));  // the old way: through the gap to the ground
	}
	{  // a thin platform (2 cm) caught from any height: 30 m up (~24 m/s at it, 0.4 m a frame) and from just above it
		const WoodWalk high = WalkWood({ 61, 32, 61 }, {}, 240, true, false);
		CHECK(high.landed && Near(high.end.y, 2.0f, 0.01f) && high.landingSpeed > 20.0f);
		const WoodWalk low = WalkWood({ 61, 2.3f, 61 }, {}, 60, true, false);
		CHECK(low.landed && Near(low.end.y, 2.0f, 0.01f));
		// standing on it and walking off its edge: off he goes
		const WoodWalk off = WalkWood({ 61, 2.0f, 61 }, { 0.05f, 0, 0 }, 90);
		CHECK(off.leftGround && Near(off.end.y, 0.0f, 0.01f));
	}
	{  // the puppet's start: the Tarnished stood on the planks with his centre over a gap -- Snake starts right there
		CHECK(!SupportedStart({ 51, 3, 50.25f }, params, &WoodRay));
		// at the scaffold's edge, centre 0.2 m past it: moved back onto the planks (4+ of the body's 5 probes on them)
		const auto rim = SupportedStart({ 54.2f, 3, 51.0f }, params, &WoodRay);
		CHECK(rim && rim->x < 54.0f - 0.1f && Near(rim->y, 3.0f, 0.01f));
		CHECK(rim && StartGround(BodyGround(*rim, top, top + params.snapDown, params.radius, &WoodRay)));
		// and then stands there
		CHECK(rim && !WalkWood(*rim, {}, 30).leftGround);
		// centre 0.1 m past the edge: two probes on the planks hold a walking body there, not a start (review 2026-10-14) -- moved in
		CHECK(BodyGround({ 54.1f, 3, 51.0f }, top, top + params.snapDown, params.radius, &WoodRay).supported);
		const auto inside = SupportedStart({ 54.1f, 3, 51.0f }, params, &WoodRay);
		CHECK(inside && inside->x <= 53.81f && Near(inside->y, 3.0f, 0.01f) &&
		      StartGround(BodyGround(*inside, top, top + params.snapDown, params.radius, &WoodRay)));
	}
	{  // Slopes (review 2026-10-14: the highest held probe floated the body 6-25 cm over every 10-40 degree slope): on a plane of
	   // any slope and heading the body stands on the ground under its axis -- the centre's, or with a gap under the centre the
	   // plane of the side probes (opposite probes' mean).
		int bad = 0, badGap = 0, badDeep = 0;
		for (const float deg : { 10.0f, 20.0f, 30.0f, 40.0f }) {
			for (const float radius : { 0.35f, 0.45f }) {
				for (int h = 0; h < 24; ++h) {
					const float ph = static_cast<float>(h) * (std::numbers::pi_v<float> / 12.0f);
					const float t = std::tan(deg * std::numbers::pi_v<float> / 180.0f);
					const float gx = t * std::cos(ph), gz = t * std::sin(ph);
					const Vec3  feet{ 1.0f, gx * 1.0f + gz * 2.0f, 2.0f };
					// exact plane; gapDepth < 0: nothing under the axis (a gap down to a drop), > 0: ground that much below the plane
					const auto planeRay = [=](float a_gapDepth) -> RayFn {
						return [=](const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit) {
							const bool axis = std::fabs(a_from.x - feet.x) < 0.03f && std::fabs(a_from.z - feet.z) < 0.03f;
							const float drop = axis ? a_gapDepth : 0.0f;
							if (axis && a_gapDepth < 0.0f) {
								return false;
							}
							const float f0 = a_from.y - (gx * a_from.x + gz * a_from.z - drop);
							const Vec3  to = a_from + a_delta;
							const float f1 = to.y - (gx * to.x + gz * to.z - drop);
							if (f0 <= 0.0f || f1 > 0.0f) {
								return f0 <= 0.0f ? (a_hit = a_from, true) : false;
							}
							a_hit = a_from + a_delta * (f0 / (f0 - f1));
							return true;
						};
					};
					const GroundProbe g = BodyGround(feet, top, top + params.snapDown, radius, planeRay(0.0f));
					bad += g.supported && g.centre && Near(g.y, feet.y, 0.005f) ? 0 : 1;
					const GroundProbe gap = BodyGround(feet, top, top + params.snapDown, radius, planeRay(-1.0f));
					badGap += gap.supported && !gap.centre && Near(gap.y, feet.y, 0.005f) ? 0 : 1;
					if (deg <= 20.0f) {  // ground 0.45 m below the plane under the axis (a boardwalk's gap on a slope)
						const GroundProbe deep = BodyGround(feet, top, top + params.snapDown, radius, planeRay(0.45f));
						badDeep += deep.supported && deep.centre && Near(deep.y, feet.y, 0.005f) ? 0 : 1;
					}
				}
			}
		}
		CHECK(bad == 0 && badGap == 0 && badDeep == 0);
		// walking a 25-degree ramp up and down (and standing on it): the feet stay on the ground under the axis
		const auto rampWalk = [&](float a_z, float a_x0, float a_step, int a_frames, float* a_worst, bool* a_ceiling) {
			KinematicParams p;
			p.bodyHeight = 1.8f;
			KinematicState s;
			s.pos = { a_x0, RampY(a_x0), a_z };
			s.grounded = true;
			*a_worst = 0.0f;
			*a_ceiling = false;
			bool air = false;
			for (int i = 0; i < a_frames; ++i) {
				const auto r = KinematicStep(s, { a_step, 0, 0 }, 1.0f / 60.0f, p, &WoodRay);
				*a_worst = std::max(*a_worst, std::fabs(s.pos.y - RampY(s.pos.x)));
				*a_ceiling |= r.ceiling;
				air |= !s.grounded;
			}
			return std::pair{ s.pos, air };
		};
		float worst = 0.0f;
		bool  ceiling = false;
		auto [stand, standAir] = rampWalk(53.0f, 83.0f, 0.0f, 30, &worst, &ceiling);
		CHECK(!standAir && worst <= 0.015f && Near(stand.x, 83.0f));
		auto [up, upAir] = rampWalk(53.0f, 80.6f, 0.04f, 120, &worst, &ceiling);
		CHECK(!upAir && worst <= 0.02f && up.x > 85.3f);
		auto [down, downAir] = rampWalk(53.0f, 86.0f, -0.04f, 120, &worst, &ceiling);
		CHECK(!downAir && worst <= 0.02f && down.x < 81.3f);
		// the plank ramp: the centre crosses its gaps (ground 0.3-0.6 m below near the bottom, nothing higher up) -- no dip, no float
		auto [pup, pupAir] = rampWalk(63.0f, 80.6f, 0.04f, 120, &worst, &ceiling);
		CHECK(!pupAir && worst <= 0.02f && pup.x > 85.3f);
		auto [pdown, pdownAir] = rampWalk(63.0f, 86.0f, -0.04f, 120, &worst, &ceiling);
		CHECK(!pdownAir && worst <= 0.02f && pdown.x < 81.3f);
		// up the sloped tunnel 1.9 m tall: the head room is measured from the feet on the slope, not 13 cm over it -- never refused
		auto [tun, tunAir] = rampWalk(73.0f, 80.6f, 0.04f, 120, &worst, &ceiling);
		CHECK(!tunAir && !ceiling && worst <= 0.02f && tun.x > 85.3f);
	}
	{  // Walls through both filters (er/Kinematic.h BodyRays; review 2026-10-14): the scaffold's floor and its railing are only in
	   // the characters' layers. The ground rays (CharRay) stand him on the floor; walls through the aim filter alone (WoodRay)
	   // walk him through the railing and off the edge, 3 m down; with the characters' layers too he stops a radius short of it.
		const RayFn both = BodyRays(RayFn(&CharRay), RayFn(&WoodRay), RayFn(&CharRay));
		const RayFn aimOnly = BodyRays(RayFn(&CharRay), RayFn(&WoodRay), RayFn{});
		const WoodWalk stopped = WalkWood({ 52, 3, 51.0f }, { 0.04f, 0, 0 }, 120, true, true, both);
		CHECK(!stopped.leftGround && Near(stopped.end.y, 3.0f, 0.01f) && stopped.end.x < 53.9f - params.radius + 0.01f && stopped.end.x > 53.4f);
		const WoodWalk through = WalkWood({ 52, 3, 51.0f }, { 0.04f, 0, 0 }, 120, true, true, aimOnly);
		CHECK(through.leftGround && Near(through.end.y, 0.0f, 0.01f) && through.end.x > 54.0f);
		// the router: vertical rays to the ground filter only, horizontal ones to the nearer of the two
		Vec3 hit;
		CHECK(BodyRays(RayFn(&WoodRay), RayFn{}, RayFn{})({ 52, 4, 51 }, { 0, -2, 0 }, hit) && Near(hit.y, 3.0f, 0.01f));
		CHECK(both({ 53.0f, 3.5f, 51 }, { 2, 0, 0 }, hit) && Near(hit.x, 53.9f, 0.01f));
		CHECK(!aimOnly({ 53.0f, 3.5f, 51 }, { 2, 0, 0 }, hit));
	}
}

static void TestStall()
{
	const float dt = 1.0f / 60.0f;
	auto        free = [&](Unstick& u, double& t, Vec3& p, int frames) {
        for (int i = 0; i < frames; ++i) {
            const Vec3 step{ 0.05f, 0, 0 };
            CHECK(!u.Stall(p, step, step, true, t, 2.5f));
            p += step;
            if (i % 3 == 0) {
                u.Update(p, 0, true, t, 0.5f, true);
            }
            t += dt;
        }
	};
	auto refused = [&](Unstick& u, double& t, const Vec3& p, const Vec3& a_want, int frames, int& fired, bool* noPlace = nullptr) -> std::optional<Vec3> {
		std::optional<Vec3> back;
		for (int i = 0; i < frames; ++i) {
			bool none = false;
			if (const auto b = u.Stall(p, a_want, Vec3{}, true, t, 2.5f, &none)) {
				back = b;
				++fired;
			}
			if (none && noPlace) {
				*noPlace = true;
				++fired;
			}
			t += dt;
		}
		return back;
	};
	{  // the player's case: refused forward, left, back, right -- 2.5 s of trying in 3+ directions: back where he stood free
		Unstick u;
		double  t = 10.0;
		Vec3    p{ 0, 0, 0 };
		free(u, t, p, 120);  // 6 m along x
		int  fired = 0;
		auto back = refused(u, t, p, { 0.05f, 0, 0 }, 50, fired);
		back = back ? back : refused(u, t, p, { 0, 0, 0.05f }, 50, fired);
		CHECK(!back && fired == 0);  // 1.7 s, two directions: not yet
		back = refused(u, t, p, { -0.05f, 0, 0.0f }, 60, fired);
		CHECK(back && fired == 1 && back->x < p.x - 0.45f && back->x > p.x - 2.1f && u.Stalls() == 1 && u.Count() == 1);
	}
	{  // pushing straight into a wall for 10 s is no stall (one direction), nor is a wall slid along (he moves)
		Unstick u;
		double  t = 10.0;
		Vec3    p{ 0, 0, 0 };
		free(u, t, p, 60);
		int fired = 0;
		CHECK(!refused(u, t, p, { 0.05f, 0, 0 }, 600, fired) && fired == 0);
		for (int i = 0; i < 300; ++i) {  // asked diagonally, moved along the wall: never refused
			CHECK(!u.Stall(p, { 0.05f, 0, 0.05f }, { 0, 0, 0.05f }, true, t, 2.5f));
			t += dt;
		}
	}
	{  // pauses (stick let go) keep the stall up to 4 s; a quarter second of real moving ends it
		Unstick u;
		double  t = 10.0;
		Vec3    p{ 0, 0, 0 };
		free(u, t, p, 60);
		int fired = 0;
		refused(u, t, p, { 0.05f, 0, 0 }, 60, fired);
		t += 3.0;  // a 3 s pause
		refused(u, t, p, { 0, 0, 0.05f }, 60, fired);
		CHECK(fired == 0 && u.StalledSeconds() > 1.9f);
		const auto back = refused(u, t, p, { 0, 0, -0.05f }, 40, fired);
		CHECK(back && fired == 1);
		refused(u, t, p, { 0.05f, 0, 0 }, 60, fired);
		free(u, t, p, 20);  // got away
		CHECK(u.StalledSeconds() == 0.0f);
		t += 5.0;  // a long pause after a short stall forgets it
		refused(u, t, p, { 0.05f, 0, 0 }, 60, fired);
		t += 5.0;
		refused(u, t, p, { 0, 0, 0.05f }, 60, fired);
		refused(u, t, p, { -0.05f, 0, 0 }, 60, fired);
		CHECK(fired == 1);
	}
	{  // nowhere free to go back to: the stall fires with no place (the caller suggests Tarnished mode)
		Unstick u;
		double  t = 10.0;
		int     fired = 0;
		bool    noPlace = false;
		const Vec3 p{ 3, 0, 3 };
		refused(u, t, p, { 0.05f, 0, 0 }, 60, fired, &noPlace);
		refused(u, t, p, { 0, 0, 0.05f }, 60, fired, &noPlace);
		refused(u, t, p, { -0.05f, 0, 0 }, 60, fired, &noPlace);
		CHECK(noPlace && fired == 1 && u.Stalls() == 1 && u.Count() == 0);
	}
	{  // a place under a low ceiling is never one to go back to (Update with no headroom records nothing)
		Unstick u;
		for (int i = 0; i < 10; ++i) {
			u.Update({ 0.1f * static_cast<float>(i), 0, 0 }, 0, true, 0.1 * i, 0.5f, false);
		}
		CHECK(!u.Update({ 3, 0, 0 }, 8, true, 2.0, 0.5f) && !u.Update({ 3, 0, 0 }, 8, true, 3.0, 0.5f));
	}
}

// Buddy changes in MGSV's own buddy menu (2026-10-14, the player: "buddy changing and dismissing"): the corner message.
static void TestBuddyChange()
{
	using namespace Buddy;
	CHECK(ChangeText(kWalkerGear, true, kWalkerGear, true).empty());
	CHECK(ChangeText(kWalkerGear, true, kNone, false) == "D-Walker dismissed -- no buddy now");
	CHECK(ChangeText(kWalkerGear, true, kWalkerGear, false) == "D-Walker dismissed -- no buddy now");
	CHECK(ChangeText(kNone, false, kHorse, true) == "Buddy: D-Horse is out");
	CHECK(ChangeText(kWalkerGear, true, kHorse, true) == "Buddy: D-Horse instead of D-Walker");
	CHECK(ChangeText(kHorse, true, kDog, true) == "Buddy: D-Dog instead of D-Horse");
	CHECK(ChangeText(kDog, false, kQuiet, false).empty());
}

// MGSV's Fulton on an Elden Ring enemy (protocol v12 kGEvFulton, er/Fulton.h): the lift and which character the event means.
static void TestFulton()
{
	using namespace Fulton;
	CHECK(LiftAt(0.0f).balloon == 0.0f && LiftAt(0.0f).rise == 0.0f && !LiftAt(0.0f).done);
	CHECK(Near(LiftAt(0.7f).balloon, 1.0f) && LiftAt(0.35f).balloon > 0.3f && LiftAt(0.35f).balloon < 0.7f);  // inflates in 0.7 s
	CHECK(LiftAt(0.6f).rise < 0.3f && Near(LiftAt(1.3f).rise, 1.25f, 0.02f) && LiftAt(2.1f).rise > 12.0f);   // off the ground, slow, then yanked
	float last = -1.0f;
	bool  rising = true;
	for (float t = 0.0f; t < kSeconds; t += 1.0f / 60.0f) {
		rising = rising && LiftAt(t).rise >= last;
		last = LiftAt(t).rise;
	}
	CHECK(rising && !LiftAt(kSeconds - 0.01f).done && LiftAt(kSeconds).done);
	const Candidate chrs[] = { { 0x10, { 0, 0, 0 }, true }, { 0x20, { 5, 0, 0 }, true }, { 0x30, { 1, 0, 0 }, false }, { 0x40, { 2, 0, 0 }, true } };
	CHECK(Pick(chrs, 0x20, { 0, 0, 0 }, 3.0f) == 1);   // its handle, wherever the proxy stood
	CHECK(Pick(chrs, 0x99, { 1.2f, 0, 0 }, 3.0f) == 3);  // unknown handle: the nearest living one to the proxy (0x30 is dead)
	CHECK(Pick(chrs, 0x30, { 1.0f, 0, 0 }, 3.0f) == 3);  // its handle is a dead one: never that
	CHECK(Pick(chrs, 0, { 10, 0, 0 }, 3.0f) == -1);       // nothing near
}

// Snake jumps (protocol v14 kGuestJumping, er/Kinematic.h KinematicJump; the player: "I'm having trouble getting into Stormveil
// castle without that"). A world to jump in: ground y = 0 everywhere but a pit 2 m wide (z in [55, 57], x in [50, 60], ground at
// -10); a ledge 1.0 m high (x in [30, 40], z in [62, 66]) and one 1.7 m high (x in [70, 80], z in [62, 66]); a slab 0.15 m thick
// 2.3 m up (x in [70, 80], z in [50, 54]); a ceiling 1.5 m up (x in [85, 95], z in [50, 54]); a cliff (x in [100, 110], ground at
// -5 from z 60).
namespace
{
	bool JumpSolid(const Vec3& p)
	{
		float ground = 0.0f;
		if (p.x >= 50.0f && p.x <= 60.0f && p.z >= 55.0f && p.z <= 57.0f) {
			ground = -10.0f;
		} else if (p.x >= 30.0f && p.x <= 40.0f && p.z >= 62.0f && p.z <= 66.0f) {
			ground = 1.0f;
		} else if (p.x >= 70.0f && p.x <= 80.0f && p.z >= 62.0f && p.z <= 66.0f) {
			ground = 1.7f;
		} else if (p.x >= 100.0f && p.x <= 110.0f && p.z >= 60.0f) {
			ground = -5.0f;
		}
		if (p.y < ground) {
			return true;
		}
		if (p.x >= 70.0f && p.x <= 80.0f && p.z >= 50.0f && p.z <= 54.0f && p.y >= 2.3f && p.y <= 2.45f) {
			return true;
		}
		return p.x >= 85.0f && p.x <= 95.0f && p.z >= 50.0f && p.z <= 54.0f && p.y >= 1.5f && p.y <= 1.6f;
	}

	bool JumpRay(const Vec3& from, const Vec3& delta, Vec3& hit)
	{
		if (JumpSolid(from)) {
			hit = from;
			return true;
		}
		constexpr int kSteps = 1000;
		Vec3          prev = from;
		for (int i = 1; i <= kSteps; ++i) {
			const Vec3 p = from + delta * (static_cast<float>(i) / kSteps);
			if (JumpSolid(p)) {
				Vec3 a = prev, b = p;
				for (int k = 0; k < 24; ++k) {
					const Vec3 m = (a + b) * 0.5f;
					(JumpSolid(m) ? b : a) = m;
				}
				hit = b;
				return true;
			}
			prev = p;
		}
		return false;
	}

	struct Hop
	{
		Vec3      end;
		JumpStart start;
		bool      jumped{ false };
		float     maxY{ -INFINITY };
		float     minY{ INFINITY };
		int       landings{ 0 };
		bool      jumpLanded{ false };
		float     landingSpeed{ -1 };
		bool      headBump{ false };
		int       airFrames{ 0 };
		bool      grounded{ false };
	};
	// Runs a_frames frames moving a_step per frame (1/60 s); jumps on the first frame at or past a_jumpZ (along +z; NAN: never).
	Hop Run(const Vec3& a_from, const Vec3& a_step, float a_jumpZ, int a_frames, float a_height = 1.8f, int a_stopAfterLanding = 1000)
	{
		KinematicParams params;
		params.bodyHeight = a_height;
		KinematicState s;
		s.pos = a_from;
		s.grounded = true;
		Hop         h;
		const float dt = 1.0f / 60.0f;
		int         sinceLanding = -1;
		for (int i = 0; i < a_frames; ++i) {
			Vec3 step = a_step;
			if (sinceLanding >= 0 && ++sinceLanding > a_stopAfterLanding) {
				step = {};
			}
			if (!h.jumped && std::isfinite(a_jumpZ) && s.pos.z >= a_jumpZ) {
				h.start = KinematicJump(s, step * (1.0f / dt), 1.2f, 0.15f, 12.0f, params, &JumpRay);
				h.jumped = true;
			}
			const auto r = KinematicStep(s, step, dt, params, &JumpRay);
			h.maxY = std::max(h.maxY, s.pos.y);
			h.minY = std::min(h.minY, s.pos.y);
			h.headBump |= r.headBump;
			h.airFrames += s.grounded ? 0 : 1;
			if (r.landed) {
				++h.landings;
				h.jumpLanded |= r.jumpLanded;
				h.landingSpeed = r.landingSpeed;
				sinceLanding = 0;
			}
		}
		h.end = s.pos;
		h.grounded = s.grounded;
		return h;
	}
}

static void TestJump()
{
	const Config d;
	{  // a flat jump standing still: up ~1.2 m, down again in ~1 s, and its landing is no fall (no fall damage)
		const Hop h = Run({ 20, 0, 20 }, {}, 20.0f, 120);
		CHECK(h.start.ok && Near(h.start.apex, 1.2f, 1e-3f) && h.maxY > 1.15f && h.maxY < 1.21f);
		CHECK(h.landings == 1 && h.jumpLanded && h.grounded && Near(h.end.y, 0.0f, 1e-3f) && h.airFrames > 54 && h.airFrames < 66);
		CHECK(h.landingSpeed >= 0.0f && h.landingSpeed < 0.5f && combat::FallFraction(h.landingSpeed, d.fallHurtSpeed, d.fallLethalSpeed) == 0.0f);
		CHECK(Near(h.end.x, 20.0f, 1e-4f) && Near(h.end.z, 20.0f, 1e-4f));
		// once in the air, no second jump
		KinematicParams params;
		params.bodyHeight = 1.8f;
		KinematicState s;
		s.pos = { 20, 0, 20 };
		s.grounded = true;
		CHECK(KinematicJump(s, {}, 1.2f, 0.15f, 12.0f, params, &JumpRay).ok);
		KinematicStep(s, {}, 1.0f / 60.0f, params, &JumpRay);
		const float vy = s.vy;
		CHECK(!KinematicJump(s, {}, 1.2f, 0.15f, 12.0f, params, &JumpRay).ok && s.vy == vy);
	}
	{  // across a 2 m gap running (6 m/s): over it and on, on the far side; without the jump he falls in
		const Hop h = Run({ 55, 0, 50 }, { 0, 0, 0.1f }, 54.6f, 150, 1.8f, 5);
		CHECK(h.start.ok && Near(h.start.speed, 6.0f, 0.01f) && h.landings == 1 && h.jumpLanded && h.grounded);
		CHECK(h.minY > -0.01f && Near(h.end.y, 0.0f, 1e-3f) && h.end.z > 58.0f);
		CHECK(h.landingSpeed < 0.5f);
		const Hop fall = Run({ 55, 0, 50 }, { 0, 0, 0.1f }, NAN, 150);
		CHECK(fall.end.y < -5.0f);
	}
	{  // onto a 1.0 m ledge, running at it (4 m/s) and jumping a metre before it; without the jump the ledge stops him
		const Hop h = Run({ 35, 0, 58 }, { 0, 0, 4.0f / 60.0f }, 61.0f, 120, 1.8f, 10);
		CHECK(h.start.ok && h.jumpLanded && h.grounded && Near(h.end.y, 1.0f, 1e-3f) && h.end.z > 62.5f && h.maxY < 1.5f);
		const Hop walk = Run({ 35, 0, 58 }, { 0, 0, 4.0f / 60.0f }, NAN, 120);
		CHECK(Near(walk.end.y, 0.0f, 1e-3f) && walk.end.z < 62.0f - 0.33f);
		// ... standing at its foot, a jump straight up and pushing on: up as well
		const Hop stand = Run({ 35, 0, 61.6f }, { 0, 0, 2.0f / 60.0f }, 61.6f, 120);
		CHECK(stand.start.ok && Near(stand.end.y, 1.0f, 1e-3f) && stand.end.z > 62.3f);
	}
	{  // a 1.7 m ledge is too high: he rises against its face and comes down in front of it
		const Hop h = Run({ 75, 0, 58 }, { 0, 0, 4.0f / 60.0f }, 61.0f, 120);
		CHECK(h.start.ok && h.jumpLanded && Near(h.end.y, 0.0f, 1e-3f) && h.end.z < 62.0f - 0.33f);
	}
	{  // under a slab 2.3 m up (0.5 m over his head): the jump rises 0.45 m, his head never goes into it, he never ends on it
		const Hop h = Run({ 75, 0, 52 }, {}, 52.0f, 120);
		CHECK(h.start.ok && h.start.room > 0.45f && h.start.room < 0.55f && h.start.apex < 0.5f);
		CHECK(h.maxY + 1.8f <= 2.3f + 1e-3f && h.grounded && Near(h.end.y, 0.0f, 1e-3f) && h.landingSpeed < 0.5f);
		// the same running out from under it: the rise stops at the slab, then he is out and down on the ground
		const Hop run = Run({ 75, 0, 51 }, { 0, 0, 0.1f }, 52.0f, 120, 1.8f, 5);
		CHECK(run.start.ok && run.maxY + 1.8f <= 2.3f + 1e-3f && run.grounded && Near(run.end.y, 0.0f, 1e-3f) && run.end.z > 54.5f);
	}
	{  // under a 1.5 m ceiling (lower than him: Elden Ring put him there): no jump at all, nothing changed -- not through it
		KinematicParams params;
		params.bodyHeight = 1.8f;
		KinematicState s;
		s.pos = { 90, 0, 52 };
		s.grounded = true;
		const auto j = KinematicJump(s, { 0, 0, 3 }, 1.2f, 0.15f, 12.0f, params, &JumpRay);
		CHECK(!j.ok && j.room < 0.0f && s.grounded && !s.jumping && s.vy == 0.0f);
		params.bodyHeight = 0.0f;  // [Puppet] ceiling = false: his head is the head ray's height + 0.2 m
		CHECK(!KinematicJump(s, {}, 1.2f, 0.15f, 12.0f, params, &JumpRay).ok);
		params.bodyHeight = 1.2f;  // crouched (the guest never jumps him crouched; the room alone would allow 0.25 m)
		const auto c = KinematicJump(s, {}, 1.2f, 0.15f, 12.0f, params, &JumpRay);
		CHECK(c.ok && c.apex < 0.3f);
		bool above = false;
		for (int i = 0; i < 120; ++i) {
			KinematicStep(s, {}, 1.0f / 60.0f, params, &JumpRay);
			above |= s.pos.y + 1.2f > 1.5f + 1e-3f;
		}
		CHECK(!above && s.grounded && Near(s.pos.y, 0.0f, 1e-3f));
	}
	{  // off a 5 m cliff: the landing is a fall from the takeoff's height (as walking off it), not from the jump's apex
		const Hop h = Run({ 105, 0, 57 }, { 0, 0, 0.1f }, 59.5f, 150, 1.8f, 5);
		CHECK(h.start.ok && h.jumpLanded && Near(h.end.y, -5.0f, 1e-3f));
		CHECK(h.landingSpeed > 9.6f && h.landingSpeed < 10.2f);  // sqrt(2 x 9.81 x 5) = 9.9 m/s (the apex: 10.9)
	}
	{  // air control: a standing jump, then Snake runs sideways (6 m/s): limited -- under 3 m/s^2 -- not a run in the air
		const Hop h = Run({ 20, 0, 30 }, { 0.1f, 0, 0 }, 30.0f, 61);
		CHECK(h.start.ok && Near(h.start.speed, 6.0f, 0.01f));  // the run in the first frame counts as his takeoff speed...
		KinematicParams params;
		params.bodyHeight = 1.8f;
		KinematicState s;
		s.pos = { 20, 0, 30 };
		s.grounded = true;
		CHECK(KinematicJump(s, {}, 1.2f, 0.15f, 12.0f, params, &JumpRay).ok);  // ... standing still it is none
		float fastest = 0.0f;
		for (int i = 0; i < 90 && !s.grounded; ++i) {
			KinematicStep(s, { 0.1f, 0, 0 }, 1.0f / 60.0f, params, &JumpRay);
			fastest = std::max(fastest, LengthXZ(s.airVel));
		}
		CHECK(s.grounded && s.pos.x > 20.5f && s.pos.x < 21.7f && fastest <= 3.05f);
	}
}

// A wooden scaffold (live 2026-10-14: the player switched back to Snake on top of one the Tarnished stood on, and Snake fell
// through it and died): plank floors 6 m up over ground at y = 0, the ground under Snake seen by the body's five ground rays
// (BodyGround; merged with the jump's own seam/gap probes, which these cases were written for). Boards along x, x in [200, 204]: 26 cm boards with 4 cm seams (board k: z in
// [0.3k, 0.3k + 0.26], k 0-9); x in [210, 214]: 30 cm planks with 20 cm gaps (plank k: z in [0.5k, 0.5k + 0.3], k 0-5). Each
// 5 cm thick (top at 6.0).
namespace
{
	bool ScaffoldSolid(const Vec3& p)
	{
		if (p.y < 0.0f) {
			return true;
		}
		if (p.y < 5.95f || p.y > 6.0f || p.z < 0.0f) {
			return false;
		}
		if (p.x >= 200.0f && p.x <= 204.0f && p.z <= 3.0f) {
			return std::fmod(p.z, 0.3f) <= 0.26f;
		}
		if (p.x >= 210.0f && p.x <= 214.0f && p.z <= 3.0f) {
			return std::fmod(p.z, 0.5f) <= 0.3f;
		}
		return false;
	}

	bool ScaffoldRay(const Vec3& from, const Vec3& delta, Vec3& hit)
	{
		if (ScaffoldSolid(from)) {
			hit = from;
			return true;
		}
		constexpr int kSteps = 2000;
		Vec3          prev = from;
		for (int i = 1; i <= kSteps; ++i) {
			const Vec3 p = from + delta * (static_cast<float>(i) / kSteps);
			if (ScaffoldSolid(p)) {
				Vec3 a = prev, b = p;
				for (int k = 0; k < 24; ++k) {
					const Vec3 m = (a + b) * 0.5f;
					(ScaffoldSolid(m) ? b : a) = m;
				}
				hit = b;
				return true;
			}
			prev = p;
		}
		return false;
	}

	struct Stand
	{
		float minY{ INFINITY };
		bool  grounded{ false };
		bool  landed{ false };
		float landingSpeed{ 0 };
		Vec3  end;
	};
	// a_frames frames (1/60 s) from a_s, moving a_step per frame.
	Stand Frames(KinematicState& a_s, const Vec3& a_step, int a_frames, const KinematicParams& a_params)
	{
		Stand st;
		for (int i = 0; i < a_frames; ++i) {
			const auto r = KinematicStep(a_s, a_step, 1.0f / 60.0f, a_params, &ScaffoldRay);
			st.minY = std::min(st.minY, a_s.pos.y);
			if (r.landed) {
				st.landed = true;
				st.landingSpeed = r.landingSpeed;
			}
		}
		st.grounded = a_s.grounded;
		st.end = a_s.pos;
		return st;
	}
}

static void TestScaffold()
{
	KinematicParams params;
	params.bodyHeight = 1.8f;
	const auto start = [](const Vec3& a_at) {
		KinematicState s;
		s.pos = a_at;
		s.grounded = true;  // Puppet Start: where the Tarnished stood
		return s;
	};
	{  // the player's case: Snake starts standing still with his axis over a 4 cm seam -- he stays up (one ray there finds nothing)
		Vec3 hit;
		CHECK(!ScaffoldRay({ 202, 6.55f, 0.28f }, { 0, -1.15f, 0 }, hit));  // the old ground check: nothing under him
		CHECK(!SupportedStart({ 202, 6, 0.28f }, params, &ScaffoldRay));     // the start's check sees the boards: start there
		auto       s = start({ 202, 6, 0.28f });
		const auto st = Frames(s, {}, 180, params);
		CHECK(st.grounded && st.minY > 5.999f && Near(st.end.y, 6.0f, 1e-3f) && !st.landed);
		KinematicParams single = params;
		single.groundProbes = false;  // one ray under his centre (the old way): through the seam, 6 m down
		auto s0 = start({ 202, 6, 0.28f });
		const auto fell = Frames(s0, {}, 180, single);
		CHECK(fell.landed && Near(fell.end.y, 0.0f, 1e-2f) && fell.landingSpeed > 10.0f);
	}
	{  // a 20 cm gap between planks under his axis: the side probes find the planks; with one ray (ground_probes = false) he falls
		auto       s = start({ 212, 6, 0.4f });
		const auto st = Frames(s, {}, 180, params);
		CHECK(st.grounded && st.minY > 5.999f);
		KinematicParams single = params;
		single.groundProbes = false;
		auto       s0 = start({ 212, 6, 0.4f });
		const auto fell = Frames(s0, {}, 180, single);
		CHECK(fell.landed && Near(fell.end.y, 0.0f, 1e-2f) && fell.landingSpeed > 10.0f);  // what killed him: 6 m, 10.8 m/s
	}
	{  // walking slowly across the boards (0.5 m/s, a few frames over each seam) and across the planks: never down
		auto s = start({ 202, 6, 0.1f });
		CHECK(Frames(s, { 0, 0, 0.5f / 60.0f }, 300, params).minY > 5.999f && s.grounded);
		auto w = start({ 212, 6, 0.1f });
		CHECK(Frames(w, { 0, 0, 0.5f / 60.0f }, 300, params).minY > 5.999f && w.grounded);
	}
	{  // a standing jump over a seam, and a jump's descent onto a seam: lands on the boards, no fall
		auto s = start({ 202, 6, 0.28f });
		CHECK(KinematicJump(s, {}, 1.2f, 0.15f, 12.0f, params, &ScaffoldRay).ok);
		const auto st = Frames(s, {}, 120, params);
		CHECK(st.landed && st.grounded && Near(st.end.y, 6.0f, 1e-3f) && st.landingSpeed < 0.5f);
		KinematicState d;  // coming down on the seam at z 0.58 (a slow jump's landing)
		d.pos = { 202, 6.6f, 0.58f };
		d.jumping = true;
		d.jumpFromY = 6.0f;
		d.vy = -2.0f;
		const auto dn = Frames(d, {}, 60, params);
		CHECK(dn.landed && dn.grounded && Near(dn.end.y, 6.0f, 1e-3f) && dn.minY > 5.999f && dn.landingSpeed < 0.5f);
		KinematicState f;  // falling (no jump) onto the 20 cm gap from 1 m up
		f.pos = { 212, 7.0f, 0.4f };
		const auto ff = Frames(f, {}, 60, params);
		CHECK(ff.landed && Near(ff.end.y, 6.0f, 1e-3f) && ff.landingSpeed > 4.0f && ff.landingSpeed < 4.8f);
	}
	{  // walking off the scaffold's end still falls, once fewer than two of his ground probes are on it: his axis at most the
	   // probes' reach (0.8 x radius) past the edge (ground on one side only is an edge, not a gap)
		auto s = start({ 203.5f, 6, 1.1f });
		bool fellOver = false;
		for (int i = 0; i < 90 && s.grounded; ++i) {
			KinematicStep(s, { 1.0f / 60.0f, 0, 0 }, 1.0f / 60.0f, params, &ScaffoldRay);
			fellOver = !s.grounded && s.pos.x > 204.0f && s.pos.x < 204.0f + kGroundProbeRadius * params.radius + 0.02f;
		}
		CHECK(fellOver);
		const auto st = Frames(s, { 1.0f / 60.0f, 0, 0 }, 240, params);
		CHECK(st.landed && Near(st.end.y, 0.0f, 1e-2f));
		// ... and so does standing with his axis 30 cm past it (10 cm past, two probes still hold a standing body: a start there
		// is moved in, SupportedStart)
		auto e = start({ 204.3f, 6, 1.1f });
		CHECK(Frames(e, {}, 120, params).landed && Near(e.pos.y, 0.0f, 1e-2f));
		auto h = start({ 204.1f, 6, 1.1f });
		CHECK(!Frames(h, {}, 120, params).landed && h.grounded && Near(h.pos.y, 6.0f, 1e-3f));
		const auto in = SupportedStart({ 204.1f, 6, 1.1f }, params, &ScaffoldRay);
		CHECK(in && in->x < 204.0f && Near(in->y, 6.0f, 1e-3f));
	}
}

// Review fixes for the jump (2026-10-14): a world shift in the air moves the takeoff height too; kGuestJumping's edge never
// counts a not-ready frame as the flag going down.
static void TestJumpReview()
{
	{  // off a 5 m cliff with Elden Ring's origin moving +8 m in the air: the landing is still a 5 m fall (9.9 m/s), not 0
		KinematicParams params;
		params.bodyHeight = 1.8f;
		Vec3        off{};
		const RayFn shifted = [&off](const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit) {
			if (!JumpRay(a_from - off, a_delta, a_hit)) {
				return false;
			}
			a_hit += off;
			return true;
		};
		KinematicState s;
		s.pos = { 105, 0, 59.5f };
		s.grounded = true;
		CHECK(KinematicJump(s, { 0, 0, 6 }, 1.2f, 0.15f, 12.0f, params, shifted).ok);
		float landing = -1.0f;
		for (int i = 0; i < 200 && landing < 0.0f; ++i) {
			if (i == 20) {
				off = { 0, 8, 0 };
				KinematicShift(s, off);
				CHECK(Near(s.jumpFromY, 8.0f, 1e-4f) && s.jumping);
			}
			const auto r = KinematicStep(s, { 0, 0, 0.1f }, 1.0f / 60.0f, params, shifted);
			if (r.landed) {
				CHECK(r.jumpLanded);
				landing = r.landingSpeed;
			}
		}
		CHECK(Near(s.pos.y, 3.0f, 1e-3f) && landing > 9.6f && landing < 10.2f);
	}
	{  // the guest's flag: one jump per rising edge; a frame not ready (or not read) mid-jump never makes a second one
		JumpEdge e;
		CHECK(!e.Update(true, true, false));
		CHECK(e.Update(true, true, true));      // pressed
		CHECK(!e.Update(true, true, true));
		CHECK(!e.Update(true, false, true));    // a hitch: not ready, the flag still up (MGSV hasn't seen the landing)
		CHECK(!e.Update(true, true, true));     // ready again: the same press, no second jump (the review's case)
		CHECK(!e.Update(false, false, false));  // nothing read: the flag stands
		CHECK(!e.Update(true, true, true));
		CHECK(!e.Update(true, true, false));
		CHECK(e.Update(true, true, true));      // a new press
		CHECK(!e.Update(true, true, false));
		CHECK(!e.Update(true, false, true));    // a press while not ready: no jump, and none later from it
		CHECK(!e.Update(true, true, true));
		e.Reset(true);                          // the puppet starts with a jump under way in MGSV: not one to make
		CHECK(!e.Update(true, true, true));
	}
}
