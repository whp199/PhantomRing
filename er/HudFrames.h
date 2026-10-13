#pragma once

// Reader side of the guest's HUD triple buffer (protocol HudCtl + HudSlotHdr[3] at kOffHudCtl, pixels at
// kOffHudSlots; SkyCraft's Overlay AcquireOverlayFrame): RGBA8 rows, width*4 bytes each, premultiplied
// alpha when HudSlotHdr.flags bit0 is set. Bit1 says the bytes come from an sRGB render target: MGSV
// blended them in linear light, so they hold encode(linear premultiplied colour), not premultiplied sRGB
// values (mgsv/Hud.h). Used on the Present thread only; the reader keeps its own front index and restarts
// (front = 2) whenever the link's attach count changes, as the guest restarts its writer (state 0, back 1)
// on every attach (docs/TOOLS.md "Attach handshake").

#include <cstdint>

namespace phantom::er::HudFrames
{
	struct View
	{
		const std::uint8_t* pixels{ nullptr };  // valid until the next Acquire
		std::uint32_t       width{ 0 }, height{ 0 };
		bool                premultiplied{ true };
		bool                srgbTarget{ false };  // flags bit1
		std::uint64_t       frame{ 0 };
	};

	// True when a newer frame was swapped in; a_out then describes it (a bad header gives false and an
	// empty view: the frame is dropped, the slot stays ours).
	bool Acquire(std::uint32_t a_attach, View& a_out);
	void Reset();
}
