#!/usr/bin/env python3
"""disasm.py - annotated x64 disassembly of RVA ranges in a PE (read-only, capstone).

Usage:
  disasm.py EXE RVA[:END] [RVA[:END] ...] [--func] [--syms syms.tsv] [-n MAXINSNS]

  RVA:END  disassemble [RVA, END)
  RVA      with --func: the whole .pdata function containing RVA; otherwise 64 instructions
  --syms   optional TSV "rva<TAB>name" used to label call/jmp targets and RIP-relative operands

RIP-relative operands are resolved to absolute RVAs, labelled with their section and, when they
point at printable data, the C string found there.
"""
from __future__ import annotations

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_common import PEImage, load_capstone  # noqa: E402

RIP = re.compile(r"\[rip ([+-]) (0x[0-9a-f]+)\]")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("ranges", nargs="+")
    ap.add_argument("--func", action="store_true")
    ap.add_argument("--syms")
    ap.add_argument("-n", type=int, default=4000)
    a = ap.parse_args()
    img = PEImage(a.exe)
    _, md = load_capstone()
    syms = {}
    if a.syms:
        for ln in open(a.syms):
            p = ln.rstrip("\n").split("\t")
            if len(p) >= 2 and p[0].startswith("0x"):
                syms[int(p[0], 16)] = p[1]

    def label(t):
        s = img.section_of(t)
        out = f"{t:#x}"
        if t in syms:
            out += f" <{syms[t]}>"
        if s is None:
            return out + " ?"
        out += f" {s.name}"
        if not s.executable:
            cs = img.cstr(t, 96)
            if len(cs) >= 2 and all(32 <= c < 127 or c in (9, 10) for c in cs):
                out += " " + repr(cs.decode("latin-1"))
        return out

    for spec in a.ranges:
        if ":" in spec:
            b, e = (int(x, 16) for x in spec.split(":"))
        else:
            b = int(spec, 16)
            f = img.func_of(b) if a.func else None
            if f:
                b, e = f
            else:
                e = b + 64 * 15
        print(f"---- {b:#x}..{e:#x}  runtime_function={img.runtime_function(b)}  section={img.section_of(b).name}")
        n = 0
        for ins in md.disasm(bytes(img.image[b:e]), b):
            ann = ""
            m = RIP.search(ins.op_str)
            if m:
                t = ins.address + ins.size + int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
                ann = "  ; " + label(t)
            elif ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
                ann = "  ; " + label(int(ins.op_str, 16))
            print(f"{ins.address:#09x}: {ins.bytes.hex():<22} {ins.mnemonic} {ins.op_str}{ann}")
            n += 1
            if n >= a.n or (not a.func and ":" not in spec and n >= 64):
                break


if __name__ == "__main__":
    main()
