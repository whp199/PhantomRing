#!/usr/bin/env python3
"""fieldaccess.py - find every instruction that accesses [reg + DISP] in a PE's code (PhantomRing RE).

Works on eldenring.exe and mgsvtpp.exe (any PE32+). Static, read-only.

    python3 -I fieldaccess.py EXE DISP [DISP ...] [--write] [--read] [--base REG] [--range LO HI]
                              [--mnem movss,movaps] [--func] [--max N]

DISP is a hex/decimal displacement (>= 0x80, so it is encoded as disp32). The scan pre-filters the
code sections for the 4 displacement bytes, then decodes candidate instructions that end right after
the displacement (or after a trailing imm8/imm16/imm32) and keeps those whose memory operand really has
that displacement at that position. --write keeps stores (memory is operand 0 of a mov/movss/...,
or an RMW like or/and/add/inc), --read keeps loads. --func prints the containing function (.pdata).
Output: VA  function  bytes  instruction.
"""
import argparse
import bisect
import struct
import sys

if __import__('os').environ.get('CAPSTONE_PATH'):  # optional: capstone outside site-packages

	sys.path.insert(0, __import__('os').environ['CAPSTONE_PATH'])
import capstone  # noqa: E402
from capstone import x86  # noqa: E402

STORE_MNEM = {"mov", "movss", "movsd", "movaps", "movups", "movdqa", "movdqu", "movq", "movd", "movlps",
              "movhps", "movntps", "vmovaps", "vmovups", "vmovss", "cvtsi2ss", "stosb", "setne", "sete",
              "seta", "setb", "setae", "setbe", "setg", "setl", "setge", "setle", "sets", "setns"}
RMW_MNEM = {"or", "and", "xor", "add", "sub", "inc", "dec", "bts", "btr", "btc", "neg", "not", "shl", "shr",
            "sar", "xchg", "lock or", "lock and", "lock add", "lock xadd", "xadd", "cmpxchg", "addss", "subss"}


class PE:
	def __init__(self, path):
		self.raw = open(path, "rb").read()
		r = self.raw
		pe = struct.unpack_from("<I", r, 0x3C)[0]
		nsec = struct.unpack_from("<H", r, pe + 6)[0]
		optsz = struct.unpack_from("<H", r, pe + 20)[0]
		opt = pe + 24
		self.base = struct.unpack_from("<Q", r, opt + 24)[0]
		exc_rva, exc_size = struct.unpack_from("<II", r, opt + 112 + 3 * 8)
		self.secs = []
		for i in range(nsec):
			o = opt + optsz + 40 * i
			name = r[o:o + 8].rstrip(b"\0").decode("latin-1")
			vs, va, rs, ro = struct.unpack_from("<IIII", r, o + 8)
			ch = struct.unpack_from("<I", r, o + 36)[0]
			self.secs.append((name, va, vs, ro, rs, ch))
		self.funcs = []
		po = self.rva2off(exc_rva)
		if po is not None:
			for i in range(exc_size // 12):
				b, e, _ = struct.unpack_from("<III", r, po + 12 * i)
				if b:
					self.funcs.append((b, e))
		self.funcs.sort()
		self.fstarts = [f[0] for f in self.funcs]

	def rva2off(self, rva):
		for _, va, vs, ro, rs, _ in self.secs:
			if va <= rva < va + rs:
				return ro + rva - va
		return None

	def exec_ranges(self):
		for name, va, vs, ro, rs, ch in self.secs:
			if ch & 0x20000000:
				yield name, va, ro, min(vs, rs) if vs else rs

	def func_of(self, rva):
		i = bisect.bisect_right(self.fstarts, rva) - 1
		if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
			return self.funcs[i][0]
		return None


def main():
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("exe")
	ap.add_argument("disps", nargs="+")
	ap.add_argument("--write", action="store_true")
	ap.add_argument("--read", action="store_true")
	ap.add_argument("--base", help="only this base register (e.g. rbx)")
	ap.add_argument("--range", nargs=2, help="VA range LO HI")
	ap.add_argument("--mnem", help="comma list of mnemonics to keep")
	ap.add_argument("--func", action="store_true")
	ap.add_argument("--nostack", action="store_true", help="drop rsp/rbp based accesses")
	ap.add_argument("--max", type=int, default=400)
	a = ap.parse_args()
	pe = PE(a.exe)
	md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
	md.detail = True
	lo = hi = None
	if a.range:
		lo, hi = int(a.range[0], 16), int(a.range[1], 16)
	mn = set(a.mnem.split(",")) if a.mnem else None
	fcache = {}
	for ds in a.disps:
		disp = int(ds, 0)
		pat = struct.pack("<i", disp)
		n = 0
		print("== disp 0x%x" % disp)
		for sname, va, ro, size in pe.exec_ranges():
			blob = pe.raw[ro:ro + size]
			pos = blob.find(pat)
			while pos != -1 and n < a.max:
				seen = None
				frva = pe.func_of(va + pos)
				if frva is not None and frva not in fcache:
					fo = pe.rva2off(frva)
					fend = pe.funcs[bisect.bisect_right(pe.fstarts, frva) - 1][1]
					fcache[frva] = {i.address: i for i in md.disasm(pe.raw[fo:fo + (fend - frva)], pe.base + frva)}
				cands = []
				if frva is not None:
					for back in range(2, 11):
						i = fcache[frva].get(pe.base + va + pos - back)
						if i is not None:
							cands.append(i)
				else:
					for back in range(10, 1, -1):
						st = pos - back
						if st < 0:
							continue
						try:
							cands.append(next(md.disasm(blob[st:st + back + 12], pe.base + va + st, 1)))
						except StopIteration:
							pass
				for ins in cands:
					back = pe.base + va + pos - ins.address
					if ins.size not in (back + 4, back + 5, back + 6, back + 8):
						continue
					mem = [i for i, op in enumerate(ins.operands) if op.type == x86.X86_OP_MEM and op.mem.disp == disp
					       and op.mem.base not in (0, x86.X86_REG_RIP)]
					if mem:
						seen = (ins, mem[0])
						break
				if seen:
					ins, mi = seen
					vaddr = ins.address
					if lo is not None and not (lo <= vaddr < hi):
						pass
					else:
						op = ins.operands[mi]
						is_store = mi == 0 and (ins.mnemonic in STORE_MNEM or ins.mnemonic.startswith("set")) and ins.mnemonic not in ("cmp", "test")
						is_rmw = mi == 0 and ins.mnemonic.replace("lock ", "") in RMW_MNEM
						w = is_store or is_rmw
						keep = True
						if a.write and not w:
							keep = False
						if a.read and w:
							keep = False
						if a.base and ins.reg_name(op.mem.base) != a.base:
							keep = False
						if a.nostack and ins.reg_name(op.mem.base) in ("rsp", "rbp"):
							keep = False
						if mn and ins.mnemonic not in mn:
							keep = False
						if keep:
							f = pe.func_of(vaddr - pe.base) if a.func else None
							fs = (" f=0x%x" % (f + pe.base)) if f is not None else ""
							print("0x%x%s  %-28s %s %s" % (vaddr, fs, ins.bytes.hex(), ins.mnemonic, ins.op_str))
							n += 1
				pos = blob.find(pat, pos + 1)
		if n >= a.max:
			print("... (max reached)")


if __name__ == "__main__":
	main()
