#!/usr/bin/env python3
"""str_xref.py - find strings in a PE and their code/data cross-references (read-only).

  * strings: NUL-terminated ASCII (and optionally UTF-16LE) runs in non-code sections
  * code xrefs: every RIP-relative disp32 in executable sections whose target equals a string RVA
                (numpy brute force over every byte offset; each hit is confirmed by decoding the
                instruction with capstone so only real 'lea/mov/cmp ... [rip+X]' remain)
  * data xrefs: 8-byte absolute pointers (ImageBase+RVA) to the string in non-code sections
                (e.g. luaL_Reg {name, func} tables) with the neighbouring qwords printed
  * containing function from .pdata (RUNTIME_FUNCTION, chained unwind info followed)

Usage:
  str_xref.py EXE -p REGEX [-p REGEX ...] [-e EXACT ...] [--utf16] [--no-code] [--no-data]
              [--max-strings 400] [--max-xrefs 12] [--tsv out.tsv] [--md out.md]

Patterns are Python regexes matched (search) against the decoded string.
"""
from __future__ import annotations

import argparse
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_common import PEImage, load_capstone  # noqa: E402


def extract_strings(img: PEImage, utf16: bool = False, minlen: int = 3):
    out = []  # (rva, text, kind)
    for s in img.sections:
        if s.executable or s.raw_size == 0 or s.name in (".pdata", ".reloc", ".ecode"):
            continue
        n = min(s.raw_size, s.vsize or s.raw_size)
        buf = bytes(img.image[s.rva: s.rva + n])
        for m in re.finditer(rb"[\x20-\x7e\t\r\n]{%d,}\x00" % minlen, buf):
            # keep only strings that start at a NUL boundary (avoid suffix duplicates)
            out.append((s.rva + m.start(), m.group(0)[:-1].decode("latin-1"), "a"))
        if utf16:
            for m in re.finditer(rb"(?:[\x20-\x7e]\x00){%d,}\x00\x00" % minlen, buf):
                out.append((s.rva + m.start(), m.group(0)[:-2].decode("utf-16le"), "u"))
    return out


def code_xrefs(img: PEImage, targets: np.ndarray, chunk: int = 1 << 24):
    """Return dict target_rva -> list of (site_rva_of_disp32, extra) for RIP-relative disp32 refs."""
    targets = np.unique(targets.astype(np.int64))
    hits: dict[int, list[int]] = {}
    for s in img.exec_sections():
        n = min(s.raw_size, s.vsize or s.raw_size)
        if n < 8:
            continue
        a = img.np_image[s.rva: s.rva + n]
        pos = 0
        while pos < n - 4:
            end = min(n - 4, pos + chunk)
            seg = a[pos: end + 4].astype(np.uint32)
            d = (seg[0:-4] | (seg[1:-3] << 8) | (seg[2:-2] << 16) | (seg[3:-1] << 24)).view(np.int32)
            base = s.rva + pos
            idx = np.arange(d.size, dtype=np.int64)
            # instruction may end right after disp32 (lea/mov) or 1/4 bytes later (imm8/imm32)
            for extra in (4, 5, 8):
                t = base + idx + extra + d.astype(np.int64)
                k = np.searchsorted(targets, t)
                k[k >= targets.size] = targets.size - 1
                ok = np.nonzero(targets[k] == t)[0]
                for o in ok:
                    hits.setdefault(int(t[o]), []).append((int(base + o), extra))
            pos = end
    return hits


def confirm(img: PEImage, md, disp_rva: int, extra: int, target: int):
    """Try decoding an instruction that contains this disp32; return (insn_rva, text) or None."""
    order = (3, 2, 4, 5, 6, 7) if 0x40 <= img.image[disp_rva - 3] <= 0x4F else (2, 3, 4, 5, 6, 7)
    for back in order:
        st = disp_rva - back
        code = bytes(img.image[st: st + 16])
        for ins in md.disasm(code, st, count=1):
            if ins.address + ins.size == disp_rva + extra and "rip" in ins.op_str:
                return ins.address, f"{ins.mnemonic} {ins.op_str}"
    return None


def data_xrefs(img: PEImage, targets: np.ndarray):
    vas = np.unique((targets.astype(np.uint64) + np.uint64(img.base)))
    hits: dict[int, list[int]] = {}
    for s in img.sections:
        if s.executable or s.raw_size == 0:
            continue
        n = min(s.raw_size, s.vsize or s.raw_size) // 8 * 8
        if n < 8:
            continue
        a = np.frombuffer(bytes(img.image[s.rva: s.rva + n]), dtype=np.uint64)
        idx = np.nonzero(np.isin(a, vas))[0]
        for i in idx:
            hits.setdefault(int(a[i]) - img.base, []).append(s.rva + int(i) * 8)
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("-p", "--pattern", action="append", default=[])
    ap.add_argument("-e", "--exact", action="append", default=[])
    ap.add_argument("--utf16", action="store_true")
    ap.add_argument("--no-code", action="store_true")
    ap.add_argument("--no-data", action="store_true")
    ap.add_argument("--max-strings", type=int, default=400)
    ap.add_argument("--max-xrefs", type=int, default=12)
    ap.add_argument("--tsv")
    ap.add_argument("--md")
    a = ap.parse_args()

    img = PEImage(a.exe)
    strs = extract_strings(img, a.utf16)
    rx = [re.compile(p) for p in a.pattern]
    ex = set(a.exact)
    sel = [(r, t, k) for (r, t, k) in strs if t in ex or any(x.search(t) for x in rx)]
    sel.sort(key=lambda x: x[0])
    print(f"[str_xref] {len(strs)} strings scanned, {len(sel)} selected", file=sys.stderr)
    if len(sel) > a.max_strings:
        print(f"[str_xref] truncating to {a.max_strings} strings", file=sys.stderr)
        sel = sel[: a.max_strings]
    targets = np.array([r for r, _, _ in sel], dtype=np.int64)
    cx = code_xrefs(img, targets) if (not a.no_code and targets.size) else {}
    dx = data_xrefs(img, targets) if (not a.no_data and targets.size) else {}
    _, md = load_capstone()

    rows = []
    for r, t, k in sel:
        off = img.rva_to_off(r)
        sec = img.section_of(r)
        crefs = []
        for disp_rva, extra in cx.get(r, [])[: a.max_xrefs * 4]:
            c = confirm(img, md, disp_rva, extra, r)
            if c is None:
                continue
            f = img.func_of(c[0])
            crefs.append((c[0], c[1], f))
            if len(crefs) >= a.max_xrefs:
                break
        drefs = []
        for slot in dx.get(r, [])[: a.max_xrefs]:
            nxt = img.u64(slot + 8) - img.base
            prv = img.u64(slot - 8) - img.base
            drefs.append((slot, prv, nxt))
        rows.append((r, off, sec.name if sec else "?", k, t, crefs, drefs, len(cx.get(r, [])),
                     len(dx.get(r, []))))

    lines_md = ["| string | RVA | file off | sec | code xrefs (insn RVA: insn  [func begin-end]) | data xrefs (slot: prev/next qword RVA) |",
                "|---|---|---|---|---|---|"]
    with open(a.tsv, "w") if a.tsv else open(os.devnull, "w") as tf:
        tf.write("string\trva\tfile_off\tsection\tkind\tcode_xrefs\tdata_xrefs\n")
        for r, off, sec, k, t, crefs, drefs, ncx, ndx in rows:
            cs = "; ".join(f"{c:#x}:{ins}" + (f" [{f[0]:#x}-{f[1]:#x}]" if f else " [no pdata]")
                           for c, ins, f in crefs)
            ds = "; ".join(f"{s_:#x}(prev {p:#x}/next {n:#x})" for s_, p, n in drefs)
            tf.write(f"{t!r}\t{r:#x}\t{off:#x}\t{sec}\t{k}\t{cs}\t{ds}\n")
            tt = t if len(t) <= 70 else t[:67] + "..."
            tt = tt.replace("|", "\\|").replace("\n", "\\n")
            lines_md.append(f"| `{tt}` | {r:#x} | {off:#x} | {sec} | {cs.replace('|', '/') or '-'} | {ds or '-'} |")
            print(f"{r:#010x} off {off:#010x} {sec:8s} {t[:90]!r}  code={ncx} data={ndx}")
            for c, ins, f in crefs:
                print(f"     code {c:#x}: {ins}   func " + (f"{f[0]:#x}-{f[1]:#x}" if f else "?"))
            for s_, p, n in drefs:
                print(f"     data slot {s_:#x}  prev={p:#x} next={n:#x}")
    if a.md:
        with open(a.md, "w") as f:
            f.write("\n".join(lines_md) + "\n")


if __name__ == "__main__":
    main()
