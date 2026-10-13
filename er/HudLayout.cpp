#include "HudLayout.h"

#include <algorithm>

namespace phantom::er::HudLayout
{
	namespace
	{
		bool Active(const Region& a_r) { return a_r.uv[2] > a_r.uv[0] && a_r.uv[3] > a_r.uv[1]; }
	}

	std::vector<Piece> Layout(float a_W, float a_H, float a_texW, float a_texH, const Settings& a_s)
	{
		std::vector<Piece> out;
		if (a_W <= 0 || a_H <= 0 || a_texW <= 0 || a_texH <= 0) {
			return out;
		}
		const float s = std::min(a_W / a_texW, a_H / a_texH) * std::clamp(a_s.scale, 0.25f, 2.0f);
		const float w = a_texW * s, h = a_texH * s;
		const float x0 = (a_W - w) * 0.5f + a_s.offset[0] * a_W, y0 = (a_H - h) * 0.5f + a_s.offset[1] * a_H;

		std::vector<float> xs{ 0, 1 }, ys{ 0, 1 };
		for (const auto& r : a_s.regions) {
			if (Active(r)) {
				xs.push_back(std::clamp(r.uv[0], 0.0f, 1.0f));
				xs.push_back(std::clamp(r.uv[2], 0.0f, 1.0f));
				ys.push_back(std::clamp(r.uv[1], 0.0f, 1.0f));
				ys.push_back(std::clamp(r.uv[3], 0.0f, 1.0f));
			}
		}
		std::sort(xs.begin(), xs.end());
		xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
		std::sort(ys.begin(), ys.end());
		ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

		for (std::size_t j = 0; j + 1 < ys.size(); ++j) {
			int   runOwner = -2;
			Piece run{};
			for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
				const float cu = (xs[i] + xs[i + 1]) * 0.5f, cv = (ys[j] + ys[j + 1]) * 0.5f;
				int         owner = -1;  // -1 = stays where MGSV drew it
				for (int k = 0; k < 3 && owner < 0; ++k) {
					const auto& r = a_s.regions[k];
					if (Active(r) && cu >= r.uv[0] && cu < r.uv[2] && cv >= r.uv[1] && cv < r.uv[3]) {
						owner = k;
					}
				}
				const float dx = owner >= 0 ? a_s.regions[owner].offset[0] * a_W : 0.0f, dy = owner >= 0 ? a_s.regions[owner].offset[1] * a_H : 0.0f;
				const Piece cell{ xs[i], ys[j], xs[i + 1], ys[j + 1], x0 + xs[i] * w + dx, y0 + ys[j] * h + dy, (xs[i + 1] - xs[i]) * w, (ys[j + 1] - ys[j]) * h };
				if (owner == runOwner) {
					run.u1 = cell.u1;
					run.w += cell.w;
				} else {
					if (runOwner != -2) {
						out.push_back(run);
					}
					run = cell;
					runOwner = owner;
				}
			}
			if (runOwner != -2) {
				out.push_back(run);
			}
		}
		return out;
	}
}
