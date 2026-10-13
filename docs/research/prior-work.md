# Lessons from the earlier asset-conversion attempt

Before PhantomRing, an earlier private project tried the opposite approach. It converted MGSV's meshes into Elden
Ring armour, NPC and weapon files, edited `regulation.bin`, and added small native patch DLLs. It ran a dozen
playtest builds and stopped there. PhantomRing replaced it with the passthrough design ([DESIGN.md](../DESIGN.md)).
This note keeps only the technical facts that carried over. The project itself is not included.

## 1. Launching Elden Ring with me3 under Proton

- **Pinned builds.** `eldenring.exe` 1.17.1 (FileVersion 2.7.1.x) is 87,042,128 bytes, SHA-256
  `1a3547101327f65d0c76da2f9190ac0aa66871ea42bae2aecc61e11a8b597891`; its `regulation.bin` is version 11711000.
  `mgsvtpp.exe` 1.0.15.4 is 166,517,760 bytes, SHA-256
  `085c2f82d1c963c40b3d2d55786661dfee2b18cbbf388a710c00fa76c5e9bb45`. Steam auto-updates Elden Ring by default,
  so a hash pin can break overnight: refuse to run on an unknown build.
- **me3 0.13.0** (Linux build) launches Elden Ring with native DLLs from a profile outside the game folder:

  ```bash
  <me3>/bin/me3 --windows-binaries-dir <me3>/bin/win64 --crash-reporting=false \
    launch --game eldenring --exe "<steam-library>/common/ELDEN RING/Game/eldenring.exe" \
    --profile <profile>.me3 --online=false --savefile <Private>0000.sl2
  ```

  The profile is TOML (`profileVersion = "v1"`, `savefile`, `start_online = false`, `[[supports]] game =
  "eldenring"`, `[[natives]] path = ...`); relative paths resolve against the profile's folder.
- **What me3 does** (from its log): picks Proton from Steam's CompatToolMapping and the tool's
  `require_tool_appid` (Proton Experimental -> SteamLinuxRuntime_4), launches with the `waitforexitandrun` verb
  (`ME3_PROTON_LAUNCH_VERB` overrides it), starts `me3-launcher.exe`, which attaches `me3_mod_host.dll`. dearxan
  defers native loading until after Arxan's init routines. It sets `Menu.IsEnableOnlineMode=false` and redirects
  the save to `%APPDATA%\EldenRing\<steamid64>\<savefile>` (and `.sl2.bak`). Logs go to
  `~/.local/share/me3/logs/<profile-name>/`. me3 does not log native loads at INFO level: a DLL's own log is the
  proof it loaded.
- **Diagnostics environment** that keeps Proton logs useful without filling the disk (a full trace reached
  2.2 GB): `PROTON_LOG=1 PROTON_LOG_DIR=<dir> WINEDEBUG=+timestamp,+pid,+tid,warn+seh,err+seh,+debugstr
  VKD3D_DEBUG=warn`.
- **Running your own tool inside the game's prefix**: `<SLR>/_v2-entry-point --verb=run -- <Proton>/proton
  runinprefix tool.exe`, with the same `STEAM_COMPAT_*` environment as the game. `runinprefix` joins the game's
  wineserver, so the tool can open the game process. Linux paths under `/home` are writable from inside the
  runtime container.
- **Launcher guards that proved their worth**: validate the profile (offline, the private save, exactly the
  expected natives); hash the exe; require `StateFlags == 4` in the app manifest (installed, not updating);
  refuse while the game runs; back up the whole save folder with a SHA-256 manifest and re-hash both sides;
  wait for a previous `eldenring.exe` to exit fully before relaunching. `tools/phantomring/launch.py` does all
  of this.

## 2. Native DLLs in Elden Ring

- MinGW-w64 g++ builds work (`-static-libgcc -static-libstdc++ -Wl,--no-insert-timestamp`); no MSVC needed.
- Guard order that worked: refuse if an EasyAntiCheat module is loaded; hash the on-disk exe; compare the
  expected bytes at every patch site; only then patch, and roll back on any failure. Log next to the DLL
  (`GetModuleFileNameW` of the DLL itself); under Proton that file lands in the Linux tree.
- With me3's dearxan, patching `.text` at a handful of sites ran for 30+ minute sessions without Arxan
  interfering. That is evidence for those sites only.
- The exe's on-disk code is not packed: AOB scans of the file work. A second `.text` section at RVA
  `0x4C13000` is Arxan's added code.

## 3. Custom models in Elden Ring (why PhantomRing draws MGSV's own meshes instead)

- The converted Snake replaced the Knight set (model 1570, `{hd,bd,am,lg}_m_1570[_l].partsbnd.dcx`).
- Low-LOD `_l` binders need `<texture>_l` texture names, or summoned actors render chrome.
- Elden Ring's normal maps carry gloss in the blue channel (B = 255 makes everything shiny); vertex layouts
  must match the native ones exactly.
- NPC and phantom shading paths differ from the player's; texture residency on a first summon stayed
  unexplained.

## 4. Known problems that shaped PhantomRing

- **Alt-Tab freeze.** One session hung on Alt-Tab and was force-closed; the recovered stack ran through the
  Steam overlay's Vulkan layer (`vkQueuePresentKHR` -> `VulkanSteamOverlayPresent`). The overlay is the lead
  suspect, unproven. With a second, hidden game, focus and presentation behaviour need early testing.
- **Controller routing.** Steam Input serves the focused app; a hidden second game gets no pad input on its own.
  PhantomRing therefore feeds MGSV a virtual XInput pad ([GUEST.md](../GUEST.md)).
- **Two wineservers.** Elden Ring's prefix and MGSV's prefix do not share Windows named objects, so the two
  games share a host file (`/dev/shm`) instead ([proton-ipc-and-launch.md](proton-ipc-and-launch.md)).
