#!/usr/bin/env python3
"""mgsv_enumtable.py - dump {int value, const char* name} pair tables that MGSV builds on the stack.

    python3 -I mgsv_enumtable.py EXE FUNC_VA [--grep PREFIX]

Pattern (VERIFIED on the TppEquip EQP_WP_* table builder 0x140A2D830): the function stores
`mov dword [base+k], imm32` then `lea rax, "name"; mov [base+k+8], rax` for consecutive 16-byte
{int, char*} records. Prints `value name` lines. Static, read-only.
"""
import struct
import sys

if __import__('os').environ.get('CAPSTONE_PATH'):  # optional: capstone outside site-packages

	sys.path.insert(0, __import__('os').environ['CAPSTONE_PATH'])
sys.path.insert(0, __import__("os").path.dirname(__file__))
import capstone  # noqa: E402
from capstone import x86  # noqa: E402
from fieldaccess import PE  # noqa: E402


def main():
	exe, fva = sys.argv[1], int(sys.argv[2], 16)
	grep = sys.argv[sys.argv.index("--grep") + 1] if "--grep" in sys.argv else None
	pe = PE(exe)
	rva = fva - pe.base
	i = pe.fstarts.index(rva)
	b, e = pe.funcs[i]
	# follow chained .pdata fragments: extend to the next pdata start that is not contiguous
	o = pe.rva2off(b)
	code = pe.raw[o:o + 0x40000]
	md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
	md.detail = True
	ints = {}
	strs = {}
	last_str = None
	for ins in md.disasm(code, fva):
		if ins.mnemonic == "ret" or ins.mnemonic == "int3":
			break
		ops = ins.operands
		if ins.mnemonic == "lea" and ops[1].type == x86.X86_OP_MEM and ops[1].mem.base == x86.X86_REG_RIP:
			t = ins.address + ins.size + ops[1].mem.disp
			so = pe.rva2off(t - pe.base)
			if so:
				s = pe.raw[so:so + 96].split(b"\0")[0]
				if len(s) >= 2 and all(32 <= c < 127 for c in s):
					last_str = s.decode()
					continue
			last_str = None
		if ins.mnemonic == "mov" and len(ops) == 2 and ops[0].type == x86.X86_OP_MEM:
			key = (ops[0].mem.base, ops[0].mem.disp)
			if ops[1].type == x86.X86_OP_IMM and ops[0].size == 4:
				ints[key] = ops[1].imm
			elif ops[1].type == x86.X86_OP_REG and ops[0].size == 8 and last_str is not None:
				strs[key] = last_str
	rows = []
	for (base, disp), name in strs.items():
		v = ints.get((base, disp - 8))
		if v is not None:
			rows.append((v, name))
	for v, name in sorted(rows):
		if grep is None or name.startswith(grep):
			print(v, name)


if __name__ == "__main__":
	main()
