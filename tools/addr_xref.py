#!/usr/bin/env python3
"""addr_xref.py - find every static reference to one or more RVAs in a PE (read-only).

Reports, per target RVA:
  * call rel32 (E8) and jmp rel32 (E9) sites
  * RIP-relative disp32 operands (lea/mov/cmp...) confirmed with capstone
  * 8-byte absolute pointers (ImageBase+RVA) in non-code sections (vtables, function tables)
and the containing .pdata function for code sites.

Usage: addr_xref.py EXE RVA [RVA ...] [--max 40]
"""
from __future__ import annotations

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_common import PEImage, load_capstone  # noqa: E402
from str_xref import code_xrefs, confirm, data_xrefs  # noqa: E402


def rel_branches(img: PEImage, targets: set[int]):
    out: dict[int, list] = {}
    tarr = np.array(sorted(targets), dtype=np.int64)
    for s in img.exec_sections():
        n = min(s.raw_size, s.vsize or s.raw_size)
        a = img.np_image[s.rva: s.rva + n]
        for op, kind in ((0xE8, "call"), (0xE9, "jmp")):
            idx = np.nonzero(a[:-4] == op)[0]
            d = (a[idx + 1].astype(np.uint32) | (a[idx + 2].astype(np.uint32) << 8) |
                 (a[idx + 3].astype(np.uint32) << 16) | (a[idx + 4].astype(np.uint32) << 24)).view(np.int32)
            t = s.rva + idx.astype(np.int64) + 5 + d.astype(np.int64)
            hit = np.isin(t, tarr)
            for i, tt in zip(idx[hit].tolist(), t[hit].tolist()):
                out.setdefault(int(tt), []).append((kind, s.rva + i))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("rvas", nargs="+", type=lambda x: int(x, 16))
    ap.add_argument("--max", type=int, default=40)
    a = ap.parse_args()
    img = PEImage(a.exe)
    _, md = load_capstone()
    targets = set(a.rvas)
    br = rel_branches(img, targets)
    cx = code_xrefs(img, np.array(sorted(targets), dtype=np.int64))
    dx = data_xrefs(img, np.array(sorted(targets), dtype=np.int64))
    for t in a.rvas:
        f = img.func_of(t)
        print(f"== {t:#x} ({img.section_of(t).name if img.section_of(t) else '?'}; pdata func "
              f"{(hex(f[0]) + '-' + hex(f[1])) if f else 'none'})")
        b = br.get(t, [])
        print(f"   rel32 branches: {len(b)}")
        for kind, site in b[: a.max]:
            ff = img.func_of(site)
            print(f"     {kind} at {site:#x}  in {(hex(ff[0])) if ff else '?'}")
        c = []
        for disp, extra in cx.get(t, []):
            r = confirm(img, md, disp, extra, t)
            if r:
                c.append(r)
        print(f"   rip-relative refs: {len(c)}")
        for site, ins in c[: a.max]:
            ff = img.func_of(site)
            print(f"     {site:#x}: {ins}  in {(hex(ff[0])) if ff else '?'}")
        d = dx.get(t, [])
        print(f"   absolute pointers in data: {len(d)}")
        for slot in d[: a.max]:
            print(f"     slot {slot:#x} ({img.section_of(slot).name})")


if __name__ == "__main__":
    main()
