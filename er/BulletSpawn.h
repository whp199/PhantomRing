#pragma once

// CSBulletManager::SpawnBullet's argument block, shared by the combat hits (er/Combat.cpp) and the
// explosion visuals (er/Explosion.cpp). fromsoftware-rs cs/bullet_manager.rs BulletSpawnData (0x110)
// [V-src; CT Bullet_code writes the same bullet id / position offsets; the combat route spawns with it live].

#include "Math.h"

#include <cstdint>
#include <cstring>

namespace phantom::er::spawn
{
	// (manager, out handle (-1 = refused), spawn data, error: 3 = bad bullet id, 4 = construction failed)
	using SpawnBulletFn = void (*)(void* a_manager, std::int32_t* a_out, const void* a_spawnData, std::int32_t* a_error);

	inline constexpr std::uint32_t kSize = 0x110;
	inline constexpr std::uint32_t kOwner = 0x00;      // FieldInsHandle
	inline constexpr std::uint32_t kBehaviorId = 0x08;
	inline constexpr std::uint32_t kMagicId = 0x0C;
	inline constexpr std::uint32_t kBulletId = 0x14;
	inline constexpr std::uint32_t kGoodsId = 0x18;
	inline constexpr std::uint32_t kDummyPoly = 0x1C;
	inline constexpr std::uint32_t kTarget = 0x20;     // FieldInsHandle ("replaces the owner's target if not -1")
	// u32 flags. bit0: the spawn's emitter shoots once and is freed when its bullets are gone. Without it the emitter (a
	// 0x9D0-byte entry in CSBulletManager's second list, +0x20: 64 + 192 of them) waits for its owner to end the attack that
	// spawned it -- which a direct spawn never does -- and is never freed: after ~256 spawns every SpawnBullet fails with error
	// 4 (session 4, and reproduced live: the count at +0x1D4 only ever grew). VERIFIED in code (ER 1.17.1): SpawnBullet
	// 0x1403A2CB0 -> 0x1403A5A20 allocates the emitter, 0x14038E550 copies this bit to its params +0x194 (= emitter +0x4C4), and
	// the emitter tick 0x1403900C0 clears its shooting flag (+0x329) when +0x4C4 is set and reports it finished once its bullet
	// count (+0x320) is 0, so the update 0x1403A9840 frees it (0x14038F8A0). Live: with it set the count goes back down.
	inline constexpr std::uint32_t kFlags = 0x44;
	inline constexpr std::uint32_t kFlagOneShot = 0x1;
	inline constexpr std::uint32_t kAccelAngle = 0x50; // forward vector
	inline constexpr std::uint32_t kAngle = 0x70;      // forward vector
	inline constexpr std::uint32_t kPosition = 0x80;

	inline constexpr std::uint64_t kNoTarget = ~0ull;  // -1: keep the owner's own target

	template <class T>
	void Put(std::uint8_t* a_data, std::uint32_t a_offset, const T& a_value)
	{
		std::memcpy(a_data + a_offset, &a_value, sizeof(T));
	}

	// A plain bullet (no spell, item or behaviour scaling: magic = goods = behaviour = -1) owned by a_owner.
	inline void Fill(std::uint8_t (&a_data)[kSize], std::uint64_t a_owner, std::int32_t a_bulletId, std::uint64_t a_target, const Vec3& a_pos, const Vec3& a_dir)
	{
		std::memset(a_data, 0, kSize);
		Put<std::uint64_t>(a_data, kOwner, a_owner);
		Put<std::int32_t>(a_data, kBehaviorId, -1);
		Put<std::int32_t>(a_data, kMagicId, -1);
		Put<std::int32_t>(a_data, kBulletId, a_bulletId);
		Put<std::int32_t>(a_data, kGoodsId, -1);
		Put<std::int32_t>(a_data, kDummyPoly, -1);
		Put<std::uint64_t>(a_data, kTarget, a_target);
		Put<std::uint32_t>(a_data, kFlags, kFlagOneShot);
		const Vec4 forward(a_dir, 0.0f);
		const Vec4 position(a_pos, 0.0f);  // w as CT's zeroed spawn data has it
		Put(a_data, kAccelAngle, forward);
		Put(a_data, kAngle, forward);
		Put(a_data, kPosition, position);
	}
}
