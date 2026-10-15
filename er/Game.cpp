#include "Game.h"

#include "Addresses.h"
#include "Mem.h"
#include "RootMotion.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <string>

namespace phantom::er::game
{
	namespace
	{
		using SetPositionFn = void (*)(void* a_chr, const Vec4* a_pos);
		using SetYawFn = void (*)(void* a_chr, const float* a_yaw);
		using KillFn = void (*)(void* a_chr, bool a_flag);
		using CastRayFn = bool (*)(void* a_world, std::uint32_t a_filter, const Vec4* a_origin, const Vec4* a_extent, Vec4* a_hit, const void* a_ignore);
		using DrawLineFn = void (*)(void* a_ezDraw, const Vec4* a_from, const Vec4* a_to);

		const Addresses& addr() { return Addr(); }

		std::uint8_t* Field(std::uint8_t* a_base, std::uint32_t a_off) { return a_base ? SafePtr(a_base + a_off) : nullptr; }

		bool IsChrVtable(const void* a_vptr)
		{
			return a_vptr && (a_vptr == addr()[A::VtPlayerIns] || a_vptr == addr()[A::VtEnemyIns] || a_vptr == addr()[A::VtChrIns]);
		}

		// PlayerIns's ctor puts a PlayerCtrl at +0x58 (0x140650C90 -> 0x1403DDF10), not a plain ChrCtrl;
		// with motion = root_motion it carries our instance vtable copy.
		bool IsCtrl(const void* a_object)
		{
			const auto vptr = a_object ? SafeRead<const void*>(a_object) : std::nullopt;
			return vptr && *vptr &&
			       (*vptr == addr()[A::VtPlayerCtrl] || *vptr == addr()[A::VtChrCtrl] || *vptr == RootMotion::FakeVtable());
		}
	}

	std::uint8_t* WorldChrMan()
	{
		auto* wcm = addr().Instance(A::WorldChrMan);
		return HasVtable(wcm, addr()[A::VtWorldChrManImp]) ? wcm : nullptr;
	}

	Player MainPlayer()
	{
		Player p;
		auto*  wcm = WorldChrMan();
		auto*  chr = Field(wcm, kWcmMainPlayer);
		if (!HasVtable(chr, addr()[A::VtPlayerIns])) {
			return {};
		}
		auto* modules = Field(chr, kChrModules);
		auto* data = Field(modules, kModData);
		auto* phys = Field(modules, kModPhysics);
		auto* ctrl = Field(chr, kChrCtrl);
		if (!HasVtable(data, addr()[A::VtData]) || !HasVtable(phys, addr()[A::VtPhysics]) || !IsCtrl(ctrl)) {
			return {};
		}
		p.chr = chr;
		p.modules = modules;
		p.data = data;
		p.phys = phys;
		p.ctrl = ctrl;
		return p;
	}

	int LoadStatus(const Player& a_player)
	{
		auto*      entry = Field(a_player.chr, kChrSetEntry);
		const auto status = entry ? SafeRead<std::uint8_t>(entry + 8) : std::nullopt;
		return status ? *status : -1;
	}

	bool IsActive(const Player& a_player)
	{
		// 2 per fromsoftware-rs; 4 VERIFIED live (2026-10-12) for the main player AND the NPCs around him, in the
		// world after a load and after fast travel, in a session where 2 was never seen (the 15 s "never reports
		// Active" fallback then delayed Snake's start). Either is "in the world"; the loading screen itself is a
		// blocking menu (er/Host.cpp).
		const int status = LoadStatus(a_player);
		return status == 2 || status == 4;
	}

	bool IsDead(const Player& a_player) { return a_player && (a_player.chr[kChrFlags1C5] & 0x80) != 0; }

	Vec3 Position(const Player& a_player)
	{
		const auto* p = reinterpret_cast<const float*>(a_player.phys + kPhysPosition);
		return { p[0], p[1], p[2] };
	}

	Quat Orientation(const Player& a_player)
	{
		const auto* q = reinterpret_cast<const float*>(a_player.phys + kPhysOrientation);
		return { q[0], q[1], q[2], q[3] };
	}

	std::int32_t Hp(const Player& a_player) { return *reinterpret_cast<const std::int32_t*>(a_player.data + kDataHp); }
	std::int32_t MaxHp(const Player& a_player) { return *reinterpret_cast<const std::int32_t*>(a_player.data + kDataMaxHp); }
	void         SetHp(const Player& a_player, std::int32_t a_hp) { *reinterpret_cast<std::int32_t*>(a_player.data + kDataHp) = a_hp; }
	std::uint64_t LastHitBy(const Player& a_player) { return *reinterpret_cast<const std::uint64_t*>(a_player.chr + kChrLastHitBy); }

	bool SetPosition(const Player& a_player, const Vec3& a_pos)
	{
		if (!a_player || !addr().Has(A::ChrInsSetPosition) || !Finite(a_pos)) {
			return false;
		}
		// Keep the w the game had (Havok vectors carry it along; its meaning is [U]).
		const float w = reinterpret_cast<const float*>(a_player.phys + kPhysPosition)[3];
		const Vec4  pos(a_pos, w);
		addr().Fn<SetPositionFn>(A::ChrInsSetPosition)(a_player.chr, &pos);
		return true;
	}

	bool SetYaw(const Player& a_player, float a_yaw)
	{
		if (!a_player || !addr().Has(A::ChrInsSetYaw) || !std::isfinite(a_yaw)) {
			return false;
		}
		// The argument is an Euler triple about Elden Ring's X, Y, Z axes (0x461F60 multiplies [0], [1], [2] by 0.5 and
		// rotates about the unit vectors at 0x1448062A0 / 0x1448062D0 / 0x1448064E0, read live: (1,0,0), (0,1,0), (0,0,1)).
		// The yaw is [1]. Passed in [0] it pitched the puppet about X instead (live: physics quaternion (0.47, 0, 0, 0.88) for
		// a 56 deg "yaw"), so prompts that check the player's facing never showed while Snake played ("Traverse the mist").
		alignas(16) float euler[4] = { 0, a_yaw, 0, 0 };
		addr().Fn<SetYawFn>(A::ChrInsSetYaw)(a_player.chr, euler);
		return true;
	}

	bool Kill(const Player& a_player)
	{
		if (!a_player || !addr().Has(A::ChrInsKill)) {
			return false;
		}
		addr().Fn<KillFn>(A::ChrInsKill)(a_player.chr, false);
		return true;
	}

	bool GetBit(std::uint8_t* a_byte, std::uint8_t a_mask)
	{
		const auto v = SafeRead<std::uint8_t>(a_byte);
		return v && (*v & a_mask) != 0;
	}

	bool SetBit(std::uint8_t* a_byte, std::uint8_t a_mask, bool a_on)
	{
		const auto v = SafeRead<std::uint8_t>(a_byte);
		if (!v) {
			return false;
		}
		const bool was = (*v & a_mask) != 0;
		if (was != a_on) {
			SafeWrite<std::uint8_t>(a_byte, static_cast<std::uint8_t>(a_on ? (*v | a_mask) : (*v & ~a_mask)));
		}
		return was;
	}

	namespace
	{
		std::uint8_t* PhysWorld()
		{
			auto* havok = addr().Instance(A::CSHavokMan);
			if (!HasVtable(havok, addr()[A::VtHavokManImp])) {
				return nullptr;
			}
			auto* world = Field(havok, 0x98);  // [V] 246 code refs
			return HasVtable(world, addr()[A::VtPhysWorld]) ? world : nullptr;
		}
	}

	bool CanCastRay() { return addr().HasAll(A::CastRay, A::CSHavokMan, A::VtHavokManImp, A::VtPhysWorld) && PhysWorld(); }

	bool CastRay(const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit, const std::uint8_t* a_ignore, std::uint32_t a_filter)
	{
		if (!addr().Has(A::CastRay) || !Finite(a_from) || !Finite(a_delta)) {
			return false;
		}
		auto* world = PhysWorld();
		if (!world) {
			return false;
		}
		const Vec4 origin(a_from, 1.0f);
		const Vec4 extent(a_delta, 0.0f);
		Vec4       hit;
		if (!addr().Fn<CastRayFn>(A::CastRay)(world, a_filter, &origin, &extent, &hit, a_ignore)) {
			return false;
		}
		a_hit = hit.xyz();
		return Finite(a_hit);
	}

	namespace
	{
		// hknpRayCastQuery as cast_ray (0xC71E00) builds it at rsp+0x40 (A::CastRayQuery / A::CastRayCalls verify the bytes):
		// the ray's temporaries it leaves behind it (to at +0x70, from at +0x80) are copied too, so the query is byte for byte
		// the game's.
		struct alignas(16) HkRayQuery
		{
			void*         codec{ nullptr };     // +0x00 [hknpWorld+0xB00]
			void*         filter{ nullptr };    // +0x08 [CSHavokMan+0xC0] (CS::CSCollisionFilter)
			std::uint16_t material{ 0xFFFF };   // +0x10
			std::uint16_t pad12{ 0 };
			std::uint32_t filterInfo{ 0 };      // +0x14 the query's collision filter info
			const void*   userData{ nullptr };  // +0x18 the ignored character
			std::uint32_t v20{ 2 };             // +0x20
			std::uint8_t  v24{ 0xFB };          // +0x24
			std::uint8_t  pad25[11]{};
			Vec4          origin;               // +0x30
			Vec4          dir;                  // +0x40 (w 1)
			Vec4          invDir;               // +0x50 (a 0 component: 3.40282e38)
			std::uint32_t v60{ 0 };             // +0x60
			std::uint8_t  pad64[12]{};
			Vec4          to;                   // +0x70
			Vec4          from;                 // +0x80
		};
		static_assert(sizeof(HkRayQuery) == 0x90 && offsetof(HkRayQuery, origin) == 0x30 && offsetof(HkRayQuery, v60) == 0x60);

		using WorldCastRayFn = void (*)(void* a_hknpWorld, HkRayQuery* a_query, void* a_collector);
		using CollectorFn = void (*)(void* a_collector);
		using AddHitFn = void (*)(void* a_collector, const std::uint8_t* a_hit);
		using HitMaterialFn = bool (*)(const std::uint32_t* a_body, std::uint32_t a_shapeKey, std::int32_t* a_out, char a_remap);

		// What KeepAddHit does on this thread's cast: keep the closest hit with these material classes kept (bit c), or list
		// every hit.
		struct RayMode
		{
			std::uint32_t keep{ 0 };
			RayHit*       all{ nullptr };
			int           max{ 0 };
			int           count{ 0 };
		};
		thread_local RayMode t_rayMode;

		// The snipe collector's vtable with addHit (slot 4) replaced; [0] = its COL (vtable[-1]) for anything that reads RTTI.
		void* g_keepVtable[7]{};
		int   g_keepState{ 0 };  // 0 not tried, 1 ready, -1 refused

		std::int32_t HitMaterial(const std::uint8_t* a_hit)
		{
			std::uint32_t body = 0, key = 0;
			std::memcpy(&body, a_hit + 0x48, 4);
			std::memcpy(&key, a_hit + 0x50, 4);
			if ((body & 0xFFFFFF) == 0xFFFFFF) {
				return -1;
			}
			std::int32_t id = -1;
			return addr().Fn<HitMaterialFn>(A::HitMaterialClass)(&body, key, &id, 1) ? id : -1;
		}

		void KeepAddHit(void* a_collector, const std::uint8_t* a_hit)
		{
			auto& m = t_rayMode;
			if (m.all) {
				// Every hit: the collector's early-out is left alone, so the query goes on past this one.
				if (m.count < m.max) {
					RayHit& h = m.all[m.count];
					Vec4    p, n;
					std::memcpy(&p, a_hit, 16);
					std::memcpy(&n, a_hit + 0x10, 16);
					h.pos = p.xyz();
					h.normal = n.xyz();
					std::memcpy(&h.fraction, a_hit + 0x20, 4);
					std::memcpy(&h.body, a_hit + 0x48, 4);
					std::memcpy(&h.hkMaterial, a_hit + 0x4C, 2);
					std::memcpy(&h.shapeKey, a_hit + 0x50, 4);
					std::memcpy(&h.shapeFilter, a_hit + 0x54, 4);
					h.material = HitMaterial(a_hit);
				}
				++m.count;
				return;
			}
			const std::int32_t id = HitMaterial(a_hit);
			const int          cls = id >= 0 ? id / 100 : -1;
			if ((cls == 2 || cls == 5) && (m.keep >> cls & 1u)) {
				addr().Fn<AddHitFn>(A::ClosestAddHit)(a_collector, a_hit);  // the closest, as the game's would without the drop
				return;
			}
			addr().Fn<AddHitFn>(A::SnipeAddHit)(a_collector, a_hit);  // the game's own: mount skip, material drop, closest
		}

		bool KeepReady()
		{
			if (g_keepState == 0) {
				g_keepState = -1;
				const auto& a = addr();
				if (a.HasAll(A::CastRayQuery, A::CastRayCalls, A::WorldCastRay, A::SnipeCollectorCtor, A::SnipeCollectorDtor, A::SnipeAddHit,
						A::ClosestAddHit, A::HitMaterialClass, A::VtSnipeCollector, A::CSHavokMan)) {
					auto* const* vt = reinterpret_cast<void* const*>(a[A::VtSnipeCollector]);
					// The vtable's addHit must be the function checked above (the slot is the one the world cast calls).
					if (vt[4] == a[A::SnipeAddHit]) {
						g_keepVtable[0] = vt[-1];
						for (int i = 0; i < 6; ++i) {
							g_keepVtable[1 + i] = vt[i];
						}
						g_keepVtable[1 + 4] = reinterpret_cast<void*>(&KeepAddHit);
						g_keepState = 1;
					}
				}
			}
			return g_keepState == 1;
		}

		// cast_ray's body with our collector vtable. Returns hits (the collector's +0xC), -1 if it couldn't cast.
		int RunKeepQuery(const Vec3& a_from, const Vec3& a_delta, const std::uint8_t* a_ignore, std::uint32_t a_filter, Vec3* a_hit, std::uint8_t a_layers)
		{
			if (!KeepReady() || !Finite(a_from) || !Finite(a_delta)) {
				return -1;
			}
			auto* world = PhysWorld();
			auto* havok = addr().Instance(A::CSHavokMan);
			auto* hknp = world ? SafePtr(world + 0x8) : nullptr;
			const auto codec = hknp ? SafeRead<void*>(hknp + 0xB00) : std::nullopt;
			const auto filter = havok ? SafeRead<void*>(havok + 0xC0) : std::nullopt;
			if (!hknp || !codec || !filter || !*filter) {
				return -1;
			}
			HkRayQuery q;
			q.codec = *codec;
			q.filter = *filter;
			q.filterInfo = a_filter;
			q.userData = a_ignore;
			q.v24 = a_layers;
			q.from = Vec4(a_from, 1.0f);
			q.to = Vec4(a_from + a_delta, 1.0f);
			q.origin = q.from;
			const float d[3] = { q.to.x - q.from.x, q.to.y - q.from.y, q.to.z - q.from.z };
			q.dir = Vec4(d[0] == 0.0f ? 0.0f : d[0], d[1] == 0.0f ? 0.0f : d[1], d[2] == 0.0f ? 0.0f : d[2], 1.0f);
			const auto inv = [](float a_v) { return a_v == 0.0f ? 3.40282e38f : 1.0f / a_v; };
			q.invDir = Vec4(inv(q.dir.x), inv(q.dir.y), inv(q.dir.z), 1.0f);
			alignas(16) std::uint8_t collector[0x100]{};
			addr().Fn<CollectorFn>(A::SnipeCollectorCtor)(collector);
			std::memcpy(collector + 0xA0, &a_ignore, sizeof(a_ignore));  // 0xC5ABE0
			void* const vptr = &g_keepVtable[1];
			std::memcpy(collector, &vptr, sizeof(vptr));
			addr().Fn<WorldCastRayFn>(A::WorldCastRay)(hknp, &q, collector);
			std::int32_t hits = 0;
			std::memcpy(&hits, collector + 0xC, 4);
			if (hits != 0 && a_hit) {
				Vec4 p;
				std::memcpy(&p, collector + 0x30, 16);
				*a_hit = p.xyz();
			}
			void* const snipe = addr()[A::VtSnipeCollector];
			std::memcpy(collector, &snipe, sizeof(snipe));
			addr().Fn<CollectorFn>(A::SnipeCollectorDtor)(collector);
			return hits;
		}
	}

	bool CanCastRayKeep() { return CanCastRay() && KeepReady(); }

	bool CastRayKeep(const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit, const std::uint8_t* a_ignore, std::uint32_t a_filter, std::uint32_t a_keep,
		std::uint8_t a_layers)
	{
		if ((a_keep == 0 && a_layers == kCastRayLayers) || !KeepReady()) {
			return CastRay(a_from, a_delta, a_hit, a_ignore, a_filter);
		}
		const RayMode saved = t_rayMode;
		t_rayMode = RayMode{ a_keep };
		Vec3      hit;
		const int hits = RunKeepQuery(a_from, a_delta, a_ignore, a_filter, &hit, a_layers);
		t_rayMode = saved;
		if (hits < 0) {
			return CastRay(a_from, a_delta, a_hit, a_ignore, a_filter);
		}
		if (hits == 0 || !Finite(hit)) {
			return false;
		}
		a_hit = hit;
		return true;
	}

	int CastRayAll(const Vec3& a_from, const Vec3& a_delta, const std::uint8_t* a_ignore, std::uint32_t a_filter, RayHit* a_out, int a_max,
		std::uint8_t a_layers)
	{
		const RayMode saved = t_rayMode;
		t_rayMode = RayMode{ 0, a_out, std::max(0, a_max), 0 };
		const int ran = RunKeepQuery(a_from, a_delta, a_ignore, a_filter, nullptr, a_layers);
		const int count = t_rayMode.count;
		t_rayMode = saved;
		if (ran < 0) {
			return -1;
		}
		std::sort(a_out, a_out + std::min(count, std::max(0, a_max)), [](const RayHit& a, const RayHit& b) { return a.fraction < b.fraction; });
		return count;
	}

	std::string BroadPhaseLayers()
	{
		auto* world = PhysWorld();
		auto* hknp = world ? SafePtr(world + 0x8) : nullptr;
		auto* bp = hknp ? SafePtr(hknp + 0x4D8) : nullptr;
		const auto layers = bp ? SafeRead<std::uint8_t*>(bp + 0x10) : std::nullopt;
		const auto count = bp ? SafeRead<std::int32_t>(bp + 0x18) : std::nullopt;
		if (!layers || !*layers || !count || *count <= 0 || *count > 32) {
			return "unknown";
		}
		std::string out;
		for (int i = 0; i < *count; ++i) {
			auto*      layer = SafePtr(*layers + i * 8);
			const auto mask = layer ? SafeRead<std::uint8_t>(layer + 0x19D0) : std::nullopt;
			out += std::format("{}{}:{}", out.empty() ? "" : " ", i, mask ? std::format("{:#04x}", *mask) : std::string("?"));
		}
		return out;
	}

	std::uint8_t* PersCam1()
	{
		auto* camera = addr().Instance(A::CSCamera);
		if (!HasVtable(camera, addr()[A::VtCameraImp])) {
			return nullptr;
		}
		auto*      cam = Field(camera, kCameraPersCam1);
		const auto vptr = cam ? SafeRead<void*>(cam) : std::nullopt;
		if (!vptr) {
			return nullptr;
		}
		const auto& a = addr();
		const bool  known = *vptr == a[A::VtPersCam] || *vptr == a[A::VtCSCam] || *vptr == a[A::VtChrCam] || *vptr == a[A::VtDebugCam];
		return known ? cam : nullptr;
	}

	std::uint8_t* ChrCamObject()
	{
		auto* cam = Field(WorldChrMan(), kWcmChrCam);
		return HasVtable(cam, addr()[A::VtChrCam]) ? cam : nullptr;
	}

	std::uint8_t* GameRendCam()
	{
		if (!addr().HasAll(A::FieldArea, A::VtGameRend, A::VtPersCam)) {
			return nullptr;
		}
		auto* rend = Field(addr().Instance(A::FieldArea), kFieldAreaGameRend);
		if (!HasVtable(rend, addr()[A::VtGameRend])) {
			return nullptr;
		}
		auto* cam = Field(rend, kGameRendCompassCam);
		return HasVtable(cam, addr()[A::VtPersCam]) ? cam : nullptr;
	}

	std::uint8_t* EzDraw()
	{
		if (!addr().HasAll(A::EzDrawLine, A::VtEzDraw, A::VtRendManImp)) {
			return nullptr;
		}
		auto* rend = addr().Instance(A::RendMan);
		if (!HasVtable(rend, addr()[A::VtRendManImp])) {
			return nullptr;
		}
		auto* ez = Field(rend, kRendDebugEzDraw);
		return HasVtable(ez, addr()[A::VtEzDraw]) ? ez : nullptr;
	}

	bool DrawLine(const Vec3& a_from, const Vec3& a_to)
	{
		auto* ez = EzDraw();
		if (!ez || !Finite(a_from) || !Finite(a_to)) {
			return false;
		}
		const Vec4 from(a_from, 1.0f);
		const Vec4 to(a_to, 1.0f);
		addr().Fn<DrawLineFn>(A::EzDrawLine)(ez, &from, &to);
		return true;
	}

	void SetDrawColor(float a_r, float a_g, float a_b, float a_a)
	{
		// fromsoftware-rs CSEzDraw::set_color: current command buffer (+0x10 + 8*index, index at
		// +0x20) -> FD4HkEzDrawState (+0x40): line colour +0x40, fill colour +0x50, flags +0x10
		// (bit 8 line, bit 9 fill). [V-src]
		auto* ez = EzDraw();
		if (!ez) {
			return;
		}
		const auto index = SafeRead<std::uint32_t>(ez + 0x20);
		if (!index || *index > 1) {
			return;
		}
		auto* buffer = SafePtr(ez + 0x10 + 8 * *index);
		auto* state = buffer ? SafePtr(buffer + 0x40) : nullptr;
		const auto flags = state ? SafeRead<std::uint32_t>(state + 0x10) : std::nullopt;
		if (!flags) {
			return;
		}
		const Vec4 color(a_r, a_g, a_b, a_a);
		SafeWriteBytes(state + 0x40, &color, sizeof(color));
		SafeWriteBytes(state + 0x50, &color, sizeof(color));
		SafeWrite<std::uint32_t>(state + 0x10, *flags | (1u << 8) | (1u << 9));
	}

	Player ChrAsPlayer(std::uint8_t* a_chr)
	{
		const auto vptr = a_chr ? SafeRead<void*>(a_chr) : std::nullopt;
		if (!vptr || !IsChrVtable(*vptr)) {
			return {};
		}
		auto* modules = Field(a_chr, kChrModules);
		auto* data = Field(modules, kModData);
		auto* phys = Field(modules, kModPhysics);
		auto* ctrl = Field(a_chr, kChrCtrl);
		if (!HasVtable(data, addr()[A::VtData]) || !HasVtable(phys, addr()[A::VtPhysics]) || !ctrl) {
			return {};
		}
		Player p;
		p.chr = a_chr;
		p.modules = modules;
		p.data = data;
		p.phys = phys;
		p.ctrl = ctrl;
		return p;
	}

	std::vector<Character> Characters(std::size_t a_max, bool a_includeDisabled)
	{
		std::vector<Character> out;
		auto*                  wcm = WorldChrMan();
		if (!wcm) {
			return out;
		}
		auto*      first = SafePtr(wcm + kWcmByDistance + 0x8);
		auto*      last = SafePtr(wcm + kWcmByDistance + 0x10);
		if (!first || !last || last < first) {
			return out;
		}
		const auto count = std::min<std::size_t>(static_cast<std::size_t>(last - first) / 0x10, std::min<std::size_t>(a_max, 1024));
		for (std::size_t i = 0; i < count; ++i) {
			Character c;
			c.chr = SafePtr(first + i * 0x10);
			const auto dist = SafeRead<float>(first + i * 0x10 + 8);
			const auto vptr = c.chr ? SafeRead<void*>(c.chr) : std::nullopt;
			if (!vptr || !IsChrVtable(*vptr)) {
				continue;
			}
			auto* modules = Field(c.chr, kChrModules);
			auto* data = Field(modules, kModData);
			auto* phys = Field(modules, kModPhysics);
			if (!HasVtable(data, addr()[A::VtData]) || !HasVtable(phys, addr()[A::VtPhysics])) {
				continue;
			}
			c.isPlayer = *vptr == addr()[A::VtPlayerIns];
			c.isEnemy = *vptr == addr()[A::VtEnemyIns];
			c.handle = SafeRead<std::uint64_t>(c.chr + kChrHandle).value_or(0);
			c.team = SafeRead<std::uint8_t>(c.chr + kChrTeam).value_or(0);
			c.npcParam = SafeRead<std::int32_t>(c.chr + kChrNpcParam).value_or(0);
			c.chrType = SafeRead<std::int32_t>(c.chr + kChrType).value_or(0);
			c.dead = (SafeRead<std::uint8_t>(c.chr + kChrFlags1C5).value_or(0) & 0x80) != 0;
			c.disabled = (SafeRead<std::uint32_t>(c.chr + kChrDebugFlags).value_or(0) & kChrDebugDisabled) != 0;
			if (c.disabled && !a_includeDisabled) {
				continue;
			}
			c.hp = SafeRead<std::int32_t>(data + kDataHp).value_or(0);
			c.maxHp = SafeRead<std::int32_t>(data + kDataMaxHp).value_or(0);
			float v[8]{};
			if (SafeReadBytes(phys + kPhysOrientation, v, sizeof(v))) {
				c.orientation = { v[0], v[1], v[2], v[3] };
			}
			if (SafeReadBytes(phys + kPhysPosition, v, 12)) {
				c.pos = { v[0], v[1], v[2] };
			}
			c.distance = dist.value_or(0);
			out.push_back(c);
		}
		return out;
	}

	std::vector<std::uint8_t> UiStates()
	{
		std::vector<std::uint8_t> states;
		auto*                     menu = addr().Instance(A::CSMenuMan);
		if (!HasVtable(menu, addr()[A::VtMenuManImp])) {
			return states;
		}
		states.resize(kMenuUiStateCount);
		if (!SafeReadBytes(menu + kMenuUiStates, states.data(), states.size())) {
			states.clear();
		}
		return states;
	}

	std::optional<bool> ModalMenuOpen()
	{
		auto* menu = addr().Instance(A::CSMenuMan);
		if (!HasVtable(menu, addr()[A::VtMenuManImp])) {
			return std::nullopt;
		}
		const auto blocking = SafeRead<std::uint64_t>(menu + kMenuBlocking);
		return blocking ? std::optional<bool>(*blocking != 0) : std::nullopt;
	}

	std::optional<bool> CutscenePlaying()
	{
		auto* remo = addr().Instance(A::CSRemo);
		if (!remo) {
			return std::nullopt;
		}
		auto* playing = SafePtr(remo + 8);
		if (!playing) {
			return false;
		}
		const auto flags = SafeRead<std::uint32_t>(playing + 0x14C);
		return flags ? std::optional<bool>((*flags & 1) != 0) : std::nullopt;
	}

	std::int32_t ActionPromptParam()
	{
		auto* man = addr().Instance(A::CSActionButtonMan);
		if (!HasVtable(man, addr()[A::VtActionButtonManImp])) {
			return -1;
		}
		auto* entry = SafePtr(man + kActionPresented);
		const auto param = entry ? SafeRead<std::int32_t>(entry + 0x08) : std::nullopt;
		return param ? std::max(0, *param) : -1;
	}

	std::optional<Vec3> ChrPosition(const std::uint8_t* a_chr)
	{
		const auto vptr = a_chr ? SafeRead<void*>(a_chr) : std::nullopt;
		if (!vptr || !IsChrVtable(*vptr)) {
			return std::nullopt;
		}
		auto* phys = Field(Field(const_cast<std::uint8_t*>(a_chr), kChrModules), kModPhysics);
		float v[3]{};
		if (!HasVtable(phys, addr()[A::VtPhysics]) || !SafeReadBytes(phys + kPhysPosition, v, sizeof(v))) {
			return std::nullopt;
		}
		return Vec3{ v[0], v[1], v[2] };
	}

	float TimeOfDay()
	{
		// WorldAreaTime: DLDateTime clock at +0 {u64 FILETIME, u64 packed date}; packed: hours bits
		// 34-38, minutes 39-44, seconds 45-50 (fromsoftware-rs dlut/date_time.rs) [V-src].
		auto*      time = addr().Instance(A::WorldAreaTime);
		const auto packed = time ? SafeRead<std::uint64_t>(time + 8) : std::nullopt;
		if (!packed) {
			return NAN;
		}
		const auto h = static_cast<unsigned>((*packed >> 34) & 0x1F);
		const auto m = static_cast<unsigned>((*packed >> 39) & 0x3F);
		const auto s = static_cast<unsigned>((*packed >> 45) & 0x3F);
		if (h >= 24 || m >= 60 || s >= 60) {
			return NAN;
		}
		return static_cast<float>(h) + static_cast<float>(m) / 60.0f + static_cast<float>(s) / 3600.0f;
	}

	std::string RttiNameOfVtable(const void* a_vtable)
	{
		const auto& a = addr();
		if (!a.InImage(a_vtable)) {
			return {};
		}
		auto*      col = SafePtr(static_cast<const std::uint8_t*>(a_vtable) - 8);
		const auto td = a.InImage(col) ? SafeRead<std::uint32_t>(col + 0xC) : std::nullopt;
		if (!td || *td >= a.size) {
			return {};
		}
		char mangled[160]{};
		if (!SafeReadBytes(a.base + *td + 0x10, mangled, sizeof(mangled) - 1)) {
			return {};
		}
		std::string_view m(mangled, strnlen(mangled, sizeof(mangled) - 1));
		if (!m.starts_with(".?AV") && !m.starts_with(".?AU")) {
			return std::string(m);
		}
		m.remove_prefix(4);
		if (m.starts_with("?$")) {
			return std::string(m);  // template: leave mangled
		}
		// "PlayerIns@CS@@" -> "CS::PlayerIns"
		std::vector<std::string_view> parts;
		while (!m.empty() && m.front() != '@') {
			const auto at = m.find('@');
			parts.push_back(m.substr(0, at));
			if (at == std::string_view::npos) {
				break;
			}
			m.remove_prefix(at + 1);
		}
		std::string out;
		for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
			if (!out.empty()) {
				out += "::";
			}
			out += *it;
		}
		return out;
	}

	namespace
	{
		// A module of the player, read only once its vtable is the one whose RTTI name was checked (cached per name).
		std::uint8_t* CheckedModule(const Player& a_player, std::uint32_t a_offset, const void*& a_vt, std::string_view a_name)
		{
			auto*      module = a_player ? Field(a_player.modules, a_offset) : nullptr;
			const auto vptr = module ? SafeRead<const void*>(module) : std::nullopt;
			if (!vptr) {
				return nullptr;
			}
			if (!a_vt && RttiNameOfVtable(*vptr) == a_name) {
				a_vt = *vptr;
			}
			return *vptr == a_vt ? module : nullptr;
		}

		const void* s_rideVt = nullptr;
		const void* s_ladderVt = nullptr;
	}

	std::optional<int> LadderState(const Player& a_player)
	{
		auto* ladder = CheckedModule(a_player, kModLadder, s_ladderVt, "CS::CSPlayerLadderModule");
		const auto state = ladder ? SafeRead<std::int8_t>(ladder + kLadderState) : std::nullopt;
		if (!state) {
			return std::nullopt;
		}
		return static_cast<int>(*state);
	}

	Mounted PlayerMounted(const Player& a_player)
	{
		if (!a_player) {
			return Mounted::kUnknown;
		}
		auto* ride = CheckedModule(a_player, kModRide, s_rideVt, "CS::CSChrRideModule");
		const auto state = LadderState(a_player);
		if (!ride || !state) {
			return Mounted::kUnknown;
		}
		const auto mounting = SafeRead<std::uint8_t>(ride + kRideMounting);
		const auto mounted = SafeRead<std::uint8_t>(ride + kRideMounted);
		if (!mounting || !mounted) {
			return Mounted::kUnknown;
		}
		if (*mounting || *mounted) {
			return Mounted::kTorrent;
		}
		return *state != -1 ? Mounted::kLadder : Mounted::kNone;
	}

	std::string RttiName(const void* a_object)
	{
		const auto vptr = SafeRead<const void*>(a_object);
		return vptr ? RttiNameOfVtable(*vptr) : std::string();
	}
}
