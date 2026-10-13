#include "RenderMesh.h"

#include <dxgiformat.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace phantom::er::RenderMesh
{
	namespace
	{
		float Half(std::uint16_t a_h)
		{
			const std::uint32_t s = (a_h >> 15) & 1, e = (a_h >> 10) & 31, m = a_h & 1023;
			const float         v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? 65504.0f : std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
			return s ? -v : v;
		}

		struct Attr
		{
			std::uint32_t slot{ 0 }, offset{ 0 }, format{ 0 };
			bool          found{ false };
		};
	}

	bool Decode(std::uint32_t a_format, const std::uint8_t* a_p, float (&a_out)[4])
	{
		a_out[0] = a_out[1] = a_out[2] = 0;
		a_out[3] = 1;
		switch (a_format) {
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			std::memcpy(a_out, a_p, 16);
			return true;
		case DXGI_FORMAT_R32G32B32_FLOAT:
			std::memcpy(a_out, a_p, 12);
			return true;
		case DXGI_FORMAT_R16G16B16A16_FLOAT: {
			std::uint16_t h[4];
			std::memcpy(h, a_p, 8);
			for (int i = 0; i < 4; ++i) {
				a_out[i] = Half(h[i]);
			}
			return true;
		}
		case DXGI_FORMAT_R16G16B16A16_SNORM: {
			std::int16_t v[4];
			std::memcpy(v, a_p, 8);
			for (int i = 0; i < 4; ++i) {
				a_out[i] = std::max(-1.0f, v[i] / 32767.0f);
			}
			return true;
		}
		case DXGI_FORMAT_R16G16B16A16_UNORM: {
			std::uint16_t v[4];
			std::memcpy(v, a_p, 8);
			for (int i = 0; i < 4; ++i) {
				a_out[i] = v[i] / 65535.0f * 2.0f - 1.0f;  // a normal stored as UNORM
			}
			return true;
		}
		case DXGI_FORMAT_R8G8B8A8_SNORM:
			for (int i = 0; i < 4; ++i) {
				a_out[i] = std::max(-1.0f, static_cast<std::int8_t>(a_p[i]) / 127.0f);
			}
			return true;
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			for (int i = 0; i < 4; ++i) {
				a_out[i] = a_p[i] / 255.0f * 2.0f - 1.0f;
			}
			return true;
		default:
			return false;
		}
	}

	bool Bounds(std::span<const proto::RenInputElement> a_elements, const std::uint8_t* const* a_streams, const std::uint32_t* a_streamBytes,
		const std::uint32_t* a_strides, std::uint32_t a_streamCount, float (&a_min)[3], float (&a_max)[3])
	{
		Attr pos;
		for (const auto& e : a_elements) {
			char name[17]{};
			std::memcpy(name, e.semantic, 16);
			if (std::strcmp(name, "POSITION") == 0 && e.semanticIndex == 0 && e.inputSlot < a_streamCount) {
				pos = { e.inputSlot, e.alignedByteOffset, e.format, true };
				break;
			}
		}
		if (!pos.found || !a_strides[pos.slot] || pos.offset + 4 > a_strides[pos.slot]) {
			return false;
		}
		const std::uint32_t count = a_streamBytes[pos.slot] / a_strides[pos.slot];
		bool                any = false;
		for (std::uint32_t v = 0; v < count; ++v) {
			float p[4];
			if (!Decode(pos.format, a_streams[pos.slot] + std::size_t(v) * a_strides[pos.slot] + pos.offset, p) || !std::isfinite(p[0]) || !std::isfinite(p[1]) ||
				!std::isfinite(p[2])) {
				continue;
			}
			for (int k = 0; k < 3; ++k) {
				a_min[k] = any ? std::min(a_min[k], p[k]) : p[k];
				a_max[k] = any ? std::max(a_max[k], p[k]) : p[k];
			}
			any = true;
		}
		return any;
	}

	int Winding(std::span<const proto::RenInputElement> a_elements, const std::uint8_t* const* a_streams, const std::uint32_t* a_streamBytes,
		const std::uint32_t* a_strides, std::uint32_t a_streamCount, const std::uint8_t* a_indices, std::uint32_t a_indexBytes, bool a_index32, bool a_strip)
	{
		Attr pos, nrm;
		for (const auto& e : a_elements) {
			char name[17]{};
			std::memcpy(name, e.semantic, 16);
			Attr* a = std::strcmp(name, "POSITION") == 0 ? &pos : std::strcmp(name, "NORMAL") == 0 ? &nrm : nullptr;
			if (a && e.semanticIndex == 0 && !a->found && e.inputSlot < a_streamCount) {
				*a = { e.inputSlot, e.alignedByteOffset, e.format, true };
			}
		}
		if (!pos.found || !nrm.found) {
			return 0;
		}
		auto vertices = [&](const Attr& a_a) { return a_strides[a_a.slot] ? a_streamBytes[a_a.slot] / a_strides[a_a.slot] : 0u; };
		const std::uint32_t count = std::min(vertices(pos), vertices(nrm));
		auto read = [&](const Attr& a_a, std::uint32_t a_v, float (&a_out)[4]) {
			return a_v < count && a_a.offset + 4 <= a_strides[a_a.slot] && Decode(a_a.format, a_streams[a_a.slot] + std::size_t(a_v) * a_strides[a_a.slot] + a_a.offset, a_out);
		};
		const std::uint32_t indices = a_indexBytes / (a_index32 ? 4 : 2);
		auto index = [&](std::uint32_t a_i) {
			if (a_index32) {
				std::uint32_t v;
				std::memcpy(&v, a_indices + std::size_t(a_i) * 4, 4);
				return v;
			}
			std::uint16_t v;
			std::memcpy(&v, a_indices + std::size_t(a_i) * 2, 2);
			return static_cast<std::uint32_t>(v);
		};
		const std::uint32_t triangles = a_strip ? (indices >= 3 ? indices - 2 : 0) : indices / 3;
		const std::uint32_t step = std::max(1u, triangles / 4000);  // a sample is plenty
		std::uint32_t       agree = 0, against = 0;
		for (std::uint32_t t = 0; t < triangles; t += step) {
			std::uint32_t i0 = a_strip ? t : t * 3, i1 = i0 + 1, i2 = i0 + 2;
			if (a_strip && (t & 1)) {
				std::swap(i1, i2);  // odd strip triangles are wound the other way
			}
			const std::uint32_t v[3] = { index(i0), index(i1), index(i2) };
			if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) {
				continue;  // degenerate (strip restarts)
			}
			float p[3][4], n[3][4];
			bool  ok = true;
			for (int k = 0; k < 3 && ok; ++k) {
				ok = read(pos, v[k], p[k]) && read(nrm, v[k], n[k]);
			}
			if (!ok) {
				continue;
			}
			const float e1[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
			const float e2[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
			const float c[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
			const float s[3] = { n[0][0] + n[1][0] + n[2][0], n[0][1] + n[1][1] + n[2][1], n[0][2] + n[1][2] + n[2][2] };
			const float d = c[0] * s[0] + c[1] * s[1] + c[2] * s[2];
			if (d > 0) {
				++agree;
			} else if (d < 0) {
				++against;
			}
		}
		const std::uint32_t total = agree + against;
		if (total < 4) {
			return 0;
		}
		if (agree * 10 >= total * 7) {
			return 1;
		}
		if (against * 10 >= total * 7) {
			return -1;
		}
		return 0;
	}
}
