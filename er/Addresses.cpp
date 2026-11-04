#include "Addresses.h"

#include "Hook.h"
#include "Log.h"
#include "Mem.h"

#include <bcrypt.h>

#include <cstring>
#include <format>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

namespace phantom::er
{
	namespace
	{
		enum class Kind
		{
			kGlobalScan,  // unique AOB, RIP operand -> slot; slot must be base+rva
			kGlobalSite,  // bytes at `site` match; RIP operand -> slot; slot must be base+rva
			kFunc,        // bytes at base+rva match the pattern
			kFuncScan,    // unique AOB, rel32 call/jmp -> function == base+rva; prologue checked too
			kFuncJmp,     // bytes at base+rva match; its rel32 jmp lands on `site`, whose bytes match `pattern2`
			kVtable,      // vtable at base+rva: its COL's type descriptor is `rtti`
		};

		struct Entry
		{
			A             id;
			const char*   name;
			Kind          kind;
			std::uint32_t rva;
			const char*   pattern{ nullptr };
			int           disp{ 0 };
			int           end{ 0 };
			std::uint32_t site{ 0 };
			const char*   pattern2{ nullptr };
			const char*   rtti{ nullptr };
		};

		// clang-format off
		// eldenring.exe 2.7.2.0 (App 1.17.2 WW). Every entry is the 2.7.1.0 one below, re-found in this exe and diffed
		// against it (the 2.7.1.0 image rebuilt from the Ghidra project): each function's reachable code is the same
		// instruction for instruction (rel32/RIP operands aside), each global the same FD4 singleton (by name) or the
		// same site, each vtable the same RTTI class with the same slots. Only FD4PadManager needed a new AOB (the old
		// one ran into an Arxan stub's bytes, which differ per build) and cast_ray's world-cast call a new rel32.
		constexpr Entry kTable2720[] = {
			// ---- singleton slots (CT baseData / PR mkpattern; tools/er_aob_scan.py verify) ----
			{ A::WorldChrMan, "WorldChrMan", Kind::kGlobalScan, 0x3D6D018, "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88", 3, 7 },
			{ A::GameDataMan, "GameDataMan", Kind::kGlobalScan, 0x3D64FB8, "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3", 3, 7 },
			{ A::GameMan, "GameMan", Kind::kGlobalScan, 0x3D709A8, "48 8B 05 ?? ?? ?? ?? 80 B8 ?? ?? ?? ?? 0D 0F 94 C0 C3", 3, 7 },
			{ A::FieldArea, "FieldArea", Kind::kGlobalScan, 0x3D70268, "48 8B 3D ?? ?? ?? ?? 49 8B D8 48 8B F2 4C 8B F1 48 85 FF", 3, 7 },
			{ A::CSMenuMan, "CSMenuMan", Kind::kGlobalScan, 0x3D72840, "48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24", 3, 7 },
			{ A::CSFlipper, "CSFlipper", Kind::kGlobalScan, 0x4590B78, "48 8B 0D ?? ?? ?? ?? 80 BB ?? ?? ?? ?? 00 0F 84", 3, 7 },
			{ A::CSCamera, "CSCamera", Kind::kGlobalScan, 0x3D699E8, "48 8B 0D ?? ?? ?? ?? 48 8D 97 E0 00 00 00", 3, 7 },
			{ A::CSTask, "CSTask", Kind::kGlobalScan, 0x4592EA8, "48 8B 0D ?? ?? ?? ?? 4C 8B C3 BA 9C 00 00 00", 3, 7 },
			{ A::CSTaskGroup, "CSTaskGroup", Kind::kGlobalScan, 0x4592F28, "48 8B 0D ?? ?? ?? ?? 49 8B 1F BA 32 00 00 00", 3, 7 },
			// a plain load + call (unique in 2.7.1.0 and 2.7.2.0, same slot as the singleton finder's FD4PadManager)
			{ A::FD4PadManager, "FD4PadManager", Kind::kGlobalScan, 0x4864D50, "48 8B 0D ?? ?? ?? ?? 33 D2 E8 ?? ?? ?? ?? 4C 8B F0", 3, 7 },
			{ A::CSHavokMan, "CSHavokMan", Kind::kGlobalScan, 0x3D7D0F0, "48 8B 0D ?? ?? ?? ?? 48 8B 9D 80 00 00 00", 3, 7 },
			{ A::RendMan, "RendMan", Kind::kGlobalScan, 0x3D82150, "48 8B 05 ?? ?? ?? ?? 48 8B 48 10 48 85 C9 75 0D 48 8B CB", 3, 7 },
			{ A::CSFade, "CSFade", Kind::kGlobalScan, 0x3D793D0, "48 8B 0D ?? ?? ?? ?? 0F 10 00 0F 11 44 24 40", 3, 7 },
			{ A::CSNowLoadingHelper, "CSNowLoadingHelper", Kind::kGlobalScan, 0x3D67F48, "48 8B 0D ?? ?? ?? ?? B8 E8 03 00 00", 3, 7 },
			{ A::WorldAreaTime, "WorldAreaTime", Kind::kGlobalScan, 0x3D70388, "48 8B 0D ?? ?? ?? ?? 8B 89 D8 00 00 00", 3, 7 },
			{ A::LockTgtMan, "LockTgtMan", Kind::kGlobalScan, 0x3D71298, "48 8B 05 ?? ?? ?? ?? 0F B6 5B 0C 48 85 C0", 3, 7 },
			// re-backlog E4: IsMenuOrCutsceneSuppressing 0x80E940 reads it right after CSMenuMan
			{ A::CSRemo, "CSRemo", Kind::kGlobalScan, 0x3D75AE8, "48 85 C0 74 ?? 80 B8 ?? ?? ?? ?? 00 0F B6 DB 0F 45 DF 48 8B 0D", 21, 25 },
			// re-backlog E6
			{ A::CSActionButtonMan, "CSActionButtonMan", Kind::kGlobalScan, 0x3D75498, "48 8B 0D ?? ?? ?? ?? 80 79 2B 00 75 14", 3, 7 },
			// combat (docs/COMBAT.md): CT's SoloParamRepository site; a PR-generated CSBulletManager load; the
			// team relation table through the only function that indexes it (team a * 79 + team b)
			{ A::SoloParamRepository, "SoloParamRepository", Kind::kGlobalScan, 0x3D88F78,
				"48 89 5C 24 48 8B FA 48 8B D9 C7 44 24 20 00 00 00 00 48 8B ?? ?? ?? ?? ??", 21, 25 },
			{ A::CSBulletManager, "CSBulletManager", Kind::kGlobalScan, 0x3D697C8, "48 8B 0D ?? ?? ?? ?? 8B 3B 48 85 C9", 3, 7 },
			{ A::TeamRelationTable, "team relation table", Kind::kGlobalSite, 0x3B2B3F0,
				"48 83 EC 28 0F B6 01 45 33 C0 48 6B C8 4F 0F B6 02 48 8D 54 24 30 48 03 C8 66 C7 44 24 30 01 00 48 8D 05 ?? ?? ?? ??", 35, 39, 0x51B870 },
			// Key guide glyphs (er/KeyGuide.h): the 0x88C-byte object 0xDEE610 allocates and builds with KeyGuideCtor, stored here
			{ A::KeyGuideState, "key guide state", Kind::kGlobalSite, 0x3D64F28, "48 8B C6 48 89 05 ?? ?? ?? ?? 48 83 3D", 6, 10, 0xDEF655 },
			// fsrs global_hinstance: its setter `mov [rip+x],rcx; ret` (non-zero once system init is done)
			{ A::GlobalHinstance, "global_hinstance", Kind::kGlobalSite, 0x3D8C728, "48 89 0D ?? ?? ?? ?? C3", 3, 7, 0xDEDDA0 },

			// ---- functions ----
			{ A::RegisterTask, "register_task", Kind::kFuncScan, 0xEB5540, "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8B C7 8B D3 E8 ?? ?? ?? ??", 18, 22, 0,
				"48 89 5C 24 08 57 48 83 EC 40 48 8D 4C 24 20 49 8B D8 8B FA" },
			{ A::CastRay, "cs_phys_world_cast_ray", Kind::kFuncScan, 0xC722C0, "BA 58 00 00 02 E8 ?? ?? ?? ??", 6, 10, 0,
				"40 55 56 57 41 54 41 55 41 56" },
			{ A::ChrInsSetPosition, "ChrIns::SetPosition", Kind::kFuncJmp, 0x3F2690, "48 8B 49 58 E9 ?? ?? ?? ??", 5, 9, 0x3C7680,
				"48 89 5C 24 08 57 48 83 EC 20 48 8B 41 10 48 8B D9 48 8B 88 90 01 00 00 48 8B 49 68 E8" },
			{ A::ChrCtrlSetPosition, "ChrCtrl::SetPosition", Kind::kFunc, 0x3C7680,
				"48 89 5C 24 08 57 48 83 EC 20 48 8B 41 10 48 8B D9 48 8B 88 90 01 00 00 48 8B 49 68 E8 ?? ?? ?? ?? 48 8B 43 10" },
			{ A::ChrInsSetYaw, "ChrIns::SetYaw", Kind::kFuncJmp, 0x3F6210, "48 8B 49 58 E9 ?? ?? ?? ??", 5, 9, 0x3C7EE0,
				"48 8B 41 10 48 8B 88 90 01 00 00 48 8B 49 68 E9" },
			{ A::ChrInsKill, "ChrIns::Kill", Kind::kFunc, 0x3EE730,
				"48 89 5C 24 08 57 48 83 EC 30 F3 0F 10 05 ?? ?? ?? ?? 48 8B D9 48 8B 89 90 01 00 00 0F B6 FA" },
			{ A::ChangeHp, "CSChrDataModule::ChangeHp", Kind::kFunc, 0x4388A0,
				"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 0F 29 74 24 30 0F 28 F3 41 0F B6 F0 8B DA 48 8B F9 85 D2 7F 0E" },
			{ A::EzDrawLine, "CSEzDraw::DrawLine", Kind::kFunc, 0x2653D70,
				"40 57 48 83 EC 40 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50 48 89 6C 24 58 48 89 74 24 60 49 8B F0 48 8B FA 48 8B D9 48 8D 69 28" },
			{ A::GetSingletonName, "get_singleton_name", Kind::kFunc, 0x1EC4940, "48 83 EC 28 E8 27 FF FF FF 48 85 C0 74 0E 48 8B 10 48 8B C8" },
			{ A::ApplyCameraMask, "CSCamera::ApplyCameraMask", Kind::kFunc, 0x3BB3E0, "8B 41 28 4C 8B C9 A8 20 74 ?? 48 8B 51 20 4C 8B 41 08" },
			// re-backlog E1. The body has an EnemyCtrl twin (0x3D1BE0), so the bytes are checked here and the
			// slot is checked against the live PlayerCtrl vtable before any swap (er/RootMotion.cpp).
			{ A::ModifyRootMotion, "PlayerCtrl::ModifyRootMotion", Kind::kFunc, 0x3DE610,
				"48 8B C4 48 89 58 18 48 89 78 20 55 48 8D 68 A1 48 81 EC B0 00 00 00 0F 29 70 E8 0F 29 78 D8 44 0F 29 40 C8 44 0F 29 48 B8 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 07 48 8B 41 10 48 8B FA" },
			{ A::RootMotionCallsite, "ChrCtrl root-motion vcall", Kind::kFunc, 0x3CC85C, "0F 29 07 48 8B 03 48 8B D7 48 8B CB FF 50 60 48 8B 43 10" },
			// re-backlog E2. Its tail (the damage worker 0x445650) gained a branch for four attack ids in 2.7.2.0; the
			// ChrDamage fields HitHook reads are untouched.
			{ A::PlayerReceiveDamage, "CSPlayerDamageModule::ReceiveDamage", Kind::kFunc, 0x44D130,
				"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 0F B6 80 15 01 00 00" },

			// combat (fsrs cs_bullet_manager_spawn_bullet / chr_ins_apply_speffect, CT Bullet_code; PR GetTeamType)
			{ A::SpawnBullet, "CSBulletManager::SpawnBullet", Kind::kFunc, 0x3A2CB0, "40 53 55 56 57 48 81 EC 98 07 00 00 48 C7 44 24 50 FE FF FF FF" },
			{ A::ApplySpEffect, "ChrIns::ApplySpEffect", Kind::kFunc, 0x3E8DC0,
				"48 8B C4 48 89 58 08 48 89 70 10 57 48 81 EC C0 00 00 00 0F 28 05 ?? ?? ?? ?? 48 8B F1" },
			{ A::GetTeamType, "ChrIns::GetTeamType", Kind::kFunc, 0x3F1C90,
				"48 89 5C 24 10 57 48 83 EC 20 0F B6 41 6C 48 8B F9 88 02 48 8B DA 48 8B 89 78 01 00 00" },
			// fsrs chr_ins_remove_speffect: `mov rcx,[rcx+0x178]` (the SpEffect module) + jmp to the module's remove
			{ A::RemoveSpEffect, "ChrIns::RemoveSpEffect", Kind::kFuncJmp, 0x3EE2E0, "48 8B 89 78 01 00 00 E9 ?? ?? ?? ??", 8, 12, 0x4F7760,
				"48 83 EC 28 8B C2 48 8B 51 08 48 85 D2 74 0F" },

			// obj+0 = obj+4 = (byte) the clamped "Debug.KeyGuideDevice" config value (0xE4A210), the rest zeroed: the device bytes
			{ A::KeyGuideCtor, "key guide ctor", Kind::kFunc, 0x242790,
				"48 89 5C 24 08 57 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? 88 07 33 C0 66 89 47 01 88 47 03 E8 ?? ?? ?? ?? 88 47 04" },

			// Collision layers (docs/HOST.md "Collision filters"): CS::CSCollisionFilter (vtable 0x2B96EE0, its slot 8 = 0xC5F6B0)
			// builds the 128x128 layer matrix; here row 0x16 is made: copy layer 3's row (0xC5F620), then drop every character
			// layer, 2-0x2D (0xC629F0). Both callees moved with the caller, so the rel32 bytes are 2.7.1.0's.
			{ A::CollisionLayers, "collision layer 0x16 (CSCollisionFilter)", Kind::kFunc, 0xC6050E,
				"BA 16 00 00 00 48 8B CB 44 8D 42 ED E8 01 F1 FF FF BA 16 00 00 00 48 8B CB E8 C4 24 00 00" },
			// Snake's own ray query (game::CastRayKeep; docs/HOST.md "Collision filters"): cast_ray 0xC722C0, its snipe
			// collector ctor 0xC5AEB0, set-ignore 0xC5B0A0, world cast 0x1880EC0 (the one call whose rel32 changed), addHit
			// 0xC5B0B0 -> material drop 0xC5ADA0, material id 0xC75A30, hknpClosestHitCollector's addHit 0x1887C00.
			{ A::CastRayQuery, "cast_ray query build", Kind::kFunc, 0xC7234D,
				"B8 FF FF 00 00 66 89 44 24 50 33 C0 89 44 24 54 48 89 44 24 58 48 89 44 24 48 48 89 44 24 40 C7 44 24 60 02 00 00 00 "
				"C6 44 24 64 FB" },
			{ A::CastRayCalls, "cast_ray query calls", Kind::kFunc, 0xC72416,
				"48 8B 80 C0 00 00 00 48 89 44 24 48 48 8B 47 08 48 8B 88 00 0B 00 00 48 89 4C 24 40 44 89 7C 24 54 48 89 74 24 58 "
				"48 8D 4D D0 E8 6B 8A FE FF 90 48 8B D6 48 8D 4D D0 E8 4E 8C FE FF 4C 8D 45 D0 48 8D 54 24 40 48 8B 4F 08 E8 5C EA C0 00 "
				"83 7D DC 00" },
			{ A::WorldCastRay, "hknpWorld cast ray", Kind::kFunc, 0x1880EC0,
				"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B E9 49 8B F8 8B 0D ?? ?? ?? ?? 48 8B F2 FF 15" },
			{ A::SnipeCollectorCtor, "snipe collector ctor", Kind::kFunc, 0xC5AEB0,
				"48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 E8 D5 FB FF FF 90 48 8D 05 ?? ?? ?? ?? 48 89 03" },
			{ A::SnipeCollectorDtor, "snipe collector dtor", Kind::kFunc, 0xC5AF70,
				"48 89 4C 24 08 48 83 EC 38 48 C7 44 24 20 FE FF FF FF 48 8D 05 ?? ?? ?? ?? 48 89 01" },
			{ A::SnipeAddHit, "snipe collector addHit", Kind::kFunc, 0xC5B0B0,
				"48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 8B 42 48 48 8B FA 25 FF FF FF 00" },
			{ A::ClosestAddHit, "hknpClosestHitCollector addHit", Kind::kFunc, 0x1887C00,
				"F3 0F 10 42 20 4C 8D 4A 20 0F 28 C8 4C 8B C1 0F 28 41 10 0F C6 C9 00 0F 2E C1 76 3B" },
			{ A::HitMaterialClass, "hit material id", Kind::kFunc, 0xC75A30,
				"48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 89 54 24 10 57 48 83 EC 20 8B 01 40 32 ED 25 FF FF FF 00" },

			// ---- vtables (MSVC RTTI) ----
			{ A::VtChrIns, "vt CS::ChrIns", Kind::kVtable, 0x2A330D8, nullptr, 0, 0, 0, nullptr, ".?AVChrIns@CS@@" },
			{ A::VtPlayerIns, "vt CS::PlayerIns", Kind::kVtable, 0x2A81CB0, nullptr, 0, 0, 0, nullptr, ".?AVPlayerIns@CS@@" },
			{ A::VtEnemyIns, "vt CS::EnemyIns", Kind::kVtable, 0x2A49090, nullptr, 0, 0, 0, nullptr, ".?AVEnemyIns@CS@@" },
			{ A::VtChrCtrl, "vt CS::ChrCtrl", Kind::kVtable, 0x2A2E6B0, nullptr, 0, 0, 0, nullptr, ".?AVChrCtrl@CS@@" },
			{ A::VtPlayerCtrl, "vt CS::PlayerCtrl", Kind::kVtable, 0x2A30B20, nullptr, 0, 0, 0, nullptr, ".?AVPlayerCtrl@CS@@" },
			{ A::VtPlayerDamageModule, "vt CS::CSPlayerDamageModule", Kind::kVtable, 0x2A3C3E0, nullptr, 0, 0, 0, nullptr, ".?AVCSPlayerDamageModule@CS@@" },
			{ A::VtActionButtonManImp, "vt CS::CSActionButtonManImp", Kind::kVtable, 0x2B4A230, nullptr, 0, 0, 0, nullptr, ".?AVCSActionButtonManImp@CS@@" },
			{ A::VtPhysics, "vt CS::CSChrPhysicsModule", Kind::kVtable, 0x2A3E890, nullptr, 0, 0, 0, nullptr, ".?AVCSChrPhysicsModule@CS@@" },
			{ A::VtData, "vt CS::CSChrDataModule", Kind::kVtable, 0x2A3A0C8, nullptr, 0, 0, 0, nullptr, ".?AVCSChrDataModule@CS@@" },
			{ A::VtChrCam, "vt CS::ChrCam", Kind::kVtable, 0x2A2CA18, nullptr, 0, 0, 0, nullptr, ".?AVChrCam@CS@@" },
			{ A::VtCSCam, "vt CS::CSCam", Kind::kVtable, 0x2AA2B08, nullptr, 0, 0, 0, nullptr, ".?AVCSCam@CS@@" },
			{ A::VtPersCam, "vt CS::CSPersCam", Kind::kVtable, 0x2AA2B30, nullptr, 0, 0, 0, nullptr, ".?AVCSPersCam@CS@@" },
			{ A::VtDebugCam, "vt CS::CSDebugCam", Kind::kVtable, 0x2B693A0, nullptr, 0, 0, 0, nullptr, ".?AVCSDebugCam@CS@@" },
			{ A::VtEzDraw, "vt CS::CSEzDraw", Kind::kVtable, 0x2B835B0, nullptr, 0, 0, 0, nullptr, ".?AVCSEzDraw@CS@@" },
			{ A::VtPhysWorld, "vt CS::CSPhysWorld", Kind::kVtable, 0x2B98A68, nullptr, 0, 0, 0, nullptr, ".?AVCSPhysWorld@CS@@" },
			{ A::VtHavokManImp, "vt CS::CSHavokManImp", Kind::kVtable, 0x2B955D8, nullptr, 0, 0, 0, nullptr, ".?AVCSHavokManImp@CS@@" },
			{ A::VtRendManImp, "vt CS::RendManImp", Kind::kVtable, 0x2BA44E8, nullptr, 0, 0, 0, nullptr, ".?AVRendManImp@CS@@" },
			{ A::VtCameraImp, "vt CS::CSCameraImp", Kind::kVtable, 0x2A2D000, nullptr, 0, 0, 0, nullptr, ".?AVCSCameraImp@CS@@" },
			{ A::VtMenuManImp, "vt CS::CSMenuManImp", Kind::kVtable, 0x2AA0FC8, nullptr, 0, 0, 0, nullptr, ".?AVCSMenuManImp@CS@@" },
			{ A::VtWorldChrManImp, "vt CS::WorldChrManImp", Kind::kVtable, 0x2A509D0, nullptr, 0, 0, 0, nullptr, ".?AVWorldChrManImp@CS@@" },
			{ A::VtTaskImp, "vt CS::CSTaskImp", Kind::kVtable, 0x2C06DB0, nullptr, 0, 0, 0, nullptr, ".?AVCSTaskImp@CS@@" },
			{ A::VtFlipperImp, "vt CS::CSFlipperImp", Kind::kVtable, 0x2C01608, nullptr, 0, 0, 0, nullptr, ".?AVCSFlipperImp@CS@@" },
			{ A::VtChrSet, "vt CS::ChrSet", Kind::kVtable, 0x2A43348, nullptr, 0, 0, 0, nullptr, ".?AVChrSet@CS@@" },
			{ A::VtOpenFieldChrSet, "vt CS::OpenFieldChrSet", Kind::kVtable, 0x2A4D620, nullptr, 0, 0, 0, nullptr, ".?AVOpenFieldChrSet@CS@@" },
			{ A::VtSoloParamRepositoryImp, "vt CS::SoloParamRepositoryImp", Kind::kVtable, 0x2BBA778, nullptr, 0, 0, 0, nullptr,
				".?AVSoloParamRepositoryImp@CS@@" },
			{ A::VtParamResCap, "vt CS::ParamResCap", Kind::kVtable, 0x2BB4CA8, nullptr, 0, 0, 0, nullptr, ".?AVParamResCap@CS@@" },
			{ A::VtTeamTypeEnemy, "vt CSTeamTypeEnemy", Kind::kVtable, 0x2A518D8, nullptr, 0, 0, 0, nullptr, ".?AVCSTeamTypeEnemy@@" },
			{ A::VtTeamTypeRival, "vt CSTemaTypeRival", Kind::kVtable, 0x2A518E8, nullptr, 0, 0, 0, nullptr, ".?AVCSTemaTypeRival@@" },
			{ A::VtGameRend, "vt CS::GameRend", Kind::kVtable, 0x2A84438, nullptr, 0, 0, 0, nullptr, ".?AVGameRend@CS@@" },
			{ A::VtSnipeCollector, "vt CS::CSClosestHitCollectorForSnipe", Kind::kVtable, 0x2B96768, nullptr, 0, 0, 0, nullptr,
				".?AVCSClosestHitCollectorForSnipe@CS@@" },

			// ---- AI sounds (er/Noise.h; docs/HOST.md "Noise") ----
			// CS::CSWorldAiManager (static singleton finder: slot 0x3D69568), read at 0x2BCD3C as in 2.7.1.0 (0x3D66548 there).
			{ A::WorldAiManager, "CSWorldAiManager", Kind::kGlobalSite, 0x3D69568, "48 8B 0D ?? ?? ?? ?? 8B 1B 8B 38 48 85 C9", 3, 7, 0x2BCD3C },
			// Its AddAiSound: `cmp [rcx+0x7628],0` (the sound system), builds the request (0x348890 -> 0x348760) and hands it to the
			// system's add (0x34A1B0). The whole AI sound family kept its RVAs in 2.7.2.0 (0x32A8E0..0x37A6C0, the same code).
			{ A::AiSoundEmit, "CSWorldAiManager::AddAiSound", Kind::kFunc, 0x37A6C0,
				"40 53 48 81 EC A0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 90 00 00 00 48 83 B9 28 76 00 00 00 48 8B D9 48 8B 84 24 D0 00 00 00 "
				"74 20 48 8D 4C 24 30 48 89 44 24 20 E8 91 E1 FC FF 48 8B 8B 28 76 00 00 48 8D 54 24 30 E8 A0 FA FC FF" },
			{ A::AiSoundAdd, "AI sound system add", Kind::kFunc, 0x34A1B0,
				"48 8B C4 56 57 41 54 41 56 41 57 48 81 EC E0 00 00 00 48 C7 44 24 20 FE FF FF FF 48 89 58 18 48 89 68 20 48 8B 05 ?? ?? ?? ?? "
				"48 33 C4 48 89 84 24 D0 00 00 00 4C 8B F2 48 8B E9 4C 8D 79 20" },
			{ A::AiSoundRequest, "AI sound request builder", Kind::kFunc, 0x348760,
				"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 0F 28 02 48 8B F1 0F 11 01 48 83 C1 10 49 8B D0 0F 29 74 24 30 "
				"49 8B F9 49 8B D8 E8 0C 05 00 00 8B 43 18 F3 0F 10 35 ?? ?? ?? ?? 89 46 28" },
			{ A::VtTargetingSystem, "vt CS::CSTargetingSystem", Kind::kVtable, 0x2A1FAE8, nullptr, 0, 0, 0, nullptr, ".?AVCSTargetingSystem@CS@@" },
			{ A::VtAiTargetingOwner, "vt CS::CSAiTargetingSystemOwner", Kind::kVtable, 0x2A20550, nullptr, 0, 0, 0, nullptr,
				".?AVCSAiTargetingSystemOwner@CS@@" },
			{ A::VtTargetSearchSys, "vt CS::CSTargetSearchSys", Kind::kVtable, 0x2A20420, nullptr, 0, 0, 0, nullptr, ".?AVCSTargetSearchSys@CS@@" },
			// CSAiTargetingSystemOwner's slots 14 / 15 (+0x70 / +0x78: is_battle_state / is_not_in_any_search_state, fromsoftware-rs
			// CSTargetingSystemOwnerVmt), read by er/Noise.cpp ReadAi (review 0.7.0: it called them unverified). The same RVAs and code
			// in 2.7.1.0 (its Ghidra project: 0x142A1E5B0 -> 0x140345E50, 0x142A1E5B8 -> 0x140345E60) and 2.7.2.0 (its exe: the vtable at
			// 0x2A20550 holds 0x140345E50 / 0x140345E60); ReadAi also checks that the live vtable slot is this very function.
			{ A::AiOwnerIsBattle, "CSAiTargetingSystemOwner::IsBattleState", Kind::kFunc, 0x345E50, "48 8B 49 08 E9 87 D3 F7 FF" },
			{ A::AiOwnerNoSearch, "CSAiTargetingSystemOwner::IsNotInAnySearchState", Kind::kFunc, 0x345E60,
				"40 53 48 83 EC 20 48 8B D9 48 8B 49 08 E8 ?? ?? ?? ?? 84 C0 75 13 48 8B 4B 08 E8 ?? ?? ?? ?? 84 C0 75 06 48 83 C4 20 5B C3 B0 01 48 83 C4 20 5B C3" },
		};

		// eldenring.exe 2.7.1.0 (App 1.17.1 WW): the table PhantomRing 0.1-0.4 was built and live-tested on.
		constexpr Entry kTable2710[] = {
			// ---- singleton slots (CT baseData / PR mkpattern; tools/er_aob_scan.py verify) ----
			{ A::WorldChrMan, "WorldChrMan", Kind::kGlobalScan, 0x3D69FF8, "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88", 3, 7 },
			{ A::GameDataMan, "GameDataMan", Kind::kGlobalScan, 0x3D61F98, "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3", 3, 7 },
			{ A::GameMan, "GameMan", Kind::kGlobalScan, 0x3D6D988, "48 8B 05 ?? ?? ?? ?? 80 B8 ?? ?? ?? ?? 0D 0F 94 C0 C3", 3, 7 },
			{ A::FieldArea, "FieldArea", Kind::kGlobalScan, 0x3D6D248, "48 8B 3D ?? ?? ?? ?? 49 8B D8 48 8B F2 4C 8B F1 48 85 FF", 3, 7 },
			{ A::CSMenuMan, "CSMenuMan", Kind::kGlobalScan, 0x3D6F820, "48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24", 3, 7 },
			{ A::CSFlipper, "CSFlipper", Kind::kGlobalScan, 0x458DB58, "48 8B 0D ?? ?? ?? ?? 80 BB ?? ?? ?? ?? 00 0F 84", 3, 7 },
			{ A::CSCamera, "CSCamera", Kind::kGlobalScan, 0x3D669C8, "48 8B 0D ?? ?? ?? ?? 48 8D 97 E0 00 00 00", 3, 7 },
			{ A::CSTask, "CSTask", Kind::kGlobalScan, 0x458FE88, "48 8B 0D ?? ?? ?? ?? 4C 8B C3 BA 9C 00 00 00", 3, 7 },
			{ A::CSTaskGroup, "CSTaskGroup", Kind::kGlobalScan, 0x458FF08, "48 8B 0D ?? ?? ?? ?? 49 8B 1F BA 32 00 00 00", 3, 7 },
			{ A::FD4PadManager, "FD4PadManager", Kind::kGlobalScan, 0x4861D30, "48 8B 0D ?? ?? ?? ?? E9 ?? ?? ?? ?? B0 01 C3 90 8B D1", 3, 7 },
			{ A::CSHavokMan, "CSHavokMan", Kind::kGlobalScan, 0x3D7A0D0, "48 8B 0D ?? ?? ?? ?? 48 8B 9D 80 00 00 00", 3, 7 },
			{ A::RendMan, "RendMan", Kind::kGlobalScan, 0x3D7F130, "48 8B 05 ?? ?? ?? ?? 48 8B 48 10 48 85 C9 75 0D 48 8B CB", 3, 7 },
			{ A::CSFade, "CSFade", Kind::kGlobalScan, 0x3D763B0, "48 8B 0D ?? ?? ?? ?? 0F 10 00 0F 11 44 24 40", 3, 7 },
			{ A::CSNowLoadingHelper, "CSNowLoadingHelper", Kind::kGlobalScan, 0x3D64F28, "48 8B 0D ?? ?? ?? ?? B8 E8 03 00 00", 3, 7 },
			{ A::WorldAreaTime, "WorldAreaTime", Kind::kGlobalScan, 0x3D6D368, "48 8B 0D ?? ?? ?? ?? 8B 89 D8 00 00 00", 3, 7 },
			{ A::LockTgtMan, "LockTgtMan", Kind::kGlobalScan, 0x3D6E278, "48 8B 05 ?? ?? ?? ?? 0F B6 5B 0C 48 85 C0", 3, 7 },
			// re-backlog E4: IsMenuOrCutsceneSuppressing 0x80E490 reads it right after CSMenuMan
			{ A::CSRemo, "CSRemo", Kind::kGlobalScan, 0x3D72AC8, "48 85 C0 74 ?? 80 B8 ?? ?? ?? ?? 00 0F B6 DB 0F 45 DF 48 8B 0D", 21, 25 },
			// re-backlog E6
			{ A::CSActionButtonMan, "CSActionButtonMan", Kind::kGlobalScan, 0x3D72478, "48 8B 0D ?? ?? ?? ?? 80 79 2B 00 75 14", 3, 7 },
			// combat (docs/COMBAT.md): CT's SoloParamRepository site; a PR-generated CSBulletManager load; the
			// team relation table through the only function that indexes it (team a * 79 + team b)
			{ A::SoloParamRepository, "SoloParamRepository", Kind::kGlobalScan, 0x3D85F58,
				"48 89 5C 24 48 8B FA 48 8B D9 C7 44 24 20 00 00 00 00 48 8B ?? ?? ?? ?? ??", 21, 25 },
			{ A::CSBulletManager, "CSBulletManager", Kind::kGlobalScan, 0x3D667A8, "48 8B 0D ?? ?? ?? ?? 8B 3B 48 85 C9", 3, 7 },
			{ A::TeamRelationTable, "team relation table", Kind::kGlobalSite, 0x3B283F0,
				"48 83 EC 28 0F B6 01 45 33 C0 48 6B C8 4F 0F B6 02 48 8D 54 24 30 48 03 C8 66 C7 44 24 30 01 00 48 8D 05 ?? ?? ?? ??", 35, 39, 0x51B5D0 },
			// Key guide glyphs (er/KeyGuide.h): the 0x88C-byte object 0xDECF20 allocates and builds with KeyGuideCtor, stored here
			{ A::KeyGuideState, "key guide state", Kind::kGlobalSite, 0x3D61F08, "48 8B C6 48 89 05 ?? ?? ?? ?? 48 83 3D", 6, 10, 0xDEDF65 },
			// fsrs global_hinstance: its setter `mov [rip+x],rcx; ret` (non-zero once system init is done)
			{ A::GlobalHinstance, "global_hinstance", Kind::kGlobalSite, 0x3D89708, "48 89 0D ?? ?? ?? ?? C3", 3, 7, 0xDEC6B0 },

			// ---- functions ----
			{ A::RegisterTask, "register_task", Kind::kFuncScan, 0xEB3E50, "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8B C7 8B D3 E8 ?? ?? ?? ??", 18, 22, 0,
				"48 89 5C 24 08 57 48 83 EC 40 48 8D 4C 24 20 49 8B D8 8B FA" },
			{ A::CastRay, "cs_phys_world_cast_ray", Kind::kFuncScan, 0xC71E00, "BA 58 00 00 02 E8 ?? ?? ?? ??", 6, 10, 0,
				"40 55 56 57 41 54 41 55 41 56" },
			{ A::ChrInsSetPosition, "ChrIns::SetPosition", Kind::kFuncJmp, 0x3F2690, "48 8B 49 58 E9 ?? ?? ?? ??", 5, 9, 0x3C7680,
				"48 89 5C 24 08 57 48 83 EC 20 48 8B 41 10 48 8B D9 48 8B 88 90 01 00 00 48 8B 49 68 E8" },
			{ A::ChrCtrlSetPosition, "ChrCtrl::SetPosition", Kind::kFunc, 0x3C7680,
				"48 89 5C 24 08 57 48 83 EC 20 48 8B 41 10 48 8B D9 48 8B 88 90 01 00 00 48 8B 49 68 E8 ?? ?? ?? ?? 48 8B 43 10" },
			{ A::ChrInsSetYaw, "ChrIns::SetYaw", Kind::kFuncJmp, 0x3F6180, "48 8B 49 58 E9 ?? ?? ?? ??", 5, 9, 0x3C7EE0,
				"48 8B 41 10 48 8B 88 90 01 00 00 48 8B 49 68 E9" },
			{ A::ChrInsKill, "ChrIns::Kill", Kind::kFunc, 0x3EE730,
				"48 89 5C 24 08 57 48 83 EC 30 F3 0F 10 05 ?? ?? ?? ?? 48 8B D9 48 8B 89 90 01 00 00 0F B6 FA" },
			{ A::ChangeHp, "CSChrDataModule::ChangeHp", Kind::kFunc, 0x438810,
				"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 0F 29 74 24 30 0F 28 F3 41 0F B6 F0 8B DA 48 8B F9 85 D2 7F 0E" },
			{ A::EzDrawLine, "CSEzDraw::DrawLine", Kind::kFunc, 0x2652640,
				"40 57 48 83 EC 40 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50 48 89 6C 24 58 48 89 74 24 60 49 8B F0 48 8B FA 48 8B D9 48 8D 69 28" },
			{ A::GetSingletonName, "get_singleton_name", Kind::kFunc, 0x1EC3250, "48 83 EC 28 E8 27 FF FF FF 48 85 C0 74 0E 48 8B 10 48 8B C8" },
			{ A::ApplyCameraMask, "CSCamera::ApplyCameraMask", Kind::kFunc, 0x3BB3E0, "8B 41 28 4C 8B C9 A8 20 74 ?? 48 8B 51 20 4C 8B 41 08" },
			// re-backlog E1. The body has an EnemyCtrl twin (0x3D1BE0), so the bytes are checked here and the
			// slot is checked against the live PlayerCtrl vtable before any swap (er/RootMotion.cpp).
			{ A::ModifyRootMotion, "PlayerCtrl::ModifyRootMotion", Kind::kFunc, 0x3DE610,
				"48 8B C4 48 89 58 18 48 89 78 20 55 48 8D 68 A1 48 81 EC B0 00 00 00 0F 29 70 E8 0F 29 78 D8 44 0F 29 40 C8 44 0F 29 48 B8 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 07 48 8B 41 10 48 8B FA" },
			{ A::RootMotionCallsite, "ChrCtrl root-motion vcall", Kind::kFunc, 0x3CC85C, "0F 29 07 48 8B 03 48 8B D7 48 8B CB FF 50 60 48 8B 43 10" },
			// re-backlog E2
			{ A::PlayerReceiveDamage, "CSPlayerDamageModule::ReceiveDamage", Kind::kFunc, 0x44D050,
				"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 0F B6 80 15 01 00 00" },

			// combat (fsrs cs_bullet_manager_spawn_bullet / chr_ins_apply_speffect, CT Bullet_code; PR GetTeamType)
			{ A::SpawnBullet, "CSBulletManager::SpawnBullet", Kind::kFunc, 0x3A2CB0, "40 53 55 56 57 48 81 EC 98 07 00 00 48 C7 44 24 50 FE FF FF FF" },
			{ A::ApplySpEffect, "ChrIns::ApplySpEffect", Kind::kFunc, 0x3E8DC0,
				"48 8B C4 48 89 58 08 48 89 70 10 57 48 81 EC C0 00 00 00 0F 28 05 ?? ?? ?? ?? 48 8B F1" },
			{ A::GetTeamType, "ChrIns::GetTeamType", Kind::kFunc, 0x3F1C90,
				"48 89 5C 24 10 57 48 83 EC 20 0F B6 41 6C 48 8B F9 88 02 48 8B DA 48 8B 89 78 01 00 00" },
			// fsrs chr_ins_remove_speffect: `mov rcx,[rcx+0x178]` (the SpEffect module) + jmp to the module's remove
			{ A::RemoveSpEffect, "ChrIns::RemoveSpEffect", Kind::kFuncJmp, 0x3EE2E0, "48 8B 89 78 01 00 00 E9 ?? ?? ?? ??", 8, 12, 0x4F7680,
				"48 83 EC 28 8B C2 48 8B 51 08 48 85 D2 74 0F" },

			// obj+0 = obj+4 = (byte) the clamped "Debug.KeyGuideDevice" config value (0xE48B20), the rest zeroed: the device bytes
			{ A::KeyGuideCtor, "key guide ctor", Kind::kFunc, 0x242790,
				"48 89 5C 24 08 57 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? 88 07 33 C0 66 89 47 01 88 47 03 E8 ?? ?? ?? ?? 88 47 04" },

			// Collision layers (docs/HOST.md "Collision filters"): CS::CSCollisionFilter (vtable 0x2B94DF0, its slot 8 = 0xC5F1F0)
			// builds a 128x128 layer matrix; cs_phys_world_cast_ray's filter is a query filter info whose bits 0-6 are its layer.
			// Here (inside 0xC5F1F0) row 0x16 is made: copy layer 3's row (0xC5F160), then drop every character layer, 2-0x2D
			// (0xC62530) -- what a character's capsule collides with, characters left out. [Puppet] ground_filter = 0x2000016 needs it.
			{ A::CollisionLayers, "collision layer 0x16 (CSCollisionFilter)", Kind::kFunc, 0xC6004E,
				"BA 16 00 00 00 48 8B CB 44 8D 42 ED E8 01 F1 FF FF BA 16 00 00 00 48 8B CB E8 C4 24 00 00" },
			// Snake's own ray query (game::CastRayKeep; docs/HOST.md "Collision filters"). cast_ray (0xC71E00) builds an
			// hknpRayCastQuery at rsp+0x40 -- codec [hknpWorld+0xB00] +0, filter [CSHavokMan+0xC0] +8, material 0xFFFF +0x10, filter
			// info +0x14, the ignored chr +0x18, 2 at +0x20, 0xFB at +0x24, the ray at +0x30 -- and a CSClosestHitCollectorForSnipe at
			// rbp-0x30 (ctor 0xC5A9F0, ignore at +0xA0 by 0xC5ABE0), casts with 0x187F7D0 and reads +0xC (hits) / +0x30 (position).
			// Its addHit 0xC5ABF0 -> 0xC5A8E0 drops a hit whose material (0xC75570: per-body table, else the mesh's per-key one) is
			// class 2 or 5 (id / 100); hknpClosestHitCollector's addHit 0x1886510 keeps the closest, no check.
			{ A::CastRayQuery, "cast_ray query build", Kind::kFunc, 0xC71E8D,
				"B8 FF FF 00 00 66 89 44 24 50 33 C0 89 44 24 54 48 89 44 24 58 48 89 44 24 48 48 89 44 24 40 C7 44 24 60 02 00 00 00 "
				"C6 44 24 64 FB" },
			{ A::CastRayCalls, "cast_ray query calls", Kind::kFunc, 0xC71F56,
				"48 8B 80 C0 00 00 00 48 89 44 24 48 48 8B 47 08 48 8B 88 00 0B 00 00 48 89 4C 24 40 44 89 7C 24 54 48 89 74 24 58 "
				"48 8D 4D D0 E8 6B 8A FE FF 90 48 8B D6 48 8D 4D D0 E8 4E 8C FE FF 4C 8D 45 D0 48 8D 54 24 40 48 8B 4F 08 E8 2C D8 C0 00 "
				"83 7D DC 00" },
			{ A::WorldCastRay, "hknpWorld cast ray", Kind::kFunc, 0x187F7D0,
				"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B E9 49 8B F8 8B 0D ?? ?? ?? ?? 48 8B F2 FF 15" },
			{ A::SnipeCollectorCtor, "snipe collector ctor", Kind::kFunc, 0xC5A9F0,
				"48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 E8 D5 FB FF FF 90 48 8D 05 ?? ?? ?? ?? 48 89 03" },
			{ A::SnipeCollectorDtor, "snipe collector dtor", Kind::kFunc, 0xC5AAB0,
				"48 89 4C 24 08 48 83 EC 38 48 C7 44 24 20 FE FF FF FF 48 8D 05 ?? ?? ?? ?? 48 89 01" },
			{ A::SnipeAddHit, "snipe collector addHit", Kind::kFunc, 0xC5ABF0,
				"48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 8B 42 48 48 8B FA 25 FF FF FF 00" },
			{ A::ClosestAddHit, "hknpClosestHitCollector addHit", Kind::kFunc, 0x1886510,
				"F3 0F 10 42 20 4C 8D 4A 20 0F 28 C8 4C 8B C1 0F 28 41 10 0F C6 C9 00 0F 2E C1 76 3B" },
			{ A::HitMaterialClass, "hit material id", Kind::kFunc, 0xC75570,
				"48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 89 54 24 10 57 48 83 EC 20 8B 01 40 32 ED 25 FF FF FF 00" },

			// ---- vtables (MSVC RTTI) ----
			{ A::VtChrIns, "vt CS::ChrIns", Kind::kVtable, 0x2A310C8, nullptr, 0, 0, 0, nullptr, ".?AVChrIns@CS@@" },
			{ A::VtPlayerIns, "vt CS::PlayerIns", Kind::kVtable, 0x2A7FBB0, nullptr, 0, 0, 0, nullptr, ".?AVPlayerIns@CS@@" },
			{ A::VtEnemyIns, "vt CS::EnemyIns", Kind::kVtable, 0x2A47090, nullptr, 0, 0, 0, nullptr, ".?AVEnemyIns@CS@@" },
			{ A::VtChrCtrl, "vt CS::ChrCtrl", Kind::kVtable, 0x2A2C6A0, nullptr, 0, 0, 0, nullptr, ".?AVChrCtrl@CS@@" },
			{ A::VtPlayerCtrl, "vt CS::PlayerCtrl", Kind::kVtable, 0x2A2EB10, nullptr, 0, 0, 0, nullptr, ".?AVPlayerCtrl@CS@@" },
			{ A::VtPlayerDamageModule, "vt CS::CSPlayerDamageModule", Kind::kVtable, 0x2A3A3E0, nullptr, 0, 0, 0, nullptr, ".?AVCSPlayerDamageModule@CS@@" },
			{ A::VtActionButtonManImp, "vt CS::CSActionButtonManImp", Kind::kVtable, 0x2B48140, nullptr, 0, 0, 0, nullptr, ".?AVCSActionButtonManImp@CS@@" },
			{ A::VtPhysics, "vt CS::CSChrPhysicsModule", Kind::kVtable, 0x2A3C890, nullptr, 0, 0, 0, nullptr, ".?AVCSChrPhysicsModule@CS@@" },
			{ A::VtData, "vt CS::CSChrDataModule", Kind::kVtable, 0x2A380B8, nullptr, 0, 0, 0, nullptr, ".?AVCSChrDataModule@CS@@" },
			{ A::VtChrCam, "vt CS::ChrCam", Kind::kVtable, 0x2A2AA08, nullptr, 0, 0, 0, nullptr, ".?AVChrCam@CS@@" },
			{ A::VtCSCam, "vt CS::CSCam", Kind::kVtable, 0x2AA0A18, nullptr, 0, 0, 0, nullptr, ".?AVCSCam@CS@@" },
			{ A::VtPersCam, "vt CS::CSPersCam", Kind::kVtable, 0x2AA0A40, nullptr, 0, 0, 0, nullptr, ".?AVCSPersCam@CS@@" },
			{ A::VtDebugCam, "vt CS::CSDebugCam", Kind::kVtable, 0x2B672C0, nullptr, 0, 0, 0, nullptr, ".?AVCSDebugCam@CS@@" },
			{ A::VtEzDraw, "vt CS::CSEzDraw", Kind::kVtable, 0x2B814C0, nullptr, 0, 0, 0, nullptr, ".?AVCSEzDraw@CS@@" },
			{ A::VtPhysWorld, "vt CS::CSPhysWorld", Kind::kVtable, 0x2B96978, nullptr, 0, 0, 0, nullptr, ".?AVCSPhysWorld@CS@@" },
			{ A::VtHavokManImp, "vt CS::CSHavokManImp", Kind::kVtable, 0x2B934E8, nullptr, 0, 0, 0, nullptr, ".?AVCSHavokManImp@CS@@" },
			{ A::VtRendManImp, "vt CS::RendManImp", Kind::kVtable, 0x2BA2238, nullptr, 0, 0, 0, nullptr, ".?AVRendManImp@CS@@" },
			{ A::VtCameraImp, "vt CS::CSCameraImp", Kind::kVtable, 0x2A2AFF0, nullptr, 0, 0, 0, nullptr, ".?AVCSCameraImp@CS@@" },
			{ A::VtMenuManImp, "vt CS::CSMenuManImp", Kind::kVtable, 0x2A9EED8, nullptr, 0, 0, 0, nullptr, ".?AVCSMenuManImp@CS@@" },
			{ A::VtWorldChrManImp, "vt CS::WorldChrManImp", Kind::kVtable, 0x2A4E980, nullptr, 0, 0, 0, nullptr, ".?AVWorldChrManImp@CS@@" },
			{ A::VtTaskImp, "vt CS::CSTaskImp", Kind::kVtable, 0x2C04B00, nullptr, 0, 0, 0, nullptr, ".?AVCSTaskImp@CS@@" },
			{ A::VtFlipperImp, "vt CS::CSFlipperImp", Kind::kVtable, 0x2BFF358, nullptr, 0, 0, 0, nullptr, ".?AVCSFlipperImp@CS@@" },
			{ A::VtChrSet, "vt CS::ChrSet", Kind::kVtable, 0x2A41348, nullptr, 0, 0, 0, nullptr, ".?AVChrSet@CS@@" },
			{ A::VtOpenFieldChrSet, "vt CS::OpenFieldChrSet", Kind::kVtable, 0x2A4B620, nullptr, 0, 0, 0, nullptr, ".?AVOpenFieldChrSet@CS@@" },
			{ A::VtSoloParamRepositoryImp, "vt CS::SoloParamRepositoryImp", Kind::kVtable, 0x2BB84C8, nullptr, 0, 0, 0, nullptr,
				".?AVSoloParamRepositoryImp@CS@@" },
			{ A::VtParamResCap, "vt CS::ParamResCap", Kind::kVtable, 0x2BB29F8, nullptr, 0, 0, 0, nullptr, ".?AVParamResCap@CS@@" },
			{ A::VtTeamTypeEnemy, "vt CSTeamTypeEnemy", Kind::kVtable, 0x2A4F888, nullptr, 0, 0, 0, nullptr, ".?AVCSTeamTypeEnemy@@" },
			{ A::VtTeamTypeRival, "vt CSTemaTypeRival", Kind::kVtable, 0x2A4F898, nullptr, 0, 0, 0, nullptr, ".?AVCSTemaTypeRival@@" },
			{ A::VtGameRend, "vt CS::GameRend", Kind::kVtable, 0x2A82338, nullptr, 0, 0, 0, nullptr, ".?AVGameRend@CS@@" },
			{ A::VtSnipeCollector, "vt CS::CSClosestHitCollectorForSnipe", Kind::kVtable, 0x2B94678, nullptr, 0, 0, 0, nullptr,
				".?AVCSClosestHitCollectorForSnipe@CS@@" },

			// ---- AI sounds (er/Noise.h): found in the Ghidra project of this build (2026-11-03): the AiSoundParam getter's
			// callers (0x32A8E0, 0x33F940 ...: the listeners), CSWorldAiManager+0x7628 and its add / builder; same RVAs in 2.7.2.0. ----
			{ A::WorldAiManager, "CSWorldAiManager", Kind::kGlobalSite, 0x3D66548, "48 8B 0D ?? ?? ?? ?? 8B 1B 8B 38 48 85 C9", 3, 7, 0x2BCD3C },
			{ A::AiSoundEmit, "CSWorldAiManager::AddAiSound", Kind::kFunc, 0x37A6C0,
				"40 53 48 81 EC A0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 90 00 00 00 48 83 B9 28 76 00 00 00 48 8B D9 48 8B 84 24 D0 00 00 00 "
				"74 20 48 8D 4C 24 30 48 89 44 24 20 E8 91 E1 FC FF 48 8B 8B 28 76 00 00 48 8D 54 24 30 E8 A0 FA FC FF" },
			{ A::AiSoundAdd, "AI sound system add", Kind::kFunc, 0x34A1B0,
				"48 8B C4 56 57 41 54 41 56 41 57 48 81 EC E0 00 00 00 48 C7 44 24 20 FE FF FF FF 48 89 58 18 48 89 68 20 48 8B 05 ?? ?? ?? ?? "
				"48 33 C4 48 89 84 24 D0 00 00 00 4C 8B F2 48 8B E9 4C 8D 79 20" },
			{ A::AiSoundRequest, "AI sound request builder", Kind::kFunc, 0x348760,
				"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 0F 28 02 48 8B F1 0F 11 01 48 83 C1 10 49 8B D0 0F 29 74 24 30 "
				"49 8B F9 49 8B D8 E8 0C 05 00 00 8B 43 18 F3 0F 10 35 ?? ?? ?? ?? 89 46 28" },
			{ A::VtTargetingSystem, "vt CS::CSTargetingSystem", Kind::kVtable, 0x2A1DAD8, nullptr, 0, 0, 0, nullptr, ".?AVCSTargetingSystem@CS@@" },
			{ A::VtAiTargetingOwner, "vt CS::CSAiTargetingSystemOwner", Kind::kVtable, 0x2A1E540, nullptr, 0, 0, 0, nullptr,
				".?AVCSAiTargetingSystemOwner@CS@@" },
			{ A::VtTargetSearchSys, "vt CS::CSTargetSearchSys", Kind::kVtable, 0x2A1E410, nullptr, 0, 0, 0, nullptr, ".?AVCSTargetSearchSys@CS@@" },
			// CSAiTargetingSystemOwner's slots 14 / 15 (+0x70 / +0x78: is_battle_state / is_not_in_any_search_state, fromsoftware-rs
			// CSTargetingSystemOwnerVmt), read by er/Noise.cpp ReadAi (review 0.7.0: it called them unverified). The same RVAs and code
			// in 2.7.1.0 (its Ghidra project: 0x142A1E5B0 -> 0x140345E50, 0x142A1E5B8 -> 0x140345E60) and 2.7.2.0 (its exe: the vtable at
			// 0x2A20550 holds 0x140345E50 / 0x140345E60); ReadAi also checks that the live vtable slot is this very function.
			{ A::AiOwnerIsBattle, "CSAiTargetingSystemOwner::IsBattleState", Kind::kFunc, 0x345E50, "48 8B 49 08 E9 87 D3 F7 FF" },
			{ A::AiOwnerNoSearch, "CSAiTargetingSystemOwner::IsNotInAnySearchState", Kind::kFunc, 0x345E60,
				"40 53 48 83 EC 20 48 8B D9 48 8B 49 08 E8 ?? ?? ?? ?? 84 C0 75 13 48 8B 4B 08 E8 ?? ?? ?? ?? 84 C0 75 06 48 83 C4 20 5B C3 B0 01 48 83 C4 20 5B C3" },
		};
		// clang-format on

		// Each table: one entry per A, in A's order (operator[] and Name() index by it), the same names.
		template <std::size_t N>
		consteval bool InOrder(const Entry (&a_table)[N])
		{
			for (std::size_t i = 0; i < N; ++i) {
				if (a_table[i].id != static_cast<A>(i)) {
					return false;
				}
			}
			return N == static_cast<std::size_t>(A::Count);
		}
		template <std::size_t N>
		consteval bool SameNames(const Entry (&a_a)[N], const Entry (&a_b)[N])
		{
			for (std::size_t i = 0; i < N; ++i) {
				if (std::string_view(a_a[i].name) != std::string_view(a_b[i].name)) {
					return false;
				}
			}
			return true;
		}
		static_assert(InOrder(kTable2720) && InOrder(kTable2710), "every A has exactly one table entry, in A's order");
		static_assert(SameNames(kTable2720, kTable2710), "both builds' tables name the entries alike");

		struct BuildTable
		{
			const ExeBuild* build;
			const Entry*    entries;
			Layout          layout;
		};
		// In kBuilds' order. Layout: WorldChrMan's chr_cam and by-distance list (VERIFIED in WorldChrManImp's ctor and every
		// [wcm+0x1E600..] access of each exe: 2.7.2.0 moved them by 0x10).
		const BuildTable kTables[] = {
			{ &kBuilds[0], kTable2720, { .wcmChrCam = 0x1ECF0, .wcmByDistance = 0x1F1E0 } },
			{ &kBuilds[1], kTable2710, { .wcmChrCam = 0x1ECE0, .wcmByDistance = 0x1F1D0 } },
		};
		static_assert(std::size(kTables) == std::size(kBuilds), "one table per supported build");

		const BuildTable* g_table = nullptr;

		const BuildTable* TableFor(std::uint32_t a_stamp, std::uint32_t a_size)
		{
			for (const auto& t : kTables) {
				if (t.build->timeDateStamp == a_stamp && t.build->sizeOfImage == a_size) {
					return &t;
				}
			}
			return nullptr;
		}

		Addresses g_addr;

		bool MatchAt(const std::uint8_t* a_at, const Pattern& a_pattern)
		{
			if (!g_addr.InImage(a_at) || !g_addr.InImage(a_at + a_pattern.bytes.size() - 1)) {
				return false;
			}
			for (std::size_t i = 0; i < a_pattern.bytes.size(); ++i) {
				if (a_pattern.bytes[i] >= 0 && a_at[i] != static_cast<std::uint8_t>(a_pattern.bytes[i])) {
					return false;
				}
			}
			return true;
		}

		std::uint8_t* Rel(std::uint8_t* a_at, int a_disp, int a_end)
		{
			std::int32_t rel;
			std::memcpy(&rel, a_at + a_disp, 4);
			return a_at + a_end + rel;
		}

		// RTTI check without scanning: vtable[-1] is the CompleteObjectLocator, whose type
		// descriptor (rva at +0xC) holds the mangled name at +0x10.
		bool VtableIs(std::uint8_t* a_vtable, const char* a_mangled)
		{
			const auto col = SafePtr(a_vtable - 8);
			if (!g_addr.InImage(col)) {
				return false;
			}
			const auto sig = SafeRead<std::uint32_t>(col);
			const auto td = SafeRead<std::uint32_t>(col + 0xC);
			if (!sig || *sig != 1 || !td || *td >= g_addr.size) {
				return false;
			}
			const auto* name = reinterpret_cast<const char*>(g_addr.base + *td + 0x10);
			const auto  len = std::strlen(a_mangled) + 1;
			return g_addr.InImage(name + len) && std::memcmp(name, a_mangled, len) == 0;
		}

		std::string Sha256File(const std::wstring& a_path)
		{
			std::ifstream in(a_path.c_str(), std::ios::binary);
			if (!in) {
				return {};
			}
			BCRYPT_ALG_HANDLE  alg = nullptr;
			BCRYPT_HASH_HANDLE hash = nullptr;
			if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
				return {};
			}
			std::string out;
			if (::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
				std::vector<char> buffer(1 << 20);
				while (in) {
					in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
					if (in.gcount() > 0) {
						::BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(in.gcount()), 0);
					}
				}
				UCHAR digest[32];
				if (::BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
					for (const auto b : digest) {
						out += std::format("{:02x}", b);
					}
				}
				::BCryptDestroyHash(hash);
			}
			::BCryptCloseAlgorithmProvider(alg, 0);
			return out;
		}
	}

	const Addresses& Addr() { return g_addr; }

	std::uint8_t* Addresses::Instance(A a_slot) const
	{
		return SafePtr(at[static_cast<std::size_t>(a_slot)]);
	}

	std::string_view Name(A a_id)
	{
		return (g_table ? g_table->entries : kTables[0].entries)[static_cast<std::size_t>(a_id)].name;
	}

	std::string SupportedBuilds()
	{
		std::string out;
		for (const auto& b : kBuilds) {
			out += std::format("{}{} (App {})", out.empty() ? "" : " / ", b.file, b.app);
		}
		return out;
	}

	bool CheckExeIdentity(bool a_sha256)
	{
		auto*       base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		wchar_t     path[MAX_PATH * 4]{};
		::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
		const auto stamp = nt->FileHeader.TimeDateStamp;
		const auto size = nt->OptionalHeader.SizeOfImage;
		const auto* table = nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 ? TableFor(stamp, size) : nullptr;
		if (!table) {
			std::string known;
			for (const auto& b : kBuilds) {
				known += std::format("{}{} (App {} WW: {:#x} / {:#x})", known.empty() ? "" : ", ", b.file, b.app, b.timeDateStamp, b.sizeOfImage);
			}
			Log::Error("exe mismatch: {} has TimeDateStamp {:#x} SizeOfImage {:#x}; PhantomRing supports eldenring.exe {}. Staying inert.",
				Narrow(path), stamp, size, known);
			return false;
		}
		const auto& build = *table->build;
		if (a_sha256) {
			const auto sha = Sha256File(path);
			if (sha != build.sha256) {
				Log::Error("exe mismatch: sha256 {} != {} (eldenring.exe {}). Staying inert.", sha.empty() ? "(unreadable)" : sha, build.sha256,
					build.file);
				return false;
			}
			Log::Info("exe sha256 verified");
		}
		g_table = table;
		g_addr.base = base;
		g_addr.size = size;
		g_addr.build = table->build;
		g_addr.layout = table->layout;
		Log::Info("exe identity OK: eldenring.exe {} (App {} WW) at {}", build.file, build.app, static_cast<void*>(base));
		return true;
	}

	bool UseImageForVerification(void* a_base)
	{
		auto*       base = static_cast<std::uint8_t*>(a_base);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto* table = TableFor(nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
		if (!table) {
			return false;
		}
		g_table = table;
		g_addr = {};
		g_addr.base = base;
		g_addr.size = nt->OptionalHeader.SizeOfImage;
		g_addr.build = table->build;
		g_addr.layout = table->layout;
		return true;
	}

	int ResolveAddresses()
	{
		if (!g_table) {
			Log::Error("address table: no supported exe identified");
			return static_cast<int>(A::Count);
		}
		const auto image = ModuleImage::Of(reinterpret_cast<HMODULE>(g_addr.base));
		int        failures = 0;
		for (const auto& e : std::span(g_table->entries, static_cast<std::size_t>(A::Count))) {
			auto&         slot = g_addr.at[static_cast<std::size_t>(e.id)];
			std::uint8_t* expect = g_addr.base + e.rva;
			std::uint8_t* got = nullptr;
			std::string   why;
			switch (e.kind) {
			case Kind::kGlobalScan:
			case Kind::kFuncScan:
				{
					const auto hits = Scan::All(image, Pattern::Parse(e.pattern), true, 4);
					if (hits.size() != 1) {
						why = std::format("{} AOB matches", hits.size());
						break;
					}
					got = Rel(hits[0], e.disp, e.end);
					if (e.kind == Kind::kFuncScan && e.pattern2 && !MatchAt(got, Pattern::Parse(e.pattern2))) {
						why = "prologue bytes differ";
						got = nullptr;
					}
					break;
				}
			case Kind::kGlobalSite:
				{
					std::uint8_t* site = g_addr.base + e.site;
					if (!MatchAt(site, Pattern::Parse(e.pattern))) {
						why = std::format("bytes at +{:#x} differ", e.site);
						break;
					}
					got = Rel(site, e.disp, e.end);
					break;
				}
			case Kind::kFunc:
				if (MatchAt(expect, Pattern::Parse(e.pattern))) {
					got = expect;
				} else {
					why = "bytes differ";
				}
				break;
			case Kind::kFuncJmp:
				if (!MatchAt(expect, Pattern::Parse(e.pattern))) {
					why = "bytes differ";
				} else if (Rel(expect, e.disp, e.end) != g_addr.base + e.site) {
					why = std::format("jmp lands on +{:#x}, expected +{:#x}", g_addr.Rva(Rel(expect, e.disp, e.end)), e.site);
				} else if (!MatchAt(g_addr.base + e.site, Pattern::Parse(e.pattern2))) {
					why = "jmp target bytes differ";
				} else {
					got = expect;
				}
				break;
			case Kind::kVtable:
				if (VtableIs(expect, e.rtti)) {
					got = expect;
				} else {
					why = std::format("RTTI is not {}", e.rtti);
				}
				break;
			}
			if (got && got != expect) {
				why = std::format("resolved to +{:#x}, expected +{:#x}", g_addr.Rva(got), e.rva);
				got = nullptr;
			}
			if (got) {
				slot = got;
				Log::Debug("addr {:<28} +{:#x} OK", e.name, e.rva);
			} else {
				++failures;
				Log::Error("addr {:<28} +{:#x} FAILED ({}); features that need it stay off", e.name, e.rva, why);
			}
		}
		Log::Info("address table (eldenring.exe {}): {}/{} verified", g_table->build->file, static_cast<int>(A::Count) - failures,
			static_cast<int>(A::Count));
		return failures;
	}
}
