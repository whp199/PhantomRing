#!/usr/bin/env python3
"""er_aob_scan.py - static AOB / RTTI / FD4-singleton scanner for eldenring.exe (PhantomRing).

Pure-Python (stdlib only; capstone optional for `disasm`).  Parses the PE file itself,
maps every section at its RVA, and runs byte patterns against the *file image* (no game
process needed).  Elden Ring's .text is not encrypted on disk (Arxan only adds integrity
checks and a second code section), so static scanning gives the same RVAs as a live scan.

Pattern syntax (a superset of Cheat Engine / IDA style plus a pelite subset, so the
patterns from fromsoftware-rs `mapper-profile.toml` work unmodified):
    48 8B 05           literal bytes (case-insensitive); "488B05" also accepted
    ? / ?? / ????????  wildcard: single '?' = 1 byte, a run of 2N '?' = N bytes (CE style)
    8? / ?8            nibble masks (high / low nibble fixed)
    [n] / [a-b]        skip n bytes / skip a..b bytes (backtracking)
    '                  save the cursor into the next capture slot (slot 0 = match start)
    $                  follow a rel32 at the cursor: cursor = cursor + 4 + i32
    %                  follow a rel8 at the cursor:  cursor = cursor + 1 + i8
    *                  follow an absolute 64-bit pointer (VA) at the cursor
    $ { ... }          follow, match the sub-pattern at the target, return to after the operand
    "text"             literal ASCII string
Search scope: by default all executable sections (.text, .interpr, second .text);
--section NAME restricts it.

Sub-commands (run with -h for details):
    info        version resource, sha256, sections, App version (TGA mapping 1.(10*(maj-1)+min).rel)
    scan        scan one pattern; optional RIP-relative resolution (--rip DISP[,END])
    verify      run the built-in PhantomRing catalog (the RVAs the host DLL needs); exit 1 on drift
    rtti        find vftables by RTTI class name (MSVC CompleteObjectLocator walk), --vfuncs N
    singletons  static FD4Singleton finder (TGA null-check pattern) named via DLRF RTTI / initialisers
    disasm      disassemble at an RVA (needs capstone)
    xref        find RIP-relative / rel32 references to an RVA (--decode keeps real instructions)
    fieldref    for a global pointer slot, list code that dereferences [slot]+offset (offset check)
    func        function bounds from .pdata (follows chained unwind info)
    mkpattern   generate a unique wildcarded AOB for a global slot (or --function start)

Examples:
    python3 er_aob_scan.py info
    python3 er_aob_scan.py verify                 # 100 catalog entries, all must be [OK ]
    python3 er_aob_scan.py scan "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88" --rip 3
    python3 er_aob_scan.py singletons --grep Camera
    python3 er_aob_scan.py fieldref 0x3d69ff8 0x1e508 0x1ece0
The exe: --exe, else $ER_EXE, else the Steam install (tools/game_paths.py). capstone is imported from site-packages
(pip install capstone), or from $CAPSTONE_PATH when set.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
from dataclasses import dataclass, field

CAPSTONE_FALLBACK_PATH = os.environ.get("CAPSTONE_PATH")  # optional: a capstone checkout outside site-packages


# --------------------------------------------------------------------------------------
# PE image
# --------------------------------------------------------------------------------------
@dataclass
class Section:
    name: str
    rva: int
    vsize: int
    raw_off: int
    raw_size: int
    chars: int

    @property
    def end(self) -> int:
        return self.rva + max(self.vsize, self.raw_size)

    @property
    def executable(self) -> bool:
        return bool(self.chars & 0x20000000)

    def contains(self, rva: int) -> bool:
        return self.rva <= rva < self.end


class PEImage:
    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as f:
            self.raw = f.read()
        raw = self.raw
        if raw[:2] != b"MZ":
            raise ValueError("not a PE file")
        pe_off = struct.unpack_from("<I", raw, 0x3C)[0]
        if raw[pe_off:pe_off + 4] != b"PE\0\0":
            raise ValueError("bad PE signature")
        (self.machine, nsec, self.timestamp, _, _, opt_size, _) = struct.unpack_from("<HHIIIHH", raw, pe_off + 4)
        opt = pe_off + 24
        magic = struct.unpack_from("<H", raw, opt)[0]
        if magic != 0x20B:
            raise ValueError("only PE32+ supported")
        self.entry_rva = struct.unpack_from("<I", raw, opt + 16)[0]
        self.image_base = struct.unpack_from("<Q", raw, opt + 24)[0]
        self.size_of_image = struct.unpack_from("<I", raw, opt + 56)[0]
        self.size_of_headers = struct.unpack_from("<I", raw, opt + 60)[0]
        ndirs = struct.unpack_from("<I", raw, opt + 108)[0]
        self.data_dirs = [struct.unpack_from("<II", raw, opt + 112 + 8 * i) for i in range(ndirs)]
        sec_off = opt + opt_size
        self.sections: list[Section] = []
        for i in range(nsec):
            o = sec_off + 40 * i
            name = raw[o:o + 8].rstrip(b"\0").decode("latin-1")
            vsize, rva, rsize, roff = struct.unpack_from("<IIII", raw, o + 8)
            chars = struct.unpack_from("<I", raw, o + 36)[0]
            self.sections.append(Section(name, rva, vsize, roff, rsize, chars))
        # Build the mapped image (sections at their RVAs).
        img = bytearray(self.size_of_image)
        img[0:self.size_of_headers] = raw[0:self.size_of_headers]
        for s in self.sections:
            n = min(s.raw_size, s.vsize) if s.vsize else s.raw_size
            img[s.rva:s.rva + n] = raw[s.raw_off:s.raw_off + n]
        self.img = bytes(img)

    # -- helpers ---------------------------------------------------------------------
    def section_of(self, rva: int) -> Section | None:
        for s in self.sections:
            if s.contains(rva):
                return s
        return None

    def sections_named(self, name: str) -> list[Section]:
        return [s for s in self.sections if s.name == name]

    def exec_sections(self) -> list[Section]:
        return [s for s in self.sections if s.executable]

    def u8(self, rva): return self.img[rva]
    def i8(self, rva): return struct.unpack_from("<b", self.img, rva)[0]
    def u32(self, rva): return struct.unpack_from("<I", self.img, rva)[0]
    def i32(self, rva): return struct.unpack_from("<i", self.img, rva)[0]
    def u64(self, rva): return struct.unpack_from("<Q", self.img, rva)[0]

    def va_to_rva(self, va: int) -> int | None:
        r = va - self.image_base
        return r if 0 <= r < self.size_of_image else None

    def cstr(self, rva: int, limit: int = 512) -> str | None:
        if not (0 <= rva < self.size_of_image):
            return None
        end = self.img.find(b"\0", rva, rva + limit)
        if end < 0:
            return None
        try:
            return self.img[rva:end].decode("utf-8")
        except UnicodeDecodeError:
            return self.img[rva:end].decode("latin-1")

    def wstr(self, rva: int, limit: int = 512) -> str | None:
        out = []
        for i in range(limit):
            c = struct.unpack_from("<H", self.img, rva + 2 * i)[0]
            if c == 0:
                return "".join(out)
            out.append(chr(c))
        return None

    def sha256(self) -> str:
        return hashlib.sha256(self.raw).hexdigest()

    def version_info(self) -> dict:
        """Parse VS_VERSIONINFO from the resource section (RT_VERSION = 16)."""
        res_rva, res_size = self.data_dirs[2] if len(self.data_dirs) > 2 else (0, 0)
        if not res_rva:
            return {}
        img = self.img

        def walk(dir_rva, depth, path):
            nnamed, nid = struct.unpack_from("<HH", img, dir_rva + 12)
            for i in range(nnamed + nid):
                name, off = struct.unpack_from("<II", img, dir_rva + 16 + 8 * i)
                ident = name & 0x7FFFFFFF if name & 0x80000000 else name
                if depth == 0 and not (name & 0x80000000) and ident != 16:
                    continue
                if off & 0x80000000:
                    yield from walk(res_rva + (off & 0x7FFFFFFF), depth + 1, path + [ident])
                else:
                    data_rva, size = struct.unpack_from("<II", img, res_rva + off)
                    yield path + [ident], data_rva, size

        out: dict = {}
        for path, data_rva, size in walk(res_rva, 0, []):
            blob = img[data_rva:data_rva + size]
            sig = blob.find(struct.pack("<I", 0xFEEF04BD))
            if sig >= 0:
                ms, ls, pms, pls = struct.unpack_from("<IIII", blob, sig + 8)
                out["FileVersion(fixed)"] = f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"
                out["ProductVersion(fixed)"] = f"{pms >> 16}.{pms & 0xFFFF}.{pls >> 16}.{pls & 0xFFFF}"
            # String table: scan for UTF-16 key/value pairs we care about.
            for key in ("FileVersion", "ProductVersion", "ProductName", "CompanyName"):
                k = key.encode("utf-16-le") + b"\0\0"
                p = blob.find(k)
                if p >= 0:
                    p += len(k)
                    p = (p + 3) & ~3
                    end = blob.find(b"\0\0", p)
                    while end >= 0 and (end - p) % 2:
                        end = blob.find(b"\0\0", end + 1)
                    out[key] = blob[p:end].decode("utf-16-le", "replace")
            out["resource_lang"] = path[2] if len(path) > 2 else None
            k = "Translation".encode("utf-16-le") + b"\0\0"
            p = blob.find(k)
            if p >= 0:
                p = (p + len(k) + 3) & ~3
                lang, cp = struct.unpack_from("<HH", blob, p)
                out["translation"] = f"0x{lang:04x} 0x{cp:04x}"
                # fromsoftware-rs GameVersion::detect uses (lang & 0x3ff): 0x09 = EN/WW, 0x11 = JP
                out["region"] = {0x09: "WW(EN)", 0x11: "JP"}.get(lang & 0x3FF, f"lang 0x{lang & 0x3FF:x}")
        return out


# --------------------------------------------------------------------------------------
# Pattern engine
# --------------------------------------------------------------------------------------
@dataclass
class Tok:
    kind: str            # byte, any, skip, save, follow32, follow8, followabs, group
    val: int = 0
    lo: int = 0
    hi: int = 0
    sub: list | None = None   # sub-pattern for $ { } / % { } / * { } (may be empty)


def parse_pattern(text: str) -> list[Tok]:
    toks, stack = [], []
    cur = toks
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace() or c == ",":
            i += 1
            continue
        if c == "'":
            cur.append(Tok("save")); i += 1; continue
        if c == "$":
            cur.append(Tok("follow32")); i += 1; continue
        if c == "%":
            cur.append(Tok("follow8")); i += 1; continue
        if c == "*":
            cur.append(Tok("followabs")); i += 1; continue
        if c == "{":
            if not cur or cur[-1].kind not in ("follow32", "follow8", "followabs"):
                raise ValueError("'{' must follow $, % or *")
            cur[-1].sub = []  # follow token owns the sub-pattern
            stack.append(cur)
            cur = cur[-1].sub
            i += 1
            continue
        if c == "}":
            if not stack:
                raise ValueError("unbalanced '}'")
            cur = stack.pop()
            i += 1
            continue
        if c == "[":
            j = text.index("]", i)
            body = text[i + 1:j]
            if "-" in body:
                a, b = body.split("-")
                cur.append(Tok("skip", lo=int(a, 0), hi=int(b, 0)))
            else:
                v = int(body, 0)
                cur.append(Tok("skip", lo=v, hi=v))
            i = j + 1
            continue
        if c == '"':
            j = text.index('"', i + 1)
            for b in text[i + 1:j].encode("latin-1"):
                cur.append(Tok("byte", b))
            i = j + 1
            continue
        if c == "?":
            j = i
            while j < n and text[j] == "?":
                j += 1
            run = j - i
            # "?8" = low-nibble mask (high nibble wildcard)
            if run == 1 and j < n and text[j] in "0123456789abcdefABCDEF" and \
                    (j + 1 >= n or text[j + 1] not in "0123456789abcdefABCDEF"):
                cur.append(Tok("mask", int(text[j], 16), lo=0x0F))
                i = j + 1
                continue
            cnt = 1 if run == 1 else run // 2
            for _ in range(cnt):
                cur.append(Tok("any"))
            i = j
            continue
        m = re.match(r"([0-9A-Fa-f])\?(?![?0-9A-Fa-f])", text[i:])
        if m:  # "8?" = high-nibble mask
            cur.append(Tok("mask", int(m.group(1), 16) << 4, lo=0xF0))
            i += 2
            continue
        m = re.match(r"[0-9A-Fa-f]+", text[i:])
        if m:
            hx = m.group(0)
            if len(hx) % 2:
                raise ValueError(f"odd hex token {hx!r}")
            for k in range(0, len(hx), 2):
                cur.append(Tok("byte", int(hx[k:k + 2], 16)))
            i += len(hx)
            continue
        raise ValueError(f"bad pattern char {c!r} at {i} in {text!r}")
    if stack:
        raise ValueError("unbalanced '{'")
    return toks


def _prefix_regex(toks: list[Tok]) -> tuple[bytes, int]:
    """Regex for the leading linear part (bytes / wildcards / fixed skips / saves)."""
    parts, consumed = [], 0
    for t in toks:
        if t.kind == "byte":
            parts.append(re.escape(bytes([t.val])))
        elif t.kind == "any":
            parts.append(b".")
        elif t.kind == "mask":
            vals = bytes(v for v in range(256) if v & t.lo == t.val)
            parts.append(b"[" + b"".join(re.escape(bytes([v])) for v in vals) + b"]")
        elif t.kind == "skip" and t.lo == t.hi:
            parts.append(b".{%d}" % t.lo)
        elif t.kind == "save":
            pass
        else:
            break
        consumed += 1
    return b"".join(parts), consumed


class Matcher:
    def __init__(self, pe: PEImage):
        self.pe = pe

    def _match(self, toks, idx, pos, saves):
        """Backtracking interpreter. Returns list of saves or None."""
        img = self.pe.img
        size = len(img)
        while idx < len(toks):
            t = toks[idx]
            k = t.kind
            if k == "byte":
                if pos < 0 or pos >= size or img[pos] != t.val:
                    return None
                pos += 1
            elif k == "any":
                pos += 1
            elif k == "mask":
                if pos < 0 or pos >= size or img[pos] & t.lo != t.val:
                    return None
                pos += 1
            elif k == "save":
                saves = saves + [pos]
            elif k == "skip":
                if t.lo == t.hi:
                    pos += t.lo
                else:
                    for s in range(t.lo, t.hi + 1):
                        r = self._match(toks, idx + 1, pos + s, saves)
                        if r is not None:
                            return r
                    return None
            elif k in ("follow32", "follow8", "followabs"):
                if k == "follow32":
                    if pos + 4 > size:
                        return None
                    tgt = pos + 4 + struct.unpack_from("<i", img, pos)[0]
                    after = pos + 4
                elif k == "follow8":
                    tgt = pos + 1 + struct.unpack_from("<b", img, pos)[0]
                    after = pos + 1
                else:
                    va = struct.unpack_from("<Q", img, pos)[0]
                    r = self.pe.va_to_rva(va)
                    if r is None:
                        return None
                    tgt, after = r, pos + 8
                if not (0 <= tgt < size):
                    return None
                if t.sub is not None:
                    r = self._match(t.sub, 0, tgt, saves)
                    if r is None:
                        return None
                    saves = r
                    pos = after
                else:
                    pos = tgt
            else:
                raise AssertionError(k)
            if pos < 0 or pos > size:
                return None
            idx += 1
        return saves

    def scan(self, pattern: str, sections: list[Section] | None = None, limit: int = 0):
        """Yield (match_rva, captures[list of rva]) ; captures[0] == match start."""
        toks = parse_pattern(pattern)
        rx, _ = _prefix_regex(toks)
        if not rx:
            raise ValueError("pattern must start with at least one literal byte or wildcard")
        creg = re.compile(b"(?=(" + rx + b"))", re.S)
        secs = sections if sections is not None else self.pe.exec_sections()
        found = 0
        for s in secs:
            end = s.rva + min(s.vsize, s.raw_size) if s.vsize else s.rva + s.raw_size
            for m in creg.finditer(self.pe.img, s.rva, end):
                start = m.start()
                r = self._match(toks, 0, start, [start])
                if r is not None:
                    yield start, r
                    found += 1
                    if limit and found >= limit:
                        return


def rip_target(pe: PEImage, match_rva: int, disp_off: int, insn_end: int | None = None) -> int:
    end = insn_end if insn_end is not None else disp_off + 4
    return match_rva + end + pe.i32(match_rva + disp_off)


# --------------------------------------------------------------------------------------
# RTTI (MSVC x64)
# --------------------------------------------------------------------------------------
_PRIM = {"C": "signed char", "D": "char", "E": "unsigned char", "F": "short", "G": "unsigned short",
         "H": "int", "I": "unsigned int", "J": "long", "K": "unsigned long", "M": "float", "N": "double",
         "X": "void", "_N": "bool", "_J": "__int64", "_K": "unsigned __int64", "_W": "wchar_t"}


def _msvc_number(s, i):
    neg = s[i] == "?"
    if neg:
        i += 1
    if s[i].isdigit():
        v, i = int(s[i]) + 1, i + 1
    else:
        j = s.index("@", i)
        v = 0
        for ch in s[i:j]:
            v = v * 16 + (ord(ch) - ord("A"))
        i = j + 1
    return (-v if neg else v), i


def _msvc_fragment(s, i, names):
    if s[i].isdigit():
        return names[int(s[i])], i + 1
    if s.startswith("?$", i):
        i += 2
        j = s.index("@", i)
        base = s[i:j]
        tnames = [base]
        i = j + 1
        args = []
        while s[i] != "@":
            a, i = _msvc_type(s, i, tnames)
            args.append(a)
        i += 1
        frag = f"{base}<{','.join(args)}>"
        if len(names) < 10:
            names.append(frag)
        return frag, i
    j = s.index("@", i)
    frag = s[i:j]
    if len(names) < 10:
        names.append(frag)
    return frag, j + 1


def _msvc_qualified(s, i, names):
    parts = []
    while s[i] != "@":
        part, i = _msvc_fragment(s, i, names)
        parts.append(part)
    return "::".join(reversed(parts)), i + 1


def _msvc_type(s, i, names):
    c = s[i]
    if c in "VUT":
        return _msvc_qualified(s, i + 1, names)
    if c == "W":  # enum: W4Name@@
        return _msvc_qualified(s, i + 2, names)
    if s.startswith("$0", i):
        v, i = _msvc_number(s, i + 2)
        return str(v), i
    if s.startswith("$1", i) or s.startswith("$H", i):
        raise ValueError("unsupported template arg")
    if c in "PQ":  # pointer: P E A <type>  (E = __ptr64, A/B = cv)
        j = i + 1
        if s[j] == "E":
            j += 1
        cv = s[j]
        t, k = _msvc_type(s, j + 1, names)
        return (f"{t} const*" if cv == "B" else f"{t}*"), k
    if c == "_" and s[i:i + 2] in _PRIM:
        return _PRIM[s[i:i + 2]], i + 2
    if c in _PRIM:
        return _PRIM[c], i + 1
    raise ValueError(f"unsupported type code {c!r}")


def demangle_type(name: str) -> str:
    """MSVC type-descriptor demangler (subset): '.?AVChrIns@CS@@' -> 'CS::ChrIns',
    '.?AV?$PadDevice@VDLMultiThreadingPolicy@DLKR@@@DLUID@@' ->
    'DLUID::PadDevice<DLKR::DLMultiThreadingPolicy>' (same spelling as undname NAME_ONLY,
    which fromsoftware-rs mapper-profile.toml uses).  Falls back to the raw string."""
    if not name.startswith(".?A") or len(name) < 5:
        return name
    try:
        out, _ = _msvc_qualified(name, 4, [])
        return out
    except (ValueError, IndexError):
        return name


def rtti_classes(pe: PEImage):
    """Yield dicts {name, mangled, vftable_rva, col_rva, offset} for every vftable."""
    rdata = pe.sections_named(".rdata")
    if not rdata:
        return
    rd = rdata[0]
    execs = pe.exec_sections()
    base = pe.image_base
    img = pe.img
    lo, hi = rd.rva, rd.rva + rd.vsize
    seen_td: dict[int, str | None] = {}
    for off in range(lo, hi - 16, 8):
        meta_va, first_va = struct.unpack_from("<QQ", img, off)
        meta = meta_va - base
        if not (lo <= meta < hi):
            continue
        first = first_va - base
        if not any(s.contains(first) for s in execs):
            continue
        sig, offset, cd_off, td_rva, chd_rva, self_rva = struct.unpack_from("<IIIIII", img, meta)
        if sig != 1 or self_rva != meta:
            continue
        if td_rva not in seen_td:
            seen_td[td_rva] = pe.cstr(td_rva + 16, 1024)
        mangled = seen_td[td_rva]
        if not mangled or not mangled.startswith(".?A"):
            continue
        yield {
            "name": demangle_type(mangled),
            "mangled": mangled,
            "vftable_rva": off + 8,
            "col_rva": meta,
            "offset": offset,
        }


# --------------------------------------------------------------------------------------
# FD4 singleton finder: technique and null-check byte pattern from TGA's fd4_singleton.h (Elden-Ring-CT-TGA),
# re-implemented statically (THIRD-PARTY-NOTICES.md)
# --------------------------------------------------------------------------------------
FD4_NULLCHECK = ("48 8b ? ? ? ? ? 48 85 ? 75 2e 48 8d 0d ? ? ? ? e8 ? ? ? ? 4c 8b c8 "
                 "4c 8d 05 ? ? ? ? ba ? ? 00 00 48 8d 0d ? ? ? ? e8 ? ? ? ?")


def fd4_singletons(pe: PEImage, rtti_rows=None):
    """Return {static_slot_rva: {name, impl, tag_rva, sites}} for every FD4Singleton.

    Technique and null-check byte pattern from TGA's fd4_singleton.h (Elden-Ring-CT-TGA), re-implemented
    statically.  Every FD4Singleton<T>::instance() null check is
        mov reg,[slot]; test reg,reg; jnz +2e; lea rcx,[tag]; call get_singleton_name; ...
        lea r8,["%s:未初期化のシングルトンにアクセスしました"]; mov edx,line; lea rcx,["...FD4Singleton.h"]
    At runtime TGA calls get_singleton_name(tag).  Statically we recover the name from RTTI:
    the DLRF runtime class of each singleton is
        DLRF::DLRuntimeClassImpl<FD4::FD4Singleton<NS::Name, NS::NameImp>, 0>
    whose vftable slot 3 is `lea rax,[tag]; ret` -> map tag -> RTTI name."""
    m = Matcher(pe)
    data = pe.sections_named(".data")[0]
    text_secs = pe.exec_sections()
    rdata = pe.sections_named(".rdata")[0]
    out: dict[int, dict] = {}
    for rva, _ in m.scan(FD4_NULLCHECK):
        slot = rva + 7 + pe.i32(rva + 3)
        tag = rva + 19 + pe.i32(rva + 15)
        fn = rva + 24 + pe.i32(rva + 20)
        path = rva + 46 + pe.i32(rva + 42)
        if not data.contains(slot) or not data.contains(tag):
            continue
        if not any(s.contains(fn) for s in text_secs) or not rdata.contains(path):
            continue
        p = pe.cstr(path, 256) or ""
        if not p.endswith("FD4Singleton.h"):
            continue
        e = out.setdefault(slot, {"tag_rva": tag, "sites": [], "name": None, "impl": None})
        e["sites"].append(rva)
    # tag -> name via DLRuntimeClassImpl<FD4Singleton<...>> RTTI
    tag_names = {}
    rows = rtti_rows if rtti_rows is not None else rtti_classes(pe)
    rx = re.compile(r"FD4Singleton@V(\w+)@(\w+)@@V(\w+)@")
    for c in rows:
        mg = c["mangled"]
        if "DLRuntimeClassImpl" not in mg or "FD4Singleton" not in mg:
            continue
        mm = rx.search(mg)
        if not mm:
            continue
        f3 = pe.va_to_rva(pe.u64(c["vftable_rva"] + 8 * 3))
        if f3 is None or pe.img[f3:f3 + 3] != b"\x48\x8d\x05" or pe.img[f3 + 7] != 0xC3:
            continue
        tag = f3 + 7 + pe.i32(f3 + 3)
        tag_names[tag] = (f"{mm.group(2)}::{mm.group(1)}", mm.group(3))
    for e in out.values():
        if e["tag_rva"] in tag_names:
            e["name"], e["impl"] = tag_names[e["tag_rva"]]
    # Fallback for singletons whose DLRuntimeClass vftable has no RTTI (FD4 library classes such
    # as FD4PadManager): the DLRF static initialiser is
    #   mov rax,[rc_global]; test rax,rax; jnz +5; call get_or_create_rc; lea rcx,["Name"];
    #   mov [rax+0x38],rcx
    # and get_or_create_rc (or a callee) does `lea reg,[rc_vftable]`, whose slot 3 is the tag getter.
    missing = {e["tag_rva"]: e for e in out.values() if not e["name"]}
    if missing:
        rd = rdata

        def tag_of_vtable(vt):
            f = pe.va_to_rva(pe.u64(vt + 0x18))
            if f is None or pe.img[f:f + 3] != b"\x48\x8d\x05" or pe.img[f + 7] != 0xC3:
                return None
            return f + 7 + pe.i32(f + 3)

        def scan_fn(fn, depth, seen):
            res = set()
            if depth < 0 or fn in seen:
                return res
            seen.add(fn)
            b = pe.img[fn:fn + 0x120]
            for i in range(len(b) - 7):
                if b[i] in (0x48, 0x4C) and b[i + 1] == 0x8D and (b[i + 2] & 0xC7) == 0x05:
                    x = fn + i + 7 + struct.unpack_from("<i", b, i + 3)[0]
                    if rd.contains(x) and rd.contains(x + 0x18):
                        t = tag_of_vtable(x)
                        if t is not None:
                            res.add(t)
                elif b[i] == 0xE8:
                    tgt = fn + i + 5 + struct.unpack_from("<i", b, i + 1)[0]
                    sec = pe.section_of(tgt)
                    if sec and sec.executable:
                        res |= scan_fn(tgt, depth - 1, seen)
                elif b[i] == 0xC3 and i > 8:
                    break
            return res

        init = "48 8b 05 $ { ' } 48 85 c0 75 05 e8 $ { ' } 48 8d 0d $ { ' } 48 89 48 38"
        for _, caps in m.scan(init):
            name = pe.cstr(caps[3], 128)
            for t in scan_fn(caps[2], 2, set()):
                if t in missing and not missing[t]["name"]:
                    missing[t]["name"] = name
                    missing[t]["impl"] = "(no RTTI; named from DLRF initialiser)"
    return out


# --------------------------------------------------------------------------------------
# Built-in catalog: everything the PhantomRing ER host DLL needs.  Each entry is checked
# by `verify`; "expect" values are this exe's RVAs (2.7.1.0 / App 1.17.1,
# sha256 1a354710...7891) so a silent pattern drift is caught.
#   resolve: "match"            -> RVA of the match start (function start / code site)
#            ("rip", d, e)      -> RIP-relative target of disp at match+d, insn end match+e
#            ("cap", i)         -> pelite capture slot i
#            ("call", d)        -> target of E8 rel32 at match+d
# --------------------------------------------------------------------------------------
CATALOG: list[dict] = []   # filled in at the bottom of this file (see _build_catalog)


def resolve_entry(pe: PEImage, e: dict, rva: int, caps: list[int]) -> int:
    r = e.get("resolve", "match")
    if r == "match":
        return rva + e.get("offset", 0)
    if r[0] == "rip":
        return rip_target(pe, rva, r[1], r[2])
    if r[0] == "cap":
        return caps[r[1]]
    if r[0] == "call":
        return rva + r[1] + 5 + pe.i32(rva + r[1] + 1)
    raise ValueError(r)


def run_verify(pe: PEImage, names: list[str] | None = None):
    m = Matcher(pe)
    results = []
    rtti = None
    singles = None
    for e in CATALOG:
        if names and e["name"] not in names:
            continue
        res = {"name": e["name"], "kind": e.get("kind", "?"), "source": e.get("source", "")}
        if "singleton" in e:
            if singles is None:
                if rtti is None:
                    rtti = {}
                    for c in rtti_classes(pe):
                        rtti.setdefault(c["name"], []).append(c)
                rows = [c for v in rtti.values() for c in v]
                singles = {}
                for slot, v in fd4_singletons(pe, rows).items():
                    singles.setdefault(v["name"], []).append((slot, len(v["sites"])))
            cands = sorted(singles.get(e["singleton"], []), key=lambda t: -t[1])
            res["pattern"] = f"FD4Singleton finder: {e['singleton']}"
            res["matches"] = len(cands)
            # a name can have a stray duplicate slot with few sites; take the dominant one
            res["rva"] = cands[0][0] if cands else None
            res["sites"] = [c[1] for c in cands]
        elif "rtti" in e:
            if rtti is None:
                rtti = {}
                for c in rtti_classes(pe):
                    rtti.setdefault(c["name"], []).append(c)
            hits = [c for c in rtti.get(e["rtti"], []) if c["offset"] == e.get("rtti_offset", 0)]
            res["pattern"] = f"RTTI {e['rtti']}"
            res["matches"] = len(hits)
            res["rva"] = hits[0]["vftable_rva"] if len(hits) == 1 else None
            if res["rva"] is not None and "vfunc" in e:
                res["vfuncs"] = {
                    k: pe.u64(res["rva"] + 8 * idx) - pe.image_base for k, idx in e["vfunc"].items()
                }
        else:
            hits = list(m.scan(e["pattern"]))
            res["pattern"] = e["pattern"]
            res["matches"] = len(hits)
            res["sites"] = [h[0] for h in hits[:8]]
            if hits:
                targets = sorted({resolve_entry(pe, e, h[0], h[1]) for h in hits})
                res["targets"] = targets[:8]
                res["rva"] = targets[0] if len(targets) == 1 else None
                # several sites resolving to the same global is fine when allowed
                if len(hits) > 1 and len(targets) == 1 and not e.get("multi_ok"):
                    res["rva"] = None
            else:
                res["rva"] = None
        exp = e.get("expect")
        res["expect"] = exp
        if e.get("kind") == "count":
            res["rva"] = res.get("targets", [None])[0]
            res["ok"] = res["matches"] == e["expect_matches"]
        else:
            res["ok"] = res["rva"] is not None and (exp is None or res["rva"] == exp)
        res["fields"] = e.get("fields")
        res["notes"] = e.get("notes", "")
        results.append(res)
    return results


def function_bounds(pe: PEImage, rva: int):
    """(begin, end) of the function containing `rva` from .pdata (RUNTIME_FUNCTION), following
    UNW_FLAG_CHAININFO chains back to the primary entry.  None if not covered."""
    import bisect
    if not hasattr(pe, "_pdata"):
        exc_rva, exc_size = pe.data_dirs[3]
        n = exc_size // 12
        ents = [struct.unpack_from("<III", pe.img, exc_rva + 12 * i) for i in range(n)]
        ents.sort()
        pe._pdata = (ents, [e[0] for e in ents])
    ents, begins = pe._pdata
    i = bisect.bisect_right(begins, rva) - 1
    if i < 0:
        return None
    b, e, u = ents[i]
    if not (b <= rva < e):
        return None
    end = e
    for _ in range(16):  # follow chained unwind info to the primary function entry
        ver_flags = pe.img[u]
        if not (ver_flags >> 3) & 0x4:
            break
        ncodes = pe.img[u + 2]
        chain = u + 4 + 2 * (ncodes + (ncodes & 1))
        b, _e, u = struct.unpack_from("<III", pe.img, chain)
    return b, end


def rip_loads(pe: PEImage, slot: int):
    """Yield (insn_rva, insn_len, reg_index) for every `mov r64,[rip+disp]` that loads `slot`."""
    rx = re.compile(rb"(?=([\x48\x4c]\x8b[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]))", re.S)
    img = pe.img
    for s in pe.exec_sections():
        end = s.rva + min(s.vsize, s.raw_size)
        for mm in rx.finditer(img, s.rva, end):
            p = mm.start()
            if p + 7 + struct.unpack_from("<i", img, p + 3)[0] == slot:
                reg = ((img[p + 2] >> 3) & 7) + (8 if img[p] == 0x4C else 0)
                yield p, 7, reg


def field_refs(pe: PEImage, slot: int, offsets: list[int], window: int = 10):
    """For every load of the global pointer at `slot`, decode the next `window` instructions and
    report memory operands [loaded_reg + off] for off in `offsets` (until the register is
    overwritten).  Used to VERIFY singleton field offsets against real game code."""
    cs = _capstone()
    from capstone import x86 as X  # noqa
    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_64)
    md.detail = True
    reg_ids = [X.X86_REG_RAX, X.X86_REG_RCX, X.X86_REG_RDX, X.X86_REG_RBX, X.X86_REG_RSP, X.X86_REG_RBP,
               X.X86_REG_RSI, X.X86_REG_RDI, X.X86_REG_R8, X.X86_REG_R9, X.X86_REG_R10, X.X86_REG_R11,
               X.X86_REG_R12, X.X86_REG_R13, X.X86_REG_R14, X.X86_REG_R15]
    hits = {o: [] for o in offsets}
    loads = 0
    for p, ln, reg in rip_loads(pe, slot):
        loads += 1
        r = reg_ids[reg]
        n = 0
        for ins in md.disasm(pe.img[p + ln:p + ln + 0x60], p + ln):
            n += 1
            if n > window:
                break
            for op in ins.operands:
                if op.type == X.X86_OP_MEM and op.mem.base == r and op.mem.index == 0 and op.mem.disp in hits:
                    hits[op.mem.disp].append(f"{ins.address:#x}: {ins.mnemonic} {ins.op_str}")
            _, written = ins.regs_access()
            if r in written or ins.mnemonic in ("ret", "jmp", "call"):
                break
    return loads, hits


def make_pattern_at(pe: PEImage, start: int, length: int):
    """Byte pattern for [start, start+length) with every RIP-relative disp / rel32 branch operand
    wildcarded (capstone-decoded), so it survives relinking of unrelated code."""
    cs = _capstone()
    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_64)
    md.detail = True
    toks = []
    for ins in md.disasm(pe.img[start:start + length + 16], start):
        b = list(ins.bytes)
        mask = [True] * len(b)
        dispoff = ins.disp_offset if hasattr(ins, "disp_offset") else 0
        if "rip" in ins.op_str and dispoff:
            for k in range(dispoff, dispoff + 4):
                mask[k] = False
        if (ins.mnemonic == "call" or ins.mnemonic.startswith("j")) and ins.op_str.startswith("0x") \
                and len(b) >= 5:
            for k in range(len(b) - 4, len(b)):
                mask[k] = False
        for byte, keep in zip(b, mask):
            toks.append(f"{byte:02x}" if keep else "??")
        if ins.address + ins.size - start >= length:
            break
    return " ".join(toks)


def make_unique_pattern(pe: PEImage, sites: list[int], min_len: int = 12, max_len: int = 40):
    """Shortest unique pattern starting at one of `sites` (returns (pattern, site) or (None, None)).
    Sites directly followed by the FD4Singleton null-check (`test r,r; jnz +2e`) are tried last,
    and patterns without wildcarded branches are preferred (less fragile across patches)."""
    m = Matcher(pe)
    img = pe.img

    def is_nullcheck(site):
        return img[site + 7] in (0x48, 0x4D) and img[site + 8] == 0x85 and img[site + 10:site + 12] == b"\x75\x2e"

    ordered = [x for x in sites if not is_nullcheck(x)] + [x for x in sites if is_nullcheck(x)]
    best = None
    for site in ordered[:24]:
        for L in range(min_len, max_len + 1, 6):
            pat = make_pattern_at(pe, site, L)
            if len(list(m.scan(pat, limit=2))) == 1:
                score = (pat.count("e9 ??") + pat.count("e8 ??"), len(pat))
                if best is None or score < best[2]:
                    best = (pat, site, score)
                break
        if best is not None and best[2][0] == 0 and len(best[0]) <= 3 * 16:
            break
    return (best[0], best[1]) if best else (None, None)


# --------------------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------------------
def _hex(v):
    return None if v is None else f"0x{v:x}"


def cmd_info(pe: PEImage, a):
    vi = pe.version_info()
    info = {
        "path": pe.path,
        "sha256": pe.sha256(),
        "size": len(pe.raw),
        "image_base": _hex(pe.image_base),
        "size_of_image": _hex(pe.size_of_image),
        "entry_rva": _hex(pe.entry_rva),
        "timestamp": pe.timestamp,
        "version": vi,
        "sections": [
            {"name": s.name, "rva": _hex(s.rva), "vsize": _hex(s.vsize), "raw_off": _hex(s.raw_off),
             "raw_size": _hex(s.raw_size), "exec": s.executable} for s in pe.sections],
    }
    fv = vi.get("FileVersion(fixed)")
    if fv:
        major, minor, rel, _ = (int(x) for x in fv.split("."))
        info["app_version"] = f"1.{10 * (major - 1) + minor}.{rel}"  # TGA CT getGameVersion() mapping
    print(json.dumps(info, indent=2))


def _scan_sections(pe, a):
    if a.section:
        secs = [s for s in pe.sections if s.name == a.section]
        if not secs:
            sys.exit(f"no section {a.section}")
        return secs
    if a.all_sections:
        return pe.sections
    return None


def cmd_scan(pe: PEImage, a):
    m = Matcher(pe)
    hits = list(m.scan(a.pattern, _scan_sections(pe, a), limit=a.max))
    out = []
    for rva, caps in hits:
        d = {"rva": _hex(rva), "section": (pe.section_of(rva) or Section("?", 0, 0, 0, 0, 0)).name,
             "captures": [_hex(c) for c in caps[1:]]}
        if a.rip:
            parts = [int(x, 0) for x in a.rip.split(",")]
            tgt = rip_target(pe, rva, parts[0], parts[1] if len(parts) > 1 else None)
            d["rip_target"] = _hex(tgt)
            sec = pe.section_of(tgt)
            d["rip_target_section"] = sec.name if sec else None
        out.append(d)
    print(json.dumps({"pattern": a.pattern, "matches": len(hits), "unique": len(hits) == 1, "hits": out},
                     indent=2))


def cmd_verify(pe: PEImage, a):
    res = run_verify(pe, a.names or None)
    if a.json:
        def conv(o):
            if isinstance(o, dict):
                return {k: conv(v) for k, v in o.items()}
            if isinstance(o, list):
                return [conv(v) for v in o]
            if isinstance(o, int) and not isinstance(o, bool) and o > 0xFFFF:
                return _hex(o)
            return o
        print(json.dumps(conv(res), indent=2))
    else:
        bad = 0
        for r in res:
            flag = "OK " if r["ok"] else "BAD"
            bad += not r["ok"]
            extra = ""
            if r.get("vfuncs"):
                extra = " " + " ".join(f"{k}={_hex(v)}" for k, v in r["vfuncs"].items())
            exp = "" if r["expect"] is None else f" expect={_hex(r['expect'])}"
            print(f"[{flag}] {r['name']:<40} rva={_hex(r['rva'])!s:<11} matches={r['matches']}{exp}{extra}")
        print(f"\n{len(res) - bad}/{len(res)} entries resolved uniquely and match expectations")
        if bad:
            sys.exit(1)


def cmd_rtti(pe: PEImage, a):
    rows = []
    for c in rtti_classes(pe):
        if a.grep:
            if a.grep.lower() not in c["name"].lower() and a.grep.lower() not in c["mangled"].lower():
                continue
        elif a.name and c["name"] != a.name:
            continue
        row = {k: (_hex(v) if isinstance(v, int) and k != "offset" else v) for k, v in c.items()}
        if a.vfuncs:
            row["vfuncs"] = [_hex(pe.u64(c["vftable_rva"] + 8 * i) - pe.image_base) for i in range(a.vfuncs)]
        rows.append(row)
    print(json.dumps(rows, indent=2))


def cmd_singletons(pe: PEImage, a):
    s = fd4_singletons(pe)
    rows = sorted(({"slot_rva": _hex(k), "name": v["name"], "impl": v["impl"], "tag_rva": _hex(v["tag_rva"]),
                    "null_check_sites": len(v["sites"])} for k, v in s.items()), key=lambda r: (r["name"] or "~"))
    if a.grep:
        rows = [r for r in rows if r["name"] and a.grep.lower() in r["name"].lower()]
    print(json.dumps(rows, indent=2))


def _capstone():
    try:
        import capstone  # noqa
    except ImportError:
        if not CAPSTONE_FALLBACK_PATH:
            raise SystemExit("capstone is needed for this command: pip install capstone (or set CAPSTONE_PATH)")
        sys.path.append(CAPSTONE_FALLBACK_PATH)
        import capstone  # noqa
    return capstone


def disasm_lines(pe: PEImage, rva: int, count: int = 30, nbytes: int = 0x400):
    cs = _capstone()
    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_64)
    out = []
    for ins in md.disasm(pe.img[rva:rva + nbytes], rva):
        out.append(f"{ins.address:#010x}: {ins.bytes.hex(' '):<32} {ins.mnemonic} {ins.op_str}")
        if len(out) >= count:
            break
    return out


def cmd_disasm(pe: PEImage, a):
    print("\n".join(disasm_lines(pe, int(a.rva, 0), a.count)))


def cmd_xref(pe: PEImage, a):
    """Find RIP-relative operands (any instruction with a disp32 that resolves to target)."""
    target = int(a.rva, 0)
    img = pe.img
    hits = []
    for s in pe.exec_sections():
        end = s.rva + min(s.vsize, s.raw_size)
        # every 4-byte window is a candidate disp32/rel32; it refers to `target` when
        # p + 4 + imm_after + disp == target (imm_after = bytes of immediate after the disp, 0..4)
        mv = memoryview(img)[s.rva:end]
        n = len(mv)
        # i32 array at all 4 alignments
        for align in range(4):
            cnt = (n - align) // 4
            arr = struct.unpack_from(f"<{cnt}i", mv, align)
            base = s.rva + align
            for idx, d in enumerate(arr):
                p = base + 4 * idx
                delta = target - (p + 4) - d
                if 0 <= delta <= 4:
                    hits.append((p, delta))
    out = []
    cs = _capstone() if a.decode else None
    for p, delta in sorted(hits):
        row = {"disp_rva": _hex(p), "imm_after": delta}
        if cs:
            md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_64)
            for back in range(7, 0, -1):  # longest valid decode wins (keeps REX/legacy prefixes)
                start = p - back
                ins = next(md.disasm(img[start:start + 16], start), None)
                is_branch = ins is not None and (ins.mnemonic == "call" or ins.mnemonic.startswith("j")) \
                    and ins.op_str.startswith("0x") and int(ins.op_str, 16) == target
                if ins and ins.address + ins.size == p + 4 + delta and ("rip" in ins.op_str or is_branch):
                    row["insn"] = f"{ins.address:#x}: {ins.mnemonic} {ins.op_str}"
                    break
            if "insn" not in row:
                continue
        out.append(row)
        if a.max and len(out) >= a.max:
            break
    print(json.dumps({"target": _hex(target), "refs": len(out), "hits": out}, indent=2))


def cmd_func(pe: PEImage, a):
    r = int(a.rva, 0)
    fb = function_bounds(pe, r)
    print(json.dumps({"rva": _hex(r), "function_begin": _hex(fb[0]) if fb else None,
                      "fragment_end": _hex(fb[1]) if fb else None}, indent=2))


def cmd_mkpattern(pe: PEImage, a):
    tgt = int(a.target, 0)
    if a.function:
        pat, site = make_unique_pattern(pe, [tgt], a.min_len, a.max_len)
        print(json.dumps({"target": _hex(tgt), "pattern": pat, "resolve": "match"}, indent=2))
        return
    sites = [p for p, _, _ in rip_loads(pe, tgt)]
    pat, site = make_unique_pattern(pe, sites, a.min_len, a.max_len)
    print(json.dumps({"target": _hex(tgt), "load_sites": len(sites), "pattern": pat, "site": _hex(site),
                      "resolve": "rip disp at +3, insn end +7 (mov r64,[rip+disp32])"}, indent=2))


def cmd_fieldref(pe: PEImage, a):
    offs = [int(x, 0) for x in a.offsets]
    loads, hits = field_refs(pe, int(a.slot, 0), offs, a.window)
    print(json.dumps({"slot": a.slot, "rip_loads": loads,
                      "offsets": {_hex(o) if o > 9 else str(o): {"count": len(v), "examples": v[:a.examples]}
                                  for o, v in hits.items()}}, indent=2))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", help="path to eldenring.exe (default $ER_EXE, else the Steam install)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    p = sub.add_parser("scan")
    p.add_argument("pattern")
    p.add_argument("--rip", help="DISP_OFF[,INSN_END]: resolve rel32 at match+DISP_OFF (insn end default DISP_OFF+4)")
    p.add_argument("--section")
    p.add_argument("--all-sections", action="store_true")
    p.add_argument("--max", type=int, default=0)
    p = sub.add_parser("verify")
    p.add_argument("names", nargs="*")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("rtti")
    p.add_argument("name", nargs="?")
    p.add_argument("--grep")
    p.add_argument("--vfuncs", type=int, default=0)
    p = sub.add_parser("singletons")
    p.add_argument("--grep")
    p = sub.add_parser("disasm")
    p.add_argument("rva")
    p.add_argument("--count", type=int, default=30)
    p = sub.add_parser("xref")
    p.add_argument("rva")
    p.add_argument("--decode", action="store_true", help="keep only hits that decode as rip-relative insns (capstone)")
    p.add_argument("--max", type=int, default=200)
    p = sub.add_parser("mkpattern", help="generate a unique AOB for a global slot (or --function start)")
    p.add_argument("target")
    p.add_argument("--function", action="store_true", help="target is a function start, not a global")
    p.add_argument("--min-len", type=int, default=12)
    p.add_argument("--max-len", type=int, default=48)
    p = sub.add_parser("func", help="function containing an RVA (.pdata)")
    p.add_argument("rva")
    p = sub.add_parser("fieldref", help="verify [global_ptr]+offset uses in code")
    p.add_argument("slot", help="RVA of the static pointer (e.g. 0x3d69ff8 WorldChrMan)")
    p.add_argument("offsets", nargs="+")
    p.add_argument("--window", type=int, default=10)
    p.add_argument("--examples", type=int, default=3)
    a = ap.parse_args(argv)
    if not a.exe:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import game_paths
        a.exe = game_paths.require(game_paths.er_exe(), "eldenring.exe", "ER_EXE (or --exe)")
    pe = PEImage(a.exe)
    {"info": cmd_info, "scan": cmd_scan, "verify": cmd_verify, "rtti": cmd_rtti,
     "singletons": cmd_singletons, "disasm": cmd_disasm, "xref": cmd_xref,
     "fieldref": cmd_fieldref, "func": cmd_func, "mkpattern": cmd_mkpattern}[a.cmd](pe, a)



# ======================================================================================
# PhantomRing catalog.  `expect` = RVA in eldenring.exe 2.7.1.0 (App 1.17.1, WW/EN),
# sha256 1a3547101327f65d0c76da2f9190ac0aa66871ea42bae2aecc61e11a8b597891.
# Sources: "CT" = TGA Cheat Table (elden-ring-ct-src .cea, v1.19.1 for App 1.17.1);
#          "fsrs" = fromsoftware-rs mapper-profile.toml / rva tables;
#          "PR" = found for PhantomRing by disassembly of this exe (see docs/research/elden-ring-runtime-re.md).
# ======================================================================================
def _g(name, pattern, expect, source, rip=(3, 7), fields=None, notes="", offset=None, multi_ok=False):
    e = {"name": name, "kind": "global", "pattern": pattern, "expect": expect, "source": source,
         "resolve": ("rip", rip[0], rip[1]), "fields": fields, "notes": notes, "multi_ok": multi_ok}
    return e


def _f(name, pattern, expect, source, resolve="match", offset=0, fields=None, notes=""):
    return {"name": name, "kind": "function", "pattern": pattern, "expect": expect, "source": source,
            "resolve": resolve, "offset": offset, "fields": fields, "notes": notes}


def _s(name, singleton, expect, notes=""):
    return {"name": name, "kind": "singleton", "singleton": singleton, "expect": expect,
            "source": "FD4Singleton null-check finder (TGA fd4_singleton.h technique) + DLRF RTTI names",
            "notes": notes}


def _v(name, cls, expect, notes="", vfunc=None):
    e = {"name": name, "kind": "vftable", "rtti": cls, "expect": expect, "source": "MSVC RTTI", "notes": notes}
    if vfunc:
        e["vfunc"] = vfunc
    return e


CATALOG.extend([
    # ---- static singleton pointers (value at RVA is the instance pointer; may be null early) ----
    _g("WorldChrMan", "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88", 0x3D69FF8, "CT baseData (commented) / fsrs singleton",
       fields="+0x10EE0 player_chr_set(ChrSet<PlayerIns>, entries* @+0x18); +0x10F90 summon_buddy_chr_set; "
              "+0x11040 open_field_chr_set; +0x1CC58 chr_set_holder_count; +0x1CC60 chr_set_holders[196] (0x18 each); "
              "+0x1DED8 chr_sets[196] (ChrSet*); +0x1E508 main_player (PlayerIns*); +0x1ECE0 chr_cam (ChrCam*)"),
    _g("GameDataMan", "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3", 0x3D61F98, "CT baseData",
       fields="+0x08 main_player_game_data (PlayerGameData*)"),
    _g("GameMan", "48 8B 05 ?? ?? ?? ?? 80 B8 ?? ?? ?? ?? 0D 0F 94 C0 C3", 0x3D6D988, "CT baseData",
       fields="+0x10 warp_requested; +0xBC4 (CT freecam: 1 while debug-cam active); +0xD60 party_member_info"),
    _g("FieldArea", "48 8B 3D ???????? 49 8B D8 48 8B F2 4C 8B F1 48 85 FF", 0x3D6D248, "CT baseData",
       fields="+0x20 GameRend*: +0x18/+0x20/+0x28 CSPersCam0/1/2, +0xC8 free-cam mode (int), +0xD0 CSDebugCam*"),
    _g("SoloParamRepository", "48 89 5C 24 48 8B FA 48 8B D9 C7 44 24 20 00 00 00 00 48", 0x3D85F58,
       "CT baseData (offset 18)", rip=(21, 25)),
    _g("CSMenuMan", "48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24", 0x3D6F820, "CT baseData",
       fields="+0x80 popup_menu*; +0x90 ui_states[0x46] (bit0 created, bit1 visible); +0x13C disable_save_menu"),
    _g("CSFlipper", "48 8B 0D ?? ?? ?? ?? 80 BB ?? ?? ?? ?? 00 0F 84", 0x458DB58, "CT baseData",
       fields="+0x264 last_frame_time; +0x268 task_delta (s); +0x2CC game_speed (dt = task_delta*game_speed)"),
    _g("CSRegulationManager", "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 0B 4C 8B C0 48 8B D7", 0x3D8ACD8, "CT baseData"),
    _g("DmgMan", "48 8B 05 ?? ?? ?? ?? 49 8B D9 49 8B F8 48 8B F2 48 85 C0 75 2E", 0x3D6A3E8, "CT baseData (DamageCtrl)"),
    _g("CSWindow", "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 48 83 C1 ?? 48 8D 45", 0x458F910, "CT baseData",
       fields="+0x08 HWND; +0x28/+0x2C screen width/height"),
    _g("WorldChrManDbg", "48 8B 0D ?? ?? ?? ?? 89 5C 24 20 48 85 C9 74 12 B8 ?? ?? ?? ?? 8B D8", 0x3D6A208, "CT baseData"),
    _g("CSLuaEventMan", "48 83 3D ?? ?? ?? ?? 00 48 8B F9 0F 84 ?? ?? ?? ?? 48", 0x3D6BEB8, "CT baseData", rip=(3, 8)),
    _g("CSEventFlagMan", "48 8B 3D ???????? 48 85 FF 74 ?? 48 8B 49", 0x3D6C4B8, "CT baseData (CSFD4VirtualMemoryFlag)"),
    _g("CSNetMan", "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 5E 48 8B 89 ?? ?? ?? ?? B2 01", 0x3D5EEC0, "CT baseData"),
    _g("CSCamera", "48 8b 0d ?? ?? ?? ?? 48 8d 97 e0 00 00 00", 0x3D669C8, "PR mkpattern (this exe only)",
       fields="+0x08 pers_cam_1 (final render cam); +0x10 pers_cam_2; +0x18 pers_cam_3; +0x20 pers_cam_4; +0x28 camera_mask"),
    _g("CSTask(CSTaskImp)", "48 8b 0d ?? ?? ?? ?? 4c 8b c3 ba 9c 00 00 00", 0x458FE88, "PR mkpattern (this exe only)"),
    _g("CSTaskGroup", "48 8b 0d ?? ?? ?? ?? 49 8b 1f ba 32 00 00 00", 0x458FF08, "PR mkpattern (this exe only)"),
    _g("FD4PadManager", "48 8b 0d ?? ?? ?? ?? e9 ?? ?? ?? ?? b0 01 c3 90 8b d1", 0x4861D30, "PR mkpattern (this exe only)",
       fields="+0x48 pad_entry_map_list (DLFixedVector<DLMap*,4>); +0x2F8 exit_foreground_signaled; +0x2F9 is_back_ground_window"),
    _g("CSHavokMan", "48 8b 0d ?? ?? ?? ?? 48 8b 9d 80 00 00 00", 0x3D7A0D0, "PR mkpattern (this exe only)",
       fields="+0x98 CSPhysWorld* (arg0 of cs_phys_world_cast_ray)"),
    _g("CSGraphics", "48 8b 05 ?? ?? ?? ?? 48 8b 48 58 48 85 c9", 0x3D75CB8, "PR mkpattern (this exe only)"),
    _g("RendMan", "48 8b 05 ?? ?? ?? ?? 48 8b 48 10 48 85 c9 75 0d 48 8b cb", 0x3D7F130, "PR mkpattern (this exe only)"),
    _g("CSFade", "48 8b 0d ?? ?? ?? ?? 0f 10 00 0f 11 44 24 40", 0x3D763B0, "PR mkpattern (this exe only)"),
    _g("CSNowLoadingHelper", "48 8b 0d ?? ?? ?? ?? b8 e8 03 00 00", 0x3D64F28, "PR mkpattern (this exe only)"),
    _g("CSFeMan", "48 8b 0d ?? ?? ?? ?? 44 0f b6 cd 44 8b c6", 0x3D6F8F0, "PR mkpattern (this exe only)"),
    _g("LockTgtMan", "48 8b 05 ?? ?? ?? ?? 0f b6 5b 0c 48 85 c0", 0x3D6E278, "PR mkpattern (this exe only)"),
    _g("CSBulletManager", "48 8b 0d ?? ?? ?? ?? 8b 3b 48 85 c9", 0x3D667A8, "PR mkpattern (this exe only)"),
    _g("CSSessionManager", "48 8b 0d ?? ?? ?? ?? 83 79 0c 03 75 1f", 0x3D7E540, "PR mkpattern (this exe only; CT AOB has 0 matches)"),
    # fsrs pelite-style patterns (captures via ')
    {"name": "FieldArea(fsrs)", "kind": "global", "pattern": "48 8b 05 ${ ' } 48 85 c0 48 0f 44 c6",
     "resolve": ("cap", 1), "expect": 0x3D6D248, "source": "fsrs field_area_ptr"},
    {"name": "GameDataMan(fsrs)", "kind": "global", "pattern": "48 89 05 ${ ' } 48 83 c4 38 e9 ${}",
     "resolve": ("cap", 1), "expect": 0x3D61F98, "source": "fsrs game_data_man"},
    {"name": "global_hinstance", "kind": "global", "pattern": "48 8b ce 48 8b f8 e8 $ { 48 89 0d $ { ' } c3 }",
     "resolve": ("cap", 1), "expect": 0x3D89708, "source": "fsrs (non-zero once CSWindow created; wait_for_system_init)"},
    {"name": "world_chr_man_dbg_flags", "kind": "global", "pattern": "80 3d $ { ? ' } 00 0f 85 [4] 32 c0 ",
     "resolve": ("cap", 1), "expect": 0x3D6A210, "source": "fsrs"},
    # ---- functions ----
    {"name": "register_task", "kind": "function", "pattern": "e8 ? ? ? ? 48 8b 0d ? ? ? ? 4c 8b c7 8b d3 e8 $ { ' }",
     "resolve": ("cap", 1), "expect": 0xEB3E50, "source": "fsrs (Ww2710 override)",
     "notes": "void(CSTaskImp* (ignored), u32 CSTaskGroupIndex, FD4TaskBase* task); task vtable {get_runtime_class, dtor, execute(FD4TaskData*)}"},
    {"name": "cs_phys_world_cast_ray", "kind": "function", "pattern": "ba 58 00 00 02 e8 $ { ' }",
     "resolve": ("cap", 1), "expect": 0xC71E00, "source": "fsrs (Ww2710 override)",
     "notes": "bool(CSPhysWorld*, u32 filter, const hkVector4* origin, const hkVector4* extent, hkVector4* hit_out, ChrIns* ignore)"},
    _f("cs_bullet_manager_spawn_bullet", "40 53 55 56 57 48 81 ec 98 07 00 00 48 c7 44 24 50 fe ff ff ff", 0x3A2CB0,
       "fsrs + CT Bullet_code", notes="(CSBulletManager*, i32* out_handle, BulletSpawnData* (0x110 bytes), i32* out)"),
    {"name": "chr_ins_apply_speffect", "kind": "function", "pattern": "48 8b c8 45 33 c0 ba 7f 14 00 00 e8 $ { ' }",
     "resolve": ("cap", 1), "expect": 0x3E8DC0, "source": "fsrs", "notes": "(ChrIns*, i32 speffect_id, bool dont_sync)"},
    {"name": "chr_ins_remove_speffect", "kind": "function", "pattern": "ba 1c 00 00 00 48 8b ce 84 c0 74 ? e8 $ { ' } eb ?",
     "resolve": ("cap", 1), "expect": 0x3EE2E0, "source": "fsrs", "notes": "(ChrIns*, i32 speffect_id)"},
    {"name": "cs_menu_man_imp_display_status_message", "kind": "function", "pattern": "ba d0 07 00 00 48 83 c4 28 e9 $ { ' }",
     "resolve": ("cap", 1), "expect": 0x7671F0, "source": "fsrs", "notes": "bool(CSMenuManImp*, i32 msg) e.g. 5 = YOU DIED"},
    {"name": "cs_ez_draw_draw_line", "kind": "function",
     "pattern": "40 57 48 83 ec 40 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 50 48 89 6c 24 58 48 89 74 24 60 49 8b f0 48 8b fa 48 8b d9 48 8d 69 28",
     "resolve": "match", "expect": 0x2652640, "source": "fsrs (Ww2710 override)", "notes": "debug draw (RendMan CSEzDraw)"},
    _f("ChrIns::SetPosition (warp)", "48 8b 49 58 e9 $ { 48 89 5c 24 08 57 48 83 ec 20 48 8b 41 10 48 8b d9 48 8b 88 90 01 00 00 48 8b 49 68 e8 }",
       0x3F2690, "PR", notes="void(ChrIns*, const hkVector4* havok_pos) -> ChrCtrl(+0x58) -> 0x3C7680"),
    _f("ChrCtrl::SetPosition", "48 89 5c 24 08 57 48 83 ec 20 48 8b 41 10 48 8b d9 48 8b 88 90 01 00 00 48 8b 49 68 e8 ?? ?? ?? ?? 48 8b 43 10",
       0x3C7680, "PR", notes="(ChrCtrl*, const hkVector4*): physics SetPosition then model-matrix refresh if physics+0x90"),
    _f("CSChrPhysicsModule::SetPosition", "0f 28 02 0f 11 41 70 0f 28 02 0f 11 81 80 00 00 00 66 c7 81 90 00 00 00 01 01 c6 81 de 01 00 00 01 e9",
       0x45EEF0, "PR", notes="writes +0x70 pos, +0x80 last pos, +0x90/+0x91=1, +0x1DE=1, tail-jumps SyncProxyPosition"),
    _f("CSChrPhysicsModule::SyncProxyPosition", "40 53 48 83 ec 40 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 30 80 b9 91 00 00 00 00 48 8b d9",
       0x45C910, "PR", notes="if +0x91: push +0x70 into char proxies +0x98/+0xA0 (0xC5D860), clear +0x91"),
    _f("ChrIns::SetYaw", "48 8b 49 58 e9 $ { 48 8b 41 10 48 8b 88 90 01 00 00 48 8b 49 68 e9 $ { 40 53 48 83 ec 30 4c 8b c2 48 8b d9 48 8d 54 24 20 e8 } }",
       0x3F6180, "PR", notes="void(ChrIns*, const float* yaw_rad) -> ChrCtrl 0x3C7EE0 -> physics 0x45FD00 (quat from yaw*0.5 -> +0x50, +0x60 recomputed)"),
    _f("CSChrPhysicsModule::SetOrientation", "40 53 48 83 ec 30 0f 28 02 48 8b d9 48 8d 54 24 20 0f 29 44 24 20 66 0f 7f 41 50 e8",
       0x45FEE0, "PR", notes="(physics*, const hkQuaternion* q): +0x50 = q, +0x60 = 0x463FA0(this,q)"),
    _f("CSChrPhysicsModule::SetPositionAndYaw", "48 89 5c 24 08 57 48 83 ec 30 0f 28 02 48 8b da 0f 11 41 70 48 8b f9 0f 28 02",
       0x45FC80, "PR", notes="(physics*, const hkVector4* pos, const float* yaw): SetPosition fields + yaw quat; no proxy sync call"),
    _f("ChrCtrl::RequestProxyPosition", "83 89 fc 00 00 00 01 0f 28 02 0f 11 81 00 01 00 00 c3", 0x3C9270, "PR",
       notes="ChrCtrl+0xFC |= 1; ChrCtrl+0x100 = pos"),
    _f("ChrIns::GetTeamType", "48 89 5c 24 10 57 48 83 ec 20 0f b6 41 6c 48 8b f9 88 02 48 8b da 48 8b 89 78 01 00 00",
       0x3F1C90, "PR", notes="reads ChrIns+0x6C (u8), may be overridden by speffect state 0x84"),
    _f("ChrIns::Kill", "48 89 5c 24 08 57 48 83 ec 30 f3 0f 10 05 ?? ?? ?? ?? 48 8b d9 48 8b 89 90 01 00 00 0f b6 fa",
       0x3EE730, "PR", notes="(ChrIns*, bool): data module ChangeHp(0) then ChrIns+0x1C5 |= 0x80 (death flag)"),
    _f("CSChrDataModule::ChangeHp", "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 40 0f 29 74 24 30 0f 28 f3 41 0f b6 f0 8b da 48 8b f9 85 d2 7f 0e",
       0x438810, "PR", notes="(data*, i32 hp, bool, f32, ..., bool) -> 0x437450 writes +0x138 clamped to +0x13C"),
    _f("CSChrDataModule::SetMaxHp", "89 54 24 10 c7 44 24 18 ff ff 07 00 c7 44 24 08 01 00 00 00", 0x438870, "PR",
       notes="clamps to [1,0x7FFFF], writes +0x13C then re-clamps +0x138"),
    _f("CSCamera::ApplyCameraMask", "8b 41 28 4c 8b c9 a8 20 74 ?? 48 8b 51 20 4c 8b 41 08", 0x3BB3E0, "PR",
       notes="copies pers_cam_{2,3,4} -> pers_cam_1 (fov/aspect/near/far +0x50..+0x5C, matrix +0x10..+0x4F) by mask bits, clears mask&0x3F"),
    _f("hkbCharacter::FireEvent (CT PlayAnimation)", "74 ?? 48 85 d2 74 ?? 48 8d 4c 24 50", 0xC15AB0, "CT PlayAnimation_code",
       offset=-0xD, notes="(hkbCharacter*, const wchar_t* event) ; hkbCharacter = [[ChrIns+0x190]+0x28]+0x10 -> +0x30"),
    _f("D3D12CreateDevice call site", "4c 8b cb 4c 8d 05 ?? ?? ?? ?? 8b d0 48 8b ce e8 ?? ?? ?? ?? 85 c0 74 0c 8b c8 e8",
       0x1EA2822, "PR", notes="inside 0x1EA2730 (ID3D12Device** out, IDXGIAdapter*, cfg*); informational only"),
    {"name": "Arxan code-restoration jb sites (count)", "kind": "count",
     "pattern": "B9 ? ? ? ? E8 ? ? ? ? F3 0F 11 05 ? ? ? ? [0-128] ' 72 ? 48 8D ? ? ? ? ?",
     "resolve": ("cap", 1), "expect_matches": 181, "source": "fsrs shared/src/arxan.rs (tremwil/Yui)",
     "notes": "capture = the `jb` byte that fsrs patches to `jmp` (0xEB) to disable code restoration"},
    # ---- singletons resolved by name (robust across patches) ----
    _s("singleton:WorldChrMan", "CS::WorldChrMan", 0x3D69FF8),
    _s("singleton:CSCamera", "CS::CSCamera", 0x3D669C8),
    _s("singleton:CSMenuMan", "CS::CSMenuMan", 0x3D6F820),
    _s("singleton:CSFlipper", "CS::CSFlipper", 0x458DB58),
    _s("singleton:CSTask", "CS::CSTask", 0x458FE88),
    _s("singleton:CSTaskGroup", "CS::CSTaskGroup", 0x458FF08),
    _s("singleton:CSRegulationManager", "CS::CSRegulationManager", 0x3D8ACD8),
    _s("singleton:SoloParamRepository", "CS::SoloParamRepository", 0x3D85F58),
    _s("singleton:CSHavokMan", "CS::CSHavokMan", 0x3D7A0D0),
    _s("singleton:CSGraphics", "CS::CSGraphics", 0x3D75CB8),
    _s("singleton:RendMan", "CS::RendMan", 0x3D7F130),
    _s("singleton:CSFade", "CS::CSFade", 0x3D763B0),
    _s("singleton:CSNowLoadingHelper", "CS::CSNowLoadingHelper", 0x3D64F28),
    _s("singleton:CSFeMan", "CS::CSFeMan", 0x3D6F8F0),
    _s("singleton:CSWindow", "CS::CSWindow", 0x458F910),
    _s("singleton:DmgMan", "CS::DmgMan", 0x3D6A3E8),
    _s("singleton:CSBulletManager", "CS::CSBulletManager", 0x3D667A8),
    _s("singleton:FD4PadManager", "FD4PadManager", 0x4861D30),
    _s("singleton:FD4TaskManager", "FD4TaskManager", 0x4861280),
    _s("singleton:FD4ParamRepository", "FD4ParamRepository", 0x4861FB0),
    # ---- vftables (RTTI) ----
    _v("vft:CS::ChrIns", "CS::ChrIns", 0x2A310C8),
    _v("vft:CS::PlayerIns", "CS::PlayerIns", 0x2A7FBB0, vfunc={"get_player_game_data(+0x580)": 45}),
    _v("vft:CS::EnemyIns", "CS::EnemyIns", 0x2A47090),
    _v("vft:CS::ChrCtrl", "CS::ChrCtrl", 0x2A2C6A0),
    _v("vft:CS::CSChrPhysicsModule", "CS::CSChrPhysicsModule", 0x2A3C890),
    _v("vft:CS::CSChrDataModule", "CS::CSChrDataModule", 0x2A380B8),
    _v("vft:CS::CSChrDamageModule", "CS::CSChrDamageModule", 0x2A39A90),
    _v("vft:CS::CSPlayerDamageModule", "CS::CSPlayerDamageModule", 0x2A3A3E0),
    _v("vft:CS::CSEnemyDamageModule", "CS::CSEnemyDamageModule", 0x2A3A0D8),
    _v("vft:CS::ChrCam", "CS::ChrCam", 0x2A2AA08),
    _v("vft:CS::CSCam", "CS::CSCam", 0x2AA0A18),
    _v("vft:CS::CSPersCam", "CS::CSPersCam", 0x2AA0A40),
    _v("vft:CS::CSDebugCam", "CS::CSDebugCam", 0x2B672C0),
    _v("vft:CS::CSInGamePad_UserInput1", "CS::CSInGamePad_UserInput1", 0x2BE35E8),
    _v("vft:CS::CSDebugCamPad", "CS::CSDebugCamPad", 0x2BE2EF0),
    _v("vft:CS::NearEnemyFinder", "CS::NearEnemyFinder", 0x2A282D0, vfunc={"run": 2}),
    _v("vft:CS::ChrSet", "CS::ChrSet", 0x2A41348),
    _v("vft:CS::OpenFieldChrSet", "CS::OpenFieldChrSet", 0x2A4B620),
    _v("vft:CS::CSPhysWorld", "CS::CSPhysWorld", 0x2B96978),
    _v("vft:CS::CSHavokManImp", "CS::CSHavokManImp", 0x2B934E8),
    _v("vft:CS::CSGraphicsImp", "CS::CSGraphicsImp", 0x2B68558),
    _v("vft:CS::RendManImp", "CS::RendManImp", 0x2BA2238),
    _v("vft:GXBS::GXCommandQueue", "GXBS::GXCommandQueue", 0x2F274C8),
    _v("vft:GXBS::GXSwapChainOperatorForMultiWindow", "GXBS::GXSwapChainOperatorForMultiWindow", 0x2F07CB8),
    _v("vft:CS::CSTaskImp", "CS::CSTaskImp", 0x2C04B00),
    _v("vft:CS::CSEzTask", "CS::CSEzTask", 0x2C048A0),
    _v("vft:CS::CSInGamePad", "CS::CSInGamePad", 0x2A2AA40),
    _v("vft:CS::CSPad", "CS::CSPad", 0x2BE3A70),
    _v("vft:CS::CSMenuViewerPad", "CS::CSMenuViewerPad", 0x2BE4778),
])

if __name__ == "__main__":
    main()
