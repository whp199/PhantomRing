# SkyCraft host side (Skyrim SKSE plugin): deep read, and the Elden Ring analog

Research report for **PhantomRing** (MGSV guest -> Elden Ring host).
Author: research subagent, 2026-10-11.
Subject: `<SkyCraft>/skse/src` (SkyCraft v0.1.2, protocol `kVersion = 11`).

## 0. How to read this report

Every claim carries a tag:

| Tag | Meaning |
|---|---|
| **[V:src]** | VERIFIED: read in local source in this session (SkyCraft, fromsoftware-rs, The Grand Archives CT source, the old MGS project). For SkyCraft this means "the code does exactly this"; in-game *effects* that only the SkyCraft authors' comments assert are marked "(author comment)". |
| **[V:bin]** | VERIFIED: checked against local game bytes in this session (`eldenring.exe` 2.7.1.0 or `mgsvtpp.exe`). |
| **[V:test]** | VERIFIED: a test program run in this session (system wine-11.0, scratch prefixes). |
| **[U]** | UNVERIFIED: from memory, inference or design reasoning; needs checking. |

Files read in full: `main.cpp`, `Link.h`, `Link.cpp`, `Game.h`, `Game.cpp`, `Launcher.cpp`, `Input.cpp`,
`Overlay.cpp`, `Combat.cpp`, `CrashLog.cpp`, `Perf.h`, `PCH.h`, `Collision.h`, `protocol/skycraft_protocol.h`,
`README.md`, `docs/DESIGN.md`, `skse/CMakeLists.txt`, `skse/SkyCraft.ini`.
Skimmed: `Collision.cpp` (about 60% read: scheduler, Havok walk, worker, voxelizer, triangles), `WorldRender.cpp`
(structure, hooks, in-frame injection, state backup, passes, about 30% of 3372 lines), the guest's
`fabric/.../link/SkyLink.java` and `fabric/.../client/SkyClient.java` (only where the host depends on guest behaviour).
Not covered here: `Dig*.cpp`, `NpcBlocks.cpp`, `PathAvoid.cpp`, `BlockLights.cpp`, `Clip.h` (Minecraft-specific
features with no MGSV equivalent), apart from their hook list in section 11.

Section 13 lists the Elden Ring facts checked against `eldenring.exe` in this session. Section 14 has the
summary table of analogs and the main difficulties.

---

## 1. The big picture: what the host plugin is

SkyCraft is built so that **neither game is rewritten** [V:src DESIGN.md §1]. The guest (Minecraft) owns the
player's movement physics, health, inventory, HUD and combat maths. The host (Skyrim) owns the world, NPCs, AI,
quests and saves, and draws everything. The SKSE plugin `SkyCraft.dll` (CommonLibSSE-NG, C++23, Address Library,
AE runtime only, tested on 1.7.104) does five jobs [V:src]:

1. **Link**: creates and owns a 191 MB shared-memory mapping and implements the host end of every channel.
2. **Puppet + camera** (`Game.cpp`): moves the real `PlayerCharacter` to Minecraft's interpolated position every
   frame and drives Skyrim's camera from Minecraft's eye and look.
3. **Input** (`Input.cpp`): takes raw keyboard and mouse from Skyrim's input manager, keeps them from Skyrim's player
   controls, and forwards them to Minecraft, except for a small allow-list of Skyrim keys.
4. **Combat** (`Combat.cpp`): publishes nearby actors, applies Minecraft's hits through Skyrim's own hit pipeline,
   refunds Skyrim damage on the player and forwards it to Minecraft, and kills the Skyrim player when the Minecraft
   player dies.
5. **Drawing**: the Minecraft HUD/GUI as a full-screen overlay at `Present` (`Overlay.cpp`), and Minecraft's world
   geometry (blocks, entities, the third-person player body) inside Skyrim's HDR scene before post-processing
   (`WorldRender.cpp`).

Plus: the **launcher** (starts the guest hidden at plugin load), **collision export** (Skyrim's Havok world to
Minecraft as 1/8-block voxels and exact triangles), crash logging and perf counters.

### 1.1 Threads and the per-frame order

| Thread | Entry point | What runs |
|---|---|---|
| Skyrim main thread | `PlayerCharacter` vtable slot `0xAD` (Update) hook, `Game.cpp:1035`, `Install` at `Game.cpp:1253` [V:src] | `Game::PerFrame`: heartbeat, McState read, reconnect/epoch handling, teleport logic, mouse-look integration, takeover detection, puppet application, camera, `Combat::PerFrame`, `WorldRender::UpdateRagdoll`, `NpcBlocks::PushActorsOut`, SkyState write, collision harvest, water grid, block lights, dig meshes, status notes, `Perf::Report` |
| Main thread (input dispatch) | `BSInputDeviceManager` event sink plus `MenuControls`/`PlayerControls` vtable slot 1 hooks (`Input.cpp:253-263`) [V:src] | routing of every key and mouse event |
| Main thread (camera) | every `call PlayerCamera::Update` site (found by scanning `.text`, `Game.cpp:1257-1278`) [V:src] | re-pin the camera to Minecraft's eye after Skyrim's camera update |
| Render (inside the frame) | call site `REL::ID(36559)+0x85E` -> `Main::RenderWorld` (`REL::ID(107142)`), `WorldRender.cpp:3291` [V:src] | `DrawInFrame`: blocks and entities into Skyrim's HDR scene and depth |
| Render (shadows) | `BSShadowDirectionalLight` vtable slot `0x0A` (`WorldRender.cpp:3296`) [V:src] | copy the sun's shadow cascades |
| Present | `IDXGISwapChain` vtable index 8, patched in place (`Overlay.cpp:393-408`) [V:src] | heartbeat (also while paused), camera diagnostics, `WorldRender::Draw` (drain render ring, readbacks, fallback drawing), `DrawOverlay`, debug capture |
| Collision worker | detached `std::thread` (`Collision.cpp:235-247`) [V:src] | voxelizes harvested shapes and writes the collision ring |
| Launcher | detached threads (`Launcher.cpp:273-283`, `304-319`) [V:src] | unpacking, starting Prism, waiting for a previous Minecraft to exit |
| Havok threads | `DigPhysics.cpp` hooks (contact collectors) [V:src] | dig features only (out of scope) |

The diagnostic frame-order log uses the letters P (player update), H (camera hook), D (in-frame drawing) and X
(Present) (`Game.h:57-61`, `Game.cpp:1186-1197`) [V:src]. A comment notes that Skyrim's camera update runs
**before** `PlayerCharacter::Update` in the frame. That is why the camera is placed both in `PerFrame` and in the
camera hook (`Game.cpp:886-891`) [V:src].

### 1.2 The guest's side of the contract (needed to understand the host)

- Minecraft opens the mapping (`OpenFileMappingW`, retried once a second), writes `mcHeartbeatMs` from its render
  thread in `SkyLink.poll()`, and treats Skyrim as gone after **8000 ms** without a Skyrim heartbeat ("Skyrim's
  loading screens can stall its heartbeat for several seconds") [V:src SkyLink.java:21-23, 95-104].
- When `Header.skyrimPid` changes, Minecraft resets its overlay back slot to 1 and bumps a `generation` so
  everything it caches is sent again [V:src SkyLink.java:95-104].
- It holds a named mutex `Local\SkyCraft_v1_minecraft` for its whole lifetime so the host can detect it
  (`SkyLink.announceRunning`) [V:src SkyLink.java:59-74].
- It **paces its own frames on the host**: after each frame it spins (up to 25 ms) until `SkyState.seq` moves on,
  and stops waiting while Skyrim is stalled (paused or alt-tabbed) [V:src SkyClient.java:402-422].
- Started with `-Dskycraft.startHidden=true` it hides its window and mutes music from the first frame
  [V:src SkyClient.java:20-25, 74-80]. With the link up it sets `pauseOnLostFocus = false`, turns vsync off and caps
  at 260 fps [V:src SkyClient.java:424-430].

---

## 2. `main.cpp`: bootstrap

Responsibilities [V:src]:

- `SKSEPluginLoad`: `SKSE::Init(a_skse, { .trampoline = true, .trampolineSize = 1024 })`, then the log file
  `SkyCraft.log` in SKSE's log directory (spdlog, flush on info, pattern `[%H:%M:%S.%e] [%l] %v`),
  `CrashLog::Install()`, registers the messaging listener, and **starts Minecraft right away**
  (`Launcher::StartMinecraft()`), because "Minecraft takes about as long to start as Skyrim does to reach its menu".
- `kDataLoaded`: `Link::Get().Create()`; if that fails the plugin logs "SkyCraft disabled" and installs nothing.
  Otherwise, in order: `Game::Install`, `Input::Install`, `Overlay::Install`, `WorldRender::Install`,
  `PathAvoid::Install`, `Dig::Install`, then `CrashLog::Install` again (in case something replaced the filter).
- `kPostLoadGame` / `kNewGame`: `Game::OnGameLoaded()` sets `teleportPending = true`, `haveLastSet = false` and
  `lookInitialized = false` (`Game.cpp:1294-1299`).

Build [V:src CMakeLists.txt]: CMake 3.25, `add_commonlibsse_plugin(... USE_ADDRESS_LIBRARY)`, links
`d3d11 d3dcompiler dxgi ole32 oleaut32`, MSVC `/W4 /permissive- /utf-8`. Config file:
`Data/SKSE/Plugins/SkyCraft.ini` with `[Minecraft] bStartWithSkyrim, sLauncher, sArguments` and
`[Debug] bDiagnostics` (`skse/SkyCraft.ini`).

### Analog for MGSV -> Elden Ring

- **Loader.** ER has no SKSE. The working loader on this machine is **portable Mod Engine 3 (me3)**, used by the old
  project at `<old-project>/tools/me3` to launch ER offline under Proton with native DLLs [V:src task
  context; me3 dir present]. `eldenring.exe` imports `DINPUT8.dll` [V:bin `objdump -p`], so a `dinput8.dll` proxy
  would also load, but that means putting a file in the game folder, which this project's rules forbid. Use me3.
- **Init timing.** There is no `kDataLoaded`. The old project's DLLs use `DllMain -> CreateThread(Initialize)`
  (`<old-project>/scripts/native/mgs_companion_support.cpp:189-197`) [V:src]. fromsoftware-rs resolves
  singletons from DLRF reflection and waits for system init before using them
  (`crates/shared/src/static.rs:82-101`) [V:src]. PhantomRing needs a "wait until `WorldChrMan`/`CSTaskGroup` exist"
  loop, then creates the link and registers tasks.
- **Per-frame entry points.** Skyrim's `PlayerCharacter::Update` vtable hook maps to **FD4 tasks**. fromsoftware-rs
  registers closures as native tasks through `register_task` (RVA `0xeb3e50` on 2.7.1.0: function prologue
  `48 89 5c 24 08 57 48 83 ec 40` at that RVA [V:bin]; `0xeb3de0` on 2.7.0.0 is mid-function on this exe [V:bin],
  so the 2.7.1.0 override in `crates/eldenring/src/rva.rs:36-44` is the right one). The task groups available
  (`crates/eldenring/src/cs/task.rs:276-444`) [V:src] include `FrameBegin`, `PadStep`,
  `WorldChrMan_PrePhysics`, `ChrIns_PostPhysics`, `WorldChrMan_PostPhysics`, `CameraStep`, `DrawParamUpdate`,
  `MenuMan`, `ScaleformStep`, `Draw_Pre`, `DrawStep`, `GameSceneDraw`, `Flip`, `FrameEnd`,
  `BeginShiftWorldPosition` and `EndShiftWorldPosition`. Proposed mapping:
  `PerFrame` -> a task in `WorldChrMan_PostPhysics` (or `ChrIns_PostPhysics`); the camera re-pin -> a task right
  after `CameraStep` (`DrawParamUpdate`); input routing -> `PadStep`; heartbeat -> `FrameBegin`. [U: which groups
  run during loading screens and menus has to be measured.]
- **Arxan.** `eldenring.exe` is Arxan-protected. fromsoftware-rs's `arxan.rs` advises "Prefer using the task
  runtime over hooking the memory image" and ships a code-restoration neutralizer. A regex equivalent of its
  `CODE_RESTORATION_PATTERN` matches **181** times in the on-disk 2.7.1.0 exe [V:bin; my regex approximates
  pelite's `[0-128]` skip, so the count is approximate]. The design rule: **tasks first, vtable hooks second,
  inline `.text` patches last.**
- **Main difficulty:** no Address Library equivalent. Every address needs an AOB, a fromsoftware-rs RVA pinned to
  2.7.1.0, or an RTTI/DLRF lookup, plus an exe version/hash gate like the old project's `HashExecutable()`
  (`mgs_companion_support.cpp:22-45`) [V:src].

---

## 3. `Link.cpp/.h` and `protocol/skycraft_protocol.h`: shared memory

### 3.1 Creation and ownership [V:src Link.cpp:67-114]

- Skyrim **creates** `Local\SkyCraft_v1`: `CreateFileMappingW(INVALID_HANDLE_VALUE, sa, PAGE_READWRITE, size)` and
  maps it all (`FILE_MAP_ALL_ACCESS`). Minecraft only opens it.
- Security descriptor (`SharedWithThisUser`, `Link.cpp:34-58`): SDDL `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;<user SID>)`
  plus `S:(ML;;NW;;;ME)` (medium-integrity label). This exists because an elevated Skyrim would otherwise produce
  an administrators-only mapping that a non-elevated Minecraft cannot open (author comment).
- **Stale-mapping reset**: Minecraft may still hold the mapping from an earlier Skyrim run, so on create Skyrim
  zeroes every region it owns: SkyState, OverlayCtl plus slot headers (0x100 bytes from 0x300), the input ring
  header, collision ring header, actor table, event ring header, world entities and render ring header. Then it
  writes `version`, `skyrimPid` and `skyrimHeartbeatMs`, and **stores `magic` last with release ordering**
  (`Link.cpp:96-110`).

### 3.2 Layout (protocol v11) [V:src skycraft_protocol.h; sizes computed in this session]

| Offset | Region | Dir | Sync | Size / notes |
|---|---|---|---|---|
| `0x0` | `Header {magic 'SKYC', version, skyrimPid, mcPid, skyrimHeartbeatMs, mcHeartbeatMs}` | both | atomics | 0x20 |
| `0x100` | `SkyState` | S->M | seqlock | 0x40: flags (InGame/MenuOpen/Loading), worldId, collisionEpoch, pos (MC coords), yaw/pitch, teleportSeq, viewport W/H, gameHour |
| `0x200` | `McState` | M->S | seqlock | 0xC8: flags (InWorld, ScreenOpen, OnGround, Sneaking, Sprinting, Dead, Swimming, Flying), interpolated feet, yaw/pitch, eyeHeight, sensitivity, teleportAck, guiScale, frameCounter, fovDeg, bob phase/amount, eye xyz, **raw tick data** (tickQpc, prev/cur feet, eye-height pair, walkDist pair, bob pair, tickMs), cameraMode, cameraDistance |
| `0x300` | `OverlayCtl {state, framesPublished}` | both | atomic xchg | triple-buffer control |
| `0x340` | 3 x `OverlaySlotHdr {w, h, flags(bit0 bottom-up), frameId}` | M->S | via triple buffer | 0x40 each |
| `0x400` | `WaterGrid` | S->M | seqlock | 16x16 water-surface heights |
| `0x1000` | input ring | S->M | SPSC, 4096 x 16 B `InputEvent {u16 type, u16 code, i32 a, b, c}` | head at +0x00, tail at +0x40, data at +0x80; ends at 0x11080 |
| `0x12000` | `ActorTable` | S->M | seqlock | 0x40 header + 256 x 64 B `ActorRecord` |
| `0x17000` | event ring | M->S | SPSC, 512 x 32 B `McEvent {type, formId, a, b, c, d, flags, weapon}` | |
| `0x1C000` | `WorldEntities` | M->S | seqlock | 160 x 96 B (arrows, items, cracks, shadows) + block-selection box |
| `0x20000` | collision byte ring | S->M | SPSC byte ring | 32 MB |
| `0x2020000` | overlay pixels | M->S | triple buffer | 3 slots x 3840x2160x4 = 33,177,600 B each |
| `0x7F0C000` | render byte ring | M->S | SPSC byte ring | 64 MB |
| total | | | | **200,327,168 B (191 MB)** |

Coordinates in the protocol are always Minecraft space (blocks, Y up, Z south). 1 block = 70 Skyrim units.
`SkyToMc(p) = (x/70, z/70, -y/70)`, and heading h (radians, 0 = north, clockwise) maps to MC yaw `h*57.29578 + 180`
(`Link.h:62-80`) [V:src].

### 3.3 Sync primitives [V:src Link.cpp]

- **Seqlock writer** (`WriteSkyState`, `WriteWaterGrid`, `WriteActors`): `seq = s+1` (relaxed), release fence,
  `memcpy` of everything after the `seq` field, `seq = s+2` (release). **Reader** (`ReadMcState`: 64 attempts;
  `ReadWorldEntities`: 16): wait while `seq` is odd (`_mm_pause`), copy, acquire fence, retry if `seq` changed.
  `SkyState.seq` going up by 2 per Skyrim frame is also the guest's **frame-pacing clock** (section 1.2).
- **Fixed-entry SPSC rings**: head and tail on separate cache lines (+0x00 / +0x40). `PushInput` **drops** the event
  when the ring is full. `PopEvent` **skips ahead** if the producer lapped it (`head - tail > entries`).
- **Byte rings** (collision, render): every message is 8-byte aligned and starts with `{u32 type, u32 payloadBytes}`.
  A message that would cross the end writes a `kColPad`/`kRenPad` marker meaning "continue at offset 0". A message
  may not exceed half the ring. A full ring makes `WriteCollision` return false; the worker retries 2000 x 1 ms and
  then drops the message with a warning (`Collision.cpp:690-700`). `DrainRender` stops after `a_maxBytes`, which is
  48 MB per Present (`WorldRender.cpp:1122`).
- **Overlay triple buffer**: `state` bits 0-1 hold the "middle" slot, bit 2 is "dirty". The writer renders into its
  private back slot, then `xchg(state, back | dirty)` and takes the returned index as its new back slot. The reader
  (`AcquireOverlayFrame`, `Link.cpp:331-343`) does `xchg(state, front)` only when the dirty bit is set and keeps the
  returned index as its new front. After a reset Skyrim's front is 2 and the middle is 0; Minecraft's back starts at
  1 (`ResetOverlay`; Java `overlayBack = 1`). No locks and no fences: the reader always gets the newest complete
  frame, and the writer never blocks.
- **Heartbeats**: `skyrimHeartbeatMs = GetTickCount64()` is written in `PerFrame` **and** in the Present hook,
  because Present also runs while menus pause the game. `McAlive()` means `mcHeartbeatMs != 0 && now - last < 3000 ms`
  (`kMcTimeoutMs`, `Link.cpp:15`). The two sides use different timeouts: 3 s on Skyrim's side, 8 s on Minecraft's.

### 3.4 Reconnect semantics [V:src Game.cpp:523-541]

`mcAlive && (!mcWasAlive || mcPid changed)` counts as "Minecraft (re)connected". It triggers `ResetOverlay()`,
`settleTimer = 1.5 s`, `++epoch` with `Collision::Reset(epoch)` (Minecraft drops all collision on an epoch change),
and `teleportPending = true`.

### Analog for MGSV -> Elden Ring

The protocol design (seqlocked latest-value slots, SPSC rings, triple-buffered overlay, heartbeats, epochs) carries
over as is. Three environment facts change the transport.

1. **The two games run in different Proton prefixes, so named kernel objects do not cross between them.**
   Test (`.../scratchpad/skycraft-host/ipc/ipctest.cpp`, wine-11.0, prefixes `pfxA` and `pfxB`) [V:test]:
   - A named section `Local\PhantomRingProbe` created in prefix A: `OpenFileMappingW` from prefix B fails with
     **error 2** (not found). From a second process in the **same** prefix it succeeds.
   - A **file-backed** section (`CreateFileW` on a `Z:\tmp\...\shm.bin` Unix path, then `CreateFileMappingW` +
     `MapViewOfFile`) is **shared across prefixes**: prefix B read prefix A's live writes.
   - `QueryPerformanceCounter` (10 MHz on both sides) and `GetTickCount64` **agree across prefixes**: the measured
     client-minus-server delta was 6-8 ms against a server that rewrote its stamp every 20 ms, so the difference is
     just staleness. SkyCraft's cross-process tick timestamps (`McState.tickQpc`) therefore still work.
   - **PIDs are per-wineserver**: pid 32 showed up in both prefixes in different runs. Use a random 64-bit session
     nonce instead of `skyrimPid`/`mcPid` for "the other side restarted".
   - Opening the backing file with `CREATE_ALWAYS` while another process has it mapped fails with
     **error 1224** (`ERROR_USER_MAPPED_FILE`). The owner must use `OPEN_ALWAYS` and reset the contents, as
     SkyCraft's stale-mapping reset does.
   - Proton Experimental (both prefixes report `11.0-100` in `compatdata/*/config_info` [V:bin]) is wine-11-based,
     so the same behaviour is expected [U: re-run the probe under Proton].
   - Alternative: run MGSV **inside ER's prefix** (started by the ER plugin; see section 8). Then named objects
     work as in SkyCraft. Note that `compatdata/287700` holds only `config_info` and no `pfx/` [V:bin], so MGSV's own
     prefix does not exist yet on this library.
2. **Named events or mutexes for wakeups cannot cross prefixes.** SkyCraft never uses events in practice (the
   "named events" of DESIGN.md §10 were not implemented). It polls and spins on `seq`, so this costs nothing.
3. **Coordinate space.** ER positions in `CSChrPhysicsModule.position` are a `HavokPosition` (4 floats) in "havok
   space", which fromsoftware-rs describes as the broadphase space, in meters, where 1 m equals a block-space meter
   (`crates/eldenring/src/position.rs:1-17`) [V:src]. ER **shifts the world origin** (the task groups
   `BeginShiftWorldPosition`/`EndShiftWorldPosition` exist [V:src task.rs]). So the protocol must carry a stable
   space, `BlockId` + block-local position (`BlockPosition {x, y, z, yaw}`, `position.rs:26-31`) or a global
   position, **not** raw havok coordinates. Otherwise every world shift looks like a teleport (see section 4.4).
   MGSV/Fox is meters too [U], so the scale is 1:1 rather than 70:1. Axis and handedness signs have to be pinned down
   with a test, as SkyCraft did in its "Phase 0" [U].

Main difficulty: the cross-prefix transport (file-backed mapping or a shared prefix) and replacing PIDs with nonces.

---

## 4. `Game.cpp/.h`: puppet, camera, teleport handshake, takeover, HUD (the core)

### 4.1 Shared runtime state (`Game.h:8-44`) [V:src]

`Runtime` holds:

- atomics `puppeting` (Minecraft drives the player), `minecraftOwnsPlayer` (puppeting **or** waiting for Minecraft
  to arrive after a teleport; Skyrim's own controls stay blocked either way), `mcScreenOpen`, `skyrimMenuOpen`,
  `mcInWorld`;
- look (`yaw`, `pitch` in MC degrees), `lookInitialized`, `sensitivity`;
- the virtual cursor (`cursorX/Y`) and viewport W/H;
- the interpolated feet the camera sees (`feetX/Y/Z`, `feetValid`), used to place the third-person body;
- `mcCrosshair` and `mcGuiScale` for the overlay's invert pass.

### 4.2 `PerFrame` step by step (`Game.cpp:516-1033`, main thread, after the original `PlayerCharacter::Update`) [V:src]

1. `link.Heartbeat()`. `mcAlive = McAlive()`, `haveMc = mcAlive && ReadMcState(mc)`. Reconnect check (section 3.4).
   `mcInWorld` and `mcScreenOpen` come from `mc.flags`. When an MC screen opens, the cursor is centred. MC's
   `sensitivity` is adopted.
2. `loading = !cell || !Is3DLoaded() || LoadingMenu open`. `menu = AnyBlockingMenuOpen` (any menu with
   `kPausesGame` or `kUsesCursor`). If a menu or loading **starts**, `Input::ReleaseAll()`.
   `skyrimMenuOpen = menu || loading`.
3. **World identity**: `worldId` = the interior cell's FormID, or else the worldspace FormID. On change: `Dig::SetWorld`,
   `++epoch`, `Collision::Reset`, `teleportPending`, `settleTimer = 1.5 s`.
4. **Skyrim moved the player itself**: while loading, keep `teleportPending` and clear `haveLastSet`. Otherwise, if
   the player is more than **300 units** (`kSkyrimTeleportThreshold`, `Game.cpp:19`) from where the plugin last put
   it, log "Skyrim moved the player" and set `teleportPending`. This catches load doors, fast travel, scripts and
   save loads.
5. If `teleportPending && !loading`: `++teleportSeq`, and the look is re-seeded from the Skyrim player's angles.
6. **Mouse look on the host** (zero added latency): `Input::ConsumeLook(dx, dy)`. Unless an MC screen or a Skyrim
   menu is open: `s = sens*0.6 + 0.2`, `factor = s^3 * 8 * 0.15` (Minecraft's own formula),
   `yaw = fmod(yaw + dx*factor, 360)`, `pitch = clamp(pitch + dy*factor, -90, 90)`. The result goes to Minecraft in
   `SkyState.yaw/pitch`, and Minecraft sets its player's rotation from it every frame when no screen is open
   [V:src SkyClient.java:137-143]. **The host owns the look; the guest owns the position.**
7. **Takeover detection** (`SkyrimTakeover`, `Game.cpp:384-413`). Skyrim takes the player back for: sit/sleep state
   not normal (furniture), mounted, kill move, "scripted scene" (byte `PlayerCharacter+0xBEA` bit 3, which
   `SetAIDriven`, AE ID 40586, sets: author's decompile of 1.7.104), and camera states `kFurniture`, `kAnimated`,
   `kBleedout`, `kDragon`, `kMount`, `kVATS`. A dead player never counts as a takeover (death uses Skyrim's own
   camera). When a takeover **ends**: `teleportPending = true`, so Minecraft picks the player up wherever Skyrim left
   them.
8. **Hold-mismatch watchdog**: when "arriving" (Minecraft in world, not loading, `mc.teleportAck != teleportSeq`, no
   takeover) and Minecraft's position is more than 8 blocks from Skyrim's player for over 1 s, teleport again
   (Minecraft would otherwise wait forever for ground that never streams in).
9. `puppet = haveMc && mcInWorld && mc.teleportAck == teleportSeq && !loading && !IsDead() && !takeover`.
   `minecraftOwnsPlayer = puppet || (arriving && !dead)`. When puppeting: `EnsureControlsEnabled`
   (section 4.7), `SyncSneak`. Every frame: `HideCrosshair`; `mcCrosshair = puppet && cameraMode == 0 && no screens`;
   `SetActivatePromptKey(puppet)`; `Combat::PerFrame`; `UpdateRagdoll`; `NpcBlocks::PushActorsOut` (when puppeting).
10. **Tick interpolation** (section 4.3), giving `feet*`, `eye*`, `bobPhaseNow`, `bobAmountNow`.
11. **Apply the puppet** (section 4.4) and **drive the camera** (section 4.5). When not puppeting: `eyeValid`,
    `idealValid` and `feetValid` go false, `Dig::SetPuppet(nullptr)`, first-person meshes are shown again, the FOV is
    restored, and gravity is restored if an older build had zeroed it.
12. `HideHud(ui, puppet)` every 0.5 s.
13. **Write `SkyState`**: flags (`kSkyInGame` if there is a cell, `kSkyMenuOpen`, `kSkyLoading`), `worldId`,
    `collisionEpoch`, the Skyrim player's **actual** position in MC coords, yaw/pitch, `teleportSeq`, viewport
    (filled in from Present), `gameHour` (from `RE::Calendar`).
14. After the settle timer (1.5 s after any load or teleport, because Havok bodies stream in over several frames):
    `Collision::Update(centre)`, where centre is Minecraft's position when puppeting and Skyrim's otherwise. Every
    3rd frame: `WriteWaterGrid` (16x16 columns, `TESObjectCELL::GetWaterHeight`, `kNoWater = -1e30`). Then
    `BlockLights::Update`, dig bookkeeping, and `ReportMinecraft` (status messages, section 8.3).

### 4.3 Interpolating the guest's 20 Hz ticks on the host clock (`Game.cpp:662-786`) [V:src]

The reason, in the authors' words: "Sampling MC's per-frame position instead judders, because the two games'
frames aren't phase-locked." So Skyrim **re-does Minecraft's partial-tick interpolation itself, on its own frame
clock**:

```
period = round(mc.tickMs * qpcPerMs)                 // 50 ms by default
on new tick (mc.tickQpc differs from history.back):
    if mc.tickQpc < back.tickQpc: history.clear()    // Minecraft restarted
    n   = round((tickQpc - last.at) / period)
    err = tickQpc - (last.at + n*period)
    if n == 0 and last.slots >= 2:                   // two ticks in one MC frame; we saw both
        last.at -= period; last.slots -= 1; tick.at = last.at + period
    elif 1 <= n <= 10 and |err| < 0.3*period:        // normal: lock to the exact rhythm
        tick.at = last.at + n*period + err/16        // the stamps are noisy, the rhythm is exact
    elif n <= 10 and ++outliers < 3:                 // one odd stamp: keep the rhythm
        tick.at = last.at + max(n,1)*period
    else: outliers = 0; tick.at = tickQpc            // lost the rhythm (pause, new tick rate)
    tickDue[ring of 40] = (lastFrameQpc - tick.at) ms if < 30 ms   // how late ticks arrive
    history.push(tick) (keep 8)
renderDelayMs: target = clamp(max(tickDue) + 1, 4, 30) ms;
    grows toward the target at 20 ms per second, shrinks at 2 ms per second
renderQpc = now - renderDelayMs
pick the newest tick with at <= renderQpc; t = clamp((renderQpc - tick.at)/period, 0, 1)
feet = lerp(tick.prev, tick.cur, t); eyeHeight = lerp(eyeO, eye, t)
bobPhase = -(walkDist + (walkDist - walkDistO)*t); bobAmount = lerp(bobO, bob, t)
if past this tick's end and a later tick exists (MC ran a tick we never saw):
    lerp from this tick's cur to the next tick's prev over the gap
eye = feet + (0, eyeHeight, 0)
```

The rule is pure interpolation, never extrapolation, so jumps and landings never hitch. The cost is a render delay
of 4-30 ms that adapts to how late ticks actually arrive. Diagnostics count frames that waited for a late tick,
stamp noise, and so on (`motion` stats, every 10 s with `bDiagnostics`). This depends on `QueryPerformanceCounter`
being comparable across the two processes. On Windows it is; under Wine it is too, including across prefixes
[V:test, section 3].

### 4.4 Applying the puppet (`Game.cpp:788-815`) [V:src]

Each frame on the main thread:

- `pos = McToSky(feet)`, then `PlayerCharacter::SetPosition(pos, true)`.
- Character controller: `SetLinearVelocityImpl(0)`, so Skyrim keeps no momentum of its own. **Gravity stays on**
  ("Skyrim's AI won't melee a target it thinks is in the air", author comment), and older builds' zero gravity is
  undone. `fallStartHeight = pos.z` and `fallTime = 0`, so there is no Skyrim fall damage (Minecraft's fall damage
  applies instead). `context.currentState = kOnGround` plus the `kSupport` flag if Minecraft reports on-ground,
  otherwise `kInAir`.
- `lastSetPos = pos` (for the 300-unit teleport detector) and `Dig::SetPuppet(proxy collidable, feet havok z)`.

The Skyrim player **stays in the world as a real actor**, so AI targeting, detection, stealth, trigger volumes,
quest location checks and projectiles keep working (DESIGN.md §6). Its own movement is blocked by swallowing input
in the `PlayerControls` hook (section 5), not by disabling controls: disabled controls stop NPC combat (section 4.7).

### 4.5 Driving the camera (`Game.cpp:816-955`, plus hooks) [V:src]

- **Eye position**: the interpolated eye, plus Minecraft's walk bob turned into a camera offset:
  `side = -sin(phase*pi)*bob*0.5`, `lift = |cos(phase*pi)*bob|`, `bobPitch = |cos(phase*pi - 0.2)*bob|*5 deg`,
  `bobRoll = sin(phase*pi)*bob*3 deg`.
- **Third person (MC F5)**: `cameraMode` 1 is behind the player, 2 is in front looking back (yaw + 180, pitch
  negated). The camera is pulled back along the look direction (without the bob) by `zoom` blocks. `zoom` follows
  `mc.cameraDistance`, which is Minecraft's own camera collision against its blocks and Skyrim's triangles. It
  snaps in immediately when the distance shrinks or the mode changes, and eases out with a 0.2 s time constant.
  **Skyrim stays in first-person state; only the camera moves.**
- **Rotation**: `BuildCameraRotation(heading, pitch, roll)` builds a NiCamera matrix (columns = forward, up, right;
  Skyrim X east, Y north, Z up). Before overriding, the plugin **learns Skyrim's camera-root axis convention at
  runtime**: over 240 frames it votes which of +/-forward, up and right each column of the live root matrix matches
  (within 6 degrees). It accepts the mapping only if every column has more than 60% of the votes on a distinct axis;
  otherwise it leaves rotation to Skyrim. After that `ApplyLookRotation` writes `local.rotate` and `world.rotate`
  of the camera root.
- **Pinning** (`PinCameraAndSky`): `cameraRoot` world translate (and local, if the parent is identity) = eye,
  `PlayerCamera` runtime `pos` = eye, `Sky::root` translate = eye. Then `UpdateDownwardPass` pushes the result down to
  the NiCamera child.
- This is done in three places, because Skyrim's camera update runs before `PlayerCharacter::Update`:
  1. in `PerFrame` (placed and turned together, so a third-person camera does not lag a frame while turning);
  2. in `PlayerCameraUpdateHook` (after Skyrim's whole camera update). It is installed by scanning the game's `.text`
     for `E8 rel32` calls whose target is `PlayerCamera::Update` (`RELOCATION_ID(49852, 50784)`) and redirecting
     **every** such call site through the SKSE trampoline (`write_call<5>`);
  3. in `FirstPersonState` vtable slot 5 (translation), which returns `eyePos`.
- **Player angles**: `data.angle.z = heading` and `data.angle.x = pitch + bobPitch`, so Skyrim's aim, projectiles
  and detection match the view. `ForceFirstPerson()` if not in first person.
- **FOV**: Skyrim's FOV is the horizontal FOV of a 4:3 view, Minecraft's is vertical:
  `worldFOV = 2*atan(tan(vfov/2) * 4/3)`. The original values are saved and restored when puppeting stops.
- **Hide Skyrim's first-person model** (arms, weapons): every `BSGeometry` under `Get3D(true)` is app-culled and
  the exact list is remembered and restored (meshes only, never nodes, which the camera and animation need).
- **Weapons**: if anyone is fighting the player, Skyrim's weapons are drawn (`DrawWeaponMagicHands(true)`), and
  sheathed afterwards, because Skyrim treats a player with sheathed weapons as yielding (author comment).
- Diagnostics: a camera check every 10 s (rendered camera vs Minecraft eye, measured in Present), a 240-frame
  capture, third-person orbit stats.

### 4.6 The teleport handshake (both sides) [V:src Game.cpp, SkyClient.java:115-143, 255-300, 385]

```
Skyrim (host)                                        Minecraft (guest)
teleportSeq seeded from QPC|1 at startup             lastTeleportSeq = -1
  (a stale ack from a previous run can't match)
teleportPending <- reconnect / world change /        on sky.teleportSeq change, or a new LocalPlayer object:
  loading / >300 u jump / save loaded /                requestTeleport(sky.pos, sky.yaw, sky.pitch)
  takeover ended / hold mismatch >8 blk for 1 s        teleportAck = sky.teleportSeq
if pending && !loading: ++teleportSeq,                 holdPos = sky.pos
  look re-seeded from Skyrim's angles              holdUntilReady (every frame):
SkyState.pos = Skyrim player's real position          freeze at holdPos (zero velocity, reset fall)
                                                       until collision is known at feet, below and one
puppet only when mc.teleportAck == teleportSeq          region below AND there is ground within 12 blocks
"arriving" meanwhile: Skyrim controls stay blocked      (or 6 s pass); lift out of geometry, release
  but the position isn't applied                     McState.teleportAck = holdPos ? ack - 1 : ack
                                                       ("not arrived until we are released")
```

The design point: **an ack means "arrived and on the ground", not "message received"**. So the host never copies a
guest position from before the teleport, or a guest falling through ground that hasn't streamed in yet.

### 4.7 Other per-frame duties [V:src]

- `EnsureControlsEnabled` (`Game.cpp:274-296`): Skyrim's per-frame NPC combat update (AE ID 38565) is skipped while
  the player's controls are off, so the plugin turns Movement, Looking, Activate, Fighting, Sneaking, POVSwitch,
  WheelZoom, Jumping and VATS back on (`ControlMap::ToggleControls`). Older saves stored them off.
- `SyncSneak` (`Game.cpp:491-514`): Minecraft's crouch becomes Skyrim sneak through
  `PlayerControls::DoAction(kActionSneak, 2)` (AE ID 42350), the same as the Sneak key. It is a toggle, so it is sent
  whenever the two disagree, with a 0.3 s retry, and never while dead, mounted or swimming.
- `HideHud` (every 0.5 s, `Game.cpp:333-351`): Scaleform `SetVariable("_root.HUDMovieBaseInstance.<X>._visible")`
  for Health, Magica, Stamina, Left/RightChargeMeter and ArrowInfoInstance. The compass, activate prompt and enemy
  health bar stay.
- `HideCrosshair` (every frame, because Skyrim re-shows it): CrosshairInstance and CrosshairAlert `_visible`/`_alpha`.
  StealthMeterInstance is shown only while sneaking.

### Analog for MGSV -> Elden Ring

**Per-frame entry**: tasks (section 2). ER does **not** pause in its Esc menu [U, widely known], so "menus stop
the per-frame update" matters less. Loading screens still need handling; candidates are `CSNowLoadingHelper`
(`cs/now_loading.rs`), `CSMenuManImp::loading_screen_data`, and `CSFade` [V:src struct names; the semantics are U].

**The player object**: `WorldChrMan.main_player: PlayerIns` (`cs/world_chr_man.rs:59`) [V:src]. The CT reaches it as
`[[WorldChrMan]+0x1E508]` and the player list as `[WorldChrMan]+0x10EF8` [V:src CT].
- The `WorldChrMan` static pointer can be found with the CT's AOB `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88`,
  which is commented out in the CT but **matches once** on 2.7.1.0 with target RVA `0x3d69ff8` [V:bin].
  `GameDataMan` is at `0x3d61f98`, `GameMan` at `0x3d6d988` and `FieldArea` at `0x3d6d248`: each matches its CT AOB
  exactly once and agrees with fromsoftware-rs's `game_data_man`, `game_man` and `field_area_ptr` [V:bin].
- Position: `ChrIns.modules (+0x190) -> physics (+0x68) -> CSChrPhysicsModule.position (+0x70, HavokPosition)`,
  with `orientation` at +0x50 (quaternion) and `chr_proxy_pos_update_requested` at +0x91
  [V:src physics.rs field order + CT `[[[[[WorldChrMan]+10EF8]+0]+190]+68]+70`; the offsets are consistent between
  the two sources but not checked against the binary].

**Puppeting (SkyCraft's model, "guest authoritative")**: each frame write the interpolated MGSV feet position into
`physics.position` (+ request a proxy position update) and the yaw into `orientation`. Block the Tarnished's own
locomotion while keeping it a valid target for AI. Candidates: `ChrDebugFlags` at `ChrIns+0x538`:
`disabled_movement` bit 5 and `disabled_secondary_actions` bit 4 (`cs/chr_ins.rs:523-560` [V:src]; the CT's
"No Move" is `+0x538` bit 5 and "No Hit" `+0x538` bit 3 [V:src CT]). Or stop polling with `CSPad`/`FD4BasePad`
`allow_polling` on `CSInGamePad` (`cs/pad.rs:33-36` [V:src]). Which of these keeps NPC aggro and AI behaving
normally is **unknown**. Skyrim had the trap that disabled controls stop NPC combat; ER may have similar ones [U].

**Alternative model ("host-authoritative collision")**: let ER's `hknpCharacterProxy` resolve collision and feed it
MGSV's desired displacement (root motion and velocity) instead of absolute positions. This removes the hardest
piece, getting ER's geometry into MGSV's physics (section 10), but MGSV no longer owns movement physics: no
vaulting or climbing on ER geometry unless re-created. This report recommends evaluating both; see section 14.

**Interpolation**: keep SkyCraft's algorithm (it is game-agnostic). MGSV is not a fixed-tick game the way Minecraft
is (Fox Engine updates per frame at up to 60 fps [U]), so `tickMs` becomes "MGSV frame interval". Stamp each MGSV
frame with QPC (comparable across prefixes [V:test]) and interpolate between its last two frames. If MGSV is paced
on ER's `SkyState.seq` as Minecraft is, both run at ER's frame rate and the delay stays small.

**Teleport handshake**: keep the "ack only after arrival and settle" semantics. ER moves the player itself on: grace
warps and fast travel (loading), death respawn, Spiritspring jumps, lifts and moving platforms, ladders, Torrent,
cutscenes (`CSRemo`; the CT has a commented-out AOB for it, not checked), fog walls, chest-trap warps, invasion/coop [U list]. Two ER-specific
problems:
1. **World shifting.** Raw havok positions jump on `BeginShiftWorldPosition` [V:src task group names], so the
   300-unit-jump detector needs a stable space (`BlockId` + block-local, or the game's global coordinates)
   [U: exact conversion]. `ChrIns.block_id` is a field (`cs/chr_ins.rs:134`) [V:src], the analog of `worldId`.
2. **MGSV's map is bounded.** Minecraft worlds are effectively unbounded, but MGSV's Afghanistan and Africa maps
   are a few km with streaming tied to them [U]. Venom probably has to stay near a fixed MGSV origin, with ER
   coordinates **re-based** (offset) and "rebase teleports" whenever the player wanders too far. This has no
   SkyCraft counterpart.

**Takeover** (ER takes the player back): riding Torrent (`CSChrRideModule`, `cs/chr_ins/module.rs`), ladders
(`CSChrLadderModule`), throws and critical animations (`CSChrThrowModule`, pair anims `CSPairAnimManager`),
cutscenes (`CSRemo`), grace sitting, lifts, death [V:src module names; detection fields U]. While ER owns the
player, MGSV's player must be frozen and re-teleported afterwards, as `teleportPending` does after a takeover.

**Camera**: write the guest's eye and orientation into ER's camera after `CameraStep`.
- `CSCamera` singleton: `pers_cam_1..4`, each a `CSCam {F32ViewMatrix matrix, fov, aspect_ratio, near, far}`
  (`cs/camera.rs:12-42`) [V:src]. `WorldChrMan.chr_cam: ChrCam` is the gameplay camera (`camera.rs:89-108`)
  [V:src]. Which `pers_cam` the renderer uses, and the FOV unit (vertical, radians?), are [U].
- Alternative: ER's built-in debug camera. The CT's "Free Camera" toggles `[[FieldArea]+0x20]+0xC8` and
  `[GameMan]+0xBC4` and patches three functions by AOB. All three AOBs and its `8b 93 c8 00 00 00 85 d2` inject
  site match **exactly once** on 2.7.1.0 [V:bin]. Driving `CSDebugCam` keeps the gameplay `ChrCam` untouched, the
  way SkyCraft keeps Skyrim in first-person state and only moves the camera [U whether gameplay keeps running
  normally with the debug cam on].
- MGSV is an over-the-shoulder third-person game, so the camera is MGSV's camera (position, orientation, FOV)
  rather than an eye plus a zoom distance. An axis-convention self-check like SkyCraft's 240-frame vote is cheap
  insurance.

**HUD**: `CSFeManImp.hud_state` (global HUD toggle) and `FrontEndViewValues` (`enable_equip_hud`,
enemy/boss/friendly tag arrays) in `cs/fe_man.rs:25-196` [V:src], plus `GameDataMan`'s `hud_type` option
(`cs/game_data_man.rs:306-308`) [V:src]. Hide ER's HP/FP/stamina and quick-slot HUD, and keep boss bars, enemy HP
tags and the compass, as SkyCraft keeps the compass and enemy health bar [U: per-element toggles may need
Scaleform/FD4 menu work].

**Main difficulties**: (a) choosing who owns collision (guest vs host); (b) world-shift-safe coordinates; (c) MGSV
map bounds; (d) the long list of ER-owned player states.

---

## 5. `Input.cpp`: input routing

### 5.1 Sources and hooks (`Input.cpp:253-263`) [V:src]

- `BSInputDeviceManager::AddEventSink(InputSink)`: sees every raw input event (Skyrim's DirectInput layer) on the
  main thread.
- `MenuControls` vtable slot 1 (`ProcessEvent`) hook: filters which events can open Skyrim menus.
- `PlayerControls` vtable slot 1 hook: **drops every event** while `minecraftOwnsPlayer` and no Skyrim menu is open.
  The authors chose this over disabling Skyrim's player controls, because disabled controls also stop its NPCs
  fighting.

### 5.2 Routing algorithm (`InputSink::ProcessEvent`, `Input.cpp:93-188`) [V:src]

`menuOpen = Game::SkyrimMenuOpen()` is checked **live**, because pausing menus stop `PerFrame`. On the transition to
open it calls `ReleaseAll`. `route = puppeting && !menuOpen`.

| Event | MC screen open (inventory, chat, ...) | Gameplay (no MC screen) |
|---|---|---|
| Mouse move | virtual cursor `+= delta`, clamped to the viewport; `kInCursor(x, y)` in overlay pixels | accumulate `lookDx/lookDy`; integrated in `PerFrame` (section 4.2 step 6) |
| Keyboard | **every** key to MC as `kInKey(SDL scancode, 1/0)`; Esc closes the MC screen; Skyrim gets nothing | Esc, `~` (console), J, M, F9 -> **Skyrim only** (through `MenuControls`); G (down) -> `ActivateSkyrimTarget()`; H (down) -> `OpenWaitMenu()`; O (down) -> `ReleaseAll` + `kInOpenMenu` (MC pause menu); every other key -> MC |
| Mouse buttons | Skyrim codes 0..4 -> SDL buttons {1 L, 3 R, 2 M, 4 X1, 5 X2} as `kInMouseButton` | same |
| Wheel (Skyrim codes 8/9) | `kInScroll(+/-120)` on press | same |
| Char | `kInText(codepoint)` | ignored |

Details:

- `kDikToSdl` (`Input.cpp:8-34`) is a 256-entry DirectInput-scan-code to SDL-scancode (USB HID usage) table, because
  Minecraft 26.x uses SDL scancodes.
- `ActivateSkyrimTarget`: `CrosshairPickData::GetActiveTarget()` then `target->ActivateRef(player, ...)`. This covers
  doors, NPCs (dialogue), containers, items and furniture. Furniture then triggers a takeover.
- `OpenWaitMenu`: `CanSleepWait`, then `UIMessageQueue::AddMessage(SleepWaitMenu, kShow)`.
- `MenuControlsHook` (`Input.cpp:194-233`): when puppeting and no Skyrim menu is up, it builds a **temporary linked
  list** of only the Skyrim menu keys by rewriting `event->next`, calls the original with that list, then restores
  every `next` pointer. If nothing is left it returns `kContinue` without calling the original. While an MC screen is
  open nothing passes.
- `SetActivatePromptKey` (`Input.cpp:277-297`): rebinds the keyboard mapping of the gameplay context's `"Activate"`
  event to G (DIK `0x22`) while Minecraft drives, so Skyrim's activate prompt shows G. The original key is restored
  when control returns.
- **Release-all** (`kInReleaseAll`) is sent when a Skyrim menu or loading screen starts (from both `PerFrame` and the
  sink) and when O opens Minecraft's menu, so Minecraft never keeps a key "held".
- The guest side (the Fabric `InputBridge` mixins) feeds these into Minecraft's `KeyboardHandler`/`MouseHandler` as
  if its hidden window had focus (DESIGN.md §7) [V:src design doc].

### Analog for MGSV -> Elden Ring

- **Host input stack**: `eldenring.exe` imports `DINPUT8.dll` and `XINPUT1_4.dll` [V:bin]. FD4 wraps them in
  `DLUserInputDevice` objects. fromsoftware-rs RVAs for 2.7.1.0: `keyboard_device_vmt 0x30cd4d0`,
  `mouse_device_vmt 0x30cd840`, `pad_device_vmt 0x30cd048`, `virtual_multi_device_vmt 0x30cda30`; each holds
  `0x1401...` code pointers on disk [V:bin]. Game logic polls through `FD4PadManager` -> `CSPad` subclasses
  **`CSInGamePad`** (gameplay) and **`CSMenuViewerPad`** (menus), plus `CSDebugCamPad` (`cs/pad.rs:1-30,160-180`)
  [V:src]. Their vtables are `csin_game_pad_vmt 0x2be35e8` and `csmenu_viewer_pad_vmt 0x2be4778`, both holding code
  pointers [V:bin].
- **This split matches Skyrim's `PlayerControls`/`MenuControls` almost one to one.** Swallow gameplay input by
  gating `CSInGamePad` (`FD4BasePad.allow_polling = false`, or hooking its poll) while MGSV drives, and let
  `CSMenuViewerPad` through for an allow-list (Esc/menu, map, interact) [U: whether `allow_polling` is safe to flip
  every frame].
- **Raw capture for forwarding**: the simplest route is the keyboard/mouse device vtables (or the DirectInput8
  device `GetDeviceState`/`GetDeviceData`) at `PadStep`, forwarding raw state into the input ring. Controller
  support adds XInput. Skyrim's sink delivered events; ER's devices are polled state, so press/release edges have
  to be derived [U].
- **Key allow-list for ER** [U, to be designed]. ER's defaults are: Esc = menu, G = map, E = event action
  (talk/open/pick up/rest at grace), and lock-on (MMB/Q) arguably. MGSV owns WASD, Space, Ctrl/C, mouse, Q/E/F/R,
  number keys, iDroid (Tab?) and so on. This collides: ER's E (interact) and MGSV's E/Space (action) conflict. Copy
  SkyCraft's solution: give ER's interact a dedicated key (SkyCraft's G) and show it in ER's prompt by rebinding the
  ER key assignment (`CSKeyAssign`/`CSInGameKeyAssign`, `cskey_assign_vmt 0x2be3748`) [V:src RVA name; U usage].
  ER's activation goes through `CSActionButtonMan::execute_action_button` (RVA `0xa641c0` in the 2.7.0.0 bundle, not
  overridden for 2.7.1.0 [V:src]; prologue `48 8b c4 55 41 54 41 55 41 56 41 57` at that RVA on 2.7.1.0 [V:bin]). It
  is the analog of `ActivateRef` on the crosshair target.
- **Guest side (MGSV)**: the strings `DirectInput8Create` and `GetRawInputData` appear in `mgsvtpp.exe` [V:bin
  string presence], so MGSV reads keyboard/mouse through DirectInput and/or Raw Input. A **hidden** MGSV window gets
  neither, so the guest DLL must inject input below MGSV's input layer (hook its DInput device or Fox's input
  manager) and fake focus (no pause on focus loss). This is the counterpart of SkyCraft's `InputBridge` and
  `pauseOnLostFocus = false`. MGSV's exe hides its import table (no `DLL Name` entries from objdump; unusual
  sections `.xtext`, `.xdata` (117 MB), `.ecode`, `.trace`) [V:bin], so it is packed or protected and needs separate
  guest-side research.
- **Cursor mode**: when an MGSV screen with a cursor is open (iDroid, Mother Base management), forward an absolute
  cursor as `kInCursor` and draw a software cursor in the overlay (section 6) [U: whether iDroid uses a mouse cursor
  or only key navigation].
- **Main difficulty**: input *injection* into a hidden, protected MGSV, and resolving the key conflicts.

---

## 6. `Overlay.cpp`: compositing the guest HUD/GUI

### 6.1 Hook [V:src Overlay.cpp:393-408]

At `kDataLoaded` it takes `RE::BSGraphics::Renderer::GetCurrentRenderWindow()->swapChain`, reads its vtable and
**overwrites slot 8 (`IDXGISwapChain::Present`) in place** (`VirtualProtect` RWX, swap, restore protection).
No trampoline or detour. Note that this changes the vtable shared by every swap chain of that class in the process.

### 6.2 `PresentHook` (`Overlay.cpp:370-388`) [V:src]

1. `Link::Heartbeat()`. Present runs even while the game is paused (menus, loading), unlike the player update.
2. `Game::CheckRenderedCamera()` (diagnostics, frame-order letter X).
3. Inside `try { } catch (...) {}`: `WorldRender::Draw` (drains the render ring, readbacks, fallback world drawing),
   then `DrawOverlay`, then `CaptureIfRequested` (debug PNG).
4. The original Present.

### 6.3 `DrawOverlay` (`Overlay.cpp:229-368`) [V:src]

- Updates `viewportW/H` from the swap chain description every Present. They go to Minecraft in `SkyState`, so
  Minecraft renders the overlay at Skyrim's back-buffer resolution, and Minecraft reports `guiScale` back.
- Draws only if the link is valid, Minecraft is alive, `mcInWorld`, and the D3D resources were created
  (`InitResources` compiles the HLSL with `D3DCompile`, `vs_5_0`/`ps_5_0`).
- `UploadLatestFrame`: `AcquireOverlayFrame()` (triple-buffer swap; returns false if nothing new, and then the last
  texture is reused). The slot header's size is validated (at most 3840x2160). A `DXGI_FORMAT_R8G8B8A8_UNORM`
  `D3D11_USAGE_DYNAMIC` texture is recreated on size change and filled with `Map(WRITE_DISCARD)` row by row
  (respecting `RowPitch`). `flipY` comes from header flags bit 0 (GL rows are bottom-up). **CPU path**: Minecraft
  reads its frame back and copies it into shared memory. The GPU-interop paths in DESIGN.md (WGL_NV_DX_interop,
  Vulkan external memory) were **not** implemented, and neither were the frame-N fences: Skyrim simply shows the
  newest complete frame.
- Skipped while `skyrimMenuOpen` (Skyrim's menus are drawn unobstructed).
- Draw: a per-frame RTV on back buffer 0, no depth/stencil, a full-screen triangle from `SV_VertexID` (no vertex
  buffer), point sampler, `CULL_NONE`, **premultiplied-alpha blend** (`ONE, INV_SRC_ALPHA`). The pixel shader also
  draws a **software arrow cursor** (18 px, black edge) at the virtual cursor when an MC screen is open (scaled from
  overlay to back-buffer pixels).
- **Crosshair invert pass**: Minecraft draws its crosshair with an invert blend against the world. The main pass
  outputs 0 inside a rectangle around the screen centre (x +/-12 GUI px, y -12..+28 GUI px, covering the 15 px
  crosshair and the 16x16 attack indicator 9 px below). A second pass with `SrcBlend = INV_DEST_COLOR,
  DestBlend = INV_SRC_COLOR` (alpha: ZERO/ONE, so the back buffer's alpha is untouched) reproduces Minecraft's look
  against Skyrim's picture.
- **Full save and restore** of the pipeline state it touches: RTVs and DSV, blend (+ factor and mask), depth state +
  stencil ref, raster, viewports, topology, input layout, VS/PS, PS SRV 0, sampler 0, CB 0.

### 6.4 What is drawn where (summary) [V:src]

| Layer | Who renders it | Where it enters Skyrim's frame |
|---|---|---|
| Minecraft HUD and GUI (hotbar, hearts, screens, hand?) | Minecraft (offscreen, CPU readback) | Present, on top of everything, premultiplied alpha (`Overlay.cpp`) |
| Minecraft blocks, entities, particles, the third-person player body, arrows, items | Skyrim's D3D11 context with SkyCraft's shaders, from meshes Minecraft streams | after `Main::RenderWorld`, into the HDR scene + depth + motion vectors + normals (`WorldRender.cpp`), or at Present as a fallback |

[U] Whether the first-person hand/item goes in the overlay or the world layer was not traced. DESIGN.md §9 planned a
separate "Hand" layer.

### Analog for MGSV -> Elden Ring

- **API change**: ER is D3D12 (imports `d3d12.dll` and `dxgi.dll` [V:bin]; under Proton through vkd3d-proton; a
  `vkd3d-proton.cache` file sits in the Game folder [V:bin]). There is no immediate context. The standard technique
  [U, the usual DX12 overlay recipe, e.g. as used by imgui-based hooks]:
  - hook `IDXGISwapChain::Present` (index 8), or `Present1` (index 22), on the swap chain;
  - get the game's `D3D12_COMMAND_LIST_TYPE_DIRECT` queue by hooking `ID3D12CommandQueue::ExecuteCommandLists`
    (index 10) once;
  - keep per-back-buffer command allocators and lists, and an RTV heap for the swap chain buffers
    (`IDXGISwapChain3::GetCurrentBackBufferIndex`);
  - in Present: barrier PRESENT->RENDER_TARGET, draw the full-screen triangle with premultiplied alpha, barrier
    back, `ExecuteCommandLists` on the game's queue, then call the original Present.
  - **Upload**: copy the newest triple-buffer slot into a persistently mapped `UPLOAD`-heap buffer (one per frame in
    flight), then `CopyTextureRegion` into a `DEFAULT` texture. Fence or ring the upload buffers so the CPU never
    overwrites a buffer the GPU is still reading.
  - ER may use an HDR swap chain format (R10G10B10A2 or FP16 when HDR is on) [U], so the overlay shader must encode
    for the back-buffer format.
- **Cross-API GPU sharing** (MGSV D3D11 via DXVK -> ER D3D12 via vkd3d-proton) **across processes and, if separate,
  prefixes** is not realistic: NT handles cannot cross wineservers [V:test showed even named sections do not]. **Use
  SkyCraft's CPU path**: file-backed shared memory + triple buffer [V:test that file-backed sharing works]. At
  2560x1440 RGBA8 that is 14.7 MB per frame; Minecraft does the same at up to 3840x2160.
- **What MGSV contributes to the overlay**: its HUD (weapon/ammo, reticle, iDroid, Reflex mode tint, damage
  vignette, CQC prompts, tags). MGSV draws all of that *over its own 3D world*, so the guest must render UI only.
  Hook Fox's frame between scene and UI and clear to transparent, or capture only the UI pass [U: Fox Engine render
  pass structure; guest-side research]. Minecraft made this easy (the mod controls what it renders; sky, clouds and
  fog turned off); for MGSV it is a main difficulty.
- **The player's body**: SkyCraft has Skyrim draw Minecraft's posed avatar mesh (`kRenAvatar`) at Skyrim's
  interpolated feet, inside the HDR scene. For Venom the options are:
  - (A) stream MGSV's skinned Venom vertices per frame and draw them in ER's D3D12 frame. That needs mid-frame D3D12
    injection into ER's render graph [U, hard].
  - (B) **draw Venom as an ER character**: reuse the old project's Venom converted to ER parts
    (`<old-project>`) and drive its skeleton from MGSV's bone pose streamed over the link (a "pose ring" in
    place of `kRenAvatar`). ER then lights, shadows and post-processes it natively, and no world-render injection is
    needed [U: skeleton retargeting between the Fox and ER skeletons is the work].
  - Option B is strongly preferred. It turns SkyCraft's largest file (`WorldRender.cpp`, 3.4k lines) into
    "pose application".
- Skip the overlay while ER menus are open (as `skyrimMenuOpen` does). Draw ER's status text (section 8.3) into the
  overlay too.
- **Main difficulty**: D3D12 Present-time compositing under vkd3d-proton (well understood but fiddly), and making
  MGSV render only its HUD.

---

## 7. `Combat.cpp`: combat bridging

### 7.1 Actor table (host to guest) (`WriteActorTable`, `Combat.cpp:171-215`) [V:src]

Written every frame while puppeting:

- Source: `ProcessLists::highActorHandles`.
- Skipped: the player, disabled actors, actors without loaded 3D, ghosts, and anything farther than
  **80 blocks (5600 units)** (`kActorRange`). At most 256 records.
- Record (64 B): `formId`; flags (`kActorHostile` = `IsHostileToActor(player)`, `kActorDead`, `kActorEssential`,
  `kActorInCombat`); feet in MC coords; yaw; `height = clamp(GetHeight()/70, 0.3, 12)`;
  `width = clamp(2*GetBoundRadius()/70, 0.3, 6)`; `healthFrac = Health / max Health`; `level`; `name[24]`
  (display name, UTF-8, truncated).
- Seqlocked full rewrite (`WriteActors`). When not puppeting it writes 0 actors, so Minecraft removes the proxies.
- Guest side: Minecraft keeps one invisible `skycraft:actor_proxy` LivingEntity per record. Vanilla weapons, crits,
  enchantments, arrows and splash potions work on them (DESIGN.md §8.1) [V:src design doc].

### 7.2 Guest to host: Minecraft hits an NPC (`ApplyHit`, `Combat.cpp:325-398`) [V:src]

Event `kEvHitActor {formId, a = MC damage after MC's modifiers, b/c = knockback dir x/z, d = knockback strength,
flags = Critical|Projectile|Sweep|Fire, weapon = Unarmed|Blade|Axe|Blunt|Pierce|Arrow}`.

1. Look up the actor; ignore it if missing or dead.
2. **Damage scaling**: `damage = a * (5 + 0.25 * actor level)` (DESIGN.md §13: a diamond-sword crit of about 10 hits
   a level-10 bandit for about 75).
3. **Stagger**: `stagger = (d > 0.45 || crit) ? clamp(crit ? 0.35 : (d - 0.4)*1.2, 0.25, 1.0) : 0`. Ordinary MC
   swings do not stagger, because Minecraft's attack rate would stun-lock everything (author comment).
4. A **stand-in vanilla weapon** gives the impact set, sounds and skill: Iron Sword `0x12EB7` (blade), Iron War Axe
   `0x13790` (axe), Iron Dagger `0x1397E` (pierce), Hunting Bow `0x13985` (arrow), Iron Mace `0x13982` (everything
   else).
5. Hit position = the first bone found among `NPC Spine2 [Spn2]`, `Spine1`, `Spine`, `Pelvis` (or root + 0.6 x
   height); direction = from the player, horizontal.
6. **Skyrim's own hit pipeline** (`ResolveHitPipeline`, `Combat.cpp:227-246`), AE only. The plugin trusts the address
   only if the engine's melee handler really calls it: it checks that `REL::ID(38627) + 0x4A8` is `E8` with target
   `REL::ID(38586)`. Then it builds a `HitData` on the stack (ctor `REL::ID(43995)`, `Populate(player, actor,
   nullptr)`), and sets `weapon`, `hitPosition`, `hitDirection`, `totalDamage = physicalDamage = damage`, resisted
   damage 0, `sneakAttackBonus = bonusHealthDamageMult = 1`, `stagger`, `pushBack` (= d if staggering), `skill`, and
   flags (`kMeleeAttack` unless projectile, `kCritical` if crit, `kPowerAttack` if d > 0.45). Then
   `processHit(actor, hit)`. Result (author comment): damage, blocking, hit reactions and stagger, pain voice,
   TESHitEvent (crime, quests, enemy health bar) and kill credit, all handled by Skyrim.
7. **Fallback** (not AE or the check failed): `actor->DoDamage(damage, player, true)`, plus stagger through the
   animation graph (`staggerDirection` and `staggerMagnitude` variables, `"staggerStart"` event).
8. Blood and sound: `BGSImpactManager::PlayImpactEffect(actor, weapon->impactDataSet, node->name, dir, 128, ...)`.
9. `TrainSkill(player, HitSkill, uses = MC damage)` through `AddSkillExperience` (Archery for projectiles,
   Two-Handed for axes and pierce, One-Handed otherwise, none unarmed).
10. `StartCombat(player)` if the actor is not a teammate and not already in combat.

Other guest events [V:src Combat.cpp:879-902]:

- `kEvExplosion`: queued 0.25 s (so Minecraft's damage lands first). Then every reference within `2 x radius` blocks:
  actors get `AIProcess::KnockExplosion(25 * falloff * power)` and a pending ragdoll fling (up to 2 s wait;
  `(6 + 12*falloff)*power` m/s on every dynamic rigid body); loose objects get a Havok linear impulse
  `14*falloff*power` m/s, under the `bhkWorld` write lock.
- `kEvArrowStuck`: `WorldRender::StickArrow` pins the arrow to the bone nearest its path (within 60 units; at most
  16 per actor and 160 in total).
- `kEvSkillUse`: `TrainSkill` for ActorValues One-Handed..Enchanting (Block, Smithing, Light/Heavy Armor).
- `kEvPlayerDied`: `KillPlayer` (section 7.4).
- Hazards (lava, fire, magma blocks, every 0.2 s): Minecraft damage per second 8/2/1 (afterburn 1), scaled like
  hits; `HazardFireSpell 0x000153BD` cast every 1.5 s; NPCs scramble to the nearest safe column within 5 blocks at
  380 units/s.

### 7.3 Host to guest: Skyrim hurts the player (`BridgePlayerDamage`, `Combat.cpp:126-169`) [V:src]

**Minecraft's health is authoritative.** Skyrim's damage is allowed to happen and then **refunded**:

- **Who and what**:
  - `HitSink` (a `TESHitEvent` sink, only for target == player while puppeting) records the attacker FormID and the
    kind: projectile if `event->projectile`; magic if the source form is a Spell, Enchantment, Scroll, Ingredient or
    AlchemyItem; melee otherwise. It also records the PowerAttack and HitBlocked flags.
  - `HandleHealthDamageHook` (`PlayerCharacter` vtable slot `0x104`) records the last damager actor.
- **Every frame**: `deficit = maxHealth - health`.
  - The first frame after puppeting starts only primes: it restores any existing deficit and sends nothing.
  - Then, if `deficit > 0.01`: `RestoreActorValue(Health, deficit)`. Attribute the damage to the last hit event if it
    is younger than **0.35 s** (`kHitMemorySeconds`; one hit event explains one health drop); else to the last
    damager (kind magic, e.g. a DoT); else to nobody.
  - Melee and projectile go out at once. Magic and other damage accumulate and flush every **0.5 s**
    (`kDotFlushSeconds`), so Minecraft's i-frames don't eat them.
- Message: `kInHurt` **on the input ring**, with `code = HurtKind`, `a = Skyrim damage * 100`, `b = attacker FormID`,
  `c = flags` (BlockedInSkyrim, PowerAttack). Minecraft divides by 5 (DESIGN.md §13; the host log prints
  `damage/5`). Minecraft then applies `player.hurt()` with custom damage types, so armour, shields, totems and
  i-frames are all vanilla.
- **Essential flag**: while puppeting the Skyrim player is set essential
  (`Actor::BOOL_FLAGS::kEssential`), so "one huge hit knocks them down (essential) for a moment instead, and
  Minecraft's health does the rest". The bleedout camera state then counts as a takeover (section 4.2 step 7).

### 7.4 Death [V:src]

- Minecraft dies -> `kEvPlayerDied` -> `KillPlayer`: clear essential, then
  `KillImpl(killer, health + 1, true, false)`, falling back to `KillImmediate()`. Skyrim's normal death camera and
  reload flow run. `WorldRender::UpdateRagdoll` then **binds Minecraft's body** (the last `kRenRagdoll` snapshot) to
  Skyrim's ragdoll bones (Head, Spine2, R/L UpperArm, R/L Thigh) and culls Skyrim's own body meshes, so the
  Minecraft body flops with Skyrim physics.
- When not puppeting, `Combat::PerFrame` still **drains** the event ring (stale hits must not land when control
  resumes), but honours `kEvPlayerDied` only if Minecraft is alive. It also clears essential and empties the actor
  table.

### 7.5 "Engaged" [V:src]

`AnyoneFighting` (every 0.25 s): any live actor in combat whose `currentCombatTarget` is the player, within
6000 units. It drives weapon drawing (section 4.5). Diagnostics (`LogNearbyCombat`, every 5 s with
`bDiagnostics`) read the AI's combat state under SEH and call AE IDs 33216, 37484, 39452, 37199 and 380233 for
reachability and gating.

### Analog for MGSV -> Elden Ring

- **Actor table**: iterate `WorldChrMan.chr_inses_by_distance: DLVector<ChrInsDistanceEntry {chr_ins, distance}>`
  (`cs/world_chr_man.rs:100,174-177`) [V:src], the ready-made counterpart of `highActorHandles` plus the distance
  filter. Per actor:
  - stable ID: `ChrIns.field_ins_handle` (block + selector; the analog of FormID);
  - position: `physics.position` (convert to the stable space, section 3), yaw from `orientation`;
  - capsule: `physics.chr_hit_radius`/`chr_hit_height` (or `hit_radius`/`hit_height`) (`physics.rs:95-100`)
    [V:src];
  - `data.hp`/`max_hp` (`cs/chr_ins/module/data.rs:29-30`) [V:src];
  - hostility: `ChrIns.team_type` + `CSTeamType*` relations (`cs/team_relation.rs`; `csteam_type_enemy_vmt` etc.)
    [V:src names; U semantics];
  - level: `npc_param_id` -> NpcParam soul/level fields [U];
  - name: NpcParam name id -> `MsgRepository` FMG [U].
- **Guest-side proxies are MGSV's hard part**: there is no "spawn an invisible LivingEntity". The candidates are
  invisible MGSV soldier/animal game objects placed and moved at ER NPC positions, so MGSV's own weapons, CQC, tranq
  and suppression logic register hits on them [U; mission-scripted, limited soldier counts per mission pack].
- **Applying MGSV hits to ER NPCs** (counterpart of `processHit` + HitData). Ranked:
  - (1) **Spawn an ER bullet/attack** from the player at the target with a chosen `AtkParam`/`BulletParam`
    (`CSBulletManager::spawn_bullet`, RVA `0x3a2cb0` from the 2.7.0.0 bundle, not overridden for 2.7.1.0 [V:src]; on
    2.7.1.0 it is the target of a `call` at RVA `0x3a2c9e` and starts `40 53 55 56 57 48 81 ec 98 07 00 00`
    [V:bin]). The hit then runs ER's damage manager (`DmgMan_Pre/ShapeCast/Post` task groups exist
    [V:src]): poise and stance, hit reactions, aggro, damage numbers, kill credit and drops, all native. This is the
    closest match to SkyCraft's "feed the engine's own hit pipeline".
  - (2) `ChrIns::apply_speffect` (RVA `0x3e8dc0`; prologue `48 8b c4 48 89 58 08 ...` [V:bin]) with damage-dealing
    SpEffectParam rows (DoT, fixed damage).
  - (3) Write `CSChrDataModule.hp` directly (the counterpart of `DoDamage`; no reaction, no aggro).
  - Damage scaling: like SkyCraft's `5 + 0.25*level`, MGSV damage (weapon damage vs MGSV's health scale) has to be
    mapped onto ER HP (hundreds to tens of thousands) with a level/scaling factor [U, a design decision]. MGSV's
    non-lethal damage (tranq, stun, sleep) has natural ER counterparts: ER's sleep and madness/stun status
    build-up, applied through SpEffects [U].
- **ER hurting the player** (counterpart of the refund loop): ER damage arrives through the damage manager. Mirror
  SkyCraft:
  - set **NoDead** on the player: `CSChrDataModule+0x19B` bit 0 per the CT, with **NoDamage** at bit 1 [V:src CT
    `NoDead.xml`/`NoDamage.xml`; offset not checked on the binary]. Or set `WorldChrManDbgFlags.player_no_dead`
    (`cs/world_chr_man_dbg.rs:108-111`; static RVA `world_chr_man_dbg_flags 0x3d6a210`) [V:src];
  - each frame read `max_hp - hp`, refund it, and attribute it to `ChrIns.last_hit_by: FieldInsHandle`
    (`cs/chr_ins.rs:205`) [V:src];
  - the CT's `LastHitNpcInfo` AOB `48 8b 03 48 8b cb ?? ?? ?? ?? ff 50 10 83 f8 01` (match exactly once on 2.7.1.0
    [V:bin]) shows where the game records the last NPC hit, which may give attack kind and parameters [U].
  - Better, and harder: hook ER's damage application to read the attack's damage types (physical, magic, fire,
    lightning, holy; status build-up) and cancel it, then send it to MGSV as a typed hurt event [U, RE needed].
  - **Do not** use NoDamage alone. That loses the hit reactions NPCs expect (ER AI reacts to the player's stagger
    state) [U].
- **Death**: MGSV death -> clear NoDead and set `hp = 0` on the ER player -> ER's "YOU DIED" and grace respawn
  [U: whether hp = 0 alone triggers death]. The reverse (ER kills the player through a scripted death: falls, instant
  death zones) has to kill Venom in MGSV too, or be treated as ER-authoritative.
- **Fall damage**: SkyCraft zeroes Skyrim's fall tracking. In ER the fall module is `CSChrFallModule` [V:src name].
  Whether MGSV or ER owns fall damage depends on who owns collision.
- **Main difficulties**: MGSV hit proxies, and a clean ER damage-application hook.

---

## 8. `Launcher.cpp` and status messages

### 8.1 Starting the guest (`StartMinecraft`, `Launcher.cpp:258-320`) [V:src]

Called from `SKSEPluginLoad`, i.e. as early as possible.

1. Read `Data/SKSE/Plugins/SkyCraft.ini` (SimpleIni). `bStartWithSkyrim = 0` means the user starts Minecraft
   themselves (status `kOff`).
2. **Detection by mutex**: `OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\SkyCraft_v1_minecraft")`. If it opens, a
   Minecraft with the mod is running (status `kRunning`). A watcher thread then polls once a second for 60 s; if that
   Minecraft quits (the previous Skyrim's Minecraft exits a few seconds after its Skyrim did), it waits 3 s and calls
   `StartMinecraft()` again.
3. Choose the program: `sLauncher` from the ini; else the bundled `Data/SKSE/Plugins/SkyCraft/SkyCraft-Minecraft.zip`;
   else an installed Prism at `%LOCALAPPDATA%\Programs\PrismLauncher\prismlauncher.exe` or
   `%ProgramFiles%\PrismLauncher\prismlauncher.exe`. If none: `kNoLauncher`. Arguments: `sArguments`, default
   `--launch SkyCraft`.
4. A detached thread:
   - `EnsureBundle()` unpacks the zip into `%LOCALAPPDATA%\SkyCraft` when its size and timestamp stamp
     (`bundle.stamp`) changed. Old `skycraft-*`, `fabric-api-*` and `e4mc-*` jars are removed first. The zip is
     copied out of MO2's virtual file system before extraction with Windows' `tar.exe -xf` (`CREATE_NO_WINDOW`,
     5 min timeout). `prismlauncher.cfg` is seeded once, and `LowMemWarning=false` is patched in.
   - With the bundled Prism and no `accounts.json` -> status `kSignIn`.
   - `Start()`: **`OpenFromDesktop`** runs the program the way a double-click would, through Explorer:
     `IShellWindows::FindWindowSW(CSIDL_DESKTOP)` -> `IServiceProvider` -> `IShellBrowser` ->
     `QueryActiveShellView` -> `IShellFolderViewDual::get_Application` -> `IShellDispatch2::ShellExecute(file,
     args, dir, "open", show)`. So Minecraft is **not a child of Skyrim**: it stays outside MO2's VFS, and MO2 does not
     wait on it. `.bat`/`.cmd` files are started hidden. If there is no desktop shell: `CreateProcessW`
     (`CREATE_NO_WINDOW` for scripts).
   - Failure -> `kFailed`.
5. **Hidden from the start** is the guest's job (`-Dskycraft.startHidden=true` in the instance's JVM args): no
   window, no music. The guest opens its world by itself once Skyrim's world is up, and **quits when Skyrim closes**
   (`skycraft.quitWithSkyrim`, default true) [V:src SkyClient.java:148-160].

### 8.2 Liveness helpers [V:src]

- `MinecraftRunning()` checks the mutex.
- `PrismRunning()` takes a Toolhelp32 snapshot and looks for `prismlauncher.exe`.
- `DiagnosticsEnabled()` is `[Debug] bDiagnostics`, read once.

### 8.3 Status messages (`ReportMinecraft`, `Game.cpp:417-484`) [V:src]

Only while in game, not loading, no menu, and not connected.

- First note after 3 s, then every 120 s, through `RE::SendHUDMessage::ShowHUDMessage` (Skyrim's corner
  notifications).
- Status `kSignIn` -> "Alt-Tab to the Prism window and sign in". `kStarting`/`kRunning` -> "starting Minecraft..."
  (once). `kNoLauncher` -> "isn't set up" (give up). `kFailed` -> "couldn't start" (give up).
- After **60 s** still unconnected, it says which kind of stuck it is: Minecraft running but not responding /
  Prism still busy / Minecraft closed (with the log path).
- Gives up after 180 s (600 s during sign-in): "still hasn't connected. See SkyCraft.log."
- "SkyCraft: Minecraft is ready." once it connects after a note was shown.

### Analog for MGSV -> Elden Ring

- **Starting MGSV hidden.** Three ways [all U unless noted]:
  1. **From the ER plugin**: `CreateProcessW("<lib>\common\MGS_TPP\mgsvtpp.exe")` inside ER's process. It then runs
     **in ER's prefix (1245620) under ER's wineserver**, so SkyCraft-style named objects work (section 3).
     `steam_appid.txt` next to the exe contains `287700` [V:bin], so `SteamAPI_Init` should bind to the running
     Steam client as appid 287700 [U: whether Proton's lsteamclient in ER's prefix accepts it, and whether MGSV's
     protector demands a Steam launch]. MGSV's prefix 287700 has no `pfx/` [V:bin], so running it in ER's prefix
     also avoids creating a second prefix. **But**: if both games are launched through me3 or Steam, two
     simultaneous "games" under one Steam login may show the user as playing both, and Steam may block a second
     app launch [U].
  2. **Steam launch of MGSV** in its own prefix (`steam -applaunch 287700`, from a Linux-side helper script that
     PhantomRing ships), with a guest DLL (MGSV-side loader, e.g. a `dinput8.dll`/`d3d11.dll` proxy or an injector).
     That requires the cross-prefix transport (file-backed mapping) [V:test that it works].
  3. A small **Linux launcher script** that starts both games with the right environment (the old project already
     launches ER through me3 from Linux). This is probably the most robust option and keeps Steam and Proton
     handling standard.
- **Detection**: the mutex trick only works within one prefix. Across prefixes, use a heartbeat plus a session nonce
  in the shared file, or a lock/pid file in the shared directory [V:test the shared file; U the design].
- **Hidden window, no audio, no pause on focus loss, quit with ER**: guest-side duties, as Minecraft's
  `startHidden`/`quitWithSkyrim`. MGSV must not pause or minimise when unfocused and must keep rendering its HUD
  offscreen [U].
- **Status messages**: ER's `CSMenuManImp::display_status_message(i32 message)` (RVA `0x7671f0`; prologue
  `40 53 48 83 ec 30` [V:bin]) shows **FMG message IDs**, not free text (signature in `cs/menu_man.rs:75`) [V:src].
  Either add FMG entries (the asset path from the old project) or **draw status text in PhantomRing's own overlay**,
  which is simpler and works before MGSV is connected.
- **Main difficulty**: Steam/DRM and the prefix choice for MGSV; MGSV's protected exe.

---

## 9. `CrashLog.cpp`, `Perf.h`, `PCH.h`

- **CrashLog** [V:src]: `SetUnhandledExceptionFilter(Filter)`, installed at load and again at `kDataLoaded`, chaining
  to the previous filter. On the first crash it logs the exception code, `module+offset`, and the faulting
  read/write address for access violations. It walks 48 frames with `RtlLookupFunctionEntry` + `RtlVirtualUnwind`
  inside `__try`, writes `SkyCraft_crash.dmp` (`MiniDumpWithIndirectlyReferencedMemory | WithThreadInfo |
  WithUnloadedModules`), flushes the log, and returns the previous filter's verdict.
- **Perf** [V:src]: QPC scopes for 15 slots (frame, update, collision, dig, lights, water, draw, in-frame draw,
  grass, readbacks, and four Havok hooks), atomics summed and logged once a minute (every 10 s with diagnostics), with
  Skyrim's own frame time measured between player updates.
- **PCH** [V:src]: CommonLib (`RE/Skyrim.h`, `SKSE/SKSE.h`), spdlog, STL, `Windows.h`, `d3d11.h`, `dxgi.h`;
  `namespace logger = SKSE::log`.

### Analog for MGSV -> Elden Ring

All three port directly. Note that **Arxan** installs its own exception handling, and me3 and Seamless Coop may
install filters too [U]. Chain, never replace, as SkyCraft does. Under Wine, `dbghelp`'s `MiniDumpWriteDump` works
[U]. Log to a PhantomRing directory rather than the game folder. Use the same Perf scopes for tasks and Present.

---

## 10. `Collision.cpp/.h` (skim): exporting the host's geometry to the guest

[V:src unless noted]

- **Units**: a region is 8x8x8 blocks, voxelized at 1/8 block (a 64^3 grid). Scheduling: 5 regions around the player
  horizontally, 3 below, 2 above, sorted nearest first (vertical distance weighted x2). Each frame at most 3
  harvests or 2.5 ms. Regions next to the player are re-sent every 1000 ms (doors and other moving things). Dug
  regions are urgent and go first. Bookkeeping is cleared past 4x the offset count.
- **Main thread**: under `bhkWorld->worldLock` (read), `GatherBodies` walks `hkpWorld` fixed, active and inactive
  simulation islands. It keeps the layers StairHelper, Static, AnimStatic, Transparent, Trees, Props, Terrain,
  Ground and InvisibleWall, and gets each body's AABB via `GetAabbImpl` under SEH. `Harvest` recurses through
  **hkp shape types**: MOPP/BVTree (query AABB in shape-local space), lists and collections, compressed and extended
  meshes, triangles (vertices at `+0x30/+0x40/+0x50`), boxes (half extents `+0x30`, radius `+0x20`),
  capsules/spheres, convex vertices (planes `hkArray` at `+0x78`), convex transform/translate, transform. Unknown
  convex shapes fall back to their AABB, and every call is wrapped in SEH ("never let a bad read take down the
  game").
- **Worker thread**: a plane-guided SAT triangle-box test (cost scales with area, not volume), primitives by
  voxel-centre containment (+0.5 voxel margin). **Steep surfaces** (|n.y| between 0.1 and 0.643, i.e. about 50-84
  degrees) are coarsened to whole-block columns, so Minecraft's 0.6 step-height rule decides what is climbable (a
  data decision, not a physics change). Dug blocks are carved out. Each region sends `kColTris` (exact triangles,
  including invisible stair-helper ramps) and then `kColRegion` (`ColBlock {x, y, z, u64 bits[8]}` per non-empty
  block). Everything carries the `epoch`; `kColClear` on reset.
- **Guest**: a Mixin appends these shapes to Minecraft's collision queries, so Minecraft's own movement code runs
  unchanged against Skyrim's world (DESIGN.md §5.1).

### Analog for MGSV -> Elden Ring

- ER uses **Havok's "new physics" (hknp)**, not Skyrim's hkp. The exe contains `hknpWorld`, `hknpShape`,
  `hknpCompressedMeshShape`, `hknpHeightFieldShape` and `hknpCharacterProxy` strings, and only one `hkpWorld`
  string [V:bin string counts]. The shape walker has to be rewritten for hknp shapes (compressed meshes, height
  fields, convex polytopes, compounds) [U on layouts; vswarte's fromsoftware-rs `CSHavokMan.phys_world:
  CSPhysWorld` is still `// TODO` (`cs/havok_man.rs:11-23`)]. A cheap MVP mirrors SkyCraft's planned "stage A":
  **ray-cast grids** through `CSPhysWorld::cast_ray` (RVA `0xc71e00` on 2.7.1.0; prologue `40 55 56 57 41 54 ...`
  [V:bin]; signature in `cs/havok_man.rs:25-58`) [V:src].
- **Injecting ER geometry into MGSV's physics is the hardest single problem in the guest-authoritative design.**
  Minecraft gave SkyCraft a clean Mixin point (`Entity.collide`). MGSV's character controller and physics are in a
  protected binary [V:bin protected], and no Havok strings are visible in it [V:bin string counts = 0; possibly
  encrypted]. So it is unknown whether Fox uses Havok or a custom engine, and what a "collision query" looks like
  there [U].
- **Hence the recommendation** (section 14): evaluate a **hybrid** in which ER's character proxy resolves collision
  and MGSV supplies locomotion intent, animation and combat. The host would then export little or no collision,
  and SkyCraft's collision subsystem would have no direct counterpart.
- Water: SkyCraft's `WaterGrid` (16x16 surface heights) has a direct counterpart if MGSV must swim in ER water
  [U: ER water query API]. MGSV has very little swimming (Venom wades or dies in deep water) [U].

---

## 11. `WorldRender.cpp` (skim): drawing the guest's world inside the host's D3D11 frame

### 11.1 Hooks [V:src WorldRender.cpp:3273-3304]

AE only. Each call-site hook first checks that the site is `E8` with the expected target; otherwise it falls back to
drawing at Present.

| Hook | Site | Purpose |
|---|---|---|
| `RenderWorldHook` | `REL::ID(36559) + 0x85E`, a call to `Main::RenderWorld` (`REL::ID(107142)`) | after it Skyrim's 3D scene is complete and post-processing (tone mapping, bloom, DoF, grading, TAA) hasn't run: `DrawInFrame()` |
| `SunShadowRenderHook` | `BSShadowDirectionalLight` vtable slot `0x0A` | copy the sun's shadow cascades before the shared shadow-map array is reused |
| `LatePassProbeHook` | `REL::ID(107142) + 0x2DF`, a call to `REL::ID(106438)` (FinishAccumulatingPostResolveDepth) | diagnostics only (logs what is bound) |

Other files' hooks, for completeness [V:src grep]: `PathAvoid.cpp` (call sites into `REL::ID(41642)`, NPC path
setup), `DigPhysics.cpp` (`bhkCharacterPointCollector`, `hkpAllCdPointCollector` vtable slot 1,
`bhkCharProxyController` vtable slot 1, and a call at `REL::ID(37507)+0xCEA`).

### 11.2 Injection technique (`DrawInFrame`, `WorldRender.cpp:2895-3020`) [V:src]

- Pulls Skyrim's own resources out of `BSGraphics::Renderer` runtime data: `renderTargets[kMAIN]` (the HDR scene;
  its RTV), `[kMOTION_VECTOR]`, `[kNORMAL_TAAMASK_SSRMASK]`, `[kSHADOW_MASK]`; `depthStencils[kMAIN]` (`views[0]`
  DSV, `depthSRV`), and its depth copies `kMAIN_COPY`, `kPOST_ZPREPASS_COPY` and `kPOST_WATER_COPY`.
- A one-time sanity check logs what is bound, and requires an HDR scene format (R16G16B16A16_FLOAT, R11G11B10_FLOAT
  or R32G32B32A32_FLOAT) and matching single-sample sizes. Otherwise in-frame drawing is turned off for good and the
  Present fallback is used.
- `CopyResource(depthCopyTex, depthTex)`, so Skyrim's depth can be **sampled** while the real depth buffer is bound
  for testing.
- **Camera**: Skyrim's own `NiCamera::worldToCam` re-based to camera-relative coordinates in double precision (the
  GPU only sees small numbers), the previous frame's matrix (for motion vectors), and near/far from `viewFrustum`.
  It checks itself once against `NiCamera::WorldPtToScreenPt3`.
- **Depth convention learned at runtime**: a 16-sample probe draws Skyrim's depth into a 16x1 target, copies it to
  staging and reads it back without stalling (`D3D11_MAP_FLAG_DO_NOT_WAIT`). Median < 0.5 means reversed Z; a flat
  result turns occlusion off. Re-probed every 5 s, or 0.5 s until known. The shadow mask is validated the same way.
- **Passes**: sun shadow map of the blocks; a multiply-blend full-screen pass that puts block shadows and contact AO
  on Skyrim's surfaces; opaque blocks and entities with **MRT** (scene + motion vectors + normals) and depth
  test+write into Skyrim's depth, so its TAA, SSAO and SSR treat them as Skyrim geometry; translucent water and glass
  (alpha blend, depth read, cull back); cracks and the selection outline (lines); then **depth-only into Skyrim's
  depth copies**, so later passes (fog, volumetric light, DoF) see the blocks.
- **Lighting from Skyrim**: the sun/moon `NiDirectionalLight` (ShadowSceneNode[0] `sunLight`, with fade),
  directional ambient (`Sky::directionalAmbientColors`, or the interior cell's lighting template DALC), fog, image
  space grading, and an exposure curve `1 - e^(-kx)` (`kExposure = 1.6`).
- `StateBackup` (`WorldRender.cpp:2244-2345`) saves and restores everything, including **all VS/PS/CS SRV slots**
  ("Binding a depth buffer or render target unbinds it wherever it's bound for reading; put every stage's resources
  back so Skyrim's renderer finds what it left").
- **Present-time half** (`Draw`, `WorldRender.cpp:3200-3271`): drains the render ring (48 MB budget; atlas,
  sections, lights, solids, dug, textures, avatar, scene, atlas regions, ragdoll), services readbacks, runs the
  depth probe, and, only if in-frame drawing didn't happen, draws on the finished back buffer with its own depth
  buffer while testing against Skyrim's depth SRV.
- Diagnostics: GPU timestamp queries per pass (6 frames in flight, no stalls) and a PNG capture when
  `skycraft_capture.request` exists in the log directory.

### Analog for MGSV -> Elden Ring

- If Venom's body is drawn **as an ER character** (option B in section 6), PhantomRing needs **no** in-frame world
  injection. That avoids reverse-engineering ER's D3D12 frame graph, descriptor heaps, resource states, async compute
  and render threads [U: ER's renderer internals are largely undocumented; task groups `GraphicsStep`, `DrawStep`,
  `GameSceneDraw` exist [V:src] but GPU submission happens on render threads].
- What would still need world-space drawing: MGSV-only visual effects (Fulton balloons, the D-Horse/D-Walker buddies,
  muzzle flashes, tracers, the Phantom Cigar). These map better onto **ER assets and SFX spawned natively**
  (`WorldSfxMan`, `CSSfx`, `spawn_geometry` RVA `0x6a5ed0` [V:src names]; that RVA starts
  `40 56 57 48 83 ec 58` on 2.7.1.0 [V:bin]) than onto injected draws. The old project
  already converted MGSV weapons and buddies to ER assets [V:src task context].
- SkyCraft's runtime-learning habits (depth convention probe, axis vote, one-time "what's bound" logs, fallback
  paths when a check fails) are worth copying for anything that touches ER's renderer.
- **Main difficulty**: none, if option B holds. Otherwise D3D12 mid-frame injection is the largest single risk.

---

## 12. Failure and safety behaviour (host side) [V:src]

| Situation | Detection | Host behaviour |
|---|---|---|
| Shared memory can't be created | `Link::Create` false | plugin disabled entirely; nothing hooked except CrashLog/Launcher |
| Guest not started yet / never connects | `McAlive()` false | no puppet; Skyrim plays normally (PlayerControls hook passes everything since `minecraftOwnsPlayer` is false); status notes (section 8.3); collision export paused (`haveMc` gate) |
| Guest crashes or freezes | heartbeat older than 3 s | `puppet` false -> Skyrim control restored the same frame; FOV restored; first-person meshes shown; essential cleared; actor table emptied; overlay not drawn; events still drained |
| Guest restarts | new `mcPid` or alive again | `ResetOverlay`, new collision epoch, teleport to Skyrim's position |
| Host paused (menus) | `AnyBlockingMenuOpen` / LoadingMenu | `ReleaseAll` to the guest; heartbeat continues from Present; overlay hidden; the guest stops pacing on `seq` when it sees Skyrim stalled |
| Host loading or teleport | `loading`, a >300-unit jump, world change | `teleportPending`; puppet off until the guest acks after landing; settle 1.5 s before collision export |
| Guest waiting in the wrong place | >8 blocks away for >1 s while arriving | re-teleport |
| Host-owned animation (furniture, mount, kill move, scene, bleedout, VATS) | `SkyrimTakeover` | puppet off; the guest is re-teleported when it ends |
| Host player dead | `IsDead()` | puppet off; Skyrim death camera; the guest body is put on Skyrim's ragdoll |
| Guest player dead | `kEvPlayerDied` | `KillImpl`/`KillImmediate` on the Skyrim player |
| Address mismatch (game update) | call-site `E8` target checks; `IsAE()` | hit pipeline -> `DoDamage` fallback; in-frame draw -> Present fallback |
| Bad Havok reads | SEH around shape reads | skip that shape and log once |
| Guest behind on rings | ring full | input dropped; collision retried 2000 ms then dropped; the event consumer skips ahead if lapped |
| Exceptions in hooks | try/catch around `PerFrame` and Present work | logged or swallowed; the original Present always runs |

### Analog for MGSV -> Elden Ring

Keep the same matrix. Additions for ER:

- clear **NoDead** whenever puppeting stops (otherwise the player becomes immortal);
- re-enable `CSInGamePad` and movement flags on every exit path, including the DLL's unload path;
- refuse to install anything unless the exe hash or version matches 2.7.1.0 (the old project's `HashExecutable`
  pattern);
- never touch EAC: me3 offline launch only.

---

## 13. Elden Ring facts checked in this session

All against `<steam-library>/common/ELDEN RING/Game/eldenring.exe` (87,042,128 bytes) using
`.../scratchpad/skycraft-host/erscan.py`.

| Fact | Result | Tag |
|---|---|---|
| exe version | VS_FIXEDFILEINFO file/product **2.7.1.0** | V:bin |
| Imports | `d3d12.dll`, `dxgi.dll`, `DINPUT8.dll`, `XINPUT1_4.dll`, `steam_api64.dll`, ... | V:bin |
| Physics | `hknpWorld` (10 hits), `hknpCompressedMeshShape` (14), `hknpHeightFieldShape` (5), `hknpCharacterProxy` (3), `hkpWorld` (1) | V:bin |
| GameDataMan | AOB `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3` unique at RVA 0x256560 -> static **0x3d61f98** (= fromsoftware-rs `game_data_man`) | V:bin |
| WorldChrMan | AOB `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88` unique at 0x3ffbf6 -> static **0x3d69ff8** | V:bin |
| GameMan | AOB `48 8B 05 ?? ?? ?? ?? 80 B8 ?? ?? ?? ?? 0D 0F 94 C0 C3` unique -> **0x3d6d988** (= `game_man`) | V:bin |
| FieldArea | AOB `48 8B 3D ?? ?? ?? ?? 49 8B D8 48 8B F2 4C 8B F1 48 85 FF` unique -> **0x3d6d248** (= `field_area_ptr`) | V:bin |
| CSMenuMan (CT, commented out) | AOB `48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24` unique -> 0x3d6f820 | V:bin (identity U) |
| `register_task` | 2.7.1.0 RVA 0xeb3e50 is a prologue; 2.7.0.0's 0xeb3de0 is not | V:bin |
| `display_status_message` 0x7671f0, `cast_ray` 0xc71e00, `apply_speffect` 0x3e8dc0, `obtain_dl_user_input_manager` 0x1328d0, `execute_action_button` 0xa641c0, `spawn_geometry` 0x6a5ed0 | prologue-like bytes | V:bin |
| `spawn_bullet` 0x3a2cb0 | target of `call` at 0x3a2c9e | V:bin |
| vtables `player_ins_vmt` 0x2a7fbb0, `chr_cam_vmt` 0x2a2aa08, `cscam_vmt` 0x2aa0a18, keyboard/mouse/pad/multi device, `csin_game_pad`, `csmenu_viewer_pad`, `cspad` | contain `0x1401...`/`0x1402...` code pointers | V:bin |
| CT free-camera AOBs (3) + inject `8b 93 c8 00 00 00 85 d2`; `LastHitNpcInfo`; `TargetedNpcInfo` `48 8B 48 08 49 89 8D` | each unique | V:bin |
| Arxan code-restoration pattern (regex equivalent) | ~181 matches | V:bin |
| On-disk `.text` is AOB-scannable (not packed) | all the AOBs above match | V:bin |
| MGSV `steam_appid.txt` | `287700`; `version_info.txt` `Tpp_steam_mst_en_day3900Mgo_patch_0707_1632` | V:bin |
| MGSV exe | odd sections (`.xtext`, `.edata`, `.data2`, `.didata`, `.trace`, `.xdata` 117 MB, `.ecode`), no import table visible to objdump; strings `D3D11CreateDevice`, `DirectInput8Create`, `GetRawInputData`, `steam_api64.dll` present; no `hkp`/`hknp`/`Havok` strings | V:bin |
| Prefixes | `compatdata/1245620/pfx` exists; `compatdata/287700` has only `config_info`; both `11.0-100` (Proton Experimental) | V:bin |
| Cross-prefix IPC | named section fails across prefixes (error 2), works within one; file-backed section works across; QPC (10 MHz) and GetTickCount64 agree; PIDs collide across prefixes; `CREATE_ALWAYS` on a mapped file gives 1224 | V:test (system wine-11.0) |

---

## 14. Summary: SkyCraft subsystem -> PhantomRing counterpart

| SkyCraft (Skyrim host) | PhantomRing (Elden Ring host) | Main difficulty |
|---|---|---|
| SKSE load + `kDataLoaded`; Address Library IDs | me3 natives; DLRF singletons, AOBs, fromsoftware-rs 2.7.1.0 RVAs; exe hash gate | no Address Library; Arxan (prefer tasks and vtables over inline patches) |
| `PlayerCharacter::Update` vtable hook | FD4 task in `WorldChrMan_PostPhysics` (via `register_task` 0xeb3e50) | which task groups run during loading and menus |
| `Local\SkyCraft_v1` named mapping, PIDs | **file-backed** mapping on a shared Unix path (or MGSV in ER's prefix); session nonces | separate Proton prefixes [V:test] |
| Coordinates: MC blocks, 70 units/block, Z-up vs Y-up | meters 1:1; stable `BlockId` + local or global space | ER world shifting; MGSV map bounds (re-basing) |
| Puppet: `SetPosition` + char controller tweaks; guest authoritative | `physics.position` (+0x70) + proxy update request; movement blocked by `ChrDebugFlags`/`CSInGamePad` gating | **who resolves collision**: MGSV (needs ER geometry in Fox physics) vs ER (hybrid) |
| Tick interpolation on the host's QPC clock | identical algorithm on MGSV frames (QPC agrees across prefixes [V:test]) | MGSV is per-frame, not fixed-tick |
| Teleport handshake (seq / ack-after-landing) | identical | many ER-owned movements (grace, lifts, Spiritsprings, Torrent, ladders, cutscenes) |
| Takeover (furniture, mount, scenes) | Torrent, ladders, throws and crits, cutscenes, grace, lifts | detection fields to RE |
| Camera: NiCamera root re-pin, FOV conversion, axis vote | `CSCamera` pers cams / `ChrCam` after `CameraStep`, or `CSDebugCam` (CT free-cam toggles) | which cam renders; FOV units |
| HUD hiding via Scaleform `SetVariable` | `CSFeManImp.hud_state`, `FrontEndViewValues` flags | per-element control |
| Input: `BSInputDeviceManager` sink; `PlayerControls`/`MenuControls` filters | DInput8 device vtables at `PadStep`; `CSInGamePad`/`CSMenuViewerPad` gating; ER key assignments for the allow-list | injection into a hidden, protected MGSV; key conflicts |
| Overlay: D3D11 Present vtable[8], dynamic texture, premultiplied alpha, invert pass, software cursor | D3D12 Present + ExecuteCommandLists hook, UPLOAD -> DEFAULT copy, full-screen triangle | D3D12 under vkd3d-proton; MGSV rendering **UI only** |
| WorldRender: blocks and avatar into Skyrim's HDR frame | **Venom as an ER character** posed from MGSV's skeleton (pose ring); MGSV FX as ER SFX/assets | skeleton retargeting (Fox -> ER) |
| Actor table from `highActorHandles` | `WorldChrMan.chr_inses_by_distance` | hit proxies inside MGSV |
| MC hit -> `HitData` + `ProcessHit` (IDs 38627/38586/43995) | spawn an ER bullet/attack (native damage pipeline) or SpEffect; HP write as fallback | damage scaling; non-lethal mapping |
| Player damage refund + essential flag | NoDead (`DataModule+0x19B` bit 0) + HP refund + `last_hit_by` | typed damage needs RE of ER's damage apply |
| Death: `kEvPlayerDied` -> `KillImpl`; ragdoll | hp = 0 with NoDead cleared; ER's YOU DIED/respawn; Venom body on the ER ragdoll is free with option B | scripted ER deaths -> MGSV |
| Collision export (hkp walk + voxelizer) | hknp walk or `cast_ray` grids, **only if** MGSV owns collision | hknp layouts; Fox physics injection |
| Launcher: mutex, Prism, Explorer `ShellExecute` | Linux launcher script or `CreateProcess` from ER (same prefix) | Steam/DRM, protected MGSV exe |
| Status: `SendHUDMessage` free text | own overlay text (ER shows FMG IDs only) | none |
| CrashLog / Perf | same | chain with Arxan/me3/Seamless handlers |

**Recommended architectural decision to settle early (from this read):** SkyCraft works because Minecraft can be
modded with Mixins at exactly the seams needed: collision queries, input handlers, render layers. For MGSV those
seams are in a protected binary. The guest-authoritative model (MGSV owns position and physics against ER geometry)
needs ER collision inside Fox's physics, input injection, and a UI-only render, all at once. A **hybrid** (ER owns
collision via its character proxy; MGSV owns intent, animation pose, weapons, HUD and health) keeps SkyCraft's
link, interpolation, handshake, input routing, overlay and combat bridging nearly unchanged, and replaces SkyCraft's
two largest subsystems (Collision, WorldRender) with "drive the ER proxy" and "pose an ER Venom" [U: design
judgement].

## 15. Open questions for follow-up research

1. Which `CSCamera` pers cam does ER render from; FOV units; does `CSDebugCam` leave gameplay intact? (camera)
2. Safe ways to stop the Tarnished's own locomotion and actions while keeping AI targeting and aggro normal
   (`ChrDebugFlags` bits, `CSInGamePad.allow_polling`, `ChrCtrl` flags). (puppet)
3. The ER damage-application function (typed damage, cancellable) and the exact NoDead/NoDamage offsets on 2.7.1.0.
   (combat)
4. ER loading-state detection and which task groups run during loading screens. (link/safety)
5. Global/block coordinate conversion and world-shift events. (coordinates)
6. MGSV: protector type; input layer; whether it can run hidden and unfocused without pausing; whether its HUD can be
   rendered alone; character-controller/physics engine; skeleton layout for retargeting. (guest-side, separate
   reports)
7. Re-run the cross-prefix IPC probe under Proton Experimental itself (`ipctest.exe` is in the scratchpad).
