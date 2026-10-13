#include "CaptureMesh.h"

#include "D3D11Reflect.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>

namespace phantom::guest::capture
{
	namespace
	{
		std::string Upper(std::string_view a_s)
		{
			std::string out(a_s);
			std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			return out;
		}

		std::uint32_t ElementBytes(std::uint32_t a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R32G32B32A32_FLOAT:
			case DXGI_FORMAT_R32G32B32A32_UINT:
			case DXGI_FORMAT_R32G32B32A32_SINT:
				return 16;
			case DXGI_FORMAT_R32G32B32_FLOAT:
			case DXGI_FORMAT_R32G32B32_UINT:
			case DXGI_FORMAT_R32G32B32_SINT:
				return 12;
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_UNORM:
			case DXGI_FORMAT_R16G16B16A16_UINT:
			case DXGI_FORMAT_R16G16B16A16_SNORM:
			case DXGI_FORMAT_R16G16B16A16_SINT:
			case DXGI_FORMAT_R32G32_FLOAT:
			case DXGI_FORMAT_R32G32_UINT:
				return 8;
			case DXGI_FORMAT_R16G16_FLOAT:
			case DXGI_FORMAT_R16G16_UNORM:
			case DXGI_FORMAT_R16G16_UINT:
			case DXGI_FORMAT_R16G16_SNORM:
			case DXGI_FORMAT_R16G16_SINT:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UINT:
			case DXGI_FORMAT_R8G8B8A8_SNORM:
			case DXGI_FORMAT_R8G8B8A8_SINT:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
			case DXGI_FORMAT_R11G11B10_FLOAT:
			case DXGI_FORMAT_R32_FLOAT:
			case DXGI_FORMAT_R32_UINT:
				return 4;
			case DXGI_FORMAT_R16_FLOAT:
			case DXGI_FORMAT_R16_UINT:
			case DXGI_FORMAT_R8G8_UNORM:
			case DXGI_FORMAT_R8G8_UINT:
				return 2;
			case DXGI_FORMAT_R8_UINT:
			case DXGI_FORMAT_R8_UNORM:
				return 1;
			default:
				return 0;
			}
		}

		std::uint64_t Mix(std::uint64_t a_h, std::uint64_t a_v) { return (a_h ^ a_v) * 0x100000001b3ull ^ ((a_h ^ a_v) >> 31); }
	}

	std::uint64_t MeshSource::Hash() const
	{
		std::uint64_t h = 0xcbf29ce484222325ull;
		for (const auto& s : vb) {
			h = Mix(h, reinterpret_cast<std::uintptr_t>(s.buffer));
			h = Mix(h, std::uint64_t(s.stride) << 32 | s.offset);
		}
		h = Mix(h, reinterpret_cast<std::uintptr_t>(ib));
		h = Mix(h, std::uint64_t(ibFormat) << 32 | ibOffset);
		h = Mix(h, std::uint64_t(startIndex) << 32 | indexCount);
		h = Mix(h, std::uint64_t(static_cast<std::uint32_t>(baseVertex)) << 32 | topology);
		for (const auto& e : elements) {
			h = Mix(h, d3d::Hash64(e.semantic.data(), e.semantic.size()));
			h = Mix(h, std::uint64_t(e.semanticIndex) << 48 | std::uint64_t(e.format) << 32 | std::uint64_t(e.slot) << 24 | e.offset);
		}
		return h;
	}

	bool DescribeMesh(const d3d::ContextState& a_s, const d3d::DrawArgs& a_a, MeshSource& a_out)
	{
		if (!a_a.indexed || !a_s.ib || !a_s.layoutInfo || !a_s.vsInfo || a_a.instances != 1) {
			return false;
		}
		a_out = {};
		const auto& layout = a_s.layoutInfo->elements;
		const d3d::InputElement* position = nullptr;
		for (const auto& e : layout) {
			if (Upper(e.semantic) == "POSITION" && e.semanticIndex == 0) {
				position = &e;
			}
		}
		if (!position) {
			return false;
		}
		bool dynamicChecked[d3d::kVbSlots]{}, dynamic[d3d::kVbSlots]{};
		for (const auto& in : a_s.vsInfo->inputs) {
			if (!in.used || in.semantic.starts_with("SV_") || in.semantic.starts_with("sv_")) {
				continue;
			}
			const auto name = Upper(in.semantic);
			const auto it = std::ranges::find_if(layout, [&](const d3d::InputElement& e) { return e.semanticIndex == in.index && Upper(e.semantic) == name; });
			if (it == layout.end() || it->perInstance || it->slot >= d3d::kVbSlots) {
				continue;
			}
			// Fox's dummies: a semantic the mesh lacks is aliased onto the POSITION bytes.
			if (&*it != position && it->slot == position->slot && it->offset == position->offset) {
				continue;
			}
			const auto& vb = a_s.vb[it->slot];
			if (!vb.buffer || !vb.stride) {
				continue;
			}
			if (!dynamicChecked[it->slot]) {
				D3D11_BUFFER_DESC d{};
				vb.buffer->GetDesc(&d);
				dynamic[it->slot] = d.Usage == D3D11_USAGE_DYNAMIC;  // per-frame data (vertex colours): not part of the mesh
				dynamicChecked[it->slot] = true;
			}
			if (dynamic[it->slot]) {
				continue;
			}
			a_out.elements.push_back(*it);
			a_out.elements.back().semantic = name;
			a_out.vb[it->slot] = { vb.buffer, vb.stride, vb.offset };
		}
		if (a_out.elements.empty() || !a_out.vb[position->slot].buffer) {
			return false;
		}
		a_out.ib = a_s.ib;
		a_out.ibFormat = a_s.ibFormat;
		a_out.ibOffset = a_s.ibOffset;
		a_out.startIndex = a_a.startIndex;
		a_out.indexCount = a_a.indexCount;
		a_out.baseVertex = a_a.baseVertex;
		a_out.topology = a_s.topology;
		return true;
	}

	std::optional<BuiltMesh> BuildMesh(const MeshSource& a_src, const std::shared_ptr<const std::vector<std::uint8_t>> a_vb[d3d::kVbSlots],
		const std::vector<std::uint8_t>& a_ib, std::string& a_why)
	{
		const std::uint32_t isz = a_src.ibFormat == DXGI_FORMAT_R32_UINT ? 4 : a_src.ibFormat == DXGI_FORMAT_R16_UINT ? 2 : 0;
		if (!isz || !a_src.indexCount) {
			a_why = std::format("index format {}", a_src.ibFormat);
			return std::nullopt;
		}
		const std::uint64_t first = a_src.ibOffset + std::uint64_t(a_src.startIndex) * isz;
		if (first + std::uint64_t(a_src.indexCount) * isz > a_ib.size()) {
			a_why = std::format("index range {}+{} beyond {} bytes", first, a_src.indexCount * isz, a_ib.size());
			return std::nullopt;
		}
		std::vector<std::uint32_t> idx(a_src.indexCount);
		std::uint32_t              lo = ~0u, hi = 0;
		for (std::uint32_t i = 0; i < a_src.indexCount; ++i) {
			std::uint32_t v = 0;
			std::memcpy(&v, a_ib.data() + first + std::uint64_t(i) * isz, isz);
			if (isz == 2 && v == 0xFFFF) {
				a_why = "strip cut index";
				return std::nullopt;
			}
			idx[i] = v;
			lo = std::min(lo, v);
			hi = std::max(hi, v);
		}
		const std::uint32_t count = hi - lo + 1;
		const std::int64_t  firstVertex = std::int64_t(a_src.baseVertex) + lo;
		if (firstVertex < 0 || count > 1u << 20) {
			a_why = std::format("vertex range {}+{}", firstVertex, count);
			return std::nullopt;
		}

		// Output streams: input slots sharing a buffer and stride become one stream when every element
		// still fits inside the stride from the lowest offset.
		struct Out
		{
			ID3D11Buffer* buffer{ nullptr };
			std::uint32_t stride{ 0 }, base{ 0 };
			int           src{ -1 };  // game slot of the bytes
		};
		std::vector<Out> outs;
		int              slotOut[d3d::kVbSlots];
		std::fill(std::begin(slotOut), std::end(slotOut), -1);
		for (unsigned s = 0; s < d3d::kVbSlots; ++s) {
			const auto& st = a_src.vb[s];
			if (!st.buffer) {
				continue;
			}
			for (std::size_t o = 0; o < outs.size(); ++o) {
				if (outs[o].buffer != st.buffer || outs[o].stride != st.stride) {
					continue;
				}
				const std::uint32_t base = std::min(outs[o].base, st.offset);
				bool                fits = true;
				for (const auto& e : a_src.elements) {
					if (e.slot != s && slotOut[e.slot] != static_cast<int>(o)) {
						continue;  // not in this group
					}
					const auto eoff = a_src.vb[e.slot].offset - base + e.offset;
					if (eoff + ElementBytes(e.format) > st.stride) {
						fits = false;
					}
				}
				if (fits) {
					outs[o].base = base;
					slotOut[s] = static_cast<int>(o);
					break;
				}
			}
			if (slotOut[s] < 0) {
				slotOut[s] = static_cast<int>(outs.size());
				outs.push_back({ st.buffer, st.stride, st.offset, static_cast<int>(s) });
			}
		}
		if (outs.empty() || outs.size() > 4) {
			a_why = std::format("{} vertex streams", outs.size());
			return std::nullopt;
		}

		BuiltMesh               mesh;
		proto::RenMesh          hdr{};
		std::vector<proto::RenInputElement> elems;
		for (const auto& e : a_src.elements) {
			proto::RenInputElement r{};
			std::memcpy(r.semantic, e.semantic.data(), std::min<std::size_t>(e.semantic.size(), sizeof(r.semantic) - 1));
			r.semanticIndex = e.semanticIndex;
			r.format = e.format;
			r.inputSlot = static_cast<std::uint32_t>(slotOut[e.slot]);
			r.alignedByteOffset = a_src.vb[e.slot].offset - outs[slotOut[e.slot]].base + e.offset;
			elems.push_back(r);
			mesh.layout += std::format("{}{}:{}@{}+{} ", e.semantic, e.semanticIndex, d3d::FormatName(e.format), r.inputSlot, r.alignedByteOffset);
		}
		std::vector<std::vector<std::uint8_t>> streams;
		for (std::size_t o = 0; o < outs.size(); ++o) {
			const auto& bytes = a_vb[outs[o].src];
			if (!bytes) {
				a_why = "vertex bytes missing";
				return std::nullopt;
			}
			const std::uint64_t from = outs[o].base + std::uint64_t(firstVertex) * outs[o].stride;
			const std::uint64_t n = std::uint64_t(count) * outs[o].stride;
			if (from + n > bytes->size()) {
				a_why = std::format("stream {} range {}+{} beyond {} bytes", o, from, n, bytes->size());
				return std::nullopt;
			}
			streams.emplace_back(bytes->begin() + from, bytes->begin() + from + n);
			hdr.streamStride[o] = outs[o].stride;
			hdr.streamBytes[o] = static_cast<std::uint32_t>(n);
			hdr.vertexBytes += static_cast<std::uint32_t>(n);
		}
		// Skinning facts the host needs: the highest bone index used (matrices to send), and weights present.
		for (const auto& r : elems) {
			const std::string sem(r.semantic);
			if (sem == "BLENDWEIGHT") {
				mesh.weights = true;
			}
			if (sem != "BLENDINDICES") {
				continue;
			}
			const auto& st = streams[r.inputSlot];
			const auto  stride = hdr.streamStride[r.inputSlot];
			const auto  bytes = ElementBytes(r.format);
			const bool  u16 = r.format == DXGI_FORMAT_R16G16B16A16_UINT || r.format == DXGI_FORMAT_R16G16_UINT || r.format == DXGI_FORMAT_R16_UINT;
			for (std::uint32_t v = 0; v < count; ++v) {
				const auto* p = st.data() + std::size_t(v) * stride + r.alignedByteOffset;
				for (std::uint32_t c = 0; c < bytes / (u16 ? 2 : 1); ++c) {
					std::uint32_t b = u16 ? (p[c * 2] | p[c * 2 + 1] << 8) : p[c];
					mesh.bones = std::max(mesh.bones, b + 1);
				}
			}
		}
		std::vector<std::uint8_t> indices(std::size_t(a_src.indexCount) * isz);
		for (std::uint32_t i = 0; i < a_src.indexCount; ++i) {
			const std::uint32_t v = idx[i] - lo;
			std::memcpy(indices.data() + std::size_t(i) * isz, &v, isz);
		}
		hdr.elementCount = static_cast<std::uint32_t>(elems.size());
		hdr.vertexStride = hdr.streamStride[0];
		hdr.indexFormat = a_src.ibFormat;
		hdr.indexBytes = static_cast<std::uint32_t>(indices.size());
		hdr.topology = a_src.topology;
		hdr.streams = static_cast<std::uint32_t>(outs.size());
		auto& out = mesh.payload;
		out.resize(sizeof(hdr) + elems.size() * sizeof(proto::RenInputElement) + hdr.vertexBytes + indices.size());
		std::size_t at = sizeof(hdr);
		std::memcpy(out.data() + at, elems.data(), elems.size() * sizeof(proto::RenInputElement));
		at += elems.size() * sizeof(proto::RenInputElement);
		for (const auto& s : streams) {
			std::memcpy(out.data() + at, s.data(), s.size());
			at += s.size();
		}
		std::memcpy(out.data() + at, indices.data(), indices.size());
		// Key: everything but the key itself (header fields, layout, bytes).
		std::memcpy(out.data(), &hdr, sizeof(hdr));
		hdr.key = d3d::Hash64(out.data() + sizeof(hdr.key), out.size() - sizeof(hdr.key));
		std::memcpy(out.data(), &hdr, sizeof(hdr));
		mesh.key = hdr.key;
		mesh.indexCount = a_src.indexCount;
		mesh.vertexCount = count;
		return mesh;
	}
}
