# PhantomRing on Windows

Play Elden Ring as Venom Snake on Windows. Elden Ring runs as usual (offline, through Mod Engine 3);
Metal Gear Solid V runs **hidden in the background** (no window, no taskbar button) and supplies Snake:
his movement, camera, animation, weapons, life and D-Walker.

> **Status: built and tested offline only.** Everything here was developed and verified on Linux
> (Proton). The Windows paths (shared memory between the games, Elden Ring starting MGSV, the hidden
> MGSV window) pass their tests under Wine, but **nobody has run PhantomRing on a real Windows PC yet**.
> See "What is untested" at the end, and please send the two log files if something goes wrong.
>
> **No prebuilt Windows download yet.** The zip this page describes is built on Linux with
> `python3 -I tools/package_windows.py` (README, "Building"). Builds are MinGW-w64 only (GCC or Clang
> with GNU assembler syntax); MSVC can't build PhantomRing.

Fan project, not affiliated with Konami, FromSoftware or Bandai Namco. You need to own both games.

## What you need

| | |
|---|---|
| Windows | 10 or 11, 64-bit |
| Steam | installed, running and signed in (both games check it) |
| Elden Ring | **1.17.1** (eldenring.exe 2.7.1.0), installed through Steam |
| MGSV: The Phantom Pain | **1.0.15.4** (the July 2026 patch), installed through Steam |
| Mod Engine 3 | **me3 0.13 or newer** for Windows (github.com/garyttierney/me3, its installer registers `.me3` files) |
| A controller | Xbox-style (XInput). Keyboard and mouse don't drive Snake yet |
| A save in MGSV | that can load **Africa free roam** (Snake is parked on the Nova Braga Airport apron). D-Walker developed if you want him (optional) |
| A strong PC | both games run at once; MGSV renders in the background |

The plugins check the exact game versions (file hashes). With any other version they do nothing and the
game runs normally.

## One-time setup

1. **MGSV settings.** Start MGSV once from Steam, normally:
   - if it asks for your country/region and Konami's updated Terms of Use (July 2026 patch), answer it
     yourself; PhantomRing never does, and runs MGSV offline anyway;
   - Options -> Graphics: **Windowed** (or borderless), resolution **1280x720** (it is never shown; lower
     costs less), and moderate quality;
   - make sure Continue loads a save that can go to Africa;
   - quit MGSV.
2. **Unzip** `PhantomRing-<version>-windows.zip` anywhere, e.g. `C:\Games\PhantomRing`. Keep the files
   together; the `mgsv` folder must stay next to `PhantomRingHost.dll`.
3. **Other MGSV mods.** If MGSV's folder (`...\steamapps\common\MGS_TPP`) already has a `dinput8.dll`
   (IHHook, Infinite Heaven, another mod), move it somewhere else while you play PhantomRing.
   PhantomRing never overwrites a `dinput8.dll` it didn't put there; it just won't start MGSV.
4. **Elden Ring save.** PhantomRing plays on its own copy of your save, so your normal one is never touched:
   - open `%APPDATA%\EldenRing\<a long number>\`;
   - back up the folder;
   - copy `ER0000.sl2` to `PhantomRing0000.sl2` in the same folder.
   Without that copy Elden Ring starts as if you had no character.

## Playing

1. Start Steam.
2. Double-click `phantomring.me3` (or in a terminal: `me3 launch --profile C:\Games\PhantomRing\phantomring.me3`).
   Elden Ring starts **offline**, with the PhantomRing plugin.
3. The plugin starts MGSV by itself, hidden. The first time it also copies `dinput8.dll` and
   `PhantomRingGuest.ini` into MGSV's folder. Nothing appears on screen. Task Manager shows `mgsvtpp.exe`.
4. Load your character. MGSV needs about **4 minutes** to boot, load its save and reach its sandbox;
   until then you play the Tarnished as usual. Then Snake takes over: you see Snake (and D-Walker) in
   Elden Ring, and the camera is MGSV's.
5. Controls: everything is MGSV's (move, stances, aim, fire, CQC, D-Walker; hold **LB** for D-Walker's
   orders), except **Start** (Elden Ring's menu) and **View/Back** tapped (Elden Ring's map; held 0.4 s it
   opens MGSV's iDroid). **Y** is Elden Ring's action whenever Elden Ring shows its own prompt (rest at a
   grace, retrieve lost runes, pick up items). The full table is in the README ("How to play").
6. Quit Elden Ring as usual. MGSV quits by itself about **2 minutes** later (or end `mgsvtpp.exe` in
   Task Manager). If you start Elden Ring again before that, it reuses the running MGSV (no 4-minute wait).

**Saves.** Elden Ring saves to `PhantomRing0000.sl2` only. MGSV **never saves** while PhantomRing runs
(its writes are kept in memory), so your MGSV progress stays exactly as it was.

**Online.** Elden Ring always runs offline and Easy Anti-Cheat is never involved. MGSV runs offline too.

**Sound.** MGSV's music is muted, but its other sounds (Snake's gunfire, D-Walker) play from the hidden
MGSV. Use Windows' volume mixer to change MGSV's volume.

## Settings

- `PhantomRingHost.ini` next to the plugin. `[Launch]` controls the start of MGSV:
  - `StartMgsv = auto` (on for Windows);
  - `MgsvExe =` the full path of `mgsvtpp.exe` if Steam's library list doesn't find it;
  - `InstallGuest = true`.
- `PhantomRingGuest.ini` **in MGSV's folder** (copied there at the first start; your edits there are
  kept). `[Window]`:
  - `Hidden = auto` (hide MGSV's window on Windows);
  - `QuitWithoutHostSec = 120`;
  - and more, documented in the file.
  `Hidden = false` shows MGSV's window (handy to see what MGSV is doing; it still never takes the focus).

## When something goes wrong

Two log files tell what happened:
- `PhantomRingHost.log` next to `PhantomRingHost.dll` (Elden Ring side);
- `PhantomRingGuest.log` in MGSV's folder (MGSV side).

| Log line | Meaning / fix |
|---|---|
| `exe mismatch` (host) | Elden Ring isn't 1.17.1: PhantomRing stays off |
| `launcher: MGSV not found through Steam` | set `[Launch] MgsvExe = C:\...\MGS_TPP\mgsvtpp.exe` |
| `guest: ...dinput8.dll exists and is not PhantomRing's` | another mod's `dinput8.dll` is in MGSV's folder: move it away |
| `mgsvtpp.exe is running ... but no PhantomRing guest answers` | MGSV was started normally (from Steam): close it, restart Elden Ring |
| `MGSV (pid N) exited ... with code ...` | MGSV closed during boot: is Steam running and signed in? Start MGSV once from Steam to see its error |
| `PHANTOMRING is not 1: plain dinput8 proxy` (guest) | MGSV was started without PhantomRing (normal play): fine |
| `is not mgsvtpp.exe 1.0.15.4` (guest) | MGSV isn't 1.0.15.4: PhantomRing stays off in MGSV |
| `hidden: the visibility check is not the expected code` | MGSV won't draw while hidden: Snake can't be shown. Report it |

**Uninstall.** Delete the PhantomRing folder. In MGSV's folder delete `dinput8.dll`,
`PhantomRingGuest.ini`, `PhantomRingGuest.installed`, `phantomring.default.lua`, `PhantomRingGuest.log`
and `PhantomRingSaveSink`. MGSV started from Steam ignores PhantomRing's `dinput8.dll` anyway: it only
acts when Elden Ring's plugin starts MGSV.

## How it differs from Linux

| | Linux (Proton) | Windows |
|---|---|---|
| Shared memory | a file in `/dev/shm`: each game has its own Wine prefix | named mapping `Local\PhantomRing_v1` |
| Hiding MGSV | headless gamescope | the guest hides MGSV's own window and tells it it has the focus |
| Starting MGSV | `tools/pr launch` | Elden Ring's plugin (`er/Launcher.cpp`), with the guest installed by hash |
| Process ids in the shared header | unix pids | Windows pids |

The rest of PhantomRing is the same code on both.

## What is untested on real Windows

Verified offline (under Wine, `cmake --build build --target check_windows`):
- the platform decisions (`common/Platform.h`);
- the Steam library lookup against a fake Steam tree;
- every guest-install case, with real files;
- the environment block and the start of a child process;
- **both plugins' real link code** in two processes over `Local\PhantomRing_v1`: guest started first, attach,
  state, pad, events both ways, Elden Ring restart / re-attach, clean exit;
- the hidden-window behaviour on a stand-in game window: hidden, never minimised or shown, off the taskbar,
  focus answered, focus-loss messages swallowed, later subclasses kept behind the filter;
- both DLLs import only stock Windows DLLs (`tools/check_imports.py`: kernel32, user32, advapi32, bcrypt, msvcrt).

Not yet seen on a real Windows PC:
- me3 for Windows loading the plugin and its `savefile`/offline settings;
- MGSV started directly with `SteamAppId=287700`. Steam and Denuvo should accept it (MGSV has no
  `RestartAppIfNecessary`), and mod loaders do the same, but nobody has checked;
- MGSV staying hidden **and still drawing** (forced visibility check `0x14024A1D0`) at a sane frame rate,
  with Present on a hidden window;
- the capture hooks on Microsoft's own D3D11 and the renderer on Microsoft's own D3D12 (on Linux they
  run on DXVK and vkd3d-proton);
- whether me3's job lets MGSV break away (if not, MGSV closes together with Elden Ring);
- MGSV's sound and performance with both games on one desktop.

## For developers

```bash
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
flock build/.buildlock cmake --build build
flock build/.buildlock cmake --build build --target check_windows   # Windows-mode tests under wine
python3 -I tools/package_windows.py                                  # -> dist/PhantomRing-<version>-windows.zip
```

- `PHANTOMRING_PLATFORM=windows` (test only) makes a process under Wine take the Windows paths: it uses
  the named mapping, hides the window and starts MGSV.
- The closest Linux stand-in for the real thing is one Wine prefix for both games with that variable set.
- `pr lua '!window'` (or the guest console) prints the hidden-window state.
