"""Validate the MGSV guest's draw capture without Elden Ring.

    python3 -m phantomring.capture_view host   [--cache DIR] [--seconds N] [--pad neutral|script]
    python3 -m phantomring.capture_view render [--cache DIR] [-o out.png] [--width 1280 --height 720]
                                               [--compare mgsv.png] [--flat] [--frame latest]

`host` stands in for the Elden Ring host (fake_er.py's FakeHost, with a neutral pad and no hurts by
default) and keeps what the guest sends: every kRenMesh / kRenTexture payload is stored in the cache
directory by key, and the newest DrawList (plus the GuestState of that moment: MGSV's camera) is saved
there several times a second as drawlist.bin / guest.bin.

`render` software-renders the latest DrawList from MGSV's own camera (GuestState camPos/camQuat/camFovY)
with the cached meshes: skinning with the published matrices (anchor-relative, + anchorPos), a z-buffer,
and albedo sampling (BC1/BC3/BC7 decoded with numpy; anything else flat-shaded). With --compare it puts
the render next to a gamescope screenshot of MGSV (gamescopectl screenshot) for a silhouette check.

Everything here is the DRAW CONTRACT read back exactly as the host must read it (protocol v2).
"""

from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
import shutil
import signal
import struct
import sys
import time

from . import fake_er, proto

DEFAULT_CACHE = os.path.join(os.environ.get("TMPDIR", "/tmp"), "phantomring_capture")


# ---- host: keep what the guest sends ---------------------------------------------------------------
class CaptureHost(fake_er.FakeHost):
	def __init__(self, link, quiet, hurt_every, cache: str):
		super().__init__(link, quiet, hurt_every)
		self.cache = cache
		os.makedirs(os.path.join(cache, "mesh"), exist_ok=True)
		os.makedirs(os.path.join(cache, "tex"), exist_ok=True)
		self.last_save = 0.0
		self.draw_lists = 0
		self.bytes_in = 0

	def consume_render_ring(self):
		st = self.stats
		while (msg := self.link.render_ring.pop()) is not None:
			typ, payload = msg
			payload = bytes(payload)
			self.bytes_in += len(payload)
			if typ == proto.RenType.ClearAll:
				st["renderClearAll"] += 1
				self.log("render: clear all")
			elif typ == proto.RenType.Mesh:
				mesh = proto.RenMesh.from_buffer_copy(payload, 0)
				with open(os.path.join(self.cache, "mesh", f"{mesh.key:016x}.bin"), "wb") as f:
					f.write(payload)
				self.meshes[mesh.key] = {"vertexBytes": mesh.vertexBytes}
				st["renderMeshes"] += 1
				_, elements, _, _ = proto.parse_mesh_msg(payload)
				self.log(f"render: mesh {mesh.key:016x} {mesh.vertexBytes} B vertices ({mesh.streams} streams), {mesh.indexBytes} B indices, "
					f"layout {' '.join(f'{e.semantic.decode()}{e.semanticIndex}:{e.format}@{e.inputSlot}+{e.alignedByteOffset}' for e in elements)}")
			elif typ == proto.RenType.Texture:
				tex = proto.RenTexture.from_buffer_copy(payload, 0)
				with open(os.path.join(self.cache, "tex", f"{tex.key:016x}.bin"), "wb") as f:
					f.write(payload)
				self.textures[tex.key] = {"w": tex.width, "h": tex.height, "format": tex.format}
				st["renderTextures"] += 1
				self.log(f"render: texture {tex.key:016x} {tex.width}x{tex.height} format {tex.format} mips {tex.mipLevels} ({tex.dataBytes} B)")
			elif typ == proto.RenType.Forget:
				st["renderForget"] += 1

	def tick(self, dt: float):
		super().tick(dt)
		# FakeHost.tick consumed the draw slot (front) if a new one arrived; keep the newest a few times a second.
		if self.guest_attached and self.stats["drawFrames"] and time.monotonic() - self.last_save > 0.25:
			self.last_save = time.monotonic()
			hdr = proto.DrawSlotHdr.from_buffer_copy(self.link.mm, proto.OFF_DRAW_SLOT_HDRS + self.draw.front * ctypes.sizeof(proto.DrawSlotHdr))
			n = min(hdr.bytes, proto.DRAW_SLOT_BYTES)
			off = proto.OFF_DRAW_SLOTS + self.draw.front * proto.DRAW_SLOT_BYTES
			blob = bytes(self.link.mm[off:off + n])
			gs = self.link.guest_state()
			tmp = os.path.join(self.cache, "drawlist.tmp")
			with open(tmp, "wb") as f:
				f.write(blob)
			os.replace(tmp, os.path.join(self.cache, "drawlist.bin"))
			if gs:
				with open(tmp, "wb") as f:
					f.write(bytes(gs))
				os.replace(tmp, os.path.join(self.cache, "guest.bin"))


def neutral_pad(_t: float):
	return 0, 0, 0, 0


def cmd_host(args) -> int:
	path = args.path or proto.default_path()
	if args.fresh and os.path.isdir(args.cache):
		shutil.rmtree(args.cache)
	try:
		link = proto.Link(path)
	except proto.MappingMissing as e:
		print(f"capture_view: no mapping ({e})", file=sys.stderr)
		return 2
	if args.pad == "neutral":
		fake_er.scripted_pad = neutral_pad
	host = CaptureHost(link, args.quiet, float("inf") if args.hurt_every <= 0 else args.hurt_every, args.cache)
	host.attach()
	stop = False

	def on_signal(*_):
		nonlocal stop
		stop = True

	signal.signal(signal.SIGTERM, on_signal)
	signal.signal(signal.SIGINT, on_signal)
	dt = 1.0 / fake_er.FPS
	next_t = time.monotonic()
	last_report = 0.0
	while not stop and (args.seconds <= 0 or host.elapsed() < args.seconds):
		host.tick(dt)
		if host.elapsed() - last_report >= 2.0:
			last_report = host.elapsed()
			st = host.stats
			host.log(f"guest {'attached' if host.guest_attached else 'absent'}: draw frames {st['drawFrames']} ({st['lastDrawCount']} draws, "
				f"{st['lastMatrixCount']} matrices), meshes {st['renderMeshes']} textures {st['renderTextures']} ({host.bytes_in / 1e6:.1f} MB in), "
				f"missing meshes {st['drawMissingMesh']}")
		next_t += dt
		delay = next_t - time.monotonic()
		if delay > 0:
			time.sleep(delay)
		else:
			next_t = time.monotonic()
	h = link.header
	h.hostStatus = int(proto.Status.Exiting)
	return 0


def main(argv=None) -> int:
	ap = argparse.ArgumentParser(prog="capture_view", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--path", default=None, help="mapping file (default $PHANTOMRING_SHM or /dev/shm/phantomring_v1)")
	ap.add_argument("--cache", default=DEFAULT_CACHE, help=f"where meshes/textures/draw lists are kept (default {DEFAULT_CACHE})")
	sub = ap.add_subparsers(dest="cmd", required=True)
	p = sub.add_parser("host", help="stand in for the Elden Ring host and keep what the guest sends")
	p.add_argument("--seconds", type=float, default=0)
	p.add_argument("--pad", choices=["neutral", "script"], default="neutral")
	p.add_argument("--hurt-every", type=float, default=0)
	p.add_argument("--fresh", action="store_true", help="empty the cache first")
	p.add_argument("--quiet", action="store_true")
	args = ap.parse_args(argv)
	if args.cmd == "host":
		return cmd_host(args)
	return 2


if __name__ == "__main__":
	sys.exit(main())
