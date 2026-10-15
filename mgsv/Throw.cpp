#include "Throw.h"

#include "Addresses.h"
#include "Blast.h"
#include "Config.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"
#include "ProjectileTag.h"
#include "Verify.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <set>

namespace phantom::guest::throwing
{
	namespace
	{
		using logic::Vec3;
		using SpawnFn = std::uint32_t (*)(std::uint8_t* a_view, const std::uint8_t* a_desc);
		SpawnFn g_spawn{ nullptr };

		std::atomic<bool>          g_installed{ false };
		std::atomic<bool>          g_lineOk{ false };
		std::atomic<bool>          g_ready{ false };
		std::atomic<std::uint64_t> g_frame{ 0 };

		// One projectile of ours: the view and slot MGSV keeps it in (until the slot dies or is reused).
		struct Live
		{
			std::uint8_t* view{ nullptr };
			std::uint32_t slot{ 0 };
			std::uint32_t id{ 0 };
			std::uint16_t equip{ 0 };
			logic::BlastKind kind{ logic::BlastKind::kIgnore };
			Vec3          at;              // where it was last seen
			std::uint64_t goneMs{ 0 };     // 0 = alive; else when its slot died (kept a moment for late blasts)
			std::uint32_t blasts{ 0 };
		};
		std::mutex             g_lock;
		std::array<Live, 32>   g_live{};
		std::uint32_t          g_lastId{ 0 };
		std::uint64_t          g_throws{ 0 }, g_skipped{ 0 }, g_matched{ 0 }, g_unmatched{ 0 }, g_aimFrames{ 0 };
		std::set<std::uint16_t> g_loggedOther;  // equip ids of non-explosive throws, logged once

		// Kept after its slot died: a gas cloud's last puff and the blast of the frame it died in.
		constexpr std::uint64_t kKeepMs = 2000;
		// A blast belongs to a throw whose projectile is this close (the frag's centre sits a little above it).
		constexpr float kMatchRadius = 3.0f;

		template <class T>
		std::optional<T> Get(std::uintptr_t a_at)
		{
			return SafeRead<T>(reinterpret_cast<const void*>(a_at));
		}

		std::optional<Vec3> GetVec(std::uintptr_t a_at)
		{
			const auto v = Get<std::array<float, 3>>(a_at);
			if (!v) {
				return std::nullopt;
			}
			const Vec3 r{ (*v)[0], (*v)[1], (*v)[2] };
			return r.Finite() ? std::optional<Vec3>(r) : std::nullopt;
		}

		bool IsView(std::uint8_t* a_view)
		{
			const auto v = reinterpret_cast<std::uintptr_t>(a_view);
			return Get<std::uintptr_t>(v).value_or(0) == addr::kThrowViewVtable && Get<std::uintptr_t>(v + 0x20).value_or(0) == addr::kThrowViewVtable2;
		}

		// The view's per-slot arrays (Addresses.h kThrowSpawn).
		std::uintptr_t Array(std::uint8_t* a_view, std::uintptr_t a_field, std::uint32_t a_slot, std::uint32_t a_stride)
		{
			const auto base = Get<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(a_view) + a_field).value_or(0);
			return base ? base + std::uintptr_t(a_slot) * a_stride : 0;
		}

		// Is the slot still that projectile (alive, same item)?
		bool Alive(const Live& a_l)
		{
			const auto rec = Array(a_l.view, 0x128, a_l.slot, 0x24);
			const auto flags = rec ? Get<std::uint32_t>(rec + 0x1C) : std::nullopt;
			const auto equip = Get<std::uint16_t>(Array(a_l.view, 0x120, a_l.slot, 4));
			return flags && (*flags & 1) && equip && *equip == a_l.equip;
		}

		void OnSpawn(std::uint8_t* a_view, std::uint32_t a_slot, const std::uint8_t* a_desc)
		{
			if (!IsView(a_view) || a_slot >= Get<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_view) + 0x50).value_or(0)) {
				return;
			}
			const auto equip = Get<std::uint16_t>(Array(a_view, 0x120, a_slot, 4));
			const auto p0 = GetVec(Array(a_view, 0x90, a_slot, 0x10));
			const auto v0 = GetVec(Array(a_view, 0x98, a_slot, 0x10));
			const auto rec = Array(a_view, 0x128, a_slot, 0x24);
			const auto fuse = rec ? Get<float>(rec + 4) : std::nullopt;
			const auto owner = Get<std::uint16_t>(reinterpret_cast<std::uintptr_t>(a_desc) + 0x3E);
			if (!equip || !p0 || !v0) {
				return;
			}
			const auto ex = logic::ExplosiveOf(*equip);
			std::scoped_lock lock(g_lock);
			// The slot is this projectile's now, whatever it held before.
			for (auto& l : g_live) {
				if (l.id && l.view == a_view && l.slot == a_slot && !l.goneMs) {
					l.goneMs = ::GetTickCount64();
				}
			}
			if (ex.use != logic::ExplosiveUse::kThrown) {
				++g_skipped;
				if (g_loggedOther.insert(*equip).second) {
					Log::Info("throw: equip {} (not an explosive we send) from {:.2f},{:.2f},{:.2f}", *equip, p0->x, p0->y, p0->z);
				}
				return;
			}
			const std::uint32_t id = blast::NextId();
			g_lastId = id;
			auto& slot = *std::ranges::min_element(g_live, {}, [](const Live& l) -> std::uint64_t { return l.id == 0 ? 0 : l.goneMs ? l.goneMs : ~0ull; });
			slot = { a_view, a_slot, id, *equip, ex.kind, *p0, 0, 0 };
			// The fuse MGSV gave it (s; MGSV counts it down in record +4); an impact item has none.
			const float fuseS = ex.impact || !fuse || !std::isfinite(*fuse) ? 0.0f : std::clamp(*fuse, 0.0f, 65.0f);
			proto::GuestEvent e{};
			e.type = proto::kGEvThrow;
			e.a = blast::ProtoKind(ex.kind);
			e.b = id;
			e.frame = static_cast<std::uint32_t>(g_frame.load(std::memory_order_relaxed));
			e.v0[0] = p0->x, e.v0[1] = p0->y, e.v0[2] = p0->z;
			e.v1[0] = v0->x, e.v1[1] = v0->y, e.v1[2] = v0->z;
			e.pad[0] = std::uint32_t(*equip) | (std::uint32_t(std::lround(fuseS * 1000.0f)) << 16);
			const bool pushed = g_ready.load(std::memory_order_relaxed) && link::PushEvent(e);
			projectile::NoteThrow(g_frame.load(std::memory_order_relaxed), e.v0, e.v1);  // its mesh, for the host's flown copy
			++g_throws;
			Log::Info("throw #{}: equip {} ({}) from {:.2f},{:.2f},{:.2f} at {:.2f},{:.2f},{:.2f} m/s ({:.1f} m/s), fuse {:.2f} s, owner {:#x}{}", id, *equip,
				logic::BlastKindName(ex.kind), p0->x, p0->y, p0->z, v0->x, v0->y, v0->z, v0->Length(), fuseS, owner.value_or(0),
				pushed ? " -> kGEvThrow" : " (not sent: not ready)");
		}

		std::uint32_t SpawnDetour(std::uint8_t* a_view, const std::uint8_t* a_desc)
		{
			const auto slot = g_spawn(a_view, a_desc);
			if (slot != addr::kThrowNoSlot && a_view && a_desc) {
				OnSpawn(a_view, slot, a_desc);
			}
			return slot;
		}

		// MGSV's arc while Snake aims: its first point and the velocity the next two imply (Addresses.h kThrowLineGetter).
		bool ReadArc(Aim& a_out)
		{
			const auto line = Get<std::uintptr_t>(addr::kThrowLinePtr).value_or(0);
			if (!line || Get<std::uintptr_t>(line).value_or(0) != addr::kThrowLineVtable) {
				return false;
			}
			const auto count = Get<std::int32_t>(line + 0x60).value_or(0);
			const auto buffer = Get<std::int32_t>(line + 0x68).value_or(-1);
			const auto shown = Get<float>(line + 0x74).value_or(0.0f);
			if (count < 3 || count > 180 || (buffer != 0 && buffer != 1) || !(shown > 0.0f)) {
				return false;
			}
			const std::uintptr_t points = line + 0xB0 + std::uintptr_t(1 - buffer) * 180 * 0x10;
			const auto p0 = GetVec(points), p1 = GetVec(points + 0x10), p2 = GetVec(points + 0x20);
			if (!p0 || !p1 || !p2) {
				return false;
			}
			const auto v = logic::ArcVelocity(*p0, *p1, *p2);
			if (!v) {
				return false;
			}
			a_out.pos[0] = p0->x, a_out.pos[1] = p0->y, a_out.pos[2] = p0->z;
			a_out.vel[0] = v->x, a_out.vel[1] = v->y, a_out.vel[2] = v->z;
			return true;
		}
	}

	bool Install()
	{
		if (!Cfg().throws) {
			Log::Info("throw: [Combat] Throws = false -- no kGEvThrow, no aim arc");
			return false;
		}
		g_lineOk = Matches(addr::kThrowLineGetter, "throwing line getter") && Matches(addr::kThrowLineAddPoint, "throwing line add point");
		if (!g_lineOk) {
			Log::Warn("throw: MGSV's throwing line differs -- no aim arc for the host");
		}
		if (!Matches(addr::kThrowSpawn, "throw spawn") || !Matches(addr::kThrowSpawnArrays, "throw spawn arrays") ||
			!Matches(addr::kThrowSpawnCall, "throw spawn call") || !Matches(addr::kThrowPredict, "throw predictor")) {
			Log::Warn("throw: MGSV's Throwing System differs -- thrown explosives send no kGEvThrow (their blasts still go out)");
			return false;
		}
		if (!Hooks::Create("throw spawn", reinterpret_cast<void*>(addr::kThrowSpawn.va), &SpawnDetour, &g_spawn)) {
			return false;
		}
		g_installed = true;
		Log::Info("throw: thrown explosives -> kGEvThrow (Throwing System spawn {:#x}){}", addr::kThrowSpawn.va,
			g_lineOk ? ", aim arc -> GuestState" : "");
		return true;
	}

	Aim Observe(bool a_ready, std::uint64_t a_frame, bool a_aiming, std::uint32_t a_inHand, std::uint16_t a_support)
	{
		g_ready.store(a_ready, std::memory_order_relaxed);
		g_frame.store(a_frame, std::memory_order_relaxed);
		const auto now = ::GetTickCount64();
		{
			std::scoped_lock lock(g_lock);
			for (auto& l : g_live) {
				if (!l.id) {
					continue;
				}
				if (!l.goneMs) {
					if (Alive(l)) {
						if (const auto at = GetVec(Array(l.view, 0x110, l.slot, 0x10))) {
							l.at = *at;
						}
					} else {
						l.goneMs = now;
						Log::Debug("throw #{}: gone ({} blasts)", l.id, l.blasts);
					}
				} else if (now - l.goneMs > kKeepMs) {
					l = {};
				}
			}
		}
		Aim aim;
		if (!g_lineOk || !a_ready || !a_aiming || a_inHand != addr::kSupportSlotInHand) {
			return aim;
		}
		const auto ex = logic::ExplosiveOf(a_support);
		if (ex.use != logic::ExplosiveUse::kThrown || !ReadArc(aim)) {
			return aim;
		}
		aim.kind = blast::ProtoKind(ex.kind);
		aim.valid = true;
		++g_aimFrames;
		return aim;
	}

	std::uint32_t MatchBlast(const Vec3& a_centre, logic::BlastKind a_kind)
	{
		std::scoped_lock lock(g_lock);
		Live*  best = nullptr;
		float  bestD = kMatchRadius;
		for (auto& l : g_live) {
			if (!l.id || l.kind != a_kind) {
				continue;
			}
			// Live ones where they are this instant (the blast happens inside MGSV's update); dead ones where last seen.
			Vec3 at = l.at;
			if (!l.goneMs && Alive(l)) {
				if (const auto now = GetVec(Array(l.view, 0x110, l.slot, 0x10))) {
					at = *now;
				}
			}
			const float d = (at - a_centre).Length();
			if (d <= bestD) {
				bestD = d;
				best = &l;
			}
		}
		if (!best) {
			++g_unmatched;
			return 0;
		}
		++best->blasts;
		++g_matched;
		// A gas/smoke cloud keeps puffing at its grenade's spot for up to a minute after the grenade's slot died: each puff
		// keeps the throw (and its id) a while longer, so every puff of one cloud carries the same id.
		if (best->goneMs) {
			best->goneMs = ::GetTickCount64();
		}
		return best->id;
	}

	std::string Describe()
	{
		if (!g_installed) {
			return "throw: off";
		}
		std::scoped_lock lock(g_lock);
		int live = 0;
		for (const auto& l : g_live) {
			live += l.id && !l.goneMs;
		}
		return std::format("throw: {} sent ({} non-explosive throws), {} in flight, last id {}; blasts matched {} / unmatched {}; aim arc {} ({} frames)",
			g_throws, g_skipped, live, g_lastId, g_matched, g_unmatched, g_lineOk.load() ? "on" : "off", g_aimFrames);
	}
}
