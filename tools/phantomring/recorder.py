"""Record what a real MGSV guest sends, so Elden Ring can be tested with MGSV's real Snake and D-Walker
without running MGSV (`replay.py` plays it back as a guest).

    pr record --seconds N --out F.prrec [--pad 'ly=32767'] [--pad-file F] [--warmup S] [--hud-every S]

It stands in for the Elden Ring host (fake_er.FakeHost: stamps a fresh session, so the guest re-sends
kRenClearAll and every live mesh/texture), keeps every render-ring message (assets), and from the end of
--warmup on records each new GuestState + PoseBlock, each draw list, each guest event and (at most every
--hud-every s) the HUD layer, with their arrival times. Assets that arrive after the warmup are kept too.

File format (all little-endian; one chunk after another until EOF):
    magic b"PRREC001"
    chunk: {u8 kind, u8 zlib, u16 0, u32 bytes, f64 t} + body (zlib-compressed when zlib = 1)
    kind 'M' meta (JSON: protocol version, path, pad, start time, guest status text)
    kind 'A' asset: u32 RenType + render-ring payload (t = arrival; assets are replayed at attach, in order)
    kind 'S' state: GuestState (0x200 bytes) + PoseBlock (up to its last used bone)
    kind 'D' draw list: the slot bytes (DrawList + DrawRecord[] + matrices), DrawSlotHdr.frame first (u64)
    kind 'E' guest event: GuestEvent (48 bytes)
    kind 'H' HUD: HudSlotHdr (0x40) + RGBA rows
    kind 'Z' end (JSON summary)
t is seconds since the recording started (warmup excluded; assets that came during warmup have t < 0).
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import signal
import struct
import sys
import time
import zlib

from . import fake_er, proto

MAGIC = b"PRREC001"
CHUNK = struct.Struct("<BBHId")  # kind, zlib, pad, bytes, t
POSE_BONES_OFF = proto.PoseBlock.bones.offset


class Writer:
	def __init__(self, path: str):
		self.path = path
		self.tmp = path + ".part"
		self.f = open(self.tmp, "wb")
		self.f.write(MAGIC)
		self.counts: dict[str, int] = {}
		self.bytes = 0

	def chunk(self, kind: str, t: float, body: bytes, compress: bool = True):
		z = 0
		if compress and len(body) > 256:
			c = zlib.compress(body, 1)
			if len(c) < len(body):
				body, z = c, 1
		self.f.write(CHUNK.pack(ord(kind), z, 0, len(body), t))
		self.f.write(body)
		self.counts[kind] = self.counts.get(kind, 0) + 1
		self.bytes += CHUNK.size + len(body)

	def close(self, summary: dict):
		self.chunk("Z", 0.0, json.dumps(summary).encode())
		self.f.close()
		os.replace(self.tmp, self.path)  # complete files only: another agent may be waiting for this path


def read_chunks(path: str):
	"""Yields (kind, t, body) for every chunk; stops quietly at a truncated tail."""
	with open(path, "rb") as f:
		if f.read(len(MAGIC)) != MAGIC:
			raise ValueError(f"{path}: not a PhantomRing recording")
		while True:
			head = f.read(CHUNK.size)
			if len(head) < CHUNK.size:
				return
			kind, z, _, n, t = CHUNK.unpack(head)
			body = f.read(n)
			if len(body) < n:
				return
			yield chr(kind), t, zlib.decompress(body) if z else body


def pose_bytes(link: proto.Link) -> bytes:
	"""The PoseBlock up to its last used bone (a consistent copy, or b'')."""
	pose = link.pose()
	if not pose:
		return b""
	n = min(pose.boneCount, proto.MAX_POSE_BONES)
	return bytes(pose)[:POSE_BONES_OFF + n * ctypes.sizeof(proto.PoseBone)]


class RecordHost(fake_er.FakeHost):
	def __init__(self, link, writer: Writer, warmup: float, hud_every: float, quiet: bool):
		super().__init__(link, quiet, float("inf"))
		self.w = writer
		self.warmup = warmup
		self.hud_every = hud_every
		self.next_hud = 0.0
		self.rec = {"states": 0, "draws": 0, "events": 0, "hud": 0, "assets": 0, "assetBytes": 0, "missingKeys": 0}
		self.referenced: set[int] = set()

	def t(self) -> float:
		return self.elapsed() - self.warmup

	def recording(self) -> bool:
		return self.guest_attached and self.t() >= 0

	def consume_render_ring(self):
		while (msg := self.link.render_ring.pop()) is not None:
			typ, payload = msg
			payload = bytes(payload)
			if typ == proto.RenType.ClearAll:
				self.meshes.clear()
				self.textures.clear()
			elif typ == proto.RenType.Mesh:
				self.meshes[proto.RenMesh.from_buffer_copy(payload, 0).key] = {}
			elif typ == proto.RenType.Texture:
				tex = proto.RenTexture.from_buffer_copy(payload, 0)
				self.textures[tex.key] = {}
				self.log(f"asset: texture {tex.key:016x} {tex.width}x{tex.height} fmt {tex.format} ({tex.dataBytes >> 10} KB)")
			self.w.chunk("A", self.t(), struct.pack("<I", typ) + payload)
			self.rec["assets"] += 1
			self.rec["assetBytes"] += len(payload)

	def on_guest_frame(self, gs):
		if self.recording():
			self.w.chunk("S", self.t(), bytes(gs) + pose_bytes(self.link))
			self.rec["states"] += 1

	def on_guest_event(self, ev):
		if self.recording():
			self.w.chunk("E", self.t(), bytes(ev), compress=False)
			self.rec["events"] += 1

	def on_draw_slot(self, index: int, hdr):
		if not self.recording():
			return
		n = min(hdr.bytes, proto.DRAW_SLOT_BYTES)
		off = proto.OFF_DRAW_SLOTS + index * proto.DRAW_SLOT_BYTES
		blob = bytes(self.link.mm[off:off + n])
		dl, draws, _ = proto.parse_draw_list(blob, 0, len(blob))
		for d in draws:
			for k in (d.mesh, *d.textures):
				if k and k not in self.meshes and k not in self.textures and k not in self.referenced:
					self.rec["missingKeys"] += 1
				if k:
					self.referenced.add(k)
		self.w.chunk("D", self.t(), struct.pack("<Q", hdr.frame) + blob)
		self.rec["draws"] += 1

	def on_hud_slot(self, hdr, pixels):
		if self.recording() and self.hud_every >= 0 and self.t() >= self.next_hud:
			self.next_hud = self.t() + self.hud_every
			self.w.chunk("H", self.t(), bytes(hdr) + bytes(pixels))
			self.rec["hud"] += 1


def main(argv=None) -> int:
	ap = argparse.ArgumentParser(prog="pr record", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--path", default=None, help="mapping file (default $PHANTOMRING_SHM or /dev/shm/phantomring_v1)")
	ap.add_argument("--out", required=True, help="recording file (written as F.part, renamed when complete)")
	ap.add_argument("--seconds", type=float, default=10.0, help="recorded time after the warmup")
	ap.add_argument("--warmup", type=float, default=3.0, help="seconds after attaching before recording starts "
		"(the guest re-sends its assets; the pad already applies)")
	ap.add_argument("--wait-ready", type=float, default=600.0, help="wait up to this long for the guest to be Ready first "
		"(0 = don't: fake_mgsv only becomes Ready once a host attaches)")
	ap.add_argument("--pad", default="neutral", help="fixed pad during the recording (fake_er pad-file syntax, e.g. 'ly=32767')")
	ap.add_argument("--pad-file", help="live pad file instead of --pad")
	ap.add_argument("--hud-every", type=float, default=0.1, help="keep the HUD layer at most this often (s); -1 = never")
	ap.add_argument("--actors", choices=["fixed", "near"], default="fixed", help="the two published actors, as fake_er's --actors "
		"(near: standing still beside Snake's start, moved/flagged with `pr er actor ...`; Fulton recordings, docs/GUEST.md 20.4)")
	ap.add_argument("--force", action="store_true", help="take the host side over even if another live process holds it")
	ap.add_argument("--quiet", action="store_true")
	args = ap.parse_args(argv)

	path = args.path or proto.default_path()
	try:
		link = proto.Link(path)
	except proto.MappingMissing as e:
		print(f"record: no mapping ({e})", file=sys.stderr)
		return 2
	stop = False

	def on_signal(*_):
		nonlocal stop
		stop = True

	signal.signal(signal.SIGTERM, on_signal)
	signal.signal(signal.SIGINT, on_signal)

	# A real MGSV only sends its assets to a host it is attached to: wait for it to be Ready (booted)
	# before stamping our session, so the warmup gets the full re-send.
	deadline = time.monotonic() + args.wait_ready
	while not stop and args.wait_ready > 0:
		g = link.side("guest")
		if g["alive"] and g["statusName"] == "Ready":
			break
		if time.monotonic() > deadline:
			print(f"record: guest not Ready ({g['statusName']} {g['text']!r})", file=sys.stderr)
			return 3
		time.sleep(0.5)

	# Never stamp a new host session over a live host (Elden Ring or another stand-in): ER stamps its
	# nonce once, so the guest would follow us and never come back to it.
	if link.refuse_if_taken("host", "record", path, args.force):
		return 4
	os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
	w = Writer(args.out)
	host = RecordHost(link, w, args.warmup, args.hud_every, args.quiet)
	host.actors_mode = args.actors
	if args.pad_file:
		host.pad_file = fake_er.PadFile(args.pad_file)
	else:
		host.pad_override = fake_er.parse_pad_line(args.pad) or None
	w.chunk("M", -args.warmup, json.dumps({"version": proto.VERSION, "mapping": path, "pad": args.pad, "padFile": args.pad_file,
		"warmup": args.warmup, "seconds": args.seconds, "unixTime": time.time(), "guestText": link.side("guest")["text"]}).encode())
	host.attach()
	dt = 1.0 / fake_er.FPS
	next_t = time.monotonic()
	last_report = 0.0
	while not stop and host.t() < args.seconds:
		host.tick(dt)
		if host.elapsed() - last_report >= 1.0:
			last_report = host.elapsed()
			r = host.rec
			host.log(f"{'recording' if host.recording() else 'warmup'} t={host.t():.1f}: states {r['states']} draw lists {r['draws']} "
				f"events {r['events']} hud {r['hud']} assets {r['assets']} ({r['assetBytes'] / 1e6:.1f} MB), file {w.bytes / 1e6:.1f} MB")
		next_t += dt
		delay = next_t - time.monotonic()
		if delay > 0:
			time.sleep(delay)
		else:
			next_t = time.monotonic()
	h = link.header
	h.hostStatus = int(proto.Status.Exiting)
	proto.set_text(h, "hostStatusText", "recorder: done")
	summary = host.rec | {"meshes": len(host.meshes), "textures": len(host.textures), "events": host.stats["events"],
		"chunks": w.counts, "referencedKeys": len(host.referenced),
		"unresolvedKeys": len([k for k in host.referenced if k not in host.meshes and k not in host.textures])}
	w.close(summary)
	print(json.dumps(summary))
	return 0 if host.rec["states"] else 1


if __name__ == "__main__":
	sys.exit(main())
