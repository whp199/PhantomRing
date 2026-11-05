# PhantomRing Linux tools (`tools/pr`, `tools/phantomring/`)

Stdlib-only Python (3.11.4 or newer). `tools/pr` runs `python3 -m phantomring` with `tools/` on `PYTHONPATH`, so it
works from any directory.

| File | What it is |
|---|---|
| `proto.py` | ctypes mirror of `protocol/phantomring_protocol.h` v2 (every struct, constant and enum, incl. the v2 GuestState buddy block), the mmap of `/dev/shm/phantomring_v1`, seqlock read/write, `EntryRing`, the render byte ring, triple-buffer reader/writer, console request/serve |
| `cli.py`, `__main__.py` | the `pr` command |
| `launch.py`, `vdf.py` | launcher: Steam/Proton discovery, preflight, save backups, guest install, start/stop, logs |
| `profile/phantomring.me3` | me3 profile for Elden Ring |
| `fake_er.py`, `fake_mgsv.py`, `luasubset.py` | stand-ins for each plugin (both run natively on Linux) |
| `test_layout.py`, `test_fakes.py`, `test_launcher.py` | offline tests (see the end of this page) |

The mapping path defaults to `/dev/shm/phantomring_v1`. `--path` or `$PHANTOMRING_SHM` overrides it for
the tools and fakes. The plugins always use their built-in default.

## Commands

```
pr status                      # header, both sides (status, unix pid, heartbeat age), states, pad, pose, rings, consoles
pr watch [-n 0.5]              # the same, refreshed, with lists/s and fps of each side
pr lua 'return vars.playerPosX, vars.buddyType'   # Lua run on MGSV's Lua thread (guest console)
pr lua -r vars.playerLife      # -r prepends "return "; -f file.lua sends a file
pr lua '!status'               # lines starting with '!' are guest worker commands (served without Lua, see mgsv/Link.cpp)
pr er 'peek player.pos'        # command run inside Elden Ring (host console; the language is er/Console.cpp's)
pr launch --dry-run            # preflight + the full plan and commands; starts nothing, writes nothing
pr launch [--mgsv-only|--er-only] [-W 1280 -H 720] [--wait] [--proton-debug WINEDEBUG] [--er-headless WxH]
                               # [--keep-backups N] [--keep-logs N]: save backups per game / logs/ sessions kept (10 each)
                               # --er-headless: test automation on a machine whose displays are off (Elden Ring then
                               # presented ~1 frame/s on the desktop): Elden Ring in its own headless gamescope too;
                               # screenshots with GAMESCOPE_WAYLAND_DISPLAY=gamescope-N gamescopectl screenshot F.png
pr er-continue                 # through Elden Ring's title: pad A and keyboard Enter every 4 s until it loads
                               # (first `pr er "input virtualpad on"`: the test virtual pad, below)
pr stop [--er|--mgsv] [--kill] # SIGTERM what pr launch started (games first), --kill SIGKILLs the rest after --timeout;
                               # both games, whichever recent launch started each (after `launch --mgsv-only` next to a
                               # running Elden Ring it used to stop MGSV only); --er / --mgsv: only that game. A recorded
                               # pid counts only while it still carries its launch's PHANTOMRING_LAUNCH_ID (pid reuse)
pr logs [-n 30] [-f]           # tail logs/current/*.log (gamescope/me3 output, Proton logs) and the plugin logs
pr backup-saves [--er-only|--mgsv-only]   # a backup made by hand: never pruned by pr launch
pr restore-saves <timestamp> [--er-only|--mgsv-only] [--all-er-files] --yes
                               # without --yes: lists every file it would overwrite. Elden Ring: only PhantomRing0000.sl2
                               # (+ its .bak) unless --all-er-files (the backup holds the whole save folder: ER0000.sl2, .co2, ...);
                               # a PhantomRing0000.sl2* the backup doesn't hold (created after it: a first launch's backup) is
                               # removed (it is in the pre-restore safety backup); the dry run lists it ("removes ...")
pr uninstall-guest             # removes MGS_TPP/dinput8.dll (+ PhantomRingGuest.ini) only if it is ours: a sha256 we installed,
                               # or any PhantomRing guest build (its build tag); another mod's dinput8.dll is left alone
pr --version                   # the tools' version (VERSION + commit, -dirty for uncommitted changes) and both built DLLs'
pr doctor                      # read-only machine check, one [ok]/[warn]/[FAIL]/[info] line each with the fix: Steam, both
                               # games' exact versions, Proton/SLR, gamescope, me3 + profile, the build vs the tools, the guest
                               # in MGS_TPP, saves, backup size, /dev/shm, MGSV offline (region/ToS), D-Walker (last run's log)
pr collect-logs [-o F.zip]     # logs/phantomring-logs-<time>.zip for a bug report: plugin + launch logs (tails), inis,
                               # versions, doctor, status; home/user/host/Steam ids redacted; never saves or game files
pr screenshot [-o file.png] [--mode full|active|monitor]     # spectacle -b -n -f -o <file>; prints the path
pr install-me3 [--archive me3-linux-amd64.tar.gz]          # extracts the verified me3 0.13.0 into third_party/me3 (launch does it on demand)
pr fake-er [...] / pr fake-mgsv [...]                         # run a stand-in peer (see below)
pr record --out F.prrec --seconds N [--pad 'ly=32767']        # host stand-in next to a real MGSV: saves its assets, draw lists, pose, state, HUD
                                                              # .prrec files contain MGSV's assets (Konami): never commit or share them
pr replay F.prrec [--loop]                                    # guest stand-in: replays a recording to a host (docs/GUEST.md 18.1)
```

### The test virtual pad (`pr er "input virtualpad on"`)

Scripted runs drive Elden Ring and MGSV with `pr er "press <buttons> <ms> [lx ly [rx ry]]"` through the host's own XInput pad:

- `input virtualpad on` (= `exclusive`, since 0.6.0): user 0 is a connected neutral pad and **every real controller is ignored**
  -- its sticks, triggers and buttons reach neither Elden Ring nor MGSV (XInput users 0-3; a game controller Elden Ring might read
  through DirectInput is made neutral too), and MGSV's rumble no longer shakes it. Unit-tested (`TestVirtualPad`); live 2026-10-31
  the DualSense had been unplugged just before the run, so the hiding of a real pad itself is UNVERIFIED live (none was there;
  nor in the integration run of 2026-11-01: `real pad polls hidden 0` throughout). Found 2026-10-31: the player's DualSense, plugged in
  with its left stick resting pushed, walked Snake around, opened Elden Ring's menus and talked to Renna in the middle of a scripted
  run, because a `press` only adds its buttons (and a non-zero stick) to the real pad. `pr er status` ends with `virtual pad
  exclusive (real pad polls hidden N)`; `input kbm` counts DirectInput game-controller reads.
- `input virtualpad shared`: the old `on` -- the virtual pad only while no real one answers on user 0 (what `[Keyboard]
  virtual_pad` turns on for a player with no controller: players are never affected by `exclusive`).
- `input virtualpad off` at the end of every run. A real pad works again at once.
- With no controller connected Elden Ring polls no pad at all; turning the virtual pad on now tells it a HID device arrived
  (docs/HOST.md, `input virtualpad`), and it polls the virtual pad every frame from then on.
- The keyboard still works meanwhile (`input key|keydown|keyup <vk>`; arrows and the other extended keys are sent as such since
  0.6.0 -- before, the down arrow reached Elden Ring as Numpad 2 and its menus never moved).

Other test commands of the host console (docs/HOST.md "Dev console"): `die [snake|er]` (a test death), `ladder early on|off`
(the climb's end told to MGSV at Elden Ring's EndTop/EndBottom, or after the settle as before 0.6.0), `mantle set <key> <m>`.
D-Dog (docs/HOST.md "D-Dog in Elden Ring"): `dog all on|off` (the whole `[Dog]` layer for a session, not saved), `dog pick` (every
character around with the reason he is or isn't the dog's pick: run it with the camera on an NPC to see he never is), `dog test boss`,
`dog attack [busy|kill|knockout|wound|fetch] [0xHANDLE]` / `dog release` (as the guest's kGEvDogAttack), `dog bark`, `dog set <key> <v>`
(0.8.0 polish: `pick_latch_ms` 0 = 0.8.0's pick with no latch, `step_guard off`, `wait_pin off` for an A/B run), `buddy` (the step guard's
and the Wait pin's counts: steps over 0.5 m raw and drawn, holds and how they ended), `buddy guard reset` and `buddy trace on|off` (the
0.8.0 integration's per-frame measure: one CSV line per frame D-Dog is drawn, `PhantomRingBuddyTrace.csv` next to the host DLL --
`ms,guestFrame,odoEpoch,raw.xyz,drawn.xyz,act,rawStep,drawnStep,jump,rawIn,drawnIn,gflags,bflags,pe.xyz,wait.x,wait.z,lift,dt,fromPub,est.xyz`:
the step guard's point (the dog's published place through its draws' anchor, `fromPub` 1) and where the dog is drawn, the guard's act
(`buddyguard::Act`), whether each is in view, Snake's Elden Ring place and the old estimate from the draws' matrices; join it with the
guest's `!dog placetrace on` CSV by `guestFrame`). Leave both traces off for play: they grow ~20 MB an hour.
With fake_mgsv `--buddy dog`: `!dog attack target <style> full` takes the host's pick (kActorDogTarget); `!order 10` is Bark.
Quiet and the grace rest (docs/HOST.md "Quiet in Elden Ring", "Resting at a grace with the buddy"; on by default since 0.9.0, her orders
opt-in): `quiet orders on|off` (her pick, reticle and LB hint: Fire / Take aim), `quiet on|off` (the layer: enabled and posts together),
`quiet all on|off` (everything, orders included) and `grace on|off` for a session; `quiet pick` (U14's dry run: every character within 80 m and why he is or isn't her pick, the
PlayerIns NPCs included; nothing is ordered, shot or marked), `quiet test boss`, `quiet post now|none` (`none` fades her out if she is in
view: the cloak's test), `quiet shoot [0xHANDLE] [lethal|tranq|heavy]` (a Hit as kGEvQuietShot brings it, from her post), `quiet survey
<label>` / `grace survey [all] <label>` (the post rule's / the seat rule's every ray written to `PhantomRingQuietRays-N.json` /
`PhantomRingGraceRays-N.json` next to the host DLL: tests/fixtures/quiet/), `quiet record on` / `grace record on` (each search's rays),
`grace fade` (Elden Ring's InGame fade plate now). The host logs `grace: U12 rest #N -- black from +A to +B ms after the press` for every
rest. With fake_mgsv `--buddy quiet --no-walk` (then `!autofire off` at once): `!quiet post on`, `!quiet seat on` make it hold her at
the host's post and seat and echo them; `!quiet shot pick full` with `!quiet weapon lethal|tranq|heavy` sends her Ordered, Hit and
Released on the host's pick. `!cam <phase> <dist> <height> <look>` fixes the fake camera (the rest's view: behind Snake, toward the
grace; the phase is MGSV's, so turn it once and read `pr er cam` to find Elden Ring's heading).
The 0.9.0 integration's helpers (both games): `pr lua "Player.RequestToSetCameraRotation{rotX=<pitch>, rotY=<MGSV yaw from
\`combat aimat\`>, interpTime=0}; return 1"` turns MGSV's camera onto an Elden Ring point; Elden Ring's map (View tapped) > Y lists the
sites of grace by region (LB / RB), A travels -- how the round moved between Rampart Tower, Gatefront and The First Step; `puppet off`,
`call setpos x y z`, `puppet auto` puts Snake at a grace's prompt (Elden Ring's coordinates move with its world shifts: find the grace
again with `geom all 300 AEG099_060`). `menu open` / `menu auto` give Elden Ring the pad when a screen is up that the host missed.
The 0.8.0 polish's measures (docs/HOST.md "D-Dog in Elden Ring"): orders through MGSV's call menu from the virtual pad (`!callmenu on
ordered`, then `pr er "press lb 3000 0 0 32767 0"`, `press lb 3000`, `press lb+r3 3000`, `press lb 3000`, `press none 50` -- each press
long enough to overlap the next one's start, since `pr er` takes ~0.3 s to run), the dog's one-frame jumps (`!dog blip 7 0 1`, the
climb-up case `!dog blip 77 0 20`), Wait with Snake run into a wall (`!order 9`, `!move 3 <yaw from combat aimat>`, `!order 8`).
The guest's side (docs/GUEST.md 8.4) with fake_er: `pr er block 1|0` (an Elden Ring wall: the puppet stays while MGSV's Snake runs on;
the `--actors near` characters move by the slip in MGSV, as the real host maps them) and `--trace-draws` (each buddy draw's bone mean,
`buddym=`).

Exit codes: 0 ok, 1 error (Lua or command error, launch refused), 2 usage / no mapping,
3 the side you asked is absent, 4 console timeout.

When a side is absent, `pr lua` and `pr er` say why ("not running (status Exiting, last heartbeat
4.2 s ago, pid 1234 gone)") instead of waiting. `--force` sends anyway. A timed-out request stays queued,
and the game runs it when it next serves its console.

## Launch

`pr launch` does these steps in order. It stops at the first failure, before it touches anything.

1. **Preflight.**
   - Steam is running (`~/.steam/steam.pid`, or a `steam` process). Its folder is `$PHANTOMRING_STEAM`, else the first of `~/.steam/root`, `~/.steam/steam`, `~/.local/share/Steam`, `~/.steam/debian-installation` and Flatpak's `~/.var/app/com.valvesoftware.Steam/.local/share/Steam` (untested) that has `steamapps/libraryfolders.vdf`.
   - Neither `eldenring.exe` nor `mgsvtpp.exe` is running.
   - Both apps have `StateFlags 4`.
   - The exe sha256 matches a pin: ER `f9118fea…98ffb328` (1.17.2 / 2.7.2.0) or `1a354710…8b597891` (1.17.1 / 2.7.1.0), MGSV `085c2f82…bb45`.
   - `build/dinput8.dll` and `build/PhantomRingHost.dll` exist.
   - `MGS_TPP/dinput8.dll` is absent or ours.
   - The Proton and SLR dirs exist, and so does gamescope (a note if its `--help` doesn't list the headless backend).
   - The me3 profile is valid. It must have `start_online=false`, `savefile=PhantomRing0000.sl2`, `disable_arxan=true` and exactly one native, `build/PhantomRingHost.dll`.
   - The ER save dir exists, with `PhantomRing0000.sl2` in it or a save to create it from (`--seed-save NAME` / `$PHANTOMRING_SEED_SAVE`, else `ER0000.sl2`).
2. **Backup.**
   - ER: `compatdata/1245620/pfx/drive_c/users/steamuser/AppData/Roaming/EldenRing/<steamid64>/` is copied to `backups/<ts>/er/`.
   - MGSV: `~/.local/share/Steam/userdata/<account>/311340/remote` is copied to `backups/<ts>/mgsv/311340-remote/`, and `.../287700` to `backups/<ts>/mgsv/287700/`.
   - Every file is re-hashed on both sides, and `manifest.json` is written and then verified. Each ER backup is about 140 MB.
   - Old backups are pruned after the new one has verified (`--keep-backups N`, `$PHANTOMRING_KEEP_BACKUPS`; **default 10 since 0.7.0**, 0 = keep all -- kept forever, ~130 MB each, they reached 18.8 GB in 146 launches). **Only the backups `pr launch` itself took** are ever pruned: each manifest says who made it (`"origin": "launch"`, `"manual"` for `pr backup-saves`, `"pre-restore"` for `restore-saves`' safety copy); a manifest from before 0.7.0 has none and counts as a launch's only while `logs/` still holds that launch's session folder of the same name with its `pids.json` -- the prune then writes `"origin": "launch"` into it, so it stays known once that log folder is pruned -- and anything else counts as made by hand. Only folders with a launcher-made name (`<yyyymmdd-hhmmss>[-n]`, sorted by its numbers) and a `manifest.json` are candidates: a copy (`20261012-003126-original`, `0-original`), `worktree-patches-...`, any other folder, a symbolic link (never followed) and a manifest that can't be read are always kept. For Elden Ring and for MGSV each, the newest N launch backups holding that game are kept, and so is the **first** backup of each game -- named in `backups/.first.json` (written the first time a game is seen: on a new install the backup just taken, on an upgrade the oldest one there; a backup taken later while the system clock ran behind, whose name sorts first, can't take its place) and the oldest by name too. A backup goes only when no game keeps it. Every `*-pre-restore` one and every backup from the last 24 hours (`$PHANTOMRING_KEEP_RECENT_HOURS`, 0 = no such protection; a value that isn't a number, or nan, keeps the 24 h; a name dated in the future counts as recent) are kept too: a day of relaunches never pushes out the backup a test session started with. A `$PHANTOMRING_KEEP_*` value that isn't a whole number ("all", "off", "", "10.0") keeps everything, with a warning. A folder that can't be removed is logged and skipped (the launch goes on); the log line names what was removed. `pr launch --dry-run` prints the rule in force. Review 0.7.0; live on the 0.7.0 integration launch: 17 folders before, 18 after (the new one), nothing pruned (all from the last 24 h, made by hand or protected), `.first.json` = er 20261031-232905, mgsv 20261012-003126; the 11 launch backups of that day got `"origin": "launch"` from their `logs/` folders, the three older ones (their log folders gone) count as made by hand.
   - Two backups in the same second get their own folders (`<ts>-2`, `<ts>-2-pre-restore`): the second used to rewrite the first's manifest.
3. **ER save.** If `PhantomRing0000.sl2` doesn't exist, it is created as a verified copy of the seed save (`--seed-save`, `$PHANTOMRING_SEED_SAVE`, else `ER0000.sl2`), through `PhantomRing0000.sl2.tmp` (renamed only once its sha256 matches; a failed copy leaves no save behind). The seed is never modified, and an existing `PhantomRing0000.sl2` is never replaced.
4. **me3.** If it isn't there yet, the official `me3-linux-amd64.tar.gz` of [me3 v0.13.0](https://github.com/garyttierney/me3/releases/tag/v0.13.0) is extracted to `third_party/me3/`. It is looked for at `pr install-me3 --archive PATH`, `$PHANTOMRING_ME3_ARCHIVE`, `third_party/downloads/` and `~/Downloads/`. Its sha256 must equal the release's published digest (`f31579de…e93fc1`, `ME3_SHA256` in `launch.py`), and it is unpacked with `tarfile` `filter="data"`.
5. **Mapping.** `/dev/shm/phantomring_v1` is created (mode 0600), truncated to 0, and sized to `kMappingBytes`. This leaves it all zero.
6. **Guest.**
   - `build/dinput8.dll` and `mgsv/PhantomRingGuest.ini`, if present, are copied into `MGS_TPP/`.
   - Each sha256 is recorded in `logs/guest-install.json`.
   - An unknown existing `dinput8.dll` is never overwritten. An `.ini` the user changed in the game dir is kept.
7. **Start.** Each command runs in its own session (`setsid`), with stdout/stderr going to `logs/<ts>/`. `logs/current` links to that dir, and `logs/current/pids.json` records the pids.
   - **MGSV** (§7.2 of `docs/research/proton-ipc-and-launch.md`):
     `gamescope --backend headless -w W -h H -W W -H H -- env -u WAYLAND_DISPLAY STEAM_COMPAT_CLIENT_INSTALL_PATH=~/.local/share/Steam STEAM_COMPAT_DATA_PATH=<lib>/compatdata/287700 STEAM_COMPAT_INSTALL_PATH=<lib>/common/MGS_TPP STEAM_COMPAT_TOOL_PATHS=<Proton>:<SLR> STEAM_COMPAT_LIBRARY_PATHS=~/.local/share/Steam/steamapps STEAM_COMPAT_APP_ID=287700 SteamAppId=287700 SteamGameId=287700 SteamOverlayGameId=287700 WINEDLLOVERRIDES=dinput8=n,b PHANTOMRING=1 PHANTOMRING_ROLE=guest PROTON_LOG=1 PROTON_LOG_DIR=logs/<ts> WINEDEBUG=… <SLR>/_v2-entry-point --verb=waitforexitandrun -- <Proton>/proton waitforexitandrun <MGS_TPP>/mgsvtpp.exe`
     - Proton comes from Steam's `CompatToolMapping`, which maps an app or global entry to a tool name, then the tool appid (me3's table), then `appmanifest` `installdir`. SLR comes from the Proton dir's `toolmanifest.vdf` `require_tool_appid`. The fallback is Proton - Experimental + SteamLinuxRuntime_4, the tested combination.
     - `-u WAYLAND_DISPLAY` is the one addition to the §7.2 recipe. It keeps Wine from reaching the desktop compositor directly; only gamescope's nested X server is reachable.
   - **ER**: `third_party/me3/bin/me3 --windows-binaries-dir third_party/me3/bin/win64 --crash-reporting=false launch --game eldenring --exe <ER>/Game/eldenring.exe --profile tools/phantomring/profile/phantomring.me3 --online=false --savefile PhantomRing0000.sl2`, run with `PHANTOMRING=1 PROTON_LOG=1 PROTON_LOG_DIR=logs/<ts>`. me3 builds the SLR + Proton chain itself.
   - `disable_arxan` is a top-level `ModProfileV1` key in me3 0.13, not a per-native key. This was checked in the me3 source (`crates/mod-protocol/src/lib.rs`, `schemas/mod-profile.md`) and in the shipped `bin/me3` strings (the `ModProfile` deserializer field list).

Each launched process gets `PHANTOMRING_LAUNCH_ID` in its environment. `pr stop` finds the whole tree through it, including the wineserver and pressure-vessel helpers that re-parent or `setsid`. It sends SIGTERM to the game exes first, then to everything else. A SIGTERMed Wine process skips its own shutdown, so quit Elden Ring from its menu first if the last minutes of play matter.

Logs:

- **Launch.** `logs/<ts>/mgsv-gamescope.log`, `er-me3.log`, `steam-287700.log` and `steam-1245620.log` (the last two are Proton logs), and `pids.json` (what `pr stop` stops). A second launch in the same second gets `logs/<ts>-2` (`pr launch --mgsv-only` straight followed by `--er-only` used to share one `pids.json`, the second replacing the first, and `pr stop --mgsv` then found no MGSV).
- **Pruning** (0.7.0; `--keep-logs N`, `$PHANTOMRING_KEEP_LOGS`; default 10, 0 = keep all): each launch, after `logs/current` points at its new folder, deletes the oldest session folders (`logs/<yyyymmdd-hhmmss>[-n]`) beyond the newest N. Never anything else under `logs/` (`guest-install.json`, evidence folders, `collect-logs` zips, other files), never the folder `logs/current` points at, never one whose launch still runs (a `pids.json` root still carrying its `PHANTOMRING_LAUNCH_ID`), and never one from the last 24 hours (`$PHANTOMRING_KEEP_RECENT_HOURS`, as for backups). A symbolic link is never followed and a folder that can't be removed is logged and skipped. The plugin logs (`build/PhantomRingHost.log`, `MGS_TPP/PhantomRingGuest.log`) are not session folders and are never pruned. `pr launch --dry-run` prints the count and the rule in force.
- **me3.** `~/.local/share/me3/logs/phantomring/`.
- **Plugins.** `build/PhantomRingHost.log` (next to the host DLL) and `MGS_TPP/PhantomRingGuest.log`; `pr logs` shows whichever exist.

## Protocol conventions the tools assume (not spelled out as constants in the header)

The plugins must match these. `test_fakes.py` exercises all of them.

- **Draw slot headers.** `DrawSlotHdr[3]` sit directly after `DrawCtl`, at `kOffDrawCtl + 0x10 + i*0x10`. Slot `i`'s bytes (a `DrawList`, then `DrawRecord[drawCount]`, then `float[matrixCount][12]`) are at `kOffDrawSlots + i*kDrawSlotBytes`.
- **HUD slot headers.** `HudCtl` owns a 0x40 line. `HudSlotHdr[3]` are at `kOffHudCtl + 0x40 + i*0x40`, and the pixels are at `kOffHudSlots + i*kHudSlotBytes`. This is SkyCraft's overlay layout, and it agrees with the header's static_asserts.
- **Render ring.**
  - `head` and `tail` are monotonically increasing byte counts; the position is the count modulo `kRenRingDataBytes`.
  - A message that would cross the end is preceded by a `kRenPad` header whose `payloadBytes` covers the rest of the ring. Both counters then skip to the next wrap.
  - A `kRenMesh` payload is `RenMesh`, then `RenInputElement[elementCount]`, then `vertexBytes`, then `indexBytes`.
  - A `kRenTexture` payload is `RenTexture` followed by `dataBytes`.
- **Attach handshake.**
  - The host stamps a new `sessionNonce` and drains what it consumes: guest events, the render ring, and both triple readers (front = 2).
  - The guest, on seeing a new nonce (or at process start), first writes `guestNonce = 0`. It then resets its producers (triple state = 0, back = 1), drains host events, bumps `odoEpoch`, and pushes `kRenClearAll` plus every live mesh and texture. Only then does it set `guestNonce = sessionNonce`.
  - The host reads guest slots only while `guestNonce == sessionNonce`. It resets its triple readers whenever that becomes true or `guestUnixPid` changes, and re-baselines the odometer on a new `odoEpoch`.
- **Consoles.**
  - A tool writes `request` (NUL-terminated, at most 0xFEF bytes), then stores `reqSeq + 1`.
  - The game answers by writing `status` and `response`, then stores `respSeq = reqSeq`.
  - The tools wait while `reqSeq != respSeq` before sending (one request in flight).
  - Each game drops leftover requests when it (re)attaches (`respSeq = reqSeq`).
- **Clock.** Proton's QPC is `CLOCK_MONOTONIC_RAW` in 100 ns ticks with a frequency of 10,000,000. This is VERIFIED in `docs/research/skycraft-guest-side.md`. The fakes stamp the same clock, and `pr status` shows real heartbeat ages from it. System Wine 11 (the offline tests) uses `CLOCK_MONOTONIC` instead (measured: the two differ by
  ~0.2 s here and drift apart with NTP slewing); `PHANTOMRING_QPC_CLOCK=monotonic` switches `proto.qpc_now()`
  to it, which `test_plugins.py` sets.
- **Buddies.** The v2 GuestState buddy block (`buddyFlags` Present/PosValid/Ridden/Loading/Missing, `buddyObjectId`, `buddyAmmo`, `buddyPos`, `buddyYaw`) is shown by `pr status` and published by fake_mgsv. MGSV's `BuddyType` is `NONE 0, HORSE 1, DOG 2, QUIET 3, WALKER_GEAR 4, BATTLE_GEAR 5, DISABLE 7`. This is VERIFIED from the enum registration at `0x140A46E21` in mgsvtpp.exe 1.0.15.4. `pr status` prints the buddy by name, plus the vehicle id and `RIDING` while `kGuestOnVehicle` is set.

## Fake peers

```
pr fake-er --create            # stand-in Elden Ring host on the real mapping: test the MGSV guest without Elden Ring
pr fake-mgsv --create          # stand-in MGSV guest: test the Elden Ring host without MGSV
```

All four stand-ins (`fake-er`, `record` as hosts; `fake-mgsv`, `replay` as guests) refuse to start (exit 4) when another live process already holds their side of the mapping: the header, seqlocked blocks and rings are single-writer per side, and Elden Ring stamps its session nonce once, so a second host would take MGSV away from it for good. `--force` overrides.

**fake_er**:
- Stamps the header and heartbeats at 60 Hz.
- Publishes HostState (InGame|Puppeting) and two actors.
- Drives a 10 s scripted pad loop: stick forward 3 s, stick right 3 s, tap A, hold RT, neutral.
- Sends Hurt (50) every 4 s.
- Applies the odometer to a puppet position.
- Prints guest state, odometer, pose bone count, fire and buddy-fire events, draw lists, render-ring meshes and textures, and HUD frames.
- Answers `pr er` with canned data: `help`, `ping`, `status`, `peek player.pos`, `peek <addr>`.
- `--actors near` + `pr er actor 0|1 asleep|awake|dead|alive|at <dx> <dz>|back|renew` and `pr er actors`: the two actors' flags,
  places (MGSV metres from Snake) and handles for ActorProxy / Fulton tests (docs/GUEST.md 20.4-20.5); a kGEvFulton a=1 takes that
  actor out of the table, as the host takes the enemy out of Elden Ring. `pr record` takes `--actors` too.
- `pr er actor 0|1 dogtarget|nodogtarget` (v17): that actor is the host's pick for D-Dog's next order (`kActorDogTarget`, at most
  one record). kGEvDogAttack (phase, style, MGSV attack id, share, handle), kGEvBuddyOrder (with D-Dog's style from `pad[0]`) and
  `kBuddyEngaged` coming and going are printed (docs/research/ddog.md §4).
- HostState carries `HostFlags.DogLayer` (v17, the host's `[Dog]` on: the real guest takes D-Dog's attack orders over only then);
  `pr er doglayer 0|1` clears / sets it.
- Quiet and the grace rest (v18, docs/research/quiet.md §4): `pr er quietlayer 0|1` (`HostFlags.QuietLayer`, off by default as the
  real host's `[Quiet]`), `pr er actor 0|1 quiettarget|noquiettarget|quietthreat|noquietthreat` (`kActorQuietTarget` /
  `kActorQuietThreat`, at most one record each), `pr er post <dx> <dz> [yaw_deg] | none` (her post dx, dz m from Snake, `buddyPostSeq`
  bumped; `none` = on the move, NaN), `pr er grace 1 [<dx> <dz> [yaw_deg]] | 1 none | 0` (a rest: `kHostAtGrace`, `graceCentre` 1.2 m
  ahead of Snake, the seat dx, dz m from him -- 1.5, 1.5 by default -- facing the grace, `graceSeq` bumped and kHEvRested 0 sent as
  the real host does; `none` = a rest without a seat, a talk; `0` = left the grace), `pr er hoststate` (what is published). Posts and
  seats are kept where they are in Elden Ring and mapped to MGSV's world every frame. kGEvQuietShot (phase, weapon, attack id, share or
  the lost reason, handle), Quiet's orders (her weapon from `pad[0]`) and the guest's echoes (`buddyPostSeq` / `graceSeqSeated`, the
  AtPost / Seated / Cloaked / CanRest / QuietMarked / QuietCover bits, her weapon) are printed as they change. `pr status` shows the
  grace, the post and the echoes once a seq is non-zero.

**fake_mgsv**:
- Waits for a host session, then publishes Snake walking a circle in a 10 m sandbox (treadmill warp kept out of the odometer), plus the pad's left stick.
- Publishes a 178-bone pose and an orbiting camera.
- Life drops on Hurt and regenerates.
- **D-Walker deployed** (`buddyType = WALKER_GEAR`), with the v2 buddy block (beside Snake, `kBuddyRidden` and under him while ridden, ammo counting down with each BuddyFire). It is **ridden 4 s of every 12** (`kGuestOnVehicle`, vehicleId 0x402, 2.5× speed).
- Sends Fire every 2 s and BuddyFire every 3 s.
- `!grab <grab|hold|throw|choke|interrogate|release>`: a v13 kGEvCqc from Snake's feet and facing (CQC holds and throws, COMBAT §7a).
- Each frame, publishes a draw list with Snake (`kOwnerPlayer`) and D-Walker (`kOwnerBuddy`) on the triple buffer, plus a 64×8 life-bar HUD.
- Sends one triangle mesh and a 4×4 texture per owner on the render ring.
- `!climbhome <ms>`: after `kHEvClimb a = 0` Snake stays on MGSV's ladder that long (`kGuestClimbing`, no walking) before he is home,
  as the real guest takes him back from its airport ladder (0.18-0.23 s live); default 0 (home at once).
- `!suppressor on [shots] | off` (v16) sets `kGuestSuppressorOn`; while it is on every `!fire` is suppressed (`kFireSuppressed` in
  Fire `b`'s bit 31) unless it says `loud`, and after `shots` suppressed shots (default 4, 0 = never) the flag drops right after
  the shot that wore it out, as MGSV's spent suppressor comes off (the host's "Suppressor worn out"). `!fire ... suppressed` marks
  one shot without the flag.
- D-Dog (v17, docs/research/ddog.md; with `--buddy dog` or `!buddy dog`): `!dog style <busy|fetch|kill|knockout|wound|unknown>`
  (its equipment), `!order <cmd>` (kGEvBuddyOrder with `pad[0]` = style + 1 for the dog), `!dog attack [target|nearest|none|0xHANDLE]
  [style] [full|ordered|connect|lost|refused]` (kGEvDogAttack on the host's pick `kActorDogTarget` by default; full = Ordered,
  Connected 0.6 s later with `kBuddyEngaged`, Released after `!dog busy <s>`, 2 s by default; a new order ends the one under way),
  `!dog release`, `!dog status`. The host's pick is logged as it changes. As the real guest (0.8.0): wound is the body attack 147, each
  event carries MGSV's share of its attack (the bite 1.1538 lives, the knife 25.2058, the stun knife and the body attack 0; `!share`
  overrides), `pad[0]` only for the attack orders 11-15, and D-Dog's style is in buddyFlags bits 8-15 every frame.
  0.8.0 polish (the host's measures): `!callmenu on|off [full|ordered|connect]` -- MGSV's call menu from the pad as the real guest takes
  an attack order: LB held, the right stick pushed and let go, R3 clicked while LB is still held -> kGEvBuddyOrder of the dog's attack
  command (11-15 by its style), then the host's pick taken at once or within 350 ms (`callmenu: order 11 takes the host's pick 0x... N
  ms after the click`), else kGEvDogAttack Refused (`REFUSED -- no host pick within 350 ms`); a taken order runs `!dog attack <pick>
  <mode>`. `!order 9` makes the dog wait where it is in Snake's odometer frame (as the real guest's treadmill keeps it; nothing moved
  by Elden Ring's slip), `!order 8` / `16` lets it follow again. `!dog blip <dx> <dz> [frames] [dy]` puts the dog's place and its
  draws off by that much for N frames (1 by default) and back; `!dog move <dx> <dz>` for good (it waits there).
- Quiet (v18, docs/research/quiet.md; with `--buddy quiet` or `!buddy quiet`, object id 0x2400 as MGSV's): `!quiet weapon
  <lethal|tranq|heavy|unknown>` (buddyFlags bits 17-19 every frame; `!order 1..3` carry it in `pad[0]`, b = 3), `!quiet post on|off`
  (held at the host's post while `HostFlags.QuietLayer` is set, `kBuddyAtPost` + `buddyPostSeq` echoed; no post = `kBuddyCloaked`),
  `!quiet seat on|off` (`kBuddyCanRest`; seated at the host's grace seat while `kHostAtGrace`, `kBuddySeated` + `graceSeqSeated`
  echoed), `!quiet cloak|cover|marked on|off`, `!quiet hit <s>` (Ordered -> Hit, 1 s by default), `!quiet shot [pick|threat|nearest|
  none|0xHANDLE] [full|ordered|hit|released|lost|refused] [mgsv|target|cancelled] [weapon]` (kGEvQuietShot on `kActorQuietTarget` by
  default; full = Ordered with `kBuddyEngaged`, Hit, Released 0.3 s later; lost = Ordered, Lost 0.5 s later with the reason; the
  attack id 290 / 291 / 292 and MGSV's share 0.9615 / 0.4615 / 1.4615 by weapon), `!quiet status`. The host's picks are logged as
  they change.
- `!binoculars on|off` (v13) sets `kGuestBinoculars` + `kGuestFirstPerson`: the real Elden Ring host then marks the characters
  near its camera's centre (docs/HOST.md "Marking"; `pr er 'mark binoculars on'` does the same without any guest flag).
- Answers `pr lua` (and `!status`) through a tiny Lua subset: `return <exprs>` and `vars.x = <expr>`, with `vars.*`, `BuddyType.*`, `math.*`, `tostring`, `..`, `~=`, and/or/not.

## Tests (offline)

```
python3 tools/phantomring/test_layout.py     # build/layout_dump.exe (117 values) + a dump generated from proto.py (552 values: every field
                                             # offset/size of all 22 structs, all constants, all enum values), both MinGW-built, run under wine
python3 tools/phantomring/test_fakes.py      # fake_mgsv vs fake_er, ~17 s, 72 checks incl. host restart -> guest re-attach + resend, buddy block, v17 D-Dog
python3 tools/phantomring/test_launcher.py   # backup/verify/restore/prune (default keep 10, the last 24 h kept, logs/ sessions only;
                                             # only launch backups, launcher-named, .first.json, symlinks, bad env values),
                                             # seed save, guest install refusals, profile validation,
                                             # me3 archive lookup (temp dirs only; any machine, games running or not)
                                             # --unit: only game-process matching on a fake /proc; --machine: also this machine's
                                             # Steam, Proton, MGSV and ER save dir (read-only)
python3 tools/phantomring/test_plugins.py    # the plugins' real C++ link code under wine vs the fakes: test_host.exe --peer vs
                                             # fake_mgsv, test_guest_link.exe --peer vs fake_er (30 checks, ~30 s; needs build/)
cmake --build build --target check_windows   # Windows players (docs/WINDOWS.md): platform decisions, Steam lookup, guest install,
                                             # both link codes over Local\PhantomRing_v1 + the launcher end to end, the hidden
                                             # window, DLL imports (tools/check_imports.py)
python3 -I tools/package_windows.py          # dist/PhantomRing-<version>-windows.zip (me3 profile, both DLLs, inis, collect-logs.cmd, README-WINDOWS.md)
python3 -I tools/package_linux.py            # dist/PhantomRing-<version>-linux.tar.gz (the checkout's layout: build/ DLLs, inis, tools/pr)
                                             # both: reproducible, BUILD.txt (version, commit, protocol, game versions, sha256
                                             # of every file) inside, <archive>.sha256 next to it; refuse DLLs of another commit
python3 tools/phantomring/test_release.py    # versions/tags, mismatch wording, both packages twice (byte-identical), pr doctor
                                             # on an empty fake machine, collect-logs redaction (needs build/ for the packages)
python3 tools/phantomring/test_replay.py     # pr record / pr replay between the stand-ins (27 checks)
```

`test_layout.py` runs wine in `$PHANTOMRING_WINEPREFIX`, else `$WINEPREFIX`, else wine's default prefix. A throwaway prefix keeps the tests off your own: `PHANTOMRING_WINEPREFIX=$(mktemp -d)`. It rebuilds `layout_dump` through cmake, and if the build tree can't regenerate it compiles `tests/layout_dump.cpp` directly.

What each check needs, on a fresh clone:

| Check | Needs | Without it |
|---|---|---|
| `test_launcher.py` | Python only | - |
| `test_fakes.py` | Python only | - |
| `test_replay.py` | Python only | - |
| `test_release.py` | `build/` for the package checks, MinGW objdump | package checks skipped |
| `test_layout.py`, `test_plugins.py` | `build/` (MinGW), wine | fails |
| `check_host`, `check_guest`, `check_windows` | `build/`, wine (`WINEPREFIX=$(mktemp -d)` keeps them off your prefix; `xvfb-run` on a machine with no display) | fails |
| `check_guest`'s `test_guest_pump` | `-DPHANTOM_LUA51_SRC=<lua-5.1.5>/src` (the stock Lua 5.1.5 sources; CI fetches lua.org's tarball, sha256-pinned) | skipped, with a message |
| `check_host`'s real-regulation checks | `build/er_params`, extracted by `tools/re/er_params.py` (needs an installed Elden Ring and `FSRS_DIR` = a fromsoftware-rs checkout) | skipped |
| `test_launcher.py --machine` | Steam with both games installed | - |

## Live-test checklist (runtime-UNVERIFIED)

1. `pr launch --dry-run` shows only `[ok]` once both DLLs are built.
2. `pr launch`:
   - `logs/current/steam-287700.log` shows `SteamGameId: 287700` and `dinput8` loaded native from `MGS_TPP`.
   - No MGSV window or audio appears on the desktop. Audio muting is not handled yet; if MGSV is audible, add `PULSE_SINK` or mute it in the guest.
   - gamescope stays up; watch for its NVIDIA `vkCreateComputePipelines` error at teardown.
3. `steam-1245620.log` and the me3 log show the `phantomring` profile, "Neutralize Arxan" true, and online off. The save path is redirected to `PhantomRing0000.sl2`.
4. `pr status`:
   - The host stamps the header.
   - The guest goes Booting → Title → Ready with `guest attached`.
   - Both heartbeat ages stay under 100 ms.
   - Both pids are real unix pids.
5. `pr lua 'return vars.buddyType, BuddyType.WALKER_GEAR'` answers. The second value must be `4`.
6. `pr er 'peek player.pos'` answers.
7. The plugin logs appear where `pr logs` looks. Adjust `plugin_log_candidates()` if they don't.
8. `pr stop` leaves no `mgsvtpp.exe`, `eldenring.exe`, wineserver or gamescope behind (`pgrep -a 'wineserver|gamescope|mgsvtpp|eldenring'`).
9. `pr screenshot` writes a PNG.
10. MGSV saves are untouched afterwards: compare against `backups/<ts>/manifest.json`.

