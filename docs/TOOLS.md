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
                               # --er-headless: test automation on a machine whose displays are off (Elden Ring then
                               # presented ~1 frame/s on the desktop): Elden Ring in its own headless gamescope too;
                               # screenshots with GAMESCOPE_WAYLAND_DISPLAY=gamescope-N gamescopectl screenshot F.png
pr er-continue                 # through Elden Ring's title: pad A and keyboard Enter every 4 s until it loads
                               # (with no controller plugged in, first `pr er "input virtualpad on"`: docs/HOST.md)
pr stop [--er|--mgsv] [--kill] # SIGTERM what pr launch started (games first), --kill SIGKILLs the rest after --timeout;
                               # both games, whichever recent launch started each (after `launch --mgsv-only` next to a
                               # running Elden Ring it used to stop MGSV only); --er / --mgsv: only that game. A recorded
                               # pid counts only while it still carries its launch's PHANTOMRING_LAUNCH_ID (pid reuse)
pr logs [-n 30] [-f]           # tail logs/current/*.log (gamescope/me3 output, Proton logs) and the plugin logs
pr backup-saves [--er-only|--mgsv-only]
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
   - The exe sha256 matches the pin: ER `1a354710…8b597891`, MGSV `085c2f82…bb45`.
   - `build/dinput8.dll` and `build/PhantomRingHost.dll` exist.
   - `MGS_TPP/dinput8.dll` is absent or ours.
   - The Proton and SLR dirs exist, and so does gamescope (a note if its `--help` doesn't list the headless backend).
   - The me3 profile is valid. It must have `start_online=false`, `savefile=PhantomRing0000.sl2`, `disable_arxan=true` and exactly one native, `build/PhantomRingHost.dll`.
   - The ER save dir exists, with `PhantomRing0000.sl2` in it or a save to create it from (`--seed-save NAME` / `$PHANTOMRING_SEED_SAVE`, else `ER0000.sl2`).
2. **Backup.**
   - ER: `compatdata/1245620/pfx/drive_c/users/steamuser/AppData/Roaming/EldenRing/<steamid64>/` is copied to `backups/<ts>/er/`.
   - MGSV: `~/.local/share/Steam/userdata/<account>/311340/remote` is copied to `backups/<ts>/mgsv/311340-remote/`, and `.../287700` to `backups/<ts>/mgsv/287700/`.
   - Every file is re-hashed on both sides, and `manifest.json` is written and then verified. Each ER backup is about 140 MB.
   - Old backups are pruned only when asked (`--keep-backups N`, `$PHANTOMRING_KEEP_BACKUPS`; default 0 = keep all): then, for Elden Ring and for MGSV each, the newest N backups holding that game are kept and so is the oldest one holding it (its saves before PhantomRing first ran); a backup goes only when neither game keeps it. Every `*-pre-restore` one and any folder whose manifest can't be read are kept too.
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

**fake_mgsv**:
- Waits for a host session, then publishes Snake walking a circle in a 10 m sandbox (treadmill warp kept out of the odometer), plus the pad's left stick.
- Publishes a 178-bone pose and an orbiting camera.
- Life drops on Hurt and regenerates.
- **D-Walker deployed** (`buddyType = WALKER_GEAR`), with the v2 buddy block (beside Snake, `kBuddyRidden` and under him while ridden, ammo counting down with each BuddyFire). It is **ridden 4 s of every 12** (`kGuestOnVehicle`, vehicleId 0x402, 2.5× speed).
- Sends Fire every 2 s and BuddyFire every 3 s.
- Each frame, publishes a draw list with Snake (`kOwnerPlayer`) and D-Walker (`kOwnerBuddy`) on the triple buffer, plus a 64×8 life-bar HUD.
- Sends one triangle mesh and a 4×4 texture per owner on the render ring.
- Answers `pr lua` (and `!status`) through a tiny Lua subset: `return <exprs>` and `vars.x = <expr>`, with `vars.*`, `BuddyType.*`, `math.*`, `tostring`, `..`, `~=`, and/or/not.

## Tests (offline)

```
python3 tools/phantomring/test_layout.py     # build/layout_dump.exe (110 values) + a dump generated from proto.py (489 values: every field
                                             # offset/size of all 22 structs, all constants, all enum values), both MinGW-built, run under wine
python3 tools/phantomring/test_fakes.py      # fake_mgsv vs fake_er, ~14 s, 50 checks incl. host restart -> guest re-attach + resend, buddy block
python3 tools/phantomring/test_launcher.py   # backup/verify/restore/prune, seed save, guest install refusals, profile validation,
                                             # me3 archive lookup (temp dirs only; any machine, games running or not)
                                             # --unit: only game-process matching on a fake /proc; --machine: also this machine's
                                             # Steam, Proton, MGSV and ER save dir (read-only)
python3 tools/phantomring/test_plugins.py    # the plugins' real C++ link code under wine vs the fakes: test_host.exe --peer vs
                                             # fake_mgsv, test_guest_link.exe --peer vs fake_er (30 checks, ~30 s; needs build/)
cmake --build build --target check_windows   # Windows players (docs/WINDOWS.md): platform decisions, Steam lookup, guest install,
                                             # both link codes over Local\PhantomRing_v1 + the launcher end to end, the hidden
                                             # window, DLL imports (tools/check_imports.py)
python3 -I tools/package_windows.py          # dist/PhantomRing-<version>-windows.zip (me3 profile, both DLLs, inis, README-WINDOWS.md)
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
| `check_guest`'s `test_guest_pump` | `-DPHANTOM_LUA51_SRC=<lua-5.1.5>/src` (the stock Lua 5.1.5 sources) | skipped, with a message |
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

