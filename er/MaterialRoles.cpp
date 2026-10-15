#include "MaterialRoles.h"

#include "Config.h"
#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>

namespace phantom::er::MaterialRoles
{
	namespace
	{
		// Bindings (each draw's slot, each guest frame and pass) and textures (once each: the readable number).
		std::atomic<std::uint64_t> g_refusedBindings{ 0 }, g_refusedTextures{ 0 }, g_normalInAlbedo{ 0 }, g_posterSkipped{ 0 }, g_posterTextures{ 0 };
		std::atomic<int>           g_logged{ 0 }, g_posterLogged{ 0 };
		constexpr int              kLogTextures = 32;  // distinct textures logged (each once): a cap against a session full of them

		// DXGI_FORMAT values (dxgiformat.h): BC1 70-72 (8-byte blocks), BC2 73-75 and BC3 76-78 (16 bytes: alpha, then a BC1 colour block).
		int BlockBytes(std::uint32_t a_f)
		{
			if (a_f >= 70 && a_f <= 72) {
				return 8;
			}
			if (a_f >= 73 && a_f <= 78) {
				return 16;
			}
			return 0;
		}

		void Rgb565(std::uint16_t a_v, float& a_r, float& a_g, float& a_b)
		{
			a_r = static_cast<float>((a_v >> 11) & 31) / 31.0f;
			a_g = static_cast<float>((a_v >> 5) & 63) / 63.0f;
			a_b = static_cast<float>(a_v & 31) / 31.0f;
		}

		bool NormalColour(float a_r, float, float a_b) { return a_r >= 0.40f && a_r <= 0.62f && a_b >= 0.40f && a_b <= 0.62f; }
		// Green this far off the block's red/blue level is a slope (DXT5nm's y), not grey (the threshold the recordings were measured with).
		constexpr float kGreenOff = 0.08f;

		// The poster's plain background (243, 242, 243), measured on the decoded poster texture (docs/HOST.md "The poster drawn").
		// 565 endpoints are ~1/31 apart and an encoder may straddle the colour (one endpoint each side), so: their mean within
		// 0.03, each within 0.06. Pure white (a default texture) is 0.047 off: not the background.
		bool Within(float a_r, float a_g, float a_b, float a_tol)
		{
			return std::fabs(a_r - 0.953f) <= a_tol && std::fabs(a_g - 0.949f) <= a_tol && std::fabs(a_b - 0.953f) <= a_tol;
		}
		bool PosterBlock(float a_r0, float a_g0, float a_b0, float a_r1, float a_g1, float a_b1)
		{
			return Within(a_r0, a_g0, a_b0, 0.06f) && Within(a_r1, a_g1, a_b1, 0.06f) && Within((a_r0 + a_r1) * 0.5f, (a_g0 + a_g1) * 0.5f, (a_b0 + a_b1) * 0.5f, 0.03f);
		}
	}

	Mode ModeOf(std::string_view a_value)
	{
		if (a_value == "on" || a_value == "true" || a_value == "1" || a_value == "yes") {
			return Mode::kOn;
		}
		return a_value == "log" ? Mode::kLog : Mode::kOff;
	}

	const char* ModeName(Mode a_mode)
	{
		switch (a_mode) {
		case Mode::kOn: return "on";
		case Mode::kLog: return "log";
		default: return "off";
		}
	}

	std::uint8_t GroupOf(unsigned a_slot)
	{
		switch (a_slot) {
		case 0: return kAlbedo;
		case 1: return kNormal;
		case 2: return kSrm;
		default: return kExtra;
		}
	}

	const char* GroupName(std::uint8_t a_group)
	{
		switch (a_group) {
		case kAlbedo: return "diffuse";
		case kNormal: return "normal map";
		case kSrm: return "SRM";
		case kExtra: return "extra";
		default: return "mixed";
		}
	}

	bool Admit(std::uint8_t a_seen, std::uint8_t a_group)
	{
		if (a_group == kExtra) {
			return true;  // shared legitimately (fulton.prrec: MGSV's flat 128x128 normal map is also one shader's t5); sampled only by camo/poster
		}
		// Diffuse, normal map and SRM exclude each other (another map in the diffuse slot is the false colours). kExtra in a_seen
		// is ignored: an extra binding never bars a role, whichever came first.
		return (a_seen & (kAlbedo | kNormal | kSrm) & ~a_group) == 0;
	}

	Traits Analyse(std::uint32_t a_dxgiFormat, std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mips, const std::uint8_t* a_data, std::size_t a_bytes)
	{
		Traits      t;
		const int   bs = BlockBytes(a_dxgiFormat);
		if (!bs || !a_data || a_width == 0 || a_height == 0 || a_mips == 0) {
			return t;
		}
		// Walk to the first mip no bigger than 64 texels (or the last one).
		std::size_t   off = 0;
		std::uint32_t w = a_width, h = a_height;
		for (std::uint32_t m = 0;; ++m) {
			const std::size_t blocks = std::size_t((w + 3) / 4) * ((h + 3) / 4);
			const std::size_t bytes = blocks * static_cast<std::size_t>(bs);
			if (off + bytes > a_bytes) {
				return t;
			}
			if ((w <= 64 && h <= 64) || m + 1 >= a_mips) {
				std::size_t normal = 0, low = 0, high = 0, poster = 0;
				for (std::size_t k = 0; k < blocks; ++k) {
					const std::uint8_t* b = a_data + off + k * static_cast<std::size_t>(bs) + (bs == 16 ? 8 : 0);
					const auto          c0 = static_cast<std::uint16_t>(b[0] | (b[1] << 8));
					const auto          c1 = static_cast<std::uint16_t>(b[2] | (b[3] << 8));
					float               r0, g0, b0, r1, g1, b1;
					Rgb565(c0, r0, g0, b0);
					Rgb565(c1, r1, g1, b1);
					if (NormalColour(r0, g0, b0) && NormalColour(r1, g1, b1)) {
						++normal;
						const float level = (r0 + b0 + r1 + b1) * 0.25f;
						low += std::min(g0, g1) < level - kGreenOff;
						high += std::max(g0, g1) > level + kGreenOff;
					}
					poster += PosterBlock(r0, g0, b0, r1, g1, b1);
				}
				const auto n = static_cast<float>(blocks);
				t.analysed = true;
				t.normalShare = static_cast<float>(normal) / n;
				t.greenLow = static_cast<float>(low) / n;
				t.greenHigh = static_cast<float>(high) / n;
				t.posterShare = static_cast<float>(poster) / n;
				return t;
			}
			off += bytes;
			w = std::max(1u, w >> 1);
			h = std::max(1u, h >> 1);
		}
	}

	void NoteRefused(std::uint64_t a_key, unsigned a_slot, std::uint32_t a_material, std::uint8_t a_seen, bool a_normalInAlbedo, bool a_acted, bool a_first)
	{
		g_refusedBindings.fetch_add(1, std::memory_order_relaxed);
		if (!a_first) {
			return;
		}
		g_refusedTextures.fetch_add(1, std::memory_order_relaxed);
		if (a_normalInAlbedo) {
			g_normalInAlbedo.fetch_add(1, std::memory_order_relaxed);
		}
		if (g_logged.fetch_add(1, std::memory_order_relaxed) >= kLogTextures) {
			return;
		}
		const char* what = a_acted ? "left out" : "would be left out with role_guard = on";
		if (a_normalInAlbedo) {
			Log::Warn("render: texture {:016x} sent as the diffuse of material {:08x} is a normal map by its content: {} (a stale guest key?)", a_key, a_material, what);
		} else {
			Log::Warn("render: texture {:016x} sent as the {} (t{}) of material {:08x} has been drawn as a {}: {} (a stale guest key?)", a_key, GroupName(GroupOf(a_slot)),
				a_slot, a_material, GroupName(a_seen & (kAlbedo | kNormal | kSrm)), what);
		}
	}

	void NotePoster(std::uint32_t a_material, std::uint64_t a_key, const Traits& a_t, Mode a_mode, bool a_first)
	{
		const bool poster = PosterLike(a_t);
		if (!poster && a_mode == Mode::kOn) {
			g_posterSkipped.fetch_add(1, std::memory_order_relaxed);
		}
		if (!a_first) {
			return;
		}
		g_posterTextures.fetch_add(1, std::memory_order_relaxed);
		// Every box's texture 4 once, posters too: the live measure of the poster's share that the threshold lacks.
		if (g_posterLogged.fetch_add(1, std::memory_order_relaxed) < kLogTextures) {
			Log::Info("render: material {:08x}: texture 4 ({:016x}) has {:.0f}% of its blocks the poster background (a poster from 15%){}: {}", a_material, a_key,
				a_t.posterShare * 100.0f, a_t.analysed ? "" : " (format not analysed)",
				poster ? "a poster" : (a_mode == Mode::kOn ? "not a poster, the box is drawn without one" : "not a poster by that, drawn with it anyway (poster_check = log)"));
		}
	}

	std::string Describe()
	{
		const auto& rc = Cfg().render;
		return std::format("material guard (role_guard {}, poster_check {}): {} textures in a wrong role ({} normal maps as a diffuse; {} bindings), {} box textures "
						   "checked, {} poster draws without the poster",
			ModeName(ModeOf(rc.roleGuard)), ModeName(ModeOf(rc.posterCheck)), g_refusedTextures.load(), g_normalInAlbedo.load(), g_refusedBindings.load(),
			g_posterTextures.load(), g_posterSkipped.load());
	}
}
