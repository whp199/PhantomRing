"""Record/replay round trip: fake_mgsv (humanoid model, the stand-in for a real MGSV) -> recorder.py (as
the host) -> a recording file -> replay.py (as the guest) -> fake_er (as the host). Checks that the
replayed side delivers the same assets, draw lists, poses, events and HUD frames, that looping keeps the
odometer growing, and that a host restart makes the replay re-send its assets.

    python3 tools/phantomring/test_replay.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from phantomring import proto, recorder  # noqa: E402
from phantomring.test_fakes import Checker  # noqa: E402


def run(module: str, path: str, *args, log: Path) -> subprocess.Popen:
	env = dict(os.environ, PYTHONPATH=str(TOOLS), PHANTOMRING_SHM=path)
	return subprocess.Popen([sys.executable, "-m", f"phantomring.{module}", *args], env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT)


def main() -> int:
	work = Path(tempfile.mkdtemp(prefix="phantomring_replay_"))
	path_a = f"/dev/shm/phantomring_rec_{os.getpid()}"
	path_b = f"/dev/shm/phantomring_rep_{os.getpid()}"
	path_c = f"/dev/shm/phantomring_own_{os.getpid()}"
	check = Checker()
	procs = []
	rec_file = work / "walk.prrec"
	try:
		print(f"logs in {work}")
		# 1. record: fake_mgsv walks its circle (draw lists, pose, events every 2-3 s, HUD) for 4 s after a 1 s warmup.
		proto.create_mapping(path_a)
		guest = run("fake_mgsv", path_a, "--model", "humanoid", "--seconds", "9", "--quiet", log=work / "mgsv.log")
		procs.append(guest)
		time.sleep(0.5)
		r = run("recorder", path_a, "--out", str(rec_file), "--seconds", "4", "--warmup", "1", "--wait-ready", "0", "--hud-every", "0.2",
			"--pad", "ly=32767", "--quiet", log=work / "record.log")
		procs.append(r)
		r.wait(timeout=30)
		check(r.returncode == 0 and rec_file.exists(), "recorder wrote the file", (work / "record.log").read_text()[-300:])
		guest.terminate()
		chunks = list(recorder.read_chunks(str(rec_file)))
		kinds = {k: sum(1 for kk, _, _ in chunks if kk == k) for k in "MASDEHZ"}
		summary = json.loads(chunks[-1][2]) if chunks and chunks[-1][0] == "Z" else {}
		check(kinds["M"] == 1 and kinds["Z"] == 1, "meta and end chunks", str(kinds))
		check(kinds["A"] >= 3, "assets recorded (clear + meshes + textures)", str(kinds["A"]))
		check(kinds["S"] >= 150 and kinds["D"] >= 150, "~60 states and draw lists per second", str(kinds))
		check(kinds["E"] >= 1, "guest events recorded", str(kinds["E"]))
		check(10 <= kinds["H"] <= 30, "HUD kept every 0.2 s", str(kinds["H"]))
		check(summary.get("unresolvedKeys", 1) == 0, "every key the draw lists use was in the assets", str(summary.get("unresolvedKeys")))
		states = [proto.GuestState.from_buffer_copy(b) for k, _, b in chunks if k == "S"]
		check(states[-1].odo[2] - states[0].odo[2] > 5.0, "the recorded pad (ly) drove the guest forward",
			f"{states[0].odo[2]:.2f} -> {states[-1].odo[2]:.2f}")
		t_states = [t for k, t, _ in chunks if k == "S"]
		check(t_states[0] >= 0 and t_states[-1] <= 4.1, "recording times start after the warmup", f"{t_states[0]:.3f}..{t_states[-1]:.3f}")

		# 2. replay (looped) against fake_er; the host restarts once: the replay must re-attach and re-send.
		proto.create_mapping(path_b)
		rep = run("replay", path_b, str(rec_file), "--loop", "--seconds", "14", "--summary", str(work / "replay.json"), "--quiet", log=work / "replay.log")
		procs.append(rep)
		time.sleep(0.3)
		host1 = run("fake_er", path_b, "--seconds", "9.5", "--hurt-every", "3", "--summary", str(work / "er1.json"), "--quiet", log=work / "er1.log")
		procs.append(host1)
		host1.wait(timeout=30)
		link = proto.Link(path_b)
		gs = link.guest_state()
		check(link.side("guest")["statusName"] == "Ready" and "replay" in link.side("guest")["text"], "replay reports Ready", link.side("guest")["text"])
		status, resp = proto.console_request(link, "guest", "!status", timeout=3, force=True)
		check(status == 0 and "replay" in resp, "replay answers !status", resp[:120])
		host2 = run("fake_er", path_b, "--seconds", "3", "--hurt-every", "0", "--summary", str(work / "er2.json"), "--quiet", log=work / "er2.log")
		procs.append(host2)
		host2.wait(timeout=30)
		rep.wait(timeout=30)
		er1 = json.loads((work / "er1.json").read_text())
		er2 = json.loads((work / "er2.json").read_text())
		rs = json.loads((work / "replay.json").read_text())
		n_mesh = sum(1 for k, _, b in chunks if k == "A" and int.from_bytes(b[:4], "little") == proto.RenType.Mesh)
		n_tex = sum(1 for k, _, b in chunks if k == "A" and int.from_bytes(b[:4], "little") == proto.RenType.Texture)
		check(er1["renderMeshes"] == n_mesh and er1["renderTextures"] == n_tex and er1["renderClearAll"] >= 1,
			"host got every recorded mesh and texture", f"meshes {er1['renderMeshes']}/{n_mesh} textures {er1['renderTextures']}/{n_tex}")
		check(er1["drawFrames"] >= 300 and er1["drawMissingMesh"] == 0, "host drew replayed lists with no missing mesh",
			f"{er1['drawFrames']} frames, missing {er1['drawMissingMesh']}")
		check(set(er1["drawOwnersSeen"]) == {"Player", "Buddy"}, "Snake and buddy draws replayed", str(er1["drawOwnersSeen"]))
		check(er1["poseBones"] > 0, "pose replayed", str(er1["poseBones"]))
		check(er1["hudFrames"] >= 20, "HUD replayed", str(er1["hudFrames"]))
		check(sum(er1["events"].values()) >= 2, "guest events replayed", str(er1["events"]))
		check(rs["loops"] >= 2, "the recording looped", str(rs["loops"]))
		travel = states[-1].odo[2] - states[0].odo[2]
		check(er1["odoDistance"] > 1.5 * travel, "odometer keeps growing across loops (no jump back)",
			f"host odo distance {er1['odoDistance']:.1f} m, one loop {travel:.1f} m")
		check(rs["hostEvents"].get("Hurt", 0) >= 2, "host events drained by the replay", str(rs["hostEvents"]))
		check(rs["attaches"] == 2 and er2["renderMeshes"] == n_mesh and er2["drawFrames"] > 50,
			"host restart: replay re-attached and re-sent its assets", f"attaches {rs['attaches']}, meshes {er2['renderMeshes']}, frames {er2['drawFrames']}")
		check(gs is not None and gs.odoEpoch >= 1, "replay stamps its own odoEpoch", str(gs.odoEpoch if gs else None))
		link.close()

		# 3. A stand-in never takes over a side another live process holds (a real ER host stamps its
		# nonce once and would lose its guest for good; two guests would share single-producer rings).
		proto.create_mapping(path_c)
		host = run("fake_er", path_c, "--seconds", "12", "--hurt-every", "0", "--quiet", log=work / "er3.log")
		guest = run("fake_mgsv", path_c, "--seconds", "12", "--quiet", log=work / "mgsv3.log")
		procs += [host, guest]
		link = proto.Link(path_c)
		deadline = time.monotonic() + 8
		while time.monotonic() < deadline and not (link.side("host")["alive"] and link.side("guest")["alive"]):
			time.sleep(0.2)
		nonce = link.header.sessionNonce
		for module, args, what in (("recorder", ("--out", str(work / "no.prrec"), "--wait-ready", "0"), "recorder"),
				("fake_er", ("--seconds", "1"), "a second fake_er"),
				("replay", (str(rec_file), "--seconds", "1"), "replay"),
				("fake_mgsv", ("--seconds", "1"), "a second fake_mgsv")):
			p = run(module, path_c, *args, log=work / f"refuse_{module}.log")
			p.wait(timeout=30)
			out = (work / f"refuse_{module}.log").read_text()
			check(p.returncode == 4 and "pass --force" in out, f"{what} refuses a live {'host' if module in ('recorder', 'fake_er') else 'guest'}", out[-200:])
		check(link.header.sessionNonce == nonce and link.side("host")["pid"] == host.pid and link.side("guest")["pid"] == guest.pid,
			"the live pair kept its session", f"nonce {nonce:#x} -> {link.header.sessionNonce:#x}")
		link.close()
	finally:
		for p in procs:
			if p.poll() is None:
				p.kill()
		for p in (path_a, path_b, path_c):
			try:
				os.unlink(p)
			except FileNotFoundError:
				pass
	print(f"{check.count - len(check.failures)}/{check.count} passed")
	return 1 if check.failures else 0


if __name__ == "__main__":
	sys.exit(main())
