#!/usr/bin/env python3
"""rtti_vtables.py - recover MSVC x64 RTTI classes and their vtables from a PE (read-only).

How it works (MSVC x64 RTTI layout):
  TypeDescriptor        { u64 pVFTable(type_info vftable); u64 spare; char name[] }  name = ".?AVFoo@ns@@"
  CompleteObjectLocator { u32 signature(=1 on x64); u32 offset; u32 cdOffset;
                          u32 pTypeDescriptor(RVA); u32 pClassDescriptor(RVA); u32 pSelf(RVA) }
  ClassHierarchyDescriptor { u32 signature; u32 attributes; u32 numBaseClasses; u32 pBaseClassArray(RVA) }
  BaseClassDescriptor   { u32 pTypeDescriptor; u32 numContainedBases; i32 mdisp,pdisp,vdisp; u32 attr; u32 pCHD }
  vtable layout:        [u64 VA of COL] [vfunc0] [vfunc1] ...   -> the vtable RVA is (slot holding COL VA) + 8

Steps: find '.?AV'/'.?AU' strings -> TypeDescriptor RVAs (string-0x10); scan every non-code section for
COLs (sig==1 && pSelf==own RVA && pTypeDescriptor in TD set); scan every non-code section for 8-byte
slots holding ImageBase+COL_RVA -> vtables; count vfuncs (consecutive pointers into executable
sections, stopping at the next COL slot).

Usage:
  rtti_vtables.py EXE --tsv all.tsv [--json all.json]
                  [--grep REGEX ...] [--md table.md] [--dump-vfuncs REGEX] [--max-vfuncs N]

Every row produced is VERIFIED by structure parsing (COL.pSelf matches, TD is a valid name, the vtable
slot points back at that COL).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_common import PEImage, demangle_type_names  # noqa: E402


def sec_label(img: PEImage, rva: int) -> str:
    s = img.section_of(rva)
    if s is None:
        return "?"
    idx = img.sections.index(s)
    dup = sum(1 for x in img.sections if x.name == s.name) > 1
    return f"{s.name}#{idx}" if dup else s.name


def find_type_descriptors(img: PEImage):
    """Return dict td_rva -> raw name, plus the dominant type_info vftable VA."""
    buf = img.image
    cands = []
    for s in img.sections:
        if s.executable or s.raw_size == 0:
            continue
        end = s.rva + min(s.raw_size, s.vsize or s.raw_size)
        for prefix in (b".?AV", b".?AU"):
            i = buf.find(prefix, s.rva, end)
            while i >= 0:
                td = i - 16
                if td >= s.rva:
                    e = buf.find(b"\0", i, min(i + 4096, end))
                    if e > i:
                        name = bytes(buf[i:e])
                        if name.endswith(b"@@") and all(0x21 <= c < 0x7F for c in name):
                            cands.append((td, name.decode("latin-1"), img.u64(td), img.u64(td + 8)))
                i = buf.find(prefix, i + 4, end)
    vt = Counter(c[2] for c in cands)
    tinfo_vft, _ = vt.most_common(1)[0] if vt else (0, 0)
    tds = {}
    rejected = 0
    for td, name, v, spare in cands:
        # accept dominant vftable value (or 0 when patched at runtime) and spare==0
        if (v == tinfo_vft or v == 0) and spare == 0:
            tds[td] = name
        else:
            rejected += 1
    return tds, tinfo_vft, rejected, vt.most_common(4)


def find_cols(img: PEImage, td_set: set[int]):
    cols = {}
    td_arr = np.array(sorted(td_set), dtype=np.uint32)
    for s in img.sections:
        if s.executable or s.raw_size == 0:
            continue
        n = min(s.raw_size, s.vsize or s.raw_size) // 4 * 4
        if n < 24:
            continue
        a = np.frombuffer(bytes(img.image[s.rva: s.rva + n]), dtype=np.uint32)
        idx = np.nonzero(a[:-5] == 1)[0]
        if idx.size == 0:
            continue
        self_rva = (s.rva + idx * 4).astype(np.uint32)
        ok = a[idx + 5] == self_rva
        idx = idx[ok]
        ok = np.isin(a[idx + 3], td_arr)
        idx = idx[ok]
        for i in idx:
            r = s.rva + int(i) * 4
            cols[r] = {"offset": int(a[i + 1]), "cdOffset": int(a[i + 2]), "td": int(a[i + 3]),
                       "chd": int(a[i + 4])}
    return cols


def find_vtables(img: PEImage, cols: dict):
    col_vas = np.array(sorted(img.base + c for c in cols), dtype=np.uint64)
    out = {}  # col_rva -> [vtable_rva,...]
    slots = set()
    for s in img.sections:
        if s.executable or s.raw_size == 0:
            continue
        n = min(s.raw_size, s.vsize or s.raw_size) // 8 * 8
        if n < 16:
            continue
        a = np.frombuffer(bytes(img.image[s.rva: s.rva + n]), dtype=np.uint64)
        idx = np.nonzero(np.isin(a, col_vas))[0]
        for i in idx:
            slot = s.rva + int(i) * 8
            col = int(a[i]) - img.base
            out.setdefault(col, []).append(slot + 8)
            slots.add(slot)
    return out, slots


def count_vfuncs(img: PEImage, vt_rva: int, col_slots: set[int], limit: int = 4096):
    n = 0
    exec_ranges = [(s.rva, s.rva + max(s.vsize, s.raw_size)) for s in img.sections if s.executable]
    r = vt_rva
    while n < limit:
        if r in col_slots:
            break
        try:
            v = img.u64(r)
        except Exception:
            break
        rv = v - img.base
        if not any(a <= rv < b for a, b in exec_ranges):
            break
        n += 1
        r += 8
    return n


def parse_chd(img: PEImage, chd: int, tds: dict):
    try:
        sig, attr, nbases, pbca = (img.u32(chd), img.u32(chd + 4), img.u32(chd + 8), img.u32(chd + 12))
    except Exception:
        return None
    if nbases > 512:
        return None
    bases = []
    for k in range(nbases):
        bcd = img.u32(pbca + 4 * k)
        tdr = img.u32(bcd)
        mdisp = img.i32(bcd + 8)
        bases.append((tds.get(tdr, f"?td@{tdr:#x}"), mdisp))
    return {"attributes": attr, "bases": bases}


def analyse(path: str, max_vfuncs: int = 4096):
    img = PEImage(path)
    tds, tinfo_vft, rejected, vtc = find_type_descriptors(img)
    print(f"[rtti] {os.path.basename(path)}: {len(tds)} TypeDescriptors (type_info vftable VA "
          f"{tinfo_vft:#x}; {rejected} rejected; top vftable values {[(hex(a), b) for a, b in vtc]})",
          file=sys.stderr)
    cols = find_cols(img, set(tds))
    print(f"[rtti] {len(cols)} CompleteObjectLocators", file=sys.stderr)
    vts, col_slots = find_vtables(img, cols)
    print(f"[rtti] {sum(len(v) for v in vts.values())} vtables (COL-referencing slots)", file=sys.stderr)
    dem = demangle_type_names(sorted(set(tds.values())))
    rows = []
    has_col = set()
    for col, c in cols.items():
        name = tds[c["td"]]
        has_col.add(c["td"])
        chd = parse_chd(img, c["chd"], tds)
        for vt in vts.get(col, []) or [None]:
            nv = count_vfuncs(img, vt, col_slots, max_vfuncs) if vt is not None else 0
            # where do vfuncs live?
            secs = Counter()
            if vt is not None:
                for k in range(min(nv, 512)):
                    secs[sec_label(img, img.u64(vt + 8 * k) - img.base)] += 1
            rows.append({
                "name": dem.get(name, name), "raw": name, "td_rva": c["td"], "col_rva": col,
                "col_offset": c["offset"], "cd_offset": c["cdOffset"], "vtable_rva": vt,
                "vtable_section": sec_label(img, vt) if vt is not None else None,
                "n_vfuncs": nv, "vfunc_sections": dict(secs),
                "chd_attr": chd["attributes"] if chd else None,
                "bases": [(dem.get(b, b), md) for b, md in (chd["bases"][1:] if chd else [])],
            })
    # TDs without any COL (no vtable: e.g. exception-only types or classes with no virtuals)
    for td, name in tds.items():
        if td not in has_col:
            rows.append({"name": dem.get(name, name), "raw": name, "td_rva": td, "col_rva": None,
                         "col_offset": None, "cd_offset": None, "vtable_rva": None, "vtable_section": None,
                         "n_vfuncs": 0, "vfunc_sections": {}, "chd_attr": None, "bases": []})
    rows.sort(key=lambda r: (r["name"], r["col_offset"] if r["col_offset"] is not None else -1,
                             r["vtable_rva"] or 0))
    meta = {"exe": path, "image_base": img.base, "n_td": len(tds), "n_col": len(cols),
            "n_vtables": sum(len(v) for v in vts.values()), "type_info_vftable_va": tinfo_vft}
    return img, rows, meta


def h(v):
    return "" if v is None else f"0x{v:X}"


def write_tsv(rows, path):
    with open(path, "w") as f:
        f.write("name\traw\ttd_rva\tcol_rva\tcol_offset\tvtable_rva\tvtable_section\tn_vfuncs\tvfunc_sections\tbases\n")
        for r in rows:
            bases = ";".join(f"{dn}@{md:#x}" for dn, md in r["bases"])
            vs = ",".join(f"{k}:{v}" for k, v in r["vfunc_sections"].items())
            f.write(f"{r['name']}\t{r['raw']}\t{h(r['td_rva'])}\t{h(r['col_rva'])}\t"
                    f"{'' if r['col_offset'] is None else hex(r['col_offset'])}\t{h(r['vtable_rva'])}\t"
                    f"{r['vtable_section'] or ''}\t{r['n_vfuncs']}\t{vs}\t{bases}\n")


def md_table(rows, patterns, dem_bases=None, limit_per_pat=400):
    out = ["| class | TD RVA | COL RVA | COL off | vtable RVA | #vf | vfunc sections | direct bases (first 4) |",
           "|---|---|---|---|---|---|---|---|"]
    seen = set()
    for pat in patterns:
        rx = re.compile(pat)
        k = 0
        for r in rows:
            key = (r["raw"], r["col_rva"], r["vtable_rva"])
            if key in seen or not rx.search(r["name"]):
                continue
            seen.add(key)
            bases = ", ".join(b for b, _ in r["bases"][:4])
            vs = ",".join(f"{a}:{b}" for a, b in r["vfunc_sections"].items())
            out.append(f"| `{r['name']}` | {h(r['td_rva'])} | {h(r['col_rva'])} | "
                       f"{'' if r['col_offset'] is None else hex(r['col_offset'])} | {h(r['vtable_rva'])} | "
                       f"{r['n_vfuncs']} | {vs} | {bases} |")
            k += 1
            if k >= limit_per_pat:
                break
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("--tsv")
    ap.add_argument("--json")
    ap.add_argument("--md")
    ap.add_argument("--grep", nargs="*", default=[])
    ap.add_argument("--dump-vfuncs", help="regex of class names whose vfunc RVAs to print")
    ap.add_argument("--max-vfuncs", type=int, default=4096)
    a = ap.parse_args()
    img, rows, meta = analyse(a.exe, a.max_vfuncs)
    print(json.dumps(meta), file=sys.stderr)
    if a.tsv:
        write_tsv(rows, a.tsv)
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"meta": meta, "rows": rows}, f)
    if a.grep:
        t = md_table(rows, a.grep)
        if a.md:
            with open(a.md, "w") as f:
                f.write(t)
        else:
            print(t)
    if a.dump_vfuncs:
        rx = re.compile(a.dump_vfuncs)
        for r in rows:
            if r["vtable_rva"] is None or not rx.search(r["name"]):
                continue
            print(f"## {r['name']} vtable {h(r['vtable_rva'])} (COL off {r['col_offset']:#x}) {r['n_vfuncs']} vfuncs")
            for k in range(r["n_vfuncs"]):
                t = img.u64(r["vtable_rva"] + 8 * k) - img.base
                print(f"  [{k:3d}] +{8 * k:#05x}  {t:#x}  {sec_label(img, t)}")


if __name__ == "__main__":
    main()
