# Elden Ring runtime RE reference for the PhantomRing host DLL

Target binary: `<steam-library>/common/ELDEN RING/Game/eldenring.exe`
Written 2026-10-11. Everything here comes from static analysis of the file on disk plus the local reference sources. The game was not launched.

**Tags.** Every claim carries one of these:

- **[V]**: VERIFIED against local bytes or local source. The evidence is named next to it: a unique AOB match, a disassembly address, a code-reference count, or a source file path.
- **[V-src]**: VERIFIED to be what a local reference source says (fromsoftware-rs or the TGA CT). It was not independently confirmed in game code.
- **[U]**: UNVERIFIED. From memory, from a community convention, or an inference that still needs a runtime test.

All addresses are **RVAs** (add the module base; the preferred base is `0x140000000`).

The tool is `PhantomRing/tools/er_aob_scan.py`. `python3 er_aob_scan.py verify` re-checks the 103 catalog entries below and exits 1 on any drift. All entries pass on this exe.

---

## 0. TL;DR for the DLL author

| Need | Use | Status |
|---|---|---|
| Per-frame callback, no code patch | `register_task` RVA `0xEB3E50`: `void(void* /*CSTaskImp, ignored*/, u32 group, FD4TaskBase* task)`. Task vtable `{get_runtime_class, dtor, execute(FD4TaskData*)}`. Groups: `FrameBegin=0`, `ChrIns_PrePhysics=78`, `CameraStep=106`, `DrawParamUpdate=107`, `Draw_Pre=144`, `FrameEnd=168` | [V] |
| Player ChrIns | `[[0x3D69FF8]+0x1E508]` (WorldChrMan.main_player) | [V] 423 code refs |
| Move player (real Havok move) | `ChrIns::SetPosition(ChrIns*, const hkVector4*)` at `0x3F2690`. It runs ChrCtrl `0x3C7680` → physics `0x45EEF0` (writes `+0x70`/`+0x80`, sets `+0x90`/`+0x91`) → `0x45C910`, which pushes the position into the char proxies at `+0x98`/`+0xA0` | [V] disasm |
| Turn player | `ChrIns::SetYaw(ChrIns*, const float* yaw_rad)` at `0x3F6180` | [V] disasm |
| HP / max HP | `[[ChrIns+0x190]+0x0]` is CSChrDataModule: `+0x138` hp, `+0x13C` max_hp. Setters at `0x438810` (ChangeHp) and `0x438870` (SetMaxHp) | [V] disasm |
| Dead flag | `ChrIns+0x1C5` bit 7. Set by `0x3EE730` ("Kill"), cleared at `0x3F58FF` | [V] disasm |
| Enemies | `WorldChrMan+0x1DED8`: `ChrSet* chr_sets[196]`. Each ChrSet has `+0x10` capacity (u32) and `+0x18` entries (stride 0x10, `{ChrIns*, u8 load_status, ...}`). Team type is `ChrIns+0x6C` | [V] code refs + getter `0x3F1C90` |
| Camera override | `CSCamera` at `0x3D669C8`; `+0x08` pers_cam_1 is the cam that `0x3BB3E0` fills from cams 2/3/4. CSCam layout: `+0x10` 4x4 world matrix (right, up, fwd, pos), `+0x50` fov, `+0x54` aspect, `+0x58` near, `+0x5C` far. Write it from a task after `CameraStep` | layout [V], timing [U] |
| Block player input | Clear `CSInGamePad.allow_polling` (FD4BasePad `+0x20`), found through FD4PadManager at `0x4861D30`. Or set ChrIns `+0x538` debug flags bit 5 (no move) and bit 4 (no actions) | offsets [V-src]/[V], behaviour [U] |
| Raycast | `cs_phys_world_cast_ray` at `0xC71E00`: `bool(CSPhysWorld*, u32 filter, const hkVector4* origin, const hkVector4* extent, hkVector4* hit, ChrIns* ignore)`. CSPhysWorld is `[[0x3D7A0D0]+0x98]`. `filter` = the query's collision filter info for `CS::CSCollisionFilter` (`[0x3D7A0D0]+0xC0`): bits 0-6 layer, 7-8 group, 25-29 sub-id, 30 own-character-only; a 128x128 layer matrix built at 0xC5F1F0. 0x58 (the game's aim ray) misses the character-only layers; 0x16 = layer 3's row minus characters (2026-10-14, HOST.md "Collision filters") | [V] RVA, [V] `+0x98` (246 refs), signature [V-src]; filter bits and matrix [V] static, layer meanings [U] |
| Damage to NPCs | Through the game's own pipeline: `cs_bullet_manager_spawn_bullet` (`0x3A2CB0`) or `chr_ins_apply_speffect` (`0x3E8DC0`). Fallback: ChangeHp `0x438810` plus Kill `0x3EE730` | RVAs [V], semantics [V-src]/[U] |
| D3D12 swapchain/queue | No stable static pointer was found. Hook `IDXGIFactory2::CreateSwapChainForHwnd`, `IDXGISwapChain::Present` and `ID3D12CommandQueue::ExecuteCommandLists` through the vtables of dummy objects (as hudhook does). That code lives in dxgi/d3d12 (vkd3d-proton), not in eldenring.exe | [U] approach; game call sites [V] |
| Arxan | Prefer data-only access, vtable/instance hooks and task registration. Inline code patches need me3 `disable_arxan = true` (dearxan 0.5.2 is bundled), or the fromsoftware-rs code-restoration disabler (181 sites in this exe) | see §7 |

---

## 1. Exact game version of the local exe

All facts in this table were computed by `er_aob_scan.py info` (a pure-Python PE parser) and cross-checked with `pefile` 2024.8.26.

| Property | Value | Status |
|---|---|---|
| Size | 87,042,128 bytes, mtime 2025-09-10 16:34 | [V] |
| sha256 | `1a3547101327f65d0c76da2f9190ac0aa66871ea42bae2aecc61e11a8b597891` | [V] |
| PE TimeDateStamp | `0x6A96B418` (2026-09-01 11:16:40 UTC) | [V] |
| ImageBase / SizeOfImage / Entry | `0x140000000` / `0x5E0DA00` / `0x24FE160` | [V] |
| VS_FIXEDFILEINFO File/ProductVersion | **2.7.1.0** | [V] |
| StringFileInfo | `040904b0`: CompanyName "FromSoftware, Inc.", ProductName "ELDEN RING™", FileVersion "2.7.1.0" | [V] |
| VarFileInfo Translation | `0x0409 0x04b0`, so `lang & 0x3FF = 0x09` (EN). This is **WW** in fromsoftware-rs `GameVersion::detect` | [V] (`crates/shared/src/game_version.rs`) |
| App version | **1.17.1**. TGA mapping: `1.(10*(major-1)+minor).release` gives `1.(10*1+7).1` | [V] (CT `.cea` `getGameVersion()`, line 182) |
| me3's own identification | `me3_mod_host: Attaching to ELDEN RING™ 1.17.1.0 Worldwide` (me3 0.13.0) | [V] (log `<old-project>/reports/v12-render-failure-host-log-snapshot.log`) |
| Same exe as the old MGS project | `scripts/build_summon_support.py` `EXPECTED_SHA` equals the sha above | [V] |

PE sections. The **second executable `.text`** is Arxan's code region. Obfuscated stubs also appear inside the main `.text`, for example the `lea rsp,[rsp-8]; mov [rsp],r13; lea r13,[..]; xchg [rsp],r13; ... jmp [rsp-8]` sequences at `0x3BB20C`. [V]

| name | RVA | VSize | exec |
|---|---|---|---|
| .text | 0x1000 | 0x29A4800 | yes |
| .interpr | 0x29A6000 | 0xC000 | yes |
| .rdata | 0x29B2000 | 0x1162800 | |
| .data | 0x3B15000 | 0xD51BC4 (raw 0x249E00) | |
| .pdata | 0x4867000 | 0x2B3200 | |
| .tls / .gfids / .rsrc / .reloc | 0x4B1B000 / 0x4B1C000 / 0x4B1E000 / 0x4B54000 | | |
| .idata | 0x4C0E000 | 0x4200 | |
| .text (2nd, Arxan) | 0x4C13000 | 0x11FAA00 | yes |

Imports relevant to us [V]:

- d3d12.dll: `D3D12SerializeRootSignature`, ord 101 and ord 102. Which ordinal is `D3D12CreateDevice` is [U]; the call through ord 101's thunk is at `0x1EA2831`.
- dxgi.dll: `CreateDXGIFactory1`, `CreateDXGIFactory`.
- DINPUT8.dll: `DirectInput8Create`.
- XINPUT1_4.dll: ord 2 and ord 3 (= XInputGetState / XInputSetState [U]).
- USER32: GetKeyState, GetKeyboardState, GetCursorPos, and others.

### 1.1 Version coverage of the reference sources

**fromsoftware-rs** (`<fromsoftware-rs>`, local copy, not a git checkout):

- `crates/eldenring/src/rva.rs` maps `(EN,"2.7.0.0")→Ww2700`, `(EN,"2.7.1.0")→Ww2710` and `(JP,"2.7.0.1")→Jp2701`. **Ww2710** is `rva_ww::RVAS` with 5 overrides: `register_task 0xEB3E50`, `cs_phys_world_cast_ray 0xC71E00`, `cs_ez_draw_draw_line 0x2652640`, `..._triangle 0x2652390`, `..._wedge 0x2651F40`. Its test is named `routes_verified_1171_without_changing_1170`, so this mapping was a local addition for 1.17.1. [V-src]
- I re-ran **every** `mapper-profile.toml` pattern and VMT entry against this exe:
  - 33 pattern captures: 29 are unique and equal Ww2710. 4 have several matches, but the first match equals the expected value: `fd4param_repository_vmt` (6), `cs_action_button_man_execute_action_button` (2), `crypto_spi_registry` (2), `obtain_dl_user_input_manager` (17).
  - All **74/74 RTTI VMTs** resolve to exactly the Ww2710 values, including the 6 templated `DLUID::*Device<DLKR::DLMultiThreadingPolicy>`. **[V]** (script: scratchpad `check_fsrs.py`, logic reproduced in `er_aob_scan.py verify`.)
- Conclusion: the local fromsoftware-rs **Ww2710 RvaBundle is fully valid for this exe**. Its struct layouts are what downstream code should mirror.

**TGA Cheat Table** (`<TGA-CT>`):

- `BUILD_ENV`: `TABLE_VERSION=1.19.1`, `GAME_VERSION=1.17.1`. The CHANGELOG `[v1.19.1] - 2026-09-08` says "Supported version to ELDEN RING App Ver. 1.17.1". Note that `VERSION` contains `1.140`, which is stale. [V-src]
- Its base AOBs live in `tga.baseData`, `.cea` lines 575-594 (most are commented out because the table now uses the FD4 singleton finder `table_files/include/tga/fd4_singleton.h`). Results on this exe:
  - **17 of 18 are unique and resolve to the expected singleton**.
  - `CSSessionManager` (`48 8B 05 ?? ?? ?? ?? 48 89 9C 24 ?? ?? ?? ?? 48 89 B4 24`) has **0 matches** (stale). A replacement is in §2. [V]

---

## 2. Address catalog (all [V] unless noted; "matches" = number of AOB matches in executable sections)

Run `python3 PhantomRing/tools/er_aob_scan.py verify` (or `--json`) to reproduce everything. Pattern syntax follows CE/IDA (`??`) plus the pelite subset (`$ { ' }` = follow rel32 and capture).

### 2.1 Static singleton pointers (`*(void**)(base+RVA)` = instance; it can be null before init or between loads)

| Singleton | Slot RVA | AOB (resolve) | matches | Source |
|---|---|---|---|---|
| WorldChrMan | `0x3D69FF8` | `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88` (rip @+3, end +7) | 1 | CT |
| GameDataMan | `0x3D61F98` | `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3` | 1 | CT; fsrs `48 89 05 ${ ' } 48 83 c4 38 e9 ${}` also 1 |
| GameMan | `0x3D6D988` | `48 8B 05 ?? ?? ?? ?? 80 B8 ?? ?? ?? ?? 0D 0F 94 C0 C3` | 1 | CT |
| FieldArea | `0x3D6D248` | `48 8B 3D ???????? 49 8B D8 48 8B F2 4C 8B F1 48 85 FF` | 1 | CT; fsrs `48 8b 05 ${ ' } 48 85 c0 48 0f 44 c6` also 1 |
| SoloParamRepository | `0x3D85F58` | `48 89 5C 24 48 8B FA 48 8B D9 C7 44 24 20 00 00 00 00 48` (insn at +18; rip @+21, end +25) | 1 | CT |
| CSMenuMan | `0x3D6F820` | `48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24` | 1 | CT |
| CSFlipper | `0x458DB58` | `48 8B 0D ?? ?? ?? ?? 80 BB ?? ?? ?? ?? 00 0F 84` | 1 | CT |
| CSRegulationManager | `0x3D8ACD8` | `48 8B 0D ?? ?? ?? ?? 48 85 C9 74 0B 4C 8B C0 48 8B D7` | 1 | CT |
| DmgMan ("DamageCtrl") | `0x3D6A3E8` | `48 8B 05 ?? ?? ?? ?? 49 8B D9 49 8B F8 48 8B F2 48 85 C0 75 2E` | 1 | CT |
| CSWindow | `0x458F910` | `48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 48 83 C1 ?? 48 8D 45` | 1 | CT |
| WorldChrManDbg | `0x3D6A208` | `48 8B 0D ?? ?? ?? ?? 89 5C 24 20 48 85 C9 74 12 B8 ?? ?? ?? ?? 8B D8` | 1 | CT |
| CSLuaEventMan | `0x3D6BEB8` | `48 83 3D ?? ?? ?? ?? 00 48 8B F9 0F 84 ?? ?? ?? ?? 48` (rip @+3, end +8) | 1 | CT |
| CSEventFlagMan | `0x3D6C4B8` | `48 8B 3D ???????? 48 85 FF 74 ?? 48 8B 49` | 1 | CT |
| CSNetMan | `0x3D5EEC0` | `48 8B 0D ?? ?? ?? ?? 48 85 C9 74 5E 48 8B 89 ?? ?? ?? ?? B2 01` | 1 | CT |
| CSCamera | `0x3D669C8` | `48 8b 0d ?? ?? ?? ?? 48 8d 97 e0 00 00 00` | 1 | PR (generated; this exe) |
| CSTask (CSTaskImp) | `0x458FE88` | `48 8b 0d ?? ?? ?? ?? 4c 8b c3 ba 9c 00 00 00` | 1 | PR (generated) |
| CSTaskGroup | `0x458FF08` | `48 8b 0d ?? ?? ?? ?? 49 8b 1f ba 32 00 00 00` | 1 | PR (generated) |
| FD4PadManager | `0x4861D30` | `48 8b 0d ?? ?? ?? ?? e9 ?? ?? ?? ?? b0 01 c3 90 8b d1` | 1 | PR (generated; fragile, prefer the singleton finder) |
| CSHavokMan | `0x3D7A0D0` | `48 8b 0d ?? ?? ?? ?? 48 8b 9d 80 00 00 00` | 1 | PR (generated) |
| CSGraphics | `0x3D75CB8` | `48 8b 05 ?? ?? ?? ?? 48 8b 48 58 48 85 c9` | 1 | PR (generated) |
| RendMan | `0x3D7F130` | `48 8b 05 ?? ?? ?? ?? 48 8b 48 10 48 85 c9 75 0d 48 8b cb` | 1 | PR (generated) |
| CSFade | `0x3D763B0` | `48 8b 0d ?? ?? ?? ?? 0f 10 00 0f 11 44 24 40` | 1 | PR (generated) |
| CSNowLoadingHelper | `0x3D64F28` | `48 8b 0d ?? ?? ?? ?? b8 e8 03 00 00` | 1 | PR (generated) |
| CSFeMan | `0x3D6F8F0` | `48 8b 0d ?? ?? ?? ?? 44 0f b6 cd 44 8b c6` | 1 | PR (generated) |
| LockTgtMan | `0x3D6E278` | `48 8b 05 ?? ?? ?? ?? 0f b6 5b 0c 48 85 c0` | 1 | PR (generated) |
| CSBulletManager | `0x3D667A8` | `48 8b 0d ?? ?? ?? ?? 8b 3b 48 85 c9` | 1 | PR (generated) |
| CSSessionManager | `0x3D7E540` | `48 8b 0d ?? ?? ?? ?? 83 79 0c 03 75 1f` | 1 | PR (generated; CT AOB is stale) |
| global_hinstance | `0x3D89708` | `48 8b ce 48 8b f8 e8 $ { 48 89 0d $ { ' } c3 }` | 1 | fsrs (non-zero once system init is done) |
| WorldChrMan dbg flags | `0x3D6A210` | `80 3d $ { ? ' } 00 0f 85 [4] 32 c0` | 1 | fsrs |

The "PR (generated)" patterns came from `er_aob_scan.py mkpattern <slot>`. They are unique in this exe but short, so they may not survive a patch. **For robustness, resolve singletons by name** with the FD4 singleton finder (§2.2), as TGA and fromsoftware-rs do.

### 2.2 FD4Singleton finder (static port, names recovered without running the game) [V]

Algorithm in `er_aob_scan.py fd4_singletons()`: the technique and null-check byte pattern of TGA's `fd4_singleton.h`, re-implemented statically:

1. Scan executable sections for the null-check emitted by every `FD4Singleton<T>::instance()`:

   `48 8b ? <slot rel32> 48 85 ? 75 2e 48 8d 0d <tag rel32> e8 <get_singleton_name> 4c 8b c8 4c 8d 05 <fmt> ba ? ? 00 00 48 8d 0d <path> e8 <log>`

   The path string must end in `FD4Singleton.h`. This gives **6663 sites and 216 distinct slots**.
2. Name each `tag`:
   - RTTI class `DLRF::DLRuntimeClassImpl<FD4::FD4Singleton<NS::Name,NS::NameImp>,0>` has vftable slot 3 = `lea rax,[tag]; ret`. Example: WorldChrMan's tag `0x3D5EE4C` is returned by `0x509340`, which is vfunc 3 of the vtable at `0x2A4E9B0`.
   - Fallback for FD4 classes without RTTI (FD4PadManager and others): the DLRF static initialiser `mov rax,[rc]; test; jnz; call ctor; lea rcx,["Name"]; mov [rax+0x38],rcx`. Inside `ctor`, the code that writes the runtime-class vtable gives the tag.
   - Result: **201/216 named**. The rest are minor, for example `0x3D6A208` = WorldChrManDbg (named via CT).
3. The live game's `get_singleton_name(tag)` (`0x1EC3250`) instead looks the tag up in a sorted runtime table (`0x1EC3180`). That is why TGA calls it at runtime.

Selected results (slot RVA → name, null-check site count):

- `0x3D69FF8` CS::WorldChrMan (869 sites)
- `0x3D669C8` CS::CSCamera (22)
- `0x3D6F820` CS::CSMenuMan (247)
- `0x458DB58` CS::CSFlipper (46)
- `0x458FE88` CS::CSTask (22)
- `0x458FF08` CS::CSTaskGroup (4)
- `0x3D8ACD8` CS::CSRegulationManager (5)
- `0x3D85F58` CS::SoloParamRepository (211)
- `0x3D7A0D0` CS::CSHavokMan (179)
- `0x3D75CB8` CS::CSGraphics (52)
- `0x3D7F130` CS::RendMan (226)
- `0x3D763B0` CS::CSFade (67)
- `0x3D64F28` CS::CSNowLoadingHelper (7)
- `0x3D6F8F0` CS::CSFeMan (38)
- `0x458F910` CS::CSWindow (45)
- `0x3D6A3E8` CS::DmgMan (36)
- `0x3D6A348` CS::DmgHitRecordMan (11)
- `0x3D667A8` CS::CSBulletManager (23)
- `0x3D6E278` CS::LockTgtMan (10)
- `0x3D7E540` CS::CSSessionManager (476)
- `0x4861D30` FD4PadManager (66)
- `0x4861280` FD4TaskManager (11)
- `0x4861FB0` FD4ParamRepository (2)
- `0x3D84D78` CS::ParamRepository (31)

The full list is printed by `er_aob_scan.py singletons`.

**Runtime recommendation for the C++ DLL:** run the same scan in-process. Name tags by reading the RTTI COL at `vftable[-1]` of every `DLRuntimeClassImpl<FD4Singleton<...>>` vtable. That needs no calls into game code and is patch-robust. Cache the slot addresses.

### 2.3 Functions

| Function | RVA | AOB (resolve) | matches | Notes / source |
|---|---|---|---|---|
| register_task | `0xEB3E50` | `e8 ? ? ? ? 48 8b 0d ? ? ? ? 4c 8b c7 8b d3 e8 $ { ' }` (cap 1) | 1 | fsrs. Disasm: `rcx` unused (overwritten), `edx`=group, `r8`=task; reads CSTaskGroup `0x458FF08` [V] |
| cs_phys_world_cast_ray | `0xC71E00` | `ba 58 00 00 02 e8 $ { ' }` | 1 | fsrs `havok_man.rs` signature [V-src] |
| cs_bullet_manager_spawn_bullet | `0x3A2CB0` | `40 53 55 56 57 48 81 ec 98 07 00 00 48 c7 44 24 50 fe ff ff ff` | 1 | fsrs and CT Bullet_code; `(CSBulletManager*, i32* out, BulletSpawnData*, i32*)` [V-src] |
| chr_ins_apply_speffect | `0x3E8DC0` | `48 8b c8 45 33 c0 ba 7f 14 00 00 e8 $ { ' }` | 1 | `(ChrIns*, i32 id, bool dont_sync)` [V-src] |
| chr_ins_remove_speffect | `0x3EE2E0` | `ba 1c 00 00 00 48 8b ce 84 c0 74 ? e8 $ { ' } eb ?` | 1 | `(ChrIns*, i32 id)` [V-src] |
| cs_menu_man_imp_display_status_message | `0x7671F0` | `ba d0 07 00 00 48 83 c4 28 e9 $ { ' }` | 1 | `bool(CSMenuManImp*, i32)` (5 = YOU DIED) [V-src] |
| cs_ez_draw_draw_line | `0x2652640` | fsrs pattern | 1 | debug drawing |
| **ChrIns::SetPosition** | `0x3F2690` | `48 8b 49 58 e9 $ { 48 89 5c 24 08 57 48 83 ec 20 48 8b 41 10 48 8b d9 48 8b 88 90 01 00 00 48 8b 49 68 e8 }` | 1 | PR: `mov rcx,[rcx+0x58]; jmp 0x3C7680` [V] |
| ChrCtrl::SetPosition | `0x3C7680` | `48 89 5c 24 08 57 48 83 ec 20 48 8b 41 10 48 8b d9 48 8b 88 90 01 00 00 48 8b 49 68 e8 ?? ?? ?? ?? 48 8b 43 10` | 1 | PR: physics SetPosition; if `physics+0x90`, calls `0x3CA960(ChrCtrl)` and clears `+0x90` [V] |
| CSChrPhysicsModule::SetPosition | `0x45EEF0` | `0f 28 02 0f 11 41 70 0f 28 02 0f 11 81 80 00 00 00 66 c7 81 90 00 00 00 01 01 c6 81 de 01 00 00 01 e9` | 1 | PR: leaf (no .pdata). A twin without the tail-jump is at `0x45FE70` [V] |
| CSChrPhysicsModule::SyncProxyPosition | `0x45C910` | `40 53 48 83 ec 40 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 30 80 b9 91 00 00 00 00 48 8b d9` | 1 | PR: if `[+0x91]`, calls `0xC5D860(proxy,&pos)` for `[+0x98]` and `[+0xA0]`, then `[+0x91]=0` [V] |
| CSChrPhysicsModule::SetPositionAndYaw | `0x45FC80` | `48 89 5c 24 08 57 48 83 ec 30 0f 28 02 48 8b da 0f 11 41 70 48 8b f9 0f 28 02` | 1 | PR: `(phys*, const hkVector4*, const float* yaw)`; also writes `+0x150`, `+0x1B0=0`; no immediate proxy sync [V] |
| **ChrIns::SetYaw** | `0x3F6180` | `48 8b 49 58 e9 $ { 48 8b 41 10 48 8b 88 90 01 00 00 48 8b 49 68 e9 $ { 40 53 48 83 ec 30 4c 8b c2 48 8b d9 48 8d 54 24 20 e8 } }` | 1 | PR: → `0x3C7EE0` → physics `0x45FD00`. `0x461F60` builds the quat from `yaw*0.5` (constant at `0x32A1920` = 0.5) [V] |
| CSChrPhysicsModule::SetOrientation | `0x45FEE0` | `40 53 48 83 ec 30 0f 28 02 48 8b d9 48 8d 54 24 20 0f 29 44 24 20 66 0f 7f 41 50 e8` | 1 | PR: `+0x50=q; +0x60=0x463FA0(this,q)`. The ChrCtrl wrapper is `0x3C8340` [V] |
| ChrCtrl::RequestProxyPosition | `0x3C9270` | `83 89 fc 00 00 00 01 0f 28 02 0f 11 81 00 01 00 00 c3` | 1 | PR: `ChrCtrl+0xFC |= 1; ChrCtrl+0x100 = pos` [V] |
| ChrIns::GetTeamType | `0x3F1C90` | `48 89 5c 24 10 57 48 83 ec 20 0f b6 41 6c 48 8b f9 88 02 48 8b da 48 8b 89 78 01 00 00` | 1 | PR: reads `+0x6C`; SpEffect state `0x84` can override [V] |
| ChrIns::Kill (my name) | `0x3EE730` | `48 89 5c 24 08 57 48 83 ec 30 f3 0f 10 05 ?? ?? ?? ?? 48 8b d9 48 8b 89 90 01 00 00 0f b6 fa` | 1 | PR: `(ChrIns*, bool)`. Calls `ChangeHp(data,0,...)`; unless `[[ChrIns+0x58]+0xC8]+0x24` bit0, sets `+0x1C5|=0x80`, `+0x210=dl`, calls `0x3F56E0`, `0x3F84C0`. 9 callers [V] |
| CSChrDataModule::ChangeHp | `0x438810` | `48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 40 0f 29 74 24 30 0f 28 f3 41 0f b6 f0 8b da 48 8b f9 85 d2 7f 0e` | 1 | PR: `(data*, i32 hp, bool, f32, ..., [rsp+0x28] bool)`. If hp<=0 and `0x437CA0` says no-death, hp becomes 1. Tail-jumps to `0x437450`, which writes `+0x138` clamped by `+0x13C` [V] |
| CSChrDataModule::SetMaxHp | `0x438870` | `89 54 24 10 c7 44 24 18 ff ff 07 00 c7 44 24 08 01 00 00 00` | 1 | PR: clamp [1, 0x7FFFF], writes `+0x13C`, re-clamps `+0x138` [V] |
| CSCamera::ApplyCameraMask | `0x3BB3E0` | `8b 41 28 4c 8b c9 a8 20 74 ?? 48 8b 51 20 4c 8b 41 08` | 1 | PR, see §5.9. Single caller `0x3BC686`, in `0x3BC5F0(this, FD4Time*)`, an update-task executor stored by `lea` at `0x9379C` [V] |
| hkbCharacter::FireEvent (CT "PlayAnimation") | `0xC15AB0` | `74 ?? 48 85 d2 74 ?? 48 8d 4c 24 50` (match − 0xD) | 1 | CT `PlayAnimation_code.cea`: `(hkbCharacter*, const wchar_t* event)`; returns `0xFFFFFFFF` on failure [V-src]; start confirmed by .pdata [V] |
| D3D12CreateDevice call site | `0x1EA2822` | `4c 8b cb 4c 8d 05 ?? ?? ?? ?? 8b d0 48 8b ce e8 ?? ?? ?? ?? 85 c0 74 0c 8b c8 e8` | 1 | Inside `0x1EA2730(ID3D12Device** out, IDXGIAdapter*, cfg*)`, called from `0x1E9FF04` in `0x1E9FD30`, called from `0x1E8919D` [V]. Informational |
| CreateDXGIFactory1 calls | `0x1EAE5E6`, `0x1EAE9CF` | via thunk `0x2624E08` | | [V] |
| CT free-camera patch targets | `0xE54580` (called at `0xE2C3D9` and `0xE2C619`), `0xE45120` | CT patterns all unique | 1 each | CT patches these to `mov al,1; ret`. These are **code patches** (Arxan risk) [V] |

### 2.4 RTTI vftables (resolved by class name; one vtable at COL offset 0 each) [V]

| Class | vftable RVA | Class | vftable RVA |
|---|---|---|---|
| CS::ChrIns | `0x2A310C8` (203 vfuncs) | CS::PlayerIns | `0x2A7FBB0` (204) |
| CS::EnemyIns | `0x2A47090` | CS::ChrCtrl | `0x2A2C6A0` |
| CS::CSChrPhysicsModule | `0x2A3C890` | CS::CSChrDataModule | `0x2A380B8` |
| CS::CSChrDamageModule | `0x2A39A90` (23) | CS::CSPlayerDamageModule | `0x2A3A3E0` (23) |
| CS::CSEnemyDamageModule | `0x2A3A0D8` (23) | CS::ChrCam | `0x2A2AA08` |
| CS::CSCam | `0x2AA0A18` | CS::CSPersCam | `0x2AA0A40` |
| CS::CSDebugCam | `0x2B672C0` | CS::CSPad | `0x2BE3A70` |
| CS::CSInGamePad | `0x2A2AA40` | CS::CSInGamePad_UserInput1 | `0x2BE35E8` |
| CS::CSMenuViewerPad | `0x2BE4778` | CS::CSDebugCamPad | `0x2BE2EF0` |
| CS::NearEnemyFinder | `0x2A282D0` (vfunc 2 `run` = `0x3E5900`) | CS::ChrSet | `0x2A41348` |
| CS::OpenFieldChrSet | `0x2A4B620` | CS::CSPhysWorld | `0x2B96978` |
| CS::CSHavokManImp | `0x2B934E8` | CS::CSGraphicsImp | `0x2B68558` |
| CS::RendManImp | `0x2BA2238` | CS::CSTaskImp | `0x2C04B00` |
| CS::CSEzTask | `0x2C048A0` | GXBS::GXCommandQueue | `0x2F274C8` (1 vfunc) |
| GXBS::GXSwapChainOperatorForMultiWindow | `0x2F07CB8` (1 vfunc) | DLRF RuntimeClassImpl<FD4Singleton<WorldChrMan>> | `0x2A4E9B0` |

The exe has **10,198 RTTI vftables**; `er_aob_scan.py rtti --grep X` lists them. Templates are demangled to undname NAME_ONLY spelling, for example `DLUID::PadDevice<DLKR::DLMultiThreadingPolicy>`.

---

## 3. Struct layouts and recipes

Each offset row says how it was verified:

- "code": a load or store with that displacement seen in game code. The counts come from `fieldref` or byte-pattern scans.
- "fsrs": a fromsoftware-rs file. I computed the repr(C) offsets by hand; they are consistent with the `unkXXXX` field names and the crate's size tests.
- "CT": a TGA CT entry path under `CheatTable/CheatEntries/Open - The Grand Archives - Elden Ring/`.

### 3.1 WorldChrMan (`*0x3D69FF8`), fsrs `crates/eldenring/src/cs/world_chr_man.rs`

| Off | Field | Verification |
|---|---|---|
| +0x10 | `WorldAreaChr[28]` (0x28 each) | fsrs |
| +0x470 | `WorldBlockChr[192]` (0x160 each; embeds a ChrSet at +0x78) | fsrs (offsets consistent with `unk10d9c`) |
| +0x10D98 | world_area_chr_list_count | fsrs |
| **+0x10EE0** | `player_chr_set`: ChrSet<PlayerIns>, size 0x58. `+0x18` entries → CT `WorldChrMan+10EF8 → +0*10` = first player | code: 13 `mov` + `lea` refs [V]; CT `Hero/*` |
| +0x10F38 | ghost_chr_set | fsrs |
| **+0x10F90** | summon_buddy_chr_set (Spirit Ashes and Torrent) | code: `lea rcx,[rbx+0x10F90]` at `0x504D01` [V] |
| +0x10FE8 | debug_chr_set | fsrs |
| **+0x11040** | open_field_chr_set (OpenFieldChrSet, 0xBC18) | code `0x504D1B`, `0x506D01` [V] |
| **+0x1CC58** | chr_set_holder_count (u32) | code: 60 refs [V] |
| +0x1CC60 | `ChrSetHolder[196]` `{ChrSet*, u32 index, pad, WorldBlockChr*}` (0x18) | code: 34 refs [V] |
| **+0x1DED8** | `ChrSet* chr_sets[196]` (indexed by FieldInsSelector.container) | code: `mov rcx,[rsi+rdi*8+0x1DED8]` at `0x508A07`, 8 refs [V] |
| +0x1E500 | player_grid_area | code: 4 refs [V] |
| **+0x1E508** | `PlayerIns* main_player` | code: **423 refs** (for example `0x1BAC93`) [V]; prior MGS project probes |
| +0x1E510 | sinner_hunter (PlayerIns*) | code: 3 refs [V] |
| +0x1E538 | summon_buddy_manager (SummonBuddyManager*) | fsrs; prior MGS probes used +0x1E538 as "buddy" |
| **+0x1ECE0** | `ChrCam* chr_cam` | code: 7 refs (for example `0x3D2568`) [V] |
| +0x1F1D0 | `DLVector<ChrInsDistanceEntry>` chr_inses_by_distance `{alloc, first, last, end}`; entry `{ChrIns*, f32 dist, u32}` | code: `+0x1F1D8`/`+0x1F1E0` at `0x50FAA5`/`0x50FAAC` [V] |
| +0x1F200 | `DLVector<ChrIns*>` chr_inses_by_update_priority | code `+0x1F208`/`+0x1F210` [V] |

**ChrSet** (vtable `0x2A41348`), fsrs:

- `+0x00` vftable (vfuncs: `get_capacity`, `safe_get_chr_ins_by_index`, `get_chr_ins_by_index`, `get_chr_ins_by_handle(FieldInsHandle)`, ...)
- `+0x08` i32 index
- `+0x10` u32 capacity
- `+0x18` `ChrSetEntry* entries`
- `+0x28` DLMap entity_id→entry
- `+0x40` DLMap group
- size 0x58

**ChrSetEntry** (0x10): `+0 ChrIns*`, `+8 u8 load_status` (0 Unloaded, 1 Initializing, **2 Active**, 3 NetInit, 4 ReadyForActivation, 5 Unloading), `+9 u8 update_type` (0 Local, 4 Remote), `+0xA u8 flags`. The CT uses entry stride `0*10`, consistent. [V-src]

**FieldInsHandle** (8 bytes, in `field_ins.rs`): `u32 selector` (bits 0-19 index, 20-27 container = chr_sets index, 28-31 type: 1 = Chr) and `i32 block_id`. [V-src]

### 3.2 ChrIns (vftable `0x2A310C8`; size 0x580), fsrs `cs/chr_ins.rs`

| Off | Field | Verification |
|---|---|---|
| +0x08 | FieldInsHandle field_ins_handle | fsrs |
| +0x10 | ChrSetEntry* chr_set_entry (`+8` load status) | fsrs |
| +0x30 / +0x3C | block_id / block_origin | fsrs |
| +0x50 | CSChrModelIns* chr_model_ins | fsrs |
| **+0x58** | ChrCtrl* chr_ctrl | code `0x3F2690`, `0x3F0E20`, `0x3EE76A`, `0x3F6180` [V] |
| +0x60 / +0x64 | npc_param_id / npc_id | fsrs |
| +0x68 | i32 chr_type (0 Local, 1 WhitePhantom, 5 Npc, ...) | fsrs |
| **+0x6C** | u8 team_type | code: `ChrIns::GetTeamType` `0x3F1C9A` [V] |
| +0x80 | F32Vector4 chunk_position (global map chunk coords) | fsrs |
| +0x90 | initial_position (Havok) | fsrs |
| **+0x178** | SpecialEffect* special_effect | code `0x3F1CA6` (state-info query 0x84) [V] |
| +0x180 | FieldInsHandle last_hit_by | fsrs |
| +0x188 | u32 character_id | fsrs |
| **+0x190** | ChrInsModuleContainer* modules | code: hundreds of refs (`+0x190`→`+0x68`: 194 sites) [V]; CT |
| +0x1C4 | flags: bit5 no_gravity, bit4 is_onscreen | fsrs; code: `test [r+0x1C4],0x20` 17 sites [V] |
| **+0x1C5** | flags: **bit7 death_flag**, bit3 enable_render, bit4 is_invincible | code: set `0x3EE77C`, clear `0x3F58FF`; bit3 set `0x4D7FDD` / clear `0x4D817B` around summon warps [V]; meaning of bit3 [V-src] |
| +0x1C8 | flags: bit4 is_active | fsrs |
| +0x1E8 | u32 event_entity_id | fsrs |
| +0x210 | u8 (set by Kill from its bool arg) | code `0x3EE786` [V] |
| +0x238 / +0x240 | opacity_keyframes_multiplier / tint_alpha_multiplier | fsrs; code write to `+0x240` at `0x4D8189` [V] |
| +0x24C / +0x250 | base_transparency / base_transparency_modifier (−1 fades out, +1 fades in) | fsrs; code writes: `+0x24C` ×11, `+0x250` ×20 [V] |
| +0x3FC | f32 distance_to_player_sqr | fsrs |
| **+0x538** | u32 debug_flags: bit3 no hit, **bit4 no attack/actions**, **bit5 no move**, bit8 no update, bit10 force_unloaded, bit11 force_loaded, bit12 character_disabled, bit16 infinite consumables | CT `Hero/Character Flags/{No Hit, No Attack, No Move, No Update}.xml` (`+538` bits 3/4/5, `+539` bit0); fsrs `ChrDebugFlags`; code: `test [r+0x538],0x20` ×3, `,0x10` ×4 [V] |

**PlayerIns** (`0x2A7FBB0`, from ChrIns `+0x580`):

| Off | Field | Verification |
|---|---|---|
| +0x580 | PlayerGameData* | code: vfunc 45 = `mov rax,[rcx+0x580]` [V] |
| +0x5D8 | snipe-mode fade timer | vfunc 151 [V] |
| +0x5F0 / +0x5F8 | npc_param* / think_param | vfuncs 47/48 [V] |
| +0x638 | ChrAsm* | vfuncs 67/68 [V] |
| +0x6A0 | CSPlayerMenuCtrl* | vfuncs 192/193 [V] |
| **+0x6C0** | BlockPosition `{x,y,z,yaw}` (map-relative) | CT `Coordinates & Teleport/Player Character/Global Coords` + teleport script `getMapRelativeCoords` |
| **+0x6D0** | current block (map) id | CT |
| +0x6D4.. | safe_block_pos / safe_block_id / play_region_id | fsrs |

**EnemyIns** (`0x2A47090`): `+0x580` com_manipulator, `+0x588` net_ai_manipulator, `+0x590` ride_manipulator, `+0x5A0` npc_think_param. [V-src]

### 3.3 ChrInsModuleContainer (`[ChrIns+0x190]`), fsrs `chr_ins/module.rs`

| Off | Module | Verification |
|---|---|---|
| +0x00 | CSChrDataModule* | code: Kill `0x3EE762` [V]; CT `Hero/Stats` |
| +0x08 | action_flag | fsrs; code `0x3F5914` |
| +0x18 | time_act (`+0x20` current anim id, `+0x24` time played) | CT `Hero/Animation/Current Animation` |
| +0x28 | behavior (`+0x17C8` anim speed; `+0x10`→`+0x30` = hkbCharacter) | CT `Animation Speed.xml`, `PlayAnimation_code.cea` |
| +0x40 / +0x48 | super_armor / toughness | CT `Hero/SuperArmor`, `Hero/Toughness` |
| +0x58 | event (`+0x18` idle anim) | CT |
| **+0x68** | CSChrPhysicsModule* | code: 194 sites, plus ChrCtrl::SetPosition/GetPosition [V] |
| +0x70 / +0x78 / +0x80 / +0x88 | fall / ladder / action_request / throw | fsrs |
| +0x98 | damage module (CSChrDamageModule*) | fsrs ("damage: usize"); [U] in code |
| +0xC0 / +0xD0 / +0xE8 / +0xF8 | behavior_data / model_param_modifier / ride / wet | fsrs |

### 3.4 CSChrPhysicsModule (vftable `0x2A3C890`), fsrs `chr_ins/module/physics.rs`

| Off | Field | Verification |
|---|---|---|
| +0x08 | ChrIns* owner | fsrs |
| +0x20 | CSChrDataModule* | fsrs |
| **+0x50** | hkQuaternion orientation (xyzw) | code: SetOrientation `0x45FEF6`, SetYaw `0x45FD21` [V] |
| **+0x60** | hkQuaternion derived/"interpolated" orientation (recomputed by `0x463FA0`; used by the model-matrix builder `0x45EAA0`) | code: getter `0x45ECA0` = `lea rax,[rcx+0x60]` [V] |
| **+0x70** | hkVector4 position (Havok space; CT labels: X `+70`, height `+74`, Y `+78`) | code: getter `0x45E8F0`, setter `0x45EEF3` [V]; CT `Local Coords` |
| +0x80 | last_update_position (SetPosition writes it too) | code `0x45EEFA` [V] |
| +0x90 | u8, set by SetPosition; ChrCtrl::SetPosition refreshes the model matrix (`0x3CA960`) and clears it | code [V] |
| **+0x91** | u8 chr_proxy_pos_update_requested: SyncProxyPosition pushes `+0x70` into the proxies and clears it | code `0x45C925`/`0x45C966` [V] |
| +0x92 / +0x93 | standing_on_solid_ground / touching_solid_ground | fsrs; code getter `0x2D48C8` reads `+0x93` |
| **+0x98 / +0xA0** | char proxy pointers (Havok character proxy / rigid body) | code `0x45C935`/`0x45C950` [V] |
| +0x1D3 | fade_out_gravity_disabled (CT "NoGravity") | CT `NoGravity.xml`; fsrs |
| +0x1D5 | gravity_disabled (TAE action 27) | fsrs |
| +0x1DE | u8, set to 1 by SetPosition (meaning unknown) | code [V] / meaning [U] |
| +0x388 | i32 state used by `0x461730` (proxy kind?) | code [V] / meaning [U] |

### 3.5 ChrCtrl (vftable `0x2A2C6A0`), fsrs `cs/chr_ins.rs` struct `ChrCtrl`

| Off | Field | Verification |
|---|---|---|
| +0x10 | ChrIns* owner | code `0x3C768A` [V] |
| +0xC8 | ChrCtrlModifier* (`+0x24` bit0 checked by Kill) | code `0x3EE76E`, `0x3F4744` [V] |
| +0xE9 | bool disable_move (ladder) | fsrs |
| +0xF0 | ChrCtrlFlags (bit0 no player collision, bit1 no hit, bit2 no map collision) | fsrs |
| **+0xFC** | chr_proxy_flags: bit0 position sync request (bit1 rotation sync per fsrs) | code: `0x3C9270` sets bit0, `0x408170` sets/clears bit0 from a bool [V]; bit1 [V-src] (writers of `|2` exist at `0xF1EB99`, `0x1194F6B`, `0x119B45F`, not analysed) |
| +0x100 | hkVector4 requested proxy position | code `0x3C927A` [V] |
| +0x1B0 / +0x230 | physics_model_matrix / model_matrix (final render transform, F32ModelMatrix 0x40) | fsrs (layout consistent with `unk2b0`) |
| +0x2C0 / +0x2D0 / +0x2D4..+0x2DC | additional_orientation_quat / vertical_position_offset / scale_size_x,y,z | fsrs (consistent with `unk2e4`) |

### 3.6 CSChrDataModule (vftable `0x2A380B8`), fsrs `chr_ins/module/data.rs`

| Off | Field | Verification |
|---|---|---|
| **+0x138** | i32 hp | code: written by `0x437450` (`0x4374A2`), read by SetMaxHp [V]; CT `Hero/Stats/HP.xml` |
| **+0x13C** | i32 max_hp | code: SetMaxHp `0x4388A6` [V]; CT `MaxHP.xml` |
| +0x140 / +0x144 | max_uncapped_hp / base_hp | fsrs / CT ("MaxHP2", "BaseMaxHP") |
| +0x148 / +0x14C / +0x150 | fp / max_fp / base_fp. The CT swaps the names of `+0x14C` and `+0x150` | fsrs vs CT |
| +0x154 / +0x158 / +0x15C | stamina / max / base | CT and fsrs agree |
| **+0x19B** | debug byte: bit0 NoDead, bit1 NoDamage, bit2 NoFP, bit3 NoStamina | CT `Hero/Character Flags/*.xml`; code tests `f6 ?? 9b 01 00 00 01/02` at `0x4379D6`/`0x437974` [V] |

### 3.7 Recipes for task 3

**Player position write (so the Havok character proxy really moves).**

1. Preferred: call `ChrIns::SetPosition(player, &pos)` at `0x3F2690` from a game task. This is the game's own warp path, also used by summon/warp code at `0x4D7FEE`, `0x4D819D`, `0x6F2A38` and `0xAFE218`. Effects:
   - writes `physics+0x70` and `+0x80`
   - sets `+0x90 = +0x91 = 1` and `+0x1DE = 1`
   - immediately pushes the position into both Havok proxies (`0xC5D860`) and clears `+0x91`
   - refreshes the ChrCtrl model matrix. [V]
2. Alternative without calls: write the 16 bytes at `physics+0x70`, then set byte `physics+0x91 = 1`. The game's own SyncProxyPosition `0x45C910` (11 call sites in physics update code) consumes the flag. [V] mechanism; the per-frame timing of those calls is [U].
3. CT style: write only `+0x70/+0x74/+0x78`. This works for teleports according to the CT, but the next physics step may move you back to the proxy position. [U]

Notes:

- `pos` is a 16-byte, 16-aligned `hkVector4` in **Havok space**. Copy the current `+0x7C` w-component back in. [U] for what w means.
- Havok space shifts with the open-world origin. The task groups `BeginShiftWorldPosition` (160) and `EndShiftWorldPosition` (11) exist, and there are RTTI `CSWorldShiftFragmentProxy<...>` classes.
- The stable coordinates are `PlayerIns+0x6C0` BlockPosition and `+0x6D0` block id. Map MGSV coordinates through BlockPosition and block id, and convert to Havok each frame using the delta between the player's Havok and block positions (CT `mapRelativeTeleport` does this). [V-src]/[U]

**Rotation / yaw.**

- Call `ChrIns::SetYaw(player, &yaw_rad)` at `0x3F6180`. It builds a Y-axis quaternion from `yaw*0.5`, writes `physics+0x50`, and recomputes `+0x60`. [V]
- Alternatively call `CSChrPhysicsModule::SetOrientation(phys, &quat)` at `0x45FEE0`.
- The map-relative yaw is readable at `PlayerIns+0x6CC`.
- Which axis convention and sign MGSV uses must be tested at runtime. [U]

**HP / max HP / dead.**

- Read `data+0x138` and `data+0x13C` directly.
- To change HP with the game's clamping and no-death rules, call `ChangeHp(data, hp, false, 0.0f, ...)` at `0x438810`. The register and stack argument meanings beyond `edx = hp` are [U]; mirror the Kill call at `0x3EE74F`-`0x3EE765`: `r8b=0`, `xmm3=0`, `[rsp+0x20] = 1.0f` (constant at `0x32A1938`), `[rsp+0x28] = 1`.
- Dead check: `(ChrIns+0x1C5) & 0x80` [V], or `hp <= 0`.
- To kill: call `0x3EE730(chr, false)` [V code; arg meaning U].
- To make the ER puppet immortal while MGSV owns health: set data `+0x19B` bit0 (NoDead), bit1 (NoDamage) [V-src CT], or ChrIns `+0x538` bit3 (no hit).

**Enemy iteration.**

```
wcm = *(u8**)(base+0x3D69FF8)
for i in 0..196:
    set = *(u8**)(wcm + 0x1DED8 + 8*i)
    if !set: continue
    cap = *(u32*)(set+0x10); ent = *(u8**)(set+0x18)
    for j in 0..cap:
        chr = *(u8**)(ent + 0x10*j)
        if !chr or *(u8*)(ent+0x10*j+8) != 2 /*Active*/: continue
        team = *(u8*)(chr+0x6C)
        handle = *(u64*)(chr+0x8)
        phys = *(u8**)(*(u8**)(chr+0x190)+0x68); pos = phys+0x70
        data = **(u8***)(chr+0x190); hp = *(i32*)(data+0x138); maxhp = *(i32*)(data+0x13C)
        dead = *(u8*)(chr+0x1C5) & 0x80
        entity_id = *(u32*)(chr+0x1E8); npc_param = *(i32*)(chr+0x60); chr_type = *(i32*)(chr+0x68)
```

- Cheaper alternative: walk the distance-sorted `DLVector` at `WorldChrMan+0x1F1D0` (`first` at +0x1F1D8, `last` at +0x1F1E0, entries `{ChrIns*, f32 dist, u32}`). [V offsets]
- Effective team (with SpEffect overrides) comes from `ChrIns::GetTeamType` `0x3F1C90(chr, &u8)`. [V]
- Hostility: `NearEnemyFinder` (vft `0x2A282D0`) compares team types via `0x51B5D0(&a,&b)`, which returns nonzero when hostile. [V that the call exists in `run`, U semantics]

**Camera override.**

- `CSCamera` (`*0x3D669C8`):
  - `+0x08` pers_cam_1, `+0x10` pers_cam_2, `+0x18` pers_cam_3, `+0x20` pers_cam_4
  - `+0x28` u32 camera_mask
  - These offsets were verified by disassembling `0x3BB3E0` [V].
- That function copies a source cam into pers_cam_1 field by field: `+0x50` fov, `+0x54` aspect, `+0x58` near, `+0x5C` far, and the matrix rows `+0x10/+0x20/+0x30/+0x40`. Then it does `mask &= ~0x3F`. Source selection [V]:
  - bit 0x20 → cam_4
  - else bit 0x10 → cam_3
  - else bits 0x08 or 0x01 → cam_2
  - else bit 0x02 → cam_3 (the fsrs comment saying cam_4 is wrong for this exe)
  - else bit 0x04 → cam_4
- **CSCam/CSPersCam** (vft `0x2AA0A18`/`0x2AA0A40`): `+0x10` right, `+0x20` up, `+0x30` forward, `+0x40` position (camera→world rows, fsrs `CSCamExt`), `+0x50` fov, `+0x54` aspect, `+0x58` near, `+0x5C` far/"Render Dist". [V layout; units U: fov is probably vertical radians]
- **ChrCam** (player follow cam, `WorldChrMan+0x1ECE0`):
  - It embeds a CSPersCam at +0
  - `+0x60` ex_follow, `+0x68` aim, `+0x70` dist_view cams
  - `+0x78` request_camera_reset, `+0x7C` camera_type (7 = death cam), `+0xB4` is_movement_locked
  - [V-src]
- **FieldArea→GameRend** (`[FieldArea+0x20]`; 6 code refs [V]): `+0x18/+0x20/+0x28` CSPersCam0/1/2, `+0xC8` free-cam mode int, `+0xD0` CSDebugCam*. [V-src CT `Camera/*.xml`]
- Plan:
  1. Register a task at `CameraStep(106)` or, better, `DrawParamUpdate(107)`. In the verified timeline table (§3.9), DrawParamUpdate runs right after CameraStep and long before `Draw_Pre(144)`.
  2. In it, overwrite **pers_cam_1** (matrix rows, fov, near, far) with the MGSV camera.

  That is hook-free. Whether the renderer snapshots pers_cam_1 after DrawParamUpdate is [U]; verify at runtime. If not, mid-hook `0x3BB3E0`'s return (a code patch, see §7).
- Community freecams:
  - The TGA "Free Camera" (pav, vswarte) enables the built-in debug cam: it writes GameRend `+0xC8 = 1`, patches `0xE54580`/`0xE45120` to `mov al,1; ret`, and injects at `0x682825` (`mov edx,[rbx+0xC8]`) to toggle `GameMan+0xBC4`. [V CT script and AOBs]
  - `GameMan+0xBC4` is a 0-3 state machine written at `0x67B7C7`-`0x67C9F8`. **Do not use it for input blocking.** [V code; meaning U]

**Disabling player control (keep the ER player as a puppet).**

- **A. Pad level (recommended).**
  - `FD4PadManager` (`*0x4861D30`, fsrs `fd4/pad_manager.rs`) has `+0x48 pad_entry_map_list` (DLFixedVector of 4 `DLMap<RuntimeTypeHandle, PadEntry>*`, layout `{T[4], usize, usize len}` = 0x30). [V-src]
  - `PadEntry` is `{CSPad* entry, bool enable_use}`. [V-src]
  - Find the entry whose `CSPad` vtable is `CS::CSInGamePad_UserInput1` (`0x2BE35E8`) or `CS::CSInGamePad` (`0x2A2AA40`). Set `FD4BasePad.allow_polling` (`+0x20`) = false; per fsrs, `poll_digital_input` then returns false. [V-src]
  - The menu pad `CSMenuViewerPad` (`0x2BE4778`) stays live for the ER pause menu. [U]
- **B. Character level.** ChrIns `+0x538 |= 0x30` (bit5 no move + bit4 no attack/jump/actions), as in CT "No Move"/"No Attack". [V-src CT, code reads exist] Animation playback via `hkbCharacter::FireEvent` still works. [U]
- **C. Global.** `FD4PadManager+0x2F9 is_back_ground_window = 1` disables all ER input, menus included. It is written at `0xE362BF` and checked at 3 sites. [V code; semantics V-src]
- **D. OS level** (outside the exe, so Arxan-safe): hook `IDirectInputDevice8W::GetDeviceState` (vtable slot 9 of a probe device). The fromsoftware-rs debug `InputBlocker` does this in `crates/debug/src/input/dinput.rs`. XInput: hook `xinput1_4!XInputGetState` (imported by ordinal 2). [V-src approach]

**Visibility.**

- The ER player must stay visible, because it renders the converted Venom Snake model from the old MGS project.
- To hide or show any ChrIns:
  - `+0x1C5` bit3 (enable_render) [V writes, V-src meaning]
  - or fade with `+0x250 base_transparency_modifier = -1/+1` [V-src]
  - `+0x538` bit12 (character_disabled) hides and freezes. [V-src]
- `ChrCtrl+0x230` model_matrix, `+0x2D0` vertical offset and `+0x2D4..+0x2DC` scale (fsrs) let you offset or scale the rendered model without moving physics. [U]

**Damage.**

- ER enemy hits the puppet:
  - Keep the puppet alive with the data `+0x19B` NoDead bit. [V-src]
  - Detect hits by HP delta on `data+0x138` each frame, or by `ChrIns+0x180 last_hit_by` changes. [V-src]
  - DmgMan (`0x3D6A3E8`) and DmgHitRecordMan (`0x3D6A348`) exist as singletons. Their layouts are not mapped. [U]
  - The damage-module vtables (`0x2A39A90` / player `0x2A3A3E0` / enemy `0x2A3A0D8`, 23 vfuncs each) are candidates for an **instance-vptr swap** hook to cancel hits. Which vfunc applies damage is unidentified. [U]
- Venom Snake hits an ER NPC: use the game pipeline.
  - Spawn a bullet with an AtkParam-based `bullet_id` from the player's position (`0x3A2CB0`, `BulletSpawnData` in fsrs `bullet_manager.rs`, 0x110 bytes). This gives hit reactions, poise and aggro. [V RVA, U behaviour]
  - Or apply a damage SpEffect (`0x3E8DC0`). [V RVA]
  - Last resort: ChangeHp `0x438810`. When HP reaches 0 the game itself runs death; otherwise call `0x3EE730`.

**Animation.**

- Fire Havok behaviour events: `0xC15AB0(hkbCharacter*, L"W_...")`, with `hkbCharacter = ([[ChrIns+0x190]+0x28]+0x10)+0x30`, exactly as the CT `PlayAnimation_code.cea` does. Returns `-1` on failure. [V-src, V AOB]
- Current animation id: `[[ChrIns+0x190]+0x18]+0x20`. [V-src CT]

### 3.8 Other managers

| Singleton | Offsets | Verification |
|---|---|---|
| CSFlipper `*0x458DB58` | `+0x264` last_frame_time, `+0x268` task delta (s), `+0x2CC` game_speed | code: `0xDEB386 movss xmm1,[rax+0x2CC]; mulss xmm1,[rax+0x268]` (3 sites each) [V]. fsrs names `+0x277` "unk227" (a typo) |
| CSHavokMan `*0x3D7A0D0` | `+0x98` CSPhysWorld* | code: 246 refs [V] |
| CSMenuMan `*0x3D6F820` | `+0x80` CSPopupMenu*, `+0x90` UIState[0x46] (bit0 created, bit1 visible), `+0x13C` disable_save_menu, `+0x660` CSPlayerMenuCtrl | `+0x80`: 98 refs [V]; `+0x13C`: 1 ref [V]; others fsrs |
| GameDataMan `*0x3D61F98` | `+0x08` main PlayerGameData*, `+0x48` (CT bloodstain), `+0x58` game_settings | `+0x08`: 441 refs [V] |
| GameMan `*0x3D6D988` | `+0x10` warp_requested, `+0xAA0..` last load position (CT InitPos), `+0xBC4` state enum, `+0xD60` PartyMemberInfo* | code refs [V] (3 / 11 / 1) |
| CSWindow `*0x458F910` | `+0x08` HWND, `+0x28/+0x2C` screen w/h | fsrs |
| CSFade `*0x3D763B0` | `+0x08` fade system, `+0x10` CSFD4FadePlate*[9]; each plate has `+0x10` current RGBA, `+0x20` start, `+0x30` end, `+0x40` FD4Time timer, `+0x50` duration | fsrs `fade.rs` |
| CSNowLoadingHelper `*0x3D64F28` | `+0xD0` menu_load_counter (incremented per loading screen) | fsrs |
| FD4PadManager `*0x4861D30` | `+0x2F8` exit_foreground_signaled, `+0x2F9` is_back_ground_window, `+0x2FB` use_lib_sce_pad | code: `+0x2F8` write ×1, `+0x2F9` reads ×3 [V] |
| SoloParamRepository `*0x3D85F58` | see fsrs `cs/solo_param_repository.rs` (param lookups by index); CT now uses FD4ParamRepository iteration (`Param Mods/Dependencies/ParamUtils/.cea`) | [V-src] |

**Loading and menu state.** I could not statically find a single "loading" or "menu open" flag that I could verify.

- Practical gates: `main_player != null` and `main_player->chr_set_entry->load_status == 2`, plus `CSNowLoadingHelper.menu_load_counter` edges. [U]
- Pause/inventory open: probe `CSMenuMan` UIState bits at runtime. [U]

### 3.9 Task system (per-frame callbacks)

- `register_task` `0xEB3E50` [V].
- The task object needs `vftable[0] get_runtime_class`, `[1] destructor`, `[2] execute(this, FD4TaskData*)`.
- `FD4TaskData` is `{FD4Time{vptr, f32 dt, pad}, u32 group, i32 seed}`. This is exactly fromsoftware-rs `RecurringTask` (`crates/shared/src/task.rs`, `crates/eldenring/src/fd4/task.rs`). Its example `examples/apply-speffect` registers at `FrameBegin`. [V-src]
- Wait for `*(base+0x3D89708) != 0` (global_hinstance) and then for CSTask `*0x458FE88 != 0` before registering (`util/system.rs`). [V-src]
- **Task group indices (VERIFIED against the exe).** The static table at `.rdata 0x2C04D28` has 169 rows of 0x20 bytes, `{wchar_t* name; u32 f[6]}`. `f[4]` of row *r* is the index of row *r+1*. Under this reading, all 169 names match fromsoftware-rs `CSTaskGroupIndex` exactly. [V]
- The rows are in **timeline order**, which differs from index order [V data; "timeline" is an interpretation U]. For example, CameraStep (106) and DrawParamUpdate (107) run after `WorldAiManager_EndUpdateFormation` (132).
- Useful indices:

| Group | Index |
|---|---|
| FrameBegin | 0 |
| PadStep | 9 |
| GameFlowStep | 10 |
| EndShiftWorldPosition | 11 |
| TaskLineIdx_InGame_InGameStep | 16 |
| ChrIns_PreBehavior | 71 |
| ChrIns_PrePhysics | 78 |
| HavokWorldUpdate_Pre / _Post | 97 / 99 |
| CameraStep | 106 |
| DrawParamUpdate | 107 |
| ChrIns_PostPhysics | 113 |
| WorldChrMan_PostPhysics | 117 |
| DmgMan_Pre / _ShapeCast / _Post | 120 / 121 / 122 |
| MenuMan | 126 |
| GameFlowStep_Post | 141 |
| Draw_Pre | 144 |
| GraphicsStep | 145 |
| DrawStep | 153 |
| GameSceneDraw | 155 |
| Draw_Post | 158 |
| BeginShiftWorldPosition | 160 |
| Flip | 164 |
| FrameEnd | 168 |

- Suggested PhantomRing schedule [U]:
  - read MGSV state and drive the puppet at `ChrIns_PrePhysics (78)`
  - read back ER results (HP, enemies) at `WorldChrMan_PostPhysics (117)`
  - write the camera at `DrawParamUpdate (107)`
  - frame-sync handshake at `Draw_Pre (144)` / `Flip (164)`

### 3.10 D3D12 swapchain / command queue

- RTTI shows `GXBS::GXCommandQueue` (`0x2F274C8`) and `GXBS::GXSwapChainOperatorForMultiWindow` (`0x2F07CB8`). Both have only a destructor in their vtables, so the real D3D12 calls are non-virtual. [V]
- Device creation:
  - `0x1E890E0` → `0x1E9FD30` (r12 = device wrapper whose `+0` receives the `ID3D12Device*`) → `0x1EA2730` → `D3D12CreateDevice` thunk `0x2624DFC` (IAT `0x4C11FEC`, ord 101).
  - The factory is created via the `CreateDXGIFactory1` thunk `0x2624E08` at `0x1EAE5E6`/`0x1EAE9CF`. [V]
- I did **not** trace these to a global pointer. CSGraphics (`0x3D75CB8`) and RendMan (`0x3D7F130`) are the likely owners. [U]
- **Recommendation [U]:** use the hudhook/Kiero technique, as the fromsoftware-rs debug tool does with `hudhook::hooks::dx12::ImguiDx12Hooks` (`tools/debug-eldenring/src/lib.rs`, [V-src]):
  1. In the DLL, create a throwaway `IDXGIFactory`, `ID3D12Device`, `ID3D12CommandQueue` and swapchain.
  2. Read their vtables and hook `IDXGIFactory2::CreateSwapChainForHwnd` (index 15; its `pDevice` argument is the game's **direct command queue**), `IDXGISwapChain::Present` (8) / `Present1` (22), `IDXGISwapChain3::ResizeBuffers` (13), and `ID3D12CommandQueue::ExecuteCommandLists` (10).
  3. All of these live in DXVK dxgi / vkd3d-proton d3d12 under Proton, not in eldenring.exe, so Arxan never sees them.
  4. Load early (me3 `load_early`) to catch CreateSwapChainForHwnd. Otherwise capture the queue on the first ExecuteCommandLists with `GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT`.

---

## 4. Arxan (GuardIT) and safe hooking

- **Presence [V]:**
  - The second `.text` section (`0x4C13000`, 18 MB) holds the protection code.
  - Obfuscated jump stubs sit inside the main `.text`.
  - me3's log on this exe says `dearxan::disabler: arxan detected: true`.
- **Code restoration [V]:** the fromsoftware-rs pattern `B9 ? ? ? ? E8 ? ? ? ? F3 0F 11 05 ? ? ? ? [0-128] ' 72 ? 48 8D ? ? ? ? ?` (`crates/shared/src/arxan.rs`, credits tremwil and Yui) matches **181 sites** in this exe. The first is `0x1ECF63`, with its `jb` at `0x1ECF88`. Each is `mov ecx,id; call timer; movss [rip+..],xmm0; ...; jb skip; lea rdx,[..]; call restore`.
  - fromsoftware-rs `disable_code_restoration` patches each captured `jb` (0x72) to `jmp` (0xEB). Arxan periodically re-writes guarded code ranges to their original bytes, which silently reverts or crashes inline hooks. That restoration behaviour is [U], community knowledge.
- **me3 (local `tools/me3`, v0.13.0) [V]:**
  - It bundles `dearxan-0.5.2` (strings in `bin/win64/me3_mod_host.dll`: "analyzing Arxan stubs", "Arxan is now neutered").
  - It exposes the profile option `disable_arxan` (also in `me3`/`me3.exe`; CLI help text: "Neutralize Arxan/GuardIT code protection?").
  - The old MGS launches ran with `disable_arxan: false`. me3 still runs `schedule_after_arxan`, deferring native DLL init until Arxan's initialisers finish (it hooks the `__security_init_cookie` call at the entry point).
  - Native entries accept `load_early`, `initializer`, `finalizer`, `optional`, `load_before`/`load_after` (AttachConfig in the logs). [V]
- **Safe-by-construction techniques, in order:**
  1. **Data-only access.** Read and write struct fields. No code is modified.
  2. **Game task registration** via `register_task`. This is fromsoftware-rs's stated preference: "Prefer using the task runtime over hooking the memory image" (`arxan.rs` doc comment). [V-src]
  3. **Calling game functions** from our task: SetPosition, SetYaw, ChangeHp, spawn_bullet, apply_speffect, FireEvent, cast_ray. No patching. Calls must happen on the main/task thread (fromsoftware-rs safety notes). [V-src]
  4. **Instance vtable swaps.** Copy an object's vtable to our memory and change the object's vptr, for example on the player's damage module. `.rdata` and code are untouched. [U] that Arxan never checks object vptrs; standard practice.
  5. **Hooks in system DLLs** (dinput8, xinput1_4, dxgi, d3d12/vkd3d-proton). Arxan does not protect them. [U]
  6. **Inline/mid-function hooks in eldenring.exe code.** Only with me3 `disable_arxan = true`, or after running the 181-site restoration disabler before installing hooks. Keep exact-hash gating plus byte checks, as the old MGS project's `scripts/native/*.cpp` do (`HashExecutable()` with `target_sha256`).
     - The CT free-cam is an example of such patches (`0xE54580`, `0xE45120`, inject at `0x682825`).
     - The old project's summon-range hooks (`0xEA1577`, `0xEA58D0`) were never runtime-tested ("not a game-runtime test" in `docs/summon-anywhere.md`). [V-src]
- **Never** touch EasyAntiCheat. Launch through me3 with `start_online = false` and a separate save (the old profile uses `MGS_ER0000.sl2`). [V-src]

---

## 5. Open questions (need a runtime probe DLL)

1. Does writing CSCamera pers_cam_1 at `DrawParamUpdate (107)` reach the renderer? Which of pers_cam_2/3/4 is ChrCam? Compare pointers at runtime against `WorldChrMan+0x1ECE0`.
2. The meaning of the `ChangeHp` arguments other than hp; the semantics of `+0x1DE`, `+0x90`, and ChrCtrl `+0xFC` bit1.
3. Damage-module vfunc map, so hits on the puppet can be cancelled cleanly. Also DmgHitRecordMan layout.
4. A loading / menu-open flag.
5. The pointer path from CSGraphics/RendMan to `ID3D12CommandQueue`/`IDXGISwapChain` (not needed if the dxgi hooks of §3.10 are used).
6. Havok w-component convention, and the world-shift handling cadence.

## 6. Reproduce

```
cd tools
python3 er_aob_scan.py info                      # version / sha / sections
python3 er_aob_scan.py verify                    # 103/103 OK on sha 1a354710...7891
python3 er_aob_scan.py singletons                # 216 slots, 201 named
python3 er_aob_scan.py rtti --grep DamageModule --vfuncs 23
python3 er_aob_scan.py fieldref 0x3d69ff8 0x1e508 0x1ece0 0x10ee0
python3 er_aob_scan.py disasm 0x45eef0 --count 8
python3 er_aob_scan.py xref 0x3f2690 --decode
python3 er_aob_scan.py mkpattern 0x3d669c8      # regenerate a unique AOB for a slot
```
