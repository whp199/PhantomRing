// Included by tests/test_host.cpp: er/MarkingLogic (binocular marking, protocol v13 kGuestBinoculars) -- the cone around the
// view's centre, the dwell that marks, the line of sight, deaths fading, the caps, and the marker's layout.

#include "MarkingLogic.h"
#include "RenderMarkers.h"

namespace marking_test
{
	using namespace phantom::er::marking;

	constexpr float kDt = 1.0f / 60.0f;

	View Ahead(bool a_binoculars = true) { return View{ { 0, 0, 0 }, { 0, 0, 1 }, 0.8f, a_binoculars }; }

	// Frames of a_seconds; returns everything newly marked.
	std::vector<std::uint64_t> Run(Marks& a_m, const View& a_v, std::span<const Seen> a_seen, float a_seconds, const Rules& a_r, const LineOfSight& a_los = {})
	{
		std::vector<std::uint64_t> all;
		const int                  n = static_cast<int>(std::lround(a_seconds / kDt));
		for (int i = 0; i < n; ++i) {
			for (auto h : a_m.Update(a_v, a_seen, kDt, a_r, a_los)) {
				all.push_back(h);
			}
		}
		return all;
	}
}

static void TestMarking()
{
	using namespace marking_test;
	using phantom::er::Marker;
	const Rules r;
	const float deg = std::numbers::pi_v<float> / 180.0f;

	// The cone: a share of the half FOV, never under min_cone_deg (zoomed far in).
	CHECK(Near(ConeHalfAngle(Ahead(), r), 0.12f * 0.4f));
	CHECK(Near(ConeHalfAngle(View{ {}, { 0, 0, 1 }, 0.05f, true }, r), 1.0f * deg));
	CHECK(Near(ConeHalfAngle(View{ {}, { 0, 0, 1 }, std::nanf(""), true }, r), 0.12f * 0.4f));

	// Off the axis: straight ahead 0, behind pi, a body 1 m to the side at 10 m: the angle less the body's own.
	float dist = 0;
	CHECK(Near(OffAxis(Ahead(), { 0, 0, 30 }, r, &dist), 0.0f) && Near(dist, 30.0f));
	CHECK(Near(OffAxis(Ahead(), { 0, 0, -30 }, r), std::numbers::pi_v<float>));
	CHECK(Near(OffAxis(Ahead(), { 1, 0, 10 }, r), std::atan(0.1f) - std::asin(0.6f / std::sqrt(101.0f)), 1e-4f));
	CHECK(Near(OffAxis(Ahead(), { 0.3f, 0, 0.2f }, r), 0.0f));  // the camera inside him
	// A close one that fills the view is in the cone though his chest point is well off it.
	CHECK(OffAxis(Ahead(), { 0.5f, 0, 2.0f }, r) <= ConeHalfAngle(Ahead(), r));

	// Dwell: half a second of looking marks, once.
	{
		Marks      m;
		const Seen s[] = { { 0x10, { 0, 1.2f, 20 }, true } };
		CHECK(Run(m, Ahead(), s, 0.45f, r).empty() && !m.Marked(0x10) && m.Dwell(0x10) > 0.4f && m.Looking() == 1);
		const auto fresh = Run(m, Ahead(), s, 0.1f, r);
		CHECK(fresh.size() == 1 && fresh[0] == 0x10 && m.Marked(0x10) && m.Count() == 1 && m.Looking() == 0);
		CHECK(Run(m, Ahead(), s, 1.0f, r).empty());  // never twice
		CHECK(Near(m.Opacity(0x10, r.fade), 1.0f) && m.Age(0x10) > 0.9f);
		// The binoculars down: the mark stays; someone not in this frame's list (unloaded) keeps his.
		CHECK(Run(m, Ahead(false), {}, 2.0f, r).empty() && m.Marked(0x10));
	}

	// No binoculars, off the centre, out of range, dead: never marked.
	{
		Marks      m;
		const Seen s[] = {
			{ 1, { 0, 0, 20 }, true },                                       // looked at, but binoculars down
		};
		CHECK(Run(m, Ahead(false), s, 2.0f, r).empty() && m.Looking() == 0);
		const Seen off[] = {
			{ 2, { std::sin(20 * deg) * 30, 0, std::cos(20 * deg) * 30 }, true },  // 20 degrees off
			{ 3, { 0, 0, 200 }, true },                                       // beyond 150 m
			{ 4, { 0, 0, 30 }, false },                                       // dead
		};
		CHECK(Run(m, Ahead(), off, 2.0f, r).empty() && m.Count() == 0 && m.Looking() == 0);
	}

	// Looking away ebbs the dwell twice as fast: 0.3 s looked, 0.2 s away -> nothing left; 0.3 + 0.3 more does not mark.
	{
		Marks      m;
		const Seen s[] = { { 7, { 0, 0, 40 }, true } };
		Run(m, Ahead(), s, 0.3f, r);
		Run(m, Ahead(false), s, 0.2f, r);
		CHECK(m.Looking() == 0 && m.Dwell(7) == 0.0f);
		Run(m, Ahead(), s, 0.3f, r);
		Run(m, Ahead(false), s, 0.05f, r);
		CHECK(Run(m, Ahead(), s, 0.25f, r).empty() && !m.Marked(7));
	}

	// Line of sight: a wall in between -> never; the rays only for the ones in the cone, at most max_rays a frame, nearest the
	// centre first.
	{
		Marks      m;
		const Seen s[] = { { 9, { 0, 0, 25 }, true }, { 10, { 0, 0, -25 }, true } };
		int        calls = 0;
		const auto wall = [&](const Vec3&, const Seen&) {
			++calls;
			return false;
		};
		CHECK(Run(m, Ahead(), s, 2.0f, r, wall).empty() && !m.Marked(9) && calls == 120 && m.Rays() == 120);
		std::vector<Seen> crowd;
		for (int i = 0; i < 10; ++i) {
			// All in the cone (3.2 m off at 60 m is just inside), 100 the farthest off the centre, 109 the nearest.
			crowd.push_back({ static_cast<std::uint64_t>(100 + i), { 0.7f + 0.25f * static_cast<float>(10 - i), 0, 60 }, true });
		}
		std::vector<std::uint64_t> asked;
		const auto clear = [&](const Vec3&, const Seen& a_to) {
			for (const auto& c : crowd) {
				if (Near(c.aim, a_to.aim)) {
					asked.push_back(c.handle);
				}
			}
			return true;
		};
		Marks m2;
		m2.Update(Ahead(), crowd, kDt, r, clear);
		CHECK(asked.size() == r.maxRays && asked.front() == 109 && std::find(asked.begin(), asked.end(), 100) == asked.end());
	}

	// A ray that stops on the body looked at still sees him (review: a troll's 2 m capsule stops Elden Ring's ray 2 m short of
	// a point 1.2 m over his feet, more than the 1 m slack); a wall in front of him doesn't; nothing usable back = clear.
	{
		using phantom::er::combat::ChrCapsule;
		const Vec3 cam{ 0, 1.6f, 0 }, chest{ 0, 1.2f, 30 };
		const auto troll = std::optional{ ChrCapsule({ 0, 0, 30 }, 6.0f, 2.0f) };
		CHECK(RayClear(cam, chest, { 0, 1.25f, 28.0f }, troll));              // on his capsule
		CHECK(!RayClear(cam, chest, { 0, 1.25f, 28.0f }, std::nullopt));     // the same hit, his body unknown: the old rule
		CHECK(!RayClear(cam, chest, { 0, 1.4f, 15.0f }, troll));             // a wall half way
		CHECK(RayClear(cam, chest, { 0, 1.2f, 29.3f }, std::nullopt));       // within kLosSlack of his chest
		CHECK(RayClear(cam, chest, { std::nanf(""), 0, 0 }, std::nullopt));
		// The update hands the target (and his body) to the line of sight.
		Marks      m;
		const Seen s[] = { { 0x30, chest, true, troll } };
		const auto ray = [&](const Vec3& a_from, const Seen& a_t) { return RayClear(a_from, a_t.aim, { 0, 1.25f, 28.0f }, a_t.body); };
		CHECK(Run(m, Ahead(), s, 0.6f, r, ray).size() == 1 && m.Marked(0x30));
		// A big body widens what the cone sees of him: 3 m to the side at 30 m is out for a 0.6 m body, in for a 2 m one.
		const Seen side[] = { { 0x31, { 3, 0, 30 }, true }, { 0x32, { -3, 0, 30 }, true, ChrCapsule({ -3, -1.2f, 30 }, 6.0f, 2.0f) } };
		Marks      m2;
		Run(m2, Ahead(), side, 0.6f, r);
		CHECK(!m2.Marked(0x31) && m2.Marked(0x32));
	}

	// A new world forgets the marks: a load screen of at least kMinLoadMs, or another ChrIns for the Tarnished; a few frames
	// with no player are not a load.
	{
		LoadWatch w;
		CHECK(!w.Update(true, 0x1000, 0) && !w.Update(true, 0x1000, 16));  // the first world: nothing to forget
		CHECK(!w.Update(false, 0, 100) && !w.Update(false, 0, 300) && !w.Update(true, 0x1000, 400));  // 300 ms: a drop-out
		CHECK(!w.Update(false, 0, 1000) && !w.Update(false, 0, 1400) && w.Update(true, 0x1000, 1600));  // 600 ms: a load
		CHECK(!w.Update(true, 0x1000, 1616));                                                            // once
		CHECK(w.Update(true, 0x2000, 1632) && !w.Update(true, 0x2000, 1648));                            // another character
		CHECK(!w.Update(true, 0, 1664) && !w.Update(true, 0x2000, 1680));  // no player for a frame, the same one back
	}

	// Death: a marked one fades over fade seconds, then is forgotten; back alive in between (a rest) keeps him.
	{
		Marks m;
		CHECK(m.Mark(0x20, r) && !m.Mark(0x20, r) && m.Marked(0x20));
		const Seen dead[] = { { 0x20, { 0, 0, 10 }, false } };
		Run(m, Ahead(false), dead, 0.5f, r);
		CHECK(m.Marked(0x20) && m.Opacity(0x20, r.fade) > 0.4f && m.Opacity(0x20, r.fade) < 0.6f);
		const Seen alive[] = { { 0x20, { 0, 0, 10 }, true } };
		Run(m, Ahead(false), alive, kDt, r);
		CHECK(Near(m.Opacity(0x20, r.fade), 1.0f));
		Run(m, Ahead(false), dead, 1.1f, r);
		CHECK(!m.Marked(0x20) && m.Count() == 0 && m.Opacity(0x20, r.fade) == 0.0f && m.Age(0x20) < 0.0f);
	}

	// max_marked: the oldest mark goes.
	{
		Rules small = r;
		small.maxMarked = 2;
		Marks m;
		m.Mark(1, small);
		m.Mark(2, small);
		m.Mark(3, small);
		CHECK(m.Count() == 2 && !m.Marked(1) && m.Marked(2) && m.Marked(3));
		m.Forget(2);
		CHECK(m.Count() == 1 && m.Marked(3));
		m.Clear();
		CHECK(m.Count() == 0);
	}

	// The distance text and its glyphs.
	CHECK(DistanceText(37.4f) == "37m" && DistanceText(37.6f) == "38m" && DistanceText(-3.0f) == "0m" && DistanceText(1e6f) == "9999m");
	CHECK(DistanceText(std::nanf("")).empty());
	CHECK((Glyphs("37m") == std::vector<std::uint8_t>{ 3, 7, 10 }) && Glyphs("x").empty());

	// The marker: a triangle with its tip on the point, the glyphs centred over it, all the same size on screen at any distance,
	// through walls; a fresh mark pops; nothing behind the camera.
	{
		const Camera cam{ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, 0.8f };
		const Style  st;
		std::vector<Marker> out;
		Build(out, cam, { 0, 2, 20 }, "37m", true, 1.0f, 5.0f, st);
		CHECK(out.size() == 4);
		if (out.size() == 4) {
			const float tri = st.size * 2.0f * 20.0f * std::tan(0.4f);
			CHECK(out[0].shape == Marker::Shape::kTriangle && Near(out[0].radius, tri) && Near(out[0].pos, Vec3{ 0, 2 + tri, 20 }));
			CHECK(Near(out[0].color[0], st.hostile[0]) && out[0].slack > 1e5f);
			CHECK(out[1].shape == Marker::Shape::kGlyph && Near(out[1].inner, 3) && Near(out[2].inner, 7) && Near(out[3].inner, 10));
			CHECK(Near(out[2].pos.x, 0.0f) && Near(out[1].pos.x, -out[3].pos.x) && out[1].pos.x < 0 && out[1].pos.y > out[0].pos.y + tri);
			CHECK(Near(out[1].radius, st.textSize * tri) && out[3].slack > 1e5f);
		}
		std::vector<Marker> distant;
		Build(distant, cam, { 0, 2, 100 }, "", false, 1.0f, 5.0f, st);
		CHECK(distant.size() == 1 && !out.empty() && Near(distant[0].radius / 100.0f, out[0].radius / 20.0f) && Near(distant[0].color[0], st.other[0]));
		std::vector<Marker> pop, faded, behind;
		Build(pop, cam, { 0, 2, 20 }, "", true, 1.0f, 0.0f, st);
		Build(faded, cam, { 0, 2, 20 }, "5m", true, 0.5f, 5.0f, st);
		Build(behind, cam, { 0, 2, -20 }, "5m", true, 1.0f, 5.0f, st);
		CHECK(pop.size() == 1 && !out.empty() && Near(pop[0].radius, out[0].radius * 1.6f));
		// The pop is the triangle's alone: the distance stays where it settles (review: it jumped ~1.2 triangles up).
		std::vector<Marker> popText;
		Build(popText, cam, { 0, 2, 20 }, "37m", true, 1.0f, 0.0f, st);
		CHECK(popText.size() == 4 && out.size() == 4);
		if (popText.size() == 4 && out.size() == 4) {
			for (std::size_t i = 1; i < 4; ++i) {
				CHECK(Near(popText[i].pos, out[i].pos) && Near(popText[i].radius, out[i].radius));
			}
		}
		CHECK(faded.size() == 3 && Near(faded[0].color[3], 0.5f) && Near(faded[2].color[3], 0.5f));
		CHECK(behind.empty());
	}
}
