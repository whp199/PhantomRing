#!/usr/bin/env python3
"""iat_xref.py - locate IAT slots of chosen imports and every code site that uses them (read-only).

Finds 'call qword ptr [rip+IAT]', 'jmp qword ptr [rip+IAT]' and 'mov reg, qword ptr [rip+IAT]'
for each requested import, with the containing .pdata function, so you can find e.g. the window
procedure registration (RegisterClassExW), the XInput poller (XInputGetState = XINPUT1_x #2),
DirectInput8Create's caller, D3D device creation, timing (QueryPerformanceCounter).

Usage:
  iat_xref.py EXE NAME [NAME ...] [--max 20]
     NAME = function name (any DLL) or DLL!name or DLL!#ordinal, case-insensitive
"""
from __future__ import annotations

import argparse
import os
import sys

import numpy as np
import pefile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_common import PEImage, load_capstone  # noqa: E402
from str_xref import code_xrefs, confirm  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("names", nargs="+")
    ap.add_argument("--max", type=int, default=20)
    a = ap.parse_args()
    img = PEImage(a.exe)
    img.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    _, md = load_capstone()
    slots = {}
    for e in img.pe.DIRECTORY_ENTRY_IMPORT:
        dll = e.dll.decode().lower()
        for i in e.imports:
            nm = i.name.decode() if i.name else f"#{i.ordinal}"
            for want in a.names:
                w = want.lower()
                if (w == nm.lower()) or (w == f"{dll}!{nm.lower()}") or ("!" in w and w.split("!")[0] in dll
                                                                          and w.split("!")[1] == nm.lower()):
                    slots[i.address - img.base] = f"{dll}!{nm}"
    if not slots:
        raise SystemExit("no matching imports")
    hits = code_xrefs(img, np.array(sorted(slots), dtype=np.int64))
    for slot, label in sorted(slots.items(), key=lambda x: x[1]):
        refs = []
        for disp, extra in hits.get(slot, []):
            c = confirm(img, md, disp, extra, slot)
            if c:
                refs.append(c)
        print(f"== {label}  IAT slot RVA {slot:#x}  ({len(refs)} code refs)")
        for site, ins in refs[: a.max]:
            f = img.func_of(site)
            sec = img.section_of(site)
            print(f"     {site:#x} [{sec.name}] {ins}   func {(hex(f[0]) + '-' + hex(f[1])) if f else '?'}")


if __name__ == "__main__":
    main()
