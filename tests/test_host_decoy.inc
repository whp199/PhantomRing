// Included by tests/test_host_combat.cpp: the pure rules of Snake's decoys (er/DecoyLogic.h, roadmap 2.8).

#include "DecoyLogic.h"

static void TestDecoyLogic()
{
	using namespace phantom::er::decoy;
	CHECK(VariantOf(397) == Variant::kPlain && VariantOf(399) == Variant::kPlain && VariantOf(402) == Variant::kActive);
	CHECK(VariantOf(405) == Variant::kShock && VariantOf(380) == Variant::kShock && VariantOf(381) == Variant::kShock);
	CHECK(VariantOf(0) == Variant::kPlain);  // unknown: still a decoy that is seen

	const Rules r;
	const Vec3  eye{ 0, 1.6f, 0 }, fwd{ 0, 0, 1 };
	// In front, within range: seen; behind or out of range: not.
	CHECK(Notices(eye, fwd, { 0, 1.1f, 10 }, Variant::kPlain, 1e9f, r) == Notice::kSees);
	CHECK(Notices(eye, fwd, { 0, 1.1f, -10 }, Variant::kPlain, 1e9f, r) == Notice::kNo);
	CHECK(Notices(eye, fwd, { 0, 1.1f, 16 }, Variant::kPlain, 1e9f, r) == Notice::kNo);
	// The cone: 60 deg off is seen at 70, 80 deg off isn't.
	CHECK(Notices(eye, fwd, { 8.66f, 1.1f, 5 }, Variant::kPlain, 1e9f, r) == Notice::kSees);
	CHECK(Notices(eye, fwd, { 9.85f, 1.1f, 1.74f }, Variant::kPlain, 1e9f, r) == Notice::kNo);
	// Only the horizontal facing counts (an enemy looking down still sees it ahead).
	CHECK(Notices(eye, { 0, -5, 1 }, { 0, 1.1f, 10 }, Variant::kPlain, 1e9f, r) == Notice::kSees);
	// An active decoy is heard behind, within active_range; not beyond.
	CHECK(Notices(eye, fwd, { 0, 1.1f, -20 }, Variant::kActive, 1e9f, r) == Notice::kHears);
	CHECK(Notices(eye, fwd, { 0, 1.1f, -26 }, Variant::kActive, 1e9f, r) == Notice::kNo);
	// Busy with Snake: next to him and nearer him than the decoy -> ignored; the decoy nearer than Snake -> noticed.
	CHECK(Notices(eye, fwd, { 0, 1.1f, 10 }, Variant::kPlain, 2.0f, r) == Notice::kNo);
	CHECK(Notices(eye, fwd, { 0, 1.1f, 1.5f }, Variant::kPlain, 3.0f, r) == Notice::kSees);
	CHECK(Notices(eye, fwd, { 0, 1.1f, 10 }, Variant::kPlain, 6.0f, r) == Notice::kSees);
	CHECK(Notices(eye, fwd, { 0, 1.1f, -10 }, Variant::kActive, 2.0f, r) == Notice::kNo);
	// On top of it / degenerate input.
	CHECK(Notices(eye, fwd, eye, Variant::kPlain, 1e9f, r) == Notice::kNo);
	CHECK(Notices(eye, { 0, 1, 0 }, { 0, 1.1f, 10 }, Variant::kPlain, 1e9f, r) == Notice::kNo);

	CHECK(PopsOn(0.0f, r) && PopsOn(1.0f, r) && !PopsOn(1.1f, r));

	// Inflates over 0.8 s from 15 %, stands, collapses over 0.5 s once popped.
	CHECK(Near(Scale(0, -1, r), 0.15f) && Near(Scale(400, -1, r), 0.575f) && Near(Scale(800, -1, r), 1.0f) && Near(Scale(60000, -1, r), 1.0f));
	CHECK(Near(Scale(60000, 0, r), 1.0f) && Near(Scale(60000, 250, r), 0.5f) && Near(Scale(60000, 500, r), 0.0f) && Near(Scale(60000, 9000, r), 0.0f));
	CHECK(!Standing(700, -1, r) && Standing(800, -1, r) && !Standing(5000, 0, r));

	// The figure: a soldier standing on its feet, its head near the top, square to its facing, flattened when collapsed.
	const auto fig = Figure({ 10, 5, 20 }, { 1, 0, 0 }, 1.75f, 1.0f);
	CHECK(fig.size() == 12);
	float top = -1e9f, bottom = 1e9f, minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f;
	for (const auto& b : fig) {
		top = std::max(top, b.pos.y + b.radius);
		bottom = std::min(bottom, b.pos.y - b.radius);
		minX = std::min(minX, b.pos.x), maxX = std::max(maxX, b.pos.x);
		minZ = std::min(minZ, b.pos.z), maxZ = std::max(maxZ, b.pos.z);
	}
	CHECK(top > 5 + 1.6f && top <= 5 + 1.8f && bottom >= 5.0f - 0.01f && bottom < 5.1f);
	CHECK(Near(minX, 10.0f) && Near(maxX, 10.0f) && maxZ - minZ > 0.4f);  // facing +x: its shoulders spread along z
	const auto flat = Figure({ 0, 0, 0 }, { 0, 0, 1 }, 1.75f, 0.2f);
	float flatTop = 0;
	for (const auto& b : flat) {
		flatTop = std::max(flatTop, b.pos.y);
	}
	CHECK(flat.size() == 12 && flatTop < 0.4f);
	CHECK(Figure({ 0, 0, 0 }, { 0, 0, 1 }, 1.75f, 0.0f).empty() && Figure({ 0, 0, 0 }, { 0, 0, 0 }, 1.75f, 1.0f).size() == 12);
	CHECK(Near(Chest({ 1, 2, 3 }, 2.0f).y, 3.3f));
}
