"""Stand-in for the Elden Ring host plugin (PhantomRingHost.dll), for testing the MGSV guest (or
fake_mgsv.py) without Elden Ring.

It attaches like the real host (stamps magic/version and a fresh session nonce, drains what it consumes),
heartbeats, publishes HostState and two actors, and drives a scripted pad (10 s loop: left stick forward
3 s, right 3 s, tap A, hold RT, neutral). It applies the guest's odometer to a "puppet" position the way
the host would, prints guest state / odometer / pose bone count / events / draw lists / render-ring
messages / HUD frames, sends a Hurt event every 4 s, and answers host-console requests with canned data.

    python3 -m phantomring.fake_er [--seconds N] [--create] [--summary out.json] [--quiet]
"""

from __future__ import annotations

import argparse
import json
import math
import os
import signal
import sys
import time

from . import proto
from .proto import GuestEventType, HostEventType, HostFlags, PadFlags, Status, XButtons

FPS = 60
PAD_SCRIPT = [  # (until second within the 10 s loop, thumbLX, thumbLY, buttons, rightTrigger)
	(3.0, 0, 32767, 0, 0),
	(6.0, 32767, 0, 0, 0),
	(6.3, 0, 0, XButtons.A, 0),
	(7.0, 0, 0, 0, 255),
	(10.0, 0, 0, 0, 0),
]


# Live pad control for guest tests (--pad-file): the file holds one line of key=value words, re-read
# whenever it changes, e.g.  "buttons=a+lb lt=255 ly=32767"  or  "neutral". Keys: buttons (a b x y start back
# lb rb ls rs up down left right, joined with +), lt rt (0..255), lx ly rx ry (-32768..32767). Missing keys
# are 0. A line "script" goes back to the built-in 10 s loop.
PAD_BUTTONS = {"a": XButtons.A, "b": XButtons.B, "x": XButtons.X, "y": XButtons.Y, "start": XButtons.START,
	"back": XButtons.BACK, "lb": XButtons.LEFT_SHOULDER, "rb": XButtons.RIGHT_SHOULDER, "ls": XButtons.LEFT_THUMB,
	"rs": XButtons.RIGHT_THUMB, "up": XButtons.DPAD_UP, "down": XButtons.DPAD_DOWN, "left": XButtons.DPAD_LEFT,
	"right": XButtons.DPAD_RIGHT}


def parse_pad_line(line: str):
	"""-> dict(buttons, lt, rt, lx, ly, rx, ry), or None for 'script'."""
	words = line.split()
	if words and words[0] == "script":
		return None
	pad = {"buttons": 0, "lt": 0, "rt": 0, "lx": 0, "ly": 0, "rx": 0, "ry": 0}
	for w in words:
		if "=" not in w:
			continue
		k, v = w.split("=", 1)
		if k == "buttons":
			for b in v.split("+"):
				if b:
					pad["buttons"] |= int(PAD_BUTTONS[b.lower()])
		elif k in pad:
			lo, hi = (0, 255) if k in ("lt", "rt") else (-32768, 32767)
			pad[k] = max(lo, min(hi, int(v, 0)))
	return pad


class PadFile:
	def __init__(self, path: str):
		self.path = path
		self.stamp = None
		self.pad = {"buttons": 0, "lt": 0, "rt": 0, "lx": 0, "ly": 0, "rx": 0, "ry": 0}

	def poll(self):
		try:
			st = os.stat(self.path)
		except OSError:
			return self.pad
		stamp = (st.st_mtime_ns, st.st_size)
		if stamp != self.stamp:
			self.stamp = stamp
			try:
				with open(self.path) as f:
					self.pad = parse_pad_line(f.read())
			except (OSError, KeyError, ValueError) as e:
				print(f"fake_er: bad pad file line ({e})", file=sys.stderr)
		return self.pad


def cam_yaw_deg(q) -> float:
	"""World yaw of the camera's forward axis (+z rotated by quat x,y,z,w), degrees, atan2(fwd.x, fwd.z) like rootYaw."""
	x, y, z, w = q[0], q[1], q[2], q[3]
	fx = 2.0 * (x * z + w * y)
	fz = 1.0 - 2.0 * (x * x + y * y)
	return math.degrees(math.atan2(fx, fz))


def scripted_pad(t: float):
	t %= 10.0
	for until, lx, ly, buttons, rt in PAD_SCRIPT:
		if t < until:
			return lx, ly, int(buttons), rt
	return 0, 0, 0, 0


class FakeHost:
	def __init__(self, link: proto.Link, quiet: bool, hurt_every: float, pad_file: str | None = None):
		self.link = link
		self.pad_file = PadFile(pad_file) if pad_file else None
		self.trace = None  # --trace: one CSV line per new guest frame (treadmill / riding tests)
		self.quiet = quiet
		self.hurt_every = hurt_every
		self.t0 = time.monotonic()
		self.frame = 0
		self.nonce = 0
		self.pad_packet = 0
		self.pad_last = None
		self.guest_attached = False
		self.guest_pid = 0
		self.odo_last = None
		self.odo_epoch = None
		self.puppet = [0.0, 0.0, 0.0]
		self.next_hurt = hurt_every
		self.draw = link.triple_reader(proto.OFF_DRAW_CTL)
		self.hud = link.triple_reader(proto.OFF_HUD_CTL)
		self.meshes: dict[int, dict] = {}
		self.textures: dict[int, dict] = {}
		self.last_guest_frame = None
		self.pad_override = None  # dict as parse_pad_line returns: a fixed pad (recorder --pad) instead of the loop
		self.placed: dict[int, tuple] = {}  # v5: kGEvPlaced ids still there (`pr er detonate <id>|all`)
		self.dead = False  # `pr er die`: HostState kHostPlayerDead until `pr er respawn` (guest death tests)
		self.tarnished = False  # `pr er tarnished 1`: Tarnished mode, no kHostPuppeting (the pad stays as given: the guest parks itself)
		# --actors near (ActorProxy tests, docs/GUEST.md 20): the two actors stand still in Elden Ring at these offsets (MGSV x, z) from
		# where Snake was at the first guest state, published the way the host maps them (rootPos + (point - odo) for a point fixed in
		# odometer space), so treadmill jumps move them in MGSV's world exactly as they do with the real host. "fixed": the old
		# constant MGSV-world points.
		self.actors_mode = "fixed"
		self.actor_points: list[list[float]] | None = None
		# `pr er actor <i> asleep|awake|dead|alive|at <dx> <dz>` (--actors near; Fulton tests, docs/GUEST.md 20.4): per-actor extra flags and
		# moves; a kGEvFulton a=1 removes that actor from the table, as the real host takes the enemy out of Elden Ring's world.
		self.actor_flags = [0, 0]
		self.actor_handles = [0xFFFFFFFF00001000, 0xFFFFFFFF00001001]  # `actor <i> renew`: a new character (a fultoned one stays gone)
		self.fultoned: set[int] = set()
		self.guest_last = None
		self.stats = {
			"frames": 0, "guestAttaches": 0, "guestStates": 0, "guestFlagsSeen": 0, "odoDistance": 0.0,
			"events": {}, "drawFrames": 0, "lastDrawCount": 0, "lastMatrixCount": 0, "drawOwnersSeen": [],
			"drawMissingMesh": 0, "renderClearAll": 0, "renderMeshes": 0, "renderTextures": 0, "renderForget": 0,
			"meshIndexOk": True, "hudFrames": 0, "hudLastSize": [0, 0], "poseBones": 0, "consoleServed": 0,
			"hurtSent": 0, "buddyTypes": [], "vehicleIdsSeen": [], "lifeLast": 0, "buddyFlagsSeen": 0, "buddyPosValid": 0,
			"buddyAmmoLast": 0,
		}

	def elapsed(self) -> float:
		return time.monotonic() - self.t0

	def log(self, text: str):
		if not self.quiet:
			print(f"[fake_er   {self.elapsed():6.2f}s] {text}", flush=True)

	def attach(self):
		link = self.link
		# What the host consumes is drained: anything there belongs to a previous session.
		link.guest_events.drain()
		link.render_ring.drain()
		self.draw.reset()
		self.hud.reset()
		self.nonce = link.stamp_host(Status.Ready, "fake Elden Ring: in game")
		self.log(f"session {self.nonce:#018x} on {link.path}")

	def tick(self, dt: float):
		link, st = self.link, self.stats
		h = link.header
		h.hostHeartbeatQpc = proto.qpc_now()
		self.frame += 1
		st["frames"] += 1
		t = self.elapsed()

		flags = HostFlags.InGame | (HostFlags.PlayerDead if self.dead else (0 if self.tarnished else HostFlags.Puppeting))
		hs = proto.HostState(flags=int(flags), frame=self.frame, qpc=proto.qpc_now(),
			timeOfDayHours=(12.0 + t / 60.0) % 24.0, cameraMaxDistance=8.0, erHpFrac=1.0, erWorldId=0x3C2A2400,
			groundY=0.0, fallSpeed=0.0)
		link.seq_write(proto.OFF_HOST_STATE, hs)

		live = self.pad_override if self.pad_override is not None else (self.pad_file.poll() if self.pad_file else None)
		if live is not None:
			state = (live["lx"], live["ly"], live["buttons"], live["rt"], live["lt"], live["rx"], live["ry"])
		else:
			lx, ly, buttons, rt = scripted_pad(t)
			state = (lx, ly, buttons, rt, 0, 0, 0)
		if state != self.pad_last:
			self.pad_last = state
			self.pad_packet += 1
		pad = proto.PadState(flags=int(PadFlags.Connected), packet=self.pad_packet, buttons=state[2], rightTrigger=state[3],
			leftTrigger=state[4], thumbLX=state[0], thumbLY=state[1], thumbRX=state[5], thumbRY=state[6])
		link.seq_write(proto.OFF_PAD, pad)

		if self.frame % 30 == 1 or self.actors_mode == "near":
			self.publish_actors()

		if t >= self.next_hurt:
			self.next_hurt += self.hurt_every
			ev = proto.HostEvent(type=int(HostEventType.Hurt), a=50, b=int(proto.HurtKind.Melee), frame=self.frame & 0xFFFFFFFF)
			ev.v0[:] = [0.0, 0.0, 1.0]
			if link.host_events.push(ev):
				st["hurtSent"] += 1

		if proto.console_serve(link.host_console, self.console):
			st["consoleServed"] += 1

		# The guest's data is only trusted while it is attached to *this* session. A new guest process
		# (pid change) or a re-attach resets our side of the triple buffers: it reset its own.
		attached = h.guestNonce == self.nonce and h.guestNonce != 0
		if attached and (not self.guest_attached or h.guestUnixPid != self.guest_pid):
			self.guest_pid = h.guestUnixPid
			self.draw.reset()
			self.hud.reset()
			self.odo_last = None
			self.placed.clear()  # a new guest session re-announces what is still placed
			st["guestAttaches"] += 1
			self.log(f"guest attached (unix pid {h.guestUnixPid})")
		elif not attached and self.guest_attached:
			self.log("guest detached")
		self.guest_attached = attached
		if not attached:
			return

		self.consume_render_ring()

		gs = link.guest_state()
		if gs and gs.frame != self.last_guest_frame:
			self.last_guest_frame = gs.frame
			st["guestStates"] += 1
			st["guestFlagsSeen"] |= gs.flags
			st["lifeLast"] = gs.life
			if gs.buddyType not in st["buddyTypes"]:
				st["buddyTypes"].append(gs.buddyType)
			if gs.vehicleId not in st["vehicleIdsSeen"]:
				st["vehicleIdsSeen"].append(gs.vehicleId)
			st["buddyFlagsSeen"] |= gs.buddyFlags
			st["buddyAmmoLast"] = gs.buddyAmmo
			self.guest_last = gs
			self.on_guest_frame(gs)
			if self.trace:
				self.trace.write(f"{time.monotonic() - self.t0:.4f},{gs.frame},{gs.flags:#x},{gs.odoEpoch},{gs.odo[0]:.4f},{gs.odo[1]:.4f},{gs.odo[2]:.4f},"
					f"{gs.rootPos[0]:.4f},{gs.rootPos[1]:.4f},{gs.rootPos[2]:.4f},{gs.rootYaw:.4f},{gs.velocity[0]:.3f},{gs.velocity[2]:.3f},"
					f"{gs.buddyFlags:#x},{gs.buddyPos[0]:.3f},{gs.buddyPos[1]:.3f},{gs.buddyPos[2]:.3f},{gs.vehicleId:#x},{gs.buddyAmmo},"
					f"{1 if gs.flags & proto.GuestFlags.ThrowAim else 0},{gs.aimThrowPos[0]:.3f},{gs.aimThrowPos[1]:.3f},{gs.aimThrowPos[2]:.3f},"
					f"{gs.aimThrowVel[0]:.3f},{gs.aimThrowVel[1]:.3f},{gs.aimThrowVel[2]:.3f},{gs.aimThrowKind},"
					f"{gs.life},{gs.lifeMax},{gs.weaponId},{gs.ammoInClip},{gs.ammoStock},{cam_yaw_deg(gs.camQuat):.3f},"
					f"{1 if gs.flags & proto.GuestFlags.Nvg else 0}\n")
			if gs.buddyFlags & proto.GuestBuddyFlags.PosValid and math.isfinite(gs.buddyPos[0]):
				st["buddyPosValid"] += 1  # the real host maps buddyPos through the anchor and snaps it to ER ground
			odo = list(gs.odo)
			if self.odo_last is None or gs.odoEpoch != self.odo_epoch:
				self.odo_last, self.odo_epoch = odo, gs.odoEpoch  # new epoch: re-baseline, never jump
			else:
				d = [odo[i] - self.odo_last[i] for i in range(3)]
				self.odo_last = odo
				# The real host rotates by the session yaw offset and lets ER collision resolve it.
				for i in range(3):
					self.puppet[i] += d[i]
				st["odoDistance"] += math.hypot(d[0], d[2])

		pose = link.pose()
		if pose:
			st["poseBones"] = pose.boneCount

		while (ev := link.guest_events.pop()) is not None:
			name = proto.enum_name(ev.type, GuestEventType)
			st["events"][name] = st["events"].get(name, 0) + 1
			self.on_guest_event(ev)
			if self.trace:
				self.trace.write(f"# event {name} a={ev.a:#x} b={ev.b} pad0={ev.pad[0]:#x} frame={ev.frame} "
					f"v0={ev.v0[0]:.2f},{ev.v0[1]:.2f},{ev.v0[2]:.2f} v1={ev.v1[0]:.2f},{ev.v1[1]:.2f},{ev.v1[2]:.2f} pad1={ev.pad[1]}\n")
			if ev.type in (GuestEventType.Fire, GuestEventType.BuddyFire):
				who = "D-Walker" if ev.type == GuestEventType.BuddyFire else "Snake"
				self.log(f"{who} fired: weapon {ev.a & 0xFFFF:#06x} ammo {proto.enum_name(ev.a >> 16, proto.AmmoKind)} "
					f"from ({ev.v0[0]:.2f},{ev.v0[1]:.2f},{ev.v0[2]:.2f}) dir ({ev.v1[0]:.2f},{ev.v1[1]:.2f},{ev.v1[2]:.2f})")
			elif ev.type == GuestEventType.Explosion:
				self.log(f"explosion: {proto.enum_name(ev.a, proto.AmmoKind)} r {ev.b / 1000:.1f} m at ({ev.v0[0]:.2f},{ev.v0[1]:.2f},{ev.v0[2]:.2f}) "
					f"MGSV attack {ev.pad[0] & 0xFFFF} class {(ev.pad[0] >> 16) & 0xFF:#x} of throw/placed #{ev.pad[1]}")
			elif ev.type == GuestEventType.Throw:
				self.log(f"throw #{ev.b}: {proto.enum_name(ev.a, proto.AmmoKind)} equip {ev.pad[0] & 0xFFFF} fuse {(ev.pad[0] >> 16) / 1000:.2f} s "
					f"from ({ev.v0[0]:.2f},{ev.v0[1]:.2f},{ev.v0[2]:.2f}) at ({ev.v1[0]:.2f},{ev.v1[1]:.2f},{ev.v1[2]:.2f}) m/s")
			elif ev.type == GuestEventType.Placed:
				self.placed[ev.b] = (ev.v0[0], ev.v0[1], ev.v0[2])
				self.log(f"placed #{ev.b}: {proto.enum_name(ev.a, proto.AmmoKind)} equip {ev.pad[0]} trigger {ev.pad[1] / 1000:.1f} m "
					f"at ({ev.v0[0]:.2f},{ev.v0[1]:.2f},{ev.v0[2]:.2f}) facing ({ev.v1[0]:.2f},{ev.v1[1]:.2f},{ev.v1[2]:.2f})")
			elif ev.type == GuestEventType.PlacedGone:
				self.placed.pop(ev.b, None)
				self.log(f"placed #{ev.b} gone")
			elif ev.type == GuestEventType.Died:
				self.log("Snake died: the host would kill the puppet now")
			elif ev.type == GuestEventType.Fulton:
				handle = ev.pad[0] | (ev.pad[1] << 32)
				if ev.a == 1:
					self.fultoned.add(handle)  # the real host lifts him out of Elden Ring's world: gone from the table
				self.log(f"fulton: Elden Ring character {handle:#x} {'fultoned' if ev.a == 1 else 'NOT fultoned'} at ({ev.v0[0]:.2f},{ev.v0[1]:.2f},{ev.v0[2]:.2f})")
			elif ev.type == GuestEventType.TimeSkip:
				# v13: MGSV's Phantom Cigar; the real host moves Elden Ring's clock by as much (er/TimeSkip.h)
				self.time_skip_min = getattr(self, "time_skip_min", 0) + ev.a
				self.log(f"time skip: +{ev.a} min of MGSV game time (Phantom Cigar; {self.time_skip_min} min this session)")

		if self.draw.acquire():
			hdr, dl, draws, mats = link.read_draw_slot(self.draw.front)
			st["drawFrames"] += 1
			st["lastDrawCount"] = len(draws)
			st["lastMatrixCount"] = len(mats)
			for d in draws:
				owner = proto.enum_name(d.owner, proto.DrawOwner)
				if owner not in st["drawOwnersSeen"]:
					st["drawOwnersSeen"].append(owner)
				if d.mesh not in self.meshes:
					st["drawMissingMesh"] += 1
			self.on_draw_slot(self.draw.front, hdr)

		if self.hud.acquire():
			hdr, pixels = link.read_hud_slot(self.hud.front)
			st["hudFrames"] += 1
			st["hudLastSize"] = [hdr.width, hdr.height]
			self.on_hud_slot(hdr, pixels)

	# Hooks for subclasses that keep what the guest sends (recorder.py, capture_view.py); the slot data is
	# only valid until the next acquire.
	def on_guest_frame(self, gs):
		pass

	def on_guest_event(self, ev):
		pass

	def on_draw_slot(self, index: int, hdr):
		pass

	def on_hud_slot(self, hdr, pixels):
		pass

	def consume_render_ring(self):
		st = self.stats
		while (msg := self.link.render_ring.pop()) is not None:
			typ, payload = msg
			if typ == proto.RenType.ClearAll:
				self.meshes.clear()
				self.textures.clear()
				st["renderClearAll"] += 1
				self.log("render: clear all")
			elif typ == proto.RenType.Mesh:
				mesh, elements, vb, ib = proto.parse_mesh_msg(payload)
				ok = len(vb) == mesh.vertexBytes and len(ib) == mesh.indexBytes and mesh.elementCount == len(elements)
				st["meshIndexOk"] &= ok
				self.meshes[mesh.key] = {"elements": [e.semantic.decode() for e in elements], "vertexBytes": mesh.vertexBytes}
				st["renderMeshes"] += 1
				self.log(f"render: mesh {mesh.key:#x} {mesh.vertexBytes} vertex bytes, stride {mesh.vertexStride}, "
					f"{mesh.indexBytes} index bytes, layout {'/'.join(self.meshes[mesh.key]['elements'])}")
			elif typ == proto.RenType.Texture:
				tex = proto.RenTexture.from_buffer_copy(payload, 0)
				self.textures[tex.key] = {"w": tex.width, "h": tex.height, "format": tex.format}
				st["renderTextures"] += 1
				self.log(f"render: texture {tex.key:#x} {tex.width}x{tex.height} format {tex.format} ({tex.dataBytes} bytes)")
			elif typ == proto.RenType.Forget:
				key = proto.RenForget.from_buffer_copy(payload, 0).key
				self.meshes.pop(key, None)
				self.textures.pop(key, None)
				st["renderForget"] += 1

	NEAR_ACTORS = [("Godrick Soldier", 15.0, 0.0), ("Wandering Noble", -10.0, 25.0)]

	def actor_positions(self) -> list[list[float]] | None:
		"""--actors near: the actors' MGSV-world feet now (None before the first guest state)."""
		gs = self.guest_last
		if gs is None:
			return None
		if self.actor_points is None:
			self.actor_points = [[gs.odo[0] + x, gs.odo[1], gs.odo[2] + z] for _, x, z in self.NEAR_ACTORS]
		return [[gs.rootPos[i] + p[i] - gs.odo[i] for i in range(3)] for p in self.actor_points]

	def publish_actors(self):
		table = proto.ActorTable(count=0)
		near = self.actor_positions() if self.actors_mode == "near" else None
		for i, (name, x, z) in enumerate([("Godrick Soldier", 6.0, 4.0), ("Wandering Noble", -5.0, 8.0)]):
			handle = self.actor_handles[i]
			if handle in self.fultoned:
				continue
			a = table.actors[table.count]
			table.count += 1
			a.handle = handle
			a.flags = int(proto.ActorFlags.Hostile) | self.actor_flags[i]
			a.pos[:] = near[i] if near else [x, 0.0, z]
			a.yaw, a.radius, a.height, a.hpFrac = math.pi, 0.4, 1.8, 1.0 if not self.actor_flags[i] & int(proto.ActorFlags.Dead) else 0.0
			a.name = name.encode()
		self.link.seq_write(proto.OFF_ACTORS, table)

	def actor_command(self, args: list[str]) -> tuple[int, str]:
		if len(args) < 2 or args[0] not in ("0", "1"):
			return 1, "usage: actor 0|1 asleep|awake|dead|alive|at <dx> <dz>|back   (at: MGSV x/z metres from Snake now; --actors near)"
		i = int(args[0])
		flag = {"asleep": int(proto.ActorFlags.Asleep), "dead": int(proto.ActorFlags.Dead)}
		match args[1:]:
			case ["asleep" | "dead" as what]:
				self.actor_flags[i] |= flag[what]
			case ["awake"]:
				self.actor_flags[i] &= ~flag["asleep"]
			case ["alive"]:
				self.actor_flags[i] &= ~flag["dead"]
			case ["back"]:
				self.fultoned.discard(self.actor_handles[i])
			case ["renew"]:
				self.actor_handles[i] = 0xFFFFFFFF00000000 | (max(h & 0xFFFFFFFF for h in self.actor_handles) + 0x10)
				self.actor_flags[i] = 0
			case ["at", dx, dz]:
				gs = self.guest_last
				if self.actors_mode != "near" or gs is None:
					return 1, "actor at: needs --actors near and a guest state"
				self.actor_positions()
				self.actor_points[i] = [gs.odo[0] + float(dx), gs.odo[1], gs.odo[2] + float(dz)]
			case _:
				return 1, "usage: actor 0|1 asleep|awake|dead|alive|at <dx> <dz>|back|renew"
		self.publish_actors()
		pos = self.actor_positions()
		where = f" at {pos[i][0]:.2f},{pos[i][1]:.2f},{pos[i][2]:.2f}" if pos else ""
		gone = " (fultoned: not published)" if self.actor_handles[i] in self.fultoned else ""
		return 0, f"actor {i} handle {self.actor_handles[i]:#x} flags {self.actor_flags[i]:#x}{where}{gone}"

	def host_event(self, typ, a: int, b: int) -> tuple[int, str]:
		ev = proto.HostEvent(type=int(typ), a=a, b=b, frame=self.frame & 0xFFFFFFFF)
		ev.v0[:] = [0.0, 0.0, 1.0]
		ok = self.link.host_events.push(ev)
		if ok and self.trace:
			self.trace.write(f"# host {proto.enum_name(int(typ), HostEventType)} a={a} b={b} t={time.monotonic() - self.t0:.4f}\n")
		return (0, f"sent {proto.enum_name(int(typ), HostEventType)} a={a}") if ok else (1, "host event ring full")

	def console(self, text: str) -> tuple[int, str]:
		cmd = text.strip().split()
		if not cmd:
			return 1, "empty command"
		match cmd:
			case ["help"]:
				return 0, ("fake_er canned commands: help, ping, status, peek player.pos, peek <addr>, hurt <er dmg>, heal <amount>, die, respawn, "
					"rested 0|1, tarnished 0|1, placed, detonate <id>|all, actors, actor 0|1 asleep|awake|dead|alive|at <dx> <dz>|back|renew")
			case ["actor", *rest]:
				return self.actor_command(rest)
			case ["actors"]:
				pos = self.actor_positions() if self.actors_mode == "near" else None
				return 0, "; ".join(f"{i}: flags {self.actor_flags[i]:#x}" + (f" at {pos[i][0]:.2f},{pos[i][1]:.2f},{pos[i][2]:.2f}" if pos else "") +
					(" fultoned" if self.actor_handles[i] in self.fultoned else "") for i in range(2))
			case ["placed"]:
				return 0, " ".join(f"#{k}@{v[0]:.1f},{v[1]:.1f},{v[2]:.1f}" for k, v in self.placed.items()) or "none"
			case ["detonate", which]:
				# v5: what the host sends when an Elden Ring enemy walks into a placed explosive's trigger radius
				ids = list(self.placed) if which == "all" else [int(which, 0)]
				for i in ids:
					self.host_event(HostEventType.Detonate, 0, i)
				return 0, f"sent Detonate for {ids}"
			case ["hurt", amount]:
				return self.host_event(HostEventType.Hurt, int(amount, 0), int(proto.HurtKind.Melee))
			case ["heal", amount]:
				# v3: what the host sends when Elden Ring heals the puppet (a = Snake's life / LifeScale units)
				return self.host_event(HostEventType.Heal, int(amount, 0), 0)
			case ["rested", which]:
				# v10: the Tarnished rested at a grace (0) or respawned (1): the guest refills Snake's ammo and life
				if which not in ("0", "1"):
					return 1, "usage: rested 0|1   (0 grace rest, 1 respawn)"
				return self.host_event(HostEventType.Rested, int(which), 0)
			case ["tarnished", which]:
				# v10: Tarnished mode on (1: Snake parks) / off (0). The real host also sends a neutral pad while the puppet is off.
				if which not in ("0", "1"):
					return 1, "usage: tarnished 0|1"
				self.tarnished = which == "1"
				return self.host_event(HostEventType.Tarnished, int(which), 0)
			case ["die"]:
				# What the real host does when the Tarnished dies: kHostPlayerDead from the death until the respawn.
				self.dead = True
				return 0, "HostState PlayerDead set (the guest should collapse Snake); `respawn` to end it"
			case ["respawn"]:
				self.dead = False
				return self.host_event(HostEventType.Respawn, 0, 0)
			case ["ping"]:
				return 0, "pong (fake_er)"
			case ["status"]:
				return 0, f"frame {self.frame} guest {'attached' if self.guest_attached else 'absent'}"
			case ["peek", "player.pos"]:
				return 0, f"{self.puppet[0]:.3f} {self.puppet[1]:.3f} {self.puppet[2]:.3f}"
			case ["peek", addr, *_]:
				try:
					return 0, f"{int(addr, 0):#x}: 00 00 00 00 (canned, fake_er)"
				except ValueError:
					return 1, f"bad address {addr!r}"
		return 1, f"unknown command {cmd[0]!r} (fake_er; try help)"


def main(argv=None) -> int:
	ap = argparse.ArgumentParser(prog="fake_er", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--path", default=None, help="mapping file (default $PHANTOMRING_SHM or /dev/shm/phantomring_v1)")
	ap.add_argument("--seconds", type=float, default=0, help="run time (0 = until Ctrl-C)")
	ap.add_argument("--create", action="store_true", help="create the mapping if it does not exist")
	ap.add_argument("--hurt-every", type=float, default=4.0, help="seconds between fake Hurt events (0 = never)")
	ap.add_argument("--summary", help="write a JSON summary here on exit")
	ap.add_argument("--quiet", action="store_true")
	ap.add_argument("--trace", help="append one CSV line per new guest frame (t,frame,flags,epoch,odo xyz,root xyz,yaw,vel xz,"
		"buddyFlags,buddyPos xyz,vehicleId,buddyAmmo,throwAim,aimThrowPos xyz,aimThrowVel xyz,aimThrowKind,life,lifeMax,weaponId,"
		"ammoInClip,ammoStock,camYawDeg,nvg) and one '# event' line per guest event (and '# host' per host event sent)")
	ap.add_argument("--pad-file", help="drive the pad from this file instead of the 10 s loop (see parse_pad_line); "
		"an absent file means a neutral pad")
	ap.add_argument("--actors", choices=["fixed", "near"], default="fixed",
		help="the two published actors: fixed MGSV-world points (default), or standing 15 m / 27 m from where Snake started, "
		"mapped like the real host does (ActorProxy tests, docs/GUEST.md 20)")
	ap.add_argument("--force", action="store_true", help="take the host side over even if another live process holds it")
	args = ap.parse_args(argv)

	path = args.path or proto.default_path()
	if args.create and not os.path.exists(path):
		proto.create_mapping(path)
	try:
		link = proto.Link(path)
	except proto.MappingMissing as e:
		print(f"fake_er: no mapping ({e}); pass --create", file=sys.stderr)
		return 2
	if link.refuse_if_taken("host", "fake_er", path, args.force):
		return 4
	host = FakeHost(link, args.quiet, args.hurt_every if args.hurt_every > 0 else float("inf"), args.pad_file)
	host.actors_mode = args.actors
	if args.trace:
		host.trace = open(args.trace, "a", buffering=1)
	host.attach()
	stop = False

	def on_signal(*_):
		nonlocal stop
		stop = True

	signal.signal(signal.SIGTERM, on_signal)
	signal.signal(signal.SIGINT, on_signal)
	dt = 1.0 / FPS
	next_t = time.monotonic()
	last_report = 0.0
	while not stop and (args.seconds <= 0 or host.elapsed() < args.seconds):
		host.tick(dt)
		if host.elapsed() - last_report >= 1.0:
			last_report = host.elapsed()
			st = host.stats
			guest = link.side("guest")
			if host.guest_attached:
				gs = link.guest_state()
				flags = proto.flag_names(gs.flags, proto.GuestFlags) if gs else "-"
				host.log(f"guest {guest['statusName']} flags {flags} odo->puppet ({host.puppet[0]:.2f}, {host.puppet[2]:.2f}) "
					f"pose {st['poseBones']} bones, events {st['events']}, draw frames {st['drawFrames']} "
					f"({st['lastDrawCount']} draws), hud {st['hudFrames']}, life {st['lifeLast']}")
			else:
				host.log(f"waiting for the guest ({guest['statusName']} {guest['text']!r})")
		next_t += dt
		delay = next_t - time.monotonic()
		if delay > 0:
			time.sleep(delay)
		else:
			next_t = time.monotonic()
	h = link.header
	h.hostStatus = int(Status.Exiting)
	proto.set_text(h, "hostStatusText", "fake Elden Ring: exiting")
	if args.summary:
		with open(args.summary, "w") as f:
			json.dump(host.stats | {"puppet": host.puppet, "nonce": host.nonce, "meshKeys": [hex(k) for k in host.meshes],
				"textureKeys": [hex(k) for k in host.textures]}, f, indent=2)
	return 0


if __name__ == "__main__":
	sys.exit(main())
