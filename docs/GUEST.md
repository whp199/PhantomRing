# PhantomRing MGSV guest (`mgsv/` → `dinput8.dll`)

The guest is the half of PhantomRing that lives inside `mgsvtpp.exe` 1.0.15.4. It lets MGSV run Snake
exactly as MGSV does, and translates that for Elden Ring: Snake's intended motion, camera, skeleton,
life, shots, melee and his battle buddy (D-Walker first) go out through the shared mapping, and the
player's pad and Elden Ring's hits come in. Architecture: `docs/DESIGN.md`. Contract:
`protocol/phantomring_protocol.h` (v2).

Status: phases 0 and 1 for the guest, plus the shot/melee/buddy parts of phases 3 and 4 that the RE
backlog made possible, all run live with both games since (README "Status", docs/STATUS.md). §10 was the
checklist for the first live run.

---

## 1. Loading and activation

- MGSV statically imports `DINPUT8!DirectInput8Create`, so the loader maps `<MGS_TPP>\dinput8.dll` before
  the exe's entry point runs. This is the injection point IHHook, tpp-mod and MGS5VR use on this exe.
  Under Proton it needs `WINEDLLOVERRIDES="dinput8=n,b"`.
- **Proxy, always.** All six exports (`DirectInput8Create`, `DllCanUnloadNow`, `DllGetClassObject`,
  `DllRegisterServer`, `DllUnregisterServer`, `GetdfDIJoystick`) forward to the real
  `%SystemRoot%\system32\dinput8.dll`. That DLL is loaded by its full path on first use, never in DllMain.
- **PhantomRing activates only when all three hold:**
  1. `PHANTOMRING=1` (the launcher sets it);
  2. the in-memory PE header matches: TimeDateStamp `0x6A4CB898`, SizeOfImage `0xA01D000`, base
     `0x140000000`;
  3. a worker thread confirms sha256 `085c2f82…bb45`. If the hash differs, every hook is disabled again
     and the guest reports `kFailed`.
  If any check fails, the DLL stays a plain proxy and logs why (`PhantomRingGuest.log` next to the DLL).
- **In DllMain** (before the engine builds its Lua state or starts its frame loop), after each feature's
  byte checks: MinHook on `lua_pcall`, the camera setter, `SkinPublish`, `ShotSolver`, and the
  kernel32 file functions (save guard). The vars registration sites are verified here, and the link
  worker and maintenance threads start.
- **Late init**, at the first `DirectInput8Create` (or 5 s after load, whichever comes first): the
  XInput IAT patch and the user32 window guards. The import table is surely bound and user32 loaded by
  then. `DirectInput8Create` also wraps the returned `IDirectInput8W` so that game controllers are hidden.
- Each feature is gated by its own byte check (`mgsv/Addresses.h` + `Verify.cpp`). A mismatch logs the
  bytes it found and turns off only that feature. `python3 -I mgsv/verify_addresses.py` runs the same
  checks against the exe on disk; all 46 sites match the local 1.0.15.4.

## 2. Files

| File | What |
|---|---|
| `main.cpp`, `dinput8.def` | proxy exports, DllMain |
| `Guest.cpp` | activation, late init, maintenance thread (sha256, Steam save guard), `!` console commands |
| `Addresses.h`, `Verify.*` | every VA with its expected bytes; identity checks |
| `Config.*`, `PhantomRingGuest.ini` | settings (the shipped ini is exactly the defaults; a test enforces it) |
| `Link.*` | mapping attach/heartbeat/nonce, producers (GuestState, PoseBlock, guest events, render ring), consumers (pad, host events), unix pid |
| `Lua.*` | `lua_pcall` depth-1 hook and a protected-call-only wrapper over MGSV's Lua 5.1 API |
| `GuestScript.*` | the embedded Lua (`PhantomRing.Tick/Buddy/LoadMission/Setup/...`) |
| `Pump.*` | runs on MGSV's Lua thread: script load/override, dev console, per-update Tick, boot state machine |
| `Camera.*` | camera-setter hook with a GNU-as stub that captures the caller's RSI (the player camera owner) |
| `Frame.*` | once per player-camera frame: odometer, hold, hurt/respawn, shot fallback, GuestState |
| `Skeleton.*` | `SkinPublish` hook → PoseBlock (178 bones) |
| `Combat.*` | `ShotSolver` hook → kGEvFire/kGEvBuddyFire; CQC attack-serial poll → kGEvMelee |
| `Hold.*` | native set-position of the player character (re-backlog M1) for the native hold modes |
| `Vars.*` | native reads of the `vars` ScriptVar block, plus the playerLife write |
| `Pad.*`, `DInputWrap.*`, `WindowGuard.*` | XInput IAT, DirectInput enumeration filter, minimise/foreground/cursor guards (every user32 detour) |
| `WindowHidden.*` | Windows: hidden MGSV window, focus spoof, forced view-visibility check `0x14024A1D0`, windowed swap chain, quit without host (`[Window]`, `!window`; docs/WINDOWS.md) |
| `SaveGuard.*` | Steam Cloud and file-level save blocking |
| `State.*` | what the Lua thread and the frame thread share |
| `Logic.*` | pure arithmetic (odometer, treadmill, life, ammo, transforms, blast kinds, lead point), unit-tested |
| `Blast.*` | attack-area system vf0 detour -> kGEvExplosion for thrown/placed explosives (§18.5) |
| `Menu.*` | MGSV's pause menu (native) / iDroid (Lua) -> kGuestInMenu; GuestState republished while frames stall (§18.6) |
| `Sound.*` | Wwise SetPosition/PostEvent detours, D-Walker volume experiment and `!sound` diagnostics (§18.2) |
| `Follow.*` | D-Walker follow: whistles, follow spot, steering, out-of-view catch-up (§18.3) |
| `FramePace.*` | MGSV's frame rate: its Sleep(1) job polls shortened (`[Pacing] JobSleepUs`), `!pace` timings, wait probe, present experiments (§19) |

## 3. Threads

| Thread | Runs |
|---|---|
| Game thread, player camera update (`0x141118B06` → setter `0x14044E960`) | `Frame.cpp`: everything per frame. It sets the frame counter. |
| MGSV's Lua thread (each outermost `lua_pcall` return) | `Pump.cpp`. `Tick` runs once per new camera frame (or every 33 ms without frames), and boot steps run every 250 ms. Only the VM that has `TppMain` counts. |
| Skin publication (any thread) | `Skeleton.cpp` publishes the player's pose only (driver/binding matched against the owner chain). |
| FireWeapon (game thread) | `Combat.cpp` ShotSolver detour. |
| Whoever polls XInput | `Pad.cpp`. |
| Link worker (30 Hz) | maps/attaches, heartbeats, writes status text, serves `!` console lines, remaps if the launcher replaced the file. |
| Maintenance (1 Hz) | sha256 once, swaps the Steam remote-storage vtable once Steam is up, runs the late-init fallback. |

**Lua safety.** Anything that can run a metamethod runs inside the engine's own `lua_pcall`:
- global lookups go through compiled chunks;
- `PhantomRing.Tick` and `PhantomRing.Console` are held as registry references (`luaL_ref`/`lua_rawgeti`);
- unprotected API use is limited to stack handling and reading numbers, strings and booleans.
An unprotected error would hit `lua_atpanic` and end the game.

## 4. What the guest publishes and consumes

- **Header:** `guestNonce` (set last on attach, after the producer sides are reset), `guestUnixPid`
  (from `PHANTOMRING_GUEST_UNIX_PID` or `getpid` through the high syscall stub, `common/Mapping.h
  UnixPid`, shared with the host), `guestHeartbeatQpc`
  (while booting, or while the game ticked in the last 10 s), `guestStatus`/`guestStatusText`
  (§5 texts).
- **On (re)attach** (new `sessionNonce`): the guest drains stale host events, publishes an empty
  GuestState and PoseBlock (a crashed MGSV's last `kGuestReady` state must not drive the puppet while the
  new one boots; the host also requires `guestStatus == Ready`), sends `kRenClearAll`, answers nothing
  left over in the console, bumps the attach generation (new `odoEpoch`), and only then writes
  `guestNonce`. A host speaking another protocol version is refused and logged.
- **GuestState, every frame:**
  - flags:
    - Ready = boot Ready + fresh Tick + in gameplay;
    - Loading, Dead, InMenu (pause menu, iDroid, popup: §18.6), Cutscene;
    - OnVehicle (status ON_VEHICLE/ON_HORSE, `playerVehicleGameObjectId`, or ridden object);
    - Aiming (PlayerVars HOLD), Sprint (DASH);
    - Standing, Crouching, Prone;
    - Cqc (CQC_HOLD/CONTINUOUS), FirstPerson (SUBJECT/BINOCLE, or riding with MGSV's cockpit camera: §18.10);
    - InBox (CBOX), BoxPoster (its item slot has a poster) (v8, §18.9);
  - mission/location codes;
  - `odoEpoch`, `odo`, `rootPos`/`rootYaw` (owner+0x30 matrix; yaw = atan2(fwd.x, fwd.z)), smoothed `velocity`;
  - camera pos/quat (setter source);
  - `camFovY` (focal length at camera+0xC through `FilmHeightMm`, or `FovOverrideDeg`), `camNear`;
  - life/lifeMax/stamina, stance;
  - `weaponId`/`ammoInClip` (`weapons[currentInventorySlot]`, UNVERIFIED slot meaning), `ammoStock` = 0 (not mapped);
  - `buddyType` (vars), `vehicleId`, `phase`;
  - **buddy (v2):** `buddyFlags` (Present, PosValid, Ridden, Loading, Missing), `buddyObjectId`,
    `buddyAmmo` (`buddyGearAmmoCount[0]`), `buddyPos` (`GameObject.SendCommand(id,{id="GetPosition"})`),
    `buddyYaw` (from its motion; Snake's yaw while ridden).
- **PoseBlock, every frame:**
  - `boneCount` (178 expected) and `frame`;
  - `root` = the binding's model→world matrix (model+0xA0);
  - parents from the model table (or −2 for all if they don't validate);
  - bones: model-space quat and position from the pose buffer.
- **Guest events:**
  - `kGEvFire` / `kGEvBuddyFire`:
    - `a` = equipId | ammoKind<<16;
    - `b` = pellets;
    - v0/v1 = MGSV's own origin and direction after spread;
    - `pad[0]` = bulletId, with bit 31 set for explosive bullets (informational);
    - `pad[1]` (v8) = the round's lethal damage as a share of a 2600-life MGSV soldier, 1/10000 (docs/COMBAT.md "Damage share").
  - `kGEvMelee`:
    - `a` = Kick (attack id 7) or Punch;
    - `b` = radius in mm;
    - v0 = the attack shape centre;
    - v1 = Snake's facing;
    - `pad[0]` = the raw attack id.
  - `kGEvExplosion` (§18.5): Snake's thrown/placed explosives; `a` = AmmoKind (lethal / tranq = sleep gas / stun /
    smoke), `b` = radius (mm), v0 = centre, `pad[0]` = attack id | class << 16 | owner << 24.
  - `kGEvDied` once, when an Elden Ring hit would have killed Snake.
  - `kGEvRumble`.
- **Consumed:**
  - **PadState:** user 0. The pad is neutral without a live host, with `kPadNeutral`, or before the
    sandbox is ready.
  - **Host events:**
    - `kHEvHurt`: life −= a × LifeScale, clamped at 1;
    - `kHEvRespawn`: refill to lifeMax and clear the died latch;
    - `kHEvHeal` (v3): life += a × LifeScale, up to lifeMax (never while he lies dead);
    - `kHEvWarped` and `kHEvStatus` are logged.
  - **Console** at `kOffGuestConsole`.

Unused so far: HostState (camera boom clamp and clock sync come later), ActorTable, draw/HUD slots
(phase 2 capture). The render ring only carries `kRenClearAll`.

## 5. Boot sequence (Pump.cpp, status text in quotes)

1. **wait-lua** "MGSV: starting": waits until our script is loaded in the game VM and Tick answers.
2. **title** "MGSV: at the title screen, pressing Continue": while `gvars.ini_isTitleMode`, an A pulse
   (150 ms) every 2.5 s through the virtual pad. It answers the logos and "press any button". The menu
   opens with CONTINUE focused (MGS5VR). No Up/Down is ever sent.
3. **wait-game** "MGSV: loading the saved game": A every 3 s and `PhantomRing.EndLoadingTips()` every
   3 s, until a `Seq_Game_*` sequence is prepared, no demo plays, and player-camera frames arrive.
4. **load-sandbox** "MGSV: loading Africa free roam (30020)" (`[Boot] Mission`, default 30020): if the mission isn't `[Boot] Mission`,
   - direct load (IH): `gvars.mis_nextMissionCodeForMissionClear` + `ExecuteMissionFinalize`;
   - from the ACC (heli space), or on alternate tries: `ReserveMissionClear{FROM_HELISPACE}`;
   - retried after 90 s; it fails after 3 tries.
5. **settle** "MGSV: arriving in …": 2 s of steady gameplay.
6. **setup** "MGSV: preparing the sandbox":
   - `WarpAndWaitBlock` to `[Boot] Position` if set;
   - `SetEnabled false` on every TppSoldier2/TppHostage2;
   - `TppGameStatus.Set('PhantomRing','S_DISABLE_PLAYER_DAMAGE')`;
   - clock set and frozen;
   - weather;
   - audio: `PhantomRing.Audio(music, ambience, radio)` (§18.2).
   Each result is logged.
7. **buddy** "MGSV: deploying D-Walker (…)", steps of `PhantomRing.Buddy`:
   1. obtained / not dead / can sortie? Otherwise it reports `missing: …`, sets `kBuddyMissing`, and
      boot goes on without the buddy;
   2. `SetSortieBuddyType` + `SetVarsBuddyType` + `TppBuddy2BlockController.Load()`;
   3. wait while the block loads;
   4. `UnsetDisableCallBuddyType` + `CallBuddy(type, Vector3 [Buddy] Ahead m in front of Snake and Side m to his
      right, yaw, true)` (never behind him: the camera is there; §18.3);
   5. `active` once a `TppCommonWalkerGear2`/`TppWalkerGear2` object answers GetPosition.
   It gives up after 60 s.
8. **centre**: 1 s later the sandbox centre is set to Snake's position.
9. **ready** "MGSV: ready (Mother Base, D-Walker deployed)":
   - the player's pad now reaches MGSV, and `kGuestReady` is set;
   - if the mission changes, boot reloads the sandbox;
   - after a reload (more than 2 s out of gameplay), boot redoes settle → setup → buddy → centre;
   - if the game is back at the title, boot goes back to step 2.

`Auto=false` skips steps 4, 6 and 7: the guest becomes ready wherever the player is. `!boot` restarts
the sequence.

## 6. Motion

- **Odometer** (`logic::Odometer`): the sum of root displacements, in doubles. A one-frame jump bigger than
  `TeleportJump` (3 m), or bigger than 0.75 m while one of our warps is pending, is a teleport and is not
  counted. A native placement rebases it, so the placement is never counted either. The epoch bumps on
  every host attach and every time boot reaches Ready.
- **Hold modes** (`[Motion] Hold`):
  - `treadmill`: when Snake is past `Radius` (6 m) from the centre and moving away, or past
    twice the radius in any direction, Lua `Warp` brings him back to the centre. `Warp` is used, not
    `WarpAndWaitBlock` (re-backlog M1: neither fades, and Warp doesn't wait for streaming). He keeps
    his facing (`vars.playerRotY`), and there is a 1 s cooldown. On a vehicle/D-Walker, the ridden
    object is moved with `SetPosition` instead. The deployed buddy is moved by the same shift.
  - `native_treadmill` (default since §19): the same decision, but through the character's own set-position call
    (Hold.cpp), in the same frame. Unlike Lua `Warp` it leaves MGSV's camera where it is (§19).
  - `native`: that call every frame at (centre.x, current y, centre.z) with the current rotation, so
    Snake runs in place (M1 option 1).
  - `off`.
  A warp never happens during CQC, a fall, a demo, a menu, death, carrying, or outside NORMAL_ACTION
  (which should cover dives and rolls; UNVERIFIED). On a vehicle it happens only with
  `TreadmillOnVehicle`. The native modes fall back to the Lua treadmill on vehicles or when their checks
  fail.

## 7. Combat and life

- **Shots:** the `ShotSolver` hook (`0x141044FF0`) reports only rounds from FireWeapon (return
  `0x141041219`).
  - Snake: weapon component vtable `0x1423B3E80` and player index = owner+0x3A0.
  - Other rounds while a buddy is deployed count as buddy fire (the sandbox has no other shooters).
  - Bullet kinds come from TppEquip `BL_*` (tranq → sleep, stun → stagger, smoke/flare/fulton → no
    event, explosive → lethal plus the explosive bit).
  - ShotSolver calls from other return addresses (aim previews, and maybe D-Walker's gun) are logged
    (first 24) for the live test.
  - Fallbacks: `vars.ammoInWeapons` deltas when the hook is refused, and `buddyGearAmmoCount` deltas
    until the hook has attributed a buddy round itself.
- **Melee:** the CQC component (character+0xD8 − 0x38, all vtables checked), with this player's state
  `+0x114` serial and `+0x118` attack id. Each new serial with a non-zero id becomes one kGEvMelee. The
  sphere comes from the attack shape's geometry vfunc when the shape validates
  (`MeleeShapeGeometry`). Otherwise it is 1 m in front of Snake at chest height, radius 1.2 m.
- **Life:** Elden Ring damage × `LifeScale` (4) is written into the native life (`0x142C0147C`, regenerating) and
  `vars.playerLife`. The native one is written only while it agrees with vars within 400 (a frame of MGSV's ~760
  life/s regeneration; an exact compare dropped every hit taken while Snake regenerated: "write did not stick",
  VERIFIED live with both games). The guest checks that the value stuck 8 frames later and warns once if the game
  overwrote it. Snake is never
  taken below 1: the hit that would kill him sends kGEvDied once, and Elden Ring runs YOU DIED and then
  kHEvRespawn. MGSV's own damage is off in the sandbox (`Invincible`).
- **Hurt feedback:** each hit that costs life queues `hurt` for the Lua pump: MGSV's camera noise (its
  camera is Elden Ring's) and blood on Snake's model (`HurtFeedback`, `HurtBlood`; docs/COMBAT.md).

## 8. Battle buddies (D-Walker first)

MGSV runs the buddy entirely: following, orders (the D-pad buddy menu works through the pad),
mounting (A prompt), dash/hover, and its guns. The guest:
- deploys the configured buddy at boot (§5.7), or reports `kBuddyMissing` with the reason;
- publishes its object id, position, derived heading and main-gun ammo every frame, plus `kBuddyRidden`
  when Snake rides it. Snake's root, odometer and `kGuestOnVehicle` then follow the walker, so Elden
  Ring's puppet moves at D-Walker speeds;
- moves it along with every treadmill jump (or moves it *as* the jump while it is ridden);
- reports its shots as kGEvBuddyFire.
Horse, dog and Quiet go through the same code (`[Buddy] Type`). Their GameObject classes are
`TppHorse2`, `TppBuddyDog2` and `TppBuddyQuiet2`. Drawing the buddy in Elden Ring comes with the phase 2
capture (`kOwnerBuddy` draws).

## 9. Saves

- **Steam Cloud** (`ISteamRemoteStorage` v012, verified from steam_api64's version string and the exe's
  own call sites). Vtable slots are swapped:
  - FileWrite / stream write → kept in memory, and return success: a failure would raise TPP's
    "save failed" dialog;
  - FileForget / FileDelete → dropped;
  - FileRead / FileExists / GetFileSize answer from the in-memory copy, so the game stays consistent
    within the session.
  Every dropped write is logged by name.
- **Files:** kernelbase `CreateFileW` / `DeleteFileW` / `MoveFileWithProgressW` are hooked
  process-wide, so the CRT and the in-process Steam client are covered too. A write, delete or rename
  of anything under `\userdata\`, or named `TPP_*`, `MGO_*` or `PERSONAL_DATA*`, goes to
  `<dll dir>\PhantomRingSaveSink\`, and later reads of those names follow it there.
- The launcher also backs up `userdata/<id>/287700` before each run.

## 10. Live test checklist (first run)

**Load and link**
- [ ] `PhantomRingGuest.log`:
  - "activating";
  - "identity: sha256 matches";
  - "vars: 19/19";
  - hooks prepared: lua_pcall, camera setter, SkinPublish, ShotSolver, CreateFileW…;
  - "late init done";
  - "pad: XInputGetState import now returns the shared pad";
  - "proxy: IDirectInput8W wrapped".
- [ ] "link: attached to host session …". `pr status` shows the guest's unix pid and the status texts
  changing through §5.
- [ ] "lua: game VM … found", then "lua: embedded script loaded". `pr lua 'return vars.missionCode'`
  answers. `!status` answers before Lua is up.

**Boot (time each step)**
- [ ] The A pulses get past the logos and title. Note: number of pulses, any dialog they hit
  (online/GZ/EULA), and time to gameplay.
- [ ] Mission 30020 (`[Boot] Mission`) loads by the direct method (else note whether `reserve` was needed). The
  loading-tip prompt is passed (A or EndLoadingTips).
- [ ] Setup log line: number of soldiers disabled, invincible=true, time/freeze/weather true, and what
  `Music` found (and whether BGM actually stops). Soldiers really gone? Staff?
- [ ] D-Walker: each Buddy step's text. Does `TppCommonWalkerGear2` or `TppWalkerGear2` resolve? Does
  GetPosition return a position? Does it appear about 4 m behind Snake?
- [ ] If the save lacks D-Walker: "missing: not developed" and Ready without a buddy.

**Camera / FOV / skeleton**
- [ ] `!status`:
  - `focal` ≈ 21 (normal), 22 (shoulder), 26 (hiding), 32 (CQC)?
  - is `fov` close to what MGSV shows? Calibrate `FilmHeightMm`, or derive it from the render camera
    focal scale.
- [ ] Is the root yaw equal to `vars.playerRotY` (both shown)? Sign and offset.
- [ ] "skeleton: first pose published (178 bones, parents ok)". The host's debug skeleton overlays
  Snake.

**Motion / hold (decides DESIGN §5.2)**
- [ ] `treadmill`: run straight. Every ~15 m a Warp:
  - visible hitch or fade?
  - animation preserved (keeps running), camera continuity?
  - "treadmill: warp didn't land" never appears;
  - the odometer grows ~5–7 m/s with no jumps (`!status` odo, teleports and warps counters).
- [ ] Crouch-walk, crawl, dive, roll and CQC near the edge: no warp mid-action. Is a dive NORMAL_ACTION?
- [ ] `Hold = native_treadmill`, then `native`: log "hold: player location object …". Then check
  whether the running animation plays in place, `vars.playerPos` stays at the anchor, the odometer
  advances, and whether foot sliding or stuck idle appears (re-backlog M1 live test).
- [ ] On D-Walker: odometer at dash/hover speeds. Does `owarp` with SetPosition move the walker with
  Snake on it?

**Combat / life**
- [ ] Fire the tranq pistol, an assault rifle and a shotgun (pellets > 1) one round at a time, and full
  auto. Expect one kGEvFire per round, with the right equip/bullet ids, |dir| = 1 and the origin at the
  hand. Do the "ShotSolver from 0x…" lines appear while aiming? Which ones appear when D-Walker fires
  (mounted and autonomous)?
- [ ] CQC strike, combo, kick, throw in the air: one kGEvMelee each, serial/attack id logged, centre in
  front of Snake within 1.5 m.
- [ ] An Elden Ring hit: life drops by dmg × 4. Does the game keep the written value (no "did not
  stick" warning)? Does the lethal hit end at life 1 with one kGEvDied, and the respawn refill?

**Input / window / saves**
- [ ] The pad drives Snake only after Ready, and is neutral when the ER menu is open or ER is closed.
  MGSV sees no second controller.
- [ ] Rumble events arrive (if MGSV calls XInputSetState at all).
- [ ] MGSV never minimises or steals focus (window guard log lines, if any).
- [ ] `pr lua 'TppSave.VarSave() return 1'`, or reach a checkpoint: "dropped Steam Cloud write
  TPP_GAME_DATA"; no error dialog; the remote/TPP_GAME_DATA mtime is unchanged; nothing new in
  userdata except under PhantomRingSaveSink.

## 11. UNVERIFIED (what the live test settles)

- **Runtime meaning of:**
  - camera+0xC (focal, mm) and the film size behind the FOV;
  - `currentInventorySlot` as an index into `weapons[]`;
  - the vars liveness rate.
- **Lua side:**
  - every Lua API except those IH/MGS5VR use on this build (see GuestScript.cpp);
  - the D-Walker GameObject class and its GetPosition / SetPosition;
  - EndLoadingTips;
  - the music calls;
  - `CallBuddy`'s exact arguments (MGS5VR's D-Dog form is used).
- **Native side:**
  - the native hold call's effect on animation;
  - the melee shape geometry vfunc;
  - D-Walker's fire path;
  - (D-Walker's mode byte and the restore/record methods: VERIFIED live, §18.3);
  - whether `vars.playerLife` writes stick;
  - the Steam vtable swap reaching the in-process client.
- **Stance/flag bits:** these come from `PlayerInfo.AndCheckStatus` names (values in re-backlog M5).
  Dive/roll detection relies on NORMAL_ACTION.

## 12. Offline verification

`cmake --build build --target check_guest` (wine; `-DPHANTOM_WINEPREFIX=…`; optional
`-DPHANTOM_LUA51_SRC=<lua-5.1.5>/src` for the pump test):

| Test | Covers |
|---|---|
| `tests/test_guest_script.lua` (lua5.1) | the embedded script against a mock engine: Tick result order, command parser (Warp vs WarpAndWaitBlock, SetPosition), D-Walker steps, mission loading (direct/reserve/ACC), setup, console packing, an empty engine and an engine that throws |
| `test_guest_pump` (real Lua 5.1.5) | Pump.cpp end to end: VM probe, script load and refs, Tick → frame state, the full boot sequence title → Continue → 10040 → load the sandbox mission → setup → D-Walker → centre → Ready (status texts), frame → Lua commands, the dev console over a real mapping (results, errors, syntax errors), live `phantomring.lua` override, leaving the sandbox; the engine's Lua stack is unchanged after every pump |
| `test_guest_logic` | odometer (teleports, rebase, drift over 100k frames), treadmill (radius, direction, cooldown, height, with the odometer over 100 m), life clamp/death, ammo-counter shots, matrix→yaw/quat, quaternion rotation, focal→FOV |
| `test_guest_link` | attach handshake on a real file mapping, version refusal, kRenClearAll, GuestState/PoseBlock/events/pad flow, `!` console, host restart (re-attach, drain, console reset), host death; the shipped ini equals the built-in defaults; ini parsing |
| `test_guest_proxy` | a dummy exe loads the proxy (`dinput8=n,b`) and gets a working real IDirectInput8W without and with `PHANTOMRING=1` (identity refusal, log checked); the wrapper hides gamepads/joysticks, keeps keyboard/mouse, and handles refcounts and QueryInterface |
| `mgsv/verify_addresses.py` | every signature in Addresses.h against the exe on disk |

## 13. Console

- `pr lua '<chunk>'` runs a chunk in MGSV's game VM and returns every result tostring'd, tab-separated.
  Errors set status 1.
- `!status` shows boot, pump, frame, the Steam guard, host and centre.
- `!boot` restarts boot automation.
- `!treadmill on|off`.
- `!centre` sets the centre to Snake's current position.
- `!debug on|off`.
- `!blast` (explosions sent / kept out), `!menu` (menu state), `!sound ...` (§18.2), `!whistle`, `!follow on|off|spot a s|<m> <ms>|method m|modes list` (§18.3).

`phantomring.lua` next to the DLL overrides the embedded script, and is reloaded within 1 s of a change.
`phantomring.default.lua` is written at start as a starting point.

## 14. Protocol

v9 (2026-10-12 evening): `kPlacedAgain = 1 << 16` in kGEvPlaced `pad[0]` (§18.8). v5-v8: thrown/placed explosives (§18.8),
the throw arc (v6), D-Walker's orders (v7), the damage share and the box flags (v8, §18.9).
v4 (2026-10-12): `AmmoKind::kAmmoSmoke = 3` for kGEvExplosion (smoke / flares: no damage); kGEvExplosion documented
(§18.5). v3 (host work): `kHEvHeal`, applied by the guest (§4).

v2 (this work): GuestState gained `buddyFlags`, `buddyObjectId`, `buddyAmmo`, `buddyPos` and `buddyYaw`
at 0x90..0xA8, plus `GuestBuddyFlags`. `tests/layout_dump.cpp` and `tools/phantomring/proto.py` are in
sync (`test_layout.py` passes). GuestEvent `pad[0]` carries informational extras (bulletId/explosive
bit, melee attack id). The host may ignore them.

## 15. Capture (phase 2): what MGSV draws for Snake

Files: `D3D11Hooks.*` (observation), `D3D11Reflect.*` (DXBC RDEF/ISGN reader, content hash),
`Capture.*` (the decisions; today: the probe), `CaptureMesh.*`, `CaptureAssets.*`, `CaptureRing.*`
(the publishing pipeline: written, NOT built yet, compiled out behind `PHANTOM_CAPTURE_PIPELINE`),
`tools/phantomring/capture_view.py` (host stand-in that keeps what the guest sends; `render` mode still
to write). `[Capture] Enabled = true` (ini).

### 15.1 Getting in (VERIFIED live 2026-10-12)

- `d3d11.dll!D3D11CreateDevice` is detoured from DllMain (MinHook). MGSV calls it once (driver hardware,
  flags 0); the device is adopted inside the detour, before the game creates any resource or shader.
  The game globals are `0x142C6B870` device, `0x142C6B878` immediate context, **`0x142C6B880` the DXGI
  factory (not the swap chain)**, `0x142C6B888` the adapter (disassembly of `0x1419F4EB0`).
- **Never modify the D3D11 vtables.** Swapping vtable slots (device/context) makes MGSV exit cleanly
  (exit code 0) about a second after device creation, before it creates a single shader (seen twice,
  `+process` trace). All hooks are MinHook detours on the DXVK functions the vtables point to (device,
  immediate context, and a deferred context we create once to learn the deferred vtable), as MGS5VR does.
- Present: DXVK's `IDXGIFactory::CreateSwapChain` / `CreateSwapChainForHwnd` are detoured (from a
  factory of our own); MGSV creates its swap chain with `CreateSwapChain` (1024x576 RGBA8 sRGB, 3
  buffers under the headless gamescope), and its `Present`/`Present1` are detoured from there.
- MGSV creates **24 deferred contexts** right after the device and records the whole frame on them
  (worker threads); the immediate context only executes the command lists (~29 per frame), Maps a few
  dynamic buffers and presents. All constant data reaches the GPU through `Map(WRITE_DISCARD)` on the
  recording context (`UpdateSubresource` on buffers: never seen). The guest shadows each context's last
  mapped pointer per buffer and reads constants at draw time from it.
- ~700 draws/frame in the sandbox; the hooks cost nothing visible (the game kept its pace).

### 15.2 The G-buffer pass and the skinning layout (VERIFIED, `!capture probe`)

- G-buffer = the only pass with **3 render targets, all `B8G8R8A8_UNORM`, + `D32_FLOAT_S8X24`**, at the
  game resolution (1280x720 here): ~1050 draws/frame. Shadow maps are 2048x2048 D24S8 with no colour
  target, so they are excluded by construction.
- Every Fox mesh VS has `cVSScene` at **b2** (480 B: `m_projectionView@0`, `m_projection@64`,
  `m_view@128`, shadow matrices @192/@256, **`m_eyepos@320`** = the camera position MGSV renders this
  frame with -- equal to the camera-setter position of the same game frame to 1e-4, so it identifies the
  game frame being drawn), and `cVSObject` at **b5** (208 B: `m_viewWorld@0`, **`m_world@64`**,
  **`m_useWeightCount@128`** (.x: 0 rigid, 1..4 weights), `m_localParam[4]@144`).
- Skinned VS add `VSBones` at **b6** (3072 B): `g_vsBone.m_boneMatrices[32]` (`float4x3`,
  column-major: 3 registers of 4 floats per bone, 48 B) then `g_vsPrevBone.m_boneMatrices[32]` (the
  previous frame's, for motion vectors).
- Both are stored so that **register r is row r of the affine matrix** in the DRAW CONTRACT convention
  (x' = m0·x + m1·y + m2·z + m3): `m_world` rows (0,0,1,820.6) (0,1,0,-11.954) (-1,0,0,1179.1) for
  Snake facing +x (yaw 90°) at his root (820.6, -11.954, 1179.1); bone rows like (0.845, 0.035, 0.534,
  0.009) (0.008, 0.997, -0.079, 1.004) ... with the pelvis ~1.0-1.15 m above the root.
- **Bones are model space, not world**: world = `m_world` · bone_i · v. The DrawRecord matrices are
  therefore `m_world · bone_i` (rigid: `m_world`), minus the anchor in the translation column.
- Snake's draws in the probe frame (Africa apron, idle, rocket launcher on his back): 3 skinned models
  with `m_world` translation **exactly his root** (VB sets 0x58b92870 body, 0x52bba5f0, 0x3f7a6c80; 4 or
  1-3 weights) and 3 attached objects with their own `m_world` 1.0-1.45 m above it (weights 1: gear and
  the launcher, rigidly skinned to their own bones) -- ~50 G-buffer draws in all; nothing else of the
  sandbox lies within 3 m of him. D-Walker had no draws: the guest's buddy position said
  (50.93, -5.10, -22.84), far from Snake, and it was not in view (open issue, see the report).
- Input layouts: Fox creates one layout per vertex format with **every** semantic and aliases the ones
  a mesh lacks onto POSITION's bytes (slot 0, offset 0), so "is it skinned" comes from the **VS input
  signature (ISGN)**, not the layout. Snake's body layout: POSITION R32G32B32_FLOAT @s0+0 (stride 12),
  NORMAL R16G16B16A16_FLOAT @s1+0 and TANGENT R16G16B16A16_FLOAT @s1+8 (stride 32), COLOR R8G8B8A8_UNORM
  @s2+0 from a **DYNAMIC** 4-byte stream, BLENDWEIGHT R8G8B8A8_UNORM @s3+0, BLENDINDICES R8G8B8A8_UINT
  @s3+4, TEXCOORD0 R16G16_FLOAT @s3+8 (TEXCOORD1 aliases it), where **s3 is the s1 buffer again at
  +20 bytes** (one 32-byte interleaved stream bound twice). Vertex/index buffers are IMMUTABLE, one set
  per model, drawn piecewise (startIndex/vertex offsets), R16_UINT indices, triangle lists.
- Pixel shaders name their textures (`fox3ddf_*` family): **`g_tex_diffuse` t0, `g_tex_normal` t1,
  `g_tex_srm` t2**, then a per-material extra at t3 (`g_tex_Dirty` on Snake's body, `g_tex_translucent`
  elsewhere); t4-t7 hold leftovers from earlier draws that the shader doesn't declare; t10-t15 are system
  textures (`g_tex_mesh` t15). Snake's body: diffuse 2048² BC1, normal 2048² BC3, SRM 2048² BC1, Dirty
  1024² BC1, all with full mip chains. PS constants: `cPSSystem` b0 (64 B), `cPSMaterial` b4 (128 B,
  `m_materials[8]` float4).

### 15.3 Publishing (designed; code in CaptureMesh/Assets/Ring, not built yet)

- Owner: G-buffer draw with `cVSObject`; `m_eyepos` picks the game frame (history of the last 64
  camera-frame samples from Frame.cpp); `kOwnerPlayer` if `m_world`'s translation is within 2.5 m of
  Snake's root of that frame, `kOwnerBuddy` within 4 m of the buddy position; a mesh's owner is
  remembered by its source once unambiguous (riding: both are close).
- Mesh (`kRenMesh`): only the vertices the draw's index range references, indices rebased to 0; only
  the VS-read inputs, POSITION-aliased dummies and DYNAMIC streams dropped; Fox's double-bound
  interleaved stream merged back (element offsets shifted). Key = `Hash64` (D3D11Reflect.cpp: FNV-1a
  over 8-byte words with a fold, seeded, length-mixed) of the whole payload after the key field.
- Textures by role from the PS resource names: [0] `g_tex_diffuse`, [1] `g_tex_normal`, [2] `g_tex_srm`,
  [3] the first other declared `g_tex_*` (`Dirty`/`translucent`), [4],[5] further declared ones;
  `g_tex_mesh` and t8+ never. Key = `Hash64` of the mip chain seeded with format/size/mips.
- Copies: staging `CopyResource` at Present, `Map(DO_NOT_WAIT)` on later Presents, 4 starts / 16 MB
  per Present; resources AddRef'd while cached.
- Planned DrawRecord fields: `material` = low 32 bits of the PS bytecode hash (the Fox shader
  permutation), `params[0]` = `m_useWeightCount`, `params[1..7]` = `cPSMaterial.m_materials[0].xyzw,
  [1].xyz`; `kDrawRigid` with one matrix when the VS has no `VSBones`, the weight count is 0, or the mesh
  has no BLENDINDICES. A mesh without BLENDWEIGHT is skinned with weight 1 on BLENDINDICES.x.

### 15.4 Console

- `!capture`: device, Present hook, counters (draws, deferred draws, constant-buffer maps, command
  lists, contexts, layouts, shaders).
- `!capture probe [N]`: describes the next N frames into `PhantomRingCapture.txt` next to the DLL.

## 16. HUD layer (phase 5): MGSV's own HUD for Elden Ring

Files: `Hud.*` (the layer), hooks in `D3D11Hooks.*` (`DrawArgs.startInstance` so a draw can be
replayed exactly), wiring in `Capture.cpp`, `tools/phantomring/hud_dump.py`. `[Capture] Hud = true`,
`HudEvery = 1` (ini). VERIFIED live 2026-10-12 (MGSV alone, fake_er as host).

### 16.1 Which draws are UI

- MGSV runs every UI render job through one executor, **`0x1402E7770`** `(renderer, info, task, worker)`
  (MGS5VR `ui_renderer.cpp` hooks it on this exe; bytes `48 8b c4 57 48 81 ec 80 06 00 00` re-read from
  the local exe). It is detoured with a thread-local depth counter: every D3D11 draw a thread issues
  while inside it is a UI draw. No shader or render-target guessing.
- Live: ~5 UI draws per job, ~50-500 per frame (title dialogs, loading screens, pause menu, iDroid, the
  in-game HUD), all recorded on the deferred contexts into the **1280x720 `B8G8R8A8_UNORM` target with
  the scene depth (`D32S8`)**, i.e. after tone mapping, in gamma space (the final copy to the sRGB swap
  chain comes later). Blend states seen: `SRC_ALPHA / INV_SRC_ALPHA` (most), `SRC_ALPHA / ONE` (glows),
  and one colour-masked draw (write mask 0). `!capture probe` lists them (`UI tid ... blend [...]` lines,
  `N in UI jobs` per pass).

### 16.2 The layer

- Each UI draw into the main UI target (the largest 8-bit colour target UI draws use; smaller offscreen
  UI targets are skipped) is drawn a second time on the same context into our own texture of the same
  size and format, with no depth target and a derived blend state: colour exactly as MGSV blends it;
  alpha `ONE, INV_SRC_ALPHA` for normal blending, replace for opaque blending, unchanged for additive.
  The game's render targets and blend state are restored right after; the game's own draw is untouched.
- At Present (immediate context): copy the layer to one of 3 staging textures, clear it to 0 for the
  next frame, map the oldest finished copy with `DO_NOT_WAIT`, and write it to the HUD triple buffer.
  Never waits on the GPU.
- **Slot format:** `HudSlotHdr{width, height, flags, frame}` + RGBA8 rows (`width*4` bytes, BGRA swizzled
  to RGBA). `flags` bit 0 = premultiplied alpha (always set), bit 1 = sRGB-encoded colour (set only when
  MGSV's UI target is an `_SRGB` view; today it is not: gamma-space values, composite them as they
  are). Composite with `ONE, INV_SRC_ALPHA`. `frame` = the guest's camera frame of the copy.
- A frame whose copy window and the one before it had no UI draws is not published again (the host
  keeps the last, already empty, layer).
- Cost: 62 fps with the layer on (same as off); 3.7 MB copied per frame.

### 16.3 Live results (2026-10-12)

- Title/DLC dialogs, the loading screen, the pause menu, the iDroid map, notifications
  ("Development Requirements Met ..."), the weapon box (ZORN-KP, ammo), the aim reticle while LT is
  held, button prompts (Y "ride" on D-Walker) are all in the layer, matching gamescope screenshots
  pixel for pixel minus the scene.
- Not in it: full-screen post effects that are not UI jobs (MGSV's damage/blood edge is a post-process
  effect, not a UI job, UNVERIFIED which).

### 16.4 Tools

- `pr lua '!capture hud [on|off|every N|reset]'`: counters (UI jobs, UI draws, mirrored, other targets,
  layer size/format, frames published).
- `python3 -m phantomring.hud_dump [-o hud.png] [--over black|white|checker|none] [--straight]
  [--watch N --dir D]` (PYTHONPATH=tools): the newest published layer to PNG without being the reader.
- `pr fake-er --pad-file F`: drive the pad live from a file (`echo "lt=255 rt=255" > F`, `buttons=a+lb`,
  `ly=32767`, `neutral`); `--trace F.csv`: one CSV line per guest frame plus `# event` lines.

## 17. Live findings 2026-10-12 (combat, D-Walker, treadmill)

- **Controls (MGSV's native pad):** A = stance (tap crouch/stand, hold prone), Y = action (mount/dismount
  D-Walker), B = reload, RT unaimed = CQC, LT+RT = fire, Back = pause menu, Start held ~1 s = iDroid.
- **CQC ids** (empty air, RT): 80 = punch (sphere 1.36 m above the feet, r 0.2), 83 = the combo's kick
  (0.3 m above the feet, r 0.3). 83 now maps to `kMeleeKick`. Throws/chokes need a target.
- **Bullet ids (fixed):** `w+0x240` is NOT a bulletId: it is the weapon's resource handle (MGS5VR names it
  so; FireWeapon passes it to the parts system's vf 0x138 = `0x140DBD610`, which indexes by
  `handle >> 16`; the old "bullet 159074" was handle 0x26D62). The bulletId is the **u16 at `w+0x22C`**,
  inside the 0x90-byte gun data copied to `w+0x1B0`. Found by dumping the WeaponState for three weapons
  and checked against their HUD names: ZORN-KP (equip 875) 23 `BL_HgGrenade` (it is a grenade pistol:
  explosive), S1000 AIR-S (871, "STN") 22 `BL_12GAAir` (stun), the rocket launcher (873) 59, a `BL_ms*`
  missile (explosive). Equip ids 871/873/875 are the save's customised weapons (not in the enum dump).
  The alternate fire mode (under-barrel launcher) is UNVERIFIED and logged when seen. Thrown grenades
  don't go through ShotSolver (no event yet).
- **D-Walker:** deployed exactly 4 m behind Snake at boot (VERIFIED: (816.6, -11.95, 1179.1) for Snake
  at (820.6, ..., 1179.1) facing +x). `SetPosition` must use `pos = {x, y, z}`: the old second call with
  `position = Vector3` (or `pos = Vector3`) sends the walker to the world origin and it is lost
  (GetPosition 0,0,0) -- that is what the first treadmill warp did. Fixed in `PR.WarpObject`.
- **Riding (VERIFIED):** Y next to D-Walker mounts it: `vars.playerVehicleGameObjectId` =
  `Player.GetGameObjectIdIsRiddenToLocal()` = the walker (22528), GuestState `OnVehicle`, buddy
  `Ridden`; PlayerStatus ON_VEHICLE is NOT set for the walker. The odometer follows it at ~7 m/s and the
  treadmill moves the walker (owarp) with Snake on it, every ~0.9 s at radius 6, odometer continuous.
- **D-Walker's flamethrower** fires no rounds (no ShotSolver call, ammo 0); MGSV reports GUN_FIRE while
  it burns. While Snake rides the buddy and GUN_FIRE is set, the guest sends ~8 kGEvBuddyFire per second
  along the aim from 1.5 m above the walker, `pad[1]` = 12000 (range in mm, guest extension). Decided per
  GUN_FIRE burst (`logic::FlameBurst`): only a burst with no buddy round at all for 300 ms is flame (a gun
  leaves most frames between its rounds without one), only with the ShotSolver hook installed, and only while `buddyGearMainWPType` is 4 (the flamethrower; any
  other main weapon is a gun whose rounds might bypass the counted ShotSolver call).
- **Treadmill:** MGSV's Warp (Lua) and the native set-position (`native_treadmill`) both put Snake ~0.6-0.8 m
  *below* the root height they are given: he climbs out of the ground over ~7 frames and **drops into a
  crouch**, so a running Snake crouch-walked (3.3 m/s) after the first warp, and the climb went into the
  odometer (+0.55 m height per warp, +6.6 m over 20 s). `[Motion] WarpLift = 0.8` (added to the target
  height) lands him exactly on his feet, still running. The odometer also counts the landing frame's own
  motion (velocity x dt) and holds its height at the warp target for 20 frames after a landing.
  Result (20 s stick forward, radius 6, cooldown 1 s): 15 warps, all landed in one frame, Standing
  throughout, 92.5 m at 4.6 m/s, per-frame odometer step median 7.8 cm / p1 7.4 / p99 8.7 cm (no stall,
  no jump), height drift 0.24 m (stride bob at the end of each hold window, <= 4 cm steps).
- **Pause menu / camera stalls:** MGSV's pause menu stops the player-camera frames (status "loading"
  for its duration). Boot used to redo the whole sandbox setup (warp back to the spot, buddy call, new
  centre) after any such spell over 2 s; now only after a real reload (the mission's prepared/sequence
  state went away).
- **D-Walker while Snake runs on foot:** it does not keep up: every treadmill jump moves it by the same
  shift (so its position relative to Snake stays MGSV's), and it never started following (65 m behind
  after 20 s). Its follow AI may need a buddy command, or the jumps reset its path (open).

## 18. Polish (2026-10-12, live with MGSV + fake_er on `/dev/shm/phantomring_g`)

### 18.1 Recording MGSV for Elden Ring tests (`pr record` / `pr replay`)

- `PHANTOMRING_SHM=<mapping> tools/pr record --out F.prrec --seconds N [--warmup S] [--pad 'ly=32767' | --pad-file F]
  [--hud-every 0.1]` stands in for the host next to a real, booted MGSV (`tools/phantomring/recorder.py`, a
  `fake_er.FakeHost`): a fresh session makes the guest re-send kRenClearAll and every live mesh/texture; it keeps
  every render-ring message, then GuestState+PoseBlock, draw lists, guest events and the HUD layer with their times
  (chunked file, zlib, written as `.part` and renamed when complete). `pr er die|respawn|hurt N` work during it.
- `tools/pr replay F.prrec [--loop] [--from S --to S] [--no-hud] [--no-events]` plays it back as the guest
  (`replay.py`): attaches like the real guest, streams the assets through the ring as the host consumes them (~99 MB
  for Snake + D-Walker, more than the 96 MB ring), republishes at recorded times with its own frame/QPC/odoEpoch;
  `--loop` keeps the odometer growing; re-sends everything when the host re-attaches. `!status` answers.
- Live recordings (Africa airport): idle with D-Walker ahead-right (67 meshes, 132 textures, 686 draw lists in 12 s,
  67 draws each), 12 s running (odometer +56 m, only Snake's draws: the walker stays behind, §18.3), the death
  sequence (§18.4). `tools/phantomring/test_replay.py`: fake_mgsv -> record -> replay -> fake_er, 22 checks.

### 18.2 Audio (`[Audio] Music / Ambience / Radio`, default off; Snake always on)

MGSV's Wwise is driven through its own Lua (`GuestScript.cpp PR.Audio / AudioKeep / AudioRefresh`):
- **Music + ambience:** `TppSoundDaemon.SetMute('Pause')`, the mute MGSV's own pause menu sets. Live: it silences
  the BGM (`Play_bgm_mafr_mission_escape` posted for the test: -32 dBFS -> silence) and the environment (wind, birds,
  insects), and keeps the player's sounds and UI blips. It can't separate music from ambience (Ambience = off also
  mutes the music; Music = off alone only stops the scene BGM / music player).
  - The pause menu resets it when it closes -> `AudioKeep` (every 60 Lua updates) sets it again, never over a
    stronger mute (`CheckHighestMuteMoreThan` means "at least": `CheckHighestMuteIs('Pause')` is excluded).
  - The mute posted during setup can be state-true and still not heard (ambience at -37..-44 dBFS with
    `CheckCurrentMuteIs('Pause')` true). Reset + Set in the same update doesn't fix it (the unmute wins), nor a Set
    a few updates later; `AudioRefresh` at Ready resets it and `AudioKeep` sets it again 90 updates (~1.5 s) later:
    silence (3/3 live). A sound posted while the mute holds stays silent (it is a bus state, not a one-off).
  - Mute names are StrCode64 hashes compared in `0x14096FFB0` (each posts a Wwise event via `0x140972240`):
    SystemPause 1, Pause 2, Cigarette 4, Telop 5, Result 8, Reward 9, Cast 10, GameOver 13, TitleMenu 15,
    TitleOut 16, Loading 17, Title 18, CutScene (special); 6, 7, 12, 14 unnamed. Loading does not silence Snake.
- **Radio:** `RadioDaemon.GetInstance():DisableRadio()` (radio calls: `TppRadio.Play('f1000_rtrg0116')` -24 dBFS ->
  silence; re-enabled: plays) and `TppUiCommand.SetAllInvalidMbSoundControllerVoice()` for the iDroid's voice
  announcements (UNVERIFIED: no way found to trigger "Development Requirements Met" on demand).
- Enemy chatter: the sandbox has no soldiers (all disabled); not handled otherwise.
- Measured (parecord on MGSV's sink input, RMS): idle with everything on -36.5 dBFS; idle muted -79.5 / digital
  silence with Snake's idle blips (-47..-65 for a frame or two); shooting muted -39 dBFS RMS, peaks -12.5 dBFS;
  running muted -37 dBFS (footsteps); weapon switch -32..-38; reload -56..-63.
- Next to Elden Ring (combined live test, both streams measured the same way): Elden Ring's own audio -35..-42 dBFS;
  MGSV idle -71..-74 (silence with blips), Snake running -37, shooting -39 (peaks -12), **D-Walker walking -21 (peaks
  -0.7)**: its footsteps are ~15 dB above everything else and no per-sound or bus volume was found in MGSV's Lua
  (`TppSoundDaemon` has mutes only). A burning flamethrower fire left on the apron: -22 for a few seconds.
- **D-Walker's volume (`[Audio] WalkerVolume`, `Sound.cpp`): NOT solved, experimental, off by default (= 1).**
  What was found (2026-10-12, MGSV + fake_er, parecord RMS on MGSV's stream, 5 s windows):
  - Wwise (AK::SoundEngine, statically linked) located through Fox's wrappers and the queued message types (`AkQueuedMsg`
    numbering here: 2/3 RTPC, 7 State, 11 RegisterGameObj, 12 Unregister, 13 GameObjPosition, 14 ActiveListeners,
    16 ListenerPosition, 18 aux sends, 19 dry level, 20 obstruction, 32 scaling factor): PostEvent `0x14033C050`,
    SetPosition `0x14033D030`, SetGameObjectOutputBusVolume `0x14033CC70`, SetGameObjectAuxSendValues `0x14033CC10`,
    SetAttenuationScalingFactor `0x14033CBC0`, SetRTPCValue `0x14033D090`; RegisterGameObj `0x14033C870` (Fox numbers
    its game objects with a counter, `0x14033E2C0`). Fox's own per-object setters: `0x14032B000..0x14032C600` on its
    sound-object pool `[0x142B9E8B0]` (256 x 0xE0, id at +0x30). Lua: `SoundCoreDaemon.SetGlobalRTPC/ResetGlobalRTPC`,
    sound objects' `SetRTPC/ResetRTPC` (RTPC names unknown).
  - Fox gives most one-shot sounds a fresh game object (each footstep a new id), positions it (`Sound.cpp` remembers
    every position) and posts the event. While the walker walks: `0xDD610D25`/`0xDD610D3B` and `0x382F5287` on objects
    at the walker (and once `0x0A074C55` every frame on never-positioned objects); Snake's run: `0x9C839E1A`/
    `0x9C839E04`/`0x74EA2906`; a shot: `0x710759C5`/`0x710759C6` (`!sound posts`).
  - **None of the object controls changed what is heard:** output bus volume 0 on every object near the walker as it is
    positioned (-29/-21 dBFS before and after), on every recently positioned object, per event before or after
    PostEvent (Snake's steps, a shotgun shot: -22.7 -> -22.8 dBFS loudest), aux sends cleared, scaling factor 0.05.
    The calls return AK_Success. So either MGSV routes these sounds where game-object levels don't reach (e.g. its own
    busses/RTPCs), or the audible voices aren't the posted objects'. Next leads: the RTPCs MGSV's own sound options use
    (SE volume), Fox's `SdxSource` layer, the bus volume path.
  - `WalkerVolume` != 1 installs the detours and the diagnostics: `!sound` (counts, objects near the walker),
    `!sound posts [ms]`, `!sound all [ms]`, `!sound set <id> <gain>`, `!sound event <id> <gain> [mode 1 dry|2 scale|4 aux]`,
    `!sound all-volume|all-scale <v>`.

### 18.3 D-Walker

- **Spawn:** `[Buddy] Ahead = 3`, `Side = 3.5` (right): `x = rx + sin(yaw)*Ahead - cos(yaw)*Side`,
  `z = rz + cos(yaw)*Ahead + sin(yaw)*Side` (VERIFIED: Snake facing +x has +z on the screen's right; walker at
  (823.6, 1182.6) for Snake at (820.6, 1179.1)). The treadmill keeps moving it by the same jump (relative place kept).
- **Following:** MGSV's D-Walker in Normal Mode does not follow on foot, by design, independent of the treadmill
  (treadmill off: it stood still while Snake walked 9, 20, 27 and 150 m away). MGSV's call menu (hold **LB**:
  Knock / **Whistle (call D-Walker)** / Search Mode / Sonar; right stick selects, **RS click** confirms) makes it
  come: placed nearer first when far (50 m -> 20 m), then it walks (~6 m/s) to where Snake whistled and stops.
  - Found natively: a whistle leaves a record on the heap (Snake's position, rotation, flag +0x5C); `!scan`/`!rescan`
    located it and a page-guard write watch (`!watch <addr> pw 12`, `Watch.cpp`; debug registers don't fire under
    Wine) caught the writer `0x141706E3B`, inside the walker's message handler (StrCode64 message ids), message
    `0x438B5DE036C4`. That id is built in one other place: `0x140A4E0B0` = the static buddy system's (`0x142C29030`,
    vtable `0x1422BC070`) vf `0x2D0` (this, command 1..26, u16), which sends buddy commands through the player's
    message box; command 20 = the whistle.
  - `Follow.cpp` (`[Buddy] Follow = true`, `FollowDistance = 2.5`, `FollowEveryMs = 1000`) calls it on MGSV's Lua
    thread when the walker is farther than FollowDistance, Snake isn't riding it, and he isn't dead/in a menu/in CQC.
    It checks the bytes, the vtable and the slot first (a mismatch disables following and logs why). `!whistle`
    once, `!follow on|off`, `!follow <m> <ms>` to tune live; `!status` shows it. No whistle gesture from Snake.
  - Live, fake_er pad stick forward 20 s (Snake 93 m at 4.6 m/s, treadmill radius 6, its warps every ~0.7 s
    moving the walker too), walker distance per 2 s: 5, 14, 15, 12, 10, 8, 6, 3.4, 4.0, 3.2 m (max 16.5, mean 8.1):
    it lags while it gets going (~6 s), then keeps 3-4 m. Same with radius 20: max 14, then 4.5 m. Settings
    compared: 8 m / 1.5 s (max 19, ~5 m at the end), 5 m / 1 s (max 21), 3 m / 2.5 s at radius 6 lost it after
    13 s (stopped answering while the treadmill kept teleporting it; 2.5 m / 1 s didn't).
  - Tried without effect before that: `RequestWalkToPosition`, `SummonBuddy(type, pos, f)` (0x140A49AE0: the
    buddy system's call position, vf 0x210/0x220), `SetCallPosition`, `CallBuddy(.., false)`, a dozen `SendCommand`
    ids. **Never** call `RequestMoveToPosition(type, ..)`: with the walker type it removed the walker (GetPosition
    0,0,0), with another type it swapped the buddy (to D-Horse).
- **Follow spot (combined live test, Elden Ring as host):** MGSV's whistle stores Snake's *own* position as the
  walker's destination. Parked there it stood in front of his muzzle (MGSV then holds fire: the walker blocks the
  barrel; "Snake stun -> world" for every round) or between him and the camera. The walker AI's message handler
  `0x141706CF0` (rcx controller, r8d walker index, r9 message, `[r9]` = StrCode64 id) is detoured (prologue, the
  record lookup `[[ctl+0x60]+8] + index*0x60`, the store at `0x141706E3B` and the `+0x5C` flag at `0x141706E49`
  byte-checked): for the whistles `Follow.cpp` issues (a counter armed per whistle; the player's own LB-menu
  whistle keeps MGSV's meaning), right after the handler stored Snake's position at record `+0x30` the destination
  is moved to `[Buddy] FollowAhead` / `FollowSide` (1 m ahead, 2.5 m left) along the camera's view of Snake (beside
  his *facing*, a Snake turned toward the camera put the walker over the screen); only when the stored position is
  within 3 m of Snake's root. The treadmill's jump moves the destination with the walker (`follow::OnShift`), and
  "far from its spot" is measured from the spot. `!follow spot <ahead> <side>` tunes it live.
  Live: settles 0.4 m from its spot; 7 s stick forward: up to 14 m behind (it starts ~3 s late, then barely gains
  on 4.7 m/s), beside Snake again ~2 s after he stops. A Snake shot no longer hits the walker.
- **Keeping up (2026-10-12, `Follow.cpp`):** what MGSV's walker AI does, from its record (polled through
  /proc/<pid>/mem at 10 Hz during runs): `+0x56` is its state (05 idle, 06 settling, 09 turning, 0A walking, 0C
  stopping); a whistle's `+0x5C` flag is taken ~1-2 s after the command and it walks ~1 s later (a direct write of
  `+0x30`/`+0x5C` is taken the same way, ~2 s); it walks ~6.5 m/s; it re-reads `+0x30` only when it plans (an order, a
  path segment) -- it kept walking 9 m past a Snake who had stopped -- and it **stops within ~3 m of a moving player**
  (at 2.7 m beside a running Snake, then fell 8 m behind before the next whistle restarted it). So:
  - whistle the moment Snake starts *and* stops moving (`!started` both ways);
  - the spot leads Snake by `FollowLeadSec` (0.8 s, at most 8 m) along his velocity, and while he runs it is
    `FollowSideRunning` (-4.5 m) out to the side (beyond the ~3 m stop radius);
  - `FollowSteer`: while it walks to our spot the spot is rewritten as Snake moves; no re-whistle then (each one stops
    and restarts it), only when it stood still for 1 s far from its spot (not when it stands within 3.5 m of an idle
    Snake: it won't come closer);
  - **catch-up** (`FollowCatchUp = 6`): a walker more than 6 m from a running Snake that the camera can't see (behind
    the camera or outside its ~70 deg field, with 2 m for its size) is put (`SetPosition`) 2.5 m behind and 4.5 m beside
    him on the spot's side if that is out of view too, else behind the camera along his motion. Nobody sees it jump.
  - Result (fake_er trace, 20 s stick forward at 4.7 m/s, treadmill radius 6): walker distance per second 2-7 m,
    median ~5 m, one second at 10.9 m before the catch-up (11 catch-ups/whistles in 20 s); Snake stops -> walker 5.0 m
    beside him 1 s later, on the left of the screen. Before: 5-19 m (mean ~10), up to ~8 s to come back.
- **Buddy commands and D-Walker's modes (offline RE 2026-10-12; `Addresses.h`, `verify_addresses.py` all ok).**
  Player report: D-Walker "kept saying 'assault mode'" and would not stay in Search Mode or wait.
  - The command function `0x140A4E0B0` (this, command 1..26, u16 arg) sends one message per command through the
    message box `[0x140BFF050()+0x60]` vf 0x38 to the active buddy (jump table `0x140A4E2D8`). The call menu's labels and
    help texts come from two switches over the same numbers (`0x14110D320` / `0x14110D0D0`, `tpp_hud` lang keys):

    | cmd | message (StrCode64) | call-menu label | buddy |
    |---|---|---|---|
    | 1, 2 | `0x7E380C754E3A` (+2 = 1, u16 arg) | Fire (kill / tranq enemy) | Quiet |
    | 3 | `0x7E380C754E3A` (+2 = 0, arg) | Take aim | Quiet |
    | 4, 5 | `0x56A36C25640E` (byte 1 / 0) | Cover me / Cease cover | Quiet |
    | 6 | `0xE26B1E1A8A9E` | Shoot this (grenade ricochet) | Quiet |
    | 7 | `0xA265A20DFEEF` | Quiet (talk) | Quiet |
    | 8 | `0xE8345CEE216E` | Whistle (call) | D-Dog |
    | 9 | `0xD429F0083A9E` | Wait | D-Dog |
    | 10 | `0x0D5CB991EBF6` | Bark | D-Dog |
    | 11..15 | `0xF28E1C4CC23A` (arg) | Keep 'em busy / Do it (kill, knock out, wound) / Get it (Fulton) | D-Dog |
    | 16 | `0xEDB2466B86D3` | DD (talk) | D-Dog |
    | 17 | `0x7C8CC24E84C3` | Whistle (call D-Horse) | D-Horse |
    | 18 | `0x53F7FDDCAF61` | Stay back | D-Horse |
    | 19 | `0x038AD2EECC28` | Do it (defecate) | D-Horse |
    | **20** | `0x438B5DE036C4` | **Whistle (call D-Walker)** | D-Walker |
    | **21** | `0x7391C316CF59` (u32 = 21) | **Normal Mode** ("disengage from its current mode") | D-Walker |
    | 22, 23 | `0x7391C316CF59` (22, 23) | (no label in the English lang files) | D-Walker |
    | **24** | `0x7391C316CF59` (24) | **Stealth Mode** ("engage stealth camouflage and wait": its *wait*) | D-Walker |
    | **25** | `0x7391C316CF59` (25) | **Search Mode** ("mark nearby enemies") | D-Walker |
    | **26** | `0x7391C316CF59` (26) | **Intercept Mode** ("attack nearby enemies") | D-Walker |

    Lua's `BuddyCommand` enum exposes 3 QUIET_AIM_TARGET, 4 QUIET_COMBAT_START, 6 QUIET_SHOOT_THIS, 10 DOG_BARKING,
    19 HORSE_SHIT (`0x140A518F0`), consistent with the table. No message name was recovered (the exe strings, the game
    Lua and ~6 M word combinations hash to none of them): the names above are the call menu's.
  - The walker handler `0x141706CF0` takes two of them. `0x7391C316CF59` maps the command (`0x141701E80`: cmd - 21) to
    the **mode byte, record +0x5A**: 0 Normal, 1/2 unnamed, 3 Stealth, 4 Search, 5 Intercept. The call menu
    (`0x14174BD20`, only for GameObject type 0x2C = the buddy walker) offers 20 and 21..26 with the current mode greyed
    out. The **whistle** case stores Snake's position/rotation (+0x30/+0x40), sets +0x5C, and **if the mode is not 0
    sets it to 0** and the walker work object's (controller vf 0xF8) reason byte +0x403 to 1 (`0x141706E53`).
  - **The voice is the mode watcher's**, not the command function's or the handler's: `0x1417166C0` runs per walker,
    compares +0x5A with the mode it saw last, and 1 s after a change posts that mode's line through
    `[[0x140BFF050()+0x98]+0x20]` vf 0x1A8 (StrCode64 ids in `0x142B1F4D0..F0`; then the controller's `[+0xC0]` vf 0x1B0
    with `0xFC52211250128A54`). Mode 0 after a whistle (reason 1..3) = `0x25B164268985`, mode 0 chosen (reason 0) =
    `0x18118958133A`, Stealth/Search/Intercept = `0xFDD125A3F355` / `0x7AF3148B5B7C` / `0x0A8BFAD310EE`, modes 1/2 and
    reason 4 silent. The announce log has the matching texts ("D-Walker: Entering auto-search mode", "... Intercept mode
    disengaged", "... Mode disengaged", ...); which spoken line is which is UNVERIFIED (the player's "assault mode" is
    one of them). **No mode change, no voice.**
  - So: every auto-follow whistle while the player had chosen Stealth (wait), Search or Intercept put D-Walker back into
    Normal Mode and made it announce that; the next menu choice was undone by the next whistle (~1 s later). The hotfix
    (256746b, pause on the player's command) stops that; this change also makes it unnecessary to rely on it:
  - `Follow.cpp` now (all byte-checked at Install; a mismatch leaves the command method as it was):
    - acts only when the active buddy object is the buddy walker (`buddyId >> 9 == 0x2C`);
    - learns the walker's controller from the first message the handler sees for a type-0x2C controller (our first
      whistle at the latest) and reads its mode each update; auto-follow only acts in `[Buddy] FollowModes` (default
      `normal`), otherwise the walker is left to its mode (no whistle, no steering, no catch-up). Until the controller is
      known, the player-command hold decides alone;
    - the player's Normal Mode (21) resumes following like their whistle (both leave the walker in Normal Mode);
    - `[Buddy] FollowMethod` (default `command`, the live-tested path): `restore` = the same whistle, but the handler
      detour sets the mode byte to 0 for the handler's call and puts it back right after (same call, same thread), so
      the whistle case neither resets it nor sets the reason byte, and the watcher sees no change; `record` = no
      whistle at all: +0x30 (the spot), +0x40 (Snake's facing, `logic::YawQuat`), +0x5C = 1 written directly, as the
      whistle case writes them, minus the mode reset (until the controller is known it whistles with `restore`).
      Both are **UNVERIFIED live**: the in-process mode read/write, the record order being taken by the AI (a write of
      +0x30/+0x5C from outside was, above), and whether MGSV's "placed nearer first when far" still happens without the
      whistle message (it is not in the handler; may be the message path's).
    - console: `!follow method command|restore|record`, `!follow modes <normal stealth search intercept mode1 mode2|all>`;
      `!status` shows method, walker mode, controller, record orders and kept modes.
- **Live check of the modes and methods (2026-10-12, MGSV + fake_er pad on `/dev/shm/phantomring_g`):**
  - **How a player gives D-Walker an order:** hold **LB** (the call menu: Knock up, Whistle (call D-Walker) up-right, the
    current mode greyed out right, the other modes down-right, Sonar down), push the **right stick** toward an entry,
    **click the right stick** ("Select") while still holding LB. The HUD menu (`0x14110F0B0`) then calls the buddy system's
    vf 0x2D0 = `0x140A4E0B0` with the entry's command -- the function `Follow.cpp` hooks: Search Mode logged `the player's
    buddy command 25` and `D-Walker mode normal -> search`, Normal Mode 21 (`search -> normal`), Whistle 20. This save's
    walker offers only Whistle, Normal and Search Mode (Stealth/Intercept: not developed). The host passes LB, the right
    stick and its click to MGSV untouched (er/InputBridge.cpp masks only Start/View and the ER prompt button), so the
    menu works through Elden Ring. **Sessions 2 and 3 have no `the player's buddy command` line at all:** no command was
    ever confirmed (releasing LB cancels; only the stick click selects) -- so the player's "it does not react to my
    commands" and "whistling does not call him" were orders never given, not orders lost.
  - **The voice:** MGSV's voice system (`0x142A79190`, vf 0x1A8 `0x140A7E610`) is now detoured to count and log what MGSV
    says (`voice: MGSV says 0x...`, `!status`). Choosing Search Mode -> `0x7AF3148B5B7C -- D-Walker's mode announcement`
    0.9 s later, Normal Mode -> `0x18118958133A`: the announcements come only from mode changes (§ above).
  - **Methods** (20 s stick forward at 4.7 m/s, treadmill radius 6; walker distance from Snake):
    command: max 18.5 / 10.5, median 5.5 / 5.3 m (two runs, the first with the usual ~4 s start lag); record: max 10.3 /
    7.3, median 4.7 / 5.1 m; standing still afterwards both settle 2.7-2.9 m beside him within 2 s. record sent no
    message at all (`whistles 0`, 23 record orders + 19 out-of-view catch-ups in 60 s).
  - **A far walker** (warped 70 m away in Normal Mode, Snake idle): with record, MGSV still placed it ~22 m away 3 s later
    and it walked over (2.7 m at 9 s): the "placed nearer first" is the walker AI's planner, not the whistle message. The
    same with command, and with the player's Normal Mode after a Search-Mode walker was left 74 m behind.
  - **restore:** our whistle in Search Mode logged `our whistle kept D-Walker in search mode` and the mode stayed (no
    announcement); the far walker was placed nearer but, in Search Mode, didn't come to Snake (MGSV's search behaviour).
  - **Search Mode chosen by the player:** auto-follow paused (the command hook) and left to its mode (mode read): Snake ran
    away 74 m in 16 s, no order, no whistle; Normal Mode from the menu brought it back (placed ~22 m, walked).
  - **Default now `FollowMethod = record`, `FollowModes = normal`:** auto-follow never sends a message, so it can't change
    the mode or make it talk; a mode the player chose stays; Normal Mode follows; far walkers come back. 60 s of mixed
    running/standing: **0 voice lines** (`voice:` counter), 0 mode changes, distance median 4.9 m (max 13.1 m); MGSV's
    stream (parecord) -24..-28 dBFS throughout: the walker's footsteps (§18.2), no announcement-shaped bursts (the
    reference: 1.25-1.5 s speech-band bursts at -22..-26 dBFS right after each menu mode change).
  - The player's own whistle keeps MGSV's meaning: the walker comes to Snake himself (1.3 m) and goes back to Normal Mode.
- **Orders shown in Elden Ring (protocol v7, combined live test 2026-10-12 afternoon):** the command hook pushes
  `kGEvBuddyOrder` (a = command, b = buddy type by command range) for the player's orders; the host shows them in the corner
  and coaches LB holds (docs/HOST.md "D-Walker's orders made visible"). MGSV takes the right-stick click only with the stick
  back at the centre (the entry stays selected): 2 of 2 clicks with the stick pushed did nothing, 4 of 4 after letting it go
  gave the order. Live with Elden Ring: Search Mode (25) and Whistle (20) through the host's pad routing; Search Mode left the
  walker behind while Snake ran 68 m; the whistle brought it from 68 m (MGSV placed it 21 m away within 4 s, beside Snake 6 s
  later); 10 min of play: 13 player orders, 13 mode changes, 12 voice lines, 0 whistles of ours.
- **Draw owners:** a captured draw belongs to whichever of Snake's root (2.5 m) / the buddy (4 m) is nearer. With
  the walker following within 2.5 m, "near Snake first" tagged 66 of 67 draws as Snake's: no buddy fade, ground snap
  or occluder test in Elden Ring. Now 53 Snake + 14 walker draws.
- **Gun instead of the flamethrower:** `vars.buddyGearMainWPType` is 4 (flamethrower) in this save (grade 3; sub 5);
  writing 1 + `TppBuddy2BlockController.Reload()` changed nothing visible (UNVERIFIED; restored to 4). The flamethrower
  path (§17) stays.
- **Revival** (`[Buddy] Revive = true`, `PR.Buddy(kind, x, y, z, yaw, revive)`, `Pump.cpp` `ReviveBuddy`): a buddy destroyed in
  MGSV's world (live 2026-10-12 evening: MGSV's own blast of a CGM 25 rocket fired with D-Walker in MGSV's line of fire, 4.7 m
  from Snake; `buddy: D-Walker gone`, and the next boot said `D-Walker: missing: dead`) has no repair in Elden Ring. At boot
  `PR.Buddy` now clears the dead mark (`TppBuddyService.UnsetDeadBuddyType`) before calling it; while Snake plays, a buddy
  object gone for 10 s is revived and called next to Snake, retried every second for a minute, then every minute. The
  cause is gone too (§18.5 `MgsvBlasts`); the revival itself is UNVERIFIED live (no buddy was lost after it was built).
- **MGSV's markers off** (`[Boot] ClearMarkersSec = 5`, `PR.ClearMarkers` = `TppMarker.DisableAll`): at the setup and every
  5 s while Snake plays (D-Walker's Search Mode and Snake's own looking mark MGSV soldiers again). A marked MGSV soldier near
  the sandbox showed in Elden Ring as MGSV's floating distance marker and pulled the launcher's aim onto itself (live: aim
  stuck ~30 deg off the Elden Ring target, the rockets flew there). Clearing the markers removes the marker; MGSV's aim
  assist still snaps toward those (disabled, invisible) soldiers (README Known issues). Live: `boot: setup: warp=ok
  disabled=542 markers off=true invincible=true`.

### 18.4 Death (`[Combat] DeathCollapse = true`, `Death.cpp`)

- Trigger: the life clamp latches (kGEvDied sent) or HostState `kHostPlayerDead`, while ready. The host now keeps
  `kHostPlayerDead` from the death until its respawn load (ER's dead bit clears seconds earlier, docs/HOST.md
  "Dying"), so Snake stays down through "Choose revival location" and the load (VERIFIED with both games).
- Snake standing: his own dive (an X pulse) into prone; crouched/prone/riding: `Player.RequestToSetTargetStance(3)`
  (PlayerStance STAND 1, SQUAT 2, CRAWL 3, VERIFIED). After 1.2 s the prone stance is made sure. The player's pad is
  cut off (`pad::SetBlocked`) until kHEvRespawn; then `RequestToSetTargetStance(1)` and the pad comes back 0.9 s
  later. Also up when the host goes away, or 3 s after `kHostPlayerDead` clears with no respawn event, or 120 s.
- MGSV's own death/game over is never triggered (life stays >= 1).
- VERIFIED live with fake_er (`pr er die` / `pr er respawn` / `pr er hurt 2500`): dive -> "down", stick input ignored
  (root unchanged), respawn -> Standing, life refilled 7801/7801, pad back; screenshots. The dive moves him ~4.8 m
  forward (odometer): the host stops the puppet while dead, so it doesn't show.
- For the host: `[Render] death_visual = keep` (draw him lying there) instead of the fade.

### 18.5 Grenades and explosives (`Blast.cpp`, `[Combat] Blasts = true`; VERIFIED live 2026-10-12, fake_er trace)

- **Controls:** D-pad Right selects the support weapon (`Player.ChangeEquip{slotType = PlayerSlotType.SUPPORT, equipId = ...,
  toActive = true}` selects one of the slots too), **LT** shows the throw arc, **RT** throws. D-pad Up = primary, Down =
  secondary (handgun).
- **Where MGSV makes blasts:** every blast reaches the static attack-area system (`0x142C1E900`, vtable `0x1422A54F0`)
  through **vf0 `0x140A0FE50` (this, BlastDesc*)**. Found live: the equip parameters (`0x142A711F0`, `TppEquip.
  ReloadEquipParameterTables`) keep the BLA rows at `+0x98` (6 bytes per `TppEquip.BLA_*`: i16 radius x10, i16 inner
  radius x10, u8 class 0x4B blast / 0x1C gas-flare / 0x38 fire / 0x4D missile); a page-guard read watch on the sleep
  grenade's row caught the thrown-weapon system (`0x1415D9D10`: per thrown object, fuse/cloud timers) reading it and
  calling vf0 at `0x1415DA4A0`; a watch on the system's vtable pointer caught the frag grenade's call at `0x1415D9804`.
  BlastDesc: `+0x00` Vec4* centre, `+0x08`/`+0x10` Vec4* (capsule end, direction; 0 = sphere), `+0x30` f32 radius (m),
  `+0x3C` u16 attack id (`TppDamage.ATK_*`), `+0x3E` u8 the row's class, `+0x3F` u8 owner index (5 = Snake here).
- **Kind by attack id** (`logic::KindOfAttack`; all 492 `TppDamage.ATK_*` ids read live from the game's own table):
  lethal (grenades 178-180/450/457-459, molotov 191-196, C4 197-199, claymore/D-mine 202-204/451, anti-tank mine
  205-207, BombAssist 212), sleep (`ATK_SleepingGusGrenade` 188-190, mines 208-210, the gas cloud `ATK_SleepGus` 286 /
  287, SleepGusAssist 24), stun (stun grenades 182-187/452-465, ElectromagneticNetMine 211, StunDarkMatter 483-486),
  smoke (181, the cloud 284/285, SmokeAssist 23, flares 26/27), **round** (rockets 167-172, `ATK_GrenadeHit` 174, cannons
  11-14, walker missile 17, mortar 282/283, tank guns 301-306, Grenader 315-318, the per-weapon `ATK_1xxxx/2xxxx` ids:
  never sent, see below), everything else
  ignored (fuel drums `ATK_GimmickBlast` 123, vehicles, helicopters: MGSV's world, not Snake's). Blasts more than 150 m
  from Snake are ignored too.
- **kGEvExplosion:** `a` = `AmmoKind` (Lethal / Tranq = sleep / Stun / **Smoke (v4)**), `b` = radius in mm, `v0` = centre
  (MGSV world), `v1` = (0, 1, 0), `pad[0]` = attack id | class << 16 | owner << 24.
- **Live (fake_er trace, Africa apron):**
  - hand grenade (`EQP_SWP_Grenade_G04`, thrown 32 m with LT): **one** event, `ATK_Grenade_G2` 180, r 7.0 m, class 0x4B,
    lethal, at the landing spot (852.98, -11.67, 1179.43);
  - sleep grenade (`EQP_SWP_SleepingGusGrenade_G01`): 189 at impact, then the cloud **`ATK_SleepGus` 286 every 0.5 s for
    ~12 s (24 events), r 9.0 m**;
  - smoke grenade: 285 at impact, then 284 every 0.5 s for ~60 s (r 8.0 m), sent as Smoke (the host does nothing with
    them);
  - ZORN-KP grenade round (bullet 23): one kGEvFire with the explosive bit (`pad0 0x80000017`); its MGSV blast is
    `ATK_10603` = 347 (r 7.0 m, class 0x4D, 37 m away where it landed in MGSV's world) and is **not** sent. Not double
    counted. The per-weapon attack ids named by number (`ATK_10307`..`ATK_20xxx`: 319-445, 454-456, 466-477, 487-488) and
    the EX weapons (446-448) all count as gun rounds.
- **One route per explosive (for the host, docs/COMBAT.md):** gun-fired explosive rounds stay kGEvFire + explosive bit
  (the host blasts where the round hits in Elden Ring's world); thrown and placed explosives are kGEvExplosion at MGSV's
  blast centre. The guest never sends both for one explosion.
- **MGSV's own blast of ours is skipped** (`[Combat] MgsvBlasts = false`, the default): a blast that goes to Elden Ring as
  ours (Snake's throw, a mine set off through kHEvDetonate) and the blast of a round Snake's gun fired (rockets, grenade
  rounds: Elden Ring blasts them where they hit its world, kGEvFire) within 150 m are reported as before, but the detour
  returns 0 without calling MGSV's vf0 (every caller ignores its result: the thrown-weapon system's two calls jump on, the
  Placed System's returns; Ghidra 0x1415D9806, 0x1415DA4A2, 0x140FC2BDC). In MGSV's sandbox those blasts could only hit
  Snake's own buddy: every mine lies within the treadmill's disc around Snake, and a CGM 25 rocket fired with D-Walker in
  MGSV's line of fire destroyed it (live). VERIFIED live with both games (2026-10-12 evening): `blast: attack 209 (sleep) r
  8.00 m class 0x1c owner 5 at ... (0.9 m from Snake) -> kGEvExplosion (of placed #1); MGSV's own blast skipped (Elden
  Ring's)` with D-Walker 2.1 m from Snake; Snake and the walker untouched, Elden Ring's sleep mist where the mine lay.
- Console: `!blast` (counters per kind, last attack id, last kept-out id, how many of ours were not blasted in MGSV). The
  log names the first blast of every attack id, each id again at most every 5 s, and every kept-out one.

### 18.6 MGSV's menus (`Menu.cpp`, `[Menu]`; VERIFIED live 2026-10-12)

- **Which button:** Back (View) = MGSV's **pause menu**; Start **held ~1 s** = the **iDroid**.
- **Pause menu:** MGSV's Lua can't see it, and it stops the player-camera frames (so GuestState used to stop and the boot
  sequence said "loading"). `TppGameSequence.GetPauseMenu` (`0x14053A320`) returns `[[0x142BD3B80]+8]`, the
  `TppPauseMenu` (vtable `0x14218F648`); its u32 **`+0x38` is 1 while it is open** (memory diff open/closed, then 3 open/close
  cycles). Read natively (vtable checked every time) by the frame and by the Lua thread.
- **iDroid / popups:** `TppUiCommand.IsMbDvcTerminalOpened()` / `IsShowPopup()` (Tick's MENU bit). Frames keep coming.
- **kGuestInMenu** is set in the GuestState of the frame the menu is seen and cleared in the first one after it closes
  (fake_er trace: frame 1665 set, 1666 cleared after an 18 s pause; iDroid 2106 -> 3994). While the pause menu stalls the
  frames, the Lua thread (it keeps running) republishes the last GuestState with InMenu, keeps the heartbeat and the
  "ready" status (`pr status` during an 18 s pause: `Ready|InMenu`, heartbeat 7 ms). The host gives MGSV the whole pad
  while the flag is set.
- **Safe pause menu** (`SafePauseMenu = true`): `TppUiCommand.RegisterPauseMenuPage{GamePauseMenu.CONTROLS_AND_TIPS_ITEM,
  GamePauseMenu.OPEN_OPTION_MENU}` at the sandbox setup: the pause menu shows only CONTROLS & MANUAL and OPTIONS
  (screenshot; was CHECKPOINT, RETURN TO ACC, RETURN TO TITLE MENU, STORE, RECORDS, ...).
- **iDroid** (`BlockMissions = true`): `TppUiCommand.SetMbTopMenuItemActive(TppTerminal.MBDVCMENU.X, false)` for
  MSN_MISSIONLIST, MSN_SIDEOPSLIST, MSN_LOCATION, MSN_HELI(_RENDEZVOUS/_DISMISS), MSN_RETURNMB, MSN_FOB, MSN_EMERGENCIE_N/F
  and every buddy DISMISS: HELICOPTER, MISSION LIST and SIDE OP LIST show struck out (screenshot). Still there and harmless:
  Supply Drop (weapons, ammo), Buddy Support (other buddy commands), Fire Support, Combat Deployment, Challenge Tasks,
  Cassette Tapes, Log, the map, Mother Base management. A fire-support barrage's blasts carry attack ids the guest
  ignores.
- Console: `!menu`.

### 18.7 Loadout (`[Loadout]`, `PR.PreLoadout` / `PR.Loadout`; VERIFIED live 2026-10-12)

Player report (session 4): "Would be nice to have a tranquilizer dart setup." The default handgun is now the WU S.PISTOL.

- **Weapons go in before the sandbox mission loads.** `[Loadout] Primary` (hip, slot 0), `Primary2` (back, 1) and `Secondary`
  (handgun, 2) are written into the sortie loadout vars -- `vars.initWeapons[slot]`, `vars.weapons[slot]`, `vars.ammoInWeapons[slot]`
  = the magazine of `TppEquip.GetAmmoInfo(id)` -- by `PR.PreLoadout` right before the boot's `LoadMission` (`Pump.cpp`,
  load-sandbox). MGSV then loads the weapons' models with the mission and fills their ammo stock itself (`ammoStockIds` got BL 10
  x 21 for the WU S.PISTOL). At the setup `PR.LoadoutMissing` checks the loaded mission; if a weapon isn't there (the mission was
  already loaded, or MGSV restored another loadout) the guest writes it again and reloads the sandbox once (`LoadMission
  direct`; the boot waits for the old mission to go, then redoes settle/setup).
  - Live, boot from the ACC (`ReserveMissionClear FROM_HELISPACE`): `boot: loadout before the load: EQP_WP_West_thg_010(7)=slot 2 was
    875` ... `boot: loadout weapons: all in the mission` (no extra reload; ready 2 min 20 s after launch). Snake draws the WU
    S.PISTOL (D-pad down), aims it (LT, reticle), scopes it (RB) and fires: `kGEvFire a = 0x10007` (equip 7 | Tranq), bullet 10
    `BL_HgTranq`, third person and scoped (screenshots; `tranq_pistol_scoped.prrec`).
  - Live, mid-mission (console `PR.PreLoadout('EQP_WP_East_ar_010', 'EQP_WP_West_sr_010', '')` + `LoadMission(30020, 'direct')`):
    the assault rifle and sniper rifle were drawn and fired after the reload (`scoped_rifle_sniper.prrec`).
  - Why not mid-mission: `Player.ChangeEquip` changes the slot (HUD, vars) but a weapon whose model the mission didn't load can't be
    drawn; a runtime `RequestLoadToEquipMissionBlock` + `StartLoadingToEquipMissionBlock` didn't load it; adding it to
    `TppRevenge._AllocateResources`' list hung the next load.
- `Support` (comma list, up to 8) stays `Player.ChangeEquip{slotType = PlayerSlotType.SUPPORT, ...}` at the setup (grenades and mines
  need no reload). `PlayerSlotType`: PRIMARY_1 0, PRIMARY_2 1, SECONDARY 2, SUPPORT 3, ITEM 4, STOLE 5, HAND 6.
- The defaults: `Primary = EQP_WP_East_ar_010` (an SVG-76 assault rifle, lethal: the save's S1000 AIR-S was a *stun* shotgun),
  `Primary2 = EQP_WP_West_ms_010` (the FB MR R-LAUNCHER, unguided rockets; `EQP_WP_Com_ms_010`, tried first, is the KILLER BEE
  anti-air launcher, which wouldn't fire at ground targets), `Secondary = EQP_WP_West_thg_010` (WU S.PISTOL; grades
  `_020`..`_050`). Empty keeps the save's weapon in that slot (the save's handgun, the ZORN-KP grenade pistol, comes back with
  `Secondary =`). Live 2026-10-12 evening: `boot: loadout before the load: EQP_WP_East_ar_010(36)=slot 0 ...`, then `vars.weapons`
  36, 54, 7 after the FB MR's reload. `TppMotherBaseManagement.IsEquipDeveloped{equipID}` answers false for every weapon id 1-128 (it tracks
  items), so "the best developed grade" isn't known to the guest: the ini names it.
- MGSV's own delivery works too: iDroid -> SUPPLY DROP -> Handgun -> BURKOV TB (darts) -> the box lands ~20 s later, Y picks it up
  (`kGEvFire a = 0x1036b`, equip 875 | Tranq).

### 18.8 Thrown and placed explosives for Elden Ring's own physics (`Throw.cpp`, `Placed.cpp`; protocol v5/v6; VERIFIED live 2026-10-12, fake_er + recorder)

Player report: "I can't see the trajectory or where the grenade lands", "sleep grenade did not work at all, neither did mines".
MGSV flies a grenade over its own flat apron and its mines only answer MGSV's characters (there are none in the sandbox), so
the host now flies its own copy through Elden Ring's world and Elden Ring's enemies set the mines off.

- **Throwing System** (Addresses.h `kThrowSpawn`): 24 worker views (vtable `0x1424293B0`) over shared struct-of-arrays;
  per slot `+0x90` launch point, `+0x98` launch velocity (gravity 9.8, predictor `0x1415D6870`), `+0x110` position,
  `+0x120` equip id, `+0x128` records (`+4` fuse left, `+0x1C` flags: bit 0 live). `Spawn(view, desc) -> slot`
  (`0x1415D2210`) creates every thrown object from the views' request queue; a bounce re-launches in place (new launch
  point/velocity, same slot). Found by polling the views from /proc/<pid>/mem while throwing.
- **kGEvThrow** (Spawn detour, after the original): `v0` launch point, `v1` launch velocity, `a` AmmoKind, `b` id, `pad[0]` =
  equip | fuse ms << 16 (molotovs 0). Only explosives (`logic::ExplosiveOf`: frag 350-355/1536-1538, molotov 382-384, sleep
  377-379, stun 372-375/1539-1541 + StunDarkMatter, smoke 357-361 and flares); magazines and decoys are logged once and skipped.
  Live: sleep grenade 378 `(821.02,-10.01,1179.43)` / `(14.31,8.53,0)` m/s, fuse 3.00 s; frag 354 the same aim, its blast
  (attack 180) 3.02 s later at MGSV's landing 32 m on; a running throw (stick forward) has the same velocity: MGSV doesn't add
  Snake's motion. Gas/smoke grenades: the cloud starts at the fuse, their first blast (189 / 285) comes ~5.0 s after release.
  An RT without LT throws nothing (it is CQC).
- **The throw id on the blast:** `kGEvExplosion pad[1]` = the id of the throw (or placed explosive) it belongs to: the
  blast's centre is matched against our live projectiles (their slot positions read inside MGSV's update, 3 m; placed ones
  4 m: the claymore's centre sits 2.5 m in front of it), by kind and by attack-id family (placed: 197-211/451; clouds
  284-287: either). Throw and placed ids share one counter (unique per MGSV session). Live: every 189/286 puff of a sleep
  grenade carried its throw id, the 180 of the frag its own.
- **Aim arc (GuestState v6, `kGuestThrowAim`):** while Snake aims (PlayerVars HOLD) with a throwable support item in hand
  (`vars.currentInventorySlot` 3, `supportWeapons[currentSupportWeaponIndex]`), the guest reads MGSV's own arc: the
  "throwing line" object `[0x142BE6D08]` (vtable `0x1421F5008`), points 0.075 s apart; `aimThrowPos` = its first point,
  `aimThrowVel` = the velocity its first three points imply (`logic::ArcVelocity`, gravity 9.8). Live: the last aim frame
  before a release equals the release's kGEvThrow to 0.01 (`(821.02,-10.01,1179.43)` / `(14.31,8.53,0)`); the arc rises from
  `(821.17,-11.10,1179.52)` / `(11.52,4.52,0)` over ~0.4 s as Snake raises his arm. ~110 aim frames per 1.9 s of LT.
- **Placed System** (`kPlaced*`): `[[hub+0x98]+0x208] - 0x20`, all placed objects of the map (252 here: the map's own
  locator mines/decoys first, Snake's from index `[+0x88]` = 220); per object `+0x28` position, `+0x30` quat, `+0x38` equip,
  `+0xB0` 3 flag bytes (byte 0 bit 0 live, **bit 2 set off**). Its views (`0x1415D35D0`) set off every live object with bit 2:
  C4/claymore/anti-tank mine blast (`0x140FC2680`), the sleep-gas mine puffs (209 then 286), the EMN mine casts its net
  (no attack-area blast). Every placement is mirrored into vars (`placedWeapons/Pos/Rot`, 32 slots, saved with the game),
  its removal clears `placedWeapons[i]`.
- **kGEvPlaced / kGEvPlacedGone:** the guest polls the vars mirror every frame (only explosives, within 150 m of Snake):
  `v0` position, `v1` = MGSV's blast direction (quat x +Z), `a` AmmoKind, `b` id, `pad[0]` equip | (v9) `kPlacedAgain`
  (bit 16), `pad[1]` trigger radius (mm: claymore/sleep/EMN 3000, anti-tank 1500, C4 0 = remote only; UNVERIFIED radii,
  MGSV's own are in its sensors). A new host session gets every live one again, and so does every new start of the host's
  puppet (HostState kHostPuppeting going up: a death, a load, an MGSV stall; only sent while the host puppets,
  `logic::RisingEdges`). A repeat carries `kPlacedAgain`: MGSV's point maps through the host's *new* anchor, not to where
  the mine lies in Elden Ring, so the host keeps a mine it knows where it lies and has MGSV set off one it can't place
  (kHEvDetonate; docs/HOST.md "Thrown and placed explosives"). Explosives already in the vars when the sandbox first
  became ready (an old save's) are MGSV's world: never announced (`-- already in the save when the sandbox loaded`). Live:
  `placed #1: announced again (the host's puppet restarted)` -> the host `mine 1 announced again -- kept where it lies`.
- **kHEvDetonate:** the guest ORs bit 2 into that object's flag byte (atomic, after checking the system's vtables, the index
  range and that the object is the same live equip) -- MGSV's own path does the rest. Live: sleep mine placed -> `pr er
  detonate all` -> blast 209 0.67 s later with `pad[1]` = its id, the cloud's 286 with it too, kGEvPlacedGone ~3.5 s later;
  the EMN mine (bit 2 via /proc/<pid>/mem) cast its net and was gone. The EMN mine has no blast: its kGEvExplosion (stun, 5 m,
  `pad[0]` 211) is sent by the guest. C4's remote detonation is the player's (MGSV sets the same bit; untouched).
- **Session 3's "no sleep blast":** that log has 3 frag blasts (180) and no 189/286 at all, i.e. no sleep grenade went off in
  MGSV. The save's support list is sleep grenade 378, **sleep-gas mine 418**, smoke 361, frag 354, shock decoy 406, magazine
  349: the item after the sleep grenade is the sleep mine, and a mine placed in the sandbox never goes off (no MGSV enemy).
  Most likely the player placed sleep mines. Now every throw logs `throw #N: equip E` and every placement `placed #N`.
- **More items** (put in an empty support slot with `Player.ChangeEquip{..., toActive = true}`; a reload of the mission
  makes MGSV load their models): stun grenade 372 -> kGEvThrow (fuse 3 s), its blast 182 (stun, r 10 m) 3.0 s later with
  the id; molotov 382 -> kGEvThrow fuse 0, attack 192 (fire, r 1 m, class 0x38) where it hit 2.0 s later with the id;
  **C4** 391 is placed (LT+RT sets it at Snake's feet: Placed System, not thrown) -> kGEvPlaced trigger 0, kHEvDetonate ->
  attack 197 (r 8 m) with its id, gone; **claymore** (M21 D-mine 413) -> kGEvPlaced facing (1,0,0) for a Snake facing +x,
  kHEvDetonate -> attack 202 r 2.5 m centred 2.5 m in front of it, gone (`place_claymore.prrec`); **anti-tank mine** 421 ->
  kGEvPlaced trigger 1500, kHEvDetonate -> attack 205 r 8 m with its id, gone; **EMN mine** 425 -> kHEvDetonate -> MGSV's
  net, the guest's own kGEvExplosion (stun, 5.0 m, attack 211, its id), gone. (Any item goes into the selected support
  slot with `Player.ChangeEquip{slotType = SUPPORT, ..., toActive = true}`; these mines could be placed without a reload.)
- **Snake's mines are Elden Ring's to set off:** MGSV's claymore answers MGSV's own characters: live, 3 of 3 claymores
  went off 0-1.7 s after they were placed while Snake ran on (his own GameObject 0 in the cone: the setter MGSV passes).
  MGSV sets an object off through `SetOff(system, setter, object)` (`0x140FC8E50`, from its proximity sensor
  `0x140FC6370` and two other paths); the guest refuses it for Snake's own mines (object index >= the system's first
  player index, a placed explosive with a trigger radius: not C4) -- live: `MGSV wanted to set off Snake's mine (object
  220, equip 413, by GameObject 0x0) -- refused`, 230 refusals while Snake stood in the cone (once per frame, logged once),
  the claymore stayed, and kHEvDetonate then blew it (202, its id). kHEvDetonate ORs the bit itself (not through SetOff)
  and C4's remote detonation (byte-0 bit 3 on every C4, turned into bit 2 one by one every 0.05 s by `0x140FC7E10`) never
  goes through SetOff either. Second line of defence: a mine's blast (attack 202-211/451, and the clouds of a mine) is sent
  only when the guest set it off; any other is logged `a mine MGSV set off itself ... kept out` (`!blast` counts them).
  The C4 detonator button wasn't found (RT, RB, B, X, Y, stick clicks, D-pad with C4 in hand: nothing; B next to a C4
  picks it up -> kGEvPlacedGone); the guest never touches the C4 chain.
- **Mines MGSV sets off itself stay out of Elden Ring:** MGSV's claymore answers MGSV's own characters -- D-Walker, whose
  follow spot is ahead-left of Snake, stands in its cone: live, 3 of 3 claymores placed next to a following walker went off
  0-1.7 s later in MGSV. A mine's blast (attack 202-211/451, and the clouds 284-287 of a mine) is sent only when the guest
  set it off for kHEvDetonate; others are logged `a mine MGSV set off itself (placed #N) ... kept out` and counted in
  `!blast`. The mine is used up in MGSV all the same (kGEvPlacedGone). C4 is the player's: MGSV's remote detonation sets
  byte-0 bit 3 on every C4 and `0x140FC7E10` turns them into bit 2 one by one every 0.05 s; the guest never touches that
  (the detonator button wasn't found: RT, RB and B unaimed with C4 in hand did nothing).
- Console: `!throw`, `!placed`, `!detonate <id>`; fake_er: `pr er placed`, `pr er detonate <id>|all`; its `--trace` gained the
  aim columns (flag, pos xyz, vel xyz, kind).

### 18.9 Cardboard box and its poster (`[Loadout] BoxPoster`, `Box`; protocol v8 flags; VERIFIED live 2026-10-12)

Player report: "Hiding in a box does not hide me from enemies at all. Also we need the anime girl box from MGSV for fun."

- **kGuestInBox** = `PlayerStatus.CBOX` (`PR.Flags`; `CBOX_EVADE` is the dive out of it), **kGuestBoxPoster** = and the current
  item's `vars.cboxPosterIndices[vars.currentItemIndex]` is a poster (non-zero). Live (`box_poster.prrec`): into the poster box at
  t=3.94 -> `InBox|BoxPoster|Crouching`, velocity 0.00 while still, ~3.2 m/s shuffling forward (left stick), back to 0 within
  ~0.4 s of letting go; out (RT) -> both flags clear the same frame; the FRST box without a poster -> `InBox` only. The host gets
  "keeps still" from `velocity` (0 exactly while still).
- **Getting in/out (MGSV's own controls):** hold D-pad left (the item menu), right stick to the box, let go: Snake is in it. RT takes it
  off (and unequips the item: RT is CQC again). X dives out. In the menu, **X = "Change Poster"** on the selected box.
- **Posters:** `vars.cboxPosterIndices[item slot]` = a `TppCollection` poster type: 47 SOL_AFGN, 48 SOL_MAFR, 49 SOL_ZRS (soldier
  posters), 50 GRAVURE_V, 51 GRAVURE_H (photo pin-ups), **52 MOE_V, 53 MOE_H (the anime girls)**; 0 none. Having one = the Mother Base
  resource `Poster1000 + (type - 47)` (`TppMotherBaseManagement.GetResourceUsableCount{resource = "Poster1005"}`; the resource names
  hash to the `CBOX_POSTER_1000..1006` table, strcode32 `0x45671c22`... at `0x140A923ED`). This save had 1000 x8, 1001 x6, 1003 x4,
  1004 x5: no anime poster. The menu's X cycles through the posters the save has, in type order, then none.
- **What applies a poster:** only the menu's X. Writing the var changes nothing on the box (re-equipping it kept the old poster) and
  a mission load clears every slot to 0 (even for owned posters). So the setup plays the menu (`Pump.cpp` kBox, after the buddy):
  1. `PR.Poster(name, box)`: the poster's resource is added if the save lacks it (`DirectAddResource{resource = "Poster1005", count = 1}`:
     queued, MGSV adds it a moment later; in memory only, saves are blocked), a box is given into a free item slot if the save has
     none (`[Loadout] Box`), then the plan: the first box slot and how many X presses reach the poster;
  2. `PR.BoxItem(slot)`: `Player.ChangeEquip{slotType = ITEM, subIndex = slot, toActive = true}` -- Snake gets into the box, so the
     item menu opens on it;
  3. a pad sequence (`pad::Sequence`, on top of the neutral boot pad): D-pad left 900 ms, (left+X 150 ms, left 350 ms) x n, left
     300 ms, nothing 700 ms; then `PR.Poster` again (the var is written when the menu closes) and, if not there yet, another plan
     (3 tries);
  4. RT (out of the box), `PR.BoxItem(-1)` puts the previous current item back.
  Live: `boot: box poster moe_v: adding` -> `press 1 5` -> `done 1` -> `box poster: on`, 8 s (once the first sequence took 4 of 5
  presses: `press 1 1`, then done). After that the box in slot 1 (the save's C.BOX (DSRT)) shows the anime girl on its front
  (gamescope screenshots around it: the poster on its front) whenever the player puts it on; the player can still change it (X in the item menu).
- **Capture:** the box is 5 meshes on Snake's root (one 9-bone skinned + 4 rigid), all `kOwnerPlayer`; Snake's body is still drawn
  inside it. Its shader declares `g_tex_diffuse/normal/srm` + `g_tex_layer` t3 + `g_tex_layerMask` t4 (+ `g_tex_Dirty` t5): the draw's
  `textures[3]` is the layer pattern, **`textures[4]` the poster** (`0xb4be80a6c26a60da`, 1024^2 BC1: decoded from the recording, the
  anime girl on white). A host that only samples `textures[0]` draws the plain box; the host's layer blend for D-Walker's camo
  (`er/Shaders.cpp`: albedo x t3 where t4.r) would tint the box with the cloud pattern instead. UNVERIFIED in Elden Ring: which of
  the two MGSV samples as the colour (the decoded t4 is the full-colour girl on white, t3 a blue/pink cloud pattern).
- `[Loadout] BoxPoster = moe_v` (default; `moe_h`, `gravure_v`, `gravure_h`, `sol_afgn`, `sol_mafr`, `sol_zrs`, empty = leave the box
  alone), `Box = EQP_IT_CBox`. The plain box ids: `EQP_IT_CBox` 456 .. `EQP_IT_CBox_LIMITED_G01` 474 (DSR desert, WR, SMK smoke,
  FRST forest, BOLE, CITY, CLB_A/B/C collaboration designs like "QPCS-A.P.D.", LIMITED).

### 18.10 Shots in first person, D-Walker's flamethrower, smoke (VERIFIED live 2026-10-12, recorder + fake_er traces)

Player report: "The gun ... doesn't register hits when looking down scope", "I can't see flames when shooting them from the walker
in first person view", the host saw a smoke cloud "jump between two places 30 m apart".

- **First person = RB** (MGSV's `PlayerPad.SUBJECT`; RS is ZOOM_CHANGE, LS is DASH): `scannedButtonsDirect` 0x400 while RB is held.
  Scoped/first-person rounds go through the same FireWeapon -> ShotSolver path: every one became a `kGEvFire` with the muzzle 0.3-0.4 m
  in front of the camera and the direction within 0.00-0.09 deg of the camera's centre line (the reticle): WU S.PISTOL, an assault
  rifle (fov 18.3 deg, a 6-round burst), a sniper rifle (fov 4.6 deg), the S1000 AIR-S. Third person the muzzle is 1.8-2.0 m from
  the camera and MGSV aims at its own world: 0.4-0.8 deg off the reticle at 30 m. So the guest's scoped shots are right; session 4's
  "no hits" were the stun shotgun's 5 HP (above) and whatever the host does with them.
- **Riding D-Walker, LT = MGSV's cockpit camera** (~0.6 m in front of the walker's root, 2.3 m up; MGSV sets no status for it):
  kGuestFirstPerson is now set then too (camera within 1.8 m of the root horizontally and over 1 m up). The flame events start at the
  flamethrower's nozzle (the walker's right arm: 1.8 m ahead, 0.45 m right, 1.9 m up, estimated from MGSV's first-person view) and,
  aiming, point at the camera's centre line 12 m away (the stream ends 0.07 m from the reticle line; before they started 1.5 m above
  the root, under and behind the cockpit camera); RT alone (third person) burns along the walker's facing. `riding_flame.prrec`.
- **Smoke:** a smoke grenade's cloud: 111 kGEvExplosion over 55 s (attack 285 then 284 every 0.5 s), every one at the same centre
  (836.10, -11.87, 1179.68) with the same throw id (`pad[1]` = 1). The host's jump (its log: the cloud's ER point went (-40.6, 9.7,
  5.5) -> (-8.6, 1.7, 5.5) -> (-40.5, 1.7, 5.5), always "throw 12") follows Elden Ring's world shifts, not the guest. The guest now
  also keeps a throw alive while its cloud puffs (Throw.cpp), and logs every attack id at most every 5 s again (the throttle used to
  swallow every repeat of a twice-a-second cloud).

## 19. Frame pacing (`FramePace.cpp`, `[Pacing]`, `!pace`; live 2026-10-12, both games)

Player report: "the camera is and has always been a bit jumpy". The host side (blending MGSV's frames into Elden Ring's) is
docs/HOST.md "Frame pacing"; what MGSV itself does:

- **One fixed game step per frame.** A constant right-stick pan turned MGSV's camera exactly 0.2884 deg per MGSV frame
  whether the frame took 16.5 or 21 ms; a 49 ms frame turned it two steps. So MGSV's game runs at its frame rate's share of
  real time.
- **Snake's root moves in whole animation keys** (120 Hz keys, ~1.14x playback while running): 2 or 3 keys per frame
  (7.8 / 11.7 cm), in a fixed pattern (`2,2,2,2,2,3,3` at 4.7 m/s) whatever the frame took. MGSV's camera moves rigidly
  with the root (camera - root unchanged frame to frame), so MGSV's own view steps the same way; the host smooths the
  puppet's motion over it (`[Camera] smooth_motion`).
- **51-58 frames/s next to Elden Ring** (17.3-19.4 ms; 62 with fake_er). Not vsync (`!pace sync 0`: 18.10 -> 17.97 ms),
  not the GPU (Elden Ring SIGSTOPped for 2.5 s: 19.7 -> 18.3 ms), not a busy thread (top -H: none above 34%). The wait
  probe (`[Pacing] WaitProbe`, `!pace start all`): Present returns in 0.01 ms; the game thread (a job-system worker) waits
  ~15 ms of each frame, almost all of it in **Sleep(1) polls** for work (`0x140032CA1 <- 0x14002C24F`, ~25 threads, ~800
  calls/s each, 1.06 ms per call under Wine); the main thread polls the same way waiting for its jobs (`0x14002E699 <-
  0x14006486D`); DXVK's threads wait in condition variables; one thread waits once per frame for a frame event
  (`FUN_140063600`). MGSV's own limiter targets 16.67 ms: with the polls shortened it holds it.
- **`[Pacing] JobSleepUs = 500`**: MGSV's own Sleep(1) (its kernel32 import, checked) becomes a 500 us delay. Live: idle
  57.7 -> 60.0 frames/s (16.66 ms median), panning (20 s, both ways) 50.5 -> 53.7 frames/s, MGSV's CPU +0.2 of a core,
  hitches unchanged (max 51 ms vs 45). 250 us panned faster (54.5-58.5) but brought 77-168 ms hitches (one per ~25 s);
  100 us was worse (57 frames/s, 294% CPU, 200 ms hitches). MGSV still can't hold 60 while the camera turns.
- **The Lua Warp turned MGSV's camera.** Every `TppPlayer.Warp` puts MGSV's camera behind Snake: running toward the camera
  (rootYaw 32 deg, camera -147 deg) each treadmill jump swung it 180 deg (`camRel (2.98,4.15) -> (-2.96,-4.17)`), every
  ~1.3 s at `Radius = 6`. The native placement (`Hold.cpp`) keeps the camera exactly (3 jumps running toward it: camera -
  root and yaw unchanged) and D-Walker's place (0.08-0.56 m), so **`[Motion] Hold = native_treadmill` is the default** now
  (riding still moves the walker with Lua `SetPosition`, see below).
- **The riding camera (`[Motion] RidingCameraKeep`, live 2026-10-12, both games):** the vehicle's Lua `SetPosition` leaves
  MGSV's riding camera where it was in the world, looking at the vehicle's new place: a 6 m warp turned it 166 deg; riding
  forward it jumped 75-100 deg once a second (8 jumps over 5 deg in 10 s, up to 101 deg in one frame). Right after the warp the
  guest asks for the pre-warp orientation, `Player.RequestToSetCameraRotation{rotX = -pitch, rotY = world yaw (deg),
  interpTime = 0.02}` (keys read from the exe, `0x1409DAF90`; interpTime 0 is ignored). After: 7 warps in 10 s of riding, the
  largest per-frame camera yaw step 0.23 deg.
- **Genlock (experiment, off):** `!pace limit 60 grid -4.5` waits in Present for a 60 Hz slot on Elden Ring's frame grid
  (HostState stamps, ~6 ms after its FrameBegin): one new MGSV frame on 99.5% of Elden Ring's frames, MGSV's stamp 4.5-9.2 ms
  before Elden Ring's FrameBegin (p5-p95). But MGSV's render then trails its game by 2-3 frames (Snake's poses that far
  behind his camera), MGSV can't hold 60 in every scene (a missed slot costs a whole period), and the host's blending already
  covers the beat; so it stays a console experiment.
- Console: `!pace start [all]` / `!pace stop` / `!pace` (frame and Present timings, with WaitProbe the waits by call chain;
  all of it in `PhantomRingGuest.pace.txt`), `!pace jobsleep <us|off>`, `!pace sync <0..4|game>`, `!pace limit <hz|off>
  [grid <phase ms>]`.
