#!/usr/bin/env python3
"""pe_scout.py - read-only PE32+ scouting report for hooking decisions.

Usage:
  python3 pe_scout.py <exe> [--json out.json] [--md out.md] [--no-disasm]

Prints (markdown) / writes (json):
  * sha256, size, TimeDateStamp, ImageBase, SizeOfImage, entry point (+section),
    subsystem, DllCharacteristics, linker version, PDB (CodeView) path, Rich header
  * every section: name, RVA, VSize, raw offset/size, characteristics, Shannon entropy
  * TLS directory + callbacks (and which section each lives in)
  * Load config: GuardFlags (CFG), security cookie
  * packer/protector heuristics (SteamStub .bind, Arxan-style, high-entropy code, odd sections)
  * full import table (DLL -> names/ordinals), delay imports, exports
  * a watch-list of input/window/graphics/timing imports
  * DLL names referenced as strings (ASCII + UTF-16) = dynamic LoadLibrary candidates
  * proxy-DLL viability table

Read-only: never writes next to the target.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pefile  # noqa: E402

from pe_common import (PEImage, decode_scn_chars, entropy, hexs, load_capstone,  # noqa: E402
                       sha256_file)

WATCH = {
    "dinput8.dll": ["DirectInput8Create"],
    "xinput": None,  # any xinput*
    "d3d11.dll": None,
    "d3d12.dll": None,
    "dxgi.dll": None,
    "winmm.dll": None,
    "steam_api64.dll": None,
    "user32.dll": ["GetRawInputData", "GetRawInputBuffer", "RegisterRawInputDevices", "GetRawInputDeviceList",
                   "GetRawInputDeviceInfoA", "GetRawInputDeviceInfoW",
                   "GetAsyncKeyState", "GetKeyState", "GetKeyboardState", "SetCursorPos", "GetCursorPos",
                   "ClipCursor", "ShowCursor", "SetCursor", "ShowWindow", "CreateWindowExW", "CreateWindowExA",
                   "RegisterClassExW", "RegisterClassExA", "RegisterClassW", "PeekMessageW", "PeekMessageA",
                   "GetMessageW", "DispatchMessageW", "TranslateMessage", "SetWindowLongPtrW", "SetWindowPos",
                   "MoveWindow", "GetForegroundWindow", "SetForegroundWindow", "SetFocus", "GetFocus",
                   "MapVirtualKeyW", "MapVirtualKeyA", "ToUnicode", "GetClientRect", "ClientToScreen",
                   "ScreenToClient", "SetCapture", "ReleaseCapture", "DefWindowProcW", "CallWindowProcW"],
    "kernel32.dll": ["QueryPerformanceCounter", "QueryPerformanceFrequency", "GetTickCount", "GetTickCount64",
                     "Sleep", "SleepEx", "CreateFileMappingW", "CreateFileMappingA", "OpenFileMappingW",
                     "MapViewOfFile", "LoadLibraryA", "LoadLibraryW", "LoadLibraryExA", "LoadLibraryExW",
                     "GetProcAddress", "CreateThread", "IsDebuggerPresent", "CheckRemoteDebuggerPresent",
                     "VirtualProtect", "VirtualAlloc", "GetModuleHandleA", "GetModuleHandleW",
                     "SetUnhandledExceptionFilter", "AddVectoredExceptionHandler", "OutputDebugStringA",
                     "OutputDebugStringW", "CreateEventW", "SetEvent", "WaitForSingleObject"],
}

PROXY_CANDIDATES = [
    "dinput8.dll", "dxgi.dll", "d3d11.dll", "d3d12.dll", "version.dll", "winmm.dll", "winhttp.dll",
    "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll", "dsound.dll", "d3d9.dll", "wininet.dll",
    "hid.dll", "dbghelp.dll", "msvcp140.dll", "vcruntime140.dll", "steam_api64.dll", "bink2w64.dll",
    "amd_ags_x64.dll", "oo2core_6_win64.dll", "dwmapi.dll", "uxtheme.dll", "iphlpapi.dll",
    "ws2_32.dll", "setupapi.dll", "cfgmgr32.dll", "powrprof.dll", "dbgcore.dll", "secur32.dll",
    "crypt32.dll", "bcrypt.dll", "wtsapi32.dll", "mswsock.dll", "xaudio2_7.dll", "xaudio2_9.dll",
    "x3daudio1_7.dll", "dinput.dll", "opengl32.dll", "nvapi64.dll", "atiadlxx.dll", "binkw64.dll",
]

# Windows 10/11 KnownDLLs (HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs) -- UNVERIFIED (memory).
KNOWN_DLLS = {
    "advapi32.dll", "clbcatq.dll", "combase.dll", "comdlg32.dll", "coml2.dll", "difxapi.dll", "gdi32.dll",
    "gdiplus.dll", "imagehlp.dll", "imm32.dll", "kernel32.dll", "msctf.dll", "msvcrt.dll", "normaliz.dll",
    "nsi.dll", "ole32.dll", "oleaut32.dll", "psapi.dll", "rpcrt4.dll", "sechost.dll", "setupapi.dll",
    "shcore.dll", "shell32.dll", "shlwapi.dll", "user32.dll", "wldap32.dll", "wow64.dll", "ws2_32.dll",
    "ntdll.dll", "kernelbase.dll", "ucrtbase.dll", "win32u.dll", "gdi32full.dll", "msvcp_win.dll",
    "bcryptprimitives.dll", "cfgmgr32.dll", "crypt32.dll", "wintrust.dll", "msasn1.dll",
}


def fmt_ts(ts: int) -> str:
    try:
        return dt.datetime.fromtimestamp(ts, dt.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    except Exception:
        return "?"


def dll_strings(img: PEImage):
    """DLL file names appearing as ASCII or UTF-16LE strings anywhere in the mapped image."""
    found: dict[str, list] = {}
    buf = bytes(img.image)
    for m in re.finditer(rb"[A-Za-z0-9_\-\.]{2,64}\.(?:dll|DLL|Dll)(?=\0)", buf):
        name = m.group(0).decode().lower()
        found.setdefault(name, []).append(("ascii", m.start()))
    for m in re.finditer(rb"(?:[A-Za-z0-9_\-\.]\0){2,64}\.\0(?:[dD]\0[lL]\0[lL]\0)(?=\0\0)", buf):
        name = m.group(0).decode("utf-16le").lower()
        found.setdefault(name, []).append(("utf16", m.start()))
    return found


def scout(path: str, disasm: bool = True) -> dict:
    r: dict = {"path": path}
    r["file_size"] = os.path.getsize(path)
    r["sha256"] = sha256_file(path)
    img = PEImage(path)
    pe = img.pe
    fh, oh = pe.FILE_HEADER, pe.OPTIONAL_HEADER
    r["TimeDateStamp"] = fh.TimeDateStamp
    r["TimeDateStamp_utc"] = fmt_ts(fh.TimeDateStamp)
    r["Machine"] = hex(fh.Machine)
    r["Characteristics"] = hex(fh.Characteristics)
    r["ImageBase"] = hex(oh.ImageBase)
    r["SizeOfImage"] = oh.SizeOfImage
    r["SizeOfHeaders"] = oh.SizeOfHeaders
    r["CheckSum"] = hex(oh.CheckSum)
    r["Subsystem"] = oh.Subsystem
    r["DllCharacteristics"] = hex(oh.DllCharacteristics)
    dc = oh.DllCharacteristics
    r["DllCharacteristics_decoded"] = [n for b, n in [(0x20, "HIGH_ENTROPY_VA"), (0x40, "DYNAMIC_BASE(ASLR)"),
                                                       (0x80, "FORCE_INTEGRITY"), (0x100, "NX_COMPAT"),
                                                       (0x200, "NO_ISOLATION"), (0x400, "NO_SEH"),
                                                       (0x800, "NO_BIND"), (0x1000, "APPCONTAINER"),
                                                       (0x2000, "WDM_DRIVER"), (0x4000, "GUARD_CF"),
                                                       (0x8000, "TERMINAL_SERVER_AWARE")] if dc & b]
    r["LinkerVersion"] = f"{oh.MajorLinkerVersion}.{oh.MinorLinkerVersion}"
    r["OSVersion"] = f"{oh.MajorOperatingSystemVersion}.{oh.MinorOperatingSystemVersion}"
    ep = oh.AddressOfEntryPoint
    eps = img.section_of(ep)
    r["EntryPoint_rva"] = hex(ep)
    r["EntryPoint_section"] = eps.name if eps else None

    # sections
    secs = []
    for s in img.sections:
        with open(path, "rb") as f:
            f.seek(s.raw_off)
            data = f.read(s.raw_size) if s.raw_size else b""
        secs.append({"name": s.name, "rva": hex(s.rva), "vsize": s.vsize, "raw_off": hex(s.raw_off),
                     "raw_size": s.raw_size, "chars": hex(s.chars), "flags": decode_scn_chars(s.chars),
                     "entropy": round(entropy(data), 3) if data else 0.0})
    r["sections"] = secs

    # data directories
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_TLS"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DEBUG"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"],
    ])
    # version resource
    r["version"] = {}
    for fi in getattr(pe, "FileInfo", []) or []:
        for e in fi:
            for st in getattr(e, "StringTable", []) or []:
                for k, v in st.entries.items():
                    r["version"][k.decode("latin-1")] = v.decode("utf-8", "replace")
    if hasattr(pe, "VS_FIXEDFILEINFO"):
        f = pe.VS_FIXEDFILEINFO[0]
        r["version"]["Fixed.FileVersion"] = (f"{f.FileVersionMS >> 16}.{f.FileVersionMS & 0xFFFF}."
                                             f"{f.FileVersionLS >> 16}.{f.FileVersionLS & 0xFFFF}")
    r["data_directories"] = {}
    for i, d in enumerate(oh.DATA_DIRECTORY):
        if d.Size:
            nm = d.name.replace("IMAGE_DIRECTORY_ENTRY_", "")
            if nm == "SECURITY":  # VirtualAddress is a FILE OFFSET for the certificate table
                r["data_directories"][nm] = {"rva": hex(d.VirtualAddress), "size": d.Size,
                                             "section": "file-offset(Authenticode)"}
                continue
            r["data_directories"][nm] = \
                {"rva": hex(d.VirtualAddress), "size": d.Size,
                 "section": (img.section_of(d.VirtualAddress).name if img.section_of(d.VirtualAddress) else None)}

    # TLS
    tls = getattr(pe, "DIRECTORY_ENTRY_TLS", None)
    r["tls"] = None
    if tls is not None:
        t = tls.struct
        cbs = []
        if t.AddressOfCallBacks:
            rva = t.AddressOfCallBacks - img.base
            for i in range(64):
                va = img.u64(rva + 8 * i)
                if va == 0:
                    break
                cr = va - img.base
                s = img.section_of(cr)
                cbs.append({"rva": hex(cr), "section": s.name if s else None})
        r["tls"] = {"StartAddressOfRawData": hex(t.StartAddressOfRawData),
                    "EndAddressOfRawData": hex(t.EndAddressOfRawData),
                    "AddressOfIndex_rva": hex(t.AddressOfIndex - img.base) if t.AddressOfIndex else None,
                    "AddressOfCallBacks_rva": hex(t.AddressOfCallBacks - img.base) if t.AddressOfCallBacks else None,
                    "callbacks": cbs}

    # debug / PDB
    r["debug"] = []
    for d in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []) or []:
        e = {"type": d.struct.Type, "TimeDateStamp": d.struct.TimeDateStamp,
             "TimeDateStamp_utc": fmt_ts(d.struct.TimeDateStamp)}
        if d.entry is not None and hasattr(d.entry, "PdbFileName"):
            e["pdb"] = d.entry.PdbFileName.rstrip(b"\0").decode("latin-1", "replace")
            try:
                g = d.entry
                e["guid"] = f"{g.Signature_Data1:08X}-{g.Signature_Data2:04X}-{g.Signature_Data3:04X}-" + \
                            bytes(g.Signature_Data4).hex().upper() if hasattr(g, "Signature_Data1") else None
                e["age"] = getattr(g, "Age", None)
            except Exception:
                pass
        r["debug"].append(e)

    # load config
    lc = getattr(pe, "DIRECTORY_ENTRY_LOAD_CONFIG", None)
    r["load_config"] = None
    if lc is not None:
        s = lc.struct
        r["load_config"] = {k: (hex(getattr(s, k)) if isinstance(getattr(s, k, None), int) else None)
                            for k in ("Size", "SecurityCookie", "GuardCFCheckFunctionPointer",
                                      "GuardCFDispatchFunctionPointer", "GuardCFFunctionTable",
                                      "GuardCFFunctionCount", "GuardFlags") if hasattr(s, k)}

    # rich header
    try:
        rh = pe.parse_rich_header()
        if rh:
            vals = rh.get("values", [])
            entries = []
            for i in range(0, len(vals), 2):
                comp, cnt = vals[i], vals[i + 1]
                entries.append({"prodid": comp >> 16, "build": comp & 0xFFFF, "count": cnt})
            r["rich"] = entries
    except Exception:
        r["rich"] = None

    # imports
    imps = {}
    for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []) or []:
        dll = e.dll.decode("latin-1")
        names = []
        for i in e.imports:
            if i.name:
                names.append(i.name.decode("latin-1"))
            else:
                names.append(f"#ord{i.ordinal}")
        key = dll
        while key in imps:  # duplicate descriptor with identical spelling
            key += "+"
        imps[key] = names
    r["imports"] = imps
    dimps = {}
    for e in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []) or []:
        dll = e.dll.decode("latin-1")
        dimps[dll] = [i.name.decode("latin-1") if i.name else f"#ord{i.ordinal}" for i in e.imports]
    r["delay_imports"] = dimps
    exps = []
    ed = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
    if ed is not None:
        for s in ed.symbols:
            exps.append({"name": s.name.decode("latin-1") if s.name else None, "ordinal": s.ordinal,
                         "rva": hex(s.address)})
    r["exports"] = exps

    # watch-list
    watch = {}
    lower_imps: dict[str, list] = {}
    for k, v in list(imps.items()) + list(dimps.items()):
        lower_imps.setdefault(k.lower(), []).extend(v)  # merge e.g. USER32.dll + user32.dll
    for dll, funcs in lower_imps.items():
        for wk, wf in WATCH.items():
            if wk == dll or (wk == "xinput" and dll.startswith("xinput")):
                hit = funcs if wf is None else [f for f in funcs if f in wf]
                if hit:
                    watch[dll] = hit
    r["watch"] = watch

    # dll strings
    ds = dll_strings(img)
    r["dll_strings"] = {k: [(t, hex(o)) for t, o in v[:4]] for k, v in sorted(ds.items())}

    # proxy viability
    static_l = set(lower_imps.keys())
    prox = []
    for c in PROXY_CANDIDATES:
        how = []
        if c in {k.lower() for k in imps}:
            how.append("static import")
        if c in {k.lower() for k in dimps}:
            how.append("delay import")
        if c in ds:
            how.append("name string in image (" + ",".join(sorted({t for t, _ in ds[c]})) + ")")
        prox.append({"dll": c, "evidence": how, "known_dll": c in KNOWN_DLLS,
                     "in_static_imports": c in static_l})
    r["proxy"] = prox

    # protector heuristics
    heur = []
    names = [s.name for s in img.sections]
    if ".bind" in names:
        heur.append("SteamStub/SteamDRM: .bind section present")
    if eps and eps.name not in (".text",):
        heur.append(f"Entry point is in section '{eps.name}' (not .text)")
    for s in secs:
        if "X" in s["flags"][:3] and s["entropy"] > 7.2:
            heur.append(f"Executable section {s['name']} entropy {s['entropy']} (>7.2: packed/encrypted?)")
        if s["name"] not in (".text", ".rdata", ".data", ".pdata", ".rsrc", ".reloc", ".tls", "_RDATA",
                             ".idata", ".edata", ".00cfg", ".gfids", ".CRT", ".bss", ".didat", ".xdata"):
            heur.append(f"Non-standard section name '{s['name']}' ({s['flags']}, vsize {s['vsize']:#x}, "
                        f"entropy {s['entropy']})")
    if r["tls"] and r["tls"]["callbacks"]:
        heur.append(f"{len(r['tls']['callbacks'])} TLS callback(s): " +
                    ", ".join(f"{c['rva']}({c['section']})" for c in r["tls"]["callbacks"]))
    # protector name strings anywhere in the file image
    buf = bytes(img.image)
    pmarks = {}
    for m in re.finditer(rb"(?i)denuvo|vmprotect|themida|winlicense|arxan|guardit|steamstub|codevirtualizer|"
                         rb"enigma ?protector|\.vmp[0-9]", buf):
        s_ = img.section_of(m.start())
        pmarks.setdefault(m.group(0).decode(), []).append(f"rva {m.start():#x} ({s_.name if s_ else '?'})")
    r["protector_strings"] = pmarks
    for k, v in pmarks.items():
        heur.append(f"protector-name string '{k}' x{len(v)}: " + ", ".join(v[:4]))
    r["heuristics"] = heur

    # entry disassembly
    if disasm:
        try:
            _, md = load_capstone()
            code = bytes(img.image[ep: ep + 96])
            r["entry_disasm"] = [f"{hexs(i.address)}: {i.mnemonic} {i.op_str}" for i in md.disasm(code, ep)][:24]
            if r["tls"]:
                r["tls_disasm"] = {}
                for c in r["tls"]["callbacks"]:
                    a = int(c["rva"], 16)
                    code = bytes(img.image[a: a + 64])
                    r["tls_disasm"][c["rva"]] = [f"{hexs(i.address)}: {i.mnemonic} {i.op_str}"
                                                 for i in md.disasm(code, a)][:12]
        except Exception as ex:
            r["entry_disasm"] = [f"capstone unavailable: {ex}"]
    return r


def to_md(r: dict) -> str:
    L = []
    a = L.append
    a(f"## `{os.path.basename(r['path'])}`\n")
    a(f"- path: `{r['path']}`")
    if r.get("version"):
        v = r["version"]
        a(f"- version resource: FileVersion {v.get('FileVersion')} / ProductVersion {v.get('ProductVersion')} / "
          f"InternalName {v.get('InternalName')} / OriginalFilename {v.get('OriginalFilename')} / "
          f"Company {v.get('CompanyName')}")
    a(f"- size: {r['file_size']} bytes; sha256 `{r['sha256']}`")
    a(f"- TimeDateStamp: {hex(r['TimeDateStamp'])} ({r['TimeDateStamp_utc']})")
    a(f"- ImageBase {r['ImageBase']}, SizeOfImage {hex(r['SizeOfImage'])}, SizeOfHeaders {hex(r['SizeOfHeaders'])}")
    a(f"- EntryPoint RVA {r['EntryPoint_rva']} in `{r['EntryPoint_section']}`")
    a(f"- Subsystem {r['Subsystem']}, DllCharacteristics {r['DllCharacteristics']} "
      f"({', '.join(r['DllCharacteristics_decoded'])}), Linker {r['LinkerVersion']}, OS {r['OSVersion']}, "
      f"CheckSum {r['CheckSum']}")
    for d in r["debug"]:
        if "pdb" in d:
            a(f"- CodeView PDB: `{d['pdb']}` (debug ts {d['TimeDateStamp_utc']})")
    if r.get("load_config"):
        a(f"- LoadConfig: " + ", ".join(f"{k}={v}" for k, v in r["load_config"].items()))
    a("")
    a("| section | RVA | VSize | raw off | raw size | flags | entropy |")
    a("|---|---|---|---|---|---|---|")
    for s in r["sections"]:
        a(f"| `{s['name']}` | {s['rva']} | {s['vsize']:#x} | {s['raw_off']} | {s['raw_size']:#x} | "
          f"{s['flags']} ({s['chars']}) | {s['entropy']} |")
    a("")
    a("Data directories: " + ", ".join(f"{k}@{v['rva']}+{v['size']:#x}({v['section']})"
                                      for k, v in r["data_directories"].items()))
    a("")
    if r["tls"]:
        a(f"TLS: {r['tls']}")
    else:
        a("TLS: none")
    a("")
    a("Protector heuristics:")
    for h in r["heuristics"]:
        a(f"- {h}")
    a("")
    if r.get("entry_disasm"):
        a("Entry point disassembly:\n```")
        L.extend(r["entry_disasm"])
        a("```")
    if r.get("tls_disasm"):
        for k, v in r["tls_disasm"].items():
            a(f"TLS callback {k}:\n```")
            L.extend(v)
            a("```")
    a("")
    a("### Watch-list imports")
    for k, v in r["watch"].items():
        a(f"- **{k}**: {', '.join(v)}")
    a("")
    a("### Proxy DLL viability evidence")
    a("| DLL | evidence | KnownDLL |")
    a("|---|---|---|")
    for p in r["proxy"]:
        if p["evidence"]:
            a(f"| {p['dll']} | {'; '.join(p['evidence'])} | {'yes' if p['known_dll'] else 'no'} |")
    a("")
    a("### Full import table")
    for k, v in r["imports"].items():
        a(f"- **{k}** ({len(v)}): {', '.join(v)}")
    if r["delay_imports"]:
        a("\nDelay imports:")
        for k, v in r["delay_imports"].items():
            a(f"- **{k}** ({len(v)}): {', '.join(v)}")
    a(f"\nExports: {len(r['exports'])}" + (": " + ", ".join(str(e['name']) for e in r['exports'][:50])
                                           if r['exports'] else ""))
    a("\n### DLL names referenced as strings")
    a(", ".join(f"{k}[{'/'.join(sorted({t for t, _ in v}))}]" for k, v in r["dll_strings"].items()))
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe", nargs="+")
    ap.add_argument("--json")
    ap.add_argument("--md")
    ap.add_argument("--no-disasm", action="store_true")
    a = ap.parse_args()
    results = [scout(p, not a.no_disasm) for p in a.exe]
    md = "\n".join(to_md(r) for r in results)
    if a.md:
        with open(a.md, "w") as f:
            f.write(md)
    else:
        print(md)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(results if len(results) > 1 else results[0], f, indent=1)


if __name__ == "__main__":
    main()
