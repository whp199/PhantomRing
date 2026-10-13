# Binary scout: `eldenring.exe` (host) and `mgsvtpp.exe` (guest)

PhantomRing research record. Static, read-only analysis. Neither game nor Steam was launched, and no
game or Steam files were modified. Date: 2026-10-11.

**Legend.** **VERIFIED** means checked against the local bytes with the tools below.
**UNVERIFIED** means from memory or the web, or a semantic interpretation that the bytes don't
prove on their own. **INFERRED** means derived from a strong structural argument (stated each
time) but not individually confirmed by reading code. All addresses are **RVAs** unless they're
marked VA. Both images prefer ImageBase `0x140000000`.

---

## 0. Key findings

1. **Elden Ring is `eldenring.exe` FileVersion 2.7.1.0** (VERIFIED, version resource), PE
   TimeDateStamp 2026-09-01. vswarte's fromsoftware-rs reference
   (`<fromsoftware-rs>`) has an explicit `Ww2710`
   bundle for this exact version. **74 of its 82 `*_vmt` RVAs match our RTTI→COL→vtable
   extraction exactly** (VERIFIED by script). The 8 misses are FD4/CSTask vtables that have no
   RTTI COL (see §2.6). Its 2.7.1.0 function RVAs (`register_task 0xeb3e50`, `cs_ez_draw_draw_line
   0x2652640`, …) all land exactly on `.pdata` function starts (VERIFIED for existence only).
2. **ER protection is Arxan (GuardIT).** VERIFIED evidence:
   - A 2nd `.text` section (#10, RVA `0x4c13000`, 18.8 MB, entropy 7.29).
   - The entry point's first call goes into a register-saving stub.
   - An extra `KERNEL32.DLL` import descriptor (GetThreadContext, Add/RemoveVectoredExceptionHandler,
     HeapLock/Walk/Unlock).
   - The strings `GuardIT CHEAT_REPORT_NG` and `RequestSendArxanInfoParams`.

   **No RTTI vtable function pointer points into the Arxan section** (0 of 10,146 vtables), so
   vtable hooks avoid Arxan-patched code. me3 0.13.0 can neuter Arxan with the profile key
   `disable_arxan = true` (VERIFIED in the me3 source and in the binary strings).
3. **MGSV is `mgsvtpp.exe` FileVersion 1.0.15.4** (VERIFIED; `InternalName Tpp_main.exe`), PE
   TimeDateStamp 2026-07-07 (matches `version_info.txt` `..._0707_1632`). **It is protected by
   what is almost certainly Denuvo** (attribution UNVERIFIED; the evidence is VERIFIED):
   - The literal `DENUVO` appears as an immediate inside `.xdata` code at RVA `0x6f3fbe1`.
   - A 117 MB RWX `.xdata` code section holds the entry point and 2 obfuscated TLS callbacks.
   - Renamed sections: `.xtext`, `.edata` (the old .rdata), `.data2`, `.ecode` (the .pdata),
     `.xtls`.
   - Extra lower-case-named import descriptors (`kernel32.dll`, `ntdll.dll` NtCreateSection /
     NtMapViewOfSection / NtQuerySystemInformation, …) whose IATs live in `.xtls`.
   - An `ExportProductMetadata` export.

   Despite this, **the game code in `.xtext` is plaintext** (entropy 6.49, and it disassembles
   cleanly). **MGSV has no ASLR**: DllCharacteristics 0x8120 lacks DYNAMIC_BASE and there is no
   reloc directory (VERIFIED). Every RVA below is therefore a fixed VA = `0x140000000 + RVA`.
4. **MGSV embeds Lua 5.1.5** (VERIFIED: `$Lua: Lua 5.1.5 Copyright (C) 1994-2012 Lua.org, PUC-Rio $`
   at RVA `0x24b47a0`). The full Lua C API is located (§3.6, `data/mgsv_lua_c_api.tsv`, 115
   entries, 57 VERIFIED / 58 INFERRED). Highlights:
   - `lua_pcall 0x1a116c0`
   - `luaL_loadbuffer 0x1a178e0`
   - `lua_getfield 0x1a111e0`
   - `lua_setfield 0x1a11e00`
   - `lua_pushcclosure 0x1a11770`
   - `luaL_register 0x1a18080`
   - `luaL_newstate 0x1a17bf0`

   All **124 native `luaL_register` libraries** were recovered with every C function RVA. That
   includes `Player` (172 functions, for example `SetPadMask 0x9df360` and
   `RequestToSetCameraRotation 0x9daf90`), `PlayerInfo`, `TppUiCommand` (254) and `Gimmick`.
   **678 Fox-binder C functions** were recovered too, for example `GameObject.SendCommand
   0xbfa380`, `Fox.StrCode32 0x178910` and `Time.GetFrameTime 0x1849b0`.
5. **MGSV/Fox Engine is built without RTTI** (VERIFIED: only 16 `.?AV` type descriptors, all CRT,
   std or `fox::Exception`/`fox::LuaOutsideProtectedEnvErrorException`). So RTTI→vtable doesn't
   work for MGSV. Use the Lua bindings and import call sites instead (§3.7).
6. **Loader choice.**
   - **ER:** load our native DLL with **me3 `[[natives]]`** (VERIFIED working pattern in the old
     project's profiles). Never drop proxy DLLs into the ER game dir, because the Steam/EAC path
     would load them too.
   - **MGSV:** **`dinput8.dll`** (one by-name import, `DirectInput8Create`), **`xinput1_3.dll`**
     (import by ordinal #2 = `XInputGetState`; MGSV also `LoadLibraryW("XInput1_4.dll")` then
     `"XInput1_3.dll"` at 0x3f4f80) and **`winmm.dll`** (3 imports) are all viable proxies.
     Under Proton they need a `WINEDLLOVERRIDES=<name>=n,b` launch option (UNVERIFIED, standard
     practice).
   - **For PhantomRing an `xinput1_3.dll` proxy (or an IAT hook of `XInputGetState`) is
     attractive.** MGSV runs hidden and needs synthetic pad input; that function is exactly where
     MGSV reads the pad (the caller is `0x34f90`).

---

## 1. Tools written (reusable, read-only)

All live in `tools/`. Python 3.14 with pefile and numpy
from the system. `pe_common.load_capstone()` auto-adds
`<capstone>` (capstone 5.0.x) to `sys.path`, so no
PYTHONPATH is needed. MSVC names are demangled through `/usr/lib/llvm/22/bin/llvm-undname`.

| script | purpose | typical run time |
|---|---|---|
| `pe_common.py` | PE mapping, section lookup, `.pdata` function lookup (follows chained unwind), entropy, demangling, capstone loader | — |
| `pe_scout.py EXE [--md out --json out]` | header, sections + entropy, TLS + callback disassembly, load config, Rich, version resource, full imports/delay imports/exports, watch-list, DLL-name strings, proxy-DLL evidence, protector heuristics | 3–5 s |
| `rtti_vtables.py EXE --tsv all.tsv [--grep RX… --md t.md] [--dump-vfuncs RX]` | MSVC x64 RTTI: TypeDescriptors → COLs (sig=1, pSelf check) → vtables (slot holding COL VA) → vfunc count and section histogram, plus the base-class list from CHD/BCA | <2 s |
| `str_xref.py EXE -p RX/-e STR [--utf16] [--tsv/--md]` | strings in data sections with **code xrefs** (numpy brute-force RIP-relative disp32 + capstone confirmation) and **data xrefs** (absolute 8-byte pointers), plus the containing `.pdata` function | 5–10 s |
| `addr_xref.py EXE RVA…` | every `call/jmp rel32`, RIP-relative reference and absolute data pointer to given RVAs | ~5 s |
| `iat_xref.py EXE NAME…` (`dll!name`, `dll!#ord`) | IAT slot of an import and every `call/jmp/mov [rip+IAT]` site, with containing function | 2–5 s |
| `lua_reg_scan.py EXE --tsv --md [--libs A,B] [--binders out.tsv]` | Lua 5.1 `luaL_register` library recovery: auto-finds `luaI_openlib`/`luaL_register`, wrappers and stack- or static-built `luaL_Reg` tables (mini emulator), plus a Fox "binder" pass (`call f(tbl, "name", cfunc)`) | ~10 s |
| `disasm.py EXE RVA[:END]… [--func]` | annotated disassembly (RIP targets resolved to section and string) | — |

Generated data (all under `docs/research/data/`):

| file | content |
|---|---|
| `er_pe.md`, `er_pe.json`, `mgsv_pe.md`, `mgsv_pe.json` | full pe_scout output, including **complete import tables** |
| `er_2.7.1.0_rtti_vtables.tsv` | all 12,046 ER type descriptors / 10,146 COL+vtables (name, TD, COL, offset, vtable, #vfuncs, vfunc sections, bases) |
| `er_curated_vtables.md` | the hook-relevant subset (reproduced in §2.6) |
| `er_vfuncs_selected.txt` | per-slot vfunc RVAs for camera/pad/task/draw/window/graphics classes |
| `mgsv_1.0.15.4_rtti_vtables.tsv` | the 16 MGSV type descriptors |
| `mgsv_lua_c_api.tsv` | Lua C API map (RVA, VA, symbol, confidence, evidence) |
| `mgsv_lua_libs.tsv` / `.md` | 124 `luaL_register` libraries, each function → C function RVA |
| `mgsv_lua_binders.tsv` | 1,360 Fox binder call sites (callee, registering function, namespace guess, name, cfunc RVA) |
| `mgsv_lua_runtime_strings_xrefs.tsv`, `mgsv_player_strings_xrefs.tsv` | string RVA, file offset and xrefs for the Lua and Player/Camera/vars strings |

**File-offset ↔ RVA** (VERIFIED from the section tables):
- **ER:** `.text#0` off = RVA−0xA00; `.rdata` off = RVA−0x1200; `.data` off = RVA−0x1A00;
  `.idata` off = RVA−0xB0CC00; `.text#10` off = RVA−0xB0DA00.
- **MGSV:** `.xtext`, `.edata` and `.data2` off = RVA−0xC00; `.xdata` off = RVA−0x14CA00.

---

## 2. Elden Ring – `<steam-library>/common/ELDEN RING/Game/eldenring.exe`

### 2.1 Identity (VERIFIED)
- size 87,042,128; **sha256 `1a3547101327f65d0c76da2f9190ac0aa66871ea42bae2aecc61e11a8b597891`**
- FileVersion/ProductVersion **2.7.1.0**, Company FromSoftware, Inc.
- TimeDateStamp `0x6a96b418` (2026-09-01 11:16:40 UTC)
- PDB `W:\GR\RootBranch\Source\GR\Title\Project\vs2015\dist_win64_d3d12\MasterLTO_SteamWW\GR_win64_MasterLTO_SteamWW_d3d12.pdb`
- Machine x64, Subsystem 2 (GUI), Linker 14.0, ImageBase `0x140000000`, SizeOfImage `0x5e0da00`
- DllCharacteristics `0x8160` = HIGH_ENTROPY_VA | **DYNAMIC_BASE (ASLR on)** | NX | TS_AWARE.
  At runtime use `GetModuleHandle(NULL)` + RVA.
- **EntryPoint `0x24fe160`** (in `.text#0`): `sub rsp,28; call 0x1dd1761; add rsp,28; jmp 0x24fdff4`.
  0x1dd1761 is an Arxan-style context-save stub (pushes all GPRs and XMM0-5, realigns the stack,
  `jmp 0x1c521c`) that replaced `__security_init_cookie`. 0x24fdff4 is the normal
  `__scrt_common_main_seh`-style body.
- **TLS:** directory at `0x32ad798`. The callback array at `0x29ca5a0` is **empty** (no static
  TLS callbacks).
- **No LOAD_CONFIG directory** (so no static CFG or cookie metadata).
- **Security** (Authenticode) at file offset `0x5300000`, size 0x2850.
- **BASERELOC** directory at `0x5ce1990` lies **inside the Arxan `.text#10`**, so Arxan rewrote the
  relocations.

### 2.2 Sections (VERIFIED, entropy in bits/byte)
| # | section | RVA | VSize | raw off | raw size | flags | entropy |
|---|---|---|---|---|---|---|---|
|0| `.text` | 0x1000 | 0x29a4800 | 0x600 | 0x29a4800 | R-X CODE | 6.583 |
|1| `.interpr` | 0x29a6000 | 0xc000 | 0x29a4e00 | 0xc000 | R-X CODE | 6.465 |
|2| `.rdata` | 0x29b2000 | 0x1162800 | 0x29b0e00 | 0x1162800 | R-- | 5.116 |
|3| `.data` | 0x3b15000 | 0xd51bc4 | 0x3b13600 | 0x249e00 | RW- | 4.695 |
|4| `.pdata` | 0x4867000 | 0x2b3200 | 0x3d5d400 | 0x2b3200 | R-- | 7.126 |
|5| `.tls` | 0x4b1b000 | 0x200 | 0x4010600 | 0x200 | RW- | 4.883 |
|6| `.gfids` | 0x4b1c000 | 0x1200 | 0x4010800 | 0x1200 | R-- | 4.176 |
|7| `.rsrc` | 0x4b1e000 | 0x35c00 | 0x4011a00 | 0x35c00 | R-- | 7.434 |
|8| `.reloc` | 0x4b54000 | 0xb9e00 | 0x4047600 | 0xb9e00 | R-- | 5.48 |
|9| `.idata` | 0x4c0e000 | 0x4200 | 0x4101400 | 0x4200 | **RW-** (writable IAT) | 4.73 |
|10| `.text` (Arxan) | 0x4c13000 | 0x11faa00 | 0x4105600 | 0x11faa00 | R-X CODE | **7.29** |

- `.interpr` holds one function, `0x29a6000`–`0x29a9a58`, with a 0x438-byte frame. It is probably
  a bytecode interpreter loop (UNVERIFIED guess: Havok Script/HKS VM).
- `.pdata` has 235,816 RUNTIME_FUNCTION entries.

### 2.3 Protector: Arxan (VERIFIED evidence, attribution confirmed by in-binary strings)
- `.text#10` has high entropy, and the BASERELOC directory lives in it.
- Separate import descriptor **`KERNEL32.DLL`** (upper-case, last in the list, IAT `0x4c1210c`):
  `GetThreadContext, AddVectoredExceptionHandler, RemoveVectoredExceptionHandler, HeapLock,
  HeapWalk, HeapUnlock`. These are anti-debug/integrity APIs.
- Strings `[Host] GuardIT CHEAT_REPORT_NG` (`.rdata` 0x2bd0ef7) and
  `FromNet::RequestSendArxanInfoParams` (0x308ab44).
- No `.bind` section, so **no SteamStub** (VERIFIED). EAC is external
  (`start_protected_game.exe`). `eldenring.exe` delay-imports `EOSSDK-Win64-Shipping.dll`
  (`EOS_AntiCheatClient_*`, 32 functions).
- **me3 handles Arxan.** `ModProfileV1` has `disable_arxan: Option<bool>` (VERIFIED,
  `me3-source/garyttierney-me3-989b522/crates/mod-protocol/src/lib.rs`). The mod-host crate
  version is 0.13.0, the same as `tools/me3/bin/me3 --version` = `me3 0.13.0`. The
  `me3_mod_host.dll` strings include `Arxan is now neutered` and `dearxan::analysis`.
  Recommended profile line: `disable_arxan = true`.

### 2.4 Imports (VERIFIED; full lists in `data/er_pe.md`)

DLLs: `d3d12, dxgi, WINHTTP, bink2w64, amd_ags_x64, WS2_32, oo2core_6_win64, WINMM, steam_api64,
CRYPT32, KERNEL32 (208), USER32 (63), GDI32, ADVAPI32, SHELL32, ole32, OLEAUT32, IMM32, SETUPAPI,
WLDAP32, XINPUT1_4, DINPUT8, KERNEL32.DLL (Arxan)`. Delay import: `EOSSDK-Win64-Shipping.dll`. 560
exports (Scaleform/Wwise `AK::` C++ symbols).

Watch-list:
- **dinput8:** `DirectInput8Create`. **xinput1_4:** ordinals **#2 = XInputGetState** and
  **#3 = XInputSetState**. These ordinals were VERIFIED against the export tables of Proton
  Experimental's `xinput1_4.dll`/`xinput1_3.dll`.
- **d3d12:** `D3D12SerializeRootSignature`, **#101 = D3D12CreateDevice**,
  **#102 = D3D12GetDebugInterface** (VERIFIED against vkd3d-proton's and wine's `d3d12.dll` in
  Proton Experimental). **dxgi:** `CreateDXGIFactory1`, `CreateDXGIFactory`. ER doesn't import
  d3d11.
- **user32 input/window:** `GetKeyboardState, GetKeyState, SetCursorPos, GetCursorPos, ClipCursor,
  ShowCursor, SetCursor, ShowWindow, CreateWindowExW, RegisterClassExW, PeekMessageW, GetMessageW,
  DispatchMessageW, TranslateMessage, SetWindowLongPtrW, CallWindowProcW, DefWindowProcW,
  MapVirtualKeyW, ToAscii, SendInput, GetForegroundWindow, SetForegroundWindow, SetFocus,
  RegisterDeviceNotificationW`.
  - **Not imported:** `GetRawInputData`, `RegisterRawInputDevices`, `GetAsyncKeyState`. ER doesn't
    use Raw Input; keyboard and mouse go through DirectInput8 and window messages (VERIFIED absence
    in the import table).
- **kernel32 timing:** `QueryPerformanceCounter` (199 call sites), `QueryPerformanceFrequency`,
  `GetTickCount`, `Sleep`, `SleepEx`. **winmm:** `timeGetTime, timeBeginPeriod, timeEndPeriod`.
- **steam_api64 (13):** `SteamAPI_Init, SteamAPI_Shutdown, SteamAPI_RestartAppIfNecessary,
  SteamAPI_RunCallbacks, SteamAPI_RegisterCallback, SteamAPI_UnregisterCallback,
  SteamAPI_RegisterCallResult, SteamAPI_UnregisterCallResult, SteamAPI_GetHSteamUser,
  SteamAPI_ReleaseCurrentThreadMemory, SteamInternal_ContextInit, SteamInternal_CreateInterface,
  SteamInternal_FindOrCreateUserInterface`.
- DLL names that appear only as strings (dynamic loads possible): `xinput1_3.dll`,
  `xinput9_1_0.dll` (UTF-16), `dbghelp.dll`, `nvapi64.dll`, `nvpowerapi.dll`,
  **`libscepad_x64.dll`** (PS pad library, UTF-16), `hkcompatformats.dll`,
  `xaudio2_7/8/9.dll`, `mscoree.dll`, `mspdb140.dll`.

### 2.5 Key import call sites (VERIFIED with `iat_xref.py` / `addr_xref.py`)
| what | site / function |
|---|---|
| Main window class + creation | `0xe8fbe0` calls `RegisterClassExW` (0xe90961) and `CreateWindowExW` (0xe90a92) |
| **Main WndProc** | **`0xe92fa0`** (address taken at 0xe908f1 in 0xe8fbe0). Handles 0x281 WM_IME_SETCONTEXT (→ DefWindowProc with lParam 0) and 0x10 WM_CLOSE, then chains handlers 0x2670720, `0xe919b0`/`0xe91a70` (CSWindow singleton), 0x26706a0, 0x26705c0 and 0xe91e90 (contains `SendInput`), and finally DefWindowProcW. CSWindow singleton pointer global: **`.data 0x458f910`** (null-checked with the `FD4Singleton.h` assert) |
| 2nd window (launcher/splash?) | `0x2670860` (RegisterClassExW + CreateWindowExW), wndproc 0x2670a50 |
| XInputGetState | import thunk `0x2624e0e`; callers **0x1f2ae80**, **0x1f6d940** |
| XInputSetState (rumble) | thunk 0x2624e14; callers 0x1f2c5b0, 0x1f2c800 |
| DirectInput8Create | thunk 0x2624e1a; caller **0x1f2a0d0** |
| D3D12CreateDevice | thunk 0x2624dfc; caller **0x1ea2730** |
| CreateDXGIFactory1 | thunk 0x2624e08; callers 0x1eae580, 0x1eae970 |
| CreateDXGIFactory | thunk 0x19e142c; callers 0xe90c30, 0xe99850, 0x1eafe50, 0x1eb0400 |
| Cursor clip/position | ClipCursor/SetCursorPos in 0xe20930; GetCursorPos 0xe207b0; ShowCursor 0xe20000 / 0xe20b20 |
| GetForegroundWindow | 0x22aae00, 0x2670780 |
| PeekMessageW (message pump) | 0x1af5360, 0x26707c0 |

These call sites are in the plain `.text#0`, not in the Arxan section. Arxan may still checksum
them, so prefer IAT hooks (the `.idata` IAT is RW) or vtable hooks over inline patches unless
`disable_arxan` is used.

### 2.6 RTTI → vtable table (VERIFIED by COL parsing; full set in `data/er_2.7.1.0_rtti_vtables.tsv`)

Totals: **12,046 TypeDescriptors**, all with type_info vftable VA `0x143256560` (0 rejected).
**10,146 COLs**, each matched by exactly one vtable. **0 vtables have any vfunc in the Arxan
`.text#10`**, and none have one in `.interpr`. "#vf" counts consecutive code pointers up to the
next COL slot (a lower bound; slot 0 is normally the scalar-deleting destructor).

| class | TD RVA | COL RVA | COL off | **vtable RVA** | #vf | direct bases |
|---|---|---|---|---|---|---|
| `CS::CSCam` | 0x3C7F500 | 0x32F0298 | 0 | **0x2AA0A18** | 4 | — |
| `CS::CSPersCam` | 0x3C7F4D8 | 0x32F01F8 | 0 | **0x2AA0A40** | 4 | CSCam |
| `CS::ChrCam` | 0x3C7F568 | 0x32CE840 | 0 | **0x2A2AA08** | 4 | CSPersCam, CSCam |
| `CS::ChrExFollowCam` | 0x3C7F698 | 0x32CEAB8 | 0 | 0x2A2ACA0 | 4 | CSPersCam, CSCam |
| `CS::ChrAimCam` | 0x3C7F4B0 | 0x32CE690 | 0 | 0x2A2A7F0 | 4 | CSPersCam, CSCam |
| `CS::CSDebugCam` | 0x3CF3A10 | 0x3343818 | 0 | 0x2B672C0 | 4 | CSPersCam, CSCam |
| `CS::CSDistViewCam` | 0x3C7FA38 | 0x32CF128 | 0 | 0x2A2BAD0 | 4 | CSPersCam, CSCam |
| `CS::CSCameraImp` (the "CSCamera" singleton) | 0x3C7F710 | 0x32CEBC0 | 0 | **0x2A2AFF0** | 1 | — |
| `CS::CSCameraStep` | 0x3C7F7A8 | 0x32CECB8 | 0 | 0x2A2B798 | 19 | CSStepTask<CSCameraStep>, FD4StepTaskBase… |
| `CS::WorldChrManImp` | 0x3C860A8 | 0x32D9A88 | 0 | **0x2A4E980** | 1 | — |
| `CS::WorldChrManDbgImp` | 0x3C862F8 | 0x32D9EB8 | 0 | 0x2A4EC48 | 1 | — |
| `CS::PlayerIns` | 0x3C85DD8 | 0x32E7DF0 | 0 | **0x2A7FBB0** | 204 | ChrIns, FieldInsBase |
| `CS::ChrIns` | 0x3C80368 | 0x32D03A0 | 0 | **0x2A310C8** | 203 | FieldInsBase |
| `CS::EnemyIns` | 0x3C84E60 | 0x32D7D90 | 0 | 0x2A47090 | 203 | ChrIns, FieldInsBase |
| `CS::FieldInsBase` | 0x3C7E240 | 0x32CD448 | 0 | 0x2A28E78 | 8 | — |
| `CS::CSTaskImp` | 0x3D10340 | 0x3374820 | 0 | **0x2C04B00** | 1 | — |
| `CS::CSTaskGroupIns` | 0x3D10490 | 0x3374A98 | 0 | 0x2C08CF8 | 3 | — |
| `CS::CSEzTask` | 0x3C67778 | 0x3374628 | 0 | 0x2C048A0 | 6 | FD4TaskBase, FD4ComponentBase |
| `CS::CSEzTaskProxy` | 0x3D102A8 | 0x33746C8 | 0 | 0x2C049F8 | 4 | FD4TaskBase, FD4ComponentBase |
| `CS::CSEzRabbitTask` | 0x3C86178 | 0x3374678 | 0 | 0x2C04910 | 6 | CSEzRabbitTaskBase, CSEzTask, … |
| `CS::CSPad` | 0x3D0BC40 | 0x336D7A0 | 0 | **0x2BE3A70** | 7 | FD4::FD4BasePad |
| `CS::CSPadStep` | 0x3D0BF78 | 0x336D848 | 0 | 0x2BE4378 | 19 | CSStepTask<CSPadStep>, … |
| `CS::CSInGamePad` | 0x3C7F588 | 0x32CE8C8 | 0 | 0x2A2AA40 | 1 | CSPadWapper<CSInGamePad_UserInput2, …UserInput1> |
| `CS::CSInGamePad_UserInput1` | 0x3D0BEB0 | 0x336D5E0 | 0 | 0x2BE35E8 | 7 | CSPad, FD4BasePad |
| `CS::PadManipulator` (player controller) | 0x3C7FFD0 | 0x32CFB18 | 0 | **0x2A2E788** | 55 | CS::ChrManipulator |
| `DLUID::PadDevice<DLMultiThreadingPolicy>` | 0x3D4BBD0 | 0x33DECD0 | 0 | **0x30CD048** | 51 | DLUserInputDeviceImpl, DLUserInputDevice |
| `DLUID::KeyboardDevice<…>` | 0x3D4BC20 | 0x33DED58 | 0 | 0x30CD4D0 | 51 | same |
| `DLUID::MouseDevice<…>` | 0x3D4BC70 | 0x33DEDE0 | 0 | 0x30CD840 | 51 | same |
| `DLUID::VirtualMultiDevice<…>` | 0x3D4BCC0 | 0x33DEE68 | 0 | 0x30CDA30 | 51 | same |
| `DLUID::DLUserInputManagerImpl<…>` | 0x3D48290 | 0x33D9A18 | 0 | 0x30BC908 | 5 | DLUserInputManagerImplBase |
| `CS::CSGraphicsImp` | 0x3CF3E50 | 0x3343FF8 | 0 | **0x2B68558** | 1 | — |
| `CS::CSGraphicsStep` | 0x3CF3F60 | 0x3344170 | 0 | 0x2B69BE0 | 19 | CSStepTask<CSGraphicsStep>, … |
| `CS::CSDrawStep` | 0x3CF3B40 | 0x3343A90 | 0 | 0x2B67DD0 | 28 | CSStepTask<CSDrawStep>, … |
| `CS::CSFlverDrawSystem` | 0x3CF58D8 | 0x3346F50 | 0 | 0x2B71800 | 25 | GXSR::GXFlverDrawSystem |
| `CS::CSEzDraw` (debug draw) | 0x3CFA358 | 0x334EAE8 | 0 | **0x2B814C0** | 6 | FD4::FD4HkEzDraw, FD4::FD4EzDraw |
| `CS::CSSgEzDraw` | 0x3CFA3A8 | 0x334EBE8 / 0x334ECB0 / 0x334ECD8 | 0 / 0x10 / 0x80 | 0x2B81560 / 0x2B815A0 / 0x2B815B0 | 7/1/2 | GXSG::GXSgEntity, … |
| `FD4::FD4HkEzDraw` | 0x3CFA308 | 0x334EA18 | 0 | 0x2B81488 | 6 | FD4EzDraw |
| `CS::CSMenuManImp` | 0x3C968A0 | 0x32EFD48 | 0 | **0x2A9EED8** | 1 | — |
| `CS::CSFeManImp` | 0x3C96AC8 | 0x32F0220 | 0 | **0x2AA0A08** | 1 | — |
| `CS::CSScaleformStep` | 0x3D04BF0 | 0x3362200 | 0 | 0x2BBD870 | 19 | … |
| `CS::CSWindowImp` | 0x3D0F540 | 0x3373490 | 0 | 0x2C011E8 | 3 | FD4::FD4Window |
| `CS::CSFlipperImp` (frame flipper/timing) | 0x3D0EF50 | 0x3372B08 | 0 | 0x2BFF358 | 1 | — |
| `CS::GameMan` | 0x3C8FD60 | 0x32E8778 | 0 | 0x2A82110 | 1 | — |
| `CS::CSChrPhysicsModule` | 0x3C83458 | 0x32D4270 | 0 | 0x2A3C890 | 2 | CSChrModuleBase |
| `CS::CSChrBehaviorModule` | 0x3C80950 | 0x32D0ED0 | 0 | 0x2A35D48 | 2 | CSChrModuleBase |
| `CS::CSChrTimeActModule` | 0x3C824B0 | 0x32D22A0 | 0 | 0x2A37670 | 2 | CSChrModuleBase |
| `CS::CSChrActionRequestModule` | 0x3C80670 | 0x32D09D8 | 0 | 0x2A32780 | 2 | CSChrModuleBase |
| `CS::CSHavokManImp` | 0x3CFD8C8 | 0x3355640 | 0 | 0x2B934E8 | 1 | — |
| `CS::CSRemoMan` (cutscenes) | 0x3CEF038 | 0x333D200 | 0 | 0x2B55EE0 | 25 | CSStepLocal<CSRemoMan>, … |
| `CS::CSSessionManagerImp` | 0x3CFF778 | 0x3358E08 | 0 | 0x2B9D1B8 | 2 | CSNRManagerListener |

Names from the brief that **do not exist** as RTTI classes in 2.7.1.0 (VERIFIED by exhaustive
grep of the 12,046 names): `FD4PadManager`, `GXDrawQueue` (and anything `*DrawQueue*`),
`CSCamera` as such (it is `CS::CSCameraImp`), `CSMenuMan` (`CS::CSMenuManImp`) and `CSFeMan`
(`CS::CSFeManImp`).

**Cross-check against fromsoftware-rs 2.7.0.0/2.7.1.0** (`crates/eldenring/src/rva/rva_ww.rs`; the
2.7.1.0 bundle only overrides `register_task`, `cs_phys_world_cast_ray` and three
`cs_ez_draw_draw_*`):
- **74/82 `_vmt` RVAs are identical to ours** (e.g. `player_ins_vmt 0x2a7fbb0`,
  `chr_cam_vmt 0x2a2aa08`, `cscam_vmt 0x2aa0a18`, `cspad_vmt 0x2be3a70`,
  `pad_device_vmt 0x30cd048`).
- The 8 misses are `cstask_vmt 0x329caf0`, `cstask_base_vmt 0x329d418`,
  `fd4task_base_vmt 0x3296d28`, `fd4res_cap_vmt 0x3296978`, `fd4file_cap_vmt 0x3297298`,
  `fd4param_repository_vmt 0x3298708`, `fd4param_res_cap_vmt 0x329b148` and
  `fd4res_rep_vmt 0x3299130`. **These vtables have no COL**: the qword before them is a code
  pointer, not a COL (VERIFIED). They are compiled without RTTI, so take them from
  fromsoftware-rs (UNVERIFIED) or via constructors.
- Function RVAs from their 2.7.1.0 bundle: `register_task 0xeb3e50`,
  `cs_phys_world_cast_ray 0xc71e00`, `cs_ez_draw_draw_line 0x2652640`, `…_triangle 0x2652390`,
  `…_wedge 0x2651f40`, `obtain_dl_user_input_manager 0x1328d0` and
  `cs_menu_man_imp_display_status_message 0x7671f0`. Each **starts exactly at a `.pdata`
  RUNTIME_FUNCTION begin** (VERIFIED existence; semantics UNVERIFIED).
- Data globals: `game_data_man 0x3d61f98`, `game_man 0x3d6d988`, `field_area_ptr 0x3d6d248` (in
  `.data`; UNVERIFIED semantics).

Selected vfunc slot lists (camera, pad, task, draw, window, graphics) are in
`data/er_vfuncs_selected.txt`. Example, `CS::CSCam` vtable `0x2AA0A18`: `[0] 0xb10670 [1] 0x76dbe0
[2] 0xb10580 [3] 0xb10540`.

### 2.7 Hooking implications for ER (host side)
- **Per-frame callback without patching Arxan code:** register a task through `CSTaskImp`
  (`register_task 0xeb3e50`, the fromsoftware-rs approach), or swap vptrs per instance (copy the
  vtable and replace the object's vptr). The latter avoids writing to `.rdata`.
- **Draw/overlay:** `CS::CSEzDraw` (vtable 0x2B814C0) plus the `cs_ez_draw_draw_*` RVAs give
  debug primitives. A D3D12 Present hook needs the DXGI swapchain vtable at runtime;
  `D3D12CreateDevice`'s caller is 0x1ea2730.
- **Input injection into ER:** the `DLUID::PadDevice` / `KeyboardDevice` / `MouseDevice` /
  `VirtualMultiDevice` vtables (51 slots each), or an IAT hook on XInputGetState (IAT slot
  `0x4c11ee4`, RW `.idata`). For PhantomRing the ER player's own input mostly has to be
  *suppressed* (the guest drives the character), so `CS::PadManipulator` (vtable 0x2A2E788,
  55 vfuncs) is the controller to override.
- **Camera:** `CS::ChrCam` / `CSPersCam` vtables. `CS::CSCameraImp` is the singleton.

### 2.8 How to load a native DLL into ER
- **Recommended: me3 `[[natives]]`.** VERIFIED pattern from
  `<old-project>/profiles/elden-ring/diamond-dogs-render-probe.me3`:
  `profileVersion = "v1"`, `savefile = "MGS_ER0000.sl2"`, `start_online = false`,
  `[[natives]] path = "…dll"`. Add `disable_arxan = true`. Launch it with
  `tools/me3/bin/me3 --windows-binaries-dir tools/me3/bin/win64 launch -p <profile>`.
- **Proxy DLLs that would technically load:**
  - Static imports, not KnownDLLs: `dinput8.dll`, `dxgi.dll`, `d3d12.dll`, `winmm.dll`,
    `winhttp.dll`, `xinput1_4.dll`.
  - App-local libraries that also ship in the game dir (don't replace them): `steam_api64.dll`,
    `bink2w64.dll`, `amd_ags_x64.dll`, `oo2core_6_win64.dll`.
  - Dynamic-load candidates (names appear in the binary as strings): `libscepad_x64.dll`,
    `xinput1_3.dll`, `xinput9_1_0.dll`, `dbghelp.dll`.

  **Don't use them.** Anything in the Game dir is also loaded by the normal Steam →
  `start_protected_game.exe` (EAC) path and would get the user flagged. The brief forbids EAC
  interaction. If Seamless Coop is installed (`ersc_launcher.exe`,
  `launch_elden_ring_seamlesscoop.exe`, `SeamlessCoop/`), don't touch it.
- The windows KnownDLLs list (UNVERIFIED; Windows 10/11 from memory) includes `ws2_32`,
  `setupapi` and `crypt32`, which therefore can't be proxied on Windows. Wine/Proton instead
  needs `WINEDLLOVERRIDES=name=n,b` for any app-dir override of a builtin (UNVERIFIED, standard
  practice).

---

## 3. MGSV – `<steam-library>/common/MGS_TPP/mgsvtpp.exe`

### 3.1 Identity (VERIFIED)
- size 166,517,760; **sha256 `085c2f82d1c963c40b3d2d55786661dfee2b18cbbf388a710c00fa76c5e9bb45`**
- FileVersion/ProductVersion **1.0.15.4**, InternalName/OriginalFilename `Tpp_main.exe`, Company
  Konami Digital Entertainment
- TimeDateStamp `0x6a4cb898` (2026-07-07 08:28:08 UTC), PDB `mgsvtpp.pdb`
- Linker **11.0** (VS2012); runtime imports are `MSVCR110.dll` and `MSVCP110.dll`
- ImageBase `0x140000000`, SizeOfImage `0xa01d000`, CheckSum 0
- DllCharacteristics **0x8120** = HIGH_ENTROPY_VA | NX | TS_AWARE. **There is no DYNAMIC_BASE and
  no BASERELOC directory, so the image always loads at 0x140000000.**
- **EntryPoint `0x69b5f80` in `.xdata`.** It saves every register to the stack
  (`lea rsp,[rsp-0x98]; mov [rsp+0x20],rax; …`), which is the shape of a VM/protector entry.
- **TLS:** directory at `0x255d060`, callback array at `0x9e78000` (`.00cfg`). Callbacks:
  `0x6cfc4ce` (.xdata, `jmp` + mutated code), `0x5aab920` (.xdata, mutated) and **`0xe200`**
  (.xtext, plain: `cmp edx,3; jne; call 0xe220` – an engine/CRT thread-detach callback).
- LoadConfig size 0x70, SecurityCookie VA `0x142b49330` (RVA 0x2b49330 in `.data2`; functions read
  it as `mov rax,[rip→0x2b49330]`).
- Export: `ExportProductMetadata` (1 export, in `.debug$P`).

### 3.2 Sections (VERIFIED)
| section | RVA | VSize | raw off | raw size | flags | entropy | role (INFERRED from data directories/contents) |
|---|---|---|---|---|---|---|---|
| `.xtext` | 0x1000 | 0x208c000 | 0x400 | 0x208c000 | R-X | 6.492 | original game code (plaintext) |
| `.edata` | 0x208d000 | 0x956000 | 0x208c400 | 0x956000 | R-- | 5.165 | original .rdata (IAT@start, DEBUG, TLS, LOAD_CONFIG, all strings) |
| `.data2` | 0x29e3000 | 0x2c1000 | 0x29e2400 | 0x176000 | RW- | 5.929 | original .data (TypeDescriptors, globals) |
| `.didata` | 0x2ca4000 | 0x19e000 | 0x2b58400 | 0x19e000 | R-- | 0.001 | zero-filled |
| `.bss` | 0x2e42000 | 0x1000 | 0x2cf6400 | 0x200 | RW- | 0 | |
| `.text` | 0x2e43000 | 0x9000 | 0x2cf6600 | 0x9000 | **R-- (not exec)** | 5.732 | ? |
| `.impdata` | 0x2e4c000 | 0x28000 | 0x2cff600 | 0x28000 | R-- | 7.846 | RESOURCE directory |
| `.trace` | 0x2e74000 | 0x8f000 | 0x2d27600 | 0x8f000 | R-- DISCARD | 4.857 | ? |
| `.xdata` | 0x2f03000 | 0x6f74df4 | 0x2db6600 | 0x6f74e00 | **RWX** | 6.66 | protector code (EP, TLS callbacks, moved functions) |
| `.00cfg` | 0x9e78000 | 0x40a8 | 0x9d2b400 | 0x4200 | R-- (CODE flag) | 5.375 | IMPORT directory, TLS callback array |
| `.xtls` | 0x9e7d000 | 0x308 | 0x9d2f600 | 0x400 | RW- | 2.938 | IAT of the extra lower-case imports |
| `.debug$P` | 0x9e7e000 | 0x100 | 0x9d2fa00 | 0x200 | R-- | 3.019 | EXPORT directory |
| `.ecode` | 0x9e7f000 | 0x19df04 | 0x9d2fc00 | 0x19e000 | R-- | 7.028 | EXCEPTION (.pdata, 141,317 entries) |

### 3.3 Protector (Denuvo; attribution UNVERIFIED, evidence VERIFIED)
- `DENUVO` bytes embedded in `.xdata` code at RVA 0x6f3fbe1 (file offset 0x6df31e1). The context
  `48 ?? "DO" "DENUVO" 48 …` is an immediate operand.
- The 117 MB RWX `.xdata` contains the entry point, two mutated TLS callbacks, and functions
  *moved out* of `.xtext`. Examples (VERIFIED):
  - The caller of `luaL_openlibs` (`jmp` at 0x37d6919).
  - The `TppShell` Lua library registration at 0x78338c0.
  - Several `RegisterClassExW`/`DefWindowProcW`/`LoadLibraryW` users.
- The second, lower-case import set has its IAT in `.xtls`: `advapi32.dll, gdi32.dll,
  kernel32.dll (26 incl. LoadLibraryExW, GetVolumeInformationW, GetComputerNameW),
  ntdll.dll (NtClose, NtCreateSection, NtMapViewOfSection, NtQuerySystemInformation,
  NtUnmapViewOfSection), shell32.dll, shlwapi.dll, user32.dll (41), wininet.dll (7),
  ws2_32.dll (htonl)`. This is typical of a licence/anti-tamper runtime: hardware fingerprinting
  and HTTP.
- **Implications:**
  - MGSV must keep running through Steam for licensing.
  - Hardware changes or frequent new Proton prefixes may consume Denuvo activations
    (UNVERIFIED general knowledge).
  - Inline hooks in `.xtext` game code are generally tolerated by Denuvo titles, but nothing that
    lives in `.xdata` is hookable in practice.
  - `.xtext` is **not encrypted at rest**, so static RVAs are valid at runtime (VERIFIED for static
    bytes only; runtime equality is UNVERIFIED).

### 3.4 Imports (VERIFIED; full lists in `data/mgsv_pe.md`)
Upper-case (original) set: `IMM32, steam_api64 (13), KERNEL32 (92), USER32 (56), ADVAPI32,
SHELL32, HID.DLL (12), DINPUT8 (1), XINPUT1_3 (1), MSACM32, MSVCP110 (75), MSVCR110 (191),
PSAPI, WINMM (3), SETUPAPI, dxgi (1), d3d11 (1), MFPlat, MFReadWrite, GDI32, ole32, OLEAUT32,
WS2_32 (30), IPHLPAPI, WINHTTP (12)`, plus the protector set above.

Watch-list:
- **dinput8:** `DirectInput8Create`. **xinput1_3:** **#2 = XInputGetState** only (VERIFIED
  ordinal mapping, see §2.4). XInputGetCapabilities and XInputSetState are resolved
  dynamically, see §3.5.
- **d3d11:** `D3D11CreateDevice`. **dxgi:** `CreateDXGIFactory1`. **winmm:**
  `timeGetTime, timeBeginPeriod, timeEndPeriod`.
- **user32 (game set `USER32.dll`, IAT in `.edata`):** **`RegisterRawInputDevices`,
  `GetRawInputData`**, `GetKeyboardState, MapVirtualKeyW, ToUnicode, ToAscii, GetCursorPos,
  SetCursorPos, ShowCursor, ScreenToClient, CreateWindowExW, RegisterClassExW, ShowWindow,
  GetMessageW, DispatchMessageW, TranslateMessage, SetWindowLongPtrW, CallWindowProcW,
  DefWindowProcW, GetForegroundWindow, SetForegroundWindow, SetFocus, SetActiveWindow,
  AttachThreadInput, IsIconic, IsWindowVisible, Get/SetWindowPlacement, MonitorFromWindow`.
  - The protector set `user32.dll` (IAT in `.xtls`) is a dialog/UI toolkit: `CreateWindowExA,
    ClientToScreen, SetCapture, ReleaseCapture, GetCapture, SetCursor, GetDlgItem,
    SendDlgItemMessageA`, clipboard, menus, `BeginPaint/EndPaint`. It is probably the
    protector's own activation/error window (INFERRED).
  - **Not imported:** `GetAsyncKeyState`, `ClipCursor`, `PeekMessageW` (VERIFIED absence).
- **kernel32 timing:** `QueryPerformanceCounter` (10 sites), `QueryPerformanceFrequency`,
  `GetTickCount`, `Sleep`, `SleepEx`, `SwitchToThread`. kernel32 also provides
  `CreateFileMappingW/MapViewOfFile` (IPC is available in-process).
- **steam_api64 (13, old flat API):** `SteamAPI_Init, SteamAPI_RunCallbacks,
  SteamAPI_RegisterCallback, SteamAPI_UnregisterCallback, SteamAPI_RegisterCallResult,
  SteamAPI_UnregisterCallResult, SteamUser, SteamFriends, SteamUtils, SteamApps,
  SteamUserStats, SteamRemoteStorage, SteamNetworking`. `steamclient64.dll` appears as a UTF-16
  string.
- DLL names that appear only as strings: `XInput1_4.dll`, `XInput1_3.dll`, `dtdata.dll` (UTF-16),
  `loadall.dll`, `win32u.dll`.

### 3.5 Key call sites (VERIFIED)
| what | site / function |
|---|---|
| Window class registration | `0x70880` (`RegisterClassExW` at 0x7098c), creation `0x705c0` (`CreateWindowExW` at 0x706d6) |
| **WndProc** | **`0x70430`** (address taken at 0x708f2). See the notes below this table |
| Message loop | `GetMessageW` in 0x71010 |
| **XInputGetState** | import thunk `0x32892c` (`jmp [0x208e070]`); the only caller is **`0x34f90`** |
| DirectInput8Create | import thunk `0x328926`; caller **`0x34ac0`** |
| Dynamic XInput | **`0x3f4f80`**: `LoadLibraryW("XInput1_4.dll")`, falling back to `"XInput1_3.dll"`, then `GetProcAddress("XInputGetCapabilities")` and `("XInputSetState")` |
| Raw-input mouse/camera | `0x53adf0` (2× `GetRawInputData`, `SetCursorPos`, `ShowCursor`); keyboard raw input lib `0x1a45540`/`0x1a45650` (`GetKeyboardState`) / `0x1a45a50` (`RegisterRawInputDevices`) |
| GetForegroundWindow | `0x24b5b0` (focus-steal helper: GetForegroundWindow → GetWindowThreadProcessId → AttachThreadInput…), `0x353300` (struct init) |
| D3D11CreateDevice / CreateDXGIFactory1 | import thunks `0x1a08c7a` / `0x1a08c74` |
| QueryPerformanceCounter | 0x23a90, 0x351370, 0x3639e0, 0x363e60, 0x394e40, 0x395fd0, 0x3ca5d0, … |

WndProc `0x70430` details:
- It calls two optional global hooks first: `[0x2b642c8]()` and
  `[0x2b642d0](hwnd,msg,wp,lp)` (`.data2`). These are engine "WndProc hook" slots that a DLL
  could fill (VERIFIED that they're called when non-null; purpose UNVERIFIED).
- It handles WM_CREATE→0x70ab0, WM_DESTROY→PostQuitMessage, WM_ERASEBKGND→1 and
  **WM_INPUT(0xFF)→0x1a45540**.
- It has **no WM_ACTIVATE, WM_ACTIVATEAPP or WM_KILLFOCUS case** (VERIFIED). Any
  background-pause logic is elsewhere, not in WndProc.

### 3.6 Embedded Lua 5.1.5 (VERIFIED version) and native bindings

**Runtime strings** (RVA / file offset → referencing function → identification; the code xrefs
are VERIFIED, and each identification is VERIFIED by matching the Lua 5.1.5 source):

| string | RVA | file off | referenced from → identified as |
|---|---|---|---|
| `$Lua: Lua 5.1.5 Copyright (C) 1994-2012 Lua.org, PUC-Rio $…` | 0x24b47a0 | 0x24b3ba0 | `lua_ident` data (no code ref) |
| `error in error handling` | 0x24b4870 | 0x24b3c70 | 0x1a13200 luaD_seterrorobj |
| `not enough memory` | 0x24b4858 | 0x24b3c58 | 0x1a13200, 0x1a16280 |
| `stack overflow` | 0x24b4888 | 0x24b3c88 | 0x1a128f0 (growCI) |
| `C stack overflow` | 0x24b48a0 | 0x24b3ca0 | 0x1a129b0 luaD_call, 0x1a13320 lua_resume |
| `cannot resume non-suspended coroutine` | 0x24b48b8 | 0x24b3cb8 | 0x1a13320 **lua_resume** |
| `attempt to yield across metamethod/C-call boundary` | 0x24b48e0 | 0x24b3ce0 | 0x1a13460 **lua_yield** |
| `bad argument #%d (%s)`, `calling '%s' on bad self (%s)`, `bad argument #%d to '%s' (%s)` | 0x24b4c00 / 0x24b4c20 / 0x24b4c40 | −0xC00 | 0x1a16d70 **luaL_argerror** |
| `%s expected, got %s` | 0x24b4c60 | 0x24b4060 | 0x1a18090 luaL_typerror; inlined in 0x1a16f90 luaL_checklstring, 0x1a171c0 luaL_checkudata, 0x1a18240 |
| `%s:%d: ` | 0x24b4c78 | 0x24b4078 | 0x1a18170 **luaL_where** |
| `stack overflow (%s)` | 0x24b4c98 | 0x24b4098 | 0x1a17140 luaL_checkstack |
| `_LOADED` | 0x24b4cc0 | 0x24b40c0 | 0x1a17c30 **luaI_openlib**, 0x1a35d10 ll_require, 0x1a35f50 ll_module, 0x1a364f0 luaopen_package |
| `name conflict for module '%s'` | 0x24b4cc8 | 0x24b40c8 | luaI_openlib, ll_module |
| `=stdin` | 0x24b4cfc | 0x24b40fc | 0x1a17910 **luaL_loadfile** |
| `PANIC: unprotected error in call to Lua API (%s)\n` | 0x24b4d18 | 0x24b4118 | 0x1a18200 panic (address taken by 0x1a17bf0 **luaL_newstate**) |
| `attempt to %s %s '%s' (a %s value)` / `attempt to %s a %s value` | 0x24b5f18 / 0x24b5f40 | −0xC00 | 0x1a20af0 luaG_typeerror |
| `attempt to compare two %s values` / `attempt to compare %s with %s` | 0x24b5f88 / 0x24b5fb0 | −0xC00 | 0x1a20a60 luaG_ordererror |
| `%s:%d: %s` | 0x24b5fd0 | 0x24b53d0 | 0x1a20130 (luaG_runerror/addinfo), 0x1a2ceb0, 0x1a2d520 (lexer) |
| `invalid key to 'next'` | 0x24b6008 | 0x24b5408 | 0x1a23720 luaH_next |
| `table index is nil` | 0x24b6030 | 0x24b5430 | 0x1a23cb0, 0x1a24260 (luaH_newkey/set) |
| `loop in gettable` / `loop in settable` | 0x24b6050 / 0x24b6068 | −0xC00 | 0x1a26d70 luaV_gettable / 0x1a26f20 luaV_settable |
| `cannot use '...' outside a vararg function` | 0x24b6468 | 0x24b5868 | 0x1a2a2e0 (parser) |
| `%s: %s in precompiled chunk` | 0x24b6550 | 0x24b5950 | 0x1a2aca0 (lundump) – binary chunks supported |
| `=(load)` | 0x24b6f38 | 0x24b6338 | 0x1a2e3c0 luaB_load |
| `too many results to unpack` | 0x24b6f58 | 0x24b6358 | 0x1a2e520 luaB_unpack |
| `cannot resume %s coroutine` | 0x24b6ff0 | 0x24b63f0 | 0x1a2eac0 auxresume |
| `Lua 5.1` | 0x24b7048 | 0x24b6448 | 0x1a2ebb0 **base_open** (`_VERSION`) |
| `attempt to use a closed file` | 0x24b7448 | 0x24b6848 | liolib (0x1a2ff20 …) |
| `stack traceback:` | 0x24b8008 | 0x24b7408 | 0x1a35610 db_errorfb (`debug.traceback`) |
| `luaopen_%s` | 0x24b81e8 | 0x24b75e8 | 0x1a36790 (loadlib C loader) |
| `LUA_PATH`, `.\?.lua;!\lua\?.lua;…` | 0x24b8388, 0x24b8348 | −0xC00 | 0x1a364f0 luaopen_package |

There are **no `lua_*`/`luaL_*` symbol-name strings** in the binary (VERIFIED). `print`,
`Fox.Log`, `Fox.Warning`, `Fox.Error`, `debug.debug` and others all point at the no-op stub
`0x1c658e0` (stripped in retail).

**Lua C API map.** Selected entries are below; all 115 are in `data/mgsv_lua_c_api.tsv`, with
VA = 0x140000000 + RVA.
- **VERIFIED** entries come from Lua 5.1.5 source-pattern matching of `luaB_pcall 0x1a2e6c0`,
  `luaB_loadstring 0x1a2e2e0`, `luaB_error 0x1a2de10`, `luaB_dofile 0x1a2e450`,
  `base_open 0x1a2ebb0`, `luaI_openlib 0x1a17c30` and `luaL_newstate 0x1a17bf0`, plus the
  strings above.
- **INFERRED** entries come from a **277-slot Lua-API dispatch table at `.edata 0x23052f0`** (no
  RTTI). Slots 0–132 are `_purecall 0x1a05976`. Slots 134–258 are thunks to lua_* and luaL_*
  functions **in alphabetical order**. The order is anchored by 30+ VERIFIED functions, and
  every macro slot resolves to the right real function (newtable→createtable, pop→settop,
  pushcfunction→pushcclosure, setglobal→setfield, tostring→tolstring,
  checkint/checklong→checkinteger, optint→optinteger).
- The table is referenced by `lea` at 0xbf26b0, 0xbf2706 (inside slot-133 function 0xbf2700,
  likely its destructor), 0xbf2746 (in 0xbf2730) and 0x207c63c. A statically-initialised object
  whose vptr is this table sits at `.data2 0x2a97488`. This looks like Fox's Lua-API interface
  singleton (purpose UNVERIFIED).

| function | RVA | conf. | | function | RVA | conf. |
|---|---|---|---|---|---|---|
| lua_pcall | **0x1a116c0** | V | | luaL_loadbuffer | **0x1a178e0** | V |
| lua_call | 0x1a10cc0 | V | | luaL_loadfile | 0x1a17910 | V (Fox's API table maps loadfile → **0xa23b10**, INFERRED override) |
| lua_gettop | 0x1a112e0 | V | | luaL_loadstring | 0x1a17b30 | I |
| lua_settop | 0x1a11f70 | V | | luaL_newstate | 0x1a17bf0 | V |
| lua_getfield | **0x1a111e0** | V | | lua_newstate | 0x1a16500 (alloc l_alloc 0x1a16bc0) | V |
| lua_setfield | **0x1a11e00** | V | | luaL_openlibs | 0x1a16980 (lualibs[] at 0x24b4b40) | V |
| lua_pushcclosure | **0x1a11770** | V | | luaL_register | **0x1a18080** (`xor r9d,r9d; jmp luaI_openlib`) | V |
| lua_pushlstring | 0x1a118c0 | V | | luaI_openlib | 0x1a17c30 | V |
| lua_pushstring | 0x1a11970 | I | | luaL_findtable | 0x1a172e0 | V |
| lua_pushnumber / pushinteger | 0x1a11950 / 0x1a11880 | I | | luaL_error | 0x1a17280 | V |
| lua_pushnil / pushboolean / pushvalue | 0x1a11930 / 0x1a11750 / 0x1a119d0 | V | | luaL_checklstring | 0x1a16f90 | V |
| lua_tolstring | 0x1a12150 | V | | luaL_checknumber / checkinteger | 0x1a17020 / 0x1a16f40 | I |
| lua_tonumber / tointeger / toboolean | 0x1a121f0 / 0x1a12120 / 0x1a120c0 | I | | luaL_optlstring / optinteger | 0x1a17e20 / 0x1a17db0 | V |
| lua_type | 0x1a12300 | V | | luaL_where / argerror / typerror | 0x1a18170 / 0x1a16d70 / 0x1a18090 | V |
| lua_insert / remove | 0x1a11390 / 0x1a11c20 | V | | luaL_ref / unref | 0x1a17fc0 / 0x1a180f0 | I |
| lua_createtable / setmetatable | 0x1a10e80 / 0x1a11e80 | V | | luaL_newmetatable / checkudata | 0x1a17b70 (I) / 0x1a171c0 (V) | |
| lua_gettable / settable / rawget / rawset / rawgeti / rawseti | 0x1a112b0 / 0x1a11f40 / 0x1a11aa0 / 0x1a11b20 / 0x1a11ae0 / 0x1a11ba0 | I | | lua_newthread / newuserdata / next | 0x1a11550 / 0x1a11590 / 0x1a11600 | I |
| lua_error / concat / atpanic / checkstack / getfenv | 0x1a10fa0 / 0x1a10db0 / 0x1a10ca0 / 0x1a10d10 / 0x1a11160 | V | | lua_resume / yield / getstack / getinfo / pushfstring | 0x1a13320 / 0x1a13460 / 0x1a20d40 / 0x1a20be0 / 0x1a11830 | V |

The `lua_State` layout is stock Lua 5.1 x64: `top +0x10`, `base +0x18`, `ci +0x28`,
`stack_last +0x38`, TValue 16 bytes (VERIFIED from `lua_checkstack 0x1a10d10` and
`lua_getfenv` code).

**Native library registration.** `tools/lua_reg_scan.py`, output in `data/mgsv_lua_libs.*` (VERIFIED
by emulation):
- `luaL_register` has **124 call sites**: 12 stock ones (`_G`, coroutine, table, io, os, string,
  math, debug, package, bit…) and the rest via the **Fox wrapper `0x6b8c0(L, name, luaL_Reg*)`**.
  The wrapper toggles a Fox "protected env" flag with 0x1a15b40/0x1a15b70 (that interpretation is
  UNVERIFIED) and then calls `luaL_register`.
- **`Player` library** (172 functions, registered by `0x9d6ae0`, call at 0x9d7d76), for example:
  - `SetPadMask=0x9df360`, `ResetPadMask=0x9dc510`
  - `RequestToSetCameraRotation=0x9daf90`, `RequestToPlayCameraNonAnimation=0x9d9d40`
  - `SetAroundCameraManualMode=0x9dcd80`, `…ManualModeParams=0x9dce00`
  - `RequestToMoveToPosition=0x9d9430`, `RequestToSetTargetStance=0x9db170`
  - `ChangeEquip=0x9d0d60`, `SetCurrentSlot=0x9ddd30`, `SetEquipState=0x9de130`
  - `RequestToPlayDirectMotion=0x9da7d0`, `IsDead=0x9d6020`, `GetPlayerType=0x9d30b0`
  - `SetPause=0x9df730`, `UnsetPause=0x9e17d0`
  - `ChangeLifeMaxValue=0x9d1040`, `SetInfiniteAmmoFromScript=0x9dec00`
  - `CopyPartsInfoFromLocalPlayer=0x9d13f0`, `SetPartsInfoAtInstanceIndex=0x9df530`
- `PlayerInfo` (0x554b80): `GetLocalPlayerIndex=0x554c20`, `AndCheckStatus=0x554520`,
  `OrCheckStatus=0x554c60`.
- Other big ones: `TppUiCommand` 254, `TppBuddyService` 80, `TppRadioCommand` 70, `Gimmick` 58,
  `SubtitlesCommand` 32, `TppSoldierFace` 32, `foxmath` 25, `TppServerManager` 23, `DemoDaemon`
  22, `Nav` 18, `MotherBaseStage` 15, `HighSpeedCamera` 3, `Gr` 5 (`DBGPrint=0x1fec60`, …).
- **Fox binder helper `0x1a15690(tbl, "name", cfunc, nup, …)`** has 678 sites; the getter /
  creator `0x1a158e0(L,"GameObject",…)` is used before it. Examples:
  - **`GameObject`** (registered in 0xbf9e30): `GetGameObjectId=0xbf9fe0`,
    `GetGameObjectIdByIndex=0xbfa110`, `CreateGameObjectId=0xbf9cf0`,
    `GetTypeIndex=0xbfa1d0`, `GetTypeIndexWithTypeName=0xbfa240`,
    `DoesGameObjectExistWithTypeName=0xbf9de0`, **`SendCommand=0xbfa380`**,
    **`SendMessage=0xbfa510`**, `NULL_ID`.
  - **`Fox`**: `StrCode32=0x178910`, `StrCode=0x178980`, `PathFileNameCode32=0x179b40`,
    `GetPlatformName=0x179c30`, `Require=0x17a870`, `SetFrameRateMode=0x17aa70`, … (45 functions).
  - **`Time`**: `GetDeltaGameTime=0x184950`, `GetGameTimeRate=0x1849e0`,
    `GetFrameTime=0x1849b0`, `GetRawElapsedTimeSinceStartUp=0x184a10`,
    `GetFrameIndex=0x184980`.
  - Second binder `0xf79d0` (379 sites: Fox entity-class methods `GetInstance`,
    `ReloadRequest`, …).
- **Warping/teleport:** there is **no plain `"Warp"` string** (VERIFIED). Lua passes command IDs as
  strings that are hashed (`Fox.StrCode32`) and compared to constants inside the
  `GameObject.SendCommand` handlers. In-game scripts warp the player with
  `GameObject.SendCommand({type="TppPlayer2",index=0},{id="Warp",…})` (UNVERIFIED, from mod
  knowledge). The candidate hash function is `0x1d920`: static initialisers such as `0x1fffda0`
  call `0x1d920("TppPlayer2", 2)` and store the result to `.data2 0x2c1b8f8` (semantics
  UNVERIFIED).
- **`vars` / player state** (string RVA → user):
  - `vars` 0x228b0bc (file 0x228a4bc), `playerPosX/Y/Z` 0x228b4c0/0x228b4d0/0x228b4e0,
    `playerCameraPosition` 0x228b620, `playerCameraRotation` 0x228b638 and `TppScriptVars`
    0x228fbd8 → all in the 80 KB declaration function **`0x996b70`–`0x9aa55b`** (script-vars
    schema).
  - `gvars` 0x22ab25c / `svars` 0x22ab264 → 0xa221d0, 0xa22e20 (also data slots 0x2a7ec30 and
    0x2a7ec38).
  - `PlayerVars` / `PlayerPad` / `PlayerCamera` enums → 0x9d3630.
  - `S_DISABLE_PLAYER_PAD` 0x2191b18 → 0x53f410.
  - Pad constants `PAD_A…PAD_AXIS_RT` at 0x2155058–0x21551e8, `MainGamePad` 0x2158dd8,
    `GkPad` 0x21537bc (→0x435920), `PadMapping` 0x2158838 (→0x44fc30), `GetPosition` 0x24ebc48
    (→0x1bb85e0, 0x1c3ccc0).
- **Fox script bootstrap:** `0x63c60` references `init.lua` (0x20b2408) and `start.lua`
  (0x20b2418). The chunk loader `0x6bc10` builds a `@%s` chunkname and calls `lua_tolstring` and
  0x1a18520 (which calls `luaL_loadbuffer`). `0x79600` loads `Fox/Scripts/Classes/%s.lua`.
  These are the natural places to inject our own Lua or to grab the main `lua_State*`. The rcx of
  `0x6b8c0` / `0x6bc10` is the `lua_State*` (VERIFIED for 0x6b8c0, which forwards rcx as L to
  `luaL_register`).
- Stock `io`, `os` (`execute`, `getenv`, …) and `package.loadlib` (0x1a35a40, which uses
  LoadLibraryA/GetProcAddress in 0x1a36300) are compiled in. Whether Fox's game environment
  exposes them at runtime is UNVERIFIED. If it does, a pure-Lua bridge (file or `loadlib`) would
  be possible.

### 3.7 RTTI in MGSV (VERIFIED absent)
Only 16 TypeDescriptors exist (`data/mgsv_1.0.15.4_rtti_vtables.tsv`), and only 5 have vtables:
`std::error_category` and its 3 subclasses, plus `type_info`. The other TDs are exception types:
`fox::Exception` (TD 0x29E3C30), `fox::LuaOutsideProtectedEnvErrorException` (0x29E3BF0),
`std::bad_alloc`, `std::bad_cast`, `std::exception` and 6 lambdas. **Fox Engine is compiled
with /GR-.** `_purecall` (0x1a05976) is referenced by 11,591 vtable slots, so vtables exist but
are anonymous.

Ways to name MGSV vtables:
- (a) Constructor stores (`lea rax,[vtbl]; mov [rcx],rax`) combined with Fox reflection names.
- (b) The Lua C functions above, which reach the engine objects (e.g. Player lib functions →
  player controller).
- (c) Runtime walking from Lua: `GameObject.GetGameObjectId("Player")` or similar
  (UNVERIFIED API usage).

### 3.8 How to load a native DLL into MGSV (no me3 equivalent)
| proxy | evidence (VERIFIED) | notes |
|---|---|---|
| **`dinput8.dll`** | static import `DINPUT8.dll!DirectInput8Create` (only 1 function); string `dinput8.dll` | Simplest. Forward `DirectInput8Create` to the system DLL. Called from 0x34ac0 |
| **`xinput1_3.dll`** | static import by **ordinal 2 only**; `0x3f4f80` also loads `XInput1_4.dll` first, then `XInput1_3.dll`, and `GetProcAddress`es XInputGetCapabilities and XInputSetState | Best fit for PhantomRing input injection: export ordinal 2 plus the names. MGSV may also pick up xinput1_4 (also proxyable) |
| `winmm.dll` | static import, 3 functions | Trivial forwarder, loaded at startup |
| `dxgi.dll` / `d3d11.dll` | static, 1 function each | Under Proton these are DXVK. Overriding them conflicts with DXVK, so avoid |
| `hid.dll`, `winhttp.dll`, `wininet.dll`, `iphlpapi.dll`, `msacm32.dll`, `mfplat.dll`, `mfreadwrite.dll` | static imports | Possible but more exports to forward |
| `msvcr110.dll` / `msvcp110.dll` | static | Don't proxy the CRT |
| `steam_api64.dll` | app-local file present | Don't replace (licensing) |

- DLL load order is: static-import DllMain runs **before** the protector's TLS callbacks and
  entry point (standard loader order, UNVERIFIED for this protector). So defer real work to a
  thread or to the first `DirectInput8Create`/`XInputGetState` call.
- Under Proton (MGSV prefix `compatdata/287700`) the user would need the Steam launch option
  `WINEDLLOVERRIDES="dinput8=n,b" %command%` (or `xinput1_3=n,b`). This is UNVERIFIED but
  standard. Changing Steam launch options is the user's call; we must not edit Steam config.

---

## 4. Implications for PhantomRing (summary for builders)

1. **ER plugin (host):**
   - Load it with me3 `[[natives]]` and `disable_arxan = true`.
   - Use ASLR-relative RVAs from §2.6 / §2.5 (exe 2.7.1.0 only; check the sha256 or
     FileVersion at startup).
   - Prefer vtable- or task-based hooks: `register_task 0xeb3e50`, `CS::PadManipulator`
     0x2A2E788, `CS::ChrCam` 0x2A2AA08, `CS::PlayerIns` 0x2A7FBB0, `CS::CSEzDraw` 0x2B814C0.
     fromsoftware-rs already models most of these structures for 2.7.1.0.
2. **MGSV plugin (guest):**
   - Load it with a `dinput8.dll` or `xinput1_3.dll` proxy. Use absolute VAs (no ASLR).
   - Drive the game through **Lua**: capture `L` from the `0x6b8c0` wrapper, or hook `lua_pcall
     0x1a116c0` / `luaL_loadbuffer 0x1a178e0`. Then use `luaL_loadbuffer` + `lua_pcall` to run
     our own scripts that call the `Player.*`, `GameObject.SendCommand` and `vars.*` APIs.
   - Read the player position and camera from `vars.playerPosX/Y/Z` and
     `vars.playerCameraRotation` (names VERIFIED as strings; runtime semantics UNVERIFIED).
   - Feed it input by hooking `XInputGetState` (IAT slot `0x208e070`, call site 0x34f90).
3. **Background/hidden operation of MGSV:**
   - WndProc has no focus-loss handling. Focus checks happen through `GetForegroundWindow`
     (0x24b5b0, 0x353300) and raw-input/mouse-trap code (0x53adf0, `S_DISABLE_MOUSE_TRAP`
     strings). These are the candidates to neutralise if MGSV stops updating when hidden; that
     behaviour itself is UNVERIFIED and needs a runtime test.
   - The WndProc hook slots `0x2b642c8` and `0x2b642d0` let us observe messages without
     patching code.

## 5. Open questions / next steps
- Runtime confirmation (needs the games running, not done here): the MGSV main `lua_State*`
  global, whether Fox's sandbox exposes `io`/`package`, and the StrCode32 algorithm (call
  `Fox.StrCode32` 0x178910 at runtime, or reimplement it and validate against the 0x1d920 call
  sites).
- MGSV player object: trace `Player.*` C functions (e.g. `IsDead 0x9d6020`,
  `RequestToMoveToPosition 0x9d9430`) to the player controller singleton and its position
  fields.
- ER: verify the semantics of the fromsoftware-rs globals (`world_chr_man`, `CSCamera` pointer) in
  2.7.1.0. They are not in their RVA bundle (they use AOB/singleton discovery); check
  `elden-ring-ct-src` AOBs against these bytes.
- The Denuvo activation and launch flow for running MGSV hidden alongside ER under Proton is
  untested.

## 6. Reproduction
```sh
T=tools
ER="<steam-library>/common/ELDEN RING/Game/eldenring.exe"
MG=<steam-library>/common/MGS_TPP/mgsvtpp.exe
python3 $T/pe_scout.py "$ER" --md er_pe.md --json er_pe.json
python3 $T/pe_scout.py "$MG" --md mgsv_pe.md --json mgsv_pe.json
python3 $T/rtti_vtables.py "$ER" --tsv er_rtti.tsv --grep '^CS::PlayerIns$' '^CS::ChrCam$' --dump-vfuncs '^CS::CSCam$'
python3 $T/rtti_vtables.py "$MG" --tsv mgsv_rtti.tsv
python3 $T/str_xref.py "$MG" -p '\$Lua' -p '^attempt to ' -p 'stack traceback' -e playerPosX -e SetPadMask --tsv x.tsv
python3 $T/lua_reg_scan.py "$MG" --tsv libs.tsv --md libs.md --libs Player,PlayerInfo --binders binders.tsv
python3 $T/iat_xref.py "$MG" "xinput1_3.dll!#2" DirectInput8Create RegisterClassExW GetRawInputData
python3 $T/iat_xref.py "$ER" "xinput1_4.dll!#2" DirectInput8Create "d3d12.dll!#101" RegisterClassExW
python3 $T/addr_xref.py "$MG" 1a18200 1a116c0
python3 $T/disasm.py "$MG" 9d6ae0 --func
```
