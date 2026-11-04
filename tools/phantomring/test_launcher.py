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
import time
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
	# A failed seed copy leaves no PhantomRing0000.sl2 (the next launch would take a partial one for the player's save).
	(er / L.ER_SAVE_NAME).unlink()
	real_copy2 = L.shutil.copy2
	def failing_copy(src, dst, *a, **k):
		Path(dst).write_bytes(b"partial")
		raise OSError(28, "No space left on device")
	L.shutil.copy2 = failing_copy
	try:
		L.ensure_er_save(m, log=quiet)
		check(False, "a failed seed copy raises")
	except OSError:
		pass
	finally:
		L.shutil.copy2 = real_copy2
	check(not (er / L.ER_SAVE_NAME).exists() and not any(er.glob("*.tmp")), "a failed seed copy leaves no save and no temp file",
		sorted(p.name for p in er.iterdir()))
	L.ensure_er_save(m, log=quiet)
	check((er / L.ER_SAVE_NAME).read_bytes() == (er / "ER0000.sl2").read_bytes(), "...and the next launch makes it whole")
	dest = L.backup_saves(m, "t1", er=True, mgsv=True, log=quiet)  # now with PhantomRing0000.sl2 in it: lands in t1-2
	check(dest.name == "t1-2", "a second backup in the same second gets its own folder", dest.name)
	check(set(L.verify_backup(L.BACKUPS / "t1")["sets"]) == {"er", "mgsv/311340-remote"}, "...and the first's manifest stays whole")
	check(L.fresh_backup_dir("t9-pre-restore", L.BACKUPS) == L.BACKUPS / "t9-pre-restore", "a free pre-restore name is used as is")
	(L.BACKUPS / "t9-pre-restore").mkdir()
	(L.BACKUPS / "t9-pre-restore/manifest.json").write_text("{}")
	check(L.fresh_backup_dir("t9-pre-restore", L.BACKUPS) == L.BACKUPS / "t9-2-pre-restore", "a taken one keeps -pre-restore last")
	shutil.rmtree(L.BACKUPS / "t9-pre-restore")
	(er / "ER0000.sl2").write_bytes(b"changed")
	(er / L.ER_SAVE_NAME).write_bytes(b"pr changed")
	# restore_saves refuses while games run: pretend none do (the real ones may be running right now).
	real_game_processes = L.game_processes
	L.game_processes = lambda *a: {"eldenring": [], "mgsvtpp": []}
	L.Machine = lambda: m  # restore_saves builds its own Machine for the safety backup
	try:
		L.restore_saves("t1-2", er=True, mgsv=False, log=quiet)
	finally:
		L.game_processes = real_game_processes
	check((er / L.ER_SAVE_NAME).read_bytes() == b"save-a" * 1000, "restore brought PhantomRing's save back")
	check((er / "ER0000.sl2").read_bytes() == b"changed", "...and left the vanilla ER0000.sl2 alone")
	safety = [p for p in L.BACKUPS.iterdir() if p.name.endswith("-pre-restore")]
	check(len(safety) == 1 and (safety[0] / "er/ER0000.sl2").read_bytes() == b"changed"
		and (safety[0] / "er" / L.ER_SAVE_NAME).read_bytes() == b"pr changed", "pre-restore safety backup kept the current saves")
	L.game_processes = lambda *a: {"eldenring": [], "mgsvtpp": []}
	try:
		L.restore_saves("t1-2", er=True, mgsv=False, log=quiet, all_er=True)
	finally:
		L.game_processes = real_game_processes
	check((er / "ER0000.sl2").read_bytes() == b"save-a" * 1000, "--all-er-files restores the vanilla save too")
	# A first launch's backup (t1) has no PhantomRing0000.sl2: restoring it removes the one created after it (review).
	(er / L.ER_SAVE_NAME).write_bytes(b"played since")
	check([p.name for p in L.created_after(L.verify_backup(L.BACKUPS / "t1")["sets"]["er"], "er")] == [L.ER_SAVE_NAME],
		"created_after lists PhantomRing's save the backup does not hold")
	L.game_processes = lambda *a: {"eldenring": [], "mgsvtpp": []}
	try:
		L.restore_saves("t1", er=True, mgsv=False, log=quiet)
	finally:
		L.game_processes = real_game_processes
	safety = sorted(p for p in L.BACKUPS.iterdir() if p.name.endswith("-pre-restore"))
	check(not (er / L.ER_SAVE_NAME).exists() and any((p / "er" / L.ER_SAVE_NAME).read_bytes() == b"played since" for p in safety
		if (p / "er" / L.ER_SAVE_NAME).is_file()), "restoring a first launch's backup removes the save created after it (kept in the safety backup)")
	check((er / "ER0000.sl2").exists(), "...and leaves the vanilla save")
	(er / L.ER_SAVE_NAME).write_bytes(b"save-a" * 1000)
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
	check(manifest.get("origin") == L.ORIGIN_MANUAL, "a backup made without saying who (pr backup-saves) is ORIGIN_MANUAL",
		manifest.get("origin"))
	safety = sorted(p for p in L.BACKUPS.iterdir() if p.name.endswith("-pre-restore"))
	check(all(json.loads((p / "manifest.json").read_text()).get("origin") == L.ORIGIN_PRE_RESTORE for p in safety),
		"restore-saves' safety backups say so in their manifest")
	no_logs = tmp / "prune_no_logs"   # no launch log folders: a manifest without an origin counts as made by hand
	both = {"er": {}, "mgsv/287700": {}}
	launch_ = {"origin": L.ORIGIN_LAUNCH}

	def mk(root, name, sets, extra=None, raw=None):
		(root / name).mkdir(parents=True)
		(root / name / "manifest.json").write_text(raw if raw is not None else json.dumps({"sets": sets} | (extra or {})))

	pb = tmp / "prune"
	for n in ("20260101-000000", "20260102-000000", "20260103-000000-pre-restore", "20260104-000000", "20260105-000000",
			"20260106-000000"):
		mk(pb, n, both, launch_)
	(pb / "notes").mkdir()
	mk(pb, "20260100-000000", None, raw="not json")
	check(L.prune_backups(0, quiet, pb, recent_hours=0, logs=no_logs) == [], "keep 0 deletes nothing")
	gone = L.prune_backups(2, quiet, pb, recent_hours=0, logs=no_logs)
	left = sorted(p.name for p in pb.iterdir() if not p.name.startswith("."))
	check(sorted(p.name for p in gone) == ["20260102-000000", "20260104-000000"], "the middle backups go", [p.name for p in gone])
	check(left == ["20260100-000000", "20260101-000000", "20260103-000000-pre-restore", "20260105-000000", "20260106-000000", "notes"],
		"kept: the oldest, the pre-restore, the newest 2, an unreadable manifest, a non-backup dir", left)
	check(L.prune_backups(2, quiet, pb, recent_hours=0, logs=no_logs) == [], "pruning again is a no-op")
	check(json.loads((pb / L.FIRST_BACKUPS).read_text()) == {"er": "20260101-000000", "mgsv": "20260101-000000"},
		"backups/.first.json names each game's first backup")
	# Per game: the first backup was MGSV-only (the real backups/ began that way), the first ER one comes second.
	pg = tmp / "prune_games"
	er_only, mgsv_only = {"er": {}}, {"mgsv/311340-remote": {}, "mgsv/287700": {}}
	for k, sets in enumerate([mgsv_only, both, mgsv_only, both, mgsv_only, mgsv_only, er_only, mgsv_only], 1):
		mk(pg, f"2026010{k}-000000", sets, launch_)
	gone = sorted(p.name[7] for p in L.prune_backups(2, quiet, pg, recent_hours=0, logs=no_logs))
	left = sorted(p.name[7] for p in pg.iterdir() if not p.name.startswith("."))
	check("2" in left, "the first Elden Ring backup survives behind an MGSV-only first one", left)
	check({"4", "7"} <= set(left), "MGSV-only relaunches don't push out the newest Elden Ring backups", left)
	check(gone == ["3", "5"], "only backups no game protects go", gone)
	# Review 0.7.0: only pr launch's backups are ever pruned. One made by hand (pr backup-saves) stays whatever its age, and a
	# manifest from before 0.7.0 (no origin) counts as a launch's only when logs/ still has that launch's folder.
	pm = tmp / "prune_manual"
	pm_logs = tmp / "prune_manual_logs"
	for k in range(1, 9):
		n = f"202601{k:02d}-000000"
		if k == 3:
			mk(pm, n, both, {"origin": L.ORIGIN_MANUAL})            # pr backup-saves
		elif k in (4, 5):
			mk(pm, n, both)                                          # before 0.7.0, its launch's log folder still there
			(pm_logs / n).mkdir(parents=True)
			(pm_logs / n / "pids.json").write_text("{}")
		elif k == 6:
			mk(pm, n, both)                                          # before 0.7.0, no log folder: made by hand?
		else:
			mk(pm, n, both, launch_)
	gone = sorted(p.name for p in L.prune_backups(2, quiet, pm, recent_hours=0, logs=pm_logs))
	check(gone == ["20260102-000000", "20260104-000000", "20260105-000000"], "a backup made by hand is never pruned; old launch "
		"backups are, also from before 0.7.0 when logs/ still has their launch", gone)
	check(L.backup_origin(pm / "20260106-000000", pm_logs) == L.ORIGIN_MANUAL and (pm / "20260106-000000").is_dir(),
		"an old manifest with no origin and no launch folder in logs/ counts as made by hand (kept)")
	for k in range(9, 13):
		mk(pm, f"202601{k:02d}-000000", both, launch_)
	(pm_logs / "20260107-000000").mkdir()
	gone = sorted(p.name for p in L.prune_backups(2, quiet, pm, recent_hours=0, logs=pm_logs))
	check(gone == ["20260107-000000", "20260108-000000", "20260109-000000", "20260110-000000"] and (pm / "20260103-000000").is_dir()
		and (pm / "20260106-000000").is_dir(), "...and stays through later prunes", gone)
	mk(pm_logs.parent / "prune_stamp", "20260101-000000", both)
	(pm_logs / "20260101-000000").mkdir(exist_ok=True)
	(pm_logs / "20260101-000000" / "pids.json").write_text("{}")
	check(L.backup_origin(pm_logs.parent / "prune_stamp/20260101-000000", pm_logs) == L.ORIGIN_LAUNCH
		and json.loads((pm_logs.parent / "prune_stamp/20260101-000000/manifest.json").read_text()).get("origin") == L.ORIGIN_LAUNCH,
		"an old launch backup gets its origin written in, so it stays known once its log folder is pruned")
	# Review 0.7.0: copies and other folders under backups/ are never in the pool, and never take the first's protection.
	pc = tmp / "prune_copies"
	for k in range(1, 7):
		mk(pc, f"202601{k:02d}-000000", both, launch_)
	mk(pc, "0-original", both, launch_)                    # sorted first by name: used to become the "first"
	mk(pc, "20260101-000000-original", both, launch_)      # a copy of the first
	(pc / "worktree-patches-20261103").mkdir()
	(pc / "worktree-patches-20261103/a.diff").write_text("x")
	gone = sorted(p.name for p in L.prune_backups(2, quiet, pc, recent_hours=0, logs=no_logs))
	check(gone == ["20260102-000000", "20260103-000000", "20260104-000000"], "only launcher-named backups go; the real first stays", gone)
	check({"0-original", "20260101-000000-original", "worktree-patches-20261103", "20260101-000000"} <= {p.name for p in pc.iterdir()},
		"copies, other folders and the first all kept")
	# Review 0.7.0: a backup taken while the clock ran behind (a name sorting first) never takes the first's protection.
	pk = tmp / "prune_clock"
	for k in range(1, 4):
		mk(pk, f"202601{k:02d}-000000", both, launch_)
	check(L.prune_backups(2, quiet, pk, recent_hours=0, logs=no_logs) == [], "three backups, keep 2 + the first: none go")
	mk(pk, "20000101-000000", both, launch_)                # the clock was reset to 2000
	for k in range(4, 7):
		mk(pk, f"202601{k:02d}-000000", both, launch_)
	gone = sorted(p.name for p in L.prune_backups(2, quiet, pk, recent_hours=0, logs=no_logs))
	check((pk / "20260101-000000").is_dir() and gone == ["20260102-000000", "20260103-000000", "20260104-000000"],
		"the first by .first.json stays though a wrong-clock backup sorts before it", gone)
	# Review 0.7.0: a symlinked backup is never followed or removed, and a folder that can't be removed doesn't stop the launch.
	ps = tmp / "prune_links"
	elsewhere = tmp / "archive/20260102-000000"
	mk(elsewhere.parent, elsewhere.name, both, launch_)
	for k in (1, 3, 4, 5, 6):
		mk(ps, f"202601{k:02d}-000000", both, launch_)
	(ps / "20260102-000000").symlink_to(elsewhere)
	(ps / "20260103-000000/er").mkdir()
	(ps / "20260103-000000/er/save.sl2").write_text("x")
	os.chmod(ps / "20260103-000000/er", 0o500)
	msgs = []
	try:
		gone = sorted(p.name for p in L.prune_backups(2, msgs.append, ps, recent_hours=0, logs=no_logs))
	except Exception as e:  # noqa: BLE001
		gone = [f"raised {e!r}"]
	finally:
		os.chmod(ps / "20260103-000000/er", 0o700)
	check(gone == ["20260104-000000"] or (os.geteuid() == 0 and gone == ["20260103-000000", "20260104-000000"]),
		"a symlink is left alone, a folder that can't be removed is logged and skipped, the rest still goes", gone)
	check(elsewhere.is_dir() and (ps / "20260102-000000").is_symlink(), "...and the symlink's target is untouched")
	check(os.geteuid() == 0 or any("could not prune" in m for m in msgs), "...the failure is logged", msgs)
	# The default since 0.7.0 (a player had 146 backups, 18.8 GB): the newest 10 per game, the first and every pre-restore.
	check(L.KEEP_BACKUPS == 10, "pr launch keeps the newest 10 backups per game by default", L.KEEP_BACKUPS)
	saved_env = {k: os.environ.pop(k, None) for k in ("PHANTOMRING_KEEP_BACKUPS", "PHANTOMRING_KEEP_LOGS", "PHANTOMRING_KEEP_RECENT_HOURS")}
	check(L.keep_backups_default() == 10, "...unless PHANTOMRING_KEEP_BACKUPS says otherwise")
	os.environ["PHANTOMRING_KEEP_BACKUPS"] = "0"
	check(L.keep_backups_default() == 0, "PHANTOMRING_KEEP_BACKUPS=0 keeps all")
	for bad in ("all", "off", "", "10.0"):
		os.environ["PHANTOMRING_KEEP_BACKUPS"] = bad
		os.environ["PHANTOMRING_KEEP_LOGS"] = bad
		check(L.keep_backups_default() == 0 and L.keep_logs_default() == 0, f"a value that isn't a count ({bad!r}) keeps all, never prunes")
	os.environ["PHANTOMRING_KEEP_RECENT_HOURS"] = "nan"
	check(L.keep_recent_hours() == L.KEEP_RECENT_HOURS, "PHANTOMRING_KEEP_RECENT_HOURS=nan keeps the 24 h guard")
	os.environ["PHANTOMRING_KEEP_RECENT_HOURS"] = "soon"
	check(L.keep_recent_hours() == L.KEEP_RECENT_HOURS, "...and so does one that isn't a number")
	os.environ["PHANTOMRING_KEEP_RECENT_HOURS"] = "0"
	check(L.keep_recent_hours() == 0, "0 hours: no such protection (documented)")
	for k, v in saved_env.items():
		os.environ.pop(k, None)
		if v is not None:
			os.environ[k] = v
	pd = tmp / "prune_default"
	for i in range(1, 16):
		mk(pd, f"202611{i:02d}-000000", both, launch_)
	mk(pd, "20261105-120000-pre-restore", both, {"origin": L.ORIGIN_PRE_RESTORE})
	gone = sorted(p.name for p in L.prune_backups(L.keep_backups_default(), quiet, pd, recent_hours=0, logs=no_logs))
	left = sorted(p.name for p in pd.iterdir() if not p.name.startswith("."))
	check(gone == [f"202611{i:02d}-000000" for i in range(2, 6)], "15 launches: the 2nd to 5th go", gone)
	check(len(left) == 12 and "20261101-000000" in left and "20261105-120000-pre-restore" in left and "20261115-000000" in left,
		"kept: the first, the newest 10 and the pre-restore", left)

	print("log pruning:")
	pl = tmp / "prune_logs"
	pl.mkdir()
	for i in range(1, 15):
		(pl / f"202611{i:02d}-000000").mkdir()
		(pl / f"202611{i:02d}-000000/er-me3.log").write_text("x")
	(pl / "20261114-000000-2").mkdir()             # a second launch in the same second
	(pl / "guest-install.json").write_text("{}")   # never a session
	(pl / "evidence-20261014").mkdir()             # never a session
	(pl / "phantomring-logs-20261101-000000.zip").write_text("zip")
	(pl / "current").symlink_to("20261102-000000")  # the current one stays whatever its age
	import subprocess
	game = subprocess.Popen(["sleep", "60"], env=os.environ | {"PHANTOMRING_LAUNCH_ID": "launch-live"}, start_new_session=True)
	(pl / "20261103-000000/pids.json").write_text(json.dumps({"launchId": "launch-live", "er": {"pid": game.pid}}))
	# 20261104's launch is over: its recorded pid is someone else's process now (no marker)
	(pl / "20261104-000000/pids.json").write_text(json.dumps({"launchId": "launch-gone", "er": {"pid": os.getpid()}}))
	try:
		check(L.prune_logs(0, quiet, pl, recent_hours=0) == [], "keep 0 deletes no log")
		check(L.KEEP_LOGS == 10 and L.keep_logs_default() == 10, "the newest 10 launch log folders by default")
		gone = sorted(p.name for p in L.prune_logs(10, quiet, pl, recent_hours=0))
	finally:
		game.kill()
		game.wait()
	left = sorted(p.name for p in pl.iterdir())
	check(gone == ["20261101-000000", "20261104-000000", "20261105-000000"], "old sessions go: not the current one, nor a running launch's",
		gone)
	check({"guest-install.json", "evidence-20261014", "phantomring-logs-20261101-000000.zip", "current"} <= set(left),
		"never anything that isn't a session folder", left)
	check("20261114-000000-2" in left and "20261106-000000" in left and "20261102-000000" in left and "20261103-000000" in left
		and len([n for n in left if L.SESSION_DIR.match(n)]) == 12, "kept: the newest 10 (a -2 sorts after its second), the current and the running one",
		left)
	check([p.name for p in L.prune_logs(10, quiet, pl, recent_hours=0)] == ["20261103-000000"], "...and a running launch's folder goes once it has stopped")
	check(L.prune_logs(10, quiet, pl, recent_hours=0) == [], "pruning logs again is a no-op")
	check(L.prune_logs(10, quiet, tmp / "no_such_logs") == [], "no logs/ yet: nothing to do")
	# Nothing from the last day goes, whatever the count (a test session restores the backup it started with).
	pr_ = tmp / "prune_recent"
	now = time.mktime(time.strptime("20261120-120000", "%Y%m%d-%H%M%S"))
	for n in ("20261101-000000", "20261102-000000", "20261103-000000", "20261120-080000", "20261120-090000", "20261120-100000"):
		(pr_ / n).mkdir(parents=True)
		(pr_ / n / "manifest.json").write_text(json.dumps({"sets": both, "origin": L.ORIGIN_LAUNCH}))
	gone = sorted(p.name for p in L.prune_backups(1, quiet, pr_, now=now, recent_hours=24, logs=no_logs))
	check(gone == ["20261102-000000", "20261103-000000"], "backups of the last 24 h stay (keep 1: only older ones go)", gone)
	(pr_ / "logs").mkdir()
	for n in ("20261101-000000", "20261120-080000", "20261120-090000", "20261120-100000"):
		(pr_ / "logs" / n).mkdir()
	gone = sorted(p.name for p in L.prune_logs(1, quiet, pr_ / "logs", now=now, recent_hours=24))
	check(gone == ["20261101-000000"], "log folders of the last 24 h stay", gone)
	check(L.KEEP_RECENT_HOURS == 24 and L.recent(pr_ / "20261120-100000", now) and not L.recent(pr_ / "20261101-000000", now),
		"folder times come from their names")

	print("pr stop (both games, whichever launch started each):")
	import subprocess
	def spawn(env):
		return subprocess.Popen(["sleep", "60"], env=os.environ | env, start_new_session=True)
	er_p = spawn({"PHANTOMRING_LAUNCH_ID": "launch-A", "PHANTOMRING_ROLE": "host"})
	mg_p = spawn({"PHANTOMRING_LAUNCH_ID": "launch-B", "PHANTOMRING_ROLE": "guest"})
	stranger = spawn({})  # an old record's pid, now someone else's process
	for ts, rec in (("t0", {"er": {"pid": stranger.pid}, "launchId": "launch-OLD"}),
			("t1", {"er": {"pid": er_p.pid}, "launchId": "launch-A"}), ("t2", {"mgsv": {"pid": mg_p.pid}, "launchId": "launch-B"})):
		(L.LOGS / ts).mkdir(parents=True, exist_ok=True)
		(L.LOGS / ts / "pids.json").write_text(json.dumps(rec))
	cur = L.LOGS / "current"
	if cur.is_symlink() or cur.exists():
		cur.unlink()
	cur.symlink_to("t2")  # the latest launch was MGSV only (pr launch --mgsv-only next to a running Elden Ring)
	rc = L.stop(timeout=4.0, log=quiet)
	for p in (er_p, mg_p, stranger):
		try:
			p.wait(timeout=2)
		except subprocess.TimeoutExpired:
			pass
	check(rc == 0 and er_p.poll() is not None and mg_p.poll() is not None, "a plain pr stop stops both games from separate launches",
		f"rc {rc}, er {er_p.poll()}, mgsv {mg_p.poll()}")
	check(stranger.poll() is None, "a stale record's pid without its launch marker is left alone")
	stranger.kill()
	stranger.wait()

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
	# uninstall refuses while a real MGSV runs (a live test next to this one): the temp folder's guest is never that game's
	real_game_processes = L.game_processes
	L.game_processes = lambda *a: {"eldenring": [], "mgsvtpp": []}
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
	L.game_processes = real_game_processes

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
