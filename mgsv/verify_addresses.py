#!/usr/bin/env python3
"""verify_addresses.py - check every byte signature in mgsv/Addresses.h against an mgsvtpp.exe on disk.

The guest repeats these checks at runtime (Verify.h) before touching anything; this script is the
offline half: run it after editing Addresses.h, or to see whether a new exe build still matches.

    python3 -I mgsv/verify_addresses.py [/path/to/mgsvtpp.exe]     (default $MGSV_EXE, else the Steam install)

Read-only. Exit status 0 when every site matches.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import game_paths  # noqa: E402
EXE = Path(sys.argv[1] if len(sys.argv) > 1 else game_paths.require(game_paths.mgsv_exe(), "mgsvtpp.exe", "MGSV_EXE"))
HEADER = Path(__file__).resolve().parent / "Addresses.h"
IMAGE_BASE = 0x140000000


def sections(data):
	pe = struct.unpack_from("<I", data, 0x3C)[0]
	count = struct.unpack_from("<H", data, pe + 6)[0]
	opt = struct.unpack_from("<H", data, pe + 20)[0]
	out = []
	for i in range(count):
		name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", data, pe + 24 + opt + i * 40)
		out.append((va, max(vsize, rsize), rsize, raw))
	return out, struct.unpack_from("<I", data, pe + 8)[0], struct.unpack_from("<I", data, pe + 24 + 56)[0]


def main():
	data = EXE.read_bytes()
	secs, stamp, size = sections(data)

	def file_off(va):
		rva = va - IMAGE_BASE
		for sva, _, rsize, raw in secs:
			if sva <= rva < sva + rsize:
				return raw + rva - sva
		return None

	text = HEADER.read_text()
	bad = 0
	print(f"{EXE.name}: TimeDateStamp {stamp:#x} SizeOfImage {size:#x}")
	m = re.search(r"kTimeDateStamp = (0x[0-9A-Fa-f]+)", text)
	if int(m.group(1), 16) != stamp:
		print("  identity: TimeDateStamp differs")
		bad += 1
	# Site{ va, { bytes } } and VarSite{ "name", site, offset, { bytes } }
	pattern = re.compile(r"inline constexpr (Site|VarSite) (\w+)\{\s*(?:\"\w+\",\s*)?(0x[0-9A-Fa-f]+),\s*(?:(0x[0-9A-Fa-f]+),\s*)?\{([^}]*)\}", re.S)
	for kind, name, va, varoff, body in pattern.findall(text):
		va = int(va, 16)
		want = bytes(int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]+", body))
		off = file_off(va)
		have = data[off:off + len(want)] if off is not None else b""
		ok = have == want
		extra = ""
		if kind == "VarSite" and ok:
			# also decode the RIP target: must be the vars pointer
			disp = struct.unpack_from("<i", want, 3)[0]
			target = va + 7 + disp
			extra = f" -> [{target:#x}] + {int(varoff, 16):#x}"
			if target != 0x142C16920:
				ok = False
		print(f"  {'ok ' if ok else 'BAD'} {name:28s} {va:#x}{extra}")
		bad += not ok
	print("all signatures match" if not bad else f"{bad} mismatches")
	return 1 if bad else 0


if __name__ == "__main__":
	sys.exit(main())
