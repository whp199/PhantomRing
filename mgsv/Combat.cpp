#include "Combat.h"

#include "Addresses.h"
#include "Config.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"
#include "Verify.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

namespace phantom::guest::combat
{
	namespace
	{
		using ShotFn = void (*)(void*, float*, float*, void*, std::uint32_t);
		ShotFn g_original{ nullptr };

		std::atomic<bool>           g_native{ false };
		std::atomic<std::uint32_t>  g_snakeIndex{ 0xFFFFFFFF };
		std::atomic<std::uintptr_t> g_character{ 0 };
		std::atomic<std::uint64_t>  g_frame{ 0 };
		std::atomic<bool>           g_buddy{ false };
		std::atomic<unsigned>       g_otherCallers{ 0 };
		std::atomic<std::uint64_t>  g_buddyRounds{ 0 };

		// Damage share (protocol v8, kGEvFire pad[1]): MGSV's damage table and a standard soldier's life, both verified at
		// install (Addresses.h "damage parameters" / "soldier parameters"); off (pad[1] = 0) when either differs.
		std::atomic<bool>          g_damageOk{ false };
		std::atomic<std::uint64_t> g_shared{ 0 }, g_unshared{ 0 };
		std::atomic<std::uint32_t> g_lastAttack{ 0 }, g_lastShare{ 0 }, g_soldierLife{ 0 };

		// CQC poll state (game thread only)
		std::uint16_t g_lastSerial{ 0 };
		bool          g_serialPrimed{ false };
		bool          g_cqcWarned{ false };

		template <class T>
		std::optional<T> Get(std::uintptr_t a_at)
		{
			return SafeRead<T>(reinterpret_cast<const void*>(a_at));
		}

		struct DamageRow
		{
			std::uint16_t lethal{ 0 };     // row [0]: life damage of one hit
			std::uint16_t nonLethal{ 0 };  // row [1]: stun / sleep damage
			std::uint8_t  source{ 0 };     // +0x16: TppDamage.DAM_SOURCE_* (weapon class)
		};

		// The damage table's row of an attack id, read where MGSV's own getter (vf0 0x1405C9E30) would point.
		std::optional<DamageRow> Row(std::uint32_t a_attack)
		{
			if (!g_damageOk.load(std::memory_order_relaxed) || a_attack == 0 || a_attack >= addr::kDamageRows ||
				Get<std::uintptr_t>(addr::kDamageParams) != addr::kDamageParamsVtable) {
				return std::nullopt;
			}
			const auto at = addr::kDamageParams + 8 + std::uintptr_t(a_attack) * addr::kDamageRowBytes;
			const auto lethal = Get<std::uint16_t>(at);
			const auto nonLethal = Get<std::uint16_t>(at + 2);
			const auto source = Get<std::uint8_t>(at + 0x16);
			if (!lethal || !nonLethal || !source) {
				return std::nullopt;
			}
			return DamageRow{ *lethal, *nonLethal, *source };
		}

		// A standard soldier's life (lifeParameterTable maxLife); 0 when the table isn't loaded or looks wrong.
		std::uint32_t SoldierLife()
		{
			if (Get<std::uintptr_t>(addr::kSoldierParams) != addr::kSoldierParamsVtable) {
				return 0;
			}
			const auto table = Get<std::uintptr_t>(addr::kSoldierParams + 0x20);
			const auto life = table && *table ? Get<std::int16_t>(*table + 0xC) : std::nullopt;
			return life && *life >= 100 && *life <= 30000 ? static_cast<std::uint32_t>(*life) : 0;
		}

		// kGEvFire pad[1]: the round's lethal damage as a share of a soldier's life (1/10000), 0 = unknown / not lethal.
		std::uint32_t ShareOf(std::uint32_t a_attack, std::uint32_t a_equip, std::uint32_t a_bullet)
		{
			const auto row = Row(a_attack);
			const auto life = SoldierLife();
			const auto share = row && life ? logic::DamageShare(row->lethal, life) : 0;
			(share ? g_shared : g_unshared).fetch_add(1, std::memory_order_relaxed);
			g_lastAttack.store(a_attack, std::memory_order_relaxed);
			g_lastShare.store(share, std::memory_order_relaxed);
			g_soldierLife.store(life, std::memory_order_relaxed);
			// Every attack id once (a weapon's rounds share one).
			static std::array<std::atomic<bool>, 512> logged{};
			if (!logged[a_attack & 511].exchange(true)) {
				if (row && life) {
					Log::Info("shots: equip {} bullet {} attack {}: lethal {} non-lethal {} (class {}) vs a soldier's life {} -> share {}/10000",
						a_equip, a_bullet, a_attack, row->lethal, row->nonLethal, row->source, life, share);
				} else {
					Log::Info("shots: equip {} bullet {} attack {}: no damage row ({}) -- pad[1] = 0", a_equip, a_bullet, a_attack,
						!g_damageOk.load() ? "table unverified" : !life ? "soldier life unknown" : "attack id out of range or table not loaded");
				}
			}
			return share;
		}

		void ShotDetour(void* a_c, float* a_origin, float* a_dir, void* a_w, std::uint32_t a_index)
		{
			g_original(a_c, a_origin, a_dir, a_w, a_index);
			const auto ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
			try {
				if (ret != addr::kShotFireReturn) {
					// Aim/trajectory previews -- or D-Walker's gun (re-backlog M5 candidates 0x14103F907 /
					// 0x14109415E). Logged for the live test, never sent.
					if (g_otherCallers.fetch_add(1) < 24) {
						Log::Info("shots: ShotSolver from {:#x} (index {}) -- not a FireWeapon round", ret, a_index);
					}
					return;
				}
				const auto w = reinterpret_cast<std::uintptr_t>(a_w);
				const auto vt = Get<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(a_c));
				const auto equip = Get<std::uint32_t>(w + 0x24C);
				const auto flags = Get<std::uint32_t>(w + 0x27C);
				const auto mode = Get<std::uint32_t>(w + 0x3D0);
				// bulletId (TppEquip.BL_*): u16 at +0x22C, in the 0x90-byte gun data copied to +0x1B0 (VERIFIED live
				// 2026-10-12: ZORN-KP 23 BL_HgGrenade, S1000 AIR-S 22 BL_12GAAir, rocket launcher 59 = a BL_ms* missile).
				// +0x240 is the weapon's resource handle (MGS5VR; FireWeapon hands it to the parts system 0x140DBD610).
				// The alternate mode (under-barrel launcher, +0x27C & 0x400000) is UNVERIFIED: logged when seen.
				const bool alternate = flags && mode && (*flags & 0x400000) && (*mode & 0x180000) == 0x80000;
				const auto bullet = Get<std::uint16_t>(w + 0x22C);
				const auto attack = Get<std::uint16_t>(w + addr::kWeaponAttackId);
				const auto pellets = std::max<std::uint32_t>(1, Get<std::uint8_t>(w + 0x3DF).value_or(1));
				const bool snake = vt && *vt == addr::kWeaponComponentVtable && a_index == g_snakeIndex.load(std::memory_order_relaxed);
				if (static std::atomic<int> logged{ 0 }; logged.fetch_add(1) < 24 || alternate) {
					Log::Info("shots: {} equip {} bullet {} pellets {}{}", snake ? "snake" : "other", equip.value_or(0), bullet.value_or(0), pellets,
						alternate ? " (ALTERNATE mode: bullet field unverified)" : "");
				}
				const auto kind = KindOfBullet(bullet.value_or(0));
				if (!snake && !g_buddy.load(std::memory_order_relaxed)) {
					Log::Debug("shots: round from a foreign weapon (index {}), no buddy deployed -- ignored", a_index);
					return;
				}
				// Counted before the no-damage check: Frame's flamethrower detection reads "a buddy round
				// arrived" as "this is a gun", whatever the round does.
				if (!snake && g_buddyRounds.fetch_add(1) == 0) {
					Log::Info("shots: first buddy round through FireWeapon (equip {}, bullet {})", equip.value_or(0), bullet.value_or(0));
				}
				if (kind == BulletKind::kNone) {
					Log::Debug("shots: bullet {} does no damage (smoke/flare/fulton)", bullet.value_or(0));
					return;
				}
				proto::GuestEvent e{};
				e.type = snake ? proto::kGEvFire : proto::kGEvBuddyFire;
				const std::uint32_t ammo = kind == BulletKind::kTranq ? proto::kAmmoTranq : kind == BulletKind::kStun ? proto::kAmmoStun : proto::kAmmoLethal;
				e.a = (equip.value_or(0) & 0xFFFF) | (ammo << 16);
				e.b = pellets;
				e.frame = static_cast<std::uint32_t>(g_frame.load(std::memory_order_relaxed));
				std::memcpy(e.v0, a_origin, sizeof(e.v0));
				std::memcpy(e.v1, a_dir, sizeof(e.v1));
				// Not in the protocol's fields: the raw bulletId (explosives are lethal kind + this id) and
				// a bit for "explosive" so the host can do area damage at the hit point.
				e.pad[0] = bullet.value_or(0) | (kind == BulletKind::kExplosive ? 0x80000000u : 0u);
				// v8: one body hit's lethal damage (per pellet for shotguns) as a share of a standard MGSV soldier's life.
				e.pad[1] = ShareOf(attack.value_or(0), equip.value_or(0), bullet.value_or(0));
				link::PushEvent(e);
				Log::Debug("shots: {} equip {} bullet {} attack {} share {} kind {} pellets {} origin {:.2f},{:.2f},{:.2f} dir {:.2f},{:.2f},{:.2f}",
					snake ? "snake" : "buddy", equip.value_or(0), bullet.value_or(0), attack.value_or(0), e.pad[1], static_cast<int>(kind), pellets,
					a_origin[0], a_origin[1], a_origin[2], a_dir[0], a_dir[1], a_dir[2]);
			} catch (...) {
			}
		}
	}

	bool InstallShots()
	{
		if (!Matches(addr::kShotSolver, "ShotSolver") || !Matches(addr::kShotSolverFireCall, "FireWeapon -> ShotSolver call")) {
			Log::Warn("shots: ShotSolver unverified -- falling back to ammo-counter shots");
			return false;
		}
		// The damage share (v8) needs MGSV's damage table and the soldier parameters exactly as analysed.
		const bool damage = Matches(addr::kDamageRowGetter, "damage table row getter") && Matches(addr::kDamageReloadClear, "damage table size") &&
		                    Matches(addr::kDamageLethalKey, "damage table lethal column key") && Matches(addr::kDamageLethalStore, "damage table lethal store") &&
		                    Get<double>(addr::kDamageLethalKeyValue).value_or(0.0) == 30.0 && Matches(addr::kSoldierLifeTableCall, "soldier life table") &&
		                    Matches(addr::kSoldierMaxLifeStore, "soldier maxLife store");
		g_damageOk.store(damage);
		if (!damage) {
			Log::Warn("shots: MGSV's damage table differs -- kGEvFire pad[1] (damage share) stays 0, the host uses its own table");
		}
		if (!Hooks::Create("ShotSolver", reinterpret_cast<void*>(addr::kShotSolver.va), &ShotDetour, &g_original)) {
			return false;
		}
		g_native.store(true);
		return true;
	}

	std::string DescribeDamage()
	{
		return std::format("damage share: {} ({} rounds with a share, {} without; last attack {} share {}; soldier life {})",
			g_damageOk.load() ? "on" : "off (table unverified)", g_shared.load(), g_unshared.load(), g_lastAttack.load(), g_lastShare.load(),
			g_soldierLife.load());
	}

	bool NativeShots() { return g_native.load(); }
	std::uint64_t BuddyRounds() { return g_buddyRounds.load(); }

	void ObserveOwner(std::uintptr_t a_owner, std::uint64_t a_frame, bool a_buddyDeployed)
	{
		g_frame.store(a_frame, std::memory_order_relaxed);
		g_buddy.store(a_buddyDeployed, std::memory_order_relaxed);
		if (const auto idx = Get<std::uint32_t>(a_owner + addr::kOwnerPlayerIndex); idx && *idx < 16) {
			g_snakeIndex.store(*idx, std::memory_order_relaxed);
		}
		if (const auto ch = Get<std::uintptr_t>(a_owner + addr::kOwnerCharacter); ch && Get<std::uintptr_t>(*ch) == addr::kCharacterVtable) {
			g_character.store(*ch, std::memory_order_relaxed);
		}
	}

	void PollMelee(const float a_root[3], float a_yaw)
	{
		if (!Cfg().melee) {
			return;
		}
		const auto character = g_character.load(std::memory_order_relaxed);
		const auto idx = g_snakeIndex.load(std::memory_order_relaxed);
		if (!character || idx >= 16) {
			return;
		}
		// Resolve the CQC component and this player's state, validating every link (re-backlog M3).
		const auto iface = Get<std::uintptr_t>(character + 0xD8);
		if (!iface || *iface < 0x38) {
			return;
		}
		const auto c = *iface - 0x38;
		const bool shapeOk = Get<std::uintptr_t>(c) == addr::kCqcVtable && Get<std::uintptr_t>(*iface) == addr::kCqcInterfaceVtable &&
		                     Get<std::uintptr_t>(c + 0x40) == character;
		if (!shapeOk) {
			if (!g_cqcWarned) {
				g_cqcWarned = true;
				Log::Warn("melee: CQC component at character+0xD8 failed validation -- no melee events");
			}
			return;
		}
		const auto states = Get<std::uintptr_t>(c + 0x58);
		const auto range = Get<std::uintptr_t>(c + 0x28);
		const auto first = range ? Get<std::uint32_t>(*range + 0x24) : std::nullopt;
		if (!states || !first || idx < *first || idx - *first > 16) {
			return;
		}
		const auto s = *states + static_cast<std::uintptr_t>(idx - *first) * 0x130;
		const auto serial = Get<std::uint16_t>(s + 0x114);
		const auto attack = Get<std::uint16_t>(s + 0x118);
		if (!serial || !attack) {
			return;
		}
		if (!g_serialPrimed) {
			g_serialPrimed = true;
			g_lastSerial = *serial;
			return;
		}
		if (*serial == g_lastSerial || *attack == 0) {
			g_lastSerial = *serial;
			return;
		}
		g_lastSerial = *serial;

		// Strike volume: the attack shape if it validates, otherwise 1 m in front of Snake at chest height.
		float      centre[3] = { a_root[0] + std::sin(a_yaw), a_root[1] + 1.0f, a_root[2] + std::cos(a_yaw) };
		float      radius = 1.2f;
		const auto shape = Get<std::uintptr_t>(s + 0xE0);
		if (Cfg().meleeShapeGeometry && shape && *shape && Get<std::uintptr_t>(*shape + 0x20) == c && Get<std::uintptr_t>(*shape + 0x28) == s + 0x110) {
			if (const auto vt = Get<std::uintptr_t>(*shape); vt && *vt >= addr::kImageBase && *vt < addr::kImageBase + addr::kSizeOfImage) {
				if (const auto fn = Get<std::uintptr_t>(*vt + 0x58); fn && *fn >= addr::kImageBase && *fn < addr::kImageBase + addr::kSizeOfImage) {
					using GeometryFn = const std::uint8_t* (*)(void*);
					if (const auto* geo = reinterpret_cast<GeometryFn>(*fn)(reinterpret_cast<void*>(*shape))) {
						float gc[3], gr;
						std::memcpy(gc, geo + 0x10, sizeof(gc));
						std::memcpy(&gr, geo + 0x20, sizeof(gr));
						if (std::isfinite(gc[0]) && std::isfinite(gc[1]) && std::isfinite(gc[2]) && gr > 0.05f && gr < 10.0f) {
							std::copy_n(gc, 3, centre);
							radius = gr;
						}
					}
				}
			}
		}
		proto::GuestEvent e{};
		e.type = proto::kGEvMelee;
		// Attack ids VERIFIED live 2026-10-12 (CQC on empty air, unaimed RT): 80 = punch (sphere at chest height,
		// r 0.2 m), 83 = the combo's kick (sphere 0.3 m above the feet, r 0.3 m); 7 = ATK_Kick per MGS5VR.
		// Throws/chokes need someone to grab, which the sandbox never has: still reported as punches with the raw id.
		e.a = (*attack == 7 || *attack == 83) ? proto::kMeleeKick : proto::kMeleePunch;
		e.b = static_cast<std::uint32_t>(radius * 1000.0f);
		e.frame = static_cast<std::uint32_t>(g_frame.load(std::memory_order_relaxed));
		std::copy_n(centre, 3, e.v0);
		e.v1[0] = std::sin(a_yaw);
		e.v1[2] = std::cos(a_yaw);
		e.pad[0] = *attack;  // raw MGSV attack id, for the live test to map throws/chokes
		link::PushEvent(e);
		Log::Debug("melee: serial {} attack {} centre {:.2f},{:.2f},{:.2f} r {:.2f}", *serial, *attack, centre[0], centre[1], centre[2], radius);
	}
}
