"""ctypes mirror of protocol/phantomring_protocol.h (protocol v2) plus the Linux side of the transport.

The header is the source of truth. test_layout.py compiles a C++ dump of every struct below against the
header (and runs build/layout_dump.exe) under wine and fails on any difference, so keep the two in sync
and bump VERSION together with kVersion.

Everything here is stdlib only. The mapping is a plain file on /dev/shm that both games map through
Wine (Z:\\dev\\shm\\phantomring_v1), so native Python mmaps the same pages.

Memory ordering: x86-64 keeps stores in program order and loads in program order, and every Python
statement here is a separate C call, so plain aligned loads/stores give the release/acquire ordering
the seqlock and SPSC rings need. The one read-modify-write the protocol needs (the triple buffer's
exchange) goes through a 5-byte `xchg` stub, see _xchg32.
"""

from __future__ import annotations

import ctypes
import enum
import mmap
import os
import secrets
import sys
import time
from ctypes import c_char, c_double, c_float, c_int16, c_int32, c_int64, c_uint8, c_uint16, c_uint32, c_uint64

# ---- constants (namespace phantom::proto) ---------------------------------------------------------
MAGIC = 0x47524850  # "PHRG"
VERSION = 12  # v2: GuestState buddy fields (0x90..0xA8); v3: HostEventType.Heal; v4: AmmoKind.Smoke; v5: Throw/Placed/PlacedGone/Detonate;
				# v6: GuestState aimThrowPos/Vel/Kind (0xA8..0xC4) + GuestFlags.ThrowAim; v7: GuestEventType.BuddyOrder; v8: GuestFlags.InBox/BoxPoster, Fire pad[1] = damage share;
				# v9: PLACED_AGAIN (Placed pad[0]: an announcement repeated); v10: GuestFlags.Nvg, HostEventType.Rested/Tarnished;
				# v11: Header guestProtocol/hostRelease/guestRelease (0xE0..0x100: plugin versions, readable on a mismatch); v12: GuestEventType.Fulton
DEFAULT_PATH = "/dev/shm/phantomring_v1"
DEFAULT_MAPPING_WIN = "file:Z:\\dev\\shm\\phantomring_v1"

OFF_HEADER = 0x0
OFF_HOST_STATE = 0x100
OFF_GUEST_STATE = 0x200
OFF_PAD = 0x400
OFF_POSE = 0x1000
OFF_GUEST_EVENTS = 0x4000
OFF_HOST_EVENTS = 0xB000
OFF_ACTORS = 0x12000
OFF_GUEST_CONSOLE = 0x18000
OFF_HOST_CONSOLE = 0x1A000
OFF_DRAW_CTL = 0x1C000
OFF_HUD_CTL = 0x1D000
OFF_RENDER_RING = 0x100000
RENDER_RING_BYTES = 96 << 20
OFF_DRAW_SLOTS = OFF_RENDER_RING + RENDER_RING_BYTES
DRAW_SLOT_BYTES = 4 << 20
OFF_HUD_SLOTS = OFF_DRAW_SLOTS + 3 * DRAW_SLOT_BYTES
MAX_HUD_W = 3840
MAX_HUD_H = 2160
HUD_SLOT_BYTES = MAX_HUD_W * MAX_HUD_H * 4
MAPPING_BYTES = OFF_HUD_SLOTS + 3 * HUD_SLOT_BYTES

HEARTBEAT_TIMEOUT_MS = 3000
# v9: GuestEvent Placed pad[0] bit -- announced before (the host's puppet restarted, a new host session): v0 is MGSV's point,
# not where it lies in Elden Ring.
PLACED_AGAIN = 1 << 16
MAX_POSE_BONES = 256
EVENT_RING_ENTRIES = 512
MAX_ACTORS = 256
CONSOLE_TEXT_BYTES = 0xFF0
DRAW_TEXTURES = 6
TRIPLE_DIRTY = 1 << 2

REN_RING_HEAD_OFF = 0x00
REN_RING_TAIL_OFF = 0x40
REN_RING_DATA_OFF = 0x80
REN_RING_DATA_BYTES = RENDER_RING_BYTES - REN_RING_DATA_OFF

# Not spelled out as constants in the header, derived from its comments and static_asserts (same
# convention as SkyCraft's overlay): DrawCtl is followed directly by DrawSlotHdr[3]; HudCtl owns a
# 0x40 line and HudSlotHdr[3] follow it. The slot payloads live at OFF_DRAW_SLOTS / OFF_HUD_SLOTS.
OFF_DRAW_SLOT_HDRS = OFF_DRAW_CTL + 0x10
OFF_HUD_SLOT_HDRS = OFF_HUD_CTL + 0x40

# Proton's QueryPerformanceCounter is CLOCK_MONOTONIC_RAW in 100 ns units (VERIFIED in
# docs/research/skycraft-guest-side.md), so native fakes stamp the same clock as the games.
QPC_FREQUENCY = 10_000_000


# Proton's QueryPerformanceCounter tracks CLOCK_MONOTONIC_RAW; system Wine 11 tracks CLOCK_MONOTONIC
# (docs/research/skycraft-guest-side.md §2; re-measured for wine-11.0 by test_plugins.py). The two drift
# apart with NTP slewing (~0.2 s here), so tests that pair the fakes with plugin code under system Wine
# set PHANTOMRING_QPC_CLOCK=monotonic.
_QPC_CLOCK = time.CLOCK_MONOTONIC if os.environ.get("PHANTOMRING_QPC_CLOCK", "raw").lower() == "monotonic" else time.CLOCK_MONOTONIC_RAW


def qpc_now() -> int:
	return time.clock_gettime_ns(_QPC_CLOCK) // 100


# ---- enums ------------------------------------------------------------------------------------------
class Status(enum.IntEnum):
	Absent = 0
	Booting = 1
	Title = 2
	Loading = 3
	Ready = 4
	Failed = 5
	Exiting = 6


class HostFlags(enum.IntFlag):
	InGame = 1 << 0
	MenuOpen = 1 << 1
	Loading = 1 << 2
	Puppeting = 1 << 3
	PlayerDead = 1 << 4
	Cutscene = 1 << 5


class GuestFlags(enum.IntFlag):
	Ready = 1 << 0
	Loading = 1 << 1
	Dead = 1 << 2
	InMenu = 1 << 3
	Cutscene = 1 << 4
	OnVehicle = 1 << 5
	Aiming = 1 << 6
	Sprint = 1 << 7
	Standing = 1 << 8
	Crouching = 1 << 9
	Prone = 1 << 10
	Cqc = 1 << 11
	FirstPerson = 1 << 12
	ThrowAim = 1 << 13  # v6: aimThrowPos/Vel/Kind valid (Snake aims a throwable)
	InBox = 1 << 14  # v8: Snake is inside a cardboard box
	BoxPoster = 1 << 15  # v8: ...a box with a poster (distracts soldiers in MGSV)
	Nvg = 1 << 16  # v10: night-vision goggles on


class GuestBuddyFlags(enum.IntFlag):
	Present = 1 << 0
	PosValid = 1 << 1
	Ridden = 1 << 2
	Loading = 1 << 3
	Missing = 1 << 4


class PadFlags(enum.IntFlag):
	Connected = 1 << 0
	Neutral = 1 << 1


class GuestEventType(enum.IntEnum):
	Fire = 1
	Melee = 2
	Died = 3
	Rumble = 4
	BuddyFire = 5
	Explosion = 6
	Log = 7
	Throw = 8  # v5: v0 release point, v1 velocity (MGSV world), a AmmoKind, b throw id, pad[0] equip | fuse ms << 16
	Placed = 9  # v5: v0 position, v1 facing, a AmmoKind, b placed id, pad[0] equip, pad[1] trigger radius mm
	PlacedGone = 10  # v5: b placed id
	BuddyOrder = 11  # v7: the player's call-menu order: a MGSV buddy command (20 whistle, 21 Normal, 24 Stealth, 25 Search, 26 Intercept), b BuddyType
	Fulton = 12  # v12: pad[0]|pad[1]<<32 ER character handle the proxy stood for, v0 proxy position, a 1 success / 0 failed


class AmmoKind(enum.IntEnum):
	Lethal = 0
	Tranq = 1
	Stun = 2
	Smoke = 3  # v4: kGEvExplosion only (smoke / flare: no damage)


class MeleeKind(enum.IntEnum):
	Punch = 0
	Kick = 1
	Throw = 2
	Choke = 3


class HostEventType(enum.IntEnum):
	Hurt = 1
	Respawn = 2
	Warped = 3
	Status = 4
	Heal = 5  # v3: a = amount in Hurt units (life / LifeScale), b = ER HP healed
	Detonate = 6  # v5: b placed id: an ER enemy is within its trigger radius
	Rested = 7  # v10: a = 0 grace rest, 1 respawn: refill ammo (and life)
	Tarnished = 8  # v10: a = 1 Tarnished mode on, 0 Snake back


class HurtKind(enum.IntEnum):
	Melee = 0
	Projectile = 1
	Magic = 2
	Fall = 3
	Other = 4


class ActorFlags(enum.IntFlag):
	Hostile = 1 << 0
	Dead = 1 << 1
	Boss = 1 << 2
	InCombat = 1 << 3
	Asleep = 1 << 4


class DrawFlags(enum.IntFlag):
	AlphaTest = 1 << 0
	TwoSided = 1 << 1
	Translucent = 1 << 2
	Rigid = 1 << 3


class DrawOwner(enum.IntEnum):
	Player = 0
	Buddy = 1
	Vehicle = 2
	Other = 3


class RenType(enum.IntEnum):
	Pad = 0
	ClearAll = 1
	Mesh = 2
	Texture = 3
	Forget = 4


# MGSV's BuddyType Lua enum (VERIFIED: registration at 0x140A46E21 in mgsvtpp.exe 1.0.15.4).
class BuddyType(enum.IntEnum):
	NONE = 0
	HORSE = 1
	DOG = 2
	QUIET = 3
	WALKER_GEAR = 4
	BATTLE_GEAR = 5
	DISABLE = 7


# XINPUT_GAMEPAD wButtons
class XButtons(enum.IntFlag):
	DPAD_UP = 0x0001
	DPAD_DOWN = 0x0002
	DPAD_LEFT = 0x0004
	DPAD_RIGHT = 0x0008
	START = 0x0010
	BACK = 0x0020
	LEFT_THUMB = 0x0040
	RIGHT_THUMB = 0x0080
	LEFT_SHOULDER = 0x0100
	RIGHT_SHOULDER = 0x0200
	A = 0x1000
	B = 0x2000
	X = 0x4000
	Y = 0x8000


DXGI_FORMAT_R32G32B32_FLOAT = 6
DXGI_FORMAT_R8G8B8A8_UNORM = 28
DXGI_FORMAT_R16_UINT = 57
DXGI_FORMAT_R32G32_FLOAT = 16
D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST = 4


def flag_names(value: int, flags: type[enum.IntFlag]) -> str:
	names = [f.name for f in flags if value & f.value]
	rest = value & ~sum(f.value for f in flags)
	if rest:
		names.append(hex(rest))
	return "|".join(names) if names else "0"


def enum_name(value: int, kind: type[enum.IntEnum]) -> str:
	try:
		return kind(value).name
	except ValueError:
		return f"?{value}"


# ---- structs ----------------------------------------------------------------------------------------
class Struct(ctypes.Structure):
	def as_dict(self) -> dict:
		out = {}
		for name, ctype in self._fields_:
			if name.startswith(("reserved", "pad")):
				continue
			value = getattr(self, name)
			if isinstance(value, bytes):  # ctypes hands char arrays back as bytes
				value = value.split(b"\0", 1)[0].decode("utf-8", "replace")
			elif isinstance(value, ctypes.Array):
				value = list(value)
			out[name] = value
		return out


class Header(Struct):
	_fields_ = [
		("magic", c_uint32),
		("version", c_uint32),
		("sessionNonce", c_uint64),
		("guestNonce", c_uint64),
		("hostUnixPid", c_uint32),
		("guestUnixPid", c_uint32),
		("qpcFrequency", c_int64),
		("hostHeartbeatQpc", c_int64),
		("guestHeartbeatQpc", c_int64),
		("hostStatus", c_uint32),
		("guestStatus", c_uint32),
		("hostStatusText", c_char * 64),
		("guestStatusText", c_char * 96),
		("guestProtocol", c_uint32),     # v11: the guest's VERSION, written even while it refuses to attach (0: older guest)
		("hostRelease", c_char * 14),    # v11: each plugin's VERSION file, e.g. b"0.2.0-alpha"
		("guestRelease", c_char * 14),
	]


class HostState(Struct):
	_fields_ = [
		("seq", c_uint32),
		("flags", c_uint32),
		("frame", c_uint64),
		("qpc", c_int64),
		("timeOfDayHours", c_float),
		("cameraMaxDistance", c_float),
		("erHpFrac", c_float),
		("erWorldId", c_uint32),
		("groundY", c_float),
		("fallSpeed", c_float),
		("reserved", c_uint8 * (0x100 - 0x30)),
	]


class GuestState(Struct):
	_fields_ = [
		("seq", c_uint32),
		("flags", c_uint32),
		("frame", c_uint64),
		("qpc", c_int64),
		("missionCode", c_uint16),
		("locationCode", c_uint16),
		("odoEpoch", c_uint32),
		("odo", c_double * 3),
		("rootPos", c_float * 3),
		("rootYaw", c_float),
		("velocity", c_float * 3),
		("camPos", c_float * 3),
		("camQuat", c_float * 4),
		("camFovY", c_float),
		("camNear", c_float),
		("life", c_uint16),
		("lifeMax", c_uint16),
		("stamina", c_uint16),
		("stance", c_uint16),
		("weaponId", c_uint16),
		("ammoInClip", c_uint16),
		("ammoStock", c_uint16),
		("buddyType", c_uint16),
		("vehicleId", c_uint32),
		("phase", c_uint32),
		# v2: the battle buddy MGSV is running (D-Walker first)
		("buddyFlags", c_uint32),
		("buddyObjectId", c_uint16),
		("buddyAmmo", c_uint16),
		("buddyPos", c_float * 3),
		("buddyYaw", c_float),
		("aimThrowPos", c_float * 3),  # v6
		("aimThrowVel", c_float * 3),
		("aimThrowKind", c_uint32),
		("reserved", c_uint8 * (0x200 - 0xC4)),
	]


class PadState(Struct):
	_fields_ = [
		("seq", c_uint32),
		("flags", c_uint32),
		("packet", c_uint32),
		("buttons", c_uint16),
		("leftTrigger", c_uint8),
		("rightTrigger", c_uint8),
		("thumbLX", c_int16),
		("thumbLY", c_int16),
		("thumbRX", c_int16),
		("thumbRY", c_int16),
		("rumbleLow", c_uint16),
		("rumbleHigh", c_uint16),
		("reserved", c_uint8 * (0x40 - 0x1C)),
	]


class PoseBone(Struct):
	_fields_ = [("rot", c_float * 4), ("pos", c_float * 4)]


class PoseBlock(Struct):
	_fields_ = [
		("seq", c_uint32),
		("boneCount", c_uint32),
		("frame", c_uint64),
		("root", c_float * 16),
		("parent", c_int16 * MAX_POSE_BONES),
		("pad", c_uint8 * (0x60 - 0x50)),
		("bones", PoseBone * MAX_POSE_BONES),
	]


class GuestEvent(Struct):
	_fields_ = [
		("type", c_uint32),
		("a", c_uint32),
		("b", c_uint32),
		("frame", c_uint32),
		("v0", c_float * 3),
		("v1", c_float * 3),
		("pad", c_uint32 * 2),
	]


class HostEvent(Struct):
	_fields_ = list(GuestEvent._fields_)


class ActorRecord(Struct):
	_fields_ = [
		("handle", c_uint64),
		("flags", c_uint32),
		("pos", c_float * 3),
		("yaw", c_float),
		("radius", c_float),
		("height", c_float),
		("hpFrac", c_float),
		("name", c_char * 24),
	]


class ActorTable(Struct):
	_fields_ = [
		("seq", c_uint32),
		("count", c_uint32),
		("pad", c_uint8 * (0x40 - 8)),
		("actors", ActorRecord * MAX_ACTORS),
	]


class Console(Struct):
	_fields_ = [
		("reqSeq", c_uint32),
		("respSeq", c_uint32),
		("status", c_uint32),
		("pad", c_uint32),
		("request", c_char * CONSOLE_TEXT_BYTES),
		("response", c_char * 0x1000),
	]


class DrawCtl(Struct):
	_fields_ = [("state", c_uint32), ("pad", c_uint32), ("framesPublished", c_uint64)]


class DrawSlotHdr(Struct):
	_fields_ = [("frame", c_uint64), ("bytes", c_uint32), ("pad", c_uint32)]


class DrawRecord(Struct):
	_fields_ = [
		("mesh", c_uint64),
		("textures", c_uint64 * DRAW_TEXTURES),
		("indexCount", c_uint32),
		("startIndex", c_uint32),
		("baseVertex", c_int32),
		("firstMatrix", c_uint32),
		("matrixCount", c_uint32),
		("flags", c_uint32),
		("owner", c_uint32),
		("material", c_uint32),
		("params", c_float * 8),
		("pad", c_uint32 * 2),
	]


class DrawList(Struct):
	_fields_ = [
		("frame", c_uint64),
		("drawCount", c_uint32),
		("matrixCount", c_uint32),
		("anchorPos", c_float * 3),
		("anchorYaw", c_float),
		("pad", c_uint8 * (0x40 - 0x20)),
	]


class RenMsgHeader(Struct):
	_fields_ = [("type", c_uint32), ("payloadBytes", c_uint32)]


class RenInputElement(Struct):
	_fields_ = [
		("semantic", c_char * 16),
		("semanticIndex", c_uint32),
		("format", c_uint32),
		("inputSlot", c_uint32),
		("alignedByteOffset", c_uint32),
	]


class RenMesh(Struct):
	_fields_ = [
		("key", c_uint64),
		("elementCount", c_uint32),
		("vertexStride", c_uint32),
		("vertexBytes", c_uint32),
		("indexFormat", c_uint32),
		("indexBytes", c_uint32),
		("topology", c_uint32),
		("streams", c_uint32),
		("streamStride", c_uint32 * 4),
		("streamBytes", c_uint32 * 4),
		("pad", c_uint32 * 3),
	]


class RenTexture(Struct):
	_fields_ = [
		("key", c_uint64),
		("format", c_uint32),
		("width", c_uint32),
		("height", c_uint32),
		("mipLevels", c_uint32),
		("arraySize", c_uint32),
		("dataBytes", c_uint32),
	]


class RenForget(Struct):
	_fields_ = [("key", c_uint64)]


class HudCtl(Struct):
	_fields_ = [("state", c_uint32), ("pad", c_uint32), ("framesPublished", c_uint64)]


class HudSlotHdr(Struct):
	_fields_ = [
		("width", c_uint32),
		("height", c_uint32),
		("flags", c_uint32),
		("pad", c_uint32),
		("frame", c_uint64),
		("reserved", c_uint8 * (0x40 - 0x18)),
	]


EVENT_RING_BYTES = 0x80 + ctypes.sizeof(GuestEvent) * EVENT_RING_ENTRIES

# Every struct test_layout.py checks field by field against the C++ header.
ALL_STRUCTS = [
	Header, HostState, GuestState, PadState, PoseBone, PoseBlock, GuestEvent, HostEvent, ActorRecord,
	ActorTable, Console, DrawCtl, DrawSlotHdr, DrawRecord, DrawList, RenMsgHeader, RenInputElement,
	RenMesh, RenTexture, RenForget, HudCtl, HudSlotHdr,
]

# Every scalar constant test_layout.py checks (python name -> C++ expression).
ALL_CONSTANTS = {
	"MAGIC": "kMagic", "VERSION": "kVersion",
	"OFF_HEADER": "kOffHeader", "OFF_HOST_STATE": "kOffHostState", "OFF_GUEST_STATE": "kOffGuestState",
	"OFF_PAD": "kOffPad", "OFF_POSE": "kOffPose", "OFF_GUEST_EVENTS": "kOffGuestEvents",
	"OFF_HOST_EVENTS": "kOffHostEvents", "OFF_ACTORS": "kOffActors", "OFF_GUEST_CONSOLE": "kOffGuestConsole",
	"OFF_HOST_CONSOLE": "kOffHostConsole", "OFF_DRAW_CTL": "kOffDrawCtl", "OFF_HUD_CTL": "kOffHudCtl",
	"OFF_RENDER_RING": "kOffRenderRing", "RENDER_RING_BYTES": "kRenderRingBytes",
	"OFF_DRAW_SLOTS": "kOffDrawSlots", "DRAW_SLOT_BYTES": "kDrawSlotBytes", "OFF_HUD_SLOTS": "kOffHudSlots",
	"MAX_HUD_W": "kMaxHudW", "MAX_HUD_H": "kMaxHudH", "HUD_SLOT_BYTES": "kHudSlotBytes",
	"MAPPING_BYTES": "kMappingBytes", "HEARTBEAT_TIMEOUT_MS": "kHeartbeatTimeoutMs", "PLACED_AGAIN": "kPlacedAgain",
	"MAX_POSE_BONES": "kMaxPoseBones", "EVENT_RING_ENTRIES": "kEventRingEntries", "MAX_ACTORS": "kMaxActors",
	"CONSOLE_TEXT_BYTES": "kConsoleTextBytes", "DRAW_TEXTURES": "kDrawTextures",
	"REN_RING_HEAD_OFF": "kRenRingHeadOff", "REN_RING_TAIL_OFF": "kRenRingTailOff",
	"REN_RING_DATA_OFF": "kRenRingDataOff", "REN_RING_DATA_BYTES": "kRenRingDataBytes",
	"EVENT_RING_BYTES": "kEventRingBytes",
}

# Every enum test_layout.py checks (python enum -> (C++ enum scope or prefix, C++ prefix of each value)).
ALL_ENUMS = {
	Status: ("Status::", "k"),
	HostFlags: ("", "kHost"),
	GuestFlags: ("", "kGuest"),
	GuestBuddyFlags: ("", "kBuddy"),
	PadFlags: ("", "kPad"),
	GuestEventType: ("", "kGEv"),
	AmmoKind: ("", "kAmmo"),
	MeleeKind: ("", "kMelee"),
	HostEventType: ("", "kHEv"),
	HurtKind: ("", "kHurt"),
	ActorFlags: ("", "kActor"),
	DrawFlags: ("", "kDraw"),
	DrawOwner: ("", "kOwner"),
	RenType: ("", "kRen"),
}


def sizeof(t) -> int:
	return ctypes.sizeof(t)


# ---- atomic exchange ------------------------------------------------------------------------------
# The triple buffer needs a real atomic exchange on a word other processes write too. Python has no
# such primitive, so map a 5-byte stub: mov eax, esi; xchg [rdi], eax; ret (xchg with memory is
# implicitly locked).
_xchg_fn = None
_xchg_page = None
_xchg_warned = False


def _make_xchg():
	global _xchg_fn, _xchg_page
	try:
		page = mmap.mmap(-1, mmap.PAGESIZE, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
		page[:5] = bytes((0x89, 0xF0, 0x87, 0x07, 0xC3))
		addr = ctypes.addressof(ctypes.c_char.from_buffer(page))
		_xchg_fn = ctypes.CFUNCTYPE(c_uint32, ctypes.c_void_p, c_uint32)(addr)
		_xchg_page = page
	except (OSError, ValueError, TypeError):
		_xchg_fn = None


_make_xchg()


def xchg32(word: ctypes.c_uint32, value: int) -> int:
	"""Atomically store value into word (a ctypes view into the mapping) and return the old value."""
	global _xchg_warned
	if _xchg_fn is not None:
		return _xchg_fn(ctypes.addressof(word), value)
	if not _xchg_warned:
		_xchg_warned = True
		print("phantomring.proto: no executable page for xchg; triple buffer exchange is NOT atomic")
	old = word.value
	word.value = value
	return old


# ---- the mapping ----------------------------------------------------------------------------------
def default_path() -> str:
	return os.environ.get("PHANTOMRING_SHM", DEFAULT_PATH)


def create_mapping(path: str | None = None, zero: bool = True) -> None:
	"""Create (or reset) the mapping file at its full size. zero=True drops all previous contents
	(truncate to 0, then extend: tmpfs pages come back zeroed and sparse)."""
	path = path or default_path()
	fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
	try:
		if zero:
			os.ftruncate(fd, 0)
		if os.fstat(fd).st_size < MAPPING_BYTES:
			os.ftruncate(fd, MAPPING_BYTES)
	finally:
		os.close(fd)


class MappingMissing(Exception):
	pass


class Link:
	"""An mmap of the protocol file with typed views of every fixed region."""

	def __init__(self, path: str | None = None, create: bool = False, zero: bool = False):
		self.path = path or default_path()
		if create:
			create_mapping(self.path, zero=zero)
		try:
			fd = os.open(self.path, os.O_RDWR)
		except FileNotFoundError:
			raise MappingMissing(self.path) from None
		try:
			size = os.fstat(fd).st_size
			if size < MAPPING_BYTES:
				raise MappingMissing(f"{self.path} is {size} bytes, protocol v{VERSION} needs {MAPPING_BYTES}")
			self.mm = mmap.mmap(fd, MAPPING_BYTES, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
		finally:
			os.close(fd)
		mm = self.mm
		self.header = Header.from_buffer(mm, OFF_HEADER)
		self.guest_console = Console.from_buffer(mm, OFF_GUEST_CONSOLE)
		self.host_console = Console.from_buffer(mm, OFF_HOST_CONSOLE)
		self.draw_ctl = DrawCtl.from_buffer(mm, OFF_DRAW_CTL)
		self.hud_ctl = HudCtl.from_buffer(mm, OFF_HUD_CTL)
		self.guest_events = EntryRing(mm, OFF_GUEST_EVENTS, GuestEvent, EVENT_RING_ENTRIES)
		self.host_events = EntryRing(mm, OFF_HOST_EVENTS, HostEvent, EVENT_RING_ENTRIES)
		self.render_ring = ByteRing(mm, OFF_RENDER_RING, RENDER_RING_BYTES)

	def close(self):
		# Views hold buffer exports on the mmap; drop them first.
		for name in ("header", "guest_console", "host_console", "draw_ctl", "hud_ctl", "guest_events", "host_events", "render_ring"):
			obj = self.__dict__.pop(name, None)
			if hasattr(obj, "release"):
				obj.release()
		try:
			self.mm.close()
		except BufferError:
			pass

	# -- raw helpers
	def u32(self, off: int) -> int:
		return int.from_bytes(self.mm[off:off + 4], "little")

	def u64(self, off: int) -> int:
		return int.from_bytes(self.mm[off:off + 8], "little")

	def snapshot(self, t, off: int):
		return t.from_buffer_copy(self.mm, off)

	# -- seqlocked slots
	def seq_read(self, t, off: int, attempts: int = 64):
		"""Consistent copy of a seqlocked slot, or None (never written / writer kept writing)."""
		size = ctypes.sizeof(t)
		for _ in range(attempts):
			s1 = self.u32(off)
			if s1 == 0:
				return None
			if s1 & 1:
				time.sleep(0)
				continue
			raw = self.mm[off:off + size]
			if self.u32(off) == s1:
				return t.from_buffer_copy(raw)
		return None

	def seq_write(self, off: int, value) -> None:
		"""Single-writer seqlock store: seq odd while the payload is copied."""
		seq = c_uint32.from_buffer(self.mm, off)
		try:
			s = seq.value
			if s & 1:  # a previous writer died mid-write
				s += 1
			seq.value = s + 1
			payload = bytes(value)
			self.mm[off + 4:off + len(payload)] = payload[4:]
			seq.value = s + 2
		finally:
			del seq

	def host_state(self):
		return self.seq_read(HostState, OFF_HOST_STATE)

	def guest_state(self):
		return self.seq_read(GuestState, OFF_GUEST_STATE)

	def pad_state(self):
		return self.seq_read(PadState, OFF_PAD)

	def pose(self):
		return self.seq_read(PoseBlock, OFF_POSE)

	def actors(self):
		return self.seq_read(ActorTable, OFF_ACTORS)

	# -- liveness
	def side(self, which: str) -> dict:
		"""Summary of one side ('host' or 'guest') from the header."""
		h = self.snapshot(Header, OFF_HEADER)
		status = getattr(h, f"{which}Status")
		pid = getattr(h, f"{which}UnixPid")
		beat = getattr(h, f"{which}HeartbeatQpc")
		text = bytes(getattr(h, f"{which}StatusText")).split(b"\0", 1)[0].decode("utf-8", "replace")
		freq = h.qpcFrequency or QPC_FREQUENCY
		age = None
		if beat:
			# The games' QPC and ours are the same clock under Proton; a big or negative age means they
			# are not (e.g. a game on real Windows, or a non-Proton Wine with a TSC QPC).
			now = qpc_now() * freq // QPC_FREQUENCY
			a = (now - beat) / freq
			age = a if -1.0 < a < 7 * 86400 else None
		pid_alive = bool(pid) and os.path.exists(f"/proc/{pid}")
		alive = Status(status) not in (Status.Absent, Status.Exiting) if status in Status._value2member_map_ else False
		if age is not None:
			alive = alive and age * 1000 < HEARTBEAT_TIMEOUT_MS
		elif beat == 0:
			alive = False
		if pid and not pid_alive:
			alive = False
		return {
			"status": status, "statusName": enum_name(status, Status), "text": text, "pid": pid,
			"pidAlive": pid_alive, "heartbeat": beat, "heartbeatAge": age, "alive": alive,
		}

	def other_alive(self, which: str) -> dict | None:
		"""side(which) if some *other* live process holds that side, else None. The stand-ins (fake_er,
		recorder, fake_mgsv, replay) check this before taking a side over: the header, the seqlocked
		blocks and the rings are single-writer per side, and the real ER host stamps its nonce once, so a
		second host stamping over it leaves ER without its guest for good."""
		s = self.side(which)
		return s if s["alive"] and s["pid"] != os.getpid() else None

	def refuse_if_taken(self, which: str, prog: str, path: str, force: bool) -> bool:
		"""True (after printing why) when another live process holds `which` and --force was not given."""
		s = self.other_alive(which)
		if s is None or force:
			return False
		print(f"{prog}: a {which} is alive on {path} (pid {s['pid'] or '?'}, {s['statusName']} {s['text']!r}); "
			"stop it or pass --force", file=sys.stderr)
		return True

	def stamp_host(self, status: Status, text: str = "") -> int:
		"""What the ER host does on attach: magic/version, a fresh nonce, its unix pid."""
		h = self.header
		nonce = secrets.randbits(64) or 1
		h.qpcFrequency = QPC_FREQUENCY
		h.hostUnixPid = os.getpid()
		h.hostHeartbeatQpc = qpc_now()
		h.hostStatus = int(status)
		set_text(h, "hostStatusText", text)
		set_text(h, "hostRelease", _release())
		h.version = VERSION
		h.sessionNonce = nonce
		h.magic = MAGIC
		return nonce

	def triple_writer(self, state_off: int) -> "TripleWriter":
		return TripleWriter(c_uint32.from_buffer(self.mm, state_off))

	def triple_reader(self, state_off: int) -> "TripleReader":
		return TripleReader(c_uint32.from_buffer(self.mm, state_off))

	# -- draw slots (guest -> host)
	def draw_slot_hdr(self, index: int) -> DrawSlotHdr:
		return DrawSlotHdr.from_buffer(self.mm, OFF_DRAW_SLOT_HDRS + index * ctypes.sizeof(DrawSlotHdr))

	def write_draw_slot(self, index: int, frame: int, payload: bytes) -> None:
		if len(payload) > DRAW_SLOT_BYTES:
			raise ValueError("draw list larger than a slot")
		off = OFF_DRAW_SLOTS + index * DRAW_SLOT_BYTES
		self.mm[off:off + len(payload)] = payload
		hdr = DrawSlotHdr(frame=frame, bytes=len(payload))
		o = OFF_DRAW_SLOT_HDRS + index * ctypes.sizeof(DrawSlotHdr)
		self.mm[o:o + ctypes.sizeof(DrawSlotHdr)] = bytes(hdr)

	def read_draw_slot(self, index: int) -> tuple[DrawSlotHdr, DrawList, list[DrawRecord], list[tuple[float, ...]]]:
		hdr = DrawSlotHdr.from_buffer_copy(self.mm, OFF_DRAW_SLOT_HDRS + index * ctypes.sizeof(DrawSlotHdr))
		return (hdr, *parse_draw_list(self.mm, OFF_DRAW_SLOTS + index * DRAW_SLOT_BYTES, min(hdr.bytes, DRAW_SLOT_BYTES)))

	# -- HUD slots (guest -> host)
	def write_hud_slot(self, index: int, width: int, height: int, frame: int, rgba: bytes, premultiplied: bool = True) -> None:
		if width > MAX_HUD_W or height > MAX_HUD_H or len(rgba) != width * height * 4:
			raise ValueError("bad HUD image size")
		off = OFF_HUD_SLOTS + index * HUD_SLOT_BYTES
		self.mm[off:off + len(rgba)] = rgba
		hdr = HudSlotHdr(width=width, height=height, flags=1 if premultiplied else 0, frame=frame)
		o = OFF_HUD_SLOT_HDRS + index * ctypes.sizeof(HudSlotHdr)
		self.mm[o:o + ctypes.sizeof(HudSlotHdr)] = bytes(hdr)

	def read_hud_slot(self, index: int) -> tuple[HudSlotHdr, bytes]:
		hdr = HudSlotHdr.from_buffer_copy(self.mm, OFF_HUD_SLOT_HDRS + index * ctypes.sizeof(HudSlotHdr))
		w, h = min(hdr.width, MAX_HUD_W), min(hdr.height, MAX_HUD_H)
		off = OFF_HUD_SLOTS + index * HUD_SLOT_BYTES
		return hdr, self.mm[off:off + w * h * 4]


def _release() -> str:
	from .version import release  # the VERSION file: the stand-ins say they are this checkout's plugins
	return release()


FIRST_NAMED_RELEASE = "0.2.0-alpha"  # common/Version.h kFirstNamedRelease: the first plugins that fill hostRelease/guestRelease


def header_text(struct_obj, field: str) -> str:
	return bytes(getattr(struct_obj, field)).split(b"\0", 1)[0].decode("ascii", "replace")


def mismatch_text(host_release: str, host_protocol: int, guest_release: str, guest_protocol: int) -> str:
	"""common/Version.cpp MismatchText, word for word (the player sees the C++ one in Elden Ring's corner)."""
	same = host_release == guest_release

	def side(release: str, protocol: int) -> str:
		if not release:
			return f"from before v{FIRST_NAMED_RELEASE}" + (f" (protocol {protocol})" if protocol else "")
		return f"v{release} (protocol {protocol})" if same and protocol else f"v{release}"
	return (f"PhantomRing versions differ: Elden Ring plugin {side(host_release, host_protocol)}, "
		f"MGSV plugin {side(guest_release, guest_protocol)} -- reinstall both")


def version_mismatch(h) -> str | None:
	"""The player-readable text when the host (h.version) and the guest (h.guestProtocol, v11+) speak different
	protocols; None when they match or the guest has not said (an older guest leaves guestProtocol 0)."""
	if h.magic != MAGIC or not h.guestProtocol or h.guestProtocol == h.version:
		return None
	return mismatch_text(header_text(h, "hostRelease"), h.version, header_text(h, "guestRelease"), h.guestProtocol)


def set_text(struct_obj, field: str, text: str) -> None:
	"""Store UTF-8 text into a fixed char array, truncated and always NUL-terminated."""
	size = getattr(type(struct_obj), field).size
	data = text.encode("utf-8")[:size - 1]
	setattr(struct_obj, field, data.ljust(size, b"\0"))


def parse_draw_list(buf, off: int, nbytes: int):
	"""DrawList + DrawRecord[drawCount] + float[matrixCount][12] at buf[off:]."""
	if nbytes < ctypes.sizeof(DrawList):
		return DrawList(), [], []
	dl = DrawList.from_buffer_copy(buf, off)
	pos = off + ctypes.sizeof(DrawList)
	end = off + nbytes
	draws = []
	for _ in range(dl.drawCount):
		if pos + ctypes.sizeof(DrawRecord) > end:
			break
		draws.append(DrawRecord.from_buffer_copy(buf, pos))
		pos += ctypes.sizeof(DrawRecord)
	mats = []
	for _ in range(dl.matrixCount):
		if pos + 48 > end:
			break
		mats.append(tuple((c_float * 12).from_buffer_copy(buf, pos)))
		pos += 48
	return dl, draws, mats


def build_draw_list(frame: int, anchor_pos, anchor_yaw: float, draws: list[DrawRecord], matrices: list) -> bytes:
	dl = DrawList(frame=frame, drawCount=len(draws), matrixCount=len(matrices), anchorYaw=anchor_yaw)
	dl.anchorPos[:] = list(anchor_pos)
	out = bytearray(bytes(dl))
	for d in draws:
		out += bytes(d)
	for m in matrices:
		out += bytes((c_float * 12)(*m))
	return bytes(out)


# ---- SPSC entry ring: {u64 head @0, u64 tail @0x40, T data[N] @0x80} ---------------------------------
class EntryRing:
	HEAD_OFF, TAIL_OFF, DATA_OFF = 0x00, 0x40, 0x80

	def __init__(self, mm: mmap.mmap, off: int, entry_type, entries: int):
		self.mm, self.off, self.t, self.n = mm, off, entry_type, entries
		self.size = ctypes.sizeof(entry_type)
		self._head = c_uint64.from_buffer(mm, off + self.HEAD_OFF)
		self._tail = c_uint64.from_buffer(mm, off + self.TAIL_OFF)

	def release(self):
		del self._head, self._tail

	@property
	def head(self) -> int:
		return self._head.value

	@property
	def tail(self) -> int:
		return self._tail.value

	def pending(self) -> int:
		return max(0, self.head - self.tail)

	def push(self, value) -> bool:
		"""Producer. False when the consumer is a full ring behind (the entry is dropped)."""
		head, tail = self._head.value, self._tail.value
		if head - tail >= self.n:
			return False
		o = self.off + self.DATA_OFF + (head & (self.n - 1)) * self.size
		self.mm[o:o + self.size] = bytes(value)
		self._head.value = head + 1
		return True

	def pop(self):
		"""Consumer. None when empty."""
		head, tail = self._head.value, self._tail.value
		if tail >= head:
			return None
		if head - tail > self.n:
			tail = head - self.n
		o = self.off + self.DATA_OFF + (tail & (self.n - 1)) * self.size
		value = self.t.from_buffer_copy(self.mm, o)
		self._tail.value = tail + 1
		return value

	def drain(self) -> None:
		self._tail.value = self._head.value


# ---- byte ring (render ring): {u64 head @0, u64 tail @0x40, data @0x80} ------------------------------
# head/tail are monotonically increasing byte counts (position = count % dataBytes), as SkyCraft's
# rings. Each message starts 8-byte aligned with RenMsgHeader{type, payloadBytes}; a message that would
# cross the end is preceded by a kRenPad header and starts again at offset 0.
class ByteRing:
	def __init__(self, mm: mmap.mmap, off: int, total_bytes: int):
		self.mm, self.off = mm, off
		self.data_off = off + REN_RING_DATA_OFF
		self.cap = total_bytes - REN_RING_DATA_OFF
		self._head = c_uint64.from_buffer(mm, off + REN_RING_HEAD_OFF)
		self._tail = c_uint64.from_buffer(mm, off + REN_RING_TAIL_OFF)

	def release(self):
		del self._head, self._tail

	@property
	def head(self) -> int:
		return self._head.value

	@property
	def tail(self) -> int:
		return self._tail.value

	def used(self) -> int:
		return max(0, self.head - self.tail)

	def push(self, msg_type: int, payload: bytes) -> bool:
		"""Producer. False if there is no room (the consumer is behind); nothing is written then."""
		msg = (8 + len(payload) + 7) & ~7
		if msg > self.cap:
			raise ValueError("message larger than the ring")
		head, tail = self._head.value, self._tail.value
		pos = head % self.cap
		skip = self.cap - pos if pos + msg > self.cap else 0
		if head + skip + msg - tail > self.cap:
			return False
		if skip:
			struct_bytes = bytes(RenMsgHeader(type=int(RenType.Pad), payloadBytes=skip - 8))
			self.mm[self.data_off + pos:self.data_off + pos + 8] = struct_bytes
			pos = 0
		base = self.data_off + pos
		self.mm[base + 8:base + 8 + len(payload)] = payload
		self.mm[base:base + 8] = bytes(RenMsgHeader(type=int(msg_type), payloadBytes=len(payload)))
		self._head.value = head + skip + msg
		return True

	def pop(self):
		"""Consumer. (type, payload bytes) or None when empty."""
		while True:
			head, tail = self._head.value, self._tail.value
			if tail >= head:
				return None
			pos = tail % self.cap
			h = RenMsgHeader.from_buffer_copy(self.mm, self.data_off + pos)
			if h.type == RenType.Pad:
				self._tail.value = tail + (self.cap - pos)
				continue
			msg = (8 + h.payloadBytes + 7) & ~7
			if pos + msg > self.cap:  # corrupt: resync to the producer
				self._tail.value = head
				return None
			payload = self.mm[self.data_off + pos + 8:self.data_off + pos + 8 + h.payloadBytes]
			self._tail.value = tail + msg
			return h.type, payload

	def drain(self) -> None:
		self._tail.value = self._head.value


def build_mesh_msg(mesh: RenMesh, elements: list[RenInputElement], vertex_bytes: bytes, index_bytes: bytes) -> bytes:
	return bytes(mesh) + b"".join(bytes(e) for e in elements) + vertex_bytes + index_bytes


def parse_mesh_msg(payload: bytes):
	mesh = RenMesh.from_buffer_copy(payload, 0)
	pos = ctypes.sizeof(RenMesh)
	elements = []
	for _ in range(mesh.elementCount):
		elements.append(RenInputElement.from_buffer_copy(payload, pos))
		pos += ctypes.sizeof(RenInputElement)
	vb = payload[pos:pos + mesh.vertexBytes]
	pos += mesh.vertexBytes
	ib = payload[pos:pos + mesh.indexBytes]
	return mesh, elements, vb, ib


# ---- triple buffer (state bits 0-1 = middle slot, bit 2 = unread) ----------------------------------
class TripleWriter:
	def __init__(self, state: ctypes.c_uint32):
		self.state = state
		self.back = 1

	def reset(self) -> None:
		"""Writer side of a (re)attach: middle = 0, writer = 1, reader = 2."""
		self.state.value = 0
		self.back = 1

	def publish(self) -> int:
		old = xchg32(self.state, self.back | TRIPLE_DIRTY)
		self.back = old & 3
		return self.back


class TripleReader:
	def __init__(self, state: ctypes.c_uint32):
		self.state = state
		self.front = 2

	def reset(self) -> None:
		self.front = 2

	def acquire(self) -> bool:
		if not self.state.value & TRIPLE_DIRTY:
			return False
		old = xchg32(self.state, self.front)
		self.front = old & 3
		return True


# ---- dev consoles -------------------------------------------------------------------------------
class ConsoleError(Exception):
	pass


def console_request(link: Link, which: str, text: str, timeout: float = 5.0, force: bool = False) -> tuple[int, str]:
	"""Send a request to the 'guest' (Lua) or 'host' (ER commands) console and wait for the answer.
	Returns (status, response): status 0 ok, 1 error."""
	con = link.guest_console if which == "guest" else link.host_console
	data = text.encode("utf-8")
	if len(data) >= CONSOLE_TEXT_BYTES:
		raise ConsoleError(f"request is {len(data)} bytes; the console takes at most {CONSOLE_TEXT_BYTES - 1}")
	deadline = time.monotonic() + timeout
	# Another request still pending (another tool, or a timed-out one of ours): wait for it first.
	while con.reqSeq != con.respSeq and not force:
		if time.monotonic() > deadline:
			raise ConsoleError(f"console busy: request #{con.reqSeq} still unanswered (answered up to #{con.respSeq}); "
				"the game is not processing its console (use --force to overwrite)")
		time.sleep(0.01)
	seq = (con.reqSeq + 1) & 0xFFFFFFFF
	con.request = data + b"\0"
	con.reqSeq = seq  # release: the request text is stored before the sequence (x86 store order)
	while con.respSeq != seq:
		if time.monotonic() > deadline:
			raise ConsoleError(f"no answer within {timeout:g} s (request #{seq} stays queued; the game will run it when it gets to it)")
		time.sleep(0.005)
	return con.status, bytes(con.response).split(b"\0", 1)[0].decode("utf-8", "replace")


def console_serve(con: Console, handler) -> bool:
	"""Game side: if a request is pending, run handler(text) -> (status, response) and answer it."""
	req = con.reqSeq
	if req == con.respSeq:
		return False
	text = bytes(con.request).split(b"\0", 1)[0].decode("utf-8", "replace")
	try:
		status, response = handler(text)
	except Exception as e:  # a console must never take its game down
		status, response = 1, f"{type(e).__name__}: {e}"
	data = str(response).encode("utf-8")[:0xFFF]
	con.status = status
	con.response = data + b"\0"
	con.respSeq = req
	return True
