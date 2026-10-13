#include "GeomProbe.h"

#include "Game.h"
#include "Log.h"
#include "Mem.h"
#include "Singletons.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace phantom::er::GeomProbe
{
	namespace
	{
		// CSWorldGeomMan [V-src fromsoftware-rs]: +0x18 DLMap<BlockId, CSWorldGeomManBlockData*> (allocator, head, size).
		constexpr std::uint32_t kBlocksHead = 0x20;
		constexpr std::uint32_t kBlocksSize = 0x28;
		// MSVC tree node: left 0, parent 8, right 0x10, color 0x18, is_nil 0x19, value 0x20 (key u32, then the value pointer at 0x28).
		constexpr std::uint32_t kNodeParent = 0x08;
		constexpr std::uint32_t kNodeLeft = 0x00;
		constexpr std::uint32_t kNodeRight = 0x10;
		constexpr std::uint32_t kNodeNil = 0x19;
		constexpr std::uint32_t kNodeKey = 0x20;
		constexpr std::uint32_t kNodeValue = 0x28;
		// CSWorldGeomManBlockData: +0x288 std::vector<CSWorldGeomIns*> (allocator, first, last, end), +0x2C8 u32 count +0x2D0 array.
		constexpr std::uint32_t kVecFirst = 0x290;
		constexpr std::uint32_t kVecLast = 0x298;
		constexpr std::uint32_t kArrayCount = 0x2C8;
		constexpr std::uint32_t kArray = 0x2D0;
		constexpr std::uint32_t kInsStride = 0x430;
		// CSWorldGeomIns: +0x18 CSWorldGeomInfo (+0x08 the param row), +0x30 CSMsbParts (+0x18 MsbPart*: +0 the name, PCWSTR).
		constexpr std::uint32_t kInsParam = 0x20;
		constexpr std::uint32_t kInsMsbPart = 0x48;
		// AssetEnvironmentGeometryParam (tools/re/er_params.py): +0x10 i16 hp, +0x3B bits (bit0 breaks by the player's body).
		constexpr std::uint32_t kParamHp = 0x10;
		constexpr std::uint32_t kParamBits = 0x3B;

		// Where a geom ins keeps its world position: the translation row of a 4x4 matrix at +0x220 (rows +0x220/+0x230/+0x240,
		// translation +0x250, w 1). VERIFIED live 2026-10-12 (Limgrave, m60_42_33/m60_43_37): `geom learn`'s distinct positions,
		// and the AEG801 bushes / AEG001_290 assets it lists sit where the screenshots show them. `geom offset` overrides it.
		std::uint32_t g_posOffset{ 0x250 };

		std::uint8_t* Manager()
		{
			auto* slot = SingletonSlot("CSWorldGeomMan");
			return slot ? SafePtr(slot) : nullptr;
		}

		std::vector<std::uint8_t*> Blocks(std::uint8_t* a_man)
		{
			std::vector<std::uint8_t*> out;
			auto* head = SafePtr(a_man + kBlocksHead);
			const auto size = SafeRead<std::uint64_t>(a_man + kBlocksSize).value_or(0);
			if (!head || size == 0 || size > 512) {
				return out;
			}
			std::vector<std::uint8_t*> todo{ SafePtr(head + kNodeParent) };
			while (!todo.empty() && out.size() < 512) {
				auto* n = todo.back();
				todo.pop_back();
				if (!n || n == head || SafeRead<std::uint8_t>(n + kNodeNil).value_or(1) != 0) {
					continue;
				}
				if (auto* data = SafePtr(n + kNodeValue)) {
					out.push_back(data);
				}
				todo.push_back(SafePtr(n + kNodeLeft));
				todo.push_back(SafePtr(n + kNodeRight));
			}
			return out;
		}

		std::vector<std::uint8_t*> Instances(std::uint8_t* a_block)
		{
			std::vector<std::uint8_t*> out;
			auto* first = SafePtr(a_block + kVecFirst);
			auto* last = SafePtr(a_block + kVecLast);
			if (first && last && last > first && (last - first) / 8 < 20000) {
				for (auto* p = first; p < last; p += 8) {
					if (auto* ins = SafePtr(p)) {
						out.push_back(ins);
					}
				}
			}
			const auto count = SafeRead<std::uint32_t>(a_block + kArrayCount).value_or(0);
			auto*      arr = SafePtr(a_block + kArray);
			if (arr && count < 20000) {
				for (std::uint32_t i = 0; i < count; ++i) {
					out.push_back(arr + std::size_t(i) * kInsStride);
				}
			}
			std::ranges::sort(out);
			out.erase(std::unique(out.begin(), out.end()), out.end());
			return out;
		}

		std::string NameOf(std::uint8_t* a_ins)
		{
			auto* part = SafePtr(a_ins + kInsMsbPart);
			auto* name = part ? SafePtr(part) : nullptr;
			std::string out;
			for (int i = 0; name && i < 48; ++i) {
				const auto c = SafeRead<std::uint16_t>(name + 2 * i).value_or(0);
				if (c == 0 || c > 0x7E) {
					break;
				}
				out.push_back(static_cast<char>(c));
			}
			return out;
		}

		bool Near3(const std::uint8_t* a_at, const Vec3& a_c, float a_r, Vec3& a_out)
		{
			float v[3];
			if (!SafeReadBytes(a_at, v, sizeof(v)) || !std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) {
				return false;
			}
			a_out = { v[0], v[1], v[2] };
			return Length(a_out - a_c) <= a_r && Length(a_out) > 1e-3f;
		}
	}

	std::vector<Asset> Near(const Vec3& a_centre, float a_radius, bool a_breakableOnly)
	{
		std::vector<Asset> out;
		auto* man = Manager();
		if (!man || !g_posOffset) {
			return out;
		}
		for (auto* block : Blocks(man)) {
			for (auto* ins : Instances(block)) {
				auto*      param = SafePtr(ins + kInsParam);
				const auto hp = param ? SafeRead<std::int16_t>(param + kParamHp).value_or(-1) : std::int16_t(-1);
				if (a_breakableOnly && hp <= 0) {
					continue;
				}
				Vec3 p;
				if (!Near3(ins + g_posOffset, a_centre, a_radius, p)) {
					continue;
				}
				Asset a;
				a.ins = ins;
				a.name = NameOf(ins);
				a.hp = hp;
				a.flags = param ? SafeRead<std::uint8_t>(param + kParamBits).value_or(0) : 0;
				a.pos = p;
				a.posValid = true;
				out.push_back(a);
			}
		}
		std::ranges::sort(out, {}, [&](const Asset& a_a) { return Length(a_a.pos - a_centre); });
		return out;
	}

	std::string Command(const std::vector<std::string>& a_w, const Frame& a_f)
	{
		auto* man = Manager();
		if (!man) {
			return "no CSWorldGeomMan";
		}
		if (!a_f.player) {
			return "no player";
		}
		const Vec3  me = game::Position(a_f.player);
		const auto  sub = a_w.size() > 1 ? a_w[1] : "status";
		const auto  blocks = Blocks(man);
		std::size_t total = 0, breakable = 0;
		for (auto* b : blocks) {
			for (auto* ins : Instances(b)) {
				++total;
				auto* param = SafePtr(ins + kInsParam);
				breakable += param && SafeRead<std::int16_t>(param + kParamHp).value_or(-1) > 0 ? 1 : 0;
			}
		}
		if (sub == "learn") {
			// Which offset of a geom ins holds a world position near the player: vote over every breakable asset (the one
			// offset whose triples land within 200 m for most of them).
			// Positions differ from asset to asset (rotation rows and scales repeat): count the distinct triples per offset.
			std::map<std::uint32_t, std::vector<Vec3>> seen;
			for (auto* b : blocks) {
				for (auto* ins : Instances(b)) {
					auto* param = SafePtr(ins + kInsParam);
					if (!param || SafeRead<std::int16_t>(param + kParamHp).value_or(-1) <= 0) {
						continue;
					}
					for (std::uint32_t off = 0x50; off + 12 <= kInsStride; off += 4) {
						Vec3 p;
						if (Near3(ins + off, me, 300.0f, p) && std::fabs(Length(p) - 1.0f) > 0.02f && std::fabs(p.y - me.y) < 80.0f) {
							auto& v = seen[off];
							if (v.size() < 4000) {
								v.push_back(p);
							}
						}
					}
				}
			}
			std::map<std::uint32_t, int> votes;
			for (auto& [off, v] : seen) {
				std::ranges::sort(v, [](const Vec3& a, const Vec3& b) { return a.x < b.x || (a.x == b.x && a.z < b.z); });
				int distinct = 0;
				for (std::size_t i = 0; i < v.size(); ++i) {
					distinct += i == 0 || Length(v[i] - v[i - 1]) > 0.05f ? 1 : 0;
				}
				votes[off] = distinct;
			}
			std::vector<std::pair<int, std::uint32_t>> best;
			for (const auto& [off, n] : votes) {
				best.emplace_back(n, off);
			}
			std::ranges::sort(best, std::greater{});
			std::string out = std::format("{} blocks, {} assets ({} breakable); offsets holding a point within 200 m of the player (votes):", blocks.size(),
				total, breakable);
			for (std::size_t i = 0; i < best.size() && i < 12; ++i) {
				out += std::format(" +{:#x}:{}", best[i].second, best[i].first);
			}
			if (!best.empty() && a_w.size() > 2 && a_w[2] == "use") {
				g_posOffset = best[0].second;
				out += std::format("\nusing +{:#x}", g_posOffset);
			}
			return out;
		}
		if (sub == "offset" && a_w.size() > 2) {
			g_posOffset = static_cast<std::uint32_t>(std::strtoul(a_w[2].c_str(), nullptr, 0));
			return std::format("geom ins position at +{:#x}", g_posOffset);
		}
		if (sub == "near" || sub == "all") {
			const float radius = a_w.size() > 2 ? std::strtof(a_w[2].c_str(), nullptr) : 30.0f;
			const std::string filter = a_w.size() > 3 ? a_w[3] : "";  // a name substring ("AEG100" the wagons); "-AEG801" leaves the bushes out
			if (!g_posOffset) {
				return "position offset unknown: `geom learn use` first";
			}
			auto list = Near(me, radius, sub == "near");
			if (!filter.empty()) {
				const bool        exclude = filter[0] == '-';
				const std::string f = exclude ? filter.substr(1) : filter;
				std::erase_if(list, [&](const Asset& a_a) { return (a_a.name.find(f) != std::string::npos) == exclude; });
			}
			std::string out = std::format("{} {} within {:.0f} m of ({:.1f}, {:.1f}, {:.1f}):", list.size(), sub == "near" ? "breakable assets" : "assets", radius,
				me.x, me.y, me.z);
			for (std::size_t i = 0; i < list.size() && i < 40; ++i) {
				const auto& a = list[i];
				out += std::format("\n {:2} {} hp {} {} at ({:.2f}, {:.2f}, {:.2f}) {:.1f} m ins {}", i, a.name, a.hp, (a.flags & 1) ? "breaks-on-touch" : "attack-only",
					a.pos.x, a.pos.y, a.pos.z, Length(a.pos - me), static_cast<void*>(a.ins));
			}
			return out;
		}
		if (sub == "dump" && a_w.size() > 2) {
			auto*       ins = reinterpret_cast<std::uint8_t*>(std::strtoull(a_w[2].c_str(), nullptr, 0));
			std::string out = std::format("{} {}:", static_cast<void*>(ins), NameOf(ins));
			for (std::uint32_t off = 0; off < kInsStride; off += 16) {
				float v[4]{};
				SafeReadBytes(ins + off, v, sizeof(v));
				out += std::format("\n +{:03x} {:10.3f} {:10.3f} {:10.3f} {:10.3f}", off, v[0], v[1], v[2], v[3]);
			}
			return out;
		}
		return std::format("geom: {} loaded blocks, {} assets ({} breakable); position at +{:#x}{}\n geom learn [use] | offset <hex> | near [radius [filter]] | all "
		                   "[radius [filter]] | dump <ins>   (filter: a name substring, -substring to leave out)",
			blocks.size(), total, breakable, g_posOffset, g_posOffset ? "" : " (unknown)");
	}
}
