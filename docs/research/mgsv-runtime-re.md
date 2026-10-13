# MGSV runtime reverse-engineering reference for the guest DLL

Target: `mgsvtpp.exe` 1.0.15.4, the July 2026 Steam patch (`version_info.txt`: `Tpp_steam_mst_en_day3900Mgo_patch_0707_1632`).
Purpose: everything the PhantomRing MGSV-side (guest) DLL needs in order to load into MGSV, find the Lua state, read and drive the player, block or inject input, and run with the window hidden.
Date: 2026-10-11. Author: research agent.

Each claim carries one of two tags:
- **VERIFIED** means I checked it against the local bytes of `<steam-library>/common/MGS_TPP/mgsvtpp.exe`, using a disassembly, an AOB scan, string or xref searches, or the PE headers. It can also mean I read the cited project source. Where a statement goes past the bytes (for example what a value means at runtime), the text says so.
- **UNVERIFIED** means it comes from memory, a web page, or third-party docs that I did not reproduce, or it is a runtime behaviour that cannot be confirmed without launching the game. The rules for this task forbid launching it.

All addresses are virtual addresses (VAs). They assume the fixed image base `0x140000000`, which section 1 shows to be fixed.

Saved data and tools that support this document:
- Data: `docs/research/data/mgsv/`. It holds the full `vars` registration dump, the full `Player.*` C-function table, the import list, the Fox Lua facade vtable, and AOB scan results.
- Tools: `tools/re/mgsv/`, a set of Python scripts. They use capstone from `<capstone>`. Run them as `python3 -I tools/re/mgsv/<x>.py <exe> ...`.

---------------------------------------------------------------------------------------------------

## 0. Key findings

1. **The local exe is 1.0.15.4, not 1.0.15.3.** TimeDateStamp is `0x6A4CB898` (2026-07-07 08:28:08 UTC). IHHook r17 and Infinite Heaven up to r264 only support **1.0.15.3**. IHHook's address tables point past the end of this image (for example `lua_newstate 0x14c1fc960`, while SizeOfImage ends at `0x14a01d000`). Its version check (`FileVersion == 1.0.15.3`) would also fail, so on this exe IHHook turns its hooks off. **VERIFIED.**
2. **Two maintained open-source projects target this exact exe:**
   - **MGS5VR** (MIT) pins sha256 `085c2f82…bb45`.
   - **tpp-mod** (GPL-3) pins sha1 `4F643576…43E3`.
   Both hashes match the local file, and both projects load as a `dinput8.dll` proxy. Their 1.0.15.4 addresses for Lua, camera, skeleton, input and the frame loop are cross-checked below. Every byte signature I re-checked matches. **VERIFIED.**
3. **The exe is Denuvo Anti-Tamper protected.** Evidence:
   - randomised section names;
   - a 111 MB RWX `.xdata` section that holds the entry point;
   - an `ExportProductMetadata` export that points to a `0VND` blob;
   - the UTF-16 string `Denuvo GmbH`;
   - a second import set with hardware-ID calls (`GetVolumeInformationW`, `GetComputerNameW`, `NtQuerySystemInformation`, `wininet` and others).
   It is **not** SteamStub (there is no `.bind` section). Relocations are stripped and there is no DYNAMIC_BASE flag, so the image always loads at `0x140000000`. **VERIFIED.**
   In 1.0.15.4 the game code in `.xtext`, including all of Lua, is plain MSVC code and is hookable with MinHook, as IHHook, tpp-mod and MGS5VR all do. In 1.0.15.3 the Lua functions had been moved into the Denuvo "hulk" at `0x14c…`.
4. **The Lua 5.1.5 C API is fully mapped for 1.0.15.4** (section 3). It is statically linked, Fox-modified, and includes LuaBitOp (`bit`). The mapping was proved three independent ways:
   - the alphabetical link order;
   - semantic checks such as `luaL_openlibs` and `luaL_newstate` bodies;
   - a Fox "Lua facade" vtable at `0x142305720`, whose ~90 thunks tail-jump to each API function.
   tpp-mod and MGS5VR addresses agree.
5. **There are three proven ways to run Lua from C++ on the game's Lua thread:**
   - hook `luaL_openlibs` (`0x141a16980`) to register a module, as IHHook does;
   - hook `lua_pcall` (`0x141a116c0`) and pump queued scripts when the call depth is 1, as MGS5VR does;
   - hook `fox CoreFramework::EnterFrame` (`0x14007f840`) and take the Lua lock with `fox::LuaAutoPtr` (`0x14006a590`), as tpp-mod does.
   **VERIFIED** (the addresses are function starts and the sources were read).
6. **Player data is reachable natively without Lua:**
   - The `vars` ScriptVar block is `*(u64*)0x142c16920`. Its fields include `playerPosX/Y/Z` (float at +0xEC/+0xF0/+0xF4), `playerRotY` (int16 at +0x108), `playerLife` (u16 at +0x11C) and `playerCameraRotation` (float[2] at +0x12C). The full table is in section 7.1.
   - `PlayerVars` (sticks and scanned buttons) are at static addresses `0x142c1c07c..0x142c1c0ac`.
   - MGS5VR adds the per-frame player camera owner (root matrix, head bone, camera pose) and the **skeleton skin-publication hook** (`0x141a6caa0`), which exposes model-space bone quaternions and positions plus the render palette.
   **VERIFIED** (registration code was disassembled and the MGS5VR byte signatures match). What the values mean at runtime is partly **UNVERIFIED**.
7. **Input.**
   - Keyboard and mouse use **Raw Input** (`RegisterRawInputDevices`, mouse and keyboard). `RIDEV_INPUTSINK` is off unless a config bit is set, so normally there is no background keyboard or mouse input.
   - Gamepads use **XInput 1.3** `XInputGetState` (import by ordinal 2, IAT slot `0x14208e070`), which works whatever the focus.
   - DirectInput8 is used only to enumerate **game controllers** (`DI8DEVCLASS_GAMECTRL`).
   - The window and its `GetMessageW` pump run on a **dedicated window thread**.
   - The renderer skips drawing when the window is not visible or is minimised.
   - A watcher thread posts engine events 10 and 11 when the window becomes iconic or is restored.
   - Exclusive fullscreen has focus-loss handling: it minimises the window and later forces it back to the foreground.
   The **hidden-MGSV recipe** that follows from this (section 8.6): windowed mode, never minimise, hide the window or move it off-screen, feed input through an XInput IAT hook, make DirectInput enumerate nothing, and guard `SetCursorPos` and `SetForegroundWindow`. **VERIFIED** (code); the runtime effect is **UNVERIFIED**.

---------------------------------------------------------------------------------------------------

## 1. Local executable identity (VERIFIED)

| Field | Value |
|---|---|
| Path | `<steam-library>/common/MGS_TPP/mgsvtpp.exe` |
| Size | 166,517,760 bytes |
| sha256 | `085c2f82d1c963c40b3d2d55786661dfee2b18cbbf388a710c00fa76c5e9bb45` |
| sha1 | `4f643576c6f6ecf6c67a9aef4d4f02285ada43e3` |
| PE TimeDateStamp | `0x6A4CB898` = 2026-07-07 08:28:08 UTC |
| FileVersion / ProductVersion (VS_FIXEDFILEINFO) | 1.0.15.4 (`0x00010000 / 0x000F0004`) |
| OriginalFilename / InternalName | `Tpp_main.exe` |
| ImageBase / SizeOfImage | `0x140000000` / `0x0A01D000` (image VA range `0x140000000..0x14A01D000`) |
| Characteristics | `0x23` (RELOCS_STRIPPED, EXECUTABLE, LARGE_ADDRESS_AWARE) |
| DllCharacteristics | `0x8120` (HIGH_ENTROPY_VA, NX_COMPAT, TS_AWARE); **no DYNAMIC_BASE**, so the base is always fixed |
| EntryPoint | `0x1469B5F80` (inside `.xdata`, the Denuvo stub: saves all registers, walks the section headers) |
| TLS callbacks | `0x146CFC4CE`, `0x145AAB920`, `0x14000E200` |
| Linker | MSVC 11.0 (VS2012); imports `MSVCP110` and `MSVCR110` |
| PDB | `mgsvtpp.pdb`, RSDS GUID `c8e1e51759dd4126852780933594d2bd`, age 4 |
| Export | 1 export: `ExportProductMetadata` → RVA `0x2F0CF4C`, data beginning `30 56 4E 44` ("0VND", Denuvo metadata) |
| `steam_appid.txt` | `287700` (present in the install dir) |
| `version_info.txt` | `Tpp_steam_mst_en_day3900Mgo_patch_0707_1632` |
| Sibling `mgsvmgo.exe` | sha1 `1582b87c…cd89`, TimeDateStamp `0x6A4CA0BF` (tpp-mod lists it as MGO 1.1.2.9) |

Steam build id: 24176213, according to MGS5VR `docs/ARCHITECTURE.md` (**UNVERIFIED** locally; no `appmanifest_287700.acf` was found under the library).

Sections (raw names; the names are randomised by Denuvo, the real role is in brackets):
```
.xtext   VA 0x140001000 size 0x208C000  RX     [real game code, MSVC]
.edata   VA 0x14208D000 size 0x956000   R      [.rdata: IAT at 0x14208D000, strings, vtables]
.data2   VA 0x1429E3000 vsz 0x2C1000    RW     [.data + .bss; raw part ends 0x142B59000]
.didata  VA 0x142CA4000 size 0x19E000   R      [role not determined; UNWIND_INFO is in .edata]
.bss/.text/.impdata(.rsrc)/.trace       small
.xdata   VA 0x142F03000 size 0x6F74DF4  RWX    [Denuvo VM/stub code, holds the entry point]
.00cfg   VA 0x149E78000                 RX     [import directory + Denuvo imports + TLS callbacks array]
.xtls/.debug$P(export)/.ecode(.pdata, 141,291 RUNTIME_FUNCTIONs: 138,460 in .xtext, 2,831 in .xdata)
```
Denuvo evidence (VERIFIED):
- the `ExportProductMetadata` "0VND" blob;
- the UTF-16 string `Denuvo GmbH` at file offset `0x9A67880`;
- the second, lower-case import set (see section 8.1);
- the entry point inside RWX `.xdata`.
DSOGaming reported in 2015 that MGSV uses Denuvo Anti-Tamper (**UNVERIFIED** web source).

Practical consequences:
- Static VAs are safe to use once the build is checked: compare `sha256`, or more cheaply `TimeDateStamp==0x6A4CB898 && SizeOfImage==0xA01D000`.
- Denuvo may need online re-activation, and players report the game "would not run offline" (**UNVERIFIED** forum claims). Steam must be running.
- tpp-mod notes that "denuvo obfuscates game functions randomly at compile time, each binary has a different set of obfuscated functions". Every function used below sits in `.xtext` as normal MSVC code (**VERIFIED** for each listed address).
- Some callers of Lua and window functions live in `.xdata`, for example the only direct reference to `luaL_openlibs` is a `jmp` from `.xdata:0x1437d6917`. Hooking the *callee* in `.xtext` still works.

### 1.1 How IHHook identifies the version, compared with this exe

Source: TinManTex/IHHook master `71801f1` (r17, 2022-07-13). Branches `dev` (`64c02df`) and `texdev` (`8e108e0`) are similar.
- `IHHook.h`: `static const DWORD GameVersion[4] = {1,0,15,3}`.
- `OS::CheckVersionDelta()` reads `GetFileVersionInfo(<gamedir>\<exe>)` → VS_FIXEDFILEINFO and compares the four words.
  - Local exe 1.0.15.4 gives delta −1 → `"ERROR: IHHook->exe version mismatch … Please update Infinte Heaven."` → `isTargetExe=false` → `doHooks=false`, so no Lua or other hooks.
- `IHH::GetLangVersion()` parses `version_info.txt` for the prefix `Tpp_steam_mst_` plus `en` or `jp` to choose `mgsvtpp_adresses_1_0_15_3_en.h` or `_jp.h`. Locally it would pick `en`.
- It skips any exe whose name contains `mgo`.
- The fallback `forceUsePatterns=true` sig-scans `hooks/mgsvtpp_patterns.h`. On 1.0.15.4 only 41 of 113 patterns match (36 of them uniquely). Most of the rest fail because the 1.0.15.3 patterns were cut from Denuvo-re-encoded code (see 4.4). So IHHook r17 cannot simply be dropped in.
- IH r264 (Nexus, 2026-05-11) also lists "1.0.15.3 in exe" (**UNVERIFIED**, from a web search summary).

---------------------------------------------------------------------------------------------------

## 2. Getting a DLL into MGSV

### 2.1 dinput8.dll proxy (used by IHHook, tpp-mod and MGS5VR)
- `mgsvtpp.exe` **statically imports `DINPUT8.dll!DirectInput8Create`** (one import; IAT slot `0x14208D048`; thunk `jmp [IAT]` at `0x140328926`). **VERIFIED.**
- The Windows loader therefore loads `<gamedir>\dinput8.dll` before the exe entry point runs, before the Denuvo stub and before the CRT.
- IHHook `DInputProxy.cpp`:
  - exports `DirectInput8Create`, `DllCanUnloadNow`, `DllGetClassObject`, `DllRegisterServer`, `DllUnregisterServer`, `GetdfDIJoystick`;
  - loads the real `%SYSTEM32%\dinput8.dll` on the first `DirectInput8Create` call.
- IHHook `dllmain.cpp`:
  - On `DLL_PROCESS_ATTACH` it calls `DisableThreadLibraryCalls`, then builds `IHH` **inside DllMain**: `MH_Initialize`, rebasing addresses, and creating the Lua and other hooks.
  - It then `CreateThread(InitThread)`, which hooks D3D11 Present and ResizeBuffers through a dummy device and swap chain (vtable slots 8 and 13).
  - The Lua hooks must be in place before the engine creates its Lua state, which is why IHHook installs them synchronously in DllMain and not on a thread. Its comment: sig-scanning "takes a literal minute for 100+ signatures", too slow for that. **VERIFIED** (source).
- MGS5VR `src/proxy.cpp` does the same, also from DllMain, and loads the real DLL with `LoadLibraryExW(<system32>\dinput8.dll, LOAD_LIBRARY_SEARCH_SYSTEM32)`.
  - Its comment: "The first game import call happens outside loader lock. Do no graphics/XR work in DllMain." Initialisation goes to a worker thread. **VERIFIED** (source).
- **Proton/Wine:** `dinput8` is a Wine builtin, so a native proxy only loads with `WINEDLLOVERRIDES="dinput8=n,b"`, for example in the Steam launch options as `WINEDLLOVERRIDES="dinput8=n,b" %command%`. **UNVERIFIED** in this setup; this is standard Wine behaviour.
- Alternative proxy: `XINPUT1_3.dll` is also a static import (ordinal 2 only). A native `xinput1_3.dll` would load the same way and would also own `XInputGetState`. That is convenient, but it needs `xinput1_3=n,b`.
- Note: placing a DLL in the game folder modifies the install. For PhantomRing, prefer a launcher-side injection or a Proton DLL override that points elsewhere. This is a design decision, not RE.

### 2.2 Recommended init order for the guest DLL (synthesis)
1. In DllMain: check the exe (TimeDateStamp and SizeOfImage, or sha256). Then `MH_Initialize` and create and enable hooks on:
   - the Lua entry point (`luaL_openlibs` or `lua_pcall`);
   - optionally `CoreFramework::EnterFrame`.
   Patch the `XInputGetState` IAT slot. Do nothing heavy and no D3D work.
2. On a worker thread: set up the shared memory and IPC with the ER host, and optionally hook D3D11 Present. The game's own device is at `0x142C6B870` (tpp-mod `s_deviceD3D`, **UNVERIFIED** at runtime).
3. On the game's Lua thread (inside the hooks): register the Lua module and pump per-frame work (section 3.4).

---------------------------------------------------------------------------------------------------

## 3. Lua inside MGSV

### 3.1 Facts
- Lua **5.1.5**, statically linked, with Fox modifications (VERIFIED):
  - The string `"Lua 5.1"` is at `0x1424B7048`.
  - `lua_newstate` (`0x141a16500`) is a thunk: `xor r8d,r8d; jmp 0x141a16510`. So Fox's internal state constructor takes an extra third parameter.
  - `lua_status` is ICF-folded to `0x14095EF80`.
  - Retail stubbed out `print` and the `Fox.Log` family (IHHook replaces them).
- `lualibs[]` (linit.c) is at `0x1424B4B40` (VERIFIED):

  | Library | `luaopen_*` |
  |---|---|
  | `""` (base) | `0x141a2f1f0` |
  | `package` | `0x141a364f0` |
  | `table` | `0x141a2fe20` |
  | `io` | `0x141a310d0` |
  | `os` | `0x141a31eb0` |
  | `string` | `0x141a33330` |
  | `math` | `0x141a34ae0` |
  | `debug` | `0x141a35a10` |
  | `bit` (LuaBitOp, a Fox addition; IH uses `bit.band`) | `0x141a37450` |

- `luaL_openlibs` = `0x141a16980` (VERIFIED). Its body is the stock loop: `lua_pushcclosure(L,f,0); lua_pushstring(L,name); lua_call(L,1,0)` over `lualibs`.
- Typical stack offsets inside `lua_State` (x64, Lua 5.1): `top` at +0x10, `base` at +0x18, `l_G` at +0x20, `status` byte at +0x0A. This is consistent with `lua_gettop` = `(L->top - L->base)>>4` at `0x141a112e0`, and the `Udata` header is 0x28 bytes (`lua_touserdata` returns `+0x28`). VERIFIED.
- Fox wraps Lua in a **C++ facade object**. Its vtable is at `0x142305700`, with the Lua API slots starting at `0x142305720`. Each slot is a thunk that drops `this` and tail-jumps to the real API, for example `lua_getglobal` = `getfield(L,-10002,k)`, `lua_setglobal`, `lua_pop`, `lua_newtable` and `lua_is*`. The dump is `data/mgsv/fox_lua_facade_vtable_1.0.15.4.txt`. VERIFIED.
- Fox **module binding API** (used by the engine and by tpp-mod; addresses VERIFIED as function starts):
  - `void* foxlua::NewModule(lua_State*, const char* name, i64, i64, i64)` = `0x141A158E0` (tpp-mod `lua_bind_start(L,"TppMod",0,0,1)`)
  - `void* foxlua::AddCFunc(void* bind, const char* name, lua_CFunction, i64 nup?, i64, i64)` = `0x141A15690` (tpp-mod `lua_bind_closure(bind,"Name",fn,1,0,0)`)
  - `void foxlua::AddEnum(bind, const char*, int)` = `0x141A156E0` (VERIFIED from the `TppScriptVars` enum registration)
  - `void foxlua::EndModule(void* bind, i64,i64,i64)` = `0x141A15760` (tpp-mod `lua_bind_end(bind,0,0,0)`)
  - `RegisterVar(module, const char* name, void* addr, lua_CFunction accessor)` = `0x141A15A90` (VERIFIED from the `vars` and `PlayerVars` registration)
  - `0x141A159C0` = NewSubModule (inferred from use)
  - The engine's Lua function-registration pass is `0x14017A0A0` (tpp-mod hooks it to add the `TppMod` module afterwards).
  - Fox Lua instance: `fox::Lua* s_instances` = `0x142B641F0`, where `LuaBase{vtbl; lua_State* state; char initialized;}`. Lua lock: `fox::LuaAutoPtr::LuaAutoPtr(LuaAutoPtr*, char, char, const char*)` = `0x14006A590`, destructor `0x14006A960`. `LuaScopedGlobalPolicy` constructor `0x14006A850`, destructor `0x14006AA40`. Source: tpp-mod; VERIFIED as function starts.

### 3.2 Lua C API, mgsvtpp.exe 1.0.15.4 (all VAs; the image base is fixed)

Verification legend:
- F: facade-vtable thunk tail-jumps here;
- S: semantic disassembly check against the Lua 5.1.5 source;
- P: unique IHHook 1.0.15.3 pattern match;
- T: tpp-mod 1.0.15.4 symbol agrees;
- M: MGS5VR byte-signature agrees;
- O: alphabetical link-order position (lapi.c and lauxlib.c are linked in name order).
Every row has at least two of these.

| Function | VA | Proof |
|---|---|---|
| lua_atpanic | 0x141A10CA0 | F P O |
| lua_call | 0x141A10CC0 | F P S T (called by `luaL_openlibs`) |
| lua_checkstack | 0x141A10D10 | F O S (`cmp edx,0x1F40`) |
| lua_concat | 0x141A10DB0 | F O |
| lua_cpcall | 0x141A10E40 | F O |
| lua_createtable | 0x141A10E80 | F O |
| lua_dump | 0x141A10EF0 | F O |
| lua_equal | 0x141A10F40 | F O |
| lua_error | 0x141A10FA0 | F O (IHHook pattern fails only on `31 C0` vs `33 C0`) |
| lua_gc | 0x141A10FB0 | F P |
| lua_getfenv | 0x141A11160 | F O |
| lua_getfield | 0x141A111E0 | F S O (tpp-mod's "lua_getfield" `0x140BF2A80` is really the facade `getfield(L, REGISTRYINDEX, k)` thunk; do not use it as `lua_getfield`) |
| lua_getmetatable | 0x141A11250 | F O |
| lua_gettable | 0x141A112B0 | F O |
| lua_gettop | 0x141A112E0 | F S T M |
| lua_getupvalue | 0x141A112F0 | P O |
| lua_insert | 0x141A11390 | F O |
| lua_iscfunction | 0x141A113E0 | F P |
| lua_isnumber | 0x141A11410 | F O |
| lua_isstring | 0x141A11440 | F O |
| lua_isuserdata | 0x141A11480 | F O |
| lua_lessthan | 0x141A114B0 | F O |
| lua_load | 0x141A11500 | F P |
| lua_newthread | 0x141A11550 | F O |
| lua_newuserdata | 0x141A11590 | F O |
| lua_next | 0x141A11600 | F T |
| lua_objlen | 0x141A11640 | F O |
| lua_pcall | 0x141A116C0 | F T M |
| lua_pushboolean | 0x141A11750 | F T S |
| lua_pushcclosure | 0x141A11770 | F P S |
| lua_pushfstring | 0x141A11830 | P O |
| lua_pushinteger | 0x141A11880 | F P T |
| lua_pushlightuserdata | 0x141A118A0 | F P |
| lua_pushlstring | 0x141A118C0 | F O |
| lua_pushnil | 0x141A11930 | F P T |
| lua_pushnumber | 0x141A11950 | F P T S (used by the vars float getter) |
| lua_pushstring | 0x141A11970 | F P T S |
| lua_pushthread | 0x141A119A0 | F P |
| lua_pushvalue | 0x141A119D0 | F O |
| lua_pushvfstring | 0x141A11A00 | F O |
| lua_rawequal | 0x141A11A50 | F O |
| lua_rawget | 0x141A11AA0 | F O |
| lua_rawgeti | 0x141A11AE0 | F O |
| lua_rawset | 0x141A11B20 | F O |
| lua_rawseti | 0x141A11BA0 | F O |
| lua_remove | 0x141A11C20 | F T |
| lua_replace | 0x141A11C70 | F O |
| lua_resume | 0x141A13320 | F (ldo.c) |
| lua_setallocf | 0x141A11D40 | F |
| lua_setfenv | 0x141A11D60 | F |
| lua_setfield | 0x141A11E00 | F S O |
| lua_setlevel | 0x141A11E70 | O S |
| lua_setmetatable | 0x141A11E80 | F O |
| lua_settable | 0x141A11F40 | F O |
| lua_settop | 0x141A11F70 | F P T M |
| lua_setupvalue | 0x141A11FD0 | P O |
| lua_status | 0x14095EF80 | F (ICF-folded) |
| lua_toboolean | 0x141A120C0 | F P T |
| lua_tocfunction | 0x141A120F0 | F P |
| lua_tointeger | 0x141A12120 | F T |
| lua_tolstring | 0x141A12150 | F T M |
| lua_tonumber | 0x141A121F0 | F T S |
| lua_topointer | 0x141A12230 | F O |
| lua_tothread | 0x141A122A0 | F P |
| lua_touserdata | 0x141A122C0 | F P S |
| lua_type | 0x141A12300 | F T |
| lua_typename | 0x141A12330 | F T |
| lua_xmove | 0x141A12350 | F O |
| lua_yield | 0x141A13460 | F |
| lua_newstate (public 2-arg) | 0x141A16500 | F S (called by `luaL_newstate`); internal 3-arg version at 0x141A16510 |
| lua_close | 0x141A16480 | P |
| luaL_openlibs | 0x141A16980 | S (xref to `lualibs`) |
| luaL_addlstring | 0x141A16BF0 | F P |
| luaL_addstring | 0x141A16C90 | F |
| luaL_addvalue | 0x141A16CB0 | F |
| luaL_argerror | 0x141A16D70 | F |
| luaL_buffinit | 0x141A16E60 | F S |
| luaL_callmeta | 0x141A16E80 | F |
| luaL_checkany | 0x141A16F00 | F |
| luaL_checkinteger | 0x141A16F40 | F |
| luaL_checklstring | 0x141A16F90 | F |
| luaL_checknumber | 0x141A17020 | F |
| luaL_checkoption | 0x141A17080 | F |
| luaL_checkstack | 0x141A17140 | F |
| luaL_checktype | 0x141A17180 | F |
| luaL_checkudata | 0x141A171C0 | F |
| luaL_error | 0x141A17280 | P |
| luaL_findtable | 0x141A172E0 | O |
| luaL_getmetafield | 0x141A17410 | F |
| luaL_gsub | 0x141A17490 | F P |
| luaL_loadbuffer | 0x141A178E0 | F P T M |
| luaL_loadfile | 0x141A17910 | F P |
| luaL_loadstring | 0x141A17B30 | F T |
| luaL_newmetatable | 0x141A17B70 | F |
| luaL_newstate | 0x141A17BF0 | F S |
| luaL_openlib (= luaI_openlib under LUA_COMPAT_OPENLIB) | 0x141A17C30 | P S (`luaL_register` jumps to it) |
| luaL_optinteger | 0x141A17DB0 | F |
| luaL_optlstring | 0x141A17E20 | F |
| luaL_optnumber | 0x141A17EA0 | F |
| luaL_prepbuffer | 0x141A17F10 | F |
| luaL_pushresult | 0x141A17F70 | F |
| luaL_ref | 0x141A17FC0 | F S |
| luaL_register | 0x141A18080 | F S (`xor r9d,r9d; jmp luaL_openlib`) |
| luaL_typerror | 0x141A18090 | F S ("%s expected, got %s") |
| luaL_unref | 0x141A180F0 | F |
| luaL_where | 0x141A18170 | F |
| (static) tag_error(L,int narg,int tag) | 0x141A18240 | S. IHHook's "luaL_typerror" (1.0.15.3 `0x141a184c0`) is actually this function, a latent IHHook bug. |

Unique AOBs generated from the 1.0.15.4 bytes (each checked to occur exactly once in the file, with rel32 and RIP displacements wildcarded; tool `mkaob.py`). VERIFIED:
```
lua_pcall            48 89 5C 24 08 57 48 83 EC 40 41 8B F8
luaL_loadbuffer      48 83 EC 38 48 89 54 24 20 4C 89 44
lua_gettop           48 8B 41 10 48 2B 41 18 48 C1 F8 04
lua_settop           85 D2 78 42 4C 63 C2 48 8B 51 18 49
lua_tolstring        48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B D8 8B F2 48 8B F9 E8 ? ? ? ? 4C 8B D0 83 78 08 04 74 53
lua_pushcclosure     48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 4C 8B 49 20 49 63
luaL_openlibs        48 89 5C 24 08 57 48 83 EC 20 48 8B 05 ? ? ? ? 48 8B F9 48 8D
lua_newstate(impl)   48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B FA 49 8B E8
luaL_openlib         48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 20 41 83
foxlua NewModule     41 56 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 48 89 6C 24 48 48 89 74 24 50 48 89 7C 24 58 49 8B E9
foxlua AddCFunc      48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B F1 48 8B 49 08 49
foxlua EndModule     48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B 49 08 E8
RegisterVar          48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B E9 48 8B 49 08 49 8B
LuaAutoPtr ctor      48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 41 0F B6 F0
```
`lua_getfield` and `lua_setfield` share a 60-byte prologue and differ only in the byte after it (`48` vs `49`). Use the VAs.

### 3.3 How IHHook (r17) uses Lua (source-VERIFIED)
- **Hooks** (MinHook, created in DllMain): `luaL_openlibs`, `lua_newstate`, `lua_newthread`, `lua_load`, `luaL_loadbuffer`, `lua_atpanic`, `lua_error`, `lua_pcall`, `lua_cpcall`. `l_StubbedOut` is created but enabled only in debug builds.
- **Catching the state:** `lua_newstateHook` saves `L`. `luaL_openlibsHook` calls the original, then:
  - `lua_pushinteger(L,17); lua_setfield(L,LUA_GLOBALSINDEX,"_IHHook")`;
  - registers C modules (`IHH` via `luaI_openlib(L,"IHH",funcs,0)`, plus character, buddy, vehicle and FoxString libraries, and imgui bindings);
  - replaces `print`.
- **Calling Lua from C++ — the important constraint:** IHHook never calls into Lua from its own threads. All C++→Lua traffic is "inside-out":
  - IH Lua calls `IHH.Init()` from `init.lua` → `InfInit.lua`;
  - `IHH.InitMain(ver)` from `InfInitMain.lua`;
  - `IHH.OnUpdate()` from the top of `TppMain.OnUpdate`;
  - it polls `IHH.GetMenuMessages()` and `IHH.GetPipeInMessages()`, which drain lock-protected queues filled by the imgui (Present thread) and the named-pipe threads `\\.\pipe\mgsv_in` and `\\.\pipe\mgsv_out`.
  - Its own comment: "since the menu is run through the normal game lua update loop can't really just have ui call lua directly … dumps them into a queue".
  - Lua updates stop while the game's pause menu is open ("The game stops lua updates … in the pause menu").
- `ReplaceStubedOutFox` restores `Fox.Log`, `Fox.Caution`, `Fox.Warning` and `Fox.Error`. It is called from `IHH.Init` because the Fox modules do not exist yet at `luaL_openlibs` time.

### 3.4 Three working per-frame Lua injection strategies for 1.0.15.4
1. **IHHook style.** Hook `luaL_openlibs` (`0x141A16980`) and register your C module there. You still need a Lua script that calls you every frame. IH gets this by patching `init.lua` and `TppMain.lua` inside `data1.dat` through SnakeBite, which modifies game archives (not allowed here).
   Archive-free option: from a C hook running on the Lua thread, once `TppMain` exists, do `lua_getglobal(L,"TppMain")`, wrap `TppMain.OnUpdate` in a C closure that calls your tick and then the original, and set it back. This is my synthesis, **UNVERIFIED** at runtime.
2. **MGS5VR style** (`src/native_actions.cpp`). MinHook `lua_pcall` at `0x141A116C0`.
   - In the detour, track `depth`.
   - When `depth==1` (the outermost engine pcall, which runs on the game's "native transaction"/Lua thread), call `pump(L)`.
   - `pump` uses `luaL_loadbuffer(0x141A178E0)` plus the *original* `lua_pcall`, `lua_gettop(0x141A112E0)`, `lua_settop(0x141A11F70)` and `lua_tolstring(0x141A12150)`.
   - Readiness check script: `return type(TppMain)=='table' and type(GameObject)=='table' and type(gvars)=='table' and type(vars)=='table' and type(TppMission)=='table' …`.
   - Proven on 1.0.15.4 by that project. **VERIFIED** (source and signature bytes match locally).
3. **tpp-mod style.** MinHook `fox::CoreFramework::EnterFrame` at `0x14007F840` (main thread, once per frame; LeaveFrame `0x14007FDB0`).
   - Construct `fox::LuaAutoPtr` (`0x14006A590`, args `(&ptr,1,1,nullptr)`) to lock the Lua instance.
   - `L = ptr.lua->state`, run `luaL_loadbuffer` and `lua_pcall`, then call the destructor `0x14006A960`.
   - Scripts load after `TppGameCore` init (`0x140991BF0`). A custom module is registered after the engine's registration pass (`0x14017A0A0`).
   - **VERIFIED** (addresses are function starts and the source was read).

---------------------------------------------------------------------------------------------------

## 4. IHHook details (source-VERIFIED; `TinManTex/IHHook@71801f1`)

### 4.1 Files
- `IHHook/dllmain.cpp`, `DInputProxy.cpp`: entry point and proxy.
- `IHHook.cpp/.h`: version check, config `ihhook_config.lua`, `RebaseAddresses()`, `CreateAllHooks()`.
- `hooks/mgsvtpp_adresses_1_0_15_3_en.h` and `_jp.h`: `std::map<std::string,int64_t>` with absolute VAs. They include, besides Lua:
  - `StrCode64 0x14c1bd730`, `PathCode64 0x14c1bd5d0`, `FNVHash32 0x143f33a20`;
  - `GetFreeRoamLangId 0x145e60f40`, `UpdateFOVLerp 0x141116800`;
  - `LoadFile 0x14319ea20`;
  - player-parts and camo loaders `0x1468…`, buddy and vehicle loaders, `CreateInPlace 0x142e8a5d0`.
  The `texdev` branch adds `foxlua::NewModule 0x14c1f9150`, `AddCFuncToModule 0x14c1f7400`, `RegisterVar 0x14c1f9e20`, `DeclareEntityClass 0x14317f430` and others.
- `hooks/mgsvtpp_patterns.h`: IDA/Ghidra-style patterns for most of the above.
- `mgsvtpp_signatures_old.h`: `UnkSomePlayerUpdateFunc`, `UnkAnotherPlayerUpdateFuncButHuge` (the 1.0.15.3 `0x1412cf110`), `GetFreeRoamLangId`, `UpdateFOVLerp`, `_mainCRTStartup`.
- `ghidra/ExportHooksToHeader.py` and `ExportInfo.py`: generate the headers from a shared Ghidra project (Modders Heaven Discord).

### 4.2 Hooks besides Lua
| Hook | What it does | Notes |
|---|---|---|
| `D3D11Hook` (Present, ResizeBuffers via a dummy NULL-driver device and swap chain, vtable slots 8 and 13) | imgui overlay | from the RE2 framework |
| `WindowsMessageHook` (`SetWindowLongPtr(GWLP_WNDPROC)` on the swap chain's `OutputWindow`) | runs `RawInput::OnMessage`; returning false eats the message (`DefWindowProc`) | **this is how IHHook blocks keyboard and mouse input to the game** |
| `RawInput.cpp` | on `WM_INPUT` calls `GetRawInputData`; drops keys whose `blockGameKeys[vk]` is set; mouse button blocking | comment: "MGSV seems to use raw input for mouse and keyboard" (confirmed in section 8) |
| `SetCursorPos` (MinHook on user32) | returns FALSE while the IHHook cursor is unlocked | |
| `UpdateFOVLerp(thisptr)` | rewrites the target focal length at `thisptr+0x2FC` (MGO `+0x2EC`): 21 normal, 22 shoulder, 26 hiding, 32 CQC | from mon's MGSV-TPP-FoV |
| `GetFreeRoamLangId(langId*, short loc, short mission)` | custom free-roam names | |
| `UnkPrintFuncStubbedOut(fmt,…)`, `nullsub_2` | restore stubbed log output | |
| `LoadFile*`, `LoadPlayerParts*`, `LoadBuddy*`, `PreparePlayerVehicle*`, `CreateInPlace` (FoxString) | file and asset redirection for IH features | |
| `CityHash`, `FNVHash32` | hash logging | |

IHHook has **no** native freecam, player-position or player-camera hooks. Those are pure IH Lua (section 5). It hooks no DirectInput or XInput functions; the dinput8 proxy only forwards.

### 4.3 Thread and timing constraints (from IHHook comments)
- Hooks must exist before the engine creates its Lua state, so they are created in DllMain.
- The D3D hook thread "shifts execution till after DllMain".
- imgui runs on the Present thread. Game Lua is not touched from there; queues are used.
- `OnMessage` runs on the window thread (see 8.3).

### 4.4 IHHook patterns scanned against 1.0.15.4 (VERIFIED; `data/mgsv/ihhook_patterns_scan_1.0.15.4.txt`)
- Unique hits: `StrCode64 0x141A09AC0`, `GetFreeRoamLangId 0x1409121C0`, `UpdateFOVLerp 0x141115ED0`, plus about 30 Lua functions (those marked P in 3.2).
- `luaopen_table`, `luaopen_os` and `luaopen_debug` share one pattern (3 hits; disambiguated by `lualibs`).
- `luaopen_package` matches at `+1` (`0x141A364F1`; the real start is `0x141A364F0`).
- **Why the others fail:** they were cut from 1.0.15.3 functions that Denuvo had moved and re-assembled, which uses `89 /r` forms such as `48 89 CB` instead of MSVC's `48 8B D9` and `31 C0` instead of `33 C0`. Converting those encodings back makes old patterns match. I confirmed this on four IGCS patterns (7.6) and on a 2015 Cheat Engine `PlyPos` pattern (7.7).

---------------------------------------------------------------------------------------------------

## 5. Infinite Heaven Lua API reference (source-VERIFIED; `TinManTex/InfiniteHeaven@b0bfab4`, paths under `tpp/`)

Name checks: the engine-side names below were searched in the 1.0.15.4 exe; see section 6. Script-side helpers (`TppPlayer.*`, `TppMission.*`) live in the game's Lua (`data1.dat`), not in the exe.

### 5.1 Position and rotation
- Read: `vars.playerPosX`, `vars.playerPosY`, `vars.playerPosZ` (float), `vars.playerRotY` (degrees, ScriptVar TYPE_INT16), `vars.playerVehicleGameObjectId` (`GameObject.NULL_ID` when on foot).
  Example: `InfWarp.lua` builds `Vector3(vars.playerPosX, vars.playerPosY, vars.playerPosZ)` and `Quat.RotationY(TppMath.DegreeToRadian(vars.playerRotY))`.
- Warp (`data1_dat-lua-ih/Assets/tpp/script/lib/TppPlayer.lua:53` `this.Warp(info)`):
  ```lua
  local rotY=foxmath.NormalizeRadian(foxmath.DegreeToRadian(info.rotY or 0))
  GameObject.SendCommand({type="TppPlayer2",index=0},{id="WarpAndWaitBlock",pos={x,y,z},rotY=rotY})
  ```
  `pos` must be a 3-element table. `InfTppUtil.Warp(gameObjectId,pos,rotY)` (`modules/InfTppUtil.lua:103`) generalises this:
  - vehicle: `{id="SetPosition",position=Vector3,rotY=rad}`;
  - walker gear: `{id="SetPosition",pos=…,rotY=…}`;
  - hostage: `{id="Warp",degRotationY=deg,position=Vector3}`.
  Delivery warp uses `{id="WarpToStation",stationId=…}`.
  `Player.RequestToWarp` **does not exist** (no string in the exe). Use the GameObject command. The string `WarpAndWaitBlock` is not present in plain text either; command ids are StrCode32-hashed on the engine side (VERIFIED absence).
- Mission start position: `TppPlayer.SetInitialPosition(pos,rotY)`, `TppPlayer.SetMissionStartPosition`, `TppPlayer.ResetInitialPosition`. vars: `initialPlayerPosX/Y/Z`, `initialPlayerRotY`, `initialPlayerFlag` (`PlayerFlag.USE_VARS_FOR_INITIAL_POS`).

### 5.2 Stance and status
- `PlayerInfo.OrCheckStatus{PlayerStatus.STAND,PlayerStatus.SQUAT,PlayerStatus.CRAWL}`, `PlayerInfo.AndCheckStatus{…}`. Other constants: `PlayerStatus.PARTS_ACTIVE`, `CARRY`, `DASH`, `NORMAL_ACTION`.
- `Player.RequestToSetTargetStance(PlayerStance.STAND|SQUAT|CRAWL)` (`InfMain.lua`, `TppPlayer.lua`).
- Native status bitfield: see 7.4 (MGS5VR reads `u32[5]` flags; bit 26 is vehicle).

### 5.3 Life and stamina
- `vars.playerLife`, `vars.playerLifeMax` (u16; IH: "vars.playerLifeMax is uint16 … capping at 50k"), `vars.playerStamina`.
- `Player.ChangeLifeMaxValue(n)` and `Player.ResetLifeMaxValue()` (`InfMainTpp.lua:312`).
- `Player.IsDead()`, `Player.OnPlayerDeath`, `Player.RequestToRespawn`, `Player.StartFOBInvincible` exist in the exe table (7.2).
- Message `ChangeLife` exists (`InfLookup.lua`).

### 5.4 Camera
- `vars.playerCameraRotation[0]` is pitch (rotX) and `[1]` is yaw (rotY), in degrees. `vars.playerCameraPosition[0..2]`.
- `Player.RequestToSetCameraRotation{rotX=-10,rotY=200}` (example in `mod/dev/f30050_sequence_dev.lua:2519`).
- Freecam (`modules/InfCamera.lua`):
  ```lua
  Player.SetAroundCameraManualMode(true)
  Player.SetAroundCameraManualModeParams{ target=Vector3(...), distance=, focalLength=, focusDistance=, aperture=,
     targetInterpTime=, ignoreCollisionGameObjectName="Player", rotationLimitMinX=, rotationLimitMaxX=, alphaDistance= }
  Player.UpdateAroundCameraManualModeParams()
  -- input while freecam: Player.SetPadMask{settingName="allButCam",except=true,sticks=PlayerPad.STICK_R}
  -- read sticks: PlayerVars.leftStickXDirect / leftStickYDirect
  ```
  Also used: `Player.RequestToPlayCameraNonAnimation{…}`, `Player.RequestToStopCameraAnimation{}`, `Player.RequestToSetCameraNoise`, `Player.RequestToSetCameraFocalLengthAndDistance`, `Player.StartTargetConstrainCamera`, `Player.SetAroundCameraManualModeShakeParams`.
- FOV: native (IHHook `UpdateFOVLerp`, tpp-mod `PlayerCameraImpl+0x2DC` fov, section 7.5).

### 5.5 Pad masks, disable flags, pause
- `Player.SetPadMask{settingName="name", except=bool, buttons=PlayerPad.X+…, sticks=PlayerPad.STICK_L+PlayerPad.STICK_R, triggers=PlayerPad.TRIGGER_L+…}` and `Player.ResetPadMask{settingName="name"}`.
  - `settingName="AllClear"` resets all masks (`InfMain.lua:1260`).
  - `except=true` means mask everything except the listed inputs.
- `vars.playerDisableActionFlag = PlayerDisableAction.X + …` (u32). Values extracted from the exe registration (VERIFIED for the large values; the small ones are inferred as consecutive bits):
  - NONE=0, then probably CQC_KNIFE_KILL=1, CQC_INTERROGATE=2, RUN=4, FULTON=8, RIDE_HELI=16, TIME_CIGARETTE=32, OPEN_EQUIP_MENU=64, CHANGE_STANCE_FROM_CRAWL=128 (inferred);
  - STAND=256, SUBJECTIVE_CAMERA=512, CQC=1024, OPEN_CALL_MENU=2048, RIDE_VEHICLE=4096, BEHIND=8192, MARKING=16384, LOOK_CAMERA_DIR=32768, CARRY=65536, STEALTHASSIST=1<<17, REFLEXMODE=1<<18, KILLING_WEAPON=1<<19, BINOCLE=1<<20, HELISPACE_CASSETTE=1<<21, BLOOD_EFFECT=1<<22, INFECTED_GOGGLES=1<<23, QUICK_NVG=1<<24.
- `Player.SetPause()` and `Player.UnsetPause()` (`InfMain.lua`); `TppMain.EnablePause` / `DisablePlayerPad` in scripts.
- Pad state seen by Lua: `PlayerVars.scannedButtonsDirect` (u32 bitmask), `scannedButtons`, `leftStickX/Y`, `rightStickX/Y`, and their `…Direct` variants (floats). IH `InfButton.lua` bit indices:
  - DECIDE 0, STANCE 1, DASH 2, HOLD 3, FIRE 4, ACTION/RIDE/JUMP 5, RELOAD/CARRY 6, STOCK/ZOOM_CHANGE 7;
  - MB_DEVICE 8, CALL/INTERROGATE 9, SUBJECT 10, UP/PRIMARY_WEAPON 11, DOWN/SECONDARY 12, LEFT 13, RIGHT 14, CQC 15;
  - SIDE_ROLL 16, LIGHT_SWITCH 17, EVADE 18, VEHICLE_FIRE 19, VEHICLE_CALL 20, VEHICLE_DASH 21, PLACE_MARKER 22, (keyboard) ESCAPE 23, WALK 25.
  The exe's `PlayerPad` enum agrees (MB_DEVICE=256 … PLACE_MARKER=4194304). VERIFIED.

### 5.6 Disabling enemies and NPCs
- Soldier ids: `GameObject.GetGameObjectId("TppSoldier2", name)`, `GameObject.GetGameObjectIdByIndex("TppSoldier2", i)`.
- `GameObject.SendCommand(id,{id="SetEnabled",enabled=false[,noAssignRoute=true]})` (`InfSoldier.lua:1087`).
- `{id="SetDisableDamage",life=true,faint=true,sleep=true}`, `{id="SetIgnoreDamageAction",flag=…}`, `{id="SetFriendly",enabled=false}`, `{id="SetSneakRoute",route=…}`, `{id="SetInvincible",enabled=true}` (heli and other objects).
- Engine type names are present in the exe: `TppSoldier2`, `TppPlayer2` (VERIFIED strings). Phase and alert: `vars.playerPhase` (u8 at +0x2E7B), message `ChangePhase`.

### 5.7 Loading a free-roam mission directly
- Codes (`TppDefine.lua:281` `SYS_MISSION_ID`): INIT=1, TITLE=5, **AFGH_FREE=30010**, **MAFR_FREE=30020**, **MTBS_FREE=30050**, MTBS_ZOO=30150, MTBS_WARD=30250, AFGH_HELI=40010, MAFR_HELI=40020, MTBS_HELI=40050, HELI_SPACE=40060, FOB=50050, SELECT=60000.
- Locations: INIT=1, AFGH=10, MAFR=20, CYPR=30, GNTN=40, OMBS=45, MTBS=50, MBQF=55, HLSP=60, FLYK=70.
- From in-game, the IH way (`InfMission.LoadMissionManual`):
  ```lua
  TppMission.ReserveMissionClear{missionClearType=TppDefine.MISSION_CLEAR_TYPE.FROM_HELISPACE, nextMissionId=30010}
  ```
  MISSION_CLEAR_TYPE: ON_FOOT=1, RIDE_ON_HELICOPTER=2, RIDE_ON_VEHILCE=3, FOB_GOAL=4, FOB_ESCAPE=5, FREE_PLAY_ORDER_BOX_DEMO=6, FROM_HELISPACE=7, …, FREE_PLAY_NO_ORDER_BOX=13.
- Direct option (IH `loadDirect`): `gvars.mis_nextMissionCodeForMissionClear=code; mvars.mis_showLoadingTipsOnMissionFinalize=false; TppMission.ExecuteMissionFinalize()`.
- Low level: `TppMission.Load(nextCode, currentCode, {showLoadingTips=false, force=true})` and `TppMission.RequestLoad(...)` (`TppMission.lua:3615/3665`); engine `Mission.LoadMission` (string present at `0x1422AA410`).
- MGS5VR reached `30010` / `Seq_Game_FreePlay` through Continue → Resume in 18.4 s with no focus change (**UNVERIFIED**, from its docs).

### 5.8 Weapons and ammo (vars, all ScriptVars; offsets in 7.1)
- `vars.weapons[0..5]`, `ammoInWeapons[0..5]`, `ammoSubInWeapons[0..5]`, `isInitialWeapon[0..5]`, `supportWeapons[0..15]`, `items[0..15]`, `handEquip`, `ammoStockIds[0..45]`, `ammoStockCounts[0..45]`, `currentInventorySlot`, `currentItemIndex`, `currentSupportWeaponIndex`, `initWeapons[…]`.
- `Player.ChangeEquip{…}`, `Player.SetItemLevel`, `Player.GetAmmoStockBySlot`, `Player.SetInfiniteAmmoFromScript`, `TppPlayer.SetInitWeapons`.

### 5.9 Time and weather
- Clock: `vars.clock` (int32 seconds); `TppClock.SetTime("hh:mm:ss")`; `TppClock.GetTime`; `TppCommand.Weather.SetClockTimeScale(n)` (IH default 20; 0 stops the clock); `WeatherManager.IsNight()`.
- Slow motion: `HighSpeedCamera.RequestEvent{continueTime=,worldTimeRate=,localPlayerTimeRate=,…}` and `HighSpeedCamera.RequestToCancel()`.
- Weather: `TppWeather.ForceRequestWeather(type,interpTime,fogParam)`, `TppWeather.CancelForceRequestWeather()`, `WeatherManager.RequestTag(tag,interp)`, `WeatherManager.SetNewWeatherProbabilities`.
- IH notes that `Time.GetRawElapsedTimeSinceStartUp` scales with game time, which is why IH uses `os.clock`.

### 5.10 How IH bootstraps (and why we should not copy it)
- `data1_dat-lua-ih/init.lua` ("first script loaded by engine") does `Script.LoadLibrary"/Assets/tpp/script/ih/InfInit.lua"`. That calls `IHH.Init()` and then `pcall(require,"mod/Assets/tpp/script/ih/InfCore")` (external Lua under `GameDir/mod`).
- IH's modified `TppMain.OnUpdate` drives module `Update(currentChecks,…)` functions.
- These Lua files ship inside the game's `.dat` archives through SnakeBite, which modifies the install. PhantomRing should inject through the C hooks in 3.4 instead.

---------------------------------------------------------------------------------------------------

## 6. Engine Lua API names present in the 1.0.15.4 exe (VERIFIED, exact NUL-terminated ASCII, `.edata`)

| Name | VA |
|---|---|
| RequestToSetCameraRotation | 0x142298858 |
| SetPadMask / ResetPadMask | 0x1422982A8 / 0x1422982B8 |
| playerPosX / Y / Z | 0x14228B4C0 / 0x14228B4D0 / 0x14228B4E0 |
| playerRotY | 0x14228B508 |
| playerCameraRotation | 0x14228B638 |
| playerLife / playerLifeMax | 0x14228B600 / 0x14228B468 |
| scannedButtonsDirect / leftStickXDirect | 0x14229A068 / 0x142299FF8 |
| PlayerVars | 0x142299FA8 |
| SetAroundCameraManualMode / …Params / UpdateAroundCameraManualModeParams | 0x142298938 / 0x142298958 / 0x142298978 |
| ChangeLifeMaxValue | 0x142298C78 |
| RequestToSetTargetStance | 0x1422989C8 |
| SetPause | 0x142298738 |
| PlayerInfo / OrCheckStatus / PlayerStatus / PlayerPad | 0x1421961B0 / 0x1421961A0 / 0x1421961C0 / 0x142299448 |
| SendCommand / GetGameObjectId | 0x1421B5128 / 0x1421CB238 |
| TppPlayer2 / TppSoldier2 | 0x1422951E0 / 0x14229CE78 |
| HighSpeedCamera / SetClockTimeScale / LoadMission | 0x1421AF078 / 0x14218EBD0 / 0x1422AA410 |
| vars / svars / gvars | 0x14228B0BC / 0x1422AB264 / 0x1422AB25C |
| Fox / StrCode32 | 0x1420E83DC / 0x1420E8448 |
| TppScriptVars | 0x14228FBD8 |

Not present as engine strings: `RequestToWarp`, `WarpAndWaitBlock` (command ids are hashed), `ReserveMissionClear` and `mvars` (Lua-side only).

---------------------------------------------------------------------------------------------------

## 7. Native memory and functions (1.0.15.4)

### 7.1 `vars` ScriptVar block (VERIFIED by disassembling the registration function `0x140996B70..0x1409AA55B`)
- Registration pattern for each var:
  - `r8 = *(u64*)0x142C16920 + off`;
  - `RegisterVar(module, "name", r8, accessor)`;
  - a parallel descriptor table at `*(u64*)0x142A65708` with count `*(u32*)0x142A65710`. Entries are 24 bytes: `{void* addr; const char* name; u32 hash; u32 flags}`.
- `flags = (category<<24) | (type<<16) | arrayCount`.
  - Types (VERIFIED from the `TppScriptVars` enum registration at `0x1409A9B60`): BOOL 0, INT32 1, UINT32 2, FLOAT 3, INT8 4, UINT8 5, INT16 6, UINT16 7.
  - Categories: NONE 0, CONFIG 1, MISSION_RESTARTABLE 2, GAME_GLOBAL 4, MISSION 8, RETRY 16, MB_MANAGEMENT 32, QUEST 64, PERSONAL 128, ALL 255.
- **Base pointer: `VARS = *(uint8_t**)0x142C16920`.** The address is in `.data2` BSS and is filled at runtime. The Lua accessor for floats is `0x1409AABF0` (lightuserdata upvalue → read or write float). The full dump is in `data/mgsv/vars_registrations_1.0.15.4.txt`.

| var | offset | type[count] | category |
|---|---|---|---|
| locationCode | +0x10 | u16 | MISSION |
| missionCode | +0x12 | u16 | MISSION |
| clock | +0x5C | i32 (seconds) | MISSION |
| clockTimeScale | +0x60 | (special accessor) | |
| weather | +0x68 | u32 | MISSION |
| initialPlayerPosX/Y/Z | +0xE0/+0xE4/+0xE8 | float | MISSION |
| **playerPosX/Y/Z** | **+0xEC/+0xF0/+0xF4** | float | MISSION |
| playerPartsType / CamoType / HandType / Type | +0xF8/+0xF9/+0xFA/+0xFB | u8 | M_RESTARTABLE |
| playerFaceId / playerFaceEquipId | +0xFC / +0xFE | u16 | M_RESTARTABLE |
| playerLifeMax | +0x104 | u16 | MISSION |
| initialPlayerRotY / **playerRotY** | +0x106 / **+0x108** | i16 | MISSION |
| playerVehicleGameObjectId | +0x110 | u16 | MISSION |
| playerDisableActionFlag | +0x118 | u32 | MISSION |
| **playerLife** / playerStamina | **+0x11C** / +0x11E | u16 | MISSION |
| playerCameraPosition | +0x120 | float[3] | MISSION |
| **playerCameraRotation** | **+0x12C** | float[2] | M_RESTARTABLE |
| playerPlayFlag | +0x13C | u32 | MISSION |
| initWeapons | +0x520 | u16[3] | MISSION |
| weapons / ammoInWeapons / ammoSubInWeapons | +0x548 / +0x554 / +0x560 | u16[6] each | MISSION |
| isInitialWeapon | +0x57E | u8[6] | MISSION |
| supportWeapons / items | +0x584 / +0x5A4 | u16[16] | MISSION |
| handEquip | +0x5D4 | u16 | MISSION |
| ammoStockIds / ammoStockCounts | +0x5E6 / +0x614 | u8[46] / u16[46] | MISSION |
| currentInventorySlot / currentSupportWeaponIndex / currentItemIndex | +0x674 / +0x678 / +0x67C | u32 | M_RESTARTABLE |
| buddyType | +0x2D68 | u8 | MISSION |
| playerPhase | +0x2E7B | u8 | MISSION |

Liveness: IH reads `vars.playerPos*` and `vars.playerRotY` every Lua update for warp and noclip, so they track the player while in-game. The exact update point and rate are **UNVERIFIED**. Treat writes as "set the saved var", not "teleport"; teleport with `WarpAndWaitBlock`.

### 7.2 `Player.*` and `PlayerVars` C functions (VERIFIED; full list in `data/mgsv/player_module_cfuncs_1.0.15.4.txt`)
- `Player` module luaL_Reg array, built in `0x1409D6AE0`. Selected entries (C function VA):

  | Function | VA |
  |---|---|
  | SetPadMask | 0x1409DF360 |
  | ResetPadMask | 0x1409DC510 |
  | RequestToSetCameraRotation | 0x1409DAF90 |
  | SetAroundCameraManualMode | 0x1409DCD80 |
  | SetAroundCameraManualModeParams | 0x1409DCE00 |
  | UpdateAroundCameraManualModeParams | 0x1409E1800 |
  | RequestToSetTargetStance | 0x1409DB170 |
  | RequestToMoveToPosition | 0x1409D9430 |
  | RequestToPlayDirectMotion | 0x1409DA7D0 |
  | SetPause | 0x1409DF730 |
  | UnsetPause | 0x1409E17D0 |
  | ChangeLifeMaxValue | 0x1409D1040 |
  | ResetLifeMaxValue | 0x1409DC4C0 |
  | IsDead | 0x1409D6020 |
  | ChangeEquip | 0x1409D0D60 |
  | SetInfiniteAmmoFromScript | 0x1409DEC00 |
  | GetAmmoStockBySlot | 0x1409D1BB0 |
  | RegisterPartsPackagePath | 0x1409D8570 |
  | SetPartsInfoAtInstanceIndex | 0x1409DF530 |

  The C functions are useful as hook points (for example to see when scripts set a pad mask). They take `lua_State*`.
- `PlayerVars` statics (`RegisterVar` with `lea r8,[abs]`; BSS):

  | Field | VA | Type |
  |---|---|---|
  | leftStickX | 0x142C1C07C | float |
  | leftStickY | 0x142C1C080 | float |
  | rightStickX | 0x142C1C084 | float |
  | rightStickY | 0x142C1C088 | float |
  | leftStickXDirect | 0x142C1C08C | float |
  | leftStickYDirect | 0x142C1C090 | float |
  | rightStickXDirect | 0x142C1C094 | float |
  | rightStickYDirect | 0x142C1C098 | float |
  | scannedButtons | 0x142C1C09C | u32 (int accessor 0x1409AAB80) |
  | scannedButtonsDirect | 0x142C1C0A0 | u32 |
  | cameraZoomRate | 0x142C1C0A4 | |
  | distanceForSeCallOfPassingObjects | 0x142C1C0A8 | |
  | …OnHorse | 0x142C1C0AC | |

  These are what the game *computed* from input. They are useful for reading MGSV's interpreted input, not for injecting it.
- Player enums registered in `0x1409D3630`: PadType, PlayerCamera (Around, Tps, Subjective, Vehicle), PlayerFlag, PlayerInitialAction, PlayerPlayFlag, PlayerPad, PlayerStance, PlayerMoveType, PlayerTrap, ActionIcon, PlayerDisableAction, PlayerType (SNAKE=0, DD_MALE, DD_FEMALE, LIQUID, AVATAR, OCELOT, QUIET), PlayerCamoType…
- Player system (tpp-mod): `Player2System` global at `0x142A82C28` → `->player2System` (Impl): `+0xB0` `PadImpl* pad` (`pad+0x28` u32 `mask`; `pad+0x2C0` `ButtonMask[8]{id,mask}`), `+0xC8` `EquipControllerImpl*`, `+0x214` `u32 localPlayerIndex` (offsets computed from tpp-mod `structs.hpp`, which has `static_assert`s for `controller==200` and `buttonMasks==704`; **UNVERIFIED** at runtime). `Player2UtilityImpl::IsLoading` = `0x1409D6590`.

### 7.3 Per-frame player camera owner, root and head bone (MGS5VR; signatures VERIFIED locally)
- Camera setter (IGCS signature) `0x14044E960`: `movaps xmm0,[rdx]; movaps [rcx+0xF0],xmm0; movaps xmm1,[rdx+0x10]; movaps [rcx+0x100],xmm1; ret`.
  - Camera object: **+0xF0 quaternion xyzw**, **+0x100 position xyz(w)**.
- The player-camera caller's return address is `0x141118B1A`. At that call site `0x141118B06` the bytes are `48 8B 8E 80 03 00 00 48 8D 95 10 03 00 00 48 8B 01 FF 50 08` (VERIFIED). **RSI = the player camera owner** there:
  - `owner+0x00` vtable == `0x1423B8218` (VERIFIED as a vtable into .xtext);
  - `owner+0x30` = **player root 4×4 affine** (float[16], translation at [12..14]); `owner+0x60` = root position;
  - `owner+0x70` = **animated head bone (index 4)** affine (model-local), "populated by the native animation reader, independently of ADS/boom state";
  - `owner+0x370` = character (vtable `0x142295210`), `owner+0x380` = camera object, `owner+0x3A0` = u32 player index.
- MGS5VR hooks the setter with an asm stub to capture RSI and RBP (`MgsCameraIntercept`). Our DLL can do the same to get the player transform every frame on the game thread.

### 7.4 Native player status state (MGS5VR `controller_rig.cpp:1855+`; layout UNVERIFIED by me at runtime)
```
phase  = *(u32*)0x142BD3FD0   (0 or 1, double-buffered)
index  = *(u32*)0x142BD3FD4   (local player index, <16)
state  = 0x142BD3FF0 + (phase*16 + index)*0xE0
  *(u64*)state == 0x142190B28 (vtable check)
  state+0x40  Vec3 root position
  state+0xC0  u32 flags[5]  (PlayerStatus bits; bit 26 = vehicle, bit 37 = "unknown/transition")
```

### 7.5 Skeleton and animation: skin publication (MGS5VR; signatures VERIFIED locally)
- `void SkinPublish(void* driver, void* binding)` at **`0x141A6CAA0`** (bytes `48 89 5C 24 18 57 48 81 EC C0 00 00 00`). It is called for every skinned model; MGS5VR filters for the player.
- Chain from the camera owner: `character = owner+0x370`, `component = *(character+0x10)`, `holder = *(component+0x10)`, `model = *(holder+0x68)` (vtable `0x1420F4D90`), `driver = *(holder+0x70)` (vtable `0x1424C1988`), `binding == model+0xA0`.
- Animation pose buffer `skeleton = *(driver+0x10)`:
  - `+0x48` u16 bone count (the player animation skeleton has **178** bones);
  - `+0x4A` i16 == 0, `+0x60` u32 == 0;
  - `+0x58` u32 offset of the rotations array (quat xyzw, 16 B each), `+0x5C` u32 offset of the positions array (Vector4, 16 B each). Both are **model space**: MGS5VR computes `world = compose(root, bone(i))` and `local = inverse(bone(parent)) * bone(i)`.
- Model (render palette, **132** bones for the body):
  - `+0x40` root 4×4 (world);
  - `+0xE0` palette matrices (float[16] per bone, 64 B);
  - `+0xE8` u32 bone-name hashes;
  - `+0xF0` parents (stride 0x20: i32 parent at +0, Vec3 bind offset at +0x10);
  - `+0xF8` u16 palette count; `+0xFA` bit0 flag; `+0x120` u64 model code;
  - `+0x180` group-name u32 array, `+0x1E8` u16 group count.
- Player animation bone indices used by MGS5VR:
  - 0 root/pelvis, 1–2 spine (2 = chest), **4 head**;
  - 5→6,7,8 left shoulder, upper arm, forearm, wrist; 9→10,11,12 right side;
  - 13 and 17 legs;
  - left fingers 24, 30, 34 (parent 8); right fingers 40, 46, 50 (parent 12).
- Appearance parts: `character+0x610` (vtable `0x1422E56C0`, `+0x38` backlink, `+0x48` model list). Hide or show a normal group with `0x1401CCC30` / `0x1401CEE30`; model set-transform `0x1401CE7F0`.
- **This is the key data source for driving the ER-side Venom Snake rig from MGSV's animation each frame.**

### 7.6 Camera, FOV, time scale, weather and time of day (IGCS MGS5 camera AOBs; scanned on 1.0.15.4)

| IGCS key | Result on 1.0.15.4 | Struct offsets |
|---|---|---|
| CAMERA_ADDRESS `0F 28 02 0F 29 81 F0 00 00 00 0F 28 4A 10 0F 29 89 00 01 00 00 C3` | **0x14044E960** | quat +0xF0, pos +0x100 |
| CAMERA_CUTSCENE_ADDRESS | **0x14042F408** | quat +0x120, pos +0x130 |
| CAMERA_CUTSCENE_WRITE1 (fixed rel32 removed) | **0x14042FD89** | |
| FOV_WRITE `F3 0F 59 8E B8 05 00 00 F3 0F 58 C8 F3 0F 11 49 0C …` | **0x141118B8C** (inside the player camera update) | fov at struct +0xC |
| FOV_WRITE1_CUTSCENE (as MSVC `48 8B CE`) | **0x14042F49B** | +0x3C |
| FOV_WRITE2_CUTSCENE | **0x14042FDAB** | +0x3C |
| GAMESPEED_WRITE `F3 0F 11 7F 08 F3 0F 11 77 0C 48 8B 47 28 48 85 C0` | **0x140081713** (CoreFramework area) | time scale +0xC |
| TIMESTOP_READ (as MSVC `45 33 C0 48 8B C8`) | **0x1405396FD** | +0x38 |
| DOF_ADDRESS / DOF_WRITE | **0x140437CED** / **0x140437D38** | focus +0x108, focal +0x110, aperture +0x124 |
| WEATHER_ADDRESS | **0x1407BEEBB** | +0x10 direct, +0x18 transition, +0x20 intensity |
| TOD_ADDRESS (as MSVC `48 8B CB`) | **0x14053808A** | +0x4C seconds |

Also from tpp-mod (VERIFIED as function starts):
- `PlayerCameraImpl::SetTpsParams` `0x14111B5D0`, `SetAroundParams` `0x14111AAA0`;
- `SubjectiveCamera::SetParameter` `0x14105AE20`, `SetDefault` `0x14129F170`;
- `PlayerCameraImpl` tpp fov at `+0x2DC`;
- `player_mouse_event_update` `0x1409CCB60`.
- MGS5VR: world and view matrix builders `0x140438AC0` / `0x140438C20`, perspective builder `0x140241B00`, viewport publication `0x1401B9490`, render job `0x1401BD390`, scene draw `0x1401BEEC0`.

### 7.7 Cheat Engine table AOBs (Hexorg/CheatEngineTables, 2015–2016 tables; `data/mgsv/ct_aob_scan_1.0.15.4.txt`)
- 44 patterns are still unique in 1.0.15.4, mostly resource, ammo, vehicle and FOB cheats.
- `TimeScaleAOB` (`0x140081718`) and `WeatherAOB` (`0x1407BEEBF`) agree with IGCS.
- `WaypointAOB` → `0x140936CE3`.
- **"Player Position Data"** `44 0F 28 50 20 89 DA` (Denuvo encoding) → MSVC form `44 0F 28 50 20 8B D3` matches uniquely at **`0x140FF556B`**, inside function `0x140FF5510..0x140FF56B5`. The table reads `X=[rax+0x20]`, then Y and Z follow (struct base = rax). Semantics in 1.0.15.4 are **UNVERIFIED**.
- Paul44's current table (v1.7, updated 2026-09-17, fearlessrevolution t=39886) could not be fetched because of a Cloudflare challenge. If someone can open it in a browser, its AOBs should be checked against this build.

### 7.8 Frame loop and misc (tpp-mod; VERIFIED as function starts)
- `fox::CoreFramework::EnterFrame` `0x14007F840`, `LeaveFrame` `0x14007FDB0`.
- Frame-limiter sleep call site `0x140032C9C`. FPS-unlock jump `0x1400810AA → 0x140081278`.
- `fox::GetTimeSystem` `0x1400F21F0`; `FoxStrHash32` (StrCode32) `0x1400234E0`.
- `fox::gm GameObjectMessageSystem` `0x142C3A5D0`.
- `g_renderWork` `0x142B73580`; D3D device and context `0x142C6B870` / `0x142C6B878`.
- `tpp::ui::utility::GetCurrentMissionId` `0x140911210`, `GetCurrentLocationId` `0x1409111D0`.
- `StrCode64` `0x141A09AC0`.
- IHHook `UpdateFOVLerp` `0x141115ED0` (unique pattern; focal target at `this+0x2FC`).

---------------------------------------------------------------------------------------------------

## 8. Input, focus, and window behaviour (VERIFIED code paths unless marked)

### 8.1 Imports relevant to input, graphics and platform (full list in `data/mgsv/imports_1.0.15.4.txt`)
- `DINPUT8.dll`: `DirectInput8Create`.
- `XINPUT1_3.dll`: **ordinal 2 (`XInputGetState`) only**, IAT slot `0x14208E070`, thunk `0x14032892C`.
- `HID.DLL` (12 functions) and `SETUPAPI.dll`: raw HID enumeration, probably for DualShock 4 (tpp-mod has a `scepad` component).
- `USER32`: `RegisterRawInputDevices`, `GetRawInputData`, `GetKeyboardState`, `MapVirtualKeyW`, `ToUnicode`, `ToAscii`, `GetCursorPos`, `SetCursorPos`, `ShowCursor`, `ClientToScreen`, `ScreenToClient`, `GetForegroundWindow`, `SetForegroundWindow`, `AttachThreadInput`, `GetWindowThreadProcessId`, `IsIconic`, `IsWindowVisible`, `IsWindow`, `GetAncestor`, `SendMessageW`, `RegisterClassExW`, `CreateWindowExW`, `GetMessageW`, `TranslateMessage`, `DispatchMessageW`, `DefWindowProcW`, `PostQuitMessage`, `SetWindowPos`, `ShowWindow`, `SetWindowLongPtrW`, `CallWindowProcW`, `RegisterDeviceNotificationW`.
  - **No `PeekMessage`, no `GetAsyncKeyState`, no `ClipCursor`.**
- `d3d11.dll!D3D11CreateDevice`, `dxgi.dll!CreateDXGIFactory1`, `IMM32!ImmDisableIME`, `WINMM` (`timeBeginPeriod`), `MFPlat`/`MFReadWrite` (video), `steam_api64.dll` (13 imports including `SteamAPI_Init`; **no `SteamAPI_RestartAppIfNecessary`**).
- Second, lower-case import set (Denuvo): `kernel32`, `user32` (dialog, menu and clipboard), `advapi32` (`CryptAcquireContextA`, `GetUserNameW`), `ntdll` (`NtQuerySystemInformation`, `NtCreateSection`, `NtMapViewOfSection`), `wininet` (`HttpOpenRequestA`…), `gdi32`, `shell32`, `shlwapi`.

### 8.2 Gamepad
- **XInput:** a per-pad object polls `XInputGetState(dwUserIndex=[obj+0xC], &state)` at `0x140034FC1` (function `0x140034F90`). A non-zero result marks the pad disconnected. XInput state does not depend on window focus, so **a hidden MGSV still sees the physical controller** unless intercepted.
  - MGS5VR replaces the IAT slot (VirtualProtect plus `InterlockedCompareExchangePointer` on `FirstThunk` entries of `XINPUT1_3.dll`) with its own `getState`. That function returns synthetic `XINPUT_STATE` for index 0 and increments `dwPacketNumber` when the state changes. It also hooks `XInputSetState` for rumble. Source: `src/gamepad_hook.cpp`. **This is the proven way to drive MGSV's player from another process's input.**
  - Button bits MGS5VR injects: run=`0x0040` (L3), stance=`0x1000` (A), carry=`0x2000` (B), dive/switch=`0x4000` (X). The game uses its "Action Type" control layouts (`PadType.ACTION_A/SHOOTER_A/ACTION_B/SHOOTER_B`).
- **DirectInput8** (`0x140034AC0`): `DirectInput8Create(GetModuleHandleW(0), 0x800, IID_IDirectInput8W @0x14212A8C0, &g_pDI @0x142B5AA98, 0)`, then `g_pDI->EnumDevices(DI8DEVCLASS_GAMECTRL=4, cb=0x1400349E0, ctx, DIEDFL_ATTACHEDONLY)`. This covers **game controllers only, not the keyboard or mouse.** A dinput8 proxy can make `EnumDevices` report nothing, so physical joysticks are ignored.

### 8.3 Window, message thread and WndProc
- A **dedicated window thread** runs `0x140071010`. It calls `0x140070880` (register class and create window), then loops `GetMessageW(&msg,0,0,0)` → `TranslateMessage` → `DispatchMessageW`, with a post-dispatch callback `0x1400710B0`. The game update does not depend on pumping messages.
- `WNDCLASSEXW`: style `CS_HREDRAW|CS_VREDRAW`, **WndProc `0x140070430`**.
- `CreateWindowExW` (`0x1400705C0`):
  - style `WS_OVERLAPPEDWINDOW` (0xCF0000) or `WS_POPUP` (0x80000000) or `WS_CAPTION|WS_SYSMENU` (0xCA0000), depending on the window or fullscreen mode;
  - `|0x46000000` (WS_CHILD|WS_CLIPSIBLINGS|WS_CLIPCHILDREN) on one code path, apparently when a parent HWND is supplied, so the engine may support embedding as a child window (exact condition UNVERIFIED);
  - default 800×600 at `CW_USEDEFAULT`.
- WndProc `0x140070430`:
  - calls optional pre-hooks `*(fn*)0x142B642C8()` and `*(fn*)0x142B642D0(hwnd,msg,wp,lp)`. Setters: `0x140070F20` (called from the renderer at `0x14024581E` and `0x140248483`) and `0x140070F30` (called from `.xdata`);
  - handles WM_CREATE (registers the window), WM_DESTROY (`PostQuitMessage` if main), `WM_ERASEBKGND`→1, and **`WM_INPUT` → `0x141A45540`**;
  - everything else goes to `DefWindowProcW`.
  - **There is no WM_ACTIVATE, WM_ACTIVATEAPP, WM_KILLFOCUS or WM_SETFOCUS handling in the WndProc itself.**
- A second message handler `0x14053ADF0` (mouse and cursor) also calls `GetRawInputData` and `ShowCursor`. It **recentres the cursor with `SetCursorPos`** (`0x14053AFD9`) under flags at `0x142BD3B74..0x142BD3B78`, then forwards to `0x141A45540`. Because it is driven by `WM_INPUT`, it should be idle when MGSV is not focused (UNVERIFIED). Guard it anyway (hook `SetCursorPos` like IHHook does).
- tpp-mod hooks the WndProc at `0x140070430` itself.

### 8.4 Keyboard and mouse: Raw Input
- `RegisterRawInputDevices(rid=0x142C6C710, n=2, cb=0x10)` in `0x141A45A50` (called from `0x141A4408C`):
  - `rid[0] = {usagePage 1, usage 2 (mouse), dwFlags = (cfg[+0x24]&1)<<8, hwndTarget = cfg[+8]}`;
  - `rid[1] = {1, 6 (keyboard), same flags}`.
  - `0x100` = **RIDEV_INPUTSINK**, which is only set when that config bit is 1. Otherwise there is **no keyboard or mouse input while MGSV is in the background**. The meaning of the bit is UNVERIFIED (possibly a dev or editor option).
- `WM_INPUT` handler `0x141A45540`: `GetRawInputData(h, RID_INPUT=0x10000003, …)`. `dwType 0` (mouse) → `0x141A45860`; `1` (keyboard) → `0x141A45790`.
  - `GetRawKeyboardData` `0x141A45650` (tpp-mod) also calls `GetKeyboardState` and `ToUnicode` for text.
  - Key table `fox::g_vkKeyTable` `0x142C6C2D0` (0x400 bytes), VK→index map at `0x1424BA950` (0x62 entries).
  - `g_rawMouseData` `0x142C6C6D8`; client-size floats `0x142C6C750/754`.
  - Listeners: `fox::KeyboardListener::SetRawKeyData` `0x1432C0A60` (tpp-mod), `fox::MouseListener::SetRawData` `0x140031AF0`.
- To block or inject keyboard and mouse: subclass the WndProc and eat `WM_INPUT` (IHHook), or hook `0x141A45540` / `0x141A45650`. For PhantomRing, the simplest choice is no keyboard or mouse at all: map ER-side input to the virtual XInput pad.

### 8.5 Focus, minimise and fullscreen behaviour
- **Iconic watcher thread** `0x140001BC0`: polls about every 33 ms (`0x14001ECC0(…,0x21)`) whether the main window `IsIconic`. On the transition to minimised it posts engine event **10**; on restore it posts event **11** (`0x140020060(10|11,0)`). It also calls `SetThreadExecutionState`.
  - Events 10 and 11 are very likely "suspend" and "resume" (**UNVERIFIED**). MGS5VR observed that "minimizing MGSV has stopped native rendering". **Never minimise the hidden MGSV.**
- **Render skip:** the render job `0x1401BD390` calls `0x14024A1D0(view)` for each view; it returns `IsWindow && !IsIconic && IsWindowVisible`. When false it calls `0x1401BEA90` (skip) instead of scene draw `0x1401BEEC0`. A **hidden (`SW_HIDE`) window therefore costs no GPU rendering**, which is what we want. Whether game logic keeps ticking at a sane rate without Present is **UNVERIFIED**; the internal frame limiter in 7.8 should cap it.
- **Exclusive-fullscreen focus logic** `0x14024AB30`: in fullscreen mode (`[obj+0x66]`) with focus lost, it may `SendMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE)`. On recovery it calls `0x14024B5B0`, which uses the **force-foreground trick**: `GetForegroundWindow` → `GetWindowThreadProcessId` → `AttachThreadInput(cur,fg,TRUE)` → **`SetForegroundWindow(hwnd)`** → `AttachThreadInput(…,FALSE)`. **Run MGSV windowed** to avoid this. Also hook `SetForegroundWindow` and `ShowWindow` in the guest so MGSV can never steal focus from ER.
- **Audio:** `0x140353300` is Wwise `AK::SoundEngine::GetDefaultPlatformInitSettings` (Windows layout matches: `hWnd=GetForegroundWindow()`, threadLEngine priority 1 and stack 0x10000, pool 16 MB, ratio 1.0f, refills 4, **`bGlobalFocus=true`** at +0x3C). By default MGSV audio is **not** muted on focus loss. Whether the game overrides this is UNVERIFIED. Muting options: `TppSoundDaemon.SetMute(...)` from Lua, or Proton or Pulse per-app volume.
- **Third-party observations (UNVERIFIED):**
  - MGS5VR booted to mission 30010 and accepted XInput-hooked input "without changing foreground focus".
  - Its attempt to run MGSV on a private desktop "did not produce a working game session".

### 8.6 Hidden-MGSV checklist for the guest DLL (synthesis)
1. Use windowed mode, not exclusive fullscreen. Set the resolution low (for example 640×360) to cut cost if anything renders.
2. After the window exists, hide it with `ShowWindow(SW_HIDE)` or `SetWindowPos` off-screen. **Never minimise.** Keep it alive. Optionally parent it to an invisible host, since the engine supports a parent HWND, but that is untested.
3. Patch the `XInputGetState` IAT slot `0x14208E070` to return the ER-side controller state from shared memory, and return "disconnected" or neutral state for indices 1–3.
4. Make the dinput8 proxy's `IDirectInput8::EnumDevices` enumerate no game controllers.
5. Ignore keyboard and mouse: eat `WM_INPUT` in a WndProc subclass, or rely on no INPUTSINK.
6. Hook `SetCursorPos`, `ShowCursor`, `SetForegroundWindow`, `AttachThreadInput` and `ShowWindow` so they become no-ops for MGSV's own window (MinHook on the user32 exports or IAT patching).
7. If MGSV's audio is not wanted, mute it with `TppSoundDaemon.SetMute`.
8. Run per-frame logic in the `lua_pcall` depth-1 hook or the `CoreFramework::EnterFrame` hook. Read player root and bones in the camera-setter and skin-publication hooks, and publish them to shared memory.

---------------------------------------------------------------------------------------------------

## 9. Other projects inspected (sources)

| Project | Revision | Target | Use to us |
|---|---|---|---|
| [TinManTex/IHHook](https://github.com/TinManTex/IHHook) (MIT) | master `71801f1` (2022-07-13); dev `64c02df`; texdev `8e108e0` | 1.0.15.3 en/jp | proxy pattern, Lua hook design, RawInput blocking, D3D11 hook; addresses obsolete |
| [TinManTex/InfiniteHeaven](https://github.com/TinManTex/InfiniteHeaven) | `b0bfab4` (2023-03-25) | 1.0.15.3 | Lua API usage (section 5) |
| [nikamigaming-create/MGS5VR](https://github.com/nikamigaming-create/MGS5VR) (MIT) | `87e51f2` (2026-10-11) | **1.0.15.4 sha256 match** | camera owner, skeleton, XInput IAT injection, Lua pump, UI input gates (`0x141DD7EB0` UiPad input-mask predicate, button getter `0x141DD7BA0`), profile `profiles/tpp-1.0.15.4.json` |
| [alicealys/tpp-mod](https://github.com/alicealys/tpp-mod) (GPL-3.0) | `0c8dc3d` (2026-10-12) | **1.0.15.4 en sha1 match** (also older branches `day1820`, `day3800`) | `src/client/game/symbols.hpp` (500+ addresses), `structs.hpp` (~10k lines of engine structs), scheduler and scripting hooks. GPL: reference only; do not copy code into a non-GPL PhantomRing. |
| [FransBouma/InjectableGenericCameraSystem](https://github.com/FransBouma/InjectableGenericCameraSystem) `Cameras/MGS5` (BSD-2) | `42e76df` | v1.0.12+ | camera, FOV, DOF, time and weather AOBs (7.6) |
| [unknown321/dynamite](https://github.com/unknown321/dynamite) (Commons Clause) | `e559c3d` | 1.0.15.3 (README: "You MUST roll back your game") | 2-player co-op; `docs/MAKING_OF.md` covers FOB/P2P player-2 internals; mentions a debug exe used for naming |
| Hexorg/CheatEngineTables | (gh search) | 2015–2016 builds | 44 AOBs still unique (7.7) |
| Paul44 CT v1.7 (fearlessrevolution t=39886) | — | updated 2026-09-17 | not fetched (Cloudflare) |
| DSOGaming 2015, Steam forum threads | — | — | Denuvo in MGSV (UNVERIFIED web) |

---------------------------------------------------------------------------------------------------

## 10. Open items and next steps
1. **Runtime confirmation needed (requires launching MGSV, which is out of scope for this task):**
   - `vars` liveness and update rate;
   - `*(u64*)0x142C16920` non-null after boot;
   - meaning of events 10 and 11;
   - logic tick rate when the window is hidden (no Present);
   - Wwise `bGlobalFocus` actually used;
   - `RIDEV_INPUTSINK` config bit default;
   - Proton `dinput8=n,b` override.
2. **Bone names:** `model+0xE8` holds u32 hashes, probably StrCode32 of names like `SKL_004_HEAD`. Map the 178-bone animation skeleton and 132-bone palette to names using the player `.gskl` and `.fmdl` (Soulstruct, Blender and FoxEngine templates in the old MGS project). This is needed for retargeting to the ER c0000 skeleton.
3. When Ghidra is available, decompile:
   - `SkinPublish 0x141A6CAA0`, `0x141118B1A` caller (player camera update);
   - `0x140FF5510` (the CT "player position" function);
   - `TppPlayer2` `WarpAndWaitBlock` handling (StrCode32 of the id);
   - `GameObject.SendCommand` dispatch, so commands can be issued natively without Lua.
4. Decide the Lua strategy. Recommended: the MGS5VR-style `lua_pcall` depth-1 pump plus a C module registered via `foxlua::NewModule` / `AddCFunc` / `EndModule` after `0x14017A0A0`. Wrapping `TppMain.OnUpdate` gives an exact per-update tick without touching game archives.
