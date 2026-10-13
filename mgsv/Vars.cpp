#include "Vars.h"

#include <atomic>

#include "Addresses.h"
#include "Hook.h"
#include "Log.h"
#include "Verify.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>

namespace phantom::guest::vars
{
	namespace
	{
		enum Var
		{
			kLocation,
			kMission,
			kClock,
			kPosX,
			kPosY,
			kPosZ,
			kLifeMax,
			kRotY,
			kVehicle,
			kLife,
			kStamina,
			kWeapons,
			kAmmo,
			kHandEquip,
			kSlot,
			kBuddyType,
			kBuddyAmmo,
			kBuddyWp,
			kPhase,
			kSupport,
			kSupportIndex,
			kStockCounts,
			kItemIndex,
			kCount
		};

		const addr::VarSite* const kSites[kCount] = { &addr::kVarLocationCode, &addr::kVarMissionCode, &addr::kVarClock, &addr::kVarPlayerPosX,
			&addr::kVarPlayerPosY, &addr::kVarPlayerPosZ, &addr::kVarPlayerLifeMax, &addr::kVarPlayerRotY, &addr::kVarVehicleId,
			&addr::kVarPlayerLife, &addr::kVarPlayerStamina, &addr::kVarWeapons, &addr::kVarAmmoInWeapons, &addr::kVarHandEquip,
			&addr::kVarInventorySlot, &addr::kVarBuddyType, &addr::kVarBuddyGearAmmo, &addr::kVarBuddyGearMainWp, &addr::kVarPlayerPhase,
			&addr::kVarSupportWeapons, &addr::kVarSupportIndex, &addr::kVarAmmoStockCounts, &addr::kVarCurrentItemIndex };

		bool g_ok[kCount]{};
		bool g_buttonsOk{ false };

		std::uint8_t* Block()
		{
			const auto p = SafeRead<std::uint8_t*>(reinterpret_cast<const void*>(addr::kVarsPtr));
			return p ? *p : nullptr;
		}

		// The block is engine-owned and lives for the whole session once allocated; a bad pointer would
		// show up as an access violation, so reads go through ReadProcessMemory (one call for the
		// player range and one per array).
		template <class T>
		T Get(const std::uint8_t* a_range, std::uint32_t a_rangeStart, Var a_var, T a_default = {})
		{
			if (!g_ok[a_var]) {
				return a_default;
			}
			T v;
			std::memcpy(&v, a_range + (kSites[a_var]->offset - a_rangeStart), sizeof(T));
			return v;
		}
	}

	void Init()
	{
		int ok = 0;
		for (int i = 0; i < kCount; ++i) {
			g_ok[i] = Matches(*kSites[i]);
			ok += g_ok[i];
		}
		g_buttonsOk = Matches(addr::kPlayerVarsButtonsReg, "PlayerVars.scannedButtonsDirect");
		Log::Info("vars: {}/{} registration sites verified", ok, static_cast<int>(kCount));
	}

	Snapshot Read()
	{
		Snapshot s;
		auto* block = Block();
		if (!block) {
			return s;
		}
		// 0x10..0x120: codes, clock, player position/rotation/life/stamina/vehicle
		std::uint8_t player[0x120 - 0x10];
		SIZE_T       got = 0;
		if (!::ReadProcessMemory(::GetCurrentProcess(), block + 0x10, player, sizeof(player), &got) || got != sizeof(player)) {
			return s;
		}
		s.valid = true;
		constexpr std::uint32_t kBase = 0x10;
		s.locationCode = Get<std::uint16_t>(player, kBase, kLocation);
		s.missionCode = Get<std::uint16_t>(player, kBase, kMission);
		s.clock = Get<std::int32_t>(player, kBase, kClock);
		s.pos[0] = Get<float>(player, kBase, kPosX);
		s.pos[1] = Get<float>(player, kBase, kPosY);
		s.pos[2] = Get<float>(player, kBase, kPosZ);
		s.lifeMax = Get<std::uint16_t>(player, kBase, kLifeMax);
		s.rotYDeg = Get<std::int16_t>(player, kBase, kRotY);
		s.vehicleId = Get<std::uint16_t>(player, kBase, kVehicle, std::uint16_t(0xFFFF));
		s.life = Get<std::uint16_t>(player, kBase, kLife);
		s.stamina = Get<std::uint16_t>(player, kBase, kStamina);

		// 0x548..0x680: weapons, ammo, support items, ammo stocks, hand equip, inventory slot, selected support item, item in use
		std::uint8_t equip[0x680 - 0x548];
		if (::ReadProcessMemory(::GetCurrentProcess(), block + 0x548, equip, sizeof(equip), &got) && got == sizeof(equip)) {
			constexpr std::uint32_t kEquip = 0x548;
			if (g_ok[kWeapons]) {
				std::memcpy(s.weapons.data(), equip + (addr::kVarWeapons.offset - kEquip), sizeof(s.weapons));
			}
			if (g_ok[kAmmo]) {
				std::memcpy(s.ammo.data(), equip + (addr::kVarAmmoInWeapons.offset - kEquip), sizeof(s.ammo));
			}
			s.handEquip = Get<std::uint16_t>(equip, kEquip, kHandEquip);
			s.inventorySlot = Get<std::uint32_t>(equip, kEquip, kSlot);
			if (g_ok[kSupport]) {
				std::memcpy(s.supportWeapons.data(), equip + (addr::kVarSupportWeapons.offset - kEquip), sizeof(s.supportWeapons));
			}
			s.supportIndex = Get<std::uint32_t>(equip, kEquip, kSupportIndex);
			s.itemIndex = Get<std::uint32_t>(equip, kEquip, kItemIndex);
			if (g_ok[kStockCounts]) {
				std::memcpy(s.stockCounts.data(), equip + (addr::kVarAmmoStockCounts.offset - kEquip), sizeof(s.stockCounts));
			}
		}

		// 0x2D68..0x2E7C: buddy type, D-Walker gear, alert phase
		std::uint8_t buddy[0x2E7C - 0x2D68];
		if (::ReadProcessMemory(::GetCurrentProcess(), block + 0x2D68, buddy, sizeof(buddy), &got) && got == sizeof(buddy)) {
			constexpr std::uint32_t kBuddy = 0x2D68;
			s.buddyType = Get<std::uint8_t>(buddy, kBuddy, kBuddyType);
			if (g_ok[kBuddyAmmo]) {
				std::memcpy(s.buddyGearAmmo, buddy + (addr::kVarBuddyGearAmmo.offset - kBuddy), sizeof(s.buddyGearAmmo));
			}
			s.buddyGearMainWp = Get<std::uint8_t>(buddy, kBuddy, kBuddyWp);
			s.phase = Get<std::uint8_t>(buddy, kBuddy, kPhase);
		}

		if (g_buttonsOk) {
			if (const auto b = SafeRead<std::uint32_t>(reinterpret_cast<const void*>(addr::kPlayerVarsButtons))) {
				s.buttons = *b;
			}
		}
		return s;
	}

	namespace
	{
		std::atomic<int> g_pendingLife{ -1 };
	}

	bool WriteLife(std::uint16_t a_life)
	{
		if (!g_ok[kLife] || !Block()) {
			return false;
		}
		g_pendingLife.store(a_life, std::memory_order_release);
		return true;
	}

	int PendingLife() { return g_pendingLife.load(std::memory_order_acquire); }

	void ApplyPendingLife()
	{
		const int pending = g_pendingLife.exchange(-1, std::memory_order_acq_rel);
		if (pending < 0) {
			return;
		}
		const auto a_life = static_cast<std::uint16_t>(pending);
		auto* block = g_ok[kLife] ? Block() : nullptr;
		if (!block) {
			return;
		}
		// vars.playerLife is only a per-frame copy. The life MGSV owns (regeneration included) is the u16 at
		// kPlayerLifeNative (VERIFIED live 2026-10-12: a write there sticks and regenerates; vars follows).
		// Only touch it while it agrees with vars, so a different build/layout is never written blindly.
		auto*         native = reinterpret_cast<std::uint8_t*>(addr::kPlayerLifeNative);
		std::uint16_t current = 0, mirror = 0;
		SIZE_T        got = 0, put = 0;
		// "Agrees" within a few frames of regeneration (~760 life/s live): vars is a per-frame copy, so while Snake
		// regenerates the two differ by a frame's worth, and an exact compare dropped every hit taken then.
		const bool    nativeOk = ::ReadProcessMemory(::GetCurrentProcess(), native, &current, sizeof(current), &got) && got == sizeof(current) &&
		                      ::ReadProcessMemory(::GetCurrentProcess(), block + addr::kVarPlayerLife.offset, &mirror, sizeof(mirror), &got) &&
		                      std::abs(static_cast<int>(current) - static_cast<int>(mirror)) <= 400;
		if (nativeOk) {
			::WriteProcessMemory(::GetCurrentProcess(), native, &a_life, sizeof(a_life), &put);
		}
		::WriteProcessMemory(::GetCurrentProcess(), block + addr::kVarPlayerLife.offset, &a_life, sizeof(a_life), &put);
	}

	bool WriteAmmoCopies(const std::array<std::uint16_t, 3>& a_magazines, const std::array<std::uint16_t, 23>& a_counts)
	{
		auto* block = g_ok[kAmmo] && g_ok[kStockCounts] ? Block() : nullptr;
		if (!block) {
			return false;
		}
		SIZE_T put = 0;
		for (std::size_t i = 0; i < a_magazines.size(); ++i) {
			if (a_magazines[i] != 0xFFFF) {
				::WriteProcessMemory(::GetCurrentProcess(), block + addr::kVarAmmoInWeapons.offset + i * 2, &a_magazines[i], 2, &put);
			}
		}
		::WriteProcessMemory(::GetCurrentProcess(), block + addr::kVarAmmoStockCounts.offset, a_counts.data(), sizeof(a_counts), &put);
		return true;
	}
}
