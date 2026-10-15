#pragma once
// Snake (and his buddy) hurting things: what MGSV fired and swung, as guest events.
//
//  Shots: MGSV's ShotSolver (0x141044FF0) computes each round's world origin and direction, after its
//  own spread, once per round from FireWeapon (return address 0x141041219; re-backlog M3). We hook it
//  and send kGEvFire with the weapon, bullet kind and pellets -- Elden Ring resolves the hit. Rounds
//  from another weapon component while a buddy is deployed go out as kGEvBuddyFire (the sandbox has no
//  other shooters). Ammo-counter deltas (Frame.cpp) remain as the fallback when this hook is refused.
//  CQC: the per-player CQC state's attack serial changes on every strike/throw; each new one becomes a
//  kGEvMelee at the attack shape's centre (re-backlog M3 "attack window").

#include "Logic.h"

#include <cstdint>
#include <string>

namespace phantom::guest::combat
{
	bool InstallShots();
	[[nodiscard]] bool NativeShots();
	[[nodiscard]] std::uint64_t BuddyRounds();  // rounds the hook attributed to the buddy so far
	// `!shots`: the v8 damage share (kGEvFire pad[1]) state.
	[[nodiscard]] std::string DescribeDamage();

	// Game thread, from the camera frame: Snake's player index and character, the frame number, and
	// whether a buddy is deployed (decides kGEvFire vs kGEvBuddyFire for foreign weapon components).
	void ObserveOwner(std::uintptr_t a_owner, std::uint64_t a_frame, bool a_buddyDeployed);
	// Game thread: poll the CQC attack serial; a_root/a_yaw place the fallback strike volume.
	void PollMelee(const float a_root[3], float a_yaw);

	// The CQC state PollMelee read last (Cqc.cpp, protocol v13 kGEvCqc): its attack id (pad[0]) and whether its action callback
	// (s+0x10, non-null while MGSV runs a grapple or throw: MGS5VR motion_melee.cpp) is set. valid = read this frame.
	struct CqcProbe
	{
		bool          valid{ false };
		std::uint16_t attack{ 0 };
		bool          action{ false };
	};
	[[nodiscard]] CqcProbe Probe();

	// MGSV bulletId (TppEquip.BL_*) -> what Elden Ring should do with it (the table lives in Logic so it is
	// unit-tested). kNone: smoke/flare/chaff/fulton.
	using BulletKind = logic::BulletKind;
	inline BulletKind KindOfBullet(std::uint32_t a_bulletId) { return logic::KindOfBullet(a_bulletId); }
}
