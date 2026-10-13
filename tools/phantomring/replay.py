"""Play a recording (recorder.py) back as the MGSV guest, so Elden Ring draws MGSV's real Snake and
D-Walker without MGSV running.

    pr replay F.prrec [--loop] [--from S] [--to S] [--no-events] [--no-hud] [--speed X]

It attaches like the real guest (waits for the host's session, resets its producers, drains host events,
bumps odoEpoch, pushes kRenClearAll + every recorded asset, then sets guestNonce), heartbeats with
status Ready, and republishes the recorded GuestState/PoseBlock, draw lists, guest events and HUD layer at
their recorded times. Frame numbers and QPC stamps are the replay's own (monotonic across loops); with
--loop the odometer keeps growing across loops (each loop adds the recording's own odometer travel), so a
running recording keeps the puppet running. Assets go out once per host session (again if the host
re-attaches); a ring that is full is refilled on the next ticks.

Host events are drained and counted (Hurt/Respawn/...); the guest console answers '!status' and refuses Lua.
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

from . import proto
from .proto import HostEventType, Status
from .recorder import POSE_BONES_OFF, read_chunks

FPS_POLL = 240  # how often the replay loop looks at the clock (it publishes at the recorded times)


class Recording:
	def __init__(self, path: str, t_from: float = 0.0, t_to: float = float("inf"), events: bool = True, hud: bool = True):
		self.path = path
		self.meta: dict = {}
		self.summary: dict = {}
		self.assets: list[tuple[int, bytes]] = []
		self.timeline: list[tuple[float, str, bytes]] = []
		for kind, t, body in read_chunks(path):
			if kind == "M":
				self.meta = json.loads(body)
			elif kind == "Z":
				self.summary = json.loads(body)
			elif kind == "A":
				self.assets.append((struct.unpack_from("<I", body)[0], body[4:]))
			elif t_from <= t <= t_to and (kind in "SD" or (kind == "E" and events) or (kind == "H" and hud)):
				self.timeline.append((t - t_from, kind, body))
		self.timeline.sort(key=lambda x: x[0])
		states = [(t, b) for t, k, b in self.timeline if k == "S"]
		if not states:
			raise ValueError(f"{path}: no guest states in the chosen range")
		first = proto.GuestState.from_buffer_copy(states[0][1])
		last = proto.GuestState.from_buffer_copy(states[-1][1])
		self.odo_travel = [last.odo[i] - first.odo[i] for i in range(3)]
		self.duration = max(t for t, _, _ in self.timeline) + 1.0 / 60.0
		self.counts = {k: sum(1 for _, kk, _ in self.timeline if kk == k) for k in "SDEH"}


class ReplayGuest:
	def __init__(self, link: proto.Link, rec: Recording, loop: bool, speed: float, quiet: bool):
		self.link, self.rec, self.loop, self.speed, self.quiet = link, rec, loop, speed, quiet
		self.t0 = time.monotonic()
		self.nonce = 0
		self.attached = False
		self.odo_epoch = 0
		self.frame = 0
		self.draw = link.triple_writer(proto.OFF_DRAW_CTL)
		self.hud = link.triple_writer(proto.OFF_HUD_CTL)
		self.asset_queue: list[tuple[int, bytes]] = []
		self.play_t0 = 0.0
		self.index = 0
		self.loops = 0
		self.odo_offset = [0.0, 0.0, 0.0]
		self.stats = {"attaches": 0, "loops": 0, "states": 0, "draws": 0, "events": 0, "hud": 0, "assetsSent": 0,
			"assetsDeferred": 0, "hostEvents": {}, "consoleServed": 0}

	def elapsed(self) -> float:
		return time.monotonic() - self.t0

	def log(self, text: str):
		if not self.quiet:
			print(f"[replay    {self.elapsed():6.2f}s] {text}", flush=True)

	def status(self, status: Status, text: str):
		h = self.link.header
		h.guestStatus = int(status)
		proto.set_text(h, "guestStatusText", text)

	def try_attach(self) -> bool:
		h = self.link.header
		if h.magic != proto.MAGIC or h.version != proto.VERSION or h.sessionNonce == 0:
			self.attached = False
			return False
		if h.sessionNonce == self.nonce and h.guestNonce == self.nonce:
			return True
		self.nonce = h.sessionNonce
		h.guestNonce = 0
		self.draw.reset()
		self.hud.reset()
		self.link.host_events.drain()
		self.odo_epoch += 1
		self.odo_offset = [0.0, 0.0, 0.0]
		self.asset_queue = [(int(proto.RenType.ClearAll), b"")] + list(self.rec.assets)
		self.push_assets()
		self.play_t0, self.index, self.loops = time.monotonic(), 0, 0
		h.guestUnixPid = os.getpid()
		h.guestNonce = self.nonce
		self.attached = True
		self.stats["attaches"] += 1
		self.status(Status.Ready, f"replay: {os.path.basename(self.rec.path)}")
		self.log(f"attached to session {self.nonce:#018x} (epoch {self.odo_epoch}); {len(self.asset_queue)} assets still queued")
		return True

	def push_assets(self):
		ring = self.link.render_ring
		while self.asset_queue:
			typ, payload = self.asset_queue[0]
			if not ring.push(typ, payload):
				self.stats["assetsDeferred"] += 1
				return
			self.asset_queue.pop(0)
			self.stats["assetsSent"] += 1

	def tick(self):
		link = self.link
		link.header.guestHeartbeatQpc = proto.qpc_now()
		if not self.try_attach():
			return
		if self.asset_queue:
			self.push_assets()
		while (ev := link.host_events.pop()) is not None:
			name = proto.enum_name(ev.type, HostEventType)
			self.stats["hostEvents"][name] = self.stats["hostEvents"].get(name, 0) + 1
			self.log(f"host event {name} a={ev.a} b={ev.b}")
		if proto.console_serve(link.guest_console, self.console):
			self.stats["consoleServed"] += 1

		tl = self.rec.timeline
		now = (time.monotonic() - self.play_t0) * self.speed
		while True:
			if self.index >= len(tl):
				if not self.loop:
					return
				# Next loop: the odometer continues from where this loop ended.
				self.loops += 1
				self.stats["loops"] += 1
				self.index = 0
				self.play_t0 += self.rec.duration / self.speed
				now -= self.rec.duration
				for i in range(3):
					self.odo_offset[i] += self.rec.odo_travel[i]
			t, kind, body = tl[self.index]
			if t > now:
				break
			self.index += 1
			self.publish(kind, body)

	def publish(self, kind: str, body: bytes):
		link = self.link
		if kind == "S":
			self.frame += 1
			gs = proto.GuestState.from_buffer_copy(body)
			gs.frame = self.frame
			gs.qpc = proto.qpc_now()
			gs.odoEpoch = self.odo_epoch
			for i in range(3):
				gs.odo[i] += self.odo_offset[i]
			link.seq_write(proto.OFF_GUEST_STATE, gs)
			pose = body[ctypes.sizeof(proto.GuestState):]
			if len(pose) >= POSE_BONES_OFF:
				pb = proto.PoseBlock()
				ctypes.memmove(ctypes.addressof(pb), pose, min(len(pose), ctypes.sizeof(pb)))
				pb.frame = self.frame
				link.seq_write(proto.OFF_POSE, pb)
			self.stats["states"] += 1
		elif kind == "D":
			blob = bytearray(body[8:])
			struct.pack_into("<Q", blob, 0, self.frame)  # DrawList.frame
			link.write_draw_slot(self.draw.back, self.frame, bytes(blob))
			self.draw.publish()
			link.draw_ctl.framesPublished += 1
			self.stats["draws"] += 1
		elif kind == "E":
			ev = proto.GuestEvent.from_buffer_copy(body)
			ev.frame = self.frame & 0xFFFFFFFF
			# v5 throw/placed ids are unique per session: each --loop pass gets its own range.
			bump = self.loops * 100000
			if bump and ev.type in (proto.GuestEventType.Throw, proto.GuestEventType.Placed, proto.GuestEventType.PlacedGone):
				ev.b += bump
			elif bump and ev.type == proto.GuestEventType.Explosion and ev.pad[1]:
				ev.pad[1] += bump
			link.guest_events.push(ev)
			self.stats["events"] += 1
		elif kind == "H":
			hdr = proto.HudSlotHdr.from_buffer_copy(body)
			pixels = body[ctypes.sizeof(proto.HudSlotHdr):]
			if len(pixels) == hdr.width * hdr.height * 4:
				link.write_hud_slot(self.hud.back, hdr.width, hdr.height, self.frame, pixels, bool(hdr.flags & 1))
				self.hud.publish()
				link.hud_ctl.framesPublished += 1
				self.stats["hud"] += 1

	def console(self, text: str) -> tuple[int, str]:
		if text.strip() == "!status":
			return 0, (f"replay {self.rec.path}: loop {self.loops} frame {self.frame} duration {self.rec.duration:.2f}s "
				f"assets {self.stats['assetsSent']}/{len(self.rec.assets) + 1} queued {len(self.asset_queue)} counts {self.rec.counts}")
		return 1, "replay: no Lua (this guest is a recording; only !status)"


def main(argv=None) -> int:
	ap = argparse.ArgumentParser(prog="pr replay", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("file")
	ap.add_argument("--path", default=None, help="mapping file (default $PHANTOMRING_SHM or /dev/shm/phantomring_v1)")
	ap.add_argument("--loop", action="store_true", help="play it again and again (odometer continues)")
	ap.add_argument("--from", dest="t_from", type=float, default=0.0, help="start at this recorded second")
	ap.add_argument("--to", dest="t_to", type=float, default=float("inf"), help="end at this recorded second")
	ap.add_argument("--speed", type=float, default=1.0)
	ap.add_argument("--no-events", action="store_true", help="don't replay guest events (shots, melee, died)")
	ap.add_argument("--no-hud", action="store_true")
	ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0 = at the end, or never with --loop)")
	ap.add_argument("--create", action="store_true", help="create the mapping if it does not exist")
	ap.add_argument("--summary", help="write a JSON summary here on exit")
	ap.add_argument("--force", action="store_true", help="take the guest side over even if another live process holds it")
	ap.add_argument("--quiet", action="store_true")
	args = ap.parse_args(argv)

	rec = Recording(args.file, args.t_from, args.t_to, not args.no_events, not args.no_hud)
	path = args.path or proto.default_path()
	if args.create and not os.path.exists(path):
		proto.create_mapping(path)
	try:
		link = proto.Link(path)
	except proto.MappingMissing as e:
		print(f"replay: no mapping ({e}); start the host first or pass --create", file=sys.stderr)
		return 2
	# A live guest (MGSV or another stand-in) would re-attach after us and both would produce on the
	# same single-producer rings and seqlocked blocks.
	if link.refuse_if_taken("guest", "replay", path, args.force):
		return 4
	g = ReplayGuest(link, rec, args.loop, args.speed, args.quiet)
	g.log(f"{args.file}: {rec.duration:.2f} s, {len(rec.assets)} assets ({sum(len(p) for _, p in rec.assets) / 1e6:.1f} MB), "
		f"{rec.counts}, odometer travel {tuple(round(x, 2) for x in rec.odo_travel)}")
	h = link.header
	h.guestNonce = 0
	h.guestUnixPid = os.getpid()
	g.status(Status.Booting, "replay: waiting for the host")
	stop = False

	def on_signal(*_):
		nonlocal stop
		stop = True

	signal.signal(signal.SIGTERM, on_signal)
	signal.signal(signal.SIGINT, on_signal)
	last_report = 0.0
	while not stop:
		g.tick()
		if args.seconds > 0 and g.elapsed() >= args.seconds:
			break
		if g.attached and not args.loop and g.index >= len(rec.timeline):
			break
		if g.elapsed() - last_report >= 2.0 and g.attached:
			last_report = g.elapsed()
			s = g.stats
			g.log(f"loop {g.loops} frame {g.frame}: states {s['states']} draw lists {s['draws']} events {s['events']} hud {s['hud']} "
				f"assets {s['assetsSent']} (queued {len(g.asset_queue)}) host events {s['hostEvents']}")
		time.sleep(1.0 / FPS_POLL)
	g.status(Status.Exiting, "replay: done")
	if args.summary:
		with open(args.summary, "w") as f:
			json.dump(g.stats | {"frame": g.frame, "odoEpoch": g.odo_epoch, "duration": rec.duration, "counts": rec.counts,
				"assets": len(rec.assets), "odoTravel": rec.odo_travel}, f, indent=2)
	return 0


if __name__ == "__main__":
	sys.exit(main())
