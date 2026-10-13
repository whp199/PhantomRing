"""Launcher side of `pr`: machine paths, preflight, save backups, guest install, starting/stopping both
games, logs.

Launch recipe (docs/research/proton-ipc-and-launch.md §7.2):
  MGSV: gamescope --backend headless -w W -h H -W W -H H -- env <Steam compat env, appid 287700,
        PHANTOMRING=1, WINEDLLOVERRIDES=dinput8=n,b> <SLR>/_v2-entry-point --verb=waitforexitandrun --
        <Proton>/proton waitforexitandrun <MGS_TPP>/mgsvtpp.exe
  ER:   me3 --windows-binaries-dir <me3>/bin/win64 --crash-reporting=false launch --game eldenring
        --exe <ER>/Game/eldenring.exe --profile tools/phantomring/profile/phantomring.me3
        --online=false --savefile PhantomRing0000.sl2
Nothing here talks to Steam: it only has to be running (SteamAPI_Init in both games needs it).
"""

from __future__ import annotations

import datetime as _dt
import hashlib
import json
import os
import shlex
import shutil
import signal
import subprocess
import tarfile
import time
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from . import proto, vdf

ROOT = Path(__file__).resolve().parents[2]  # PhantomRing/
TOOLS = ROOT / "tools/phantomring"
BUILD = ROOT / "build"
LOGS = ROOT / "logs"
BACKUPS = ROOT / "backups"
PROFILE = TOOLS / "profile/phantomring.me3"
GUEST_RECORD = LOGS / "guest-install.json"

ER_APPID = 1245620
MGSV_APPID = 287700
ER_SHA256 = "1a3547101327f65d0c76da2f9190ac0aa66871ea42bae2aecc61e11a8b597891"   # 1.17.1 / 2.7.1.0
MGSV_SHA256 = "085c2f82d1c963c40b3d2d55786661dfee2b18cbbf388a710c00fa76c5e9bb45"  # TPP 1.0.15.4
ER_SAVE_NAME = "PhantomRing0000.sl2"
# PhantomRing0000.sl2 is created once as a copy of an existing save (never modified): `pr launch --seed-save` or
# $PHANTOMRING_SEED_SAVE (a file name in the save dir, or a path), else Elden Ring's own ER0000.sl2.
ER_SAVE_SOURCES = ("ER0000.sl2",)
MGSV_SAVE_APPIDS = ("311340", "287700")  # TPP saves live under Ground Zeroes' 311340/remote (VERIFIED, §7.1)

# me3 0.13.0's Linux release (https://github.com/garyttierney/me3/releases/tag/v0.13.0) is not redistributed: download
# it yourself; `pr install-me3` checks this sha256 (the release's own published digest) before extracting it.
ME3_ASSET = "me3-linux-amd64.tar.gz"
ME3_URL = "https://github.com/garyttierney/me3/releases/download/v0.13.0/" + ME3_ASSET
ME3_SHA256 = "f31579deebf75b4702059694b664bdd8917c4773ab0c266715ca8f92e1e93fc1"
ME3_DIR = ROOT / "third_party/me3"
# Where the archive is looked for (after `pr install-me3 --archive` and $PHANTOMRING_ME3_ARCHIVE).
ME3_ARCHIVE_CANDIDATES = (ROOT / "third_party/downloads" / ME3_ASSET, Path.home() / "Downloads" / ME3_ASSET)
# Save backups kept under backups/ (`pr launch --keep-backups N`, $PHANTOMRING_KEEP_BACKUPS; 0 keeps all). The oldest
# one (the saves as they were before PhantomRing first ran) and every *-pre-restore one are always kept. Pruning is
# opt-in: deleting someone's save backups is never a default.
KEEP_BACKUPS = 0

# me3's own table (crates/cli/src/commands/launch/strategy/compat_tool.rs): Steam compat tool name -> appid.
COMPAT_TOOL_APPIDS = {
	"proton_experimental": 1493710, "proton_hotfix": 2180100, "proton_6": 1580130, "proton_7": 1887720,
	"proton_8": 2348590, "proton_9": 2805730, "proton_10": 3658110, "proton_11": 4628710,
}
DEFAULT_PROTON_DIR = "Proton - Experimental"
DEFAULT_SLR_DIR = "SteamLinuxRuntime_4"
STEAMID64_BASE = 76561197960265728


class LaunchError(Exception):
	pass


def sha256(path: Path) -> str:
	h = hashlib.sha256()
	with open(path, "rb") as f:
		while chunk := f.read(1 << 22):
			h.update(chunk)
	return h.hexdigest()


def timestamp() -> str:
	return _dt.datetime.now().strftime("%Y%m%d-%H%M%S")


def fresh_log_dir(ts: str, logs: Path | None = None) -> Path:
	"""logs/<ts>, or <ts>-2, -3, ... when a launch in the same second already recorded its processes there: `pr launch
	--mgsv-only` straight followed by `pr launch --er-only` wrote both into one pids.json, the second record replaced the
	first, and `pr stop --mgsv` then found no MGSV (live 2026-10-12)."""
	logs = logs or LOGS
	d, n = logs / ts, 2
	while (d / "pids.json").exists():
		d, n = logs / f"{ts}-{n}", n + 1
	return d


# ---- machine / Steam layout ------------------------------------------------------------------------
def steam_root_candidates() -> list[Path]:
	"""Where a native Steam keeps its root: ~/.steam/root and ~/.steam/steam link to it on most distros
	(~/.local/share/Steam, Debian/Ubuntu's ~/.steam/debian-installation). Flatpak Steam (UNTESTED with PhantomRing:
	gamescope and Proton are started outside its sandbox) lives under ~/.var/app."""
	home = Path.home()
	out: list[Path] = []
	for cand in (home / ".steam/root", home / ".steam/steam", home / ".local/share/Steam", home / ".steam/debian-installation",
			home / ".var/app/com.valvesoftware.Steam/.local/share/Steam"):
		real = Path(os.path.realpath(cand))
		if real not in out:
			out.append(real)
	return out


def find_steam_root() -> Path:
	"""$PHANTOMRING_STEAM, else the first candidate with steamapps/libraryfolders.vdf, else ~/.local/share/Steam."""
	if os.environ.get("PHANTOMRING_STEAM"):
		return Path(os.environ["PHANTOMRING_STEAM"])
	for cand in steam_root_candidates():
		if (cand / "steamapps/libraryfolders.vdf").is_file():
			return cand
	return Path.home() / ".local/share/Steam"


@dataclass
class Machine:
	steam: Path = field(default_factory=find_steam_root)
	notes: list[str] = field(default_factory=list)

	def libraries(self) -> list[Path]:
		libs = [self.steam]
		try:
			data = vdf.load(self.steam / "steamapps/libraryfolders.vdf")
			for entry in (vdf.get_ci(data, "libraryfolders") or {}).values():
				if isinstance(entry, dict) and entry.get("path") and Path(entry["path"]) not in libs:
					libs.append(Path(entry["path"]))
		except OSError:
			pass
		return libs

	def app_manifest(self, appid: int) -> tuple[Path, dict] | None:
		for lib in self.libraries():
			acf = lib / f"steamapps/appmanifest_{appid}.acf"
			if acf.exists():
				return lib, vdf.get_ci(vdf.load(acf), "AppState") or {}
		return None

	def app_dir(self, appid: int) -> Path | None:
		found = self.app_manifest(appid)
		if not found:
			return None
		lib, state = found
		installdir = state.get("installdir")
		return Path(os.path.realpath(lib / "steamapps/common" / installdir)) if installdir else None

	def compatdata(self, appid: int) -> Path:
		found = self.app_manifest(appid)
		lib = found[0] if found else self.steam
		return Path(os.path.realpath(lib / f"steamapps/compatdata/{appid}"))

	def compat_tool(self, appid: int) -> tuple[Path | None, Path | None, str]:
		"""(Proton dir, SLR dir, how it was chosen) for an app, from Steam's CompatToolMapping and the
		tool's toolmanifest.vdf require_tool_appid; defaults to Proton - Experimental + SLR 4."""
		name, how = None, "default"
		try:
			cfg = vdf.load(self.steam / "config/config.vdf")
			mapping = vdf.get_ci(cfg, "InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping") or {}
			entry = mapping.get(str(appid)) or mapping.get("0")
			if isinstance(entry, dict) and entry.get("name"):
				name = entry["name"]
				how = f"CompatToolMapping[{'app' if str(appid) in mapping else 'global'}] = {name}"
		except OSError:
			pass
		proton = None
		if name in COMPAT_TOOL_APPIDS:
			proton = self.app_dir(COMPAT_TOOL_APPIDS[name])
		elif name:
			for root in [self.steam / "compatibilitytools.d", Path("/usr/share/steam/compatibilitytools.d")]:
				for manifest in root.glob("*/compatibilitytool.vdf"):
					tools = vdf.get_ci(vdf.load(manifest), "compatibilitytools", "compat_tools") or {}
					if name in tools:
						proton = manifest.parent
		if proton is None:
			for lib in self.libraries():
				cand = Path(os.path.realpath(lib / "steamapps/common" / DEFAULT_PROTON_DIR))
				if cand.exists():
					proton, how = cand, f"{how}; fallback {DEFAULT_PROTON_DIR}"
					break
		slr = None
		if proton:
			try:
				manifest = vdf.get_ci(vdf.load(proton / "toolmanifest.vdf"), "manifest") or {}
				req = manifest.get("require_tool_appid")
				if req:
					slr = self.app_dir(int(req))
			except OSError:
				pass
		if slr is None:
			for lib in self.libraries():
				cand = Path(os.path.realpath(lib / "steamapps/common" / DEFAULT_SLR_DIR))
				if cand.exists():
					slr = cand
					break
		return proton, slr, how

	def steamid64(self) -> str | None:
		try:
			users = vdf.get_ci(vdf.load(self.steam / "config/loginusers.vdf"), "users") or {}
		except OSError:
			return None
		best = None
		for sid, u in users.items():
			if not isinstance(u, dict):
				continue
			if u.get("MostRecent") == "1" or best is None:
				best = sid
		return best

	def er_save_dir(self) -> Path | None:
		root = self.compatdata(ER_APPID) / "pfx/drive_c/users/steamuser/AppData/Roaming/EldenRing"
		sid = self.steamid64()
		if sid and (root / sid).is_dir():
			return root / sid
		dirs = [d for d in root.glob("7656119*") if d.is_dir()] if root.is_dir() else []
		return dirs[0] if len(dirs) == 1 else None

	def mgsv_save_dirs(self) -> list[tuple[str, Path]]:
		sid = self.steamid64()
		if not sid:
			return []
		account = int(sid) - STEAMID64_BASE
		user = self.steam / f"userdata/{account}"
		return [("311340-remote", user / "311340/remote"), ("287700", user / "287700")]


# ---- process inspection ---------------------------------------------------------------------------
def processes() -> list[tuple[int, str, str]]:
	"""(pid, comm, cmdline) of every process we can see."""
	out = []
	for d in Path("/proc").iterdir():
		if not d.name.isdigit():
			continue
		try:
			comm = (d / "comm").read_text().strip()
			cmd = (d / "cmdline").read_bytes().replace(b"\0", b" ").decode("utf-8", "replace").strip()
		except OSError:
			continue
		out.append((int(d.name), comm, cmd))
	return out


GAME_EXES = {"eldenring.exe": "eldenring", "mgsvtpp.exe": "mgsvtpp"}


def game_of(comm: str, argv0: str) -> str | None:
	"""Which game ('eldenring'/'mgsvtpp') a process *is*, from its own name only. Wine names a Windows
	process after its exe (comm, at most 15 chars, so both names fit) and puts the Windows path in
	argv[0]. The rest of the command line is deliberately ignored: a `bash -c '... pgrep eldenring.exe'`,
	the Proton/me3/reaper wrappers or an editor with the path open merely *mention* the exe, and taking
	them for the game made preflight and restore refuse forever (or `pr stop` signal the wrong thing)."""
	names = {comm.strip().lower()}
	# argv[0] may be a Windows path (Z:\...\eldenring.exe), a Unix path or a bare name.
	names.add(argv0.replace("\\", "/").rsplit("/", 1)[-1].strip().lower())
	for exe, game in GAME_EXES.items():
		if exe in names:
			return game
	return None


def argv0_of(cmdline: bytes) -> str:
	return cmdline.split(b"\0", 1)[0].decode("utf-8", "replace")


def game_processes(proc: Path = Path("/proc")) -> dict[str, list[int]]:
	found = {"eldenring": [], "mgsvtpp": []}
	for d in proc.iterdir():
		if not d.name.isdigit():
			continue
		try:
			game = game_of((d / "comm").read_text(), argv0_of((d / "cmdline").read_bytes()))
		except OSError:
			continue
		if game:
			found[game].append(int(d.name))
	return found


def steam_running(m: Machine) -> tuple[bool, str]:
	pid_file = Path.home() / ".steam/steam.pid"
	try:
		pid = int(pid_file.read_text().strip())
	except (OSError, ValueError):
		pid = 0
	if pid and Path(f"/proc/{pid}").exists():
		try:
			cmd = Path(f"/proc/{pid}/cmdline").read_bytes()
		except OSError:
			cmd = b""
		if b"steam" in cmd.lower():
			return True, f"Steam pid {pid}"
	for p, comm, cmd in processes():
		if comm == "steam" and "ubuntu12_32/steam" in cmd:
			return True, f"Steam pid {p}"
	return False, "Steam is not running (both games call SteamAPI_Init)"


def environ_of(pid: int) -> dict[str, str]:
	try:
		raw = Path(f"/proc/{pid}/environ").read_bytes()
	except OSError:
		return {}
	env = {}
	for item in raw.split(b"\0"):
		k, sep, v = item.partition(b"=")
		if sep:
			env[k.decode(errors="replace")] = v.decode(errors="replace")
	return env


# ---- me3 ---------------------------------------------------------------------------------------------
def me3_paths() -> tuple[Path, Path]:
	return ME3_DIR / "bin/me3", ME3_DIR / "bin/win64"


def me3_expected_digest() -> str:
	return ME3_SHA256


def find_me3_archive(archive: Path | str | None = None) -> Path | None:
	"""The me3 release archive: the explicit path, else $PHANTOMRING_ME3_ARCHIVE, else ME3_ARCHIVE_CANDIDATES."""
	if archive:
		return Path(archive).expanduser()
	if os.environ.get("PHANTOMRING_ME3_ARCHIVE"):
		return Path(os.environ["PHANTOMRING_ME3_ARCHIVE"]).expanduser()
	for cand in ME3_ARCHIVE_CANDIDATES:
		if cand.is_file():
			return cand
	return None


def me3_archive_hint() -> str:
	return (f"download {ME3_URL} into {ME3_ARCHIVE_CANDIDATES[0].parent}/ or {ME3_ARCHIVE_CANDIDATES[1].parent}/ "
		"(or pass `pr install-me3 --archive PATH`, or set PHANTOMRING_ME3_ARCHIVE)")


def install_me3(log=print, archive: Path | str | None = None) -> Path:
	"""Extract the me3 0.13.0 Linux build into third_party/me3 after checking its sha256 against the
	GitHub release's published digest. The archive is untrusted input: tarfile's 'data' filter refuses absolute
	paths, '..', links out of the tree and device files."""
	me3, win64 = me3_paths()
	if me3.exists() and (win64 / "me3-launcher.exe").exists():
		return me3
	tarball = find_me3_archive(archive)
	if tarball is None or not tarball.is_file():
		raise LaunchError(f"me3 archive missing{f': {tarball}' if tarball else ''}; {me3_archive_hint()}")
	want = me3_expected_digest()
	got = sha256(tarball)
	if got != want:
		raise LaunchError(f"me3 archive {tarball} sha256 {got} != the v0.13.0 release digest {want}")
	ME3_DIR.parent.mkdir(parents=True, exist_ok=True)
	staging = ME3_DIR.parent / f".me3-extract-{os.getpid()}"
	shutil.rmtree(staging, ignore_errors=True)
	staging.mkdir()
	try:
		with tarfile.open(tarball) as tar:
			tar.extractall(staging, filter="data")
		for need in ("bin/me3", "bin/win64/me3-launcher.exe", "bin/win64/me3_mod_host.dll"):
			if not (staging / need).is_file():
				raise LaunchError(f"me3 archive lacks {need}")
		(staging / "bin/me3").chmod(0o755)
		(staging / "PHANTOMRING-SOURCE.txt").write_text(f"{ME3_URL}\nsha256 {got} (matches the v0.13.0 release digest)\n")
		if ME3_DIR.exists():
			shutil.rmtree(ME3_DIR)
		staging.rename(ME3_DIR)
	finally:
		shutil.rmtree(staging, ignore_errors=True)
	log(f"me3 0.13.0 extracted to {ME3_DIR} (sha256 verified)")
	return me3


def check_profile(profile: Path = PROFILE) -> list[str]:
	"""Problems with the PhantomRing me3 profile (empty list = fine)."""
	problems = []
	try:
		data = tomllib.loads(profile.read_text())
	except (OSError, tomllib.TOMLDecodeError) as e:
		return [f"profile {profile}: {e}"]
	if data.get("profileVersion") != "v1":
		problems.append("profileVersion must be v1")
	if data.get("start_online") is not False:
		problems.append("start_online must be false (Elden Ring stays offline)")
	if data.get("savefile") != ER_SAVE_NAME:
		problems.append(f"savefile must be {ER_SAVE_NAME}")
	if data.get("disable_arxan") is not True:
		problems.append("disable_arxan must be true")
	if data.get("supports") != [{"game": "eldenring"}]:
		problems.append("supports must be [{game = eldenring}]")
	natives = data.get("natives", [])
	want = (BUILD / "PhantomRingHost.dll").resolve()
	if len(natives) != 1 or (profile.parent / natives[0].get("path", "")).resolve() != want:
		problems.append(f"natives must be exactly [{{path -> {want}}}]")
	if data.get("packages"):
		problems.append("no packages expected")
	return problems


# ---- saves --------------------------------------------------------------------------------------------
def copy_tree_verified(src: Path, dst: Path) -> list[dict]:
	"""Copy every file under src to dst, re-hashing both sides; returns manifest entries."""
	entries = []
	for path in sorted(p for p in src.rglob("*") if p.is_file()):
		rel = path.relative_to(src)
		target = dst / rel
		target.parent.mkdir(parents=True, exist_ok=True)
		digest = sha256(path)
		shutil.copy2(path, target)
		if sha256(target) != digest or sha256(path) != digest:
			raise LaunchError(f"backup mismatch or source changed while copying: {path}")
		entries.append({"path": str(rel), "bytes": target.stat().st_size, "sha256": digest})
	return entries


def backup_saves(m: Machine, ts: str, er: bool, mgsv: bool, log=print) -> Path:
	dest = BACKUPS / ts
	manifest: dict = {"created": ts, "sets": {}}
	if er:
		src = m.er_save_dir()
		if not src:
			raise LaunchError("can't find the Elden Ring save dir (compatdata/1245620/.../EldenRing/<steamid>)")
		manifest["sets"]["er"] = {"source": str(src), "files": copy_tree_verified(src, dest / "er")}
		log(f"backed up {len(manifest['sets']['er']['files'])} ER save files: {src} -> {dest / 'er'}")
	if mgsv:
		for name, src in m.mgsv_save_dirs():
			if src.is_dir():
				files = copy_tree_verified(src, dest / "mgsv" / name)
				manifest["sets"][f"mgsv/{name}"] = {"source": str(src), "files": files}
				log(f"backed up {len(files)} MGSV files: {src} -> {dest / 'mgsv' / name}")
			else:
				log(f"note: no MGSV save dir {src}")
	dest.mkdir(parents=True, exist_ok=True)
	(dest / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
	return dest


def verify_backup(dest: Path) -> dict:
	manifest = json.loads((dest / "manifest.json").read_text())
	for set_name, info in manifest["sets"].items():
		for e in info["files"]:
			p = dest / set_name / e["path"]
			if not p.is_file() or sha256(p) != e["sha256"]:
				raise LaunchError(f"backup {dest} is damaged: {p}")
	return manifest


def keep_backups_default() -> int:
	try:
		return int(os.environ.get("PHANTOMRING_KEEP_BACKUPS", KEEP_BACKUPS))
	except ValueError:
		return KEEP_BACKUPS


def prune_backups(keep: int, log=print, backups: Path | None = None) -> list[Path]:
	"""Delete all but the newest `keep` launch backups under backups/ (keep <= 0: delete nothing). Always kept: the
	oldest backup (the saves before PhantomRing ever ran), every *-pre-restore backup, and anything that is not a
	backup with a manifest. Only called after the new backup has verified. Returns what was removed."""
	backups = backups or BACKUPS
	if keep <= 0 or not backups.is_dir():
		return []
	sets = sorted(p for p in backups.iterdir() if p.is_dir() and (p / "manifest.json").is_file())
	if not sets:
		return []
	candidates = [p for p in sets[1:] if not p.name.endswith("-pre-restore")]
	doomed = candidates[:-keep] if len(candidates) > keep else []
	for p in doomed:
		shutil.rmtree(p)
	if doomed:
		log(f"pruned {len(doomed)} old save backup(s) from {backups} (kept the newest {keep}, the oldest {sets[0].name} "
			"and every *-pre-restore; --keep-backups 0 keeps all)")
	return doomed


def backups_summary(backups: Path | None = None) -> str:
	backups = backups or BACKUPS
	if not backups.is_dir():
		return "none yet"
	sets = [p for p in backups.iterdir() if p.is_dir() and (p / "manifest.json").is_file()]
	size = sum(f.stat().st_size for p in sets for f in p.rglob("*") if f.is_file())
	return f"{len(sets)} in {backups} ({size / 1e9:.1f} GB)"


def restore_saves(ts: str, er: bool, mgsv: bool, log=print) -> None:
	src = BACKUPS / ts
	if not (src / "manifest.json").exists():
		raise LaunchError(f"no backup {src}")
	manifest = verify_backup(src)
	running = game_processes()
	if (er and running["eldenring"]) or (mgsv and running["mgsvtpp"]):
		raise LaunchError(f"close the games first (running: {running})")
	sets = {k: v for k, v in manifest["sets"].items() if (er and k == "er") or (mgsv and k.startswith("mgsv/"))}
	if not sets:
		raise LaunchError("nothing to restore in that backup for the selected game(s)")
	# Keep what is there now, in case the restore was a mistake.
	m = Machine()
	safety = backup_saves(m, f"{timestamp()}-pre-restore", er and "er" in sets, mgsv and any(k.startswith("mgsv/") for k in sets), log)
	log(f"current saves kept in {safety}")
	for set_name, info in sets.items():
		target = Path(info["source"])
		for e in info["files"]:
			s = src / set_name / e["path"]
			t = target / e["path"]
			t.parent.mkdir(parents=True, exist_ok=True)
			shutil.copy2(s, t)
			if sha256(t) != e["sha256"]:
				raise LaunchError(f"restore verify failed: {t}")
		log(f"restored {len(info['files'])} files -> {target}")


def seed_save(save_dir: Path, seed: str | None = None) -> Path | None:
	"""The save PhantomRing0000.sl2 is first copied from: `seed` (--seed-save), else $PHANTOMRING_SEED_SAVE (a file
	name in the save dir, or a path), else the first of ER_SAVE_SOURCES that exists. None if there is none."""
	seed = seed or os.environ.get("PHANTOMRING_SEED_SAVE")
	if seed:
		p = Path(seed).expanduser()
		return p if p.is_absolute() else save_dir / p
	for name in ER_SAVE_SOURCES:
		if (save_dir / name).is_file():
			return save_dir / name
	return None


def ensure_er_save(m: Machine, log=print, seed: str | None = None) -> str:
	d = m.er_save_dir()
	if not d:
		raise LaunchError("can't find the Elden Ring save dir")
	target = d / ER_SAVE_NAME
	if target.exists():
		return f"{target} exists"
	source = seed_save(d, seed)
	if source is None or not source.is_file():
		raise LaunchError(f"{target} missing and no save to copy it from ({source or d / ER_SAVE_SOURCES[0]} not found): "
			"start Elden Ring normally once and create or load a character, or name a save with --seed-save / PHANTOMRING_SEED_SAVE")
	shutil.copy2(source, target)
	if sha256(source) != sha256(target):
		raise LaunchError(f"copy of {source} -> {target} did not verify")
	log(f"created {target} from {source.name}")
	return f"created {target}"


# ---- guest install ------------------------------------------------------------------------------------
def guest_records() -> dict:
	try:
		return json.loads(GUEST_RECORD.read_text())
	except (OSError, ValueError):
		return {"dll": [], "ini": [], "history": []}


def guest_is_ours(path: Path, kind: str) -> bool:
	if not path.exists():
		return True
	digest = sha256(path)
	recs = guest_records()
	own = BUILD / ("dinput8.dll" if kind == "dll" else "")
	return digest in recs.get(kind, []) or (kind == "dll" and own.is_file() and sha256(own) == digest)


def install_guest(mgsv_dir: Path, log=print) -> dict:
	src_dll = BUILD / "dinput8.dll"
	src_ini = ROOT / "mgsv/PhantomRingGuest.ini"
	dst_dll = mgsv_dir / "dinput8.dll"
	dst_ini = mgsv_dir / "PhantomRingGuest.ini"
	if not src_dll.is_file():
		raise LaunchError(f"{src_dll} not built")
	if not guest_is_ours(dst_dll, "dll"):
		raise LaunchError(f"{dst_dll} exists and is not a PhantomRing build (sha256 {sha256(dst_dll)}); "
			"move it away yourself if it belongs to another mod")
	recs = guest_records()
	dll_hash = sha256(src_dll)
	shutil.copy2(src_dll, dst_dll)
	if sha256(dst_dll) != dll_hash:
		raise LaunchError(f"copy to {dst_dll} did not verify")
	if dll_hash not in recs["dll"]:
		recs["dll"].append(dll_hash)
	result = {"dll": str(dst_dll), "dllSha256": dll_hash}
	if src_ini.is_file():
		if dst_ini.exists() and not (sha256(dst_ini) in recs["ini"] or sha256(dst_ini) == sha256(src_ini)):
			log(f"note: {dst_ini} differs from mgsv/PhantomRingGuest.ini and was not written by us; keeping it")
		else:
			shutil.copy2(src_ini, dst_ini)
			ini_hash = sha256(dst_ini)
			if ini_hash not in recs["ini"]:
				recs["ini"].append(ini_hash)
			result["ini"] = str(dst_ini)
			result["iniSha256"] = ini_hash
	else:
		log(f"note: {src_ini} does not exist; the guest runs with its defaults")
	recs["history"].append({"time": timestamp(), **result})
	LOGS.mkdir(parents=True, exist_ok=True)
	GUEST_RECORD.write_text(json.dumps(recs, indent=2) + "\n")
	log(f"guest installed: {dst_dll} (sha256 {dll_hash[:16]}...)")
	return result


def uninstall_guest(mgsv_dir: Path, log=print) -> None:
	if game_processes()["mgsvtpp"]:
		raise LaunchError("MGSV is running; stop it first (pr stop)")
	recs = guest_records()
	for name, kind in (("dinput8.dll", "dll"), ("PhantomRingGuest.ini", "ini")):
		p = mgsv_dir / name
		if not p.exists():
			log(f"{p}: not present")
			continue
		digest = sha256(p)
		ours = digest in recs.get(kind, []) or (kind == "dll" and (BUILD / "dinput8.dll").is_file() and sha256(BUILD / "dinput8.dll") == digest)
		if not ours:
			log(f"{p}: sha256 {digest[:16]}... is not one we installed; left alone")
			continue
		p.unlink()
		log(f"removed {p}")


# ---- the launch -----------------------------------------------------------------------------------
@dataclass
class LaunchOptions:
	mgsv: bool = True
	er: bool = True
	dry_run: bool = False
	width: int = 1280
	height: int = 720
	wait: bool = False
	# Test automation on a machine whose displays are off (no outputs: Elden Ring then presents ~1 frame/s): Elden Ring in
	# its own headless gamescope too (W x H). A player never needs it. Screenshots: gamescopectl / the host's `hud shot`.
	er_headless: tuple[int, int] | None = None
	proton_debug: str = "+timestamp,+pid,+tid,warn+seh,err+seh,+debugstr,+loaddll"
	seed_save: str | None = None  # the save PhantomRing0000.sl2 is first copied from (default: see seed_save())
	keep_backups: int | None = None  # None: keep_backups_default()


@dataclass
class Plan:
	ts: str
	log_dir: Path
	launch_id: str = ""
	checks: list[tuple[str, bool, str]] = field(default_factory=list)  # (name, ok, detail)
	mgsv_cmd: list[str] = field(default_factory=list)
	er_cmd: list[str] = field(default_factory=list)
	er_env: dict[str, str] = field(default_factory=dict)
	mgsv_dir: Path | None = None
	er_dir: Path | None = None
	info: list[str] = field(default_factory=list)

	def check(self, name: str, ok: bool, detail: str = "") -> bool:
		self.checks.append((name, ok, detail))
		return ok

	@property
	def ok(self) -> bool:
		return all(ok for _, ok, _ in self.checks)


def gamescope_has_headless(gamescope: str) -> bool:
	"""Whether this gamescope knows `--backend headless` (its --help lists the backends). Unknown -> True."""
	try:
		r = subprocess.run([gamescope, "--help"], capture_output=True, text=True, timeout=10)
	except (OSError, subprocess.SubprocessError):
		return True
	text = (r.stdout or "") + (r.stderr or "")
	return "headless" in text or not text.strip()


def build_plan(opts: LaunchOptions, m: Machine | None = None) -> Plan:
	m = m or Machine()
	ts = timestamp()
	launch_id = f"phantomring-{ts}-{os.getpid()}"
	plan = Plan(ts=ts, log_dir=fresh_log_dir(ts), launch_id=launch_id)

	ok, detail = steam_running(m)
	plan.check("Steam running", ok, detail)
	has_apps = (m.steam / "steamapps").is_dir()
	plan.check("Steam folder", has_apps, str(m.steam) + ("" if has_apps else " has no steamapps/: set PHANTOMRING_STEAM to your Steam folder"))
	running = game_processes()
	# Only the game being started must be closed: MGSV may join a running Elden Ring and vice versa
	# (the mapping is then kept, never truncated under the running game).
	if opts.er:
		plan.check("Elden Ring not running", not running["eldenring"], f"pids {running['eldenring']}" if running["eldenring"] else "")
	if opts.mgsv:
		plan.check("MGSV not running", not running["mgsvtpp"], f"pids {running['mgsvtpp']}" if running["mgsvtpp"] else "")
	plan.check("/dev/shm available", Path("/dev/shm").is_dir(), "")

	if opts.mgsv:
		mgsv_dir = m.app_dir(MGSV_APPID)
		plan.mgsv_dir = mgsv_dir
		exe = mgsv_dir / "mgsvtpp.exe" if mgsv_dir else None
		if plan.check("MGSV installed", bool(exe and exe.is_file()), str(exe)):
			state = m.app_manifest(MGSV_APPID)[1]
			plan.check("MGSV fully installed (StateFlags 4)", state.get("StateFlags") == "4", f"StateFlags {state.get('StateFlags')}")
			digest = sha256(exe)
			plan.check("mgsvtpp.exe is 1.0.15.4", digest == MGSV_SHA256, digest)
		dll = BUILD / "dinput8.dll"
		plan.check("build/dinput8.dll built", dll.is_file(), str(dll) + ("" if dll.is_file() else " (build with -B build, see README)"))
		if mgsv_dir:
			existing = mgsv_dir / "dinput8.dll"
			plan.check("MGS_TPP/dinput8.dll absent or ours", guest_is_ours(existing, "dll"),
				f"{existing} sha256 {sha256(existing)[:16]}... unknown" if existing.exists() and not guest_is_ours(existing, "dll")
				else ("present (ours, will be replaced)" if existing.exists() else "absent"))
		if not (ROOT / "mgsv/PhantomRingGuest.ini").is_file():
			plan.info.append("mgsv/PhantomRingGuest.ini missing: guest will run with its built-in defaults")
		proton, slr, how = m.compat_tool(MGSV_APPID)
		plan.check("Proton for MGSV", bool(proton and (proton / "proton").is_file()), f"{proton} ({how})")
		plan.check("Steam Linux Runtime", bool(slr and (slr / "_v2-entry-point").is_file()), str(slr))
		gamescope = shutil.which("gamescope")
		plan.check("gamescope", bool(gamescope), gamescope or "not in PATH")
		if gamescope and not gamescope_has_headless(gamescope):
			plan.info.append(f"{gamescope} --help does not mention the 'headless' backend MGSV is hidden in; a newer gamescope may be needed")
		if mgsv_dir and proton and slr:
			compat_env = {
				"STEAM_COMPAT_CLIENT_INSTALL_PATH": str(m.steam),
				"STEAM_COMPAT_DATA_PATH": str(m.compatdata(MGSV_APPID)),
				"STEAM_COMPAT_INSTALL_PATH": str(mgsv_dir),
				"STEAM_COMPAT_TOOL_PATHS": f"{proton}:{slr}",
				"STEAM_COMPAT_LIBRARY_PATHS": str(m.steam / "steamapps"),
				"STEAM_COMPAT_APP_ID": str(MGSV_APPID),
				"SteamAppId": str(MGSV_APPID),
				"SteamGameId": str(MGSV_APPID),  # keeps Proton's MGSV media workaround (winedmo) active
				"SteamOverlayGameId": str(MGSV_APPID),
				"WINEDLLOVERRIDES": "dinput8=n,b",
				"PHANTOMRING": "1",
				"PHANTOMRING_ROLE": "guest",
				"PHANTOMRING_MAPPING": plugin_mapping(),
				"PHANTOMRING_LAUNCH_ID": launch_id,
				"PROTON_LOG": "1",
				"PROTON_LOG_DIR": str(plan.log_dir),
				"WINEDEBUG": opts.proton_debug,
			}
			plan.mgsv_cmd = [
				gamescope or "gamescope", "--backend", "headless",
				"-w", str(opts.width), "-h", str(opts.height), "-W", str(opts.width), "-H", str(opts.height), "--",
				# WAYLAND_DISPLAY unset so nothing can reach the user's desktop compositor directly.
				"env", "-u", "WAYLAND_DISPLAY", *[f"{k}={v}" for k, v in compat_env.items()],
				str(slr / "_v2-entry-point"), "--verb=waitforexitandrun", "--",
				str(proton / "proton"), "waitforexitandrun", str(mgsv_dir / "mgsvtpp.exe"),
			]

	if opts.er:
		er_dir = m.app_dir(ER_APPID)
		plan.er_dir = er_dir
		exe = er_dir / "Game/eldenring.exe" if er_dir else None
		if plan.check("Elden Ring installed", bool(exe and exe.is_file()), str(exe)):
			state = m.app_manifest(ER_APPID)[1]
			plan.check("Elden Ring fully installed (StateFlags 4)", state.get("StateFlags") == "4", f"StateFlags {state.get('StateFlags')}")
			digest = sha256(exe)
			plan.check("eldenring.exe is 1.17.1 (2.7.1.0)", digest == ER_SHA256, digest)
		host = BUILD / "PhantomRingHost.dll"
		plan.check("build/PhantomRingHost.dll built", host.is_file(), str(host) + ("" if host.is_file() else " (build with -B build, see README)"))
		problems = check_profile()
		plan.check("me3 profile", not problems, "; ".join(problems) or str(PROFILE))
		me3, win64 = me3_paths()
		if me3.exists():
			plan.check("me3 0.13.0", True, str(me3))
		else:
			tarball = find_me3_archive()
			have = bool(tarball and tarball.is_file())
			plan.check("me3 0.13.0 archive", have, f"{tarball} (will be extracted to {ME3_DIR})" if have else me3_archive_hint())
		save_dir = m.er_save_dir()
		if plan.check("ER save dir", bool(save_dir), str(save_dir)):
			target = save_dir / ER_SAVE_NAME
			source = seed_save(save_dir, opts.seed_save)
			plan.check(f"{ER_SAVE_NAME} or a save to copy it from", target.exists() or bool(source and source.is_file()),
				"exists" if target.exists() else (f"will be created from {source}" if source and source.is_file() else
				f"no {source or save_dir / ER_SAVE_SOURCES[0]}: start Elden Ring normally once with a character, or pass --seed-save NAME"))
		if er_dir:
			plan.er_cmd = [str(me3), "--windows-binaries-dir", str(win64), "--crash-reporting=false", "launch", "--game", "eldenring",
				"--exe", str(er_dir / "Game/eldenring.exe"), "--profile", str(PROFILE), "--online=false", "--savefile", ER_SAVE_NAME]
			plan.er_env = {"PHANTOMRING": "1", "PHANTOMRING_ROLE": "host", "PHANTOMRING_LAUNCH_ID": launch_id, "PHANTOMRING_MAPPING": plugin_mapping(),
				"PROTON_LOG": "1", "PROTON_LOG_DIR": str(plan.log_dir), "WINEDEBUG": opts.proton_debug}
			if opts.er_headless:
				gs = shutil.which("gamescope")
				plan.check("gamescope (Elden Ring headless)", bool(gs), gs or "not in PATH")
				w, h = opts.er_headless
				plan.er_cmd = [gs or "gamescope", "--backend", "headless", "-w", str(w), "-h", str(h), "-W", str(w), "-H", str(h), "-r", "60", "--",
					"env", "-u", "WAYLAND_DISPLAY", *plan.er_cmd]
	return plan


def plugin_log_candidates(m: Machine | None = None) -> list[Path]:
	"""Where the plugins' own logs are (each writes next to its DLL), plus me3's log for the phantomring profile."""
	m = m or Machine()
	out = [BUILD / "PhantomRingHost.log"]
	mgsv_dir = m.app_dir(MGSV_APPID)
	if mgsv_dir:
		out += [mgsv_dir / "PhantomRingGuest.log", mgsv_dir / "dinput8.log"]
	out.append(Path.home() / ".local/share/me3/logs/phantomring")
	return out


def print_plan(plan: Plan, opts: LaunchOptions, m: Machine, log=print) -> None:
	log(f"PhantomRing launch plan ({'dry run' if opts.dry_run else 'live'}) {plan.ts}")
	log("preflight:")
	for name, ok, detail in plan.checks:
		log(f"  [{'ok' if ok else 'FAIL'}] {name}{': ' + detail if detail else ''}")
	for note in plan.info:
		log(f"  [note] {note}")
	log("saves (backed up before anything else):")
	if opts.er:
		log(f"  ER   {m.er_save_dir()} -> {BACKUPS / plan.ts / 'er'}")
	if opts.mgsv:
		for name, d in m.mgsv_save_dirs():
			log(f"  MGSV {d} -> {BACKUPS / plan.ts / 'mgsv' / name}{'' if d.is_dir() else '  (absent)'}")
	log(f"mapping: create + zero {proto.default_path()} ({proto.MAPPING_BYTES} bytes), or keep it if a running game has it mapped")
	if opts.mgsv and plan.mgsv_dir:
		log(f"guest: copy {BUILD / 'dinput8.dll'} + {ROOT / 'mgsv/PhantomRingGuest.ini'} -> {plan.mgsv_dir}/ (sha256 recorded in {GUEST_RECORD})")
	log(f"logs: {plan.log_dir}/ (logs/current -> it; pids in logs/current/pids.json)")
	if plan.mgsv_cmd:
		log("MGSV (hidden, headless gamescope):")
		log("  " + shlex.join(plan.mgsv_cmd))
		log(f"  stdout/stderr -> {plan.log_dir / 'mgsv-gamescope.log'}; Proton log -> {plan.log_dir / f'steam-{MGSV_APPID}.log'}")
	if plan.er_cmd:
		log("Elden Ring (me3):")
		log("  " + " ".join(f"{k}={shlex.quote(v)}" for k, v in plan.er_env.items()) + " " + shlex.join(plan.er_cmd))
		log(f"  stdout/stderr -> {plan.log_dir / 'er-me3.log'}; Proton log -> {plan.log_dir / f'steam-{ER_APPID}.log'}")
	keep = opts.keep_backups if opts.keep_backups is not None else keep_backups_default()
	log(f"save backups: {backups_summary()}; " + (f"keeping the newest {keep} (+ the oldest)" if keep > 0 else "all kept"))
	log("plugin logs (expected): " + ", ".join(str(p) for p in plugin_log_candidates(m)))


def launch(opts: LaunchOptions, log=print) -> int:
	m = Machine()
	plan = build_plan(opts, m)
	print_plan(plan, opts, m, log)
	if opts.dry_run:
		log("dry run: nothing started, nothing written." + ("" if plan.ok else " (a live launch would refuse: see FAIL above)"))
		return 0 if plan.ok else 1
	if not plan.ok:
		log("refusing to launch: fix the FAIL items above")
		return 1

	backup = backup_saves(m, plan.ts, opts.er, opts.mgsv, log)
	verify_backup(backup)
	log(f"backup verified: {backup}")
	prune_backups(opts.keep_backups if opts.keep_backups is not None else keep_backups_default(), log)
	if opts.er:
		ensure_er_save(m, log, opts.seed_save)
		install_me3(log)
		problems = check_profile()
		if problems:
			raise LaunchError("me3 profile: " + "; ".join(problems))
	# Never truncate a mapping a running game has mapped: shrinking it under the other game SIGBUSes it.
	live = live_peers(proto.default_path())
	if live:
		proto.create_mapping(proto.default_path(), zero=False)
		log(f"mapping {proto.default_path()} kept (live: {', '.join(live)})")
	else:
		proto.create_mapping(proto.default_path(), zero=True)
		log(f"mapping {proto.default_path()} created ({proto.MAPPING_BYTES} bytes, zeroed)")
	if opts.mgsv:
		install_guest(plan.mgsv_dir, log)

	plan.log_dir.mkdir(parents=True, exist_ok=True)
	current = LOGS / "current"
	if current.is_symlink() or current.exists():
		current.unlink()
	current.symlink_to(plan.log_dir.name)
	pids: dict = {"ts": plan.ts, "logDir": str(plan.log_dir)}
	children = []
	if plan.mgsv_cmd:
		out = open(plan.log_dir / "mgsv-gamescope.log", "ab")
		p = subprocess.Popen(plan.mgsv_cmd, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT, start_new_session=True,
			env=os.environ | {"PHANTOMRING_LAUNCH_ID": plan.launch_id})
		pids["mgsv"] = {"pid": p.pid, "pgid": p.pid, "cmd": plan.mgsv_cmd}
		children.append(p)
		log(f"MGSV started: gamescope pid {p.pid}")
	if plan.er_cmd:
		out = open(plan.log_dir / "er-me3.log", "ab")
		p = subprocess.Popen(plan.er_cmd, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT, start_new_session=True,
			env=os.environ | plan.er_env)
		pids["er"] = {"pid": p.pid, "pgid": p.pid, "cmd": plan.er_cmd}
		children.append(p)
		log(f"Elden Ring started: me3 pid {p.pid}")
	pids["launchId"] = plan.launch_id
	(plan.log_dir / "pids.json").write_text(json.dumps(pids, indent=2) + "\n")
	log(f"pids: {plan.log_dir / 'pids.json'}")
	log("watch it: pr status / pr watch / pr logs -f ; stop it: pr stop")
	if opts.wait:
		for c in children:
			c.wait()
	return 0


# ---- stop ---------------------------------------------------------------------------------------------
def alive(pid: int) -> bool:
	"""Running (zombies count as gone: they are only waiting for their parent to reap them)."""
	try:
		return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0] != "Z"
	except (OSError, IndexError):
		return False


def _descendants(roots: set[int]) -> set[int]:
	parent = {}
	for d in Path("/proc").iterdir():
		if d.name.isdigit():
			try:
				stat = (d / "stat").read_text()
				parent[int(d.name)] = int(stat.rsplit(")", 1)[1].split()[1])
			except (OSError, IndexError, ValueError):
				pass
	out = set(roots)
	changed = True
	while changed:
		changed = False
		for pid, ppid in parent.items():
			if ppid in out and pid not in out:
				out.add(pid)
				changed = True
	return {p for p in out if alive(p)}


def our_processes(pids: dict) -> set[int]:
	roots = {v["pid"] for k, v in pids.items() if isinstance(v, dict) and "pid" in v}
	found = _descendants(roots)
	launch_id = pids.get("launchId")
	if launch_id:
		# wineserver and pressure-vessel helpers may re-parent or setsid; the marker env follows them.
		for d in Path("/proc").iterdir():
			if d.name.isdigit() and alive(int(d.name)) and environ_of(int(d.name)).get("PHANTOMRING_LAUNCH_ID") == launch_id:
				found.add(int(d.name))
	found.discard(os.getpid())
	return found


def game_processes_launched(only: str) -> set[int]:
	"""Process trees of one game ('er' or 'mgsv') from the recent launch records, whichever launch
	started it (the two games may come from separate `pr launch` runs)."""
	role = "host" if only == "er" else "guest"
	found: set[int] = set()
	for f in sorted(LOGS.glob("*/pids.json"), reverse=True)[:12]:
		try:
			pids = json.loads(f.read_text())
		except (OSError, ValueError):
			continue
		v = pids.get(only)
		if not isinstance(v, dict) or "pid" not in v:
			continue
		found |= _descendants({v["pid"]})
		launch_id = pids.get("launchId")
		if launch_id:
			for d in Path("/proc").iterdir():
				if d.name.isdigit() and alive(int(d.name)):
					env = environ_of(int(d.name))
					if env.get("PHANTOMRING_LAUNCH_ID") == launch_id and env.get("PHANTOMRING_ROLE") == role:
						found.add(int(d.name))
	found.discard(os.getpid())
	return found


def stop(timeout: float = 20.0, kill: bool = False, log=print, only: str | None = None) -> int:
	if only:
		pids = {}
		procs = game_processes_launched(only)
		if not procs:
			log(f"no running {only} process from a pr launch")
			return 0
	else:
		f = LOGS / "current/pids.json"
		if not f.exists():
			log("nothing recorded in logs/current/pids.json (was anything started with pr launch?)")
			return 1
		pids = json.loads(f.read_text())
		procs = our_processes(pids)
	if not procs:
		log("none of the launched processes are running")
		return 0

	def comm(p):
		try:
			return Path(f"/proc/{p}/comm").read_text().strip()
		except OSError:
			return "?"

	def is_game(p):
		try:
			return game_of(comm(p), argv0_of(Path(f"/proc/{p}/cmdline").read_bytes())) is not None
		except OSError:
			return False

	games = {p for p in procs if is_game(p)}
	# The games first (a terminated Wine process exits without its own shutdown, so quit ER from its
	# menu first if you care about the last seconds of play), then everything else we started.
	for group in (games, procs - games):
		for p in group:
			try:
				os.kill(p, signal.SIGTERM)
				log(f"SIGTERM {p} ({comm(p)})")
			except ProcessLookupError:
				pass
		deadline = time.monotonic() + timeout / 2
		while time.monotonic() < deadline and any(alive(p) for p in group):
			time.sleep(0.25)
	left = {p for p in (game_processes_launched(only) if only else our_processes(pids)) if alive(p)}
	if left and kill:
		for p in left:
			try:
				os.kill(p, signal.SIGKILL)
				log(f"SIGKILL {p} ({comm(p)})")
			except ProcessLookupError:
				pass
		time.sleep(0.5)
		left = {p for p in left if alive(p)}
	if left:
		log(f"still running: {sorted(left)} (pr stop --kill to force)")
		return 1
	log("stopped")
	return 0

def live_peers(path: str) -> list[str]:
	"""Which games currently heartbeat into the mapping at `path` (empty if none or no file)."""
	try:
		link = proto.Link(path)
	except Exception:
		return []
	try:
		h = link.header
		if h.magic != proto.MAGIC or not h.qpcFrequency:
			return []
		now = proto.qpc_now()
		out = []
		for name, beat in (("Elden Ring", h.hostHeartbeatQpc), ("MGSV", h.guestHeartbeatQpc)):
			if beat and (now - beat) * 1000 // h.qpcFrequency < proto.HEARTBEAT_TIMEOUT_MS:
				out.append(name)
		return out
	finally:
		link.close()


def plugin_mapping() -> str:
	"""The mapping as the plugins name it (Wine path of $PHANTOMRING_SHM or the default file)."""
	return "file:Z:" + proto.default_path().replace("/", "\\")
