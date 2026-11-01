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
	//   x = 120: rubble in steps: 0.7 m for z in [1, 1.3], then 1.3 m from z = 1.3 on -> climbed onto the 1.3 m step (a farther landing)
	//   x = 130: a 0.7 m step 0.3 m deep, then a 2.5 m wall from z = 1.3 on -> refused (a wall past it, higher than he climbs)
	//   x = 140: a rock whose top rises past its lip: 0.8 m at z = 1, then up 0.5 m per metre -> climbed (onto the rising top)
	//   x = 160: a rail fence: posts aside, two 0.1 m deep rails at y [0.4, 0.5] and [0.8, 0.95], z in [1, 1.1] -> vault over (the
	//            face rays at 0.3, 0.7 and 1.1 m alone all passed between the rails)
	//   x = 150: a rounded boulder: a 0.4 m lip at z = 1 rising 1 m per metre to its 0.9 m crown at z = 1.5 -> climbed (the lip alone
	//            reads as a step the walk can't take: its crown stops the knee ray)
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
		} else if (lane(120.0f) && p.z >= 1.0f) {
			ground = p.z < 1.3f ? 0.7f : 1.3f;
		} else if (lane(130.0f) && p.z >= 1.0f) {
			ground = p.z < 1.3f ? 0.7f : 2.5f;
		} else if (lane(140.0f) && p.z >= 1.0f) {
			ground = 0.8f + 0.5f * (p.z - 1.0f);
		} else if (lane(150.0f) && p.z >= 1.0f) {
			ground = std::min(0.9f, 0.4f + (p.z - 1.0f));
		}
		if (lane(160.0f) && p.z >= 1.0f && p.z <= 1.1f && ((p.y >= 0.4f && p.y <= 0.5f) || (p.y >= 0.8f && p.y <= 0.95f))) {
			return true;
		}
		// x = 170: Limgrave's wooden rail fence as measured live (2026-10-31, AEG700_068 by the Church of Elleh: its top 1.26 m up,
		// "nothing to stand on up there" with vault_max_height 1.2): log rails at y [0.45, 0.6] and [1.05, 1.26], 0.12 m deep
		if (lane(170.0f) && p.z >= 1.0f && p.z <= 1.12f && ((p.y >= 0.45f && p.y <= 0.6f) || (p.y >= 1.05f && p.y <= 1.26f))) {
			return true;
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

	// Elden Ring's own rays never hit from inside a shape (a ray starting under a rock's surface sees nothing of it): the same
	// world with that rule, so no plan relies on a hit at a ray's origin.
	bool MantleRayEr(const Vec3& from, const Vec3& delta, Vec3& hit)
	{
		if (MantleSolid(from)) {
			// march out of the solid first; a hit only on entering another one
			constexpr int kSteps = 600;
			bool          out = false;
			Vec3          prev = from;
			for (int i = 1; i <= kSteps; ++i) {
				const Vec3 p = from + delta * (static_cast<float>(i) / kSteps);
				const bool solid = MantleSolid(p);
				if (!out && !solid) {
					out = true;
				} else if (out && solid) {
					hit = prev;
					return true;
				}
				prev = p;
			}
			return false;
		}
		return MantleRay(from, delta, hit);
	}

	// Snake stands at a_x, z 0.55 (his radius 0.35 + 0.1 from the face), facing +z.
	MantlePlan Plan(float a_x, bool a_vault = true, float a_body = 1.8f, const MantleParams& a_m = MantleParams{})
	{
		KinematicParams params;
		params.bodyHeight = a_body;
		return PlanMantle({ a_x, 0.0f, 0.55f }, { 0.0f, 0.0f, 1.0f }, a_body, a_m, params, &MantleRay, a_vault);
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
	for (float x = 0.0f; x <= 170.0f; x += 10.0f) {
		const auto p = Plan(x);
		std::printf("mantle lane %3.0f: %-10s %-52s h %.2f face %.2f depth %.2f %.2f s\n", x, std::string(Name(p.kind)).c_str(), p.why, p.height, p.face,
			p.depth, p.duration);
	}
	{  // a 1 m ledge: climbed onto, ~2 s (MGSV's full climb-up onto its crate / drum since 0.6.0: ~2 s from its Y, any height), ending
	   // on its top clear of the edge
		const auto p = Plan(0.0f);
		CHECK(p.kind == MantleKind::kClimb && p.ledge);
		CHECK(Near(p.height, 1.0f, 0.02f) && Near(p.face, 0.45f, 0.03f));
		CHECK(p.duration > 1.8f && p.duration < 2.2f);
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
	{  // rubble stacked in steps: onto the higher step behind the first (the first has no room for his body), the path clear
		const auto p = Plan(120.0f);
		CHECK(p.kind == MantleKind::kClimb && p.farther >= 1 && Near(p.End().y, 1.3f, 0.02f) && Near(p.height, 1.3f, 0.02f) && PathClear(p));
		// the same first step with a 2.5 m wall behind it: never onto or into the wall
		const auto q = Plan(130.0f);
		CHECK(q.kind == MantleKind::kNone && q.ledge);
		// a rock whose top keeps rising past its lip: climbed, landing on the slope within reach of his climb
		const auto rck = Plan(140.0f);
		CHECK(rck.kind == MantleKind::kClimb && rck.End().y > 0.8f && rck.End().y - 0.0f <= 1.8f && PathClear(rck));
		// a rail fence (two rails, gaps between): found, vaulted over, the path clear of both rails
		const auto fence = Plan(160.0f);
		CHECK(fence.kind == MantleKind::kVault && Near(fence.height, 0.95f, 0.03f) && fence.End().z > 1.1f + 0.3f && PathClear(fence));
		// Limgrave's rail fence (its top 1.26 m): vaulted with the defaults since 0.6.0 (vault_max_height 1.35), and running into it
		// too (run_into_max 1.3); the old 1.2 refused it ("nothing to stand on up there") and a run into it broke it instead
		const auto rail = Plan(170.0f);
		CHECK(rail.kind == MantleKind::kVault && Near(rail.height, 1.26f, 0.03f) && rail.End().z > 1.12f + 0.3f && PathClear(rail));
		CHECK(rail.height <= Config{}.mantleRunIntoMax);
		MantleParams old;
		old.vaultMaxHeight = 1.2f;
		CHECK(Plan(170.0f, true, 1.8f, old).kind == MantleKind::kNone);
		// a rounded boulder whose lip alone is a step: its crown counts, climbed onto
		const auto boulder = Plan(150.0f);
		CHECK(boulder.kind == MantleKind::kClimb && boulder.height >= 0.5f && boulder.End().y > 0.6f && PathClear(boulder));
	}
	{  // every lane with Elden Ring's ray rule (no hit from inside): whatever it plans keeps the body out of everything
		KinematicParams params;
		params.bodyHeight = 1.8f;
		for (float x = 0.0f; x <= 160.0f; x += 10.0f) {
			for (const float yaw : { 0.0f, 30.0f, -45.0f, 65.0f }) {
				const Vec3 dir{ std::sin(yaw * 0.0174533f), 0.0f, std::cos(yaw * 0.0174533f) };
				const auto p = PlanMantle({ x, 0.0f, 0.55f }, dir, 1.8f, MantleParams{}, params, &MantleRayEr, true, 70.0f);
				if (p.kind != MantleKind::kNone && !PathClear(p)) {
					std::printf("  lane %.0f yaw %.0f: %s through something\n", x, yaw, std::string(Name(p.kind)).c_str());
					CHECK(false);
				}
			}
		}
		// the 2.4 m wall, the drop and the rubble-and-wall lanes stay refused with that rule too
		for (const float x : { 20.0f, 40.0f, 130.0f }) {
			CHECK(PlanMantle({ x, 0.0f, 0.55f }, { 0.0f, 0.0f, 1.0f }, 1.8f, MantleParams{}, params, &MantleRayEr, true, 70.0f).kind == MantleKind::kNone);
		}
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
	{  // a slanted approach (live 2026-10-31: running into a ledge at a slant slid along it): the face's normal from three rays, and
	   // a plan square to it when the slanted one is refused -- within the slant allowed, never past it
		KinematicParams params;
		const auto      dirAt = [](float a_deg) { return Vec3{ std::sin(a_deg * 0.0174533f), 0.0f, std::cos(a_deg * 0.0174533f) }; };
		const auto      n = FaceNormal({ 0.0f, 0.0f, 0.55f }, dirAt(30.0f), 1.5f, 0.2f, &MantleRay);
		CHECK(n && Near(*n, Vec3{ 0.0f, 0.0f, -1.0f }, 0.01f));
		CHECK(!FaceNormal({ 80.0f, 0.0f, 0.55f }, dirAt(0.0f), 1.5f, 0.2f, &MantleRay));  // nothing there
		// 70 degrees off the normal, 0.4 m from the face (the slide left him there): the face is 1.17 m away along his way, out of
		// reach -- squared up (allowed up to 75) it is a plain climb, straight onto the top
		const Vec3 at{ 0.0f, 0.0f, 0.6f };
		const auto straight = PlanMantle(at, dirAt(70.0f), 1.8f, MantleParams{}, params, &MantleRay, true, 0.0f);
		CHECK(straight.kind == MantleKind::kNone && !straight.squared && Near(straight.slant, 70.0f, 2.0f));  // a_maxSlantDeg 0: never squared
		const auto squared = PlanMantle(at, dirAt(70.0f), 1.8f, MantleParams{}, params, &MantleRay, true, 75.0f);
		CHECK(squared.kind == MantleKind::kClimb && squared.squared && Near(squared.slant, 70.0f, 2.0f) && Near(squared.dir, Vec3{ 0.0f, 0.0f, 1.0f }, 0.01f));
		CHECK(Near(squared.End().x, 0.0f, 0.02f) && squared.End().z > 1.3f && PathClear(squared));
		// past the slant allowed (running along it): no climb
		CHECK(PlanMantle(at, dirAt(70.0f), 1.8f, MantleParams{}, params, &MantleRay, true, 50.0f).kind == MantleKind::kNone);
		// the fence lane at 60 degrees: vaulted square over it, down on the far side
		const auto fence = PlanMantle({ 30.0f, 0.0f, 0.6f }, dirAt(-60.0f), 1.8f, MantleParams{}, params, &MantleRay, true, 75.0f);
		CHECK(fence.kind == MantleKind::kVault && Near(fence.dir, Vec3{ 0.0f, 0.0f, 1.0f }, 0.01f) && Near(fence.End().x, 30.0f, 0.02f) && PathClear(fence));
		// straight on: nothing changes (no squaring needed, slant ~0)
		const auto on = PlanMantle({ 0.0f, 0.0f, 0.55f }, dirAt(0.0f), 1.8f, MantleParams{}, params, &MantleRay, true, 75.0f);
		CHECK(on.kind == MantleKind::kClimb && !on.squared && on.slant < 5.0f);
		// a refusal says where it was blocked: the ceiling over him at lane 60
		const auto low = PlanMantle({ 60.0f, 0.0f, 0.55f }, dirAt(0.0f), 1.8f, MantleParams{}, params, &MantleRay, true, 75.0f);
		CHECK(low.kind == MantleKind::kNone && low.hasBlock && Near(low.block.y, 1.7f, 0.03f));
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

// The way Snake's body is drawn while MGSV plays a staged move (er/StagedFacing.h; review 2026-10-31: a climb-up drawn sideways,
// a ladder's yaw kept in the delta): eased in to the move's heading, held whatever MGSV's own facing there is, eased back to his own.
static void TestStagedFacing()
{
	using phantom::er::StagedFacing;
	constexpr float kPi = std::numbers::pi_v<float>;
	StagedFacing    f;
	CHECK(!f.On() && !f.Heading(1000, 0.3f));
	// A climb-up squared to the face: he faced 0.2 rad, the plan goes at -0.15 rad.
	f.Begin(0.2f, -0.15f, 1000);
	CHECK(f.On() && !f.Ending());
	CHECK(Near(*f.Heading(1000, 9.0f), 0.2f, 1e-4f));                      // eased in from the heading he had
	const float mid = *f.Heading(1000 + 75, 9.0f);
	CHECK(mid < 0.2f && mid > -0.15f);
	CHECK(Near(*f.Heading(1000 + 150, 9.0f), -0.15f, 1e-4f) && Near(*f.Heading(5000, 1.0f), -0.15f, 1e-4f));  // held: a_own doesn't matter
	// Back home: eased to his own heading (MGSV facing + delta), then over.
	f.End(6000);
	CHECK(f.Ending() && Near(*f.Heading(6000, 0.2f), -0.15f, 1e-4f));
	const float back = *f.Heading(6075, 0.2f);
	CHECK(back > -0.15f && back < 0.2f);
	CHECK(!f.Heading(6150, 0.2f));
	f.End(7000);  // once
	CHECK(!f.Heading(6150, 0.2f));
	// Across the seam at +-pi: the short way round.
	f.Begin(kPi - 0.1f, -kPi + 0.1f, 0);
	const float across = *f.Heading(75, 0.0f);
	CHECK(std::fabs(across) > kPi - 0.11f);
	// The draw anchor it makes: whatever yaw MGSV's list has him at, the body faces the held heading (delta = heading - list yaw).
	phantom::er::Axes axes;
	for (const float listYaw : { 0.0f, 1.0f, -2.5f }) {
		const float want = 0.7f;
		const float delta = WrapPi(want - MgsvHeading(listYaw, axes));
		CHECK(Near(WrapPi(MgsvHeading(listYaw, axes) + delta), want, 1e-4f));
	}
	f.Reset();
	CHECK(!f.On() && !f.Heading(0, 0.0f));
}
