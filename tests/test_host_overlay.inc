// Included by tests/test_host.cpp: the overlay's decisions (er/Hud.h Brain) and its CPU-made pictures
// (er/OverlayArt.h: GDI text panels under wine, the reticle).
#include "Hud.h"
#include "HudLayout.h"
#include "OverlayArt.h"

#include <cstdlib>

// MGSV's HUD layer next to Elden Ring's (er/HudLayout.h): regions move on their own, the rest stays put, nothing is lost.
static void TestHudLayout()
{
	using namespace phantom::er::HudLayout;
	Settings none;
	auto     one = Layout(3840, 2160, 1280, 720, none);
	CHECK(one.size() == 1 && one[0].x == 0 && one[0].y == 0 && one[0].w == 3840 && one[0].h == 2160 && one[0].u1 == 1 && one[0].v1 == 1);

	const phantom::er::OverlayConfig d;  // the shipped defaults
	Settings s;
	std::copy_n(d.hudWeaponRegion, 4, s.regions[0].uv);
	std::copy_n(d.hudWeaponOffset, 2, s.regions[0].offset);
	std::copy_n(d.hudWeaponBarRegion, 4, s.regions[1].uv);
	std::copy_n(d.hudWeaponOffset, 2, s.regions[1].offset);
	std::copy_n(d.hudNotifyRegion, 4, s.regions[2].uv);
	std::copy_n(d.hudNotifyOffset, 2, s.regions[2].offset);
	for (const float W : { 3840.0f, 1920.0f }) {
		const float H = W * 9 / 16;
		const auto  pieces = Layout(W, H, 1280, 720, s);
		CHECK(pieces.size() > 1 && pieces.size() <= 12);  // the overlay has 32 descriptors a frame
		double area = 0;
		bool   weaponUp = false, centreKept = false;
		for (const auto& p : pieces) {
			area += (p.u1 - p.u0) * (p.v1 - p.v0);
			CHECK(std::fabs(p.w - (p.u1 - p.u0) * W) < 0.01f && std::fabs(p.h - (p.v1 - p.v0) * H) < 0.01f);  // never stretched
			const bool weapon = (p.u0 >= 0.78f && p.v0 >= 0.70f) || (p.u0 >= 0.62f && p.v0 >= 0.84f);
			// MGSV's call-menu help line (centred, y 0.775-0.79, x 0.31-0.69) stays whole, where MGSV drew it.
			if (p.v0 < 0.79f && p.v1 > 0.775f && p.u0 < 0.69f && p.u1 > 0.31f) {
				CHECK(!weapon);
			}
			if (weapon) {
				weaponUp = weaponUp || std::fabs(p.y - (p.v0 * H - 0.05f * H)) < 0.01f;  // 5% of the screen up
			} else {
				CHECK(std::fabs(p.y - p.v0 * H) < 0.01f && std::fabs(p.x - p.u0 * W) < 0.01f);  // the rest where MGSV drew it
			}
			centreKept = centreKept || (p.u0 <= 0.5f && p.u1 >= 0.5f && p.v0 <= 0.5f && p.v1 >= 0.5f);
		}
		CHECK(std::fabs(area - 1.0) < 1e-4);  // every texel once
		CHECK(weaponUp && centreKept);
		// MGSV's weapon box (layer y 0.78-0.91, measured) ends above Elden Ring's rune counter (screen y 0.94) with room
		// for the "+N runes" line in between.
		CHECK(0.911f - 0.05f < 0.88f);
	}
	Settings scaled;
	scaled.scale = 0.5f;
	scaled.offset[1] = 0.1f;
	const auto half = Layout(1920, 1080, 1280, 720, scaled);
	CHECK(half.size() == 1 && std::fabs(half[0].w - 960) < 0.01f && std::fabs(half[0].x - 480) < 0.01f && std::fabs(half[0].y - (270 + 108)) < 0.01f);
}

static void TestOverlay()
{
	TestHudLayout();
	using namespace phantom::er::Hud;
	OverlayConfig cfg;
	Brain         b;
	Inputs        in;
	auto          run = [&](double a_seconds) {
		for (double t = 0; t < a_seconds; t += 1.0 / 60.0) {
			in.now += 1.0 / 60.0;
			b.Update(in, cfg, 1.0f / 60.0f);
		}
	};
	auto has = [&](std::string_view a_text) {
		for (const auto& l : b.Out().lines) {
			if (l.text.find(a_text) != std::string::npos && l.opacity > 0.99f) {
				return true;
			}
		}
		return false;
	};

	// MGSV boots: its status text shows in the corner; nothing of Snake's screen yet.
	in.guestAlive = in.guestAttached = true;
	in.guestStatus = phantom::proto::Status::kLoading;
	in.guestText = "MGSV: loading Africa free roam (30020)";
	in.inGame = true;
	run(1.0);
	CHECK(has("loading Africa"));
	CHECK(b.Out().hudOpacity == 0.0f && b.Out().reticleOpacity == 0.0f && b.Out().readHud);

	// Ready + puppet: status goes, "Snake is in control"; the HUD layer waits for a fresh guest frame.
	in.guestStatus = phantom::proto::Status::kReady;
	in.puppet = true;
	run(0.1);
	CHECK(has("Snake is in control") || b.Out().lines.size() >= 1);
	run(1.0);
	CHECK(has("Snake is in control"));
	CHECK(!has("loading Africa"));
	CHECK(b.Out().hudOpacity == 0.0f);
	in.hudFresh = true;
	run(1.0);
	CHECK(b.Out().hudOpacity == 1.0f);
	// The guest stops publishing (a frozen last frame): the layer fades out, and back in when frames return.
	in.hudFresh = false;
	run(1.0);
	CHECK(b.Out().hudOpacity == 0.0f);

	// Reticle: only while aiming, and (auto) only without a live MGSV HUD layer.
	CHECK(b.Out().reticleOpacity == 0.0f);
	in.aiming = true;
	run(0.3);
	CHECK(b.Out().reticleOpacity == 1.0f);
	in.hudFresh = true;
	run(0.3);
	CHECK(b.Out().reticleOpacity == 0.0f);
	cfg.reticle = "on";
	run(0.3);
	CHECK(b.Out().reticleOpacity == 1.0f);
	cfg.reticle = "auto";
	in.aiming = false;
	run(1.0);
	CHECK(b.Out().hudOpacity == 1.0f);

	// An Elden Ring menu hides Snake's screen (fade), closing it brings it back.
	in.menuOpen = true;
	run(1.0);
	CHECK(b.Out().hudOpacity == 0.0f);
	in.menuOpen = false;
	run(1.0);
	CHECK(b.Out().hudOpacity == 1.0f);
	// Death: hidden until the respawn.
	in.dying = true;
	run(1.0);
	CHECK(b.Out().hudOpacity == 0.0f);
	in.dying = false;
	run(1.0);
	CHECK(b.Out().hudOpacity == 1.0f);

	// The guest goes away: the layer fades (not cut), a message says who has the controller, then the
	// absent line for absent_seconds only.
	in.guestAlive = in.guestAttached = false;
	in.puppet = false;
	in.guestStatus = phantom::proto::Status::kAbsent;
	run(0.1);
	CHECK(b.Out().hudOpacity > 0.0f && b.Out().hudOpacity < 1.0f);
	CHECK(!b.Out().readHud);
	run(1.0);
	CHECK(b.Out().hudOpacity == 0.0f);
	CHECK(has("MGSV stopped -- Elden Ring has the controller"));
	CHECK(has("MGSV is not running"));
	run(cfg.messageSeconds + 1.0);
	CHECK(!has("MGSV stopped"));
	run(cfg.absentSeconds);
	CHECK(b.Out().lines.empty());

	// A failure stays up after MGSV exits.
	in.guestAlive = in.guestAttached = true;
	in.guestStatus = phantom::proto::Status::kFailed;
	in.guestText = "MGSV: boot failed: no sandbox";
	run(1.0);
	CHECK(has("boot failed"));
	CHECK(b.Out().lines.front().tone == Tone::kError);
	in.guestAlive = in.guestAttached = false;
	run(5.0);
	CHECK(has("boot failed"));
	// A guest that recovered and then exits does not bring the old failure back.
	in.guestAlive = in.guestAttached = true;
	in.guestStatus = phantom::proto::Status::kReady;
	run(1.0);
	in.guestAlive = in.guestAttached = false;
	in.guestStatus = phantom::proto::Status::kAbsent;
	run(1.0);
	CHECK(!has("boot failed"));
	CHECK(has("MGSV is not running"));
	// MGSV's plugin speaks another protocol (never attaches): the versions, in red, in place of "not running".
	in.guestAlive = true;
	in.versionText = "PhantomRing versions differ: Elden Ring plugin v0.2.0-alpha, MGSV plugin v0.1.0 -- reinstall both";
	run(1.0);
	CHECK(has("versions differ") && !has("MGSV is not running"));
	CHECK(b.Out().lines.front().tone == Tone::kError);
	in.versionText.clear();
	in.guestAlive = false;
	run(1.0);
	CHECK(!has("versions differ"));
	// Disabled: nothing.
	cfg.enabled = false;
	run(1.0);
	CHECK(b.Out().lines.empty());

	// Pictures. PHANTOM_DUMP_OVERLAY=<dir> writes them as raw RGBA (premultiplied) for a look.
	phantom::er::OverlayArt::Image img;
	phantom::er::OverlayArt::Reticle(96, img);
	CHECK(img.width == 96 && img.height == 96 && img.pixels.size() == 96u * 96u);
	CHECK((img.pixels[48 * 96 + 48] & 0xFF) > 200 && (img.pixels[48 * 96 + 48] >> 24) > 200);  // the white centre dot
	CHECK((img.pixels[48 * 96 + 41] >> 24) < 10);                                               // the gap left of it
	CHECK((img.pixels[48 * 96 + 22] & 0xFF) > 200);                                             // the left tick
	CHECK((img.pixels[2 * 96 + 2] >> 24) == 0);                                                 // the corner
	const bool text = phantom::er::OverlayArt::TextPanel("MGSV: loading Africa free roam (30020)", 0xE8A33D, 36, 1600, img);
	CHECK(text);
	if (text) {
		CHECK(img.width > 400 && img.width < 1600 && img.height > 30 && img.height < 90);
		std::size_t bright = 0;
		for (const auto p : img.pixels) {
			bright += (p & 0xFF) > 200 && (p >> 24) > 200;
		}
		CHECK(bright > 500);                                     // the glyphs
		CHECK((img.pixels[0] & 0xFFFFFF) == 0x3DA3E8);  // the accent bar (RGBA8: R in the low byte)
		CHECK((img.pixels[img.width - 1] >> 24) > 100 && (img.pixels[img.width - 1] & 0xFF) < 40);  // the dark box
		std::printf("overlay: text panel %ux%u, %zu bright pixels\n", img.width, img.height, bright);
		if (const char* dir = std::getenv("PHANTOM_DUMP_OVERLAY")) {
			auto dump = [&](const char* a_name, const phantom::er::OverlayArt::Image& a_img) {
				if (FILE* f = std::fopen(std::format("{}/{}_{}x{}.rgba", dir, a_name, a_img.width, a_img.height).c_str(), "wb")) {
					std::fwrite(a_img.pixels.data(), 4, a_img.pixels.size(), f);
					std::fclose(f);
				}
			};
			dump("panel", img);
			phantom::er::OverlayArt::Image r;
			phantom::er::OverlayArt::Reticle(96, r);
			dump("reticle", r);
		}
	}
	CHECK(phantom::er::OverlayArt::TextPanel("a long line that has to wrap because it is wider than the box allows for", 0xE6463C, 36, 400, img));
	CHECK(img.width <= 400 + 64 && img.height > 70);
}
