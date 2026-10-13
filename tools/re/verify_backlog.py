#!/usr/bin/env python3
"""verify_backlog.py - re-check every AOB / vtable slot cited in docs/research/re-backlog.md.

    python3 -I tools/re/verify_backlog.py [--er ER_EXE] [--mgsv MGSV_EXE]

Defaults: $ER_EXE / $MGSV_EXE, else the Steam installs (tools/game_paths.py).

ER patterns use er_aob_scan.py's matcher (supports ?? and the pelite `$ { ' }` subset), so RVAs are
reported; MGSV patterns are plain byte/?? patterns reported as VAs. Each entry states the expected
count and address; the script exits 1 if anything drifts. Static and read-only.
"""
import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import er_aob_scan as er  # noqa: E402

import game_paths  # noqa: E402

# (name, pattern, expected count, expected RVA of the match (or capture 1 when the pattern has one))
ER_AOBS = [
	("E1 PlayerCtrl::ModifyRootMotion (twin with EnemyCtrl)", "48 8b c4 48 89 58 18 48 89 78 20 55 48 8d 68 a1 48 81 ec b0 00 00 00 0f 29 70 e8 0f 29 78 d8 44 0f 29 40 c8 44 0f 29 48 b8 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 45 07 48 8b 41 10 48 8b fa", 2, None),
	("E1 rootmotion vcall site [rax+0x60]", "0f 29 07 48 8b 03 48 8b d7 48 8b cb ff 50 60 48 8b 43 10", 1, 0x3CC85C),
	("E1 ChrCtrl::UpdateRootMotion", "40 53 48 81 ec 80 00 00 00 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 60", 1, 0x3CC6D0),
	("E1 Physics::PrepareStep", "48 8b c4 56 57 41 56 48 83 ec 70 48 c7 40 a0 fe ff ff ff 48 89 58 08 48 89 68 10", 1, 0x4609C0),
	("E1 rootmotion->velocity", "4c 8b dc 55 57 41 56 49 8d ab 58 ff ff ff", 1, 0x467280),
	("E1 CsChrProxy::SetVelocity", "48 8b 81 a0 00 00 00 48 85 c0 74 06 0f 28 02 0f 29 00", 1, 0xC5DB30),
	("E2 CSPlayerDamageModule::ReceiveDamage", "48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 41 0f b6 80 15 01 00 00", 1, 0x44D050),
	("E2 ProcessChrHit", "48 8b c4 55 56 57 41 56 41 57 48 8d a8 78 fc ff ff", 1, 0x44AE70),
	("E4 IsMenuOrCutsceneSuppressing", "48 89 5c 24 08 57 48 83 ec 20 48 8b 05 ?? ?? ?? ?? 32 db", 1, 0x80E490),
	("E4 CSRemo slot (CT)", "48 85 C0 74 ?? 80 B8 ?? ?? ?? ?? 00 0F B6 DB 0F 45 DF 48 8B 0D $ { ' }", 1, 0x3D72AC8),
	("E6 CSActionButtonMan slot", "48 8b 0d $ { ' } 80 79 2b 00 75 14", 1, 0x3D72478),
	("E6 ActionButton select", "40 57 48 83 ec 40 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 50 0f 29 7c 24 30 48 8b fa 48 8b d9", 1, 0xA65AB0),
	("E6 ActionButton input", "48 8b c4 41 56 48 83 ec 60 48 c7 40 b8 fe ff ff ff 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 0f 29 78 e8", 1, 0xA655D0),
]
# (class mangled name, slot index, expected RVA of the slot's function)
ER_VTABLE_SLOTS = [
	(".?AVPlayerCtrl@CS@@", 12, 0x3DE610),
	(".?AVCSPlayerDamageModule@CS@@", 7, 0x44D050),
	(".?AVCSChrDamageModule@CS@@", 7, 0x4455C0),
]

MGSV_AOBS = [
	("M1 TppPlayer2 command handler", "48 8B C4 4C 89 48 20 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 A8", 1, 0x1409C5F80),
	("M1 Player2 Warp impl", "4C 8B DC 57 41 57 48 81 EC A8 00 00", 1, 0x1409CEAE0),
	("M1 Warp vcall site", "49 8D 4E 20 4C 8B 11 48 B8 3E 89 C9", 1, 0x1409C6563),
	("M1 StrCode64", "48 89 6C 24 18 56 48 83 EC 20 80 3C", 1, 0x141A09AC0),
	("M3 ShotSolver", "48 8B C4 55 56 57 41 56 41 57 48 8D A8 D8 FE FF FF 48 81 EC 00 02 00 00 0F", 1, 0x141044FF0),
	("M3 FireWeapon", "4C 8B DC 55 53 56 57 41 54 41 56 41 57 49 8D AB B8", 1, 0x141041110),
	("M3 CQC contact filter", "40 55 41 54 41 56 48 83 EC 60 48 8B", 1, 0x141176560),
	("M2 SteamStorage::Write", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B D9 49 8B F8 48 8B F2 FF", 1, 0x1401703C0),
	("M2 SteamStorage::Delete", "40 53 48 83 EC 20 48 8B DA FF 15 ? ? ? ? 48 8B", 1, 0x14016EAE0),
	("M2 Title Show", "40 53 48 83 EC 20 48 8B 41 40 48 8B D9 C6", 1, 0x1412D73E0),
	("M2 Title Update", "40 56 48 83 EC 40 48 8B F1 48 8B 49", 1, 0x1412D86A0),
	("M2 PressStart Show", "40 57 48 83 EC 40 48 8B F9 48 8B 49 08", 1, 0x1412D6D70),
]
# (vtable VA, slot, expected function VA)
MGSV_VTABLE_SLOTS = [
	(0x142295490, 54, 0x1409CEAE0),   # Player2 sub-object at TppPlayer2+0x20: Warp
	(0x1422953D0, 16, 0x1409C5F80),   # TppPlayer2 GO impl: command handler
	(0x1420E5C60, 0, 0x1401703C0),    # Fox Steam storage: Write
	(0x1420E5C60, 2, 0x14016EAE0),    # Fox Steam storage: Delete
]


class Raw:
	def __init__(self, path):
		self.d = open(path, "rb").read()
		d = self.d
		pe = struct.unpack_from("<I", d, 0x3C)[0]
		nsec = struct.unpack_from("<H", d, pe + 6)[0]
		opt = struct.unpack_from("<H", d, pe + 20)[0]
		self.base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
		self.secs = []
		for i in range(nsec):
			o = pe + 24 + opt + 40 * i
			vs, va, rs, ro = struct.unpack_from("<IIII", d, o + 8)
			ch = struct.unpack_from("<I", d, o + 36)[0]
			self.secs.append((va, vs, ro, rs, ch))

	def va2off(self, v):
		r = v - self.base
		for va, vs, ro, rs, ch in self.secs:
			if va <= r < va + rs:
				return ro + r - va
		return None

	def scan(self, pattern):
		rx = b""
		for t in pattern.split():
			rx += b"." if "?" in t else re.escape(bytes([int(t, 16)]))
		rx = re.compile(rx, re.S)
		hits = []
		for va, vs, ro, rs, ch in self.secs:
			if not ch & 0x20000000:
				continue
			for m in rx.finditer(self.d, ro, ro + rs):
				hits.append(self.base + va + m.start() - ro)
		return hits


def main():
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--er", help="eldenring.exe (default $ER_EXE, else the Steam install)")
	ap.add_argument("--mgsv", help="mgsvtpp.exe (default $MGSV_EXE, else the Steam install)")
	a = ap.parse_args()
	a.er = game_paths.require(a.er or game_paths.er_exe(), "eldenring.exe", "ER_EXE (or --er)")
	a.mgsv = game_paths.require(a.mgsv or game_paths.mgsv_exe(), "mgsvtpp.exe", "MGSV_EXE (or --mgsv)")
	bad = 0
	pe = er.PEImage(a.er)
	print("ER", a.er)
	matcher = er.Matcher(pe)
	for name, pat, cnt, exp in ER_AOBS:
		hits = [caps[1] if len(caps) > 1 else rva for rva, caps in matcher.scan(pat, None, limit=0)]
		ok = len(hits) == cnt and (exp is None or exp in hits)
		bad += not ok
		print("  [%s] %-52s count=%d %s" % ("OK " if ok else "BAD", name, len(hits), ", ".join(hex(h) for h in hits[:3])))
	vts = {}
	for row in er.rtti_classes(pe):
		if row["offset"] == 0:
			vts.setdefault(row["mangled"], []).append(row["vftable_rva"])
	for mangled, slot, exp in ER_VTABLE_SLOTS:
		cand = vts.get(mangled, [])
		vt = cand[0] if len(cand) == 1 else None
		got = pe.u64(vt + 8 * slot) - pe.image_base if vt is not None else None
		ok = got == exp
		bad += not ok
		print("  [%s] %-52s vtbl=%s slot %d -> %s" % ("OK " if ok else "BAD", mangled, hex(vt) if vt else None, slot, hex(got) if got is not None else None))
	m = Raw(a.mgsv)
	print("MGSV", a.mgsv)
	for name, pat, cnt, exp in MGSV_AOBS:
		hits = m.scan(pat)
		ok = len(hits) == cnt and exp in hits
		bad += not ok
		print("  [%s] %-52s count=%d %s" % ("OK " if ok else "BAD", name, len(hits), ", ".join(hex(h) for h in hits[:3])))
	for vt, slot, exp in MGSV_VTABLE_SLOTS:
		got = struct.unpack_from("<Q", m.d, m.va2off(vt + 8 * slot))[0]
		ok = got == exp
		bad += not ok
		print("  [%s] vtable %s slot %-3d -> %s" % ("OK " if ok else "BAD", hex(vt), slot, hex(got)))
	print("%d problem(s)" % bad)
	return 1 if bad else 0


if __name__ == "__main__":
	sys.exit(main())
