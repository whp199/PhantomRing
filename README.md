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

## Install

For players on Linux. On Windows, see [Windows](#windows). To build from source, see
[Build from source and run](#build-from-source-and-run-linux).

**Supported versions.** The plugins check the exact game executables. On any other version they stay inert, and the
game runs as usual.

| | Version | |
|---|---|---|
| ELDEN RING | **1.17.1** (`eldenring.exe` 2.7.1.0) | sha256 `1a354710…8b597891` |
| METAL GEAR SOLID V: THE PHANTOM PAIN | **1.0.15.4** | sha256 `085c2f82…c5e9bb45` |
| Mod Engine 3 | **0.13.0** (`me3-linux-amd64.tar.gz`, you download it) | sha256 `f31579de…e1e93fc1` |
| PhantomRing | see `tools/pr --version` | both plugins must come from the same release |

1. **Get PhantomRing.** Download `PhantomRing-<version>-linux.tar.gz` and `SHA256SUMS` from the
   [Releases](https://github.com/whp199/PhantomRing/releases) page. Tagged releases only: until there is one, build from
   source (below). Then:
   ```bash
   sha256sum -c --ignore-missing SHA256SUMS
   tar xzf PhantomRing-*-linux.tar.gz && cd PhantomRing-*-linux
   ```
2. **Get me3.** Download `me3-linux-amd64.tar.gz` from the
   [me3 v0.13.0 release](https://github.com/garyttierney/me3/releases/tag/v0.13.0) into `~/Downloads/`, then run
   `tools/pr install-me3`. It checks the archive's sha256 before unpacking.
3. **Check the machine.** `tools/pr doctor` checks Steam, both game versions, me3, gamescope, Proton, the plugins and
   your saves. It changes nothing. Fix every `[FAIL]` line; each one says how.
4. **Prepare both games once** (details in [step 3 below](#build-from-source-and-run-linux)). MGSV's Continue must load
   a save that can go to Africa. Elden Ring needs a character in `ER0000.sl2`.
5. **Play.** `tools/pr launch` backs up your saves and starts both games: MGSV is hidden, Elden Ring is offline. Load
   your character, and Snake takes over after 2-4 minutes. `tools/pr stop` when you're done.
6. **Updating.** Replace the whole folder (both plugins come from one release: `tools/pr --version` lists them). Mixed
   plugins don't link up, and Elden Ring's corner says so in red ("PhantomRing versions differ: ... -- reinstall both").
   Every launch keeps a ~140 MB save backup; `tools/pr doctor` warns when they pile up (`--keep-backups N` prunes).

Something wrong? Run `tools/pr collect-logs` and [open an issue](https://github.com/whp199/PhantomRing/issues/new/choose)
with the zip it writes.

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

## Build from source and run (Linux)

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

Built and tested under Wine, but **never run on Windows**. On Windows, Elden Ring starts MGSV hidden by itself.
The zip (`PhantomRing-<version>-windows.zip`) is attached to tagged releases, or you can build it on Linux with
`python3 -I tools/package_windows.py`. Setup and status: [docs/WINDOWS.md](docs/WINDOWS.md). If you try it, please
report how it went (below).

## How to play

Start it (`tools/pr launch`, see [Install](#install)), load your character in Elden Ring and play the Tarnished
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
| LT / RT | aim / fire; RT without LT, standing, next to an enemy = CQC strike |
| D-pad | tap up: primary weapons (by default an SVG-76 assault rifle at the hip and an FB MR R-LAUNCHER rocket launcher on the back: hold up and push the right stick to switch), down: handgun (a WU S.PISTOL with tranquilizer darts), right: support items (grenades, mines), left: items -- hold it and push the right stick to pick one (the cardboard box, the night-vision goggles), a quick tap uses the one picked |
| RB (while aiming) | first person / down the scope: you aim with the gun's own sights (MGSV shows no reticle there) |
| LB (hold) | MGSV's call menu: D-Walker's orders (below) |
| **Start** | **Elden Ring's** menu (equipment, items, system, quit) |
| **View, tapped** | **Elden Ring's** map (and fast travel from it) |
| **View, held a moment (0.4 s)** | **MGSV's iDroid** (supply drops, buddy support, Mother Base); while it is open MGSV has the whole pad; Start closes it |
| **View + Start, held together 1 s** | **Tarnished mode**: you play the Tarnished (flasks, spirit ashes, Torrent, jumps, answering NPCs); the same again gives Snake back, where the Tarnished stands. Neither button opens a menu or the iDroid |

**Tarnished mode.** Everything Snake can't do yet, the Tarnished does: hold **View + Start together for a second** and the
corner says "Tarnished mode -- View+Start to return to Snake". He shows with his weapon and Elden Ring has the whole pad: X
drinks a flask (the quick item), and so on; Snake waits, parked, in MGSV. The same chord gives Snake back where the Tarnished
stands, and what the Tarnished went through is Snake's too: a Flask of Crimson Tears that took the Tarnished from 74% to 100%
gives Snake the same share of his life back, a boss's hit takes it. Snake takes over only on foot: on Torrent (or a
ladder) the corner says "get off Torrent first". Torrent: the Spectral Steed Whistle from a quick slot, or Start ->
Inventory -> Tools -> the whistle -> Use.

**Ladders.** Y on a ladder's "Climb" prompt: the Tarnished takes the ladder and you climb it with the left stick, as in Elden
Ring; Snake is back where the Tarnished gets off (let go of the stick first: Snake doesn't run on with it). To go down, stand at
the top next to the hole and turn Snake to face it (a light touch of the stick turns him): "Descend" shows, Y. Walking on into
the hole drops Snake down the shaft, as it would the Tarnished (a 19 m tower costs ~90% of his life).

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

**D-Horse instead of D-Walker** (optional): set `[Buddy] Type = horse` in `PhantomRingGuest.ini` (in MGSV's folder) before
`tools/pr launch`, or switch while you play: MGSV's iDroid (hold View) > Missions > Buddy Support > D-Horse ("Request deployment",
GMP spent in memory only, MGSV never saves), or `tools/pr lua '!buddy horse'` (`!buddy walker` back). The horse waits where it
is; hold LB and pick **Whistle** and it gallops over (PhantomRing also whistles it when it is more than 15 m away). Y next to
it mounts, the left stick walks it, **tap X** to canter and gallop (13-17 m/s), Y gets off. It is drawn under Snake and
stands on Elden Ring's ground; walls and ruins stop it as they stop Snake, and galloping off a ledge hurts like a fall. The
ride is limited by MGSV's own world (Known issues), so ride it from point to point (100 m at a time is fine), not for
kilometres. The next launch deploys the ini's buddy again.

**Guns.** Each round does what it does in MGSV: an assault-rifle round takes a fifth of a soldier's life (five body hits kill
a Limgrave soldier, so does one headshot), a rocket kills the soldier it hits. Rockets fly visibly (a dark body with a bright trail) and blow up
where they hit. The tranquilizer pistol: two body darts, or one in the head, put a soldier to sleep without a fight.

**Progression** (optional, off by default): `[Progression] damage_scaling = level | weapon | both` in `PhantomRingHost.ini`
makes the Tarnished's rune level and his right-hand weapon's upgrade scale Snake's damage ([docs/COMBAT.md](docs/COMBAT.md)):
x(level / 20)^0.5 and x(1 + 1.5 x upgrade / max upgrade), between x0.5 and x3. A level-15 Tarnished with a +0 weapon: a rifle
round on a soldier did 35 off, 31 with `level`. Try it while you play: `tools/pr er "progression level"` (`off`, `weapon`,
`both`); the ini makes it stick. His Vigor already counts: Elden Ring's hits cost Snake the share of the Tarnished's HP they take.

**Life.** Elden Ring's hits take Snake's life as MGSV's own would, and he recovers as in MGSV: nothing for ~2.4 s after a hit,
then slowly, then faster (full ~7 s after a 20% hit, ~12 s after a 65% one). Each new hit starts it again, so standing in a
boss's combo kills him (`[Combat] RegenGate` in `PhantomRingGuest.ini`; `false` brings back the old instant regeneration).

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

**Night vision.** Hold D-pad left, push the right stick to the NVG and let go: the goggles go on (a quick tap of D-pad
left takes them off and puts them on again). Elden Ring's picture -- the world, Snake and D-Walker -- turns MGSV's bright
green, the dark lifted most; Elden Ring's HUD and menus stay as they are. Off in cutscenes, loads and while dying.

**Ammo and life refill.** Resting at a grace refills Snake as it refills the Tarnished's flasks: his life, every magazine,
the ammo reserve and his grenades and other items. So does every respawn (a death, the grace or a Stake of Marika). A
supply drop from the iDroid still works too.

**Dying** is Elden Ring's: YOU DIED, the grace or a Stake of Marika, and Snake gets up with full life and ammo. A fall into
a chasm with no ground to land on counts as a death after a few seconds (~60 m). Resting at a grace (its menu: Pass time,
Level Up, ...), fast travel, NPCs' questions and Elden Ring's menus (Start, or Esc and G on the keyboard) work as usual:
Elden Ring has the pad while one of its screens is up.

**Bosses.** Walking into a boss's mist: Y on "Traverse the mist" -- Snake steps aside for a moment while Elden Ring walks the
Tarnished through, then he plays on inside. The boss's health bar is Elden Ring's HUD; Snake keeps the pad for the fight.
A boss counts as many soldiers: one 30-round SVG-76 magazine takes about 14.5% of Margit's health. Margit's hits cost Snake
11-45% of his life each.

## Saves, safety and uninstalling

- **Elden Ring** plays only `PhantomRing0000.sl2`, a copy of your save made on the first launch (your
  `ER0000.sl2` is never touched). It is yours: keep it, or delete it to start the copy over.
- **MGSV never saves** while PhantomRing runs (its writes stay in memory), so your MGSV progress stays as it
  was.
- **Backups.** Every `pr launch` first copies both games' saves to `backups/<time>/` and verifies them by
  sha256. All of them are kept unless you ask for pruning (`--keep-backups N` or `$PHANTOMRING_KEEP_BACKUPS=N`:
  for each game the newest N and the very first one survive, and every `*-pre-restore` one; each ER backup is
  ~140 MB, so prune now and then). `pr backup-saves` makes one by hand;
  `pr restore-saves <time> --yes` puts one back (and backs up the current saves first). For Elden Ring it restores
  only `PhantomRing0000.sl2` (and its `.bak`), never your other saves in that folder (`--all-er-files` does).
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

- Elden Ring's grass is drawn over Snake's models even where it stands behind them.
- D-Walker's footsteps are loud, and it follows Snake 2-3 s late.
- MGSV's own effects (explosions, muzzle flashes, smoke, flames) are replaced by Elden Ring's pot effects
  and Dragonfire flames; muzzle flashes are missing. Grenades and rockets in flight are simple shapes.
- Placed C4 can't be set off yet; CQC throws and chokes need a target MGSV doesn't have.
- Sturdy objects (Limgrave's palisade fences) don't break, even in a blast.
- The cardboard box doesn't shake off an enemy that has already spotted Snake.
- Keyboard and mouse don't drive Snake (Elden Ring's Esc and G menus are noticed).
- Flasks, spirit ashes, Torrent and jumps need Tarnished mode (View + Start held 1 s).
- Snake's life can tick up by up to ~2% for a moment after a hit before MGSV's recovery curve holds it back again.
- D-Horse can't be moved by PhantomRing inside MGSV's world, so while Snake rides it MGSV's own map limits the ride (the
  airport's terminal ~40 m one way, open ground ~250 m another); and after getting off far away, the horse jumps back to
  Snake in Elden Ring and gallops in. D-Dog and Quiet are untested.
- At a ladder's top, walking into the hole drops Snake down the shaft (face the hole and press Y on "Descend" instead).
- `[Puppet] motion = root_motion` (Elden Ring's own movement for the hidden Tarnished) is there to try
  (`tools/pr er "puppet motion root_motion"`); `kinematic` stays the default: neither is clearly better yet.
- Skipping a boss's intro cutscene with Start: fixed and unit-tested, not yet re-tried in the game (Margit's plays only
  once per save).
- MGSV's night-vision goggles on Snake's head are drawn with a noisy texture on their lenses.
- Elden Ring shows an occasional 36-40 ms frame in the open world; MGSV slows to ~54 frames/s while the camera
  turns.

## Troubleshooting

- **Start with `tools/pr doctor`**: versions, me3, gamescope, Proton, the build, your saves. Then run `tools/pr launch
  --dry-run`, which checks what one launch needs. Both say what to fix for each `[FAIL]`.
- **"PhantomRing versions differ: Elden Ring plugin vX, MGSV plugin vY -- reinstall both"** (in Elden Ring's corner):
  the two plugins come from different builds. Rebuild both (`cmake --build build`) or unpack one release package
  whole; `tools/pr --version` shows what you have.
- **"Steam folder ... has no steamapps/"**: set `PHANTOMRING_STEAM` to your Steam folder (the one holding
  `steamapps/libraryfolders.vdf`).
- **"eldenring.exe is not 1.17.1" / "mgsvtpp.exe is not 1.0.15.4"**: Steam updated the game. PhantomRing
  needs an update for the new version.
- **MGSV never takes over**: `tools/pr status` shows what MGSV is doing, and the top-right corner of Elden Ring
  says it too. `tools/pr logs` shows both plugins' logs. MGSV's save must be able to load Africa free roam.
- **Something is stuck after quitting**: `tools/pr stop` (`--kill` if it reports leftovers).
- **Elden Ring drops to ~10 frames/s**: don't build PhantomRing (`cmake --build`) or run other heavy jobs while you play;
  both games together use most of the machine.
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
python3 tools/phantomring/test_replay.py      # record/replay of a session between the stand-ins
python3 tools/phantomring/test_release.py     # versions, both packages (reproducible), pr doctor, collect-logs
cmake --build build --target check_host check_guest check_windows
```

CI runs all of these on every push ([.github/workflows/ci.yml](.github/workflows/ci.yml)). A `v<VERSION>` tag also
publishes both packages as a GitHub pre-release. The version lives in one place, the [VERSION](VERSION) file
([CONTRIBUTING.md](CONTRIBUTING.md#building-and-testing)).

What each one needs (wine, Lua 5.1.5 sources, extracted game data): [docs/TOOLS.md](docs/TOOLS.md#tests-offline).
Design: [docs/DESIGN.md](docs/DESIGN.md); per-plugin details: [docs/HOST.md](docs/HOST.md),
[docs/GUEST.md](docs/GUEST.md), [docs/COMBAT.md](docs/COMBAT.md); reverse-engineering notes:
[docs/research/](docs/research/README.md).

## Reporting problems and contributing

**Bug reports** are welcome as [GitHub issues](https://github.com/whp199/PhantomRing/issues/new/choose); the form
asks for the following:

- what you did and what happened;
- the first line of `tools/pr --version` and the output of `tools/pr doctor`;
- the zip from `tools/pr collect-logs`: both plugins' logs (`build/PhantomRingHost.log`,
  `MGS_TPP/PhantomRingGuest.log`), the last launch's logs, the inis, the versions. Your home folder, user name, host
  name and Steam ids are redacted;
- your distro, desktop, GPU and driver, Proton version, and whether Steam is native or Flatpak.

Look the logs over before you post them anyway. Never attach saves or `pr record` files (`.prrec`), which contain
MGSV's assets.

**Pull requests**: read [CONTRIBUTING.md](CONTRIBUTING.md) (the legal rules, how to test, live-test etiquette),
run the offline tests above first, and keep to the project's rules:

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
