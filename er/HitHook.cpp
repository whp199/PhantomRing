#include "HitHook.h"

#include "Addresses.h"
#include "Log.h"
#include "VptrSwap.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>

namespace phantom::er::HitHook
{
	namespace
	{
		constexpr std::size_t   kDamageModuleSlots = 23;  // RTTI vtable walk
		constexpr std::size_t   kReceiveSlot = 7;         // +0x38
		constexpr std::uint32_t kModDamage = 0x98;        // ChrIns modules -> damage module [V: 0x44AE70]

		using ReceiveFn = void (*)(std::uint8_t* a_dm, std::uint8_t* a_attacker, std::uint8_t* a_damage);

		VptrSwap<kDamageModuleSlots> g_swap;
		ReceiveFn                    g_original{ nullptr };
		std::atomic<std::uint8_t*>   g_dm{ nullptr };
		std::atomic<bool>            g_cancel{ false };
		std::atomic<std::uint64_t>   g_calls{ 0 };
		std::atomic<std::uint64_t>   g_cancelled{ 0 };
		std::atomic_flag             g_lock = ATOMIC_FLAG_INIT;
		std::vector<Hit>             g_queue;

		struct StudyDump
		{
			std::uint8_t raw[0x300]{};
			std::int32_t hpBefore{ 0 }, hpAfter{ 0 };
		};
		std::atomic<bool> g_study{ false };
		StudyDump         g_dumps[8];
		std::atomic<int>  g_dumpCount{ 0 };

		std::int32_t OwnerHp(std::uint8_t* a_dm)
		{
			// damage module +0x8: owner ChrIns; ChrIns +0x190 -> modules; modules +0 -> data; data +0x138 hp
			auto* owner = SafeRead<std::uint8_t*>(a_dm + 0x8).value_or(nullptr);
			auto* modules = owner ? SafeRead<std::uint8_t*>(owner + 0x190).value_or(nullptr) : nullptr;
			auto* data = modules ? SafeRead<std::uint8_t*>(modules).value_or(nullptr) : nullptr;
			return data ? SafeRead<std::int32_t>(data + 0x138).value_or(-1) : -1;
		}

		std::uint8_t* OwnerData(std::uint8_t* a_dm)
		{
			auto* owner = SafeRead<std::uint8_t*>(a_dm + 0x8).value_or(nullptr);
			auto* modules = owner ? SafeRead<std::uint8_t*>(owner + 0x190).value_or(nullptr) : nullptr;
			return modules ? SafeRead<std::uint8_t*>(modules).value_or(nullptr) : nullptr;
		}

		void RestoreHp(std::uint8_t* a_dm, std::int32_t a_hp)
		{
			if (auto* data = OwnerData(a_dm)) {
				*reinterpret_cast<std::int32_t*>(data + 0x138) = a_hp;
			}
		}

		std::int32_t OwnerMaxHp(std::uint8_t* a_dm)
		{
			auto* data = OwnerData(a_dm);
			return data ? SafeRead<std::int32_t>(data + 0x13C).value_or(-1) : -1;
		}

		struct Guard
		{
			Guard()
			{
				while (g_lock.test_and_set(std::memory_order_acquire)) {
					YieldProcessor();
				}
			}
			~Guard() { g_lock.clear(std::memory_order_release); }
		};

		Vec3 ReadVec(const std::uint8_t* a_at)
		{
			float v[3]{};
			SafeReadBytes(a_at, v, sizeof(v));
			return { v[0], v[1], v[2] };
		}

		void Detour(std::uint8_t* a_dm, std::uint8_t* a_attacker, std::uint8_t* a_damage)
		{
			if (a_dm != g_dm.load(std::memory_order_acquire) || !a_damage) {
				g_original(a_dm, a_attacker, a_damage);
				return;
			}
			g_calls.fetch_add(1, std::memory_order_relaxed);
			Hit hit;
			hit.amount = SafeRead<std::int32_t>(a_damage + 0x228).value_or(0);
			hit.at = ReadVec(a_damage + 0x1A0);
			hit.from = -ReadVec(a_damage + 0x1C0);  // stored attacker -> target
			hit.attacker = a_attacker;
			hit.kind = SafeRead<std::uint32_t>(a_damage + 0x22C).value_or(0);
			hit.atkParam = SafeRead<std::uint32_t>(a_damage + 0x234).value_or(0);
			if (g_study.load(std::memory_order_acquire)) {
				const int n = g_dumpCount.load(std::memory_order_relaxed);
				if (n < 8) {
					auto& d = g_dumps[n];
					d.hpBefore = OwnerHp(a_dm);
					SafeReadBytes(a_damage, d.raw, sizeof(d.raw));
					g_original(a_dm, a_attacker, a_damage);
					d.hpAfter = OwnerHp(a_dm);
					g_dumpCount.store(n + 1, std::memory_order_release);
				} else {
					g_original(a_dm, a_attacker, a_damage);
				}
				return;
			}
			if (!g_cancel.load(std::memory_order_acquire)) {
				g_original(a_dm, a_attacker, a_damage);
				return;
			}
			// The amount isn't in ChrDamage when ReceiveDamage starts (VERIFIED live: +0x228 is 0; ER computes
			// it from the attack and the defence inside). So let ER apply the hit, measure the HP it took
			// (ER's own damage math), put the HP straight back and forward that amount to MGSV.
			// At full HP: the puppet's HP mirrors Snake's life, and with NoDead a hit can't take its last point -- at low
			// life the measured hit shrank to (HP - 1) and Snake could never be killed by one.
			const std::int32_t before = OwnerHp(a_dm);
			const std::int32_t full = OwnerMaxHp(a_dm);
			if (before > 0 && full > before) {
				RestoreHp(a_dm, full);
			}
			const std::int32_t start = std::max(before, full);
			g_original(a_dm, a_attacker, a_damage);
			const std::int32_t after = OwnerHp(a_dm);
			if (before > 0 && after >= 0 && after < start) {
				hit.amount = start - after;
			}
			if (before > 0) {
				RestoreHp(a_dm, before);
			}
			g_cancelled.fetch_add(1, std::memory_order_relaxed);
			if (hit.amount > 0) {
				Guard guard;
				if (g_queue.size() < 64) {
					g_queue.push_back(hit);
				}
			}
		}
	}

	bool Install(const game::Player& a_player)
	{
		const auto& a = Addr();
		if (!a.HasAll(A::VtPlayerDamageModule, A::PlayerReceiveDamage) || !a_player) {
			Log::Warn("hit hook: CSPlayerDamageModule / ReceiveDamage unverified -- hits seen through HP loss only");
			return false;
		}
		auto* dm = SafePtr(a_player.modules + kModDamage);
		g_original = reinterpret_cast<ReceiveFn>(a[A::PlayerReceiveDamage]);
		if (!g_swap.Install(dm, a[A::VtPlayerDamageModule], kReceiveSlot, a[A::PlayerReceiveDamage], reinterpret_cast<void*>(&Detour))) {
			Log::Warn("hit hook: modules+0x98 is not a stock CSPlayerDamageModule ({}) -- hits seen through HP loss only", game::RttiName(dm));
			return false;
		}
		g_dm.store(dm, std::memory_order_release);
		Log::Info("hit hook: CSPlayerDamageModule {} slot 7 -> PhantomRing", static_cast<void*>(dm));
		return true;
	}

	void Remove(const game::Player& a_player)
	{
		g_cancel.store(false, std::memory_order_release);
		auto* dm = g_dm.exchange(nullptr, std::memory_order_acq_rel);
		if (dm && a_player && SafePtr(a_player.modules + kModDamage) == dm && g_swap.Restore(dm)) {
			Log::Info("hit hook: damage module vtable restored ({} calls, {} cancelled)", g_calls.load(), g_cancelled.load());
		}
		Guard guard;
		g_queue.clear();
	}

	void SetCancel(bool a_cancel) { g_cancel.store(a_cancel && g_dm.load(std::memory_order_acquire), std::memory_order_release); }

	std::vector<Hit> Drain()
	{
		Guard            guard;
		std::vector<Hit> out;
		out.swap(g_queue);
		return out;
	}

	bool Installed() { return g_dm.load(std::memory_order_acquire) != nullptr; }

	std::string Describe()
	{
		return Installed() ? std::format("hit hook ON: {} calls, {} cancelled", g_calls.load(), g_cancelled.load()) : std::string("hit hook off");
	}

	void SetStudy(bool a_on)
	{
		if (a_on) {
			g_dumpCount.store(0, std::memory_order_release);
		}
		g_study.store(a_on, std::memory_order_release);
	}

	std::string DumpStudy()
	{
		std::string out = std::format("hit study {}: {} hit(s) recorded", g_study.load() ? "on" : "off", g_dumpCount.load());
		for (int i = 0; i < g_dumpCount.load(); ++i) {
			const auto& d = g_dumps[i];
			const int   lost = d.hpBefore - d.hpAfter;
			out += std::format("\n#{} hp {} -> {} (lost {})", i, d.hpBefore, d.hpAfter, lost);
			std::string ints, floats;
			for (std::size_t off = 0; off + 4 <= sizeof(d.raw); off += 4) {
				std::int32_t v;
				float        f;
				std::memcpy(&v, d.raw + off, 4);
				std::memcpy(&f, d.raw + off, 4);
				if (lost > 0 && v == lost) {
					ints += std::format(" +{:#x}", off);
				}
				if (lost > 0 && std::fabs(f - static_cast<float>(lost)) < 0.75f) {
					floats += std::format(" +{:#x}({:.2f})", off, f);
				}
			}
			out += std::format("\n   int == lost at:{}\n   float ~ lost at:{}", ints.empty() ? " none" : ints, floats.empty() ? " none" : floats);
			out += std::format("\n   +0x228 {} +0x22c {} +0x234 {}", *reinterpret_cast<const std::int32_t*>(d.raw + 0x228), *reinterpret_cast<const std::int32_t*>(d.raw + 0x22c),
				*reinterpret_cast<const std::int32_t*>(d.raw + 0x234));
		}
		return out;
	}
}
