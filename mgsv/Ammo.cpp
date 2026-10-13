#include "Ammo.h"

#include "Addresses.h"
#include "Hook.h"
#include "Log.h"
#include "Logic.h"
#include "State.h"
#include "Vars.h"
#include "Verify.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <utility>

namespace phantom::guest::ammo
{
	namespace
	{
		constexpr std::uint32_t kMaxStocks = 64;  // 23 on 1.0.15.4 ([this+0x88]); more means the layout moved
		constexpr std::uint32_t kMaxSlots = 16;   // weapon slots per player ([this+0xC], 7 on 1.0.15.4)
		constexpr int           kWeaponSlots = 3;  // PRIMARY_HIP, PRIMARY_BACK, SECONDARY: the slots with magazines

		std::atomic<bool> g_ok{ false };
		std::atomic<bool> g_hudOk{ false };  // the HUD's static copy of the magazines may be written (Addresses.h kHudEquip*)
		std::mutex        g_lock;  // the request and the counters (frame thread, console, Lua thread)
		std::string       g_pending;  // why the requested refill is wanted; empty = none
		std::uint64_t     g_pendingAt{ 0 };  // ... queued then: a 'refill' lost with a failed PhantomRing.Tick is queued again
		constexpr std::uint64_t kPendingStaleMs = 5000;
		std::atomic<bool> g_drainWanted{ false };  // `!ammo drain`: run on the Lua thread with the next Tick's results
		std::uint64_t     g_requested{ 0 }, g_done{ 0 }, g_failed{ 0 };
		std::string       g_last{ "none yet" };

		template <class T>
		std::optional<T> Get(std::uintptr_t a_at)
		{
			return SafeRead<T>(reinterpret_cast<const void*>(a_at));
		}

		struct Component
		{
			std::uintptr_t self{ 0 };
			std::uint32_t  player{ 0 };
		};

		// The ammo component of the local player, the way Player.GetAmmoStockBySlot finds it; nullopt (and why) on any doubt.
		std::optional<Component> Find(std::string& a_why)
		{
			const auto impl = Get<std::uintptr_t>(addr::kPlayer2System + 8);
			if (!impl || !*impl || Get<std::uintptr_t>(*impl) != addr::kPlayer2ImplVtable) {
				a_why = "Player2System impl vtable";
				return std::nullopt;
			}
			const auto player = Get<std::uint32_t>(*impl + 0x214);
			const auto holder = Get<std::uintptr_t>(*impl + 0xC8);
			const auto self = holder && *holder ? Get<std::uintptr_t>(*holder + 8) : std::nullopt;
			if (!player || *player >= 16 || !self || !*self) {
				a_why = "no ammo component (loading?)";
				return std::nullopt;
			}
			const auto vt = Get<std::uintptr_t>(*self);
			if (!vt || *vt != addr::kAmmoVtable) {
				a_why = std::format("ammo component vtable {:#x}", vt.value_or(0));
				return std::nullopt;
			}
			return Component{ *self, *player };
		}

		using Mag = logic::SlotMagazine;

		struct Copies
		{
			std::array<std::uint16_t, kWeaponSlots> mags{ 0xFFFF, 0xFFFF, 0xFFFF };
			std::array<std::uint16_t, kWeaponSlots> before{ 0xFFFF, 0xFFFF, 0xFFFF };
			std::array<std::uint16_t, 23>            counts{};
			bool                                     countsOk{ false };
		};

		// The HUD weapon box's copy of the magazines, where it still shows the count from before the refill. Returns how many.
		int WriteHudCopy(const Copies& a_copies)
		{
			if (!g_hudOk.load() || Get<std::uintptr_t>(addr::kHudEquip) != addr::kHudEquipVtable) {
				return 0;
			}
			int written = 0;
			for (int i = 0; i < kWeaponSlots; ++i) {
				const auto at = addr::kHudEquip + addr::kHudEquipMagazines + i * 4;
				if (a_copies.mags[i] != 0xFFFF && a_copies.mags[i] != a_copies.before[i] && Get<std::uint16_t>(at) == a_copies.before[i]) {
					SIZE_T put = 0;
					written += ::WriteProcessMemory(::GetCurrentProcess(), reinterpret_cast<void*>(at), &a_copies.mags[i], 2, &put) && put == 2;
				}
			}
			return written;
		}

		std::string Apply(const Component& a_c, const std::array<Mag, kWeaponSlots>& a_mags, Copies& a_copies)
		{
			const auto vt = addr::kAmmoVtable;
			const auto setMag = Get<std::uintptr_t>(vt + addr::kAmmoVfSetMagazine * 8);
			const auto setCount = Get<std::uintptr_t>(vt + addr::kAmmoVfSetCount * 8);
			if (!setMag || *setMag != addr::kAmmoSetMagazine.va || !setCount || *setCount != addr::kAmmoSetCount.va) {
				return "the ammo component's setters moved -- nothing refilled";
			}
			using SetMagazineFn = void (*)(std::uintptr_t, std::int32_t, std::int32_t, std::uint16_t, bool);
			using SetCountFn = void (*)(std::uintptr_t, std::int32_t, std::int32_t, std::uint32_t);
			const auto slots = Get<std::uint32_t>(a_c.self + 0xC);
			const auto mags = Get<std::uintptr_t>(a_c.self + 0x50);
			std::string out = "magazines";
			if (!slots || *slots < kWeaponSlots || *slots > kMaxSlots || !mags || !*mags) {
				out += " skipped (slot table)";
			} else {
				for (int i = 0; i < kWeaponSlots; ++i) {
					const auto& m = a_mags[i];
					const auto  at = *mags + (std::uintptr_t(a_c.player) * *slots + i) * 6;
					const auto  before = Get<std::uint16_t>(at).value_or(0xFFFF);
					// Never down: a magazine with a round chambered holds one more than its size (live: 36:31->30 lost it).
					if (m.equip && m.rounds && m.rounds < 0xFFFF && (before == 0xFFFF || before < m.rounds)) {
						reinterpret_cast<SetMagazineFn>(*setMag)(a_c.self, static_cast<std::int32_t>(a_c.player), i, static_cast<std::uint16_t>(m.rounds), false);
					}
					if (m.equip && m.sub && m.sub < 0xFFFF) {
						reinterpret_cast<SetMagazineFn>(*setMag)(a_c.self, static_cast<std::int32_t>(a_c.player), i, static_cast<std::uint16_t>(m.sub), true);
					}
					const auto after = Get<std::uint16_t>(at).value_or(0xFFFF);
					a_copies.mags[i] = after;
					a_copies.before[i] = before;
					out += std::format(" {}:{}->{}", m.equip, before, after);
				}
			}
			const auto n = Get<std::uint32_t>(a_c.self + 0x88);
			const auto ids = Get<std::uintptr_t>(a_c.self + 0x90);
			const auto counts = Get<std::uintptr_t>(a_c.self + 0x98);
			const auto maxima = Get<std::uintptr_t>(a_c.self + 0xA0);
			if (!n || *n == 0 || *n > kMaxStocks || !ids || !counts || !maxima || !*ids || !*counts || !*maxima) {
				return out + "; stocks skipped (stock table)";
			}
			int topped = 0;
			out += "; stocks";
			for (std::uint32_t i = 0; i < *n; ++i) {
				const auto id = Get<std::uint8_t>(*ids + i);
				const auto count = Get<std::uint16_t>(*counts + i * 2);
				const auto max = Get<std::uint16_t>(*maxima + i * 2);
				if (!id || !*id || !count || !max || *count >= *max) {
					continue;  // empty entry, or already full
				}
				reinterpret_cast<SetCountFn>(*setCount)(a_c.self, static_cast<std::int32_t>(i), 0, *max);
				out += std::format(" #{}(bullet {}) {}->{}", i, *id, *count, Get<std::uint16_t>(*counts + i * 2).value_or(0));
				++topped;
			}
			if (!topped) {
				out += " all full";
			}
			if (*n == a_copies.counts.size()) {
				a_copies.countsOk = true;
				for (std::uint32_t i = 0; i < *n; ++i) {
					a_copies.counts[i] = Get<std::uint16_t>(*counts + i * 2).value_or(0);
				}
			}
			return out;
		}
	}

	bool Init()
	{
		const bool ok = Matches(addr::kPlayer2SystemAccessor, "Player2System accessor") && Matches(addr::kAmmoComponentLookup, "ammo component lookup") &&
		                Matches(addr::kAmmoSetMagazine, "ammo SetMagazine") && Matches(addr::kAmmoSetCount, "ammo SetCount") &&
		                Matches(addr::kAmmoGetId, "ammo GetId") && Matches(addr::kAmmoFind, "ammo stock count") &&
		                Get<std::uintptr_t>(addr::kAmmoVtable + addr::kAmmoVfSetMagazine * 8) == addr::kAmmoSetMagazine.va &&
		                Get<std::uintptr_t>(addr::kAmmoVtable + addr::kAmmoVfSetCount * 8) == addr::kAmmoSetCount.va &&
		                Get<std::uintptr_t>(addr::kAmmoVtable + addr::kAmmoVfGetId * 8) == addr::kAmmoGetId.va &&
		                Get<std::uintptr_t>(addr::kAmmoVtable + addr::kAmmoVfFind * 8) == addr::kAmmoFind.va;
		g_ok.store(ok);
		g_hudOk.store(ok && Matches(addr::kHudEquipInit, "HUD equipment copy"));
		if (ok) {
			Log::Info("ammo: refill on rest/respawn ready (MGSV's ammo component setters verified)");
		} else {
			Log::Warn("ammo: MGSV's ammo component differs -- no refill on rest/respawn");
		}
		return ok;
	}

	bool Available() { return g_ok.load(); }

	void Request(const char* a_why)
	{
		{
			std::scoped_lock lock(g_lock);
			++g_requested;
			if (!g_ok.load()) {
				g_last = std::format("{}: refill unavailable (addresses differ)", a_why);
				return;
			}
			const auto now = ::GetTickCount64();
			const bool queued = !g_pending.empty();
			const bool stale = queued && now - g_pendingAt > kPendingStaleMs;
			g_pending = a_why;
			if (queued && !stale) {
				return;  // one 'refill' command is already on its way
			}
			if (stale) {
				// Review: Tick takes the commands before it calls PhantomRing.Tick; a failed call lost this one, and every later
				// rest/respawn returned here for the rest of the session.
				Log::Warn("ammo: the refill asked {} ms ago never came back (a failed Lua update?) -- asking again", now - g_pendingAt);
			}
			g_pendingAt = now;
		}
		state::QueueCommand("refill");
	}

	namespace
	{
		std::string DrainNow();
	}

	void OnResults(std::string_view a_results)
	{
		if (g_drainWanted.exchange(false)) {
			auto out = DrainNow();  // here, on the Lua thread (review: it ran MGSV's Find/SetCount on the link worker)
			std::scoped_lock lock(g_lock);
			g_last = std::move(out);
		}
		const auto at = a_results.find("refill=");
		if (at == std::string_view::npos) {
			return;
		}
		auto text = a_results.substr(at + 7);
		text = text.substr(0, text.find(' '));
		std::string why;
		{
			std::scoped_lock lock(g_lock);
			why = std::exchange(g_pending, {});
		}
		if (why.empty()) {
			why = "console";
		}
		std::array<Mag, kWeaponSlots> mags{};
		std::string                   line;
		std::string                   fail;
		if (!logic::ParseMagazines(text, mags)) {
			line = std::format("{}: no magazine sizes from Lua ('{}')", why, text);
		} else if (const auto c = Find(fail); !c) {
			line = std::format("{}: {}", why, fail);
		} else {
			Copies copies;
			line = std::format("{}: {}", why, Apply(*c, mags, copies));
			// The vars copies follow at Snake's next shot; write them now so GuestState (and the HUD's ammo) agree at once
			if (copies.countsOk && !vars::WriteAmmoCopies(copies.mags, copies.counts)) {
				line += " (vars copies not written)";
			}
			if (const int hud = WriteHudCopy(copies)) {
				line += std::format("; HUD box {} slot(s)", hud);
			}
		}
		const bool ok = line.find("magazines") != std::string::npos;
		Log::Info("ammo: refill ({})", line);
		std::scoped_lock lock(g_lock);
		(ok ? g_done : g_failed) += 1;
		g_last = std::move(line);
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		return std::format("ammo: {}; {} asked, {} refilled, {} failed{}; last {}", g_ok.load() ? "refill ready" : "refill OFF (addresses differ)", g_requested, g_done,
			g_failed, g_pending.empty() ? "" : std::format(", '{}' pending", g_pending), g_last);
	}

	std::string Drain()
	{
		if (!g_ok.load()) {
			return "ammo drain: unavailable (addresses differ)";
		}
		g_drainWanted.store(true);
		state::QueueCommand("drain");  // PR.Run answers 'drain=unknown': the results still reach OnResults
		return "ammo drain queued (next Lua update); see !ammo";
	}

	namespace
	{
	std::string DrainNow()
	{
		std::string why;
		const auto  c = g_ok.load() ? Find(why) : std::nullopt;
		const auto  setCount = Get<std::uintptr_t>(addr::kAmmoVtable + addr::kAmmoVfSetCount * 8);
		const auto  n = c ? Get<std::uint32_t>(c->self + 0x88) : std::nullopt;
		const auto  ids = c ? Get<std::uintptr_t>(c->self + 0x90) : std::nullopt;
		const auto  counts = c ? Get<std::uintptr_t>(c->self + 0x98) : std::nullopt;
		if (!c || !setCount || *setCount != addr::kAmmoSetCount.va || !n || *n == 0 || *n > kMaxStocks || !ids || !counts) {
			return "ammo drain: " + (why.empty() ? std::string("unavailable") : why);
		}
		std::string out = "ammo drain:";
		for (std::uint32_t i = 0; i < *n; ++i) {
			const auto id = Get<std::uint8_t>(*ids + i);
			const auto count = Get<std::uint16_t>(*counts + i * 2);
			if (id && *id && count && *count > 1) {
				using SetCountFn = void (*)(std::uintptr_t, std::int32_t, std::int32_t, std::uint32_t);
				reinterpret_cast<SetCountFn>(*setCount)(c->self, static_cast<std::int32_t>(i), 0, *count / 2u);
				out += std::format(" #{} {}->{}", i, *count, Get<std::uint16_t>(*counts + i * 2).value_or(0));
			}
		}
		Log::Info("{}", out);
		return out;
	}
	}
}
