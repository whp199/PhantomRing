// Included by tests/test_host.cpp: Snake climbs onto / vaults over a ledge (protocol v15 kHEvMantle, er/Mantle.h PlanMantle), in a
// synthetic world: a ledge of the right height with room is climbed, a thin low fence with ground behind is vaulted, and
// everything else is refused cleanly -- never a path through a wall, a ceiling or into a drop.

namespace
{
	// Lanes along +z, 10 m apart in x (each spans x in [lane - 2, lane + 2]); ground y = 0; the obstacle's face at z = 1.
	//   x = 0:   a 1.0 m ledge (z >= 1)                          -> climb up
	//   x = 10:  a 1.6 m ledge                                   -> climb up
	//   x = 20:  a 2.4 m wall                                    -> too high
	//   x = 30:  a 0.9 m fence, 0.15 m thick (z in [1, 1.15])     -> vault over
	//   x = 40:  the same fence with a 4 m drop behind it          -> refused (a drop behind it, nothing to stand on)
	//   x = 50:  a 1.0 m ledge under a ceiling slab y in [2.0, 2.2] for z >= 1 -> no room to stand up there
	//   x = 60:  a 1.0 m ledge, a ceiling y in [1.7, 1.9] over where he stands (z < 1) -> no room above him to climb
	//   x = 70:  a 0.4 m step                                    -> low enough to step onto
	//   x = 80:  nothing                                         -> nothing in front
	//   x = 90:  a 1.2 m wall 0.3 m thick (z in [1, 1.3]), then a 3 m wall at z >= 1.6 -> refused (no landing, nothing to stand on)
	//   x = 100: a 1.0 m fence, 0.15 m thick, ground 0.5 m lower behind it (z > 1.15) -> vault over, down to -0.5
	//   x = 110: a 1.1 m ledge under a 0.12 m beam at y [2.0, 2.12] above its face only (z in [0.9, 1.1]) -> no room above him to climb
	bool MantleSolid(const Vec3& p)
	{
		const auto lane = [&](float a_x) { return p.x >= a_x - 2.0f && p.x <= a_x + 2.0f; };
		float      ground = 0.0f;
		if (lane(0.0f) && p.z >= 1.0f) {
			ground = 1.0f;
		} else if (lane(10.0f) && p.z >= 1.0f) {
			ground = 1.6f;
		} else if (lane(20.0f) && p.z >= 1.0f) {
			ground = 2.4f;
		} else if (lane(30.0f) && p.z >= 1.0f && p.z <= 1.15f) {
			ground = 0.9f;
		} else if (lane(40.0f) && p.z >= 1.0f) {
			ground = p.z <= 1.15f ? 0.9f : -4.0f;
		} else if (lane(50.0f) && p.z >= 1.0f) {
			ground = 1.0f;
		} else if (lane(60.0f) && p.z >= 1.0f) {
			ground = 1.0f;
		} else if (lane(70.0f) && p.z >= 1.0f) {
			ground = 0.4f;
		} else if (lane(90.0f) && p.z >= 1.0f && p.z <= 1.3f) {
			ground = 1.2f;
		} else if (lane(90.0f) && p.z >= 1.6f) {
			ground = 3.0f;
		} else if (lane(100.0f) && p.z >= 1.0f) {
			ground = p.z <= 1.15f ? 1.0f : -0.5f;
		} else if (lane(110.0f) && p.z >= 1.0f) {
			ground = 1.1f;
		}
		if (p.y < ground) {
			return true;
		}
		if (lane(50.0f) && p.z >= 1.0f && p.y >= 2.0f && p.y <= 2.2f) {
			return true;
		}
		if (lane(60.0f) && p.z < 1.0f && p.y >= 1.7f && p.y <= 1.9f) {
			return true;
		}
		return lane(110.0f) && p.z >= 0.9f && p.z <= 1.1f && p.y >= 2.0f && p.y <= 2.12f;
	}

	bool MantleRay(const Vec3& from, const Vec3& delta, Vec3& hit)
	{
		if (MantleSolid(from)) {
			hit = from;
			return true;
		}
		constexpr int kSteps = 600;
		Vec3          prev = from;
		for (int i = 1; i <= kSteps; ++i) {
			const Vec3 p = from + delta * (static_cast<float>(i) / kSteps);
			if (MantleSolid(p)) {
				Vec3 a = prev, b = p;
				for (int k = 0; k < 24; ++k) {
					const Vec3 m = (a + b) * 0.5f;
					(MantleSolid(m) ? b : a) = m;
				}
				hit = b;
				return true;
			}
			prev = p;
		}
		return false;
	}

	// Snake stands at a_x, z 0.55 (his radius 0.35 + 0.1 from the face), facing +z.
	MantlePlan Plan(float a_x, bool a_vault = true, float a_body = 1.8f)
	{
		KinematicParams params;
		params.bodyHeight = a_body;
		return PlanMantle({ a_x, 0.0f, 0.55f }, { 0.0f, 0.0f, 1.0f }, a_body, MantleParams{}, params, &MantleRay, a_vault);
	}

	// The body along the whole path never inside anything: its axis at the feet + 0.05, the knees, the waist, his shoulders, and
	// at 0.8 x its radius around it. A climb up bends him over the top (the shoulders at most 1 m above the top while rising).
	bool PathClear(const MantlePlan& a_p, float a_r = 0.35f)
	{
		const int n = 120;
		for (int i = 0; i <= n; ++i) {
			const Vec3 f = MantlePoint(a_p, a_p.duration * static_cast<float>(i) / n);
			for (const float h : { 0.05f, 0.4f, 0.9f }) {
				for (const Vec3 off : { Vec3{}, Vec3{ 0.8f * a_r, 0, 0 }, Vec3{ -0.8f * a_r, 0, 0 }, Vec3{ 0, 0, -0.8f * a_r } }) {
					if (MantleSolid(f + off + Vec3{ 0.0f, h, 0.0f })) {
						std::printf("  path inside at t=%.2f: (%.2f, %.2f, %.2f) +%.2f\n", a_p.duration * static_cast<float>(i) / n, f.x + off.x, f.y + h,
							f.z + off.z, h);
						return false;
					}
				}
			}
		}
		return true;
	}
}

static void TestMantle()
{
	for (float x = 0.0f; x <= 110.0f; x += 10.0f) {
		const auto p = Plan(x);
		std::printf("mantle lane %3.0f: %-10s %-52s h %.2f face %.2f depth %.2f %.2f s\n", x, std::string(Name(p.kind)).c_str(), p.why, p.height, p.face,
			p.depth, p.duration);
	}
	{  // a 1 m ledge: climbed onto, ~1.2 s, ending on its top clear of the edge
		const auto p = Plan(0.0f);
		CHECK(p.kind == MantleKind::kClimb && p.ledge);
		CHECK(Near(p.height, 1.0f, 0.02f) && Near(p.face, 0.45f, 0.03f));
		CHECK(p.duration > 1.0f && p.duration < 1.4f);
		const Vec3 end = p.End();
		CHECK(Near(end.y, 1.0f, 0.02f) && end.z >= 1.0f + 0.3f && Near(end.x, 0.0f));
		CHECK(Near(MantlePoint(p, 0.0f), Vec3{ 0.0f, 0.0f, 0.55f }) && Near(MantlePoint(p, p.duration), end) && Near(MantlePoint(p, 99.0f), end));
		CHECK(PathClear(p));
		// rises before it moves over (the feet clear the edge first)
		const Vec3 mid = MantlePoint(p, 0.55f * p.duration);
		CHECK(mid.y >= 1.0f && mid.z < 1.0f);
	}
	{  // 1.6 m: still climbed, longer
		const auto p = Plan(10.0f);
		CHECK(p.kind == MantleKind::kClimb && Near(p.height, 1.6f, 0.02f) && p.duration > Plan(0.0f).duration);
		CHECK(PathClear(p));
	}
	{  // 2.4 m: a wall
		const auto p = Plan(20.0f);
		CHECK(p.kind == MantleKind::kNone && p.ledge && std::string_view(p.why).find("too high") != std::string_view::npos);
	}
	{  // a 0.9 m fence with ground behind: vaulted, landing on the far side's ground
		const auto p = Plan(30.0f);
		CHECK(p.kind == MantleKind::kVault && Near(p.height, 0.9f, 0.02f) && p.depth < 0.3f);
		CHECK(Near(p.End().y, 0.0f, 0.02f) && p.End().z > 1.15f + 0.3f && Near(p.drop, 0.0f, 0.02f));
		CHECK(p.duration > 1.1f && p.duration < 1.5f);  // MGSV's vault: down on the far side ~1.35 s after the request (live)
		CHECK(PathClear(p));
		// with vaults not wanted, nothing to stand on up there (0.15 m): it still vaults (the only way over)
		CHECK(Plan(30.0f, false).kind == MantleKind::kVault);
	}
	{  // the fence over a 4 m drop: never over it, and nothing to stand on
		const auto p = Plan(40.0f);
		CHECK(p.kind == MantleKind::kNone && p.ledge);
		CHECK(std::string_view(p.why).find("drop") != std::string_view::npos);
	}
	{  // a ledge with a ceiling 1 m over its top: no room to stand up there (crouched 1.2 m wouldn't fit either)
		const auto p = Plan(50.0f);
		CHECK(p.kind == MantleKind::kNone && std::string_view(p.why).find("no room") != std::string_view::npos);
	}
	{  // a ceiling 1.7 m over where he stands: no room to rise 1 m along the face
		const auto p = Plan(60.0f);
		CHECK(p.kind == MantleKind::kNone && std::string_view(p.why).find("no room above him") != std::string_view::npos);
	}
	{  // a 0.4 m step is walked onto (KinematicStep's step-up), never climbed
		const auto p = Plan(70.0f);
		CHECK(p.kind == MantleKind::kNone && std::string_view(p.why).find("step") != std::string_view::npos);
	}
	{  // nothing in front: no ledge at all (the Y stays MGSV's, no log)
		const auto p = Plan(80.0f);
		CHECK(p.kind == MantleKind::kNone && !p.ledge);
	}
	{  // a 1.2 m wall with a higher wall right behind it: no landing, nothing to stand on
		const auto p = Plan(90.0f);
		CHECK(p.kind == MantleKind::kNone && p.ledge);
	}
	{  // a fence with lower ground behind it: down to it
		const auto p = Plan(100.0f);
		CHECK(p.kind == MantleKind::kVault && Near(p.End().y, -0.5f, 0.02f) && Near(p.drop, 0.5f, 0.02f));
		CHECK(PathClear(p));
	}
	{  // a beam over the face: his head would go into it over the edge
		const auto p = Plan(110.0f);
		CHECK(p.kind == MantleKind::kNone && Near(p.height, 1.1f, 0.02f) && std::string_view(p.why).find("in the way") != std::string_view::npos);
	}
	{  // facing away from the ledge: nothing in front
		KinematicParams params;
		const auto      p = PlanMantle({ 0.0f, 0.0f, 0.55f }, { 0.0f, 0.0f, -1.0f }, 1.8f, MantleParams{}, params, &MantleRay);
		CHECK(p.kind == MantleKind::kNone && !p.ledge);
		// out of reach (1.5 m from the face): nothing in front either
		CHECK(PlanMantle({ 0.0f, 0.0f, -0.5f }, { 0.0f, 0.0f, 1.0f }, 1.8f, MantleParams{}, params, &MantleRay).kind == MantleKind::kNone);
	}
	{  // a diagonal approach (45 degrees to the face) still finds it and stays clear
		KinematicParams params;
		const float     s = std::sqrt(0.5f);
		const auto      p = PlanMantle({ 0.0f, 0.0f, 0.6f }, { s, 0.0f, s }, 1.8f, MantleParams{}, params, &MantleRay);
		CHECK(p.kind == MantleKind::kClimb && Near(p.height, 1.0f, 0.02f) && PathClear(p));
	}
	{  // the run: waits for MGSV's kGuestMantling (or wait_ms), then moves along the plan, done once
		const auto p = Plan(0.0f);
		MantleRun  run;
		CHECK(!run.Active() && !run.Update(1000, false, 300));
		run.Begin(p, 1000);
		bool done = false;
		auto at = run.Update(1100, false, 300, &done);
		CHECK(run.Current() == MantleRun::Phase::kWaiting && at && Near(*at, p.keys[0]) && !done);
		at = run.Update(1150, true, 300, &done);  // MGSV plays it: the path starts now
		CHECK(run.Current() == MantleRun::Phase::kMoving && run.GuestPlayed() && run.WaitedMs() == 150);
		at = run.Update(1150 + static_cast<std::uint64_t>(p.duration * 500.0f), true, 300, &done);
		CHECK(at && at->y > 0.3f && !done);
		at = run.Update(1150 + static_cast<std::uint64_t>(p.duration * 1000.0f) + 1, false, 300, &done);
		CHECK(done && at && Near(*at, p.End()) && !run.Active());
		CHECK(!run.Update(5000, false, 300, &done) && !done);
		// without MGSV's word: it goes after wait_ms
		MantleRun alone;
		alone.Begin(p, 0);
		alone.Update(299, false, 300);
		CHECK(alone.Current() == MantleRun::Phase::kWaiting);
		alone.Update(300, false, 300);
		CHECK(alone.Current() == MantleRun::Phase::kMoving && !alone.GuestPlayed());
		// an origin shift moves the whole path
		alone.Shift({ 8.0f, -8.0f, 0.0f });
		CHECK(Near(alone.Plan().End(), p.End() + Vec3{ 8.0f, -8.0f, 0.0f }));
	}
	CHECK(Name(MantleKind::kClimb) == "climb up" && Name(MantleKind::kVault) == "vault over");
}
