#include "NoiseLogic.h"

#include <algorithm>
#include <cmath>

namespace phantom::er::noise
{
	WeaponClass ClassOfShot(std::uint32_t a_id, bool a_explosive, bool a_flame)
	{
		if (a_flame) {
			return WeaponClass::kFlame;
		}
		if (a_explosive) {
			return WeaponClass::kLauncher;
		}
		// tools/re/data/tppequip_bullet_ids_1.0.15.4.txt
		switch (a_id) {
		case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11:  // Hg* (114Auto .. 44Mag, rubber, tranquilizer)
		case 12: case 13: case 14:                                                         // the water pistol
		case 24: case 25: case 26:                                                         // HgSmoke, HgStun, HgSleep (the grenade pistol's)
		case 102: case 106: case 107:                                                      // EX_hg_000, EX_hg_010, Hg114AutoPP
		case 242:                                                                           // the bionic arm's stun
			return WeaponClass::kPistol;
		case 36: case 37: case 38: case 39: case 104:  // Rf*SniperMag*, EX_sr_000
			return WeaponClass::kSniper;
		case 18: case 19: case 20: case 21: case 22: case 101:  // 12GA*
			return WeaponClass::kShotgun;
		case 1: case 23:  // the cannon, HgGrenade
		case 27: case 28: case 29: case 30: case 31: case 32: case 33: case 34: case 35:  // 40mm / 20mm grenades, the 20mm rocket
		case 63: case 64: case 65: case 96: case 97: case 98: case 103: case 244: case 245:
			return WeaponClass::kLauncher;
		default:
			break;
		}
		if (a_id >= 43 && a_id <= 60) {
			return WeaponClass::kLauncher;  // ms00..ms03: missiles
		}
		if (a_id >= 61 && a_id <= 95) {
			return WeaponClass::kLauncher;  // turrets, tank and heli guns, MGSV's vehicles
		}
		return WeaponClass::kRifle;  // Rf556x45mm, Rf762x51mm, RfRubber, RfTranq, machine guns, and an unknown round (0)
	}

	std::string_view Name(WeaponClass a_class)
	{
		switch (a_class) {
		case WeaponClass::kPistol:
			return "pistol";
		case WeaponClass::kRifle:
			return "rifle";
		case WeaponClass::kSniper:
			return "sniper rifle";
		case WeaponClass::kShotgun:
			return "shotgun";
		case WeaponClass::kLauncher:
			return "launcher";
		case WeaponClass::kFlame:
			return "flamethrower";
		}
		return "?";
	}

	float RadiusOf(WeaponClass a_class, bool a_suppressed, const Radii& a_r)
	{
		float r = 0.0f;
		switch (a_class) {
		case WeaponClass::kPistol:
			r = a_r.pistol;
			break;
		case WeaponClass::kRifle:
			r = a_r.rifle;
			break;
		case WeaponClass::kSniper:
			r = a_r.sniper;
			break;
		case WeaponClass::kShotgun:
			r = a_r.shotgun;
			break;
		case WeaponClass::kLauncher:
			r = a_r.launcher;
			break;
		case WeaponClass::kFlame:
			r = a_r.flame;
			break;
		}
		if (a_suppressed) {
			r = std::min(r, a_r.suppressed);  // a suppressor never makes a shot louder
		}
		return std::isfinite(r) ? std::max(0.0f, r) : 0.0f;
	}

	float ScaleFor(float a_wanted, float a_rowRadius)
	{
		if (!(a_wanted > 0.0f) || !(a_rowRadius > 0.0f) || !std::isfinite(a_wanted) || !std::isfinite(a_rowRadius)) {
			return 0.0f;
		}
		return std::clamp(a_wanted / a_rowRadius, 0.01f, 100.0f);
	}

	void SuppressorWatch::OnShot(bool a_suppressed, std::uint64_t a_nowMs)
	{
		if (!a_suppressed) {
			return;
		}
		lastSuppressedMs_ = a_nowMs;
		shotWeapon_ = weapon_;  // the weapon in hand (the GuestState's id: the shot's own equip id is another numbering)
		if (!wasOn_ && !pendingSince_ && dropAt_ && a_nowMs >= dropAt_ && a_nowMs - dropAt_ <= windowMs && weapon_ == dropWeapon_ && !prevBlocked_) {
			pendingSince_ = a_nowMs;  // the state's drop came in before the shot's event
		}
	}

	bool SuppressorWatch::Update(bool a_on, std::uint32_t a_weapon, bool a_blocked, std::uint64_t a_nowMs)
	{
		bool       worn = false;
		const bool blocked = a_blocked || prevBlocked_;
		if (pendingSince_) {
			if (a_on || a_weapon != shotWeapon_ || blocked) {
				pendingSince_ = 0;  // back on, another weapon, a menu / climb ...: not wear
			} else if (a_nowMs >= pendingSince_ + confirmMs) {
				worn = true;
				pendingSince_ = 0;
				lastSuppressedMs_ = 0;
			}
		} else if (wasOn_ && !a_on && !blocked && lastSuppressedMs_ != 0 && a_nowMs >= lastSuppressedMs_ && a_nowMs - lastSuppressedMs_ <= windowMs &&
				   a_weapon == shotWeapon_ && weapon_ == shotWeapon_) {
			// It dropped right after a suppressed shot, the weapon that fired it still in hand: wear, if it stays so.
			pendingSince_ = a_nowMs;
		}
		if (wasOn_ && !a_on) {
			dropAt_ = blocked ? 0 : a_nowMs;
			dropWeapon_ = a_weapon;
		}
		weapon_ = a_weapon;
		wasOn_ = a_on;
		prevBlocked_ = a_blocked;
		return worn;
	}
}
