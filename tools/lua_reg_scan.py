#!/usr/bin/env python3
"""lua_reg_scan.py - recover native Lua 5.1 library registrations (luaL_Reg tables) from a PE.

Built for MGSV (mgsvtpp.exe, Fox Engine, embedded Lua 5.1.5) but generic for any MSVC x64 binary
that statically links Lua 5.1.

Pipeline (all static, read-only):
  1. luaI_openlib  = function whose code references BOTH "_LOADED" and "name conflict for module '%s'"
                     and which is the target of a 'xor r9d,r9d ; jmp luaI_openlib' thunk
     luaL_register = that thunk (Lua 5.1: luaL_register(L,n,l) { luaI_openlib(L,n,l,0); })
     (override with --openlib/--register RVA)
  2. wrappers      = callers of luaL_register that forward their own rdx/r8 (e.g. Fox's
                     register helper) - their callers are scanned too (depth 2)
  3. at each call site the containing function (from .pdata) is linearly emulated: rsp/rbp frame
     offsets, 'lea reg,[rip+X]', 'lea reg,[rsp/rbp+X]', 'mov [frame],reg', 'xor r,r' are tracked so
     the luaL_Reg {const char* name; lua_CFunction func;} array that r8 points to can be rebuilt
     (stack-built or static in .rdata/.data) together with the library name in rdx.

Usage:
  lua_reg_scan.py EXE [--tsv out.tsv] [--md out.md] [--libs Player,Pad,...] [--openlib RVA] [--register RVA]
"""
from __future__ import annotations

import argparse
import bisect
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_common import PEImage, load_capstone  # noqa: E402

REG64 = {}
for r in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
    REG64["r" + r] = "r" + r
    REG64["e" + r] = "r" + r
for i in range(8, 16):
    for suf in ("", "d", "w", "b"):
        REG64[f"r{i}{suf}"] = f"r{i}"
VOLATILE = ("rax", "rcx", "rdx", "r8", "r9", "r10", "r11")
RIPREL = re.compile(r"\[rip ([+-]) (0x[0-9a-f]+|\d+)\]")
MEM = re.compile(r"\[([a-z0-9]+)(?: ([+-]) (0x[0-9a-f]+|\d+))?\]")


def num(s):
    return int(s, 16) if s.startswith("0x") else int(s)


def find_rel_branches(img: PEImage, target: int, opcode: int):
    out = []
    for s in img.exec_sections():
        n = min(s.raw_size, s.vsize or s.raw_size)
        a = img.np_image[s.rva: s.rva + n]
        idx = np.nonzero(a[:-4] == opcode)[0]
        if idx.size == 0:
            continue
        d = (a[idx + 1].astype(np.uint32) | (a[idx + 2].astype(np.uint32) << 8) |
             (a[idx + 3].astype(np.uint32) << 16) | (a[idx + 4].astype(np.uint32) << 24)).view(np.int32)
        t = s.rva + idx.astype(np.int64) + 5 + d.astype(np.int64)
        out += [int(s.rva + i) for i in idx[t == target]]
    return out


def string_rva(img: PEImage, text: bytes):
    out = []
    for s in img.sections:
        if s.executable or s.raw_size == 0:
            continue
        end = s.rva + min(s.raw_size, s.vsize or s.raw_size)
        i = img.image.find(b"\0" + text + b"\0", s.rva, end)
        while i >= 0:
            out.append(i + 1)
            i = img.image.find(b"\0" + text + b"\0", i + 1, end)
    return out


def riprel_refs(img: PEImage, target: int):
    """RVAs of the disp32 field of RIP-relative instructions pointing at target (lea/mov, no imm)."""
    from str_xref import code_xrefs
    hits = code_xrefs(img, np.array([target], dtype=np.int64))
    return [d for d, extra in hits.get(target, []) if extra == 4]


def auto_openlib(img: PEImage):
    loaded = string_rva(img, b"_LOADED")
    conflict = string_rva(img, b"name conflict for module '%s'")
    if not loaded or not conflict:
        return None, None
    f1 = {img.func_of(r)[0] for t in loaded for r in riprel_refs(img, t) if img.func_of(r)}
    f2 = {img.func_of(r)[0] for t in conflict for r in riprel_refs(img, t) if img.func_of(r)}
    for f in sorted(f1 & f2):
        # thunk: 45 31/33 C9 (xor r9d,r9d) E9 rel32 (jmp f)
        for j in find_rel_branches(img, f, 0xE9):
            if bytes(img.image[j - 3: j]) in (b"\x45\x31\xc9", b"\x45\x33\xc9"):
                return f, j - 3
    return (sorted(f1 & f2)[0] if f1 & f2 else None), None


class Emu:
    """Tiny linear-sweep emulator: tracks rsp offset, registers holding RIP-relative addresses,
    frame addresses (any reg = rsp+k, e.g. rbp/r11 frame pointers), zero/immediates, and 8-byte
    stores into the frame."""

    def __init__(self, img: PEImage, md):
        self.img, self.md = img, md

    @staticmethod
    def _mem(op: str, rsp: int, regs: dict):
        """Resolve '[base +/- disp]' to a frame offset, or None."""
        fm = MEM.search(op)
        if not fm or "*" in op:
            return None
        base = REG64.get(fm.group(1))
        off = num(fm.group(3)) * (1 if fm.group(2) == "+" else -1) if fm.group(2) else 0
        if base == "rsp":
            return rsp + off
        v = regs.get(base)
        if v and v[0] == "frame":
            return v[1] + off
        return None

    def run(self, fbegin: int, stop: int):
        """Linear-sweep emulate [fbegin, stop). Return (regs, stores)."""
        img = self.img
        rsp = 0
        regs: dict[str, tuple] = {}
        stores: dict[int, tuple] = {}
        code = bytes(img.image[fbegin: stop])
        for ins in self.md.disasm(code, fbegin):
            if ins.address >= stop:
                break
            m, ops = ins.mnemonic, ins.op_str
            parts = [p.strip() for p in ops.split(",")] if ops else []
            if m == "push":
                rsp -= 8
                continue
            if m == "pop":
                rsp += 8
                if parts and parts[0] in REG64:
                    regs.pop(REG64[parts[0]], None)
                continue
            if m in ("sub", "add") and parts and parts[0] == "rsp":
                try:
                    v = num(parts[1])
                    rsp += -v if m == "sub" else v
                except ValueError:
                    pass
                continue
            if m == "call":
                for r in VOLATILE:
                    regs.pop(r, None)
                continue
            if m == "xor" and len(parts) == 2 and parts[0] == parts[1] and parts[0] in REG64:
                regs[REG64[parts[0]]] = ("imm", 0)
                continue
            if m == "lea" and len(parts) == 2 and parts[0] in REG64:
                dst = REG64[parts[0]]
                mm = RIPREL.search(parts[1])
                if mm:
                    t = ins.address + ins.size + (num(mm.group(2)) * (1 if mm.group(1) == "+" else -1))
                    regs[dst] = ("abs", t)
                    continue
                fo = self._mem(parts[1], rsp, regs)
                if fo is not None:
                    regs[dst] = ("frame", fo)
                else:
                    regs.pop(dst, None)
                continue
            if m == "mov" and len(parts) == 2:
                if parts[0] in REG64:
                    dst = REG64[parts[0]]
                    if parts[1] == "rsp":
                        regs[dst] = ("frame", rsp)
                    elif parts[1] in REG64:
                        v = regs.get(REG64[parts[1]])
                        if v is None:
                            regs.pop(dst, None)
                        else:
                            regs[dst] = v
                    else:
                        try:
                            regs[dst] = ("imm", num(parts[1]))
                        except ValueError:
                            regs.pop(dst, None)
                    continue
                if parts[0].startswith("qword ptr ["):
                    fo = self._mem(parts[0], rsp, regs)
                    if fo is not None:
                        if parts[1] in REG64:
                            stores[fo] = regs.get(REG64[parts[1]], ("unk",))
                        else:
                            try:
                                stores[fo] = ("imm", num(parts[1]))
                            except ValueError:
                                stores[fo] = ("unk",)
                    continue
            # any other write to a 64-bit reg destroys knowledge
            if parts and parts[0] in REG64 and m not in ("cmp", "test"):
                regs.pop(REG64[parts[0]], None)
        return regs, stores


def read_table_frame(stores, off, img, limit=512):
    out = []
    for k in range(limit):
        n = stores.get(off + 16 * k)
        f = stores.get(off + 16 * k + 8)
        if n is None:
            out.append(("<?missing>", None))
            break
        if n == ("imm", 0):
            break
        if n[0] != "abs":
            out.append(("<?" + n[0] + ">", f[1] if f and f[0] == "abs" else None))
            continue
        name = img.cstr(n[1], 200).decode("latin-1")
        out.append((name, f[1] if f and f[0] == "abs" else None))
    return out


def read_table_static(rva, img, limit=1024):
    out = []
    for k in range(limit):
        n = img.u64(rva + 16 * k)
        f = img.u64(rva + 16 * k + 8)
        if n == 0:
            break
        out.append((img.cstr(n - img.base, 200).decode("latin-1"), f - img.base if f else None))
    return out


def binder_scan(img: PEImage, md, max_gap: int = 40):
    """Pattern pass for 'set field to C function' helpers (Fox-style bindings):
    a call whose rdx = &"name" (lea rdx,[rip+str]) and r8 = &cfunc (lea r8,[rip+code]),
    both loaded within max_gap bytes before the call. Returns list of dicts."""
    out = []
    for s in img.exec_sections():
        n = min(s.raw_size, s.vsize or s.raw_size)
        a = img.np_image[s.rva: s.rva + n]
        def rel_targets(b0, b1, b2):
            idx = np.nonzero((a[:-7] == b0) & (a[1:-6] == b1) & (a[2:-5] == b2))[0]
            d = (a[idx + 3].astype(np.uint32) | (a[idx + 4].astype(np.uint32) << 8) |
                 (a[idx + 5].astype(np.uint32) << 16) | (a[idx + 6].astype(np.uint32) << 24)).view(np.int32)
            return idx, s.rva + idx.astype(np.int64) + 7 + d.astype(np.int64)
        i8, t8 = rel_targets(0x4C, 0x8D, 0x05)      # lea r8,[rip+X]
        i2, t2 = rel_targets(0x48, 0x8D, 0x15)      # lea rdx,[rip+Y]
        ex = [(x.rva, x.rva + max(x.vsize, x.raw_size)) for x in img.exec_sections()]
        is_code = np.zeros(t8.size, dtype=bool)
        for lo, hi in ex:
            is_code |= (t8 >= lo) & (t8 < hi)
        i8, t8 = i8[is_code], t8[is_code]
        calls = np.nonzero(a[:-4] == 0xE8)[0]
        for p8, f in zip(i8.tolist(), t8.tolist()):
            # nearest following call within max_gap
            k = np.searchsorted(calls, p8 + 7)
            if k >= calls.size or calls[k] - p8 > max_gap:
                continue
            c = int(calls[k])
            # an rdx string load between (p8 - max_gap) and c
            lo = np.searchsorted(i2, p8 - max_gap)
            hi = np.searchsorted(i2, c)
            cand = [(int(i2[j]), int(t2[j])) for j in range(lo, hi)]
            name = None
            rdx_pos = None
            for pos, t in reversed(cand):
                sec = img.section_of(t)
                if sec and not sec.executable:
                    cs = img.cstr(t, 120)
                    if cs and all(32 <= ch < 127 for ch in cs):
                        name = cs.decode()
                        rdx_pos = pos
                        break
            if name is None:
                continue
            rel = int(np.frombuffer(bytes(a[c + 1: c + 5]), dtype=np.int32)[0])
            callee = s.rva + c + 5 + rel
            site = s.rva + c
            fn = img.func_of(site)
            out.append({"callee": callee, "site": site, "name": name, "cfunc": int(f),
                        "func": fn[0] if fn else None, "rdx_pos": s.rva + rdx_pos, "sec_rva": s.rva})
        # namespace guess: nearest earlier 'lea rdx,[rip+str]' in the same function that is not
        # itself part of a binder triple (e.g. the table name passed to a get/create-table helper)
        used = {b["rdx_pos"] for b in out if b["sec_rva"] == s.rva}
        i2_rva = (s.rva + i2).tolist()
        t2l = t2.tolist()
        for b in out:
            if b["sec_rva"] != s.rva or "ns" in b:
                continue
            j = bisect.bisect_left(i2_rva, b["rdx_pos"]) - 1
            b["ns"] = None
            while j >= 0 and b["func"] is not None and i2_rva[j] >= b["func"]:
                if i2_rva[j] not in used:
                    sec = img.section_of(t2l[j])
                    if sec and not sec.executable:
                        cs = img.cstr(t2l[j], 120)
                        if cs and all(32 <= ch < 127 for ch in cs):
                            b["ns"] = cs.decode()
                            break
                j -= 1
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("--openlib", type=lambda s: int(s, 0))
    ap.add_argument("--register", type=lambda s: int(s, 0))
    ap.add_argument("--tsv")
    ap.add_argument("--md")
    ap.add_argument("--libs", default="")
    ap.add_argument("--binders", help="also run the Fox-style binder pattern pass; write TSV here")
    ap.add_argument("--binder-min", type=int, default=8, help="min call count for a callee to be listed")
    a = ap.parse_args()
    img = PEImage(a.exe)
    _, md = load_capstone()
    openlib, register = a.openlib, a.register
    if openlib is None or register is None:
        o, r = auto_openlib(img)
        openlib = openlib or o
        register = register or r
    print(f"[lua] luaI_openlib={openlib and hex(openlib)} luaL_register={register and hex(register)}", file=sys.stderr)
    if register is None:
        raise SystemExit("could not locate luaL_register")
    emu = Emu(img, md)
    targets = {register: "luaL_register"}
    if openlib:
        targets[openlib] = "luaI_openlib"
    sites = []  # (callsite, target)
    frontier = list(targets)
    seen_wrappers = set()
    depth = 0
    while frontier and depth < 3:
        nxt = []
        for t in frontier:
            for c in find_rel_branches(img, t, 0xE8):
                sites.append((c, t))
                f = img.func_of(c)
                if f is None:
                    continue
                regs, _ = emu.run(f[0], c)
                # wrapper heuristic: small function whose rdx/r8 are not resolved locally
                if (f[1] - f[0] < 0x180 and "r8" not in regs and "rdx" not in regs
                        and f[0] not in seen_wrappers and f[0] not in targets):
                    seen_wrappers.add(f[0])
                    targets[f[0]] = f"wrapper@{f[0]:#x}"
                    nxt.append(f[0])
        frontier = nxt
        depth += 1
    print(f"[lua] wrappers: {[hex(w) for w in seen_wrappers]}; {len(sites)} call sites", file=sys.stderr)
    libs = []
    for c, t in sorted(sites):
        if t in seen_wrappers and False:
            pass
        f = img.func_of(c)
        if f is None or f[0] in seen_wrappers:
            continue
        regs, stores = emu.run(f[0], c)
        lib = regs.get("rdx")
        tab = regs.get("r8")
        libname = None
        if lib and lib[0] == "abs":
            libname = img.cstr(lib[1], 100).decode("latin-1")
        elif lib == ("imm", 0):
            libname = "<NULL: into table on stack top>"
        entries = []
        how = None
        if tab and tab[0] == "frame":
            entries = read_table_frame(stores, tab[1], img)
            how = "stack"
        elif tab and tab[0] == "abs":
            entries = read_table_static(tab[1], img)
            how = f"static@{tab[1]:#x}"
        libs.append({"site": c, "func": f[0], "via": targets[t], "lib": libname, "how": how, "entries": entries})

    want = {x for x in a.libs.split(",") if x}
    with open(a.tsv, "w") if a.tsv else open(os.devnull, "w") as tf:
        tf.write("lib\tfunction\tcfunc_rva\tregistered_at\tregistering_func\tvia\ttable\n")
        for L in libs:
            for name, fn in L["entries"]:
                tf.write(f"{L['lib']}\t{name}\t{'' if fn is None else hex(fn)}\t{L['site']:#x}\t{L['func']:#x}\t"
                         f"{L['via']}\t{L['how']}\t\n")
    summary = ["| library | #funcs | registered at (call site) | registering function | via | table |",
               "|---|---|---|---|---|---|"]
    for L in libs:
        summary.append(f"| `{L['lib']}` | {len(L['entries'])} | {L['site']:#x} | {L['func']:#x} | {L['via']} | {L['how']} |")
    detail = []
    for L in libs:
        if want and L["lib"] not in want:
            continue
        detail.append(f"\n#### `{L['lib']}` ({len(L['entries'])} functions; registered by {L['func']:#x} at {L['site']:#x})\n")
        detail.append(", ".join(f"`{n}`={'?' if fn is None else hex(fn)}" for n, fn in L["entries"]))
    text = (f"luaI_openlib = {openlib:#x}, luaL_register = {register:#x}; wrappers: "
            + ", ".join(hex(w) for w in sorted(seen_wrappers)) + "\n\n" + "\n".join(summary) + "\n" + "\n".join(detail) + "\n")
    if a.binders:
        from collections import Counter, defaultdict
        bl = binder_scan(img, md)
        cnt = Counter(b["callee"] for b in bl)
        keep = {c for c, k in cnt.items() if k >= a.binder_min}
        # namespace guess: last string passed in rdx to another callee earlier in the same function
        with open(a.binders, "w") as f:
            f.write("callee\tregistering_func\tsite\tnamespace_guess\tname\tcfunc_rva\n")
            for b in sorted(bl, key=lambda x: x["site"]):
                if b["callee"] in keep:
                    f.write(f"{b['callee']:#x}\t{(b['func'] or 0):#x}\t{b['site']:#x}\t{b.get('ns')}\t{b['name']}\t{b['cfunc']:#x}\n")
        text += "\n### Binder-pattern callees (call with rdx=&\"name\", r8=&cfunc)\n\n| callee | #calls | sample names |\n|---|---|---|\n"
        byc = defaultdict(list)
        for b in bl:
            byc[b["callee"]].append(b["name"])
        for c, k in cnt.most_common():
            if k < a.binder_min:
                break
            text += f"| {c:#x} | {k} | {', '.join(byc[c][:8])} |\n"
    if a.md:
        with open(a.md, "w") as f:
            f.write(text)
    else:
        print(text)


if __name__ == "__main__":
    main()
