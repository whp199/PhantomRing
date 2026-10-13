#include "KeyGuide.h"

#include "Addresses.h"
#include "Config.h"
#include "Log.h"
#include "Mem.h"

#include <format>

namespace phantom::er::KeyGuide
{
	namespace
	{
		constexpr std::uint32_t kDeviceByte = 0x4;  // [V] 0x242790 stores the device here (and at +0); live: 0 pad, 1 keyboard

		std::uint64_t g_corrections{ 0 };
		int           g_lastSeen{ -1 };
		bool          g_warned{ false };
	}

	void Tick(bool a_puppeting)
	{
		const int want = Cfg().padGlyphDevice;
		if (!a_puppeting || want < 0) {
			return;
		}
		auto* obj = Addr().Instance(A::KeyGuideState);
		if (!obj || !Addr().Has(A::KeyGuideCtor)) {
			if (!g_warned) {
				g_warned = true;
				Log::Warn("key guide: state object {} -- Elden Ring keeps choosing its glyphs itself",
					Addr().Has(A::KeyGuideState) ? "not created yet" : "address unverified");
			}
			return;
		}
		const auto now = SafeRead<std::uint8_t>(obj + kDeviceByte);
		if (!now) {
			return;
		}
		if (*now != want) {
			if (g_corrections == 0 || g_lastSeen != *now) {
				Log::Info("key guide: Elden Ring chose glyph device {} while Snake plays -> {} (pad glyphs)", *now, want);
			}
			SafeWrite<std::uint8_t>(obj + kDeviceByte, static_cast<std::uint8_t>(want));
			++g_corrections;
		}
		g_lastSeen = *now;
	}

	std::string Describe()
	{
		auto*      obj = Addr().Instance(A::KeyGuideState);
		const auto dev = obj ? SafeRead<std::uint8_t>(obj + kDeviceByte) : std::nullopt;
		return std::format("key guide device {} (want {} while Snake plays), {} corrections", dev ? std::to_string(*dev) : std::string("?"),
			Cfg().padGlyphDevice, g_corrections);
	}
}
