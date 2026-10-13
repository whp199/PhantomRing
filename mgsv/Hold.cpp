#include "Hold.h"

#include "Addresses.h"
#include "Hook.h"
#include "Log.h"
#include "Verify.h"

#include <windows.h>

#include <cstdint>

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

		// Replays the resolution in Player2SystemImpl::Warp for the local player; 0 on any mismatch.
		std::uintptr_t ResolveLoc()
		{
			const auto impl = Get<std::uintptr_t>(addr::kPlayer2System + 8);
			if (!impl || !*impl || Get<std::uintptr_t>(*impl) != addr::kPlayer2ImplVtable) {
				Fail("Player2System impl vtable");
				return 0;
			}
			const auto p = *impl;
			const auto idx = Get<std::uint32_t>(p + 0x214);
			const auto slots = Get<std::uintptr_t>(p + 0x78);
			const auto objs = Get<std::uintptr_t>(p + 0x70);
			const auto insts = Get<std::uintptr_t>(p + 0x378);
			if (!idx || *idx >= 16 || !slots || !objs || !insts || !*insts) {
				Fail("player tables");
				return 0;
			}
			const auto slot = Get<std::uint8_t>(*slots + *idx);
			const auto obj = Get<std::uint16_t>(*objs + 2 * *idx);
			if (!slot || *slot == 0xFF || !obj || *obj == 0xFFFF) {
				return 0;  // no player instance right now (loading)
			}
			const auto inst = *insts + static_cast<std::uintptr_t>(*slot) * 0x6A8;
			const auto a = Get<std::uintptr_t>(inst + 0x78);
			const auto mgr = a ? Get<std::uintptr_t>(*a + 0x238) : std::nullopt;
			const auto mgrVt = mgr && *mgr ? Get<std::uintptr_t>(*mgr) : std::nullopt;
			const auto getLoc = mgrVt && InImage(*mgrVt) ? Get<std::uintptr_t>(*mgrVt + 0x388) : std::nullopt;
			if (!getLoc || !InImage(*getLoc)) {
				Fail("location manager");
				return 0;
			}
			using GetLocFn = std::uintptr_t (*)(std::uintptr_t, std::uint32_t);
			const auto loc = reinterpret_cast<GetLocFn>(*getLoc)(*mgr, *obj);
			const auto locVt = loc ? Get<std::uintptr_t>(loc) : std::nullopt;
			if (!locVt || !InImage(*locVt)) {
				Fail("location object");
				return 0;
			}
			if (loc != g_lastLoc) {
				g_lastLoc = loc;
				Log::Info("hold: player location object {:#x} (vtable {:#x}, mgr {:#x}, obj {}, slot {})", loc, *locVt, *mgr, *obj, *slot);
			}
			return loc;
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
		const auto loc = ResolveLoc();
		const auto vt = loc ? Get<std::uintptr_t>(loc) : std::nullopt;
		const auto set = vt ? Get<std::uintptr_t>(*vt + 0x68) : std::nullopt;
		if (!set || !InImage(*set)) {
			return false;
		}
		alignas(16) float pos[4] = { a_pos.x, a_pos.y, a_pos.z, 0.0f };
		alignas(16) float rot[4] = { a_rot[0], a_rot[1], a_rot[2], a_rot[3] };
		using SetFn = void (*)(std::uintptr_t, const float*, const float*);
		reinterpret_cast<SetFn>(*set)(loc, pos, rot);
		return true;
	}
}
