#include "D3D11Reflect.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>

namespace phantom::guest::d3d
{
	namespace
	{
		// A bounds-checked view of one chunk: every offset in RDEF is relative to the chunk data.
		struct Chunk
		{
			const std::uint8_t* data{ nullptr };
			std::size_t         size{ 0 };

			bool U32(std::size_t a_off, std::uint32_t& a_out) const
			{
				if (a_off + 4 > size) {
					return false;
				}
				std::memcpy(&a_out, data + a_off, 4);
				return true;
			}
			bool U16(std::size_t a_off, std::uint16_t& a_out) const
			{
				if (a_off + 2 > size) {
					return false;
				}
				std::memcpy(&a_out, data + a_off, 2);
				return true;
			}
			std::string Str(std::size_t a_off) const
			{
				std::string out;
				for (std::size_t i = a_off; i < size && data[i] && out.size() < 128; ++i) {
					out.push_back(static_cast<char>(data[i]));
				}
				return out;
			}
		};

		// Flattens a variable's type (structs into their members) into a_out.
		void AddVar(const Chunk& a_c, std::vector<ShaderVar>& a_out, const std::string& a_name, std::uint32_t a_offset, std::uint32_t a_size, std::uint32_t a_type,
			int a_depth)
		{
			ShaderVar v;
			v.name = a_name;
			v.offset = a_offset;
			v.size = a_size;
			std::uint16_t members = 0;
			std::uint32_t memberOff = 0;
			if (!a_c.U16(a_type + 0, v.cls) || !a_c.U16(a_type + 4, v.rows) || !a_c.U16(a_type + 6, v.cols) || !a_c.U16(a_type + 8, v.elements) ||
				!a_c.U16(a_type + 10, members) || !a_c.U32(a_type + 12, memberOff)) {
				a_out.push_back(v);
				return;
			}
			if (v.cls != 5 || members == 0 || a_depth > 3) {  // D3D_SVC_STRUCT
				a_out.push_back(v);
				return;
			}
			v.size = a_size;
			a_out.push_back(v);  // the struct itself, then its members
			for (std::uint16_t m = 0; m < members && m < 64; ++m) {
				std::uint32_t nameOff = 0, typeOff = 0, off = 0;
				if (!a_c.U32(memberOff + m * 12u, nameOff) || !a_c.U32(memberOff + m * 12u + 4, typeOff) || !a_c.U32(memberOff + m * 12u + 8, off)) {
					break;
				}
				AddVar(a_c, a_out, a_name + "." + a_c.Str(nameOff), a_offset + off, 0, typeOff, a_depth + 1);
			}
		}
	}

	std::uint64_t Hash64(const void* a_data, std::size_t a_bytes, std::uint64_t a_seed)
	{
		constexpr std::uint64_t kPrime = 0x100000001b3ull;
		const auto*             p = static_cast<const std::uint8_t*>(a_data);
		std::uint64_t           h = a_seed;
		std::size_t             i = 0;
		for (; i + 8 <= a_bytes; i += 8) {
			std::uint64_t w;
			std::memcpy(&w, p + i, 8);
			h = (h ^ w) * kPrime;
			h ^= h >> 29;  // the multiply only carries upwards: fold the high bits back down every word
		}
		for (; i < a_bytes; ++i) {
			h = (h ^ p[i]) * kPrime;
		}
		h ^= a_bytes;
		h ^= h >> 33;
		h *= 0xff51afd7ed558ccdull;
		h ^= h >> 33;
		return h;
	}

	ShaderInfo Reflect(const void* a_bytecode, std::size_t a_bytes)
	{
		ShaderInfo info;
		info.bytes = static_cast<std::uint32_t>(a_bytes);
		if (!a_bytecode || a_bytes < 32) {
			return info;
		}
		info.hash = Hash64(a_bytecode, a_bytes);
		const auto* p = static_cast<const std::uint8_t*>(a_bytecode);
		if (std::memcmp(p, "DXBC", 4) != 0) {
			return info;
		}
		std::uint32_t total = 0, chunks = 0;
		std::memcpy(&total, p + 24, 4);
		std::memcpy(&chunks, p + 28, 4);
		if (total > a_bytes || chunks > 64 || 32 + chunks * 4ull > a_bytes) {
			return info;
		}
		Chunk rdef, isgn;
		for (std::uint32_t i = 0; i < chunks; ++i) {
			std::uint32_t off = 0, size = 0;
			std::memcpy(&off, p + 32 + i * 4, 4);
			if (off + 8ull > a_bytes) {
				return info;
			}
			std::memcpy(&size, p + off + 4, 4);
			if (off + 8ull + size > a_bytes) {
				return info;
			}
			if (std::memcmp(p + off, "RDEF", 4) == 0) {
				rdef = { p + off + 8, size };
			} else if (std::memcmp(p + off, "ISGN", 4) == 0 || std::memcmp(p + off, "ISG1", 4) == 0) {
				isgn = { p + off + 8, size };
			}
		}
		// ISGN: u32 count, u32 8, then {nameOff, semanticIndex, systemValue, componentType, register, u8 mask, u8 rwMask, u16} each.
		if (isgn.data) {
			std::uint32_t count = 0;
			if (isgn.U32(0, count) && count <= 32) {
				for (std::uint32_t i = 0; i < count; ++i) {
					const std::size_t b = 8 + i * 24ull;
					ShaderInput       in;
					std::uint32_t     nameOff = 0, masks = 0;
					if (!isgn.U32(b, nameOff) || !isgn.U32(b + 4, in.index) || !isgn.U32(b + 16, in.reg) || !isgn.U32(b + 20, masks)) {
						break;
					}
					in.semantic = isgn.Str(nameOff);
					in.mask = static_cast<std::uint8_t>(masks & 0xFF);
					in.used = static_cast<std::uint8_t>((masks >> 8) & 0xFF);
					info.inputs.push_back(std::move(in));
				}
			}
		}
		if (!rdef.data) {
			return info;
		}
		std::uint32_t cbCount = 0, cbOff = 0, resCount = 0, resOff = 0, version = 0;
		if (!rdef.U32(0, cbCount) || !rdef.U32(4, cbOff) || !rdef.U32(8, resCount) || !rdef.U32(12, resOff) || !rdef.U32(16, version)) {
			return info;
		}
		const std::uint32_t major = (version >> 8) & 0xFF;  // u8 minor, u8 major, u16 program type (0xFFFF pixel, 0xFFFE vertex)
		info.stage = static_cast<std::uint8_t>((version >> 16) == 0xFFFE ? 1 : 0);
		const std::uint32_t varStride = major >= 5 ? 40 : 24;
		if (cbCount > 64 || resCount > 256) {
			return info;
		}
		for (std::uint32_t r = 0; r < resCount; ++r) {
			const std::size_t b = resOff + r * 32ull;
			ShaderResource    res;
			std::uint32_t     nameOff = 0;
			if (!rdef.U32(b, nameOff) || !rdef.U32(b + 4, res.type) || !rdef.U32(b + 12, res.dimension) || !rdef.U32(b + 20, res.slot) ||
				!rdef.U32(b + 24, res.count)) {
				return info;
			}
			res.name = rdef.Str(nameOff);
			info.resources.push_back(std::move(res));
		}
		for (std::uint32_t c = 0; c < cbCount; ++c) {
			const std::size_t b = cbOff + c * 24ull;
			ShaderCBuffer     cb;
			std::uint32_t     nameOff = 0, varCount = 0, varOff = 0;
			if (!rdef.U32(b, nameOff) || !rdef.U32(b + 4, varCount) || !rdef.U32(b + 8, varOff) || !rdef.U32(b + 12, cb.size)) {
				return info;
			}
			cb.name = rdef.Str(nameOff);
			for (const auto& res : info.resources) {
				if (res.type == 0 && res.name == cb.name) {  // D3D_SIT_CBUFFER
					cb.slot = res.slot;
				}
			}
			for (std::uint32_t v = 0; v < varCount && v < 256; ++v) {
				const std::size_t vb = varOff + v * static_cast<std::size_t>(varStride);
				std::uint32_t     vName = 0, vStart = 0, vSize = 0, vFlags = 0, vType = 0;
				if (!rdef.U32(vb, vName) || !rdef.U32(vb + 4, vStart) || !rdef.U32(vb + 8, vSize) || !rdef.U32(vb + 12, vFlags) || !rdef.U32(vb + 16, vType)) {
					break;
				}
				AddVar(rdef, cb.vars, rdef.Str(vName), vStart, vSize, vType, 0);
			}
			info.cbuffers.push_back(std::move(cb));
		}
		info.reflected = true;
		return info;
	}

	bool ShaderInfo::Reads(std::string_view a_semantic, std::uint32_t a_index) const
	{
		for (const auto& in : inputs) {
			if (in.index == a_index && in.semantic.size() == a_semantic.size() &&
				std::equal(in.semantic.begin(), in.semantic.end(), a_semantic.begin(), [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == b; })) {
				return true;
			}
		}
		return false;
	}

	const ShaderCBuffer* ShaderInfo::CBufferAt(std::uint32_t a_slot) const
	{
		for (const auto& cb : cbuffers) {
			if (cb.slot == a_slot) {
				return &cb;
			}
		}
		return nullptr;
	}

	const ShaderCBuffer* ShaderInfo::CBufferNamed(std::string_view a_name) const
	{
		for (const auto& cb : cbuffers) {
			if (cb.name == a_name) {
				return &cb;
			}
		}
		return nullptr;
	}

	const ShaderResource* ShaderInfo::TextureAt(std::uint32_t a_slot) const
	{
		for (const auto& r : resources) {
			if (r.type == 2 && a_slot >= r.slot && a_slot < r.slot + (r.count ? r.count : 1)) {  // D3D_SIT_TEXTURE
				return &r;
			}
		}
		return nullptr;
	}

	std::string ShaderInfo::Describe() const
	{
		std::string out = std::format("{} {:#018x} {} bytes{}", stage ? "vs" : "ps", hash, bytes, reflected ? "" : " (no RDEF)");
		for (const auto& cb : cbuffers) {
			out += std::format("\n    cb b{} {} ({} bytes):", static_cast<int>(cb.slot), cb.name, cb.size);
			for (const auto& v : cb.vars) {
				out += std::format(" {}@{}[{}c{}r{}x{}e{}]", v.name, v.offset, v.size, v.cls, v.rows, v.cols, v.elements);
			}
		}
		if (!inputs.empty()) {
			out += "\n    inputs:";
			for (const auto& in : inputs) {
				out += std::format(" {}{}(v{} {:x}/{:x})", in.semantic, in.index, in.reg, in.mask, in.used);
			}
		}
		for (const auto& r : resources) {
			if (r.type != 0) {
				out += std::format("\n    res {} {} slot {} x{} dim {}", r.type == 2 ? "tex" : r.type == 3 ? "smp" : std::format("t{}", r.type), r.name, r.slot, r.count,
					r.dimension);
			}
		}
		return out;
	}
}
