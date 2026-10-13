"""A realistic stand-in for MGSV's HUD layer (what the real guest publishes on the HUD triple buffer, see
mgsv/Hud.h): a 1280x720 transparent RGBA8 picture, premultiplied, with MGSV's aim reticle while Snake aims
and the weapon/ammo box in the bottom-right corner. fake_mgsv publishes it with --hud mgsv.

Stdlib only: rectangles and a 5x7 bitmap font drawn into a bytearray. Made only when what it shows
changes (ammo, aiming), so publishing it every few frames costs one 3.7 MB copy.
"""

from __future__ import annotations

W, H = 1280, 720

# 5x7 glyphs, one string of 7 rows of 5 bits per character (only what the box shows).
FONT = {
	"0": "01110 10001 10011 10101 11001 10001 01110", "1": "00100 01100 00100 00100 00100 00100 01110",
	"2": "01110 10001 00001 00010 00100 01000 11111", "3": "11110 00001 00001 01110 00001 00001 11110",
	"4": "00010 00110 01010 10010 11111 00010 00010", "5": "11111 10000 11110 00001 00001 10001 01110",
	"6": "00110 01000 10000 11110 10001 10001 01110", "7": "11111 00001 00010 00100 01000 01000 01000",
	"8": "01110 10001 10001 01110 10001 10001 01110", "9": "01110 10001 10001 01111 00001 00010 01100",
	"/": "00001 00010 00010 00100 01000 01000 10000", "-": "00000 00000 00000 11111 00000 00000 00000",
	" ": "00000 00000 00000 00000 00000 00000 00000", ".": "00000 00000 00000 00000 00000 01100 01100",
	"A": "01110 10001 10001 11111 10001 10001 10001", "M": "10001 11011 10101 10101 10001 10001 10001",
	"R": "11110 10001 10001 11110 10100 10010 10001", "S": "01111 10000 10000 01110 00001 00001 11110",
	"F": "11111 10000 10000 11110 10000 10000 10000", "K": "10001 10010 10100 11000 10100 10010 10001",
	"E": "11111 10000 10000 11110 10000 10000 11111", "L": "10000 10000 10000 10000 10000 10000 11111",
	"T": "11111 00100 00100 00100 00100 00100 00100", "H": "10001 10001 10001 11111 10001 10001 10001",
	"G": "01110 10001 10000 10111 10001 10001 01111", "N": "10001 11001 10101 10011 10001 10001 10001",
	"O": "01110 10001 10001 10001 10001 10001 01110", "D": "11100 10010 10001 10001 10001 10010 11100",
	"W": "10001 10001 10001 10101 10101 10101 01010", "P": "11110 10001 10001 11110 10000 10000 10000",
}

WHITE = (236, 233, 224)
AMBER = (232, 163, 61)


class Canvas:
	def __init__(self, w: int = W, h: int = H):
		self.w, self.h = w, h
		self.px = bytearray(w * h * 4)  # premultiplied RGBA, transparent

	def rect(self, x: int, y: int, w: int, h: int, rgb, a: float):
		"""Premultiplied "over" of a flat rectangle (a solid colour at coverage a)."""
		x0, y0, x1, y1 = max(0, x), max(0, y), min(self.w, x + w), min(self.h, y + h)
		if x0 >= x1 or y0 >= y1:
			return
		ia = 1.0 - a
		src = (round(rgb[0] * a), round(rgb[1] * a), round(rgb[2] * a), round(255 * a))
		for yy in range(y0, y1):
			o = (yy * self.w + x0) * 4
			row = self.px[o:o + (x1 - x0) * 4]
			if ia == 0.0:
				row = bytes(src) * (x1 - x0)
			else:
				row = bytes(min(255, src[i % 4] + round(row[i] * ia)) for i in range(len(row)))
			self.px[o:o + (x1 - x0) * 4] = row

	def text(self, x: int, y: int, s: str, scale: int, rgb, shadow: bool = True):
		for ch in s.upper():
			glyph = FONT.get(ch, FONT[" "]).split()
			for gy, bits in enumerate(glyph):
				for gx, bit in enumerate(bits):
					if bit == "1":
						if shadow:
							self.rect(x + gx * scale + 1, y + gy * scale + 1, scale, scale, (0, 0, 0), 0.7)
						self.rect(x + gx * scale, y + gy * scale, scale, scale, rgb, 1.0)
			x += 6 * scale

	def reticle(self, cx: int, cy: int):
		"""MGSV's assault-rifle reticle: four ticks with a gap and a centre dot, white with a dark rim."""
		for dark in (True, False):
			grow = 1 if dark else 0
			rgb, a = ((0, 0, 0), 0.6) if dark else (WHITE, 1.0)
			for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
				gap, length, thick = 10, 14, 2
				if dx:
					x = cx + dx * gap if dx > 0 else cx - gap - length
					self.rect(x - grow, cy - thick // 2 - grow, length + 2 * grow, thick + 2 * grow, rgb, a)
				else:
					y = cy + dy * gap if dy > 0 else cy - gap - length
					self.rect(cx - thick // 2 - grow, y - grow, thick + 2 * grow, length + 2 * grow, rgb, a)
			self.rect(cx - 1 - grow, cy - 1 - grow, 2 + 2 * grow, 2 + 2 * grow, rgb, a)


def mgsv_hud(ammo: int, stock: int, aiming: bool, weapon: str = "AM MRS-4") -> bytes:
	"""The picture (premultiplied RGBA8 rows, W x H)."""
	c = Canvas()
	if aiming:
		c.reticle(W // 2, H // 2)
	# Weapon box, bottom right: dark translucent plate, weapon name, amber rule, magazine / reserve.
	bx, by, bw, bh = W - 300, H - 118, 262, 82
	c.rect(bx, by, bw, bh, (12, 14, 12), 0.55)
	c.rect(bx, by, 4, bh, AMBER, 1.0)
	c.text(bx + 16, by + 10, weapon, 2, WHITE)
	c.rect(bx + 16, by + 30, bw - 32, 2, AMBER, 0.9)
	c.text(bx + 16, by + 40, f"{ammo:2d}", 5, WHITE)
	c.text(bx + 16 + 6 * 5 * 2 + 10, by + 52, f"/ {stock}", 3, WHITE)
	return bytes(c.px)


def life_bar(life: int, life_max: int) -> tuple[int, int, bytes]:
	"""The old 64x8 bar (fake_mgsv --hud bar; test_fakes.py checks its size)."""
	w, h = 64, 8
	filled = int(w * life / max(1, life_max))
	row = bytes((40, 200, 60, 255)) * filled + bytes((0, 0, 0, 128)) * (w - filled)
	return w, h, row * h
