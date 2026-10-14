// ActorProxy v0: the pure half (docs/GUEST.md §20). No game, Windows or mapping access: tests/test_guest_actorproxy.cpp.
#include "ActorProxy.h"
#include "FultonAnchor.h"

#include "phantomring_protocol.h"

#include <algorithm>
#include <string>
#include <cstdio>
#include <charconv>
#include <cmath>
#include <numbers>

namespace phantom::guest::proxy::plan
{
	void OffsetHistory::Record(std::uint64_t a_nowMs, const Vec3& a_k)
	{
		m_ring[m_head] = { a_nowMs, a_k };
		m_head = (m_head + 1) % kSize;
		m_count = std::min(m_count + 1, kSize);
	}

	Vec3 OffsetHistory::Latest() const { return m_count ? m_ring[(m_head + kSize - 1) % kSize].k : Vec3{}; }

	Vec3 OffsetHistory::At(std::uint64_t a_tMs) const
	{
		if (!m_count) {
			return {};
		}
		// Newest to oldest: the first record at or before a_tMs.
		Vec3 oldest{};
		for (std::size_t i = 0; i < m_count; ++i) {
			const auto& e = m_ring[(m_head + kSize - 1 - i) % kSize];
			if (e.t <= a_tMs) {
				return e.k;
			}
			oldest = e.k;
		}
		return oldest;
	}

	Vec3 TreadmillOffset(const Vec3& a_root, const double a_odo[3])
	{
		return { static_cast<float>(static_cast<double>(a_root.x) - a_odo[0]), static_cast<float>(static_cast<double>(a_root.y) - a_odo[1]),
			static_cast<float>(static_cast<double>(a_root.z) - a_odo[2]) };
	}

	std::vector<Target> SelectTargets(const proto::ActorRecord* a_records, std::uint32_t a_count, const Vec3& a_snakeRoot, const Vec3& a_kThen,
		const Vec3& a_kNow, const Settings& a_s)
	{
		std::vector<Target> out;
		if (!a_records) {
			return out;
		}
		const Vec3 shift = a_kNow - a_kThen;
		a_count = std::min(a_count, proto::kMaxActors);
		for (std::uint32_t i = 0; i < a_count; ++i) {
			const auto& r = a_records[i];
			if (!(r.flags & proto::kActorHostile) || (r.flags & proto::kActorDead) || !r.handle) {
				continue;
			}
			const Vec3 p = Vec3{ r.pos[0], r.pos[1], r.pos[2] } + shift;
			if (!p.Finite() || !std::isfinite(r.yaw)) {
				continue;
			}
			const float d = (p - a_snakeRoot).LengthXZ();
			if (d > a_s.maxDistance) {
				continue;
			}
			out.push_back({ r.handle, p, r.yaw, d, (r.flags & proto::kActorAsleep) != 0 });
		}
		std::stable_sort(out.begin(), out.end(), [](const Target& a, const Target& b) { return a.distance < b.distance; });
		return out;
	}

	Vec3 ParkPosition(const Vec3& a_centre, std::size_t a_index, const Settings& a_s)
	{
		return { a_centre.x + a_s.parkSpacing * static_cast<float>(a_index), a_centre.y - a_s.parkDepth, a_centre.z };
	}

	void Assign(std::vector<Slot>& a_slots, const std::vector<Target>& a_targets, const Vec3& a_snakeRoot, const Vec3* a_buddy, const Vec3& a_centre,
		const Settings& a_s)
	{
		const std::size_t wanted = std::min<std::size_t>(a_targets.size(), static_cast<std::size_t>(std::max(0, a_s.maxProxies)));
		// The nearest `wanted` targets are the ones that get soldiers.
		const auto chosen = [&](std::uint64_t a_handle) -> const Target* {
			for (std::size_t i = 0; i < wanted; ++i) {
				if (a_targets[i].handle == a_handle) {
					return &a_targets[i];
				}
			}
			return nullptr;
		};
		std::vector<bool> taken(wanted, false);
		// 1. Soldiers keep their characters.
		for (auto& s : a_slots) {
			const Target* t = s.handle ? chosen(s.handle) : nullptr;
			if (!t) {
				s.handle = 0;
				continue;
			}
			taken[static_cast<std::size_t>(t - a_targets.data())] = true;
		}
		// 2. Free soldiers (in pool order: but only the first maxProxies of the pool ever stand in) take new characters, nearest first.
		const std::size_t usable = std::min<std::size_t>(a_slots.size(), static_cast<std::size_t>(std::max(0, a_s.maxProxies)));
		for (std::size_t ti = 0; ti < wanted; ++ti) {
			if (taken[ti]) {
				continue;
			}
			for (std::size_t si = 0; si < usable; ++si) {
				if (!a_slots[si].handle) {
					a_slots[si].handle = a_targets[ti].handle;
					taken[ti] = true;
					break;
				}
			}
		}
		// 3. Positions: a proxy where its character is (unless too near Snake or the buddy), everything else parked.
		for (std::size_t si = 0; si < a_slots.size(); ++si) {
			auto&         s = a_slots[si];
			const Target* t = s.handle ? chosen(s.handle) : nullptr;
			bool          clear = t != nullptr;
			if (t) {
				clear = t->asleep ||
				        ((t->pos - a_snakeRoot).LengthXZ() >= a_s.snakeClearance && (!a_buddy || (t->pos - *a_buddy).LengthXZ() >= a_s.buddyClearance));
			}
			if (clear) {
				s.proxy = true;
				s.pos = t->pos;
				s.yaw = t->yaw;
			} else {
				s.proxy = false;
				s.pos = ParkPosition(a_centre, si, a_s);
				s.yaw = 0.0f;
			}
		}
	}

	bool NeedsPlace(const Vec3& a_last, float a_lastYaw, const Vec3& a_want, float a_wantYaw, std::uint64_t a_framesSince, const Settings& a_s)
	{
		if (a_framesSince >= static_cast<std::uint64_t>(std::max(1, a_s.refreshFrames))) {
			return true;
		}
		if ((a_want - a_last).Length() >= a_s.minMove) {
			return true;
		}
		float dy = std::fmod(a_wantYaw - a_lastYaw, 2.0f * std::numbers::pi_v<float>);
		if (dy > std::numbers::pi_v<float>) {
			dy -= 2.0f * std::numbers::pi_v<float>;
		} else if (dy < -std::numbers::pi_v<float>) {
			dy += 2.0f * std::numbers::pi_v<float>;
		}
		return std::fabs(dy) >= std::numbers::pi_v<float> / 180.0f;
	}

	Pool ParsePool(std::string_view a_text)
	{
		Pool out;
		const auto parseList = [](std::string_view a_list, std::vector<std::uint16_t>& a_into) {
			while (!a_list.empty()) {
				const auto comma = a_list.find(',');
				auto       item = a_list.substr(0, comma);
				while (!item.empty() && item.front() == ' ') {
					item.remove_prefix(1);
				}
				while (!item.empty() && item.back() == ' ') {
					item.remove_suffix(1);
				}
				unsigned v = 0;
				const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), v);
				if (ec == std::errc{} && end == item.data() + item.size() && v < 0xFFFF &&
					std::find(a_into.begin(), a_into.end(), static_cast<std::uint16_t>(v)) == a_into.end()) {
					a_into.push_back(static_cast<std::uint16_t>(v));
				}
				if (comma == std::string_view::npos) {
					break;
				}
				a_list.remove_prefix(comma + 1);
			}
		};
		const auto semi = a_text.find(';');
		parseList(a_text.substr(0, semi), out.soldiers);
		if (semi != std::string_view::npos) {
			parseList(a_text.substr(semi + 1), out.hostages);
		}
		return out;
	}

	bool NearProxy(const Vec3& a_drawPos, const Vec3& a_snakeRoot, const Vec3* a_proxies, std::size_t a_count, float a_radius)
	{
		if (!a_proxies) {
			return false;
		}
		const float toSnake = (a_drawPos - a_snakeRoot).LengthXZ();
		for (std::size_t i = 0; i < a_count; ++i) {
			const Vec3  d = a_drawPos - a_proxies[i];
			const float h = d.LengthXZ();
			if (h <= a_radius && std::fabs(d.y) <= 2.5f && h < toSnake) {
				return true;
			}
		}
		return false;
	}

	const char* PoolRefusal(bool a_disableSoldiers)
	{
		return a_disableSoldiers ? nullptr : "[Boot] DisableSoldiers = false: the slot soldiers would be MGSV's live AI -- proxies and the Fulton proxy off";
	}

	bool AutoAimWanted(bool a_enabled, bool a_autoAimForProxies, bool a_savesGuarded, std::size_t a_pool, std::size_t a_placed, std::size_t a_unplaced)
	{
		return a_enabled && a_autoAimForProxies && a_savesGuarded && a_pool > 0 && a_placed == a_pool && a_unplaced == 0;
	}
}

// Fulton anchor's pure half (FultonAnchor.h, docs/GUEST.md 20.4): here so tests/test_guest_actorproxy.cpp links it without the game glue.
namespace phantom::guest::fulton
{
	using logic::Vec3;

	namespace plan
	{
		int PickSleeper(const proto::ActorRecord* a_records, std::uint32_t a_count, const Vec3& a_snake, float a_reach,
			const std::vector<std::uint64_t>& a_done)
		{
			int   best = -1;
			float bestD = a_reach;
			for (std::uint32_t i = 0; i < a_count; ++i) {
				const auto& r = a_records[i];
				if ((r.flags & (proto::kActorHostile | proto::kActorAsleep)) != (proto::kActorHostile | proto::kActorAsleep) || (r.flags & proto::kActorDead) ||
					!r.handle || std::find(a_done.begin(), a_done.end(), r.handle) != a_done.end()) {
					continue;
				}
				const Vec3 p{ r.pos[0], r.pos[1], r.pos[2] };
				if (!p.Finite() || std::fabs(p.y - a_snake.y) > 3.0f) {
					continue;
				}
				const float d = (p - a_snake).LengthXZ();
				if (d <= bestD) {
					bestD = d;
					best = static_cast<int>(i);
				}
			}
			return best;
		}

		Vec3 SnakeSpot(const Vec3& a_snake, const Vec3& a_enemy, const Vec3& a_soldier, float a_ground)
		{
			const Vec3 off = a_enemy - a_snake;
			return { a_soldier.x - off.x, a_ground, a_soldier.z - off.z };
		}

		std::vector<Candidate> ParseCandidates(std::string_view a_text)
		{
			std::vector<Candidate> out;
			while (!a_text.empty()) {
				const auto semi = a_text.find(';');
				const auto item = a_text.substr(0, semi);
				a_text = semi == std::string_view::npos ? std::string_view{} : a_text.substr(semi + 1);
				const auto colon = item.find(':');
				if (colon == std::string_view::npos) {
					continue;
				}
				unsigned id = 0;
				if (std::from_chars(item.data(), item.data() + colon, id).ec != std::errc{} || id < 0x400 || id > 0xFFFE) {
					continue;
				}
				Candidate c;
				c.id = static_cast<std::uint16_t>(id);
				const std::string xyz(item.substr(colon + 1));
				if (std::sscanf(xyz.c_str(), "%f,%f,%f", &c.pos.x, &c.pos.y, &c.pos.z) == 3 && c.pos.Finite()) {
					out.push_back(c);
				}
			}
			return out;
		}
	}

}
