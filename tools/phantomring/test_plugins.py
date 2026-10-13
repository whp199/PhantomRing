"""Cross-implementation end-to-end test: the plugins' real C++ link code against the Python fake peers,
under wine, on a private mapping in /dev/shm.

  1. real host  (er/Link.cpp + er/Console.cpp, `test_host.exe --peer`)        vs fake_mgsv.py
  2. real guest (mgsv/Link.cpp, `test_guest_link.exe --peer`)                 vs fake_er.py

test_fakes.py proves the two fakes agree with each other; this proves each plugin agrees with the fake
that stands in for the other game -- the pairing of every live test before both games run together
(docs/LIVE_TEST_P01.md). It also checks that Wine's QueryPerformanceCounter and proto.qpc_now() are the
same clock (heartbeats cross between the two).

    WINEPREFIX=... python3 tools/phantomring/test_plugins.py [build dir]
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
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
os.environ.setdefault("PHANTOMRING_QPC_CLOCK", "monotonic")  # system Wine's QPC clock (see below)
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


# System Wine's QPC is CLOCK_MONOTONIC (Proton's is CLOCK_MONOTONIC_RAW): the fakes and this script stamp
# the clock the plugin code under test reads (PHANTOMRING_QPC_CLOCK, set above and inherited by the fakes).


def spawn_fake(module: str, path: str, seconds: float, summary: Path, log: Path, *extra) -> subprocess.Popen:
	env = dict(os.environ, PYTHONPATH=str(TOOLS), PHANTOMRING_SHM=path)
	return subprocess.Popen([sys.executable, "-m", f"phantomring.{module}", "--seconds", str(seconds), "--summary", str(summary), *extra],
		env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT)


def spawn_wine(exe: Path, args: list[str], log: Path) -> subprocess.Popen:
	env = dict(os.environ, WINEDEBUG="-all")
	return subprocess.Popen(["wine", str(exe), *args], env=env, cwd=str(exe.parent), stdout=open(log, "w"), stderr=subprocess.STDOUT)


def peer_line(log: Path) -> dict:
	for line in log.read_text(errors="replace").splitlines():
		if line.startswith("PEER {"):
			return json.loads(line[5:])
	raise RuntimeError(f"no PEER line in {log}: {log.read_text(errors='replace')[-400:]}")


def pr(path: str, *args) -> subprocess.CompletedProcess:
	env = dict(os.environ, PYTHONPATH=str(TOOLS))
	return subprocess.run([sys.executable, "-m", "phantomring", "--path", path, *args], env=env, capture_output=True, text=True, timeout=30)


def wine_path(path: str) -> str:
	return "file:Z:" + path.replace("/", "\\")


def host_vs_fake_mgsv(build: Path, work: Path, check: Checker):
	print("-- real host link (test_host.exe --peer) vs fake_mgsv.py")
	path = f"/dev/shm/phantomring_plugtest_h{os.getpid()}"
	proto.create_mapping(path)
	procs = []
	try:
		guest = spawn_fake("fake_mgsv", path, 16.0, work / "mgsv.json", work / "mgsv.log")
		procs.append(guest)
		time.sleep(0.5)
		host = spawn_wine(build / "test_host.exe", ["--peer", wine_path(path), "10"], work / "host_peer.log")
		procs.append(host)
		link = proto.Link(path)
		deadline = time.monotonic() + 20
		while time.monotonic() < deadline and not (link.header.magic == proto.MAGIC and link.header.guestNonce == link.header.sessionNonce != 0):
			time.sleep(0.1)
		check(link.header.guestNonce == link.header.sessionNonce != 0, "fake guest attached to the real host's session")
		check(link.header.hostUnixPid != 0, "real host stamped its unix pid (getpid stub)", link.header.hostUnixPid)
		time.sleep(1.0)
		host_side, guest_side = link.side("host"), link.side("guest")
		check(host_side["alive"] and guest_side["alive"], "both heartbeats alive across wine/native (same QPC clock)",
			f"host {host_side['heartbeatAge']}, guest {guest_side['heartbeatAge']}")
		status, resp = proto.console_request(link, "host", "status", timeout=5)
		hb = None
		for word in resp.replace("\n", " ").split(" hb ")[1:2]:
			hb = float(word.split()[0])
		check(status == 0 and hb is not None and 0 <= hb < 500, "real host console answers `status`; guest heartbeat age sane (same clock)",
			f"hb {hb} ms")
		r = pr(path, "er", "help")
		check(r.returncode == 0 and "peek" in r.stdout, "`pr er help` round trip to the real host console", (r.stdout + r.stderr).strip()[:80])
		r = pr(path, "er", "frobnicate")
		check(r.returncode == 1, "`pr er` error exit code from the real host console", r.stderr.strip()[:80])
		host.wait(timeout=40)
		guest.terminate()
		guest.wait(timeout=10)
		link.close()
		h = peer_line(work / "host_peer.log")
		g = json.loads((work / "mgsv.json").read_text())
		print("   host:", json.dumps(h))
		check(h["attaches"] == 1, "host saw the guest attach once", h["attaches"])
		check(h["states"] > 200 and h["readyFrames"] > 200, "GuestState read by the host (kGuestReady)", f"{h['states']} / {h['readyFrames']}")
		check(h["odoXZ"] > 2.0, "odometer advances on the host", f"{h['odoXZ']:.2f} m")
		check(h["buddyType"] == proto.BuddyType.WALKER_GEAR and h["buddyFlags"] & proto.GuestBuddyFlags.Present, "D-Walker buddy block read",
			proto.flag_names(h["buddyFlags"], proto.GuestBuddyFlags))
		check(h["maxBones"] == 178 and h["poses"] > 200, "pose block read (178 bones)", h["maxBones"])
		check(h["events"].get(str(int(proto.GuestEventType.Fire)), 0) >= 2, "Fire events popped by the host", h["events"])
		check(h["events"].get(str(int(proto.GuestEventType.BuddyFire)), 0) >= 1, "BuddyFire events popped by the host", h["events"])
		check(h["consoleAsks"] >= 3, "host console requests served", h["consoleAsks"])
		check(g["maxThumbLY"] == 32767 and g["padPackets"] >= 2, "real host's pad slot reaches the guest", f"LY {g['maxThumbLY']}, packets {g['padPackets']}")
		check(g["hostEvents"].get("Hurt", 0) >= 5 and g["lifeMin"] < 6000, "real host's Hurt events reach the guest", g["hostEvents"])
	finally:
		for p in procs:
			if p.poll() is None:
				p.kill()
		try:
			os.unlink(path)
		except OSError:
			pass


def guest_vs_fake_er(build: Path, work: Path, check: Checker):
	print("-- real guest link (test_guest_link.exe --peer) vs fake_er.py")
	path = f"/dev/shm/phantomring_plugtest_g{os.getpid()}"
	proto.create_mapping(path)
	procs = []
	try:
		guest = spawn_wine(build / "test_guest_link.exe", ["--peer", wine_path(path), "9"], work / "guest_peer.log")
		procs.append(guest)
		time.sleep(1.5)
		host = spawn_fake("fake_er", path, 14.0, work / "er.json", work / "er.log", "--hurt-every", "1")
		procs.append(host)
		link = proto.Link(path)
		deadline = time.monotonic() + 20
		while time.monotonic() < deadline and not (link.header.guestNonce == link.header.sessionNonce != 0):
			time.sleep(0.1)
		check(link.header.guestNonce == link.header.sessionNonce != 0, "real guest attached to the fake host's session")
		check(link.header.guestUnixPid != 0, "real guest stamped its unix pid (getpid stub)", link.header.guestUnixPid)
		time.sleep(1.0)
		check(link.side("guest")["alive"], "guest heartbeat alive as seen from native Python", link.side("guest")["heartbeatAge"])
		r = pr(path, "lua", "!status")
		check(r.returncode == 0 and "echo !status" in r.stdout, "`pr lua '!status'` answered by the real guest worker", (r.stdout + r.stderr).strip()[:80])
		guest.wait(timeout=40)
		host.terminate()
		host.wait(timeout=10)
		link.close()
		g = peer_line(work / "guest_peer.log")
		e = json.loads((work / "er.json").read_text())
		print("   guest:", json.dumps(g))
		check(g["attached"] and g["hostAlive"], "guest saw the host alive (fake_er's QPC heartbeat)")
		check(g["padPackets"] >= 2 and g["maxThumbLY"] == 32767, "fake host's scripted pad read by the guest", f"packets {g['padPackets']}, LY {g['maxThumbLY']}")
		check(g["hostStates"] > 100 and g["hostFlags"] & proto.HostFlags.Puppeting, "HostState read by the guest", g["hostStates"])
		check(g["hurts"] >= 3 and g["life"] < 6000, "Hurt events popped by the guest", f"{g['hurts']} hurts, life {g['life']}")
		check(e["guestAttaches"] == 1 and e["guestStates"] > 150, "fake host read the real guest's GuestState", f"{e['guestStates']} states")
		check(e["odoDistance"] > 3.0, "real guest's odometer moves the fake puppet", f"{e['odoDistance']:.2f} m")
		check(e["poseBones"] == 178, "real guest's pose read by the fake host", e["poseBones"])
		check(e["events"].get("Fire", 0) >= 3, "real guest's Fire events reach the fake host", e["events"])
		check(e["renderClearAll"] == 1, "real guest sent kRenClearAll on attach", e["renderClearAll"])
		check(proto.BuddyType.WALKER_GEAR in e["buddyTypes"] and e["buddyPosValid"] > 100, "real guest's D-Walker block read", e["buddyTypes"])
	finally:
		for p in procs:
			if p.poll() is None:
				p.kill()
		try:
			os.unlink(path)
		except OSError:
			pass


def main() -> int:
	build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build"
	for exe in ("test_host.exe", "test_guest_link.exe"):
		if not (build / exe).is_file():
			print(f"{build / exe} missing: build first (cmake --build build)")
			return 2
	if not shutil.which("wine"):
		print("wine not found")
		return 2
	work = Path(tempfile.mkdtemp(prefix="phantomring_plugins_"))
	check = Checker()
	try:
		host_vs_fake_mgsv(build, work, check)
		guest_vs_fake_er(build, work, check)
	except Exception as ex:
		check(False, f"test crashed: {type(ex).__name__}: {ex}")
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
