#pragma once

// Elden Ring's button glyphs (prompts, menus' key guides) while Snake plays.
//
// Elden Ring shows keyboard glyphs ("E: Acquire materials") once it decides the keyboard/mouse is the device in use,
// and while Snake plays its own pad sees nothing but Start/View (MGSV gets the rest), so nothing ever switches it
// back: Y at a grace showed "E" (VERIFIED live, also with injected pad presses). The decision lives in one byte: the
// key guide object ([eldenring+0x3D61F08], 0x88C bytes, built by 0x242790 with +0 and +4 = the "Debug.KeyGuideDevice"
// config value) +4: 0 = pad (Xbox glyphs), 1 = keyboard, 2 = none shown (VERIFIED live by poking it: the map's key
// guide changed from "E Q R F Z" to "A B X Y LB R" the next time it opened). Elden Ring writes it only when it sees
// another device's input, so while the puppet runs it is put back to [Input] pad_glyph_device each frame it differs.
//
// Not restored when Snake stops: the player holds a pad, and Elden Ring switches it again on keyboard/mouse input.

#include <string>

namespace phantom::er::KeyGuide
{
	void        Tick(bool a_puppeting);  // FrameBegin
	std::string Describe();
}
