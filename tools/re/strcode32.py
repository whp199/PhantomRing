#!/usr/bin/env python3
"""strcode32.py - Fox Engine StrCode64 / StrCode32 (MGSV TPP 1.0.15.4) and an immediate finder.

Implements exactly what mgsvtpp.exe does (VERIFIED by disassembly, see docs/research/re-backlog.md M1):
  StrCode32 thunk 0x1400234E0 -> 0x141A098D0 -> StrCode64 0x141A09AC0(str, len)
  StrCode64 = CityHash64WithSeeds(str + "\\0", len + 1, seed0=0x9AE16A3B2F90404F, seed1=(str[0] << 16) + len)
              & 0xFFFFFFFFFFFF                                     (0x141A09770; word at +6 zeroed)
  StrCode32 = low 32 bits of StrCode64 (callers truncate).
  CityHash64 = Google CityHash v1.0.3 (the "legacy" variant Fox uses; 0x141A091F0 / 0x141A08CB0).

usage:
  python3 -I strcode32.py NAME [NAME ...]                    print StrCode32/StrCode64
  python3 -I strcode32.py --find EXE NAME [NAME ...]         also list code sites that use the
                                                             StrCode32 as an imm32 (cmp/mov/sub)

The CityHash64 functions below are a Python port of Google's city.cc (CityHash v1.0.3), under its licence:

  Copyright (c) 2011 Google, Inc.

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
"""
import struct
import sys

M = 0xFFFFFFFFFFFFFFFF
k0 = 0xC3A5C85C97CB3127
k1 = 0xB492B66FBE98F273
k2 = 0x9AE16A3B2F90404F
k3 = 0xC949D7C7509E6557
kMul = 0x9DDFEA08EB382D69


def f64(b, i):
	return struct.unpack_from("<Q", b, i)[0]


def f32(b, i):
	return struct.unpack_from("<I", b, i)[0]


def rot(v, s):
	return v if s == 0 else ((v >> s) | (v << (64 - s))) & M


def rot_by_at_least_1(v, s):
	return ((v >> s) | (v << (64 - s))) & M


def shift_mix(v):
	return v ^ (v >> 47)


def hash128to64(u, v):
	a = ((u ^ v) * kMul) & M
	a ^= a >> 47
	b = ((v ^ a) * kMul) & M
	b ^= b >> 47
	return (b * kMul) & M


def hash_len16(u, v):
	return hash128to64(u, v)


def hash_len0to16(s):
	n = len(s)
	if n > 8:
		a = f64(s, 0)
		b = f64(s, n - 8)
		return (hash_len16(a, rot_by_at_least_1((b + n) & M, n)) ^ b) & M
	if n >= 4:
		a = f32(s, 0)
		return hash_len16((n + (a << 3)) & M, f32(s, n - 4))
	if n > 0:
		a, b, c = s[0], s[n >> 1], s[n - 1]
		y = (a + (b << 8)) & 0xFFFFFFFF
		z = (n + (c << 2)) & 0xFFFFFFFF
		return (shift_mix(((y * k2) ^ (z * k3)) & M) * k2) & M
	return k2


def hash_len17to32(s):
	n = len(s)
	a = (f64(s, 0) * k1) & M
	b = f64(s, 8)
	c = (f64(s, n - 8) * k2) & M
	d = (f64(s, n - 16) * k0) & M
	return hash_len16((rot((a - b) & M, 43) + rot(c, 30) + d) & M,
	                  (a + rot(b ^ k3, 20) - c + n) & M)


def weak_hash_len32_with_seeds_v(w, x, y, z, a, b):
	a = (a + w) & M
	b = rot((b + a + z) & M, 21)
	c = a
	a = (a + x) & M
	a = (a + y) & M
	b = (b + rot(a, 44)) & M
	return (a + z) & M, (b + c) & M


def weak_hash_len32_with_seeds(s, i, a, b):
	return weak_hash_len32_with_seeds_v(f64(s, i), f64(s, i + 8), f64(s, i + 16), f64(s, i + 24), a, b)


def hash_len33to64(s):
	n = len(s)
	z = f64(s, 24)
	a = (f64(s, 0) + (n + f64(s, n - 16)) * k0) & M
	b = rot((a + z) & M, 52)
	c = rot(a, 37)
	a = (a + f64(s, 8)) & M
	c = (c + rot(a, 7)) & M
	a = (a + f64(s, 16)) & M
	vf = (a + z) & M
	vs = (b + rot(a, 31) + c) & M
	a = (f64(s, 16) + f64(s, n - 32)) & M
	z = f64(s, n - 8)
	b = rot((a + z) & M, 52)
	c = rot(a, 37)
	a = (a + f64(s, n - 24)) & M
	c = (c + rot(a, 7)) & M
	a = (a + f64(s, n - 16)) & M
	wf = (a + z) & M
	ws = (b + rot(a, 31) + c) & M
	r = shift_mix(((vf + ws) * k2 + (wf + vs) * k0) & M)
	return (shift_mix((r * k0 + vs) & M) * k2) & M


def city_hash64(s):
	n = len(s)
	if n <= 32:
		return hash_len0to16(s) if n <= 16 else hash_len17to32(s)
	if n <= 64:
		return hash_len33to64(s)
	x = f64(s, 0)
	y = f64(s, n - 16) ^ k1
	z = f64(s, n - 56) ^ k0
	v = weak_hash_len32_with_seeds(s, n - 64, n, y)
	w = weak_hash_len32_with_seeds(s, n - 32, (n * k1) & M, k0)
	z = (z + shift_mix(v[1]) * k1) & M
	x = (rot((z + x) & M, 39) * k1) & M
	y = (rot(y, 33) * k1) & M
	n2 = (n - 1) & ~63
	i = 0
	while True:
		x = (rot((x + y + v[0] + f64(s, i + 16)) & M, 37) * k1) & M
		y = (rot((y + v[1] + f64(s, i + 48)) & M, 42) * k1) & M
		x ^= w[1]
		y ^= v[0]
		z = rot(z ^ w[0], 33)
		v = weak_hash_len32_with_seeds(s, i, (v[1] * k1) & M, (x + w[0]) & M)
		w = weak_hash_len32_with_seeds(s, i + 32, (z + w[1]) & M, y)
		z, x = x, z
		i += 64
		n2 -= 64
		if n2 == 0:
			break
	return hash_len16((hash_len16(v[0], w[0]) + shift_mix(y) * k1 + z) & M,
	                  (hash_len16(v[1], w[1]) + x) & M)


def city_hash64_with_seeds(s, seed0, seed1):
	return hash_len16((city_hash64(s) - seed0) & M, seed1)


def strcode64(text):
	b = text.encode("ascii")
	if not b:
		return 0  # [U] empty string; not needed
	seed1 = ((b[0] if b[0] < 0x80 else b[0] - 0x100) << 16) + len(b)
	return city_hash64_with_seeds(b + b"\0", k2, seed1 & M) & 0xFFFFFFFFFFFF


def strcode32(text):
	return strcode64(text) & 0xFFFFFFFF


def find_imm(exe, values):
	if __import__('os').environ.get('CAPSTONE_PATH'):  # optional: capstone outside site-packages
		sys.path.insert(0, __import__('os').environ['CAPSTONE_PATH'])
	import capstone  # noqa
	raw = open(exe, "rb").read()
	pe = struct.unpack_from("<I", raw, 0x3C)[0]
	nsec = struct.unpack_from("<H", raw, pe + 6)[0]
	optsz = struct.unpack_from("<H", raw, pe + 20)[0]
	base = struct.unpack_from("<Q", raw, pe + 24 + 24)[0]
	md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
	out = {}
	for i in range(nsec):
		o = pe + 24 + optsz + 40 * i
		vs, va, rs, ro = struct.unpack_from("<IIII", raw, o + 8)
		ch = struct.unpack_from("<I", raw, o + 36)[0]
		if not ch & 0x20000000:
			continue
		blob = raw[ro:ro + rs]
		for v in values:
			pat = struct.pack("<I", v)
			p = blob.find(pat)
			while p != -1:
				for back in range(1, 8):
					st = p - back
					try:
						ins = next(md.disasm(blob[st:st + back + 4 + 4], base + va + st, 1))
					except StopIteration:
						continue
					if ins.size == back + 4 and ins.mnemonic in ("cmp", "mov", "sub", "lea", "add", "xor"):
						out.setdefault(v, []).append("0x%x %s %s" % (ins.address, ins.mnemonic, ins.op_str))
						break
				p = blob.find(pat, p + 1)
	return out


def main(argv):
	if not argv:
		print(__doc__)
		return 2
	exe = None
	if argv[0] == "--find":
		exe = argv[1]
		argv = argv[2:]
	vals = {}
	for name in argv:
		h = strcode32(name)
		vals[h] = name
		print("%-32s StrCode32=0x%08x StrCode64=0x%012x" % (name, h, strcode64(name)))
	if exe:
		hits = find_imm(exe, list(vals))
		for v, name in vals.items():
			sites = hits.get(v, [])
			print("%-32s 0x%08x  %d code sites%s" % (name, v, len(sites), (": " + "; ".join(sites[:6])) if sites else ""))
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv[1:]))
