#include "Hold.h"

#include "Addresses.h"
#include "Hook.h"
#include "Log.h"
#include "Verify.h"

#include <windows.h>

#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>
#include <string>

namespace phantom::guest::hold
{
	namespace
	{
		bool           g_ok{ false };
		std::uintptr_t g_lastLoc{ 0 };
		bool           g_warned{ false };

		template <class T>
		std::optional<T> Get(std::uintptr_t a_at)
		{
			return SafeRead<T>(reinterpret_cast<const void*>(a_at));
		}

		bool InImage(std::uintptr_t a_p) { return a_p >= addr::kImageBase && a_p < addr::kImageBase + addr::kSizeOfImage; }

		void Fail(const char* a_why)
		{
			if (!g_warned) {
				g_warned = true;
				Log::Warn("hold: native placement unavailable ({}) -- using the Lua warp", a_why);
			}
		}

		struct Resolved
		{
			std::uintptr_t mgr{ 0 };
			std::uintptr_t getLoc{ 0 };
			std::uint16_t  obj{ 0xFFFF };
			std::uint8_t   slot{ 0xFF };
		};

		// Replays the resolution in Player2SystemImpl::Warp for the local player: its location manager and GameObject id.
		bool ResolveMgr(Resolved& a_out)
		{
			const auto impl = Get<std::uintptr_t>(addr::kPlayer2System + 8);
			if (!impl || !*impl || Get<std::uintptr_t>(*impl) != addr::kPlayer2ImplVtable) {
				Fail("Player2System impl vtable");
				return false;
			}
			const auto p = *impl;
			const auto idx = Get<std::uint32_t>(p + 0x214);
			const auto slots = Get<std::uintptr_t>(p + 0x78);
			const auto objs = Get<std::uintptr_t>(p + 0x70);
			const auto insts = Get<std::uintptr_t>(p + 0x378);
			if (!idx || *idx >= 16 || !slots || !objs || !insts || !*insts) {
				Fail("player tables");
				return false;
			}
			const auto slot = Get<std::uint8_t>(*slots + *idx);
			const auto obj = Get<std::uint16_t>(*objs + 2 * *idx);
			if (!slot || *slot == 0xFF || !obj || *obj == 0xFFFF) {
				return false;  // no player instance right now (loading)
			}
			const auto inst = *insts + static_cast<std::uintptr_t>(*slot) * 0x6A8;
			const auto a = Get<std::uintptr_t>(inst + 0x78);
			const auto mgr = a ? Get<std::uintptr_t>(*a + 0x238) : std::nullopt;
			const auto mgrVt = mgr && *mgr ? Get<std::uintptr_t>(*mgr) : std::nullopt;
			const auto getLoc = mgrVt && InImage(*mgrVt) ? Get<std::uintptr_t>(*mgrVt + 0x388) : std::nullopt;
			if (!getLoc || !InImage(*getLoc)) {
				Fail("location manager");
				return false;
			}
			a_out = { *mgr, *getLoc, *obj, *slot };
			return true;
		}

		std::uintptr_t LocOf(const Resolved& a_r, std::uint16_t a_id)
		{
			using GetLocFn = std::uintptr_t (*)(std::uintptr_t, std::uint32_t);
			const auto loc = reinterpret_cast<GetLocFn>(a_r.getLoc)(a_r.mgr, a_id);
			const auto locVt = loc ? Get<std::uintptr_t>(loc) : std::nullopt;
			return locVt && InImage(*locVt) ? loc : 0;
		}

		std::uintptr_t ResolveLoc()
		{
			Resolved r;
			if (!ResolveMgr(r)) {
				return 0;
			}
			const auto loc = LocOf(r, r.obj);
			if (!loc) {
				Fail("location object");
				return 0;
			}
			if (loc != g_lastLoc) {
				g_lastLoc = loc;
				Log::Info("hold: player location object {:#x} (vtable {:#x}, mgr {:#x}, obj {}, slot {})", loc, *Get<std::uintptr_t>(loc), r.mgr, r.obj, r.slot);
			}
			return loc;
		}

		bool SetLoc(std::uintptr_t a_loc, const logic::Vec3& a_pos, const std::array<float, 4>& a_rot)
		{
			const auto vt = a_loc ? Get<std::uintptr_t>(a_loc) : std::nullopt;
			const auto set = vt ? Get<std::uintptr_t>(*vt + 0x68) : std::nullopt;
			if (!set || !InImage(*set)) {
				return false;
			}
			alignas(16) float pos[4] = { a_pos.x, a_pos.y, a_pos.z, 0.0f };
			alignas(16) float rot[4] = { a_rot[0], a_rot[1], a_rot[2], a_rot[3] };
			using SetFn = void (*)(std::uintptr_t, const float*, const float*);
			reinterpret_cast<SetFn>(*set)(a_loc, pos, rot);
			return true;
		}
	}

	bool Init()
	{
		g_ok = Matches(addr::kPlayer2SystemAccessor, "Player2System accessor") && Matches(addr::kPlayer2Warp, "Player2SystemImpl::Warp") &&
		       Matches(addr::kPlayer2WarpLocCall, "Warp: location lookup") && Matches(addr::kPlayer2WarpSetCall, "Warp: set position") &&
		       Get<std::uintptr_t>(addr::kPlayer2ImplVtable + 54 * 8) == addr::kPlayer2Warp.va;
		if (!g_ok) {
			Log::Warn("hold: Warp internals differ -- native hold modes disabled");
		}
		return g_ok;
	}

	bool Available() { return g_ok; }

	bool Place(const logic::Vec3& a_pos, const std::array<float, 4>& a_rot)
	{
		if (!g_ok || !a_pos.Finite()) {
			return false;
		}
		return SetLoc(ResolveLoc(), a_pos, a_rot);
	}

	std::uintptr_t LocationOf(std::uint16_t a_id)
	{
		Resolved r;
		if (!g_ok || a_id == 0xFFFF || !ResolveMgr(r)) {
			return 0;
		}
		return LocOf(r, a_id);
	}

	bool PlaceObject(std::uint16_t a_id, const logic::Vec3& a_pos, const std::array<float, 4>& a_rot)
	{
		if (!g_ok || !a_pos.Finite()) {
			return false;
		}
		return SetLoc(LocationOf(a_id), a_pos, a_rot);
	}

	namespace
	{
		std::mutex                g_reqLock;
		std::optional<std::tuple<std::uint16_t, logic::Vec3, float>> g_req;
		std::string               g_reqResult{ "none" };
	}

	void RequestPlaceObject(std::uint16_t a_id, const logic::Vec3& a_pos, float a_yaw)
	{
		std::scoped_lock lock(g_reqLock);
		g_req = std::make_tuple(a_id, a_pos, a_yaw);
	}

	void ServicePending()
	{
		std::optional<std::tuple<std::uint16_t, logic::Vec3, float>> req;
		{
			std::scoped_lock lock(g_reqLock);
			req = std::exchange(g_req, std::nullopt);
		}
		if (!req) {
			return;
		}
		const auto [id, pos, yaw] = *req;
		const bool ok = PlaceObject(id, pos, logic::YawQuat(yaw));
		Log::Info("hold: placed object {:#x} at {:.2f},{:.2f},{:.2f}: {}", id, pos.x, pos.y, pos.z, ok);
		std::scoped_lock lock(g_reqLock);
		g_reqResult = std::format("{:#x} -> {:.2f},{:.2f},{:.2f}: {}", id, pos.x, pos.y, pos.z, ok);
	}

	std::string LastPlaceResult()
	{
		std::scoped_lock lock(g_reqLock);
		return g_reqResult;
	}

	std::string DescribeLocation(std::uint16_t a_id)
	{
		const auto loc = LocationOf(a_id);
		if (!loc) {
			return std::format("location of {:#x}: none", a_id);
		}
		std::string out = std::format("location of {:#x}: {:#x} vtable {:#x}\n", a_id, loc, *Get<std::uintptr_t>(loc));
		for (std::uintptr_t o = 0; o < 0x100; o += 16) {
			out += std::format("  +{:03x}:", o);
			for (std::uintptr_t k = 0; k < 16; k += 4) {
				const auto f = Get<float>(loc + o + k);
				const auto u = Get<std::uint32_t>(loc + o + k);
				out += f && u ? std::format(" {:08x}({:.3g})", *u, *f) : std::string(" ????????");
			}
			out += "\n";
		}
		const auto vt = *Get<std::uintptr_t>(loc);
		out += "  vtable:";
		for (std::uintptr_t k = 0; k < 0x100; k += 8) {
			const auto f = Get<std::uintptr_t>(vt + k);
			out += f ? std::format(" {:x}", *f) : std::string(" ?");
		}
		return out;
	}

}
