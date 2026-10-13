# Live test plan: phases 0 and 1 (link, drive, D-Walker groundwork)

The first run of PhantomRing inside the real games. Ordered cheapest first: **MGSV alone against
`fake_er.py`**, then **Elden Ring alone against `fake_mgsv.py`**, then **both**, then the opt-in RE
features. Each step names the command, what proves it (log line, console answer), and what to do if it
fails. Stop at the first failure that blocks the next step, fix, rebuild, and repeat that stage.

Conventions:

- `$PR` = the repository root; commands run from there. `tools/pr` is the CLI.
- **G-log** = `<MGS_TPP>/PhantomRingGuest.log`; **H-log** = `$PR/build/PhantomRingHost.log`;
  `tools/pr logs` tails both plus the launch logs (`logs/current/`).
- Host settings are edited in `er/PhantomRingHost.ini` (the build copies it to `build/`, run
  `cmake --build build`); guest settings in `<MGS_TPP>/PhantomRingGuest.ini` (installed from
  `mgsv/PhantomRingGuest.ini` by `pr launch`; a hand-edited copy there is kept).
- Every in-game state not yet seen live is marked UNVERIFIED in the code; this plan is the list of them.

## 0. Before anything (5 min, no game)

```
cd $PR && cmake --build build
cmake --build build --target check_host check_guest
python3 tools/phantomring/test_layout.py && python3 tools/phantomring/test_fakes.py && python3 tools/phantomring/test_plugins.py
(cd build && wine verify_er_addresses.exe 'Z:<steam-library>\common\ELDEN RING\Game\eldenring.exe' && grep 'address table' verify_er_addresses.log)
python3 -I mgsv/verify_addresses.py && python3 -I tools/re/verify_backlog.py
```

Pass: every suite says PASS / all passed; `address table: 56/56 verified`; `all signatures match`;
`0 problem(s)`. Then start Steam (both games call SteamAPI_Init) and run `tools/pr launch --dry-run`: every
line `[ok]`.

## 1. MGSV alone + fake_er.py (the guest)

```
tools/pr launch --mgsv-only          # backs up saves, creates /dev/shm/phantomring_v1, installs dinput8.dll, starts MGSV in headless gamescope
tools/pr fake-er --hurt-every 0      # second terminal: stand-in host (stamps the session, heartbeats, scripted pad loop)
tools/pr watch                       # third terminal: live header/state view
```

fake_er's pad loop (10 s): left stick forward 3 s, right 3 s, tap A, hold RT 0.7 s, neutral. The
guest only passes it to Snake after Ready.

### 1.1 Activation and link

| Check | Proof |
|---|---|
| dinput8 proxy loaded natively | `logs/current/steam-287700.log`: `dinput8` loaded from `MGS_TPP`; G-log exists |
| guest activated | G-log: `PhantomRing guest 0.1.0 activating`, `identity: sha256 matches mgsvtpp.exe 1.0.15.4` |
| hooks verified | G-log: no `signatures differ` / `failed validation` lines; `late init done`; `pad: XInputGetState import now returns the shared pad` |
| link | G-log `link: attached to host session 0x…`; `pr status`: guest unix pid non-zero, guest heartbeat < 100 ms, status text moving |
| no window/sound on the desktop | look; note if MGSV audio is audible (muting is not handled yet) |

If the guest never attaches: `pr status` shows host magic/version (fake_er must be running) and the
guest's status text; protocol mismatch is logged as `host speaks protocol vN`.

### 1.2 Consoles

```
tools/pr lua '!status'                       # works before Lua is up (worker thread)
tools/pr lua 'return vars.missionCode, vars.playerLife, vars.buddyType'
tools/pr lua 'return BuddyType.WALKER_GEAR'  # must be 4
```

Pass: `!status` answers immediately; the Lua line answers once G-log shows `lua: game VM … found` and
`lua: embedded script loaded`.

### 1.3 Boot to the sandbox (time each step)

Watch the status text in `pr watch` and G-log `boot: … -> …` lines:
`MGSV: starting` → `at the title screen, pressing Continue` → `loading the saved game` →
`loading Mother Base free roam (30050)` → `arriving in …` → `preparing the sandbox` →
`deploying D-Walker (…)` → `ready (Mother Base, D-Walker deployed)`.

| Check | Proof / note |
|---|---|
| A pulses pass logos, title, dialogs | reaches `loading the saved game`; record pulse count and any dialog that stopped it (`pr screenshot` cannot see gamescope headless: use the status text and `pr lua 'return gvars.ini_isTitleMode'`) |
| 30050 loads directly | `boot: load mission 30050 (direct): …`; if `reserve` was needed, note it |
| soldiers gone, invincible, clock, weather | `boot: setup: …` counts; `pr lua 'return vars.playerLife'` stays put |
| D-Walker | `boot: D-Walker: …` steps end in `active`; G-log names the GameObject class that resolved (`TppCommonWalkerGear2` or `TppWalkerGear2`); `pr status` shows buddy WALKER_GEAR, flags Present|PosValid, a position ~4 m behind Snake |
| no D-Walker in the save | `missing: not developed` and Ready without buddy (kBuddyMissing) |
| Ready | `kGuestReady` in `pr status`; `boot: sandbox centre x, y, z (radius 15 m)` |

### 1.4 Pad injection and motion (odometer, treadmill)

With Ready, fake_er's scripted stick drives Snake.

| Check | Proof |
|---|---|
| pad reaches MGSV | `!status` `pad polls` climbing; Snake's `root` in `!status` moves during the stick phases |
| odometer | `pr watch` odo grows ~ 3–6 m per stick phase; fake_er prints the puppet moving; no jumps |
| treadmill | after ~15 m, G-log debug `treadmill: warp …` (`!debug on`); `!status` `warps` increments, `teleports` increments by the same, odo has **no** jump; `treadmill: warp didn't land within 3 s` never appears |
| A / RT reach MGSV | RT phase: `shots:` lines (1.6) appear if a weapon is out |
| hold modes (later, 4.3) | leave `Hold = treadmill` for now |

### 1.5 Camera, FOV, skeleton (guest side)

| Check | Proof |
|---|---|
| camera hook | G-log `camera: player camera owner 0x…`; `!status` shows `fov`, `focal` |
| FOV | `focal` ≈ 21 normal, 22 shoulder, 26 hiding, 32 CQC (MGS5VR); compare `fov` with MGSV's look; set `[Camera] FilmHeightMm` |
| yaw | `!status` root `yaw` vs `(vars …)`: same angle (sign/offset noted) |
| skeleton | G-log `skeleton: first pose published (178 bones, parents ok)`; fake_er prints pose bone count 178 |

### 1.6 Combat, life, saves, window

| Check | Proof |
|---|---|
| shots | fire tranq pistol / rifle / shotgun singly (RT phase or `pr lua` equip): one `kGEvFire` per round in fake_er's output, kind tranq for the tranq pistol (bullet 10/11), pellets > 1 for the shotgun; G-log `shots: ShotSolver from 0x…` lines while aiming are previews (not sent) |
| D-Walker gun | ride D-Walker and fire: which `ShotSolver from 0x…` callers appear (re-backlog M5: candidates 0x14103F907 / 0x14109415E); `kGEvBuddyFire` either from FireWeapon or the ammo counter |
| CQC | one `kGEvMelee` per strike (G-log debug `melee: serial … attack …`) |
| hurt → life | restart fake_er with `--hurt-every 4`: life drops by 50 × 4 = 200 per hurt; no `native playerLife write did not stick`; at life 1 one `kGEvDied` (`life: Snake would have died`) |
| saves blocked | `pr lua 'TppSave.VarSave() return 1'` or a checkpoint: G-log `dropped Steam Cloud write …`, no dialog; `userdata/…/287700/remote` mtimes unchanged |
| window | MGSV never minimises (G-log window guard lines if any) |

End: `tools/pr stop` (kill fake_er with Ctrl-C). `pr stop` must leave no `mgsvtpp.exe`, gamescope or wineserver.

## 2. Elden Ring alone + fake_mgsv.py (the host)

```
tools/pr launch --er-only            # me3 + PhantomRingHost.dll, offline, PhantomRing0000.sl2
```

Load the PhantomRing character at the title. **Do not start fake_mgsv yet** (2.1–2.3 need ER driving
the Tarnished normally).

### 2.1 Load, addresses, tasks (at the title)

H-log, in order: `exe identity OK`, `address table: 56/56 verified`, `shared memory … attached, session …,
host unix pid N` (N ≠ 0), `input: XInputGetState hooked`, `system init done after … ms`, four
`task … registered in group` (0, 78, 107, 117), `PhantomRingHost ready`.

```
tools/pr er status        # task counters FB/PP/CAM/POST climbing
tools/pr er 'peek player.pos'
tools/pr er sym           # every symbol resolves once in game (menu.blocking remo actionbtn dmgmod …)
```

If a task never runs: the counter for that group stays 0 — try another `[Camera] task_group` (106/144).

### 2.2 Plain Elden Ring still works, calibration

Play normally for a minute (run around, open/close the Start menu, rest at a grace, talk to an NPC):

| Check | Proof |
|---|---|
| ER fully controllable | no puppet without a guest (`pr er status`: `puppet off (ready to start)`) |
| pad published | H-log `input: publishing XInput pad 0 to MGSV`; `pr status` pad section changes with the sticks |
| calibration | H-log `calibration: ER camera … m from the player` (2–6 m), `ER character forward (+Z) = …`, `camera +0x30 row is forward = …` |
| E4 menu flag | every Start menu / inventory / map / grace / shop / talk / message → `ER blocking menu open` … `closed`; note any menu that does **not** flip it (MenuWatch's learned states still cover those) |
| E4 cutscene | a cutscene (e.g. Margit's intro, or any boss fog intro) → `ER cutscene playing` … `over` |
| E6 prompt | stand at a grace / door / item / NPC → `ER interaction prompt shown (ActionButtonParam N)`, `gone` when walking away; N differs per object |

### 2.3 Puppet, motion, camera, skeleton with fake_mgsv

```
tools/pr fake-mgsv                   # stand-in Snake: walks a 10 m circle + your left stick, D-Walker ridden 4 s of every 12
```

| Check | Proof |
|---|---|
| guest attach | H-log `guest attached to session … (attach #1, guest unix pid N)`, `guest status: ready "fake MGSV: …"` |
| puppet start | H-log `puppet ON (kinematic motion): delta …` with the calibration line; Tarnished disappears (shadow/hitbox stay; if not, `hide_mode = fade`) |
| ER pad masked | sticks/face buttons no longer move the Tarnished; the left stick now bends fake Snake's circle (fake_mgsv adds it) |
| motion through collision | the puppet follows the circle; walking it into a wall stops/slides it; small steps climbed (< 0.5 m), ledges fall; `pr er status` shows grounded/airborne; `pr er 'call ray <x> <y+1> <z> 0 -5 0'` hits the ground (ray filter 0x2000058) |
| skeleton | green lines stand on the puppet (178 bones, facing yellow line); no mirror (else `[Axes] mirror_x = -1`), yaw right (else `mgsv_yaw_sign = -1`); if nothing draws: `pr er ez` |
| camera | H-log `camera: driving pers_cam_1 (…)`; the view orbits as fake_mgsv's camera does; walls pull it in (`maxDist` in `pr er status` < boom); FOV sane (else `fov_scale`) — if the view does not follow, `task_group = 144` |
| buddy | `pr er buddy`: D-Walker (4), `placed from guest pose`, cyan marker on ER ground beside the puppet; `puppet: Snake mounted` / `dismounted` every 12 s; `Riding` raises speed/step limits |
| shots | H-log `Snake fired weapon … -> …` every 2 s and `buddy fired …` every 3 s, red tracer lines |
| Start menu | Start opens the ER menu, navigable with the stick (ER gets the whole pad while `blocking` is open); fake_mgsv sees a neutral pad meanwhile |
| prompt | at a grace with the puppet: press A → ER's interaction happens (rest/open); `pr status` pad buttons show no A (0x1000) while the prompt is up (MGSV's side is checked in stage 3) |
| hurts | let an enemy hit the puppet: H-log debug `puppet: hurt N (kind …)`; HP bar mirrors fake Snake's life; fake_mgsv's Died at life 0 → H-log `puppet: killed the ER player` → YOU DIED → `ER player respawned` |
| guest lost | Ctrl-C fake_mgsv: within 3 s `guest LOST`, `puppet OFF (guest lost)`; Tarnished visible and controllable that frame |

## 3. Both games

```
tools/pr launch                      # MGSV hidden + Elden Ring
```

Load the ER character while MGSV boots (`pr watch`: guest status text goes through §1.3). When the guest
reaches Ready and the character is in the world, the puppet starts by itself.

| Check | Proof |
|---|---|
| host and guest attached | `pr status`: both alive, both unix pids, heartbeats < 100 ms; `pr lua` and `pr er` both answer |
| real pad → MGSV | the controller moves Snake (G-log `!status` pad polls climbing); ER ignores sticks |
| intended motion → puppet | running Snake moves the puppet at MGSV speed through ER collision (walls, slopes, ledges); crouch-walk, crawl, dive, roll all move it |
| treadmill invisible | no hitch in the puppet's motion at Snake's ~15 m warps (odometer continuity) |
| skeleton on the puppet | the debug skeleton animates exactly like Snake (run cycle, crouch, prone, CQC), at the puppet's feet, same facing |
| camera = MGSV camera | over-the-shoulder, aim zoom, crouch/prone framing, CQC/dive cameras all come through; ER walls pull it in |
| ER menus | Start / View → ER menu with the whole pad; Snake stands still meanwhile (neutral pad) |
| prompts | A at a grace/door rests/opens and Snake does nothing |
| D-Walker | spawned behind Snake in MGSV (`pr status` buddy PosValid), cyan marker in ER on the ground beside the puppet; mount it in MGSV → `puppet: Snake mounted`, puppet moves at D-Walker speed; its gun → `buddy fired …` tracers |
| hits and death | ER enemy hits → Snake's life drops (`pr lua 'return vars.playerLife'`), HP bar follows; Snake's "death" → YOU DIED in ER → respawn → Snake's life refilled |
| quit | close MGSV (`pr stop` or kill): `puppet OFF (guest lost)`, Tarnished back in the same frame |

## 4. Opt-in RE features (each on its own run)

### 4.1 E1 root motion: `[Puppet] motion = root_motion`

H-log: `root motion: PlayerCtrl … slot 12 -> PhantomRing (instance vtable copy)` and
`puppet ON (root motion)`. Then (re-backlog E1 live test):

- `pr er status`: `root motion ON: N hook calls, last X ms ago` — calls ≈ 1 per frame, last < 50 ms.
- `pr er 'peek phys+0x388 u32'` = 2 while Snake moves, 1 when idle.
- Run Snake straight 3 s at ~6 m/s: the puppet covers ~18 m; walls stop it and it slides; slopes and
  stairs lift it; ledges drop it with gravity (and fall damage arrives through the HP-delta path).
- Turn Snake 90°: the puppet moves where Snake moves (if it moves rotated, the conjugate is wrong: use
  `q` instead of `conj(q)` in `er/RootMotion.cpp`).
- D-Walker dash (~15 m/s): note if the speed is capped.
- If `the root-motion hook has not run for 1 s` appears, the puppet fell back to kinematic: note when
  (fog wall, cutscene, ladder).

### 4.2 E2 hit hook: `[Puppet] hit_hook = true`

H-log `hit hook: CSPlayerDamageModule … slot 7 -> PhantomRing`. One debug line
`puppet: hit hook N HP (kind …, atk …) from D m` per landed hit, the hidden Tarnished does not stagger, ER
HP only follows Snake's life; arrows/spells too; poison and falls still arrive (HP-delta path).
`pr er status` shows `hit hook ON: calls, cancelled`.

### 4.3 M1 hold modes (guest): `[Motion] Hold = native_treadmill`, then `native`

G-log `hold: player location object …`. With `native`: Snake runs in place (animation plays, `vars.playerPos`
stays at the centre), the odometer still advances, no foot sliding or stuck idle (re-backlog M1).
Crouch/crawl/dive/CQC near the centre behave.

## 5. What to record (feeds the next build)

- Pinned ini values: `[Axes] mirror_x`, `mgsv_yaw_sign`, `er_forward`, `mgsv_root_offset_y`;
  `[Camera] forward_row`, `task_group`, `fov_scale`; `[Input] menu_ui_states` (learned lines);
  guest `[Camera] FilmHeightMm`.
- E4: which menus flip `+0x798`, which need the learned UI states; loading screens (no flag yet).
- E6: param ids seen; whether A pass-through felt right.
- M5: D-Walker GameObject class, CallBuddy result, which code fires its gun.
- Timings: MGSV boot to Ready; hitches at treadmill warps.
- Logs: copy `logs/current/`, G-log, H-log into the test notes.
