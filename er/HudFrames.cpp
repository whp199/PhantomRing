#include "HudFrames.h"

#include "Link.h"
#include "Log.h"
#include "Mapping.h"
#include "phantomring_protocol.h"

namespace phantom::er::HudFrames
{
	namespace
	{
		std::uint32_t g_front{ 2 };
		std::uint32_t g_attach{ ~0u };
		bool          g_loggedBad{ false };
	}

	void Reset()
	{
		g_front = 2;
		g_attach = ~0u;
	}

	bool Acquire(std::uint32_t a_attach, View& a_out)
	{
		a_out = {};
		auto* base = Link::Get().MapBase();
		if (!base) {
			return false;
		}
		if (a_attach != g_attach) {
			g_attach = a_attach;
			g_front = 2;
		}
		auto*        ctl = reinterpret_cast<proto::HudCtl*>(base + proto::kOffHudCtl);
		TripleReader reader{ &ctl->state, g_front };
		if (!reader.Acquire()) {
			return false;
		}
		g_front = reader.front;
		if (g_front > 2) {
			g_front = 2;  // a corrupt state word: never index past the three slots
			return false;
		}
		const auto* hdr = reinterpret_cast<const proto::HudSlotHdr*>(base + proto::kOffHudCtl + 0x40 + g_front * sizeof(proto::HudSlotHdr));
		const auto  w = hdr->width, h = hdr->height;
		if (w == 0 || h == 0 || w > proto::kMaxHudW || h > proto::kMaxHudH) {
			if (!g_loggedBad) {
				g_loggedBad = true;
				Log::Warn("overlay: guest HUD frame with a bad size {}x{} dropped (max {}x{})", w, h, proto::kMaxHudW, proto::kMaxHudH);
			}
			return false;
		}
		a_out.pixels = base + proto::kOffHudSlots + g_front * proto::kHudSlotBytes;
		a_out.width = w;
		a_out.height = h;
		a_out.premultiplied = (hdr->flags & 1) != 0;
		a_out.srgbTarget = (hdr->flags & 2) != 0;
		a_out.frame = hdr->frame;
		return true;
	}
}
