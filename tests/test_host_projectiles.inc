// Included by tests/test_host_combat.cpp: projectile meshes (er/ProjectileLogic.h, er/Projectiles.h) and the muzzle flash's
// carrier row (er/ExplosionLogic.h MakeFlash), roadmap 5.4.

#include "Anchor.h"
#include "ProjectileLogic.h"
#include "Projectiles.h"
#include "RenderScene.h"

namespace projectile_test
{
	using namespace phantom::er::projectile;

	bool NearV(const Vec3& a, const Vec3& b, float eps = 1e-3f) { return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps); }

	Vec3 Col(const float* m, int c) { return { m[c], m[4 + c], m[8 + c] }; }

	template <class T>
	void Put(std::uint8_t* a_row, std::uint32_t a_offset, T a_value)
	{
		std::memcpy(a_row + a_offset, &a_value, sizeof(T));
	}

	// A rigid matrix: the flight basis of a_dir as rotation, a_at as translation.
	void Rigid(const Vec3& a_dir, const Vec3& a_at, float* a_m)
	{
		const Basis b = FlightBasis(a_dir);
		const float m[12] = { b.x.x, b.y.x, b.z.x, a_at.x, b.x.y, b.y.y, b.z.y, a_at.y, b.x.z, b.y.z, b.z.z, a_at.z };
		std::memcpy(a_m, m, sizeof(m));
	}

	proto::DrawRecord Draw(std::uint64_t a_mesh, std::uint32_t a_owner, std::uint32_t a_first)
	{
		proto::DrawRecord d{};
		d.mesh = a_mesh;
		d.owner = a_owner;
		d.firstMatrix = a_first;
		d.matrixCount = 1;
		d.flags = proto::kDrawRigid;
		d.indexCount = 36;
		return d;
	}
}

static void TestProjectiles()
{
	using namespace projectile_test;
	// The flight basis: z along the flight, orthonormal, y up-ish; straight up/down still a frame; zero/NaN -> identity.
	for (const Vec3 d : { Vec3{ 3, 1, 4 }, Vec3{ 0, -1, 0 }, Vec3{ 0, 5, 0.01f }, Vec3{ -1, 0, 0 } }) {
		const Basis b = FlightBasis(d);
		CHECK(NearV(b.z, Normalize(d)) && Near(Dot(b.x, b.y), 0) && Near(Dot(b.x, b.z), 0) && Near(Dot(b.y, b.z), 0) && Near(Length(b.x), 1) &&
		      Near(Length(b.y), 1));
		CHECK(NearV(Cross(b.x, b.y), b.z));  // right-handed
	}
	CHECK(FlightBasis({ 3, 0.5f, 4 }).y.y > 0.9f);
	CHECK(NearV(FlightBasis({ 0, 0, 0 }).z, { 0, 0, 1 }) && NearV(FlightBasis({ NAN, 0, 1 }).x, { 1, 0, 0 }));

	// A rocket of two parts (body at its origin, a fin 0.3 m behind) flying along d0 in MGSV, plus one of Snake's draws.
	const Vec3 d0 = Normalize(Vec3{ 1, 0.2f, 0.5f });
	const Vec3 o{ 2, 1.5f, -1 };
	std::vector<float> mats(36);
	Rigid({ 0, 0, 1 }, { 0, 1, 0 }, mats.data());          // Snake's
	Rigid(d0, o, mats.data() + 12);                         // body
	Rigid(d0, o - d0 * 0.3f, mats.data() + 24);             // fin
	std::vector<proto::DrawRecord> draws = { Draw(7, proto::kOwnerPlayer, 0), Draw(11, proto::kOwnerProjectile, 1), Draw(12, proto::kOwnerProjectile, 2) };
	const auto t = Learn(draws, mats, d0);
	CHECK(t.parts.size() == 2 && t.parts[0].record.mesh == 11 && t.parts[1].record.mesh == 12 && Near(t.extent, 0.3f));
	// Its own frame: identity rotation (it was drawn along its flight), the fin 0.3 m back on -z.
	if (t.parts.size() == 2) {
		const float* l = t.parts[1].local.data();
		CHECK(NearV(Col(l, 0), { 1, 0, 0 }) && NearV(Col(l, 2), { 0, 0, 1 }) && NearV(Col(l, 3), { 0, 0, -0.3f }));
	}
	// Put back where it was: the same matrices. Elsewhere, along another flight: turned with it.
	std::vector<proto::DrawRecord> out;
	std::vector<float>             outM(12, 0.0f);  // something already in the list
	Place(t, o, d0, 0.0f, out, outM);
	CHECK(out.size() == 2 && out[0].firstMatrix == 1 && out[1].firstMatrix == 2 && out[0].owner == proto::kOwnerProjectile && out[1].mesh == 12);
	CHECK(outM.size() == 36);
	for (int i = 0; i < 24 && outM.size() == 36; ++i) {
		CHECK(Near(outM[12 + i], mats[12 + i]));
	}
	const Vec3 d1 = Normalize(Vec3{ -0.2f, -0.6f, 1 });
	const Vec3 p{ 40, 3, 7 };
	out.clear();
	outM.clear();
	Place(t, p, d1 * 25.0f, 0.0f, out, outM);
	CHECK(outM.size() == 24 && NearV(Col(outM.data(), 3), p) && NearV(Col(outM.data(), 2), d1) && NearV(Col(outM.data() + 12, 3), p - d1 * 0.3f));
	// The tumble turns it about its flight axis only: z stays, the fin stays behind.
	out.clear();
	outM.clear();
	Place(t, p, d1, 1.3f, out, outM);
	CHECK(outM.size() == 24 && NearV(Col(outM.data(), 2), d1) && !NearV(Col(outM.data(), 0), FlightBasis(d1).x) && NearV(Col(outM.data() + 12, 3), p - d1 * 0.3f));
	// Not a projectile: too many parts, or wider than one.
	std::vector<proto::DrawRecord> many;
	std::vector<float>             manyM;
	for (std::uint32_t i = 0; i < kMaxParts + 1; ++i) {
		many.push_back(Draw(100 + i, proto::kOwnerProjectile, i));
		manyM.insert(manyM.end(), mats.begin() + 12, mats.begin() + 24);
	}
	CHECK(Learn(many, manyM, d0).parts.empty());
	std::vector<float> wide = mats;
	wide[24 + 3] += 3.0f;
	CHECK(Learn(draws, wide, d0).parts.empty());
	CHECK(Learn(std::vector<proto::DrawRecord>{ Draw(7, proto::kOwnerPlayer, 0) }, mats, d0).parts.empty());
	// A record pointing past the matrices is skipped, not read.
	CHECK(Learn(std::vector<proto::DrawRecord>{ Draw(11, proto::kOwnerProjectile, 9) }, mats, d0).parts.empty());

	// The anchor's inverse: an Elden Ring point / direction back into the list's space (a yaw and an X mirror).
	{
		const float yaw = 0.7f, c = std::cos(yaw), s = std::sin(yaw);
		const Vec3  cx = RotY(yaw, { -1, 0, 0 }), cy{ 0, 1, 0 }, cz = RotY(yaw, { 0, 0, 1 });
		(void)c;
		(void)s;
		const float a[12] = { cx.x, cy.x, cz.x, 100, cx.y, cy.y, cz.y, 20, cx.z, cy.z, cz.z, -50 };
		const Vec3  q{ 1.5f, -2, 3 };
		CHECK(NearV(ToListPoint(a, FromListPoint(a, q)), q) && NearV(FromListPoint(a, ToListPoint(a, q)), q));
		CHECK(NearV(ToListDir(a, FromListPoint(a, q) - FromListPoint(a, {})), q));
		// The same as the puppet's own mapping: ER direction -> MGSV (Anchor::DirToMgsv).
		Anchor an;
		an.delta = yaw;
		an.axes.mirrorX = -1;
		CHECK(NearV(ToListDir(a, { 0.3f, 0.1f, -0.9f }), an.DirToMgsv({ 0.3f, 0.1f, -0.9f })));
	}

	// Which release a list's projectile draws belong to: the newest unlearned one within the window.
	const Pending pend[] = { { 1, 1000, false }, { 2, 1900, false }, { 3, 2000, true }, { 4, 500, false } };
	CHECK(PickRelease(pend, 2100, 1500) == 1);
	CHECK(PickRelease(pend, 3400, 1500) == 1 && PickRelease(pend, 3401, 1500) == -1);
	CHECK(PickRelease(std::span<const Pending>{}, 0, 1500) == -1 && PickRelease(pend, 900, 1500) == 3);  // 1000/1900 are "in the future"

	// Review fixes: a candidate counts only when a later list (3+ MGSV frames on) shows the same meshes flying away along the
	// release -- Snake's gear (still relative to his root, the list's space) and a prop down range never do.
	{
		Template a = t, b = t;
		a.at = o;
		b.at = o + d0 * 1.2f;  // 4 frames later: 18 m/s
		CHECK(Confirms(a, 100, b, 104, d0));
		CHECK(!Confirms(a, 100, a, 104, d0));  // still
		CHECK(!Confirms(a, 100, b, 101, d0) && !Confirms(a, 100, b, 130, d0) && !Confirms(a, 104, b, 100, d0));
		CHECK(!Confirms(a, 100, b, 104, d0 * -1.0f));  // the wrong way
		Template c = b;
		c.parts.pop_back();
		CHECK(!Confirms(a, 100, c, 104, d0));  // other meshes
		CHECK(Signature(b) == std::vector<std::uint64_t>({ 11, 12 }) && Near(Learn(draws, mats, d0).at.x, o.x - d0.x * 0.15f));
	}
	// Its real size from the mesh boxes (review: the origin spread alone read "0.00 m across" for any one-mesh mis-learn).
	{
		Template one;
		one.parts.push_back(t.parts[0]);  // at its own origin, along its flight
		const Box grenade{ true, { -0.04f, -0.04f, -0.06f }, { 0.04f, 0.04f, 0.06f } };
		const Box gb[] = { grenade };
		CHECK(Near(Across(one, gb), std::sqrt(0.08f * 0.08f * 2 + 0.12f * 0.12f)) && Across(one, gb) < kMaxThrownAcross);
		const Box rifle[] = { { true, { -0.05f, -0.1f, -0.2f }, { 0.05f, 0.1f, 0.8f } } };
		CHECK(Across(one, rifle) > kMaxThrownAcross && Across(one, rifle) < kMaxRoundAcross);
		CHECK(Near(Across(t, std::span<const Box>{}), 0.3f));  // no boxes: the part origins only
		const Box two[] = { grenade, grenade };
		CHECK(Across(t, two) > 0.4f);
	}
	// A flown copy's pose: along its velocity while it moves, held when it stops (review: it snapped to world +z, no roll).
	{
		Pose still;
		Advance(still, { 0, 0, 0 }, 0.0f, true, 9.0f);
		CHECK(still.have && NearV(still.heading, { 0, 0, 1 }) && still.spin == 0.0f);  // never seen moving: level
		Pose fly;
		Advance(fly, { 3, 4, 0 }, 0.1f, true, 10.0f);
		Advance(fly, { 3, 2, 0 }, 0.2f, true, 10.0f);
		CHECK(NearV(fly.heading, { 3, 2, 0 }) && Near(fly.spin, 1.0f));
		Advance(fly, { 0.1f, 0, 0.1f }, 0.3f, true, 10.0f);  // landed
		CHECK(NearV(fly.heading, { 3, 2, 0 }) && Near(fly.spin, 1.0f));
		Advance(fly, { 0, 0, 0 }, 0.9f, true, 10.0f);
		Advance(fly, { 0, 3, 3 }, 1.0f, true, 10.0f);  // kicked by a blast: spins on from where it was
		CHECK(NearV(fly.heading, { 0, 3, 3 }) && Near(fly.spin, 2.0f));
		Pose round;
		Advance(round, { 0, 0, 50 }, 0.1f, false, 10.0f);
		Advance(round, { 0, 0, 50 }, 0.2f, false, 10.0f);
		CHECK(round.spin == 0.0f && NearV(round.heading, { 0, 0, 50 }));
	}
	// Off: the guest's tags go back to where it put those draws before v13 (Snake's within 2.5 m of his root, else out).
	{
		const std::vector<proto::DrawRecord> tagged = { Draw(7, proto::kOwnerPlayer, 0), Draw(11, proto::kOwnerProjectile, 1),
			Draw(13, proto::kOwnerProjectile, 0), Draw(14, proto::kOwnerProjectile, 9) };
		auto hb = tagged;
		HandBack(hb, mats, true);
		CHECK(hb.size() == 2 && hb[1].mesh == 13 && hb[1].owner == proto::kOwnerPlayer);
		hb = tagged;
		HandBack(hb, mats, false);  // [Render] draw_player off
		CHECK(hb.size() == 1 && hb[0].mesh == 7);
	}

	// er/Projectiles: a snapshot's tagged draws are learned (on, once they flew) and never drawn where MGSV has them.
	{
		auto&      rc = Cfg().render;
		const bool was = rc.projectileMeshes, wasPlayer = rc.drawPlayer;
		RenderSnapshot s;
		const float id[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
		std::memcpy(s.anchor, id, sizeof(id));
		// The rocket's list, a_ahead m further along its flight, MGSV frame a_frame; a_mesh: other meshes for it.
		auto list = [&](float a_ahead, std::uint64_t a_frame, std::uint64_t a_mesh = 0) {
			s.draws = draws;
			s.matrices = mats;
			for (int m = 1; m <= 2; ++m) {
				s.matrices[m * 12 + 3] += d0.x * a_ahead;
				s.matrices[m * 12 + 7] += d0.y * a_ahead;
				s.matrices[m * 12 + 11] += d0.z * a_ahead;
				if (a_mesh) {
					s.draws[m].mesh = a_mesh + m;
				}
			}
			s.guestFrame = a_frame;
		};
		auto says = [](const char* a_text) { return Projectiles::Describe().find(a_text) != std::string::npos; };

		rc.projectileMeshes = false;
		list(0, 100);
		CHECK(Projectiles::Apply(s) && s.draws.size() == 1 && s.draws[0].owner == proto::kOwnerPlayer);  // 2.7 m out: dropped
		s.draws.push_back(Draw(13, proto::kOwnerProjectile, 0));  // a tagged draw on Snake himself: his again
		CHECK(Projectiles::Apply(s) && s.draws.size() == 2 && s.draws[1].owner == proto::kOwnerPlayer);
		rc.drawPlayer = false;
		s.draws = { draws[0], Draw(13, proto::kOwnerProjectile, 0) };
		CHECK(Projectiles::Apply(s) && s.draws.size() == 1);
		rc.drawPlayer = wasPlayer;
		CHECK(says("0 templates learned") && says("1 handed back"));

		rc.projectileMeshes = true;
		const auto key = Projectiles::KeyOf(Projectiles::Source::kRounds, 59);
		Projectiles::NoteRelease(key, d0);
		list(0, 100);
		CHECK(Projectiles::Apply(s) && s.draws.size() == 1 && says("0 templates learned"));  // one list proves nothing
		list(0.3f, 101);
		CHECK(Projectiles::Apply(s) && says("0 templates learned"));  // a frame on: too little to judge
		list(1.2f, 104);
		CHECK(Projectiles::Apply(s) && s.draws.size() == 1 && says("1 templates learned") && says("first mesh 000000000000000b"));
		// Later rockets of that bullet showing other meshes: once, the first template stays; twice in a row, they take over.
		Projectiles::NoteRelease(key, d0);
		list(0, 200, 20);
		Projectiles::Apply(s);
		list(1.2f, 204, 20);
		Projectiles::Apply(s);
		CHECK(says("first mesh 000000000000000b") && says("0 replaced"));
		Projectiles::NoteRelease(key, d0);
		list(0, 300, 20);
		Projectiles::Apply(s);
		list(1.2f, 304, 20);
		Projectiles::Apply(s);
		CHECK(says("first mesh 0000000000000015") && says("1 replaced"));
		// A release whose tagged draws never move (a prop down range, Snake's gear): never learned.
		Projectiles::NoteRelease(Projectiles::KeyOf(Projectiles::Source::kRounds, 60), d0);
		for (std::uint64_t f = 400; f <= 440; f += 4) {
			list(0, f);
			CHECK(Projectiles::Apply(s) && s.draws.size() == 1);
		}
		CHECK(!says("round bullet 60") && says("1 never flew"));
		// No list from MGSV this frame, and its mesh never reached this test's GPU: nothing placed, so the ball stays.
		Projectiles::Set(Projectiles::Source::kRounds, { { key, 1, { 5, 1, 5 }, { 0, 0, 30 }, 0.2f } });
		s.draws.clear();
		s.matrices.clear();
		s.erFrame = 50;
		CHECK(!Projectiles::Apply(s) && s.draws.empty());
		CHECK(!Projectiles::Drawn(Projectiles::Source::kRounds, 1, 50) && !Projectiles::Drawn(Projectiles::Source::kRounds, 1, 51));
		Projectiles::OnPuppetStop();
		CHECK(Projectiles::KeyOf(Projectiles::Source::kThrow, 5) != Projectiles::KeyOf(Projectiles::Source::kRounds, 5));
		rc.projectileMeshes = was;
	}

	// The muzzle flash's row: one bullet, no speed, short life, its effect cut with it, nothing after it, and no harm.
	{
		std::array<std::uint8_t, params::kBullet.rowSize> row{};
		Put<std::int32_t>(row.data(), params::bullet::kSfxIdBullet, 300122);
		Put<float>(row.data(), params::bullet::kInitVelocity, 25.0f);
		Put<std::uint16_t>(row.data(), params::bullet::kNumShoot, 18);
		Put<std::int32_t>(row.data(), params::bullet::kHitBulletId, 10030002);
		explosion::MakeFlash(row.data(), 0.06f);
		CHECK(Near(At<float>(row.data(), params::bullet::kLife), 0.06f) && At<float>(row.data(), params::bullet::kInitVelocity) == 0.0f &&
		      At<std::uint16_t>(row.data(), params::bullet::kNumShoot) == 1 && At<std::int8_t>(row.data(), params::bullet::kSfxDeleteByLifeDead) == 1 &&
		      At<std::int32_t>(row.data(), params::bullet::kHitBulletId) == -1 && At<std::int32_t>(row.data(), params::bullet::kIntervalCreateBulletId) == -1);
		explosion::MakeFlash(row.data(), 99.0f);
		CHECK(Near(At<float>(row.data(), params::bullet::kLife), 0.5f));
		explosion::MakeFlash(row.data(), NAN);
		CHECK(Near(At<float>(row.data(), params::bullet::kLife), 0.06f));
	}
	std::printf("projectile meshes and the muzzle flash row checked\n");
}
