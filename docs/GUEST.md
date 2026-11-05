# PhantomRing MGSV guest (`mgsv/` → `dinput8.dll`)

The guest is the half of PhantomRing that lives inside `mgsvtpp.exe` 1.0.15.4. It lets MGSV run Snake
exactly as MGSV does, and translates that for Elden Ring: Snake's intended motion, camera, skeleton,
life, shots, melee and his battle buddy (D-Walker first) go out through the shared mapping, and the
player's pad and Elden Ring's hits come in. Architecture: `docs/DESIGN.md`. Contract:
`protocol/phantomring_protocol.h` (v10).

Status: phases 0 and 1 for the guest, plus the shot/melee/buddy parts of phases 3 and 4 that the RE
backlog made possible, all run live with both games since (README "Status", docs/STATUS.md). §10 was the
checklist for the first live run (kept for reference). §18.11-18.14 (protocol v10: ammo refill on rest/respawn, night
vision, MGSV's auto-aim off, Tarnished mode) ran live with both games on 2026-10-13 (docs/STATUS.md). §18.15 (MGSV's
regeneration curve after Elden Ring's hits), §8.1 (D-Horse) and §8.2 (another buddy in session) are 2026-10-13 morning
work, live with MGSV and fake_er only (no protocol change).

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
  checks against the exe on disk; all 177 sites match the local 1.0.15.4 (2026-11-03).

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
| `Skeleton.*` | `SkinPublish` hook → PoseBlock (178 bones); the buddy's own skin for buddies without GetPosition (D-Horse, §8.1) |
| `Combat.*` | `ShotSolver` hook → kGEvFire/kGEvBuddyFire; CQC attack-serial poll → kGEvMelee |
| `Hold.*` | native set-position of the player character (re-backlog M1) for the native hold modes; `!loc`/`!placeobj` experiments |
| `Vars.*` | native reads of the `vars` ScriptVar block, plus the playerLife write |
| `Pad.*`, `DInputWrap.*`, `WindowGuard.*` | XInput IAT, DirectInput enumeration filter, minimise/foreground/cursor guards (every user32 detour) |
| `WindowHidden.*` | Windows: hidden MGSV window, focus spoof, forced view-visibility check `0x14024A1D0`, windowed swap chain, quit without host (`[Window]`, `!window`; docs/WINDOWS.md) |
| `SaveGuard.*` | Steam Cloud and file-level save blocking |
| `State.*` | what the Lua thread and the frame thread share |
| `Logic.*` | pure arithmetic (odometer, treadmill, life, ammo, transforms, blast kinds, lead point), unit-tested |
| `Blast.*` | attack-area system vf0 detour -> kGEvExplosion for thrown/placed explosives (§18.5) |
| `C4.*`, `C4Logic.*` | C4's detonator: MGSV's own (ACTION with C4 in hand) watched, a refused or unanswered press set off by the guest (§18.8a, `[Combat] C4Detonator`); the pure part unit-tested |
| `Ammo.*` | kHEvRested: magazines, ammo stocks and item counts back to full through MGSV's own ammo component (§18.11) |
| `Suppressor.*` | Snake's suppressor (§18.18, protocol v16): MGSV's reader replicated (kFireSuppressed at each shot, kGuestSuppressorOn), the [Loadout] guns' suppressors at MGSV's full life, the refill at a rest / a pickup |
| `Menu.*` | MGSV's pause menu (native) / iDroid (Lua) -> kGuestInMenu; GuestState republished while frames stall (§18.6) |
| `Sound.*` | Wwise SetPosition/PostEvent detours, D-Walker volume experiment and `!sound` diagnostics (§18.2) |
| `Horse.*`, `HorseLogic.*` | D-Horse treadmill (§8.1a, `[Buddy] HorseTreadmill`, off): the horse's own location object found near its skin and moved with each treadmill jump; the pure part unit-tested |
| `Follow.*` | D-Walker follow: whistles, follow spot, steering, out-of-view catch-up (§18.3); D-Horse's whistle (§8.1); the player's buddy orders |
| `Dog.*`, `DogLogic.*`, `BuddyLocation.*` | D-Dog in Elden Ring (§8.4, `[Dog]`, on; its attacks only while the host's layer is on): its location object moved with the treadmill jumps (Wait, the leash), its attack orders on the host's pick through one dedicated inert soldier (kGEvDogAttack), the observe-only detours of MGSV's dog and the soldier's dog-attack handler; the pure part unit-tested (`test_guest_dog`) |
| `ActorProxy.*`, `ActorProxyGame.cpp` | ActorProxy v0 (§20, `[Proxies]`, off): disabled soldiers placed at the nearest hostile ER characters, the rest parked; the pure part unit-tested |
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
  (under Wine from `/proc`, as the host does: the wineserver's pid from `Z:\proc\self\stat`, the first of MGSV's own threads
  above it in `Z:\proc\self\task\<n>`, then -- a long-lived wineserver far below -- down from `/proc/loadavg`'s newest pid while a
  fresh thread waits; looked up on the link's worker thread since 0.6.0, never in DllMain; written to
  `PHANTOMRING_GUEST_UNIX_PID`; never the raw getpid syscall stub, which Proton's Wine 11 turned into a crash), `guestHeartbeatQpc`
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
    - Nvg: the night-vision goggles are on (v10, §18.12);
    - SuppressorOn (v16): the weapon in hand has a suppressor on that isn't worn out (MGSV's reading every frame, §18.18);
    - Binoculars: PlayerStatus BINOCLE (RB held, not aiming; FirstPerson is set too) -- the host marks the Elden Ring characters
      Snake looks at (v13, docs/HOST.md "Marking"; live 2026-10-14: on while RB holds the binoculars up, off on release);
  - mission/location codes;
  - `odoEpoch`, `odo`, `rootPos`/`rootYaw` (owner+0x30 matrix; yaw = atan2(fwd.x, fwd.z)), smoothed `velocity`;
  - camera pos/quat (setter source);
  - `camFovY` (focal length at camera+0xC through `FilmHeightMm`, or `FovOverrideDeg`), `camNear`;
  - life/lifeMax/stamina, stance;
  - `weaponId`/`ammoInClip`/`ammoStock`: the weapon in hand (`vars.currentInventorySlot` 0-2 = `weapons[]`/`ammoInWeapons[]`,
    3 = the selected support item; VERIFIED live). `ammoStock` is MGSV's count for it: every round left with the magazine's
    included (the SVG-76: 30 + 180 = 210 full), a support item's count (§18.11);
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
    - `b` = pellets (low 16 bits) | `kFireSuppressed` (v16: the round went through a working suppressor, MGSV's own reading at
      the shot, §18.18);
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
    - `kHEvHurt`: life −= a × LifeScale, clamped at 1; MGSV's own post-hit regeneration curve follows (§18.15);
    - `kHEvRespawn`: refill to lifeMax and clear the died latch;
    - `kHEvHeal` (v3): life += a × LifeScale, up to lifeMax (never while he lies dead). VERIFIED live 2026-10-13 (fake_er
      `heal 500` 0.2 s after `hurt 1500`: life 2002 -> 4064, +2000 = 500 × 4, plus MGSV's own regeneration);
    - `kHEvRested` (v10): life to lifeMax (unless he lies dead: kHEvRespawn does it) and the ammo refill (§18.11);
    - `kHEvTarnished` (v10): Snake parks / gets the pad back (§18.14);
    - `kHEvWarped` and `kHEvStatus` are logged.
  - **Console** at `kOffGuestConsole`.

HostState is read for `kHostPlayerDead` (Death.cpp) and `kHostPuppeting` (Placed.cpp re-announces mines when the puppet
starts); its camera boom clamp and clock are unused. ActorTable is read only by ActorProxy v0 (`[Proxies]`, off by default, §20). The draw lists, the render ring (meshes,
textures) and the HUD slots carry the capture (§15, §16).

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
   - audio: `PhantomRing.Audio(music, ambience, radio)` (§18.2);
   - MGSV's pause menu / iDroid restrictions (§18.6), and its "Auto-Aim Correction" off (`PhantomRing.AutoAim`, §18.13).
   Each result is logged.
7. **buddy** "MGSV: deploying D-Walker (…)", steps of `PhantomRing.Buddy`:
   1. obtained / not dead / can sortie? Otherwise it reports `missing: …`, sets `kBuddyMissing`, and
      boot goes on without the buddy;
   2. `SetSortieBuddyType` + `SetVarsBuddyType` + `TppBuddy2BlockController.Load()`;
   3. wait while the block loads;
   4. `UnsetDisableCallBuddyType` + `CallBuddy(type, Vector3 [Buddy] Ahead m in front of Snake and Side m to his
      right, yaw, true)` (never behind him: the camera is there; §18.3);
   5. `active` once a `TppCommonWalkerGear2`/`TppWalkerGear2` object answers GetPosition; D-Horse (no GetPosition) once its
      skin is known (§8.1): looked for where it was called, and if it isn't there after ~8 s MGSV's own horse whistle brings
      it (it gallops over; live 2026-10-13: `whistling it over: ok` -> `buddy skin learned ... (69 bones)` 5 s later).
   It gives up after 60 s. The block is requested again when it hasn't come within ~10 s: a mission reload (MGSV's continue
   after a game over) brings the save's buddy back (live: `waiting for block (active 4)` for D-Horse until this).
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
- **Jump** (`[Jump]`, protocol v14 `kGuestJumping`, `logic::JumpButton` / `logic::JumpTracker`; live pass B 2026-10-15: R3 in
  MGSV's item wheel (D-pad left held) was a jump, so its Use -- the Phantom Cigar's only way -- never reached MGSV: `PassWith`
  gained the D-pad (`lb rb lt up down left right`, 0x030F), and with the wheel open and the stick on the cigar R3 lit it; live pass A 2026-10-14:
  `jump: Snake lifted 1.00 m at ... (running 2.9 m/s) -- kGuestJumping until he lands`, refused crouched, LB/LT + R3 stay
  MGSV's, docs/FEATURES-1.md; the lift and land lines are logged for the first 10 jumps only). MGSV has
  no jump, and L3 -- the button one would pick -- is MGSV's DASH (sprint; `PlayerPad` DASH, §18.10). The default `Button =
  rs` (R3): MGSV uses the right-stick click only with LB held (its call menu's select, §18.3), aiming (the shoulder change)
  or in the binoculars (zoom), and in its weapon / item wheels (a D-pad direction held: Use), so `PassWith = lb rb lt up down left
  right` leaves it to MGSV when one of those is held at its press; otherwise
  `Pad.cpp` keeps the whole hold from MGSV and counts the press. LT counts as aiming past XInput's own threshold (30 of 255,
  `logic::AimingTrigger`; it was 64, and between them R3 was neither a jump nor the shoulder change), and the game thread tells
  the pad thread every frame when R3 is MGSV's anyway (`pad::SetMgsvOwnsR3`, fresh for 250 ms): Snake aims by MGSV's own HOLD,
  rides, is in a menu or the binoculars -- the press goes to MGSV then (no `jump: not now` line for it). The game thread (`Frame.cpp`, before the hold) takes it: Snake
  must be ready, on foot, standing (walking and running are standing: `Standing throughout` while running, §17), not aiming,
  crouched, prone, riding, in a box, a menu, CQC, a fall or another action (NORMAL_ACTION), not parked or Fulton-anchored, no
  warp landing, and the native placement must be there (`jump: not now -- <why>` otherwise). Then he is placed `Lift` (1.0 m)
  above his root (+ `WarpLift`, as for every placement) with his rotation, through the treadmill's native set-position call:
  MGSV keeps his running and plays its own fall and landing (a 1 m drop: never MGSV's fall damage, which `Invincible` turns off
  anyway). The odometer is rebased at the lifted root (the lift is no motion; the fall's height the host ignores).
  `kGuestJumping` is up from the press until he has landed in MGSV: his root came down 0.2 m from the highest it rose and stood
  still 3 frames, or MGSV's FALL status came and went; `MaxAirMs` (2 s) at the latest; a lift that never shows in his root
  (not 0.3 m up within 0.3 s) ends it at once. No treadmill warp or native hold while it is up; no new lift for 0.2 s after a
  landing. `!jump` presses it from the console. Logged: `jump: Snake lifted 1.00 m ... -- kGuestJumping until he lands`.
  With the host's keyboard and mouse (`[Keyboard]`) the middle mouse button is R3: the jump (with Q = LB held, the call menu's
  select). A dive (X, §18.17) in the air of a jump never hops (the host refuses it).

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
  (`MeleeShapeGeometry`) and its centre lies within 2.5 m of Snake's root (horizontally; -1..+3 m up). Otherwise it is 1 m in
  front of Snake at chest height, radius 1.2 m. Live pass B 2026-10-15: on the frame its serial moves the shape can still sit
  where the last strike was -- a punch after an 8 m walk came out 8 m behind Snake (the host's arc still found the soldier next
  to him, but a crate there would break 8 m back): `melee: MGSV's attack shape is N m from Snake (stale: the last strike's)`.
- **Life:** Elden Ring damage × `LifeScale` (4) is written into the native life (`0x142C0147C`, regenerating) and
  `vars.playerLife`. The native one is written only while it agrees with vars within 400 (a frame of MGSV's ~760
  life/s regeneration; an exact compare dropped every hit taken while Snake regenerated: "write did not stick",
  VERIFIED live with both games). The guest checks that the value stuck and warns once if the game overwrote it: 8 frames
  after the worker applied it (`ApplyPendingLife`, ~33 ms ticks), life nearer the value it replaced than ours
  (`logic::LifeWriteLost`, tested; writes under 400 not judged). Its first form (any rise of 300 in 8 frames) fired on
  Margit's hits that had stuck: MGSV regenerated right after our write (it never saw a hit, so its regeneration delay never
  started; §18.15 now holds it), and a 2-frame version caught writes still pending (live 2026-10-13; 0 warnings in 11 hits after). Snake is never
  taken below 1: the hit that would kill him sends kGEvDied once, and Elden Ring runs YOU DIED and then
  kHEvRespawn. MGSV's own damage is off in the sandbox (`Invincible`).
- **Regeneration after an Elden Ring hit** follows MGSV's own post-hit curve (§18.15, `[Combat] RegenGate`): life is held
  ~2.4 s, then regenerates at a rate that grows to ~750/s over ~6.25 s, as after an MGSV hit. Before, MGSV regenerated
  from every Elden Ring hit at once at the full rate.
- **Hurt feedback:** each hit that costs life queues `hurt` for the Lua pump: MGSV's camera noise (its
  camera is Elden Ring's) and blood on Snake's model (`HurtFeedback`, `HurtBlood`; docs/COMBAT.md).

## 8. Battle buddies (D-Walker first)

MGSV runs the buddy entirely: following, orders (the LB call menu works through the pad),
mounting (Y prompt), dash/hover, and its guns. The guest:
- deploys the configured buddy at boot (§5.7), or reports `kBuddyMissing` with the reason;
- publishes its object id, position, derived heading and main-gun ammo every frame, plus `kBuddyRidden`
  when Snake rides it. Snake's root, odometer and `kGuestOnVehicle` then follow the walker, so Elden
  Ring's puppet moves at D-Walker speeds;
- moves it along with every treadmill jump (or moves it *as* the jump while it is ridden);
- reports its shots as kGEvBuddyFire.
Horse, dog and Quiet go through the same code (`[Buddy] Type`). Their GameObject classes are
`TppHorse2`, `TppBuddyDog2` and `TppBuddyQuiet2`. The buddy is drawn in Elden Ring from the capture (`kOwnerBuddy` draws,
live with D-Walker and D-Horse).

### 8.1 D-Horse (`[Buddy] Type = horse`; live 2026-10-13, MGSV + fake_er/recorder)

- **Position:** `TppHorse2` answers neither `GetPosition` nor `SetPosition` (SendCommand returns true and nothing; `Warp`,
  `WarpRequest`, `SetWarpPosition`, `position = Vector3` forms and `CallBuddy` again: no effect, live). Its position is its
  own skin's (`Skeleton.cpp`): SkinPublish gives every skinned model's (driver, binding), the binding starts with the
  model's root matrix (the horse's model `+0xD0` translation followed it in a /proc memory scan). The pair is learned near
  a hint -- where boot called it, beside Snake after a horse whistle (6 m, 20 s), under Snake while he rides it (1.5 m) --
  as the nearest skin with 53+ bones that isn't Snake's (178) or one of his other skins standing exactly on his root (64
  bones: once learned as the horse before this rule), stable for 10 publications; then every publication of that pair is
  the buddy's position and heading (`MatrixYaw` of the root). Not published for 3 s = learned again at the next hint.
  The horse's skeleton has 69 bones; a re-learn wants the same count. `!buddy` shows it (`buddy skin: driver ... (69
  bones) at ...`). The Tick's Lua position (none for the horse) is replaced by it (Pump.cpp), so GuestState `buddyPos`
  / `kBuddyPosValid`, capture ownership (`kOwnerBuddy` within 4 m) and revival work as for D-Walker.
- **Calling it:** MGSV's call menu (hold LB) offers Knock / **Whistle (call D-Horse)** (right stick right, then click it
  with the stick released) / Do it (defecate) / Sonar. The horse doesn't follow by itself; its whistle (buddy command
  17, message `0x7C8CC24E84C3`, case byte-checked: `kBuddyCommandHorseWhistle`) makes it gallop to Snake and stop
  beside him -- MGSV places it nearer first when it is far (live: 170 m -> beside Snake ~5 s later). `Follow.cpp`
  issues it ([Buddy] `HorseFollowDistance = 15` m, `HorseWhistleEveryMs = 6000`) when it is that far from Snake on foot,
  and at once after a treadmill jump (it can't be moved with him). The player's "Stay back" (18) holds that until their
  next horse whistle (as D-Walker's modes do).
- **Riding:** Y next to it mounts (PlayerStatus ON_HORSE, `vars.playerVehicleGameObjectId` = the horse 0x1a00): GuestState
  `kGuestOnVehicle` (0x20), buddyFlags Present|PosValid|**Ridden** (0x7), `vehicleId` 0x1a00; Snake's root sits ~1.4 m
  above the horse's. Left stick walks it (~3-4 m/s), **X taps** speed it up: canter ~6 m/s, gallop 13-17 m/s (live
  odometer: 180 m in ~15 s); Y dismounts. No treadmill while riding (unless `[Buddy] HorseTreadmill`, §8.1a): nothing moves the horse (Lua SetPosition, the
  player's native placement while riding, the player's Warp while riding: all tried) -- the guest logs `D-Horse can't
  be moved -- no treadmill while Snake rides it` and leaves it alone. MGSV's own world then limits the ride: the
  airport terminal is ~40 m north of the sandbox centre, something stopped a gallop ~50 m east; south (~90 m) and then
  west (~160 m) are open. Dismounting far away, the treadmill puts Snake back at the centre and the horse is whistled
  over (in Elden Ring it jumps by that distance, then gallops back).
- **Recording:** `horse_ride.prrec` (scratch recordings, README line): mount, walk, gallop, turn, stop, dismount,
  the whistle back; 2616 of 2895 draw lists carry 14-20 buddy-owned draws.

### 8.1a D-Horse treadmill (`[Buddy] HorseTreadmill`, default true since live pass B 2026-10-15)

Live pass B (2026-10-15, both games, Limgrave): `!horse shift 3 0` with it off -> `shift refused -- [Buddy] HorseTreadmill is
off`; `!horse on` -> `location object ... stands on D-Horse's skin (30 scanned) -- confirming`, `confirmed (31 checks on the skin)`
0.5 s later; then 307 ridden moves (`treadmill (ridden): feet ... moved by 7.39,0.00,-1.82`), every one followed by the skin, 0
failed, no `didn't follow` warning; the odometer ran smooth (12-15 m root jumps taken out) to 1.24 km in one general direction
in MGSV terms, far past the airport's limits; in Elden Ring a straight 299.9 m east from Gatefront in 27 s (the host's position
integrated across 9 world shifts: Elden Ring moves its origin every ~30 m at a gallop, which reads as the horse jumping back if
one doesn't take them out). After the ride Snake got off and D-Horse stood 3.7 m beside him for 12 s: no jump, no whistle.
Galloping into Limgrave's ravines killed Snake three times (Elden Ring's falls, as on foot). On by default since.

Goal: riding anywhere (no airport limit) and no horse jump in Elden Ring. The horse is moved the way ActorProxy v1 moves
soldiers (20.5) -- by its own location object -- because it answers no Lua placement at all (above).

- **Static RE (2026-10-14, mgsvtpp.exe 1.0.15.4, byte-checked in Addresses.h, `verify_addresses.py` ok):** the location class
  (vtable `0x1424C8EF0`, Snake's and realized soldiers') is built by `0x141ACED60` at +0x10 of a 0x3F0-byte body (vtable
  `0x1424C8EE0`; `kLocationCtorVtables`), allocated by `0x141AD2940`, called from `0x140B6F0B0` -- a generic character builder
  (class hash `0x92839b779862`) reached from `0x140B6D980`, not from player or soldier code, so any realized character class can
  own one. Its vf 0x68 (`0x141AD56D0`, `kLocationSet` / `kLocationSetSlot`) is vf 0x58 (position) + vf 0x60 (rotation), then
  clears bit 2 of +0x1A0 and the qword +0x1F0 -- what Hold and ActorProxy already call. Whether TppHorse2 realizes one is
  not proven statically (Fox's own reflection, no RTTI for it): the guest finds out at runtime.
- **Finding it (Horse.cpp, game thread):** once D-Horse is out and its skin is known (8.1), a background scan lists every object
  with that vtable in MGSV's writable memory (as ActorProxy's, ~220 ms); the one whose feet (+0x10) stand within 0.75 m
  (horizontal) and 1 m (vertical) of the skin root -- never Snake's own object (1.4 m above it in the saddle, and excluded by
  address) nor one near an ActorProxy soldier, never one parked at the origin -- is the candidate. It is **confirmed** after 10
  checks on the skin over at least 0.5 s; a candidate that leaves the skin once is dropped; a confirmed one is lost after 15
  checks in a row off the skin, or at once when its vtable changes. No candidate: scanned again 30 s later (5 s otherwise).
- **Moving it:** the setter gets the object's own +0x20 point moved by the shift and its own +0x60 rotation (so the class's lift
  and the horse's heading come from the object), then +0x20 is read back (0.3 m, horizontal). The skin must then catch up with
  the object (10 checks of grace); a horse whose body doesn't follow, or a setter that doesn't take, counts a failure, and 3 in a
  row put D-Horse back to the old way for the session (`horse: ... -- D-Horse is left to MGSV again`).
  - **On foot:** every treadmill jump moves it by the same shift as Snake (all three axes: it keeps its height relative to him)
    in the same frame as his native placement -- no whistle after a jump; Elden Ring sees it keep its place beside him. The
    Fulton anchor's placement moves it horizontally, as the walker.
  - **Riding:** the horse is moved *as* the jump (the vehicle branch, no Lua), its feet put on the centre's ground (the
    centre's height + the object's own lift: the far point can be metres above or below the centre, and Snake's root is a
    saddle above the feet). Snake in the saddle comes along and his root's jump is taken out as the warp's landing, as for
    D-Walker's `owarp` (its height, the saddle's at the centre, held for 20 frames). **If his root doesn't jump within 10
    frames and 300 ms** the saddle doesn't carry him: the horse is put back under him at once and no ridden jump is tried
    again this session (`horse: Snake's root didn't follow the ridden jump ... D-Horse put back under him`; `!horse` says
    `the saddle didn't carry Snake`) -- MGSV's map limits the ride again, as without the feature. A ridden jump starts only
    within 2 x radius + 20 m (32 m) of the centre: a ride that went further without the treadmill (object not confirmed
    yet, or lost and found again) is not thrown back in one go; it waits until Snake rides back within that, or dismounts.
    `HorseMoveRider = true` also places Snake by the horse's jump in the same frame (+ `WarpLift`, as every native placement;
    no landing watch then). `RidingCameraKeep` asks MGSV's riding camera back as for the walker. Gallop and the 1 s
    `WarpCooldownMs`: Snake gets ~15 m from the centre between jumps (the airport terminal is ~40 m north).
- Console: `!horse` (state: object, confirmed, scans, moves, followed, failures), `!horse on|off` (this session; off forgets the
  object, `on` finds it anew), `!horse scan`, `!horse shift <dx> <dz>` (moves the horse by that much at the next frame, Snake on
  foot, to see whether it goes; refused while off or without a confirmed object, never left queued).
- Offline: `tests/test_guest_horse.cpp` (pick, tracker, setter arguments and ridden heights, jump limit and landing watch,
  decisions). `TestRiddenGallop` is an odometer/treadmill **simulation under the assumption that the saddle carries Snake** (with
  Frame.cpp's and Horse.cpp's glue copied in: tracker, decision, landing, settle height): a minute at 15 m/s up a 15 % slope,
  60 jumps, odometer 900.2 of 900 m, the tracker confirmed throughout, the horse's feet on the ground after every jump. Its
  second run, the saddle *not* carrying him: one jump, put back 18 frames later, no other. It proves nothing about MGSV.

### 8.2 Another buddy in session (`!buddy`, the iDroid; live 2026-10-13)

- `!buddy walker|horse|dog|quiet|none` (console) switches buddies without leaving the sandbox: the boot's buddy step runs
  again from Ready (Snake keeps the pad, no box sequence, the centre kept), next to Snake. Live: walker -> horse 1.0 s
  (`in-session buddy switch done`, the walker gone, the horse beside Snake, screenshots), horse -> walker 1.0 s (the walker
  following again after a 5 s run).
- **MGSV's own way works too:** iDroid (hold Start) > MISSIONS (RB) > BUDDY SUPPORT > D-HORSE ("Request deployment of
  D-Horse", a drop point on the map, GMP -35,500 in memory only: saves are blocked) -- "Support Unit: Dispatch request
  received [D-Horse]", the walker leaves and D-Horse arrives at Snake. `BlockMissions` only strikes out the DISMISS
  entries, so this stays available. The guest follows the player's choice: once MGSV's active buddy type has been another
  kind for 2 s, `[Buddy] Type` becomes it for the session (`boot: the player deployed D-Horse from the iDroid (was
  D-Walker) -- following that choice`), the skin is learned anew (live: 69 bones, beside Snake). The next boot deploys
  the ini's buddy again.

### 8.3 Dismissing and changing the buddy from MGSV's own menu (`[Menu] BuddyDismiss`; live 2026-10-14, MGSV + fake_er)

Player report (session 6): "buddy CHANGING and DISMISSING supported (MGSV's own buddy menu)".
- iDroid > MISSIONS > BUDDY SUPPORT lists what the save has developed and isn't out: here D-HORSE, D-DOG, D-WALKER, and since 0.9.0
  QUIET (SCOUT) / QUIET (ATTACK) (the player's save has no Quiet of its own: she is borrowed in memory, 8.5; ATTACK deploys her by her
  HALO drop onto the sniping point picked on MGSV's map, live 0.9.0 integration), plus DISMISS <the buddy that is out> and CHANGE EQUIPMENT (struck out with no buddy). `BlockMissions` used to
  strike out every DISMISS; `[Menu] BuddyDismiss = true` (default) leaves them (`boot: menus: ... missions=10 blocked, 5 buddy
  dismiss allowed, 0 missing`). The menu remembers its cursor between visits.
- **Dismiss** (live): DISMISS D-WALKER -> the walker is fultoned out (screenshot: it flies off), MGSV's active buddy type
  (`TppBuddy2BlockController.GetActiveBuddyType`, `vars.buddyType`) becomes NONE (0). Before: the guest's revival called it
  back 10 s later (`boot: D-Walker lost 10 s ago -- block requested`). Now `AdoptBuddy` (Pump.cpp) takes a NONE that lasted 2 s
  within 15 s of an MGSV menu as the player's dismissal: `boot: the player dismissed D-Walker from the iDroid -- no buddy until
  another is deployed`, `[Buddy] Type` = none for the session (no revival, no follow, no hint), GuestState `buddyType` NONE (0),
  `buddyFlags` 0, status `MGSV: ready (Africa)`. A NONE with no menu open (or `BuddyDismiss = false`) is still a lost buddy:
  `MGSV has no buddy out any more (... no iDroid DISMISS seen) -- calling it back`. No buddy is stable: 18 s of running and
  turning with the treadmill (`no_buddy.prrec`, 1049 states, only Snake's draws).
- **Told by MGSV's own `leave` order** (since the player's 0.9.0 session, `mgsv/BuddyLogic.h` `DismissWatch`, `TestBuddyDismiss`;
  Pump.cpp's wiring of it: `test_guest_pump`, 12).
  The 15 s window ran out on Quiet: DISMISS QUIET at 15:10:47, her type NONE 12.8 s later from her far sniping point, and the guest
  called her back. MGSV's buddy system sends the buddy that is out the message `leave` (0x6626CE35F43D) the moment the iDroid sends
  it away (DISMISS, CHANGE EQUIPMENT, another buddy); the guest's detours on Quiet's, D-Dog's and (with auto-follow) D-Walker's
  message handlers note it with whether an MGSV menu was open. A NONE within 120 s of an order given with the menu open (or seen open
  up to 2 s before it) is the player's dismissal, however long the buddy takes to go; the same buddy back after >= 1 s gone (a
  re-equip) or another buddy out forgets it, and `ReviveBuddy` never calls back a buddy leaving by that order. Without an order
  (D-Horse's handler isn't hooked) the old window stays as the fallback, from the last time the menu was seen open: 20 s for
  D-Walker / D-Horse, 30 s for Quiet / D-Dog. Logs: `boot: MGSV ordered <buddy> to leave N ms ago with its menu open ...`, `boot: the
  player dismissed <buddy> from the iDroid (MGSV's leave order, NONE N s after it)` or `(NONE N s after its menu, no leave order
  seen)`, `boot: MGSV's leave order to <buddy> forgotten (<why>)`.
  **Live 2026-11-04** (both games, Gatefront, the virtual pad driving Buddy Support): DISMISS QUIET three times from her posts 35 m
  out (once with three soldiers on Snake) -> `quiet: her handler got message 0x6626ce35f43d (leave)` with the iDroid open, `MGSV
  ordered Quiet to leave 30-217 ms ago with its menu open`, `the player dismissed Quiet from the iDroid (MGSV's leave order, NONE
  12.3 / 12.3 / 12.1 s after it)`, no call-back in the 25-40 s watched after; DISMISS D-WALKER -> the order seen live for D-Walker
  too, NONE 8.6 s after it; D-Dog deployed and DISMISS D-DOG -> NONE 9.3 s after it; Quiet (Attack) deployed over D-Walker ->
  `MGSV's leave order to D-Walker forgotten (Quiet out instead)`, then `the player deployed Quiet from the iDroid (was D-Walker)`.
  Not reproduced live: a far post like the player's (MGSV's own sniping point 70 m up, reached through an order; the host's
  post rule gave 25-35 m at Gatefront), so the exits ran 12.1-12.3 s; the leave rule doesn't depend on that time.
- **Deploy another** (live): with no buddy, BUDDY SUPPORT > D-DOG > drop point > OK -> "Support Unit: Dispatch request received
  [D-Dog]", D-Dog lands beside Snake in its pod ~10 s later: `boot: the player deployed D-Dog from the iDroid (was no buddy)`.
  Then D-DOG -> D-WALKER directly (the dog leaves, the walker is dropped from ~85 m up and lands beside Snake ~20 s later, the
  mount prompt shows, auto-follow steers it again: `follow: ... 2 steered`). Each switch re-runs the buddy's own setup: D-Horse is
  whistled over (8.1), D-Dog/Quiet get a skin hint near Snake (every 5 s while their place is unknown), D-Walker is found by
  GetPosition and auto-follow takes it (it keys on `[Buddy] Type`).
- **D-Dog** (`TppBuddyDog2`, id 0x2600): no GetPosition (nil) and SetPosition does nothing (live: it stayed put) -- like D-Horse,
  its place is its skin: **94 bones**, learned 0.2 m from the hint (`skeleton: buddy skin learned: ... (94 bones)`), GuestState
  `buddyFlags` Present|PosValid, the capture owns its draws (`dog_follow.prrec`: 15465 buddy draws in 859 frames, ~18 a frame).
  It follows Snake by itself (MGSV's dog AI): 20 s run with turns, median 1.9 m, max 5.8 m from him. The treadmill no longer
  sends it (or Quiet) `owarp`: SetPosition is ignored and its `position = Vector3` form put D-Walker at the world origin (18.3);
  MGSV brings the dog back to Snake itself after a jump (a 6 m jump in Elden Ring each time, measured 2026-11-03). Since then
  `[Dog]` (8.4) moves it with the jumps through its own location object and carries out its attack orders on Elden Ring's enemies.
- **D-Horse** the same way (live): walker -> D-HORSE from the iDroid: `the player deployed D-Horse ... following that choice`,
  whistled over (`D-Horse whistled #1 (deployed from the iDroid)`), its 69-bone skin learned beside Snake 20 s later, buddyFlags
  0x3, the mount prompt on screen; DISMISS D-HORSE -> `the player dismissed D-Horse from the iDroid`, buddy NONE (0), flags 0.
- **Quiet** (`TppBuddyQuiet2`, id 0x2400): she answers GetPosition (live 2026-11-04), her 136-bone skin gives her heading (hinted at
  her GetPosition since 0.9: Pump.cpp); on the player's save she is gone after "A Quiet Exit" and is borrowed in memory ([Quiet]
  Borrow, 8.5), after which BUDDY SUPPORT lists QUIET (SCOUT) / QUIET (ATTACK) and deploys her by her HALO drop.
- `!buddy hint` (dev): look for the buddy's skin within 12 m of Snake for 20 s; `!buddy` also names the bone count of the last
  skin in a hint's window that wasn't taken (`kMinBuddyBones` 53).
- **With the real Elden Ring** (combined run 2026-10-14, the injected pad driving MGSV's iDroid; screenshots of each step):
  DISMISS D-WALKER in the Roundtable Hold -> host `buddy: D-Walker dismissed -- no buddy now`, GuestState NONE; D-HORSE from
  there -> `D-Horse deployed (object id 0x1a00)`, placed on Elden Ring's ground 25 s later; D-DOG at Gatefront -> D-Horse leaves
  under its Fulton balloon (drawn in Elden Ring's sky), `D-Dog deployed (object id 0x2600)`, the dog beside Snake 10 s later
  ("D-Dog: Found enemy" / "Found weapon" in the HUD layer, its pet prompt); D-WALKER -> dropped from 2.2 km up in MGSV,
  `D-Walker out again: drawn and placed`, Y mounts it. Seen: D-Dog's coat is drawn in false colours at times (orange/green,
  normal grey at others) and D-Walker right next to the camera in magenta/orange (MGSV's near-camera fade): a material the
  capture doesn't map yet.

### 8.4 D-Dog in Elden Ring (`[Dog]`, `Dog.cpp`, `DogLogic.*`, `BuddyLocation.*`; protocol v17; live 2026-11-03, MGSV + fake_er on `/dev/shm/phantomring_g`)

The design is docs/research/ddog.md (§3.5-3.7); what the live pass settled in MGSV is its §2.10. On by default since the live pass
below (`[Dog] Enabled`, `Treadmill`, `Attack`, `KeepDrawsNearProxy`; `Announce` is parsed only); `!dog off` (or the switches one by
one) gives back 0.7.0's D-Dog for a session. **The attacks are taken over only while the host's D-Dog layer is on** (HostState
`kHostDogLayer`, v17, set while the host's `[Dog] enabled`): with it off an attack order is MGSV's own again, as in 0.7.0 (`dog: the
player's attack command N left to MGSV (the host's D-Dog layer is off)`) -- before the 0.8.0 review the guest took it over anyway, found
no pick and whistled the dog back with nothing on screen.

**0.8.0 (review and the integration run with both games, 2026-11-04):**
- **The style byte's 2 and 4 were swapped.** `+0xD4` is `vars.buddyDogEquipType`; with MGSV's own CHANGE EQUIPMENT (iDroid > MISSIONS >
  BUDDY SUPPORT, a supply drop, memory only): NAKED 0 -> the call menu's command 11 "Keep 'em busy", TACTICAL (FULTON) 1 -> 15 "Get it",
  SNEAKING (STUN) 2 -> 13 "Do it (knock out enemy)", BATTLE DRESS 3 -> 14 "Do it (wound enemy)", SNEAKING (KNIFE) 4 -> 12 "Do it (kill
  enemy)". The knife message's `+0xA` byte is the stun knife's flag (set for 2), not "lethal": the stun knife had gone to the host as a
  kill with the knife's 25-lives share. `StyleOf` / `AttackIdOfMessage` fixed, and an order's style is now the one its **command** names
  (`StyleOfCommand`; the byte's is published every frame in buddyFlags bits 8-15 so the host picks for it before any order, and is what
  `!dog go` with no number uses). Live after the fix: knife -> `knife, lethal`, kill 4420 -> dead; stun knife -> `knife, not lethal
  (stun)`, the soldier ASLEEP.
- **The dog system is forgotten while no dog is out** (a re-equip's supply drop, another buddy, a dismissal): MGSV frees it, and its
  style byte had read ~400 garbage values in the 22 s of one re-deploy (the host's style flipping with them).
- The player's **Wait** or **DD** during an order ends it without our whistle (it used to undo the Wait at once); an order with no pick
  while the dog waits leaves it waiting; no pick (or an awake one for "Get it") at the order is looked for again for 350 ms (the host
  re-picks a frame after the order's kGEvBuddyOrder); the dedicated soldier stays put when Snake walks up to the held enemy (it was
  parked inside Snake's 3.5 m, which ended MGSV's bite and the host's hold); `!dog attack on` re-arms the alert guard; the job's texts
  under one lock (a data race between the Lua, game and console threads); a fetch's Connected carries no stale attack id.
- **D-Dog's commands (8-16) no longer pause D-Walker's following** (Follow.cpp's command detour): a walker deployed after the dog had
  stood 36 m away until whistled (live); after the fix `!dog player 14` with D-Walker out -> `follow: the player's buddy command 14 (arg
  0) -- D-Dog's`, and its leads went on.
- Live with both games (docs/STATUS.md 0.8.0): deploys from the iDroid, Wait (the call menu's command 9: the dog's MGSV place unchanged
  through two treadmill jumps while Snake ran 15 m away and back), the whistle (command 8, between Wait and the top of the menu), Bark as
  the player's command, every suit's attack (realized in 0.83-1.03 s, connected 1.7-4.6 s after the order), Get it 12.3 s; 0 alerts and
  0 warnings in four MGSV sessions; `recordings/ddog_kill_080.prrec` (the knife's kill on the recorder's actor).

**0.8.0 polish (MGSV + the host stand-in, 2026-11-04; docs/research/ddog.md §9 "0.8.0 polish" has the table of causes and numbers):**
what Elden Ring sees of the dog no longer steps over 0.5 m in a frame -- 10 minutes, 20 orders, ladders, climb-ups, jumps and a Wait
against a wall: the published place at most 0.32 m a frame (was 93.9 m), the drawn dog 0.42 m (was 1.43 m). No new game address.
- **The bitten soldier is held** (`proxy::HoldDedicated`, set by Dog.cpp from the accepted attack state to its exit): ActorProxy no
  longer puts him back onto the Elden Ring enemy's place on its refresh -- MGSV's takedown pushes him 0.3-1 m and holds the dog on
  him, so every refresh had snapped the dog 0.3-0.7 m -- and moves him only with Snake's treadmill jumps, by the offset's change, in
  the frame the dog's own shift lands (the table-lag double move behind 0.8.0's one-frame 6-8 m blips is gone with it). `!proxy`:
  `while the dog held him: N moves with the treadmill, N frames left where the bite put him`.
- **What Elden Ring is shown never moves faster than the dog runs** (`[Dog] MaxShownSpeed = 14` m/s, `DogLogic` `DisplayLimiter`):
  MGSV's lunge starts with a 0.56 m step and its running dog moves every other frame; the published buddyPos and the dog's draws (moved
  by the same offset per frame, `capture::NoteBuddyOffset`) follow it at most 0.25 m a frame apart, in Elden Ring's terms (treadmill
  jumps are no motion there). A step over 15 m (MGSV's own warp) and our own leash / staging moves are taken at once.
- **Every placement of ours is watched** for 3 frames (45 after a staging move): MGSV undid one treadmill shift in 76 right after a
  bite, and warps its dog when Snake is placed far; moved more than 3 m from where we put it, it is put there again (`dog: MGSV moved it
  N m from where it was put (...) -- put there again`, at most 3 times).
- **The dog goes along with Snake to MGSV's ladders and ledges** (`[Dog] StageAlong`): once his root shows at the site a free dog is put
  2 m behind him there (MGSV used to run it after him and then warp it to him: the 0.8.0 run's ~77 m), and put back by the same move
  when he is home (`dog: staged: taken along to Snake at the site (84.0 m)` / `back home with Snake`); MGSV's warp home a frame later
  (17 m from him) is caught by the watch above. Published meanwhile relative to his home without the move, its draws with it.
- **Wait in Elden Ring's terms** (`[Dog] WaitKeepsPlace`): the slip -- what Elden Ring's walls kept Snake from of his MGSV motion -- is
  seen in the ActorTable (characters standing still in Elden Ring move by it in odometer space; `proxy::plan::SlipObserver`: the median
  of their moves since the Wait began when at least two agree, never more than Snake's path) and added to what Elden Ring is shown of the
  waiting dog, not to MGSV's dog (moving a waiting dog's location object again and again ended MGSV's Wait within 3 s live). Live
  against a fake wall: 17.3-17.7 m of slip per run, the shown dog moved at most 0.8-1.6 m. With fewer than two characters near Snake it
  keeps its MGSV place as before and the host's own `WaitPin` (it knows Elden Ring's slip) holds it -- the two never add up (ddog.md §9). `!proxy`: `slip (...) N m over N m of Snake's path`.
- **MGSV's own markers during an order** (the green one over the dog, the red one with its distance over its soldier) stay out of the
  HUD layer while the order runs and 1.5 s after: their UI draws are told by their colour (`cVSLight[1].rgb`, `DogLogic`
  `IsMgsvMarkerColor`); `!capture`: `D-Dog's order: MGSV's markers LEFT OUT now (N draws left out)`. `!capture probe` now names each UI
  draw's job and its vertex constants.
- **No pick at the click**: the guest keeps the host's last `kActorDogTarget` (read every 100 ms while its layer is on) and an order with
  none takes the one seen within 1 s before it, if he is still hostile and alive in the table (`dog: no pick at the order -- the host's
  pick of 874 ms before it ... is taken`, live).
- `!dog placetrace on|off`: one CSV line per frame (`PhantomRingDogTrace.csv` next to the DLL) -- the dog by every source, the published
  place before and after the display rule, our placements, the attack state, the job and its soldier. fake_er: `--trace-draws` adds
  each buddy draw's bone mean (`buddym=`); `pr er block 1|0` (an Elden Ring wall: the puppet stays while MGSV's Snake runs on; the
  `--actors near` characters move by the slip in MGSV, as the real host maps them; the trace's last two columns are the slip).
- Friendly fire (saves blocked): Snake's tranquilizer darts at the dog biting its soldier went to the soldier; the bite went on to its
  let-go, the dog's bond (`TppBuddyService.GetFriendlyPoint`) stayed 78/81. A round landing on the dog itself was not achieved.

**0.8.0 integration with both games (2026-11-04; the review fixes, then MGSV + Elden Ring 1.17.2 live; docs/STATUS.md "0.8.0 polish"):**
- Review fixes (commit "Review fixes for the D-Dog polish"): MGSV's order markers are shown again when the Lua tick returns early (a
  dismissal, a load or Attack turned off mid-order had left them hidden for every buddy); the slip's baseline comes only from a settled
  table (a treadmill jump's table/k mismatch read as slip), and each change of the level is no longer than Snake's path since the
  last level taken and never against his motion (a patrol walking together was taken for the slip); after a Wait the shown offset
  shrinks with the dog's way to its goal (`DogLogic` `WaitFade`: the order's soldier, else Snake; none once it holds its enemy; a Wait
  again keeps what is left) -- at a fixed 4 m/s a bite from Wait landed metres off its enemy; ActorProxy's buddy clearance uses MGSV's
  own dog, not the shown one; while Snake is staged a rigid buddy draw is the buddy's only if its mesh was the buddy's at home
  (StageAlong puts the dog among the site's props); `[Dog] Enabled = false` gives MGSV's place untouched (no limiter, no Wait offset).
- Live: 40 attack orders in two 10-minute runs (22 + 18; 36 connected, every one released), every suit the save has, the published
  place at most 0.31 m a frame (0 over 0.5 m in 73,000 frames), 0 alerts, 0 warnings. A Wait run into a wall: MGSV's Snake 12.25 m on,
  the dog's Elden Ring place moved 1 cm (the guest's offset 12.26 m, the host's pin found nothing to add). A climb-up: `staged: taken
  along to Snake at the site (70.5 m)`, `back home`, `MGSV moved it 14.6 m from where it was put (back home with Snake) -- put there
  again`. The guest's own 1 s pick memory was used 0 times in both runs (the host's latch had the pick at every click).
- Found live and fixed after (unit-tested, not yet live): Elden Ring teleporting Snake while the dog waited (`call setpos` 21 m in the
  test) was taken for slip once his runs had made his path that long -- the waiting dog was shown 20.9 m off. `SlipObserver` now counts
  the characters anew when they all move at once by more than Snake's MGSV motion since the last table (`Rebased()`; the level kept).
- Open: MGSV's green marker over the dog (with its distance) still showed over a biting dog in 2 of ~15 frames looked at (session 1,
  the knife's and the battledress's bites): the colour rule misses that draw; `!capture probe` during a bite would name it.

- **Observed, never changed** (byte-checked: `Addresses.h` D-Dog block, `verify_addresses.py`): the dog system's message handler
  `0x140C56B10` (learns the dog system -- live `0x68fb7e40`, one dog -- its style byte `+0xD4` and each order it takes, with the order
  record `R = [this+0x28] vf 0x58(index, 0, 0x47EB44EF48E3)`: `+0x54` bits, `+0x5C` timer, `+0xC2` target), the attack state
  `0x140C64DE0` (enter / contact / result / exit), TppSoldier2's dog-attack handler `0x141491A70` (a sentinel in the result tells an
  accept -- result 0 -- from a refusal, which writes nothing; the soldier's id from its own local-index table). `!dog trace on` adds,
  at run time, the dog's other 13 action states (the table MGSV builds at `0x142C3BB40`, checked live entry by entry), the attack
  decision `0x140C64770` and the message box's send `0x140BF3A80` (messages to the order's target).
- **The dog's place.** Its location object (the location class, as D-Horse's and D-Walker's) is found on its 94-bone skin (one scan,
  confirmed 0.5 s later: `dog: location object 0x68f76040 confirmed (31 checks on it)`); `!dog shift 3 3` moved it, the skin and its
  position cache (`[[sys+0x10]+0x20]`) in the same frame, and it sat where it was put.
  - `[Dog] Treadmill`: every treadmill jump moves it by the same shift at the next frame (when Snake's own placement shows), while it is
    within `TreadmillMaxDistance` (40 m) of him -- a waiting dog within 4x that, a dog on an order always. As Elden Ring draws it (the
    odometer + each dog mesh's place relative to its list's anchor; props near it excluded), a 22 s run toward the camera: **off -- 5
    jumps of 6.01-6.03 m; on -- largest step 0.22 m, none over 0.5 m**. The fake_er trace's buddyPos and rootPos jump in the same frame.
  - **Wait** (the call menu's Wait, or `!dog order 9`): MGSV's order bit 2 of `R+0x54` (polled) keeps the dog moved with the jumps;
    Snake ran 22 m away and back (3 jumps each way): the dog's Elden Ring place stayed at (-1.15, 0.37) to the centimetre; MGSV ended
    the Wait itself when Snake came back within ~2 m.
  - **The leash** (`Leash`, 30 m): a free dog farther than that, outside MGSV's camera field, is put out of view beside / behind Snake
    (Follow.cpp's catch-up rule): `!dog shift -20 0` twice -> `dog: leash: 39.1 m behind, out of view -> 4.8 m from Snake` 17 ms later.
- **Attack orders** (`[Dog] Attack`; the job is `DogLogic` `Job`, the player's command reaches it through Follow.cpp's command detour):
  the call menu's attack entry (this save: hold LB, right stick right = "Do it (wound enemy)", let go, click it) -> `follow: the
  player's buddy command 14 (arg 65535)` -> `dog: the player's attack command 14 (MGSV's own target arg 0xffff) taken over` -> the
  host's pick (the ActorTable record with kActorDogTarget, re-checked hostile and alive) gets one soldier of its own (ActorProxy
  `Dedicate`: enabled inert -- restrict-notice + puppet before his first update --, on the sandbox ground, never parked for the buddy's
  clearance) -> `soldier 0x450 realized in 807-1070 ms -> command 14 arg 0x450` (MGSV's own order on him) -> kGEvDogAttack
  **Ordered** -> MGSV's dog: the attack decision, a wind-up (state 0, 0.7-1.5 s), the run (state 2), the attack state with the
  soldier's accept (`dog: bite connected (0x48e4ede3daed body attack accepted by soldier 0x450, local 0, from dog 0x2600) -- he stands
  in for Elden Ring character 0xffffffff00001000`) -> **Connected** (MGSV's attack id and share) and `kBuddyEngaged` -> contact 51-59 ms
  later, the pin (MGSV's own; the dog sends the soldier `0x93B01C23483C` every frame) -> MGSV lets go (result 14) -> **Released**, the
  soldier given back (disabled), the dog whistled back (`WhistleBack`).
  - Every style, once each in a row (styles other than this save's set with `!dog style N`: memory only): wound (byte 3, the
    battledress, ATK 147) Ordered -> Connected 6.2 s, pinned 6.2 s; busy (byte 0, the bite, ATK 144) 3.3 s, 7.9 s; byte 2 (then read
    as the knife -- it is the stun knife, see 0.8.0 above) 6.0 s, 4.4 s; byte 4 (then read as the stun knife -- it is the knife) 6.0 s,
    4.4 s. The dog back
    within 1.3-1.5 m of Snake 6 s after each release. Alert phase 0 throughout, 0 `alert:` lines. While Snake ran (6 jumps during an
    order) it connected all the same.
  - **The style byte** (live): this save's `+0xD4` is 3 = `buddyDogEquipType` 3, and the call menu says "Do it (wound enemy)": byte 3
    is the battledress (a body attack: `0x48E4EDE3DAED`, no knife bit at the soldier). (This round also read byte 4 as the stun knife
    from the soldier's `+0xA` byte; the 0.8.0 integration showed with MGSV's own equipment that 2 is the stun knife and 4 the knife.)
  - **No pick** (no kActorDogTarget record): `kGEvDogAttack Refused ... on Elden Ring 0x0: no Elden Ring enemy picked`, the dog
    whistled back. A torn read of the host's table keeps the last good one; the target counts as gone only when absent from three
    good tables in a row (a torn read once ended a "Get it" as Lost).
  - **Lost** when it never connects: `ReachTimeoutMs` (12 s: the knife styles took 6-7 s at 15 m), the target gone, or stuck -- no 0.5 m
    gained for 3 s after a 2.5 s grace (MGSV's wind-up alone is up to 1.5 s: the first rule, 2 s from the order, called two good runs
    stuck).
  - **Why one dedicated soldier:** with plain full proxies (`!proxy on`) MGSV's dog ran at the soldier, and as it came within 5.5 m
    ActorProxy parked him (`BuddyClearance`: disabled, out of the world) -- the soldier vanished in the frames, the run ended with
    result 8 (the target check `0x140C64940` failed) and nothing was bitten.
- **"Get it"** (byte 1, set with `!dog style 1`): the order on a sleeping enemy's asleep proxy (`[Fulton] Method = proxy`): the dog
  ran 27 m to him and MGSV's own Fulton took him: `proxy: fulton of Elden Ring character 0xffffffff00001001 succeeded (MGSV's own
  Fulton on its proxy) -- kGEvFulton a=1`, 16 s after the order (fake_er took the actor out). The job waits for that yank
  (`fetchTimeoutMs` 30 s; a dog standing within 6 m of the sleeper is busy, not stuck).
- **Capture** (`KeepDrawsNearProxy`): the dog's own meshes are learned from buddy draws rooted on its skin (within 1 m) with no proxy
  within 4 m; while it bites, its draws within the proxy's capture radius are kept as the buddy's, the soldier's are dropped
  (`!capture`: `buddy draws kept near a proxy 7371`, `proxy draws dropped 8387` after one order). A soldier who stops being a placed
  proxy stays excluded at his place for 5 s (his knocked-down body is still drawn a moment after the Lua thread disables him): live,
  his draws had been taken for the dog's beside it, 1.5-3.2 s after the release with a 1.5 s linger.
- **Phase lock:** ActorProxy's lock (calm + sweep) runs once a dog soldier has been out; if MGSV's phase stays up 20 s, `dog::DisableAttack`
  turns `Attack` off for the session (`dog: Attack OFF for this session -- ...`; `!dog attack on` tries again). Never needed live.
- **M3: two 10-minute runs** (orders through MGSV's real call menu from the pad file, runs with treadmill jumps between them, a Wait
  with a run away and back, the fetch, an order with no pick; `m3.py` in the session's scratch): (1) 25 orders in 11 min: 18 Released
  (every attack style), 2 Refused (no pick, as asked), 5 Lost -- 3 fetches whose balloons went up 3-4 s after the job had called the
  dog stuck, and 2 stalking knife runs; (2) on the build with MGSV's own "let go" as the Lost verdict: 23 orders in 10.5 min, 19
  Released, 1 Refused, 0 attacks lost, 3 fetches called stuck as the dog ran back from a sleeper whose balloon then went up (fixed:
  no stuck verdict once it has been at its target; the next fetch: Ordered, `kGEvFulton a=1` 15.3 s later, Connected, Released).
  Both runs: soldier realized in 0.90 / 0.92 s median (max 1.05 s), Ordered -> Connected 2.9-7.4 s (median 4.4 / 5.0), the dog
  back within 3 m of Snake 0-3.9 s after every end, **0 alerts, 0 warnings in the guest log** (`dog:` or any), kBuddyEngaged from
  each Connected to its Released (6230 / 6480 frames). As drawn (the dog's 21 body meshes, mesh by mesh): no treadmill jump at all;
  33 / 27 frames over 0.5 m (max 1.27 m), every one within 1.4 s of a Connected or a contact -- MGSV's leap and the takedown's root
  snapping onto the soldier (the draws' root, not their skinned vertices). Then four orders and a fetch on the final build, all
  connected. **D-Walker and D-Horse** on the same build: a lead `glided 0.00 s / 0.0 m`, D-Walker ridden 38 m and dismounted;
  D-Horse deployed in session, its object confirmed 0.5 s later, ridden 100 m at a gallop with its treadmill, dismounted. An
  in-session D-Dog is now active 1.3 s after `!buddy dog` (its skin looked for within 12 m of Snake: it used to take 60 s).
- **Recordings** (`recordings/`, never committed; README.txt there): `ddog_attack.prrec` (deploy, Bark, an attack through the call
  menu with its Ordered / Connected / Released and kBuddyEngaged, Wait with a run away and back, the whistle), `ddog_follow.prrec`
  (runs with treadmill jumps, two jumps, a ladder; the dog as drawn never over 0.16 m a frame).
- **Not done:** MGSV's own "D-Dog: Found enemy" (`Announce`: parsed only), the transform query sent natively (`!dog where`: the box
  routes it but the dog's handler never sees it; not needed), whether Snake's MGSV rounds hurt the dog mid-bite.
- Console: `!dog` (state: sites, the dog system, style byte and vars, order record, location object, attack state, accepts, the job,
  the ATK rows, capture counts), `!dog on|off`, `!dog treadmill|attack|keep|puppet on|off`, `!dog shift <dx> <dz>`, `!dog leash <m>`,
  `!dog scan`, `!dog where`, `!dog order <8-16> [arg]` (MGSV's command as ours), `!dog go [11-15]` (as the player's attack command),
  `!dog dedicate <handle>|off`, `!dog style <0-7>` (test: the style byte in memory), `!dog trace on|off`, `!dog watch <id>`.
- Offline: `tests/test_guest_dog.cpp` (style and attack ids, the job's stages and timeouts, the treadmill's choice, the leash),
  `tests/test_guest_actorproxy.cpp` `TestDedicated` (first, on the ground, never parked for the buddy, still for Snake).

### 8.5 Quiet in Elden Ring (`[Quiet]`, `Quiet.cpp`, `QuietLogic.*`; protocol v18; live 2026-11-04, MGSV + fake_er on `/dev/shm/phantomring_g`)

The design is docs/research/quiet.md; what the live rounds settled is its §2.10 (M1, M2/M3). In short:
- **The borrow** (`Borrow = always`, `BorrowListInIdroid = true`, the player's choice): at the sandbox's start, while MGSV's saves are
  guarded, her bits are read (`quiet: save state gone (...)`), snapshotted with the whole buddy vars block and SetDisableBuddyType's
  static byte, and her state's plan applied in memory (gone: obtained + sortie + LOST cleared; never recruited / killed: obtained +
  sortie; recruited-not-deployable: sortie). Each of the four states passed its live round trip (her block loads; put back CLEAN).
  The put-back (`!quiet borrow off`, a guard lost, Elden Ring gone for 3 s) uses the setters, writes her counters and her bits (the
  `*MissionStart` copies included) back raw and diffs the block: `-- CLEAN (no byte of Quiet's differs)`. Never a gvar, the bond or
  MGSV's reunion; a Quiet in sickbay / dying / dead isn't borrowed.
- **Her place**: her location object (the BuddyLocation finder: confirmed in 0.5 s), held at the host's post mapped into MGSV
  (`MirrorRegion`, on MGSV's ground), moved with the treadmill in the frame Snake's placement shows (0 of 2829 draw-list steps over
  0.5 m through 10 jumps). She stays where she is put while no MGSV enemy is about.
- **The grace**: on a new `graceSeq` with a seat: her own `MoveToPosition` with her aim off, then her object at the seat in the same
  tick -> `kBuddySeated`; she crouches with her rifle upright ~0.4 s later (`SitPose = kneel`). Up on `kHostAtGrace` falling: her
  order with aim to her post.
- **Orders**: MGSV's call menu offers Fire / Take aim only while she snipes from one of MGSV's snipe points with the stand-in as her
  target; the job carries any order MGSV offers onto the host's pick (6 of 6 hit, 3.1 s), but getting her into that state is not
  reliable yet (quiet.md §2.10 M3; the 0.9.0 integration's one try with both games: the "sniping position" request brought her to
  Snake, Fire stayed greyed): the host's `[Quiet] orders` (0.9.0) ships off, so the host publishes no pick and nothing here runs.

**0.9.0 integration with both games (2026-11-04; review fixes and what the real Elden Ring showed):**
- **The borrow waits for the host's layer** (the review: with the host's `[Quiet]` off every save would have listed 0.8.0's Quiet,
  without FireOnlyOnOrders, NoDying or the mirror): at the sandbox's start, at `!buddy quiet` / `[Buddy] Type = quiet`, or from
  LuaStep once `kHostQuietLayer` is seen (every 5 s at most; `quiet: borrowed ... (Elden Ring's Quiet layer is on)`). The put-back for
  "Elden Ring is gone" (3 s without a heartbeat) is followed by a new borrow when a host with the layer comes back (live: put back
  CLEAN when Elden Ring stopped, borrowed again 15 s later by the recorder's host, put back CLEAN again when it stopped).
- **Quiet (Scout) / (Attack) are greyed only while she is out** (`StrikeMgsvPosts`): greyed from the boot, they couldn't be chosen,
  so no player could deploy her (live). Now `QUIET SCOUT / ATTACK active (they deploy her)` while she isn't out, and ATTACK deployed
  her (`boot: the player deployed Quiet from the iDroid (was D-Walker)`), held at the host's post 103.8 m from the hangar roof she
  dropped onto, once settled.
- **The sit settles before its echo**: from her order MGSV's Quiet takes ~3 s to settle into her rest at the seat (a turn, a hop of up
  to 0.7 m -- she was seen 2 m in the air -- then the crouch with her rifle upright), longer than the rest's black (~1.5 s): live she
  hopped beside the grace 1.5 s into the menu. Her order now goes out from 2.2-3.5 m off the seat (she is placed 2.5 m off first: an
  order from her post 47 m away played a longer arrival) and `kBuddySeated` is echoed once she has settled (`kSitSettleMs` 3200 after
  the order and still for 300 ms, at the latest 4500): `seated at the grace seat (rest N, settled 3219-4158 ms after her order)`; the
  host fades her in (er/GraceRest's late echo).
- **Getting up without a dash**: her get-up order (MoveToPosition with aim) makes her relocate as MGSV's Quiet does -- a turn, then a
  cloaked dash during which her AI owns her location object (live: she vanished in view 0.5-1 s after Snake left the grace; sent with
  her next post she ran back to where the order found her and 40 placements in a burst didn't hold). She now keeps her rest pose and
  is placed at her posts by her object (which keeps the pose); the order goes out only while the host has no post for her (unseen),
  where her own relocation is what Elden Ring would show anyway (`quiet: getting up while unseen (no post)`).
- **The treadmill and the host's post**: the post comes back corrected by k 33 ms ago, so for a frame or two after a treadmill jump it
  was off by the jump (placed 5.9 m one way and 6.0 m back the next frame). The holder now waits 250 ms after k moves.
- **Review fixes** (unit-tested where pure): EnableFire / EnableDying re-sent after every sandbox load (`AtReady`) and every 5 s, a
  send counted only when MGSV answered `true`; her "can't" (`kQuietNoTarget`) ends the order at once (Lost, MGSV); a queued get-up is
  dropped when she is no longer out; nothing is placed while Snake isn't ready on foot (climbing at MGSV's staging site -- live, a
  mantle during a hold: no placement --, parked, in a vehicle, on the Fulton anchor); her treadmill shift and our placements are no
  dash for the cloak test; her hit counts for the order only with her rifle (`OrderRound`: not her dash attacks 141 / 142) and is
  reported with the attack id's rifle or her equipment's (`ShotWeapon`); a rigid draw within RigidRadius is hers once its mesh moved
  with her (two places >= 5 m apart) or while she is within 15 m of Snake; the console reads a snapshot and `!quiet set region` hands
  the game thread a new region (no data race).
- Console: `!quiet` (state), `!quiet where|r|hits|bits|diff|last`, `!quiet borrow [off|test-unguarded]`, `!quiet hold <x> <z>|rel
  <dx> <dz>|off`, `!quiet shift <dx> <dz>`, `!quiet standin <0xhandle>|off`, `!quiet cmd <1-7> [arg]`, `!quiet trace on|off`
  (PhantomRingQuietTrace.csv), `!quiet layer|orders|keepmarks|menufire ...`, `!quiet set <key> <value>`, `!quiet lua <chunk>`.

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

## 10. Live test checklist (first run, 2026-10-12; kept for reference -- every item has been run since)

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

Settled live since the first run (2026-10-12/13): `currentInventorySlot` as the weapon slot in hand (0-2, 3 = support
item); the native life write sticks (§7); the D-Walker class (`TppCommonWalkerGear2`), its GetPosition / SetPosition
(`pos = {x, y, z}`, §17) and `CallBuddy`'s arguments; EndLoadingTips; the audio mutes (§18.2); MGSV's ammo component
(§18.11). Still open:

- **Runtime meaning of:** camera+0xC (focal, mm) and the film size behind the FOV; the vars liveness rate.
- **Native side:** the native hold call's effect on animation (native_treadmill is the default and works; `native` was
  never played); the melee shape geometry vfunc; the Steam vtable swap reaching the in-process client.
- **Stance/flag bits:** these come from `PlayerInfo.AndCheckStatus` names (values in re-backlog M5).
  Dive/roll detection relies on NORMAL_ACTION.
- Every Lua API not named in §18 is still used as IH/MGS5VR use it on this build, without a check of its own.
- D-Horse with Elden Ring (§8.1): only MGSV + fake_er/recorder so far -- how the host draws and grounds the horse, the
  jump after a far dismount (gone with `[Buddy] HorseTreadmill`, §8.1a: does TppHorse2 own a location object, does the saddle
  carry Snake when it is moved?), riding speeds against Elden Ring's slopes (DESIGN `max_speed_vehicle`).
- The regeneration curve (§18.15) against Elden Ring's real hits (fake_er's `hurt` only).

## 12. Offline verification

`cmake --build build --target check_guest` (wine; `-DPHANTOM_WINEPREFIX=…`; optional
`-DPHANTOM_LUA51_SRC=<lua-5.1.5>/src` for the pump test):

| Test | Covers |
|---|---|
| `tests/test_guest_script.lua` (lua5.1) | the embedded script against a mock engine: Tick result order, command parser (Warp vs WarpAndWaitBlock, SetPosition), D-Walker steps, mission loading (direct/reserve/ACC), setup, console packing, the NVG flag, `refill`, `AutoAim`, an empty engine and an engine that throws |
| `test_guest_pump` (real Lua 5.1.5) | Pump.cpp end to end: VM probe, script load and refs, Tick → frame state, the full boot sequence title → Continue → 10040 → load the sandbox mission → setup → D-Walker → centre → Ready (status texts), frame → Lua commands, the dev console over a real mapping (results, errors, syntax errors), the buddy dismissed from the iDroid or lost (8.3: MGSV's leave order with its menu open, ReviveBuddy holding off while the buddy goes, the menu window from NONE's start per buddy, a lost buddy called back; the pump's clock skipped ahead), live `phantomring.lua` override, leaving the sandbox; the engine's Lua stack is unchanged after every pump |
| `test_guest_logic` | odometer (teleports, rebase, drift over 100k frames), treadmill (radius, direction, cooldown, height, with the odometer over 100 m), life clamp/death, ammo-counter shots, the refill's magazine sizes (§18.11), matrix→yaw/quat, quaternion rotation, focal→FOV |
| `test_guest_horse` | the D-Horse treadmill's pure half (§8.1a): its location object picked near the skin (Snake's, origin and excluded ones never), confirmed/lost, the setter's arguments for a jump (ridden: feet on the centre's ground), the ridden jump limit and landing watch, the treadmill's choice per case, a ridden minute at 15 m/s up a slope with the frame glue copied in -- a simulation that *assumes* the saddle carries Snake (odometer exact, tracker confirmed, horse on the ground under him) -- and the run where it doesn't (one jump, put back, no more) |
| `test_guest_link` | attach handshake on a real file mapping, version refusal, kRenClearAll, GuestState/PoseBlock/events/pad flow, `!` console, host restart (re-attach, drain, console reset), host death; the shipped ini equals the built-in defaults; ini parsing |
| `test_guest_proxy` | a dummy exe loads the proxy (`dinput8=n,b`) and gets a working real IDirectInput8W without and with `PHANTOMRING=1` (identity refusal, log checked); the wrapper hides gamepads/joysticks, keeps keyboard/mouse, and handles refcounts and QueryInterface |
| `mgsv/verify_addresses.py` | every signature in Addresses.h against the exe on disk |

## 13. Console

- `pr lua '<chunk>'` runs a chunk in MGSV's game VM and returns every result tostring'd, tab-separated.
  Errors set status 1.
- `!status` shows boot, pump, frame, the Steam guard, host and centre.
- `!boot` restarts boot automation.
- `!treadmill on|off`.
- `!jump`: as if the `[Jump]` button went down (§6; the log says whether Snake jumped or why not).
- `!centre` sets the centre to Snake's current position.
- `!debug on|off`.
- `!blast` (explosions sent / kept out), `!menu` (menu state), `!sound ...` (§18.2), `!whistle`, `!follow on|off|spot a s|<m> <ms>|method m|modes list` (§18.3).
- `!buddy [walker|horse|dog|quiet|none]` (state of the buddy and its skin, or another buddy in session, §8.2), `!regen
  on|off` ([Combat] RegenGate for A/B tests, §18.15), `!loc <id>` / `!placeobj <id> x y z [yaw]` (experiments: the
  player location manager's object for a GameObject id, and its set-position on the next frame; it knows only Snake), `!proxy`
  (ActorProxy: the slots, each soldier's job and life, §20.5), `!proxy on|off` (this session), `!fulton` (the anchor's state, §20.4),
  `!fulton <handle> [fail]` (kGEvFulton by hand), `!skins [radius] [ms]` (every skinned model published), `!buddy hint` (§8.3),
  `!horse [on|off|scan|shift dx dz]` (the D-Horse treadmill, §8.1a).
- `!refill` (what kHEvRested does), `!ammo` (refill state, last refill), `!ammo drain` (dev: every ammo/item stock to half
  through MGSV's own setter, §18.11; queued and run on the Lua thread with the next Tick's results -- it used to call MGSV's
  component lookup and setter from the console's worker thread -- `!ammo` shows the outcome); a refill asked more than 5 s
  ago that never came back (a failed `PhantomRing.Tick` loses that update's commands) is queued again on the next request; `!capture hud skip <ps hash>` (dev: leave one UI pixel shader's draws out of the HUD
  layer, how the night-vision overlay was found, §18.12). `!status` also shows the ammo refill and Tarnished mode.
- `!dive` (§18.17): dives seen (and the sign that confirmed the last one), X presses while standing with no dive.

`phantomring.lua` next to the DLL overrides the embedded script, and is reloaded within 1 s of a change.
`phantomring.default.lua` is written at start as a starting point.

## 14. Protocol

v10 (2026-10-13): `kGuestNvg = 1 << 16` (§18.12), `kHEvRested = 7` (a = 0 grace rest, 1 respawn; §18.11),
`kHEvTarnished = 8` (a = 1 Tarnished mode on, 0 off; §18.14). GuestState `ammoStock` is filled now (§4).
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
`Capture.*` (the decisions and the probe), `CaptureMesh.*`, `CaptureAssets.*`, `CaptureRing.*`
(the publishing pipeline: built and live since 2026-10-12, docs/STATUS.md),
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

### 15.3 Publishing (CaptureMesh/Assets/Ring; live)

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
- **Stale keys** (`[Capture] Revalidate`, default true; 2026-10-14; live pass A: `stale entries restarted 0 / pruned 482 -> 713`
  over two buddy swaps, no false colours on D-Walker/D-Dog close up). Capture keys textures by MGSV's
  `ID3D11Texture2D*` and meshes by their buffers' pointers, and kept those keys forever; but the copy (and its AddRef) was
  evicted a minute after it was *first* asked for (Capture never asked again once it had the bytes), so after a minute
  nothing held MGSV's resources and MGSV could create another one at a freed address, which then went out under the old
  key: another model's SRM, normal map or layer noise in D-Dog's or D-Walker's diffuse slot (the player's false colours;
  session 6 shows the pattern: low-resolution textures copied first, full-size ones a moment later, the box's textures
  copied anew 48 minutes in while only 3 of its 5 meshes were new; docs/HOST.md "Material roles"). Now every copy carries
  a serial (`assets::Serial`, never reused); each use touches it (so a resource in use is never evicted) and compares it
  with the one the key was made from: another one (or none) = copy and key it afresh. After each eviction entries whose
  copies are gone are pruned (their bytes too). `!capture` shows `stale entries restarted N / pruned M`. The rules
  (`Stale`, the eviction age, restart, prune) live in `mgsv/CaptureKeys.h`, which `test_guest_logic` runs against a fake
  copy table: a texture drawn every frame for minutes keeps its copy and key; one let go and replaced at the same address
  is copied, keyed and sent afresh (never under the old key); unused entries are pruned with their copies.
- Planned DrawRecord fields: `material` = low 32 bits of the PS bytecode hash (the Fox shader
  permutation), `params[0]` = `m_useWeightCount`, `params[1..7]` = `cPSMaterial.m_materials[0].xyzw,
  [1].xyz`; `kDrawRigid` with one matrix when the VS has no `VSBones`, the weight count is 0, or the mesh
  has no BLENDINDICES. A mesh without BLENDWEIGHT is skinned with weight 1 on BLENDINDICES.x.

### 15.4 Console

- `!capture`: device, Present hook, counters (draws, deferred draws, constant-buffer maps, command
  lists, contexts, layouts, shaders).
- `!capture probe [N]`: describes the next N frames into `PhantomRingCapture.txt` next to the DLL.

### 15.5 Projectile draws (protocol v13 `kOwnerProjectile`, roadmap 5.4; **works since 2026-10-31**, `[Capture] ProjectileMeshes = true`)

**Why it learned nothing (2026-10-31, `!capture probe` right after a release):** MGSV draws its thrown grenade and its rocket
as *skinned* G-buffer draws with one weight (`m_useWeightCount` 1, bone 0 the identity), not rigid ones -- the sleep grenade
`vs 0x4bd72652708ef87d`, 618 indices, its `m_world` on the throw's parabola within 0.04 m frame after frame; the FB MR rocket
`vs 0xc5a2b913756f1ec3` in three LOD pairs (306/204, 222/108, 138/102 indices) -- and the rocket flies 250-375 m/s (41.7 m
down the line 10 frames after the shot, then 4.2-6.3 m a frame), past the 120 m/s cap. Now a one-bone skinned draw is tested
where its bone puts it (`m_world * bone 0`, as it is published), the round cap is 420 m/s (`test_guest_logic`: the measured
rocket, every frame of the window), and two-triangle cards (<= 6 indices: the rocket's flame quads) are never tagged.
Live: a frag grenade `projectile: first thrown draw tagged ... (8 frames after the release)`; `recordings/throw_grenade.prrec`
(two frag throws): 46 lists with a projectile draw (mesh `b1332cea2c489926`, 642 indices, 1 matrix) from 0.15 s after the
release on, nothing else tagged (Snake 33653, D-Walker 6739 draws); `recordings/fire_rocket.prrec`: 16 lists, 32 draws, the
rocket's three LOD pairs only. fake_er counts them (`projectile draws N in M lists`, `drawsByOwner` in its summary).

**Re-recorded in the open (2026-10-31 night, MGSV + `pr record --trace F --trace-draws` on the apron; the support item made active
with `Player.ChangeEquip{..., toActive = true}`, the launcher through the D-pad-up wheel):** fake_er's `--trace-draws` writes each
list's projectile draws (mesh and place) next to the `# event` lines, so every tag can be checked against its release.
- `recordings/throw_grenade.prrec` (frag twice, the sleep grenade, the shock decoy): each grenade tagged in 23 lists (frames +8..+30
  of its release), one mesh each (frag `b1332cea2c489926`, 642 indices; sleep `2142aaa5a2f2dabc`, 618), at most 0.01 m off its arc;
  those meshes are Snake's (`Player`) only while in his hand before the throw. The decoy (`throw: equip 406 (not an explosive we
  send)`, then `placed #4 ... (decoy)`): nothing tagged. 0 tags outside a release's window.
- C4 (391) and the sleep mine (418), aimed and not (both were set at his feet: `placed #5`, `placed #6`): no release noted, 0 of
  1968 lists with a tag.
- `recordings/fire_rocket.prrec` (FB MR R-LAUNCHER, three rockets flying 129, 52 and 99 m before they hit): 12-17 lists each,
  frames +1..+17, 4.2 to 70.9 m down the line (4.17 m a frame, 250 m/s), only the rocket's three LOD pairs (306/204, 222/108,
  138/102 indices; the 306 one is also Snake's while it sits loaded in the tube). Nothing else, nowhere.
- **D-Walker beside him** (led 2.8 m off since this round): its one-bone parts -- the kind of draw a grenade is -- lie within 1.23 m
  of its feet, 0.48-1.76 m up (10439 of them in the grenade recording), so a throw his way passes through them. A draw inside the
  buddy's body (1.6 m around its feet, 0.5 m below to 2.5 m above) is never tagged now (`ProjectileLogic.h InBuddy`). Live, a frag
  thrown through the walker (0.94 m from its feet at 1.3 m up): frames +1..+14 tagged nothing (the walker's 13-14 draws stayed its
  own), the grenade tagged from +15 on, 1.7 m past it. `test_guest_logic` `TestProjectileTagLive` replays these releases and draws
  (arc, line, Snake's gear and the tube never, the walker's parts never, decoy / C4 / mine not noted at all).

*Before (live pass A, 2026-10-14):*

**Live pass A** (docs/FEATURES-1.md): 3 frag grenades thrown -> no draw tagged (`!capture`: `3 throws, 2 explosive rounds
noted, 26 draws tagged`, all from the rockets): MGSV's grenade is not a rigid G-buffer draw within the arc's slack, or not
there at all; 2 FB MR rockets -> the first tag 18 frames after the release 41 m out, a 4-vertex quad (`capture: mesh ...: 4
verts, 6 indices, 0 bones (projectile)`), so the tag missed the rocket body (an exhaust sprite or decal) and the host never
confirmed a template. Stays off; next: probe a throw's frames (`!capture probe`) for the grenade's own draw.

`ProjectileTag.*` (rules: `ProjectileLogic.h`, `test_guest_logic`), `[Capture] ProjectileMeshes = false` (off by default:
see below). Throw.cpp notes each
thrown explosive at its `kGEvThrow` (release point, velocity, game frame); Combat.cpp each explosive round of Snake's at its
`kGEvFire` (muzzle, direction). For 30 game frames after a release, a **rigid** G-buffer draw (weight count 0) whose
`m_world` origin lies where the projectile must be is published as `kOwnerProjectile`, whoever it would have belonged to:
- thrown: within `slack` = 0.45 m + two frames of its travel of `p0 + v0 t - g t^2 / 2` (g 9.8; t from the draw's game
  frame) -- but only once that sphere is clear of the hand (|predicted| - slack >= 0.6 m: frame 8 for a 12.6 m/s throw), and
  the draw itself at least 1.05 m from the release point and ahead of it (dot with v0 > 0). Review: in the first frames the
  sphere is a shell around the hand that took in Snake's own rigid gear (a holstered sidearm 0.7 m below, the gun in his
  other hand, a slung rifle behind) while the grenade itself was still inside 0.6 m, and the host learned that gear;
- round: at least 0.6 m ahead of the muzzle on its line (never the launcher's tube), no further along it than 1.05 m +
  120 m/s x (t + two frames) (review: a prop or the walker 40 m down the line was tagged one frame after the shot), and
  within 0.5 m of the line plus the drop so far, capped at 0.4 m (a grenade-launcher round falls; a rocket's line doesn't
  widen by metres).
Proxy/Fulton-anchor exclusions apply to these too (a proxy's rifle can sit on a rocket's line; a grenade passing a proxy only
loses that frame's tag). `!capture` adds a line: throws / rounds noted, draws tagged; the first tag is logged
(`projectile: first thrown draw tagged ...`). The host takes a template only from two tagged lists 3+ frames apart that show
the same meshes flying away along the release, then checks the meshes' real size (docs/HOST.md "Projectile meshes and muzzle
flashes"). **Off by default** because a mis-tag takes that draw away from Snake or the buddy in Elden Ring's frame (a host
with `[Render] projectile_meshes` off gives tagged draws within 2.5 m of Snake back to him, but a buddy part is lost), and
tagged draws further out have their meshes and textures sent over the render ring. Turn it on together with the host switch.
(2026-10-31: they are skinned with one weight -- see the top of this section; such draws are tagged now.)

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
- The night-vision goggles' noise and darkening IS drawn in a UI job (one full-screen quad, pixel shader
  `0xae282a7fe2bdbdd9`): it is left out while the goggles are on (§18.12).

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
- **D-Walker's volume, 2026-10-31: `[Audio] WalkerDistance = 8` (default).** D-Walker's Wwise game objects (the same
  tagging as below: first positioned within 2.8 m of the walker and 1.5 m nearer to it than to Snake) are positioned 8 times
  as far from MGSV's listener (its camera) along the same line, in the `SetPosition` detour: Wwise's own distance attenuation
  turns them down and their direction stays. The walker gate is 1.8 m from Snake now (was 3 m: the loudest steps, beside him
  as he starts and stops, stayed in). Measured with **`[Audio] Meter`** (`AudioMeter.cpp`, diagnostics, off by default):
  recording MGSV's stream from outside gives no samples on this machine's PipeWire (`parecord --monitor-stream` writes only a
  header, for every stream), so the guest meters MGSV's own output -- `CoCreateInstance(CLSID_XAudio2)` watched, the engine's
  `CreateSourceVoice` and the voices' `SubmitSourceBuffer` vtable slots swapped, RMS per 0.5 s of what Wwise hands XAudio2
  (one voice, 2 ch 48 kHz float: the mix the player hears from MGSV); `!sound meter [mark | since | n]`. Snake running 11 s
  with the walker following: factor 1 median -30.7 / -29.4 dBFS (loudest 0.5 s -19 / -20), 4: -36.0 / -35.7, 8: -39.7 (the
  steady run -37..-41, Snake's own running ~-37; Elden Ring's sounds -35..-42) -- but the loudest windows (-20) stayed, when
  Snake started and stopped with the walker beside him: hence the 1.8 m gate. With it (final build): factor 1 median -29.2,
  loudest -19.8, peak -4.7 dBFS; **factor 8 median -39.9, loudest -33.2, peak -15.6**. `!sound distance <k>` live.
  `!sound event <id> <gain> 8` drops an event's posts (diagnostics).
- **Before (2026-10-12): `[Audio] WalkerVolume` NOT solved, experimental, off by default (= 1).**
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
- **Lag, 2026-10-31 (`FollowLeadMin`, `FollowSettleMs`, both off by default; `!follow lead <s> [side] [catch-up] [min m]
  [settle ms]`):** fake_er trace + the walker record's state polled at 20 Hz (`+0x56`, `+0x5C`) over 12 s runs: an order given
  as Snake starts (`Snake started moving`, 0.4 s after the stick) was taken and *not* walked (its spot only 3.8 m away: the
  speed is still low, so the lead is short); the "standing still" re-order 1 s later and a catch-up warp 1.8 s in each
  restarted the walker's ~1-2 s reaction: it walked 2.4 s after Snake started, then kept up (8 m -> 3 m over 7 s of walking).
  A minimum lead (5-6 m) and no re-order / warp for 2.5 s after an order were tried live: running medians 5.2-6.1 m (baseline
  5.0, 4.2-4.7 with other spots); the walker stops near a moving player and starts late whatever the spot. Solved by the lead
  below.
- **Led alongside (2026-10-31 night, `Walker.cpp`, `WalkerLogic.h`; `[Buddy] WalkerLead = true`, `WalkerTreadmill = true`;
  `!walker`):** D-Walker owns a location object of the class Snake's belongs to (a scan of MGSV's memory: of 30 such objects one
  stood exactly on `GetPosition`), and MGSV's own `SetPosition` for the walker (the handler `0x141706190`, case `0xb3d854925648` ->
  `0x14170D410`) is that object's setter (vf 0x68, `kLocationSet`) plus a request slot. So the walker is moved the way D-Horse is
  (8.1a): found by the scan (`walker: location object ... confirmed (31 checks on D-Walker)` ~0.5 s after it is out), tracked
  against `GetPosition`, moved through the setter (byte-checked as for the horse; a differing exe leaves everything as before):
  - **The treadmill** moves it by Snake's jump through its object -- one frame *after* Snake's own placement (his root and draws show
    a placement the frame after it, the walker's in the same frame: the list in between drew it the radius away), its destination
    record with it in that frame (a destination left behind it made its AI stop). Was: Lua `owarp` a frame or two late, a 6 m
    flash in Elden Ring at every jump.
  - **The lead**: while Snake moves on foot (> 1.5 m/s; < 0.6 m/s for 0.5 s ends it) a point tracks the spot `WalkerLeadSide`
    (-2.8 m: the screen's left) / `WalkerLeadAhead` (0) beside him as the camera sees him -- his velocity fed forward, the gap closed
    with `WalkerLeadTau` (0.35 s), never faster than `WalkerLeadMaxSpeed` (9 m/s: 0.15 m a frame) -- and the walker is put there
    every frame, turned toward where it goes at `WalkerLeadTurnRate` (6 rad/s). Only where auto-follow acts (`follow::LeadAllowed`:
    Normal Mode or a `FollowModes` one, following on, no player hold, not ridden, nothing busy), only within `WalkerLeadAcquire`
    (6 m) of the spot (farther: MGSV walks it over as before), never during a climb, Tarnished mode or the Fulton anchor. A walker
    found 2 m from where it was put (a whistle's placement, a warp) is MGSV's again; three such in 20 s, or a placement that doesn't
    take, turn it off for the session. Follow.cpp meanwhile only keeps the AI walking (its legs): an order to the lead's lane ahead
    of him when he starts, whenever its state byte (`+0x56`, `kWalkerRecordState`) says it stood 0.8 s, and beside him when he
    stops; no catch-up warp, no whistle.
  - **Live (MGSV + fake_er, the same 63 s pad script each time: 12 s run, stop, 8 s walk, stop, 10 s run with two turns, stop, a
    walk at an angle, 8 s run, stop; distance from the GuestState, the walker "as drawn" = the odometer + each buddy draw's place
    relative to its list's anchor, mesh by mesh -- what Elden Ring draws):**

    | | running median / p90 / max | walking median | standing median / p90 | as drawn: largest step per frame, steps > 0.5 m |
    |---|---|---|---|---|
    | before (0.6.0 build) | 4.97 / 8.36 / 11.77 m | 4.06 m | 3.46 / 10.27 m | 6.02 m, 58 (the parts' mean: culled parts count too) |
    | before (this build, `!walker lead off`, `treadmill off`) | 5.11 / 6.88 / 10.88 m | 3.73 m | 2.70 / 4.90 m | 6.04 m, 3 |
    | led | **2.80 / 2.82 / 2.93 m** | 2.80 m | 2.98 / 3.09 m | **0.16 m, 0** |

    `walker: ... treadmill on (29 moved, 0 failed); lead on ...: 5 leads, 2961 frames, 5 stopped, 0 lost, largest step 0.135 m`.
    Its AI walked (state 0A) 57% of the running time (1096 of 1919 polls at 20 Hz; 64% before) and started 0.2-3.3 s after Snake
    did; the rest it glides beside him (MGSV's frames: beside him on the left at every shot, legs walking once its order is
    taken). **The player's orders:** the call menu's Search Mode (`the player's buddy command 25 -> auto-follow paused`, `mode
    normal -> search`) -> an 8 s run: 0 lead frames, the walker 18-42 m away searching; Whistle (20) -> `auto-follow resumed`, led
    again (median 2.80 m). **Riding it:** Y beside it -> `OnVehicle`, 5 s ridden 36 m: 0 lead frames; Y off and a run: led again,
    no warning in the log. Offline: `TestWalkerLead` (`tests/test_guest_horse.cpp`: a minute of runs, walks, turns and stops with the
    treadmill and an AI of its own: median 2.80 m, no frame > 0.12 m as Elden Ring sees it, no lead lost).
  - **With both real games** (2026-11-01, Gatefront at night, `pr status` at ~8 Hz): 127 samples of a 22 s run (MGSV's Snake at
    4.7 m/s) -- D-Walker **median 2.81 m, p90 2.81, max 2.82** from him; standing after it 2.4-2.6 m; a short run out of the Church of
    Elleh's grace (the walker starting 5.6 m away): median 2.79, p90 3.43, max 4.28 while it closed in. Frames of a 27 m run west on
    Gatefront's path: the walker beside Snake on his left in every one, its legs walking. `walker: ... treadmill on (140 moved, 0
    failed); lead on ...: 30 leads, 21977 frames, 30 stopped, 0 lost, largest step 0.265 m`. Seen once: led to Snake's left, the
    walker stood between him and a rail fence he was walking at (he stopped against it).
- **Never faster than its legs (2026-11-03, `WalkerLogic.h` leg gate, `Walker.cpp`, `Follow.cpp`; `[Buddy] WalkerLeadLegs`,
  `WalkerLeadLegSpeed`, `WalkerLeadIdleSpeed`, `WalkerLeadAccel`, `WalkerLeadOrderAhead`, `WalkerLeadReorderMs`; `!walker legs
  on|off`, `!walker orderahead <m>`, `!walker reorder <ms>`):** README's "glides at the start of a run". The lead moved the walker
  from Snake's first step while its AI (record `+0x56`) still stood: a glide until its legs walked. Now:
  - **Measured** for every lead (`walker: lead #N: 10.5 s led, its legs walking 2.45 s after the start (AI state 0x5 at the start);
    glided 4.77 s / 13.8 m, 2.39 s of it before they walked (legs gate off)`; totals and the legs' delays in `!walker`): a frame is a
    glide when the lead moved it > 0.3 m/s with its AI's state known and not walking (0A) or stopping (0C).
  - **The gate**: while its AI doesn't walk the lead moves it at most `WalkerLeadIdleSpeed` (0: it stays), walking at most
    `WalkerLeadLegSpeed` (6.5 m/s, MGSV's walker's own pace; the lead's 9 m/s before), its speed changing by at most `WalkerLeadAccel`
    (8 m/s per s: no jerk when the legs start). Unknown legs (Follow hasn't read the record in 0.5 s): as before.
  - **Its own walk sooner**: its AI took the order given as Snake starts within 25 ms and stayed idle (polled through
    /proc/<pid>/mem at 40 Hz: `+0x5C` cleared, `+0x56` still 05), and walked only with the next one, given when it had stood 0.8 s and
    1.5 s after the first (taken ~0.5 s later: 09 turning, 0A walking 20 ms after). The orders while led (as he starts, when its AI
    stood) now go `WalkerLeadOrderAhead` (8 m) ahead of him in the lead's lane, the steering keeps that spot ahead while he moves (its AI
    always has somewhere to walk: no glide in the middle of a run), and the AI is ordered again after `WalkerLeadReorderMs` (500 ms)
    of standing. The first step after a run's start did not come sooner (below): MGSV's walker answers ~2.2 s after a stand of
    3-4 s whatever the orders; why is open (a cooldown of its AI's after it arrives?).
  - **Live (MGSV + fake_er, the same 50 s pad script each time: stand 3 s, run 10 s, stand 4 s, run 6 s turning, stand 4 s, run 3 s,
    stand 3 s, run 8 s turning back, stand 5 s; distances from the fake_er trace while Snake runs > 3 m/s):**

    | | legs walking after the start (4 leads) | glided (time / distance, the 4 leads) | running median / p90 / max | first 3 s of a run: median / max |
    |---|---|---|---|---|
    | 0.6.0 (gate off, orders as before) | 2.44, 2.47, 2.18, 1.35 s (and 2.45, 2.42, 2.40, 0.47 s) | 13.4 s / 38.5 m (13.9 s / 41.0 m) | 2.80 / 2.80 / 2.91 m | 2.80 / 2.91 m |
    | orders ahead, gate off | 3.54, 2.53, 2.25, 0.62 s | 8.9 s / 26.3 m (only before the legs first walk) | 2.80 / 2.80 / 2.91 m | 2.80 / 2.91 m |
    | gate on, orders as before | 3.56, 2.45, 2.37, 1.63 s | **0.0 s / 0.0 m** | 4.29 / 8.55 / 10.83 m | 4.77 / 10.64 m |
    | **gate on, orders ahead, reorder 500 (default)** | 2.26, 2.12, 2.49, 0.32 s | **0.0 s / 0.0 m** | **2.83** / 6.77 / 8.36 m | 4.68 / 8.36 m |

    So the walker never glides now: at a run's start it stands with its legs until its AI walks (~2 s after a stand), then catches up
    at its own 6.5 m/s and runs beside him (the running median is 0.6.0's); the price is the first seconds of a run, where it is up to
    ~8 m behind him instead of gliding beside him. `WalkerLeadLegs = false` is 0.6.0's glide. Offline: `TestWalkerLegs`
    (`tests/test_guest_horse.cpp`: legs standing 1 s, then walking: never moved while standing, never faster than 6.5 m/s or 8 m/s
    per s, catching up; unknown legs and the gate off as before).
  - **Far behind** (review 0.7.0): legSpeed (6.5 m/s) alone left the walker ever further behind a dashing Snake (~7 m/s), and nothing
    let go of it while led. Now a walker more than `WalkerLeadAcquire` (6 m) from its spot whose legs walk catches up at
    `WalkerLeadMaxSpeed` (9 m/s, still at most 8 m/s per s faster each second), and one whose legs stand that far behind for 4 s
    (`farReleaseMs`) is let go: `walker: the lead let D-Walker go -- N m behind its spot, legs standing 4000 ms: Follow's catch-up takes
    it`. Offline (`TestWalkerLegs`): a 12 s dash at 7 m/s with the legs standing 2.2 s -- behind more than 6 m, then back to about
    6 m and held there, never over 9 m/s, never a glide; legs that never walk -> let go. Live with both games (0.7.0 integration,
    Stormveil's rooms and courtyards, short runs): every lead `glided 0.00 s / 0.0 m (legs gate on)`, legs walking 0.00-2.44 s after
    the start.
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
  (every buddy DISMISS only with `[Menu] BuddyDismiss = false`, 8.3): HELICOPTER, MISSION LIST and SIDE OP LIST show struck out (screenshot). Still there and harmless:
  Supply Drop (weapons, ammo), Buddy Support (other buddy commands), Fire Support, Combat Deployment, Challenge Tasks,
  Cassette Tapes, Log, the map, Mother Base management. A fire-support barrage's blasts carry attack ids the guest
  ignores.
- Console: `!menu` (also the iDroid requests below).
- **The iDroid while Snake is busy** (`[Menu] IDroidRetryMs = 3000`, `logic::IDroidRequest` in Pad.cpp, tested; live 2026-10-14).
  The host opens the iDroid by holding MGSV's Start (View held -> `MGSV Start held 1200 ms`, ended early once MGSV's menu is up
  since 0.9.0, so an iDroid closed within the hold isn't reopened by its rest; HOST.md, Control hand-over). In MGSV a Start it takes opens the
  iDroid 40-70 ms after the press (measured in idle, running, crouched, prone, mid-reload: `kGuestInMenu` within 70 ms, the
  iDroid on screen), but it **refuses** Start while Snake **aims (LT held)**, looks through the **binoculars (RB held)** or is
  **in a dive (X while running)**; then nothing opens and the press is lost (before: the 1200 ms hold just ended). Now the guest
  keeps such a request: a Start from the host while Snake plays (ready, in game, not dead, not parked, no MGSV menu) that hasn't
  opened anything after 250 ms gets a neutral pad for 120 ms (that ends the aim / the binoculars) and then Start alone for
  200 ms, again every ~570 ms until the iDroid opens or 3 s pass (`pad: MGSV didn't open the iDroid on Start -- Snake is busy
  (aiming (LT)); pressing it again with the pad neutral`, `pad: the iDroid opened 400 ms after Start (1 presses again ...)`).
  Live: aiming 434 ms, binoculars 445 ms, dive 418 ms after the Start, one extra press each; idle unchanged (41 ms, no extra
  press). A Start inside an MGSV menu or while Snake isn't playing is never a request; a new one needs Start released first.
  0 = off. Not tried: CQC holds, climbing, a ladder, riding (D-Walker/D-Horse) -- the same path covers them if MGSV takes the
  neutral-pad press. The player's "View went down a bit before Start" chord problem is the host's (`er/Chord.h`): it cuts
  its 1200 ms hold when Start follows View within 700 ms, and a host Start that goes up within `kCutMs` (600 ms) of the request,
  before the iDroid opened, ends it (`kCut`, `pad: the host cut its iDroid hold (View+Start chord) -- no more Start presses`):
  the retries no longer open the iDroid as Tarnished mode begins (an aiming Snake; test_guest_logic). Assumes the host's
  `guest_idroid_start_ms` >= 600.

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
  377-379, stun 372-375/1539-1541 + StunDarkMatter, smoke 357-361 and flares); magazines and decoys are logged once and skipped
  here (a thrown decoy is expected to land in the Placed System: see the decoys below).
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
  From a death (HostState kHostPlayerDead) until the puppet runs again with nobody dying every new one is dropped
  (`logic::AfterDeath`): since the review of 0.6.0 also when the puppet ran on without a new start -- a kill Elden Ring refused
  (no dead bit: the host gives up after three tries and sends the respawn) used to leave the guest "after the death" for good,
  dropping every later mine, C4 and decoy (`placed: the death is over and the puppet ran on`; tested in `test_guest_logic`).
- **Decoys** (protocol v13, `[Combat] Decoys`; live pass B 2026-10-15: MGSV throws them -- LT, RT -- and mirrors each into
  `placedWeapons` as it lands, HOST.md "Decoys"): `logic::ExplosiveOf` calls the decoys (397-399, active
  401-403, shock 405-407 / 380-381) `ExplosiveUse::kDecoy` (blast kind kIgnore: no blast is ever matched to one). Found in
  the vars mirror they go out as `kGEvPlaced` with a = `kAmmoDecoy`, trigger 0, `v1` its facing, and gone as
  `kGEvPlacedGone`, like a mine (UNVERIFIED that MGSV mirrors a set decoy into `placedWeapons`: the Placed System holds the
  map's own decoys, so it is expected). A `kHEvDetonate` for a decoy means an Elden Ring enemy knocked it over there
  (er/Decoys.h), or the host dropped it (set before a load, unknown to that host): the guest sends `kGEvPlacedGone` at once and never announces it again (`popped`); in MGSV it stays unless
  `DecoyPopInMgsv = true` also sets its "set off" bit (UNVERIFIED what MGSV's decoy views do with it). `placed #N: equip E
  (decoy)` is logged at each.
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
  The C4 detonator button wasn't found live (RT, RB, B, X, Y, stick clicks, D-pad with C4 in hand: nothing; B next to a C4
  picks it up -> kGEvPlacedGone). Since 18.8a: it is ACTION (Y), and the guest answers a press MGSV doesn't.
- **Mines MGSV sets off itself stay out of Elden Ring:** MGSV's claymore answers MGSV's own characters -- D-Walker, whose
  follow spot is ahead-left of Snake, stands in its cone: live, 3 of 3 claymores placed next to a following walker went off
  0-1.7 s later in MGSV. A mine's blast (attack 202-211/451, and the clouds 284-287 of a mine) is sent only when the guest
  set it off for kHEvDetonate; others are logged `a mine MGSV set off itself (placed #N) ... kept out` and counted in
  `!blast`. The mine is used up in MGSV all the same (kGEvPlacedGone). C4 is the player's: MGSV's remote detonation sets
  byte-0 bit 3 on every C4 and `0x140FC7E10` turns them into bit 2 one by one every 0.05 s; the guest never touches that
  (the detonator button wasn't found: RT, RB and B unaimed with C4 in hand did nothing).
- Console: `!throw`, `!placed`, `!detonate <id>`; fake_er: `pr er placed`, `pr er detonate <id>|all`; its `--trace` gained the
  aim columns (flag, pos xyz, vel xyz, kind).

### 18.8a C4's detonator (`C4.cpp`, `C4Logic.cpp`, `Placed.cpp`; `[Combat] C4Detonator`, `C4Backup`; live pass A 2026-10-14)

**Live pass A** (docs/FEATURES-1.md): MGSV's own detonator answered **0 of 5** Y taps with C4 in hand (with and without D-Walker
near): step 3's `c4: a tap with C4 in hand got no detonate request from MGSV in 250 ms`, PlayerDetonate never ran (no step-2
line). With `C4Backup = true` each tap set off the oldest C4 in Elden Ring (`placed #4: C4 set off by the guest (MGSV's
detonator didn't answer the press ...)` -> the host's `mines: mine 4 went off ... lethal r 8.0 m where it lies`, then #5);
a Y tap at D-Walker with C4 in hand mounted it and set nothing off, and Y to dismount set nothing off either. So
**`C4Backup` is on by default** since then; the supply-box pickup case is untested.

Roadmap 5.8: C4 could be placed (kGEvPlaced, trigger 0) but never went off -- the detonator button wasn't found (18.8). The
chain, read from the exe on disk (capstone; every site below byte-checked in Addresses.h "C4's detonator" and by
`verify_addresses.py`):

- **Who sets it off.** DetonateOwn, the Placed System interface's slot 13 (`0x140FC54F0`, vtable `0x1423A9FD0` at system+0x20):
  `(player & 0x1FF, equip)` -> the player's live object of that equip with the lowest `+0xD8` order (the oldest) ->
  `SetOff(system, player, object)` (`0x140FC8E50`, bit 2), so one C4 per press, returning to `0x140FC561E`. Its only caller
  is PlayerDetonate (`0x141053270`, `call [rax+0x68]` on `[[hub+0x98]+0x208]`).
- **PlayerDetonate(ctx, status, player)** runs from the player's action states (`0x141068A8C`, `0x14107F2D9`,
  `0x14107F77B`, `0x14108A46B`, tail jump `0x141069061`) while status `+0x3CC` bit 19 is set. It needs status `+0x3C0` bit 0
  and `+0x3CC` bit 25 (set by `0x141096C80` when the system's slot 10 says a placed one of the equip in hand exists); then it
  calls DetonateOwn with the equip in hand (`+0x24C`) and sends message `0x61FE32CC`, else message `0x2A805080` (refused).
  Both clear bit 19.
- **The press.** The support-item input (`0x141095DF0`, per frame; equip use type `+0x27C` bits 6-8, status `+0x1B0` = the
  item's parameters) on a placed item (type 2) with the remote flag (status `+0x280` bit 23, C4): an **ACTION edge** (pad
  object vfunc `+0x40` bit 5 = PlayerPad.ACTION, **Y** by default: the D-Walker mount button) sets bit 19
  (`0x141096796`; the type-3 path `0x1410968DF` too) -- when `+0x3C0` bit 18 or 19 is set and bit 26 isn't, `+0x3CC` bit 7
  isn't, and no other action holds the item (`0x140FF3E90` on 0xC2/0xC3). FIRE (RT) in the same handler places it.
- **Not the detonator:** byte-0 bit 3 (the "chain" queue `0x140FC7E10` turns into bit 2 every 0.05 s) is set by
  `0x140FCDA40` for an object caught by a blast (slot 14 `0x140FC5630`, any live object within a radius) or hit
  (`0x140FCC9B0`); online it goes out as message 6. 18.8's "MGSV's remote detonation sets bit 3" was this chain.
- **Why Y did nothing live (18.8)** is not settled by the static RE: one of the conditions above fails in the sandbox
  (status bits whose meaning isn't known). So the guest watches all three steps:
  1. Placed.cpp's SetOff detour (installed for the mines) sees MGSV set off one of Snake's C4: logged
     `placed #N: C4 (object O, equip E) set off by MGSV's detonator (DetonateOwn)` (the return address tells the detonator
     from the sensor/update), the C4 is marked as set off by us: its blast is Elden Ring's where the host's copy lies, and
     MGSV's own blast is skipped with `[Combat] MgsvBlasts = false` (the C4 lies in the treadmill's disc next to D-Walker).
     Only a C4 that reached Elden Ring counts: DetonateOwn takes the oldest, so an old save's C4 (`preexisting`) goes first --
     that one is logged `... an old save's: MGSV's world, not the player's answer`, stays MGSV's (its blast too), and step 2
     sets the player's oldest off for that press. With `C4DetonateAll` the guest sets off the rest right after MGSV's one.
  2. PlayerDetonate detour: a request (bit 19) with C4 in hand that set nothing off is logged with the status bits it needs
     (`c4: MGSV's detonator refused the press (player P status +0x3C0 ... (bit 0 B) +0x3CC ... (bit 25 B) equip E) -> 1 set
     off by the guest`) and answered at once.
  3. A tap MGSV never answers (`PlayerVars.scannedButtonsDirect` & `C4Buttons`, rising edge, C4 391-395 in hand =
     `vars.currentInventorySlot` 3, on foot, no menu, a live C4 of ours; released within `kHoldMs` 400 -- a hold is Fulton's
     or carrying's) within `C4GraceMs` (250), or at its release if later: with `[Combat] C4Backup = true` (the default since
     live pass A) `c4: the detonator press got no answer from MGSV in 250 ms -> 1 set off by the guest`; off only logged
     `c4: a tap with C4 in hand got no detonate request from MGSV ... nothing set off`. It was off at first because MGSV's
     ACTION is also the supply-box pickup and D-Walker's mount, which never make a request (live: a mount takes the C4 out of
     hand within the grace, and nothing went off; the supply box is untested; the setter's own conditions, `+0x3C0` bits
     18/19/26, `+0x3CC` bit 7, would tell them apart once their meaning is known).
  4. MGSV's own request coming for a press the guest already answered at step 3 (within 2 s, no newer press -- e.g. if its
     action state plays a detonator animation first) is dropped (bit 19 cleared, PlayerDetonate not run): no second C4;
     logged `c4: MGSV's detonator came ~N ms after the press ...` -- a `C4GraceMs` above N lets MGSV's own answer instead.
  The guest's set-off is kHEvDetonate's (byte 0 bit 2, `placed::SetOffC4`), the oldest live C4 of the grade in hand first
  (`c4::PickOldest`; `C4DetonateAll` = every one). A press answered by MGSV earlier in the same frame (its player update can
  run before Frame.cpp's) still counts as answered (`kRecentMs` 150). Unit-tested: `tests/test_guest_c4.cpp`.
- A refused set-off (`placed::SetOffC4`: the system pointer, the live bit not set yet) skips that C4 for that press only.
- `!placed` ends with the detonator's state: `c4: in hand E, presses N (answered A, fallbacks F[ -- backup off: U only
  logged], holds H), MGSV's detonator D / other set-offs O, refused requests R (last: ...), set off by the guest G`.

### 18.8b Placed explosives when the Tarnished dies (`Placed.cpp`, `[Combat] PlacedClearOnDeath = true`; live 2026-10-31, MGSV + fake_er)

README Known issues: C4 placed in the frame Snake dies reappeared at the grace after the respawn. MGSV put it down (its vars
entry appeared) after the host had stopped puppeting, so it wasn't announced then; the next puppet start announced it for the
first time and the host mapped MGSV's point through its new anchor -- next to the grace. Now, from HostState
`kHostPlayerDead` until the puppet starts again after it, every one of Snake's placed explosives is **dropped**: kGEvPlacedGone
if it was announced (the host forgets its copies at the death anyway: its world reloads), never announced again, never set off
for the player (C4's detonator and kHEvDetonate skip it, a blast matched to it is MGSV's world: not sent), and removed from
MGSV's own list through MGSV's removal (the Placed System interface's slot 5, 0x140FC7A20, its offline path -> Remove(system,
index, 1, 0): live bit, the order record and `placedWeapons[i]` cleared, its view released; byte-checked, `Addresses.h`
kPlacedRemove*). One put down while he is dead is dropped the same way (`put down as the Tarnished died: never sent`).
Live (`pr er die` / `respawn`): a sleep mine placed, then `die`: `placed #2: equip 418 dropped (the Tarnished died) ->
kGEvPlacedGone; in MGSV removed`, `vars.placedWeapons[0]` 418 -> 0; a mine placed with `die` 0.1 s after RT: `placed #3 ...
dropped (put down as the Tarnished died: never sent to Elden Ring) (never announced); in MGSV removed`; after `respawn`
(`the puppet started again after the death`) no kGEvPlaced for it (fake_er trace: one Placed and one PlacedGone, both #2).

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

### 18.11 Ammo refill on rest and respawn (`Ammo.cpp`, protocol v10 `kHEvRested`; live 2026-10-13, MGSV + fake_er)

Elden Ring refills the flasks when the player rests at a grace or respawns; the host sends `kHEvRested` (a = 0 rest, 1
respawn) and the guest gives Snake everything back: life to full (unless he lies dead -- `kHEvRespawn` ends that), every
weapon's magazine, every ammo stock, every support item (grenades, mines) and item count (NVG battery, ...) to its maximum.

- **Where MGSV keeps ammo.** `vars.ammoInWeapons` / `vars.ammoStockCounts` (and a static copy at `0x142A5D110`+0xD8 that the
  HUD's weapon box shows) are copies: a Lua write to vars is overwritten at the next shot (VERIFIED: 7/21 written, one shot
  later 4/18). The authoritative counts are in the player's ammo component, the one `Player.GetAmmoStockBySlot` (Lua C
  function `0x1409D1BB0`) reads: Player2System impl (`[0x142A82C28+8]`) +0xC8 -> +8, vtable `0x1423895D0` (found by a memory
  scan for the magazine, then the Lua binding's code). Its layout and own setters (Addresses.h `kAmmo*`, byte-checked):
  magazines u16 at `[+0x50] + (player * [+0xC] + slot) * 6` (+2 the sub magazine), set by vf 4 `SetMagazine(this, player,
  slot, rounds, sub)` (`0x140EE96C0`, which also notifies the component's listener); 23 stocks with ids u8 `[+0x90]`, counts
  u16 `[+0x98]`, maxima u16 `[+0xA0]`, count = min(value, max) by vf 15 `SetCount(this, index, -, count)` (`0x140EE9730`).
  Stock indices: 0/2/4 the weapon slots (1/3 their sub ammo), 5 the hand (bionic arm), 6-13 the 8 support items, 14-21 the
  items. A stock count is every round left, the magazine's included (it drops with each shot: SVG-76 full = 210 = 30 + 180).
- **Refill:** the frame thread queues the Lua `refill` command; `PR.Magazines()` answers each weapon slot's magazine and sub
  magazine from `TppEquip.GetAmmoInfo` (as MGSV's own `TppPlayer.SupplyWeaponAmmoFull` reads it); on the Lua thread right
  after that Tick, `Ammo.cpp` calls `SetMagazine` for the three slots and `SetCount(max)` for every stock below its maximum,
  then writes the vars copies and the HUD box's copy (only where it still shows the old count), so GuestState and MGSV's HUD
  agree at once (MGSV itself refreshes them at the next shot or weapon change). Failures (vtable, a setter moved, no
  component) refill nothing and log why. `TppPlayer.SupplyAllAmmoFullOnMissionFinalize` was not usable: it fills the gvars
  for the next mission start. `Player.ChangeEquip` with the same weapon and full counts broke the weapon (Snake could no
  longer raise the pistol; HUD "7/∞") -- never used for weapons.
- **Live (fake_er `rested 0|1`):** `ammo: refill (rest: magazines 36:24->30 54:1->1 7:5->7; stocks #0(bullet 15) 204->210
  #4(bullet 10) 19->21 #15(bullet 202) 7->8; HUD box 2 slot(s))` 15 ms after the event; vars and GuestState `ammoInClip` /
  `ammoStock` 5/19 -> 7/21 in the next frame (fake_er trace); the weapon box went from "5/∞" to "7/∞" (screenshots). Respawn
  (`die`, `rested 1`, `respawn`): magazines and stocks refilled, life from `kHEvRespawn` (7801). Life on a rest: 4064 ->
  7801 in the frame after the event. After `!ammo drain` (every stock halved) a rest put all 11 back: grenades 4 -> 8, the
  rocket 4 -> 8, AR 105 -> 210, ... This save never uses up grenades or reserve rounds (HUD "∞", an Infinity Bandana-like
  setting: two thrown sleep grenades left the count at 8), so the drain is the way to see a support refill here.

### 18.12 Night vision (`kGuestNvg`, Hud.cpp; live 2026-10-13, MGSV + fake_er/recorder)

Player report: "MGSV's night-vision goggles do nothing in ER and make everything look darker".

- **Detection:** `vars.currentItemIndex` is the item slot Snake uses (0 = none; the box's slot while he is in it), so the
  goggles are on when `vars.items[vars.currentItemIndex] == TppEquip.EQP_IT_Nvg` (492). Found by a memory diff over four
  toggles (vars+0x67C, registered as `currentItemIndex`: 3 on, 0 off; no PlayerStatus bit changes). `PR.Flags` sets `NVG`,
  the frame publishes `kGuestNvg`. Toggle in MGSV: a short tap of D-pad left with the goggles as the current item (taps of 150
  ms or more open the item menu instead).
- **HUD layer:** with the goggles on the layer was 100% covered (MGSV's noise and vignette). Leaving out one UI pixel shader at a
  time (`!capture hud skip`): `0xae282a7fe2bdbdd9` (a full-screen quad drawn inside a UI job; its textures are
  `g_ReflectionTexture`, `g_tex_mesh`: a filter, not a HUD element) brought it to 4.6% (weapon box, NVG item box, prompts). It is
  left out while the goggles are on (`hud::SetNightVision` from the frame's flags) and kept otherwise; `!capture hud` counts
  the draws left out. Layer coverage now: 2.2% off, 4.1-5.4% on (notifications, the item box with its battery bar).
- MGSV's own picture still turns green and noisy (the host never shows it); the host recreates night vision from `kGuestNvg`.
- **The binoculars' lens** (live 2026-10-14, the same method): with RB held MGSV's picture showed around the lens in Elden
  Ring's frame (the airport's concrete in the corners). `!capture probe 1` with them up: the one UI draw with a screen texture is
  pixel shader `0x2f38cdb182e4da4f` (a mesh ring, idx 216; t0 `inScreenTexture` 1280x720, t1 `inNormalTexture`: the lens'
  refraction of MGSV's own picture). Left out (`!capture hud skip 2f38cdb182e4da4f`) the binoculars' HUD and dark vignette
  stay and MGSV's world is gone; it is not drawn at all in third person. It is now always left out (`kLensScreenPs`;
  `!capture hud`: `lens draws of MGSV's picture left out N`). The scopes probably use it too (untested).
- Recordings for the host: `nvg2.prrec` (goggles on t = 3.5-10.6 s and 14.7-19.8 s, a crouch-walk and a pan with them on).

### 18.13 MGSV's auto-aim off (`[Boot] AutoAim = false`, `PR.AutoAim`; live 2026-10-13)

Known issue: aiming (LT) swung Snake's aim onto the disabled MGSV soldiers that still stand around the airport (live: a
rocket launcher's aim 30 deg off an Elden Ring target); `ClearMarkersSec` didn't help.

- Reproduced: with the FB MR launcher, a camera yaw anywhere from -2 to 25 deg turned to exactly 12.57 deg when LT was pressed
  (a disabled soldier at the guard post, screenshot); the pistol didn't snap. Moving or "vanishing" disabled soldiers didn't work
  (`Warp`, `RequestVanish`, `Unrealize` left them where they stood; `SetEnabled` true + `Warp` moved them 4 m).
- It is MGSV's option OPTIONS > Control Settings > **Auto-Aim Correction** ("readying a weapon will make you aim automatically
  at enemies"). Applying it in the menu changed one byte of `.data2`, `0x142C14509` = `vars.optionSelectedIndices[11]` (the
  registration table names the array; 0 ON, 1 OFF); the game reads it directly. The menu's own apply tried to save the config:
  the save guard dropped `TPP_CONFIG_DATA` (Steam) and redirected `TPP_CONFIG_DATA0/1` to the sink; the option was set back
  to ON in the menu afterwards.
- The sandbox setup writes `vars.optionSelectedIndices[11] = 1` (`boot: auto-aim correction: off (was on)`), only while
  MGSV's saves are blocked, so it is never saved: `[Saves] Block` *and* the file guards installed *and* the Steam Cloud swap
  complete (`hidden::SavesBlocked() && saves::SteamGuarded()`; review 2026-10-13 -- with either guard incomplete the guest
  carries on with saves live, and the write is skipped: `left as it is (the save guards are incomplete ...)`, checked by
  test_guest_pump). Live after a boot: aiming the launcher at -1.8, 3, 8, 12.5, 17 and 25 deg
  kept those yaws exactly (trace camera yaw); with the option set back on, 8 deg swung to 18.0 deg. `AutoAim = true` keeps
  MGSV's setting.

### 18.14 Tarnished mode (protocol v10 `kHEvTarnished`; live 2026-10-13, MGSV + fake_er)

The player can take the Tarnished (flasks, spirit ashes, Torrent): the host stops the puppet, sends a neutral pad and
`kHEvTarnished` a = 1; Snake parks where he stands until a = 0.

- Parked: the pad is neutral whatever the host sends (`pad::SetParked`, on top of the death block), no treadmill warps
  (`state::Parked()` in Frame.cpp's hold), no follow whistles, steering or catch-up for D-Walker (Follow.cpp). Death,
  hurt/heal, rested and the capture go on as usual. A new host session unparks him.
- Live: 4 s of full stick forward plus A, LT+RT while parked: root unchanged (820.60, 1179.10), no shot; after a = 0 the
  same stick moved him at once. A 4-minute park with the pad cycling every 10 s through full stick, LT/RT, Y+X and the
  right stick: Snake at (820.60, 1179.10) at all 24 samples, alert phase 0, life full, D-Walker standing at (821.81, -11.90,
  ...) 0.5 m from its spot, 0 whistles, 0 treadmill warps, MGSV's frames and poses still flowing (frame 17040); a = 0 and
  4 s of stick: 6 m walked.
- fake_er: `pr er tarnished 1|0` (it also drops `kHostPuppeting` meanwhile, like the real host).

### 18.15 Regeneration after Elden Ring's hits (`[Combat] RegenGate`; live 2026-10-13, MGSV + fake_er)

Player report (Margit): Snake regenerated ~760 life/s right after each Elden Ring hit and survived 18 hits in 70 s
standing still: MGSV never saw those hits, so the pause it takes after its own never started.

- **MGSV's own curve, measured:** S_DISABLE_PLAYER_DAMAGE reset, Snake warped 11.5 m up (Lua `Warp`): the landing took
  7801 -> 2651 (a 15 m fall and a 22 m fall killed him: MGSV game over and its continue; 8 m: nothing). Life sampled at 10 Hz
  from /proc/<pid>/mem: held at 2651 for 2.4 s, then +17, +27, +37 ... per 0.3 s up to +225 per 0.3 s (~750/s) about 6.3 s
  into it, then steady: full 12.5 s after the hit. Fitted: life = base + 60·s² for the first 6.25 s of regeneration (s =
  time since the 2.4 s pause), then +750/s (`logic::RegenCap`, unit-tested against these numbers).
- **Where MGSV keeps it:** the hit shows in the player's life block (`Player2System impl -> [+0x380]`: f32 life `+0x60`, max
  `+0x64`, a 0.13 s hit flash `+0x70`, the last damage `+0x74` = 5150) and a damage-reaction record (a 2.25 s timer, attack
  id 103); the regeneration's own timer/ramp state wasn't found in the player's objects (depth 1 from the impl and its
  instance, and 256 KB around both: memory diffs over two falls). A page-guard write watch on the native life
  (`0x142C0147C`, PAGE_WRITECOPY .data2) crashed MGSV (access violation while single-stepping): never watch that page.
  So the guest **emulates** the curve instead of triggering MGSV's state: after every kHEvHurt it caps life with
  `RegenCap(post-hit life, time since the hit)` and writes it back down (the usual native+vars write) whenever MGSV
  regenerated above it; a new hit starts it again from its own post-hit life; a heal (kHEvHeal) raises the base by what
  it gave; kHEvRested/kHEvRespawn end it. MGSV's blood/screen damage effects stay the `HurtFeedback` ones (camera shake,
  blood on the model).
- **Live, fake_er `hurt 1000` (-4000 life):** gate off (`!regen off`): 3801 and regenerating at once, ~750/s, full 5.3 s
  later. Gate on: 3801 held 2.4 s (+13: one frame of MGSV's own regeneration before each write lands), then 3830, 3877,
  3945, 4054, 4192 ... per 0.5 s, full 10.6 s after the hit -- MGSV's own curve from that life (its fall: 12.5 s from
  2651). `!status` frame line: `life ... (regen gate from 3801, 3.1 s, N capped)`.
- **Live with Elden Ring (2026-10-13 morning, combined run, the real MGSV):** a 9.8 m fall at the Castleward Tunnel grace
  (`landed at 13.8 m/s -> fall damage 18%`, kHEvHurt 354): 7801 -> 6385, held (6385-6398) until ~3.3 s after the hit, then
  6441, 6527, 6638, 6776, 6950, 7181, 7427, 7709 per 0.5 s, full **7.25 s** after the hit; the same fall with `!regen off`:
  regenerating 0.13 s after it, full **2.0 s** after it. **Margit**, Snake standing still inside his arena: gate on -- 5 hits
  (15/15/33/29/16%) in 12 s and Snake died (a second try: dead in ~17 s); gate off -- 22 hits in 75 s and still alive,
  regenerating 0.1-0.17 s after every hit (the player's "18 hits in 70 s"). Between a hit and the cap's first write life may
  rise 50-140 (MGSV's own regeneration while the hit's write is pending: `RegenGate` skips while `vars::PendingLife()`), then
  goes back to the curve.
- `[Combat] RegenDelaySec = 2.4`, `RegenRampSec = 6.25`, `RegenRate = 750` tune it; `RegenGate = false` restores the old
  behaviour.

### 18.16 Phantom Cigar = Elden Ring time skip (`Cigar.cpp`, `[Cigar]`, protocol v13 `kGEvTimeSkip`; live pass B 2026-10-15, on by default)

Roadmap 2.7. MGSV's Phantom Cigar (`EQP_IT_TimeCigarette`, equip id 451) is a time-lapse: Snake smokes, MGSV's day races
by. Live pass B (2026-10-15): `cigar: Snake lights the Phantom Cigar (MGSV's clock 12:00:00, vars.clock 43200)`, the probe's
`stood still 500 ms into the smoke: it runs at ClockScale x20` (it stood still: MGSV's own time-lapse takes over ~2.5 s in,
1400-1500 game seconds per 0.4 s, after the lighting animation), `put out -- 12:00:00 -> 17:48:17: +348 min` and `17:48:17 ->
01:05:24: +437 min` (`!cigar`: 2 smokes, 54 events, 785 min sent, 0 lost); Elden Ring followed (HOST.md). `vars.clock` stayed
put after each smoke. The `GameStatus` flag and the mute both read true while he smoked. The player's save carried no cigar, so
with `TimeSkip` on the boot puts one into the last free item slot (`PR.CigarItem`, `boot: phantom cigar: put into item slot 7`;
not slot 0, a fresh load's current item) -- only while MGSV can't save it (`SavesGuarded()` in `Pump.cpp`: `[Saves] Block`, the
file guards and Steam's storage swapped, as for `[Boot] AutoAim`; review 2026-10-31). With the guards incomplete the log says
`boot: phantom cigar: left as it is (...) -- put it into an item slot yourself`, and the box and its poster (§18.9) are left out
the same way (`test_guest_pump`). The `[Loadout]` weapons are written either way: Snake needs them, and the sortie loadout is
the player's to change back. It is smoked from MGSV's item wheel: D-pad left held, the right stick on it, R3 (Use)
while still pointing -- needs `[Jump] PassWith` to include the D-pad (§6). A tap of D-pad left puts it out. The sandbox's clock is set and frozen at boot (`[Boot] Clock = 12:00:00`, `FreezeClock`: `SetClockTimeScale(0)`).
`[Cigar] TimeSkip = false` ships: the smoking signal is unverified and the effect lands in Elden Ring's saved time of day
(night-only spawns), so it is not purely additive. Turn it on for the live test.
- **Smoking** (`PR.CigarSmoking`, Lua flag `CIGAR` = `state::kLCigar`, bit 28 so it stays clear of new stance bits): MGSV's
  game status flag `S_IS_TIME_CIGARETTE` (`TppGameStatusFlag` 0x15 -- found in the enum registration `0x14053F410`,
  `TppGameStatus.IsSet('', ...)`), or its `Cigarette` sound mute (`CheckCurrentMuteIs` / `CheckHighestMuteIs`, the mute
  table of §18.2). Which of the two MGSV keeps up for the whole smoke is UNVERIFIED; either counts. Related, not used:
  `Player.SetTimeCigaretteStopTime(t)` (`0x1409DFEE0`) stores a float at `0x142C1C214` (the time the cigar stops at);
  `CIGAR_START` / `CIGAR_END` are registered at `0x141FE8C50` / `0x141FE9B80` (a message or UI id: not traced further).
- **The clock** (`phantom::timeskip::ClockGovernor` in `common/TimeSkipLogic.h`, driven from `cigar::Tick`; the Lua acts on
  its word, `PR.Run('cigar lit|run|out')` -> `PR.CigarClock`). Whether MGSV's cigar sets the clock's speed itself (e.g. from
  its `CIGAR_START` handler) or needs a running clock is UNVERIFIED, so nothing is set blindly: when the signal rises the
  guest notes `vars.clock`; if 500 ms later it has not moved, the frozen clock runs at `[Cigar] ClockScale` (20, MGSV's own
  speed; `SetClockTimeScale`'s cfunc is `0x1405364E0`), else MGSV's own time-lapse is left to run. When he stops it is frozen
  again, and while he doesn't smoke `PR.CigarHold` re-freezes it every 60 Ticks (~1 s) in the loaded sandbox, whenever MGSV
  puts its own speed back. With `FreezeClock = false` the clock is left alone. A "smoke" longer than 120 s (measured in ms:
  `PhantomRing.Tick` runs at MGSV's frame rate, ~60 Hz, so ticks are no clock) is a stuck signal (or one that means something
  else): the clock is frozen again and the signal ignored until it drops (`!cigar` counts them, and says which smokes MGSV's
  clock moved by itself and which the guest ran).
- **The minutes** (`cigar::Tick`, MGSV's Lua thread after every Tick, ~60 Hz, ~30 Hz while frames stall): MGSV's clock is
  `vars.clock` (seconds of the day, read natively, Vars.cpp; 43200 after the boot's 12:00:00). `phantom::timeskip::Tracker`
  (`common/TimeSkipLogic.h`, unit-tested in `tests/test_host_timeskip.inc`) counts the forward steps (round midnight) while
  he smokes, the step just before the flag rose, and 1.5 s after it dropped; steps over 6 h (a `SetTime`, a load) are not the
  cigar. Whole minutes go out as `kGEvTimeSkip` (a = minutes) at most every 250 ms while the time-lapse runs, so Elden Ring's
  sky follows it; the rest is rounded at the end. A load ends a smoke (nothing counted across it).
- **Log:** `boot: phantom cigar: watching (status check ok, mute check ok); the clock is let run at x20 unless MGSV moves it
  itself while he smokes`; `cigar: Snake lights the Phantom Cigar (MGSV's clock 12:00:00, vars.clock 43200)`; only if the
  clock stood still: `cigar: MGSV's clock stood still 500 ms into the smoke: it runs at [Cigar] ClockScale x20`; `cigar: put
  out -- MGSV's clock 12:00:00 -> 18:30:00: +390 min of game time for Elden Ring`. `+0 min ... never moved, even let run`
  means neither moved the clock: try `ClockScale`, or look at what the cigar changes (`pr lua 'return vars.clock,
  TppClock.GetTime("string")'`). A stuck signal: `cigar: the smoking signal has been on for over 120 s: taken as stuck`.
- `!cigar`: smoking / signal STUCK / not, MGSV's clock, smokes (moved by MGSV itself N, run by the guest N, stuck signals N),
  events / minutes sent / implausible steps. `pr lua 'return PhantomRing.cigar.on, PhantomRing.cigar.lit,
  PhantomRing.cigar.runs, PhantomRing.cigar.holds, PhantomRing.CigarSmoking()'` from the Lua side.
- Offline: `tests/test_guest_script.lua` (the flag from either signal, `cigar lit` sets no speed, `run` x20, `out` frozen,
  held frozen every 60 Ticks only while not smoking and in the sandbox), `test_host` (the tracker; the governor: MGSV's own
  time-lapse not overridden, the probe, the stuck guard in ms, a load); fake_er logs `time skip: +N min of MGSV game time`.

### 18.17 The dive as kGuestDiving (`Dive.*`, `DiveGuest.cpp`, `[Motion] DiveFlag`, protocol v13; roadmap 2.9; live pass A 2026-10-14: `dive: X -> diving (confirmed by 'left STAND' 18-19 ms after X, 4.0-4.8 m/s)`, three of three running dives; a dive ends prone, and X while prone or in a jump arms none)

The host hops its kinematic puppet over a gap or onto a ledge while Snake dives (HOST.md "Dive = hop"). MGSV has **no dive
status**: the `PlayerStatus` names in mgsvtpp.exe 1.0.15.4 (VA `0x1421961D0`..`0x142196830`, read from the exe on disk) are
STAND SQUAT CRAWL NORMAL_ACTION PARALLEL_MOVE IDLE GUN_FIRE GUN_FIRE_SUPPRESSOR STOP WALK RUN DASH ON_HORSE ON_VEHICLE
ON_HELICOPTER HORSE_* BINOCLE SUBJECT INTRUDE LFET_STOCK CUTIN DEAD DEAD_FRESH NEAR_DEATH NEAR_DEAD FALL CBOX CBOX_EVADE
TRASH_BOX TRASH_BOX_HALF_OPEN INJURY_LOWER INJURY_UPPER CURE CQC_CONTINUOUS BEHIND ENABLE_TARGET_MARKER_CHECK UNCONSCIOUS
PARTS_ACTIVE CARRY CURTAIN VOLGIN_CHASE (CBOX_EVADE is the box's own dive). So the dive is read from what it does:

- **X**: the player's own press (`pad::Presses(X)`: rising edges of the host's pad as MGSV polled it -- never Death.cpp's own
  dive pulse), while Snake **stands** and is eligible (ready, on foot, not CQC / FALL / DEAD / a menu / a cutscene / the box /
  carrying / Tarnished mode).
- **A sign within `DiveConfirmMs` (600 ms)**: he left STAND, went CRAWL (a dive ends prone), left NORMAL_ACTION (only if it
  was on at X: already off then -- a reload, a weapon switch -- it says nothing about a dive), or sped up to 2 m/s from below
  it (from a stand or a walk; already running, speed alone is no sign). Each sign is a change since X. No sign: no flag
  (`dive: X while standing but no sign of a dive`).
- **The flag** stays up from the sign until he comes to rest (< 0.8 m/s after 300 ms), stops being eligible, or 1.5 s.
- Which sign MGSV gives first is the open question (above: "Is a dive NORMAL_ACTION?"): each dive logs
  `dive: X -> diving (confirmed by '<sign>' N ms after X, S m/s) -> kGuestDiving`; console `!dive` counts dives and X presses
  with no dive. `DiveFlag = false` never sets the flag.
- Tests: `tests/test_guest_dive.cpp` (each sign; crouched X; no sign; already running; not eligible / cancelled; one flag per
  dive; NORMAL_ACTION already off at X is no sign).

### 18.18 Suppressor (`Suppressor.*`, `[Loadout] Suppressors`, `!suppressor`; protocol v16 `kGuestSuppressorOn` / `kFireSuppressed`; live 2026-11-03, MGSV + fake_er)

Player report (0.6.0): a silencer on the sleep pistol. MGSV's WU S.PISTOL takes a suppressor, but the boot's loadout put the gun
into a slot whose suppressor vars were the save's ZORN-KP's (none): Snake's tranquilizer pistol was loud, with no SUP gauge.

- **Where MGSV keeps it** (static RE, Addresses.h "Snake's suppressor", every site byte-checked; `verify_addresses.py`: 135 sites, all match):
  the ammo component's 6-byte slot record (`[comp+0x50] + (player * [comp+0xC] + slot) * 6`, the one Ammo.cpp refills) is u16
  magazine, u16 sub magazine, **u8 life** (+4: rounds it still takes, 0xFF never wears) and u8 flags (+5: **0x02 on**).
  `vars.suppressorLives[slot]` / `isUseSuppressor[slot]` (u8 x6, `+0x56C` / `+0x572`) are the copies a mission load builds the
  records from. FireWeapon (0x141041110) asks MGSV's own reader `0x141050870` (WeaponComponent, &life, &on, WeaponState, player)
  **before its pellet loop**: not a firearm -> 0/0; on = (rec[5] & 2) && life && the gun's traits (`[w+0x23A]` & 0x40: the gun
  takes one); the alternate (under-barrel) magazine has its own bit 0x40. Pellet i is fired suppressed while on && i < life
  (quiet, no muzzle flash); afterwards life -= pellets through the equip controller vf 0x150 (not for 0xFF, the alternate mode,
  or `[w+0x3C0]` & 0x20000000), and at 0 the "broke" notices (the gauge goes). The component's own setters: vf 5
  `SetSuppressorLife` (rec[4], then its listener's notice: the HUD) and vf 6 `SetUseSuppressor` (rec[5] bit 1). Each gun's
  **full life** is its 0x90-byte gun data's +0x80 (the listener `[comp+0x58]` vf 0x150 GunData(slot type), replicated with safe
  reads): live the WU S.PISTOL **30** (traits 0x55), the SVG-76 and the FB MR launcher 0 (traits 0x11 / 0x01: they take none).
- **The boot's loadout** (`[Loadout] Suppressors = sleep|all|none`, default sleep; only while MGSV's saves are blocked and the
  setters verified, else `boot: suppressors (...): left as the save has them (why)`): `PR.PreLoadout` writes each [Loadout] gun's
  `isUseSuppressor` / `suppressorLives` before the sandbox loads -- on with a provisional 254 for the sleep guns (bullet 10/11) or
  all, off/0 for the others -- and at the setup, the guns' parameters loaded, `suppressor::ApplyLoadout` sets MGSV's own full
  life natively (a reload keeps a worn life; a gun that takes none is turned off). Live: `boot: loadout before the load: ...
  EQP_WP_West_thg_010(7)=slot 2 was 875 suppressor 2=on` -> `suppressor: in hand equip 7 (slot 2): on, life 254` (MGSV doesn't
  clamp it) -> `boot: suppressors (sleep): slot 0 equip 36 (bullet 15, takes none): none; slot 1 equip 54 (bullet 46, takes
  none): none; slot 2 equip 7 (bullet 10, takes one, MGSV's full 30): slot 2: 254 on -> 30 on`. The SVG-76's record had kept the
  save's shotgun's life 15 / on; now 0 / off. MGSV's weapon box: the pistol drawn with its suppressor, `SUP` and a full gauge.
- **kFireSuppressed** (`Combat.cpp` ShotDetour, inside FireWeapon before the loop and the wear): MGSV's reading for the firing
  WeaponState; `b = pellets | kFireSuppressed` when on && life > 0 (logged `shots: equip 7 SUPPRESSED (suppressor life N before
  the shot)`; `!shots` counts). **kGuestSuppressorOn** (`Frame.cpp`, every frame): the same reading for Snake's WeaponState
  (`[character+0x80]` = his WeaponComponent, `[c+0x58] + (player - [[c+0x38]+0x24]) * 0x610`); a change is logged (`suppressor:
  in hand equip 7 (slot 2): on, life 30` / `none or off`).
- **Refill**: kHEvRested (rest 0 / respawn 1) and `!refill` refill every gun that has one this session (the loadout's, or one seen
  working: a supply drop's) to its full life with Ammo.cpp's rounds, on the Lua thread (`suppressor: refill (rest; slot 2: 0 on ->
  30 on (equip 7, MGSV's full 30))`). A weapon slot's ammo stock rising outside a rest (a supply drop's ammo picked up) refills that
  gun's suppressor too (debug `suppressor: slot N's stock rose A -> B ...`). Every write (the refill, `!suppressor <slot> <life>`)
  waits for MGSV's save guards as the boot's loadout does (`left as it is (MGSV's save guards are incomplete ...)`). **Known
  limit:** with the save's Infinity Bandana MGSV refills the reserve itself at every reload, which looks like a pickup (harmless:
  the suppressor never wears then, below; such a "refill" that changes nothing is logged at debug level only). MGSV's own supply
  drop was not tried.
- **The Infinity Bandana**: this save's Snake wears it (`vars.playerFaceEquipId` 2 -> `PlayerPlayFlag.USE_MUGEN_BANDANA`,
  `vars.playerPlayFlag` 384): MGSV then shows the reserve as "∞" and **never wears a suppressor** (FireWeapon skips the wear, the
  `[w+0x3C0]` 0x20000000 bit): live 18 shots, life 30 throughout. That is MGSV's own rule, kept. For the wear test it was taken off
  in memory (`vars.playerFaceEquipId = 0` + a sandbox reload; saves blocked; the next launch has it again).
- **Live (MGSV + fake_er on /dev/shm/phantomring_g, the bandana off):** `!suppressor 2 10 on`, then 14 aimed RT taps: 10 kGEvFire
  `x1 SUPPRESSED` with the guest's life before each shot 10, 9, ..., 1 (`... 1 before the shot: it wears out with this one`), fake_er
  `guest: suppressor off (kGuestSuppressorOn) -- weapon 0x7 4/11` 20 ms after the 10th, then 4 shots without the bit; fake_er `rested
  0` -> `suppressor: refill (rest; slot 2: 0 on -> 30 on ...)` 1 ms after the rounds' refill, `guest: suppressor ON` at once, the next
  shot SUPPRESSED (life 30 -> 29). The first shot from a full one: life 30 -> 29 in the record. **MGSV's output** (`[Audio] Meter`,
  each shot metered alone, loudest 0.5 s window / peak): suppressed (11 shots) -29.1 .. -29.7 dBFS / -12.7 .. -13.4; the worn-out
  pistol (3 shots) -24.4 .. -24.6 / -10.2: **~5 dB quieter**; the shot it broke on -23.2 / -7.1 (MGSV's "broke" sound with it).
  **MGSV's HUD** (gamescope frames, the weapon box): `SUP` with a full gauge; worn out, no gauge (MGSV's own); refilled at a rest
  the gauge comes back at the next shot or weapon change (the box refreshes then, as its ammo does; known limit).
- `recordings/fire_suppressed.prrec` (git-ignored; recordings/README.txt): 4 suppressed shots, the wear-out, 3 unsuppressed.
- **Review 0.7.0**: `Suppressors = sleep` means every gun firing tranquilizer darts (`logic::KindOfBullet` kTranq: bullets 10, 11,
  26, 30, 34, 40, 49, 67, 98; `PR.SLEEP_BULLETS` the same list), not only the handgun's 10 / 11. A gun whose suppressor never wears
  (gun data +0x80 = 0xFF) gets exactly 0xFF (a provisional 254 used to stay and wear out for good after 254 shots: MGSV skips the
  wear only at 0xFF), is refilled to 0xFF, and is left on whatever the mode (SetUseSuppressor forces it on; zeroing it only made the
  reader call it worn out). `kFireSuppressed` only when every pellet goes through it (life >= pellets, or 0xFF): MGSV suppresses pellet
  i < life, so a shotgun's last lives were partly loud. Turning one off sets "off" first and then the life (SetSuppressorLife on a
  record still on calls its listener, and with 0 that is MGSV's "suppressor broke" notice). `!suppressor` reads the refill state under
  the lock (the Lua thread's refill reassigned the string meanwhile), `g_pickups` is atomic. A byte check that fails now logs 64
  bytes around the first difference (`found from +0x..`), not the site's first 64 (Verify.cpp).
- **Live with both games** (0.7.0 integration, Elden Ring 1.17.2 + this MGSV, the player's save): the boot's `slot 2: 254 on -> 30
  on`; Elden Ring's frames show MGSV's weapon box with the pistol, `SUP` and its gauge; a head dart `shots: equip 7 SUPPRESSED
  (suppressor life 30 before the shot)` -> the host's `... suppressed) -> ... HEADSHOT ... sleep +252` and no AI sound; the bandana
  taken off in memory (`vars.playerFaceEquipId = 0`, `!boot`: MGSV reloaded the sandbox and Snake took over again; `vars.playerPlayFlag`
  still read 384 but the suppressor wore), `!suppressor 2 3 on`, three shots: life 3, 2, 1, then the host's corner "Suppressor worn
  out", the weapon box without its gauge, the fourth shot loud; a rest at a grace `suppressor: refill (rest; slot 2: 0 on -> 30 on (equip
  7, MGSV's full 30))` and the gauge back at the next shot. Respawns: `suppressor: refill (respawn; slot 2 (equip 7): full (30))`.
- No suppressor toggle of MGSV's own was found to wire up (none among its pad actions the guest knows; the component's
  `SetUseSuppressor` is called by its equip setup); `!suppressor <slot> <life> [on|off]` sets one through MGSV's setters (tests),
  `!suppressor` shows the slots' records and gun data, the vars copies and the weapon in hand.
- Offline: `test_guest_logic` `TestSuppressor` (the reader with the live values: the pistol, the rifle with a stale record, the
  non-firearms, the alternate magazine), `test_guest_link` (the ini's `Suppressors = sleep`).

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

## 20. ActorProxy (`[Proxies] Enabled = false`, `ActorProxy*.cpp`, `!proxy`; ROADMAP 2.1; v0 failed live, **v1 works live with fake_er 2026-10-14**, 20.5)

Goal: the sandbox's disabled MGSV soldiers become inert position proxies of the nearest hostile Elden Ring characters, so
MGSV's own aim assist, marking and buddies (D-Walker's Search Mode, D-Dog, Quiet) have targets where Elden Ring's enemies are.
Elden Ring keeps damage authority. Everything below is built and unit-tested offline (`test_guest_actorproxy`, in
`check_guest`); none of it has run in MGSV yet, and the ini default is off (with it off nothing changes: no pool, no reads,
no writes).

### 20.1 The pieces

| Question | v0 | Where |
|---|---|---|
| Where ER's enemies are | The host's `ActorTable` (already published: `er/Actors.cpp`, `[Actors] radius 60 / max 64`): `kActorHostile` (enemy, other team), feet in MGSV world through the same anchor as Snake: `pos = Pm + DirToMgsv(erPos - Pe)`, `Pm` = the `rootPos` of the GuestState the host consumed. The guest reads it with `link::ReadActors` (seqlock, used records only). No protocol change | `Link.cpp` |
| The treadmill / odometer offset | `ActorTable.pos` is MGSV world *as of the GuestState the host used*; a treadmill jump since then moved MGSV's world under ER's enemies. `k = rootPos - odo` (computed in doubles) is constant between the guest's own warps; the guest keeps 2 s of it (`plan::OffsetHistory`) and places a record at `pos + k(now) - k(table first seen - TableLagMs)` (33 ms). Off by one jump for at most a frame or two right after a warp | `plan::SelectTargets`, tested |
| Which soldiers | `PhantomRing.ProxyPool()` (Lua, at the sandbox setup, after `SetEnabled false`): every `TppSoldier2` and `TppHostage2` id (`GetGameObjectIdByIndex`), returned as `"soldiers;hostages"`; soldiers also get `SetDisableDamage{life, faint, sleep}` (InfSoldier's command; UNVERIFIED on a disabled soldier). The first `Max` (4) soldiers may stand in; every other soldier and every hostage is parked | `GuestScript.cpp`, `plan::ParsePool` |
| Who stands in for whom | The nearest `Max` hostile, living characters within `MaxDistance` (60 m). A character keeps the soldier it has (no swaps: a mark or the aim would jump); a freed soldier takes the next one, nearest first | `plan::Assign`, tested |
| Moving them | **Native, every frame, on the game thread** right after the GuestState is published: `hold::PlaceObject(id, pos, YawQuat(yaw))` = the location manager `Player2SystemImpl::Warp` uses for Snake (`(*(inst+0x78)+0x238)->vf[0x388](id)`, then `loc->vf[0x68](loc, pos, rot)`; Hold's byte checks). Written only when the target moved 2 cm or turned 1 deg, or every 30 frames. Not Lua: `Warp` / `SetPosition` leave a disabled soldier standing, `SetEnabled true` + `Warp` moved one only 4 m (18.13), and a command per soldier per frame would load the Lua thread | `ActorProxyGame.cpp` |
| Will that manager move a soldier? | Static analysis says maybe: 40+ engine sites do the same `+0x78 -> +0x238 -> vf 0x388(id)` lookup for other objects' ids; the location class is Fox core (vtable `0x1424C8EF0`, built by two factories `0x141ACF260` / `0x141AD2940`); its `vf 0x68` = `vf 0x58` (position: `+0x20` xyzw, `+0x10` = that minus the foot offset `+0x160`, clears flag 4 of `+0x1A0`, zeroes `+0x1F0`) + `vf 0x60` (rotation, `+0x60`). **But** `!loc` found it "knows only Snake" for D-Horse (13): it may hold the player family only. Live step 1 settles it for soldiers | Ghidra; `Addresses.h kLocationPositionRead` |
| Reading back | Every 30 frames each pool soldier's `loc+0x20` is compared with what was written (`hold::LocationPosition`); `!proxy` and the log count them | `ActorProxyGame.cpp` |
| Parking | Every non-proxy soldier and every hostage waits `ParkDepth` (100 m) below the sandbox centre, 2 m apart. A proxy whose character comes nearer than `SnakeClearance` (3.5 m) to Snake's root or `BuddyClearance` (5.5 m) to the buddy parks for as long as that lasts (it keeps its character). UNVERIFIED: whether a disabled soldier 100 m under the apron falls, meets a kill plane or is put back by the mission | `plan::Assign`, tested |
| No AI, no alerts | They stay `SetEnabled false` (no AI, perception, radio); only their location changes. `vars.playerPhase` must stay 0. (v1 enables puppets; full proxies since 20.6: restrict-notice + puppet before the enable, the phase lock) | setup; 20.6 |
| Kept out of Elden Ring's draws | Capture takes a G-buffer draw as Snake's within 2.5 m of his root and as the buddy's within 4 m of it (15). The clearances keep proxies outside both, and `Capture.cpp` also drops any draw within `CaptureRadius` (1.5 m) of a placed proxy that is nearer to it than to Snake (`!capture`: `proxy draws dropped`). Skeleton.cpp's buddy-skin learning (D-Horse, 8.1) skips skins rooted within 1 m of a proxy | `plan::NearProxy`, tested |
| MGSV's own hits don't count | Nothing reads damage on soldiers: the guest reports Snake's rounds only from `ShotSolver` (7) and melee from the CQC serial, whatever the round hits in MGSV. Proxies only change where MGSV *aims*. `SetDisableDamage` keeps a hit soldier from dying or ragdolling in MGSV (UNVERIFIED) | Combat.cpp unchanged |
| Aim assist for proxies only | MGSV's Auto-Aim Correction (`vars.optionSelectedIndices[11]`, 18.13) has no per-target switch: its getter `0x1405301B0` (byte-checked: `table[vars + 0x131A3]`) is an option interface's slot (`0x14218D300`) with virtual callers. v0 makes "proxies only" true by construction: the option goes back ON (`PR.AutoAim(true)`) only while **every** pool soldier reads back where it was put (proxies at ER enemies, the rest parked out of reach) and the save guards are complete (as `[Boot] AutoAim`), and OFF again as soon as one doesn't (`plan::AutoAimWanted`, once a second on the Lua thread). `[Boot] AutoAim = true` keeps MGSV's setting and ActorProxy leaves it alone | `Pump.cpp`, tested |
| Markers | `[Proxies] KeepMarkers = true` skips the periodic `TppMarker.DisableAll` (`[Boot] ClearMarkersSec`) while proxies run and every pool soldier reads back placed (else a marked soldier at his post pulls the aim, 18.13): marking an ER enemy through his proxy is the point. The HUD layer (16) carries MGSV's marker into Elden Ring | `Pump.cpp` |

`[Proxies]`: `Enabled` (false), `Max` (4), `MaxDistance` (60), `SnakeClearance` (3.5), `BuddyClearance` (5.5), `ParkDepth` (100),
`TableLagMs` (33), `AutoAim` (true), `KeepMarkers` (true), `CaptureRadius` (1.5); full proxies only (20.6): `Inert` (true),
`NoAssignRoute` (false), `PhaseLock` (true), `AlertGiveUpSec` (20), `Height` (ground), `HeightTolerance` (2), `AimHeightFix` (true). Byte checks (`mgsv/verify_addresses.py` and at
the first proxy frame): `kLocationPositionRead` (0x14103FB4A), `kAutoAimOptionGetter` (0x1405301B0) and its slot `0x14218D300`, plus
Hold's Warp sites. Any mismatch: proxies off, logged. `fake_er --actors near` publishes its two actors standing still 15 m and
27 m from where Snake started, mapped the way the host maps them (treadmill jumps move them in MGSV's world as with the real host).

### 20.2 Known limits of v0

- Heights are not flattened: a proxy stands at the ER enemy's height relative to Snake (a ledge 3 m up = 3 m above the apron).
  That is where aim assist should aim; whether a disabled soldier hangs there is part of the live test.
- A soldier is human-sized: aim assist goes for a soldier's chest, so a giant's proxy pulls the aim to ~1.3 m above its feet.
- After a treadmill jump the proxies are late by a frame or two (`TableLagMs`); the jump's own frame places them with the
  pre-jump offset.
- MGSV's marks are not mapped back to Elden Ring characters (the host draws nothing for them); the HUD layer shows MGSV's own.

### 20.3 Live test (next session: MGSV + fake_er first, then both games)

Before: saves backed up (the launcher does it), offline, `[Saves] Block = true`, `[Proxies] Enabled = false` for steps 1-3.

1. **Does the location manager know a soldier?** At Ready: `pr lua "return PhantomRing.ProxyPool()"` (two ids, decimal), then
   `pr lua '!loc <id>'` for each. A location object (vtable `0x1424c8ef0`, like Snake's) = go on. `none` = the manager holds
   the player family only: stop (v0 can't move soldiers; next RE: the TppSoldier2 system's own location lookup -- its Warp
   command handler -- or the soldier's own controller found through its SkinPublish model).
2. `pr lua '!placeobj <id> <x> <y> <z> 0'` at a spot 10 m in front of Snake on the apron (his position: `!status`). In MGSV
   (screenshot) and with `!loc <id>`: `+020` holds the spot? He is drawn there, not at his post? Again at y + 3 (hangs or
   falls?) and y - 100 (`!loc` after 30 s: still there? `vars.playerPhase` 0?).
3. Aim assist at his new place: `pr lua "return PhantomRing.AutoAim(true)"` (saves blocked), aim the FB MR 10-20 deg beside
   him: the aim swings onto him where he now is, not to his old post (`pr record`, camera yaw). `PhantomRing.AutoAim(false)`.
4. `[Proxies] Enabled = true`, fake_er `--actors near`, `!boot`. Log: `boot: proxy pool: N soldiers, M hostages`, `proxy:
   pool of ...`, `proxy: soldier 0x... now stands in for Elden Ring character 0xffffffff00001000` (and `...1001`), `proxy: N of
   N pool soldiers read back where they were put (0 not)`, `boot: proxies: auto-aim correction on (was off)`. `!proxy`: two
   `proxy ... placed (read back)`, the rest `parked ... (read back)`.
5. Treadmill: run past the radius (native_treadmill): `!proxy` positions jump with each treadmill shift and otherwise hold; a
   `pr record` replay shows no proxy off by a jump for more than 2 frames.
6. Capture: walk Snake to within 2 m of an actor: `!proxy` shows it parked; `!capture` counts `proxy draws dropped`; fake_er's
   draw lists / the recorder carry no soldier mesh.
7. Shoot a proxy in MGSV: exactly one `kGEvFire` per round (ShotSolver), no other event; the soldier neither dies nor ragdolls
   (SetDisableDamage), `vars.playerPhase` stays 0, no alert music or radio.
8. Both games (ROADMAP 2.1 "Verify"): an ER soldier at 20 m -> LT pulls the aim onto him; marking him shows MGSV's "20m" over
   him in Elden Ring (HUD layer); 10 minutes of play with 0 double hits (host log: one hit per `kGEvFire`); D-Walker's Search
   Mode marks; `pr record` + replay for regressions. Any failure: `[Proxies] Enabled = false` is today's behaviour.

### 20.3a Live result 2026-10-14 (MGSV + fake_er, Africa): soldiers can't be moved this way

- **Step 1 fails.** The pool is the 270 TppSoldier2 ids 0x400..0x50D (`maxInstanceCount 270`; ids past it raise "index 270 is
  larger than maxInstanceCount 270"), no TppHostage2. `!loc 0x405` / `!loc 0x44c` (soldiers at their posts): **none**; `!loc 0`
  (Snake) answers the location object (vtable 0x1424C8EF0). The location manager knows the player family only, as it did for
  D-Horse. Proxies stay off.
- `PR.ProxyPool` listed 1024 soldiers with every id twice: `GetGameObjectIdByIndex('TppSoldier2', i)` = 0x400 + i % 512 for any i
  (no failure past the end). Fixed: it stops at a repeat or at the first id GetPosition refuses.
- **TppSoldier2's command handler** (0x141356170, 42 KB, `SendCommand` -> vtable 0x1423E8060 slot 26 of the soldier system, one
  instance) compares 174 command hashes; 80 named from the strings of every function in MGSV's live Lua VM (`string.dump` of
  `_G`, StrCode32 of each identifier). **No Warp / SetPosition** among them (`Warp` only in the hostage handler 0x140E85E10, and
  Africa has no hostages); a soldier answers nil to both. Useful ones: `GetPosition`, `ChangeLifeState{state=}` (1 dead, 2 dying,
  3 sleep, 4 faint), `GetLifeStatus`, `GetStatus`, `GetActionStatus`, `RequestForceFulton` (no arguments: sets bit 0 of the
  soldier's flags `[[sys+0x110]+8] + i*40 + 0x18`), `SetEnabled`, `SetPuppet{enabled}`, `SetForceRealize`, `SetRelativeVehicle`,
  `SetDisableDamage`, `SetEverDown`, `SetZombie`, `SetFriendly`, routes (`SetSneakRoute` ...).
- `GetPosition` reads a cache: `[[soldierSystem+0x18]+0x20] + index * 16` (xyz + yaw). Writing it moves nothing (a soldier
  rewritten 10 m in front of Snake: GetPosition answers the new place, nothing is drawn there).
- **Disabled soldiers aren't drawn or skinned**: none of them reaches SkinPublish (`!skins`: 13 skins published, none with a
  soldier's skeleton), and soldier 0x450 appeared at his post (screenshot) only once enabled. What 18.13 saw pulling the aim may
  be such a soldier enabled again by the mission, or another NPC: a figure stands near the terminal that no soldier id projects to.
- **What does work** (live): `SetEnabled true`, then a few updates later (not in the same update) `ChangeLifeState{state=3}`:
  the soldier lies asleep where he stood (GetLifeStatus 3, GetStatus 1, `vars.playerPhase` stays 0, only "Intel Team: Spotted new
  threat" / "Map Info Updated" in MGSV's log). Next to him MGSV offers its own Fulton ("Y 100% x48", B: carry); **Y held ~2 s**
  fultons him ("Fulton Recovery Expenses: GMP -300", in memory: saves are blocked); `RequestForceFulton` does the same without
  Snake. While the balloon has him: GetStatus 0, GetActionStatus 1 (asleep: 1 / 0), and the GetPosition cache only creeps (2.6 m
  up after 6 s with the soldier far above the roofs). Under the terminal's overhang the balloon hit the roof (sparks).
- Next RE for real proxies: the enabled soldier's own transform (character controller) -- what writes the GetPosition cache each
  update, or the realize/route system that places soldiers when a mission starts. `!skins [radius] [ms]` lists every skinned
  model published (driver, binding, bones, root).

### 20.5 ActorProxy v1: enabled puppets moved by their own location object (live 2026-10-14, MGSV + fake_er `--actors near`)

The breakthrough after 20.3a: writing a copy of an *enabled* soldier's position moved his body. The copy that drives everything
(the others are recomputed from it each update; only this one persisted, and his GetPosition cache followed it) sits at +0x20 of
an object whose vtable is **0x1424C8EF0 -- the same location class as Snake's** (`!loc 0`): +0x10 the feet, +0x20 the feet + 0.8 m,
+0x60 the rotation quaternion. Disabled soldiers have none (they aren't realized); about 30 such objects exist and are reused
between realized characters (soldier 0x4B9 got the object 0x44E had). Then:
- **Proxy = enabled puppet.** `SetEnabled true`, 150 ms later `SetPuppet{enabled=true}` (+ `SetDisableDamage` life/faint/sleep):
  he stands with his weapon and does nothing -- 20 s 5 m in front of Snake in plain view, `vars.playerPhase` 0. His location object
  is found by position (`PR.ProxyPool(cx, cz)` sorts soldiers by distance to the sandbox: only soldiers near Snake are realized when
  enabled -- 0x405/0x406, 3 km away, never were; the unused instance slots 0x400-0x404 stand at the origin and are skipped, and so
  are location objects at the origin), from a background scan of MGSV's writable memory for the vtable (2.3 GB in ~220 ms;
  `proxy: scan (pool): 30 location objects, soldier system 0x4a0591a0`) and the soldier's GetPosition cache (read natively:
  `[[system+0x18]+0x20] + index*16`, the system found by its vtable 0x1423E8060, byte-checked: GetPosition's code, slot 26 =
  the handler). Every frame its own setter (vf 0x68, `hold::PlaceLocation`, vtable re-checked each call) puts him at the Elden Ring
  character's place (feet + 0.8 m), as v0 planned. A free slot soldier is simply **disabled** again: out of the world (not drawn,
  not targeted) -- no parking under the map. 4 slots (`Max`); the other ~260 soldiers stay disabled as the sandbox setup leaves them.
- Live: actor 0 15 m in front of Snake -> soldier 0x450 standing exactly there (screenshot, projected 771,386, seen at 765,380);
  running 18 m through two treadmill jumps the proxies kept the Elden Ring places (treadmill offset k applied; `4 of 4 slot
  soldiers where they should be`); `boot: proxies: auto-aim correction on (was off)` -> LT with the camera 14 deg beside the proxy:
  **the aim swung onto him** (red reticle on his chest, camera yaw -22 -> -36.6) -- MGSV's aim assist on the proxy, and only proxies
  can be targets; one WU S.PISTOL dart: **exactly one kGEvFire** (a=0x10007, bullet 0xa), nothing else, the puppet stayed standing,
  phase 0, MGSV marked him ("4m" over his head, in the HUD layer). `!capture`: `proxy draws dropped 23867` (none reached Elden Ring).
- **Asleep -> MGSV's own Fulton:** `pr er actor 0 asleep` -> `proxy: soldier 0x450 asleep for Elden Ring character
  0xffffffff00001000`. A puppet ignores ChangeLifeState, and so does a soldier whose sleep damage is off: `PR.ProxySleep` lifts both
  first, then state 3 (re-sent every 8 s). An asleep character's proxy is never parked near Snake (the clearance would have taken
  him away exactly when Snake walks up). Snake walked up -> "Y 100% x48", Y held -> the balloon -> **kGEvFulton a=1 handle
  0xffffffff00001000 at 824.6,-12.0,1179.6 (its proxy)** ~4 s after the Y was released, fake_er removed the actor, `soldier 0x450
  spent (fultoned) -- soldier 0x44a takes his slot`; phase 0 throughout. With proxies on, the 20.4 anchor stands aside.
- **`[Fulton] Method = proxy` (the default):** with `[Proxies] Enabled = false` ActorProxy still runs for **sleeping** characters
  only -- no aim assist change, no proxy for an awake enemy. Live (fake_er, proxies off): actor 0 awake 8 m away got none, actor 1
  flagged asleep 4 m ahead got soldier 0x450 asleep at his place within 4 s (`proxies off (sleeping characters only: [Fulton]
  Method = proxy)`), Snake walked up, Y held -> `kGEvFulton a=1 ... 0xffffffff00001001 at 824.6,-12.0,1179.6 (its proxy)`, the slot
  took soldier 0x44a, phase 0. Nothing moves Snake (the 20.4 anchor's teleport and MGSV-side surroundings are gone).
- Console: `!proxy` (slots: soldier, job, life -- disabled/enabling/puppet/locating/active/spent --, asleep, location object),
  `!proxy on|off` (this session; on asks for the pool), `!skins`, `!fulton`.
- **With the real Elden Ring** (combined run 2026-10-14, Gatefront): the default `[Fulton] Method = proxy` works end to end (HOST.md
  "Fulton": a darted Limgrave soldier, his asleep proxy 0x450, MGSV's Fulton prompt and "ZZZ" marker over the Elden Ring soldier,
  Y held, `kGEvFulton a=1` 5.3 s later, the host's balloon, +64 runes); four realizations of the asleep proxy, alert phase 0
  throughout. **Full proxies (`!proxy on`) are not ready with the real host**: Elden Ring's enemies stand above and below Snake
  (-1.3 to -5.5 m here) and MGSV's flat apron can't hold a soldier there, so the read-back flapped (`4 of 4` / `1 of 4 slot
  soldiers where they should be` every ~3 s) and auto-aim correction with it; and within 80 s MGSV went to **alert phase 3**
  with an MGSV riot-shield soldier on the apron shooting Snake (his life to 649 of 7801 in Elden Ring; MGSV's own view shot with
  `GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 gamescopectl screenshot`). Likely the 150 ms each realized soldier is live AI before
  `SetPuppet` (he realizes at his post -- `0x44a realized ... at 826.3,-3.8,1226.1`, a rooftop) or a flapping re-placement.
  `!proxy off` disabled the slot soldiers, but the alert and the reinforcement stayed until MGSV restarted. The guest now logs
  `alert: MGSV's alert phase a -> b` (a warning when it leaves 0). A new pool clears the old read-back counts (`SetPool`: the
  first `!proxy on` turned auto-aim on from the Fulton-only pool's counts).
- Still to do before `[Proxies] Enabled = true` by default: step 8 with the real Elden Ring (10 minutes, double hits, D-Walker's
  Search Mode, the HUD layer's marker over the real enemy), the host's `kActorAsleep`, a stability run of an hour (sleeping proxies
  wake after a few minutes without the 8 s refresh: a soldier woken that way went to Reflex and ALERT, live, before the refresh
  existed), and a proxy's height/size for giants (20.2).

### 20.6 Inert full proxies (2026-10-14; live pass B 2026-10-15: safe for 11 minutes, no aim assist seen; only with `[Proxies] Enabled = true`, still off by default)

Live pass B (2026-10-15, both games, Gatefront, `!proxy on`): `pool of 265 soldiers (4 slots, 261 in reserve)`, 4 realized and
`4 of 4 slot soldiers where they should be`, `boot: proxies: auto-aim correction on`; `!proxy` every 20 s for 643 s: `inert true
(sites ok), height ground ... phase lock true: phase 0, 0 alerts, 0 calms, 0 sweeps`, Snake's life never moved (no stand-in shot
him), no alert line. A Fulton through a stand-in: `fulton ... succeeded (MGSV's own Fulton on its proxy) -- kGEvFulton a=1`, phase
0. MGSV's analyzer read a stand-in through the binoculars. Not good enough to turn on: LT never swung the aim onto a stand-in (3
tries, the reticle 4-11 deg off a soldier 20 m away and 7 m lower; rounds then flew where aimed, `0 rounds turned`), the fourth
slot switched between two enemies at about the same distance every ~20 s (`soldier 0x4cb now stands in for ...1c` / `...07`),
and `location objects lost` climbed to 4 (two Fultons and churn).

Answer to 20.5's real-host run (alert phase 3 within 80 s, a riot-shield reinforcement shooting Snake, the read-back flapping on
Elden Ring's heights). Everything here applies only to full proxies: the Fulton-only proxies (`[Fulton] Method = proxy` with
`[Proxies] Enabled = false`, live with the real host) keep exactly the 20.5 sequence and heights.

**What RE found** (static, the exe on disk; every site byte-checked in `Addresses.h`, `mgsv/verify_addresses.py`: all match).
Command names were tested by their StrCode32 as immediates in the handlers (`tools/re/strcode32.py`'s hash; a raw imm32 search
since capstone isn't installed):

| Command | Where | What it does |
|---|---|---|
| `SetRestrictNotice{enabled}` | TppSoldier2's handler 0x141356170: `cmp eax,0x681B5200` at 0x14135C9A2 -> 0x14135CCA9 (one soldier); 0x141358ECC -> 0x1413591BE (sent to the type: every soldier) | sets / clears **bit 51** of the soldier's first flags qword in the system's per-instance table (`[[system+0x110]+8] + index*40`) -- the same table RequestForceFulton (bit 0 of +0x18) and SetPuppet use. By its name: the soldier may not *notice* (see, hear) Snake. Exists in no other handler. |
| `SetPuppet{enabled}` | `cmp eax,0xD3B35172` at 0x14135FBD7 -> 0x14135FCEB | bit 27 of the second qword (+8). Nothing else. |
| `SetEnabled{enabled, noAssignRoute}` | -> 0x1413604CB, helper 0x141368C20 | enable ORs bits 30 and 36 into +8 (disable sets bit 33 of +0, clears 30/36); **leaves bits 27 and 51 alone**. `noAssignRoute` (bool) sets bit 20 of `[[system+0xD0]+0x18] + index*20`. |
| `SetPhase{phase}` on `TppCommandPost2` | CP handler: `cmp edx,0x924FEBFD` at 0x140D5FAE0 -> 0x140D5FB81 (type target: loops over every post, writes `phase` to +0x4C of each 96-byte entry at `[cp+0x40]`, count `[cp+0x174]`); 0x140D60A05 -> 0x140D60A81 (one post) | the command posts' phase. Siblings in that handler: `SetKeepAlert{enable}`, `SetKeepCaution{enable}`, `SetReinforceEnable`, `SetReinforcePlan`, `SetNominateList`, `SetCombatArea`, `ChangePhase` (message). No `SetMinPhase`/`SetMaxPhase`/`SetPhaseLock` exist. |
| not in this exe | | `SetNoticeEnabled`, `SetIgnoreNotice` exist only in other handlers (0x140DD..., 0x1417...: vehicles/animals?), `SetSightCheckFlag`, `SetHearingEnabled`, `SetDisableNotice`, `SetEnableNotice`, `SetVoiceEnabled`, `SetRadioEnabled`, `SetReinforceDisable` are nowhere. |

So the per-soldier switches live in a table that exists for disabled soldiers too: they can be set **before** the soldier is
enabled and are in force from his first update -- no 150 ms of live AI at his post (20.5's likely start of the alert).

**What the guest does now** (`[Proxies]` keys, all default on except `NoAssignRoute`):
- `Inert`: `PR.ProxyEnable(id)` = SetDisableDamage, SetRestrictNotice + SetPuppet, SetEnabled, SetRestrictNotice + SetPuppet again
  (one Lua call); 150 ms later the old ProxyPuppet; re-sent at every (re)realization and every 2 s (`PR.ProxyInert(id, puppet)`;
  an asleep proxy keeps restrict-notice only, since a puppet ignores ChangeLifeState). `NoAssignRoute = true` adds
  `noAssignRoute` to the enable (off: it changes what an enabled soldier is handed, untried).
- `PhaseLock` + `AlertGiveUpSec` (20): every 0.5 s `PR.ProxyPhase()` (vars.playerPhase). On the first non-zero read: a **sweep**
  (`PR.ProxySweep(keep)`: every TppSoldier2/TppHostage2 disabled except the slot soldiers in the world -- the reinforcement, or a
  soldier the mission enabled again) and a **calm** (`PR.ProxyCalm()`: `SetPhase{phase=TppGameObject.PHASE_SNEAK or 0}` to the
  type and to each post id), the calm again every second and the sweep every 3 s while it stays up (MGSV's reinforcements are
  enabled some time into an alert, not at its first read); still up after 20 s: `[Proxies] Enabled` off (as `!proxy off`; the
  Fulton-only proxies stay, and with `[Fulton] Method = proxy` the lock keeps calming -- with `anchor` nothing runs proxies any
  more and the lock stops with them, so its sweep never disables the anchor's soldier). `!proxy on` afterwards starts a fresh
  guard (it can give up again; `!proxy` no longer says `gave up`). `plan::PhaseGuard`, tested.
- `Height = ground` + `HeightTolerance` (2): a proxy stands at his enemy's x/z on the sandbox ground (the centre's height);
  the read-back takes x/z within 1 m and y within 2 m (MGSV's own ground under a far proxy); `Height = enemy` is 20.5.
  `plan::ProxyFeet`, `plan::ReadBackOk`, tested.
- `AimHeightFix`: in Combat.cpp's ShotSolver detour a round whose ray passes within 0.9 m of a flattened proxy's body (2 m tall,
  within MaxDistance + 20 m; the nearest along the ray) is sent in kGEvFire to the same point moved by (enemy - proxy): MGSV's
  aim assist on the stand-in, Elden Ring's hit test on the enemy at his real height. A copy: MGSV's own round is untouched.
  A ray that meets any character's REAL body (same radius and height) is left as aimed: the camera is MGSV's, so a round aimed by
  hand at an enemy on a ledge (or at an enemy behind another's stand-in) already goes where Elden Ring has him, and turning it
  would lift it a second time over his head or onto the wrong character. `plan::CorrectShot`, tested (`!proxy`: rounds turned;
  the first 24 logged).
- kGEvFulton's v0 is the enemy's own place (`Slot::real`), not the flattened stand-in.
- Location ownership: each second an awake proxy's position cache is compared with his location object (x/z, 5 m); 3 misses in a
  row -> found again (`location objects lost` in `!proxy`). The ~30 objects are pooled per realized character: after an
  unrealize the object may carry another character, who must never be moved.
- Console: `!proxy` gains a line (inert, the five sites, height, rounds turned, phase lock: phase, alerts, calms, sweeps, gave up)
  and each slot's enemy height.

**Limits.** MGSV's marker is drawn at the stand-in (Snake's ground height), not on an enemy on a ledge; RestrictNotice's effect,
SetPhase sticking and vars.playerPhase following it are the names' and the code's word only; a reinforcement already in the air may
still arrive (the sweep disables it once it is a soldier).

**Live test (10 minutes, both games; the player's save, Limgrave: Gatefront or the Stormhill road, enemies above and below).**
Before: saves backed up (`pr launch` does it), offline, `[Saves] Block = true`, `[Proxies] Enabled = false` in the ini (turned on
from the console so a bad run is one `!proxy off` away).
1. Boot as usual. `pr lua '!proxy'` -> `proxies off (sleeping characters only ...)`. `pr lua '!proxy on'`; within 2 s the guest
   log (`tools/pr logs` or `PhantomRingGuest.log`) shows `proxy: pool of N soldiers`, the five site checks without a
   `not where this build has them` warning, `proxy: soldier 0x... realized ... standing in for 0x...`, `N of N slot soldiers where
   they should be (0 not)`, `boot: proxies: auto-aim correction on`.
2. `pr lua '!proxy'` every minute: `inert true (sites ok), height ground`, each `proxy` slot `active ... placed ... (where it
   should be)` with `enemy y` differing from its `at` y where the enemy stands on a slope/ledge; `phase lock true: phase 0, 0 alerts`.
   The `where they should be` log line must NOT flap (one line per real change only: an enemy dying or walking out of 60 m).
3. MGSV's own view (`GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 gamescopectl screenshot`): the stand-ins stand still on the apron,
   weapon lowered, no other soldier drawn; no alert music, no radio. The guest log has no `alert: MGSV's alert phase 0 -> ...`.
4. Aim assist: an Elden Ring soldier 15-25 m away on lower/higher ground -> LT with the camera 10-15 deg beside him: the camera
   swings onto his STAND-IN (his x/z on Snake's ground), so in Elden Ring the reticle sits below him (enemy above) or above him
   (enemy below) -- expected. Fire the AR without touching the aim: the guest log `proxy: round aimed at a proxy turned to its
   enemy's height (+x.xx m)`, the host log one hit per `kGEvFire` on that enemy (no double hits, no misses over his head / into the
   ground). Then aim by hand at his real chest in Elden Ring (no LT) and fire: NO `turned` line for those rounds, and still one hit
   each. With two enemies in line (one on a ledge in front, one on the ground behind), a round aimed at the one behind hits him.
5. Marking: aim and mark him (MGSV's marking with the scope/binoculars): MGSV's marker shows in Elden Ring's HUD layer at his feet
   or below him (the stand-in's height: expected), and stays on him as he walks.
6. Ten minutes of ordinary play with enemies around (no stealth needed: shoot, run past). Pass: `!proxy` still `phase 0, 0 alerts`
   (or every alert calmed within a few seconds: `alerts N, calms M`, no `gave up`), Snake never shot by MGSV, no `location objects
   lost` growing every minute, 0 warnings from proxy except during a real alert.
7. If an alert happens: note the time, `!proxy` (sweeps, calms), MGSV screenshot, and whether the phase returns to 0 within ~2 s
   after `phase lock: phase N: SetPhase 0 ...`; during a longer alert `phase lock: sweep` repeats every ~3 s. Not returning (gave
   up after 20 s) = SetPhase doesn't stick: report it. Then `!proxy on`: `!proxy` must no longer show `gave up` and its alert count
   restarts; if the phase is still up the log shows `alert phase 0 -> N`, a sweep at once, and a new give-up 20 s later. `!proxy off`.
8. Fulton through a full proxy: dart an enemy asleep (host `kActorAsleep`), walk up: MGSV's Fulton prompt beside him, Y held ->
   `kGEvFulton a=1 ... (its proxy)` with v0 at the enemy's own height, phase 0 throughout.
Afterwards: `!proxy off`; if 1-8 pass, a one-hour stability run, then `[Proxies] Enabled = true` by default is worth discussing.

### 20.4 Fulton of a sleeping Elden Ring enemy without proxies (`[Fulton] Method = anchor`, `FultonAnchor.cpp`, protocol v12 kGEvFulton; live 2026-10-14, MGSV + fake_er)

(Written before 20.5 existed. The default is now `[Fulton] Method = proxy`: a sleeping enemy gets an asleep proxy soldier at his own
place even with `[Proxies] Enabled = false` -- see the end of 20.5. The anchor below stays as the alternative.)

Player wish (session 6): "FULTON extraction of sleeping Elden Ring enemies". Since soldiers can't be moved (20.3a), Snake moves:
- The host reports a hostile Elden Ring character **asleep** (ActorTable `kActorAsleep`) within `[Fulton] Reach` (3 m,
  horizontal, MGSV coordinates as for proxies) of Snake, who is on foot and playing (no menu, not riding, dead or parked).
- An **anchor soldier** is chosen: `PR.FultonCandidates` lists soldiers standing at the sandbox's height within `CandidateRadius`
  (90 m) of its centre, nearest first (Africa: 0x450, 0x44C, 0x44E, 0x4B9, 0x4BA). He is enabled, put to sleep 300 ms later,
  and read until he lies asleep and still (1.5 s).
- **Snake is placed** with the treadmill's native placement (game thread, Frame.cpp: MGSV's camera kept, the odometer rebased:
  Elden Ring sees no jump; D-Walker moved by the same shift) so that the sleeping soldier lies exactly where the enemy lies
  relative to him, and the treadmill's centre goes with him (no treadmill pulls while anchored). The soldier's draws are never
  Snake's (Capture: within 1.6 m of him and nearer to him than to Snake).
- MGSV offers its Fulton when Snake walks up to the soldier as the player walks up to the enemy. **Y held** -> MGSV's balloon;
  the guest sees GetStatus 0 + GetActionStatus 1 twice in a row (or the soldier `Rise` m above where he lay) and sends
  **kGEvFulton a=1, handle = that enemy's, v0 = the enemy's MGSV position**; the enemy is never anchored again. 3.5 s later
  (Snake's animation) Snake is placed back on the sandbox, his offset from the spot kept within the radius.
- Released without a fulton (soldier disabled again, Snake back) when the enemy is gone, dies or wakes up, Snake walks `Leave`
  (5 m) away, or rides / dies / goes Tarnished / the host goes away.
- Live (fake_er `--actors near`, `pr er actor 0 at 1.5 0.5`, `pr er actor 0 asleep`): `fulton: Elden Ring character
  0xffffffff00001000 'Godrick Soldier' asleep 1.6 m from Snake -- waking soldier 0x450`, 2 s later `Snake placed at
  815.37,-11.95,1211.25 (shift 32.6 m) ... MGSV offers its Fulton (hold Y)` (odometer unchanged: 0.00), two steps toward him ->
  "Y 100% x48" on screen, Y held 2.2 s -> the balloon (screenshot), `kGEvFulton a=1 ... at 816.9,-12.0,1211.8`, fake_er removed
  the actor, `Snake placed at 821.53,-11.95,1179.50 (fultoned)`. `!fulton` shows the state and counts.
- **Timing** (live, soldier 0x44C, status polled every 0.26 s from the Y press): Snake attaches the balloon, the soldier's position
  cache starts rising at 2.5 s (the balloon inflates and he hangs ~2 m up) and stays there until **~8.3 s**, when MGSV yanks him
  away: GetStatus 1 -> 0 and GetActionStatus 0 -> 1 in the same read. kGEvFulton goes at that yank (MGSV's commitment), so Elden
  Ring's own balloon starts ~8 s after the player's Y. Both fultons (open sky; under the terminal's overhang) went through.
- **The position cache lags far soldiers**: it is refreshed at a rate that depends on the distance to Snake (live: 0.9-1.1 m off for
  a soldier lying 33 m from Snake, corrected 1.3 s after Snake was placed beside him). The anchor waits for two reads 5 cm apart
  before placing Snake, and while anchored a soldier read 0.6 m from where he lay makes Snake follow (`soldier 0x450 reads 0.91 m
  from where he lay -- Snake follows`), so the Fulton prompt shows where the enemy lies in Elden Ring.
- **Needs from the host:** `kActorAsleep` on sleeping (and stunned/fainted: MGSV fultons those alike) hostile characters --
  `er/Actors.cpp` sets only Hostile and Dead today, so with the real Elden Ring nothing starts yet. Elden Ring's own sleep (the
  WU S.PISTOL's darts, sleep pots) is what the player would use.
- Limits: each anchor soldier is spent once fultoned (270 in Africa; the next boot has them all again: saves are blocked); a soldier
  under a roof can't be fultoned (MGSV's balloon fails); MGSV's surroundings of the anchor soldier (sandbags, walls) are Snake's
  while anchored (Elden Ring doesn't have them). `!fulton <handle> [fail]` still sends kGEvFulton by hand.

## 21. CQC holds and throws (`[Cqc]`, `Cqc.cpp` / `CqcLogic.h`, `!cqc`; ROADMAP 2.2 tier 1; protocol v13 kGEvCqc; live pass B 2026-10-15)

**The punch's second ~25 HP (README Known issues; checked 2026-10-31, MGSV + fake_er, `!debug on`):** the guest sends one
kGEvMelee per MGSV strike. Its CQC record's +0x114 is the game frame the strike began (a held RT's punch 45506, the next press
26 s later 48671: ~120 a second), rewritten only by a new strike; the attack id +0x118 stays 80 after it. Per press, the trace's
kGEvMelee count: a 150 ms tap 0 (a crouched Snake), RT held 3.5 s 1 (+ kGEvCqc grab 0.35 s and choke 2.85 s later, release),
held + LB 1 (+ grab, interrogate, release), two presses 0.55 s apart 2, a combo continued 2 (kick 83, then 80) -- never two for
one strike. In live pass B the host logged one `Snake CQC kind` per punch and the second ~25 HP came "with no hit of ours
logged", on a held (immobilized) or guarding soldier, never on a free one (knocked out of reach): the host's punch bullet
landing twice on a target that stays inside it is the likely cause (host side, er/Combat.cpp's hit bullet; see the report of
2026-10-31). Nothing to fix in the guest; its one-event-per-strike rule is now `logic::StrikeEdge` (Combat.cpp), pinned by `test_guest_logic` (a 10 s hold with its grab, choke and interrogation: 0 more events; the next press, a combo: one each).

Live pass B (2026-10-15, Gatefront soldiers): RT held -> `kGEvCqc grab (RT held; MGSV attack 0)` 0.36 s after the press, held
on -> `choke` at 2.86 s, LB while holding -> `interrogate`, let go -> `release`, RT with the stick pushed -> `throw`; MGSV logged
`CQC status 0 action callback 1` throughout (never a status 1: `MgsvHold` stays false). Host side in COMBAT.md 7a.

MGSV runs its holds, throws, chokes and interrogations only on its own soldiers, so in the sandbox an RT next to an Elden Ring
enemy is a punch and nothing more. The guest now reads the player's CQC intent every frame on the game thread (after
`combat::PollMelee`) and sends it; the host picks the Elden Ring enemy (COMBAT §7a).
- **Input:** MGSV's own pad bits (`PlayerVars.scannedButtonsDirect`, the same read as `kGuestAiming`): the CQC button is FIRE
  (0x10) or CQC (0x8000) without HOLD (0x8, aiming); interrogate = CALL/INTERROGATE (0x200, LB). What MGSV saw, so Elden Ring's
  menus (a neutral pad) never count. The left stick (0..1 past XInput's dead zone) from the host's PadState.
- **Tracker** (`cqc::Tracker`, pure, `test_guest_cqc`): idle -> RT pressed with the stick past `ThrowStick` = `kCqcThrow`;
  RT held `HoldMs` = `kCqcGrab`; while held: the interrogate bit's press = `kCqcInterrogate` and `ChokeButtons` or `ChokeMs` after
  the grab = `kCqcChoke`, each once per hold; RT up, `MaxHoldMs`, or CQC impossible = `kCqcRelease`; then `CooldownMs`. A button
  held from before the guest's first frame is no press.
- **CQC impossible:** not ready, riding, menu / pause, cutscene, dead / down, carrying a body (RT throws it), in the box, prone,
  falling, first person or binoculars.
- **Event:** v0 Snake's feet, v1 his facing (MGSV world), b `Range` (1.4 m, informational), pad[0] the CQC state's attack id of
  the moment (`combat::Probe`), pad[1] 0 (no proxy target in tier 1).
- **Mapping it live:** while RT is down or MGSV holds someone, every change of the pad bits and of MGSV's own CQC signals
  (PlayerStatus CQC_HOLD / CQC_CONTINUOUS = Lua flag `kLCqc`, the CQC state's action callback `s+0x10`) is logged (64 lines a
  session): `cqc: [holding] pad bits 0x10 -> 0x210, stick 0.00, MGSV CQC status 0 action callback 0 attack 80`. With
  `MgsvHold = true` MGSV's own hold starts the grab at once (an ActorProxy soldier really grabbed); off until a plain punch is
  seen to set neither.
- `!cqc`: on/off, the tracker's state, counts, the last event; `!cqc grab|hold|throw|choke|interrogate|release` sends one by hand.

## 22. Ladders, climb-ups and vaults with MGSV's own moves (`Climb.*`, `ClimbLogic.*`, `[Climb]`, `[Mantle]`, `!climb`, `!mantle`; protocol v15 kHEvClimb / kHEvMantle, kGuestClimbing / kGuestMantling)

Player feedback on the video: "Snake can't jump" and "it switches back to the Tarnished to climb ladders". Elden Ring keeps
driving the physical climb (its ladder state moves the hidden Tarnished; the host draws Snake there, HOST.md); the guest
makes MGSV play Snake's own matching move for as long as it lasts.

### 22.1 Why Snake is staged at real MGSV geometry

- **Direct motion can't reach the moves.** `Player.RequestToPlayDirectMotion{name, {gani path, loop, "", "", "", false}}`
  (0x1409DA7D0: entry 0 = the request's name, entries 1-8 = motions: a `/`-path hashed to a PathCode64, or a name; the 0x128-byte
  request goes to 0x142C1BF48 with bit 4 of 0x142C1C2A8) plays MGSV's own `snapnon_f_idl7.gani` (the mission-clear pose, live:
  the pose's hands rose 0.09 m for ~3.5 s), also when the raw hash is written there from outside -- but nothing from the
  additional motion archives the player loads in a package of 17 (`player2_ladder.mtar` 23 motions, `player2_jump.mtar` 37,
  `player2_elude.mtar` 35, ...; the package parsed in memory, each mtar `72 2B 01 0C`, count at +4, 0x20-byte entries
  `{u64 PathCode64, u32 offset, ...}`): every one of the 23 ladder and 3 CQC hashes was taken (the player copied the request)
  and played nothing. The gani names aren't in memory either (only hashes). So MGSV plays these moves only where they belong.
- **The airport's ladders.** Nova Braga's wall ladders are all one model, `mafr_lddr001` (an 8 m ladder on a building wall).
  Their TppLadder entities found in memory through their DataSet names (`mafr_lddr001_NNNN|MGSLadder00`, a fox::String with
  its chars at +0x1C; the entity's transform, quaternion + translation, follows the reference to it): 11 load with the apron:
  `0005` (916.35, -11.52, 1072.30) yaw 60, `0006` (788.61, -11.83, 1299.85) 60, `0008` (846.66, -11.79, 1029.53) 240, `0009`
  (948.07, -11.77, 1276.28) 0, `0011` (879.31, -11.77, 1240.43) 60, `0012` (867.50, -11.70, 992.67) -120, `0013` (872.20,
  -12.06, 1077.12) 150, `0014` (792.07, -3.74, 1221.29) -120 (on the terminal's roof), `0015` (732.86, -12.04, 1183.95) 150,
  `0016` (691.98, -11.81, 1124.04) 210, `0017` (850.82, -12.05, 1064.40) 149. Two steel towers carry taller ladders
  (`mafr_sttw001_gim_0000|ladderData` at (696.85, -11.77, 1067.82) yaw 156, `_0001` at (795.97, -11.90, 1066.21) yaw 150;
  not measured). The default is `0011`, 85 m from the apron sandbox (`0005`, 143 m, measured the same).
- **Measured on 0005 and 0011 (2026-10-31):** facing the ladder at its foot (the entity's facing x 1.0 m out, facing yaw + 180)
  MGSV shows "Y + ladder"; Y mounts him in ~0.15-0.5 s, hanging with his root 0.5 m above the foot on the climbing line (0.65 m
  out); from the roof (0.2 m behind the foot, 7.92 m up, facing the yaw) Y mounts him going down in ~1.4 s (root 6.0 m up). The
  stick climbs 1.25-1.30 m/s up (rung cycles of +0.18/+0.32 m per 0.2 s) and 1.22 m/s down -- a half stick and L3 the same; he
  steps off the foot below ~0.4 m (0.75 s) and climbs over the top above ~6.8 m (~1 s). PlayerStatus on the ladder: STAND
  without NORMAL_ACTION (WALK while climbing, STOP while hanging).
- **Moving him on it -- nothing does.** The native placement (Hold.cpp) is ignored while he hangs on the ladder (and leaves his
  facing alone everywhere: yaw 90 asked, -90 kept; `Player2SystemImpl::Warp` 0x1409CEAE0 writes the yaw to `[impl+0xB8]+0x274`
  besides the location's set-position and posts a message (type 2) through `impl+0x148` -- vf 0x90, 0x1409C97F0). MGSV's own
  Warp (Lua `TppPlayer.Warp`, `TppPlayer2` `Warp`) moved him along it and off its foot in the first session on 0005 (twice) and
  never again in four later ones, hanging still or not, on 0005 or 0011, after a raw direct-motion request or not: the
  enabling state wasn't found. A Warp sent mid-step was always dropped.
- **Getting him off it.** A direct-motion request (`RequestToPlayDirectMotion` of `snapnon_s_win_idl`, then
  `RequestToStopDirectMotion`) ends the ladder action at once: he lets go and drops (root -8.5 -> the ground in 0.5 s), and
  then the native placement reaches him; MGSV plays its landing (~1 s crouched) wherever he lands (also after a placement:
  the drop is a fall) -- unless the same request comes again right after the placement (he stands). At its ends he leaves it
  as MGSV plays it: over the top (~1 s) or off the foot (~0.75 s).
- **Streaming.** A native placement 85 m away (_0011) runs smooth; 143 m (_0005) stalled MGSV's frames ~0.4 s each way (three
  of three). Lua Warps that far put the guest in "loading" for 11 s once.

### 22.2 The ladder climb (`[Climb]`)

- **kHEvClimb a = 1** (b = 1 down, v0[0] the climb's speed): refused (logged) when MGSV isn't ready, in Tarnished mode, the
  Fulton anchor, riding, dead / a cutscene / a menu / the box, or without the native placement. Otherwise Snake's sandbox spot,
  facing and camera are kept, he is *staged*: the native placement to the ladder's stand point (the foot for up, the roof for
  down), MGSV's Warp to the same spot to face it, a `stance 1` request if he was crouched or prone; once he has stood 150 ms
  (STAND, NORMAL_ACTION) the guest pulses Y. On the ladder (NORMAL_ACTION gone, his root on the climbing line) the left stick
  climbs, paced: pushed while MGSV has climbed less than Elden Ring's speed x time. Speed exactly 0 = Elden Ring's climber is still
  on the rungs: no stick, he hangs still (before 0.6.0 0 meant "unknown" and he climbed on and looped while the Tarnished hung
  still); below 0 or not finite = unknown: MGSV's own pace (the stick always held).
- **Seamless (`[Climb] Seamless = true`, 2026-10-31, `LadderRung.*`).** MGSV keeps Snake's place on a ladder as a rung pair
  (u16, 0.5 m) in the player's 0x50-byte ladder record: `rec = [S+0x60] + (index - [[S+0x28]+0x24]) * 0x50` of the ladder
  system S (vtable 0x1423C6050; `[S+0x40]` == Player2SystemImpl+0x58); `+0x44` the pair he is on (his climbing animation
  plays from an anchor `(pair - 5) * 0.5 m` above the foot -- `!watch` on that attach transform found its writer 0x1411F0410
  and its five callers, the ladder states), `+0x46` the pair he climbs to, `+0x20` the TppLadder data (`+0x10` foot, `+0x30`
  top pair: 16). Live RE: a native placement or a write to the attach transform is undone the next frame; poking `+0x44/+0x46`
  14 -> 8 moved him 3 m down his ladder mid-climb, still climbing. So near either end (up: on or to pair top - 2; down: pair
  7) the guest moves both by 6 (3 m) in one compare-and-swap (`climb::plan::RecycleShift`, `test_guest_climb`): his pose goes
  on unchanged, only his root jumps (more than `kPlacementJump`: not counted as climbed), and MGSV's climb-over / step-off
  never come. S is found once by its vtables (a worker-thread scan of the private heap when the sandbox first plays: 1 match,
  969-1615 MB in 102-139 ms) and re-checked every use; the record's ladder must be the staged one (foot within 1.5 m).
  Live (guest-driven `!climb up|down 1.2`, 18 s each): `21.4 m climbed in MGSV, 6 moved along MGSV's ladder (seamless), 0
  loops`; down `20.8 m ..., 6 moved ..., 0 loops`; every gamescope frame of the up climb (29, 0.6 s apart) shows him on the
  rungs (going down MGSV's own camera stays behind the wall: see the recording instead). `recordings/climb_up.prrec` /
  `climb_down.prrec` (22 s, `climb 1` [down] at t~1, `climb 0` at 19.5): root on the ladder's line every frame from 1.8 s to
  20.1 s (0 frames off it; the 0.5.0 recordings: 27 / 34 during the climb-over / step-off), 22.6 / 22.8 m climbed in ~18 s,
  7 / 6 shifts, root 0-4.5 m (up) / 2.0-8.0 m (down) above the foot, 52-60 draw lists a second throughout. A refused shift (the
  record moved) ends seamless climbing for that climb: it loops as before.
- **Loops (without Seamless, or a refused shift).** MGSV's ladder is 8 m: going up he climbs over its top (~1 s, a climber's
  move) and is placed, faced and mounted again at its foot (~0.5 s); going down he steps off its foot and is mounted again from
  the roof (the top mount, ~1.4 s). So an Elden Ring climb of any length is one MGSV climb with a short MGSV dismount + mount
  every ~5.5 m (`!climb` counts them).
- **a = 1 again** while climbing changes the direction / speed (no new mount) -- unless that climb is already on its way out
  (`Ladder::Ending()`: its a = 0 came, he is leaving MGSV's ladder or going home): then it is a new climb, kept and started 80 ms
  after the last one is over (before 0.6.0 it only changed the dying climb, and the new Elden Ring climb got no MGSV climb). **a =
  0**: `dmoff` (the direct-motion request
  above) makes him let go, and the moment he is off the ladder (NORMAL_ACTION back, his root 0.15 m down or off its line) the
  native placement puts him back on his sandbox spot, MGSV's Warp there turns him the way he faced, and MGSV's camera is turned
  back (`camrot`); right after that placement `dmoff` once more cancels the landing MGSV would play for the drop (~1 s
  crouched; live: with it he stood at once); kGuestClimbing drops when he stands there (or 1.5 s).
- **Home, really** (0.6.0, review 2026-10-31). The way home (kReturn, ladder and mantle alike) ends only with his root within 2 m
  (`plan::kHomeRadius`) of his sandbox spot: a placement that didn't take him there 250 ms on (still on MGSV's ladder -- no
  placement reaches him there -- or it landed nowhere) is tried again, off the ladder first (`dmoff`) when he is still on its
  line, 3 tries in all (`kReturnTries`); then MGSV's own Warp home (Lua, a warning in the log) and the move ends. A refused native
  placement on the way home is retried (it used to end the move where he was: at the airport ladder); anywhere else it sends him
  home. For 5 s after a move, rigid draws near him while he is more than 10 m from that spot are not learned as his
  (`climb::AwayFromHome`: the staging site's props).
- **Elden Ring gone** (0.6.0, review 2026-10-31). Only Elden Ring's a = 0 ended a climb, and an Elden Ring crash or restart sends
  none: Snake stayed staged at MGSV's ladder, the pad owned, the odometer still, until a later ladder. Now a climb ends by itself
  (as a = 0 would) when the host's heartbeat is gone 1 s, a new host session attaches, or after 5 minutes. Live: `!climb up 1.0`,
  then `pr stop --er` -> `climb: ended here -- Elden Ring is gone (no heartbeat)` and `back on the sandbox` 0.18 s later.
- **While a move plays** (`state::Staged()`): the odometer is still (`Odometer::Still` instead of `Update`: Elden Ring sees none
  of it), velocity 0; GuestState's camera is the one he had when it began, at the same place relative to his root (MGSV's
  swings round the wall); the buddy's position is published as `buddyPos + (root - home)` (where it stands relative to Snake's
  sandbox spot); no treadmill, jump, dive, buddy whistles, Fulton anchor or proxies; the guest owns the pad
  (`pad::SetOwned`: neutral, its own stick, the Y pulses -- the player's pad is Elden Ring's ladder's); MGSV's HUD layer keeps its
  last frame (its "Y" prompt and ladder icon never reach Elden Ring) until `[Climb] HudThawMs` (150) after it ends -- 300 ms before
  2026-11-03. Measured that day (`!climb hudthaw 0`, `pr record --hud-every 0`, every HUD frame against the HUD 1 s later): after a
  ladder climb MGSV's "Y" + ladder prompt is still there, fading, at +36 ms (773 of the prompt area's samples differ), +53 (612), +70
  (499), +87 (144) and gone at +103 ms; after a vault and a climb-up there is none at +36 ms. The odometer's own settle stays 300 ms.
- **After it** (review of 0.6.0, `plan::OdoThaw`, `TestOdoThaw`): the odometer stays still for the settle after the placement
  home (MGSV's root drifts 0.5-0.9 m forward there), until the player's own left stick is pushed past the dead zone or 300 ms --
  before, the whole 300 ms whatever the player did, on top of the way home: the stick did nothing for 0.2-0.5 s after every
  ladder. A stick held through Elden Ring's end never counts (the host keeps it neutral until let go). `climb::OdometerStill()`
  for Frame.cpp; `state::Staged()` (no treadmill, jump or dive) still lasts the 300 ms (MGSV's HUD thaws at `HudThawMs`, 150). Logged `climb: the
  player's stick N ms after the move -- Snake's motion counts again`; `!climb` counts them. Live with both games 2026-11-01
  (Stormveil's 14.5 m ladder): the stick pushed 35-53 ms after Elden Ring's end, the guest home 195-205 ms after it, the stick
  counted 110-122 ms after that (when the host's puppet starts and its pad reaches MGSV, 301-317 ms after the end), Snake walking in
  MGSV 440-522 ms after the end (MGSV's own first step).
- Console: `!climb up|down [m/s]`, `!climb off`, `!climb` (state, counts, the last climb's summary); fake_er / `pr record`:
  `pr er "climb 1|0 [down] [speed]"`.

### 22.3 Climb-ups and vaults (`[Mantle]`)

- **kHEvMantle** (a = height cm, b = 1 vault, v0[0] Elden Ring's duration): the site by kind and height -- `Vault` for vaults;
  for climb-ups `Low` up to `LowMaxCm`, `High` above, either alone when only one is set, and with neither the vault site *cut at
  its top* (the first half of MGSV's climb-over is a climb-up: up onto the obstacle, hands on top, a knee up) -- staged there
  facing it (native placement + Warp for the facing), Y once he has stood 150 ms; kGuestMantling from the request until he is
  back home.
- **The end of the move.** A vault is over once he is down on its far side (his root back at the start's height after rising
  0.5 m); a climb-up on the vault site once he is on top (risen 0.5 m and starting down). Then `dmoff` ends MGSV's move (its
  far-side drop and the ~0.7 s recovery MGSV adds), the native placement puts him home, `dmoff` again (no landing), facing
  and camera back. MGSV's natural timing, for the host to match (live 2026-10-31, the jersey barrier, 1.0 m): Y -> his root
  +1.0 m (on top, hands on the barrier) at ~0.5-0.7 s; over and down the far side 2 m on at ~1.1-1.5 s; NORMAL_ACTION back at
  2.2 s. So a climb-up shows ~0.6-1.0 s of MGSV's move (the cut came 1.03 s after Y), a vault ~1.5 s. Elden Ring's v0[0] isn't
  used to stretch it (MGSV plays at its own speed); kGuestMantling covers the whole staging (~2-2.5 s in all, the staging and
  the stand-up before Y included).
- **Sites.** The vault: the terminal's jersey barrier, stand (826.39, -11.95, 1218.62) facing 90 ("Y + step" shows; MGSV
  turns him to 60, the barrier's normal, as he climbs). **Climb-ups that end on top (2026-10-31)**, found by walk probes
  (Snake warped, walking forward pulsing Y, his root tracked) in the storage yard under the shed ~70 m north-east of the apron:
  `Low` = a wooden crate, stand (843.05, -11.77, 1244.75) facing -90, 103 cm; `High` = an oil drum, stand (854.15, -12.0,
  1243.76) facing -90, 119 cm (frames: hands on the drum, a knee up, standing on it). MGSV plays its climb-up for both (on top
  0.5 / 0.4 s after the climb starts, Y to on his feet on top ~2.0 s: `MGSV's move took 1986 / 2011 ms ... rose 1.12 / 1.20 m`);
  a stand 0.2-0.5 m further from either gets `MGSV offered no move there`. `LowMaxCm` 110: up to 110 cm the crate, above it
  the drum. MGSV's climb-up looks the same at both heights (no lower step-up or higher hang-and-pull found near the apron);
  the barrier's first half (cutAtTop) is the fallback with neither site. **`Step` (2026-10-31 night)**: a wider walk-probe search
  of the same yard (warped, walking with Y pulsed, the root tracked; `!treadmill off`) found the stacked sacks beside the crates:
  stand (843.93, -11.78, 1241.65) facing -90, 75 cm. MGSV's move there is a knee onto the sacks, no hands on top, up in 0.2 s (root
  +0.75 m between 0.38 and 0.58 s after Y), then the same stand-up on top: `mantle #1: 70 cm climb-up ... Snake staged at MGSV's 75
  cm obstacle` -> `MGSV's move took 2002 ms (Y to back on his feet), his root rose 0.77 m`; frames: the knee up, standing on the
  sacks. A climb-up up to `StepMaxCm` (89) takes it, the crate up to 110, the drum above. No separate "step over" action exists
  near the apron as far as the probes reached: from the same point, walking into the crates (yaw 0) ended 1.87 m up on their stack,
  south (yaw 180) he went over a ~1 m one (+0.99 m, then down 1.5 s after it began). `!mantle site step|low|high|vault <x> <y> <z> <yaw> <cm> | none` sets one live. `recordings/mantle_low.prrec` / `mantle_high.prrec` (`mantle 80 0.8` / `mantle 160 1.2`):
  kGuestMantling 2.48 / 2.30 s, his root on top (+1.21 / +1.14 m) from 1.0 / 0.8 s to the end (MGSV's stand-up on top), then
  home. The host now paces its climb-up to it (0.6.0: `[Mantle] climb_base_s` 1.8 + `climb_per_m` 0.2: 1.71 m -> 2.14 s; live
  `climb up done` 8 ms from MGSV's `move over`).
- Console: `!mantle <cm> [vault] [secs]`, `!mantle`; `pr er "mantle <cm> [vault] [secs]"`.

### 22.3a Live with the real Elden Ring host (2026-10-31, docs/STATUS.md "0.5.0-alpha")

- **Ladders**: Stormveil's 14.5 m ladder and Fort Haight's 19 m tower, up and down: `climb #N: up|down at 1.20 m/s -- Snake staged`
  within 1-12 ms of `kHEvClimb`, on MGSV's ladder 0.13-0.5 s after Y, 1-2 loops per climb, `back on the sandbox` 0.18-0.23 s
  after `a = 0` (1.6 s once, when the end came during a mount from the roof). The host draws him on Elden Ring's ladder.
- **Climb-up / vault**: Church of Elleh, 1.09 m (`mantle #1: 109 cm climb-up ... on top: cut there`, 1.3 s staged) and 0.52 m
  (`52 cm vault ... down on the far side`, MGSV's move 1185 ms from Y).
- **Staging-site props** (`Capture.cpp`): staged at the jersey barrier the barrier stood within 2.5 m of Snake's root and was
  captured as his -- Elden Ring drew it under his vault. While `state::Staged()`, a rigid (unskinned) draw is his only if its
  mesh was his on the sandbox before (his weapons and gear: they always are); skinned draws always are. `!capture` counts
  `staging-site props dropped` (335 in one vault); none showed in the frames after the fix.
- The player's Y at a ledge no longer reaches MGSV (HOST.md "Climb up and vault": `InputBridge::SetLedge`): it had mounted
  D-Walker following ~2.5 m away before `kHEvMantle` arrived; the guest's own Y at the staging site is what plays the move.

### 22.3b Live with both games, 0.6.0 (2026-10-31 evening, docs/STATUS.md "0.6.0-alpha")

- **Ladders, seamless**: Stormveil's 14.5 m ladder (AEG219_067_4000) up `+14.6 m` in 18.9 s with a 5 s stop on the rungs half
  way (`climb: now up at still (on the rungs)`, frames: the same pose throughout the stop), `15.0 m climbed in MGSV, 4 moved along
  MGSV's ladder (seamless), 0 loops`; down `-14.5 m` in 14.0 s, `15.7 m ..., 4 moved ..., 0 loops`; `back on the sandbox` 0.19-0.20 s
  after a = 0.
- **A new climb while one ends** (`!climb off` then `!climb up` 70 ms later): `a new climb (up) while the last one ends` -> `climb
  #4` 84 ms after `back on the sandbox`, on the ladder 0.3 s later.
- **Climb-ups**: Church of Elleh rubble, by Y: 1.71 m -> the drum (`MGSV's move took 1988 ms ... rose 1.20 m`), 0.57 m -> the crate
  (`2002 ms ... 1.12 m`); a 1.15 m one from the console -> the drum (1971 ms). Y went through the host's ledge latch (`2 Y presses
  kept from MGSV`).
- **Placed explosives at a death**: a sleep mine set, then the Tarnished killed (`call kill yes`): `placed #1: equip 418 dropped (the
  Tarnished died) -> kGEvPlacedGone; in MGSV removed`, the host's `mine 1 gone (MGSV)`, nothing announced after the respawn.

### 22.4 Quick throws (`[Input] RtTapMs`, README Known issues)

A short RT press while aiming a throwable (LT held, a support item -- grenade, C4, mine, decoy -- in hand: the game thread tells
the pad thread every frame, `pad::SetThrowableInHand`) is held on to MGSV for `RtTapMs` (120 ms), LT with it; a gun's tap is left
alone (one shot). Live 2026-10-31 (MGSV + fake_er, the save's sleep grenade, equip 378): 14 of 14 aimed throws with 30-60 ms RT
presses (`!pad`: `RT taps held to 120 ms while aiming: 14`), where the 2026-10-26 run had about one throw in 20 short presses.
