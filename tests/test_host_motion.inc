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
		return p.x >= 30.0f && p.x <= 32.0f && p.z >= 31.4f && p.z <= 31.6f && p.y >= 0.7f && p.y <= 0.75f;
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
