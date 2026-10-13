# PhantomRing

Play Elden Ring as Venom Snake. MGSV runs hidden in the background and supplies Snake himself:
his movement, stances, CQC, weapons, animation, life and his battle buddy (D-Walker first). Elden
Ring runs its world, enemies, bosses, quests and saves, and draws everything.

Neither game is rewritten. An Elden Ring plugin and an MGSV plugin talk to each other through shared
memory. It's the same idea as [SkyCraft](https://github.com/chasmlol/SkyCraft), which does it for a Minecraft
player in Skyrim.

![Snake and D-Walker at Gatefront, lit by Elden Ring, MGSV's HUD on top](docs/screenshots/snake_walker_gatefront.jpg)

| | |
|---|---|
| ![Riding D-Walker into Gatefront Ruins](docs/screenshots/riding_dwalker_gatefront.jpg) | ![Mounting D-Walker, its flamethrower selected in MGSV's HUD](docs/screenshots/dwalker_flamethrower.jpg) |

> **Status: playable, early.** Developed and tested on Linux (Proton) with both games running; see
> [Known issues](#known-issues) and [docs/STATUS.md](docs/STATUS.md). The Windows build has never been run on
> Windows ([docs/WINDOWS.md](docs/WINDOWS.md)).

> **Read this first**
> - **Unofficial fan-made mod.** Not affiliated with, endorsed or sponsored by Konami, FromSoftware or
>   Bandai Namco.
> - **No game files are included.** You must own both games on Steam: ELDEN RING (1.17.1) and METAL GEAR
>   SOLID V: THE PHANTOM PAIN (1.0.15.4). Snake's models, textures, animation and sound are read at runtime
>   from your own MGSV install.
> - **Play offline only.** PhantomRing always starts Elden Ring offline through Mod Engine 3, which
>   neutralises Arxan and never loads Easy Anti-Cheat. Never take a PhantomRing save or setup online.
> - **Back up your saves.** `pr launch` backs up both games' saves before every start, and Elden Ring plays on
>   its own copy of your save (`PhantomRing0000.sl2`). Mods can still go wrong: keep your own backups too.
> - Use at your own risk.

## How it works

| | MGSV (guest, hidden) | Elden Ring (host) |
|---|---|---|
| Movement | Snake runs, crouches, crawls, dives in a sandbox on the Nova Braga Airport apron; only his *intended motion* leaves MGSV | the invisible Elden Ring character takes that motion through Elden Ring's collision (walls, slopes, ledges) |
| Camera | MGSV's over-the-shoulder / aim / CQC cameras | Elden Ring renders from exactly that camera, mapped onto the player |
| Look | captures what it draws for Snake and D-Walker: real meshes, textures, skinning matrices | draws them inside its own frame |
| Controls | gets your controller through a virtual XInput pad | owns the real controller; keeps Start (menu) and View (map) |
| Combat | fires, CQCs, tranqs; life regenerates as in MGSV | resolves the hits on its enemies; its hits drain Snake's life |
| Buddy | D-Walker deployed, following, rideable | drawn next to Snake, on Elden Ring's ground |

Details: [docs/DESIGN.md](docs/DESIGN.md). The shared layout: [protocol/phantomring_protocol.h](protocol/phantomring_protocol.h).

## Requirements

| | |
|---|---|
| Linux | Tested on Gentoo with KDE Plasma (Wayland) and an NVIDIA RTX 50-series GPU. Untested: AMD/Intel GPUs, Steam Deck, other desktops |
| Steam | the native client, running and signed in (both games check it). Flatpak Steam is untested |
| ELDEN RING | **1.17.1** (`eldenring.exe` 2.7.1.0), installed through Steam, run with Proton (Proton Experimental is what was tested) |
| METAL GEAR SOLID V: THE PHANTOM PAIN | **1.0.15.4**, installed through Steam (Proton) |
| An Elden Ring character | in your normal save, `ER0000.sl2`: PhantomRing makes its own copy of it |
| An MGSV save | that can load **Africa free roam** (Snake is parked on the Nova Braga Airport apron); D-Walker developed if you want him (optional) |
| gamescope | with the headless backend (`gamescope --backend headless`); MGSV runs hidden inside it |
| Mod Engine 3 | **me3 0.13.0**, the official Linux release, which you download yourself (below) |
| A controller | Xbox-style (XInput). Keyboard and mouse don't drive Snake yet |
| A strong PC | both games run at once; MGSV renders in the background |

To build: **MinGW-w64 GCC 13 or newer** (the posix thread model is recommended), **CMake 3.25+**, **Ninja**,
**Python 3.11.4+** (standard library only). For the tests, also wine.

Package names (only Gentoo has been tried):

```bash
sudo pacman -S mingw-w64-gcc cmake ninja python gamescope                          # Arch
sudo dnf install mingw64-gcc-c++ cmake ninja-build python3 gamescope              # Fedora
sudo apt install g++-mingw-w64-x86-64-posix cmake ninja-build python3 gamescope   # Ubuntu 24.04 / Debian 13
```

The plugins check the exact game versions (file hashes). After a Steam update to either game they do nothing
and the game runs normally, until PhantomRing is updated for the new version.

## Install and run (Linux)

**1. Get the code and build it.**

```bash
git clone --recursive https://github.com/whp199/PhantomRing PhantomRing    # --recursive: MinHook is a submodule
cd PhantomRing                                               # (after a plain clone: git submodule update --init)
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Use `-B build`: the launcher looks for the two DLLs there.

**2. Get me3.** Download `me3-linux-amd64.tar.gz` from the
[me3 v0.13.0 release](https://github.com/garyttierney/me3/releases/tag/v0.13.0) into `~/Downloads/` (or
`third_party/downloads/` in this folder), then:

```bash
tools/pr install-me3       # checks the archive's sha256 against the release, extracts it into third_party/me3/
```

**3. Prepare both games once.**

- Start **MGSV** normally from Steam. If it asks for your country/region and Konami's updated Terms of Use
  (July 2026 patch), answer it yourself: PhantomRing never does, and runs MGSV offline anyway. Make sure
  Continue loads a save that can go to Africa, then quit.
- Start **Elden Ring** normally once if you have never played it on this machine, so that `ER0000.sl2` holds a
  character. PhantomRing copies it to `PhantomRing0000.sl2` on its first launch and only ever plays that copy
  (another save: `tools/pr launch --seed-save NAME`).

**4. Play.**

```bash
tools/pr launch --dry-run   # checks everything and prints what it would do; fix any [FAIL] it lists
tools/pr launch             # backs up both games' saves, starts MGSV hidden, then Elden Ring (offline)
tools/pr status             # both sides, live
tools/pr stop               # when you're done (quit Elden Ring from its menu first)
```

Load your character in Elden Ring and play the Tarnished as usual while MGSV boots hidden (2-4 minutes);
then Snake takes over. All `pr` commands: [docs/TOOLS.md](docs/TOOLS.md).

## Windows

Built, tested under Wine, **never run on Windows**, and there is no prebuilt download yet. On Windows, Elden
Ring starts MGSV hidden by itself; the zip is built on Linux with `python3 -I tools/package_windows.py`. Setup
and status: [docs/WINDOWS.md](docs/WINDOWS.md). If you try it, please report how it went (below).

## How to play

Start it (`tools/pr launch`, see [Install and run](#install-and-run-linux)), load your character in Elden Ring and play the Tarnished
as usual while MGSV boots hidden (2-4 minutes; the top-right corner says what it is doing). "Snake is in control" means
your controller now plays Venom Snake. Use an Xbox-style pad.

**Snake's controls are MGSV's own**, with three exceptions for Elden Ring:

| Button | What it does |
|---|---|
| Left stick / right stick | move (tilt = walk or run) / camera |
| A | tap: crouch / stand up, hold: lie down |
| X | dive |
| B | reload |
| Y | MGSV's action (next to D-Walker: get on; riding: hold to get off; pick up a supply box), **or Elden Ring's** whenever Elden Ring shows its own prompt ("Y: Rest at site of grace", "Y: Retrieve lost runes", items) |
| LT / RT | aim / fire; RT without LT next to an enemy = CQC |
| D-pad | tap up: primary weapons (by default an SVG-76 assault rifle at the hip and an FB MR R-LAUNCHER rocket launcher on the back: hold up and push the right stick to switch), down: handgun (a WU S.PISTOL with tranquilizer darts), right: support items (grenades, mines), left (hold): items, e.g. the cardboard box |
| RB (while aiming) | first person / down the scope: you aim with the gun's own sights (MGSV shows no reticle there) |
| LB (hold) | MGSV's call menu: D-Walker's orders (below) |
| **Start** | **Elden Ring's** menu (equipment, items, system, quit) |
| **View, tapped** | **Elden Ring's** map (and fast travel from it) |
| **View, held a moment (0.4 s)** | **MGSV's iDroid** (supply drops, buddy support, Mother Base); while it is open MGSV has the whole pad; Start closes it |

**Ordering D-Walker.** Hold **LB**: MGSV's call menu opens. Push the **right stick** toward an order (Whistle up-right,
the modes to the right; your current mode is greyed out), **let the stick go** (the order stays selected), then **click the
right stick in** while still holding LB. Releasing LB without the click cancels, and MGSV ignores a click while the stick is
still pushed. Elden Ring's corner then shows the order ("D-Walker: Search Mode -- it marks enemies nearby"); the first
few times you hold LB it also shows how, and it tells you when you let go without giving one.
- **Whistle**: D-Walker comes to you, from anywhere (a far one is brought closer first, then it walks).
- **Normal Mode**: it follows you, a few metres to your left. This is the mode it starts in.
- **Search / Stealth / Intercept Mode** (whatever your save has developed): it stays in that mode and does not follow
  until you whistle or choose Normal Mode again.
- Y next to it mounts it: ride it through Elden Ring, RT (or LT+RT in its cockpit view) burns enemies with its
  flamethrower; hold Y to get off. If D-Walker is ever destroyed in MGSV's world, PhantomRing has MGSV bring it back
  after ~10 s (not yet seen live).

**Guns.** Each round does what it does in MGSV: an assault-rifle round takes a fifth of a soldier's life (five body hits kill
a Limgrave soldier, so does one headshot), a rocket kills the soldier it hits. Rockets fly visibly (a dark body with a bright trail) and blow up
where they hit. The tranquilizer pistol: two body darts, or one in the head, put a soldier to sleep without a fight.

**Grenades and mines.** D-pad right picks the support item, hold LT: the throw arc shows (dots and a ring where it lands,
in Elden Ring's world); press RT while still holding LT to throw (letting both go together throws nothing). Sleep gas puts enemies to sleep, stun grenades stagger them, frag grenades and molotovs
hurt. Mines (LT+RT places them) lie where you put them with a blinking light and go off when an Elden Ring enemy walks
into them. They stay while you play on; a death or fast travel clears them (MGSV takes them back).

**The cardboard box.** Hold D-pad left, push the right stick to the box, let go: Snake is in it (RT takes it off, X dives
out). Keep still in it (or creep slowly for a moment): Elden Ring's enemies that haven't spotted Snake walk past; one
that is already after him, or comes within 2 m, still finds him. Snake's first box carries MGSV's anime pin-up girl on its
lid (`[Loadout] BoxPoster`; X in that item menu changes the poster): an enemy that sees it within 15 m stops and stares for
a few seconds.

**Breaking things.** Shoot a crate, cart or pot, punch it (CQC with no enemy in reach) or blow it up: it breaks as it
would for the Tarnished. Walking into a cart or a rock, Snake stops at it as the Tarnished would, never inside it.

**Dying** is Elden Ring's: YOU DIED, the grace or a Stake of Marika, and Snake gets up with full life. Resting at a grace,
fast travel and Elden Ring's menus work as usual.

## Saves, safety and uninstalling

- **Elden Ring** plays only `PhantomRing0000.sl2`, a copy of your save made on the first launch (your
  `ER0000.sl2` is never touched). It is yours: keep it, or delete it to start the copy over.
- **MGSV never saves** while PhantomRing runs (its writes stay in memory), so your MGSV progress stays as it
  was.
- **Backups.** Every `pr launch` first copies both games' saves to `backups/<time>/` and verifies them by
  sha256. All of them are kept unless you ask for pruning (`--keep-backups N` or `$PHANTOMRING_KEEP_BACKUPS=N`:
  the newest N, the very first one and every `*-pre-restore` one survive; each ER backup is ~140 MB, so prune
  now and then). `pr backup-saves` makes one by hand;
  `pr restore-saves <time> --yes` puts one back (and backs up the current saves first).
- **Online.** Elden Ring always runs offline: me3's profile sets `start_online = false` and `pr launch`
  passes `--online=false`, and Easy Anti-Cheat is never loaded. MGSV runs offline too (`[Online] Offline =
  true`). Since its July 2026 patch, MGSV's first login asks you to pick your country/region and accept
  Konami's updated Terms of Use; PhantomRing never answers that for you.
- **MGSV's folder.** `pr launch` puts `dinput8.dll` (the MGSV plugin) and `PhantomRingGuest.ini` there. The plugin
  only activates when started by `pr launch`; otherwise it passes everything through. It never overwrites a
  `dinput8.dll` from another mod (IHHook, Infinite Heaven): move that aside while you play PhantomRing.
- **Uninstall.** `tools/pr stop`, then `tools/pr uninstall-guest` (removes the two files from MGSV's folder, only
  if they are the ones it installed), then delete this folder. `backups/` holds your save backups: keep what
  you want first.

## Known issues

The full list, with measurements: [docs/STATUS.md](docs/STATUS.md#known-issues).

- Getting on D-Walker right after walking up to it sometimes doesn't work; step back and try again.
- MGSV's aim assist still knows its own (invisible) airport soldiers and can snap the aim toward one.
- Elden Ring's grass is drawn over Snake's models even where it stands behind them.
- D-Walker's footsteps are loud, and it follows Snake 2-3 s late.
- MGSV's own effects (explosions, muzzle flashes, smoke, flames) are replaced by Elden Ring's pot effects
  and Dragonfire flames; muzzle flashes are missing. Grenades and rockets in flight are simple shapes.
- Placed C4 can't be set off yet; CQC throws and chokes need a target MGSV doesn't have.
- Sturdy objects (Limgrave's palisade fences) don't break, even in a blast.
- The cardboard box doesn't shake off an enemy that has already spotted Snake.
- Elden Ring's menu opened with Esc on the keyboard isn't noticed (MGSV's HUD stays on top); the pad's Start
  is. Keyboard and mouse don't drive Snake.
- Flasks can't be drunk while Snake plays.
- Elden Ring shows an occasional 36-40 ms frame in the open world; MGSV slows to ~54 frames/s while the camera
  turns.

## Troubleshooting

- **Start with `tools/pr launch --dry-run`.** It runs every check and says what to fix for each `[FAIL]`.
- **"Steam folder ... has no steamapps/"**: set `PHANTOMRING_STEAM` to your Steam folder (the one holding
  `steamapps/libraryfolders.vdf`).
- **"eldenring.exe is not 1.17.1" / "mgsvtpp.exe is not 1.0.15.4"**: Steam updated the game. PhantomRing
  needs an update for the new version.
- **MGSV never takes over**: `tools/pr status` shows what MGSV is doing, and the top-right corner of Elden Ring
  says it too. `tools/pr logs` shows both plugins' logs. MGSV's save must be able to load Africa free roam.
- **Something is stuck after quitting**: `tools/pr stop` (`--kill` if it reports leftovers).
- **A save went wrong**: the backups are the folders in `backups/`; `tools/pr restore-saves <time>` shows what
  it would restore, and `--yes` puts it back.

## Building and tests

The build is above (`cmake -B build`, MinGW-w64). The offline tests don't need the games:

```bash
python3 tools/phantomring/test_launcher.py    # launcher guards on temp dirs
python3 tools/phantomring/test_fakes.py       # the protocol between two stand-in games, on a private mapping
export WINEPREFIX=$(mktemp -d)                # keeps the wine-based tests off your own prefix
python3 tools/phantomring/test_layout.py      # proto.py vs the C++ header, byte for byte
python3 tools/phantomring/test_plugins.py     # the plugins' link code under wine vs the stand-ins
cmake --build build --target check_host check_guest check_windows
```

What each one needs (wine, Lua 5.1.5 sources, extracted game data): [docs/TOOLS.md](docs/TOOLS.md#tests-offline).
Design: [docs/DESIGN.md](docs/DESIGN.md); per-plugin details: [docs/HOST.md](docs/HOST.md),
[docs/GUEST.md](docs/GUEST.md), [docs/COMBAT.md](docs/COMBAT.md); reverse-engineering notes:
[docs/research/](docs/research/README.md).

## Reporting problems and contributing

**Bug reports** are welcome as GitHub issues. Please include:

- what you did and what happened;
- the output of `tools/pr launch --dry-run` and `tools/pr logs -n 200` (the launch logs and both plugins'
  logs: `build/PhantomRingHost.log` and `MGS_TPP/PhantomRingGuest.log`);
- your distro, desktop, GPU and driver, Proton version, and whether Steam is native or Flatpak.

Logs contain your local paths: look them over before you post them. Never attach saves or `pr record`
files (`.prrec`), which contain MGSV's assets.

**Pull requests**: run the offline tests above first, and keep to the project's rules:

- No game code or assets in the repository, ever.
- tpp-mod is GPL: it may be read for facts, never copied.
- Third-party code must have an MIT-compatible licence and an entry in
  [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
- Elden Ring stays offline.

## Roadmap

| # | Phase | Status |
|---|---|---|
| 0 | Link | done, live |
| 1 | Drive (movement, camera, input, puppet) | done, live |
| 2 | See Snake (MGSV meshes drawn in Elden Ring, lit by Elden Ring) | done, live |
| 3 | Fight (shots/CQC/stun hit Elden Ring enemies; grenades, mines, tranq darts; their hits drain Snake's life; death/respawn) | done, live (C4 detonation open) |
| 4 | D-Walker (deployed, following, drawn, rideable, flamethrower) | done, live |
| 5 | Polish: MGSV HUD layer, Elden Ring lighting, interaction prompts, audio (done); keyboard and mouse | mostly done |
| 6 | MGSV collision from Elden Ring (vault, climb, cover), proxies for native MGSV combat | planned |
| 7 | Windows players ([docs/WINDOWS.md](docs/WINDOWS.md)) | built, tested under Wine; not yet run on Windows |
| 8 | Seamless Co-op | planned |

## Layout

| Folder | |
|---|---|
| `er/` | Elden Ring plugin, `PhantomRingHost.dll` (Mod Engine 3 native) |
| `mgsv/` | MGSV plugin, built as `dinput8.dll` |
| `common/` | shared core: mapping, rings, log, ini, hooks, signature scans |
| `protocol/` | the shared-memory layout |
| `tools/` | `pr` launcher and consoles, fake peers, reverse-engineering scripts |
| `docs/` | design, per-plugin docs, research reports |

## Legal

METAL GEAR SOLID V: THE PHANTOM PAIN and related names are trademarks of Konami Digital Entertainment. ELDEN
RING is a trademark of Bandai Namco Entertainment Inc. and FromSoftware, Inc. All other names and trademarks
belong to their respective owners. PhantomRing is an unofficial fan mod, not affiliated with or endorsed by any
of them.

No game code or assets are included in this repository. Models, textures, animation and sound are read at
runtime from your own installs. The exception is `pr record` files, which you create yourself and must not
share. Screenshots in `docs/screenshots/` show the mod running.

Mod Engine 3 neutralises Arxan so the plugins can load, for offline play only: never play online with this
setup. Use at your own risk.

## Credits and licence

PhantomRing is released under the [MIT License](LICENSE). Third-party parts keep their own licences:
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) lists them, with what was adapted and from where. With thanks
to [SkyCraft](https://github.com/chasmlol/SkyCraft), whose design and shared-memory core PhantomRing grew from,
and to [MGS5VR](https://github.com/nikamigaming-create/MGS5VR), [me3](https://github.com/garyttierney/me3),
[MinHook](https://github.com/TsudaKageyu/minhook), fromsoftware-rs, The Grand Archives, IHHook and Infinite
Heaven, hudhook and IGCS.
