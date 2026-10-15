#pragma once

// Every Elden Ring address the host uses, in one table (er/Addresses.cpp), each with the check that
// proves it on this exe: a unique AOB that resolves to the expected RVA (globals), the expected
// bytes at the function's RVA (functions), or the RTTI name behind a vtable. Target: eldenring.exe
// 2.7.1.0 (App 1.17.1 WW), sha256 1a354710...8b597891, TimeDateStamp 0x6A96B418. Sources and
// evidence: docs/research/elden-ring-runtime-re.md, tools/er_aob_scan.py (catalog).
//
// A failed entry is logged and left null; features check what they need (Has*) and disable
// themselves instead of guessing.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace phantom::er
{
	enum class A : std::uint32_t
	{
		// static singleton slots (*slot = instance, may be null)
		WorldChrMan,
		GameDataMan,
		GameMan,
		FieldArea,
		CSMenuMan,
		CSFlipper,
		CSCamera,
		CSTask,
		CSTaskGroup,
		FD4PadManager,
		CSHavokMan,
		RendMan,
		CSFade,
		CSNowLoadingHelper,
		WorldAreaTime,
		LockTgtMan,
		CSRemo,              // cutscene manager (re-backlog E4)
		CSActionButtonMan,   // interaction prompts (re-backlog E6)
		SoloParamRepository, // regulation params (combat: bullet/AtkParam rows, sleep SpEffects)
		CSBulletManager,     // combat: SpawnBullet's this
		TeamRelationTable,   // CSTeamTypeBase* [79][79] (who may hit whom), read by 0x51B5D0
		KeyGuideState,       // the key guide object (0x88C bytes, no vtable): +4 = glyph device, 0 pad / 1 keyboard (er/KeyGuide.h)
		GlobalHinstance,
		// functions
		RegisterTask,
		CastRay,
		ChrInsSetPosition,
		ChrCtrlSetPosition,
		ChrInsSetYaw,
		ChrInsKill,
		ChangeHp,
		EzDrawLine,
		GetSingletonName,
		ApplyCameraMask,
		ModifyRootMotion,    // PlayerCtrl vfunc 12 (re-backlog E1)
		RootMotionCallsite,  // ChrCtrl::UpdateRootMotion's `call [rax+0x60]` that proves the slot
		PlayerReceiveDamage, // CSPlayerDamageModule vfunc 7 (re-backlog E2)
		SpawnBullet,         // CSBulletManager::SpawnBullet (combat)
		ApplySpEffect,       // ChrIns::ApplySpEffect(chr, id, dontSync) (combat: sleep build-up)
		GetTeamType,         // ChrIns::GetTeamType(chr, u8* out) (team + SpEffect overrides)
		RemoveSpEffect,      // ChrIns::RemoveSpEffect(chr, id) (combat: re-arm a sleep build-up row)
		KeyGuideCtor,        // the key guide object's constructor: +0 and +4 = Debug.KeyGuideDevice (proves the layout)
		CollisionLayers,     // CS::CSCollisionFilter's layer matrix builder, where layer 0x16 gets layer 3's collisions minus the characters'
		// Snake's own ray query (game::CastRayKeep, docs/HOST.md "Collision filters"): cast_ray's query and collector, so the
		// hits Elden Ring's snipe collector drops (wooden planks: material class 5xx) can be kept for his ground.
		CastRayQuery,        // cast_ray's query build (the hknpRayCastQuery layout: material 0xFFFF, +0x20 = 2, +0x24 = 0xFB)
		CastRayCalls,        // ... its filter/codec/ignore fields and the collector ctor, set-ignore and world-cast calls
		WorldCastRay,        // hknpWorld cast ray (0x187F7D0: [world+0x4D8] vfunc 0x80)
		SnipeCollectorCtor,  // CS::CSClosestHitCollectorForSnipe ctor
		SnipeCollectorDtor,
		SnipeAddHit,         // its addHit (vfunc 4): the mount skip, then the raycast collector's (the material-class drop)
		ClosestAddHit,       // hknpClosestHitCollector::addHit: the closest hit, no material check
		HitMaterialClass,    // (bodyId*, shapeKey, int* out, remap) -> the hit's material id (hundreds = its class)
		// vtables
		VtChrIns,
		VtPlayerIns,
		VtEnemyIns,
		VtChrCtrl,
		VtPlayerCtrl,        // what PlayerIns actually holds at +0x58 (its ctor installs it over ChrCtrl's)
		VtPlayerDamageModule,
		VtActionButtonManImp,
		VtPhysics,
		VtData,
		VtChrCam,
		VtCSCam,
		VtPersCam,
		VtDebugCam,
		VtEzDraw,
		VtPhysWorld,
		VtHavokManImp,
		VtRendManImp,
		VtCameraImp,
		VtMenuManImp,
		VtWorldChrManImp,
		VtTaskImp,
		VtFlipperImp,
		VtChrSet,
		VtOpenFieldChrSet,
		VtSoloParamRepositoryImp,
		VtParamResCap,
		VtTeamTypeEnemy,
		VtTeamTypeRival,
		VtGameRend,          // FieldArea+0x20: its +0x18 CSPersCam is the camera ER's HUD compass reads (MoveMapStep 0xB01FC0 -> 0x7774A0)
		VtSnipeCollector,    // CS::CSClosestHitCollectorForSnipe (game::CastRayKeep copies it, addHit replaced)
		Count
	};

	inline constexpr std::uint32_t kExpectedTimeDateStamp = 0x6A96B418;
	inline constexpr std::uint32_t kExpectedSizeOfImage = 0x5E0DA00;
	inline constexpr char          kExpectedSha256[] = "1a3547101327f65d0c76da2f9190ac0aa66871ea42bae2aecc61e11a8b597891";

	struct Addresses
	{
		std::uint8_t* base{ nullptr };
		std::size_t   size{ 0 };
		std::array<std::uint8_t*, static_cast<std::size_t>(A::Count)> at{};

		[[nodiscard]] std::uint8_t* operator[](A a_id) const { return at[static_cast<std::size_t>(a_id)]; }
		[[nodiscard]] bool          Has(A a_id) const { return at[static_cast<std::size_t>(a_id)] != nullptr; }
		template <class... Ids>
		[[nodiscard]] bool HasAll(Ids... a_ids) const
		{
			return (Has(a_ids) && ...);
		}
		// Instance behind a singleton slot (nullptr if unresolved or not created yet).
		[[nodiscard]] std::uint8_t* Instance(A a_slot) const;
		template <class F>
		[[nodiscard]] F Fn(A a_id) const
		{
			return reinterpret_cast<F>(at[static_cast<std::size_t>(a_id)]);
		}
		[[nodiscard]] bool InImage(const void* a_p) const
		{
			return a_p >= base && a_p < base + size;
		}
		[[nodiscard]] std::uintptr_t Rva(const void* a_p) const
		{
			return reinterpret_cast<std::uintptr_t>(a_p) - reinterpret_cast<std::uintptr_t>(base);
		}
	};

	const Addresses& Addr();

	// Exe identity: TimeDateStamp + SizeOfImage of the main module (+ the file's sha256 if asked).
	// Returns false (and logs why) for anything that isn't the exact supported build.
	bool CheckExeIdentity(bool a_sha256);

	// Resolves and verifies the whole table; logs one line per entry. Returns the number of failures.
	int ResolveAddresses();

	// Offline verification only (tests/verify_er_addresses.cpp): use an image mapped with
	// LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES) instead of the main module, after the same
	// TimeDateStamp/SizeOfImage check.
	bool UseImageForVerification(void* a_base);

	std::string_view Name(A a_id);
}
