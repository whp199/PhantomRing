# Rendering options: putting MGSV's Venom Snake on screen in Elden Ring

Research report for **PhantomRing** (MGSV guest -> Elden Ring host).
Author: research subagent, 2026-10-11/12. Scope: how Snake gets drawn in ER while MGSV drives him, and how
a native plugin injects drawing into ER's D3D12 frame. Movement, input, combat and launching are covered in the
sibling reports (`skycraft-host-side.md`, `skycraft-guest-side.md`, `prior-work.md`). This report cites
them where they overlap and does not repeat their tests.

## 0. How to read this report

| Tag | Meaning |
|---|---|
| **[V:src]** | VERIFIED: read in local source this session (SkyCraft, the old MGS project, fromsoftware-rs copy in `<old-project>/tools`, sources downloaded into the scratchpad). |
| **[V:bin]** | VERIFIED: checked against local game bytes this session (`eldenring.exe` 2.7.1.0, `mgsvtpp.exe` 1.0.15.4, Snake FMDL files, Proton files). |
| **[V:test]** | VERIFIED: a test program run this session (system `wine-11.0`, scratch prefixes). |
| **[V:web]** | Read this session from a primary web source (GitHub source file, project README, author's article). It was not checked against local bytes. |
| **[U]** | UNVERIFIED: memory, inference or design reasoning. Needs checking. |

Scratch artefacts (scripts, downloaded sources) are under
`<scratch>/render/`:
`peinfo.py`, `rtti.py`, `dis.py`, `aob.py`, `vtidx.py`, `fmdl_groups.py`, `shmtest/shm.cpp`, and `dl/`
(ERSkeletonMan, hudhook 0.9.3, erfps2, DirectXHook, IHHook, Dasaav's fromsoftware-rs fork).

---

## 1. Summary and recommendation

The options, ranked:

| | Option | Result quality | Effort | Main risk | Verdict |
|---|---|---|---|---|---|
| **A** | **ER draws Snake natively** (the old project's converted armour), and MGSV supplies the **pose**: per-frame bone transforms written into the ER player's Havok pose | High. ER lighting, shadows, fog, TAA, DoF and weapons all work. Snake's proportions follow ER's skeleton; no face or hair animation. | Medium (about 3-5 weeks) | Finding MGSV's bone palette (D3D11 capture), the ER override point, and retarget math | **Build this.** Phase 2 |
| C | ER draws the converted Snake with **ER's own animations**; MGSV supplies only gameplay state and position | Low faithfulness. The body moves like the ER Tarnished; no prone, CQC or box. | Small (days, assets exist) | None for rendering | **Build first as the baseline and fail-safe** (Phase 1). It is what A falls back to when the link is down. |
| D | **SkyCraft-literal**: MGSV supplies the bone palette and draw list; Snake's **original FMDL meshes** (already extracted) are drawn by a PhantomRing renderer **inside ER's D3D12 frame**, depth-tested against ER | Highest fidelity. Exact MGSV mesh, face, hair, items. Lighting is approximated unless injected into the GBuffer. | Large (about 6-10 weeks on top of A's capture work) | D3D12 mid-frame injection into an undocumented, GPU-driven, multithreaded renderer | Optional Phase 4, only if A's look is not good enough |
| B | **MGSV renders Snake; its frame is composited into ER** at Present with a depth test | Poor. Wrong lighting, no ER shadows or fog, 1-2 frames of camera lag, edge halos, double GPU cost. | Large (about 4-8 weeks) | Camera lockstep across processes, Snake isolation, latency | **Do not build.** Keep only as a debugging picture-in-picture. |

Plus the **GUI layer** (MGSV's HUD as a full-screen overlay, SkyCraft's `Overlay.cpp` model). This is
independent of A to D and needed by all of them (Phase 1b).

Why A beats SkyCraft's own method. SkyCraft ships Minecraft's posed **geometry** into Skyrim (`kRenAvatar`)
and draws it mid-frame in D3D11, using CommonLib's named render targets [V:src]. ER is D3D12: there is no
immediate context to save and restore, descriptor heaps and resource states must be tracked, and command lists
are recorded on several threads. ER's FLVER draws are also GPU-driven (`GXFlver_Common_MeshletCullForDrawCS`,
`..._MakeDrawIndirectArgsCS` [V:bin]). The old project already turned Venom into ER armour parts (model **1570**,
`{hd,bd,am,lg}_m_1570[_l]`, replacing the Knight set [V:bin file listing; identity per `prior-work.md`]). Driving that armour's skeleton from MGSV gives ER-quality rendering with no
render injection at all. The sibling host report reaches the same conclusion independently
(`skycraft-host-side.md` section 6, "Option B is strongly preferred" there = option A here).

---

## 2. What SkyCraft actually does for rendering (lessons) [V:src]

Read from `<SkyCraft>`:

1. **The design doc and the code differ.** `docs/DESIGN.md` section 9 planned three GPU-shared image layers
   (world, hand, GUI), each composited with a depth test. The shipped code (protocol `kVersion = 11`) sends
   **pixels only for the GUI layer**:
   - `fabric/.../FrameExporter.java` does `copyTextureToBuffer` into 3 staging buffers. The CPU then copies into
     the shared-memory triple buffer (`OverlayCtl` at 0x300, 3 slots of 3840x2160x4).
   - `skse/src/Overlay.cpp` uploads to a `D3D11_USAGE_DYNAMIC` texture and draws a full-screen triangle with
     premultiplied alpha at `IDXGISwapChain::Present`. The hook patches vtable index 8 in place
     (`Overlay.cpp:393-408`). Minecraft's crosshair gets a second "invert" blend pass.

   Everything world-attached goes as **geometry** over the 64 MB render ring: blocks (`kRenSection`), the
   player's posed body (`kRenAvatar`, built by `AvatarExporter.java` from Minecraft's entity renderer), other
   entities (`kRenScene`) and ragdoll parts.
2. **Mid-frame draw point.** `WorldRender.cpp` `DrawInFrame` runs right after `Main::RenderWorld`. The call site
   is `REL::ID(36559)+0x85E`, hooked with `write_call<5>` (`WorldRender.cpp:3291`). It writes into Skyrim's HDR
   scene target, depth, motion vectors and normal/TAA-mask targets. Skyrim's fog, tone mapping, bloom and TAA
   then treat the blocks as native. It copies the depth first (`CopyResource(depthCopyTex, depthTex)`) and saves
   and restores the full D3D11 pipeline state (`StateBackup`).
3. **Runtime self-calibration.** It learns the depth convention from the depth buffer itself: 16 probe samples
   decide reversed, standard or unusable (`WorldRender.cpp:1830-1876`). It logs what is bound at first use and
   falls back to drawing on the finished frame when targets look unexpected. **PhantomRing should copy these
   habits for anything touching ER's renderer.**
4. **Hiding the host's body.** Skyrim's first-person meshes are `SetAppCulled(true)` but their nodes are kept,
   because camera and animation use them (`Game.cpp:123-145`). The Skyrim HUD is hidden element by element
   through Scaleform `GFxValue`, keeping only the compass and a few prompts (`Game.cpp:331-366`).
5. **Transport.** A pagefile-backed named mapping `Local\SkyCraft_v1`
   (`CreateFileMappingW(INVALID_HANDLE_VALUE, ...)`, `Link.cpp:77`). **Across two Proton prefixes this does not
   work**; a file-backed mapping does (section 10).

The lesson for PhantomRing: only the GUI layer should cross as pixels. The body should cross as compact data
(pose or palette) and be drawn by the host.

---

## 3. Environment facts that matter for rendering

| Fact | Value | Tag |
|---|---|---|
| ER exe | 2.7.1.0, 87,042,128 B, sha256 `1a3547101327f65d0c76da2f9190ac0aa66871ea42bae2aecc61e11a8b597891` (same hash as the old project's live probes) | V:bin |
| ER D3D imports | `d3d12.dll`: `D3D12SerializeRootSignature`, ordinal **101**, ordinal **102**. `dxgi.dll`: `CreateDXGIFactory1`, `CreateDXGIFactory`. Proton's vkd3d-proton `d3d12.dll` exports 101 = `D3D12CreateDevice`, 102 = `D3D12GetDebugInterface`. | V:bin |
| ER graphics wrapper classes (RTTI) | `GXBS::GXCommandQueue` (vtable RVA 0x2f274c8), `GXBS::GXDeviceContext`, `GXBS::GXProxyDeviceContext`, `GXBS::GXSwapChainOperator`, `GXBS::GXSwapChainOperatorForMultiWindow` | V:bin |
| ER UI | Scaleform GFx (`Scaleform::Render::ThreadCommandQueue`, `CS::CSScaleformThreadCommandQueue` RTTI) | V:bin |
| ER render-feature strings | `Performance.IsEnableDynamicResolution`, `ToneMapDynamicResolutionVS.vpo`, `TempAA.vpo`, `DLAA.ppo`, `FXAA*.ppo`, `GBufferRT0..3`, `CopyDepthCS.cpo`, `DownSampleDepthCS.cpo`, `GXFlver_Common_{InsCullTestCS,SubmeshCullTestCS,MeshletCullForDrawCS,MeshletCullInstancingCS,MakeDrawIndirectArgsCS}.cpo`, `shader:/GXRayTracing.shaderbnd`, `ToneMap_PostOETFPS` (UTF-16) | V:bin |
| MGSV exe | 1.0.15.4 (`Tpp_main.exe`), timestamp 2026-07-07, sha256 `085c2f82...bb45`. Packed or obfuscated layout (entry in `.xdata`). The import table lists `dinput8.dll` (`DirectInput8Create`), `user32` (`RegisterRawInputDevices`, `GetRawInputData`) and **no d3d11/dxgi**, but strings `d3d11.dll` (x2), `D3D11CreateDevice`, `dxgi.dll`, `CreateDXGIFactory` are present. So D3D11 is loaded dynamically. | V:bin (dynamic-load mechanism U) |
| MGSV RTTI | Almost none (78 `.?AV` hits, all std/fox exceptions). Fox reflection names exist (`TppObjectBrushSkeletonModel`, `GeoxPartsTargetFollowSkeleton`, `AttachSkeleton`, ...). Expect RE of Fox internals to be slow. | V:bin |
| Proton Experimental `experimental-11.0-20261001` | vkd3d-proton `vkd3d-1.1-5641-g44cf7c20` (= 3.1.0, matches the old project's Proton log), DXVK `v3.1.1-47-g68530156` (`files/lib/wine/{vkd3d-proton,dxvk}/version`) | V:bin |
| Prefixes | ER `compatdata/1245620`, MGSV `compatdata/287700` (the sibling found the MGSV prefix not yet populated). They are separate wineservers. | V:bin (sibling) |
| Cross-prefix shared memory | A file-backed mapping on `Z:\dev\shm\...` stays coherent across two prefixes: a reader in prefix B saw all 301 sequence values a writer in prefix A published. Named pagefile sections fail across prefixes (sibling test). | V:test |
| Cross-process GPU sharing | DXVK/vkd3d-proton shared resources are "partially supported, Proton patches required" (DXVK dev guidelines; vkd3d-proton 2.7 notes). NT handles live in the wineserver, so **not across prefixes**. | V:web + U |

---

## 4. Option A: ER draws Snake; MGSV supplies the pose

### 4.1 Assets already available

- **Snake = model 1570.** `<old-project>/mods/elden-ring-v12/parts/{hd,bd,am,lg}_m_1570[_l].partsbnd.dcx`
  (5.8-28.6 MB each) replace the Knight set: protector rows 1500000/1500100/1500200/1500300, `equipModelId=1570`.
  **Correction to the task brief**: `*_m_9900` is **Quiet** (rows 9900000-9900300), not Snake. This is verified
  by the sibling report `prior-work.md` section 4.7 and by `docs/armor-low-texture-namespace-v7.md` (the
  player loads `*_m_1570`, the summon loads `*_m_9900_l`) [V:src]. The Snake retarget report's export path is
  `staging/elden-ring/character-variants-material-v3/snake/HD_M_1570.flver` [V:src]. Guns: `wp_a_9910/9911` (AM MRS-4, WU S.
  Pistol) [V:bin].
- The `_l` low-LOD binders must carry `_l`-suffixed texture names. Ignoring this gave the "chrome" look in the
  old project (`armor-low-texture-namespace-v7.md`) [V:src]. This matters for rendering at distance.
- Snake's retarget (`build_er_retarget.py`, `reports/snake-retarget-material-v3.json`) [V:src]:
  - The source skeleton has **169 bones**. **53** are mapped explicitly to ER `c0000` bones. All others (face,
    hair `_SIM`, belts, `_HLP` helpers) are rigidly bound to the nearest mapped ancestor. Most go to
    `SKL_004_HEAD` (53 bones), `SKL_013_LHAND` (18) and `SKL_002_CHEST` (14).
  - The exported armour is weighted to **39 ER bones**: `Pelvis`, `Spine`, `Spine2`, `Neck`, `Head`, clavicles,
    upper arms, forearms, hands, thighs, calves, feet, `Toe0`, and fingers. There are no twist bones and no
    `Spine1`.
  - The mesh was **reposed into ER's bind pose** and fitted to ER proportions. The old docs record a
    head-height/neck distortion history (V3/V4, improved in V5).
- The mapping is verbatim from `reports/snake-retarget-material-v3.json` `source_to_target` [V:src]:

  ```
  SKL_000_WAIST->Pelvis  SKL_001_SPINE->Spine  SKL_002_CHEST->Spine2  SKL_003_NECK->Neck  SKL_004_HEAD->Head
  SKL_0{1,2}0_{L,R}SHLD->{L,R}_Clavicle  _{L,R}UARM->_UpperArm  _{L,R}FARM->_Forearm  _{L,R}HAND->_Hand
  SKL_0{3,4}0_{L,R}THIGH->_Thigh  _LEG->_Calf  _FOOT->_Foot  _TOE->_Toe0
  SKL_{1,2}01..16 fingers -> {L,R}_Finger0/01/02, 1/11/12, 2/21/22, 3/3/31/32 (LF40 and LF41 both -> Finger3), 4/41/42
  ```

### 4.2 ER's player skeleton

Decoded from the installed `c0000.anibnd` by the old project (`reports/er-native-pose-v4-c0000.json`, sha256 of
the binder recorded there) [V:src]: **150 Havok bones**. Hierarchy points that matter for retargeting:

- `Master(0) -> RootPos(7) -> Pelvis(8) -> {L,R}_Thigh -> _Calf -> _Foot -> _Toe0`
- `RootPos(7) -> RootRotY(45) -> RootRotXZ(46) -> Spine(47) -> Spine1(48) -> Spine2(50) -> {Neck(85) -> Head(86), {L,R}_Clavicle -> _UpperArm -> _Forearm -> _Hand -> fingers, _Weapon -> _Shield}`
- Twist and helper bones are siblings of the main chain (`L_UpArmTwist`, `L_ForeArmTwist`, `L_ThighTwist`,
  `L_CalfTwist`, `*_Skirt`, `*_Mantle`, `Xtra_Multipurpose_Bone*`). There are also foot IK targets
  (`L_Foot_Target*` under `Master`) and `L_Hand_Target`/`R_Hand_Target` under `Spine2`.

So **Spine is not a child of Pelvis in ER** (both hang off `RootPos`), while MGSV's `SKL_001_SPINE` is a child of
`SKL_000_WAIST`. Local rotations cannot be copied; retargeting must go through model space (section 4.5).

The player FLVER has 488 bones and the Havok skeleton 150. Map by **name** (old project docs) [V:src].

### 4.3 How ER mods override bones at runtime (prior art)

1. **ERSkeletonMan** (Dasaav-dsv, MIT, last commit 2024-04-27) [V:web, source read]:
   - Hooks `CS::PlayerIns`/`CS::EnemyIns` vtable slot 10 (post-init, as an exit hook), slot 11 (unload) and
     slot 1 (destructor) to track characters.
   - Hooks **`CS::NoUpdateInterface` vtable slot 10** "for its good execution timing" and, every frame, writes
     modifiers into each tracked skeleton's **local** bone transforms.
   - Bone data layout `{V4D xzyVec; V4D qSpatial; V4D xzyScale}` = Havok `hkQsTransform` (48 bytes).
   - 2023 pointer chains:
     - character position: `ChrIns+0x190 -> +0x68 -> +0x70`; orientation `... +0x50`
     - hkbCharacter: `ChrIns+0x190 -> +0x28 -> +0x10 -> +0x30`
     - hkaSkeleton: `hkbCharacter+0x90 -> +0x28 -> +0x0`
     - pose buffer: `hkbCharacter+0x38 -> +0x0`; bone array at `layout + *(int*)(layout+0x54)`
     - hkaSkeleton fields: `+0x20` parent indices (`int16*`), `+0x28` bone count, `+0x30` bones (name pointer at
       `[i*2]`), `+0x40` reference pose
   - Also used by others: thefifthmatt's `runtimesouls` ships `include/SkeletonMan.h` [V:web file listing].
     thefifthmatt's **Floppy Ring** exaggerates all animations at runtime in ER, Sekiro and DS3 [V:web search
     summary].
2. **erfps2** (Dasaav, Rust, pushed 2026-10-07, MIT/Apache) [V:web, source read]. A current first-person mod for
   this game version:
   - Reads the behavior graph through `ChrIns.modules.behavior.hkb_context.hkb_character.behavior_graph.flat`.
   - Its fromsoftware-rs fork (commit `79266c97`) defines `CSChrBehaviorModule+0x60 = hkb_context`,
     `hkbCharacterContext+0x30 = hkb_character`, `hkbCharacter+0x98 = behavior_graph`. **This differs from
     SkeletonMan's 2023 chain** (`behavior+0x10 -> +0x30`); the layout moved between 1.x and 2.x.
   - Hides model parts but keeps their shadows through `CSModelDispEntity.disp_flags1` bit 0. Code comment:
     `0x100000A1` visible and casts a shadow, `0x100000A0` invisible and casts a shadow.
   - Hides face, helmet and hair (`parts_model_ins[0,2,6,21..25]`) and toggles model masks in
     `chr_asm_model_res+0x50/0x58/0x60`.
   - Reads the head dummy-poly matrix (dmy 907) through `GET_DMY_POS` 0x3e96b0.
   - **Replaces ER's `ToneMap_PostOETFPS` pixel shader** by hooking the shader repository's add-pixel-shader
     function. See section 7.4.
3. **VR mods** (Luke Ross R.E.A.L., Ilya's ERVR) move the player's weapon or hands from tracked controllers
   [U, closed source; search summary only]. They show that ER's arm pose can be driven externally.

### 4.4 Verified engine structure for the override point (2.7.1.0) [V:bin]

The RTTI scan is `rtti.py`, the disassembly `dis.py` (capstone).

- **`CS::NoUpdateInterface`**:
  - Primary vtable at RVA **0x2b8c8c0**. Slot 10 = **0x19b2960**.
  - Disassembly: profiler push with label **`"TtpEndFrame"`** (string at RVA 0x2efb800, next to
    `hkbnpPhysicsInterface::createShape`), `call 0x19b1520`, then profiler pop.
  - So SkeletonMan's timing hook still exists in 2.7.1. It is the behavior/physics end-of-frame
    ("TTP" = time-to-physics?) [interpretation U].
- **`CS::CSFD4LocationHkaPoseImporter`** (the FD4 "location" node that imports a Havok pose into the model
  transform tree; `ChrIns` has a field named `hka_pose_importer` in fromsoftware-rs [V:src]):
  - Vtable RVA **0x2b70360**, 11 slots: `0xb516a0, 0xb4b610, 0xb4b960, 0xb4b950, 0x4a01c0, 0xb4b790, 0xb4b670,
    0xb4b810, 0xb4b6e0, 0xb4b900, 0xb51450`.
  - Slots 5 and 6 (`0xb4b790`, `0xb4b670`) take a bone index. They `add rcx,0x48`, call **0x1654c30**, then index
    the returned array with `idx*0x30` (48 bytes = hkQsTransform).
  - Slots 7 and 8 (`0xb4b810`, `0xb4b6e0`) take `(out, first, count)` and loop over a bone range:
    - slot 7 writes 0x40-byte matrices through `0x1681b70` (hkQsTransform -> 4x4);
    - slot 8 writes 0x30-byte records through `0xb459d0`.
  - These are the batch exporters the model and skinning side reads [interpretation U].
  - **0x1654c30** = `call 0x16551c0` then `lea rax,[rbx+0x18]; ret`.
  - **0x16551c0** checks a byte at `+0x38`, loops over bones, tests `boneFlags[i] & 2` (array at `+0x28`), reads
    the parent index as `int16` from `skeleton+0x20` (`-1` = root), and composes local transforms (`+0x08`) into
    model transforms (`+0x18`). The bone count comes from `skeleton+0x38`.
  - This is exactly Havok `hkaPose::syncModelSpace()` and `getSyncedPoseModelSpace()`. **In 2.7.1 the hkaPose
    layout is**: `+0x00 skeleton*`, `+0x08 m_localPose` (hkArray of hkQsTransform: data, size, capAndFlags with
    `& 0x3fffffff`), `+0x18 m_modelPose`, `+0x28 m_boneFlags`, `+0x38 m_modelInSync`. The importer embeds or
    holds it at `+0x48`.
  - The hkaSkeleton offsets used here (`+0x20` parent indices, `+0x38` bone count) agree with SkeletonMan's
    struct. That makes SkeletonMan's hkaSkeleton layout still valid [V:bin + V:src].
- Related RTTI present [V:bin]: `CS::CSFD4LocationPose` (vtable 0x2b71100), `CS::CSFD4LocationExtraFaceBoneModifier`
  (0x2b700a0), `CS::CSFD4LocationMsbPartsPoseModifier`, `CS::CSHkBehFootIkSceneModifier` (0x2b89100),
  `CS::CSHkBehHandIkSceneModifier`, `CustomLookAtTwistModifier`, `hkbPoseStoringGeneratorOutputListener`,
  `hkbFootIkModifier`, `hkbHandIkModifier`, `hkbLookAtModifier`, `hknpRagdoll*`, `hkbGetUpModifier`.
- `CS::PlayerIns` primary vtable 0x2a7fbb0 (slot 10 = 0x651a40, slot 11 = 0x655390), matching the sibling [V:bin].

**Two candidate override points:**

| | Where | Space | Pros | Cons |
|---|---|---|---|---|
| **A1** | `NoUpdateInterface` slot 10 (`TtpEndFrame`, RVA 0x19b2960) post-hook. Write **local** hkQsTransforms of the player's pose buffer, SkeletonMan-style. | local | Proven technique (SkeletonMan, Floppy Ring). Everything downstream follows: model matrices, armour parts, weapon dummies, probably hit capsules. | The 2.7.1 pointer chain to the pose buffer must be re-derived; SkeletonMan's hkbCharacter offsets are 1.x-era. Ordering against foot IK and the importer must be checked. |
| **A2** | Player's `CSFD4LocationHkaPoseImporter`: replace vtable slots 7/8 (0xb4b810/0xb4b6e0) for that instance, or write `pose+0x18` model-space transforms and keep `+0x38` in sync | model | Model space is what the retarget naturally produces. Sits after Havok IK. Purely visual (the behavior/physics state is untouched). | Static analysis only. The vtable is shared by all characters, so filter on `this`. Unknown whether dummy polys and hitboxes read the same path. Writing model space while `boneFlags` mark local dirty could be re-synced over. |

Recommendation: probe both (section 12, P0.2). Prefer **A1** if the chain verifies, since it is the proven
path. Use A2 as the fallback.

### 4.5 Retarget math (MGSV -> ER) [U, standard; constants found in Phase 0]

Per frame, for each mapped ER bone `e` with MGSV source `s`:

1. MGSV gives `W_s` = bone world transform. With a captured skinning palette `P_s` (section 4.6):
   `W_s = P_s * T(bindWorldPos_s)`. FMDL bones store **only positions** (`localPosition`, `worldPosition`) and no
   bind rotation (`Fmdl.cs ReadBones` [V:src]). The old project cancels Blender display orientations rather than
   treating them as Fox bind rotations [V:src doc], so Fox bind rotations are taken as identity [U, check by
   bone-length self-test].
2. Make it character-relative: `C_s = Root_mgsv^-1 * W_s`, where `Root_mgsv` is Snake's position and yaw (the
   same values the movement link already streams).
3. Axis change `A` (Fox -> Havok/ER). Pin it once with a T-pose capture. Both are Y-up metres [U].
4. Model-space target rotation: `Q_er_model(e) = A(Q(C_s)) * Q_er_bind_model(e)`. Fox bind rotation is identity,
   so the ER bind rotation is the only correction.
5. Translations: keep ER's bind bone lengths (the mesh is fitted to them). Only **Pelvis/RootPos height** comes
   from MGSV, scaled by `pelvisHeight_er / pelvisHeight_mgsv_bind`.
6. Convert to local for A1: `L(e) = M(parent_er(e))^-1 * M(e)`, walking ER's hierarchy root-first and using ER's
   own pose for unmapped parents (`RootRotY`, `RootRotXZ`, `Spine1`). For `Spine1` (no source), give it half of
   the `Spine -> Spine2` rotation (slerp 0.5) so the armour's Spine/Spine2 weights bend smoothly.
7. Twist bones are not used by the armour; leave them on ER's pose.

Data per frame is about 1.5 KB (53 x (quaternion + position)), or about 12 KB for the full palette (244 bone
slots x 48 B), so latency and bandwidth do not matter.

### 4.6 Capturing Snake's pose in MGSV

Fox internals are mostly unnamed (section 3), so capture at the **D3D11 layer**, independent of Fox internals:

- Snake's meshes [V:bin, `fmdl_groups.py` on the old project's extracted files]:

  | FMDL | Bones | Meshes | Vertices | Bone groups (per-draw palettes) | Largest group |
  |---|---:|---:|---:|---|---:|
  | `sna0_main0_def.fmdl` (normal fatigues body) | 153 | 28 | 5,297 | 11: 13,1,2,21,4,2,8,2,10,1,32 | **32** |
  | `sna0_face0_cov.fmdl` | 57 | 24 | 4,341 | 6: 26,32,32,32,19,5 | **32** |
  | `sna0_arm0_cov.fmdl` (bionic arm) | 34 | 2 | 3,122 | 2: 21,1 | 21 |

  FMDL mesh records carry a `boneGroupIndex`, and FMDL Studio's `maxBonesPerMesh = 32` [V:src]. Fox therefore
  skins on the GPU with **at most 32 matrices per draw**, uploaded per draw [U: the CB vs buffer and 3x4 vs 4x4
  layout are confirmed by probe].
- Hooks in MGSV (DXVK D3D11). Vtable indices from mingw headers [V:src]:
  - `ID3D11Device::CreateBuffer` (3): tag Snake's vertex and index buffers by byte size. The FMDL gives exact
    vertex counts and strides.
  - `ID3D11DeviceContext::IASetVertexBuffers` (18) and `IASetIndexBuffer` (19).
  - `Map` (14), `Unmap` (15), `UpdateSubresource` (48): shadow the contents of constant buffers.
  - `VSSetConstantBuffers` (7), and `DrawIndexed` (12) / `DrawIndexedInstanced` (20): at each Snake draw,
    snapshot the bound VS constant buffer whose size fits 32 x 48 or 32 x 64 bytes.
  - `FinishCommandList` (114) / `ExecuteCommandList` (58): handle deferred contexts if Fox uses them [U].
- Mesh `i` -> `boneGroups[meshInfo[i].boneGroupIndex].boneIndices[j]` -> FMDL bone -> name (FMDL string table) ->
  `SKL_*` -> ER bone.
- **Self-check**: `|pos(child) - pos(parent)|` from captured palettes must equal the FMDL bind distance. If it
  does not, the palette convention is different (e.g. camera-relative or transposed).
- **Space**: Fox may upload camera-relative world matrices [U]. Undo this with the view CB of the same draw, or
  with Snake's root position from the movement link.
- **Visibility caveat**: GPU palettes only exist when Snake is drawn (and not culled). In MGSV's third-person
  camera he nearly always is. The MGSV hook can also **drop the actual draw calls** after capturing the CB, which
  saves GPU (section 9). A later memory hook into Fox's animation output (found with Ghidra) removes the
  dependency [U].
- **Existing MGSV hooking base**: IHHook (TinManTex) [V:web, file listing + changelog]:
  - A `dinput8.dll` proxy (MGSV imports `dinput8` [V:bin]).
  - `D3D11Hook.cpp`: Present and ResizeBuffers through a dummy `D3D11CreateDeviceAndSwapChain(D3D_DRIVER_TYPE_NULL)`
    vtable, code from REFramework.
  - RawInput handling, MinHook, Ghidra address-export scripts, `Hooks_FOV`/InfCam camera hooks, Lua bindings.
  - Address headers are for **1.0.15.3** (en/jp). The local exe is **1.0.15.4** (2026-07-07), so pattern-scan
    rather than reuse addresses [V:bin version].

### 4.7 What ER must keep doing (and stop doing)

- **Neutral behavior state.** If ER plays its own locomotion or attack animations underneath, their TAE events
  still fire: root motion, attack hitboxes, i-frames, sounds, SFX. While MGSV drives, keep the ER player in a
  neutral idle or "puppet" state and get combat events from MGSV (SkyCraft's `HitActor` model). The behavior
  side belongs to the movement/combat research.
- **Feet.** Overriding leg bones discards ER's foot IK (`hkbFootIkModifier`, `CSHkBehFootIkSceneModifier`).
  MGSV's own IK works against MGSV's ground, which matches ER's only if collision is mirrored (collision research).
  Fallback: a small two-bone foot IK on the ER side using ER raycasts (`cs_phys_world_cast_ray`, fromsoftware-rs
  2.7.1 RVA 0xc71e00, prologue checked by the sibling [V:bin]).
- **Weapons** attach to `R_Weapon`/`L_Weapon`, children of the hands, so they follow the MGSV hand pose.
  Per-weapon grip offsets need tuning because the guns were authored for ER crossbow grips [U].
- **Not reproduced in A**: facial animation (ER has `Jaw`, eye bones and an extra-face-bone modifier class, but no
  MGSV facial rig), hair and scarf simulation (`SKL_6xx_*_SIM` are rigid), the cardboard box, the Fulton
  balloon, the bionic arm variants, and stance-specific props. Each would need an ER asset, or option D.
- **Co-op (Seamless)**: other players see the ER puppet with ER's network-synced animation, not the overridden
  pose [U].

### 4.8 A: pros, cons, risks, effort

- **Pros**:
  - ER lights it, shadows it (cast and received), fogs it, and runs TAA/DoF/motion blur on it.
  - Weapons and dummy polys follow; no D3D12 injection.
  - The pose stream is tiny, and ER's own animation is a built-in fallback.
- **Cons**:
  - ER proportions and the converted mesh's known quality issues (neck/head history).
  - No face or hair animation; MGSV-only props need ER assets.
- **Risks**:
  - (1) MGSV palette capture (is Fox's skinning where we think it is?).
  - (2) ER override timing in 2.7.1 (IK or the importer may overwrite it).
  - (3) Foot contact on uneven ER ground.
  - (4) Retarget sign and axis bugs. These are cheap to find with CSEzDraw (section 7.6).
- **Effort**: P0 probes about 1 week; MGSV capture 1-2 weeks; ER override and retarget 1-2 weeks; polish 1 week.
- **RE needed**:
  - MGSV: Snake draw identification, palette CB layout, space convention.
  - ER: 2.7.1 chain from `PlayerIns` to the pose buffer or the importer instance; confirm write timing.

---

## 5. Option B: MGSV renders Snake; its frame is composited into ER

### 5.1 Mechanics

1. ER sends its camera (`CSCamera` singleton -> `pers_cam_*`, `CSCam{matrix, fov, aspect_ratio, near_plane,
   far_plane}` in fromsoftware-rs [V:src]) to MGSV each frame.
2. MGSV forces its camera to match. Lua has `Player.SetAroundCameraManualMode` 0x9dcd80,
   `...ManualModeParams` 0x9dce00, `Player.RequestToSetCameraRotation` 0x9daf90 and
   `Player.RequestToSetCameraFocalLengthAndDistance` 0x9dad10 (1.0.15.4 RVAs from the sibling's Lua map
   `data/mgsv_lua_libs.tsv` [V:bin via sibling]). IHHook has camera/FOV hooks [V:web]. A per-frame exact
   override would need a native hook [U].
3. MGSV renders at ER's resolution. The hook reads back colour, depth and a Snake mask (CopyResource to staging,
   Map). This crosses prefixes through file-backed shared memory: 1440p RGBA8 is 14.7 MB plus 14.7 MB for R32F
   depth per frame, about 1.8 GB/s at 60 fps.
4. At ER's Present (or before the UI composite, section 7.3), upload and draw Snake's pixels where
   `linearDepth_mgsv < linearDepth_er`. Both depths must be linearised with their own near/far. MGSV's depth is
   reversed float32 (Courrèges' MGSV study [V:web]); ER's is probably reversed [U] (section 7.5).

### 5.2 Isolating Snake

MGSV draws a full world around him. Ways to cut him out:

- **Dynamic-mesh velocity mask.** Fox renders a velocity pass for dynamic meshes first, with the red channel as a
  "dynamic" mask (Courrèges [V:web]). Intersect it with a screen box around Snake's projected bounds. This
  catches soldiers and vehicles that overlap him.
- **Draw tagging.** Use the same Snake VB/IB tagging as section 4.6, and replay Snake's draws into a separate
  mask target, or set a stencil bit. This is exact.
- **Depth-reconstructed world mask.** Stand Snake on a flat, empty MGSV area and keep pixels whose reconstructed
  world position is within a capsule around him. This is fragile.

### 5.3 Why not

- **Lighting.** Snake is lit by MGSV's sun, sky, SH GI and SSAO in Afghanistan or Africa, not ER's world.
- **Shadows.** He casts no shadow in ER and receives none.
- **Post-processing.** ER fog, bloom, DoF and colour grading skip him unless he is injected before ER's post
  chain, which needs the same mid-frame D3D12 work as D.
- **Edges.** MGSV uses FXAA (post AA) rather than TAA (Courrèges [V:web]), so the halos expected from TAA are
  smaller. Hair cards and eyelashes still blend MGSV background into the edge. MGSV's motion blur, DoF, bloom and
  lens flares must be disabled.
- **Latency and lockstep.** ER frame N's camera must reach MGSV, MGSV must render (16 ms), then readback, copy and
  upload. That is 1-2 frames of lag behind ER's camera, visible as Snake swimming against the world during camera
  turns, unless ER waits (stalls) or reprojects. SkyCraft designed exactly this lockstep in DESIGN.md section 9,
  then shipped geometry instead [V:src].
- **Transparency.** ER particles and alpha-tested grass that write no depth get drawn over or under wrongly.
- **Cost.** Both games render full scenes at full resolution, plus about 2 GB/s of memcpy.
- **What B buys over A**: the exact MGSV look, including face and hair. Option D gives that too, with correct
  occlusion and no camera lag.

Keep B only as a **debug picture-in-picture**: MGSV's own frame, downscaled into an ER corner. It is useful for
validating camera and pose sync.

---

## 6. Option C: ER animations only

- Baseline that needs **no rendering work**:
  - The player wears the v12 Snake armour.
  - MGSV-driven position and yaw come from the movement link.
  - ER's behavior plays its own locomotion. MGSV state (crouch, prone, aim, sprint) maps onto ER actions through
    action requests or HKS [U] (TGA has a "PlayAnimation" script [V:src CT listing]).
- **Weakest faithfulness.** ER has no prone crawl, CQC throws, dives, box, or MGSV aiming poses. Bringing MGSV
  clips over offline (the old project proved GANI -> ER HKX for D-Dog [V:src `ddog-original-er-hkx-proof.md`])
  needs behavior-graph integration per clip. That is heavy and still not a passthrough.
- **Role in the plan.** Phase 1's first visible result, and **A's fail-safe**: when the pose stream is stale
  (more than 3 frames old) or the link is down, stop overriding and ER's animation takes over. This is SkyCraft's
  "drop to a safe state" rule.

---

## 7. ER D3D12 injection technique

### 7.1 Getting the device, queue and swap chain

Vtable indices, computed from `/usr/lib/mingw64-toolchain/mingw/include/{d3d12,dxgi1_2,dxgi1_4,d3d11}.h` [V:src]:

| Interface | Method | Index |
|---|---|---:|
| `IDXGISwapChain` | Present / GetBuffer / ResizeBuffers / Present1 | 8 / 9 / 13 / 22 |
| `IDXGISwapChain3` | GetCurrentBackBufferIndex / ResizeBuffers1 | 36 / 39 |
| `IDXGIFactory2` | CreateSwapChain / CreateSwapChainForHwnd | 10 / 15 |
| `ID3D12CommandQueue` | ExecuteCommandLists / Signal / GetDesc | 10 / 14 / 18 |
| `ID3D12Device` | CreateCommandQueue / CreateGraphicsPipelineState / CreateShaderResourceView / CreateRenderTargetView / CreateDepthStencilView / CreateCommittedResource / CreatePlacedResource | 8 / 10 / 18 / 20 / 21 / 27 / 29 |
| `ID3D12GraphicsCommandList` | Close / Reset / DrawInstanced / DrawIndexedInstanced / CopyTextureRegion / CopyResource / SetPipelineState / ResourceBarrier / SetDescriptorHeaps / SetGraphicsRootSignature / OMSetRenderTargets / ClearDepthStencilView / ClearRenderTargetView / ExecuteIndirect | 9 / 10 / 12 / 13 / 16 / 17 / 25 / 26 / 28 / 30 / 46 / 47 / 48 / 59 |

How existing tools get the queue:

- **hudhook 0.9.3** (veeenu, 2026-09-08; Rust; "runs on Windows and Wine/Proton") [V:web, `src/hooks/dx12.rs`
  read]. Three capture paths:
  1. Hook `IDXGIFactory::CreateSwapChain`/`IDXGIFactory2::CreateSwapChainForHwnd`. The `device` argument of a D3D12
     swap chain **is the command queue** (`command_queue_from_unknown`, checks `GetDesc().Type == DIRECT`).
  2. `ResizeBuffers1`'s `ppPresentQueue`.
  3. Hook `ExecuteCommandLists` and accept a queue whose pointer appears in the first 512 pointer-sized words of
     the swap chain object (`check_command_queue`).

  Vtables are found through a dummy device, queue, factory and swap chain on a dummy window. **eldenring-practice-tool 1.9.9** uses `hudhook = 0.9.3, features = ["dx12","inject"]`, and its README
  says it "fully supports Linux and should run on Steam Deck seamlessly" [V:web]. This is the strongest evidence
  that a D3D12 Present-time overlay works on ER under Proton.
- **DirectXHook** (techiew; base of the ER mods "First Person Souls" v1 and "Pause the game") [V:web, source
  read]: creates a dummy queue, patches the shared vtable slot `ExecuteCommandLists` (index 10), takes the first
  queue seen, then renders through **D3D11On12** [U whether DXVK and vkd3d-proton's D3D11On12 works for this
  under Proton; native D3D12 rendering as in hudhook is safer].
- **Load timing.** me3 loads natives in `before_game_main` (`load_early = true`) or `after_game_main`
  (`crates/mod-host/src/lib.rs:148-200` [V:src me3 source in the old project]). Both run before ER initialises
  graphics [U, but every overlay tool depends on it]. So path 1 above (factory hook) catches the queue directly,
  with no pointer scanning.
- **From game structures**: an instance of `GXBS::GXCommandQueue` (vtable 0x2f274c8 [V:bin]) presumably wraps
  the native queue [U]. Not needed if the factory hook works.
- **Arxan**: vtable patches on vkd3d-proton/DXVK COM objects are outside `eldenring.exe` and unaffected by
  ER's code-restoration guards. Inline hooks inside `eldenring.exe` (A2, shader repository) are what erfps2 and
  me3 already do in practice [V:web erfps2 uses inline `diversion` hooks on 2.7.1].

### 7.2 Drawing at Present (overlay recipe; GUI layer and debug views)

Standard D3D12 (hudhook backend, sibling section 6) [U, standard]:

1. Per back buffer: a command allocator, a list, and an RTV in our own heap.
2. Fence-guarded upload ring: copy the newest shared-memory slot into a persistently mapped UPLOAD buffer, then
   `CopyTextureRegion` into a DEFAULT texture.
3. In the Present hook: barrier back buffer PRESENT -> RENDER_TARGET, draw a full-screen triangle with
   premultiplied alpha, barrier back, `ExecuteCommandLists` on **the game's queue**, then call the original
   Present.
4. Handle `ResizeBuffers`/`ResizeBuffers1` (release references to back buffers first).
5. **HDR**: ER has HDR output. Read the swap-chain format (`GetDesc1`) and colour space. If R10G10B10A2 with
   PQ/ST.2084 or FP16, encode the overlay accordingly [U: ER's exact HDR swap-chain format].

### 7.3 ER's frame structure and where to insert mid-frame

From mamoniem's "Behind the Pretty Frames: Elden Ring" (2022, version 1.0x, 1080p max) [V:web article]. **The
pass order may have changed with ray tracing (2.x)** [U]:

| Order | Pass | Detail |
|---:|---|---|
| 1 | Resource copy and create | about 25k `CopyDescriptorsSimple`/`CreateConstantBufferView` per frame |
| 2 | Copy and clear | previous-frame GBuffer data carried over; `ClearDepthStencilView` |
| 3 | CSM depth | 5 cascades, 8192x4096, `R16_TYPELESS` |
| 4 | GBuffer | Colour `R8G8B8A8_TYPELESS`, Normal `R10G10B10A2_UNORM`, Surface `R8G8B8A8_UNORM` (metallic, roughness, shadow), two sometimes-black extra RTs. Depth **`D32S8` typeless**, stencil `R8_UINT`, motion `R16G16_FLOAT`. 8-20+ passes. |
| 5 | Decals | |
| 6 | Irradiance/specular accumulation | `R11G11B10_FLOAT` |
| 7 | Depth downsample | compute |
| 8 | SSAO | |
| 9 | Shadow composite | |
| 10 | Lighting | |
| 11 | Sky and atmosphere | |
| 12 | Post | AA (temporal), glow/bloom, light shafts, glare, tone mapping, LUT grading |
| 13 | **UI** | rendered into its own **`UIScene` (R8G8B8A8_UNORM, full res)** |
| 14 | **Final composite** | combines `HDRScene` and `UIScene` (UI alpha), with `DisplayMappingData{outputGammaForSDR, noUIBlend, enableDithering,...}`, into the swap chain |

Other article facts: a graphics queue plus a compute queue; and "dynamic screen-percentage parameters" for post
buffers.

Local binary corroboration [V:bin]: `GBufferRT0..3`, `ToneMap*` shaders, `TempAA.vpo`, `DLAA.ppo`,
`CopyDepthCS.cpo`, `ToneMapDynamicResolutionVS.vpo`, `Performance.IsEnableDynamicResolution`, Scaleform RTTI.

**Insertion strategies, from easiest to hardest:**

1. **Present-time** (section 7.2). This is after the UI composite. Fine for the GUI layer. For world drawing it
   puts things over ER's HUD.
2. **Queue-level insertion before the final composite.** Hook `ExecuteCommandLists` (index 10). If the command
   list holding the composite is identifiable, submit **our own command list** just before it on the same queue,
   splitting the game's array into two `ExecuteCommandLists` calls if needed.
   - Each command list starts with clean state, so no state save or restore is needed; this is D3D12's big
     advantage here.
   - The work: identify that list. Tag command lists while they record, through `SetPipelineState` (25) and
     `OMSetRenderTargets` (46) hooks: "this list binds the swap-chain RTV" or "this list uses the PSO containing
     shader X".
   - The **resource states** of ER resources we touch (HDRScene, depth) at that point must be known.
     `ResourceBarrier` (26) hooks track the last state per resource; transition back afterwards.
3. **Inline insertion inside a game command list** at a recognised draw. Requires shadow-tracking the root
   signature, descriptor heaps, PSO, root arguments (not queryable in D3D12), RTs, viewports, scissors and
   vertex/index buffers to restore them (ReShade's add-on API does this). ER records on several threads
   (`GXDeviceContext`/`GXProxyDeviceContext` RTTI [V:bin]). Highest risk.
4. **GBuffer injection** (for D at full quality). Write Snake into GBufferRT0..3 + depth + motion between the
   GBuffer and lighting passes, matching ER's encodings (normal packing, surface channels). ER then lights and
   shadows him (receiving only; casting needs CSM-pass injection too). Needs detailed RE of ER's GBuffer
   shaders [U].

**Shader-name -> PSO identification** (makes strategies 2-4 practical):

- ER registers shaders **by name** with its shader repository. erfps2 hooks the add-pixel-shader function at
  **RVA 0xb7bfc0**. The full pattern `48 8b 0d ?? ?? ?? ?? 4d 8b cd 4d 8b c4 48 8b d7 e8 <rel32> eb` is unique on
  the local exe at 0x2230fc and resolves to exactly 0xb7bfc0 [V:bin].
- The callback gets `(repository, PCWSTR name, const u8* blob, usize len)`. erfps2 swaps the blob for
  `"ToneMap_PostOETFPS"` [V:web].
- The pattern prefix without the trailing `eb` also hits five other sites: 0xb78180, 0xb78e90, 0xb7a5a0,
  0xb7b2b0, and one more [V:bin]. These are plausibly the other stages' add functions (vertex, compute, ...) [U].
- Recipe:
  1. Log `name -> blob hash` there.
  2. In `ID3D12Device::CreateGraphicsPipelineState` (10), map PSO -> shader names by matching the
     `VS`/`PS` bytecode pointer or hash.
  3. In `SetPipelineState` (25), each command list then reports which named pass it is recording.

  This gives a robust per-frame timeline, e.g. "list #k contains `ToneMap_*`", "list #m binds the swap-chain RTV".
- erfps2's other verified hooks show in-pipeline shader edits are viable on 2.7.1 [V:bin pattern matches]:
  - `USES_DITHERING` 0x1a69830 (dithering toggle).
  - `CB_FISHEYE_HOOK` 0x1b931ab: a mid-function hook that patches the tone-map constant buffer
    (`ToneMapCb` layout in `src/shaders.rs`).

### 7.4 Shader replacement as a cheap compositing trick [U]

erfps2 shows a replaced ER post shader runs fine. A variant of option B or D could replace a late post shader
(e.g. the tone map) with one that also samples an extra texture. That needs an extra descriptor in the root
signature, which shader replacement alone cannot add, so it would have to reuse an existing slot. This is
listed for completeness, not recommended.

### 7.5 Is ER's depth buffer reachable at Present time?

- **Format**: `D32S8` typeless per the 2022 article [V:web]; read through an SRV as `R32_FLOAT_X8X24_TYPELESS`.
  **Convention**: one Nexus ReShade preset tells users to set `RESHADE_DEPTH_INPUT_IS_REVERSED=1` for ER
  [V:web, weak], which suggests reversed-Z [U]. Use SkyCraft's runtime probe (16 samples; the sky is 0 if
  reversed).
- **Lifetime**:
  - The article says each frame *starts* by copying and clearing (carrying GBuffer data across frames). So the
    scene depth probably survives until the next frame's clear, and **is probably intact at Present** [U].
  - D3D12 games often alias transient resources in placed heaps [U for ER]. The robust method copies depth at a
    known point (just before the final composite, strategy 2) with `CopyResource` or `CopyTextureRegion` into
    our own `R32` texture.
  - Track its state through the `ResourceBarrier` hook.
  - Identify the resource in `CreateCommittedResource`/`CreatePlacedResource` (27/29): typeless D32S8,
    `ALLOW_DEPTH_STENCIL`, render size.
- **Resolution**: ER has a dynamic-resolution tone-map path (`ToneMapDynamicResolutionVS.vpo`,
  `Performance.IsEnableDynamicResolution` [V:bin]) and the article mentions dynamic screen-percentage for post
  buffers. Render size can therefore differ from the back buffer. Compare `GetDesc` sizes every frame and scale
  UVs [U how often it changes on PC].
- **Ray tracing** (2.x) adds RT shadow and AO composites (`RayTracing AO Composite`,
  `RayTracing DirLightShadow Composite` strings [V:bin]). Depth semantics are unchanged [U].

### 7.6 In-engine debug drawing (cheap Phase-0 visualisation)

ER's own debug drawer `CS::CSEzDraw` (RTTI vtable 0x2b814c0 [V:bin]) draws lines, triangles, capsules and
spheres in the world, depth-tested by the engine. fromsoftware-rs 2.7.1 RVAs (`Ww2710` bundle) [V:src], all with
function prologues preceded by `int3` padding on the local exe [V:bin]:

| Function | RVA |
|---|---|
| `draw_line` | 0x2652640 |
| `draw_triangle` | 0x2652390 |
| `draw_wedge` | 0x2651f40 |
| `draw_capsule` | 0x2c0ba0 |
| `draw_sphere` | 0x1d2630 |

Access: `RendMan` singleton -> `debug_ez_draw` (`rend_man.rs` [V:src]). Use it to draw the **streamed MGSV
skeleton as lines** next to the ER player before any pose override. This is the quickest test of axis, scale and
mapping. Whether `debug_ez_draw` is populated in retail builds is [U].

---

## 8. Hiding the ER player's own model

| Method | Where | Effect | Tag |
|---|---|---|---|
| **Wear the Snake armour** (option A/C) | Equip the model-1570 rows (1500000-1500300); armour `invisible_flag_*` model masks (`EQUIP_PARAM_PROTECTOR_ST.invisible_flag_sex_ver00..98`) hide body under armour | Nothing to hide: the player **is** Snake | V:src (param defs, old project assets) |
| **Per-part visibility, keeping the shadow** | `PlayerIns.chr_asm_model_ins -> CSChrAsmModelIns.parts_model_ins[27]` (array at +0x28) `-> CSModelIns+0x18 model_disp_entity -> CSModelDispEntity+0x20 disp_flags1`, bit 0 | invisible but still casts a shadow (`0x100000A0`) | V:web (erfps2 2.7.1 code and comment); offsets computed from Dasaav's fork [U runtime] |
| Body model | same `disp_flags1` bit 0 on `chr_asm_model_ins.chr_model_ins` / `ChrIns.chr_model_ins` (+0x50) | presumably the same | U |
| Model masks | `chr_asm_model_res` bytes `+0x50..+0x60` (erfps2 toggles masks 3 and 36) | hides sub-meshes (ears, hood) | V:web |
| Whole-character render flag | `ChrIns.chr_flags1c5` bit 3 `enable_render` (+0x1C5) | probably also removes the shadow | V:src (fromsoftware-rs); effect U |
| Transparency | `ChrIns.base_transparency` +0x24C (also `+0x238` opacity keyframes, `+0x240` tint alpha, `+0x248` camouflage). The old project's live probe read 1.0 at `+0x238/+0x240/+0x248/+0x24C` on this exact exe hash | fade or dither | V:bin (old project probe) + V:src |
| Do NOT use | `ChrDebugFlags.character_disabled` / `disabled_updates` | stops physics and updates of the puppet | V:src |

For **option D**, hide every part's visibility bit but **keep shadow casting**. With A's pose override still
running underneath, the ER body's shadow then matches Snake's pose, which hides D's main lighting weakness
(no ER shadow for injected geometry).

---

## 9. MGSV's HUD as a SkyCraft-style GUI layer

**Facts**:

- Courrèges' MGSV frame study lists, in order: depth prepass, velocity, GBuffer (3x `B8G8R8A8`), SSAO, GI, lights,
  shadows, **tone map early (LDR afterwards)**, emissive/transparent, SSR, particles, bloom, DoF, lens effects,
  motion blur, LUT, **FXAA**, "final touch" sprites. Depth is **32-bit float reversed**. **The HUD pass is not
  described** [V:web].
- MGSV's UI system is Fox UI (`UiGraph`, `UiModel` strings [V:bin]). Its Lua exposes HUD element toggles, e.g.
  `TppUiStatusManager.SetStatus/UnsetStatus` (0x85c450/0x85c5a0) and `TppUiCommand.Show*/Hide*` [V:bin via sibling
  `data/mgsv_lua_libs.tsv`].

**Capture technique** (D3D11 in MGSV) [U, to be confirmed by probe P0.3]:

1. Find the scene/UI boundary. Track `OMSetRenderTargets` (33): the first time the **swap-chain back buffer** is
   bound after the last full-screen post pass (FXAA / "final touch"), the UI begins. Confirm with a one-frame
   draw log (RT, PS hash, blend state, draw count).
2. **Clear the back buffer to (0,0,0,0)** at that boundary, so the scene is gone and only the UI draws.
3. **Force coverage into alpha.** For UI draws, substitute blend states (hook `OMSetBlendState` (35) and
   `CreateBlendState` (20)) so the colour equation is unchanged but alpha uses `SrcBlendAlpha=ONE,
   DestBlendAlpha=INV_SRC_ALPHA`. The result is premultiplied UI with real coverage, which is what SkyCraft's
   overlay shader expects. Additive glows keep alpha low, which is correct for an additive overlay.
4. At MGSV's Present: `CopyResource` to a staging texture (3 in flight), `Map`, then memcpy into the
   **file-backed** shared triple buffer (SkyCraft's `OverlayCtl` plus 3 slots, unchanged).
5. ER side: section 7.2 Present overlay, skipped while ER menus are open (SkyCraft `skyrimMenuOpen` rule).

**Complications** [U]:

- World-anchored HUD (enemy markers, Reflex "!", Fulton or target tags, iDroid world markers) is projected with
  **MGSV's camera**. It lands correctly only if MGSV's camera follows ER's camera (Lua `Player.SetAroundCamera*`
  or a native hook), or those markers get suppressed through `TppUiStatusManager`.
- UI that samples the scene (blurred iDroid backdrop, distortion) shows black behind it.
- **Resolution.** For sharp text MGSV must render at ER's resolution (or the HUD scale ratio). To avoid paying
  for MGSV's 3D scene, the MGSV hook can **skip scene draw calls** (keep CB uploads for pose capture) once the
  UI boundary detector works. Risk: Fox logic depending on GPU queries or readbacks (occlusion, sun visibility)
  could change [U].
- **Window and focus.** MGSV must keep rendering while hidden. Minimised or unfocused behaviour under Proton
  needs testing. The old project's Alt-Tab freeze involved the Steam overlay Vulkan layer
  (`prior-work.md`).

---

## 10. Transport between the two prefixes

- **Test this session** [V:test]: `shmtest/shm.cpp` (mingw-w64, static).
  - The writer in scratch prefix A opens `Z:\dev\shm\phantomring_shmtest` with
    `CreateFileW(..., OPEN_ALWAYS)`, then `CreateFileMappingW(file, PAGE_READWRITE, 64 MiB)` and `MapViewOfFile`.
    It writes a 1080p RGBA8 frame plus a sequence number 300 times.
  - The reader in **a different prefix B** (separate wineserver) saw **all 301 distinct sequence values** and the
    matching frame byte. The wine was system `wine-11.0`; the prefixes were deleted afterwards.
  - This confirms the sibling's finding (named sections fail across prefixes; file-backed sections work).
  - (The memcpy throughput number the test printed is not meaningful: the copy loop was probably optimised
    away.)
- **Bandwidth per option** (60 fps):

  | Payload | Per frame | Per second |
  |---|---|---|
  | A: pose (53 bones) | about 1.5 KB | 90 KB |
  | D: full palette (244 x 48 B) plus a visibility mask | about 12 KB | 0.7 MB |
  | GUI layer, 1440p RGBA8 | 14.7 MB | 0.9 GB |
  | GUI layer, 4K | 33 MB | 2 GB |
  | B: colour plus depth, 1440p | about 29 MB | 1.8 GB |

  For the GUI layer, consider dirty-rectangle or "changed since last frame" skipping (HUD changes rarely) [U].
- GPU-to-GPU sharing (DXVK D3D11 -> vkd3d-proton D3D12) is unavailable across prefixes. Even inside one prefix it
  relies on Proton-only shared-resource patches [V:web + U]. Plan for the CPU path.

---

## 11. Option D (optional fidelity upgrade): SkyCraft-literal geometry passthrough

- **Data**:
  - Snake's original FMDL meshes and textures are **already extracted** by the old project. FMDLs are under
    `staging/mgsv-assets/packages/.../chara/sna/Scenes/`; 60 full-resolution DDS are recorded in
    `reports/venom-source-detail-audit.json` [V:src].
  - Preconvert them offline to a simple vertex format and pack them with the plugin. Only **per-frame palettes
    plus a "which meshes/props are visible" mask** cross the link: the section 4.6 capture, extended to every
    Snake-attached draw (weapons, box, Fulton).
  - This is SkyCraft's `kRenAvatar` with the static part cached up front (as SkyCraft's `kRenTexture` and atlas
    are sent once).
- **Drawing**:
  - First version: strategy 2 (queue-level list before the final composite). Draw Snake into `HDRScene`,
    depth-tested against a copy of ER depth, lit by an approximation of ER's sun and ambient. ER's GParam values
    would have to be read; `CSWorldSceneDrawParamManager` only exposes GParam IDs and lerpers [V:src
    `world_scene_draw_param_manager.rs`].
  - Keep the hidden ER body casting the shadow (section 8).
  - Full version: GBuffer injection (strategy 4).
- **Pros**: exact MGSV look, face and hair, every MGSV prop for free; no asset conversion per item.
- **Cons**:
  - The largest RE surface: ER pass identification, resource states, GBuffer encodings, HDR/LDR position, TAA
    motion vectors (else ghosting).
  - Snake falls outside ER's post chain unless inserted before it.
  - Highest maintenance on ER patches.
- **Effort**: about 6-10 weeks after A's MGSV capture exists.

---

## 12. Phased plan

| Phase | Build | Done when | Depends on |
|---|---|---|---|
| **P0 probes** (about 1 week, no gameplay) | **P0.1 ER frame probe** (me3 native, log-only): DXGI factory, queue, Present and ECL hooks; shader-name hook at 0xb7bfc0 (+ siblings); `CreateGraphicsPipelineState`, `SetPipelineState`, `OMSetRenderTargets`, `ResourceBarrier`, resource creation. Dumps one frame's timeline: which ECL batch and list holds `ToneMap_*`, the swap-chain-RTV composite, the depth resource (format, size, state at Present), HDRScene/UIScene candidates, swap-chain format and colour space. | Timeline file explains passes 1-14 of section 7.3 on 2.7.1 | none |
| | **P0.2 ER skeleton probe**: from `WorldChrMan` (0x3d69ff8 [V:bin sibling]) main player, walk both candidate chains (SkeletonMan 2023 vs Dasaav fork `behavior+0x60 -> +0x30`) and `ChrIns.hka_pose_importer` (check vtable == base+0x2b70360). Read hkaSkeleton bone names and compare with the 150 names of section 4.2. Then a **write test**: rotate `L_UpperArm` 45 degrees at `TtpEndFrame` (A1) and separately through the importer (A2); screenshot. | Armour arm visibly rotates; one method chosen | P0.1 not required |
| | **P0.3 MGSV D3D11 probe** (inside MGSV's prefix; inject without touching the install dir; IHHook-style Present hook): log draws with VB/IB sizes, strides, VS CB sizes, RT/DSV formats, blend states; find Snake's draws by FMDL sizes; dump one frame of candidate palettes; find the scene/UI boundary. | Palette convention known; bone-length self-test passes; UI boundary found | MGSV prefix populated (first launch is done by the user) |
| | **P0.4 Debug skeleton**: stream the MGSV skeleton (from P0.3) over shared memory and draw it with CSEzDraw next to the ER player | Lines track Snake's motion in ER's world | P0.2 (player position), P0.3 |
| **P1 baseline (C)** (days) | ER player wears the v12 Snake armour; position and yaw driven by the movement link; ER's own animation | Walk around ER as (ER-animated) Snake | movement research |
| **P1b GUI layer** (1.5-3 weeks) | MGSV UI-only capture (clear, alpha-coverage blend states), staging readback, file-backed triple buffer, ER Present overlay (hudhook-like native D3D12, SDR first, then HDR encode); hide ER HUD parts; skip ER overlay while ER menus are open | MGSV HUD (ammo, reticle, iDroid) over ER, no scene leaking | P0.1, P0.3 |
| **P2 pose passthrough (A)** (2-3 weeks) | Palette capture -> character-space bone transforms -> retarget (section 4.5) -> A1/A2 write; neutral ER behaviour state; staleness fallback to C; Spine1 split; pelvis height | Snake's MGSV animations (crouch, prone, CQC, aim) play on the ER Snake in sync | P0.2, P0.3, P1 |
| **P3 polish** | Foot IK on ER ground, weapon grip offsets, head/aim look, finger poses, latency tuning (extrapolate by MGSV velocity if needed), MGSV camera follows ER camera so world-anchored HUD markers line up, HDR overlay | Side-by-side check vs MGSV picture-in-picture (debug B) looks right | P2 |
| **P4 optional (D)** | Original-mesh renderer in ER (queue insertion before the final composite, depth copy, approximate lighting, hidden ER body keeps the shadow); later GBuffer injection | Exact MGSV Snake with face and hair in ER | P0.1 timeline, P2 capture |

---

## 13. RE checklist (concrete)

**ER (2.7.1.0)**:

1. Pointer chain `PlayerIns -> ... -> hkbCharacter -> pose buffer` (local hkQsTransform array) on 2.7.1.
   Candidates: SkeletonMan (`+0x190,+0x28,+0x10,+0x30`; then `+0x38 -> +0x0`, `+0x54` offset) vs Dasaav fork
   (`+0x190,+0x28,+0x60,+0x30`).
2. `ChrIns.hka_pose_importer` offset (fromsoftware-rs field order gives about +0x398 [U]). Check its vtable ==
   0x2b70360. Find the embedded hkaPose (+0x48).
3. Call order within a frame: behavior graph -> foot/hand IK scene modifiers -> `TtpEndFrame` -> importer
   export -> render. Set breakpoints or log with a counter.
4. Neutral behavior state for the puppet. Owned by movement/combat research; needed so TAE events do not fire.
5. Render: the P0.1 timeline (UI composite list, depth resource and state, HDR format, dynamic resolution).
6. GParam light values (only for D).

**MGSV (1.0.15.4)**:

1. How D3D11 is loaded and created (`D3D11CreateDevice` + `CreateDXGIFactory` strings [V:bin]) and the hook
   entry. Use a dinput8 proxy or remote injection in MGSV's own prefix, without modifying the install directory.
2. Snake draw identification (VB and IB sizes from FMDL strides) and the palette CB layout. Deferred contexts?
3. Matrix space (world vs camera-relative) and the view CB.
4. Scene/UI boundary; UI blend states; world-marker projection.
5. Exact camera override (Lua `Player.SetAroundCameraManualMode*` vs a native hook), needed for HUD markers
   (and for B debug only).
6. Long term: Fox animation output in memory, to remove the "must be drawn" dependency.

---

## 14. Risks

| Risk | Affects | Mitigation |
|---|---|---|
| Fox skinning not where expected (structured buffer, compute skinning, deferred contexts) | A, D | P0.3 logs every VS resource bound for Snake draws; fall back to Ghidra RE of the animation output |
| ER overwrites the pose after our write (IK, importer resync) | A | A2 (importer, post-IK); write model and local both; P0.2 decides |
| ER layout drift on patches (2.7.1 now) | A, D | AOB and RTTI-based lookups (vtables by RTTI name like SkeletonMan's `RTTIScanner`); exe-hash gate like the old project |
| Foot sliding or floating | A | ER-side foot IK; collision mirroring |
| Converted armour quality (neck, proportions) | A, C | Known from old project; D as fidelity escape hatch |
| D3D12 mid-frame state and resource-state errors (device removed) | D, B | Queue-level insertion only; barrier tracking; vkd3d-proton validation (`VKD3D_CONFIG=...`) during development [U] |
| HDR swap chain | GUI layer | Encode per format; SDR first |
| MGSV stops rendering when hidden or unfocused | all (pose capture needs draws) | Test early; keep the window "focused" as SkyCraft does with Minecraft; memory-based pose capture later |
| Two games' GPU and RAM | all | MGSV skips scene draws (keep CB uploads); lowest MGSV settings; this needs MGSV's config file, which belongs to the user and is not edited by research agents |
| Steam overlay Vulkan layer and Alt-Tab freeze (seen in old project) | all | A/B with the overlay disabled (`prior-work.md`) |

---

## 15. Sources

**Local**:

- SkyCraft (`<SkyCraft>`):
  - `README.md`, `docs/DESIGN.md`, `protocol/skycraft_protocol.h`
  - `skse/src/{Overlay.cpp, WorldRender.cpp, Game.cpp, Link.cpp}`
  - `fabric/src/client/java/dev/skycraft/client/FrameExporter.java`, `.../render/AvatarExporter.java`
- Old project (`<old-project>`):
  - `docs/{retarget-diagnosis-v4, er-native-pose-references, venom-source-detail-audit, elden-ring-visual-v3,
    quiet-render-runtime-probe, ddog-original-rig-proof, er-character-physics-compatibility}.md`
  - `scripts/build_er_retarget.py`
  - `reports/{snake-retarget-material-v3.json, er-native-pose-v4-c0000.json}`
  - `mods/elden-ring-v12/parts/`
  - `tools/fmdl-reference/Fmdl.cs`, `tools/fromsoftware-rs-command-reference`, `tools/elden-ring-ct-src` (v1.140),
    `tools/me3-source`
- Sibling reports: `docs/research/{skycraft-host-side, prior-work}.md`, `docs/research/data/mgsv_lua_libs.tsv`.
- Game bytes: `eldenring.exe` 2.7.1.0, `mgsvtpp.exe` 1.0.15.4, Proton Experimental vkd3d-proton/dxvk files, Snake
  FMDLs.

**Web (read this session)**:

- [ERSkeletonMan (Dasaav-dsv)](https://github.com/Dasaav-dsv/ERSkeletonMan): `skeleton/SkeletonMan.h`,
  `skeleton/HkSkeleton.h`, `modifiers/*`.
- [erfps2 (Dasaav-dsv)](https://github.com/Dasaav-dsv/erfps2): `src/shaders.rs`, `src/player.rs`, `src/rva/ww.rs`,
  `binary-mapper/profile.toml`; fork [Dasaav-dsv/fromsoftware-rs @79266c97](https://github.com/Dasaav-dsv/fromsoftware-rs)
  (`behavior.rs`, `chr_ins.rs`).
- [hudhook 0.9.3](https://github.com/veeenu/hudhook) `src/hooks/dx12.rs`;
  [eldenring-practice-tool](https://github.com/veeenu/eldenring-practice-tool) `Cargo.toml`, README.
- [DirectXHook (techiew)](https://github.com/techiew/DirectXHook) `src/DirectXHook.cpp`, `src/Renderer.cpp`.
- [IHHook (TinManTex)](https://github.com/TinManTex/IHHook) file tree, `Changelog.txt`,
  `IHHook/D3D11Hook.cpp`, `Hooks_Character.h`.
- [runtimesouls (thefifthmatt)](https://github.com/thefifthmatt/runtimesouls) file listing.
- [mamoniem: Behind the Pretty Frames: Elden Ring](https://mamoniem.com/behind-the-pretty-frames-elden-ring/).
- [Adrian Courrèges: Metal Gear Solid V graphics study](https://www.adriancourreges.com/blog/2017/12/15/mgs-v-graphics-study/).
- [DXVK 1.10.1 shared resources (Phoronix)](https://www.phoronix.com/news/DXVK-1.10.1-Released),
  [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton).
- [Nexus ER ReShade preset (reversed depth note)](https://www.nexusmods.com/eldenring/mods/4420).
