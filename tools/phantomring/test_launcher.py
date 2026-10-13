"""Offline checks of the launcher's guards, on temp dirs only (never the real saves or game dirs):
save backup/verify/restore/prune, the seed save, guest install/uninstall refusing unknown files, profile
validation, the me3 archive lookup and, with --machine, Steam/compat-tool discovery on this machine.

    python3 tools/phantomring/test_launcher.py            # unit + temp-dir checks: any machine, games running or not
    python3 tools/phantomring/test_launcher.py --unit     # only the pure checks (fake /proc)
    python3 tools/phantomring/test_launcher.py --machine  # also this machine's Steam, Proton, MGSV and ER save (read-only)
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from phantomring import launch as L  # noqa: E402

RealMachine = L.Machine

failures = []


def check(cond, what, detail=""):
	print(f"  [{'ok' if cond else 'FAIL'}] {what}{f'  ({detail})' if detail else ''}")
	if not cond:
		failures.append(what)


class FakeMachine(RealMachine):
	def __init__(self, er_dir: Path, mgsv_dirs: list[tuple[str, Path]]):
		super().__init__()
		self._er, self._mgsv = er_dir, mgsv_dirs

	def er_save_dir(self):
		return self._er

	def mgsv_save_dirs(self):
		return self._mgsv


def unit() -> None:
	"""Process matching on a fake /proc: only the game itself counts, never a process whose command
	line merely mentions the exe (a `bash -c` wrapper running pgrep, Proton's waitforexitandrun, me3)."""
	print("game process matching:")
	check(L.game_of("eldenring.exe", "/x/ELDEN RING/Game/eldenring.exe") == "eldenring", "ER by comm and Unix argv0")
	check(L.game_of("mgsvtpp.exe", "Z:\\steam\\common\\MGS_TPP\\mgsvtpp.exe") == "mgsvtpp", "MGSV by comm and Windows argv0")
	check(L.game_of("wine64-preload", "C:\\Games\\MGSVTPP.EXE") == "mgsvtpp", "MGSV by argv0 alone, any case")
	check(L.game_of("bash", "/bin/bash") is None, "bash -c wrapper is not a game")
	check(L.game_of("python3", "python3") is None, "proton waitforexitandrun is not a game")
	check(L.game_of("steam.exe", "c:\\windows\\system32\\steam.exe") is None, "Proton's steam.exe is not a game")
	check(L.game_of("me3", "/opt/me3/bin/me3") is None, "me3 launcher is not a game")
	tmp = Path(tempfile.mkdtemp(prefix="phantomring_proc_"))
	procs = {
		101: ("bash", [b"/bin/bash", b"-c", b"sleep 9; pgrep -f eldenring.exe; pgrep -f mgsvtpp.exe"]),
		102: ("python3", [b"python3", b"/proton", b"waitforexitandrun", b"/g/MGS_TPP/mgsvtpp.exe"]),
		103: ("eldenring.exe", [b"/g/ELDEN RING/Game/eldenring.exe"]),
		104: ("mgsvtpp.exe", [b"Z:\\g\\MGS_TPP\\mgsvtpp.exe"]),
		105: ("pgrep", [b"pgrep", b"eldenring.exe"]),
	}
	for pid, (comm, argv) in procs.items():
		d = tmp / str(pid)
		d.mkdir()
		(d / "comm").write_text(comm + "\n")
		(d / "cmdline").write_bytes(b"\0".join(argv) + b"\0")
	(tmp / "self").mkdir()
	(tmp / "106").mkdir()  # a process that exited between listing and reading: skipped
	found = L.game_processes(tmp)
	check({k: sorted(v) for k, v in found.items()} == {"eldenring": [103], "mgsvtpp": [104]}, "fake /proc: only the game processes", found)
	shutil.rmtree(tmp, ignore_errors=True)

	print("launch log dirs:")
	logs = Path(tempfile.mkdtemp(prefix="phantomring_logs_"))
	ts = "20261012-204059"
	check(L.fresh_log_dir(ts, logs) == logs / ts, "a new second: logs/<ts>")
	(logs / ts).mkdir()
	check(L.fresh_log_dir(ts, logs) == logs / ts, "an empty dir of that second is reused")
	(logs / ts / "pids.json").write_text("{}")
	check(L.fresh_log_dir(ts, logs) == logs / f"{ts}-2", "a second launch in the same second gets its own dir (-2)")
	(logs / f"{ts}-2").mkdir()
	(logs / f"{ts}-2" / "pids.json").write_text("{}")
	check(L.fresh_log_dir(ts, logs) == logs / f"{ts}-3", "...and a third -3")
	check(sorted([f"{ts}-2", ts], reverse=True)[0] == f"{ts}-2", "the later one sorts first (pr stop --er/--mgsv scan newest first)")
	shutil.rmtree(logs, ignore_errors=True)


def main() -> int:
	unit()
	if "--unit" in sys.argv[1:]:
		print("PASS" if not failures else f"FAIL: {failures}")
		return 1 if failures else 0
	tmp = Path(tempfile.mkdtemp(prefix="phantomring_launcher_"))
	quiet = lambda *_: None  # noqa: E731
	# Redirect every place the launcher writes.
	L.BACKUPS = tmp / "backups"
	L.LOGS = tmp / "logs"
	L.GUEST_RECORD = L.LOGS / "guest-install.json"
	real_build = L.BUILD
	L.BUILD = tmp / "build"
	L.BUILD.mkdir()

	er = tmp / "EldenRing/76561198000000000"
	er.mkdir(parents=True)
	(er / "ER0000.sl2").write_bytes(b"save-a" * 1000)
	(er / "steam_autocloud.vdf").write_text("x")
	mg = tmp / "userdata/311340/remote"
	mg.mkdir(parents=True)
	(mg / "TPP_GAME_DATA").write_bytes(b"tpp" * 100)
	m = FakeMachine(er, [("311340-remote", mg), ("287700", tmp / "userdata/287700")])

	print("saves:")
	dest = L.backup_saves(m, "t1", er=True, mgsv=True, log=quiet)
	manifest = L.verify_backup(dest)
	check(set(manifest["sets"]) == {"er", "mgsv/311340-remote"}, "backup sets", list(manifest["sets"]))
	check(len(manifest["sets"]["er"]["files"]) == 2, "ER files backed up")
	os.environ.pop("PHANTOMRING_SEED_SAVE", None)
	check(L.seed_save(er) == er / "ER0000.sl2", "seed save defaults to ER0000.sl2")
	check(L.seed_save(er, "MySnake.sl2") == er / "MySnake.sl2", "--seed-save NAME is looked up in the save dir")
	check(L.seed_save(er, "/x/y.sl2") == Path("/x/y.sl2"), "--seed-save /path is taken as is")
	check(L.seed_save(tmp) is None, "no seed save in an empty dir")
	L.ensure_er_save(m, log=quiet)
	check((er / L.ER_SAVE_NAME).read_bytes() == (er / "ER0000.sl2").read_bytes(), "PhantomRing0000.sl2 created from ER0000.sl2")
	check(L.ensure_er_save(m, log=quiet).endswith("exists"), "an existing PhantomRing0000.sl2 is never replaced")
	(er / "ER0000.sl2").write_bytes(b"changed")
	# restore_saves refuses while games run: pretend none do (the real ones may be running right now).
	real_game_processes = L.game_processes
	L.game_processes = lambda *a: {"eldenring": [], "mgsvtpp": []}
	L.Machine = lambda: m  # restore_saves builds its own Machine for the safety backup
	try:
		L.restore_saves("t1", er=True, mgsv=False, log=quiet)
	finally:
		L.game_processes = real_game_processes
	check((er / "ER0000.sl2").read_bytes() == b"save-a" * 1000, "restore brought the old save back")
	safety = [p for p in L.BACKUPS.iterdir() if p.name.endswith("-pre-restore")]
	check(len(safety) == 1 and (safety[0] / "er/ER0000.sl2").read_bytes() == b"changed", "pre-restore safety backup kept the current save")
	L.game_processes = lambda *a: {"eldenring": [4242], "mgsvtpp": []}
	try:
		L.restore_saves("t1", er=True, mgsv=False, log=quiet)
		check(False, "restore refuses while Elden Ring runs")
	except L.LaunchError as e:
		check("close the games first" in str(e), "restore refuses while Elden Ring runs")
	finally:
		L.game_processes = real_game_processes
	(dest / "er/ER0000.sl2").write_bytes(b"corrupt")
	try:
		L.verify_backup(dest)
		check(False, "damaged backup detected")
	except L.LaunchError:
		check(True, "damaged backup detected")

	print("backup pruning:")
	pb = tmp / "prune"
	names = ["20260101-000000", "20260102-000000", "20260103-000000-pre-restore", "20260104-000000", "20260105-000000",
		"20260106-000000"]
	for n in names:
		(pb / n).mkdir(parents=True)
		(pb / n / "manifest.json").write_text("{}")
	(pb / "notes").mkdir()
	check(L.prune_backups(0, quiet, pb) == [], "keep 0 deletes nothing")
	gone = L.prune_backups(2, quiet, pb)
	left = sorted(p.name for p in pb.iterdir())
	check(sorted(p.name for p in gone) == ["20260102-000000", "20260104-000000"], "the middle backups go", [p.name for p in gone])
	check(left == ["20260101-000000", "20260103-000000-pre-restore", "20260105-000000", "20260106-000000", "notes"],
		"kept: the oldest, the pre-restore, the newest 2, a non-backup dir", left)
	check(L.prune_backups(2, quiet, pb) == [], "pruning again is a no-op")

	print("me3 archive:")
	check(len(L.me3_expected_digest()) == 64 and all(c in "0123456789abcdef" for c in L.me3_expected_digest()), "me3 digest is a sha256")
	os.environ.pop("PHANTOMRING_ME3_ARCHIVE", None)
	check(L.find_me3_archive(tmp / "x.tar.gz") == tmp / "x.tar.gz", "--archive wins")
	os.environ["PHANTOMRING_ME3_ARCHIVE"] = str(tmp / "env.tar.gz")
	check(L.find_me3_archive() == tmp / "env.tar.gz", "$PHANTOMRING_ME3_ARCHIVE next")
	os.environ.pop("PHANTOMRING_ME3_ARCHIVE")
	real_me3_dir = L.ME3_DIR
	L.ME3_DIR = tmp / "me3"
	bogus = tmp / L.ME3_ASSET
	bogus.write_bytes(b"not me3")
	try:
		L.install_me3(log=quiet, archive=bogus)
		check(False, "an archive with the wrong sha256 is refused")
	except L.LaunchError as e:
		check("sha256" in str(e) and not L.ME3_DIR.exists(), "an archive with the wrong sha256 is refused")
	try:
		L.install_me3(log=quiet, archive=tmp / "missing.tar.gz")
		check(False, "a missing archive names the download")
	except L.LaunchError as e:
		check(L.ME3_URL in str(e), "a missing archive names the download")
	L.ME3_DIR = real_me3_dir

	print("Steam root:")
	os.environ["PHANTOMRING_STEAM"] = str(tmp / "steam")
	check(L.find_steam_root() == tmp / "steam", "$PHANTOMRING_STEAM wins")
	os.environ.pop("PHANTOMRING_STEAM")
	check(all(p.is_absolute() for p in L.steam_root_candidates()), "candidates are absolute")

	print("guest install:")
	game = tmp / "MGS_TPP"
	game.mkdir()
	(L.BUILD / "dinput8.dll").write_bytes(b"MZ ours v1")
	res = L.install_guest(game, log=quiet)
	check((game / "dinput8.dll").read_bytes() == b"MZ ours v1", "dinput8.dll installed", res["dllSha256"][:12])
	(L.BUILD / "dinput8.dll").write_bytes(b"MZ ours v2")
	L.install_guest(game, log=quiet)
	check((game / "dinput8.dll").read_bytes() == b"MZ ours v2", "our older build is replaced")
	(game / "dinput8.dll").write_bytes(b"MZ somebody else's proxy")
	try:
		L.install_guest(game, log=quiet)
		check(False, "unknown dinput8.dll is refused")
	except L.LaunchError as e:
		check("not a PhantomRing build" in str(e), "unknown dinput8.dll is refused")
	L.uninstall_guest(game, log=quiet)
	check((game / "dinput8.dll").exists(), "uninstall leaves an unknown dinput8.dll alone")
	(game / "dinput8.dll").write_bytes(b"MZ ours v1")
	L.uninstall_guest(game, log=quiet)
	check(not (game / "dinput8.dll").exists(), "uninstall removes a dinput8.dll we installed earlier")
	recs = json.loads(L.GUEST_RECORD.read_text())
	check(len(recs["dll"]) == 2 and len(recs["history"]) == 2, "install records kept", f"{len(recs['dll'])} hashes")

	print("profile:")
	L.BUILD = real_build
	check(L.check_profile() == [], "shipped phantomring.me3 passes", L.check_profile())
	bad = tmp / "bad.me3"
	bad.write_text(L.PROFILE.read_text().replace("start_online = false", "start_online = true").replace("disable_arxan = true", ""))
	problems = L.check_profile(bad)
	check(any("start_online" in p for p in problems) and any("disable_arxan" in p for p in problems), "bad profile rejected", problems)

	L.Machine = RealMachine
	if "--machine" in sys.argv[1:]:
		print("this machine (read-only):")
		mach = RealMachine()
		print(f"  Steam root: {mach.steam}")
		proton, slr, how = mach.compat_tool(L.MGSV_APPID)
		check(proton is not None and (proton / "proton").is_file(), "Proton found for MGSV", f"{proton} ({how})")
		check(slr is not None and (slr / "_v2-entry-point").is_file(), "SLR found via require_tool_appid", slr)
		check(mach.app_dir(L.MGSV_APPID) and (mach.app_dir(L.MGSV_APPID) / "mgsvtpp.exe").is_file(), "MGSV dir", mach.app_dir(L.MGSV_APPID))
		check(mach.er_save_dir() is not None and mach.er_save_dir().is_dir(), "ER save dir", mach.er_save_dir())
	shutil.rmtree(tmp, ignore_errors=True)
	print("PASS" if not failures else f"FAIL: {failures}")
	return 1 if failures else 0


if __name__ == "__main__":
	sys.exit(main())
