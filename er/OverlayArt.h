#pragma once

// The overlay's own pictures, made on the CPU as premultiplied RGBA8 (R in the low byte):
//  - text panels: GDI draws the text (antialiased, the prefix's system font through Wine's FreeType),
//    so there is no font atlas to ship; a dark translucent box with a coloured accent bar keeps it
//    readable over any part of Elden Ring's picture. Made only when a line's text changes.
//  - the fallback reticle: four ticks around a centre dot, white with a dark outline (MGSV's style).

#include <cstdint>
#include <string_view>
#include <vector>

namespace phantom::er::OverlayArt
{
	struct Image
	{
		std::uint32_t              width{ 0 }, height{ 0 };
		std::vector<std::uint32_t> pixels;  // premultiplied RGBA8, rows top-down, width per row
	};

	// a_accent: 0xRRGGBB of the bar on the left. a_px: text height (pixels). a_maxWidth: wrap width of the text.
	bool TextPanel(std::string_view a_utf8, std::uint32_t a_accent, int a_px, int a_maxWidth, Image& a_out);
	void Reticle(int a_size, Image& a_out);
}
