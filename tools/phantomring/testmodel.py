"""A real-looking skinned test model for the Elden Ring renderer (fake_mgsv.py --model humanoid).

Everything follows the DRAW CONTRACT (protocol/phantomring_protocol.h) the way the real guest sends MGSV's
meshes: 64-bit content-hash keys (FNV-1a over the message bytes), normalised semantic names, MGSV-like
vertex formats spread over two streams (POSITION float3 in slot 0; NORMAL/TANGENT half4, TEXCOORD half2,
BLENDWEIGHT unorm8x4, BLENDINDICES uint8x4, COLOR unorm8x4 in slot 1), a BC1 albedo with a full mip chain,
an RGBA8 normal map and an SRM map, and per-frame 3x4 skinning matrices (world = M * vertex, column vectors)
that map mesh space to MGSV world with the anchor position subtracted.

The humanoid's skeleton is also what fake_mgsv publishes as its PoseBlock, so the green CSEzDraw skeleton
and the drawn mesh must coincide in Elden Ring. It is deliberately asymmetric: the +X arm is raised to the
side and a "nose" block sits on the front (+Z) of the head, so a mirrored or turned mapping is visible at once.

D-Walker is a rigid (kDrawRigid) single-stream model with UNORM8 normals and 32-bit indices, so both
renderer paths (skinned, rigid) and several format decoders are exercised.
"""

from __future__ import annotations

import math
import struct

from . import proto

# DXGI / D3D constants not in proto.py
R32G32B32_FLOAT = 6
R16G16B16A16_FLOAT = 10
R32G32_FLOAT = 16
R8G8B8A8_UNORM = 28
R8G8B8A8_UINT = 30
R16G16_FLOAT = 34
R16_UINT = 57
R32_UINT = 42
BC1_UNORM = 71
TRIANGLELIST = 4


def fnv64(*parts: bytes) -> int:
	h = 0xCBF29CE484222325
	for part in parts:
		for b in part:
			h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
	return h


# ---- small linear algebra (3x4 / 4x4 as row lists, column-vector convention) ----------------------
def mat_mul(a, b):
	"""4x4 * 4x4 (lists of 4 rows)."""
	return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def translate(x, y, z):
	return [[1, 0, 0, x], [0, 1, 0, y], [0, 0, 1, z], [0, 0, 0, 1]]


def rot_x(a):
	c, s = math.cos(a), math.sin(a)
	return [[1, 0, 0, 0], [0, c, -s, 0], [0, s, c, 0], [0, 0, 0, 1]]


def rot_y(a):
	"""Maps +Z to (sin a, 0, cos a): the protocol's yaw convention."""
	c, s = math.cos(a), math.sin(a)
	return [[c, 0, s, 0], [0, 1, 0, 0], [-s, 0, c, 0], [0, 0, 0, 1]]


def rot_z(a):
	c, s = math.cos(a), math.sin(a)
	return [[c, -s, 0, 0], [s, c, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]


def apply(m, p):
	return tuple(m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3] for r in range(3))


def quat_of(m):
	"""xyzw quaternion of the rotation part of a 4x4."""
	t = m[0][0] + m[1][1] + m[2][2]
	if t > 0:
		s = math.sqrt(t + 1.0) * 2
		return ((m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25 * s)
	if m[0][0] > m[1][1] and m[0][0] > m[2][2]:
		s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
		return (0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s)
	if m[1][1] > m[2][2]:
		s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
		return ((m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s)
	s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
	return ((m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s, (m[1][0] - m[0][1]) / s)


def norm(v):
	l = math.sqrt(sum(c * c for c in v)) or 1.0
	return tuple(c / l for c in v)


def cross(a, b):
	return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


# ---- skeleton -----------------------------------------------------------------------------------
# name, bind position (model space, Y up, Snake faces +Z, feet at y = 0), parent
BONES = [
	("pelvis", (0.0, 0.95, 0.0), -1),
	("chest", (0.0, 1.40, 0.0), 0),
	("head", (0.0, 1.58, 0.0), 1),
	("head_top", (0.0, 1.86, 0.0), 2),
	("arm_px", (0.22, 1.45, 0.0), 1),      # +X side: raised in every frame (asymmetry marker)
	("forearm_px", (0.22, 1.17, 0.0), 4),
	("hand_px", (0.22, 0.90, 0.0), 5),
	("arm_nx", (-0.22, 1.45, 0.0), 1),
	("forearm_nx", (-0.22, 1.17, 0.0), 7),
	("hand_nx", (-0.22, 0.90, 0.0), 8),
	("thigh_px", (0.11, 0.92, 0.0), 0),
	("shin_px", (0.11, 0.50, 0.0), 10),
	("foot_px", (0.11, 0.06, 0.0), 11),
	("thigh_nx", (-0.11, 0.92, 0.0), 0),
	("shin_nx", (-0.11, 0.50, 0.0), 13),
	("foot_nx", (-0.11, 0.06, 0.0), 14),
]

# (bone, from bind point, to bind point, radius, parent bone for the blended first ring or None)
SEGMENTS = [
	(0, BONES[0][1], BONES[1][1], 0.16, None),
	(1, BONES[1][1], BONES[2][1], 0.19, 0),
	(2, BONES[2][1], BONES[3][1], 0.11, 1),
	(1, BONES[7][1], BONES[4][1], 0.07, None),   # shoulder bar (chest)
	(4, BONES[4][1], BONES[5][1], 0.065, 1),
	(5, BONES[5][1], BONES[6][1], 0.055, 4),
	(7, BONES[7][1], BONES[8][1], 0.065, 1),
	(8, BONES[8][1], BONES[9][1], 0.055, 7),
	(0, BONES[13][1], BONES[10][1], 0.09, None),  # hip bar (pelvis)
	(10, BONES[10][1], BONES[11][1], 0.085, 0),
	(11, BONES[11][1], BONES[12][1], 0.07, 10),
	(13, BONES[13][1], BONES[14][1], 0.085, 0),
	(14, BONES[14][1], BONES[15][1], 0.07, 13),
]


def pose(t: float):
	"""Model-space 4x4 per bone at time t (walk cycle; +X arm raised)."""
	ph = t * 4.0
	local = [rot_y(0.0)] * len(BONES)
	local = list(local)
	local[0] = rot_x(0.05 * math.sin(2 * ph))
	local[1] = rot_y(0.15 * math.sin(ph))
	local[2] = rot_y(0.3 * math.sin(t * 0.7))
	local[4] = rot_z(2.1 + 0.15 * math.sin(ph))    # +X arm up and out
	local[5] = rot_z(0.5)
	local[7] = rot_x(-0.6 * math.sin(ph))
	local[8] = rot_x(-0.5)
	local[10] = rot_x(-0.6 * math.sin(ph))
	local[11] = rot_x(0.9 * max(0.0, math.sin(ph + 1.2)))
	local[13] = rot_x(0.6 * math.sin(ph))
	local[14] = rot_x(0.9 * max(0.0, math.sin(-ph + 1.2)))
	model = []
	for i, (_, p, parent) in enumerate(BONES):
		if parent < 0:
			m = mat_mul(translate(*p), local[i])
		else:
			pp = BONES[parent][1]
			m = mat_mul(model[parent], mat_mul(translate(p[0] - pp[0], p[1] - pp[1], p[2] - pp[2]), local[i]))
		model.append(m)
	return model


def skin_matrices(model, yaw: float):
	"""Per bone: rot_y(yaw) * model * bind^-1, 3x4 rows, anchor-relative (anchor = Snake's root)."""
	ry = rot_y(yaw)
	out = []
	for i, (_, p, _) in enumerate(BONES):
		m = mat_mul(ry, mat_mul(model[i], translate(-p[0], -p[1], -p[2])))
		out.append(tuple(m[r][c] for r in range(3) for c in range(4)))
	return out


# ---- geometry -----------------------------------------------------------------------------------
def half(x: float) -> bytes:
	return struct.pack("<e", x)


class MeshBuilder:
	def __init__(self):
		self.verts = []  # (pos, normal, tangent4, uv, weights4, indices4, color4)
		self.indices = []

	def add(self, pos, nrm, tan, uv, w=(255, 0, 0, 0), bi=(0, 0, 0, 0), col=(255, 255, 255, 255)):
		self.verts.append((pos, nrm, tan, uv, w, bi, col))
		return len(self.verts) - 1

	def tri(self, a, b, c):
		# Front faces have cross(b - a, c - a) pointing outwards: with D3D's default rasterizer state
		# (clockwise front) and an orientation-preserving camera that is what renders as the front.
		self.indices += [a, b, c]

	def cylinder(self, bone, a, b, radius, parent=None, sides=12, v0=0.0, uscale=1.0):
		axis = norm(tuple(b[i] - a[i] for i in range(3)))
		ref = (0.0, 0.0, 1.0) if abs(axis[2]) < 0.9 else (1.0, 0.0, 0.0)
		ex = norm(cross(ref, axis))
		ey = cross(axis, ex)
		length = math.dist(a, b)
		rings = []
		for k, t in enumerate((0.0, 0.5, 1.0)):
			w = (128, 127, 0, 0) if (k == 0 and parent is not None) else (255, 0, 0, 0)
			bi = (bone, parent if parent is not None else 0, 0, 0)
			c = tuple(a[i] + (b[i] - a[i]) * t for i in range(3))
			ring = []
			for s in range(sides + 1):
				ang = 2 * math.pi * s / sides
				n = tuple(math.cos(ang) * ex[i] + math.sin(ang) * ey[i] for i in range(3))
				tg = tuple(-math.sin(ang) * ex[i] + math.cos(ang) * ey[i] for i in range(3))
				p = tuple(c[i] + n[i] * radius for i in range(3))
				ring.append(self.add(p, n, tg + (1.0,), (s / sides * uscale, v0 + t * length * 2.0), w, bi))
			rings.append(ring)
		for r in range(2):
			for s in range(sides):
				i0, i1, i2, i3 = rings[r][s], rings[r][s + 1], rings[r + 1][s], rings[r + 1][s + 1]
				self.tri(i0, i1, i2)
				self.tri(i1, i3, i2)
		# caps
		for end, (ring, sign) in enumerate(((rings[0], -1.0), (rings[2], 1.0))):
			cpos = a if end == 0 else b
			n = tuple(axis[i] * sign for i in range(3))
			w = (128, 127, 0, 0) if (end == 0 and parent is not None) else (255, 0, 0, 0)
			bi = (bone, parent if parent is not None else 0, 0, 0)
			centre = self.add(cpos, n, ex + (1.0,), (0.5, 0.5), w, bi)
			cap = []
			for s in range(sides):
				p = self.verts[ring[s]][0]
				cap.append(self.add(p, n, ex + (1.0,), (0.5 + 0.5 * math.cos(2 * math.pi * s / sides), 0.5 + 0.5 * math.sin(2 * math.pi * s / sides)), w, bi))
			for s in range(sides):
				a_, b_ = cap[s], cap[(s + 1) % sides]
				if sign > 0:
					self.tri(centre, a_, b_)
				else:
					self.tri(centre, b_, a_)

	def box(self, bone, centre, half_size, w=(255, 0, 0, 0), bi=None, uv_scale=1.0):
		bi = bi or (bone, 0, 0, 0)
		cx, cy, cz = centre
		hx, hy, hz = half_size
		faces = [((1, 0, 0), (0, 0, -1)), ((-1, 0, 0), (0, 0, 1)), ((0, 1, 0), (1, 0, 0)), ((0, -1, 0), (1, 0, 0)), ((0, 0, 1), (1, 0, 0)), ((0, 0, -1), (-1, 0, 0))]
		for n, tg in faces:
			bt = cross(n, tg)
			corners = []
			for (su, sv) in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
				p = (cx + (n[0] + tg[0] * su + bt[0] * sv) * hx, cy + (n[1] + tg[1] * su + bt[1] * sv) * hy, cz + (n[2] + tg[2] * su + bt[2] * sv) * hz)
				corners.append(self.add(p, n, tg + (1.0,), ((su + 1) * 0.5 * uv_scale, (1 - sv) * 0.5 * uv_scale), w, bi))
			# n = tg x bt is outward: (0,1,2) and (0,2,3) have outward cross products
			self.tri(corners[0], corners[1], corners[2])
			self.tri(corners[0], corners[2], corners[3])


def humanoid_mesh() -> tuple[bytes, int]:
	mb = MeshBuilder()
	for bone, a, b, r, parent in SEGMENTS:
		mb.cylinder(bone, a, b, r, parent)
	mb.box(2, (0.0, 1.72, 0.12), (0.035, 0.03, 0.04))           # nose: Snake's front (+Z)
	mb.box(1, (0.0, 1.30, -0.21), (0.12, 0.14, 0.05))            # backpack
	mb.box(12, (0.11, 0.04, 0.06), (0.06, 0.04, 0.12))           # boots, toes forward (+Z)
	mb.box(15, (-0.11, 0.04, 0.06), (0.06, 0.04, 0.12))
	elements = [
		proto.RenInputElement(semantic=b"POSITION", semanticIndex=0, format=R32G32B32_FLOAT, inputSlot=0, alignedByteOffset=0),
		proto.RenInputElement(semantic=b"NORMAL", semanticIndex=0, format=R16G16B16A16_FLOAT, inputSlot=1, alignedByteOffset=0),
		proto.RenInputElement(semantic=b"TANGENT", semanticIndex=0, format=R16G16B16A16_FLOAT, inputSlot=1, alignedByteOffset=8),
		proto.RenInputElement(semantic=b"TEXCOORD", semanticIndex=0, format=R16G16_FLOAT, inputSlot=1, alignedByteOffset=16),
		proto.RenInputElement(semantic=b"BLENDWEIGHT", semanticIndex=0, format=R8G8B8A8_UNORM, inputSlot=1, alignedByteOffset=20),
		proto.RenInputElement(semantic=b"BLENDINDICES", semanticIndex=0, format=R8G8B8A8_UINT, inputSlot=1, alignedByteOffset=24),
		proto.RenInputElement(semantic=b"COLOR", semanticIndex=0, format=R8G8B8A8_UNORM, inputSlot=1, alignedByteOffset=28),
	]
	s0 = b"".join(struct.pack("<3f", *v[0]) for v in mb.verts)
	s1 = bytearray()
	for pos, n, tg, uv, w, bi, col in mb.verts:
		s1 += b"".join(half(c) for c in (*n, 0.0)) + b"".join(half(c) for c in tg) + half(uv[0]) + half(uv[1])
		s1 += bytes(w) + bytes(bi) + bytes(col)
	ib = struct.pack(f"<{len(mb.indices)}H", *mb.indices)
	mesh = proto.RenMesh(elementCount=len(elements), vertexStride=12, vertexBytes=len(s0) + len(s1), indexFormat=R16_UINT,
		indexBytes=len(ib), topology=TRIANGLELIST, streams=2)
	mesh.streamStride[0], mesh.streamStride[1] = 12, 32
	mesh.streamBytes[0], mesh.streamBytes[1] = len(s0), len(s1)
	mesh.key = 0
	body = b"".join(bytes(e) for e in elements) + s0 + bytes(s1) + ib
	mesh.key = fnv64(bytes(mesh), body)
	return proto.build_mesh_msg(mesh, elements, s0 + bytes(s1), ib), len(mb.indices)


def walker_mesh() -> tuple[bytes, int]:
	"""D-Walker stand-in: body, two legs, a gun barrel; rigid, one stream, UNORM8 normals, 32-bit indices."""
	mb = MeshBuilder()
	mb.box(0, (0.0, 1.25, 0.0), (0.45, 0.35, 0.55), uv_scale=2.0)
	mb.box(0, (0.35, 0.55, 0.0), (0.1, 0.5, 0.12))
	mb.box(0, (-0.35, 0.55, 0.0), (0.1, 0.5, 0.12))
	mb.box(0, (0.25, 1.35, 0.75), (0.05, 0.05, 0.3))
	elements = [
		proto.RenInputElement(semantic=b"POSITION", semanticIndex=0, format=R32G32B32_FLOAT, inputSlot=0, alignedByteOffset=0),
		proto.RenInputElement(semantic=b"NORMAL", semanticIndex=0, format=R8G8B8A8_UNORM, inputSlot=0, alignedByteOffset=12),
		proto.RenInputElement(semantic=b"TEXCOORD", semanticIndex=0, format=R32G32_FLOAT, inputSlot=0, alignedByteOffset=16),
	]
	vb = bytearray()
	for pos, n, tg, uv, w, bi, col in mb.verts:
		vb += struct.pack("<3f", *pos) + bytes(int(round((c * 0.5 + 0.5) * 255)) for c in (*n, 0.0)) + struct.pack("<2f", *uv)
	ib = struct.pack(f"<{len(mb.indices)}I", *mb.indices)
	mesh = proto.RenMesh(elementCount=len(elements), vertexStride=24, vertexBytes=len(vb), indexFormat=R32_UINT, indexBytes=len(ib),
		topology=TRIANGLELIST, streams=1)
	mesh.streamStride[0] = 24
	mesh.streamBytes[0] = len(vb)
	body = b"".join(bytes(e) for e in elements) + bytes(vb) + ib
	mesh.key = fnv64(bytes(mesh), body)
	return proto.build_mesh_msg(mesh, elements, bytes(vb), ib), len(mb.indices)


# ---- textures -----------------------------------------------------------------------------------
def _rgb565(c):
	return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)


def _from565(v):
	r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
	return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def bc1_encode(pixels, w, h) -> bytes:
	"""Plain BC1 (endpoints = darkest/brightest of the block, 4-colour mode)."""
	out = bytearray()
	for by in range(0, h, 4):
		for bx in range(0, w, 4):
			block = [pixels[min(h - 1, by + y) * w + min(w - 1, bx + x)] for y in range(4) for x in range(4)]
			lum = [p[0] * 3 + p[1] * 6 + p[2] for p in block]
			hi, lo = block[lum.index(max(lum))], block[lum.index(min(lum))]
			c0, c1 = _rgb565(hi), _rgb565(lo)
			if c0 == c1:
				out += struct.pack("<HHI", c0, c1, 0)
				continue
			if c0 < c1:
				c0, c1 = c1, c0
			p0, p1 = _from565(c0), _from565(c1)
			pal = [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)), tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
			bits = 0
			for i, p in enumerate(block):
				best = min(range(4), key=lambda k: sum((p[j] - pal[k][j]) ** 2 for j in range(3)))
				bits |= best << (2 * i)
			out += struct.pack("<HHI", c0, c1, bits)
	return bytes(out)


def downsample(pixels, w, h):
	nw, nh = max(1, w // 2), max(1, h // 2)
	out = []
	for y in range(nh):
		for x in range(nw):
			acc = [0, 0, 0, 0]
			for dy in (0, 1):
				for dx in (0, 1):
					p = pixels[min(h - 1, 2 * y + dy) * w + min(w - 1, 2 * x + dx)]
					for c in range(len(p)):
						acc[c] += p[c]
			out.append(tuple(a // 4 for a in acc[:len(pixels[0])]))
	return out, nw, nh


def texture_msg(fmt: int, w: int, h: int, mips: list[bytes]) -> bytes:
	data = b"".join(mips)
	tex = proto.RenTexture(key=0, format=fmt, width=w, height=h, mipLevels=len(mips), arraySize=1, dataBytes=len(data))
	tex.key = fnv64(bytes(tex), data)
	return bytes(tex) + data


def camo_albedo(size=256) -> bytes:
	"""Tiger-stripe-ish fatigues (sRGB values), BC1 with every mip."""
	cols = [(92, 88, 58), (64, 60, 40), (120, 104, 70), (40, 38, 30)]
	px = []
	for y in range(size):
		for x in range(size):
			u, v = x / size, y / size
			n = math.sin(u * 19 + math.sin(v * 7) * 2) + math.sin(v * 23 + u * 5) * 0.7 + math.sin((u + v) * 41) * 0.3
			k = 0 if n < -0.5 else 1 if n < 0.2 else 2 if n < 0.9 else 3
			if (x // 8 + y // 8) % 2 == 0 and k == 0:
				k = 1
			px.append(cols[k])
	mips = []
	w = h = size
	while True:
		mips.append(bc1_encode(px, w, h))
		if w == 1 and h == 1:
			break
		px, w, h = downsample(px, w, h)
	return texture_msg(BC1_UNORM, size, size, mips)


def weave_normal(size=128) -> bytes:
	"""Tangent-space normal map of a fabric weave (xy in rg, z in b)."""
	px = bytearray()
	for y in range(size):
		for x in range(size):
			dx = 0.35 * math.cos(x / size * 2 * math.pi * 16) * (1 if (y // 4) % 2 else -1)
			dy = 0.35 * math.cos(y / size * 2 * math.pi * 16) * (1 if (x // 4) % 2 else -1)
			n = norm((dx, dy, 1.0))
			px += bytes((int((n[0] * 0.5 + 0.5) * 255), int((n[1] * 0.5 + 0.5) * 255), int((n[2] * 0.5 + 0.5) * 255), 255))
	return texture_msg(R8G8B8A8_UNORM, size, size, [bytes(px)])


def srm_map(size=64, spec=80, rough=170, metal=0) -> bytes:
	px = bytearray()
	for y in range(size):
		for x in range(size):
			r = rough + int(30 * math.sin(x * 0.7) * math.sin(y * 0.5))
			px += bytes((spec, max(0, min(255, r)), metal, 255))
	return texture_msg(R8G8B8A8_UNORM, size, size, [bytes(px)])


def walker_albedo(size=128) -> bytes:
	px = bytearray()
	for y in range(size):
		for x in range(size):
			panel = (x % 32 < 2) or (y % 32 < 2)
			base = 70 if panel else 128 + int(10 * math.sin(x * 0.3 + y * 0.2))
			px += bytes((base, base + 4, base + 12, 255))
	return texture_msg(R8G8B8A8_UNORM, size, size, [bytes(px)])


class Humanoid:
	"""The assets once, then per-frame pose + draw records."""

	def __init__(self):
		self.mesh_msg, self.index_count = humanoid_mesh()
		self.walker_msg, self.walker_index_count = walker_mesh()
		self.tex = {"albedo": camo_albedo(), "normal": weave_normal(), "srm": srm_map(), "walker": walker_albedo(),
			"walker_srm": srm_map(spec=120, rough=90, metal=200)}
		key = lambda msg: struct.unpack_from("<Q", msg, 0)[0]
		self.mesh_key = key(self.mesh_msg)
		self.walker_key = key(self.walker_msg)
		self.tex_keys = {k: key(v) for k, v in self.tex.items()}

	def messages(self):
		yield proto.RenType.Mesh, self.mesh_msg
		yield proto.RenType.Mesh, self.walker_msg
		for msg in self.tex.values():
			yield proto.RenType.Texture, msg

	def draw(self, t: float, yaw: float, walker_offset):
		"""(draw records, matrices) for this frame: Snake skinned with len(BONES) matrices, D-Walker rigid."""
		model = pose(t)
		mats = skin_matrices(model, yaw)
		snake = proto.DrawRecord(mesh=self.mesh_key, indexCount=self.index_count, startIndex=0, baseVertex=0, firstMatrix=0,
			matrixCount=len(mats), flags=0, owner=proto.DrawOwner.Player, material=1)
		snake.textures[0] = self.tex_keys["albedo"]
		snake.textures[1] = self.tex_keys["normal"]
		snake.textures[2] = self.tex_keys["srm"]
		w = mat_mul(translate(*walker_offset), rot_y(yaw))
		mats.append(tuple(w[r][c] for r in range(3) for c in range(4)))
		walker = proto.DrawRecord(mesh=self.walker_key, indexCount=self.walker_index_count, startIndex=0, baseVertex=0,
			firstMatrix=len(mats) - 1, matrixCount=1, flags=int(proto.DrawFlags.Rigid), owner=proto.DrawOwner.Buddy, material=2)
		walker.textures[0] = self.tex_keys["walker"]
		walker.textures[2] = self.tex_keys["walker_srm"]
		return [snake, walker], mats, model
