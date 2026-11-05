"""Stand-in for the Elden Ring host plugin (PhantomRingHost.dll), for testing the MGSV guest (or
fake_mgsv.py) without Elden Ring.

It attaches like the real host (stamps magic/version and a fresh session nonce, drains what it consumes),
heartbeats, publishes HostState and two actors, and drives a scripted pad (10 s loop: left stick forward
3 s, right 3 s, tap A, hold RT, neutral). It applies the guest's odometer to a "puppet" position the way
the host would, prints guest state / odometer / pose bone count / events / draw lists / render-ring
messages / HUD frames, sends a Hurt event every 4 s, and answers host-console requests with canned data.

    python3 -m phantomring.fake_er [--seconds N] [--create] [--summary out.json] [--quiet]

D-Dog (protocol v17, docs/research/ddog.md): `pr er actor <i> dogtarget` makes that actor the host's pick for the dog's next
order (ActorFlags.DogTarget, at most one record; `nodogtarget` clears it); kGEvDogAttack, kGEvBuddyOrder (with D-Dog's style)
and GuestBuddyFlags.Engaged are printed as they come. HostState carries HostFlags.DogLayer (the host's [Dog] on: the guest takes
D-Dog's attack orders over only then); `pr er doglayer 0` clears it, as a host with `[Dog] enabled = false`.
Quiet and the grace rest (protocol v18, docs/research/quiet.md): HostState carries HostFlags.QuietLayer only after `pr er
quietlayer 1` (the real host's [Quiet] is off until its live pass); `pr er actor <i> quiettarget|noquiettarget|quietthreat|
noquietthreat` sets her pick / Cover me's threat pick (at most one record each); `pr er post <dx> <dz> [yaw_deg]|none` publishes her
post dx, dz m from Snake (buddyPost*, seq bumped; none = on the move: NaN); `pr er grace 1 [dx dz [yaw_deg]]|1 none|0` a rest
(HostFlags.AtGrace, graceCentre 1.2 m ahead of Snake, the seat dx, dz m from him -- default 1.5, 1.5 -- facing the grace; `none` =
no seat, a talk; also sends kHEvRested 0, as the real host does on a rest) and its end. Posts and seats stay where they are in
Elden Ring: they are kept in Snake's odometer frame and mapped to MGSV's world every frame, as the real host maps them.
`pr er hoststate` prints what is published; kGEvQuietShot, Quiet's orders (her weapon in pad[0]) and the guest's echoes
(buddyPostSeq / graceSeqSeated, GuestBuddyFlags.AtPost / Seated / Cloaked / CanRest / QuietMarked / QuietCover, her weapon) are
printed as they come.
`pr er block 1|0`: an Elden Ring wall -- the puppet stays while MGSV's Snake runs on; --actors near characters move by that slip in
MGSV's world, as the real host maps them (the guest's waiting D-Dog reads it there; --trace's last two columns are the slip).
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
		self.trace_draws = False  # --trace-draws: also a '# draws' line per draw list (D-Walker follow, projectile tags)
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
		self.dog_layer = True  # v17 HostFlags.DogLayer: the host's D-Dog layer is on (`pr er doglayer 0|1`)
		# v18 Quiet and the grace rest (`pr er quietlayer 0|1`, `pr er post ...`, `pr er grace ...`): points kept in Snake's odometer
		# frame (where they stay in Elden Ring) and mapped to MGSV's world every frame through the latest guest state
		self.quiet_layer = False
		self.post = None          # (x, y, z) odometer frame, or None = on the move (NaN)
		self.post_yaw = 0.0
		self.post_seq = 0
		self.grace = None         # {"centre": (x, y, z), "seat": (x, y, z) | None, "yaw": float} odometer frame, or None
		self.grace_seq = 0
		self.echo_last = (0, 0, 0)  # the guest's (buddyPostSeq, graceSeqSeated, Quiet's buddy bits) last logged
		self.climbing = False  # `pr er climb 1`: an Elden Ring ladder climb under way (v15 kHEvClimb)
		# `pr er block 1`: Snake pushes into an Elden Ring wall -- the puppet doesn't move while MGSV's Snake runs on (the odometer's
		# horizontal steps go into the slip instead); the --actors near characters, standing still in Elden Ring, then move in MGSV's
		# world by the slip, as the real host maps them (docs/research/ddog.md §9: a waiting D-Dog kept in its Elden Ring place).
		self.blocked = False
		self.slip = [0.0, 0.0, 0.0]
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
			"events": {}, "drawFrames": 0, "lastDrawCount": 0, "lastMatrixCount": 0, "drawOwnersSeen": [], "drawsByOwner": {},
			"listsWithProjectiles": 0, "projectileMeshes": [],
			"drawMissingMesh": 0, "renderClearAll": 0, "renderMeshes": 0, "renderTextures": 0, "renderForget": 0,
			"meshIndexOk": True, "hudFrames": 0, "hudLastSize": [0, 0], "poseBones": 0, "consoleServed": 0,
			"hurtSent": 0, "buddyTypes": [], "vehicleIdsSeen": [], "lifeLast": 0, "buddyFlagsSeen": 0, "buddyPosValid": 0,
			"buddyAmmoLast": 0, "dogAttacks": [], "buddyOrders": [], "dogEngagedFrames": 0,
			"quietShots": [], "postEchoFrames": 0, "seatEchoFrames": 0, "postSeqEchoed": 0, "graceSeqEchoed": 0, "quietWeaponsSeen": [],
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
		if self.dog_layer:
			flags |= HostFlags.DogLayer
		if self.quiet_layer:
			flags |= HostFlags.QuietLayer
		if self.grace is not None:
			flags |= HostFlags.AtGrace
		hs = proto.HostState(flags=int(flags), frame=self.frame, qpc=proto.qpc_now(),
			timeOfDayHours=(12.0 + t / 60.0) % 24.0, cameraMaxDistance=8.0, erHpFrac=1.0, erWorldId=0x3C2A2400,
			groundY=0.0, fallSpeed=0.0)
		self.fill_quiet_state(hs)
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
			# v16: the weapon in hand's working suppressor, logged as it comes and goes (with the weapon and its rounds)
			sup = bool(gs.flags & proto.GuestFlags.SuppressorOn)
			if self.guest_last is None or sup != bool(self.guest_last.flags & proto.GuestFlags.SuppressorOn):
				self.log(f"guest: suppressor {'ON' if sup else 'off'} (kGuestSuppressorOn) -- weapon {gs.weaponId:#x} {gs.ammoInClip}/{gs.ammoStock}")
				if self.trace:
					self.trace.write(f"# suppressor {'on' if sup else 'off'} weapon={gs.weaponId} clip={gs.ammoInClip} t={time.monotonic() - self.t0:.4f}\n")
			# v17: D-Dog carries out an attack (GuestBuddyFlags.Engaged, Connected .. Released), logged as it comes and goes
			engaged = bool(gs.buddyFlags & proto.GuestBuddyFlags.Engaged)
			if engaged:
				st["dogEngagedFrames"] += 1
			if self.guest_last is None or engaged != bool(self.guest_last.buddyFlags & proto.GuestBuddyFlags.Engaged):
				self.log(f"guest: buddy {'ENGAGED' if engaged else 'not engaged'} (GuestBuddyFlags.Engaged; buddy type {gs.buddyType}, "
					f"flags {proto.flag_names(gs.buddyFlags, proto.GuestBuddyFlags)})")
			self.log_quiet_echo(gs)
			self.guest_last = gs
			self.on_guest_frame(gs)
			if self.trace:
				self.trace.write(f"{time.monotonic() - self.t0:.4f},{gs.frame},{gs.flags:#x},{gs.odoEpoch},{gs.odo[0]:.4f},{gs.odo[1]:.4f},{gs.odo[2]:.4f},"
					f"{gs.rootPos[0]:.4f},{gs.rootPos[1]:.4f},{gs.rootPos[2]:.4f},{gs.rootYaw:.4f},{gs.velocity[0]:.3f},{gs.velocity[2]:.3f},"
					f"{gs.buddyFlags:#x},{gs.buddyPos[0]:.3f},{gs.buddyPos[1]:.3f},{gs.buddyPos[2]:.3f},{gs.vehicleId:#x},{gs.buddyAmmo},"
					f"{1 if gs.flags & proto.GuestFlags.ThrowAim else 0},{gs.aimThrowPos[0]:.3f},{gs.aimThrowPos[1]:.3f},{gs.aimThrowPos[2]:.3f},"
					f"{gs.aimThrowVel[0]:.3f},{gs.aimThrowVel[1]:.3f},{gs.aimThrowVel[2]:.3f},{gs.aimThrowKind},"
					f"{gs.life},{gs.lifeMax},{gs.weaponId},{gs.ammoInClip},{gs.ammoStock},{cam_yaw_deg(gs.camQuat):.3f},"
					f"{1 if gs.flags & proto.GuestFlags.Nvg else 0},{self.slip[0]:.4f},{self.slip[2]:.4f}\n")
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
					if self.blocked and i != 1:
						self.slip[i] += d[i]  # a wall: Elden Ring's Snake stays, MGSV's runs on
					else:
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
				self.trace.write(f"# event {name} a={ev.a:#x} b={ev.b}{' suppressed' if ev.type in (GuestEventType.Fire, GuestEventType.BuddyFire) and ev.b & proto.FIRE_SUPPRESSED else ''} "
					f"pad0={ev.pad[0]:#x} frame={ev.frame} "
					f"v0={ev.v0[0]:.2f},{ev.v0[1]:.2f},{ev.v0[2]:.2f} v1={ev.v1[0]:.2f},{ev.v1[1]:.2f},{ev.v1[2]:.2f} pad1={ev.pad[1]}\n")
			if ev.type in (GuestEventType.Fire, GuestEventType.BuddyFire):
				who = "D-Walker" if ev.type == GuestEventType.BuddyFire else "Snake"
				# v16: b = pellets (low 16 bits) | FIRE_SUPPRESSED (the round went through a working suppressor)
				suppressed = bool(ev.b & proto.FIRE_SUPPRESSED)
				st["firesSuppressed" if suppressed else "firesUnsuppressed"] = st.get("firesSuppressed" if suppressed else "firesUnsuppressed", 0) + 1
				self.log(f"{who} fired: weapon {ev.a & 0xFFFF:#06x} ammo {proto.enum_name(ev.a >> 16, proto.AmmoKind)} x{ev.b & 0xFFFF}"
					f"{' SUPPRESSED' if suppressed else ''} "
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
			elif ev.type == GuestEventType.DogAttack:
				# v17: a = DogPhase | DogStyle << 8 | MGSV attack id << 16, b = MGSV's share (1/10000), v0 the dog, v1 dog -> target,
				# pad = the target's handle. The real host applies the style's effect on Connected (er/Dog.cpp).
				phase, style, attack = proto.dog_attack_fields(ev.a)
				handle = ev.pad[0] | (ev.pad[1] << 32)
				name = f"{proto.enum_name(phase, proto.DogPhase)}/{proto.enum_name(style, proto.DogStyle)}"
				st["dogAttacks"].append(name)
				who = next((i for i in range(2) if self.actor_handles[i] == handle), None)
				self.log(f"dog: {name} MGSV attack {attack} share {ev.b / 10000:.3f} on ER {handle:#x}"
					f"{f' (actor {who})' if who is not None else ' (not one of ours)' if handle else ' (no target)'} "
					f"the dog at ({ev.v0[0]:.2f},{ev.v0[1]:.2f},{ev.v0[2]:.2f}) dir ({ev.v1[0]:.2f},{ev.v1[1]:.2f},{ev.v1[2]:.2f})")
			elif ev.type == GuestEventType.QuietShot:
				# v18: a = QuietPhase | QuietWeapon << 8 | MGSV attack id << 16, b = MGSV's share (1/10000) or the QuietLostReason (Lost),
				# v0 her feet, v1 her -> the stand-in, pad = the target's handle. The real host fires from her post on Hit (er/Quiet.cpp).
				phase, weapon, attack = proto.quiet_shot_fields(ev.a)
				handle = ev.pad[0] | (ev.pad[1] << 32)
				name = f"{proto.enum_name(phase, proto.QuietPhase)}/{proto.enum_name(weapon, proto.QuietWeapon)}"
				st["quietShots"].append(name)
				who = next((i for i in range(2) if self.actor_handles[i] == handle), None)
				what = (f"reason {proto.enum_name(ev.b, proto.QuietLostReason)}" if phase == proto.QuietPhase.Lost else f"share {ev.b / 10000:.3f}")
				self.log(f"quiet: {name} MGSV attack {attack} {what} on ER {handle:#x}"
					f"{f' (actor {who})' if who is not None else ' (not one of ours)' if handle else ' (no target)'} "
					f"Quiet at ({ev.v0[0]:.2f},{ev.v0[1]:.2f},{ev.v0[2]:.2f}) dir ({ev.v1[0]:.2f},{ev.v1[1]:.2f},{ev.v1[2]:.2f})")
			elif ev.type == GuestEventType.BuddyOrder:
				# v7 (v17: pad[0] = D-Dog's style + 1; v18: Quiet's weapon + 1 for her commands 1-3)
				st["buddyOrders"].append([ev.a, ev.b, ev.pad[0]])
				style = (f" D-Dog style {proto.enum_name(ev.pad[0] - 1, proto.DogStyle)}" if ev.b == proto.BuddyType.DOG and ev.pad[0] else
					f" Quiet weapon {proto.enum_name(ev.pad[0] - 1, proto.QuietWeapon)}" if ev.b == proto.BuddyType.QUIET and ev.pad[0] else "")
				self.log(f"buddy order {ev.a} for {proto.enum_name(ev.b, proto.BuddyType)}{style}")
			elif ev.type == GuestEventType.TimeSkip:
				# v13: MGSV's Phantom Cigar; the real host moves Elden Ring's clock by as much (er/TimeSkip.h)
				self.time_skip_min = getattr(self, "time_skip_min", 0) + ev.a
				self.log(f"time skip: +{ev.a} min of MGSV game time (Phantom Cigar; {self.time_skip_min} min this session)")

		if self.draw.acquire():
			hdr, dl, draws, mats = link.read_draw_slot(self.draw.front)
			st["drawFrames"] += 1
			st["lastDrawCount"] = len(draws)
			st["lastMatrixCount"] = len(mats)
			projectiles = 0
			for d in draws:
				owner = proto.enum_name(d.owner, proto.DrawOwner)
				if owner not in st["drawOwnersSeen"]:
					st["drawOwnersSeen"].append(owner)
				st["drawsByOwner"][owner] = st["drawsByOwner"].get(owner, 0) + 1
				if d.owner == proto.DrawOwner.Projectile:
					projectiles += 1
					key = f"{d.mesh:016x}"
					if key not in st["projectileMeshes"]:
						st["projectileMeshes"].append(key)
						self.log(f"draws: projectile mesh {key} ({d.indexCount} indices, {d.matrixCount} matrices) in list frame {hdr.frame}")
				if d.mesh not in self.meshes:
					st["drawMissingMesh"] += 1
			if projectiles:
				st["listsWithProjectiles"] += 1
			if self.trace and self.trace_draws:
				self.trace.write(self.draw_trace_line(dl, draws, mats))
			self.on_draw_slot(self.draw.front, hdr)

		if self.hud.acquire():
			hdr, pixels = link.read_hud_slot(self.hud.front)
			st["hudFrames"] += 1
			st["hudLastSize"] = [hdr.width, hdr.height]
			self.on_hud_slot(hdr, pixels)

	def draw_trace_line(self, dl, draws, mats) -> str:
		"""--trace-draws: one '# draws' line per draw list -- where each owner's draws are, relative to the list's anchor (Snake's
		root): the mean of their first matrices' translations (the host draws them at its puppet + this, so a jump here between
		lists is a jump the player sees), their count, and every projectile and buddy draw's own place (mesh@x,y,z: a part
		culled in one list moves no mean); buddym= each buddy draw's mean over all of its matrices (its bones: where its skinned
		vertices are)."""
		sums: dict[int, list[float]] = {}
		proj, buddy, bones = [], [], []
		for d in draws:
			if d.firstMatrix >= len(mats):
				continue
			m = mats[d.firstMatrix]
			acc = sums.setdefault(d.owner, [0.0, 0.0, 0.0, 0])
			acc[0] += m[3]; acc[1] += m[7]; acc[2] += m[11]; acc[3] += 1
			if d.owner == proto.DrawOwner.Projectile:
				proj.append(f"{d.mesh:016x}@{m[3]:.2f},{m[7]:.2f},{m[11]:.2f}")
			elif d.owner == proto.DrawOwner.Buddy:
				buddy.append(f"{d.mesh & 0xFFFFFFFF:08x}@{m[3]:.3f},{m[7]:.3f},{m[11]:.3f}")
				# its skinned vertices follow all of its bone matrices: their mean is where the part is drawn (the first can be a root
				# MGSV snaps while the bones keep the mesh in place)
				n = max(1, min(d.matrixCount, len(mats) - d.firstMatrix))
				bx = sum(mats[d.firstMatrix + j][3] for j in range(n)) / n
				by = sum(mats[d.firstMatrix + j][7] for j in range(n)) / n
				bz = sum(mats[d.firstMatrix + j][11] for j in range(n)) / n
				bones.append(f"{d.mesh & 0xFFFFFFFF:08x}@{bx:.3f},{by:.3f},{bz:.3f}")
		parts = []
		for owner, (x, y, z, n) in sorted(sums.items()):
			parts.append(f"{proto.enum_name(owner, proto.DrawOwner)}={n}@{x / n:.3f},{y / n:.3f},{z / n:.3f}")
		return (f"# draws frame={dl.frame} anchor={dl.anchorPos[0]:.3f},{dl.anchorPos[1]:.3f},{dl.anchorPos[2]:.3f} " + " ".join(parts)
			+ (" proj=" + ";".join(proj) if proj else "") + (" buddy=" + ";".join(buddy) if buddy else "")
			+ (" buddym=" + ";".join(bones) if bones else "") + f" t={time.monotonic() - self.t0:.4f}\n")

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

	# ---- v18 Quiet and the grace rest -------------------------------------------------------------------
	def to_mgsv(self, p) -> list[float]:
		"""A point kept in Snake's odometer frame (fixed in Elden Ring) -> MGSV world now, as the real host maps it."""
		gs = self.guest_last
		return [gs.rootPos[i] + p[i] - gs.odo[i] + self.slip[i] for i in range(3)]

	def to_odo(self, dx: float, dz: float) -> tuple[float, float, float]:
		"""dx, dz m from Snake now -> the odometer frame (Elden Ring's Snake is at odo - slip)."""
		gs = self.guest_last
		return (gs.odo[0] - self.slip[0] + dx, gs.odo[1], gs.odo[2] - self.slip[2] + dz)

	def fill_quiet_state(self, hs):
		nan = float("nan")
		hs.graceCentre[:] = hs.graceSeat[:] = hs.buddyPost[:] = [nan, nan, nan]
		hs.graceSeq, hs.buddyPostSeq = self.grace_seq, self.post_seq
		if self.guest_last is None:
			return
		if self.grace is not None:
			hs.graceCentre[:] = self.to_mgsv(self.grace["centre"])
			if self.grace["seat"] is not None:
				hs.graceSeat[:] = self.to_mgsv(self.grace["seat"])
				hs.graceSeatYaw = self.grace["yaw"]
		if self.post is not None:
			hs.buddyPost[:] = self.to_mgsv(self.post)
			hs.buddyPostYaw = self.post_yaw

	def log_quiet_echo(self, gs):
		"""The guest's echoes of the post and the seat and Quiet's buddy bits, logged as they change."""
		st = self.stats
		bits = gs.buddyFlags & int(proto.GuestBuddyFlags.AtPost | proto.GuestBuddyFlags.Seated | proto.GuestBuddyFlags.Cloaked |
			proto.GuestBuddyFlags.CanRest | proto.GuestBuddyFlags.QuietMarked | proto.GuestBuddyFlags.QuietCover | proto.BUDDY_QUIET_WEAPON_MASK)
		if gs.buddyFlags & proto.GuestBuddyFlags.AtPost and gs.buddyPostSeq:
			st["postEchoFrames"] += 1
			st["postSeqEchoed"] = gs.buddyPostSeq
		if gs.buddyFlags & proto.GuestBuddyFlags.Seated and gs.graceSeqSeated:
			st["seatEchoFrames"] += 1
			st["graceSeqEchoed"] = gs.graceSeqSeated
		weapon = proto.buddy_quiet_weapon(gs.buddyFlags)
		if weapon != 0xFF and proto.enum_name(weapon, proto.QuietWeapon) not in st["quietWeaponsSeen"]:
			st["quietWeaponsSeen"].append(proto.enum_name(weapon, proto.QuietWeapon))
		now = (gs.buddyPostSeq, gs.graceSeqSeated, bits)
		if now != self.echo_last:
			self.echo_last = now
			self.log(f"guest: Quiet {proto.flag_names(bits & ~proto.BUDDY_QUIET_WEAPON_MASK, proto.GuestBuddyFlags)} weapon "
				f"{proto.enum_name(weapon, proto.QuietWeapon)}, post seq {gs.buddyPostSeq} (ours {self.post_seq}), seated for rest "
				f"{gs.graceSeqSeated} (ours {self.grace_seq}), buddy at ({gs.buddyPos[0]:.2f},{gs.buddyPos[1]:.2f},{gs.buddyPos[2]:.2f}) "
				f"yaw {math.degrees(gs.buddyYaw):.1f} deg")

	def quiet_state_text(self) -> str:
		hs = proto.HostState()
		self.fill_quiet_state(hs)
		v = lambda a: "none" if not math.isfinite(a[0]) else f"({a[0]:.2f},{a[1]:.2f},{a[2]:.2f})"
		flags = (["QuietLayer"] if self.quiet_layer else []) + (["AtGrace"] if self.grace is not None else []) + (["DogLayer"] if self.dog_layer else [])
		return (f"flags {'|'.join(flags) or '0'}; grace seq {hs.graceSeq} centre {v(hs.graceCentre)} seat {v(hs.graceSeat)} yaw "
			f"{math.degrees(hs.graceSeatYaw):.1f} deg; post seq {hs.buddyPostSeq} at {v(hs.buddyPost)} yaw {math.degrees(hs.buddyPostYaw):.1f} deg")

	def post_command(self, args: list[str]) -> tuple[int, str]:
		if args == ["none"]:
			self.post = None
			return 0, f"no post (on the move: buddyPost NaN, seq stays {self.post_seq}); {self.quiet_state_text()}"
		if self.guest_last is None:
			return 1, "post: needs a guest state"
		try:
			dx, dz = float(args[0]), float(args[1])
			yaw = math.radians(float(args[2])) if len(args) > 2 else self.guest_last.rootYaw
		except (ValueError, IndexError):
			return 1, "usage: post <dx> <dz> [yaw_deg] | none   (MGSV x/z metres from Snake now)"
		self.post, self.post_yaw = self.to_odo(dx, dz), yaw
		self.post_seq += 1
		return 0, f"post #{self.post_seq} {dx:g}, {dz:g} m from Snake; {self.quiet_state_text()}"

	def grace_command(self, args: list[str]) -> tuple[int, str]:
		usage = "usage: grace 1 [<dx> <dz> [yaw_deg]] | grace 1 none | grace 0   (the seat dx, dz m from Snake; none: a rest without a seat)"
		if args == ["0"]:
			was = self.grace is not None
			self.grace = None
			return 0, f"{'left the grace' if was else 'not at a grace'} (AtGrace off, the grace fields NaN); {self.quiet_state_text()}"
		if not args or args[0] != "1":
			return 1, usage
		gs = self.guest_last
		if gs is None:
			return 1, "grace: needs a guest state"
		seatless = args[1:] == ["none"]
		try:
			dx, dz = (float(args[1]), float(args[2])) if len(args) > 2 else (1.5, 1.5)
		except ValueError:
			return 1, usage
		fwd = (math.sin(gs.rootYaw), math.cos(gs.rootYaw))
		centre = self.to_odo(1.2 * fwd[0], 1.2 * fwd[1])
		seat = None if seatless else self.to_odo(dx, dz)
		if seat is not None and len(args) > 3:
			yaw = math.radians(float(args[3]))
		elif seat is not None:
			yaw = math.atan2(centre[0] - seat[0], centre[2] - seat[2])  # facing the grace
		else:
			yaw = 0.0
		self.grace = {"centre": centre, "seat": seat, "yaw": yaw}
		if seat is not None:
			self.grace_seq += 1  # as the real host: graceSeq bumps with every PUBLISHED seat (er/GraceRest.cpp), not at a seatless rest
		self.host_event(HostEventType.Rested, 0, 0)  # the real host's refill edge on every rest
		return 0, f"rest{' without a seat (graceSeq stays ' + str(self.grace_seq) + ')' if seatless else ' #' + str(self.grace_seq)} (+ kHEvRested 0); {self.quiet_state_text()}"

	def actor_positions(self) -> list[list[float]] | None:
		"""--actors near: the actors' MGSV-world feet now (None before the first guest state)."""
		gs = self.guest_last
		if gs is None:
			return None
		if self.actor_points is None:
			self.actor_points = [[gs.odo[0] + x, gs.odo[1], gs.odo[2] + z] for _, x, z in self.NEAR_ACTORS]
		# Elden Ring's Snake is at odo - slip (what a wall kept him from): a character standing still in Elden Ring is mapped by it.
		return [[gs.rootPos[i] + p[i] - gs.odo[i] + self.slip[i] for i in range(3)] for p in self.actor_points]

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
			return 1, ("usage: actor 0|1 asleep|awake|dead|alive|dogtarget|nodogtarget|quiettarget|noquiettarget|quietthreat|noquietthreat|"
				"at <dx> <dz>|back   (at: MGSV x/z metres from Snake now; --actors near)")
		i = int(args[0])
		flag = {"asleep": int(proto.ActorFlags.Asleep), "dead": int(proto.ActorFlags.Dead)}
		match args[1:]:
			case ["asleep" | "dead" as what]:
				self.actor_flags[i] |= flag[what]
			case ["dogtarget"]:
				# v17: the host's pick for D-Dog's next order -- at most one record carries it
				for j in range(len(self.actor_flags)):
					self.actor_flags[j] &= ~int(proto.ActorFlags.DogTarget)
				self.actor_flags[i] |= int(proto.ActorFlags.DogTarget)
			case ["nodogtarget"]:
				self.actor_flags[i] &= ~int(proto.ActorFlags.DogTarget)
			case ["quiettarget" | "quietthreat" as what]:
				# v18: the host's pick for Quiet's Fire / Take aim, or Cover me's threat pick -- at most one record carries each
				bit = int(proto.ActorFlags.QuietTarget if what == "quiettarget" else proto.ActorFlags.QuietThreat)
				for j in range(len(self.actor_flags)):
					self.actor_flags[j] &= ~bit
				self.actor_flags[i] |= bit
			case ["noquiettarget"]:
				self.actor_flags[i] &= ~int(proto.ActorFlags.QuietTarget)
			case ["noquietthreat"]:
				self.actor_flags[i] &= ~int(proto.ActorFlags.QuietThreat)
			case ["awake"]:
				self.actor_flags[i] &= ~flag["asleep"]
			case ["alive"]:
				self.actor_flags[i] &= ~flag["dead"]
			case ["back"]:
				self.fultoned.discard(self.actor_handles[i])
			case ["renew"]:
				self.actor_handles[i] = 0xFFFFFFFF00000000 | (max(h & 0xFFFFFFFF for h in self.actor_handles) + 0x10)
				self.actor_flags[i] = 0
			case ["at", dx, dz, *dy]:
				# (Quiet, Q4's elevated case) an optional dy: the character that many metres above Snake's feet (Elden Ring's enemies
				# on walls and ledges: 34-36 m up at Gatefront, docs/research/quiet.md §2.9)
				gs = self.guest_last
				if self.actors_mode != "near" or gs is None:
					return 1, "actor at: needs --actors near and a guest state"
				self.actor_positions()
				self.actor_points[i] = [gs.odo[0] - self.slip[0] + float(dx), gs.odo[1] + (float(dy[0]) if dy else 0.0), gs.odo[2] - self.slip[2] + float(dz)]
			case _:
				return 1, "usage: actor 0|1 asleep|awake|dead|alive|dogtarget|nodogtarget|quiettarget|noquiettarget|quietthreat|noquietthreat|at <dx> <dz> [dy]|back|renew"
		self.publish_actors()
		pos = self.actor_positions()
		where = f" at {pos[i][0]:.2f},{pos[i][1]:.2f},{pos[i][2]:.2f}" if pos else ""
		gone = " (fultoned: not published)" if self.actor_handles[i] in self.fultoned else ""
		return 0, (f"actor {i} handle {self.actor_handles[i]:#x} flags {self.actor_flags[i]:#x} "
			f"({proto.flag_names(int(proto.ActorFlags.Hostile) | self.actor_flags[i], proto.ActorFlags)}){where}{gone}")

	def host_event(self, typ, a: int, b: int, v0: list[float] | None = None) -> tuple[int, str]:
		ev = proto.HostEvent(type=int(typ), a=a, b=b, frame=self.frame & 0xFFFFFFFF)
		ev.v0[:] = v0 if v0 is not None else [0.0, 0.0, 1.0]
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
					"rested 0|1, tarnished 0|1, placed, detonate <id>|all, actors, actor 0|1 asleep|awake|dead|alive|dogtarget|nodogtarget|at <dx> <dz>|back|renew, "
					"climb 1|0 [down] [speed m/s], mantle <cm> [vault] [secs], doglayer 0|1, block 1|0, "
					"quietlayer 0|1, post <dx> <dz> [yaw_deg]|none, grace 1 [<dx> <dz> [yaw_deg]]|1 none|0, hoststate; "
					"actor 0|1 quiettarget|noquiettarget|quietthreat|noquietthreat")
			case ["quietlayer", which]:
				# v18: HostFlags.QuietLayer -- the real host sets it while [Quiet] enabled; without it the guest leaves Quiet to MGSV (0.8.0)
				if which not in ("0", "1"):
					return 1, "usage: quietlayer 0|1"
				self.quiet_layer = which == "1"
				return 0, f"HostFlags.QuietLayer {'on' if self.quiet_layer else 'off'}"
			case ["post", *rest] if rest:
				return self.post_command(rest)
			case ["grace", *rest]:
				return self.grace_command(rest)
			case ["hoststate"]:
				return 0, self.quiet_state_text()
			case ["block", which]:
				# Snake against an Elden Ring wall: the puppet stays while MGSV's Snake runs on (the slip; the actors move by it in MGSV)
				if which not in ("0", "1"):
					return 1, "usage: block 1|0"
				self.blocked = which == "1"
				if self.trace:
					self.trace.write(f"# block {which} slip={self.slip[0]:.3f},{self.slip[2]:.3f} t={time.monotonic() - self.t0:.4f}\n")
				return 0, f"Elden Ring's Snake {'blocked (a wall)' if self.blocked else 'free'}; slip so far {self.slip[0]:.2f},{self.slip[2]:.2f} m"
			case ["doglayer", which]:
				# v17: HostFlags.DogLayer -- the real host sets it while [Dog] enabled; without it the guest leaves D-Dog's orders to MGSV
				if which not in ("0", "1"):
					return 1, "usage: doglayer 0|1"
				self.dog_layer = which == "1"
				return 0, f"HostFlags.DogLayer {'on' if self.dog_layer else 'off'}"
			case ["actor", *rest]:
				return self.actor_command(rest)
			case ["actors"]:
				pos = self.actor_positions() if self.actors_mode == "near" else None
				return 0, "; ".join(f"{i}: flags {self.actor_flags[i]:#x} ({proto.flag_names(int(proto.ActorFlags.Hostile) | self.actor_flags[i], proto.ActorFlags)})" + (f" at {pos[i][0]:.2f},{pos[i][1]:.2f},{pos[i][2]:.2f}" if pos else "") +
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
			case ["climb", which, *rest]:
				# v15: an Elden Ring ladder climb started (1) / ended (0): b = 1 going down, v0[0] = its vertical speed (m/s; 0 = unknown,
				# the guest climbs at MGSV's own pace). The guest plays MGSV's ladder climb meanwhile (kGuestClimbing).
				if which not in ("0", "1"):
					return 1, "usage: climb 1|0 [down] [speed m/s]"
				down = "down" in rest
				speed = 0.0
				for w in rest:
					if w != "down" and w != "up":
						try:
							speed = float(w)
						except ValueError:
							return 1, f"climb: bad speed {w!r}"
				self.climbing = which == "1"
				return self.host_event(HostEventType.Climb, int(which), 1 if down else 0, [speed, 0.0, 0.0])
			case ["mantle", cm, *rest]:
				# v15: Snake climbs onto (b = 0) / vaults over (b = 1) a ledge a cm high; v0[0] = the move's duration (s)
				try:
					height = int(cm, 0)
					secs = next((float(w) for w in rest if w != "vault"), 0.0)
				except ValueError:
					return 1, "usage: mantle <cm> [vault] [secs]"
				return self.host_event(HostEventType.Mantle, height, 1 if "vault" in rest else 0, [secs, 0.0, 0.0])
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
		"ammoInClip,ammoStock,camYawDeg,nvg,slip x,slip z) and one '# event' line per guest event (and '# host' per host event sent)")
	ap.add_argument("--trace-draws", action="store_true", help="with --trace: also one '# draws frame=F anchor=x,y,z Owner=n@x,y,z ... "
		"proj=mesh@x,y,z;... buddy=mesh@x,y,z;...' line per draw list (each owner's mean place relative to the anchor; projectile and buddy "
		"draws one by one)")
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
		host.trace_draws = args.trace_draws
		# the trace's t is time.monotonic() - t0: other probes (e.g. a /proc/<pid>/mem poller) line up with it through this
		host.trace.write(f"# t0 monotonic={host.t0:.4f}\n")
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
					f"({st['lastDrawCount']} draws), hud {st['hudFrames']}, life {st['lifeLast']}"
					+ (f", projectile draws {st['drawsByOwner'].get('Projectile', 0)} in {st['listsWithProjectiles']} lists" if st['listsWithProjectiles'] else ""))
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
