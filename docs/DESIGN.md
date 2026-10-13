# PhantomRing — Design Doc

> Play Elden Ring as Venom Snake: Metal Gear Solid V runs hidden in the background and supplies
> Snake himself (movement, stances, CQC, guns, animation, life, HUD, buddies), while Elden Ring runs
> its world, enemies, bosses, quests and saves and draws everything.

Status: draft v0.1 · 2026-10-11. Modeled on [SkyCraft](https://github.com/chasmlol/SkyCraft) ("Minecraft player in Skyrim").
Research behind every decision is in `docs/research/` (each claim there is tagged verified/unverified).

---

## 1. Core principle

**Neither game is rewritten.** MGSV runs its own, unmodified player logic: Fox Engine's locomotion,
stances (stand/crouch/prone), dive, CQC, weapon handling, aiming, reloads, recoil, life/regeneration,
buddy AI, and its own skeletal animation. Elden Ring runs its world: terrain and collision, enemies,
AI, bosses, NPCs, quests, graces, saves.

The two plugins only **translate**:

- MGSV tells Elden Ring *how Snake moves* (intended motion), *what he looks like this frame* (his real
  meshes, textures and skinning matrices), *where his camera is*, *what he fired at*, and *his life*.
- Elden Ring tells MGSV *what the player pressed*, *what hit Snake*, and *what is around him*.

If we ever re-implement an MGSV mechanic in Elden Ring (or the reverse), the design has gone wrong. The
exceptions are explicit, temporary, and listed in §12.

## 2. Target environment

| Thing | Value | Notes |
|---|---|---|
| Elden Ring | 1.17.1 (exe 2.7.1.0, sha256 `1a354710…8b597891`) | Arxan-protected; offline only; loaded through **me3 0.13** `[[natives]]` with `disable_arxan = true`, `start_online = false`, isolated save |
| MGSV | TPP 1.0.15.4 (sha256 `085c2f82…bb45`, July 2026 patch) | Denuvo wrapper, game code plain; fixed base 0x140000000; `dinput8.dll` proxy (same as IHHook/tpp-mod/MGS5VR) |
| Platform | Linux, Steam + Proton Experimental (11.0), each game in **its own prefix** | MGSV runs inside **headless gamescope** (invisible, believes it has focus) |
| Toolchain | x86_64-w64-mingw32-g++ 16 (C++23), CMake/Ninja, MinHook 1.3.4 | Static runtime, no MSVC needed |
| RE | Ghidra 12.3 projects for both exes (local, not included), MGS5VR (MIT) as the main MGSV reference | tpp-mod is GPL: reference only, never copy |

## 3. Components

```
┌────────────── eldenring.exe (Proton prefix 1245620) ───────────────┐     ┌──────── mgsvtpp.exe (prefix 287700, headless gamescope) ────────┐
│ PhantomRingHost.dll (me3 native)                                    │     │ dinput8.dll = PhantomRingGuest (proxy)                           │
│                                                                     │     │                                                                  │
│ Link        create/stamp mapping, heartbeats, session nonce ────────┼─────┼─▶ Link       attach, heartbeats                                  │
│ Tasks       per-frame callbacks via ER's own task system (no patch) │     │ LuaPump      lua_pcall depth-1 hook: runs our Lua on MGSV's thread│
│ InputBridge ER's XInput/DInput → pad slot; ER gameplay input masked ┼────▶│ PadInject    XInputGetState IAT → pad slot                        │
│ Puppet      applies Snake's intended motion to the ER player        │◀────┼─ Motion      reads Snake each frame, holds him in the sandbox,    │
│             (ER collision), yaw, hides the Tarnished                │     │              publishes odometer / root / velocity / stance        │
│ CameraDriver ER camera = MGSV camera mapped into ER world           │◀────┼─ Camera      MGSV camera setter hook (pos, quat, fov)             │
│ SnakeRender  D3D12: draws captured meshes with this frame's matrices│◀────┼─ Capture     D3D11 hooks: Snake's VB/IB/layout/textures + matrices│
│ Combat      shots/CQC → ER raycasts → ER damage; ER hits → Snake    │◀───▶┼─ Combat      fire/CQC events out; hurt events in (life)           │
│ Overlay     MGSV HUD layer + boot status                            │◀────┼─ Hud         MGSV UI pass captured to a transparent layer         │
│ Console     dev command mailbox (peek/poke/call)                    │     │ Console      dev Lua mailbox                                      │
└─────────────────────────────────────────────────────────────────────┘     └──────────────────────────────────────────────────────────────────┘
                     /dev/shm/phantomring_v1  (protocol/phantomring_protocol.h) — mapped by both + the Linux tools
```

Plus the **Linux side** (`tools/phantomring/`): the launcher (backs up saves, creates the mapping, starts
MGSV in headless gamescope through SLR+Proton, starts ER through me3), the dev consoles, and
`fake_er.py` / `fake_mgsv.py` stand-ins that let each plugin be tested without the other game
(SkyCraft's `fake_skyrim.py` idea; the mapping is a plain file, so native Python maps it directly).

## 4. Worlds and coordinate mapping

Both games are Y-up, meters. Snake never actually travels through MGSV's map: he is **held in a
sandbox** (§5.2) and only his *intended motion* crosses over. So there is no global MGSV↔ER map
alignment, only a per-frame **anchor**:

```
MGSV side: Snake's root  (Pm, yaw_m)      ER side: the puppet's feet  (Pe, yaw_e)
yaw offset  Δ = yaw_e − yaw_m   (fixed for the session; ER follows Snake's yaw so Δ stays constant)

ER_point = Pe + Ry(Δ) · (MGSV_point − Pm)          (for camera, bones, muzzles, buddy, actors↔)
MGSV_dir = Ry(−Δ) · ER_dir
```

- Snake's *intended motion* is the odometer `odo` (GuestState). The host applies
  `Ry(Δ)·(odo − odo_last)` as the puppet's requested displacement; Elden Ring's collision decides where
  the puppet really ends up (walls stop him, slopes lift him, ledges drop him).
- Elden Ring shifts its Havok origin in the open world; nothing above stores absolute ER positions
  across frames (only per-frame deltas and the current `Pe`), so world shifts are harmless.
- Axis/handedness signs between Fox and Havok are confirmed at runtime with the debug skeleton draw
  (Phase 1) and recorded here.

## 5. The player

### 5.1 Elden Ring side: the puppet

The Tarnished stays in the world as an invisible **puppet** (SkyCraft's PlayerPuppet): enemies target
it, its hitbox takes hits, triggers/fog walls/graces/item pickups see it, saves store it.

- **Control off:** ER's gameplay pad is masked (XInput/DInput hooks in system DLLs, Arxan-safe; or
  `CSInGamePad.allow_polling = false`); ER menus keep working (Start = ER menu, View = ER map).
- **Motion:** each ER frame (task `ChrIns_PrePhysics`, group 78) the host turns the odometer delta into
  the puppet's motion. Target: feed it into ER's own character proxy so Havok resolves collision
  (RE item E1, answered: PlayerCtrl vfunc 12 root motion, `[Puppet] motion = root_motion`, being
  verified live). Default until then: a host-side kinematic controller using ER's raycast
  (`cs_phys_world_cast_ray`, RVA 0xC71E00): step-up/ground snap/wall stop/gravity, applied with the
  game's own warp `ChrIns::SetPosition` (0x3F2690) and `ChrIns::SetYaw` (0x3F6180).
- **Look:** the Tarnished's own model is hidden (ChrIns `+0x1C5` bit3 / model mask), keeping shadow
  and hitbox; Snake is drawn instead (§9).
- **Health:** MGSV's life is authoritative (§8). The puppet is NoDead/NoDamage
  (data `+0x19B`); its HP bar mirrors Snake's life fraction.

### 5.2 MGSV side: the sandbox and intended motion

At boot the guest steers MGSV (Lua via the pcall pump) from the title into a free-roam mission and
parks Snake on a large flat, empty spot (Mother Base command platform deck by default; configurable),
with soldiers/staff disabled, clock and weather controllable, and **saving blocked** (the user's MGSV
progress is never written; the launcher also backs it up).

Each MGSV frame the guest reads Snake's root (MGS5VR's camera-owner root matrix / `vars.playerPos*`),
adds the frame-to-frame displacement to the odometer, then **holds him in place** (native position write
found in RE item M1; fallback: treadmill — let him walk up to a radius, then warp back to the centre,
excluding the warp from the odometer). His animation never knows: he runs, crouch-walks, crawls, dives
and rolls exactly as MGSV plays it, on flat ground.

Known limitation of the sandbox (until §12's collision injection): MGSV's ledge-vault/climb/cover need
MGSV geometry and won't trigger against Elden Ring walls; falling off an ER cliff moves the puppet (and
the drawn Snake) down while MGSV keeps its ground animation.

## 6. Input

Controller first (the user plays on a pad). Elden Ring has OS focus and owns the real devices.

- The host hooks ER's `XInputGetState` (xinput1_4, ordinal 2) and ER's DirectInput keyboard device.
  The real pad state goes to the `PadState` slot every frame; the copy returned to Elden Ring is masked
  to the allow-list below. The guest returns the slot from MGSV's own `XInputGetState` (its IAT slot
  0x14208E070, MGS5VR's proven method) and makes DirectInput enumerate no game controllers, so MGSV
  sees exactly the user's pad and nothing else.
- **Allow-list to Elden Ring** while puppeting: **Start** (ER menu: equipment, map, graces, system) and
  **View** (ER map). Everything else is MGSV's.
- **Interaction arbitration** (SkyCraft's "use" rule): when Elden Ring shows an action prompt (door,
  NPC, item, grace, lever — `CSActionButtonMan+0x20`, RE E6), a fresh press of A goes to Elden Ring
  instead (until released) and MGSV gets a neutral A. Implemented in phase 1.
- **Elden Ring menu open / loading / dead:** the guest gets a neutral pad (`kPadNeutral`).
- Keyboard & mouse: phase 5 (MGSV reads raw input, which is empty while unfocused; we'll feed its
  raw-input handler from the host's keyboard/mouse).

## 7. Camera

MGSV's camera drives Elden Ring's (as Minecraft's drives Skyrim's in SkyCraft): over-the-shoulder,
aim zoom, crouch/prone framing, CQC and dive cameras all come from MGSV.

- Guest: MGS5VR's camera setter hook (0x14044E960: quat @+0xF0, pos @+0x100) gives the per-frame
  player camera; FOV from the player camera (+0x2DC / the FOV-lerp hook).
- Host: in a task at `DrawParamUpdate` (107) it writes the mapped camera into `CSCamera.pers_cam_1`
  (matrix rows, fov, near/far). (RE item E3 confirms the renderer takes it; fallback: hook the
  `ApplyCameraMask` return with Arxan disabled.)
- MGSV's boom doesn't collide with Elden Ring geometry. The host ray-casts eye→camera in ER and pulls
  the camera in; it also publishes `cameraMaxDistance` so the guest can shorten MGSV's own boom next
  frame (aim rays stay consistent).

## 8. Combat

### 8.1 Snake hits Elden Ring

Phase 3 (first version): **Elden Ring resolves MGSV's shots.** The guest emits `kGEvFire` for every
round (muzzle, direction, weapon, ammo kind), detected from MGSV's ammo/fire state (RE item M3 finds
the native fire point). The host maps it into ER, ray-casts (`cs_phys_world_cast_ray` against
characters + world), and applies damage through ER's own pipeline (spawned bullet with an AtkParam, or a
damage SpEffect) so enemies flinch, stagger, aggro and die normally. Damage scales with the MGSV weapon's
own damage and the enemy's level (config, like SkyCraft's §13). **Tranquilizer → Elden Ring sleep
build-up**, stun → poise damage, lethal → physical damage. CQC strikes/throws emit `kGEvMelee` (arc
check in front of Snake).

Later (§12): SkyCraft's ActorProxy model — invisible MGSV soldiers mirroring nearby ER enemies so MGSV's
own ballistics, headshots, CQC grabs and buddy AI hit them.

### 8.2 Elden Ring hits Snake

The puppet is kept alive (NoDead) and its HP is reset every frame; every HP loss (or a hooked damage
vfunc, RE item E2) becomes `kHEvHurt` with amount and direction. The guest subtracts it (scaled) from
Snake's life. MGSV's life regenerates as in MGSV. If Snake's life would reach zero the guest keeps him
at 1 and emits `kGEvDied`; the host kills the puppet so Elden Ring runs **YOU DIED** and the respawn at
the last grace; `kHEvRespawn` refills Snake. Fall damage is Elden Ring's.

## 9. Rendering Snake (SkyCraft-literal)

SkyCraft ships Minecraft's *posed avatar geometry* to Skyrim and draws it inside Skyrim's frame. We do
the same with MGSV's real Snake:

1. **Capture (guest, D3D11 under DXVK):** hooks on device creation and the immediate context
   (`CreateBuffer`, `CreateTexture2D`, `CreateInputLayout`, `Map/Unmap`, `UpdateSubresource`,
   `*SetShaderResources`, `VSSetConstantBuffers`, `IASet*`, `DrawIndexed*`, `OMSetRenderTargets`).
   In MGSV's G-buffer pass, any skinned draw whose matrices place it within a few meters of Snake's root
   is Snake (or his gear, or the buddy — the sandbox is empty). For each such draw: its mesh (vertex/
   index bytes + input layout, sent once on the render ring), its textures (sent once, BC-compressed as
   MGSV has them), and this frame's skinning matrices (≤32 per draw in Fox), anchor-relative.
2. **Draw (host, D3D12 under vkd3d-proton):** device/queue/swapchain via dummy-object vtable hooks
   (hudhook technique; Arxan never sees them). Stage A: at Present, depth-tested against ER's scene
   depth when it is still valid, lit with ER's sun direction/colour + ambient approximation. Stage B:
   mid-frame, into ER's HDR scene before post-processing (fog, tonemap, TAA treat Snake as native),
   using SkyCraft's habits (self-calibrating depth convention, state save/restore, fall back to
   end-of-frame when targets look wrong). Stage C: G-buffer injection so ER lights and shadows him.
3. **Hide in MGSV:** nothing to hide — MGSV renders in its own invisible gamescope. (MGS5VR's
   normal-only group switches exist if we ever want MGSV to skip drawing Snake's shadows to save GPU.)

The same capture brings **D-Walker** (and later D-Horse/D-Dog/Quiet): his draws are near Snake too, tagged
`kOwnerBuddy`.

**HUD (SkyCraft's GUI layer):** MGSV's UI pass (weapon/ammo, damage/blood, marking, prompts, iDroid)
is captured as a transparent layer (MGS5VR separates the native UI jobs) and composited on top at
Present. Until then a small host-side HUD shows life, weapon and ammo from GuestState.

## 10. Buddies (D-Walker first)

MGSV runs the buddy: deployment, following, commands, riding, weapons.

- Boot: the guest selects D-Walker as the sortie buddy (`vars.buddyType`, Lua) before the sandbox
  mission loads, or calls him in via the buddy service.
- Position: D-Walker's draws come through the capture with his own matrices; mapped through the same
  anchor, he appears next to Snake in Elden Ring. His vertical is snapped to ER ground (host ray-cast),
  since he walks on MGSV's flat sandbox.
- **Riding** (the main D-Walker use): Snake mounted = `kGuestOnVehicle`; the odometer follows the
  vehicle, so the puppet moves at D-Walker's speeds (dash, hover) through ER's collision; Snake+D-Walker
  both draw at the puppet.
- Combat: D-Walker's gun → `kGEvBuddyFire`, resolved like Snake's shots (Phase 4); with ActorProxies
  later, his own AI targets ER enemies natively.
- Requires D-Walker to be developed in the user's MGSV save (checked at boot; message if not).

## 11. Protocol / IPC

`protocol/phantomring_protocol.h` (mirrored by `tools/phantomring/proto.py`): one ~203 MB file in
`/dev/shm`. Header (magic, version, session nonce, unix pids, QPC heartbeats, status text), seqlocked
latest-value slots (HostState, GuestState, PadState, PoseBlock, ActorTable), two event rings, two dev
consoles, a 96 MB render ring (meshes, textures), a triple-buffered draw list, a triple-buffered HUD.
Liveness: QPC heartbeats (both prefixes share the clock) with a 3 s timeout; the host drops back to
plain Elden Ring control the same frame the guest disappears, and resumes when it comes back.

## 12. Phased plan

Each phase ends in something runnable and is verified in the real games (the user allowed test launches).

| # | Phase | Done when |
|---|---|---|
| 0 | **Link** | Launcher starts both games; both plugins load (logs), attach to the mapping, heartbeat; guest reaches the sandbox automatically; dev consoles answer (`pr lua 'return vars.playerPosX'`, `pr er 'peek player.pos'`); fake peers pass |
| 1 | **Drive** | The controller moves Snake (MGSV) and the puppet follows through ER collision; ER camera = MGSV camera; Tarnished hidden; MGSV skeleton drawn in ER with debug lines (CSEzDraw) to confirm axes |
| 2 | **See Snake** | Captured meshes/textures/matrices drawn in ER (stage A), animated and placed exactly where the skeleton was |
| 3 | **Fight** | Shots/CQC damage ER enemies (tranq = sleep), ER hits drain Snake's life, death/respawn loop works, small HUD |
| 4 | **D-Walker** | Buddy deployed in the sandbox, drawn in ER, rideable, his gun hurts ER enemies |
| 5 | **Polish** | MGSV HUD layer; stage B/C rendering; interaction arbitration; time/weather sync; KB/M; Seamless Co-op check |
| 6 | **Native depth** | MGSV collision injection (Fox Geo queries answered with ER geometry → vault, climb, cover, falls in MGSV itself); ActorProxies for native MGSV combat/buddy AI |

## 13. Reverse-engineering backlog

| Id | Question | Approach |
|---|---|---|
| E1 | Move the ER player *through* its Havok character proxy (collision-resolved) instead of warping | Ghidra on `CSChrPhysicsModule` update / proxy integrate; runtime test of `ChrCtrl::RequestProxyPosition` (0x3C9270) into a wall |
| E2 | Which damage-module vfunc applies hits to the player (cancel + forward) | vtable 0x2A3A3E0 (player damage module), runtime logging via instance vptr swap |
| E3 | Does writing `pers_cam_1` at DrawParamUpdate reach the renderer | runtime |
| E4 | Menu-open / loading / cutscene flags | runtime probing of CSMenuMan / CSNowLoadingHelper / CSFade |
| E5 | ER scene depth + HDR target at Present / mid-frame; sun direction | D3D12 command-list hooks, PSO/shader-name probe (0xB7BFC0) |
| E6 | Action-prompt state for interaction arbitration | `CSActionButtonMan` (fromsoftware-rs) |
| M1 | Hold Snake in place without disturbing animation | MGS5VR/tpp-mod player structs; Ghidra on the player position write path; runtime |
| M2 | Boot automation: title → Continue → free roam → sandbox; block saves | Lua (TppMission, TppPlayer.Warp), `ISteamRemoteStorage::FileWrite` hook |
| M3 | Native fire point (muzzle, dir, weapon, ammo kind) and CQC hits | Ghidra; `vars.ammoInWeapons` deltas as a first approximation |
| M4 | Fox G-buffer pass identification + skinning CB layout (3x4/4x4, world/camera-relative) | runtime capture logs; MGS5VR palette notes |
| M5 | Buddy selection/call (D-Walker) and riding state | IH Lua (`vars.buddyType`, buddy service), runtime |
| M6 | Writing Snake's life (damage in) with MGSV's own reaction | `vars.playerLife` write test; Player damage functions |

## 13b. Decisions

1. **Rendering:** MGSV's own meshes/textures/skinning drawn by ER (§9). Not converted assets, not ER
   animations, not a composited MGSV frame.
2. **Health:** MGSV authoritative; ER kills the puppet when Snake "dies"; ER handles respawn.
3. **Saves:** Elden Ring uses an isolated save (`PhantomRing0000.sl2`, created from a copy of an existing
   character); MGSV never saves while PhantomRing runs.
4. **Online:** Elden Ring always offline (me3 `start_online = false`); EasyAntiCheat never touched.
5. **Damage scaling:** MGSV→ER `mgsvDamage × (4 + 0.25 × enemyLevel)`; ER→MGSV `erDamage × 4` life
   points (MGSV life is ~6000 at max upgrades). Config values, tuned in Phase 3.

## 14. Risks

| Risk | Mitigation |
|---|---|
| Denuvo/MGSV refuses to run headless or offline | MGS5VR shows input + Lua work unfocused; gamescope gives real focus; Steam stays running |
| Holding Snake in place breaks his locomotion | Treadmill fallback (warp back every N meters, exclude from odometer) |
| ER renderer injection (D3D12, GPU-driven) | Stage A at Present first; self-calibrating depth; end-of-frame fallback |
| Arxan reverts inline hooks | me3 `disable_arxan`; prefer tasks, data, vtable swaps, system-DLL hooks |
| Game patches | exact-hash gating in both plugins; signatures generated and verified by `tools/*_aob_scan.py` |
| Two games' GPU/RAM | MGSV at modest resolution in gamescope; fine on the test machine (an RTX 50-series GPU, ~61 GB RAM) |

## 14b. Windows players (phase 7: built, offline-tested under Wine, not yet run on Windows)

The plugins are plain Windows DLLs (MinGW, static runtime); only the Linux glue differs. Player guide and
what is untested: docs/WINDOWS.md. Code: `common/Platform.h` (the decisions), `er/Launcher.h`,
`er/SteamLibrary.h`, `er/GuestInstall.h`, `mgsv/WindowHidden.h`, `mgsv/WindowGuard.h`; package:
`tools/package_windows.py`.

| Piece | Linux (now) | Windows |
|---|---|---|
| Mapping | file on `/dev/shm` (`file:Z:\dev\shm\phantomring_v1`) | named mapping `name:Local\PhantomRing_v1` (common/Mapping already supports both; plugins pick by detecting Wine via `ntdll!wine_get_version`) |
| Hiding MGSV | headless gamescope (real focus inside it) | guest hides its own window and spoofs focus (`GetForegroundWindow`, `WM_ACTIVATEAPP`), forces MGSV's render-visibility check (0x14024A1D0) true so capture still renders, never minimises |
| Launch | `tools/pr launch` (SLR + Proton + me3) | the host plugin starts MGSV itself (SkyCraft `Launcher.cpp` model: `CreateProcess` with `PHANTOMRING=1`, `SteamAppId=287700`; `steam://` can't pass the environment), guest `dinput8.dll` installed in the MGSV folder by hash, never over another mod's; ER through me3 for Windows |
| Header pids | unix pids | Windows pids |

Keep everything else platform-neutral (QPC clocks, no Windows pids in logic, no `/proc`). The "same prefix"
Wine mode (MGSV started by the host inside ER's prefix) is the closest Linux stand-in for testing it.

## 14c. Seamless Co-op (planned phase 8)

- Loading: ersc.dll and PhantomRingHost.dll side by side as me3 natives (load order after ersc); Seamless
  uses its own save extension; never official online.
- **Appearance sync:** other players only receive the ER puppet. Each PhantomRing peer sends its Snake's
  per-frame draw list (mesh keys + skinning matrices, a few KB/frame; meshes/textures are *not* sent —
  every player owns MGSV and their own guest capture can supply the same assets by key) over a small
  P2P channel; receivers hide remote puppets and draw remote Snakes (and buddies) at the puppet's synced
  position. Mesh/texture keys must therefore be content hashes, not pointers (do this from phase 2 on).
- Combat: damage goes through ER's own pipeline (spawned bullets/SpEffects), which ER's netcode syncs;
  direct HP writes never do. Verify host/client enemy ownership in Seamless.
- Death/respawn follows Seamless's rules (spectate until all dead / grace respawn) — co-op mode switch.

## 15. Repo layout

```
PhantomRing/
  docs/DESIGN.md, docs/research/   design + RE references
  protocol/phantomring_protocol.h  the shared layout
  common/                          mapping, rings, log, ini, hooks, scanning (both plugins)
  er/                              PhantomRingHost.dll (me3 native)
  mgsv/                            dinput8.dll guest proxy
  tools/phantomring/               launcher, proto.py, consoles, fake peers
  tools/                           AOB scanners, Ghidra scripts
  extern/minhook                   MinHook 1.3.4
```
