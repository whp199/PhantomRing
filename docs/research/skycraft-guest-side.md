# SkyCraft guest side (Minecraft Fabric mod): how it works, and the MGSV analogs

Research report for **PhantomRing** (MGSV *guest*, Elden Ring *host*). Written 2026-10-11.

This report reads SkyCraft's **guest** half: the Minecraft Fabric mod in
`<SkyCraft>/fabric/src` and the tools in `<SkyCraft>/tools`.
It reads the host (SKSE) code only where the guest's contract depends on it. Each mechanism ends with
**"Analog for MGSV (guest)"**: what our `mgsvtpp.exe` DLL would have to do, and the main difficulty.

## Verification legend

| Tag | Meaning |
|---|---|
| **[V:src]** | VERIFIED by reading the local SkyCraft source (file and line given where useful) |
| **[V:bin]** | VERIFIED against local bytes: `mgsvtpp.exe`, the extracted MGSV Lua, or the Proton prefix files |
| **[V:test]** | VERIFIED by a test program run in this session (system Wine and both Proton Experimental builds, in scratch prefixes only; no game or Steam was launched) |
| **[U]** | UNVERIFIED: from memory, inference or documentation, not checked locally |

Unless a path is given, SkyCraft paths are relative to `<SkyCraft>/`.
`fabric/.../client/X.java` means `fabric/src/client/java/dev/skycraft/client/X.java`, and
`fabric/.../X.java` under `main` means `fabric/src/main/java/dev/skycraft/X.java`.

Versions in the repo **[V:src]**: protocol `kVersion = 11` (`protocol/skycraft_protocol.h`, `link/Proto.java`),
Minecraft 26.3, Fabric loader 0.19.5, fabric-api 0.161.0+26.3, mod version 0.1.2 (`fabric/gradle.properties`).

---

## 0. TL;DR for PhantomRing

1. **The guest is a passive client of a host-owned mapping.** SkyCraft's guest opens it, checks magic and version,
   writes its PID and heartbeat, and talks only through:
   - seqlock "latest value" slots: SkyState, McState, the actor table, world entities, the water grid
   - SPSC rings: input (host→guest), events (guest→host), collision (host→guest), render (guest→host)
   - a lock-free overlay triple buffer

   There are **no named events**. Everything is polled **[V:src]**: a grep for `CreateEvent|SetEvent|WaitOnAddress`
   finds none, although `docs/DESIGN.md` §10 promises them.
2. **SkyCraft's transport does not work for us as written.** Our two games run in **separate Proton prefixes**, so
   separate wineservers. A `Local\…` named mapping made in one prefix is **invisible** in the other
   (`OpenFileMappingW` → error 2) **[V:test]**. What does work is a **file-backed mapping on a Unix path**
   (`Z:\dev\shm\<file>`): it is coherent across both prefixes *and* a native Linux `mmap` **[V:test]**. Two more
   results from the same test:
   - Wine PIDs **collide** across prefixes (both test processes were pid 32 under system Wine) **[V:test]**.
   - Proton's `QueryPerformanceCounter` is **CLOCK_MONOTONIC_RAW in 100 ns units**, so QPC stamps are comparable
     across both games and Linux Python **[V:test]**.
3. **SkyCraft's testing model carries straight over, and it gets easier for us.** `fake_skyrim.py` and
   `fake_guest.py` each stand in for the other game by speaking the wire format with `struct`. With a `/dev/shm`
   mapping, our fakes (`fake_er.py`, `fake_mgsv.py`) can run **natively on Linux**, against a DLL running in either
   game or inside a tiny Wine test EXE in a scratch prefix (feasibility shown in §12).
4. **Every SkyCraft Mixin becomes a native hook in MGSV.** `mgsvtpp.exe` facts **[V:bin]**:
   - fixed ImageBase 0x140000000, relocations stripped, no DYNAMIC_BASE
   - imports `XInputGetState` (XINPUT1_3 ordinal 2), `GetRawInputData`, `GetForegroundWindow`, `D3D11CreateDevice`
   - statically links Lua 5.1.5
   - has its own physics (`Ph*`) and collision/query system (`Geo*`, `GeoSearch*`, `GeoxDynamicPolygon`), not Havok

   The cheap hooks are IAT slots (input, focus, cursor) and the embedded Lua (state reads and writes, warps, damage
   messages). The expensive ones need reverse engineering: the Geo collision queries, the player skeleton, and the
   damage functions.
5. **The parts that change most:**
   - **Look/camera ownership.** SkyCraft integrates the mouse on the host for zero latency. MGSV's orbit and aim
     camera is game logic, so the guest should probably own it.
   - **The avatar.** Minecraft is first-person, so SkyCraft barely needs a body. Venom is always on screen, so we
     need a per-frame skeleton stream that drives a pre-converted Venom model in Elden Ring.

---

## 1. Guest architecture and per-frame call graph

### 1.1 Entry points [V:src]

| Hook | Where | Calls |
|---|---|---|
| `ModInitializer.onInitialize` | `fabric/.../SkyCraft.java` | `SkyCombat.init()` (registers the proxy entity type, its attributes and `END_SERVER_TICK`), `SkyNet.init()` (MC-multiplayer packets), `SkyDig.init()`, `SERVER_STARTED → configureServer` (gamerules), join → starter and builder kits |
| `ClientModInitializer.onInitializeClient` | `client/SkyCraftClient.java` | `SkyLink.announceRunning()` (named mutex), Discord, destruction toggle, `/join` and `/leave`, `END_CLIENT_TICK → SkyClient.clientTick`, LAN env vars, `EntityRendererRegistry.register(SKYRIM_ACTOR, NoopRenderer::new)`, `SkyCollision.setSmoothCollider(player && linked)` |
| `@Inject Minecraft.runTick HEAD` | `client/mixin/MinecraftMixin.java` | `SkyClient.beginFrame()` |
| `@Inject Minecraft.renderFrame`, right after `GameRenderer.render()` | same | `SkyClient.afterRender()` |
| `@Inject Minecraft.renderFrame TAIL` | same | `SkyClient.paceFrame()` |
| Daemon thread "SkyCraft collision" | `world/SkyCollision.startConsumer()` | drains the collision ring every ≤2 ms |
| Server thread (integrated server) | `combat/SkyCombat.serverTick` and server mixins | proxies, hits, deaths, explosions, skill use |

### 1.2 One Minecraft frame while linked [V:src]

```
runTick HEAD ── SkyClient.beginFrame()
   SkyLink.poll()                    open the mapping (≤1/s) / write MC heartbeat / detect a new Skyrim PID
   quitWithSkyrim()                  exit if Skyrim's PID is gone for >5 s
   hide the window + mute music      once, if -Dskycraft.startHidden=true
   SkyLink.active()?                 Skyrim heartbeat younger than 8000 ms
   readSkyState(sky)                 seqlock; a torn read keeps last frame's copy
   SkyWater.refresh()                seqlock water grid
   on link up:   tookOver = true, start the collision consumer, applyLinkedOptions()
   on link down: InputBridge.releaseAll(), remember a freeze position
   hideWindowOnce / applyViewportSize / MirrorWorld.openWhenReady
   if a Skyrim menu or loading screen is up: releaseAll()
   InputBridge.drain()               replay host input into Minecraft's handlers
   ProxySync.frame()                 snap client-side NPC proxies to this frame's actor table
   teleport handling                 teleportSeq / new player object → setPos + server teleportTo
   look:                             player.setYRot/XRot (and the *O fields) = sky.yaw/pitch, unless a GUI screen is open
 … Minecraft runs 0..n client ticks …
   END_CLIENT_TICK → SkyClient.clientTick()
      MirrorWorld.tick, Discord, SkyDigClient.tick
      freezeWhileUnlinked()          link dropped after takeover: pin the player in place
      holdUntilReady()               after a teleport: pin until collision has arrived
      publishTick()                  raw 20 Hz physics tick + QPC stamp → McState
 … GameRenderer.render() (the level pass is cancelled; hand and GUI only) …
renderFrame ── SkyClient.afterRender()
   build McState (interpolated feet, eye, fov, camera mode, bob, flags, frameCounter) → writeMcState
   WorldExporter.frame()             block meshes, atlas, entities, avatar → render ring + world-entity table
   FrameExporter.capture()           async GPU→CPU copy of the GUI/hand frame → overlay triple buffer
renderFrame TAIL ── SkyClient.paceFrame()
   spin/yield until SkyState.seq/2 changes (one MC frame per Skyrim frame), 25 ms deadline
```

### Analog for MGSV (guest)

We need the same three per-frame hook points plus a tick hook, inside `mgsvtpp.exe`.

- **Frame end, which also paces:** `IDXGISwapChain::Present`. Under Proton, D3D11 is DXVK. The swap chain comes from
  `D3D11CreateDevice` + `CreateDXGIFactory1`; their IAT slots are VA `0x14208e080` and `0x14208e090` **[V:bin]**.
  Hook the factory's `CreateSwapChain*` or the swap-chain vtable after creation **[U]**.
- **Frame begin and the game-logic tick:** `TppMain.OnUpdate` exists in the extracted Lua and runs the per-frame
  update lists (`master__0__00.dat/Assets/tpp/script/lib/TppMain.lua:382`) **[V:bin]**. It runs every frame **[U]**.
  The DLL can wrap it from injected Lua without touching any archive:
  `local o=TppMain.OnUpdate; TppMain.OnUpdate=function(...) PR.Frame() return o(...) end`.
  That needs the Lua C API located in the binary (§2 analog). A native per-frame function found by reverse
  engineering is the alternative.
- **Worker thread** for the host→guest collision ring, like `SkyCollision.consumeLoop`.
- **Main difficulty:** Minecraft hands Fabric a named, stable `runTick`/`renderFrame`. Fox gives us no named hook
  points. We must find:
  - the Lua state and `lua_pcall`-family functions
  - the main-loop or game-update function
  - Present (easy)

  Prior art: TinManTex's **IHHook**, a dinput8 proxy that hooks MGSV 1.0.15.x Lua functions using per-version
  address tables, used by Infinite Heaven **[U]**.
- **Injection without touching the install:** the rules forbid dropping a proxy `dinput8.dll` next to
  `mgsvtpp.exe`, although DINPUT8 is imported **[V:bin]**. Options:
  - a launcher that starts `mgsvtpp.exe` suspended and injects (the me3-style approach), run inside the MGSV prefix
    via `proton run` or a Steam launch-option wrapper **[U]**
  - `WINEDLLOVERRIDES` plus a DLL search path outside the install **[U]**

  The launch path is a design decision for the host/launcher report.

---

## 2. The shared-memory link

### 2.1 Opening, validation, heartbeat, instance changes [V:src]

`fabric/src/main/java/dev/skycraft/link/SkyLink.java`:

- **The host creates the mapping, the guest opens it.**
  - `OpenFileMappingW(FILE_MAP_ALL_ACCESS=0xF001F, FALSE, L"Local\\SkyCraft_v1")` through Java FFM (`Linker`),
    capturing `GetLastError` (`SkyLink.java:46-48,129`).
  - Then `MapViewOfFile` of the whole mapping, `reinterpret(MAPPING_BYTES)`.
  - It tries at most **once per second** (`lastOpenAttempt`) and logs each Windows error once: 2 means not created
    yet, 5 means Skyrim is elevated.
- **Validation:** `magic == 0x43594B53` ("SKYC") and `version == 11`. A mismatch is logged and the mapping is not
  used.
- **On success** the guest writes `Header.mcPid = GetCurrentProcessId()` and `mcHeartbeatMs = GetTickCount64()`
  (`setRelease`), and remembers `skyrimPid`. It also bumps `generation`, which tells guest caches (atlas, meshes) to
  resend everything.
- **Every frame** (`poll()` with an open mapping): rewrite `mcHeartbeatMs`. If `Header.skyrimPid` changed, Skyrim
  restarted: reset `overlayBack = 1` and `generation++`.
- **Liveness:** `active()` is `GetTickCount64() - skyrimHeartbeatMs < 8000`. The comment says Skyrim's loading
  screens can stall its heartbeat for seconds. The host's view of the guest uses **3000 ms**
  (`skse/src/Link.cpp:15 kMcTimeoutMs`), and so does `fake_skyrim.py mc_alive()`.
- **Host-side creation** (`skse/src/Link.cpp:62-112`):
  - explicit DACL so an elevated Skyrim doesn't lock out the non-elevated MC
  - `CreateFileMappingW(INVALID_HANDLE_VALUE, …, kMappingBytes, L"Local\\SkyCraft_v1")`
  - **memset of every host-owned region** (SkyState, overlay control, ring heads and tails, actor table, world
    entities, render ring header), because a stale mapping survives if MC still holds it
  - then `version`, `skyrimPid` and heartbeat, and **`magic` last with release ordering**
- **Single-instance guard:** `SkyLink.announceRunning()` creates the named mutex `Local\SkyCraft_v1_minecraft` and
  holds it for the process lifetime. The SKSE launcher checks it with `OpenMutexW` before starting another
  Minecraft (`skse/src/Launcher.cpp:214-219`).
- **Clock:** `SkyLink.qpc()` / `qpcFrequency()` call `QueryPerformanceCounter` directly, "the same clock Skyrim
  reads", so tick timestamps line up across processes.
- **Test hook:** `Proto.MAPPING_NAME = System.getProperty("skycraft.link", "Local\\SkyCraft_v1")` lets a second
  client use its own link (`Local\SkyCraft_guest`, see `fake_guest.py`).

### 2.2 Memory layout and ownership (protocol v11) [V:src]

| Offset | Region | Size | Writer → reader | Sync |
|---|---|---|---|---|
| 0x0 | Header {magic, version, skyrimPid, mcPid, skyrimHeartbeatMs, mcHeartbeatMs} | 0x20 | both | release/acquire stores |
| 0x100 | SkyState | 0x40 | Skyrim → MC | seqlock |
| 0x200 | McState | 0xC8 | MC → Skyrim | seqlock |
| 0x300 | OverlayCtl {state, pad, framesPublished} | 0x10 | MC ↔ Skyrim | atomic xchg |
| 0x340 | 3 × OverlaySlotHdr {w, h, flags(bit0 bottom-up), frameId} | 3×0x40 | MC → Skyrim | owned by the slot holder |
| 0x400 | WaterGrid {seq, originX, originZ, worldId, float[16×16]} | ≤0xC00 | Skyrim → MC | seqlock (seq==0: never written) |
| 0x1000 | Input ring: head@+0, tail@+0x40, data@+0x80, 4096 × 16 B `InputEvent{u16 type, u16 code, i32 a, b, c}` | 0x10080 | Skyrim → MC | SPSC |
| 0x12000 | ActorTable {seq, count, pad→0x40, 256 × 64 B ActorRecord} | 0x4040 | Skyrim → MC | seqlock |
| 0x17000 | Event ring: head, tail, data, 512 × 32 B `McEvent{type, formId, a, b, c, d, flags, weapon}` | 0x4080 | MC → Skyrim | SPSC |
| 0x1C000 | WorldEntities {seq, count, hasSelection, selMin[3], selMax[3], 160 × 96 B} | 0x3C40 | MC → Skyrim | seqlock |
| 0x20000 | Collision byte ring (32 MiB) | 32 MiB | Skyrim → MC | SPSC byte ring |
| 0x2020000 | Overlay pixels, 3 slots × 3840×2160×4 = 33,177,600 B each | 99.5 MB | MC → Skyrim | triple buffer |
| 0x7F0C000 | Render byte ring (64 MiB) | 64 MiB | MC → Skyrim | SPSC byte ring |
| **total** | `kMappingBytes` = **200,327,168 B (191.05 MiB)** (computed from the constants) | | | |

Seqlock implementation:

- **Java writer** (`writeMcState`, `writeWorldEntities`): read seq, `setRelease(seq+1)`, `storeStoreFence`, plain
  field writes, `setRelease(seq+2)`.
- **Java reader** (`readSkyState`, `readActors`, `readWaterGrid`): `getAcquire(seq1)`; if odd, spin; plain reads;
  `loadLoadFence`; `getAcquire(seq2)`; retry if seq1 ≠ seq2. Attempts: 1000 for SkyState (`Thread.yield` after 100),
  16 for actors, 100 for water. On failure the caller keeps last frame's copy.
- **Host C++** (`Link.cpp`): the same protocol with `std::atomic_ref` and `atomic_thread_fence`.

SPSC rings:

- head and tail are u64 *monotonic totals* on separate 64-byte cache lines (+0x00, +0x40); data starts at +0x80.
- **Fixed-entry rings** (input, events) mask with `entries-1`. The producer drops when `head-tail >= entries`. The
  consumer skips ahead if lapped (`SkyLink.drainInput`, `Link::PopEvent`).
- **Byte rings** (collision, render) carry messages starting 8-byte aligned with `{u32 type, u32 payloadBytes}`. A
  `type 0` PAD means "skip to the ring start". A message may be at most half the ring.
  `SkyLink.writeRender` retries every 2 ms for up to 500 attempts (`tryWriteRender`: 1 attempt, for per-frame data
  the next frame replaces).

Overlay triple buffer (header comment and `SkyLink.publishOverlay` / `Link::AcquireOverlayFrame`):

- `state` bits 0-1 hold the "middle" slot index; bit 2 (`kOverlayDirty=4`) means the middle holds an unread frame.
- The writer starts with back slot 1, the reader with front slot 2, and the middle is 0.
- **Writer:** fill the back slot (pixels and slot header), then `old = xchg(state, back|DIRTY)`, then
  `back = old & 3`, then `framesPublished++`.
- **Reader:** only if DIRTY: `old = xchg(state, front)`, then `front = old & 3`.
- **Reset:** when MC reconnects, the host stores `state=0` and `front=2`. When Skyrim's PID changes, the guest sets
  `back=1`.

### Analog for MGSV (guest)

- **Transport (must change).** Use a **file-backed shared mapping on a Unix path both prefixes can see**. Each side
  calls `CreateFileW("Z:\\dev\\shm\\phantomring_v1.bin", RW, share RW, OPEN_ALWAYS)`, sets the size, then
  `CreateFileMappingW(hFile, …)` + `MapViewOfFile`. In this session's test it was coherent across:
  - two system-Wine 11.0 prefixes
  - the two different Proton Experimental builds the games actually use (ER's prefix points at
    `~/.local/share/Steam/steamapps/common/Proton - Experimental`, MGSV's at
    `<steam-library>/common/Proton - Experimental`; both report `11.0-100` in
    `compatdata/*/version`) **[V:bin]**
  - a native Linux Python `mmap` of `/dev/shm/…`

  All three readers saw all ~2,800 counter updates in 3 s **[V:test]**. Named `Local\…` objects do **not** cross
  prefixes **[V:test]** (mappings). The same holds for mutexes and events by the same per-wineserver namespace
  **[U]**, so SkyCraft's mutex guard and any named-event wakeups are unusable. Keep SkyCraft's polling design.
- **Instance identity:** replace "PID changed ⇒ generation++" with a **random 64-bit session nonce** written by the
  host at creation, plus a guest nonce. Wine PIDs are small per-wineserver numbers and collided (32 and 32) in the
  test **[V:test]**. `ProcessHandle.of(pid)`-style "quit when host dies" cannot work across prefixes either. Use
  heartbeat staleness instead, and optionally the host's Unix PID for a Linux launcher to watch **[U]**.
- **Clocks:** Proton's QPC frequency is 10,000,000 and its value tracks `CLOCK_MONOTONIC_RAW` (median offset
  0.28 ms, min 0.001 ms). `CLOCK_MONOTONIC` was off by about −180 ms. System Wine 11.0 instead tracked
  `CLOCK_MONOTONIC` (min 0.001 ms). `GetTickCount64` agreed across both Proton prefixes (difference 0 ms)
  **[V:test]**. So the protocol can keep QPC stamps, and Linux-side fakes must stamp with
  `clock_gettime_ns(CLOCK_MONOTONIC_RAW)//100` when talking to Proton processes.
- **Layout:** keep the same region types: seqlock slots, SPSC rings, overlay triple buffer. MGSV is a native C++ DLL
  (MinGW 16 or clang-cl), so it can share one C++ header with the ER DLL, and the 32-bit and 64-bit atomics map
  1:1. Generate the Python struct definitions from that header (§12).
- **Main difficulty:** none conceptually. This is the best-understood part. The risk is only in getting the
  cross-prefix details right: nonces, clocks, and the reset ordering (memset first, magic last).

---

## 3. How the guest paces itself

[V:src]:

- **Frame lockstep** (`SkyClient.paceFrame`, `SkyClient.java:403-422`): at the end of every MC frame, spin until
  `SkyState.seq >>> 1` changes, which is one step per Skyrim frame because seq goes up by 2 per write. Deadline
  **25 ms**; `Thread.onSpinWait()`, plus `Thread.yield()` while more than 2 ms remain. If the deadline passes without
  a change, `skyrimStalled = true`, and following frames skip the wait until seq moves again. That covers a paused
  Skyrim (menus, alt-tab) without blocking every frame.
- **Minecraft's own limiters are disabled while linked:**
  - `FramerateLimitTrackerMixin.getFramerateLimit → 260`
  - `applyLinkedOptions`: `enableVsync=false`, `framerateLimit=260`
- **Heartbeat:** written every frame from `poll()`. The guest treats Skyrim as gone after 8 s, Skyrim treats MC as
  gone after 3 s.
- **Frame counter:** `McState.frameCounter = ++frameCounter` per rendered MC frame (`afterRender`). The diagnostics
  `tick_monitor.py` and `trace_motion.py` use it to detect MC frames and >20 ms stalls. The host paces nothing on it.
- **Tick timing:** `publishTick` stamps each 20 Hz physics tick with
  `QPC − partialTick × tickMs × qpcFreq / 1000`: the time the tick really happened, correcting for
  `DeltaTracker`'s remainder. `tickMs` comes from `level.tickRateManager()` (50 unless `/tick rate`).

### Analog for MGSV (guest)

- **Pace in the Present hook:** after MGSV submits its frame, wait (spin+yield, 25 ms cap, stall latch) for the
  host's frame sequence to advance. Elden Ring and MGSV both default to a 60 fps cap **[U]**, so the lockstep should
  cost little.
- **Throttle MGSV's GPU cost**, because two AAA renderers share one GPU (our analog of
  `renderDistance=8 / simulationDistance=8`):
  - lowest graphics settings in a **separate MGSV config** (MGSV keeps its settings in its own files; location
    **[U]**)
  - a small internal resolution, since we don't display MGSV's 3D frame
- **Tick vs frame:** Minecraft has a clean 20 Hz fixed tick, so the host can interpolate exactly like Minecraft's
  renderer does. Fox's player update is per frame with a variable dt **[U]**. Publish per-frame samples with QPC
  stamps and let the host use the same "render slightly in the past and interpolate between stamped samples"
  history (host `Game.cpp:660-783`). That algorithm does not need a fixed rate, only stamps.
- **Main difficulty:**
  - MGSV's own frame limiter and v-sync must not fight the pacing. Under DXVK, `dxvk.conf` /
    `DXVK_FRAME_RATE`-style settings can be applied per prefix via environment, without touching the install
    **[U]**.
  - Blocking in Present stretches MGSV's frame dt. That's harmless if Fox uses real dt **[U]**.

---

## 4. Keeping the guest hidden, "focused", silent, and alive

[V:src]:

| What | How (SkyCraft) |
|---|---|
| Start hidden | The bundled Prism instance passes `-Dskycraft.startHidden=true` (`tools/minecraft-bundle/Prism/instances/SkyCraft/instance.cfg: JvmArgs=--enable-native-access=ALL-UNNAMED -Dskycraft.startHidden=true`). On the first `beginFrame`: `SDL_HideWindow(window.handle())`, MUSIC volume 0, `getMusicManager().stopPlaying()` (no title-screen music). Without the flag, the window hides when the link first comes up. `-Dskycraft.showWindow=true` keeps it for debugging |
| Pretend focus | `WindowMixin.isFocused → SkyClient.linked()` once taken over; `isIconified → false` while linked. `options.pauseOnLostFocus=false` |
| Never touch the real mouse | `InputConstantsMixin.grabMouse` / `releaseMouse` cancelled after takeover; `InputConstantsMixin.isKeyDown → InputBridge.isKeyDown()` (virtual keyboard) |
| Never pause | `ScreenMixin.isPauseScreen → false` while linked: Skyrim keeps running while an MC screen (inventory, options) is open, so MC must too. Arrows keep flying and NPC hits land |
| Don't draw the world | `LevelRendererMixin`: cancel `LevelRenderer.render(...)` at HEAD while linked: no sky, fog, terrain or entities, so the GUI and hand sit on a transparent clear |
| Don't wait for chunk meshes | `WaitingForPlayerChunkMixin.isReady → true` (the level is never rendered) |
| Size | `applyViewportSize`: `window.setWindowed(min(sky.viewportW, 3840), min(sky.viewportH, 2160))`, so the overlay matches Skyrim's back buffer |
| Options | `applyLinkedOptions()`: no vignette, no vsync, framerate 260, render/simulation distance 8, autoJump off, onboarding off, tutorial NONE, music 0, `options.save()` |
| Sound | **Only music is muted.** Minecraft's other sounds (swing, eat, place, bow, hurt, digging) keep playing from the hidden process. SkyCraft relies on that: `SkyrimActorEntity.getHurtSound → null` because "Skyrim plays the NPC's own pain sounds", while the player's own sounds stay Minecraft's |
| Lifetime | `quitWithSkyrim`: every 1 s, `ProcessHandle.of(skyrimPid).isAlive()`; gone for >5 s → `minecraft.stop()` (saves). Started hidden but never connected in 10 min → quit, so it doesn't block the next launch. `-Dskycraft.quitWithSkyrim=false` for dev |
| Unlinked safety | `freezeWhileUnlinked`: if the link drops after takeover, pin the player (position, `xo/yo/zo`, velocity 0, fall distance reset) instead of letting them fall through collision that is about to change |
| Launch | The host's `Launcher.cpp` (`OpenFromDesktop`) starts Prism through Explorer: `IShellWindows::FindWindowSW(CSIDL_DESKTOP)` → `IShellDispatch2::ShellExecute`. So Minecraft is not a child of Skyrim/MO2 and is never elevated |

### Analog for MGSV (guest)

| SkyCraft | MGSV DLL equivalent | Difficulty |
|---|---|---|
| `SDL_HideWindow` | Hook `CreateWindowExW` (IAT slot VA `0x14208dde8` **[V:bin]**) to drop `WS_VISIBLE`, and/or `ShowWindow(hwnd, SW_HIDE)` after creation (`ShowWindow` is imported **[V:bin]**; its IAT slot wasn't extracted). Fallback: move it off-screen and remove it from the taskbar | Under DXVK, Present on a hidden or occluded window may stall or return `DXGI_STATUS_OCCLUDED` **[U]**; test first in a scratch prefix with a D3D11 sample |
| `isFocused=true`, `pauseOnLostFocus=false` | Hook `GetForegroundWindow` (IAT VA `0x14208dd28` **[V:bin]**) to return MGSV's hwnd. Subclass the WndProc (`SetWindowLongPtrW(GWLP_WNDPROC)`), drop `WM_ACTIVATEAPP(FALSE)`/`WM_ACTIVATE(WA_INACTIVE)`/`WM_KILLFOCUS`, and inject `WM_ACTIVATEAPP(TRUE)` | Whether and how MGSV pauses or mutes on focus loss is **[U]**; find by test or reverse engineering |
| `isIconified=false` | Hook `IsIconic` (IAT VA `0x14208de40` **[V:bin]**) → FALSE | trivial |
| cancel `grabMouse`/`releaseMouse` | Hook `SetCursorPos` (VA `0x14208deb0`), `ShowCursor` (VA `0x14208deb8`) and `ClipCursor` (not in the extracted import lists, maybe absent **[U]**), so the hidden MGSV never warps or confines the real cursor that ER needs | trivial once hooked |
| `isPauseScreen=false` | Never let MGSV's **pause menu** open: route Esc to ER (§5). Some Lua uses `Player.SetPause` / `UnsetPause` and `TppPauseMenu.SetIgnoreActorPause` (names **[V:bin]** in extracted Lua); avoid states that pause actors. iDroid in the field is real-time **[U]** | medium: MGSV has more modal UI (iDroid, cassette, weapon wheel) |
| Skip level rendering | No cheap equivalent: Fox will render its whole world. Options: lowest settings + tiny resolution; or, after reverse engineering via RenderDoc, skip the scene passes. See §7 for the HUD problem | hard |
| Mute music, keep SFX | Lua `TppSoundDaemon.SetMute"<name>"` / `ResetMute`, `TppMusicManager.StopMusicPlayer` / `EndSceneMode` (all **[V:bin]** names in extracted Lua; mute names seen: `Loading`, `Outro`, `Result`, `Telop`). Or Wwise bus volumes by native reverse engineering. Or, on Linux, mute the whole MGSV PipeWire stream (`pactl set-sink-input-mute`) if we decide MGSV's SFX shouldn't play at all | Choosing **what** to keep is the real design question: Snake's gunshots, CQC and footsteps (keep?) versus MGSV world ambience, radio and music (must go, since MGSV's world is not the one on screen). MGSV's audio is Wwise **[U]**; the exe has `AK::` strings **[V:bin]** |
| `quitWithSkyrim` (PID) | Heartbeat-based: no host heartbeat for N s plus a host "shutdown" flag ⇒ MGSV DLL exits the process (or the Linux launcher kills it) | PIDs are useless across prefixes **[V:test]** |
| Launch hidden | ER's DLL (ER prefix) cannot `CreateProcess` MGSV *into the MGSV prefix*. A **Linux-side launcher** must start MGSV through Steam (`steam://rungameid/287700`) or `proton run` with `STEAM_COMPAT_DATA_PATH=…/287700` **[U]**. MGSV imports `SteamAPI_Init` (**[V:bin]**: steam_api64.dll with 13 imports), so it may need the Steam client running (it is) and may relaunch itself through Steam **[U]** | medium |
| Single instance | Lock or nonce in the `/dev/shm` header (the guest writes its nonce and heartbeat; the launcher checks both) | easy |
| `freezeWhileUnlinked` | Pin Venom: block input (`Player.SetPadMask{settingName=…, except=true}` **[V:bin]** Lua name, or a zeroed virtual pad) and keep re-warping or holding position | medium |

---

## 5. Input from the host → the guest's input handlers

### 5.1 Host side contract [V:src] (`skse/src/Input.cpp`)

- An `RE::BSInputDeviceManager` event sink sees all raw input.
- While Minecraft drives the player (`st.puppeting && !menuOpen`):
  - **Keyboard:** the DirectInput scan code goes through a 256-entry `kDikToSdl` table to an **SDL scancode** (USB
    HID usage). The host pushes `kInKey{code=sdl, a=1/0}`.
  - **Keys Skyrim keeps:** Esc, console (`~`), J, M and F9 go to Skyrim's menus. **G** = Skyrim Activate on the
    crosshair target. **H** = Wait menu. **O** = `kInOpenMenu` (MC pause menu). With an MC screen open, *every* key
    goes to MC.
  - **Mouse buttons:** Skyrim codes map to SDL buttons (`{1,3,2,4,5}` for codes 0..4). Wheel → `kInScroll a=±120`.
  - **Mouse movement:**
    - Gameplay: accumulated host-side (`lookDx/lookDy`) and integrated into yaw/pitch **by the host** with
      Minecraft's formula, `s = sens·0.6+0.2; factor = s³·8·0.15`, pitch clamped ±90 (`Game.cpp:601-607`). The
      result goes to MC as `SkyState.yaw/pitch`. **MC never receives gameplay mouse deltas.**
    - With an MC screen open: an absolute virtual cursor, clamped to the viewport → `kInCursor{a=x, b=y}`. Typed
      characters → `kInText{a=codepoint}`.
  - A `PlayerControls` vfunc hook (vtbl index 1) swallows input to Skyrim's own player controls.
  - A `MenuControls` hook passes only the allow-listed keys to Skyrim's menu opener.
- When a Skyrim menu or loading screen appears: `kInReleaseAll`.
- Damage to the player also rides this ring: `kInHurt` (§9), so it is ordered with input.

### 5.2 Guest side [V:src] (`client/InputBridge.java`, `client/mixin/InputConstantsMixin.java`)

- `drain()` runs once per frame at the start of `runTick` (before client ticks). It walks the input ring from tail to
  head and dispatches:
  - `IN_KEY`: keeps a **virtual keyboard** `boolean[512]` and recomputes SDL modifier bits (LShift 0x1, RShift 0x2,
    LCtrl 0x40, RCtrl 0x80, LAlt 0x100, RAlt 0x200). Action = 1 press, 0 release, **−1 repeat** (pressed while
    already down). Calls `keyboardHandler.keyPress(windowHandle, action, new KeyEvent(scancode,
    SDL_GetKeyFromScancode(scancode, mods, true), mods))`: Minecraft's own SDL callback path, as if its window had
    focus.
  - `IN_MOUSE_BUTTON` → `mouseHandler.onButton(handle, new MouseButtonInfo(button, mods), 1/0)`. The first 20
    clicks log Minecraft's `hitResult`.
  - `IN_SCROLL` → `mouseHandler.onScroll(handle, 0, a/120.0)`.
  - `IN_CURSOR` → `mouseHandler.onMove(handle, x, y, dx, dy)` with dx/dy from the last position.
  - `IN_TEXT` → `keyboardHandler.textInput(handle, codepoint)`, only when a GUI screen is open.
  - `IN_RELEASE_ALL` → `releaseAll()`: synthesizes releases for every held key and button.
  - `IN_HURT` → §9. `IN_OPEN_MENU` → `setScreen(new PauseScreen(true))` if no screen is open.
- `InputConstantsMixin.isKeyDown` returns the virtual keyboard state, so polling code (sneak, sprint keys) sees host
  input, not the real SDL keyboard.
- Gameplay look: `beginFrame` writes `player.setYRot/XRot(sky.yaw/pitch)` and the previous-tick copies
  (`yRotO/xRotO`), so Minecraft's interpolation shows no rotation smear. Skipped while a GUI screen is open.
- Quirk: the first click after link-up "only grabs the mouse, like focusing a window" (comment in `fake_skyrim.py`
  line 305), so the test clicks twice.

### Analog for MGSV (guest)

MGSV's input imports **[V:bin]** (IAT VAs from `mgsvtpp.exe`):

- `XINPUT1_3.dll` **ordinal 2**. That ordinal is `XInputGetState`: checked against the export table of Proton's
  `xinput1_3.dll`, which has 1 DllMain, 2 XInputGetState, 3 XInputSetState, 4 XInputGetCapabilities, …,
  100 XInputGetStateEx **[V:bin]**. IAT VA **`0x14208e070`**.
- `DINPUT8!DirectInput8Create`: VA `0x14208d048`.
- `USER32!RegisterRawInputDevices`: VA `0x14208de48`.
- `USER32!GetRawInputData`: VA `0x14208de98`.
- `USER32!GetKeyboardState`: VA `0x14208de68`.
- `HID.DLL`: 12 imports.

Which device path MGSV actually reads keyboard and mouse from at runtime is **[U]**. Raw Input is likely, given the
imports.

**Recommended injection: a virtual XInput pad.**

- Patch the IAT slot `0x14208e070` to our `XInputGetState`. It returns a synthesized `XINPUT_STATE` for user 0 built
  from the shared memory, and the real pad's state can pass through otherwise.
- Why a pad and not keyboard/mouse:
  - MGSV's movement has meaningful **analog magnitude** (stalk, walk, run), and so does its camera stick.
  - XInput is polled, so focus is irrelevant and there are no window messages to fake.
  - It's one pointer write, and the import is resolved by the loader before our hook runs.

  The exe's RWX `.xdata` section holds the entry point (§13), so check the slot at runtime in case a protection layer
  re-resolves imports **[U]**.
- **Protocol shape:**
  - XInput is *state*, not events, so add a **seqlock "pad state" slot** (buttons bitfield, sticks, triggers, plus a
    **"pressed since last poll" latch** so taps shorter than one MGSV frame are not lost).
  - Keep the SPSC input ring for discrete events: hurt, release-all, open/close commands, text.

  This replaces SkyCraft's event replay plus virtual keyboard (`KEYS[]` + `isKeyDown` mixin).
- **Keyboard/mouse alternative** (for players who want KB/M aim feel):
  - hook `GetRawInputData` to return synthesized `RAWINPUT` for fake handles our WndProc subclass injects as
    `WM_INPUT`
  - hook `GetKeyboardState`
  - keep `DirectInput8Create` untouched or neutered

  More work, more reverse engineering **[U]**.

**Look/camera ownership (design decision).** SkyCraft keeps the camera zero-latency by integrating the look on the
host with MC's own formula. MGSV's camera has orbit, shoulder swap, aim, first-person aim, cover, CQC and
cutscene-style requests, so re-implementing it on the host breaks the "never re-implement the guest's mechanics"
rule. Recommendation:

- **(A, default)** Send the right stick (or mouse delta) to MGSV through the pad. MGSV computes the camera, publishes
  its pose (§6), and the host applies it to ER's camera. That adds about 1 to 1.5 frames (IPC plus pacing);
  measure it with a `trace_motion.py` analog.
- **(B, optional)** Host-integrated yaw/pitch pushed into MGSV with `Player.RequestToSetCameraRotation{rotX=…,
  rotY=…, interpTime=…}` (Lua API, **[V:bin]** in `TppPlayer`/`TppDemo` usage; whether it is callable every frame is
  **[U]**), or by writing the camera's rotation in memory after reverse engineering.

**Routing:** the host (ER DLL) keeps an allow-list, the analog of SkyCraft's G/Esc/J/M/H:

- **ER's own:** the system menu, map, and the interact button for graces, doors, items, NPC talk. Use arbitration
  like DESIGN §7: the nearest target wins.
- **To MGSV:** everything else.
- **"MGSV UI open" flag:** iDroid, weapon or item wheel, cassette player. The analog of `MC_SCREEN_OPEN`; it switches
  routing so everything goes to MGSV.
- **ER menu open or loading:** `ReleaseAll` (zeroed pad plus latch clear).

**Main difficulty:**

- **Camera ownership and latency.** It has to be measured.
- **Detecting "MGSV UI open"** needs either Lua state (`TppUiStatusManager.CheckStatus`, `TppUiCommand`… names
  **[V:bin]**) or reverse engineering.
- **MGSV contextual actions** (CQC, hold-up, fulton, climb, cover) all ride the same buttons. That is fine as long
  as MGSV owns the pad.

---

## 6. Publishing the guest player's state every frame (and every tick)

### 6.1 Fields, writers, and why [V:src]

There is one `McState` object and one writer thread (render thread). It is written in two places:

**Per frame**, in `afterRender()`, after `GameRenderer.render()`:

| Field | Source | Why (host use) |
|---|---|---|
| `flags` | `MC_IN_WORLD`, `SCREEN_OPEN` (`gui.screen()!=null`), `ON_GROUND`, `SNEAKING` (`isShiftKeyDown`), `SPRINTING`, `DEAD` (`isDeadOrDying`), `SWIMMING`, `FLYING` | Host routing (screen → cursor mode and all keys to MC). `SyncSneak` makes Skyrim's stealth detection match MC sneaking. `ON_GROUND` drives the Skyrim char controller state (`kOnGround`/`kInAir`) because "Skyrim's AI won't melee a target it thinks is in the air" (`Game.cpp:793-800`) |
| `x, y, z` | `player.getPosition(partialTick)`: interpolated feet | fallback pose when no tick data |
| `yaw, pitch` | `player.getYRot/XRot()` (= host's look) | echo/debug |
| `eyeHeight`, `eyeX/Y/Z` | `camera.position()` in first person; in third person `player.getEyePosition(partial)` ("the eye, not the camera") | camera pivot; includes the sneak eye lerp |
| `fovDeg` | `camera.getFov()`: effective **vertical** FOV including sprint and fluid modifiers | `ApplyMcFov` converts it to Skyrim's horizontal FOV |
| `cameraMode`, `cameraDistance` | `options.getCameraType().ordinal()` (0 first person, 1 behind, 2 front); the distance from MC's camera to the eye after Minecraft's own zoom collision | Host F5: places Skyrim's camera on the same orbit and eases the zoom out (0.2 s time constant) but snaps it in (`Game.cpp:839-856`) |
| `bobPhase`, `bobAmount` | `entityRenderState.backwardsInterpolatedWalkDistance` and `.bob`, exactly what `GameRenderer.bobView()` uses this frame; 0 if bobbing is off | fallback bob |
| `sensitivity` | `options.sensitivity()` | the host's look integration uses MC's own formula |
| `teleportAck` | `teleportAck`, or **`teleportAck − 1` while holding** (`holdPos != null`) | handshake: the host only starts puppeting once MC is *released on ground* at the new spot (§6.2) |
| `guiScale` | `window.getGuiScale()` | the host sizes the crosshair invert rectangle (§7) |
| `frameCounter` | `++frameCounter` | diagnostics |

**Per physics tick**, in `publishTick()` from `END_CLIENT_TICK`:

| Field | Source | Why |
|---|---|---|
| `tickQpc` | `QPC − partialTick·tickMs·freq/1000` | When the tick happened, on the shared clock |
| `prevX/Y/Z`, `curX/Y/Z` | `player.xo/yo/zo` and `getX/Y/Z()` | The host interpolates feet itself, exactly like MC's partial-tick renderer. The comment: "Sampling MC's per-frame position instead judders, because the two games' frames aren't phase-locked" |
| `tickEyeO`, `tickEye` | `eyeSmoothed`, recomputed the same way as `Camera.tick()` (eases halfway to `player.getEyeHeight()` each tick) | smooth sneak and stand eye transitions on the host clock |
| `walkDistO/walkDist`, `bobO/bob` | `avatarState().getInterpolatedWalkDistance(0/1)`, `getInterpolatedBob(0/1)` (0 if bob is off) | the host rebuilds MC's view bob on its own clock |
| `tickMs` | `tickRateManager().millisecondsPerTick()` | the interpolation period |

How the host consumes it (`skse/src/Game.cpp:660-783,816-836`) [V:src]:

- **Tick history:** keeps the last 8 ticks and re-times noisy stamps onto the exact period. It handles two ticks in
  one MC frame and missed ticks.
- **Render delay:** renders **renderDelayMs in the past**, adaptive between 4 and 30 ms. The target is the max
  lateness seen over recent ticks + 1 ms; it grows 20 ms/s and shrinks 2 ms/s. That gives pure interpolation, never
  extrapolation.
- **Feet:** `prev + (cur−prev)·t`.
- **Eye:** `feet + lerp(tickEyeO, tickEye)`.
- **Bob:** `phase = −(walkDist + (walkDist−walkDistO)·t)·π`, `bob = lerp(bobO, bob)`. Then:
  - side = `−sin(phase)·bob·0.5` blocks
  - lift = `|cos(phase)·bob|` blocks
  - pitch = `|cos(phase−0.2)·bob|·5°`
  - roll = `sin(phase)·bob·3°`

  These are applied as a camera offset and rotation.
- **Puppet:** sets the Skyrim player position to the feet with velocity 0, `fallStartHeight`/`fallTime` reset (no
  Skyrim fall damage), and gravity kept on for ground contact.
- **Camera:** placed and rotated in the same frame.

### 6.2 Teleport and hold handshake [V:src]

- **The host owns teleports.** On loads, doors, fast travel, a world change, Skyrim moving the player more than a
  threshold, or the end of a Skyrim "takeover" (furniture, scenes), the host does `teleportSeq++` and resets the
  look to the Skyrim player's heading (`Game.cpp:575-591, 610-621`).
- **The guest applies it** (`beginFrame`). It treats a new `teleportSeq` *or* a new `LocalPlayer` object (join or
  respawn) as a teleport:
  - `setPos`, zero velocity, reset fall distance
  - `server.execute(teleportTo, setYRot, setXRot)` so the integrated server agrees
  - `holdPos = target`
- **Guest hold** (`holdUntilReady`, every tick):
  - While holding, pin position, `xo/yo/zo`, velocity and fall distance.
  - Release when the collision regions at `by−1`, `by` and `by−8` are known **and**
    `SkyCollision.hasSolidBelow(bx, by, bz, 12)` (a 3×3 column 12 blocks deep). Or after a 6 s timeout, for being
    mid-air on purpose.
  - On release, `liftOutOfGeometry`: if the exact Skyrim ground (`TriCollider.groundAt`) is up to 2.5 blocks above
    the feet, stand on it ("Minecraft's collision never pushes you out of a shape").
  - While Skyrim is not in game, or is loading: park and keep `teleportPending`.
- **Ack:** `teleportAck` is reported as `seq−1` until release.
  - Host: `puppet = mc.teleportAck == teleportSeq && !loading && !dead && !takeover` (`Game.cpp:639`). While
    `arriving`, if MC waits more than 8 blocks from the Skyrim player for over 1 s, the host re-teleports
    (`Game.cpp:624-634`).

### Analog for MGSV (guest)

**What to publish** (names **[V:bin]** from the extracted Lua; memory layout and native addresses **[U]**):

- **Pose:**
  - Feet: `vars.playerPosX/Y/Z` (Lua `TppPlayer.GetPosition()` returns them; `TppPlayer.lua:28`).
  - Facing: `vars.playerRotY`.
  - Camera: `vars.playerCameraRotation`.

  Reading `vars` from Lua every frame works for an MVP. A native read is cheaper (reverse-engineer the vars
  storage).
- **Stance and state:** `PlayerStatus.STAND/SQUAT/CRAWL/CARRY` via `PlayerInfo.AndCheckStatus` / `OrCheckStatus`;
  in vehicle: `vars.playerVehicleGameObjectId`; CBox: `Player.IsVarsCurrentItemCBox()`; dead, in cutscene or
  "disabled action" (`vars.playerDisableActionFlag`), and so on. These map to ER-side needs: crouch for stealth
  (ER's crouch state and its noise/visibility), on-ground for AI targeting, as with Skyrim.
- **Camera:** world position, orientation, **vertical FOV** (ER's FOV convention **[U]**, to be pinned by the host
  report), and mode (normal, aim, first-person aim, cover). This replaces SkyCraft's eye + yaw/pitch + bob + F5
  distance. MGSV's camera already includes its own shake and bob, so publish the *final* camera transform.
- **Venom's skeleton.** SkyCraft does not need this: MC is first-person, and its F5 body is shipped as posed
  triangles in the render ring (§7). Venom is always visible in third person.
  - Publish the **bone palette** (world or model-space matrices, or quaternion+translation per bone) of the
    `TppPlayer2` model each frame, stamped with QPC.
  - ER draws a **pre-converted Venom mesh**, which the prior project already converted, skinned with the streamed
    pose. MGSV skinning runs on the GPU **[U]**, so CPU-posed triangles (SkyCraft's `AvatarExporter` approach) are
    not available.
  - Two routes to the palette:
    - native: find the player's skeleton or animation-output buffer by reverse engineering
    - D3D11: identify the player draw calls and their bone constant buffer in RenderDoc and hook
      `UpdateSubresource`/`Map` for it

    Either is substantial work.
- **Samples:** per-frame `{qpc, feet, rot, camera, pose}` into a small ring (or double seqlock: previous + current)
  so the host can interpolate on its own clock, as in §6.1.
- **Teleport:** the same seq/ack handshake. The MGSV apply path is the Lua
  `TppPlayer.Warp{pos={x,y,z}, rotY=deg}`, which sends
  `GameObject.SendCommand({type="TppPlayer2",index=0},{id="WarpAndWaitBlock",pos=…,rotY=radians})`
  (`TppPlayer.lua:31-42` **[V:bin]**). It is a built-in "wait for streaming blocks", a partial analog of
  `holdUntilReady`. We still need our own hold until ER collision has arrived around the target (§10), and a
  "lift out of geometry" step.
- **Takeover:** ER cutscenes, grace rest, ladders, lifts, Torrent, and doors with animations hand the player to ER.
  Freeze MGSV (zero pad + `Player.SetPadMask`) and re-teleport when it ends, the same as SkyCraft's `SkyrimTakeover`.

**Main difficulty:**

- **The skeleton stream** (reverse engineering plus retargeting to the ER-converted Venom).
- **Choosing per-frame sampling** without a fixed tick (Fox dt jitter).
- **Lua-only reads** are easy, but cost a Lua call per frame and may lag a frame behind native state **[U]**.

---

## 7. Exporting the guest's frame and overlay

### 7.1 Overlay: GUI and hand on transparent, CPU readback [V:src] (`client/FrameExporter.java`)

1. `LevelRendererMixin` cancels the level pass, so `mainRenderTarget` holds **only** the first-person hand/item, the
   HUD (hotbar, hearts, crosshair) and any open screen, over a transparent clear.
2. Each frame after render, `capture()`:
   - **First:** ship the newest finished readback.
   - **Then:** take a FREE staging slot (up to **3** `GpuBuffer`s, created with
     `RenderSystem.getDevice().createBuffer(label, 9, w·h·4)`; usage bit meaning **[U]**) and issue
     `createCommandEncoder().copyTextureToBuffer(colorTexture, buffer, 0, callback → READY, mip 0)`. The copy is
     asynchronous.
   - If all 3 are in flight, skip the frame.
   - Capture is skipped above 3840×2160.
3. `shipReadyFrames()`:
   - pick the READY slot with the highest `frameId`
   - `buffer.map(read)`, then `MemorySegment.copy` into `SkyLink.overlayBackSlotOffset()`
   - `publishOverlay(w, h, bottomUp=true, frameId)`
   - mark that slot and every older READY slot FREE

   Latency is about one frame. At 1080p that's 8.3 MB per frame.
4. **Host compositor** (`skse/src/Overlay.cpp`, D3D11 Present hook):
   - `AcquireOverlayFrame()`, upload to a texture, full-screen triangle.
   - **Premultiplied alpha** "straight from Minecraft": blend `ONE, INV_SRC_ALPHA`, with `flipY` because GL rows are
     bottom-up.
   - Minecraft's **crosshair and attack indicator** need MC's *invert* blend against Skyrim's picture, so they are
     drawn in a separate pass (`INV_DEST_COLOR / INV_SRC_COLOR`) inside an "invert rect" around the screen centre,
     sized from `guiScale`.
   - The **mouse cursor** for MC screens is drawn by the host shader at `st.cursorX/Y` (MC's OS cursor is invisible).

### 7.2 World layer: geometry, not pixels [V:src] (skimmed `client/render/*.java`)

The DESIGN's "three GPU layers with depth compositing" is **not** what shipped. Minecraft's world contents travel as
**geometry and data**, and Skyrim draws them with its own lighting, shadows and fog:

- `WorldExporter.frame`:
  - On a new `generation`, level or atlas: `REN_CLEAR_ALL`, then `REN_ATLAS`, built by `SkyAtlas` (block atlas with
    the item atlas stacked below, plus crack stages and arrow textures).
  - Up to 12 dirty 16³ sections per frame, re-meshed **with Minecraft's own `ModelBlockRenderer`/`FluidRenderer`**:
    `REN_SECTION`, plus `REN_LIGHTS` (emissive blocks, colour from `BlockLightColors`), `REN_SOLIDS` (a bitset so
    Skyrim NPCs collide with player blocks), and `REN_DUG`.
  - `REN_ATLAS_REGION` for animated sprites.
  - The **WorldEntities** seqlock table: arrows, dropped items, tridents, item blocks, cracks, contact shadows, and
    the block-selection outline.
- `AvatarExporter` is a custom `SubmitNodeCollector`: Minecraft's entity renderers submit into it instead of the GPU.
  - It captures the **third-person player body** posed and animated (`REN_AVATAR`, feet-relative; empty in first
    person) and **other entities, block entities and particles** (`REN_SCENE`, camera-relative origin).
  - It sends a standing **ragdoll** mesh split into 6 body parts once a second (`REN_RAGDOLL`). Skyrim hangs these on
    its ragdoll at death.
  - Textures are sent once (`REN_TEXTURE`) and cached by id.

### Analog for MGSV (guest)

**Overlay readback (HUD and UI).**

- **Mechanism:** a DXVK Present hook. `CopyResource(backbuffer → staging[i])` into a ring of 3
  `D3D11_USAGE_STAGING` textures. One or two frames later, `Map(…, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT)` the
  newest finished copy, `memcpy` it into the shared back slot, and publish with the same `xchg` protocol. This is
  FrameExporter's design ported.
- **Cross-process GPU sharing is out.** DXGI shared handles or keyed mutexes across two wineservers cannot work,
  because handles are per-wineserver (inferred from the **[V:test]** named-object result; **[U]** for D3D
  specifically). So **CPU readback is not just the first step, it is the expected design.**
- **The alpha problem.** Minecraft's frame is transparent outside the GUI because SkyCraft cancels the level pass.
  MGSV's back buffer is the full 3D scene with the HUD composited on top and no usable alpha **[U]**. Options:
  - **(1) Host-drawn HUD from data, the MVP.** Publish life, the equipped weapon or item and its ammo, the
    reflex/alert state and markers; the ER DLL draws them. Breaks "don't re-implement", but only for presentation.
  - **(2) UI-pass capture.** In RenderDoc, find where Fox's UI is drawn. Redirect those draws to a cleared
    transparent RGBA target (or render the UI twice) and read that back. This is real reverse engineering of the
    renderer.
  - **(3)** Render the scene black or skip it, then key. Fragile.

  Recommend (1) first, with (2) as the stretch for iDroid, the weapon wheel and other menus. The iDroid is
  full-screen and could be shipped *opaque* while open.
- **World things:**
  - Venom's body: the skeleton stream from §6.
  - Held weapon: id plus hand-bone attachment; the ER-side meshes come from the prior project's weapon conversions.
  - Muzzle flashes, tracers, impacts, grenades, smoke, decoys, the fulton balloon and buddies: send them as **events
    or entity tables** (kind, transform, id). The host renders them with ER SFX or converted assets. This is
    SkyCraft's `WorldEntities` and `REN_SCENE` idea, at the "data" level rather than "triangles", because we can't
    cheaply capture posed MGSV geometry on the CPU.

**Main difficulty:** MGSV has no "draw only the HUD" switch like cancelling Minecraft's `LevelRenderer`. Every visual
in MGSV's world needs an ER-side representation.

---

## 8. Host actors become hittable proxies; hits flow back

### 8.1 Actor table → proxies [V:src]

**Host** (`skse/src/Combat.cpp WriteActorTable`, each frame while puppeting):

- Covers every high-process actor within **80 blocks**: not the player, enabled, 3D loaded, not ghost.
- Writes `ActorRecord {formId, flags (HOSTILE|DEAD|ESSENTIAL|IN_COMBAT), feet x/y/z in MC coordinates, yaw (MC
  degrees), width = boundRadius·2 clamped to 0.3..6, height clamped to 0.3..12, healthFrac, level, name[24]}`.
- Up to 256 actors, under the seqlock.

**Guest server** (`combat/SkyCombat.serverTick`, every server tick):

- If the link is down, `removeAll()`.
- `readActors`.
- Discard proxies whose actor is gone or **dead**, or whose level changed.
- Spawn new `SkyrimActorEntity`s with `formId`, size and `snapTo(x, y, z, yaw, 0)`, `setCustomName(name)` if
  present, `addFreshEntity`.
- Update existing proxies: `setSize`, `setPos`, `setYRot`, `setYHeadRot`, and `stepOnTriggers` (proxies have no
  physics, so pressure plates and tripwires under their box get `entityInside` called by hand).
- Also `pickUpNearby`: items and arrows within a 1.25-block inflated box call `playerTouch` (Skyrim ground voxels can
  sit a bit off the player's exact-triangle stance).

**`SkyrimActorEntity`** (`combat/SkyrimActorEntity.java`):

- A `LivingEntity` with synced `FORM_ID`, `WIDTH` and `HEIGHT` (`EntityDimensions.scalable`).
- `noGravity`, `noPhysics`, invisible, silent. Not pushable, `canBeCollidedWith → false`. No name shown, never saved,
  no hurt or death sounds.
- `tick()` = `baseTick()` + `setHealth(max)`. **Its own health never drops.**
- Entity type: `EntityType.Builder.of(…, MISC).sized(0.6,1.8).noSave().noSummon().noLootTable().clientTrackingRange(10).updateInterval(1)`.
- The client renders it with `NoopRenderer`.

**`ProxySync.frame`** (client, every frame): moves the *client copies* of proxies to this frame's actor-table
positions (`setPos`, `xo/yo/zo`, `yRot`). The comment: "so the crosshair and melee reach line up with what's on
screen (the server copy only moves once per tick and reaches the client a tick or two later)".

Guest-unused fields [V:src, by grep]: `ACTOR_HOSTILE`/`hostile()`, `healthFrac`, `level`, `ESSENTIAL`, `IN_COMBAT`
are read into the record but not used by any guest logic. Only `DEAD` (filters proxies out) and `name` are used.

### 8.2 Hits back to the host [V:src]

- Minecraft's own combat runs unchanged on proxies: swords, crits, sweeps, enchantments, attack cooldown, bows,
  tridents, fire.
- **`actuallyHurt` override:** after Minecraft has applied everything (crit, sharpness, strength, cooldown,
  i-frames), it does not lower health. It accumulates `pendingDamage` and sets:
  - `HIT_PROJECTILE` if the direct entity is a projectile
  - `HIT_FIRE` for `IS_FIRE` damage types
  - `pendingWeapon` from `weaponClass(source)`: Trident → PIERCE, projectile → ARROW, `ItemTags.SWORDS` → BLADE,
    `AXES` → AXE, trident item → PIERCE, empty → UNARMED, anything else → BLUNT
- **`knockback` override:** keeps the *strongest* push this tick as a direction `(−xd, −zd)/len` plus strength.
  Skyrim owns the actor's position.
- **Crits:** `ServerPlayerMixin.crit` → `proxy.markCritical()` (`HIT_CRITICAL`).
- **One combined hit per actor per tick:** `SkyCombat.serverTick` calls `takeHit()` on each proxy and pushes
  `EV_HIT_ACTOR {formId, a=damage, b/c=push dir x/z, d=strength, flags=float bits of hit flags, weapon=float bits of
  weapon class}`. The comment explains why: "Player.attack adds its sprint/enchantment knockback after hurtServer
  returns".
- **Host `ApplyHit`:** damage × (5 + 0.25·NPC level) per DESIGN §13; hit pipeline, stagger, sounds, crime. Read only
  as far as the event switch (`Combat.cpp:880-903`).
- **Other guest→host events:**
  - `EV_ARROW_STUCK` (`AbstractArrowMixin`, where MC records an arrow stuck in a proxy): hit point, flight yaw, pitch
    float bits in `flags`, arrow texture in `weapon`; Skyrim pins an arrow to the NPC skeleton.
  - `EV_EXPLOSION` (`ServerExplosionMixin`, centre and radius).
  - `EV_SKILL_USE` (Smithing from crafting via `SmithingMixin`; Block, Light Armor and Heavy Armor from hits taken).
  - `EV_PLAYER_DIED` (§9).

### Analog for MGSV (guest)

There are two ways to make ER enemies (`ChrIns`) hittable by Venom's bullets, tranquilizer darts, CQC, the bionic
arm and explosives. Either way, results come back as events.

**Path 1: puppet MGSV soldiers as proxies.** This is the closest to SkyCraft (a real native entity, so the guest's
own combat code runs on it).

- Use a pool of `TppSoldier2` GameObjects in the loaded mission. Enable, disable, warp and refresh them through
  `GameObject.SendCommand(id, {id="SetEnabled"|"Warp"|…})`; command ids seen in the extracted Lua include
  `SetEnabled`, `Warp`, `ChangeLifeState`, `RequestVanish`, `GetPosition`, `ChangeFova` **[V:bin]**.
- Snap them to the ER actors every frame, the `ProxySync` analog.
- Disable their AI and perception, make them invisible (render off or a transparent FOVA), and make them unkillable
  or reset them like `setHealth(max)`.
- MGSV's own systems then work against them: headshots, sleep and stun, CQC grabs, hold-ups, fulton, interrogation.
- Read the results from Lua GameObject messages: `msg="Damage"` (handler `func(gameObjectId, attackId,
  attackerId)`; parameter meaning inferred **[U]**), `"Dead"`, `"Unconscious"`, `"HeadShot"`, `"Neutralize"`,
  `"Fulton"` (names **[V:bin]** from `TppEnemy.lua`, `TppMission.lua`, `TppRevenge.lua`). Or hook the native damage
  application to get the numeric damage **[U]**.
- **Difficulty:**
  - Soldier count is bounded by the mission's soldier pool. Proxies for the nearest N ER actors only.
  - Shape mismatch: ER bosses and beasts versus a human soldier hit volume. Scale or substitute.
  - Suppressing soldier AI, voices, alerts and radio, and the cost of a whole soldier per proxy.
  - Fulton of an ER enemy needs a *host* meaning: despawn? capture? A design decision.

**Path 2: generic hit volumes.**

- Fox has entity classes `GeoxPartsTargetObject`, `GeoxPartsTargetFollowSkeleton`, `GeoxPartsTargetDefenseObject`,
  `GeoTrap*` and `GeoxDynamicPolygon` (strings **[V:bin]**; semantics **[U]**). Or hook the bullet ray query
  (`GeoSearchLine`/`GeoSearchSegment` class names **[V:bin]**) to also test host-supplied capsules.
- Cheaper per proxy, but CQC, hold-ups and fulton would need Path 1 or custom logic.

**Event payload** (EV_HIT analog):

- `{hostActorId, damage, attackId (TppDamage.ATK_*), hitPart (head/body/limb), lethal|sleep|stun|faint, push
  dir/strength, isCqc, isProjectile, weaponId}`.
- Accumulate per frame per actor, like `takeHit()` per tick.
- **Non-lethal is the important semantic MGSV adds:** tranquilizer sleep, stun and CQC throws should map to ER
  sleep, stagger or poise break, not to HP.

**Main difficulty:** this is the biggest reverse-engineering item on the guest. MGSV combat is built around soldier
GameObjects and their AI. Making invisible, AI-less, host-driven soldiers that still accept every MGSV interaction is
much harder than SkyCraft's 200-line `LivingEntity` subclass.

---

## 9. Host hurt events applied to the guest player

[V:src]:

**Host** (`skse/src/Combat.cpp:85-170`):

- Makes the Skyrim player **essential** while puppeting.
- Every frame, **refunds** any health deficit (`RestoreActorValue`) and attributes it to the last `TESHitEvent`
  (within 0.35 s): attacker, kind (melee, projectile, magic by source form type), power attack, blocked. Otherwise to
  the last `HandleHealthDamage` attacker (as magic).
- Melee and projectile go immediately. Magic and other damage are **batched over 0.5 s**, because Minecraft has
  i-frames.
- Sends `kInHurt {code=HurtKind, a=round(damage·100), b=attackerFormId, c=HurtFlags}` **through the input ring**.

**Guest** (`InputBridge.hurt` → `SkyCombat.hurtPlayer`, on the server thread):

- The attacker proxy is used only if it is within 24 blocks ("a guest's own NPC with the same form id as one of the
  host's").
- Damage source by kind:
  - `HURT_MELEE` → `mobAttack(proxy)`
  - `HURT_PROJECTILE` → `mobProjectile(proxy, proxy)`
  - `HURT_MAGIC` → `indirectMagic`
  - otherwise `generic`
- `damage = skyrimDamage / 5` (`SKYRIM_TO_MC_DAMAGE`), then `player.hurtServer(level, source, damage)`. Armor,
  Protection, shield blocking, totems, i-frames, hurt sound, red flash and knockback are all vanilla Minecraft.
- A **power attack** that wasn't blocked adds `player.knockback(0.5, toward the attacker)`.
- `trainDefence`: Block if the shield caught it, otherwise Light or Heavy Armor by equipped armor material →
  `EV_SKILL_USE`.
- `HURT_BLOCKED_IN_SKYRIM` is sent but **unused** by the guest (grep).
- In MC multiplayer, a guest client forwards hurt to the host server via the `SkyNet.Hurt` packet, clamped to 0..10000
  (`net/SkyNet.java`).

**Death:**

- `ServerPlayerMixin.die` → `EV_PLAYER_DIED {formId=attacker}`. The host kills the Skyrim player (Skyrim's death
  camera, the MC ragdoll mesh) and runs the normal reload flow.
- Gamerules `KEEP_INVENTORY` and `IMMEDIATE_RESPAWN`. Respawn creates a new `LocalPlayer`, which the guest treats as a
  teleport (§6.2).

### Analog for MGSV (guest)

- **Host (ER):** cancel or refund damage to the player character so ER doesn't apply it. Prefer a damage hook over a
  refund. Classify the hit (melee, projectile, magic, fall, status buildup, attacker `ChrIns` id, poise/stagger
  class) and send it through the input ring.
- **Guest (MGSV): apply through MGSV's own damage pipeline**, so Venom reacts the MGSV way: flinch, knockdown,
  bleeding, the Battle Dress or suit level damage reduction, and auto-regen of life.
  - Map ER attack classes to MGSV attack ids. The exe holds **492 distinct `ATK_*` strings** **[V:bin]**, e.g.
    `ATK_Knife`, `ATK_KnifeSlash`, `ATK_Kick`, `ATK_DownKick`, `ATK_MsfPunch`, `ATK_ParasiteCamoufMachete`,
    `ATK_FireBurn`, `ATK_Fall`, and many `ATK_Cqc*`. Lua exposes some as `TppDamage.ATK_*`.
  - The native "deal damage to player with attackId" function is **[U]** and must be found by reverse engineering
    (search for xrefs of the ATK table and of the player's life var).
  - Fallback: write the life value directly, the analog of `DamageActorValue`. MGSV's life var name appears as
    `playerLife` in `Tpp.lua` **[V:bin]**; storage **[U]**.
- **Attacker identity:** the ER actor id maps to a proxy soldier, if Path 1 (§8), so MGSV's directional damage
  indicator and reactions point the right way.
- **Death:** MGSV's player death runs `TppMission.OnPlayerDead` with option `isExecGameOver=true` (seen in
  `TppMission.lua` **[V:bin]**), which leads to MGSV's game-over or continue screens. We must:
  - suppress or short-circuit that from injected Lua
  - send `PLAYER_DIED` to ER (ER's "YOU DIED", rune drop, respawn at grace)
  - on ER respawn (`teleportSeq++` plus a "revive" flag), revive Venom in MGSV and warp him

  Equivalents of `IMMEDIATE_RESPAWN` and `KEEP_INVENTORY` (MGSV doesn't lose equipment on death **[U]**).
- **Main difficulty:** finding the native damage function and the death/game-over control flow, and keeping MGSV's
  mission system from failing or restarting the mission.

---

## 10. Host collision injected into the guest's physics

### 10.1 Stream format and consumer [V:src]

**Collision ring** (`protocol/skycraft_protocol.h`, `world/SkyCollision.java`). Messages, 8-byte aligned:

- `kColClear{epoch}`: on a world change. The guest drops all collision and adopts the epoch. A fresh guest
  **adopts whatever epoch is in progress** (`adoptEpochIfFresh`).
- `kColTris{ColRegion(count=triangles), ColTri[count]}`, sent **before** the region's `kColRegion`.
  - `ColTri{float v[9] (MC space), flags}`
  - Flags: `kTriStairHelper` (invisible stair ramp: walkable, never a wall), `kTriDiggable` (material in bits 8-15,
    outward winding), `kTriGhost` (pre-dig surface, not collision), `kTriTerrain`.
  - Stored per 8³-block region, replacing what was there. A content hash marks "changed" regions for the dig code.
- `kColRegion{ColRegion{min, max (inclusive), epoch, count}, ColBlock[count]}`.
  - `ColBlock{x, y, z, pad, u64 bits[8]}`: an **8×8×8 sub-voxel mask** (1/8 block), `bits[y]` bit `z·8+x`.
  - The guest builds a `CubeVoxelShape` (or `Shapes.block()` if full) per block, plus `FILL` info: count, lower/upper
    half, top layer.
  - It replaces every block in [min, max] *atomically from the reader's view*: it builds the new map first, then
    swaps entries.
  - It marks the regions `KNOWN`. Stale epochs are ignored.
- Region size is **8 blocks** (`REGION_SIZE`, must match SKSE). The host streams around the puppet's MC position
  (`Collision::Get().Update(...)`, `Game.cpp:995-999`).
- **Consumer:** the daemon thread `SkyCraft collision` drains everything, sleeping 2 ms when idle and 500 ms after
  errors. It stores into `ConcurrentHashMap`s, so physics on the client and server threads reads lock-free.

### 10.2 How Minecraft's physics sees it [V:src]

- **Voxels for everything except players:** `BlockCollisionsMixin` wraps
  `CollisionContext.getCollisionShape(...)` inside `BlockCollisions.computeNext`. It ORs Skyrim's per-block voxel
  shape (and dug-hole walls) into the block's shape. Vanilla movement, step-up, `onGround` and fall damage run
  unchanged. Entities flagged by `SkyCollision.usesSmoothCollider` skip the voxels.
- **Exact triangles for players** (client *and* the integrated server's movement re-check, to prevent
  rubber-banding; `SkyCraftClient` sets the predicate `e instanceof Player && linked`):
  - `EntityCollideMixin` injects at `Entity.collide` RETURN: after vanilla resolved against MC blocks,
    `SkyCollider.collide` gathers triangles near the swept box (inflated 1 + step) and runs `TriCollider.resolve`.
  - `TriCollider.resolve`:
    - horizontal sub-steps of 0.1 block, pushing a vertical cylinder out of steep triangles
    - walkable means `|ny| ≥ 0.7`, about 45°
    - floor sampled with a 13-point footprint of radius 0.15
    - step-up of `maxUpStep` when grounded, 0.3 in air
    - ceiling clamp, and stick-to-ground going downhill
  - It returns axes **bit-identical** where unchanged, because MC compares for collision with exact equality. Then
    it collides the result with MC blocks again.
- **Other mixins:**
  - `PlayerEdgeMixin`: no sneak edge-stop (MC looks for *block* collision under the feet).
  - `MinecraftServerFlightMixin`: `allowFlight=true` while linked, since guests stand on ground the server doesn't
    know.
  - `BlockStateBaseMixin`: Skyrim surfaces are "sturdy" faces for torches and the like.
  - `FallingBlockMixin`: sand rests on Skyrim ground.
  - `FlowingFluidMixin`: fluids follow Skyrim's surface.
  - `PressurePlateMixin`: pressure plates sense the whole block height.
- **Ray casts** (`world/SkyClip.refine`, nearest of vanilla and Skyrim wins):
  - the crosshair pick: `EntityPickMixin` on `Entity.pick`
  - projectiles: `AbstractArrowMixin`, `ProjectileUtilMixin`
  - `ProjectileMixin` turns a hit on "air that is Skyrim" into a real hit
  - item use: `ItemMixin.getPlayerPOVHitResult`
  - **third-person camera zoom**: `CameraZoomMixin.getMaxZoom`
- **Water:**
  - A Skyrim `WaterGrid` (16×16 columns of surface heights) is read each frame.
  - `EntityFluidInteractionMixin` and `EntitySwimMixin` make air below the surface report as water, so Minecraft's
    swimming, floating and drowning apply.
- **Unit tests:** `fabric/src/test/java/dev/skycraft/world/TriColliderTest.java` (flat, ramps at angles, walls,
  steps; gravity tick −0.0784) and `SkyRayTest.java` exercise the collider and raycaster as pure Java, with no game.

### Analog for MGSV (guest)

MGSV facts **[V:bin]** (strings in `mgsvtpp.exe`):

- **No Havok.** No `hkp*`/`hknp*`/"Havok" strings.
- **Own physics:** `PhDaemon`, `PhRigidBody`, `PhShape`, `PhPrimitiveShape`, `PhPolyhedronShape`, constraint classes.
- **Geo collision/query system:**
  - collision: `GeoCollisionTerrain2`, `GeoCollisionTerrainMap`, `GeoCollisionFreeShape`, `GeoxCollisionPrimitive`
  - geometry files: `GeoGeomFile`, `.geom`
  - queries: `GeoSearchService`, `GeoSearchLine`, `GeoSearchSegment`, `GeoSearchCapsule`, `GeoSearchSphere`,
    `GeoSearchAABB`, `GeoSearchOBB`, `GeoSearchBVH`, `GeoSearchDynamic`
  - runtime collision: **`GeoxDynamicPolygon`/`GeoxDynamicPolygonBody`**
  - triggers: `GeoTrap*`
- Venom's character movement collides through Geo queries **[U]**, probably capsule and line searches.

What the MGSV DLL must do, mirroring SkyCraft:

1. **Consume the same triangle stream.** Keep SkyCraft's `ColTri` with flags (stair helper, terrain, material).
   Voxels are a Minecraft-ism, so drop them. Use ER's coordinate scale: ER is metres, and MGSV is metres **[U]**, so
   the mapping is probably a rigid transform plus a handedness flip, to be pinned in a coordinate report.
2. **Make Venom's movement collide with it.** Options, best first:
   - **(a)** Hook the Geo query functions the player controller and camera use (capsule, sphere, line, segment, BVH
     searches) and **union** ER triangles into their results. This is the `BlockCollisionsMixin` /
     `EntityCollideMixin` analog. It needs reverse engineering of the GeoSearch result structures.
   - **(b)** Create `GeoxDynamicPolygon` instances from ER triangles in streamed regions. Viability **[U]**: polygon
     limits, update cost, whether the player controller honours dynamic polygons.
   - **(c)** A pragmatic Phase-0 fallback that **breaks the guest-authoritative principle**: ER's character
     controller resolves collision for Venom's *intended* motion, and the corrected position is fed back by warping
     MGSV each frame. It may fight MGSV's animation and root motion, cause foot sliding, and break climbing and
     vaulting.
3. **The "void world" problem.** Minecraft can run a truly empty void, so only Skyrim collision exists. MGSV cannot:
   Venom must stand in a loaded MGSV location whose own terrain, buildings and props would collide where ER has
   none, and vice versa. Options:
   - filter MGSV's static Geo results out in the same hooks, so only ER collision answers player queries
   - place Venom where MGSV has no geometry above or below (outside the map), if MGSV doesn't kill or respawn
     out-of-bounds players **[U]**
   - a flat MGSV area (a Mother Base platform?) as the base plane, with ER collision added on top. Wrong under
     slopes.

   Filtering is the clean answer and needs (a).
4. **Ray casts** (SkyClip analogs): bullet and dart traces, the aim/auto-aim target test, the third-person **camera
   collision** (MGSV's camera pulls in against walls), the CQC and climb probes, and the "can vault" checks. All must
   see ER geometry, and none should see MGSV geometry, or Venom shoots through or into invisible walls.
5. **Hold until collision arrives:** the `holdUntilReady` analog, keyed on "ER regions around the warp target are
   known and there is ground below". Lift out of geometry by up to about 1.75 m (the 2.5-block analog).
6. **Water:** ER has shallow and deep water and drowning areas. MGSV has rivers, ponds and the sea with death in
   deep water **[U]**. Low priority; start with "ER deep water = host kills the player".
7. **Unit-test the pure parts** natively on Linux: transforms, region bookkeeping, a C++ TriCollider-like resolver if
   we need option (c) (a `TriColliderTest` analog).

**Main difficulty:** this is the deepest reverse-engineering task on the guest. Fox's Geo system has no public
documentation, the player controller's queries are unknown, and we must also *remove* MGSV's own world from them.
Expect to need RenderDoc-free static analysis plus runtime tracing. Ghidra is not to be used in this task, but it
will be needed later.

---

## 11. World setup on the guest

[V:src]:

- **World preset `skycraft:mirror`** (`main/resources/data/skycraft/worldgen/world_preset/mirror.json`): overworld
  type `skycraft:mirror`, generator `minecraft:flat` with **no layers**, biome `the_void`, no features, lakes or
  structures.
- **Dimension type `skycraft:mirror`** (`…/dimension_type/mirror.json`): `min_y −1024`, `height 2048`,
  `logical_height 2048` (Skyrim's vertical range exceeds 384), skylight on, `ambient_light 0`, no ceiling, fixed fog
  and sky colours.
- **Gamerules on `SERVER_STARTED`** (`SkyCraft.configureServer`):

  | Rule | Value |
  |---|---|
  | `ADVANCE_TIME` | false |
  | `ADVANCE_WEATHER` | false |
  | `SPAWN_MOBS` / `SPAWN_MONSTERS` / `SPAWN_PHANTOMS` / `SPAWN_PATROLS` / `SPAWN_WANDERING_TRADERS` | false |
  | `PLAYER_MOVEMENT_CHECK` | false |
  | `KEEP_INVENTORY` | true |
  | `IMMEDIATE_RESPAWN` | true |
  | `SHOW_ADVANCEMENT_MESSAGES` | false |

  Then `time set noon`.
- **Auto open** (`client/MirrorWorld.openWhenReady`, every linked frame):
  - Wait for the title screen; skip onboarding.
  - Then either join `config/skycraft.properties join=` (MC multiplayer) or open the world `"SkyCraft"` if it exists.
  - Otherwise create it: Survival, Normal difficulty, cheats on, seed 0, preset `skycraft:mirror`.
  - `WorldOpenFlowsMixin` skips the "experimental settings, make a backup?" prompt for that world only.
  - Disconnects bounce back to the title screen and on to the mirror world.
- **Kits on join:** a starter kit (diamond sword and pickaxe, bow, food, planks, torches, arrows, shield) if the
  inventory is empty. A one-time builder kit (iron armour plus blocks), tracked with the entity tag
  `skycraft_builder_kit`.
- **DESIGN vs code:** DESIGN §5 and §9 say time and weather are driven from Skyrim. In the code, `SkyState.gameHour`
  is **never read** by the guest (grep), and time is pinned at noon. README says all Skyrim interiors share one
  Minecraft world (DESIGN proposed per-interior slots).

### Analog for MGSV (guest)

The "mirror world" for MGSV is **a loaded MGSV gameplay session tuned to have no world of its own**. All of it must be
reached at runtime from the DLL, because install files must not change.

- **Boot automation (the `openWhenReady` analog):** skip logos and the title, then load a fixed mission or free roam
  with a fixed loadout. Drive MGSV's Lua flow from injected Lua: `TppMission`, `TppMain`, `TppSequence`,
  `TppUiBootInit.lua` (exists in the extracted `master__0__00.dat` **[V:bin]**). Which mission is best (free-roam
  Afghanistan 30010, Africa 30020, Mother Base, or a small story mission) is open; it decides the soldier pool for
  proxies (§8) and the MGSV geometry to suppress (§10).
- **"Gamerules":**
  - no enemy patrols, reinforcements, vehicles or animals: disable via Lua (`TppEnemy`, `TppReinforceBlock`,
    `TppAnimal`, `TppRevenge`), except the proxy soldier pool
  - phase or alert changes ignored (`vars.playerPhase` exists **[V:bin]**)
  - clock fixed (`TppClock`) and weather fixed (`TppCommand.Weather`/`TppWeather`, names **[V:bin]**), or driven from
    ER's time of day if NVG and lighting ever matter
  - no radio, telop, objectives, music or helicopter
  - no buddy, initially
  - optional infinite ammo (`Player.SetInfiniteAmmoFromScript` **[V:bin]**), versus an MGSV ammo economy with
    resupply as an ER item
- **Persistence:** MGSV must play on a **separate save** (its Steam-userdata save, not the user's real campaign).
  Offline only: no FOB/MGO. Save location and an offline switch **[U]**. The analog of SkyCraft's per-world
  `SkyCraft` save folder.
- **Kits:** loadout via sortie-prep vars (`vars.sortiePrep*` used in `TppPlayer.lua` **[V:bin]**) or
  `Player.SetItemLevel` and equip ids.
- **Main difficulty:** MGSV's Lua-driven mission flow is large and stateful. Booting straight into a stable
  "sandbox" without story scripts interfering is a substantial reverse-engineering and Lua job. Infinite Heaven
  proves much of it is possible **[U]**.

---

## 12. Testing without the other game: fake_skyrim / fake_guest and diagnostics

This is the part of SkyCraft most worth copying.

### 12.1 `tools/fake_skyrim.py`: a scripted stand-in for the SKSE plugin [V:src]

Mechanics:

- `mmap.mmap(-1, SIZE, tagname=NAME)` *creates* the named mapping. This is Windows Python; `NAME` defaults to
  `Local\SkyCraft_v1`, overridable with `SKYCRAFT_LINK`.
- It zeroes the host-owned headers and writes `magic`, `version` and its PID, as `Link::Create` does.
- `ctypes.windll.kernel32.GetTickCount64` provides the heartbeat.
- Every offset is hard-coded and all packing uses `struct`. There is no shared schema with C++ or Java.

Per loop at ~60 Hz:

- `heartbeat()`: writes the Skyrim heartbeat, **drains the event ring** (prints damage and push events) and the
  **render ring**. It records the atlas, prints the first sections' vertices, and looks up atlas texels for UVs.
- `write_sky()`: seqlock write of `flags=IN_GAME`, `worldId 0x3C`, epoch, spawn `(X0+0.5, 100, 0.5)` with
  `X0=100000` (far from any real worldspace mapping), `yaw −90` (facing +X), `pitch 10`, a **teleportSeq that is
  new every run** (`time()%100000+2`), viewport 1280×720, `gameHour 12`.

Once MC's heartbeat is alive, it streams collision once:

- `COL_CLEAR(1)`
- for regions `rx ∈ X0/8±3`, `rz ∈ −3..2`, `ry ∈ 11..14`: `COL_TRIS` (the flat floor top as 2 triangles per region)
  then `COL_REGION` (voxels)
- voxel content: a full floor at y=99, a **half-block staircase** toward +X (x 3..10), and a **1/8-block ramp**
  toward −X (x −12..−4)

The scripted scenario starts once MC is "in world": `flags&1`, `ack==tseq`, collision sent, |y−100|<0.05,
|x−spawn|<1. Times are seconds after that:

| t (s) | Action | Checks |
|---|---|---|
| 0+ | Actor table: `0xFF00ABCD` "Test Bandit" 2 blocks ahead, hostile, lvl 10 | — |
| 0.8, 1.5 | Left click down/up (the first click only grabs the mouse) | event ring prints `EV_HIT_ACTOR` |
| 2.5 | `kInHurt` melee, 20 Skyrim damage, from `0xFF00ABCD` | MC log: health drop |
| 3.0–3.7 | Face +Z, pitch 55, key '8' (SDL 37), right click | block placed on the Skyrim floor → render ring section |
| 4.2–5.85 | Face −Z, key '3' (SDL 32), hold right 1.2 s | bow draws and looses |
| 8 | Read WorldEntities | arrow (kind 1) present, block outline |
| 10 | `acquire_overlay()` → PNG (flip if bottom-up) | sampled non-transparent pixel count |
| 12 | WorldEntities again; ASCII-dump the arrow's atlas rects | stuck arrow hasn't moved; texture correct |
| 12.5–13.15 | Face −X, key '8', **Q** (SDL 20) drop | — |
| 15–16.5 | Read entities (kind 2 item), hold **W** (SDL 26) | — |
| 18 | Read entities | item picked up |
| end | Summary: section meshes count, event types; optional atlas PNG | — |

Usage: `python tools/fake_skyrim.py [seconds] [out.png] [atlas.png]`.

### 12.2 `tools/test_mc.sh`: one-command integration test [V:src]

It:

- starts `fake_skyrim.py` and `gradlew runClient` (the Loom dev client)
- every 2 s, greps `fake.log` for `Traceback`, `mc.log` for `Crash report|BUILD FAILED|Exception in thread`, and
  stops 4 s after `saved overlay`
- kills Java via `powershell.exe` (it ran under a Windows bash or WSL)
- prints the fake's log and the filtered `fabric/run/logs/latest.log` (`skycraft|mixin apply|InvalidInjection|Crash|Exception`)

Pass or fail is **human-read log plus PNG**. There are no asserts.

### 12.3 `tools/fake_guest.py`: a second "Skyrim" for a second Minecraft client (MC multiplayer on one PC) [V:src]

- The guest MC is started by `run_guest.ps1` with `-Dskycraft.link=Local\SkyCraft_guest` and `--username Guest`.
  `config/skycraft.properties` says `join=localhost:25599`. The host runs with `SKYCRAFT_LAN_PORT=25599` and
  `SKYCRAFT_LAN_OFFLINE=1`.
- `HostLink` opens the **host's** live `Local\SkyCraft_v1` and:
  - reads the host's McState (position, look)
  - **taps the host's collision ring without consuming it**: it starts at the current head and walks forward on its
    own cursor, resetting if lapped
  - copies `kColRegion`/`kColTris` messages into the guest link, **rewriting the epoch** (`payload[24:28]`)
- The fake drives the guest:
  - spawns it at a slot around the host (angle and radius args)
  - looks where the host looks
  - holds W/A/S/D chosen by the angle to its slot, plus LCtrl sprint when farther than 8 blocks and within 30°
  - re-teleports if the guest fell 20 blocks below the host or is more than 64 blocks away
- It keeps the guest link serviced (heartbeat, rings, overlay).

### 12.4 Read-only live diagnostics [V:src]

| Tool | Reads | Purpose |
|---|---|---|
| `tick_monitor.py [secs]` | McState `tickQpc` @0x268 and `frameCounter` @0x238, polled every 0.5 ms with QPC | Tick interval median/p99/max, "published late" (now−stamp), MC frame gaps >20 ms |
| `trace_motion.py [secs] [dir]` | the full McState incl. tick fields | `ticks.csv` (stamp, seen-time, prev/cur feet, eye) + `frames.csv` (camera mode/dist, feet, eye, look, bob) to diagnose jitter |
| `probe_feet.py` | SkyState, McState, the entire collision ring re-walked from `head−DATA` | The 9 columns of voxels under the player by height |
| `dump_collision.py [x y z]` | collision ring | head/tail, CLEARs, region list with voxel counts |

They work because the mapping is plain memory and **ring data stays readable after consumption**: tail only moves a
cursor.

### Analog for MGSV (guest): PhantomRing's testing plan (strongly recommended)

1. **One schema, three outputs.** SkyCraft hand-maintains `skycraft_protocol.h`, `Proto.java` and the Python
   offsets. Generate the C++ header and a Python module
   from one definition, with `static_assert` and Python `struct.calcsize` checks. (`fake_skyrim.py`'s `read_mc`
   unpacks only the first 0x68 bytes of the 0xC8-byte McState: the kind of drift a generated schema prevents.)
2. **Linux-native fakes on the `/dev/shm` mapping**, no Wine needed:
   - `fake_er.py` (host stand-in for testing the MGSV DLL):
     - creates and initializes the file
     - beats the heartbeat and steps HostState seq at 60 Hz
     - streams a test collision scene (flat ground + stairs + ramp + wall, *in ER coordinates*)
     - writes an actor table (a "test Godrick soldier" in front of Venom)
     - scripts the **virtual pad** (walk, crouch, aim, fire, CQC button)
     - injects a hurt event
     - asserts on the guest's state, pose, camera and events
     - saves the overlay PNG
   - `fake_mgsv.py` (guest stand-in for testing the ER DLL): publishes a scripted Venom (walk cycle via a canned bone
     palette, camera orbit, stamps from `CLOCK_MONOTONIC_RAW//100`), consumes collision and actors, emits hits, and
     publishes a test-pattern overlay.
   - Feasibility **[V:test]**: a Linux `mmap` of `/dev/shm/phantomring_probe.bin` saw every update written by a
     Proton process. Clock domains are as in §2 (Proton = `CLOCK_MONOTONIC_RAW`, system Wine = `CLOCK_MONOTONIC`).
3. **A Wine harness for DLL code without the game.** Build the link and protocol code into a tiny test EXE and run it
   in **scratch prefixes** with either Proton build's `files/bin/wine`. Both ran this session's probe: `wineboot -i`
   in a scratch `WINEPREFIX` works, and so did `wineserver -k` cleanup **[V:test]**. That tests seqlocks, rings, the
   triple buffer and clock stamps exactly as the games will see them.
4. **Record and replay.** Record a real ER session's host stream (collision, actors, input, hurts) to a file once.
   Replay it into MGSV-only sessions, and vice versa. That is `fake_guest.py`'s "tap the host ring" idea, made
   durable. Add the read-only monitors (tick and frame stalls, motion traces) from day one.
5. **Pure-logic unit tests** natively on Linux (C++ with the host compiler, or Python): coordinate transforms, hit
   accumulation, interpolation with jittery stamps, and any collision resolver. This is the
   `TriColliderTest`/`SkyRayTest` analog.
6. **Probe source used in this session:** `scratchpad/guest-side/shmtest/shmtest.c` (a writer/reader over
   `Z:\dev\shm\…`, plus a named-mapping attempt), `reader.py` (Linux reader with a selectable clock), and
   `peimports.py` / `peiat.py` / `peexports.py` (PE import, IAT and export dumps). Scratchpad root:
   `<scratch>/`. Appendix A
   reproduces the key results.

---

## 13. mgsvtpp.exe facts gathered for the analogs [V:bin]

From `<steam-library>/common/MGS_TPP/mgsvtpp.exe`. All read-only.

**PE header and sections:**

- PE32+, linker 11.0 (VS2012; MSVCR110/MSVCP110 imports), Windows GUI.
- **ImageBase 0x140000000, relocations stripped, no DYNAMIC_BASE** (DllCharacteristics 0x8120). Absolute addresses
  are stable run to run.
- PDB name `mgsvtpp.pdb`. One export, `ExportProductMetadata`.
- Unusual section names: `.xtext` (RX, 33 MB), `.edata`, `.data2`, `.didata`, `.text` (only 36 KB),
  `.impdata` (entropy 7.85), `.trace`, `.xdata` (**RWX**, 111 MB, contains the entry point RVA 0x69b5f80), `.00cfg`,
  `.xtls`, `.debug$P`, `.ecode`.
- This looks like a **protection or obfuscation layer** **[U]** as to which one. Expect self-modifying or relocated
  code in `.xdata`. Prefer data-xref and runtime-trace approaches, and verify that hooks survive start-up.

**Imports:**

- `steam_api64` (13), `DINPUT8!DirectInput8Create`, `XINPUT1_3!#2` (XInputGetState), `HID.DLL` (12),
  `d3d11!D3D11CreateDevice`, `dxgi!CreateDXGIFactory1`, `MFPlat`/`MFReadWrite` (video), `WS2_32`, `WINHTTP`, plus
  USER32 (56).
- USER32 includes `RegisterRawInputDevices`, `GetRawInputData`, `GetKeyboardState`, `GetForegroundWindow`,
  `SetForegroundWindow`, `IsIconic`, `ShowCursor`, `SetCursorPos`, `GetCursorPos`, `CreateWindowExW`, `ShowWindow`,
  `SetWindowLongPtrW`.
- A second lowercase `user32.dll`/`kernel32.dll` import set (41/26 functions: clipboard, dialogs, A-variants) suggests
  an embedded secondary module or tool.

**IAT slot VAs**, useful for hooks: see §4 and §5. Examples: `XInputGetState` 0x14208e070, `GetRawInputData`
0x14208de98, `GetForegroundWindow` 0x14208dd28, `D3D11CreateDevice` 0x14208e080.

**Lua and engine strings:**

- **Lua 5.1.5** is statically linked: the string `Lua 5.1.5 Copyright (C) 1994-2012 Lua.org, PUC-…` is at VA
  0x1424b47a6.
- Standard Lua 5.1 strings that let us find the C API by xref:
  `attempt to yield across metamethod/C-call boundary` (VA 0x1424b48e0), `C stack overflow`, `_LOADED`,
  `package.loaders`, `bad argument #%d to '%s' (%s)`, `loadstring`, `=stdin`.
- Engine API names present as strings: `SetPadMask`, `RequestToSetCameraRotation`, `playerPosX`,
  `playerCameraRotation`, `SoundDaemon`, `TppPlayer2*` classes, `PlayerCamera`, `PlayerPad`, `PadMapping`.
- Physics and collision: `Ph*` classes, `Geo*`/`GeoSearch*`/`Geox*` classes (§10), `.geom`. **No Havok strings.**
- 492 `ATK_*` attack-id strings.

**Extracted MGSV Lua** (plain-text, minified Lua in the old project at
`<old-project>/staging/mgsv-behavior/master__0__00.dat/Assets/tpp/script/lib/*.lua` and
`…/master__data1.dat/…`). It is the cheapest MGSV "documentation" for the Lua surface. Used above:

- `TppPlayer.Warp` → `WarpAndWaitBlock`
- `vars.player*` fields: `playerPosX/Y/Z`, `playerRotY`, `playerCameraRotation`, `playerVehicleGameObjectId`,
  `playerDisableActionFlag`, `playerPhase`, …
- `Player.*` and `GameObject.SendCommand` command ids
- `TppSoundDaemon.SetMute`/`ResetMute`
- GameObject messages: `Damage`, `Dead`, `Unconscious`, `HeadShot`, `Neutralize`, `Fulton`, `OnInjury`
- `TppMain.OnUpdate`

---

## 14. Summary: SkyCraft mechanism → MGSV guest DLL

| # | SkyCraft (guest) | MGSV DLL equivalent | Difficulty | First experiment |
|---|---|---|---|---|
| 1 | `OpenFileMappingW(Local\SkyCraft_v1)`, magic/version, PID, heartbeat (8 s) | `CreateFileW(Z:\dev\shm\…)` + `CreateFileMappingW(file)`; session nonce; heartbeat | Low (done in the probe) | Port `shmtest` into the shared link library |
| 2 | `paceFrame` on `SkyState.seq` (25 ms) | Present-hook wait on the host frame seq | Low | Measure with a `tick_monitor` analog |
| 3 | Hide (`SDL_HideWindow`), focus mixins, no grab, no pause, music off | `CreateWindowExW`/`ShowWindow` hide; `GetForegroundWindow`/`IsIconic`/cursor IAT hooks; WndProc subclass; Lua mute of music and ambience | Medium (DXVK and hidden windows; MGSV focus behaviour) | D3D11 sample under Proton with a hidden window |
| 4 | Input ring → `keyboardHandler`/`mouseHandler`; virtual keyboard | **Virtual XInput pad** (IAT 0x14208e070) from a pad-state seqlock + event ring | Low for the pad; medium for KB/M | Hook and drive Venom from `fake_er.py` |
| 5 | Host-integrated look (yaw/pitch in SkyState) | Guest-owned camera, published (default); optional `RequestToSetCameraRotation` | Medium (latency) | Measure camera latency |
| 6 | McState per frame + per tick (QPC, prev/cur feet, eye, bob, fov, camera mode) | Per-frame stamped samples: feet, rot, stance, final camera; **bone palette** | High (skeleton reverse engineering) | Lua-read `vars.playerPos*`/`playerRotY` per frame |
| 7 | teleportSeq/ack(−1 while holding), `holdUntilReady`, lift-out | Same handshake; `TppPlayer.Warp` (`WarpAndWaitBlock`) + hold until ER collision is known | Medium | Warp from `fake_er.py` |
| 8 | FrameExporter async GPU→CPU, triple buffer, premultiplied overlay | DXVK Present hook, staging ring readback; HUD from data first; UI-pass capture later | Medium (readback) / High (UI pass) | Read back the back buffer to a PNG via `fake_er.py` |
| 9 | Render ring: blocks, entities, avatar, ragdoll as geometry | Data events and tables (weapon, projectiles, effects, fulton) drawn with ER assets | Medium-high | — |
| 10 | Actor table → invisible `LivingEntity` proxies, `ProxySync` per frame | Puppeted `TppSoldier2` pool or Geo hit targets, snapped every frame | **High** | Spawn and warp one soldier from Lua; read its Damage messages |
| 11 | `actuallyHurt`/`knockback`/`crit` → one `EV_HIT_ACTOR` per tick | Lua `Damage`/`Dead`/`Unconscious`/`HeadShot`/`Fulton` messages or a native damage hook → one event per frame per actor, with **non-lethal** semantics | High | Log Damage messages while shooting a soldier |
| 12 | `kInHurt` via the input ring → `hurtServer` with a proxy damage source, /5 | ER classifies → input ring → native player-damage call with an `ATK_*` id; suppress MGSV game over | High | Find the player-damage function |
| 13 | Collision ring → voxels (`BlockCollisions`) + triangles (`Entity.collide`) + ray casts + water | Same triangle stream → hooked `GeoSearch*` queries (union ER, filter MGSV) or `GeoxDynamicPolygon` | **Highest** | Trace which Geo queries Venom's movement issues |
| 14 | Void world preset, tall dimension, gamerules, auto-open, kits | Injected-Lua boot into a fixed mission with AI, clock, weather and radio disabled; separate save; offline | High | Lua injection proof: print from `TppMain.OnUpdate` |
| 15 | `fake_skyrim.py` / `fake_guest.py` / monitors / JUnit | `fake_er.py` / `fake_mgsv.py` natively on Linux over `/dev/shm`; Wine harness in scratch prefixes; record/replay; generated schema | Low; **do first** | — |

---

## 15. Open questions and risks for the MGSV guest

1. **Lua state access:** where the `lua_State*` lives, and the addresses of `luaL_loadbuffer`/`lua_pcall`/
   `lua_getfield`/… in this protected build. IHHook's per-version tables may already list them **[U]**. Everything
   cheap in this report depends on it.
2. **Protection layer:** RWX `.xdata` holding the entry point. Are IAT patches and code hooks stable? Are there
   integrity checks?
3. **Present on hidden windows under DXVK:** does a hidden or occluded MGSV keep rendering and ticking? Does MGSV
   pause on focus loss?
4. **Fox Geo query reverse engineering** for movement, camera, bullets and CQC probes, and **filtering MGSV's own
   world** out of them.
5. **Skeleton pose source and retargeting** to the ER-side Venom (the prior project's converted meshes).
6. **Proxies:** soldier pool limits and AI suppression, versus generic Geo targets. What fulton, hold-up and
   interrogation mean against ER enemies.
7. **MGSV mission and save hygiene:** a separate save, offline, no FOB, and no story scripts breaking the sandbox.
8. **Launch orchestration across prefixes:** a Linux launcher, Steam relaunch behaviour of `SteamAPI_Init`, and the
   injection method without touching install files.
9. **GPU budget:** MGSV at minimum settings plus ER, on one GPU, both at 60 fps.
10. **Clock domain:** in production both games run under Proton (`CLOCK_MONOTONIC_RAW`). Make test tools take the
    clock domain from a header field so system-Wine tests (`CLOCK_MONOTONIC`) don't silently misalign.

---

## Appendix A: cross-prefix probe results (this session) [V:test]

Program: `shmtest.exe` (MinGW, `-municode`). The writer maps `Z:\dev\shm\phantomring_probe.bin` (1 MiB) and every
1 ms writes `{counter, QPC, GetTickCount64, QPC freq, pid}`. The reader polls in another prefix. A Linux `reader.py`
mmaps `/dev/shm/phantomring_probe.bin` at the same time.

**Run 1: system Wine 11.0, prefixes `pfxA` and `pfxB`.**

- `OpenFileMappingW("Local\PhantomRingProbe")` in B after A created it: NULL, error 2.
- B saw 2850 counter changes. QPC freq 10,000,000. B's QPC minus A's stamp: mean 0.001 ms, max 0.766 ms. Tick
  difference 0 ms.
- **Writer and reader both had pid 32.**
- Linux reader: 2850 updates. `CLOCK_MONOTONIC` minus QPC: min 0.001, median 0.276, max 0.764 ms.

**Run 2: ER's Proton (`~/.local/share/Steam/steamapps/common/Proton - Experimental/files/bin/wine`) as writer,
MGSV's Proton (`<steam-library>/common/Proton - Experimental/files/bin/wine`) as reader, scratch
prefixes `pfxP1` and `pfxP2`.** Both printed "ntsync: up and running".

- Named mapping open: error 2.
- Reader saw 2792 changes. QPC difference mean 0.005 ms, max 0.941 ms. PIDs 220 and 224.
- Linux reader with `CLOCK_MONOTONIC_RAW`: min 0.001, median 0.280 ms.
- Linux reader with `CLOCK_MONOTONIC`: median −179.6 ms.
- So **Proton QPC = CLOCK_MONOTONIC_RAW / 100 ns**.

All scratch wineservers were stopped (`wineserver -k`) afterwards. Nothing outside the scratchpad or `/dev/shm` was
written; the probe file was deleted after run 2.
