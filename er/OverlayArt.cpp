#include "OverlayArt.h"

#include "Log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace phantom::er::OverlayArt
{
	namespace
	{
		// gdi32 through GetProcAddress: the DLL links no GDI import (and a missing export just turns text off).
		struct Gdi
		{
			decltype(&::CreateCompatibleDC) createDc{ nullptr };
			decltype(&::DeleteDC)           deleteDc{ nullptr };
			decltype(&::CreateFontW)        createFont{ nullptr };
			decltype(&::SelectObject)       select{ nullptr };
			decltype(&::DeleteObject)       deleteObject{ nullptr };
			decltype(&::CreateDIBSection)   createDib{ nullptr };
			decltype(&::SetTextColor)       textColor{ nullptr };
			decltype(&::SetBkMode)          bkMode{ nullptr };
			decltype(&::GdiFlush)           flush{ nullptr };
			bool                            ok{ false };
		};

		const Gdi& Load()
		{
			static Gdi            gdi;
			static std::once_flag once;
			std::call_once(once, [] {
				HMODULE m = ::LoadLibraryW(L"gdi32.dll");
				if (!m) {
					Log::Error("overlay: gdi32.dll not loadable; no text");
					return;
				}
				auto get = [m](auto& a_fn, const char* a_name) { a_fn = reinterpret_cast<std::remove_reference_t<decltype(a_fn)>>(reinterpret_cast<void*>(::GetProcAddress(m, a_name))); };
				get(gdi.createDc, "CreateCompatibleDC");
				get(gdi.deleteDc, "DeleteDC");
				get(gdi.createFont, "CreateFontW");
				get(gdi.select, "SelectObject");
				get(gdi.deleteObject, "DeleteObject");
				get(gdi.createDib, "CreateDIBSection");
				get(gdi.textColor, "SetTextColor");
				get(gdi.bkMode, "SetBkMode");
				get(gdi.flush, "GdiFlush");
				gdi.ok = gdi.createDc && gdi.deleteDc && gdi.createFont && gdi.select && gdi.deleteObject && gdi.createDib && gdi.textColor && gdi.bkMode && gdi.flush;
				if (!gdi.ok) {
					Log::Error("overlay: gdi32 exports missing; no text");
				}
			});
			return gdi;
		}

		std::uint32_t Pack(float r, float g, float b, float a)
		{
			auto q = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
			return q(r) | q(g) << 8 | q(b) << 16 | q(a) << 24;
		}

		struct Rgba
		{
			float r, g, b, a;
		};
		Rgba Unpack(std::uint32_t p) { return { (p & 0xFF) / 255.0f, (p >> 8 & 0xFF) / 255.0f, (p >> 16 & 0xFF) / 255.0f, (p >> 24) / 255.0f }; }

		// Premultiplied "over".
		std::uint32_t Over(std::uint32_t a_dst, float r, float g, float b, float a)
		{
			const Rgba d = Unpack(a_dst);
			return Pack(r * a + d.r * (1 - a), g * a + d.g * (1 - a), b * a + d.b * (1 - a), a + d.a * (1 - a));
		}

		// White text rendered by GDI into a w x h 32-bit DIB; returns coverage per pixel (0..1).
		bool Rasterize(const Gdi& a_gdi, const std::wstring& a_text, int a_px, int a_maxWidth, std::vector<float>& a_cov, int& a_w, int& a_h)
		{
			HDC dc = a_gdi.createDc(nullptr);
			if (!dc) {
				return false;
			}
			HFONT font = a_gdi.createFont(-a_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
				DEFAULT_PITCH | FF_SWISS, L"Arial");
			bool ok = false;
			if (font) {
				HGDIOBJ oldFont = a_gdi.select(dc, font);
				RECT    r{ 0, 0, a_maxWidth, 0 };
				constexpr UINT kFlags = DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX;
				::DrawTextW(dc, a_text.c_str(), static_cast<int>(a_text.size()), &r, kFlags | DT_CALCRECT);
				// Wrap with the measured box but let glyphs overflow it (DT_NOCLIP) into slack on the right:
				// under Proton the emboldened font inks past its measured advance (the last letter was cut).
				const int boxW = std::max<int>(1, r.right - r.left);
				const int slack = a_px;
				const int dibW = boxW + slack;
				a_h = std::max<int>(1, r.bottom - r.top);
				BITMAPINFO bi{};
				bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
				bi.bmiHeader.biWidth = dibW;
				bi.bmiHeader.biHeight = -a_h;  // top-down
				bi.bmiHeader.biPlanes = 1;
				bi.bmiHeader.biBitCount = 32;
				bi.bmiHeader.biCompression = BI_RGB;
				void*   bits = nullptr;
				HBITMAP bmp = a_gdi.createDib(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
				if (bmp && bits) {
					HGDIOBJ oldBmp = a_gdi.select(dc, bmp);
					std::memset(bits, 0, std::size_t(dibW) * a_h * 4);
					a_gdi.textColor(dc, RGB(255, 255, 255));
					a_gdi.bkMode(dc, TRANSPARENT);
					RECT dr{ 0, 0, boxW, a_h };
					::DrawTextW(dc, a_text.c_str(), static_cast<int>(a_text.size()), &dr, kFlags | DT_NOCLIP);
					a_gdi.flush();
					const auto* px = static_cast<const std::uint32_t*>(bits);
					// The inked width: the measured box, or further right where the glyphs actually went.
					int inkW = boxW;
					for (int y = 0; y < a_h; ++y) {
						for (int x = dibW - 1; x >= inkW; --x) {
							if (px[std::size_t(y) * dibW + x] & 0xFFFFFF) {
								inkW = x + 1;
								break;
							}
						}
					}
					a_w = inkW;
					a_cov.resize(std::size_t(a_w) * a_h);
					float maxCov = 0;
					for (int y = 0; y < a_h; ++y) {
						for (int x = 0; x < a_w; ++x) {
							const std::uint32_t p = px[std::size_t(y) * dibW + x];
							const auto          m = std::max({ p & 0xFF, p >> 8 & 0xFF, p >> 16 & 0xFF });
							const float         c = static_cast<float>(m) / 255.0f;
							a_cov[std::size_t(y) * a_w + x] = c;
							maxCov = std::max(maxCov, c);
						}
					}
					ok = maxCov > 0.0f;  // nothing drawn: no usable font
					a_gdi.select(dc, oldBmp);
					a_gdi.deleteObject(bmp);
				}
				a_gdi.select(dc, oldFont);
				a_gdi.deleteObject(font);
			}
			a_gdi.deleteDc(dc);
			return ok;
		}

		// Distance from p to segment ab.
		float SegDist(float px, float py, float ax, float ay, float bx, float by)
		{
			const float vx = bx - ax, vy = by - ay;
			const float t = std::clamp(((px - ax) * vx + (py - ay) * vy) / std::max(1e-6f, vx * vx + vy * vy), 0.0f, 1.0f);
			const float dx = px - (ax + vx * t), dy = py - (ay + vy * t);
			return std::sqrt(dx * dx + dy * dy);
		}
	}

	bool TextPanel(std::string_view a_utf8, std::uint32_t a_accent, int a_px, int a_maxWidth, Image& a_out)
	{
		a_out = {};
		const Gdi& gdi = Load();
		if (!gdi.ok || a_utf8.empty()) {
			return false;
		}
		std::wstring text(static_cast<std::size_t>(::MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), nullptr, 0)), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), text.data(), static_cast<int>(text.size()));

		std::vector<float> cov;
		int                tw = 0, th = 0;
		if (!Rasterize(gdi, text, a_px, std::max(a_px * 4, a_maxWidth), cov, tw, th)) {
			return false;
		}
		const int pad = std::max(4, a_px * 9 / 20);
		const int bar = std::max(3, a_px / 7);
		const int shadow = std::max(1, a_px / 18);
		const int w = bar + pad * 2 + tw + shadow;
		const int h = pad * 2 + th + shadow - std::max(1, a_px / 8);  // GDI's line box has room below the baseline already
		a_out.width = static_cast<std::uint32_t>(w);
		a_out.height = static_cast<std::uint32_t>(h);
		a_out.pixels.assign(std::size_t(w) * h, Pack(0, 0, 0, 0.62f));
		const float ar = (a_accent >> 16 & 0xFF) / 255.0f, ag = (a_accent >> 8 & 0xFF) / 255.0f, ab = (a_accent & 0xFF) / 255.0f;
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < bar; ++x) {
				a_out.pixels[std::size_t(y) * w + x] = Pack(ar, ag, ab, 1.0f);
			}
		}
		const int ox = bar + pad, oy = pad - std::max(1, a_px / 16);
		auto      at = [&](int x, int y) -> std::uint32_t& { return a_out.pixels[std::size_t(y) * w + x]; };
		for (int y = 0; y < th; ++y) {
			for (int x = 0; x < tw; ++x) {
				const float c = cov[std::size_t(y) * tw + x];
				if (c <= 0) {
					continue;
				}
				const int sx = ox + x + shadow, sy = oy + y + shadow;
				if (sx < w && sy >= 0 && sy < h) {
					at(sx, sy) = Over(at(sx, sy), 0, 0, 0, c * 0.85f);
				}
			}
		}
		// Warm white like MGSV's UI text.
		for (int y = 0; y < th; ++y) {
			for (int x = 0; x < tw; ++x) {
				const float c = cov[std::size_t(y) * tw + x];
				const int   tx = ox + x, ty = oy + y;
				if (c > 0 && ty >= 0 && ty < h && tx < w) {
					at(tx, ty) = Over(at(tx, ty), 0.95f, 0.93f, 0.88f, c);
				}
			}
		}
		return true;
	}

	void Reticle(int a_size, Image& a_out)
	{
		const int s = std::clamp(a_size, 16, 512);
		a_out.width = a_out.height = static_cast<std::uint32_t>(s);
		a_out.pixels.assign(std::size_t(s) * s, 0);
		const float c = s * 0.5f;
		const float half = std::max(1.0f, s * 0.022f);  // half line width
		const float gap = s * 0.15f, len = s * 0.2f;
		const float dot = std::max(1.2f, s * 0.026f);
		const float outline = std::max(1.0f, s * 0.018f);
		const float segs[4][4] = { { c, c - gap, c, c - gap - len }, { c, c + gap, c, c + gap + len }, { c - gap, c, c - gap - len, c },
			{ c + gap, c, c + gap + len, c } };
		for (int y = 0; y < s; ++y) {
			for (int x = 0; x < s; ++x) {
				const float px = x + 0.5f, py = y + 0.5f;
				float       d = std::hypot(px - c, py - c) - dot + half;  // the dot, as a capsule of radius `dot`
				for (const auto& g : segs) {
					d = std::min(d, SegDist(px, py, g[0], g[1], g[2], g[3]));
				}
				const float white = std::clamp(half + 0.5f - d, 0.0f, 1.0f);
				const float dark = std::clamp(half + outline + 0.5f - d, 0.0f, 1.0f) * 0.75f;
				std::uint32_t p = Pack(0, 0, 0, dark);
				p = Over(p, 1.0f, 1.0f, 1.0f, white * 0.95f);
				a_out.pixels[std::size_t(y) * s + x] = p;
			}
		}
	}
}
