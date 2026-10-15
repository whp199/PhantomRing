// ActorProxy v1: the game glue (docs/GUEST.md §20, 20.5). [Proxies] Enabled = false by default.
//
// v0 moved pool soldiers through the player location manager; live (2026-10-14) it knows only the player family. What works:
// a soldier *enabled* (SetEnabled true) is realized and gets a location object of the same Fox class as Snake's (vtable
// 0x1424C8EF0; disabled soldiers have none, are not drawn and not targeted), `SetPuppet{enabled=true}` a few updates later makes
// him stand inert (no AI: 20 s 5 m in front of Snake, playerPhase 0), and that location's own setter (vf 0x68, as Hold's) moves
// him: drawn there, and MGSV's aim assist puts its reticle on him.
// Inert full proxies (20.6, [Proxies] Enabled only): SetRestrictNotice + SetPuppet go out before and right after the enable (the
// flags live in TppSoldier2's per-instance table, which SetEnabled leaves alone) and are refreshed; proxies stand on the sandbox
// ground (Height = ground) with Snake's rounds turned to the real height (CorrectShot); MGSV's alert phase is held at 0 (PhaseGuard). So a proxy is an enabled puppet whose location object is found
// by its position (the ~30 location objects are pooled per realized character; a scan of MGSV's writable memory lists them),
// and a free soldier is simply disabled again (out of the world).
// An Elden Ring character flagged asleep gets an asleep proxy (ChangeLifeState 3, refreshed): MGSV offers its own Fulton next to
// him, and when its balloon takes him (GetStatus 0 + GetActionStatus 1) the guest sends kGEvFulton for that character.
#include "ActorProxy.h"

#include "Addresses.h"
#include "Config.h"
#include "Hold.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"
#include "State.h"
#include "Verify.h"
#include "phantomring_protocol.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace phantom::guest::proxy
{
	namespace
	{
		using logic::Vec3;

		// A proxy soldier's way into the world: disabled (as the sandbox leaves them) -> enabled -> puppet -> its location object
		// found -> placed every frame. kSpent: MGSV's balloon took him (never used again).
		enum class Life : std::uint8_t
		{
			kDisabled,
			kEnabling,
			kPuppet,
			kLocating,
			kActive,
			kSpent,
		};
		const char* LifeName(Life a_l)
		{
			switch (a_l) {
			case Life::kDisabled:
				return "disabled";
			case Life::kEnabling:
				return "enabling";
			case Life::kPuppet:
				return "puppet";
			case Life::kLocating:
				return "locating";
			case Life::kActive:
				return "active";
			case Life::kSpent:
				return "spent";
			}
			return "?";
		}

		struct Runtime
		{
			Life           life{ Life::kDisabled };
			std::uint64_t  since{ 0 };
			std::uintptr_t loc{ 0 };
			bool           asleep{ false };  // ChangeLifeState 3 sent (its character is asleep in Elden Ring)
			std::uint64_t  sleptAt{ 0 };
			std::uint64_t  polledAt{ 0 };
			int            liftSeen{ 0 };
			std::uint64_t  inertAt{ 0 };   // last SetRestrictNotice/SetPuppet refresh (inert full proxies)
			std::uint64_t  ownedAt{ 0 };   // last location ownership check
			int            ownMiss{ 0 };   // consecutive checks the soldier's position cache was away from his location object
			std::uint32_t  recFlags{ 0 };  // the ActorTable flags of the character it stands for (game thread)
			// placement
			Vec3           pos;
			float          yaw{ 0 };
			std::uint64_t  frame{ 0 };
			bool           ok{ false };
			bool           readBack{ false };
		};

		std::mutex                      g_lock;  // pool, slots, runtimes (game thread, Lua thread, console)
		std::optional<plan::Pool>       g_pendingPool;
		plan::Pool                      g_pool;
		std::vector<std::uint16_t>      g_reserve;  // pool soldiers not in a slot (they replace spent ones)
		std::vector<plan::Slot>         g_slots;
		std::vector<Runtime>            g_rt;
		plan::OffsetHistory             g_k;
		std::uint32_t                   g_tableSeq{ 0 };
		std::uint64_t                   g_tableSeenMs{ 0 };
		std::vector<proto::ActorRecord> g_records;
		std::vector<std::uint64_t>      g_fultoned;  // Elden Ring characters fultoned: never a proxy's again
		std::vector<std::uint16_t>      g_spentIds;  // soldiers MGSV's balloon took: never in a pool again (this session)
		std::uint32_t                   g_fultonEvents{ 0 };
		std::uint32_t                   g_recordCount{ 0 };
		std::uint64_t                   g_frame{ 0 };
		std::size_t                     g_readOk{ 0 }, g_readBad{ 0 };
		std::uint64_t                   g_writes{ 0 }, g_writeFails{ 0 };
		bool                            g_warnedNoHold{ false };
		int                             g_sites{ -1 };
		int                             g_inertSites{ -1 };
		// MGSV's alert phase while full proxies are (or were, this session) in the world (Lua thread).
		plan::PhaseGuard                g_phase;
		std::uint64_t                   g_phaseAt{ 0 };
		int                             g_lastPhase{ 0 };
		bool                            g_fullRan{ false };
		bool                            g_fullWasOn{ false };  // Cfg().proxies at the last LuaStep: an off -> on starts a fresh guard
		std::uint32_t                   g_calms{ 0 }, g_sweeps{ 0 }, g_ownLost{ 0 };
		std::atomic<std::uint64_t>      g_shotsTurned{ 0 };  // game thread (ShotSolver), read by the console

		// The location objects and the soldier system, from a scan of MGSV's writable memory (background thread).
		std::mutex                  g_scanLock;
		std::vector<std::uintptr_t> g_locs;
		std::uintptr_t              g_soldierSystem{ 0 };
		std::atomic<bool>           g_scanning{ false };
		std::atomic<std::uint64_t>  g_scanAt{ 0 };
		std::atomic<std::uint32_t>  g_scans{ 0 };

		bool SitesOk()
		{
			if (g_sites < 0) {
				g_sites = Matches(addr::kAutoAimOptionGetter, "auto-aim option getter") &&
				                  SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(addr::kAutoAimOptionGetterSlot)).value_or(0) == addr::kAutoAimOptionGetter.va &&
				                  Matches(addr::kSoldierGetPosition, "TppSoldier2 GetPosition (position cache)") &&
				                  SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(addr::kSoldierSystemVtable + addr::kSoldierCommandSlot * 8)).value_or(0) ==
				                      addr::kSoldierCommandHandler
				              ? 1
				              : 0;
				if (!g_sites) {
					Log::Warn("proxy: this exe differs from the one ActorProxy was written for -- proxies off");
				}
			}
			return g_sites == 1;
		}

		// The Lua commands of the inert full proxies exist in this exe (20.6). Logged only: an unknown SendCommand id does nothing.
		bool InertSitesOk()
		{
			if (g_inertSites < 0) {
				g_inertSites = Matches(addr::kSoldierRestrictNoticeCase, "TppSoldier2 SetRestrictNotice") &&
				                       Matches(addr::kSoldierRestrictNoticeBody, "TppSoldier2 SetRestrictNotice (flag)") &&
				                       Matches(addr::kSoldierSetPuppetCase, "TppSoldier2 SetPuppet") && Matches(addr::kCpSetPhaseCase, "TppCommandPost2 SetPhase") &&
				                       Matches(addr::kCpSetPhaseAll, "TppCommandPost2 SetPhase (every post)")
				                   ? 1
				                   : 0;
				if (!g_inertSites) {
					Log::Warn("proxy: SetRestrictNotice / SetPuppet / SetPhase not where this build has them -- inert proxies and the phase lock may do nothing");
				}
			}
			return g_inertSites == 1;
		}

		// Every 8-byte aligned qword equal to the location vtable or the soldier system's in MGSV's committed private writable
		// memory (ReadProcessMemory on ourselves: a region can go away under the scan). ~2.8 GB, a second or two.
		void Scan(const std::string& a_why)
		{
			if (g_scanning.exchange(true)) {
				return;
			}
			g_scanAt.store(::GetTickCount64());
			std::thread([why = a_why] {
				const auto                  t0 = ::GetTickCount64();
				std::vector<std::uintptr_t> locs;
				std::uintptr_t              sys = 0;
				std::vector<std::uint64_t>  buf(1 << 16);
				MEMORY_BASIC_INFORMATION    mbi{};
				std::uintptr_t              at = 0x10000;
				std::uint64_t               bytes = 0;
				while (at < 0x7FFFFFFF0000ull && ::VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) == sizeof(mbi)) {
					const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
					const auto size = static_cast<std::uintptr_t>(mbi.RegionSize);
					if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
						size <= (std::uintptr_t{ 1 } << 31)) {
						for (std::uintptr_t off = 0; off < size; off += buf.size() * 8) {
							const auto n = std::min<std::uintptr_t>(buf.size() * 8, size - off);
							SIZE_T     got = 0;
							if (!::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<const void*>(base + off), buf.data(), n, &got)) {
								continue;
							}
							bytes += got;
							for (std::size_t i = 0; i < got / 8; ++i) {
								if (buf[i] == addr::kLocationVtable) {
									locs.push_back(base + off + i * 8);
								} else if (buf[i] == addr::kSoldierSystemVtable && !sys) {
									sys = base + off + i * 8;
								}
							}
						}
					}
					at = base + size;
				}
				{
					std::scoped_lock sl(g_scanLock);
					g_locs = std::move(locs);
					if (sys) {
						g_soldierSystem = sys;
					}
					Log::Info("proxy: scan ({}): {} location objects, soldier system {:#x}, {} MB in {} ms", why, g_locs.size(), g_soldierSystem, bytes >> 20,
						::GetTickCount64() - t0);
				}
				g_scans.fetch_add(1);
				g_scanning.store(false);
			}).detach();
		}

		// TppSoldier2's position cache of soldier a_id (what GetPosition answers: [[system+0x18]+0x20] + index * 16).
		std::optional<Vec3> SoldierCache(std::uint16_t a_id)
		{
			std::uintptr_t sys = 0;
			{
				std::scoped_lock sl(g_scanLock);
				sys = g_soldierSystem;
			}
			if (!sys || a_id < 0x400 || a_id >= 0x600 || SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(sys)).value_or(0) != addr::kSoldierSystemVtable) {
				return std::nullopt;
			}
			const auto p18 = SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(sys + 0x18));
			const auto arr = p18 && *p18 ? SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(*p18 + 0x20)) : std::nullopt;
			if (!arr || !*arr) {
				return std::nullopt;
			}
			const auto v = SafeRead<std::array<float, 3>>(reinterpret_cast<const void*>(*arr + (a_id - 0x400u) * 16u));
			if (!v) {
				return std::nullopt;
			}
			const Vec3 p{ (*v)[0], (*v)[1], (*v)[2] };
			return p.Finite() ? std::optional<Vec3>(p) : std::nullopt;
		}

		std::optional<Vec3> LocationFeet(std::uintptr_t a_loc)
		{
			if (!a_loc || SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(a_loc)).value_or(0) != addr::kLocationVtable) {
				return std::nullopt;
			}
			const auto f = SafeRead<std::array<float, 3>>(reinterpret_cast<const void*>(a_loc + 0x10));
			if (!f) {
				return std::nullopt;
			}
			const Vec3 p{ (*f)[0], (*f)[1], (*f)[2] };
			return p.Finite() ? std::optional<Vec3>(p) : std::nullopt;
		}

		// The location object standing (+0x10, feet) nearest a_at within a_radius, not one of a_taken; 0 = none.
		std::uintptr_t LocationNear(const Vec3& a_at, float a_radius, const std::vector<std::uintptr_t>& a_taken)
		{
			std::vector<std::uintptr_t> locs;
			{
				std::scoped_lock sl(g_scanLock);
				locs = g_locs;
			}
			std::uintptr_t best = 0;
			float          bestD = a_radius;
			for (const auto loc : locs) {
				if (std::find(a_taken.begin(), a_taken.end(), loc) != a_taken.end()) {
					continue;
				}
				const auto p = LocationFeet(loc);
				if (!p) {
					continue;
				}
				const float d = (*p - a_at).LengthXZ();
				if (d <= bestD && std::fabs(p->y - a_at.y) < 2.0f && (std::fabs(p->x) > 1.0f || std::fabs(p->z) > 1.0f)) {
					bestD = d;
					best = loc;
				}
			}
			return best;
		}

		// Snake's own location object, resolved on the game thread (OnFrame) every second: never a soldier's.
		std::atomic<std::uintptr_t> g_playerLoc{ 0 };

		// Capture's view (any thread): the placed proxies' feet.
		constexpr std::size_t      kMaxExcl = 32;
		std::atomic<bool>          g_active{ false };
		std::mutex                 g_exclLock;
		std::array<Vec3, kMaxExcl> g_excl{};
		std::size_t                g_exclCount{ 0 };
		// CorrectShot's view (game thread, under g_exclLock): placed proxies' feet and their characters' real feet.
		std::array<plan::Body, kMaxExcl> g_bodies{};
		std::size_t                      g_bodyCount{ 0 };

		plan::Settings Settings()
		{
			const auto&    c = Cfg();
			plan::Settings s;
			s.maxProxies = static_cast<int>(g_slots.size());
			s.maxDistance = c.proxyMaxDistance;
			s.snakeClearance = c.proxySnakeClearance;
			s.buddyClearance = c.proxyBuddyClearance;
			s.parkDepth = c.proxyParkDepth;
			s.tableLagMs = c.proxyTableLagMs;
			// Flattened only for full proxies: the Fulton-only path (live with the real host) keeps the enemy's height.
			s.height = c.proxies ? plan::ParseHeight(c.proxyHeight, plan::Height::kGround) : plan::Height::kEnemy;
			return s;
		}

		void AdoptPool(const plan::Pool& a_pool)
		{
			g_pool = a_pool;
			g_slots.clear();
			g_rt.clear();
			g_reserve.clear();
			const auto max = static_cast<std::size_t>(std::max(0, Cfg().proxyMax));
			for (const auto id : g_pool.soldiers) {
				if (std::find(g_spentIds.begin(), g_spentIds.end(), id) != g_spentIds.end()) {
					continue;  // fultoned this session (MGSV's sky has him)
				}
				if (g_slots.size() < max) {
					g_slots.push_back({ .id = id });
				} else {
					g_reserve.push_back(id);
				}
			}
			g_rt.assign(g_slots.size(), {});
			g_readOk = g_readBad = 0;
			Log::Info("proxy: pool of {} soldiers ({} slots, {} in reserve; every other soldier stays disabled: not drawn, not targeted)", g_pool.soldiers.size(),
				g_slots.size(), g_reserve.size());
			Scan("pool");
		}

		// kGEvFulton (callers hold g_lock).
		std::string SendFulton(std::uint64_t a_handle, bool a_ok, const char* a_why, std::optional<Vec3> a_at, const char* a_from)
		{
			proto::GuestEvent e{};
			e.type = proto::kGEvFulton;
			e.a = a_ok ? 1u : 0u;
			e.pad[0] = static_cast<std::uint32_t>(a_handle & 0xFFFFFFFFu);
			e.pad[1] = static_cast<std::uint32_t>(a_handle >> 32);
			if (!a_at) {
				std::vector<proto::ActorRecord> recs(proto::kMaxActors);
				std::uint32_t                   n = 0, seq = 0;
				if (link::ReadActors(recs.data(), proto::kMaxActors, n, seq)) {
					for (std::uint32_t i = 0; i < n; ++i) {
						if (recs[i].handle == a_handle) {
							a_at = Vec3{ recs[i].pos[0], recs[i].pos[1], recs[i].pos[2] };
							a_from = "the ActorTable";
						}
					}
				}
			}
			if (a_ok && std::find(g_fultoned.begin(), g_fultoned.end(), a_handle) == g_fultoned.end()) {
				g_fultoned.push_back(a_handle);
			}
			++g_fultonEvents;
			const auto fr = state::Frame();
			e.frame = static_cast<std::uint32_t>(fr.frame);
			if (!a_at) {
				a_at = fr.root;
				a_from = "Snake's root";
			}
			e.v0[0] = a_at->x;
			e.v0[1] = a_at->y;
			e.v0[2] = a_at->z;
			const bool sent = link::PushEvent(e);
			Log::Info("proxy: fulton of Elden Ring character {:#x} {} ({}) -- kGEvFulton a={} at {:.1f},{:.1f},{:.1f} ({}){}", a_handle, a_ok ? "succeeded" : "failed",
				a_why, e.a, a_at->x, a_at->y, a_at->z, a_from, sent ? "" : " -- event ring full, NOT sent");
			return std::format("kGEvFulton a={} handle {:#x} at {:.1f},{:.1f},{:.1f} ({}){}", e.a, a_handle, a_at->x, a_at->y, a_at->z, a_from,
				sent ? "" : " NOT sent (ring full)");
		}
	}

	namespace
	{
		std::atomic<bool> g_poolRequest{ false };
	}
	void RequestPool() { g_poolRequest.store(true); }
	bool TakePoolRequest() { return g_poolRequest.exchange(false); }

	void SetPool(const plan::Pool& a_pool)
	{
		std::scoped_lock lock(g_lock);
		g_pendingPool = a_pool;
		// The last pool's read-back counts say nothing about this one: Pump's auto-aim check on the same Tick read them (live
		// 2026-10-14, `!proxy on` after the Fulton-only pool: "auto-aim correction on" before one new slot soldier was placed).
		g_readOk = g_readBad = 0;
	}

	void OnFrame(const Vec3& a_root, const double a_odo[3], const Vec3* a_buddy, const Vec3& a_centre, std::uint64_t a_nowMs)
	{
		if (!Cfg().ProxiesRun()) {
			return;
		}
		std::scoped_lock lock(g_lock);
		++g_frame;
		if (g_pendingPool) {
			AdoptPool(*g_pendingPool);
			g_pendingPool.reset();
		}
		const Vec3 kNow = plan::TreadmillOffset(a_root, a_odo);
		g_k.Record(a_nowMs, kNow);
		if (g_frame % 60 == 1 && hold::Available()) {
			g_playerLoc.store(hold::PlayerLocation(), std::memory_order_relaxed);
		}
		if (g_slots.empty() || !SitesOk()) {
			g_active.store(false, std::memory_order_relaxed);
			return;
		}
		if (!hold::Available() && !g_warnedNoHold) {
			hold::Init();
		}
		if (!hold::Available()) {
			if (!g_warnedNoHold) {
				g_warnedNoHold = true;
				Log::Warn("proxy: the native placement isn't available (hold byte checks) -- proxies off");
			}
			g_active.store(false, std::memory_order_relaxed);
			return;
		}

		if (g_records.size() < proto::kMaxActors) {
			g_records.resize(proto::kMaxActors);
		}
		std::uint32_t n = 0, seq = 0;
		if (link::ReadActors(g_records.data(), proto::kMaxActors, n, seq)) {
			if (seq != g_tableSeq) {
				g_tableSeq = seq;
				g_tableSeenMs = a_nowMs;
			}
			g_recordCount = n;
		} else if (!link::HostAlive()) {
			g_recordCount = 0;
		}
		const auto s = Settings();
		const Vec3 kThen = g_k.At(g_tableSeenMs > static_cast<std::uint64_t>(s.tableLagMs) ? g_tableSeenMs - static_cast<std::uint64_t>(s.tableLagMs) : 0);
		auto       targets = plan::SelectTargets(g_records.data(), g_recordCount, a_root, kThen, kNow, s);
		if (!Cfg().proxies) {
			// [Fulton] Method = proxy with [Proxies] off: only sleeping characters get a proxy (MGSV's Fulton on him; no aim assist).
			std::erase_if(targets, [](const plan::Target& t) { return !t.asleep; });
		}
		if (!g_fultoned.empty()) {
			std::erase_if(targets, [](const plan::Target& t) { return std::find(g_fultoned.begin(), g_fultoned.end(), t.handle) != g_fultoned.end(); });
		}
		std::vector<std::uint64_t> before(g_slots.size());
		for (std::size_t i = 0; i < g_slots.size(); ++i) {
			before[i] = g_slots[i].handle;
		}
		plan::Assign(g_slots, targets, a_root, a_buddy, a_centre, s);

		std::array<Vec3, kMaxExcl>       excl{};
		std::array<plan::Body, kMaxExcl> bodies{};
		std::size_t                      exclCount = 0;
		for (std::size_t i = 0; i < g_slots.size(); ++i) {
			const auto& slot = g_slots[i];
			auto&       rt = g_rt[i];
			if (slot.handle != before[i] && slot.handle) {
				Log::Info("proxy: soldier {:#x} now stands in for Elden Ring character {:#x}", slot.id, slot.handle);
			}
			rt.recFlags = 0;
			for (std::uint32_t r = 0; r < g_recordCount; ++r) {
				if (slot.handle && g_records[r].handle == slot.handle) {
					rt.recFlags = g_records[r].flags;
				}
			}
			if (!slot.proxy || rt.life != Life::kActive) {
				rt.frame = 0;
				continue;
			}
			if (!rt.frame || plan::NeedsPlace(rt.pos, rt.yaw, slot.pos, slot.yaw, g_frame - rt.frame, s)) {
				// The location's set-position takes the point 0.8 m above the feet (+0x20 for Snake and soldiers; +0x10 = the feet).
				rt.ok = hold::PlaceLocation(rt.loc, slot.pos + Vec3{ 0.0f, 0.8f, 0.0f }, logic::YawQuat(slot.yaw));
				rt.pos = slot.pos;
				rt.yaw = slot.yaw;
				rt.frame = g_frame;
				++g_writes;
				if (!rt.ok) {
					++g_writeFails;
				}
			}
			if (exclCount < kMaxExcl) {
				bodies[exclCount] = { slot.pos, slot.real };
				excl[exclCount++] = slot.pos;
			}
		}
		// Every 30 frames: a proxy wanted in the world stands where it was put, every other slot soldier is out of it (disabled).
		if (g_frame % 30 == 0) {
			std::size_t ok = 0, bad = 0;
			for (std::size_t i = 0; i < g_slots.size(); ++i) {
				auto& rt = g_rt[i];
				if (g_slots[i].proxy) {
					const auto at = rt.life == Life::kActive ? LocationFeet(rt.loc) : std::nullopt;
					// On the ground (Height = ground) MGSV's own ground under a far proxy may sit a step off the centre's: y gets
					// HeightTolerance (20.6); at the enemy's height the old 1 m sphere.
					const float yTol = s.height == plan::Height::kGround ? Cfg().proxyHeightTolerance : 1.0f;
					rt.readBack = rt.ok && at && plan::ReadBackOk(rt.pos, *at, 1.0f, yTol) && (s.height == plan::Height::kGround || (*at - rt.pos).Length() < 1.0f);
				} else {
					rt.readBack = rt.life == Life::kDisabled || rt.life == Life::kSpent;
				}
				rt.readBack ? ++ok : ++bad;
			}
			if (ok != g_readOk || bad != g_readBad) {
				Log::Info("proxy: {} of {} slot soldiers where they should be ({} not)", ok, g_slots.size(), bad);
			}
			g_readOk = ok;
			g_readBad = bad;
		}
		{
			std::scoped_lock el(g_exclLock);
			g_excl = excl;
			g_exclCount = exclCount;
			g_bodies = bodies;
			g_bodyCount = exclCount;
		}
		if (Cfg().proxies && exclCount) {
			g_fullRan = true;
		}
		g_active.store(true, std::memory_order_relaxed);
	}

	void Suspend() { g_active.store(false, std::memory_order_relaxed); }

	namespace
	{
		// MGSV's alert phase held at 0 while full proxies are (or were, this session) in the world (20.6). Lua thread, g_lock held.
		void PhaseLock(std::uint64_t a_nowMs, const std::function<std::string(const std::string&)>& a_call)
		{
			const auto& c = Cfg();
			if (!c.proxyPhaseLock || !(c.proxies || g_fullRan) || a_nowMs - g_phaseAt < 500) {
				return;
			}
			g_phaseAt = a_nowMs;
			int        phase = -1;
			const auto text = a_call("return PhantomRing.ProxyPhase()");
			if (std::sscanf(text.c_str(), "%d", &phase) != 1) {
				phase = -1;
			}
			plan::PhaseSettings ps;
			ps.giveUpMs = static_cast<std::uint64_t>(c.proxyAlertGiveUpSec * 1000.0f);
			const auto step = g_phase.Update(phase, a_nowMs, ps);
			if (phase >= 0 && phase != g_lastPhase) {
				if (phase > 0 && g_lastPhase == 0) {
					Log::Warn("proxy: MGSV's alert phase 0 -> {} with proxies in the world -- phase lock: soldiers outside the slots disabled, command "
					          "posts to sneak",
						phase);
				} else {
					Log::Info("proxy: MGSV's alert phase {} -> {}", g_lastPhase, phase);
				}
				g_lastPhase = phase;
			}
			if (step.sweep) {
				// Every soldier and hostage out of the world but the slot soldiers in it (a reinforcement, a soldier the mission
				// enabled again: 2026-10-14's riot-shield soldier was neither a slot soldier nor disabled).
				std::string keep;
				for (std::size_t i = 0; i < g_slots.size(); ++i) {
					if (g_rt[i].life != Life::kDisabled && g_rt[i].life != Life::kSpent) {
						keep += std::format("{}{}", keep.empty() ? "" : ",", g_slots[i].id);
					}
				}
				++g_sweeps;
				const auto r = a_call(std::format("return PhantomRing.ProxySweep('{}')", keep));
				if (g_sweeps <= 20 || g_sweeps % 30 == 0) {
					Log::Info("proxy: phase lock: sweep (keeping {}): {} (#{})", keep.empty() ? "none" : keep, r, g_sweeps);
				}
			}
			if (step.calm) {
				++g_calms;
				const auto r = a_call("return PhantomRing.ProxyCalm()");
				if (g_calms <= 20 || g_calms % 30 == 0) {
					Log::Info("proxy: phase lock: {} (#{})", r, g_calms);
				}
			}
			if (step.giveUp && c.proxies) {
				// LuaStep stops before this lock once nothing runs proxies any more: it only keeps going with [Fulton] Method = proxy.
				Log::Warn("proxy: MGSV's alert phase still {} after {:.0f} s of phase lock -- full proxies OFF (`!proxy on` tries again; {})", phase,
					c.proxyAlertGiveUpSec,
					c.FultonProxies() ? "the Fulton-only proxies stay and the lock keeps calming" : "the phase lock stops with them");
				MutableCfg().proxies = false;
			}
		}
	}

	void LuaStep(std::uint64_t a_nowMs, const std::function<std::string(const std::string&)>& a_call)
	{
		if (Cfg().proxies != g_fullWasOn) {
			// Full proxies (back) on (`!proxy on` after a give-up or a `!proxy off`): a fresh phase guard, so a later alert that
			// MGSV can't calm gives up again, a phase still up sweeps at once, and `!proxy` stops saying it gave up.
			std::scoped_lock lock(g_lock);
			g_fullWasOn = Cfg().proxies;
			if (g_fullWasOn) {
				g_phase.Reset();
				g_lastPhase = 0;
				g_phaseAt = 0;
			}
		}
		if (!Cfg().ProxiesRun()) {
			// Turned off in session (`!proxy off`): the slot soldiers in the world go out of it once.
			std::scoped_lock lock(g_lock);
			for (std::size_t i = 0; i < g_slots.size(); ++i) {
				auto& rt = g_rt[i];
				if (rt.life != Life::kDisabled && rt.life != Life::kSpent) {
					a_call(std::format("return PhantomRing.FultonDisable({})", g_slots[i].id));
					rt = Runtime{};
					g_slots[i].handle = 0;
					g_slots[i].proxy = false;
				}
			}
			g_active.store(false, std::memory_order_relaxed);
			{
				std::scoped_lock el(g_exclLock);
				g_exclCount = 0;
			}
			return;
		}
		if (!g_active.load(std::memory_order_relaxed)) {
			return;
		}
		std::scoped_lock            lock(g_lock);
		const auto&                 cfg = Cfg();
		// Inert full proxies (20.6): only with [Proxies] Enabled -- the Fulton-only proxies keep the sequence that is live.
		const bool                  inert = cfg.proxies && cfg.proxyInert;
		if (inert || (cfg.proxies && cfg.proxyPhaseLock)) {
			InertSitesOk();
		}
		PhaseLock(a_nowMs, a_call);
		std::vector<std::uintptr_t> taken;
		for (const auto& rt : g_rt) {
			if (rt.loc) {
				taken.push_back(rt.loc);
			}
		}
		for (std::size_t i = 0; i < g_slots.size(); ++i) {
			auto&      slot = g_slots[i];
			auto&      rt = g_rt[i];
			const auto in = a_nowMs - rt.since;
			const auto enter = [&](Life a_l) {
				rt.life = a_l;
				rt.since = a_nowMs;
			};
			const auto release = [&] {
				std::erase(taken, rt.loc);
				rt.loc = 0;
				rt.asleep = false;
				rt.liftSeen = 0;
			};
			if (rt.life == Life::kSpent) {
				// MGSV's balloon took him: the next reserve soldier takes the slot (the character he stood for is gone too).
				if (!g_reserve.empty()) {
					Log::Info("proxy: soldier {:#x} spent (fultoned) -- soldier {:#x} takes his slot", slot.id, g_reserve.front());
					slot.id = g_reserve.front();
					g_reserve.erase(g_reserve.begin());
					rt = Runtime{};
				}
				continue;
			}
			if (!slot.proxy) {
				if (rt.life != Life::kDisabled) {
					a_call(std::format("return PhantomRing.FultonDisable({})", slot.id));
					release();
					enter(Life::kDisabled);
				}
				continue;
			}
			switch (rt.life) {
			case Life::kDisabled:
				if (inert) {
					// Puppet and restrict-notice before the enable (they are in TppSoldier2's per-instance table, which SetEnabled
					// leaves alone) and again right after: no update of live AI at his post (the likely start of the 2026-10-14 alert).
					a_call(std::format("return PhantomRing.ProxyEnable({}, {})", slot.id, cfg.proxyNoAssignRoute ? "true" : "false"));
					rt.inertAt = a_nowMs;
				} else {
					a_call(std::format("return PhantomRing.FultonEnable({})", slot.id));
				}
				enter(Life::kEnabling);
				break;
			case Life::kEnabling:
				if (in >= 150) {
					a_call(std::format("return PhantomRing.ProxyPuppet({})", slot.id));
					if (inert) {
						a_call(std::format("return PhantomRing.ProxyInert({}, true)", slot.id));
					}
					enter(Life::kPuppet);
				}
				break;
			case Life::kPuppet:
				if (in >= 150) {
					enter(Life::kLocating);
				}
				break;
			case Life::kLocating: {
				// Realized at his post: his location object stands where his position cache says (within the cache's lag).
				const auto cache = SoldierCache(slot.id);
				// (Unused location objects wait at the origin, and so do unused soldier slots: never matched there.) Snake's and the
				// buddy's are of the same class: never taken -- Snake's by its address, any within 2.5 m of Snake or the buddy by place.
				const bool real = cache && (std::fabs(cache->x) > 1.0f || std::fabs(cache->z) > 1.0f);
				auto       avoid = taken;
				if (const auto own = g_playerLoc.load(std::memory_order_relaxed)) {
					avoid.push_back(own);
				}
				const auto fr = state::Frame();
				const auto lv = state::Lua();
				const bool nearSnake = real && fr.valid && (*cache - fr.root).LengthXZ() < 2.5f;
				const bool nearBuddy = real && lv.buddyPosValid && (*cache - lv.buddyPos).LengthXZ() < 2.5f;
				const auto loc = real && !nearSnake && !nearBuddy ? LocationNear(*cache, 2.0f, avoid) : 0;
				if (loc) {
					rt.loc = loc;
					taken.push_back(loc);
					rt.frame = 0;
					rt.ownMiss = 0;
					rt.ownedAt = a_nowMs;
					if (inert) {
						// (Re)realized: the flags are the instance's, but say them again before he is moved.
						a_call(std::format("return PhantomRing.ProxyInert({}, {})", slot.id, rt.asleep ? "false" : "true"));
						rt.inertAt = a_nowMs;
					}
					enter(Life::kActive);
					Log::Info("proxy: soldier {:#x} realized (location object {:#x} at {:.1f},{:.1f},{:.1f}) -- standing in for {:#x}", slot.id, loc, cache->x, cache->y,
						cache->z, slot.handle);
				} else if (in > 8000) {
					// Not realized (too far from Snake?): disabled again, and the next reserve soldier tries (this one goes to the back).
					Log::Warn("proxy: soldier {:#x}'s location object not found in 8 s ({}) -- disabled again{}", slot.id,
						cache ? std::format("cache {:.1f},{:.1f},{:.1f}", cache->x, cache->y, cache->z) : std::string("no position cache"),
						g_reserve.empty() ? "" : std::format(", soldier {:#x} tries", g_reserve.front()));
					a_call(std::format("return PhantomRing.FultonDisable({})", slot.id));
					if (!g_reserve.empty()) {
						g_reserve.push_back(slot.id);
						slot.id = g_reserve.front();
						g_reserve.erase(g_reserve.begin());
					}
					enter(Life::kDisabled);
				} else if (in > 1500 && !g_scanning.load() && a_nowMs - g_scanAt.load() > 5000) {
					Scan(std::format("soldier {:#x} not found", slot.id));
				}
				break;
			}
			case Life::kActive: {
				if (!LocationFeet(rt.loc)) {
					release();
					enter(Life::kLocating);  // unrealized under us: find it again
					break;
				}
				if (cfg.proxies && !rt.asleep && a_nowMs - rt.ownedAt >= 1000) {
					// The pooled location object still carries him (20.6): else a reinforcement realized into it would be moved. Not
					// while asleep: MGSV's balloon carries him away from both, and losing the object would lose the fulton's yank.
					rt.ownedAt = a_nowMs;
					const auto cache = SoldierCache(slot.id);
					const auto feet = LocationFeet(rt.loc);
					rt.ownMiss = cache && feet && !plan::OwnsLocation(*cache, *feet, 5.0f) ? rt.ownMiss + 1 : 0;
					if (rt.ownMiss >= 3) {
						++g_ownLost;
						Log::Warn("proxy: soldier {:#x}'s position cache {:.1f},{:.1f},{:.1f} is {:.1f} m from his location object {:#x} for 3 s -- found again",
							slot.id, cache->x, cache->y, cache->z, (*cache - *feet).LengthXZ(), rt.loc);
						release();
						enter(Life::kLocating);
						break;
					}
				}
				if (inert && a_nowMs - rt.inertAt >= 2000) {
					// Refreshed (each is one flag bit in the instance table: idempotent). An asleep proxy keeps only restrict-notice:
					// a puppet ignores ChangeLifeState (ProxySleep lifted it).
					a_call(std::format("return PhantomRing.ProxyInert({}, {})", slot.id, rt.asleep ? "false" : "true"));
					rt.inertAt = a_nowMs;
				}
				const bool wantAsleep = (rt.recFlags & proto::kActorAsleep) != 0;
				if (wantAsleep && (!rt.asleep || a_nowMs - rt.sleptAt > 8000)) {
					a_call(std::format("return PhantomRing.{}({})", rt.asleep ? "FultonSleep" : "ProxySleep", slot.id));
					if (!rt.asleep) {
						Log::Info("proxy: soldier {:#x} asleep for Elden Ring character {:#x} (MGSV offers its Fulton beside him)", slot.id, slot.handle);
					}
					rt.asleep = true;
					rt.sleptAt = a_nowMs;
				} else if (!wantAsleep && rt.asleep) {
					// Elden Ring's character woke up: a fresh puppet (disabled now, enabled again next step).
					a_call(std::format("return PhantomRing.FultonDisable({})", slot.id));
					release();
					enter(Life::kDisabled);
					break;
				}
				if (rt.asleep && a_nowMs - rt.polledAt >= 150) {
					rt.polledAt = a_nowMs;
					const auto text = a_call(std::format("return PhantomRing.FultonSoldier({})", slot.id));
					float      x = 0, y = 0, z = 0;
					int        life = -1, st = -1, act = -1;
					const bool parsed = std::sscanf(text.c_str(), "%f,%f,%f,%d,%d,%d", &x, &y, &z, &life, &st, &act) == 6;
					rt.liftSeen = parsed && st == 0 && act == 1 ? rt.liftSeen + 1 : 0;
					if (rt.liftSeen >= 2) {
						SendFulton(slot.handle, true, "MGSV's own Fulton on its proxy", slot.real, "its proxy");
						Log::Info("proxy: MGSV's balloon took soldier {:#x} ({}) -- his slot gets a fresh soldier", slot.id, text);
						g_spentIds.push_back(slot.id);
						release();
						enter(Life::kSpent);
						slot.handle = 0;
						slot.proxy = false;
					}
				}
				break;
			}
			default:
				break;
			}
		}
	}

	std::string Fulton(std::uint64_t a_handle, bool a_ok, const char* a_why)
	{
		std::scoped_lock    lock(g_lock);
		std::optional<Vec3> at;
		const char*         from = "Snake's root";
		for (const auto& slot : g_slots) {
			if (slot.handle == a_handle && slot.proxy) {
				at = slot.real;  // the character's own place (the proxy may stand on the sandbox ground: Height = ground)
				from = "its proxy";
			}
		}
		return SendFulton(a_handle, a_ok, a_why, at, from);
	}

	bool ExcludesDraw(const float a_drawPos[3], const float a_snakeRoot[3])
	{
		if (!g_active.load(std::memory_order_relaxed)) {
			return false;
		}
		std::scoped_lock el(g_exclLock);
		return plan::NearProxy({ a_drawPos[0], a_drawPos[1], a_drawPos[2] }, { a_snakeRoot[0], a_snakeRoot[1], a_snakeRoot[2] }, g_excl.data(), g_exclCount,
			Cfg().proxyCaptureRadius);
	}

	bool NearAnyProxy(const float a_pos[3], float a_radius)
	{
		if (!g_active.load(std::memory_order_relaxed)) {
			return false;
		}
		std::scoped_lock el(g_exclLock);
		const Vec3       p{ a_pos[0], a_pos[1], a_pos[2] };
		for (std::size_t i = 0; i < g_exclCount; ++i) {
			const Vec3 d = p - g_excl[i];
			if (d.LengthXZ() <= a_radius && std::fabs(d.y) <= 2.5f) {
				return true;
			}
		}
		return false;
	}

	bool AutoAimWanted(bool a_savesGuarded)
	{
		const auto&      c = Cfg();
		std::scoped_lock lock(g_lock);
		return plan::AutoAimWanted(c.proxies && g_active.load(std::memory_order_relaxed), c.proxyAutoAim, a_savesGuarded, g_slots.size(), g_readOk, g_readBad);
	}

	bool Active() { return g_active.load(std::memory_order_relaxed); }

	bool AllPlaced()
	{
		std::scoped_lock lock(g_lock);
		return g_active.load(std::memory_order_relaxed) && !g_slots.empty() && g_readOk == g_slots.size() && g_readBad == 0;
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		std::size_t      locs = 0;
		std::uintptr_t   sys = 0;
		{
			std::scoped_lock sl(g_scanLock);
			locs = g_locs.size();
			sys = g_soldierSystem;
		}
		std::string out = std::format("proxies {}{} active {} pool {} soldiers ({} slots, {} reserve), table seq {} ({} records), writes {} ({} failed), where they "
		                              "should be {} ok {} not; {} scans ({} location objects, soldier system {:#x}); {} fulton events\n",
			Cfg().proxies ? "on" : "off", !Cfg().proxies && Cfg().FultonProxies() ? " (sleeping characters only: [Fulton] Method = proxy)" : "", g_active.load(),
			g_pool.soldiers.size(), g_slots.size(), g_reserve.size(), g_tableSeq, g_recordCount, g_writes, g_writeFails,
			g_readOk, g_readBad, g_scans.load(), locs, sys, g_fultonEvents);
		const auto k = g_k.Latest();
		out += std::format("treadmill offset k {:.2f},{:.2f},{:.2f}\n", k.x, k.y, k.z);
		const auto& c = Cfg();
		out += std::format("inert {} (sites {}), height {} (y tolerance {:.1f}), aim height fix {} ({} rounds turned), phase lock {}: phase {}, {} alerts, {} calms, "
		                   "{} sweeps{}, location objects lost {}\n",
			c.proxies && c.proxyInert, g_inertSites < 0 ? "unchecked" : g_inertSites ? "ok" : "MISMATCH", c.proxies ? c.proxyHeight : std::string("enemy (Fulton-only)"),
			c.proxyHeightTolerance, c.proxyAimHeightFix, g_shotsTurned.load(), c.proxyPhaseLock, g_lastPhase, g_phase.Alerts(), g_calms, g_sweeps,
			g_phase.GaveUp() ? " (gave up: full proxies off)" : "", g_ownLost);
		for (std::size_t i = 0; i < g_slots.size(); ++i) {
			const auto& s = g_slots[i];
			const auto& r = g_rt[i];
			out += std::format("  {:#06x} {} {:#x} at {:.2f},{:.2f},{:.2f} (enemy y {:.2f}) yaw {:.0f} -- {}{}{} loc {:#x}{}\n", s.id, s.proxy ? "proxy" : "free",
				s.handle, s.pos.x, s.pos.y, s.pos.z, s.real.y, s.yaw * 57.2958f, LifeName(r.life), r.asleep ? " asleep" : "", r.ok ? " placed" : "", r.loc,
				r.readBack ? " (where it should be)" : "");
		}
		return out;
	}

	void CorrectShot(const float a_origin[3], float a_dir[3])
	{
		const auto& c = Cfg();
		if (!c.proxies || !c.proxyAimHeightFix || !g_active.load(std::memory_order_relaxed)) {
			return;
		}
		std::array<plan::Body, kMaxExcl> bodies{};
		std::size_t                      n = 0;
		{
			std::scoped_lock el(g_exclLock);
			bodies = g_bodies;
			n = g_bodyCount;
		}
		const Vec3 origin{ a_origin[0], a_origin[1], a_origin[2] };
		Vec3       dir{ a_dir[0], a_dir[1], a_dir[2] };
		// 0.9 m around a soldier's axis, 2 m tall: aim assist puts the reticle on his chest; the radius takes a near miss too.
		const int i = plan::CorrectShot(origin, dir, bodies.data(), n, 0.9f, 2.0f, c.proxyMaxDistance + 20.0f);
		if (i < 0) {
			return;
		}
		const auto& b = bodies[static_cast<std::size_t>(i)];
		if ((b.real - b.placed).Length() < 0.05f) {
			return;  // at the enemy's height already (Height = enemy, or level ground): nothing to turn
		}
		const auto turned = g_shotsTurned.fetch_add(1) + 1;
		if (turned <= 24) {
			Log::Info("proxy: round aimed at a proxy turned to its enemy's height ({:+.2f} m): dir {:.3f},{:.3f},{:.3f} -> {:.3f},{:.3f},{:.3f}", b.real.y - b.placed.y,
				a_dir[0], a_dir[1], a_dir[2], dir.x, dir.y, dir.z);
		}
		a_dir[0] = dir.x;
		a_dir[1] = dir.y;
		a_dir[2] = dir.z;
	}
}
