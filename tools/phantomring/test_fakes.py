"""End-to-end protocol test: fake_mgsv.py (guest) against fake_er.py (host), both native, on a private
mapping in /dev/shm. Checks that data flows both ways: pad -> guest, odometer/pose/events/draw lists/HUD/
render ring -> host, hurt events + actors -> guest, both consoles (through the same code `pr lua` / `pr er`
use), and that a host restart (new session nonce) makes the guest re-attach and resend its meshes.

    python3 tools/phantomring/test_fakes.py
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from phantomring import proto  # noqa: E402


class Checker:
	def __init__(self):
		self.failures = []
		self.count = 0

	def __call__(self, cond, what, detail=""):
		self.count += 1
		print(f"  [{'ok' if cond else 'FAIL'}] {what}{f'  ({detail})' if detail else ''}")
		if not cond:
			self.failures.append(what)


def spawn(module: str, path: str, seconds: float, summary: Path, log: Path) -> subprocess.Popen:
	env = dict(os.environ, PYTHONPATH=str(TOOLS), PHANTOMRING_SHM=path)
	return subprocess.Popen([sys.executable, "-m", f"phantomring.{module}", "--seconds", str(seconds), "--summary", str(summary)],
		env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT)


def pr(path: str, *args) -> subprocess.CompletedProcess:
	env = dict(os.environ, PYTHONPATH=str(TOOLS))
	return subprocess.run([sys.executable, "-m", "phantomring", "--path", path, *args], env=env, capture_output=True, text=True, timeout=30)


def main() -> int:
	path = f"/dev/shm/phantomring_test_{os.getpid()}"
	work = Path(tempfile.mkdtemp(prefix="phantomring_fakes_"))
	check = Checker()
	proto.create_mapping(path)
	procs = []
	try:
		print(f"mapping {path}, logs in {work}")
		# Guest first: it must wait for a host session without publishing anything.
		guest = spawn("fake_mgsv", path, 27.0, work / "mgsv.json", work / "mgsv.log")
		procs.append(guest)
		time.sleep(0.7)
		link = proto.Link(path)
		check(link.header.guestNonce == 0 and link.guest_state() is None, "guest waits for the host (no state before a session)")
		check(link.side("guest")["statusName"] == "Booting", "guest reports Booting while waiting", link.side("guest")["text"])

		host1 = spawn("fake_er", path, 5.0, work / "er1.json", work / "er1.log")
		procs.append(host1)
		time.sleep(2.5)
		hdr = link.snapshot(proto.Header, 0)
		check(hdr.magic == proto.MAGIC and hdr.version == proto.VERSION, "host stamped magic/version")
		check(hdr.guestNonce == hdr.sessionNonce != 0, "guest attached to the host's session")
		check(link.side("host")["alive"] and link.side("guest")["alive"], "both heartbeats alive",
			f"host {link.side('host')['heartbeatAge']}, guest {link.side('guest')['heartbeatAge']}")

		status, resp = proto.console_request(link, "guest", "return vars.buddyType, BuddyType.WALKER_GEAR, vars.playerLifeMax", timeout=3)
		check(status == 0 and resp == "4\t4\t6000", "guest console (Lua subset) answers", repr(resp))
		status, resp = proto.console_request(link, "guest", "return nosuch.field", timeout=3)
		check(status == 1 and "nil" in resp, "guest console reports Lua errors", repr(resp))
		status, resp = proto.console_request(link, "host", "peek player.pos", timeout=3)
		check(status == 0 and len(resp.split()) == 3, "host console answers peek player.pos", repr(resp))
		r = pr(path, "lua", "return", "vars.missionCode")
		check(r.returncode == 0 and r.stdout.strip() == "50050", "`pr lua` round trip", (r.stdout + r.stderr).strip())
		r = pr(path, "lua", "!status")
		check(r.returncode == 0 and "WALKER_GEAR" in r.stdout, "`pr lua '!status'` guest worker command", r.stdout.strip())
		# v13: the binoculars flag the host marks Elden Ring's characters with (er/Marking.h)
		r = pr(path, "lua", "!binoculars on")
		time.sleep(0.2)
		gs = link.guest_state()
		check(r.returncode == 0 and gs is not None and gs.flags & proto.GuestFlags.Binoculars and gs.flags & proto.GuestFlags.FirstPerson,
			"`!binoculars on` sets GuestFlags.Binoculars", (r.stdout + r.stderr).strip())
		pr(path, "lua", "!binoculars off")
		# v16: a suppressor on the weapon in hand (GuestFlags.SuppressorOn), worn out by its last suppressed shot
		r = pr(path, "lua", "!suppressor on 2")
		time.sleep(0.2)
		gs = link.guest_state()
		check(r.returncode == 0 and gs is not None and gs.flags & proto.GuestFlags.SuppressorOn, "`!suppressor on` sets GuestFlags.SuppressorOn",
			(r.stdout + r.stderr).strip())
		r1 = pr(path, "lua", "!fire tranq")
		r2 = pr(path, "lua", "!fire tranq")
		time.sleep(0.2)
		gs = link.guest_state()
		check("suppressed" in r1.stdout and "wore out" in r2.stdout and gs is not None and not gs.flags & proto.GuestFlags.SuppressorOn,
			"two suppressed shots wear `!suppressor on 2` out (the flag drops)", (r1.stdout + r2.stdout).strip())
		r = pr(path, "lua", "!fire rifle 1 0 0 0 suppressed")
		check(r.returncode == 0 and "suppressed" in r.stdout, "`!fire ... suppressed` without a suppressor still marks the shot", r.stdout.strip())
		r = pr(path, "lua", "!fire rifle")
		check(r.returncode == 0 and "suppressed" not in r.stdout, "a plain `!fire` with no suppressor is loud", r.stdout.strip())
		r = pr(path, "er", "ping")
		check(r.returncode == 0 and "pong" in r.stdout, "`pr er` round trip", (r.stdout + r.stderr).strip())
		r = pr(path, "er", "frobnicate")
		check(r.returncode == 1 and "unknown command" in r.stderr, "`pr er` error exit code", r.stderr.strip())
		r = pr(path, "status")
		check(r.returncode == 0 and "ALIVE" in r.stdout and "WALKER_GEAR" in r.stdout and "178 bones" in r.stdout,
			"`pr status` shows both sides, buddy and pose")

		host1.wait(timeout=10)
		time.sleep(0.3)
		check(not link.side("host")["alive"], "host exit is visible (Exiting)", link.side("host")["statusName"])
		r = pr(path, "er", "ping")
		check(r.returncode == 3 and "not running" in r.stderr, "`pr er` with no host says so", r.stderr.strip())
		nonce1 = link.header.sessionNonce

		host2 = spawn("fake_er", path, 19.0, work / "er2.json", work / "er2.log")
		procs.append(host2)
		time.sleep(1.0)
		check(link.header.sessionNonce != nonce1 and link.header.guestNonce == link.header.sessionNonce,
			"guest re-attached to the restarted host's new session")

		# v17 D-Dog: the host's pick (ActorFlags.DogTarget), the attack phases, GuestBuddyFlags.Engaged, BuddyOrder pad[0]
		r = pr(path, "er", "actor 1 dogtarget")
		check(r.returncode == 0 and "DogTarget" in r.stdout, "`pr er actor 1 dogtarget` sets ActorFlags.DogTarget", (r.stdout + r.stderr).strip())
		r = pr(path, "lua", "!dog attack")
		check(r.returncode == 1 and "!buddy dog" in r.stdout + r.stderr, "`!dog attack` needs D-Dog out", (r.stdout + r.stderr).strip())
		pr(path, "lua", "!buddy dog")
		pr(path, "lua", "!dog style kill")
		r = pr(path, "lua", "!order 12")
		check(r.returncode == 0 and "pad0=3" in r.stdout, "`!order 12` with D-Dog: BuddyOrder pad[0] = style + 1", r.stdout.strip())
		r = pr(path, "lua", "!order 9")
		check(r.returncode == 0 and "pad0=0" in r.stdout, "`!order 9` (Wait): no style in pad[0] (only 11-15 carry it)", r.stdout.strip())
		time.sleep(0.2)
		gs = link.guest_state()
		check(gs is not None and proto.buddy_dog_style(gs.buddyFlags) == proto.DogStyle.Kill,
			"buddyFlags bits 8-15: D-Dog's style published every frame (Kill)", hex(gs.buddyFlags) if gs else "-")
		hs = link.host_state()
		check(hs is not None and hs.flags & proto.HostFlags.DogLayer, "HostFlags.DogLayer from the host stand-in (its [Dog] on)",
			proto.flag_names(hs.flags, proto.HostFlags) if hs else "-")
		r = pr(path, "er", "doglayer 0")
		time.sleep(0.1)
		hs = link.host_state()
		check(r.returncode == 0 and hs is not None and not hs.flags & proto.HostFlags.DogLayer, "`pr er doglayer 0` clears HostFlags.DogLayer",
			(r.stdout + r.stderr).strip())
		pr(path, "er", "doglayer 1")
		r = pr(path, "lua", "!dog attack target connect")
		check(r.returncode == 0 and "Ordered/Kill" in r.stdout and "Connected/Kill" in r.stdout and "0xffffffff00001001" in r.stdout,
			"`!dog attack target connect`: Ordered + Connected on the host's pick", (r.stdout + r.stderr).strip())
		time.sleep(0.2)
		gs = link.guest_state()
		check(gs is not None and gs.buddyFlags & proto.GuestBuddyFlags.Engaged and gs.buddyType == proto.BuddyType.DOG,
			"GuestBuddyFlags.Engaged while the dog bites", proto.flag_names(gs.buddyFlags, proto.GuestBuddyFlags) if gs else "-")
		r = pr(path, "lua", "!dog release")
		time.sleep(0.2)
		gs = link.guest_state()
		check(r.returncode == 0 and "Released" in r.stdout and gs is not None and not gs.buddyFlags & proto.GuestBuddyFlags.Engaged,
			"`!dog release`: Released, Engaged off", (r.stdout + r.stderr).strip())
		r = pr(path, "lua", "!dog attack nearest wound lost")
		check(r.returncode == 0 and "Ordered/Wound" in r.stdout, "`!dog attack nearest wound lost`", (r.stdout + r.stderr).strip())
		r = pr(path, "lua", "!dog attack none busy")
		check(r.returncode == 0 and "Refused/Busy" in r.stdout and "0x0" in r.stdout, "`!dog attack none`: Refused, handle 0", (r.stdout + r.stderr).strip())
		pr(path, "lua", "!dog busy 0.5")
		r = pr(path, "lua", "!dog attack target knockout")
		check(r.returncode == 0 and "Ordered/KnockOut" in r.stdout, "`!dog attack target knockout` (a full run)", (r.stdout + r.stderr).strip())
		time.sleep(1.6)
		# v18 Quiet and the grace rest: nothing published until the host has it (seqs 0, NaN), her object id, weapon, orders, picks
		hs = link.host_state()
		check(hs is not None and not hs.flags & (proto.HostFlags.QuietLayer | proto.HostFlags.AtGrace) and hs.graceSeq == 0 and hs.buddyPostSeq == 0
			and all(v != v for v in list(hs.buddyPost) + list(hs.graceSeat) + list(hs.graceCentre)),
			"v18 HostState: no QuietLayer / AtGrace, grace and post NaN with seq 0 until asked", proto.flag_names(hs.flags, proto.HostFlags) if hs else "-")
		r = pr(path, "lua", "!quiet shot")
		check(r.returncode == 1 and "!buddy quiet" in r.stdout + r.stderr, "`!quiet shot` needs Quiet out", (r.stdout + r.stderr).strip())
		pr(path, "lua", "!buddy quiet")
		pr(path, "lua", "!quiet weapon heavy")
		r1 = pr(path, "lua", "!order 1")
		r2 = pr(path, "lua", "!order 5")
		check("pad0=3" in r1.stdout and "pad0=0" in r2.stdout, "Quiet's `!order 1` carries her weapon + 1 in pad[0], `!order 5` none",
			(r1.stdout + r2.stdout).strip())
		gs = link.guest_state()
		check(gs is not None and gs.buddyObjectId == 0x2400 and gs.buddyType == proto.BuddyType.QUIET and
			proto.buddy_quiet_weapon(gs.buddyFlags) == proto.QuietWeapon.Heavy, "Quiet: object 0x2400, buddyFlags bits 17-19 = Heavy",
			f"{gs.buddyObjectId:#x} {gs.buddyFlags:#x}" if gs else "-")
		r = pr(path, "er", "actor 0 quiettarget")
		check(r.returncode == 0 and "QuietTarget" in r.stdout, "`pr er actor 0 quiettarget` sets ActorFlags.QuietTarget", (r.stdout + r.stderr).strip())
		pr(path, "lua", "!quiet hit 0.3")
		r = pr(path, "lua", "!quiet shot")
		check(r.returncode == 0 and "Ordered/Heavy" in r.stdout and "0xffffffff00001000" in r.stdout, "`!quiet shot`: Ordered on the host's pick",
			(r.stdout + r.stderr).strip())
		time.sleep(0.15)
		gs = link.guest_state()
		check(gs is not None and gs.buddyFlags & proto.GuestBuddyFlags.Engaged, "GuestBuddyFlags.Engaged from Ordered (Quiet aims)",
			proto.flag_names(gs.buddyFlags, proto.GuestBuddyFlags) if gs else "-")
		time.sleep(0.8)
		r = pr(path, "lua", "!quiet shot none")
		check(r.returncode == 0 and "Refused/Heavy" in r.stdout, "`!quiet shot none`: Refused, handle 0", (r.stdout + r.stderr).strip())
		r = pr(path, "lua", "!quiet shot pick lost target tranq")
		check(r.returncode == 0 and "Ordered/Tranq" in r.stdout, "`!quiet shot pick lost target tranq`", (r.stdout + r.stderr).strip())
		time.sleep(0.8)
		# her post: only with HostFlags.QuietLayer; echoed in GuestState.buddyPostSeq with AtPost; no post = cloaked (on the move)
		pr(path, "lua", "!quiet post on")
		r = pr(path, "er", "post 20 5 90")
		time.sleep(0.25)
		gs = link.guest_state()
		check(r.returncode == 0 and gs is not None and not gs.buddyFlags & proto.GuestBuddyFlags.AtPost and gs.buddyPostSeq == 0,
			"a post without HostFlags.QuietLayer is not held", (r.stdout + r.stderr).strip())
		pr(path, "er", "quietlayer 1")
		time.sleep(0.25)
		gs, hs = link.guest_state(), link.host_state()
		check(gs is not None and hs is not None and hs.flags & proto.HostFlags.QuietLayer and gs.buddyFlags & proto.GuestBuddyFlags.AtPost and
			gs.buddyPostSeq == hs.buddyPostSeq == 1 and abs(gs.buddyPos[0] - hs.buddyPost[0]) < 0.5 and abs(gs.buddyPos[2] - hs.buddyPost[2]) < 0.5,
			"`pr er quietlayer 1` + `post 20 5 90`: Quiet held at the post, seq 1 echoed", f"{list(gs.buddyPos)} vs {list(hs.buddyPost)}" if gs and hs else "-")
		pr(path, "er", "post none")
		time.sleep(0.25)
		gs = link.guest_state()
		check(gs is not None and gs.buddyFlags & proto.GuestBuddyFlags.Cloaked and gs.buddyPostSeq == 0, "`pr er post none`: on the move, cloaked",
			proto.flag_names(gs.buddyFlags, proto.GuestBuddyFlags) if gs else "-")
		# the grace rest: kBuddyCanRest, then the seat echoed in graceSeqSeated with Seated; a rest without a seat isn't sat at
		pr(path, "lua", "!quiet seat on")
		r = pr(path, "er", "grace 1")
		time.sleep(0.3)
		gs, hs = link.guest_state(), link.host_state()
		check(r.returncode == 0 and gs is not None and hs is not None and hs.flags & proto.HostFlags.AtGrace and
			gs.buddyFlags & (proto.GuestBuddyFlags.CanRest | proto.GuestBuddyFlags.Seated) == (proto.GuestBuddyFlags.CanRest | proto.GuestBuddyFlags.Seated) and
			gs.graceSeqSeated == hs.graceSeq == 1 and abs(gs.buddyPos[0] - hs.graceSeat[0]) < 0.5,
			"`pr er grace 1`: Quiet seated at the grace seat, rest 1 echoed", (r.stdout + r.stderr).strip())
		r = pr(path, "status")
		check(r.returncode == 0 and "grace #1" in r.stdout and "seated for rest #1" in r.stdout, "`pr status` shows the grace and the echo",
			r.stdout.strip()[-300:])
		pr(path, "er", "grace 1 none")
		time.sleep(0.25)
		gs = link.guest_state()
		check(gs is not None and not gs.buddyFlags & proto.GuestBuddyFlags.Seated and gs.graceSeqSeated == 0,
			"`pr er grace 1 none` (a rest without a seat: graceSeq stays): not seated", proto.flag_names(gs.buddyFlags, proto.GuestBuddyFlags) if gs else "-")
		pr(path, "er", "grace 0")
		pr(path, "lua", "!quiet cover on")
		pr(path, "lua", "!quiet marked on")
		time.sleep(0.25)
		gs, hs = link.guest_state(), link.host_state()
		check(gs is not None and hs is not None and not hs.flags & proto.HostFlags.AtGrace and
			gs.buddyFlags & (proto.GuestBuddyFlags.QuietCover | proto.GuestBuddyFlags.QuietMarked) == (proto.GuestBuddyFlags.QuietCover | proto.GuestBuddyFlags.QuietMarked),
			"`grace 0` ends AtGrace; `!quiet cover|marked on` set QuietCover / QuietMarked", proto.flag_names(gs.buddyFlags, proto.GuestBuddyFlags) if gs else "-")
		pr(path, "er", "quietlayer 0")
		pr(path, "lua", "!buddy walker")
		host2.wait(timeout=15)
		guest.wait(timeout=15)
		link.close()

		g = json.loads((work / "mgsv.json").read_text())
		e1 = json.loads((work / "er1.json").read_text())
		e2 = json.loads((work / "er2.json").read_text())
		print("guest summary:", json.dumps({k: g[k] for k in ("attaches", "padPackets", "maxThumbLY", "maxThumbLX", "buttonsSeen", "hostEvents", "actorsSeen", "consoleServed", "rideFrames")}))
		print("host#1 summary:", json.dumps({k: e1[k] for k in ("guestStates", "odoDistance", "events", "drawFrames", "renderMeshes", "hudFrames", "poseBones")}))

		# host -> guest
		check(g["attaches"] == 2, "guest attached twice (two host sessions)", g["attaches"])
		check(g["padPackets"] >= 4, "pad packets reach the guest", g["padPackets"])
		check(g["maxThumbLY"] == 32767 and g["maxThumbLX"] == 32767, "scripted sticks reach the guest", f"LY {g['maxThumbLY']} LX {g['maxThumbLX']}")
		check(g["buttonsSeen"] & proto.XButtons.A, "button A reaches the guest", hex(g["buttonsSeen"]))
		check(g["hostStates"] > 300 and g["hostFlagsSeen"] & proto.HostFlags.Puppeting, "HostState reaches the guest", g["hostStates"])
		check(g["hostEvents"].get("Hurt", 0) >= 2, "Hurt events reach the guest", g["hostEvents"])
		check(g["lifeMin"] < 6000, "Hurt lowered Snake's life", g["lifeMin"])
		check(g["actorsSeen"] == 2, "actor table reaches the guest", g["actorsSeen"])
		check(g["consoleServed"] >= 4, "guest served console requests", g["consoleServed"])
		# guest -> host
		for name, e in (("host#1", e1), ("host#2", e2)):
			check(e["guestAttaches"] == 1, f"{name}: saw the guest attach once", e["guestAttaches"])
			check(e["guestStates"] > 150, f"{name}: GuestState frames", e["guestStates"])
			check(e["odoDistance"] > 2.0, f"{name}: odometer moves the puppet", f"{e['odoDistance']:.2f} m")
			check(e["poseBones"] == 178, f"{name}: pose bone count", e["poseBones"])
			check(e["renderClearAll"] == 1 and e["renderMeshes"] == 2 and e["renderTextures"] == 2 and e["meshIndexOk"],
				f"{name}: render ring ClearAll + 2 meshes + 2 textures, parsed", f"{e['renderClearAll']}/{e['renderMeshes']}/{e['renderTextures']}")
			check(e["drawFrames"] > 150 and e["lastDrawCount"] == 2 and e["lastMatrixCount"] == 4, f"{name}: draw list triple buffer",
				f"{e['drawFrames']} frames, {e['lastDrawCount']} draws, {e['lastMatrixCount']} matrices")
			check(set(e["drawOwnersSeen"]) == {"Player", "Buddy"} and e["drawMissingMesh"] == 0, f"{name}: Snake + D-Walker draws reference sent meshes",
				e["drawOwnersSeen"])
			check(e["hudFrames"] > 15 and e["hudLastSize"] == [64, 8], f"{name}: HUD triple buffer", e["hudFrames"])
			check(e["events"].get("Fire", 0) >= 2, f"{name}: Fire events", e["events"])
			check(e["consoleServed"] >= 1 or name == "host#2", f"{name}: host console served", e["consoleServed"])
		check(e1["events"].get("BuddyFire", 0) >= 1 and e2["events"].get("BuddyFire", 0) >= 1, "D-Walker BuddyFire events")
		check(proto.BuddyType.WALKER_GEAR in e2["buddyTypes"], "buddyType WALKER_GEAR published", e2["buddyTypes"])
		bf = proto.GuestBuddyFlags
		check(e2["buddyFlagsSeen"] & (bf.Present | bf.PosValid | bf.Ridden) == (bf.Present | bf.PosValid | bf.Ridden) and e2["buddyPosValid"] > 100,
			"v2 buddy block: present, positioned, ridden", proto.flag_names(e2["buddyFlagsSeen"], bf))
		check(0 < e2["buddyAmmoLast"] < 500, "D-Walker ammo counts down with BuddyFire", e2["buddyAmmoLast"])
		# v17 D-Dog, as host#2 saw it
		want = ["Ordered/Kill", "Connected/Kill", "Released/Kill", "Ordered/Wound", "Lost/Wound", "Refused/Busy", "Ordered/KnockOut",
			"Connected/KnockOut", "Released/KnockOut"]
		check(e2["dogAttacks"] == want, "host#2: kGEvDogAttack phases and styles in order", e2["dogAttacks"])
		check([12, int(proto.BuddyType.DOG), int(proto.DogStyle.Kill) + 1] in e2["buddyOrders"], "host#2: BuddyOrder 12 for D-Dog, style Kill",
			e2["buddyOrders"])
		check(e2["dogEngagedFrames"] > 10 and e2["buddyFlagsSeen"] & proto.GuestBuddyFlags.Engaged, "host#2: GuestBuddyFlags.Engaged seen",
			e2["dogEngagedFrames"])
		check(g["dogTargetsSeen"] == ["0xffffffff00001001"] and g["dogTargetFrames"] > 10, "guest saw ActorFlags.DogTarget on actor 1 only",
			g["dogTargetsSeen"])
		# v18 Quiet, as host#2 saw it
		want = ["Ordered/Heavy", "Hit/Heavy", "Released/Heavy", "Refused/Heavy", "Ordered/Tranq", "Lost/Tranq"]
		check(e2["quietShots"] == want, "host#2: kGEvQuietShot phases and weapons in order", e2["quietShots"])
		check([1, int(proto.BuddyType.QUIET), int(proto.QuietWeapon.Heavy) + 1] in e2["buddyOrders"] and [5, int(proto.BuddyType.QUIET), 0] in e2["buddyOrders"],
			"host#2: Quiet's BuddyOrder 1 with her weapon, 5 without", e2["buddyOrders"])
		check(e2["postSeqEchoed"] == 1 and e2["postEchoFrames"] > 5 and e2["graceSeqEchoed"] == 1 and e2["seatEchoFrames"] > 5 and
			"Heavy" in e2["quietWeaponsSeen"], "host#2: the post and the seat echoed (seq 1 each), her weapon seen",
			f"post {e2['postSeqEchoed']}/{e2['postEchoFrames']} seat {e2['graceSeqEchoed']}/{e2['seatEchoFrames']} {e2['quietWeaponsSeen']}")
		check(g["quietTargetsSeen"] == ["0xffffffff00001000"] and g["quietTargetFrames"] > 10 and g["hostEvents"].get("Rested", 0) >= 2,
			"guest saw ActorFlags.QuietTarget on actor 0 only, and a kHEvRested per `grace 1`", f"{g['quietTargetsSeen']} {g['hostEvents']}")
		check(e2["guestFlagsSeen"] & proto.GuestFlags.OnVehicle and 0x402 in e2["vehicleIdsSeen"], "riding D-Walker: OnVehicle + vehicleId",
			e2["vehicleIdsSeen"])
	except Exception as ex:  # report, then clean up
		check(False, f"test crashed: {type(ex).__name__}: {ex}")
	finally:
		for p in procs:
			if p.poll() is None:
				p.terminate()
		for p in procs:
			try:
				p.wait(timeout=5)
			except subprocess.TimeoutExpired:
				p.kill()
		try:
			os.unlink(path)
		except OSError:
			pass
	print(f"{check.count - len(check.failures)}/{check.count} checks passed")
	if check.failures:
		print("FAIL:", "; ".join(check.failures))
		print(f"logs kept in {work}")
		return 1
	print("PASS")
	shutil.rmtree(work, ignore_errors=True)
	return 0


if __name__ == "__main__":
	sys.exit(main())
