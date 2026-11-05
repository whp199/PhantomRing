// The grace rest with the buddy (er/GraceLogic.h, docs/research/quiet.md §3.9, §7): the InGame fade plate's alpha (the numbers read
// live on 2026-11-04), U12's black watch, the gate (no seat without an observed black; a "talk" rest publishes nothing and moves
// nothing; too late; the black ending first; the echo inside the black or hidden; a dialog; a re-rest), the grace's centre, and the
// seat search on the study's three surveyed graces (tests/fixtures/quiet/survey_*.json: the recorded verdicts answer the rays) and on
// the raw rays the host recorded live (grace_rays_*.json, replayed ray for ray). Included by tests/test_host_combat.cpp.
#include "GraceLogic.h"

namespace gracetest
{
	using namespace phantom::er::grace;

	inline Plate Fade(float a_fromA, float a_toA, float a_left, float a_len, float a_r = 0.0f)
	{
		Plate p;
		p.from[0] = a_r;
		p.from[3] = a_fromA;
		p.to[3] = a_toA;
		p.left = a_left;
		p.length = a_len;
		return p;
	}

	// The study's verdicts as a ray oracle: every ray the seat rule casts is answered from the recorded spot it belongs to.
	struct SurveyOracle
	{
		struct S
		{
			float x, z, h, spread;
			bool  headroom, walls, reach, cam, fail;
		};
		std::vector<S> spots;
		Vec3           centre;
		float          floorY{ 0 };
		Vec3           cam;
		int            rays{ 0 };

		const S* Near(float a_x, float a_z, float a_max) const
		{
			const S* best = nullptr;
			float    bd = a_max;
			for (const auto& s : spots) {
				const float d = std::hypot(s.x - a_x, s.z - a_z);
				if (d < bd) {
					bd = d;
					best = &s;
				}
			}
			return best;
		}

		std::optional<Hit> operator()(const Vec3& a_f, const Vec3& a_d, Ray a_k)
		{
			++rays;
			const Vec3 to = a_f + a_d;
			if (a_k == Ray::kGround) {
				// the floor ray (on the spot) or a footprint ray (0.35 m off it)
				const auto* s = Near(a_f.x, a_f.z, 0.6f);
				if (!s || s->fail) {
					return std::nullopt;
				}
				const float off = std::hypot(s->x - a_f.x, s->z - a_f.z);
				float       y = s->h;
				if (off > 0.2f) {
					// +x half the spread up, -x half down, the z ones on the floor: the recorded spread exactly
					y += (a_f.x > s->x + 0.2f ? 0.5f : a_f.x < s->x - 0.2f ? -0.5f : 0.0f) * s->spread;
				}
				return Hit{ { a_f.x, y, a_f.z } };
			}
			// world rays
			if (Length(a_f - cam) < 0.05f) {
				const auto* s = Near(to.x, to.z, 0.2f);
				return s && s->cam ? std::optional<Hit>(Hit{ a_f + a_d * 0.5f }) : std::nullopt;
			}
			if (LengthXZ(a_f - centre) < 0.05f) {
				const auto* s = Near(to.x, to.z, 0.2f);
				return s && s->reach ? std::optional<Hit>(Hit{ a_f + a_d * 0.5f }) : std::nullopt;
			}
			const auto* s = Near(a_f.x, a_f.z, 0.2f);
			if (!s) {
				return std::nullopt;
			}
			if (a_d.y > 0.5f) {
				return s->headroom ? std::optional<Hit>(Hit{ a_f + a_d * 0.5f }) : std::nullopt;
			}
			// knee rays: the study's 8 at 0.5 m (any hit refuses); its 0.25 m ring was never cast (none recorded)
			if (std::fabs(a_f.y - (s->h + 0.5f)) < 0.05f && s->walls) {
				return Hit{ a_f + a_d * 0.5f };
			}
			return std::nullopt;
		}
	};

	struct Survey
	{
		Scene                      scene;
		SurveyOracle               oracle;
		std::string                label;
		std::vector<std::pair<float, float>> spots;  // r, deg
	};

	inline std::optional<Survey> LoadSurvey(const char* a_file)
	{
		const auto j = jsontest::Load(jsontest::g_fixtures / a_file);
		if (!j) {
			return std::nullopt;
		}
		Survey s;
		const auto* g = j->Get("G");
		const auto* sn = j->Get("S");
		const auto* cam = j->Get("cam");
		if (!g || !sn || !cam) {
			return std::nullopt;
		}
		s.scene.centre = jsontest::V3(g);
		s.scene.floorY = static_cast<float>(j->Num("gfloor"));
		s.scene.snake = { static_cast<float>(sn->arr[0].num), s.scene.floorY, static_cast<float>(sn->arr[1].num) };
		s.scene.view.pos = jsontest::V3(cam->Get("pos"));
		s.scene.view.fwd = Normalize(jsontest::V3(cam->Get("fwd")));
		s.scene.view.right = Normalize(jsontest::V3(cam->Get("right")));
		s.scene.view.up = Normalize(jsontest::V3(cam->Get("up")));
		s.scene.view.fovY = static_cast<float>(j->Num("fov", 1.0472));
		s.scene.view.aspect = 16.0f / 9.0f;
		s.scene.view.valid = true;
		s.oracle.centre = s.scene.centre;
		s.oracle.floorY = s.scene.floorY;
		s.oracle.cam = s.scene.view.pos;
		for (const auto& sp : j->Get("spots")->arr) {
			SurveyOracle::S x{};
			x.x = static_cast<float>(sp.Num("x"));
			x.z = static_cast<float>(sp.Num("z"));
			x.fail = sp.Has("fail");
			if (!x.fail) {
				const auto* gf = sp.Get("h_groundfilter");
				x.h = gf && gf->type == jsontest::Value::Type::kNum ? static_cast<float>(gf->num) : static_cast<float>(sp.Num("h"));
				x.spread = static_cast<float>(sp.Num("foot_spread", 9.0));
				x.headroom = sp.Bool("headroom_hit");
				x.walls = sp.Num("walls8") > 0;
				x.reach = sp.Bool("reach_blocked");
				x.cam = sp.Bool("cam_los_blocked");
			}
			s.oracle.spots.push_back(x);
			s.spots.emplace_back(static_cast<float>(sp.Num("r")), static_cast<float>(sp.Num("deg")));
		}
		s.label = a_file;
		return s;
	}

	// The raw rays the host recorded (`grace survey`, `grace record on`): each ray answered by the recorded one nearest to it.
	struct RecordedOracle
	{
		struct R
		{
			Vec3 f, d;
			int  k;
			bool hit;
			Vec3 h;
		};
		std::vector<R> rays;
		int            misses{ 0 };
		std::optional<Hit> operator()(const Vec3& a_f, const Vec3& a_d, Ray a_k)
		{
			const int k = a_k == Ray::kGround ? 0 : 1;
			const R*  best = nullptr;
			float     bd = 0.03f;
			for (const auto& r : rays) {
				if (r.k != k) {
					continue;
				}
				const float e = Length(r.f - a_f) + Length(r.d - a_d);
				if (e < bd) {
					bd = e;
					best = &r;
				}
			}
			if (!best) {
				++misses;
				return std::nullopt;
			}
			return best->hit ? std::optional<Hit>(Hit{ best->h }) : std::nullopt;
		}
	};
}

static void TestGraceFade()
{
	using namespace gracetest;
	// The InGame plate as read live at Rampart Tower (2026-11-04): the fade-out (from alpha 0 to 1, 0.75 s), the black, the fade-in
	// (from 1 to 0, 0.5 s), idle; the luminance was 0.0 exactly while alpha >= 0.99.
	CHECK(Near(FadeAlpha(Fade(0, 1, 0.6f, 0.75f)), 0.2f));
	CHECK(Near(FadeAlpha(Fade(0, 1, 0.033f, 0.75f)), 0.956f));
	CHECK(!PlateDark(Fade(0, 1, 0.033f, 0.75f)));
	CHECK(Near(FadeAlpha(Fade(0, 1, 0.0f, 0.75f)), 1.0f) && PlateDark(Fade(0, 1, 0.0f, 0.75f)));
	CHECK(Near(FadeAlpha(Fade(1, 0, 0.45f, 0.5f)), 0.9f) && !PlateDark(Fade(1, 0, 0.45f, 0.5f)));
	CHECK(Near(FadeAlpha(Fade(1, 0, 0.0f, 0.5f)), 0.0f));                 // idle after a fade-in
	CHECK(!PlateDark(Fade(0, 0, 0, 0.5f, 1.0f)));                         // InGameBokeh's from (1, 0, 0, 0): not black
	Plate red = Fade(0, 1, 0, 0.75f);
	red.to[0] = 1.0f;                                                     // a red fade at full alpha: not a black screen
	CHECK(!PlateDark(red));
	CHECK(std::isnan(FadeAlpha(Fade(0, 1, 0.1f, -3.0f))) && std::isnan(FadeAlpha(Fade(7, 1, 0, 0.5f))));  // not the verified layout
	Plate nan = Fade(0, 1, 0, 0.75f);
	nan.left = NAN;
	CHECK(std::isnan(FadeAlpha(nan)) && !PlateDark(nan));

	// U12's watch: one rest, the black +1550 .. +2960 ms after the press (Rampart Tower's re-rest).
	BlackWatch w;
	w.Press(10000);
	bool said = false;
	for (std::uint64_t t = 10000; t < 14500; t += 16) {
		const bool dark = t >= 11550 && t < 12960;
		said = w.Update(t, dark) || said;
		if (said) {
			break;
		}
	}
	CHECK(said && w.StartMs() >= 1550 && w.StartMs() < 1570 && w.EndMs() >= 2960 && w.EndMs() < 2980 && w.Blacks() == 1);
	CHECK(!w.Active());  // reported: a later black at the grace (Pass time) is the host's "another black", not this rest's
	BlackWatch talk;  // a talk: no black at all -> reported once the window closes
	talk.Press(1000);
	bool told = false;
	for (std::uint64_t t = 1000; t < 1000 + BlackWatch::kWindowMs + 100; t += 50) {
		told = talk.Update(t, false) || told;
	}
	CHECK(told && talk.StartMs() < 0);
}

static void TestGraceGate()
{
	using namespace gracetest;
	using E = Gate::Event;
	using P = Gate::Phase;
	const Rules r;  // wait 2000 ms for the black, the seat by 750 ms into it
	Gate::In    in;
	in.atGrace = true;
	auto step = [&](Gate& a_g, std::uint64_t a_t, bool a_dark, bool a_echo = false, bool a_dialog = false) {
		in.nowMs = a_t;
		in.dark = a_dark;
		in.echo = a_echo;
		in.dialog = a_dialog;
		return a_g.Update(in, r);
	};

	// The usual rest: armed at the press, nothing before the black; the black at +1050 ms starts the search; the seat ready 120 ms
	// later is published (hidden until the echo); the echo inside the black shows her; she stays through the menu (light again).
	Gate g;
	g.Rest(1000);
	CHECK(g.Current() == P::kArmed && !g.SeatPublished() && !g.Hide() && !g.AtSeat());
	CHECK(step(g, 1500, false) == E::kNone && step(g, 2049, false) == E::kNone);
	CHECK(step(g, 2050, true) == E::kBlack && g.Searching() && g.BlackMs() == 2050 && !g.Hide());
	CHECK(g.SearchDone(2170, true, true, r) == E::kPublish && g.SeatPublished() && g.Hide() && !g.AtSeat());
	CHECK(g.ReadyForReload(r) == false);  // 120 ms > seat_reload_ready_ms 100
	CHECK(step(g, 2200, true, false) == E::kNone && g.Hide());
	CHECK(step(g, 2216, true, true) == E::kShown && g.AtSeat() && !g.Hide());
	CHECK(step(g, 3600, false, true) == E::kNone && g.AtSeat());  // the menu: she stays
	CHECK(step(g, 9000, true, true) == E::kNone && g.AtSeat());   // Pass time's black: she stays
	g.Rest(9500);                                                  // a re-rest at the same grace: she stays where she sits
	CHECK(g.AtSeat() && g.Current() == P::kShown);
	in.atGrace = false;
	CHECK(step(g, 12000, false) == E::kEnded && g.Current() == P::kIdle && !g.AtSeat() && !g.Hide());
	in.atGrace = true;

	// A rest that turns into a talk (Melina, Castleward Tunnel: the HUD up, no black): no seat, nothing moves -- the safety rule
	// the live tests can't try.
	Gate t;
	t.Rest(1000);
	for (std::uint64_t ms = 1000; ms <= 3000; ms += 16) {
		CHECK(step(t, ms, false) == E::kNone);
	}
	CHECK(step(t, 3001, false) == E::kNoBlack && t.Current() == P::kNoSeat && !t.SeatPublished() && !t.Hide() && !t.AtSeat());
	CHECK(step(t, 3100, false, false, true) == E::kNone && !t.Hide());  // the dialog that follows: she was never moved, nothing to hide
	CHECK(step(t, 3500, true) == E::kNone && !t.SeatPublished());       // a black much later is not this rest's
	Gate t2;                                                             // the dialog comes first (before any black): cancelled, nothing published
	t2.Rest(1000);
	CHECK(step(t2, 1800, false, false, true) == E::kCancelled && !t2.SeatPublished() && t2.Hide());
	CHECK(step(t2, 1900, false, false, false) == E::kNone && !t2.Hide());

	// The black after seat_black_wait_ms (2000 ms): no seat. A black at +1.55 s (the Tarnished turned to the grace first, live) is in time.
	Gate late;
	late.Rest(1000);
	CHECK(step(late, 3001, false) == E::kNoBlack && step(late, 3100, true) == E::kNone && !late.SeatPublished());
	Gate turned;
	turned.Rest(1000);
	CHECK(step(turned, 2400, false) == E::kNone && step(turned, 2560, true) == E::kBlack);

	// The search past the deadline (750 ms after the black's start): dropped, by the deadline or when it finishes.
	Gate slow;
	slow.Rest(1000);
	CHECK(step(slow, 2000, true) == E::kBlack);
	CHECK(step(slow, 2750, true) == E::kNone && step(slow, 2751, true) == E::kTooLate && !slow.SeatPublished());
	Gate slow3;
	slow3.Rest(1000);
	CHECK(step(slow3, 2000, true) == E::kBlack && slow3.SearchDone(2800, true, true, r) == E::kTooLate && !slow3.SeatPublished());
	Gate fast;  // ready within seat_reload_ready_ms: a reload placement could still fit
	fast.Rest(1000);
	CHECK(step(fast, 2000, true) == E::kBlack && fast.SearchDone(2080, true, true, r) == E::kPublish && fast.ReadyForReload(r));

	// The black ended before the seat was ready; no seat found.
	Gate light;
	light.Rest(1000);
	CHECK(step(light, 2000, true) == E::kBlack && step(light, 2100, false) == E::kLight && !light.SeatPublished());
	Gate none;
	none.Rest(1000);
	CHECK(step(none, 2000, true) == E::kBlack && none.SearchDone(2100, false, true, r) == E::kNoSpot && !none.SeatPublished() && !none.Hide());

	// (0.9.0 integration) The echo after the black -- the real Quiet settles ~3 s after her order: hidden until it comes, then faded in
	// (her cloak's fade, never a pop-in); none within seat_late_echo_ms: hidden through this rest.
	Gate echo;
	echo.Rest(1000);
	CHECK(step(echo, 2000, true) == E::kBlack && echo.SearchDone(2100, true, true, r) == E::kPublish);
	CHECK(step(echo, 3400, false, false) == E::kNone && echo.Hide() && !echo.AtSeat());
	CHECK(step(echo, 5600, false, true) == E::kShown && echo.AtSeat() && !echo.Hide() && echo.ShownLate());
	CHECK(echo.SeatAlpha(5600, r) == 0.0f && Near(echo.SeatAlpha(5600 + r.lateFadeMs / 2, r), 0.5f) && echo.SeatAlpha(5600 + r.lateFadeMs, r) == 1.0f);
	Gate inBlack;  // an echo inside the black: drawn at once (alpha 1)
	inBlack.Rest(1000);
	step(inBlack, 2000, true);
	inBlack.SearchDone(2100, true, true, r);
	CHECK(step(inBlack, 2400, true, true) == E::kShown && !inBlack.ShownLate() && inBlack.SeatAlpha(2400, r) == 1.0f);
	Gate never;
	never.Rest(1000);
	step(never, 2000, true);
	never.SearchDone(2100, true, true, r);
	CHECK(step(never, 2100 + r.lateEchoMs, false, false) == E::kNone && never.Hide());
	CHECK(step(never, 2101 + r.lateEchoMs, false, false) == E::kLateEcho && never.Hide() && !never.AtSeat());
	CHECK(step(never, 2200 + r.lateEchoMs, false, true) == E::kNone && never.Hide() && !never.AtSeat());

	// A dialog while seated: hidden while it lasts, then her post rule (not hidden, not at the seat).
	Gate d;
	d.Rest(1000);
	step(d, 2000, true);
	d.SearchDone(2100, true, true, r);
	CHECK(step(d, 2150, true, true) == E::kShown);
	CHECK(step(d, 5000, false, true, true) == E::kCancelled && d.Hide() && !d.AtSeat() && !d.SeatPublished());
	CHECK(step(d, 6000, false, false, false) == E::kNone && !d.Hide() && !d.AtSeat());

	// A respawn never arms (the host calls Rest only on kHEvRested 0): an idle gate does nothing whatever the screen does.
	Gate idle;
	CHECK(step(idle, 1000, true, true) == E::kNone && !idle.SeatPublished() && !idle.Hide() && !idle.AtSeat());
}

static void TestGraceCentre()
{
	using namespace gracetest;
	const Vec3        player{ 0, 2.45f, 0 };
	std::vector<Vec3> assets{ { 1.15f, 2.45f, 0 }, { 0.5f, 30.0f, 0.5f }, { 2.0f, 2.45f, 2.0f }, { 4.0f, 2.45f, 0 } };
	const auto        c = NearestCentre(assets, player);
	CHECK(c && Near(c->x, 1.15f));  // the one 1.15 m away; not the one 27 m up, not the one 2.8 m away
	std::vector<Vec3> far{ { 3.0f, 2.45f, 0 } };
	CHECK(!NearestCentre(far, player));
	const auto f = FacingCentre(player, { 0, 0, -1 });
	CHECK(Near(f.x, 0) && Near(f.z, -1.25f) && Near(f.y, 2.45f));
}

static void TestGraceSeatRules()
{
	using namespace gracetest;
	const Rules r;
	// A flat open floor, the camera 5 m behind Snake looking at the grace: the first candidate in order passes and is scored 2 m out,
	// right of Snake on screen.
	Scene s;
	s.centre = { 0, 0, 0 };
	s.floorY = 0;
	s.snake = { 0, 0, -1.2f };
	s.view.pos = { 0, 1.55f, -6.3f };
	s.view.fwd = Normalize(Vec3{ 0, -0.05f, 1 });
	s.view.right = Normalize(Cross(Vec3{ 0, 1, 0 }, s.view.fwd));
	s.view.up = Normalize(Cross(s.view.fwd, s.view.right));
	s.view.fovY = 1.0472f;
	s.view.valid = true;
	int  flatRays = 0;
	auto flat = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) -> std::optional<Hit> {
		++flatRays;
		if (a_k == Ray::kGround && a_d.y < 0 && a_f.y > 0 && a_f.y + a_d.y < 0) {
			return Hit{ { a_f.x, 0, a_f.z }, { 0, 1, 0 } };
		}
		return std::nullopt;
	};
	Search search;
	search.Start(s, r);
	while (!search.Step(36, flat)) {
	}
	const auto& seat = search.Result();
	CHECK(seat && seat->why == Why::kOk && !seat->fallback);
	CHECK(seat && Near(seat->r, 2.0f) && seat->screenX >= r.menuX && LengthXZ(seat->feet - s.snake) >= r.snakeClear);
	CHECK(seat && search.Evaluated() < 10 && search.Rays() < 250);  // the ordered search stops early (quiet.md §3.9)
	// She faces the grace (v1b's watch turns her out on the guest's side).
	CHECK(seat && std::fabs(WrapPi(seat->heading - Heading(s.centre - seat->feet))) < 1e-3f);

	// Each hard rule on its own, on one candidate right of Snake.
	Spot c;
	c.r = 2.0f;
	c.deg = 0;
	c.feet = { 2.0f, 0, 0 };
	CHECK(Evaluate(c, s, flat, r).why == Why::kOk);
	Spot onSnake = c;
	onSnake.feet = { 0.3f, 0, -1.0f };
	CHECK(Evaluate(onSnake, s, flat, r).why == Why::kOnSnake);
	Scene withVarre = s;
	withVarre.others.push_back({ 2.5f, 0, 1.0f });  // a character 1.1 m from the seat
	CHECK(Evaluate(c, withVarre, flat, r).why == Why::kCharacter);
	Spot left = c;
	left.feet = { -3.0f, 0, 0.5f };  // left of Snake on screen: under the grace menu
	CHECK(Evaluate(left, s, flat, r).why == Why::kUnderMenu);
	Spot behind = c;
	behind.feet = { 0, 0, -8.0f };
	CHECK(Evaluate(behind, s, flat, r).why == Why::kBehindView);
	Spot covers = c;
	covers.feet = { 0.02f, 0, -2.5f };  // between the camera and Snake, in front of his silhouette
	CHECK(Evaluate(covers, s, flat, r).why == Why::kCoversSnake || Evaluate(covers, s, flat, r).why == Why::kOnSnake);
	Spot hidden = c;
	hidden.feet = { 0.02f, 0, 2.0f };  // straight behind Snake's legs from the camera
	CHECK(Evaluate(hidden, s, flat, r).why == Why::kBehindSnake);
	// Between the camera and Snake, clear of his silhouette on screen but within the renderer's occluder fade (1 m of the camera ->
	// Snake line + her body): she would be drawn at 15 % (live, Rampart Tower with the camera turned 90 deg) -- refused.
	Spot faded = c;
	faded.feet = { 0.9f, 0, -3.5f };
	CHECK(Evaluate(faded, s, flat, r).why == Why::kCoversSnake);
	Rules noOccluder = r;
	noOccluder.occluderClear = 0.0f;
	CHECK(Evaluate(faded, s, flat, noOccluder).why == Why::kOk);
	// A 0.4 m stone border at the seat's knees escapes the 0.5 m knee rays: the 0.25 m ring catches it (the study's Gatefront gap).
	auto border = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) -> std::optional<Hit> {
		if (a_k == Ray::kWorld && std::fabs(a_d.y) < 1e-3f && a_f.y < 0.4f && a_f.x > 1.0f) {
			return Hit{ a_f + a_d * 0.5f };
		}
		return flat(a_f, a_d, a_k);
	};
	CHECK(Evaluate(c, s, border, r).why == Why::kKnee);
	// Indoors the ray filter hits a loose plank 0.08 m over the floor; the seat's height comes from the ground filter (the floor).
	auto plank = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) -> std::optional<Hit> {
		if (a_k == Ray::kWorld && a_d.y < 0 && std::fabs(a_d.x) < 1e-3f && std::fabs(a_d.z) < 1e-3f) {
			return Hit{ { a_f.x, 0.08f, a_f.z } };  // straight down: the plank
		}
		return flat(a_f, a_d, a_k);
	};
	const auto onPlank = Evaluate(c, s, plank, r);
	CHECK(onPlank.why == Why::kOk && Near(onPlank.feet.y, 0.0f));
	auto step = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) -> std::optional<Hit> {
		if (a_k == Ray::kGround && a_d.y < 0 && a_f.x > 1.0f) {
			return Hit{ { a_f.x, 0.45f, a_f.z } };  // a 0.45 m step up from the grace's floor
		}
		return flat(a_f, a_d, a_k);
	};
	CHECK(Evaluate(c, s, step, r).why == Why::kStep);
	auto noRoof = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) -> std::optional<Hit> {
		if (a_k == Ray::kWorld && a_d.y > 0.5f) {
			return Hit{ a_f + Vec3{ 0, 0.9f, 0 } };  // a beam 0.9 m over the seat
		}
		return flat(a_f, a_d, a_k);
	};
	CHECK(Evaluate(c, s, noRoof, r).why == Why::kHeadroom);
	auto blocked = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) -> std::optional<Hit> {
		if (a_k == Ray::kWorld && Length(a_f - s.view.pos) < 0.01f) {
			return Hit{ a_f + a_d * 0.4f };  // a pillar between the camera and the seat
		}
		return flat(a_f, a_d, a_k);
	};
	CHECK(Evaluate(c, s, blocked, r).why == Why::kViewBlocked);
	// Nothing passes in view (a cramped corner, the camera turned away): the fallback, 2 m opposite Snake, floor and knees only.
	Scene away = s;
	away.view.fwd = Vec3{ 0, 0, -1 };
	away.view.pos = { 0, 1.55f, 5.0f };
	away.view.right = Normalize(Cross(Vec3{ 0, 1, 0 }, away.view.fwd));
	away.view.up = Normalize(Cross(away.view.fwd, away.view.right));
	away.view.pos = { 0, 1.55f, -30.0f };  // behind everything: every candidate is behind the camera
	Search fb;
	fb.Start(away, r);
	while (!fb.Step(36, flat)) {
	}
	CHECK(fb.Result() && fb.Result()->fallback && Near(LengthXZ(fb.Result()->feet - away.centre), 2.0f) &&
	      Dot(fb.Result()->feet - away.centre, away.centre - away.snake) > 0);
	// Review (integration 0.9.0): the fallback keeps the character and reach rules too -- a character on the spot, or a thin wall between
	// the grace and it (the knee rays are 0.45 m: a wall 1 m out isn't theirs), and there is no seat.
	{
		const Vec3 spot = fb.Result()->feet;
		Scene      crowded = away;
		crowded.others.push_back(spot + Vec3{ 0.5f, 0, 0 });
		CHECK(Fallback(crowded, flat, r).why == Why::kCharacter);
		auto wall = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) -> std::optional<Hit> {
			if (a_k == Ray::kWorld && std::fabs(a_d.y) < 0.01f && LengthXZ(a_f - away.centre) < 0.01f) {
				return Hit{ a_f + a_d * 0.5f };  // a balustrade 1 m out from the grace, toward the spot
			}
			return flat(a_f, a_d, a_k);
		};
		CHECK(Fallback(away, wall, r).why == Why::kReach);
		CHECK(Fallback(away, flat, r).why == Why::kOk);
	}
	// ... and with no floor anywhere: no seat at all.
	auto void_ = [](const Vec3&, const Vec3&, Ray) -> std::optional<Hit> { return std::nullopt; };
	Search nothing;
	nothing.Start(away, r);
	while (!nothing.Step(36, void_)) {
	}
	CHECK(!nothing.Result() && nothing.Count(Why::kNoFloor) > 0);
	// A 32 m world shift moves everything the search keeps (the drawn seat stays on its spot).
	Search shifted;
	shifted.Start(s, r);
	while (!shifted.Step(36, flat)) {
	}
	const Vec3 before = shifted.Result()->feet;
	shifted.Shift({ 32, -8, 24 });
	CHECK(Near(shifted.Result()->feet.x, before.x + 32) && Near(shifted.Result()->feet.y, before.y - 8) && Near(shifted.CurrentScene().centre.z, 24));
}

static void TestGraceSurveys()
{
	using namespace gracetest;
	// The study's three graces with the real rest camera of each (quiet.md §2.9): the rule picked r 2.0 m @68 deg at Rampart Tower and
	// Gatefront, r 2.0 m @45 deg at The First Step (screen x 0.62-0.63). The recorded verdicts answer the rays; the 0.25 m knee ring was
	// never cast then (it can't refuse here), and the >= 2 m from a character rule has no character recorded within 6 m.
	struct Want
	{
		const char* file;
		float       deg;
	};
	for (const Want w : { Want{ "survey_rampart.json", 67.5f }, Want{ "survey_gatefront.json", 67.5f }, Want{ "survey_firststep_real.json", 45.0f } }) {
		auto s = LoadSurvey(w.file);
		CHECK(s.has_value());
		if (!s) {
			std::printf("  (fixture %s missing in %s)\n", w.file, jsontest::g_fixtures.string().c_str());
			continue;
		}
		const Rules r;
		Search      search;
		search.Start(s->scene, r);
		auto oracle = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) { return s->oracle(a_f, a_d, a_k); };
		while (!search.Step(36, oracle)) {
		}
		const auto& seat = search.Result();
		CHECK(seat && !seat->fallback && Near(seat->r, 2.0f));
		// Within one bearing (22.5 deg) of the study's pick, scored within 0.2 of it, right of the menu, clear of Snake.
		const float dd = seat ? std::fabs(WrapPi((seat->deg - w.deg) * 3.14159265f / 180.0f)) * 180.0f / 3.14159265f : 999.0f;
		CHECK(dd <= 23.0f);
		CHECK(seat && seat->screenX >= r.menuX && seat->screenX <= 1.0f && LengthXZ(seat->feet - s->scene.snake) >= r.snakeClear);
		std::printf("  grace survey %s: seat %.1f m @%.1f deg, screen x %.3f, %.2f m from Snake, score %.3f; %s\n", w.file, seat ? seat->r : 0.0f, seat ? seat->deg : 0.0f,
			seat ? seat->screenX : 0.0f, seat ? LengthXZ(seat->feet - s->scene.snake) : 0.0f, seat ? seat->score : 0.0f, search.Summary().c_str());
	}
}

// The raw rays the host recorded live (`grace survey`, `grace record on` -> PhantomRingGraceRays-N.json, copied to
// tests/fixtures/quiet/grace_rays_*.json): the same search on the same rays must reach the recorded seat.
static void TestGraceRecorded()
{
	using namespace gracetest;
	int files = 0;
	std::error_code ec;
	for (const auto& e : std::filesystem::directory_iterator(jsontest::g_fixtures, ec)) {
		const auto name = e.path().filename().string();
		if (!name.starts_with("grace_rays_") || e.path().extension() != ".json") {
			continue;
		}
		const auto j = jsontest::Load(e.path());
		CHECK(j.has_value());
		if (!j) {
			continue;
		}
		++files;
		Scene s;
		s.centre = jsontest::V3(j->Get("centre"));
		s.floorY = static_cast<float>(j->Num("floorY"));
		s.snake = jsontest::V3(j->Get("snake"));
		const auto* v = j->Get("view");
		s.view.valid = v && v->Bool("valid");
		if (v) {
			s.view.pos = jsontest::V3(v->Get("pos"));
			s.view.fwd = jsontest::V3(v->Get("fwd"));
			s.view.right = jsontest::V3(v->Get("right"));
			s.view.up = jsontest::V3(v->Get("up"));
			s.view.fovY = static_cast<float>(v->Num("fovY", 1.0));
			s.view.aspect = static_cast<float>(v->Num("aspect", 16.0 / 9.0));
		}
		for (const auto& o : j->Get("others")->arr) {
			s.others.push_back(jsontest::V3(&o));
		}
		RecordedOracle oracle;
		for (const auto& r : j->Get("rays")->arr) {
			RecordedOracle::R x;
			x.f = jsontest::V3(r.Get("f"));
			x.d = jsontest::V3(r.Get("d"));
			x.k = static_cast<int>(r.Num("k"));
			x.hit = !r.Get("h")->IsNull();
			if (x.hit) {
				x.h = jsontest::V3(r.Get("h"));
			}
			oracle.rays.push_back(x);
		}
		Search search;
		Rules  rules;
		if (j->Bool("all")) {
			rules.goodEnough = -1e9f;  // `grace survey all`: every candidate was looked at
		}
		search.Start(s, rules);
		auto ray = [&](const Vec3& a_f, const Vec3& a_d, Ray a_k) { return oracle(a_f, a_d, a_k); };
		while (!search.Step(36, ray)) {
		}
		const auto* want = j->Get("result");
		const auto& got = search.Result();
		const bool  wantSeat = want && !want->IsNull();
		CHECK(got.has_value() == wantSeat && oracle.misses == 0);
		if (got && wantSeat) {
			CHECK(Near(got->r, static_cast<float>(want->Num("r")), 0.01f) && Near(got->deg, static_cast<float>(want->Num("deg")), 0.6f) &&
			      got->fallback == want->Bool("fallback"));
		}
		std::printf("  grace rays %s: %s, %d rays answered (%d unmatched)\n", name.c_str(),
			got ? std::format("seat {:.1f} m @{:.0f} deg", got->r, got->deg).c_str() : "no seat", static_cast<int>(oracle.rays.size()), oracle.misses);
	}
	std::printf("  grace: %d recorded ray file(s) replayed\n", files);
}
