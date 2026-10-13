"""Shared helpers for PhantomRing static-analysis tools (read-only PE scouting).

Everything here only READS the target executable. Nothing is written next to it.

Main entry point: PEImage(path) -> a mapped, in-memory view of a PE32+ file with
  * .pe        : pefile.PE (fast_load; directories parsed on demand)
  * .image     : bytearray of the file mapped at RVAs (SizeOfImage bytes)
  * .base      : preferred ImageBase
  * .sections  : list of Section(name, rva, vsize, raw_off, raw_size, chars)
  * helpers    : rva_to_off, off_to_rva, section_of, read_u32/u64 at RVA, cstr at RVA
  * .pdata     : numpy arrays (begin, end, unwind) of RUNTIME_FUNCTION entries (lazy)
  * func_of(rva): (begin, end) of the RUNTIME_FUNCTION containing rva, following chained
                  unwind info back to the primary entry.

Requires: pefile, numpy (both available system-wide on the dev box).
Optional: capstone (pip install capstone, or a checkout named by $CAPSTONE_PATH),
          llvm-undname (/usr/lib/llvm/22/bin/llvm-undname) for MSVC demangling.
"""
from __future__ import annotations

import bisect
import hashlib
import math
import os
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass

import numpy as np
import pefile

CAPSTONE_PATH = os.environ.get("CAPSTONE_PATH")  # optional: a capstone checkout outside site-packages

SCN_CNT_CODE = 0x00000020
SCN_CNT_INIT = 0x00000040
SCN_CNT_UNINIT = 0x00000080
SCN_MEM_DISCARDABLE = 0x02000000
SCN_MEM_EXECUTE = 0x20000000
SCN_MEM_READ = 0x40000000
SCN_MEM_WRITE = 0x80000000


def sha256_file(path: str, bufsize: int = 1 << 22) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(bufsize)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def entropy(data) -> float:
    """Shannon entropy in bits/byte (0..8)."""
    if len(data) == 0:
        return 0.0
    arr = np.frombuffer(bytes(data) if not isinstance(data, (bytes, bytearray, memoryview)) else data,
                        dtype=np.uint8)
    counts = np.bincount(arr, minlength=256).astype(np.float64)
    p = counts[counts > 0] / arr.size
    return float(-(p * np.log2(p)).sum())


def decode_scn_chars(c: int) -> str:
    flags = []
    if c & SCN_CNT_CODE:
        flags.append("CODE")
    if c & SCN_CNT_INIT:
        flags.append("IDATA")
    if c & SCN_CNT_UNINIT:
        flags.append("UDATA")
    if c & SCN_MEM_DISCARDABLE:
        flags.append("DISCARD")
    perm = ("R" if c & SCN_MEM_READ else "-") + ("W" if c & SCN_MEM_WRITE else "-") + \
           ("X" if c & SCN_MEM_EXECUTE else "-")
    return perm + " " + "|".join(flags)


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
        return bool(self.chars & SCN_MEM_EXECUTE)

    @property
    def writable(self) -> bool:
        return bool(self.chars & SCN_MEM_WRITE)


class PEImage:
    def __init__(self, path: str, map_image: bool = True):
        self.path = path
        self.pe = pefile.PE(path, fast_load=True)
        if self.pe.FILE_HEADER.Machine != 0x8664:
            raise SystemExit(f"{path}: not an x64 PE (Machine=0x{self.pe.FILE_HEADER.Machine:x})")
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.size_of_image = self.pe.OPTIONAL_HEADER.SizeOfImage
        self.sections: list[Section] = []
        for s in self.pe.sections:
            name = s.Name.rstrip(b"\0").decode("latin-1")
            self.sections.append(Section(name, s.VirtualAddress, s.Misc_VirtualSize,
                                         s.PointerToRawData, s.SizeOfRawData, s.Characteristics))
        self._sec_starts = [s.rva for s in self.sections]
        self.image: bytearray | None = None
        if map_image:
            self._map()
        self._pdata = None

    # ------------------------------------------------------------------ mapping
    def _map(self):
        img = bytearray(self.size_of_image)
        with open(self.path, "rb") as f:
            hdr = f.read(self.pe.OPTIONAL_HEADER.SizeOfHeaders)
            img[: len(hdr)] = hdr
            for s in self.sections:
                n = min(s.raw_size, s.vsize if s.vsize else s.raw_size)
                if n <= 0 or s.raw_off == 0:
                    continue
                f.seek(s.raw_off)
                data = f.read(n)
                img[s.rva: s.rva + len(data)] = data
        self.image = img
        self.np_image = np.frombuffer(self.image, dtype=np.uint8)

    def section_of(self, rva: int) -> Section | None:
        i = bisect.bisect_right(self._sec_starts, rva) - 1
        if i >= 0:
            s = self.sections[i]
            if s.rva <= rva < s.rva + max(s.vsize, s.raw_size):
                return s
        return None

    def section_by_name(self, name: str) -> Section | None:
        for s in self.sections:
            if s.name == name:
                return s
        return None

    def rva_to_off(self, rva: int) -> int | None:
        s = self.section_of(rva)
        if s is None:
            return rva if rva < self.pe.OPTIONAL_HEADER.SizeOfHeaders else None
        d = rva - s.rva
        if d >= s.raw_size:
            return None
        return s.raw_off + d

    def off_to_rva(self, off: int) -> int | None:
        for s in self.sections:
            if s.raw_off <= off < s.raw_off + s.raw_size:
                return s.rva + (off - s.raw_off)
        if off < self.pe.OPTIONAL_HEADER.SizeOfHeaders:
            return off
        return None

    def u32(self, rva: int) -> int:
        return struct.unpack_from("<I", self.image, rva)[0]

    def i32(self, rva: int) -> int:
        return struct.unpack_from("<i", self.image, rva)[0]

    def u64(self, rva: int) -> int:
        return struct.unpack_from("<Q", self.image, rva)[0]

    def cstr(self, rva: int, maxlen: int = 512) -> bytes:
        end = self.image.find(b"\0", rva, rva + maxlen)
        if end < 0:
            end = rva + maxlen
        return bytes(self.image[rva:end])

    def is_code_rva(self, rva: int) -> bool:
        s = self.section_of(rva)
        return s is not None and s.executable

    def va_to_rva(self, va: int) -> int:
        return va - self.base

    def exec_sections(self) -> list[Section]:
        return [s for s in self.sections if s.executable]

    def data_sections(self) -> list[Section]:
        return [s for s in self.sections if not s.executable and s.name not in (".reloc", ".rsrc", ".pdata")
                and s.raw_size > 0]

    def sec_np(self, s: Section) -> np.ndarray:
        n = min(s.raw_size, s.vsize if s.vsize else s.raw_size)
        return self.np_image[s.rva: s.rva + n]

    # ------------------------------------------------------------------ .pdata
    def _load_pdata(self):
        d = self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]]
        if d.Size == 0:
            self._pdata = (np.zeros(0, np.uint32),) * 3
            return
        n = d.Size // 12
        arr = np.frombuffer(bytes(self.image[d.VirtualAddress: d.VirtualAddress + n * 12]),
                            dtype=np.uint32).reshape(n, 3)
        order = np.argsort(arr[:, 0], kind="stable")
        arr = arr[order]
        self._pdata = (arr[:, 0].copy(), arr[:, 1].copy(), arr[:, 2].copy())

    @property
    def pdata(self):
        if self._pdata is None:
            self._load_pdata()
        return self._pdata

    def runtime_function(self, rva: int):
        """Return (begin, end, unwind) for the RUNTIME_FUNCTION covering rva, or None."""
        b, e, u = self.pdata
        i = int(np.searchsorted(b, rva, side="right")) - 1
        if i >= 0 and b[i] <= rva < e[i]:
            return int(b[i]), int(e[i]), int(u[i])
        return None

    def func_of(self, rva: int, max_chain: int = 32):
        """Primary function (begin, end) containing rva, following UNW_FLAG_CHAININFO."""
        rf = self.runtime_function(rva)
        if rf is None:
            return None
        begin, end, unwind = rf
        for _ in range(max_chain):
            if unwind & 1:  # points to another RUNTIME_FUNCTION (rare form)
                break
            try:
                ver_flags = self.image[unwind]
            except IndexError:
                break
            flags = ver_flags >> 3
            if not (flags & 0x4):  # UNW_FLAG_CHAININFO
                break
            count = self.image[unwind + 2]
            off = unwind + 4 + ((count + 1) & ~1) * 2
            pb, pe_, pu = struct.unpack_from("<III", self.image, off)
            begin, end, unwind = pb, pe_, pu
        return begin, end


# ---------------------------------------------------------------------- strings

def find_all(buf, needle: bytes, start: int = 0, end: int | None = None):
    """Yield all offsets of needle in buf[start:end]."""
    if end is None:
        end = len(buf)
    i = buf.find(needle, start, end)
    while i >= 0:
        yield i
        i = buf.find(needle, i + 1, end)


def ascii_string_at(buf, off: int, maxlen: int = 400) -> bytes:
    """Expand to the full NUL-terminated printable string containing offset off."""
    s = off
    while s > 0 and off - s < maxlen:
        c = buf[s - 1]
        if 0x20 <= c < 0x7F or c in (9, 10, 13):
            s -= 1
        else:
            break
    e = buf.find(b"\0", off, off + maxlen)
    if e < 0:
        e = off + maxlen
    return bytes(buf[s:e]), s


# ---------------------------------------------------------------------- demangling

_UNDNAME = shutil.which("llvm-undname") or "/usr/lib/llvm/22/bin/llvm-undname"


def demangle_type_names(raw_names: list[str]) -> dict[str, str]:
    """Demangle MSVC RTTI type names such as '.?AVFoo@Bar@@' -> 'Bar::Foo'.

    Uses llvm-undname by turning each into a vftable symbol '??_7Foo@Bar@@6B@'.
    Falls back to a naive splitter when llvm-undname is missing or fails.
    """
    out: dict[str, str] = {}
    todo = []
    for n in raw_names:
        if n.startswith(".?AV") or n.startswith(".?AU"):
            todo.append(n)
        else:
            out[n] = n
    if todo and os.path.exists(_UNDNAME):
        syms = "\n".join("??_7" + n[4:] + "6B@" for n in todo) + "\n"
        try:
            res = subprocess.run([_UNDNAME], input=syms, capture_output=True, text=True, timeout=600)
            lines = res.stdout.splitlines()
            # llvm-undname echoes the mangled name then prints the demangled line, then a blank line
            dem = []
            i = 0
            while i < len(lines):
                ln = lines[i]
                if ln.startswith("??_7"):
                    if i + 1 < len(lines):
                        dem.append(lines[i + 1])
                    i += 2
                    continue
                i += 1
            if len(dem) == len(todo):
                for n, d in zip(todo, dem):
                    d = d.strip()
                    if d.startswith("const ") and d.endswith("::`vftable'"):
                        d = d[len("const "):-len("::`vftable'")]
                        out[n] = d
                    else:
                        out[n] = _naive_demangle(n)
                return out
        except Exception as ex:  # pragma: no cover
            print(f"[warn] llvm-undname failed: {ex}", file=sys.stderr)
    for n in todo:
        out[n] = _naive_demangle(n)
    return out


def _naive_demangle(n: str) -> str:
    body = n[4:]
    if body.endswith("@@"):
        body = body[:-2]
    if "?$" in body:
        return n  # templates: leave raw
    parts = [p for p in body.split("@") if p]
    return "::".join(reversed(parts))


def load_capstone():
    try:
        import capstone  # noqa: F401
    except ImportError:
        if CAPSTONE_PATH and CAPSTONE_PATH not in sys.path:
            sys.path.insert(0, CAPSTONE_PATH)
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = False
    return capstone, md


def hexs(v: int, w: int = 0) -> str:
    return f"0x{v:0{w}X}" if w else f"0x{v:X}"
