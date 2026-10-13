"""Stand-in for the MGSV guest plugin (dinput8.dll inside mgsvtpp.exe), for testing the Elden Ring host
(or fake_er.py) without MGSV.

It attaches like the real guest (waits for the host's session nonce, then resets its producer sides),
heartbeats, and every frame publishes: Snake walking a circle in a sandbox (plus whatever the pad's left
stick adds) with an odometer, a 178-bone pose, an orbiting camera, D-Walker deployed as the buddy (and
ridden for 4 s of every 12), fire events every 2 s and buddy fire every 3 s, a draw list (Snake +
D-Walker) on the triple buffer, a HUD image (--hud: mgsv = a 1280x720 MGSV-like layer with the aim reticle
and the weapon/ammo box, fakehud.py; bar = the old 64x8 life bar; none), and a triangle mesh + texture per
owner on the render ring. It answers guest-console Lua with a tiny subset (luasubset.py).

With --model humanoid it sends a real-looking skinned test model instead (testmodel.py: a 16-bone walking
figure with BC1 camo albedo, normal and SRM maps, two vertex streams, raised +X arm and a nose on its +Z
front, plus a rigid D-Walker block), and publishes the same skeleton as its pose, so the Elden Ring renderer
can be checked against the debug skeleton.

    python3 -m phantomring.fake_mgsv [--seconds N] [--create] [--summary out.json] [--quiet] [--model triangle|humanoid]
        [--stand]   (humanoid: walk in place, facing a fixed direction, no circle)

Combat by hand (docs/COMBAT.md), through the guest console (`tools/pr lua '!<cmd>'`):
    !walk on|off        Snake walks his circle / stands still facing where he faces (aim with the ER camera)
    !autofire on|off    the periodic fire/buddy-fire events
    !fire <kind> [n] [yaw_deg] [pitch_deg] [share]   kind: lethal|handgun|rifle|sniper|buck|tranq|stun|explosive|rocket|grenade
    !cqc <punch|kick|throw|choke>            a strike 1 m in front of Snake
    !blast [radius_m]                        a kGEvExplosion 4 m in front of Snake
    !die                                     life 1 + kGEvDied (Elden Ring should run YOU DIED)
    !life <n> [regen]                        set Snake's life (of 6000) and the regeneration per second (default 300)
    !blast [radius_m] [lethal|sleep|stun|smoke]  a kGEvExplosion 4 m in front of Snake (no throw id)
    !share <s>                               (v8) MGSV's damage share sent with every !fire (pad[1] = s x 10000; 0 = unknown)
    !move <speed m/s> [yaw_deg]              walk/run at a constant speed along an absolute MGSV yaw (0 = +Z); !move 0 stops
    !ride on|off|auto                        D-Walker ridden / not / the old 4 s of every 12
    !box on|off|poster                       (v8) kGuestInBox (+ kGuestBoxPoster): Snake hides in a cardboard box
    !flame [seconds]                         D-Walker's flamethrower: kGEvBuddyFire with no bullet (pad[1] = 12000 mm reach) ~8/s
    !fpv on|off                              kGuestFirstPerson
    !cam <phase_rad> [dist] [height] [look_y]   a fixed camera (no orbit) looking along -(cos phase, sin phase)

Thrown and placed explosives (protocol v5/v6, er/Throw.h, er/Mines.h), simulated MGSV-style over the fake's flat
ground (y = 0, gravity 9.8 m/s^2, no bounce):
    !throw <frag|sleep|stun|smoke> [speed m/s] [pitch_deg] [yaw_deg]   kGEvThrow from Snake's hand, then MGSV's blast
                                             (kGEvExplosion, pad[1] = the throw id) where the fake's copy lands (impact
                                             kinds) or after the fuse (frag 3.5 s, stun 1.5 s); sleep gas and smoke keep
                                             reporting their cloud every 0.5 s (12 s / 20 s)
    !aimthrow on|off [speed] [pitch_deg] [kind]  kGuestThrowAim + aimThrowPos/Vel/Kind every frame (the host draws the arc)
    !place <claymore|mine|sleepmine|stunmine|c4> [trigger_m]   kGEvPlaced 1 m in front of Snake (c4: trigger 0)
    !detonate <id>                           Snake's detonator: that placed explosive goes off (kGEvExplosion + kGEvPlacedGone)
    The host's kHEvDetonate sets a placed one off the same way.

Overlay tests (er/Overlay.h):
    !aim on|off         kGuestAiming (the MGSV HUD shows its reticle; ER's fallback reticle needs --hud none|bar)
    !hud mgsv|bar|none  what the HUD layer shows
    !boot N [text]      back to "booting" for N s: not ready, guestStatusText = text (ER shows it in a corner)
    !fail [text]        status Failed with that text; !ready to recover
    !menu on|off        kGuestInMenu (MGSV's iDroid/pause owns the pad: the host must give MGSV the whole pad, ER nothing)

    python3 -m phantomring.fake_mgsv [--seconds N] [--create] [--summary out.json] [--quiet]
"""

from __future__ import annotations

import argparse
import json
import math
import os
import signal
import struct
import sys
import time

from . import fakehud, luasubset, proto, testmodel
from .proto import GuestEventType, GuestFlags, HostEventType, Status

FPS = 60
BONES = 178               # Snake's skeleton size the fake publishes
SANDBOX_RADIUS = 10.0     # m: Snake is warped back to the centre past this (the warp is not intended motion)
LIFE_MAX = 6000
WEAPON_ID = 0x0123        # arbitrary fake equip id
DWALKER_VEHICLE_ID = 0x0402
MESH_SNAKE, MESH_DWALKER = 0x5A4B000000000001, 0x5A4B000000000002
TEX_SNAKE, TEX_DWALKER = 0x7E57000000000001, 0x7E57000000000002
# !fire kinds: TppEquip bullet id, AmmoKind, pellets, explosive (GuestEvent.pad[0] = id | 0x80000000 if explosive)
FIRE_KINDS = {
	"lethal": (5, proto.AmmoKind.Lethal, 1, False), "handgun": (5, proto.AmmoKind.Lethal, 1, False),
	"rifle": (15, proto.AmmoKind.Lethal, 1, False), "sniper": (37, proto.AmmoKind.Lethal, 1, False),
	"buck": (18, proto.AmmoKind.Lethal, 8, False), "tranq": (10, proto.AmmoKind.Tranq, 1, False),
	"stun": (7, proto.AmmoKind.Stun, 1, False), "explosive": (27, proto.AmmoKind.Lethal, 1, True),
	"rocket": (59, proto.AmmoKind.Lethal, 1, True), "grenade": (23, proto.AmmoKind.Lethal, 1, True),
}


BLAST_KINDS = {"lethal": proto.AmmoKind.Lethal, "sleep": proto.AmmoKind.Tranq, "stun": proto.AmmoKind.Stun, "smoke": proto.AmmoKind.Smoke}
# !throw kinds: AmmoKind, MGSV equip id (fake), fuse ms (0 = impact), MGSV blast radius m, cloud seconds
THROW_KINDS = {
	"frag": (proto.AmmoKind.Lethal, 0x0201, 3500, 7.0, 0.0), "sleep": (proto.AmmoKind.Tranq, 0x0202, 0, 9.0, 12.0),
	"stun": (proto.AmmoKind.Stun, 0x0203, 1500, 6.0, 0.0), "smoke": (proto.AmmoKind.Smoke, 0x0204, 0, 8.0, 20.0),
}
# !place kinds: AmmoKind, equip id (fake), trigger radius m (0 = remote only), blast radius m
PLACE_KINDS = {
	"claymore": (proto.AmmoKind.Lethal, 0x0301, 2.5, 6.0), "mine": (proto.AmmoKind.Lethal, 0x0302, 2.0, 5.0),
	"sleepmine": (proto.AmmoKind.Tranq, 0x0303, 2.0, 6.0), "stunmine": (proto.AmmoKind.Stun, 0x0304, 2.0, 5.0),
	"c4": (proto.AmmoKind.Lethal, 0x0305, 0.0, 7.0),
}


def triangle_mesh(key: int, size: float) -> bytes:
	elements = [
		proto.RenInputElement(semantic=b"POSITION", semanticIndex=0, format=proto.DXGI_FORMAT_R32G32B32_FLOAT, inputSlot=0, alignedByteOffset=0),
		proto.RenInputElement(semantic=b"TEXCOORD", semanticIndex=0, format=proto.DXGI_FORMAT_R32G32_FLOAT, inputSlot=0, alignedByteOffset=12),
	]
	verts = [(0.0, 0.0, 0.0, 0.0, 1.0), (size, 0.0, 0.0, 1.0, 1.0), (0.0, size * 1.8, 0.0, 0.0, 0.0)]
	vb = b"".join(struct.pack("<5f", *v) for v in verts)
	ib = struct.pack("<3H", 0, 1, 2)
	mesh = proto.RenMesh(key=key, elementCount=len(elements), vertexStride=20, vertexBytes=len(vb),
		indexFormat=proto.DXGI_FORMAT_R16_UINT, indexBytes=len(ib), topology=proto.D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, streams=1)
	mesh.streamStride[0] = 20
	mesh.streamBytes[0] = len(vb)
	return proto.build_mesh_msg(mesh, elements, vb, ib)


def texture(key: int, rgba: tuple[int, int, int, int]) -> bytes:
	data = bytes(rgba) * 16
	tex = proto.RenTexture(key=key, format=proto.DXGI_FORMAT_R8G8B8A8_UNORM, width=4, height=4, mipLevels=1, arraySize=1, dataBytes=len(data))
	return bytes(tex) + data


def quat_look(fwd) -> tuple[float, float, float, float]:
	"""Quaternion (xyzw) for a camera looking along fwd with Y up (yaw + pitch only)."""
	yaw = math.atan2(fwd[0], fwd[2])
	pitch = -math.atan2(fwd[1], math.hypot(fwd[0], fwd[2]))
	cy, sy, cp, sp = math.cos(yaw / 2), math.sin(yaw / 2), math.cos(pitch / 2), math.sin(pitch / 2)
	return (cy * sp, sy * cp, -sy * sp, cy * cp)


def matrix_3x4(x, y, z, yaw=0.0) -> tuple[float, ...]:
	c, s = math.cos(yaw), math.sin(yaw)
	return (c, 0.0, s, x, 0.0, 1.0, 0.0, y, -s, 0.0, c, z)


class FakeGuest:
	def __init__(self, link: proto.Link, quiet: bool, model: str = "triangle", stand: bool = False, hud: str = ""):
		self.link = link
		self.quiet = quiet
		self.humanoid = testmodel.Humanoid() if model == "humanoid" else None
		self.stand = stand
		self.orbit_speed = 0.5     # rad/s of the fake camera around Snake
		self.orbit_phase = 0.0
		self.freeze = -1.0        # humanoid: >= 0 holds the animation at this time (s)
		self.cam_dist, self.cam_height, self.cam_look_y = 6.0, 1.8, 1.0
		self.last_model = None
		self.hud_mode = hud or ("mgsv" if model == "humanoid" else "bar")
		self.hud_cache = (None, b"")
		self.aiming = False
		self.ammo, self.stock = 30, 120
		self.boot_until, self.boot_text = 0.0, ""
		self.failed = ""
		self.nonce = 0
		self.attached = False
		self.frame = 0
		self.t0 = time.monotonic()
		self.odo = [0.0, 0.0, 0.0]
		self.odo_epoch = 0
		self.root = [0.0, 0.0, 0.0]
		self.yaw = 0.0
		self.heading = 0.0
		self.life = LIFE_MAX
		self.regen = 300.0        # life per second (MGSV regenerates)
		self.regen_acc = 0.0
		self.in_menu = False
		self.last_buttons = 0
		self.vars = luasubset.Table()
		self.env = luasubset.base_env() | {
			"vars": self.vars,
			"BuddyType": luasubset.Table({b.name: b.value for b in proto.BuddyType}),
		}
		self.draw = link.triple_writer(proto.OFF_DRAW_CTL)
		self.hud = link.triple_writer(proto.OFF_HUD_CTL)
		self.buddy_ammo = 500
		self.next_fire = 2.0
		self.next_buddy_fire = 3.0
		self.walk = True
		self.autofire = True
		self.stats = {
			"attaches": 0, "frames": 0, "padPackets": 0, "maxThumbLY": 0, "maxThumbLX": 0, "minThumbLY": 0,
			"buttonsSeen": 0, "neutralFrames": 0, "hostStates": 0, "hostFlagsSeen": 0, "hostEvents": {},
			"actorsSeen": 0, "fireSent": 0, "buddyFireSent": 0, "diedSent": 0, "drawFramesPublished": 0,
			"hudFramesPublished": 0, "renderMessages": 0, "renderDropped": 0, "consoleServed": 0, "warps": 0,
			"rideFrames": 0, "lifeMin": LIFE_MAX, "healed": 0, "backPresses": 0, "startPresses": 0, "menuButtons": 0,
		}
		self.last_packet = None
		self.grenades = []        # thrown, flying over the fake's flat ground (v5)
		self.mines = {}           # placed id -> dict (v5)
		self.clouds = []          # lingering gas / smoke reports
		self.next_id = 1
		self.aim_throw = None     # (speed, pitch_rad, AmmoKind) while !aimthrow on (v6)
		self.share = 0.0          # v8: MGSV damage share sent with !fire (pad[1] / 10000)
		self.move = None          # (speed, yaw_rad): !move, a constant velocity in MGSV world
		self.ride = "auto"        # !ride on|off|auto
		self.box = ""             # !box: "", "on", "poster"
		self.fpv = False
		self.flame_until = 0.0
		self.next_flame = 0.0

	def elapsed(self) -> float:
		return time.monotonic() - self.t0

	def status(self, status: Status, text: str):
		h = self.link.header
		h.guestStatus = int(status)
		proto.set_text(h, "guestStatusText", text)

	# -- (re)attach: everything the host might have dropped is reset and resent
	def try_attach(self) -> bool:
		h = self.link.header
		if h.magic != proto.MAGIC or h.version != proto.VERSION or h.sessionNonce == 0:
			if self.attached:
				self.attached = False
			return False
		if h.sessionNonce == self.nonce and h.guestNonce == self.nonce:
			return True
		self.nonce = h.sessionNonce
		h.guestNonce = 0  # detached while the producer sides reset
		self.draw.reset()
		self.hud.reset()
		self.link.host_events.drain()
		self.odo_epoch += 1
		self.odo = [0.0, 0.0, 0.0]
		ring = self.link.render_ring
		if self.humanoid:
			messages = [(proto.RenType.ClearAll, b"")] + list(self.humanoid.messages())
		else:
			messages = [
				(proto.RenType.ClearAll, b""),
				(proto.RenType.Mesh, triangle_mesh(MESH_SNAKE, 0.6)),
				(proto.RenType.Texture, texture(TEX_SNAKE, (90, 80, 60, 255))),
				(proto.RenType.Mesh, triangle_mesh(MESH_DWALKER, 1.2)),
				(proto.RenType.Texture, texture(TEX_DWALKER, (120, 120, 130, 255))),
			]
		for msg_type, payload in messages:
			if ring.push(msg_type, payload):
				self.stats["renderMessages"] += 1
			else:
				self.stats["renderDropped"] += 1
		h.guestUnixPid = os.getpid()
		h.guestNonce = self.nonce  # last: the host reads our slots only once this matches
		self.attached = True
		self.stats["attaches"] += 1
		self.status(Status.Ready, "fake MGSV: in the sandbox, D-Walker deployed")
		self.log(f"attached to session {self.nonce:#018x} (epoch {self.odo_epoch})")
		return True

	def log(self, text: str):
		if not self.quiet:
			print(f"[fake_mgsv {self.elapsed():6.2f}s] {text}", flush=True)

	def tick(self, dt: float):
		self.link.header.guestHeartbeatQpc = proto.qpc_now()
		if not self.try_attach():
			return
		self.frame += 1
		t = self.elapsed()
		link, st = self.link, self.stats
		st["frames"] += 1

		host = link.host_state()
		host_flags = 0
		if host:
			st["hostStates"] += 1
			host_flags = host.flags
			st["hostFlagsSeen"] |= host.flags

		pad = link.pad_state()
		lx = ly = 0
		if pad:
			if pad.packet != self.last_packet:
				self.last_packet = pad.packet
				st["padPackets"] += 1
			neutral = pad.flags & proto.PadFlags.Neutral or host_flags & proto.HostFlags.MenuOpen
			if neutral:
				st["neutralFrames"] += 1
			else:
				lx, ly = pad.thumbLX, pad.thumbLY
				st["maxThumbLY"] = max(st["maxThumbLY"], ly)
				st["minThumbLY"] = min(st["minThumbLY"], ly)
				st["maxThumbLX"] = max(st["maxThumbLX"], lx)
				st["buttonsSeen"] |= pad.buttons
				pressed = pad.buttons & ~self.last_buttons
				if pressed & 0x0020:  # Back/View: MGSV opens its iDroid with it
					st["backPresses"] += 1
					self.log("pad: Back pressed (MGSV would open its iDroid)")
				if pressed & 0x0010:
					st["startPresses"] += 1
					self.log("pad: Start pressed (MGSV would pause)")
				if self.in_menu and pressed:
					st["menuButtons"] += 1
					self.log(f"pad: {pressed:#06x} pressed in the menu")
			self.last_buttons = 0 if neutral else pad.buttons

		while (ev := link.host_events.pop()) is not None:
			name = proto.enum_name(ev.type, HostEventType)
			st["hostEvents"][name] = st["hostEvents"].get(name, 0) + 1
			if ev.type == HostEventType.Hurt:
				self.life = max(0, self.life - ev.a * 4)
				if self.life == 0:
					self.life = 1  # the guest keeps Snake alive; Elden Ring runs the death
					self.push_event(GuestEventType.Died)
					st["diedSent"] += 1
			elif ev.type == HostEventType.Respawn:
				self.life = LIFE_MAX
			elif ev.type == HostEventType.Detonate:
				self.log(f"host: kHEvDetonate placed {ev.b}")
				self.set_off(ev.b, "kHEvDetonate")
			elif ev.type == HostEventType.Heal:
				before = self.life
				self.life = min(LIFE_MAX, self.life + ev.a * 4)
				st["healed"] += self.life - before
				self.log(f"heal: {ev.a} x 4 (ER {ev.b} HP) -> life {before} -> {self.life}")
		self.regen_acc += self.regen * dt  # MGSV-style regeneration
		self.life = min(LIFE_MAX, self.life + int(self.regen_acc))
		self.regen_acc -= int(self.regen_acc)
		st["lifeMin"] = min(st["lifeMin"], self.life)

		actors = link.actors()
		if actors:
			st["actorsSeen"] = actors.count

		# Motion: a slow circle plus the left stick (MGSV would turn the stick into locomotion itself).
		riding = (6.0 <= (t % 12.0) < 10.0) if self.ride == "auto" else self.ride == "on"
		if riding:
			st["rideFrames"] += 1
		speed = 0.0 if (self.stand or not self.walk) else 1.4 * (2.5 if riding else 1.0)
		self.heading += speed / 4.0 * dt
		vx, vz = math.cos(self.heading) * speed, -math.sin(self.heading) * speed
		vx += lx / 32767.0 * 3.0
		vz += ly / 32767.0 * 3.0
		if self.move:
			vx, vz = math.sin(self.move[1]) * self.move[0], math.cos(self.move[1]) * self.move[0]
		dx, dz = vx * dt, vz * dt
		self.odo[0] += dx
		self.odo[2] += dz
		self.root[0] += dx
		self.root[2] += dz
		if math.hypot(self.root[0], self.root[2]) > SANDBOX_RADIUS:
			self.root = [0.0, 0.0, 0.0]  # treadmill warp: not part of the odometer
			st["warps"] += 1
		if abs(vx) + abs(vz) > 1e-3:
			self.yaw = math.atan2(vx, vz)
		elif self.stand:
			self.yaw = 0.6  # a fixed, non-axis-aligned facing

		orbit = t * self.orbit_speed + self.orbit_phase
		cam = (self.root[0] + self.cam_dist * math.cos(orbit), self.cam_height, self.root[2] + self.cam_dist * math.sin(orbit))
		look = (self.root[0] - cam[0], self.cam_look_y - cam[1], self.root[2] - cam[2])

		gs = proto.GuestState()
		flags = GuestFlags.Ready | GuestFlags.Standing
		if not self.ready():
			flags = GuestFlags.Loading  # booting / failed: the host must not start the puppet
			if self.failed:
				self.status(Status.Failed, self.failed)
			else:
				self.status(Status.Loading, self.boot_text)
		elif self.boot_until or self.link.header.guestStatus != int(Status.Ready):
			self.boot_until = 0.0
			self.status(Status.Ready, "fake MGSV: in the sandbox, D-Walker deployed")
		if self.aiming:
			flags |= GuestFlags.Aiming
		if self.in_menu:
			flags |= GuestFlags.InMenu
		if riding:
			flags |= GuestFlags.OnVehicle
		if math.hypot(vx, vz) > 4.0:
			flags |= GuestFlags.Sprint
		if self.box:
			flags |= GuestFlags.InBox | (GuestFlags.BoxPoster if self.box == "poster" else 0)
		if self.fpv:
			flags |= GuestFlags.FirstPerson
		if self.aim_throw and self.ready():
			flags |= GuestFlags.ThrowAim
			hand, vel = self.release(*self.aim_throw[:2])
			gs.aimThrowPos[:] = hand
			gs.aimThrowVel[:] = vel
			gs.aimThrowKind = int(self.aim_throw[2])
		gs.flags = int(flags)
		gs.frame = self.frame
		gs.qpc = proto.qpc_now()
		gs.missionCode, gs.locationCode = 50050, 50  # Mother Base free roam, fake
		gs.odoEpoch = self.odo_epoch
		gs.odo[:] = self.odo
		gs.rootPos[:] = self.root
		gs.rootYaw = self.yaw
		gs.velocity[:] = [vx, 0.0, vz]
		gs.camPos[:] = cam
		gs.camQuat[:] = quat_look(look)
		gs.camFovY = math.radians(60)
		gs.camNear = 0.1
		gs.life, gs.lifeMax, gs.stamina, gs.stance = self.life, LIFE_MAX, 100, 0
		gs.weaponId, gs.ammoInClip, gs.ammoStock = WEAPON_ID, self.ammo, self.stock
		gs.buddyType = proto.BuddyType.WALKER_GEAR
		gs.vehicleId = DWALKER_VEHICLE_ID if riding else 0xFFFF
		gs.phase = 0
		# v2 buddy block: D-Walker beside Snake, or under him while ridden.
		bflags = proto.GuestBuddyFlags.Present | proto.GuestBuddyFlags.PosValid
		if riding:
			bflags |= proto.GuestBuddyFlags.Ridden
		off = (0.0, 0.0) if riding else (1.5 * math.cos(self.yaw) - 0.5 * math.sin(self.yaw), -1.5 * math.sin(self.yaw) - 0.5 * math.cos(self.yaw))
		gs.buddyFlags = int(bflags)
		gs.buddyObjectId = DWALKER_VEHICLE_ID
		gs.buddyAmmo = self.buddy_ammo
		gs.buddyPos[:] = [self.root[0] + off[0], 0.0, self.root[2] + off[1]]
		gs.buddyYaw = self.yaw
		link.seq_write(proto.OFF_GUEST_STATE, gs)
		self.vars.update(playerPosX=self.root[0], playerPosY=self.root[1], playerPosZ=self.root[2],
			playerRotY=int(math.degrees(self.yaw)), playerLife=self.life, playerLifeMax=LIFE_MAX,
			buddyType=int(proto.BuddyType.WALKER_GEAR), playerVehicleGameObjectId=gs.vehicleId,
			missionCode=gs.missionCode, locationCode=gs.locationCode, frame=self.frame)

		self.publish_pose()

		if not self.autofire:
			self.next_fire, self.next_buddy_fire = t + 2.0, t + 3.0
		if t >= self.next_fire:
			self.next_fire += 2.0
			fwd = (math.sin(self.yaw), 0.0, math.cos(self.yaw))
			self.push_event(GuestEventType.Fire, a=WEAPON_ID | (proto.AmmoKind.Lethal << 16), b=1,
				v0=(self.root[0], 1.4, self.root[2]), v1=fwd)
			st["fireSent"] += 1
			self.spend_round()
		if t >= self.next_buddy_fire:
			self.next_buddy_fire += 3.0
			self.buddy_ammo = max(0, self.buddy_ammo - 1)
			self.push_event(GuestEventType.BuddyFire, a=0x0F00 | (proto.AmmoKind.Lethal << 16), b=1,
				v0=(self.root[0] + 1.5, 2.2, self.root[2]), v1=(math.sin(self.yaw), 0.0, math.cos(self.yaw)))
			st["buddyFireSent"] += 1

		self.tick_explosives(t, dt)
		if t < self.flame_until and t >= self.next_flame:
			# D-Walker's flamethrower as the real guest reports it (docs/GUEST.md 17): no rounds, ~8 events/s along the aim from
			# 1.5 m above the walker, pad[1] = the reach in mm.
			self.next_flame = t + 0.133
			fwd = (math.sin(self.yaw), -0.05, math.cos(self.yaw))
			self.push_event(GuestEventType.BuddyFire, a=4, b=1, v0=(self.root[0], 1.5 + (1.0 if riding else 0.0), self.root[2]), v1=fwd, pad0=0, pad1=12000)
			st["buddyFireSent"] += 1

		self.publish_draw_list(riding)
		if self.frame % 10 == 0:
			self.publish_hud()

		if proto.console_serve(link.guest_console, self.lua):
			st["consoleServed"] += 1

	def ready(self) -> bool:
		return not self.failed and self.elapsed() >= self.boot_until

	def spend_round(self):
		self.ammo -= 1
		if self.ammo <= 0:  # reload from the reserve
			take = min(30, self.stock)
			self.ammo, self.stock = (take, self.stock - take) if take else (30, 120)

	def lua(self, chunk: str) -> tuple[int, str]:
		if chunk.startswith("!"):  # the real guest serves '!' lines on its worker thread, without Lua
			cmd = chunk[1:].strip()
			if cmd == "status":
				st = self.stats
				return 0, (f"fake guest: frame {self.frame}, session {self.nonce:#018x}, buddy WALKER_GEAR ammo {self.buddy_ammo}, life {self.life} "
					f"(regen {self.regen:.0f}/s, healed {st['healed']}), menu {'open' if self.in_menu else 'closed'}, "
					f"pad: Back {st['backPresses']} Start {st['startPresses']} in-menu presses {st['menuButtons']}, events {st['hostEvents']}")
			return self.combat_command(cmd.split())
		try:
			return 0, luasubset.run_chunk(chunk, self.env)
		except luasubset.LuaError as e:
			return 1, str(e)

	def combat_command(self, w: list[str]) -> tuple[int, str]:
		usage = ("fake guest commands: !status !walk on|off !autofire on|off !fire <kind> [n] [yaw] [pitch] [share] !share <s> !move <speed> [yaw] "
			"!ride on|off|auto !box on|off|poster !flame [s] !fpv on|off !cqc <kind> !blast [r] [kind] !die "
			"!throw <frag|sleep|stun|smoke> [speed] [pitch] [yaw] !aimthrow on|off [speed] [pitch] [kind] !place <kind> [trigger_m] !detonate <id> "
			"!life <n> [regen] !menu on|off !aim on|off !hud mgsv|bar|none !boot N [text] !fail [text] !ready")
		if not w:
			return 0, usage
		if w[0] == "menu" and len(w) > 1:
			self.in_menu = w[1] == "on"
			return 0, f"MGSV menu {'open' if self.in_menu else 'closed'} (kGuestInMenu)"
		if w[0] == "life" and len(w) > 1:
			self.life = max(1, min(LIFE_MAX, int(w[1])))
			if len(w) > 2:
				self.regen = max(0.0, float(w[2]))
			return 0, f"life {self.life}/{LIFE_MAX}, regeneration {self.regen:.0f}/s"
		if w[0] == "aim" and len(w) > 1:
			self.aiming = w[1] == "on"
			return 0, f"aiming {'on' if self.aiming else 'off'}"
		if w[0] == "hud" and len(w) > 1 and w[1] in ("mgsv", "bar", "none"):
			self.hud_mode = w[1]
			return 0, f"HUD layer: {self.hud_mode}"
		if w[0] == "boot":
			secs = float(w[1]) if len(w) > 1 else 20.0
			self.boot_until = self.elapsed() + secs
			self.boot_text = " ".join(w[2:]) or "MGSV: loading Africa free roam (30020)"
			self.failed = ""
			return 0, f"booting for {secs:.0f} s: {self.boot_text}"
		if w[0] == "fail":
			self.failed = " ".join(w[1:]) or "MGSV: boot failed: fake failure (see PhantomRingGuest.log)"
			return 0, f"failed: {self.failed}"
		if w[0] == "ready":
			self.failed, self.boot_until = "", 0.0
			return 0, "ready"
		fwd = (math.sin(self.yaw), 0.0, math.cos(self.yaw))
		chest = (self.root[0], 1.4, self.root[2])
		if w[0] == "share" and len(w) > 1:
			self.share = max(0.0, float(w[1]))
			return 0, f"damage share {self.share:.3f} (pad[1] {int(round(self.share * 10000))}) with every !fire"
		if w[0] == "move" and len(w) > 1:
			speed = float(w[1])
			self.move = (speed, math.radians(float(w[2]) if len(w) > 2 else math.degrees(self.yaw))) if speed > 0 else None
			return 0, f"move {speed} m/s" + (f" along MGSV yaw {math.degrees(self.move[1]):.1f} deg" if self.move else " (stopped)")
		if w[0] == "ride" and len(w) > 1 and w[1] in ("on", "off", "auto"):
			self.ride = w[1]
			return 0, f"ride {self.ride}"
		if w[0] == "box" and len(w) > 1 and w[1] in ("on", "off", "poster"):
			self.box = "" if w[1] == "off" else w[1]
			return 0, f"box {w[1]} (kGuestInBox{' + kGuestBoxPoster' if self.box == 'poster' else ''})"
		if w[0] == "cam" and len(w) > 1:
			# Point the fake camera (no orbit): from Snake's root along -(cos phase, sin phase) in MGSV x/z.
			self.orbit_speed = 0.0
			self.orbit_phase = float(w[1])
			if len(w) > 2:
				self.cam_dist = float(w[2])
			if len(w) > 3:
				self.cam_height = float(w[3])
			if len(w) > 4:
				self.cam_look_y = float(w[4])
			return 0, f"camera phase {self.orbit_phase:.2f} rad, {self.cam_dist} m back, {self.cam_height} m up, looking at {self.cam_look_y} m"
		if w[0] == "fpv" and len(w) > 1:
			self.fpv = w[1] == "on"
			return 0, f"first person {'on' if self.fpv else 'off'}"
		if w[0] == "flame":
			secs = float(w[1]) if len(w) > 1 else 3.0
			self.flame_until = self.elapsed() + secs
			self.next_flame = self.elapsed()
			return 0, f"D-Walker flamethrower for {secs:.1f} s"
		if w[0] in ("walk", "autofire") and len(w) > 1:
			setattr(self, w[0], w[1] == "on")
			return 0, f"{w[0]} {'on' if getattr(self, w[0]) else 'off'}"
		if w[0] == "fire":
			kind = w[1] if len(w) > 1 else "lethal"
			if kind not in FIRE_KINDS:
				return 1, f"kinds: {' '.join(FIRE_KINDS)}"
			bullet, ammo, pellets, explosive = FIRE_KINDS[kind]
			n = int(w[2]) if len(w) > 2 else 1
			yaw = self.yaw + math.radians(float(w[3]) if len(w) > 3 else 0.0)
			pitch = math.radians(float(w[4]) if len(w) > 4 else 0.0)
			d = (math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch))
			share = float(w[5]) if len(w) > 5 else self.share
			for _ in range(max(1, min(n, 50))):
				self.push_event(GuestEventType.Fire, a=WEAPON_ID | (int(ammo) << 16), b=pellets, v0=chest, v1=d,
					pad0=bullet | (0x80000000 if explosive else 0), pad1=int(round(share * 10000)))
				self.stats["fireSent"] += 1
				self.spend_round()
			return 0, f"fired {n} x {kind} (bullet {bullet}, ammo {ammo.name}, pellets {pellets}, share {share:.3f}) dir ({d[0]:.2f}, {d[1]:.2f}, {d[2]:.2f})"
		if w[0] == "cqc":
			kinds = {k.name.lower(): k for k in proto.MeleeKind}
			kind = kinds.get(w[1] if len(w) > 1 else "punch")
			if kind is None:
				return 1, f"kinds: {' '.join(kinds)}"
			centre = (self.root[0] + fwd[0], 1.1, self.root[2] + fwd[2])
			self.push_event(GuestEventType.Melee, a=int(kind), b=900, v0=centre, v1=fwd, pad0=7 if kind == proto.MeleeKind.Kick else 1)
			return 0, f"cqc {kind.name} at ({centre[0]:.2f}, {centre[1]:.2f}, {centre[2]:.2f})"
		if w[0] == "blast":
			radius = float(w[1]) if len(w) > 1 else 4.0
			kind = BLAST_KINDS.get(w[2] if len(w) > 2 else "lethal", proto.AmmoKind.Lethal)
			centre = (self.root[0] + 4 * fwd[0], 0.5, self.root[2] + 4 * fwd[2])
			self.push_event(GuestEventType.Explosion, a=int(kind), b=int(radius * 1000), v0=centre, v1=(0, 1, 0))
			return 0, f"explosion {kind.name} r {radius} m at ({centre[0]:.2f}, {centre[1]:.2f}, {centre[2]:.2f})"
		if w[0] == "throw":
			kind = w[1] if len(w) > 1 else "frag"
			if kind not in THROW_KINDS:
				return 1, f"kinds: {' '.join(THROW_KINDS)}"
			speed = float(w[2]) if len(w) > 2 else 12.0
			pitch = math.radians(float(w[3]) if len(w) > 3 else 30.0)
			yaw = math.radians(float(w[4]) if len(w) > 4 else 0.0)
			return 0, self.throw(kind, speed, pitch, yaw)
		if w[0] == "aimthrow" and len(w) > 1:
			if w[1] != "on":
				self.aim_throw = None
				return 0, "aimthrow off"
			speed = float(w[2]) if len(w) > 2 else 12.0
			pitch = math.radians(float(w[3]) if len(w) > 3 else 30.0)
			kind = THROW_KINDS.get(w[4] if len(w) > 4 else "frag", THROW_KINDS["frag"])[0]
			self.aim_throw = (speed, pitch, kind)
			return 0, f"aimthrow on: {speed} m/s at {math.degrees(pitch):.0f} deg, kind {kind.name}"
		if w[0] == "place":
			kind = w[1] if len(w) > 1 else "mine"
			if kind not in PLACE_KINDS:
				return 1, f"kinds: {' '.join(PLACE_KINDS)}"
			ammo, equip, trigger, radius = PLACE_KINDS[kind]
			if len(w) > 2:
				trigger = float(w[2])
			pid = self.next_id
			self.next_id += 1
			pos = (self.root[0] + fwd[0], 0.0, self.root[2] + fwd[2])
			self.mines[pid] = {"kind": ammo, "pos": pos, "radius": radius, "name": kind}
			self.push_event(GuestEventType.Placed, a=int(ammo), b=pid, v0=pos, v1=fwd, pad0=equip, pad1=int(trigger * 1000))
			return 0, f"placed {kind} id {pid} (trigger {trigger} m) at ({pos[0]:.2f}, 0, {pos[2]:.2f})"
		if w[0] == "detonate" and len(w) > 1:
			return 0, self.set_off(int(w[1]), "Snake's detonator")
		if w[0] == "die":
			self.life = 1
			self.push_event(GuestEventType.Died)
			self.stats["diedSent"] += 1
			return 0, "Snake died (life 1, kGEvDied sent)"
		return 1, usage

	def release(self, speed: float, pitch: float, yaw_off: float = 0.0):
		"""Snake's hand and a throw's initial velocity (MGSV world) for a speed, pitch and yaw off his facing."""
		yaw = self.yaw + yaw_off
		fwd = (math.sin(yaw), 0.0, math.cos(yaw))
		hand = (self.root[0] + 0.3 * fwd[0], 1.6, self.root[2] + 0.3 * fwd[2])
		vel = (fwd[0] * speed * math.cos(pitch), speed * math.sin(pitch), fwd[2] * speed * math.cos(pitch))
		return hand, vel

	def throw(self, kind: str, speed: float, pitch: float, yaw: float) -> str:
		ammo, equip, fuse_ms, radius, cloud_s = THROW_KINDS[kind]
		tid = self.next_id
		self.next_id += 1
		hand, vel = self.release(speed, pitch, yaw)
		self.grenades.append({"id": tid, "kind": ammo, "pos": list(hand), "vel": list(vel), "t": 0.0, "fuse": fuse_ms / 1000.0,
			"landed": False, "radius": radius, "cloud": cloud_s, "name": kind})
		self.push_event(GuestEventType.Throw, a=int(ammo), b=tid, v0=hand, v1=vel, pad0=equip | (fuse_ms << 16))
		return f"threw {kind} id {tid} at {speed} m/s ({'fuse %d ms' % fuse_ms if fuse_ms else 'impact'})"

	def blast(self, oid: int, kind, pos, radius: float, cloud_s: float):
		self.push_event(GuestEventType.Explosion, a=int(kind), b=int(radius * 1000), v0=pos, v1=(0, 1, 0), pad0=180 | (0x4B << 16) | (5 << 24), pad1=oid)
		self.log(f"MGSV blast of {oid}: {kind.name} r {radius} m at ({pos[0]:.2f}, {pos[1]:.2f}, {pos[2]:.2f})")
		if cloud_s > 0:
			self.clouds.append({"id": oid, "kind": kind, "pos": tuple(pos), "radius": radius, "until": self.elapsed() + cloud_s, "next": self.elapsed() + 0.5})

	def set_off(self, pid: int, why: str) -> str:
		m = self.mines.pop(pid, None)
		if m is None:
			return f"no placed explosive {pid}"
		self.blast(pid, m["kind"], m["pos"], m["radius"], 12.0 if m["kind"] == proto.AmmoKind.Tranq else 0.0)
		self.push_event(GuestEventType.PlacedGone, b=pid)
		return f"{m['name']} {pid} set off ({why})"

	def tick_explosives(self, t: float, dt: float):
		"""MGSV's own flight of the fake's grenades (flat ground at y = 0), fuses and lingering clouds."""
		for g in list(self.grenades):
			g["t"] += dt
			if not g["landed"]:
				for i in range(3):
					g["pos"][i] += g["vel"][i] * dt
				g["vel"][1] -= 9.8 * dt
				if g["pos"][1] <= 0.0:
					g["pos"][1] = 0.0
					g["vel"] = [0.0, 0.0, 0.0]
					g["landed"] = True
			if (g["fuse"] <= 0 and g["landed"]) or (g["fuse"] > 0 and g["t"] >= g["fuse"]):
				self.grenades.remove(g)
				self.blast(g["id"], g["kind"], tuple(g["pos"]), g["radius"], g["cloud"])
		for c in list(self.clouds):
			if t >= c["until"]:
				self.clouds.remove(c)
			elif t >= c["next"]:
				c["next"] += 0.5
				self.push_event(GuestEventType.Explosion, a=int(c["kind"]), b=int(c["radius"] * 1000), v0=c["pos"], v1=(0, 1, 0), pad0=286, pad1=c["id"])

	def push_event(self, typ, a=0, b=0, v0=(0, 0, 0), v1=(0, 0, 0), pad0=0, pad1=0):
		ev = proto.GuestEvent(type=int(typ), a=int(a), b=int(b), frame=self.frame & 0xFFFFFFFF)
		ev.v0[:] = list(v0)
		ev.v1[:] = list(v1)
		ev.pad[0] = int(pad0) & 0xFFFFFFFF
		ev.pad[1] = int(pad1) & 0xFFFFFFFF
		self.link.guest_events.push(ev)

	def publish_pose(self):
		if self.humanoid:
			return self.publish_humanoid_pose()
		pb = proto.PoseBlock()
		pb.boneCount = BONES
		pb.frame = self.frame
		pb.root[:] = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, self.root[0], self.root[1], self.root[2], 1]
		phase = self.elapsed() * 4
		for i in range(BONES):
			pb.parent[i] = i - 1 if i else -1
			b = pb.bones[i]
			b.rot[:] = [0.0, 0.0, 0.0, 1.0]
			b.pos[:] = [0.05 * math.sin(phase + i * 0.1), 0.01 * i, 0.0, 0.0]
		for i in range(BONES, proto.MAX_POSE_BONES):
			pb.parent[i] = -1
		self.link.seq_write(proto.OFF_POSE, pb)

	def anim_time(self) -> float:
		return self.freeze if self.freeze >= 0 else self.elapsed()

	def publish_humanoid_pose(self):
		"""The test model's own skeleton (same bones the mesh is skinned to), so both must coincide in ER."""
		model = testmodel.pose(self.anim_time())
		self.last_model = model
		c, s_ = math.cos(self.yaw), math.sin(self.yaw)
		pb = proto.PoseBlock()
		pb.boneCount = len(testmodel.BONES)
		pb.frame = self.frame
		# model -> MGSV world, row-major, row-vector convention (p * M): rot_y(yaw) transposed + root
		pb.root[:] = [c, 0, -s_, 0, 0, 1, 0, 0, s_, 0, c, 0, self.root[0], self.root[1], self.root[2], 1]
		for i, (_, _, parent) in enumerate(testmodel.BONES):
			pb.parent[i] = parent
			b = pb.bones[i]
			b.rot[:] = list(testmodel.quat_of(model[i]))
			b.pos[:] = [model[i][0][3], model[i][1][3], model[i][2][3], 0.0]
		for i in range(len(testmodel.BONES), proto.MAX_POSE_BONES):
			pb.parent[i] = -1
		self.link.seq_write(proto.OFF_POSE, pb)

	def publish_draw_list(self, riding: bool):
		if self.humanoid:
			walker = (0.0, -1.0, 0.0) if riding else (1.6 * math.cos(self.yaw) - 0.4 * math.sin(self.yaw), 0.0, -1.6 * math.sin(self.yaw) - 0.4 * math.cos(self.yaw))
			draws, mats, _ = self.humanoid.draw(self.anim_time(), self.yaw, walker)
			payload = proto.build_draw_list(self.frame, self.root, self.yaw, draws, mats)
			self.link.write_draw_slot(self.draw.back, self.frame, payload)
			self.draw.publish()
			self.link.draw_ctl.framesPublished += 1
			self.stats["drawFramesPublished"] += 1
			return
		rec_snake = proto.DrawRecord(mesh=MESH_SNAKE, indexCount=3, startIndex=0, baseVertex=0, firstMatrix=0, matrixCount=2,
			flags=0, owner=proto.DrawOwner.Player, material=1)
		rec_snake.textures[0] = TEX_SNAKE
		rec_walker = proto.DrawRecord(mesh=MESH_DWALKER, indexCount=3, startIndex=0, baseVertex=0, firstMatrix=2, matrixCount=2,
			flags=0, owner=proto.DrawOwner.Buddy, material=2)
		rec_walker.textures[0] = TEX_DWALKER
		# Anchor-relative matrices: Snake at the anchor; D-Walker under him while ridden, else beside him.
		walker = (0.0, -0.8, 0.0) if riding else (1.5, 0.0, -0.5)
		mats = [matrix_3x4(0, 0, 0, self.yaw), matrix_3x4(0, 0.9, 0, self.yaw),
			matrix_3x4(*walker, self.yaw), matrix_3x4(walker[0], walker[1] + 1.0, walker[2], self.yaw)]
		payload = proto.build_draw_list(self.frame, self.root, self.yaw, [rec_snake, rec_walker], mats)
		self.link.write_draw_slot(self.draw.back, self.frame, payload)
		self.draw.publish()
		self.link.draw_ctl.framesPublished += 1
		self.stats["drawFramesPublished"] += 1

	def publish_hud(self):
		if self.hud_mode == "none":
			return
		if self.hud_mode == "mgsv":
			key = (self.ammo, self.stock, self.aiming)
			if self.hud_cache[0] != key:  # drawing it costs ~10 ms in Python: only when it changes
				self.hud_cache = (key, fakehud.mgsv_hud(self.ammo, self.stock, self.aiming))
			w, h, pixels = fakehud.W, fakehud.H, self.hud_cache[1]
		else:
			w, h, pixels = fakehud.life_bar(self.life, LIFE_MAX)
		self.link.write_hud_slot(self.hud.back, w, h, self.frame, pixels)
		self.hud.publish()
		self.link.hud_ctl.framesPublished += 1
		self.stats["hudFramesPublished"] += 1


def main(argv=None) -> int:
	ap = argparse.ArgumentParser(prog="fake_mgsv", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--path", default=None, help="mapping file (default $PHANTOMRING_SHM or /dev/shm/phantomring_v1)")
	ap.add_argument("--seconds", type=float, default=0, help="run time (0 = until Ctrl-C)")
	ap.add_argument("--create", action="store_true", help="create the mapping if it does not exist")
	ap.add_argument("--summary", help="write a JSON summary here on exit")
	ap.add_argument("--quiet", action="store_true")
	ap.add_argument("--model", choices=("triangle", "humanoid"), default="triangle", help="what to send on the render ring (default: triangle)")
	ap.add_argument("--stand", action="store_true", help="stand still (walk animation in place) at a fixed facing")
	ap.add_argument("--hud", choices=("mgsv", "bar", "none"), default="", help="HUD layer: mgsv (1280x720, default with --model humanoid), bar (64x8), none")
	ap.add_argument("--boot", type=float, default=0.0, help="stay 'booting' (not ready, status text shown in ER) this many seconds after attaching")
	ap.add_argument("--orbit", type=float, default=0.5, help="camera orbit speed around Snake (rad/s; 0 = fixed)")
	ap.add_argument("--orbit-phase", type=float, default=0.0, help="camera orbit angle at start (rad)")
	ap.add_argument("--freeze", type=float, default=-1.0, help="humanoid: hold the animation at this time (s)")
	ap.add_argument("--cam", type=float, nargs=3, metavar=("DIST", "HEIGHT", "LOOK_Y"), default=(6.0, 1.8, 1.0),
		help="fake camera: distance from Snake, height, height of the point it looks at (m)")
	ap.add_argument("--force", action="store_true", help="take the guest side over even if another live process holds it")
	args = ap.parse_args(argv)

	path = args.path or proto.default_path()
	if args.create and not os.path.exists(path):
		proto.create_mapping(path)
	try:
		link = proto.Link(path)
	except proto.MappingMissing as e:
		print(f"fake_mgsv: no mapping ({e}); start the host first or pass --create", file=sys.stderr)
		return 2
	if link.refuse_if_taken("guest", "fake_mgsv", path, args.force):
		return 4
	guest = FakeGuest(link, args.quiet, args.model, args.stand, args.hud)
	if args.boot > 0:
		guest.boot_until, guest.boot_text = args.boot, "MGSV: loading Africa free roam (30020)"
	guest.orbit_speed, guest.orbit_phase, guest.freeze = args.orbit, args.orbit_phase, args.freeze
	guest.cam_dist, guest.cam_height, guest.cam_look_y = args.cam
	h = link.header
	h.guestNonce = 0  # a new guest process: the host must treat whatever was attached before as gone
	h.guestUnixPid = os.getpid()
	guest.status(Status.Booting, "fake MGSV: waiting for the host")
	stop = False

	def on_signal(*_):
		nonlocal stop
		stop = True

	signal.signal(signal.SIGTERM, on_signal)
	signal.signal(signal.SIGINT, on_signal)
	guest.log(f"mapping {path}; waiting for a host session")
	dt = 1.0 / FPS
	next_t = time.monotonic()
	last_report = 0.0
	while not stop and (args.seconds <= 0 or guest.elapsed() < args.seconds):
		guest.tick(dt)
		if guest.elapsed() - last_report >= 1.0 and guest.attached:
			last_report = guest.elapsed()
			st = guest.stats
			guest.log(f"frame {guest.frame} odo ({guest.odo[0]:.2f}, {guest.odo[2]:.2f}) life {guest.life} pad packets {st['padPackets']} "
				f"hurt {st['hostEvents'].get('Hurt', 0)} actors {st['actorsSeen']} console {st['consoleServed']}")
		next_t += dt
		delay = next_t - time.monotonic()
		if delay > 0:
			time.sleep(delay)
		else:
			next_t = time.monotonic()
	guest.status(Status.Exiting, "fake MGSV: exiting")
	if args.summary:
		with open(args.summary, "w") as f:
			json.dump(guest.stats | {"odo": guest.odo, "nonce": guest.nonce, "odoEpoch": guest.odo_epoch}, f, indent=2)
	return 0


if __name__ == "__main__":
	sys.exit(main())
