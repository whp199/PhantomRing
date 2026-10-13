#pragma once

// Where MGSV's HUD layer goes on Elden Ring's screen ([Overlay] hud_*). Pure (tests/test_host_overlay.inc).
//
// The layer is MGSV's whole 16:9 HUD (1280x720), scaled to the back buffer like Elden Ring scales its own HUD. Both
// HUDs then share the corners: MGSV's weapon box (layer x 0.66-0.92, y 0.78-0.91) sits right above Elden Ring's rune
// counter (screen y 0.94-0.98) and the "+N runes" it shows on a kill; MGSV's notifications ("Development Complete
// ...", x 0.09-0.34, y 0.58-0.61, measured on the recordings) end just above Elden Ring's skill label (y 0.63-0.65)
// and its equipment slots (y 0.74-0.97). Regions of the layer can be moved on their own: hud_weapon_* (bottom right:
// the box itself and the bar row under it, which reaches further left, x 0.66) and hud_notify_* (left); the rest stays
// where MGSV drew it (its reticle, prompts and markers sit on the world and must not move). The box's region stays
// right of x 0.78: one rectangle from 0.62 cut MGSV's call-menu help line (centred at y 0.78, out to x 0.69) in two
// and moved its end up (VERIFIED live: "...mark nearb" | "y enemies."). Everything is in fractions, so 4K and 1080p
// (any 16:9) look the same.

#include <vector>

namespace phantom::er::HudLayout
{
	struct Region
	{
		float uv[4]{ 0, 0, 0, 0 };  // x0 y0 x1 y1 of the MGSV layer (0..1); empty = off
		float offset[2]{ 0, 0 };    // moved by this fraction of the screen width / height
	};

	struct Settings
	{
		float  scale{ 1.0f };         // the whole layer, about the screen centre
		float  offset[2]{ 0, 0 };     // the whole layer, fraction of the screen
		Region regions[3];            // weapon box, the bar row under it (its ammo / stamina bar), notifications
	};

	struct Piece
	{
		float u0, v0, u1, v1;  // texture coordinates
		float x, y, w, h;      // back-buffer pixels
	};

	// The quads that draw a a_texW x a_texH layer on a a_W x a_H back buffer: the layer cut along the regions' edges,
	// each region's cells moved by its offset, neighbouring cells of a row merged. One piece when no region is set.
	std::vector<Piece> Layout(float a_W, float a_H, float a_texW, float a_texH, const Settings& a_s);
}
