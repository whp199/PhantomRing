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
		guest = spawn("fake_mgsv", path, 14.0, work / "mgsv.json", work / "mgsv.log")
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

		host2 = spawn("fake_er", path, 7.5, work / "er2.json", work / "er2.log")
		procs.append(host2)
		time.sleep(1.0)
		check(link.header.sessionNonce != nonce1 and link.header.guestNonce == link.header.sessionNonce,
			"guest re-attached to the restarted host's new session")
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
