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
    !fire <kind> [n] [yaw_deg] [pitch_deg] [share] [suppressed|loud]   kind: lethal|handgun|rifle|sniper|buck|tranq|stun|explosive|
                        rocket|grenade; (v16) suppressed: Fire b carries FIRE_SUPPRESSED (a shot through a working suppressor);
                        without either word a shot is suppressed while !suppressor is on (and wears it)
    !suppressor on [shots]|off   (v16) GuestFlags.SuppressorOn: the weapon in hand has a working suppressor, worn out after that
                        many suppressed shots (default 4; 0 = never); the flag drops right after the shot that wears it out
    !cqc <punch|kick|throw|choke>            a strike 1 m in front of Snake
    !grab <grab|hold|throw|choke|interrogate|release>   (v13) kGEvCqc: a CQC hold/throw as the guest reads it from MGSV's pad
                                             (Snake's feet and facing; the host picks the Elden Ring enemy in reach)
    !blast [radius_m]                        a kGEvExplosion 4 m in front of Snake
    !die                                     life 1 + kGEvDied (Elden Ring should run YOU DIED)
    !life <n> [regen]                        set Snake's life (of 6000) and the regeneration per second (default 300)
    !blast [radius_m] [lethal|sleep|stun|smoke]  a kGEvExplosion 4 m in front of Snake (no throw id)
    !share <s>                               (v8) MGSV's damage share sent with every !fire (pad[1] = s x 10000; 0 = unknown)
    !move <speed m/s> [yaw_deg]              walk/run at a constant speed along an absolute MGSV yaw (0 = +Z); !move 0 stops
    !ride on|off|auto                        D-Walker ridden / not / the old 4 s of every 12
    !idroid on|off                           a Start press opens the iDroid (kGuestInMenu) and one in it closes it, as MGSV
    !fulton [awake|any|0xHANDLE] [fail]      v12 kGEvFulton for the nearest sleeping hostile Elden Ring character of the ActorTable
                                             (kActorAsleep; awake: hostile, asleep or not; any: anyone; or that handle; fail: a = 0)
    !buddy walker|horse|dog|quiet|none       the buddy: D-Walker (BuddyType WALKER_GEAR) or D-Horse (HORSE: ridden, its gallop),
                                             D-Dog / Quiet (not ridden), none (dismissed: no buddy, its cube still drawn)
    !stick world|camera [m/s]                the left stick moves Snake along MGSV x/z (+3 m/s), or relative to the camera as
                                             MGSV does (up = away from it; full stick m/s on foot, default 5; ridden: the buddy's)
    !box on|off|poster                       (v8) kGuestInBox (+ kGuestBoxPoster): Snake hides in a cardboard box
    !flame [seconds]                         D-Walker's flamethrower: kGEvBuddyFire with no bullet (pad[1] = 12000 mm reach) ~8/s
    !fpv on|off                              kGuestFirstPerson
    !cam <phase_rad> [dist] [height] [look_y]   a fixed camera (no orbit) looking along -(cos phase, sin phase)
    !order <cmd>                             (v7) kGEvBuddyOrder: the player's call-menu command for the buddy out (b = its
                                             BuddyType); (v17) with D-Dog pad[0] = its style + 1 (!dog style); (v18) with Quiet
                                             (b = 3) pad[0] = her weapon + 1 for commands 1-3 (!quiet weapon)

D-Dog's attacks (protocol v17, docs/research/ddog.md; needs `!buddy dog`):
    !dog [status]                            the dog's style, kBuddyEngaged, the job under way, the host's pick (kActorDogTarget)
    !dog style <busy|fetch|kill|knockout|wound|unknown>   its equipment (the style the real guest reads from MGSV)
    !dog attack [target|nearest|none|0xHANDLE] [style] [full|ordered|connect|lost|refused]
                                             kGEvDogAttack on an Elden Ring character of the ActorTable: target = the host's pick
                                             (ActorFlags.DogTarget, the default), nearest = the nearest hostile living non-boss one
                                             (fetch: a sleeping one), none = no target (Refused, handle 0). full (default): Ordered,
                                             Connected 0.6 s later (GuestBuddyFlags.Engaged on), Released after the busy time (off);
                                             ordered: only Ordered; connect: Ordered + Connected, engaged until !dog release; lost:
                                             Ordered, Lost 0.5 s later; refused: Refused. b = the !share value, the attack id by
                                             style (bite 144, knife 145, stun knife 146)
    !dog release                             Released for the engaged target (Engaged off)
    !dog busy <seconds>                      Connected -> Released time of a full run (default 2)
    !dog blip <dx> <dz> [frames] [dy]        (0.8.0 polish) the dog's place and draws off by dx, dz m for N frames (default 1)
    !dog move <dx> <dz>                      ...moved for good (it waits there; !order 8 to follow again)
    !callmenu on|off [full|ordered|connect]  MGSV's call menu from the pad: hold LB, push the right stick, let it go, click R3 ->
                                             the dog's attack command; the host's pick taken at once or within 350 ms, else Refused
    !order 9 / !order 8                      with D-Dog: Wait keeps it where it is in Snake's odometer frame; the whistle ends it

Quiet and the grace rest (protocol v18, docs/research/quiet.md; needs `!buddy quiet`, object id 0x2400 as MGSV's):
    !quiet [status]                          her weapon, the bits she publishes, the post / seat she holds, the host's picks
    !quiet weapon <lethal|tranq|heavy|unknown>   her rifle (buddyFlags bits 17-19 = weapon + 1 every frame; unknown = 0)
    !quiet post on|off                       follow the host's post as the real guest's mirror does: while HostFlags.QuietLayer is
                                             set and buddyPostSeq != 0 she stands at buddyPost (facing buddyPostYaw) with
                                             GuestBuddyFlags.AtPost and echoes the seq in GuestState.buddyPostSeq; with no post
                                             (NaN: on the move) she is Cloaked
    !quiet seat on|off                       follow the host's grace seat: GuestBuddyFlags.CanRest while on; while HostFlags.AtGrace
                                             is set, graceSeq != 0 and graceSeat finite she sits there (graceSeatYaw) with
                                             GuestBuddyFlags.Seated and echoes graceSeq in GuestState.graceSeqSeated (before a post)
    !quiet cloak on|off                      GuestBuddyFlags.Cloaked (her stealth camo: MGSV doesn't draw her)
    !quiet cover on|off                      GuestBuddyFlags.QuietCover (MGSV's Cover me is on)
    !quiet marked on|off                     GuestBuddyFlags.QuietMarked (MGSV holds a mark on her stand-in: Fire / Take aim live)
    !quiet hit <seconds>                     Ordered -> Hit time of a full shot (default 1; live 3.1-4.3 s)
    !quiet shot [pick|threat|nearest|none|0xHANDLE] [full|ordered|hit|released|lost|refused] [mgsv|target|cancelled] [weapon]
                                             kGEvQuietShot on an Elden Ring character of the ActorTable: pick = the host's pick
                                             (ActorFlags.QuietTarget, the default), threat = ActorFlags.QuietThreat, nearest = the
                                             nearest hostile living non-boss one, none = no target (Refused, handle 0). full: Ordered
                                             (GuestBuddyFlags.Engaged on), Hit after the hit time, Released 0.3 s later (Engaged off);
                                             ordered / hit / released / refused: that one phase now; lost: Ordered, Lost 0.5 s later,
                                             b = the reason (mgsv, the default; target; cancelled). The attack id by weapon (290 /
                                             291 / 292), b = the !share value or MGSV's share of that rifle (0.9615 / 0.4615 / 1.4615)

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
    !nvg on|off         (v10) GuestFlags.Nvg: Snake's night-vision goggles (the host recreates them on Elden Ring's frame)
    !binoculars on|off  (v13) GuestFlags.Binoculars + FirstPerson: Snake looks through his binoculars (the host marks the
                        Elden Ring characters near the view centre, er/Marking.h)

Protocol v10 host events are logged: kHEvRested (a = 0 grace rest, 1 respawn) refills ammo (30+120) and life;
kHEvTarnished a = 1 parks Snake (no walking, no autofire) until a = 0.

Protocol v15 (Snake climbs ladders and ledges with MGSV's own moves): kHEvClimb a = 1 (b = 0 up / 1 down, v0[0] = the climb's
speed m/s; a = 1 again = it turned or changed pace) makes the fake "play the ladder climb" until a = 0: GuestFlags.Climbing, no
walking (the odometer stands still), and a climb pose (triangle: Snake's upper matrix swings with the climb's pace; humanoid: its
walk cycle at the climb's pace, still at speed 0). kHEvMantle (a = height cm, b = 0 climb up / 1 vault, v0[0] = duration s) sets
GuestFlags.Mantling for that long, 50 ms after it (MGSV's own reaction), with no walking meanwhile. Both are logged.
    !climbplay on|off   off: kHEvClimb is logged but no climb is played (no GuestFlags.Climbing: a guest that can't)
    !climbhome <ms>     after kHEvClimb a = 0 Snake stays on MGSV's ladder this long (GuestFlags.Climbing, no walking) before he
                        is home: the real guest takes him back from its airport ladder 0.18-0.23 s after the end (default 0)
    !mantleplay on|off  off: kHEvMantle is logged but no GuestFlags.Mantling (the host times the move out by itself)
    !projmesh on|off    (v13, --model humanoid) each grenade in flight is drawn as a rigid kOwnerProjectile draw of a 9 cm grenade
                        stand-in, as the real guest tags MGSV's own (the host learns it and draws it on its flown copy); default on

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
HORSE_OBJECT_ID = 0x0403  # arbitrary fake GameObjectId of D-Horse
# The buddy the fake runs (--buddy / !buddy): BuddyType, object id, riding speed factor on the circle walk, stick speed (m/s)
# while ridden, status text.
BUDDIES = {
	"walker": (proto.BuddyType.WALKER_GEAR, DWALKER_VEHICLE_ID, 2.5, 8.0, "D-Walker deployed"),
	"horse": (proto.BuddyType.HORSE, HORSE_OBJECT_ID, 4.0, 13.0, "D-Horse deployed"),
	# Not rideable: riding (--ride / !ride) is ignored with them. "none" = dismissed: GuestState says no buddy, while the fake
	# keeps drawing its cube (as MGSV may draw a buddy leaving): the host must leave those draws out.
	"dog": (proto.BuddyType.DOG, 0x0404, 1.0, 5.0, "D-Dog deployed"),
	"quiet": (proto.BuddyType.QUIET, 0x2400, 1.0, 5.0, "Quiet deployed"),  # v18: MGSV's own GameObjectId of TppBuddyQuiet2 (quiet.md §2.1)
	"none": (proto.BuddyType.NONE, 0xFFFF, 1.0, 5.0, "no buddy"),
}
# v17 D-Dog: !dog style words -> DogStyle, and the MGSV attack id each one's kGEvDogAttack carries (TppDamage ATK_DDogBite 144,
# ATK_DDogKnife 145, ATK_DDogStunKnife 146; docs/research/ddog.md §2.5; fetch: none)
DOG_STYLES = {"busy": proto.DogStyle.Busy, "fetch": proto.DogStyle.Fetch, "kill": proto.DogStyle.Kill, "knockout": proto.DogStyle.KnockOut,
	"wound": proto.DogStyle.Wound, "unknown": proto.DogStyle.StyleUnknown}
# As the real guest (mgsv/DogLogic AttackIdOf, Dog.cpp ShareOf with MGSV's ATK rows, live 2026-11-03): busy = the bite 144, kill = the
# knife 145, knock out = the stun knife 146, wound = the body attack 147; MGSV's lethal share of each in soldier lives (b / 10000):
# the bite 1.1538, the knife 25.2058, the two non-lethal ones 0. `!share` overrides the share for every event.
DOG_ATTACK_IDS = {proto.DogStyle.Busy: 144, proto.DogStyle.Kill: 145, proto.DogStyle.KnockOut: 146, proto.DogStyle.Wound: 147}
DOG_SHARES = {proto.DogStyle.Busy: 1.1538, proto.DogStyle.Kill: 25.2058, proto.DogStyle.KnockOut: 0.0, proto.DogStyle.Wound: 0.0}
# v18 Quiet: !quiet weapon words -> QuietWeapon, the MGSV attack id of her hit (ATK_Quiet_sr_010/020/030) and MGSV's share of it
# in soldier lives (kDamageParams: 2500 lethal, 1200 non-lethal, 3800 lethal of a 2600-life soldier; docs/research/quiet.md §2.6).
QUIET_WEAPONS = {"lethal": proto.QuietWeapon.Lethal, "tranq": proto.QuietWeapon.Tranq, "heavy": proto.QuietWeapon.Heavy,
	"unknown": proto.QuietWeapon.WeaponUnknown}
QUIET_ATTACK_IDS = {proto.QuietWeapon.Lethal: 290, proto.QuietWeapon.Tranq: 291, proto.QuietWeapon.Heavy: 292}
QUIET_SHARES = {proto.QuietWeapon.Lethal: 0.9615, proto.QuietWeapon.Tranq: 0.4615, proto.QuietWeapon.Heavy: 1.4615}
QUIET_LOST = {"mgsv": proto.QuietLostReason.LostMgsv, "target": proto.QuietLostReason.LostTarget, "cancelled": proto.QuietLostReason.LostCancelled}
RIDEABLE = ("walker", "horse")
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



# Every button MGSV's pad carried a press of (`!status`: "presses X 2 down 1"): what the host kept from MGSV shows by its absence
# (the quick item's View + X / View + D-pad down never reach MGSV, docs/HOST.md "Flasks and the quick item").
PAD_BUTTON_NAMES = ((0x0001, "up"), (0x0002, "down"), (0x0004, "left"), (0x0008, "right"), (0x0010, "start"), (0x0020, "back"),
	(0x0040, "l3"), (0x0080, "r3"), (0x0100, "lb"), (0x0200, "rb"), (0x1000, "a"), (0x2000, "b"), (0x4000, "x"), (0x8000, "y"))

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
		self.idroid = False     # --idroid / !idroid: a Start press opens (and in it closes) the iDroid, as MGSV
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
			"rideFrames": 0, "lifeMin": LIFE_MAX, "healed": 0, "backPresses": 0, "startPresses": 0, "menuButtons": 0, "pressCounts": {},
			"buddyOrdersSent": 0, "dogAttacksSent": [], "dogTargetFrames": 0, "dogTargetsSeen": [], "dogEngagedFrames": 0,
			"quietShotsSent": [], "quietTargetFrames": 0, "quietTargetsSeen": [], "quietThreatsSeen": [], "quietEngagedFrames": 0,
			"quietPostFrames": 0, "quietSeatFrames": 0, "quietCloakFrames": 0, "quietPostSeqs": [], "quietGraceSeqs": [],
		}
		self.last_packet = None
		self.grenades = []        # thrown, flying over the fake's flat ground (v5)
		self.proj_meshes = True   # v13: (--model humanoid) each grenade in flight drawn as a kOwnerProjectile draw (`!projmesh on|off`)
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
		self.nvg = False          # v10: !nvg on|off -> GuestFlags.Nvg
		self.binoculars = False   # v13: !binoculars on|off -> GuestFlags.Binoculars
		self.suppressor = None    # v16: !suppressor on [shots] -> shots left before it wears out (0 = never); None = no suppressor
		self.parked = False       # v10: kHEvTarnished a=1 -> Snake parks (no walking, no autofire) until a=0
		self.climb = None         # v15: kHEvClimb a=1 -> {"down", "speed", "since", "phase"} until a=0 (GuestFlags.Climbing)
		self.mantle = None        # v15: kHEvMantle -> {"vault", "height", "start", "end"} (GuestFlags.Mantling from start to end)
		self.climb_play = True    # !climbplay on|off
		self.climb_home_ms = 0    # !climbhome <ms>: still staged this long after kHEvClimb a = 0
		self.climb_home_until = 0.0
		self.mantle_play = True   # !mantleplay on|off
		self.climbing = self.mantling = False  # this frame: GuestFlags.Climbing / Mantling
		self.buddy = "walker"     # --buddy / !buddy walker|horse
		self.stick_mode = "world"  # !stick world|camera: the left stick along MGSV x/z (+3 m/s), or relative to the camera like MGSV
		self.stick_speed = 5.0     # !stick camera <m/s>: full stick on foot (MGSV's run ~4.7 m/s)
		self.last_cam = None
		# v17 D-Dog (!dog): its style (equipment), the attack job under way, kBuddyEngaged, the host's pick last seen
		self.dog_style = proto.DogStyle.Busy
		self.dog_job = None       # {"handle", "style", "pos", "steps": [(t, DogPhase), ...]}
		self.dog_engaged = None   # handle of the target while GuestBuddyFlags.Engaged
		self.dog_engaged_style = proto.DogStyle.Busy
		self.dog_busy_s = 2.0
		self.dog_target = 0       # ActorFlags.DogTarget handle in the last ActorTable (0 = none)
		self.buddy_pos = None
		# (0.8.0 polish) test hooks: MGSV's call menu from the pad as the real guest takes an attack order (!callmenu), Wait (the dog keeps
		# its place in Snake's odometer frame, as the real guest's treadmill keeps it), one-frame jumps of the dog (!dog blip)
		self.callmenu = False     # !callmenu on|off
		self.cm = {"lb": False, "pointed": False, "let_go": False, "pending": None}
		self.dog_wait = None      # (x, z) in the odometer frame while D-Dog waits
		self.dog_blip = None      # {"dx", "dy", "dz", "frames"}
		self.buddy_rel = (1.5, 0.0, -0.5)  # the buddy relative to Snake's root this frame (MGSV world axes)
		# v18 Quiet (!quiet): her weapon, the host's post / grace seat followed and echoed, her bits, the shot job, the host's picks
		self.quiet_weapon = proto.QuietWeapon.Lethal
		self.quiet_post = False   # !quiet post on: held at the host's post (AtPost + buddyPostSeq echo)
		self.quiet_seat = False   # !quiet seat on: CanRest; seated at the host's grace seat (Seated + graceSeqSeated echo)
		self.quiet_cloak = False
		self.quiet_cover = False
		self.quiet_marked = False
		self.quiet_hit_s = 1.0
		self.quiet_job = None     # {"handle", "weapon", "pos", "steps": [(t, QuietPhase, b), ...]}
		self.quiet_engaged = None  # handle while GuestBuddyFlags.Engaged (Ordered .. Released)
		self.quiet_target = 0     # ActorFlags.QuietTarget handle in the last ActorTable (0 = none)
		self.quiet_threat = 0     # ActorFlags.QuietThreat handle
		self.quiet_echo = (0, 0)  # (buddyPostSeq, graceSeqSeated) last published, logged as it changes
		self.host_last = None

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
		self.status(Status.Ready, f"fake MGSV: in the sandbox, {BUDDIES[self.buddy][4]}")
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
			self.host_last = host
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
				for bit, name in PAD_BUTTON_NAMES:
					if pressed & bit:
						st["pressCounts"][name] = st["pressCounts"].get(name, 0) + 1
				if pressed & 0x0020:  # Back/View: MGSV opens its iDroid with it
					st["backPresses"] += 1
					self.log("pad: Back pressed (MGSV would open its iDroid)")
				if pressed & 0x0010:
					st["startPresses"] += 1
					self.log("pad: Start pressed (MGSV would pause)")
					if self.idroid:
						# As MGSV: Start opens its iDroid at once (live: 34 ms after the host's iDroid hold), Start in it closes it.
						self.in_menu = not self.in_menu
						st["idroidToggles"] = st.get("idroidToggles", 0) + 1
						self.log(f"pad: Start -> iDroid {'OPEN' if self.in_menu else 'closed'} (kGuestInMenu)")
						pressed &= ~0x0010
				if self.in_menu and pressed:
					st["menuButtons"] += 1
					self.log(f"pad: {pressed:#06x} pressed in the menu")
			if self.callmenu and self.buddy == "dog" and not neutral:
				self.call_menu(t, pad)
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
			elif ev.type == HostEventType.Rested:
				# v10: Elden Ring refilled the flasks (a = 0 grace rest, 1 respawn): the guest refills ammo and life.
				before = (self.ammo, self.stock, self.life)
				self.ammo, self.stock, self.life = 30, 120, LIFE_MAX
				self.log(f"host: kHEvRested a={ev.a} ({'respawn' if ev.a else 'grace rest'}) -> ammo {before[0]}+{before[1]} -> 30+120, "
					f"life {before[2]} -> {self.life}")
			elif ev.type == HostEventType.Tarnished:
				# v10: Tarnished mode: Snake parks (the real guest: neutral pad, no follow whistles) until a = 0.
				self.parked = ev.a == 1
				self.log(f"host: kHEvTarnished a={ev.a} -> Snake {'parks (Tarnished mode)' if self.parked else 'is back'}")
			elif ev.type == HostEventType.Climb:
				# v15: Elden Ring climbs a ladder with the hidden Tarnished: MGSV plays Snake's own climb at its pace.
				speed = float(ev.v0[0])
				if ev.a == 1:
					again = self.climb is not None
					phase = self.climb["phase"] if again else 0.0
					self.climb = {"down": ev.b == 1, "speed": speed, "since": self.climb["since"] if again else t, "phase": phase}
					self.log(f"host: kHEvClimb a=1 b={ev.b} v={speed:.2f} m/s -> {'the climb goes on' if again else 'Snake climbs'} "
						f"{'down' if ev.b else 'up'}{'' if speed > 0.01 else ' (still on the ladder)'}"
						f"{'' if self.climb_play else ' -- not played (!climbplay off)'}")
				else:
					was = self.climb
					self.climb = None
					if was and self.climb_home_ms > 0 and self.climb_play:
						self.climb_home_until = t + self.climb_home_ms / 1000.0
					self.log(f"host: kHEvClimb a=0 -> off the ladder{(' after %.1f s' % (t - was['since'])) if was else ''}"
						f"{(', home in %d ms' % self.climb_home_ms) if was and self.climb_home_ms > 0 and self.climb_play else ''}")
			elif ev.type == HostEventType.Mantle:
				# v15: Snake climbs onto / vaults over an Elden Ring ledge: MGSV plays its matching move for v0[0] s.
				dur = max(0.1, float(ev.v0[0]))
				start = t + 0.05
				self.mantle = {"vault": ev.b == 1, "height": ev.a / 100.0, "start": start, "end": start + dur}
				self.log(f"host: kHEvMantle a={ev.a} ({ev.a / 100.0:.2f} m) b={ev.b} ({'vault over' if ev.b else 'climb up'}) v={dur:.2f} s"
					f"{'' if self.mantle_play else ' -- not played (!mantleplay off)'}")
		self.regen_acc += self.regen * dt  # MGSV-style regeneration
		self.life = min(LIFE_MAX, self.life + int(self.regen_acc))
		self.regen_acc -= int(self.regen_acc)
		st["lifeMin"] = min(st["lifeMin"], self.life)

		actors = link.actors()
		if actors:
			st["actorsSeen"] = actors.count
			# v17: the host's pick for D-Dog (at most one record), logged as it changes
			pick = next((actors.actors[i].handle for i in range(min(actors.count, proto.MAX_ACTORS))
				if actors.actors[i].flags & proto.ActorFlags.DogTarget), 0)
			if pick:
				st["dogTargetFrames"] += 1
				if hex(pick) not in st["dogTargetsSeen"]:
					st["dogTargetsSeen"].append(hex(pick))
			if pick != self.dog_target:
				self.log(f"dog: the host's pick (ActorFlags.DogTarget) {pick:#x}" if pick else "dog: the host has no pick (no ActorFlags.DogTarget)")
				self.dog_target = pick
			# v18: the host's picks for Quiet (her target; Cover me's threat), logged as they change
			recs = [actors.actors[i] for i in range(min(actors.count, proto.MAX_ACTORS))]
			qpick = next((r.handle for r in recs if r.flags & proto.ActorFlags.QuietTarget), 0)
			qthreat = next((r.handle for r in recs if r.flags & proto.ActorFlags.QuietThreat), 0)
			if qpick:
				st["quietTargetFrames"] += 1
				if hex(qpick) not in st["quietTargetsSeen"]:
					st["quietTargetsSeen"].append(hex(qpick))
			if qthreat and hex(qthreat) not in st["quietThreatsSeen"]:
				st["quietThreatsSeen"].append(hex(qthreat))
			if qpick != self.quiet_target:
				self.log(f"quiet: the host's pick (ActorFlags.QuietTarget) {qpick:#x}" if qpick else "quiet: the host has no pick (no ActorFlags.QuietTarget)")
				self.quiet_target = qpick
			if qthreat != self.quiet_threat:
				self.log(f"quiet: Cover me's threat pick (ActorFlags.QuietThreat) {qthreat:#x}" if qthreat else "quiet: no threat pick")
				self.quiet_threat = qthreat
		self.tick_call_menu(t)
		self.tick_dog(t)
		self.tick_quiet(t)

		# Motion: a slow circle plus the left stick (MGSV would turn the stick into locomotion itself).
		riding = ((6.0 <= (t % 12.0) < 10.0) if self.ride == "auto" else self.ride == "on") and self.buddy in RIDEABLE
		if riding:
			st["rideFrames"] += 1
		btype, bobject, ride_factor, ride_stick, _ = BUDDIES[self.buddy]
		if self.mantle and t >= self.mantle["end"]:
			self.log(f"mantle over ({'vault' if self.mantle['vault'] else 'climb up'}, {self.mantle['height']:.2f} m) -> GuestFlags.Mantling off")
			self.mantle = None
		mantling = self.mantle is not None and self.mantle_play and t >= self.mantle["start"]
		going_home = self.climb is None and t < self.climb_home_until  # the real guest's way back from its ladder
		if self.climb_home_until and self.climb is None and t >= self.climb_home_until:
			self.climb_home_until = 0.0
			self.log("home from MGSV's ladder -> GuestFlags.Climbing off")
		climbing = (self.climb is not None and self.climb_play) or going_home
		if climbing and self.climb is not None:
			# The climb cycle's phase at its pace (one cycle per ~0.6 m of rungs).
			self.climb["phase"] += dt * self.climb["speed"] / 0.6 * 2 * math.pi
		busy = climbing or mantling or (self.mantle is not None and self.mantle_play)  # MGSV's move holds Snake (no walking)
		speed = 0.0 if (self.stand or not self.walk or self.parked or busy) else 1.4 * (ride_factor if riding else 1.0)
		self.heading += speed / 4.0 * dt
		vx, vz = math.cos(self.heading) * speed, -math.sin(self.heading) * speed
		if self.stick_mode == "camera" and self.last_cam and not self.parked:
			# As MGSV moves Snake: stick up = away from the camera, right = the screen's right as Elden Ring shows it (calibrated
			# live against ER's camera rows: with [Axes] mirror_x -1 that is (-fz, fx) in MGSV x/z).
			fx, fz = self.root[0] - self.last_cam[0], self.root[2] - self.last_cam[2]
			n = math.hypot(fx, fz) or 1.0
			fx, fz = fx / n, fz / n
			sx, sy = lx / 32767.0, ly / 32767.0
			mag = min(1.0, math.hypot(sx, sy))
			top = ride_stick if riding else self.stick_speed
			if mag > 0.25:  # MGSV's dead zone
				k = top * mag / math.hypot(sx, sy)
				vx += (-fz * sx + fx * sy) * k
				vz += (fx * sx + fz * sy) * k
		else:
			vx += lx / 32767.0 * 3.0
			vz += ly / 32767.0 * 3.0
		if self.move and not self.parked:
			vx, vz = math.sin(self.move[1]) * self.move[0], math.cos(self.move[1]) * self.move[0]
		if busy:
			vx = vz = 0.0  # MGSV plays its climb / climb-up: no walking meanwhile
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
		self.last_cam = cam
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
			self.status(Status.Ready, f"fake MGSV: in the sandbox, {BUDDIES[self.buddy][4]}")
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
		if self.nvg:
			flags |= GuestFlags.Nvg
		if self.binoculars:
			flags |= GuestFlags.Binoculars | GuestFlags.FirstPerson  # MGSV's BINOCLE status is first person too (mgsv/Frame.cpp)
		if self.suppressor is not None:
			flags |= GuestFlags.SuppressorOn  # v16: the weapon in hand has a working suppressor
		if climbing and self.ready():
			flags |= GuestFlags.Climbing
		if mantling and self.ready():
			flags |= GuestFlags.Mantling
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
		gs.buddyType = btype
		gs.vehicleId = bobject if riding else 0xFFFF
		gs.phase = 0
		# v2 buddy block: D-Walker beside Snake, or under him while ridden.
		bflags = proto.GuestBuddyFlags.Present | proto.GuestBuddyFlags.PosValid if self.buddy != "none" else proto.GuestBuddyFlags(0)
		if riding:
			bflags |= proto.GuestBuddyFlags.Ridden
		if self.dog_engaged is not None and self.buddy == "dog":
			bflags |= proto.GuestBuddyFlags.Engaged  # v17: D-Dog carries out an attack (Connected .. Released)
			st["dogEngagedFrames"] += 1
		if self.buddy == "dog":
			bflags |= proto.buddy_dog_style_bits(self.dog_style)  # v17: its style every frame (buddyFlags bits 8-15)
		off = (0.0, 0.0) if riding else (1.5 * math.cos(self.yaw) - 0.5 * math.sin(self.yaw), -1.5 * math.sin(self.yaw) - 0.5 * math.cos(self.yaw))
		dy = 0.0
		if self.buddy == "dog" and not riding:
			if self.dog_wait is not None:
				off = (self.dog_wait[0] - self.odo[0], self.dog_wait[1] - self.odo[2])  # Wait: its place in the odometer frame
			if self.dog_blip and self.dog_blip["frames"] > 0:
				off = (off[0] + self.dog_blip["dx"], off[1] + self.dog_blip["dz"])
				dy = self.dog_blip["dy"]
				self.dog_blip["frames"] -= 1
				st["dogBlipFrames"] = st.get("dogBlipFrames", 0) + 1
		buddy_yaw = self.yaw
		if self.buddy == "quiet" and not riding:
			# v18: her bits every frame, and the host's grace seat / post held and echoed as the real guest's sit job / mirror would
			qbits, place, buddy_yaw, post_seq, grace_seq = self.quiet_frame(buddy_yaw)
			bflags |= qbits
			if place is not None:
				off, dy = (place[0] - self.root[0], place[2] - self.root[2]), place[1]
			gs.buddyPostSeq, gs.graceSeqSeated = post_seq, grace_seq
		self.buddy_rel = (off[0], dy, off[1])
		gs.buddyFlags = int(bflags)
		gs.buddyObjectId = bobject
		gs.buddyAmmo = self.buddy_ammo
		gs.buddyPos[:] = [self.root[0] + off[0], dy, self.root[2] + off[1]]
		self.buddy_pos = list(gs.buddyPos)
		gs.buddyYaw = buddy_yaw
		link.seq_write(proto.OFF_GUEST_STATE, gs)
		self.vars.update(playerPosX=self.root[0], playerPosY=self.root[1], playerPosZ=self.root[2],
			playerRotY=int(math.degrees(self.yaw)), playerLife=self.life, playerLifeMax=LIFE_MAX,
			buddyType=int(btype), playerVehicleGameObjectId=gs.vehicleId,
			missionCode=gs.missionCode, locationCode=gs.locationCode, frame=self.frame)

		self.publish_pose()

		if not self.autofire or self.parked:
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

		self.climbing, self.mantling = climbing, mantling
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
				return 0, (f"fake guest: frame {self.frame}, session {self.nonce:#018x}, buddy {BUDDIES[self.buddy][0].name} ammo {self.buddy_ammo}, stick {self.stick_mode} ({self.stick_speed:.1f} m/s), life {self.life} "
					f"(regen {self.regen:.0f}/s, healed {st['healed']}), ammo {self.ammo}+{self.stock}, nvg {'on' if self.nvg else 'off'}, "
					f"{'PARKED (Tarnished mode), ' if self.parked else ''}menu {'open' if self.in_menu else 'closed'}, "
					f"{('CLIMBING %s at %.2f m/s, ' % ('down' if self.climb['down'] else 'up', self.climb['speed'])) if self.climb else ''}"
					f"{('MANTLING (%s %.2f m), ' % ('vault' if self.mantle['vault'] else 'climb up', self.mantle['height'])) if self.mantle else ''}"
					f"pad: Back {st['backPresses']} Start {st['startPresses']} in-menu presses {st['menuButtons']}, "
					f"presses {' '.join(f'{k} {v}' for k, v in sorted(st['pressCounts'].items())) or 'none'}, events {st['hostEvents']}")
			return self.combat_command(cmd.split())
		try:
			return 0, luasubset.run_chunk(chunk, self.env)
		except luasubset.LuaError as e:
			return 1, str(e)

	def combat_command(self, w: list[str]) -> tuple[int, str]:
		usage = ("fake guest commands: !status !walk on|off !autofire on|off !fire <kind> [n] [yaw] [pitch] [share] [suppressed|loud] !suppressor on [shots]|off !share <s> !move <speed> [yaw] "
			"!ride on|off|auto !buddy walker|horse|dog|quiet|none !fulton [awake|any|0xHANDLE] [fail] !idroid on|off !stick world|camera [m/s] !box on|off|poster !flame [s] !fpv on|off !cqc <kind> !grab <action> !blast [r] [kind] !die "
			"!throw <frag|sleep|stun|smoke> [speed] [pitch] [yaw] !aimthrow on|off [speed] [pitch] [kind] !place <kind> [trigger_m] !detonate <id> "
			"!life <n> [regen] !menu on|off !aim on|off !hud mgsv|bar|none !boot N [text] !fail [text] !ready !nvg on|off !binoculars on|off "
			"!climbplay on|off !climbhome <ms> !mantleplay on|off !order <cmd> !dog [status|style <s>|attack [target|nearest|none|0xH] [style] [full|ordered|connect|lost|refused]|release|busy <s>] "
			"!quiet [status|weapon <w>|post on|off|seat on|off|cloak on|off|cover on|off|marked on|off|hit <s>|shot [pick|threat|nearest|none|0xH] [phase] [reason] [weapon]]")
		if not w:
			return 0, usage
		if w[0] == "projmesh" and len(w) > 1 and w[1] in ("on", "off"):
			self.proj_meshes = w[1] == "on"
			return 0, f"grenades in flight {'drawn as kOwnerProjectile draws' if self.proj_meshes else 'not drawn'} (--model humanoid only)"
		if w[0] == "climbhome" and len(w) > 1:
			self.climb_home_ms = max(0, int(float(w[1])))
			return 0, f"after kHEvClimb a = 0 Snake is home {self.climb_home_ms} ms later (GuestFlags.Climbing and no walking until then)"
		if w[0] == "climbplay" and len(w) > 1:
			self.climb_play = w[1] == "on"
			return 0, f"ladder climb {'played' if self.climb_play else 'NOT played'} on kHEvClimb (GuestFlags.Climbing)"
		if w[0] == "mantleplay" and len(w) > 1:
			self.mantle_play = w[1] == "on"
			return 0, f"climb-up / vault {'played' if self.mantle_play else 'NOT played'} on kHEvMantle (GuestFlags.Mantling)"
		if w[0] == "order" and len(w) > 1:
			return self.buddy_order(int(w[1], 0))
		if w[0] == "dog":
			return self.dog_command(w[1:])
		if w[0] == "quiet":
			return self.quiet_command(w[1:])
		if w[0] == "callmenu" and len(w) > 1:
			self.callmenu = w[1] == "on"
			self.callmenu_mode = w[2] if len(w) > 2 and w[2] in ("full", "ordered", "connect") else "full"
			return 0, (f"call menu from the pad {'on' if self.callmenu else 'off'} (D-Dog: LB, right stick, let go, R3 -> its attack command; "
				f"a taken order runs `!dog attack <pick> {self.callmenu_mode}`)")
		if w[0] == "binoculars" and len(w) > 1:
			self.binoculars = w[1] == "on"
			return 0, f"binoculars {'on' if self.binoculars else 'off'} (GuestFlags.Binoculars)"
		if w[0] == "nvg" and len(w) > 1:
			self.nvg = w[1] == "on"
			return 0, f"night-vision goggles {'on' if self.nvg else 'off'} (GuestFlags.Nvg)"
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
			return 0, f"ride {self.ride} ({BUDDIES[self.buddy][4]})"
		if w[0] == "buddy" and len(w) > 1 and w[1] in BUDDIES:
			self.buddy = w[1]
			return 0, f"buddy {BUDDIES[self.buddy][0].name} ({BUDDIES[self.buddy][4]}; ridden: x{BUDDIES[self.buddy][2]} on the circle, "\
				f"{BUDDIES[self.buddy][3]} m/s on the stick)"
		if w[0] == "stick" and len(w) > 1 and w[1] in ("world", "camera"):
			self.stick_mode = w[1]
			if len(w) > 2:
				self.stick_speed = max(0.1, float(w[2]))
			return 0, f"stick {self.stick_mode}" + (f": full stick {self.stick_speed:.1f} m/s on foot, relative to the camera" if self.stick_mode == "camera" else
				": MGSV x/z, 3 m/s")
		if w[0] == "idroid" and len(w) > 1 and w[1] in ("on", "off"):
			self.idroid = w[1] == "on"
			return 0, f"idroid {w[1]}: a Start press {'opens / closes the iDroid (kGuestInMenu)' if self.idroid else 'is only counted'}"
		if w[0] == "fulton":
			# v12 kGEvFulton for an Elden Ring character from the ActorTable (as the real guest sends when Snake fultons the
			# soldier that stood for it): the nearest sleeping hostile one (kActorAsleep; "awake": asleep or not, "any": anyone),
			# a handle given in hex, or "fail" (a = 0: the balloon never went up).
			actors = self.link.actors()
			recs = [actors.actors[i] for i in range(min(actors.count, len(actors.actors)))] if actors else []
			fail = "fail" in w[1:]
			want = next((int(x, 16) for x in w[1:] if x.startswith("0x")), None)
			pick = None
			for r in recs:
				if want is not None:
					if r.handle == want:
						pick = r
					continue
				if (r.flags & proto.ActorFlags.Dead) or (not (r.flags & proto.ActorFlags.Hostile) and "any" not in w[1:]):
					continue
				if not (r.flags & proto.ActorFlags.Asleep) and "any" not in w[1:] and "awake" not in w[1:]:
					continue  # as the real guest: MGSV's Fulton takes a sleeping (or knocked-out) soldier

				d = math.hypot(r.pos[0] - self.root[0], r.pos[2] - self.root[2])
				if pick is None or d < math.hypot(pick.pos[0] - self.root[0], pick.pos[2] - self.root[2]):
					pick = r
			if pick is None:
				return 1, f"fulton: no {'such' if want is not None else 'sleeping hostile'} actor in the ActorTable ({len(recs)} records)"
			self.push_event(GuestEventType.Fulton, a=0 if fail else 1, v0=tuple(pick.pos), pad0=pick.handle & 0xFFFFFFFF, pad1=pick.handle >> 32)
			name = bytes(pick.name).split(b"\0", 1)[0].decode("utf-8", "replace")
			return 0, f"kGEvFulton a={0 if fail else 1} handle {pick.handle:#x} ({name}) at ({pick.pos[0]:.1f}, {pick.pos[1]:.1f}, {pick.pos[2]:.1f})"
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
		if w[0] == "suppressor":
			if len(w) > 1 and w[1] == "on":
				self.suppressor = int(w[2]) if len(w) > 2 else 4
				return 0, f"suppressor on (GuestFlags.SuppressorOn), worn out after {self.suppressor or 'no'} suppressed shots"
			self.suppressor = None
			return 0, "suppressor off"
		if w[0] == "fire":
			words = [x for x in w if x not in ("suppressed", "loud")]
			quiet = "suppressed" in w or ("loud" not in w and self.suppressor is not None)
			w = words
			kind = w[1] if len(w) > 1 else "lethal"
			if kind not in FIRE_KINDS:
				return 1, f"kinds: {' '.join(FIRE_KINDS)}"
			bullet, ammo, pellets, explosive = FIRE_KINDS[kind]
			n = int(w[2]) if len(w) > 2 else 1
			yaw = self.yaw + math.radians(float(w[3]) if len(w) > 3 else 0.0)
			pitch = math.radians(float(w[4]) if len(w) > 4 else 0.0)
			d = (math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch))
			share = float(w[5]) if len(w) > 5 else self.share
			worn = False
			for _ in range(max(1, min(n, 50))):
				self.push_event(GuestEventType.Fire, a=WEAPON_ID | (int(ammo) << 16), b=pellets | (proto.FIRE_SUPPRESSED if quiet else 0), v0=chest, v1=d,
					pad0=bullet | (0x80000000 if explosive else 0), pad1=int(round(share * 10000)))
				self.stats["fireSent"] += 1
				self.spend_round()
				if quiet and self.suppressor:
					self.suppressor -= 1
					if self.suppressor == 0:
						self.suppressor, quiet, worn = None, False, True  # MGSV: the spent suppressor comes off; the next shots are loud
			return 0, (f"fired {n} x {kind} (bullet {bullet}, ammo {ammo.name}, pellets {pellets}, share {share:.3f}{', suppressed' if quiet or worn else ''})"
				f" dir ({d[0]:.2f}, {d[1]:.2f}, {d[2]:.2f})" + (" -- the suppressor wore out (GuestFlags.SuppressorOn off)" if worn else ""))
		if w[0] == "cqc":
			kinds = {k.name.lower(): k for k in proto.MeleeKind}
			kind = kinds.get(w[1] if len(w) > 1 else "punch")
			if kind is None:
				return 1, f"kinds: {' '.join(kinds)}"
			centre = (self.root[0] + fwd[0], 1.1, self.root[2] + fwd[2])
			self.push_event(GuestEventType.Melee, a=int(kind), b=900, v0=centre, v1=fwd, pad0=7 if kind == proto.MeleeKind.Kick else 1)
			return 0, f"cqc {kind.name} at ({centre[0]:.2f}, {centre[1]:.2f}, {centre[2]:.2f})"
		if w[0] == "grab":
			actions = {a.name.lower(): a for a in proto.CqcAction}
			action = actions.get(w[1] if len(w) > 1 else "grab")
			if action is None:
				return 1, f"actions: {' '.join(actions)}"
			self.push_event(GuestEventType.Cqc, a=int(action), b=1400, v0=tuple(self.root), v1=fwd, pad0=0, pad1=0)
			return 0, f"kGEvCqc {action.name} at ({self.root[0]:.2f}, {self.root[1]:.2f}, {self.root[2]:.2f})"
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

	# ---- v17 D-Dog -------------------------------------------------------------------------------------------
	def buddy_order(self, cmd: int) -> tuple[int, str]:
		"""kGEvBuddyOrder as the real guest's CommandDetour sends it: b = the buddy's type; D-Dog: pad[0] = its style + 1."""
		btype = BUDDIES[self.buddy][0]
		if self.buddy == "dog":
			# (0.8.0 polish) Wait keeps the dog where it is in Snake's odometer frame (the real guest moves it with the treadmill's
			# jumps); the whistle / DD / an attack order let it follow again.
			if cmd == 9:
				self.dog_wait = (self.buddy_rel[0] + self.odo[0], self.buddy_rel[2] + self.odo[2])
				self.log(f"dog: waits at ({self.dog_wait[0]:.2f}, {self.dog_wait[1]:.2f}) in the odometer frame")
			elif cmd in (8, 16) and self.dog_wait is not None:
				self.dog_wait = None
				self.log("dog: called back: follows Snake again")
		# pad[0] only for the attack orders 11-15, as the real guest's OrderStylePad (the style reaches the host every frame in
		# buddyFlags anyway).
		style = self.dog_style + 1 if btype == proto.BuddyType.DOG and 11 <= cmd <= 15 and self.dog_style != proto.DogStyle.StyleUnknown else 0
		# v18: Quiet's commands 1-3 carry her weapon + 1 (0 = unknown), as the real guest's CommandDetour once it reads her weapon
		if btype == proto.BuddyType.QUIET and 1 <= cmd <= 3 and self.quiet_weapon != proto.QuietWeapon.WeaponUnknown:
			style = self.quiet_weapon + 1
		self.push_event(GuestEventType.BuddyOrder, a=cmd, b=int(btype), pad0=style)
		self.stats["buddyOrdersSent"] += 1
		what = (f" (D-Dog style {self.dog_style.name})" if btype == proto.BuddyType.DOG else f" (Quiet weapon {self.quiet_weapon.name})") if style else ""
		return 0, f"kGEvBuddyOrder a={cmd} b={int(btype)} ({btype.name}) pad0={style}" + what

	def dog_records(self) -> list:
		actors = self.link.actors()
		return [actors.actors[i] for i in range(min(actors.count, proto.MAX_ACTORS))] if actors else []

	def dog_send(self, phase, style, handle: int, pos) -> str:
		dog = self.buddy_pos or list(self.root)
		d = (0.0, 0.0, 0.0)
		if pos is not None:
			dx, dy, dz = pos[0] - dog[0], pos[1] - dog[1], pos[2] - dog[2]
			n = math.sqrt(dx * dx + dy * dy + dz * dz)
			d = (dx / n, dy / n, dz / n) if n > 1e-3 else d
		a = proto.dog_attack_a(phase, style, DOG_ATTACK_IDS.get(style, 0))
		share = self.share if self.share > 0 else DOG_SHARES.get(style, 0.0)
		self.push_event(GuestEventType.DogAttack, a=a, b=int(round(share * 10000)), v0=tuple(dog), v1=d,
			pad0=handle & 0xFFFFFFFF, pad1=handle >> 32)
		if phase == proto.DogPhase.Ordered and self.dog_wait is not None:
			# (review 0.8.0) as the real guest: an attack order MGSV takes ends the Wait (a refused one leaves the dog waiting)
			self.dog_wait = None
			self.log("dog: ordered from its Wait: follows its order (Snake) again")
		if phase == proto.DogPhase.Connected:
			self.dog_engaged, self.dog_engaged_style = handle, style
		elif phase in (proto.DogPhase.Released, proto.DogPhase.Lost, proto.DogPhase.Refused) and self.dog_engaged == handle:
			self.dog_engaged = None
		name = f"{proto.DogPhase(phase).name}/{proto.enum_name(style, proto.DogStyle)}"
		self.stats["dogAttacksSent"].append(name)
		text = f"kGEvDogAttack {name} (MGSV attack {DOG_ATTACK_IDS.get(style, 0)}, share {share:.3f}) on ER {handle:#x}"
		self.log(f"dog: {text}{' -- GuestBuddyFlags.Engaged' if self.dog_engaged is not None else ''}")
		return text

	# (0.8.0 polish) MGSV's call menu as the real guest sees it (!callmenu on): hold LB, push the right stick, let it go, click it (R3)
	# while LB is held -> the attack command of the dog's style (kGEvBuddyOrder); like the real guest's job the host's pick
	# (ActorFlags.DogTarget) is taken at once or within 350 ms, else Refused.
	CM_COMMANDS = {proto.DogStyle.Busy: 11, proto.DogStyle.Kill: 12, proto.DogStyle.KnockOut: 13, proto.DogStyle.Wound: 14, proto.DogStyle.Fetch: 15}

	def call_menu(self, t: float, pad):
		lb = bool(pad.buttons & 0x0100)
		cm = self.cm
		if not lb:
			cm["lb"] = cm["pointed"] = cm["let_go"] = False
			return
		cm["lb"] = True
		rmag = math.hypot(pad.thumbRX, pad.thumbRY) / 32767.0
		if rmag > 0.5:
			cm["pointed"], cm["let_go"] = True, False
		elif cm["pointed"] and rmag < 0.3:
			cm["let_go"] = True
		if (pad.buttons & ~self.last_buttons & 0x0080) and cm["let_go"]:
			cmd = self.CM_COMMANDS.get(self.dog_style, 11)
			st = self.stats
			st["callMenuOrders"] = st.get("callMenuOrders", 0) + 1
			self.buddy_order(cmd)
			pick = self.dog_target
			self.log(f"callmenu: the player's attack command {cmd} at t={t:.3f} -- the host's pick now {pick:#x}")
			cm["pending"] = {"cmd": cmd, "at": t, "until": t + 0.35}
			cm["pointed"] = cm["let_go"] = False

	def tick_call_menu(self, t: float):
		p = self.cm["pending"]
		if p is None:
			return
		st = self.stats
		if self.dog_target:
			self.cm["pending"] = None
			st["callMenuOrdered"] = st.get("callMenuOrdered", 0) + 1
			self.log(f"callmenu: order {p['cmd']} takes the host's pick {self.dog_target:#x} {1000 * (t - p['at']):.0f} ms after the click")
			self.dog_command(["attack", hex(self.dog_target), getattr(self, "callmenu_mode", "full")])
		elif t >= p["until"]:
			self.cm["pending"] = None
			st["callMenuRefused"] = st.get("callMenuRefused", 0) + 1
			self.log(f"callmenu: order {p['cmd']} REFUSED -- no host pick within 350 ms of the click")
			self.dog_send(proto.DogPhase.Refused, self.dog_style, 0, None)

	def tick_dog(self, t: float):
		job = self.dog_job
		if job is None:
			return
		while job["steps"] and t >= job["steps"][0][0]:
			_, phase = job["steps"].pop(0)
			self.dog_send(phase, job["style"], job["handle"], job["pos"])
		if not job["steps"]:
			self.dog_job = None

	def dog_command(self, w: list[str]) -> tuple[int, str]:
		if not w or w[0] == "status":
			job = self.dog_job
			return 0, (f"D-Dog: buddy {self.buddy}, style {self.dog_style.name}, engaged {'%#x' % self.dog_engaged if self.dog_engaged is not None else 'no'}, "
				f"job {('%#x %s, %d steps left' % (job['handle'], job['style'].name, len(job['steps']))) if job else 'none'}, "
				f"the host's pick {self.dog_target:#x}, busy {self.dog_busy_s:.1f} s")
		if w[0] == "style" and len(w) > 1 and w[1] in DOG_STYLES:
			self.dog_style = DOG_STYLES[w[1]]
			return 0, f"D-Dog style {self.dog_style.name} (BuddyOrder pad[0] {self.dog_style + 1 if self.dog_style != proto.DogStyle.StyleUnknown else 0})"
		if w[0] == "busy" and len(w) > 1:
			self.dog_busy_s = max(0.1, float(w[1]))
			return 0, f"a full run holds {self.dog_busy_s:.1f} s between Connected and Released"
		if w[0] == "blip" and len(w) > 2:
			# (0.8.0 polish) the dog's place (buddyPos and its draws) off by dx, dz (dy) m for N frames, then back: the host's step
			# guard must hold it (docs/HOST.md "D-Dog in Elden Ring")
			frames = int(w[3]) if len(w) > 3 else 1
			self.dog_blip = {"dx": float(w[1]), "dz": float(w[2]), "dy": float(w[4]) if len(w) > 4 else 0.0, "frames": max(1, frames)}
			return 0, f"D-Dog off by ({self.dog_blip['dx']}, {self.dog_blip['dy']}, {self.dog_blip['dz']}) m for {self.dog_blip['frames']} frame(s)"
		if w[0] == "move" and len(w) > 2:
			# (0.8.0 polish) the dog put somewhere else for good (as the guest's leash does): it waits there in the odometer frame
			self.dog_wait = (self.buddy_rel[0] + self.odo[0] + float(w[1]), self.buddy_rel[2] + self.odo[2] + float(w[2]))
			return 0, f"D-Dog moved by ({w[1]}, {w[2]}) m (and waits there; !order 8 to follow)"
		if self.buddy != "dog":
			return 1, f"!dog: the buddy is {self.buddy} (`!buddy dog` first)"
		if w[0] == "release":
			if self.dog_engaged is None:
				return 1, "!dog release: not engaged"
			handle = self.dog_engaged
			pos = next((list(r.pos) for r in self.dog_records() if r.handle == handle), None)
			self.dog_job = None
			return 0, self.dog_send(proto.DogPhase.Released, self.dog_engaged_style, handle, pos)
		if w[0] != "attack":
			return 1, "usage: !dog [status] | style <busy|fetch|kill|knockout|wound|unknown> | attack [target|nearest|none|0xHANDLE] [style] [full|ordered|connect|lost|refused] | release | busy <s>"
		which, style, mode = "target", self.dog_style, "full"
		for x in w[1:]:
			if x in DOG_STYLES:
				style = DOG_STYLES[x]
			elif x in ("full", "ordered", "connect", "lost", "refused"):
				mode = x
			elif x in ("target", "nearest", "none") or x.startswith("0x"):
				which = x
			else:
				return 1, f"!dog attack: what is {x!r}?"
		out = []
		# A new order ends the one under way, as MGSV's would: a bite lets go (Released), a run never finishes (Lost).
		if self.dog_engaged is not None:
			out.append(self.dog_send(proto.DogPhase.Released, self.dog_engaged_style, self.dog_engaged, None))
		elif self.dog_job is not None:
			out.append(self.dog_send(proto.DogPhase.Lost, self.dog_job["style"], self.dog_job["handle"], self.dog_job["pos"]))
		self.dog_job = None
		recs = self.dog_records()
		pick = None
		if which == "target":
			pick = next((r for r in recs if r.flags & proto.ActorFlags.DogTarget), None)
		elif which == "nearest":
			for r in recs:
				if r.flags & (proto.ActorFlags.Dead | proto.ActorFlags.Boss) or not r.flags & proto.ActorFlags.Hostile:
					continue  # the real host's pick: hostile and alive only, never a boss by default
				if style == proto.DogStyle.Fetch and not r.flags & proto.ActorFlags.Asleep:
					continue
				d = math.hypot(r.pos[0] - self.root[0], r.pos[2] - self.root[2])
				if pick is None or d < math.hypot(pick.pos[0] - self.root[0], pick.pos[2] - self.root[2]):
					pick = r
		elif which.startswith("0x"):
			want = int(which, 16)
			pick = next((r for r in recs if r.handle == want), None)
		if pick is None or mode == "refused":
			handle = pick.handle if pick is not None else 0
			out.append(self.dog_send(proto.DogPhase.Refused, style, handle, list(pick.pos) if pick is not None else None))
			return 0, "; ".join(out) + ("" if pick is not None else f" -- no {which} in the ActorTable ({len(recs)} records)")
		pos = list(pick.pos)
		out.append(self.dog_send(proto.DogPhase.Ordered, style, pick.handle, pos))
		t = self.elapsed()
		if mode == "connect":
			out.append(self.dog_send(proto.DogPhase.Connected, style, pick.handle, pos))
		elif mode == "lost":
			self.dog_job = {"handle": pick.handle, "style": style, "pos": pos, "steps": [(t + 0.5, proto.DogPhase.Lost)]}
		elif mode == "full":
			self.dog_job = {"handle": pick.handle, "style": style, "pos": pos,
				"steps": [(t + 0.6, proto.DogPhase.Connected), (t + 0.6 + self.dog_busy_s, proto.DogPhase.Released)]}
		name = bytes(pick.name).split(b"\0", 1)[0].decode("utf-8", "replace")
		return 0, "; ".join(out) + f" ({name}, {mode})"

	# ---- v18 Quiet ------------------------------------------------------------------------------------------
	def quiet_frame(self, yaw: float):
		"""This frame's Quiet bits, her place (MGSV world; None = beside Snake as before), yaw and the two echoes."""
		st, host = self.stats, self.host_last
		bits = proto.buddy_quiet_weapon_bits(self.quiet_weapon)
		if self.quiet_cover:
			bits |= proto.GuestBuddyFlags.QuietCover
		if self.quiet_marked:
			bits |= proto.GuestBuddyFlags.QuietMarked
		if self.quiet_seat:
			bits |= proto.GuestBuddyFlags.CanRest
		if self.quiet_engaged is not None:
			bits |= proto.GuestBuddyFlags.Engaged  # Ordered .. Released
			st["quietEngagedFrames"] += 1
		place, post_seq, grace_seq = None, 0, 0
		hflags = host.flags if host is not None else 0
		if (self.quiet_seat and host is not None and hflags & proto.HostFlags.AtGrace and host.graceSeq and
				all(math.isfinite(v) for v in host.graceSeat)):
			place, yaw, grace_seq = list(host.graceSeat), host.graceSeatYaw, host.graceSeq
			bits |= proto.GuestBuddyFlags.Seated
			st["quietSeatFrames"] += 1
			if grace_seq not in st["quietGraceSeqs"]:
				st["quietGraceSeqs"].append(grace_seq)
		elif self.quiet_post and host is not None and hflags & proto.HostFlags.QuietLayer and host.buddyPostSeq:
			if all(math.isfinite(v) for v in host.buddyPost):
				place, yaw, post_seq = list(host.buddyPost), host.buddyPostYaw, host.buddyPostSeq
				bits |= proto.GuestBuddyFlags.AtPost
				st["quietPostFrames"] += 1
				if post_seq not in st["quietPostSeqs"]:
					st["quietPostSeqs"].append(post_seq)
			else:
				bits |= proto.GuestBuddyFlags.Cloaked  # no post: on the move, cloaked
		if self.quiet_cloak:
			bits |= proto.GuestBuddyFlags.Cloaked
		if bits & proto.GuestBuddyFlags.Cloaked:
			st["quietCloakFrames"] += 1
		if (post_seq, grace_seq) != self.quiet_echo:
			self.quiet_echo = (post_seq, grace_seq)
			where = f" at ({place[0]:.2f},{place[1]:.2f},{place[2]:.2f}) yaw {math.degrees(yaw):.1f} deg" if place else ""
			self.log(f"quiet: {'seated for rest %d' % grace_seq if grace_seq else 'at post %d' % post_seq if post_seq else 'beside Snake'}{where} "
				f"(GuestState buddyPostSeq {post_seq}, graceSeqSeated {grace_seq})")
		return bits, place, yaw, post_seq, grace_seq

	def quiet_send(self, phase, weapon, handle: int, pos, b: int | None = None) -> str:
		me = self.buddy_pos or list(self.root)
		d = (0.0, 0.0, 0.0)
		if pos is not None:
			dx, dy, dz = pos[0] - me[0], pos[1] - me[1], pos[2] - me[2]
			n = math.sqrt(dx * dx + dy * dy + dz * dz)
			d = (dx / n, dy / n, dz / n) if n > 1e-3 else d
		attack = QUIET_ATTACK_IDS.get(weapon, 0)
		share = self.share if self.share > 0 else QUIET_SHARES.get(weapon, 0.0)
		if b is None:
			b = int(round(share * 10000))
		self.push_event(GuestEventType.QuietShot, a=proto.quiet_shot_a(phase, weapon, attack), b=b, v0=tuple(me), v1=d,
			pad0=handle & 0xFFFFFFFF, pad1=handle >> 32)
		if phase == proto.QuietPhase.Ordered:
			self.quiet_engaged = handle
		elif phase in (proto.QuietPhase.Released, proto.QuietPhase.Lost, proto.QuietPhase.Refused) and self.quiet_engaged == handle:
			self.quiet_engaged = None
		name = f"{proto.QuietPhase(phase).name}/{proto.enum_name(weapon, proto.QuietWeapon)}"
		self.stats["quietShotsSent"].append(name)
		what = f"reason {proto.enum_name(b, proto.QuietLostReason)}" if phase == proto.QuietPhase.Lost else f"share {b / 10000:.3f}"
		text = f"kGEvQuietShot {name} (MGSV attack {attack}, {what}) on ER {handle:#x}"
		self.log(f"quiet: {text}{' -- GuestBuddyFlags.Engaged' if self.quiet_engaged is not None else ''}")
		return text

	def tick_quiet(self, t: float):
		job = self.quiet_job
		if job is None:
			return
		while job["steps"] and t >= job["steps"][0][0]:
			_, phase, b = job["steps"].pop(0)
			self.quiet_send(phase, job["weapon"], job["handle"], job["pos"], b)
		if not job["steps"]:
			self.quiet_job = None

	def quiet_command(self, w: list[str]) -> tuple[int, str]:
		onoff = {"post": "quiet_post", "seat": "quiet_seat", "cloak": "quiet_cloak", "cover": "quiet_cover", "marked": "quiet_marked"}
		if not w or w[0] == "status":
			job, host = self.quiet_job, self.host_last
			hf = proto.flag_names(host.flags & int(proto.HostFlags.QuietLayer | proto.HostFlags.AtGrace), proto.HostFlags) if host is not None else "-"
			return 0, (f"Quiet: buddy {self.buddy}, weapon {self.quiet_weapon.name}, post {'on' if self.quiet_post else 'off'}, seat "
				f"{'on' if self.quiet_seat else 'off'}, cloak {'on' if self.quiet_cloak else 'off'}, cover {'on' if self.quiet_cover else 'off'}, "
				f"marked {'on' if self.quiet_marked else 'off'}, engaged {'%#x' % self.quiet_engaged if self.quiet_engaged is not None else 'no'}, "
				f"job {('%#x, %d steps left' % (job['handle'], len(job['steps']))) if job else 'none'}, echoes post {self.quiet_echo[0]} rest "
				f"{self.quiet_echo[1]}, the host's {hf} pick {self.quiet_target:#x} threat {self.quiet_threat:#x}, hit {self.quiet_hit_s:.1f} s")
		if w[0] == "weapon" and len(w) > 1 and w[1] in QUIET_WEAPONS:
			self.quiet_weapon = QUIET_WEAPONS[w[1]]
			bits = proto.buddy_quiet_weapon_bits(self.quiet_weapon)
			return 0, f"Quiet's weapon {self.quiet_weapon.name} (buddyFlags {bits:#x}; BuddyOrder 1-3 pad[0] {bits >> proto.BUDDY_QUIET_WEAPON_SHIFT})"
		if w[0] in onoff and len(w) > 1 and w[1] in ("on", "off"):
			setattr(self, onoff[w[0]], w[1] == "on")
			return 0, f"Quiet {w[0]} {w[1]}"
		if w[0] == "hit" and len(w) > 1:
			self.quiet_hit_s = max(0.0, float(w[1]))
			return 0, f"a full shot hits {self.quiet_hit_s:.1f} s after Ordered"
		if self.buddy != "quiet":
			return 1, f"!quiet: the buddy is {self.buddy} (`!buddy quiet` first)"
		if w[0] != "shot":
			return 1, ("usage: !quiet [status] | weapon <lethal|tranq|heavy|unknown> | post|seat|cloak|cover|marked on|off | hit <s> | "
				"shot [pick|threat|nearest|none|0xHANDLE] [full|ordered|hit|released|lost|refused] [mgsv|target|cancelled] [weapon]")
		which, mode, reason, weapon = "pick", "full", proto.QuietLostReason.LostMgsv, self.quiet_weapon
		for x in w[1:]:
			if x in QUIET_WEAPONS:
				weapon = QUIET_WEAPONS[x]
			elif x in ("full", "ordered", "hit", "released", "lost", "refused"):
				mode = x
			elif x in QUIET_LOST:
				reason = QUIET_LOST[x]
			elif x in ("pick", "threat", "nearest", "none") or x.startswith("0x"):
				which = x
			else:
				return 1, f"!quiet shot: what is {x!r}?"
		recs = self.dog_records()
		pick = None
		if which == "pick":
			pick = next((r for r in recs if r.flags & proto.ActorFlags.QuietTarget), None)
		elif which == "threat":
			pick = next((r for r in recs if r.flags & proto.ActorFlags.QuietThreat), None)
		elif which == "nearest":
			for r in recs:
				if r.flags & (proto.ActorFlags.Dead | proto.ActorFlags.Boss) or not r.flags & proto.ActorFlags.Hostile:
					continue  # the real host's pick: hostile and alive only, never a boss
				d = math.hypot(r.pos[0] - self.root[0], r.pos[2] - self.root[2])
				if pick is None or d < math.hypot(pick.pos[0] - self.root[0], pick.pos[2] - self.root[2]):
					pick = r
		elif which.startswith("0x"):
			want = int(which, 16)
			pick = next((r for r in recs if r.handle == want), None)
		handle = pick.handle if pick is not None else 0
		pos = list(pick.pos) if pick is not None else None
		if mode in ("hit", "released"):
			# one phase now, on the engaged target when no other is named
			if pick is None and which == "pick" and self.quiet_engaged is not None:
				handle = self.quiet_engaged
				pos = next((list(r.pos) for r in recs if r.handle == handle), None)
			if mode == "released":
				self.quiet_job = None
			return 0, self.quiet_send(proto.QuietPhase.Hit if mode == "hit" else proto.QuietPhase.Released, weapon, handle, pos)
		out = []
		# A new order ends the one under way, as MGSV's would: an aim that never fired is lost (cancelled), a shot done is released.
		if self.quiet_job is not None:
			out.append(self.quiet_send(proto.QuietPhase.Lost, self.quiet_job["weapon"], self.quiet_job["handle"], self.quiet_job["pos"],
				int(proto.QuietLostReason.LostCancelled)))
		elif self.quiet_engaged is not None:
			out.append(self.quiet_send(proto.QuietPhase.Released, weapon, self.quiet_engaged, None))
		self.quiet_job = None
		if pick is None or mode == "refused":
			out.append(self.quiet_send(proto.QuietPhase.Refused, weapon, handle, pos))
			return 0, "; ".join(out) + ("" if pick is not None else f" -- no {which} in the ActorTable ({len(recs)} records)")
		out.append(self.quiet_send(proto.QuietPhase.Ordered, weapon, handle, pos))
		t = self.elapsed()
		if mode == "lost":
			self.quiet_job = {"handle": handle, "weapon": weapon, "pos": pos, "steps": [(t + 0.5, proto.QuietPhase.Lost, int(reason))]}
		elif mode == "full":
			hit = t + self.quiet_hit_s
			self.quiet_job = {"handle": handle, "weapon": weapon, "pos": pos,
				"steps": [(hit, proto.QuietPhase.Hit, None), (hit + 0.3, proto.QuietPhase.Released, None)]}
		name = bytes(pick.name).split(b"\0", 1)[0].decode("utf-8", "replace")
		return 0, "; ".join(out) + f" ({name}, {mode})"

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

	def climb_anim_time(self) -> float:
		"""The humanoid's walk cycle as the climb's (or the mantle's) pose: at the climb's pace, still while he holds on the rungs."""
		if self.climbing and self.climb:
			return self.climb["phase"] / (2 * math.pi)
		return self.anim_time()

	def publish_draw_list(self, riding: bool):
		if self.humanoid:
			walker = (0.0, -1.0, 0.0) if riding else (1.6 * math.cos(self.yaw) - 0.4 * math.sin(self.yaw), 0.0, -1.6 * math.sin(self.yaw) - 0.4 * math.cos(self.yaw))
			if self.buddy in ("dog", "quiet") and not riding:
				walker = self.buddy_rel  # its draws where its buddyPos is (Wait, !dog blip; v18 Quiet's post / seat)
			# v13: MGSV's own draws of a grenade in flight, tagged kOwnerProjectile as the real guest's [Capture] ProjectileMeshes does (the
			# host learns the mesh from them and draws it on its flown copy; `!projmesh off`: none).
			projectiles = [((g["pos"][0] - self.root[0], g["pos"][1] - self.root[1], g["pos"][2] - self.root[2]), 9.0 * g["t"])
				for g in self.grenades if not g["landed"]] if self.proj_meshes else []
			draws, mats, _ = self.humanoid.draw(self.climb_anim_time(), self.yaw, walker, projectiles)
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
		if self.buddy in ("dog", "quiet") and not riding:
			walker = self.buddy_rel  # its draws where its buddyPos is (Wait, !dog blip; v18 Quiet's post / seat)
		upper = (0, 0.9, 0)
		if self.climbing and self.climb:
			upper = (0.15 * math.sin(self.climb["phase"]), 1.3, 0)  # the climb pose: arms up, swinging with its pace
		elif self.mantling:
			upper = (0, 1.5, 0.3)  # the climb-up / vault pose: reaching over the ledge
		mats = [matrix_3x4(0, 0, 0, self.yaw), matrix_3x4(*upper, self.yaw),
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
	ap.add_argument("--idroid", action="store_true", help="a Start press opens the iDroid (kGuestInMenu), one in it closes it, as MGSV")
	ap.add_argument("--buddy", choices=tuple(BUDDIES), default="walker", help="the buddy deployed: walker (D-Walker, default), horse (D-Horse), dog, quiet, or none")
	ap.add_argument("--stick", choices=("world", "camera"), default="world", help="the left stick: MGSV x/z (default) or camera-relative like MGSV")
	ap.add_argument("--no-walk", action="store_true", help="start with the circle walk, the periodic fire and the ride cycle off (!walk off, "
		"!autofire off, !ride off): Snake moves only with the stick or !move, and keeps his facing")
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
	guest.buddy, guest.stick_mode = args.buddy, args.stick
	guest.idroid = args.idroid
	if args.no_walk:
		guest.walk, guest.autofire, guest.ride = False, False, "off"
	h = link.header
	h.guestNonce = 0  # a new guest process: the host must treat whatever was attached before as gone
	h.guestUnixPid = os.getpid()
	h.guestProtocol = proto.VERSION  # who is talking, as the real guest says it (Header v11)
	proto.set_text(h, "guestRelease", proto._release())
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
