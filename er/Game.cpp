#include "Game.h"

#include "Addresses.h"
#include "Mem.h"
#include "RootMotion.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
