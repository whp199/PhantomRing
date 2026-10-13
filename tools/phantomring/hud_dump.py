"""Dump the guest's HUD layer (protocol HUD triple buffer) to PNG, without being the reader.

The HUD reader is the host (or fake_er); this tool only peeks: it picks the slot with the newest frame
number (never the guest's back slot, whose header still holds an older frame while it is rewritten) and
copies it. Good enough for a still image while a stand-in host runs.

    python3 -m phantomring.hud_dump [--path MAPPING] [-o hud.png] [--over black|white|checker|none]
                                    [--straight] [--watch N --dir DIR]

Pixels in the slot are RGBA8, premultiplied (HudSlotHdr.flags bit 0) and sRGB-encoded when bit 1 is set.
--over composites the layer over a background (what Elden Ring would show over a flat scene); 'none'
writes the RGBA itself (--straight un-premultiplies it, so ordinary viewers show it right).
"""

from __future__ import annotations

import argparse
import ctypes
import os
import sys
import time

from . import proto


def newest_slot(link: proto.Link):
	best = None
	for i in range(3):
		hdr = proto.HudSlotHdr.from_buffer_copy(link.mm, proto.OFF_HUD_SLOT_HDRS + i * ctypes.sizeof(proto.HudSlotHdr))
		if 0 < hdr.width <= proto.MAX_HUD_W and 0 < hdr.height <= proto.MAX_HUD_H and (best is None or hdr.frame > best[1].frame):
			best = (i, hdr)
	return best


def read_layer(link: proto.Link):
	"""-> (header, rgba bytes) of the newest published HUD frame, or None."""
	found = newest_slot(link)
	if not found:
		return None
	i, hdr = found
	off = proto.OFF_HUD_SLOTS + i * proto.HUD_SLOT_BYTES
	return hdr, bytes(link.mm[off:off + hdr.width * hdr.height * 4])


def to_image(hdr, rgba: bytes, over: str, straight: bool):
	from PIL import Image  # only this tool needs Pillow

	img = Image.frombytes("RGBA", (hdr.width, hdr.height), rgba)
	premult = bool(hdr.flags & 1)
	if over == "none":
		if premult and straight:
			# Pillow's 'RGBa' mode is premultiplied: converting it to RGBA divides by alpha.
			img = Image.frombytes("RGBa", img.size, rgba).convert("RGBA")
		return img
	w, h = img.size
	if over == "checker":
		bg = Image.new("RGB", (w, h), (90, 90, 90))
		tile = Image.new("RGB", (16, 16), (140, 140, 140))
		for y in range(0, h, 32):
			for x in range(0, w, 32):
				bg.paste(tile, (x, y))
				bg.paste(tile, (x + 16, y + 16))
	else:
		bg = Image.new("RGB", (w, h), (0, 0, 0) if over == "black" else (255, 255, 255))
	if premult:
		# out = layer + bg * (1 - a)   (blending in the encoded space: close enough to judge the layer)
		from PIL import ImageChops
		a = img.getchannel("A")
		inv = Image.eval(a, lambda v: 255 - v)
		bg_part = Image.merge("RGB", [ImageChops.multiply(c, inv) for c in bg.split()])
		return ImageChops.add(img.convert("RGB"), bg_part)
	bg.paste(img, (0, 0), img)
	return bg


def main(argv=None) -> int:
	ap = argparse.ArgumentParser(prog="hud_dump", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--path", default=None, help="mapping file (default $PHANTOMRING_SHM or /dev/shm/phantomring_v1)")
	ap.add_argument("-o", "--out", default="hud.png")
	ap.add_argument("--over", default="checker", choices=("black", "white", "checker", "none"))
	ap.add_argument("--straight", action="store_true", help="with --over none: un-premultiply")
	ap.add_argument("--watch", type=int, default=0, help="dump N frames, one per --every seconds, into --dir")
	ap.add_argument("--every", type=float, default=0.5)
	ap.add_argument("--dir", default=".")
	args = ap.parse_args(argv)
	try:
		link = proto.Link(args.path or proto.default_path())
	except proto.MappingMissing as e:
		print(f"hud_dump: no mapping ({e})", file=sys.stderr)
		return 2
	print(f"hud frames published: {link.hud_ctl.framesPublished} (state {link.hud_ctl.state:#x})")
	shots = max(1, args.watch)
	for n in range(shots):
		layer = read_layer(link)
		if not layer:
			print("hud_dump: no HUD frame published yet", file=sys.stderr)
			return 3
		hdr, rgba = layer
		alpha = rgba[3::4]
		covered = sum(1 for v in alpha if v)
		out = args.out if not args.watch else os.path.join(args.dir, f"hud_{n:03d}_f{hdr.frame}.png")
		to_image(hdr, rgba, args.over, args.straight).save(out)
		print(f"{out}: frame {hdr.frame} {hdr.width}x{hdr.height} flags {hdr.flags:#x} "
			f"({'premultiplied' if hdr.flags & 1 else 'straight'}{', sRGB' if hdr.flags & 2 else ''}), "
			f"{covered} covered pixels ({100.0 * covered / max(1, hdr.width * hdr.height):.2f}%)")
		if n + 1 < shots:
			time.sleep(args.every)
	return 0


if __name__ == "__main__":
	sys.exit(main())
