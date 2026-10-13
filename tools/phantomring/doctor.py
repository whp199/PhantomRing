"""`pr doctor`: is this machine ready for PhantomRing? Read-only: one line per check ([ok], [warn], [FAIL], [info])
with how to fix what isn't. `pr launch --dry-run` checks what one launch needs; the doctor also looks at versions,
the build, the last run's logs and the disk.

What it can't know offline, it says so: MGSV keeps its saves encrypted (D-Walker developed? the region/Terms of Use
prompt answered?), so for D-Walker it reads the guest's log of the last run, and for the Konami prompt it checks that
PhantomRing keeps MGSV offline (then the prompt never comes up).
"""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

from . import launch as L
from . import proto, version

OK, WARN, FAIL, INFO = "ok", "warn", "FAIL", "info"
BACKUPS_WARN_BYTES = 5 * 10**9


@dataclass
class Check:
	level: str
	name: str
	detail: str = ""
	fix: str = ""


@dataclass
class Report:
	checks: list[Check] = field(default_factory=list)

	def add(self, level: str, name: str, detail: str = "", fix: str = "") -> Check:
		c = Check(level, name, detail, fix)
		self.checks.append(c)
		return c

	def count(self, level: str) -> int:
		return sum(1 for c in self.checks if c.level == level)

	def text(self) -> str:
		out = [f"PhantomRing doctor -- {version.describe()}"]
		for c in self.checks:
			out.append(f"  [{c.level}]{' ' * (5 - len(c.level))}{c.name}{': ' + c.detail if c.detail else ''}")
			if c.fix and c.level in (WARN, FAIL):
				out.append(f"          fix: {c.fix}")
		out.append(f"{self.count(OK)} ok, {self.count(WARN)} warnings, {self.count(FAIL)} failures"
			+ (" -- PhantomRing should run" if not self.count(FAIL) else " -- fix the FAIL lines first"))
		return "\n".join(out)


def pe_file_version(data: bytes) -> str | None:
	"""FileVersion from a PE's VS_FIXEDFILEINFO (its 0xFEEF04BD signature), e.g. '2.7.1.0'."""
	i = data.find(struct.pack("<I", 0xFEEF04BD))
	if i < 0 or i + 16 > len(data):
		return None
	ms, ls = struct.unpack_from("<II", data, i + 8)
	return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"


def run_quiet(cmd: list[str], timeout: float = 10.0) -> str | None:
	try:
		r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
	except (OSError, subprocess.SubprocessError):
		return None
	return (r.stdout or "") + (r.stderr or "")


def check_game(r: Report, m: L.Machine, appid: int, name: str, exe_rel: str, want_sha: str, want_label: str, want_file: str | None) -> Path | None:
	d = m.app_dir(appid)
	exe = d / exe_rel if d else None
	if not exe or not exe.is_file():
		r.add(FAIL, f"{name} installed", f"not found in any Steam library ({', '.join(str(p) for p in m.libraries())})",
			f"install {name} through Steam (and run it once normally)")
		return None
	r.add(OK, f"{name} installed", str(d))
	state = (m.app_manifest(appid) or (None, {}))[1]
	if state.get("StateFlags") != "4":
		r.add(WARN, f"{name} up to date in Steam", f"StateFlags {state.get('StateFlags')} (an update or verification is pending)",
			"let Steam finish it -- but an update to a new game version stops PhantomRing until it is updated too")
	data = exe.read_bytes()
	digest = hashlib.sha256(data).hexdigest()
	fv = pe_file_version(data)
	build = state.get("buildid", "?")
	if digest == want_sha:
		r.add(OK, f"{name} version", f"{want_label} (exact match, Steam build {build})")
	else:
		r.add(FAIL, f"{name} version", f"{exe.name} {fv or '?'} (sha256 {digest[:16]}..., Steam build {build}) is not {want_label}",
			f"PhantomRing supports {want_label} only{f' ({want_file})' if want_file else ''}: wait for a PhantomRing update for this "
			"game version; the plugins stay inert on any other")
	return d


def check_build(r: Report, mgsv_dir: Path | None) -> None:
	tags = version.built_dlls(L.BUILD)
	built = all((L.BUILD / n).is_file() for n in tags)
	if not built:
		missing = [n for n in tags if not (L.BUILD / n).is_file()]
		r.add(FAIL, "PhantomRing build", f"{', '.join(missing)} missing in {L.BUILD}",
			"build it (README: cmake -S . -B build ... && cmake --build build) or unpack a release package")
		return
	problems = version.dll_problems(L.BUILD)
	tools = version.release()
	host = tags["PhantomRingHost.dll"]
	if host and host["release"] != tools:
		problems.append(f"the DLLs are v{host['release']}, these tools v{tools}")
	if problems:
		r.add(FAIL, "PhantomRing build", "; ".join(problems), "rebuild both DLLs (cmake --build build) or reinstall the whole package")
	else:
		r.add(OK, "PhantomRing build", f"both DLLs {version.describe_tag(host)}, matching these tools")
	if not mgsv_dir:
		return
	installed = mgsv_dir / "dinput8.dll"
	if not installed.exists():
		r.add(INFO, "guest in MGSV's folder", "not installed (pr launch copies it there)")
		return
	tag = version.dll_tag(installed)
	same = (L.BUILD / "dinput8.dll").is_file() and L.sha256(installed) == L.sha256(L.BUILD / "dinput8.dll")
	if same:
		r.add(OK, "guest in MGSV's folder", "the same build")
	elif version.is_phantomring_guest(installed) or L.guest_is_ours(installed, "dll"):
		r.add(INFO, "guest in MGSV's folder", f"another PhantomRing guest build ({version.describe_tag(tag)}): pr launch replaces it")
	else:
		r.add(WARN, "guest in MGSV's folder", f"{installed} is not PhantomRing's (another mod's dinput8.dll?)",
			"move it out of MGSV's folder while you play PhantomRing (pr launch refuses to overwrite it)")


def check_me3(r: Report) -> None:
	me3, win64 = L.me3_paths()
	if me3.is_file():
		out = run_quiet([str(me3), "--version"]) or ""
		m = re.search(r"me3\s+v?([0-9][0-9A-Za-z.\-]*)", out)
		ver = m.group(1) if m else None
		if ver == "0.13.0" and (win64 / "me3-launcher.exe").is_file():
			r.add(OK, "Mod Engine 3", f"me3 {ver} ({me3})")
		elif ver:
			r.add(WARN, "Mod Engine 3", f"me3 {ver} at {me3}; PhantomRing is tested with 0.13.0", f"remove {L.ME3_DIR} and run pr install-me3 with the 0.13.0 archive")
		else:
			r.add(FAIL, "Mod Engine 3", f"{me3} did not say its version ({out.strip()[:80]!r})", f"remove {L.ME3_DIR} and run pr install-me3")
	else:
		archive = L.find_me3_archive()
		if archive and archive.is_file():
			r.add(WARN, "Mod Engine 3", f"not extracted yet; archive {archive}", "pr install-me3 (pr launch also does it)")
		else:
			r.add(FAIL, "Mod Engine 3", "me3 0.13.0 not installed and its archive not found", L.me3_archive_hint() + ", then pr install-me3")
	problems = L.check_profile()
	if problems:
		r.add(FAIL, "me3 profile", "; ".join(problems), "restore tools/phantomring/profile/phantomring.me3 from the release")
	else:
		r.add(OK, "me3 profile", "offline, isolated save, the host plugin")


def check_gamescope(r: Report) -> None:
	gs = shutil.which("gamescope")
	if not gs:
		r.add(FAIL, "gamescope", "not in PATH", "install gamescope (MGSV runs hidden in its headless backend)")
		return
	out = re.sub(r"\x1b\[[0-9;]*m", "", run_quiet([gs, "--version"]) or "")
	m = re.search(r"gamescope version\s+v?([0-9][^\s]*)", out)
	headless = L.gamescope_has_headless(gs)
	detail = f"{gs}{f' {m.group(1)}' if m else ''}"
	if headless:
		r.add(OK, "gamescope", detail + ", headless backend")
	else:
		r.add(FAIL, "gamescope", detail + ": --help does not list the headless backend", "update gamescope (3.14 or newer has --backend headless)")


def check_proton(r: Report, m: L.Machine) -> None:
	proton, slr, how = m.compat_tool(L.MGSV_APPID)
	if proton and (proton / "proton").is_file():
		try:
			ver = (proton / "version").read_text().split()[-1]
		except (OSError, IndexError):
			ver = "?"
		r.add(OK, "Proton", f"{proton.name} ({ver}; {how})")
	else:
		r.add(FAIL, "Proton", f"no Proton found ({how})", "install Proton Experimental from Steam (Library > Tools) or pick one for MGSV")
	if slr and (slr / "_v2-entry-point").is_file():
		r.add(OK, "Steam Linux Runtime", slr.name)
	else:
		r.add(FAIL, "Steam Linux Runtime", "not found", "install 'Steam Linux Runtime 4.0' from Steam (Library > Tools); starting any Proton game once also does it")


def last_buddy_result(log: Path, buddy: str = "D-Walker") -> str | None:
	"""The guest's last 'boot: D-Walker: <result>' line (mgsv/Pump.cpp), e.g. 'active' or 'missing: not developed in this save'."""
	try:
		text = log.read_text(errors="replace")
	except OSError:
		return None
	found = re.findall(rf"boot: {re.escape(buddy)}: ([^\n]+)", text)
	return found[-1].strip() if found else None


def check_mgsv_extras(r: Report, mgsv_dir: Path | None) -> None:
	# Konami's region / Terms of Use prompt (July 2026 patch): its answer lives in MGSV's encrypted save and on Konami's
	# servers, nothing PhantomRing can read. What matters is whether MGSV goes online at all under PhantomRing.
	ini = (mgsv_dir / "PhantomRingGuest.ini") if mgsv_dir and (mgsv_dir / "PhantomRingGuest.ini").is_file() else L.ROOT / "mgsv/PhantomRingGuest.ini"
	offline = None
	try:
		section = ""
		for line in ini.read_text(errors="replace").splitlines():
			s = line.split(";", 1)[0].strip()
			if s.startswith("[") and s.endswith("]"):
				section = s[1:-1].strip().lower()
			elif section == "online" and "=" in s:
				k, v = (x.strip() for x in s.split("=", 1))
				if k.lower() == "offline":
					offline = v.lower() in ("1", "true", "yes", "on")
	except OSError:
		pass
	if offline is False:
		r.add(WARN, "MGSV region / Terms of Use", f"{ini}: [Online] Offline = false, so MGSV logs in to Konami",
			"if its first login asks for a country/region and the Terms of Use, answer it yourself in a normal MGSV session first "
			"(PhantomRing never does), or set Offline = true")
	else:
		r.add(OK, "MGSV region / Terms of Use", "MGSV runs offline under PhantomRing, so Konami's login prompt never comes up "
			"(whether it was answered can't be read offline: it is in MGSV's encrypted save)")
	log = mgsv_dir / "PhantomRingGuest.log" if mgsv_dir else None
	result = last_buddy_result(log) if log else None
	if result is None:
		r.add(INFO, "D-Walker developed", "unknown: MGSV's save is encrypted; the first pr launch tells (Elden Ring's corner, and "
			"'boot: D-Walker: ...' in PhantomRingGuest.log)")
	elif result.startswith("active"):
		r.add(OK, "D-Walker developed", f"deployed in the last run ({result})")
	elif result.startswith("missing"):
		r.add(WARN, "D-Walker developed", f"the last run said: {result}",
			"develop D-Walker in MGSV (it is optional: Snake plays without a buddy)")
	else:
		r.add(INFO, "D-Walker developed", f"the last run said: {result}")


def check_saves(r: Report, m: L.Machine) -> None:
	d = m.er_save_dir()
	if not d:
		r.add(FAIL, "Elden Ring save", "no save folder found (compatdata/1245620/.../EldenRing/<steam id>)",
			"start Elden Ring normally once and create or load a character")
	elif (d / L.ER_SAVE_NAME).is_file():
		r.add(OK, "Elden Ring save", f"{L.ER_SAVE_NAME} exists (PhantomRing's own copy)")
	elif L.seed_save(d) and L.seed_save(d).is_file():
		r.add(OK, "Elden Ring save", f"{L.ER_SAVE_NAME} will be copied from {L.seed_save(d).name} on the first launch")
	else:
		r.add(FAIL, "Elden Ring save", f"no {L.ER_SAVE_SOURCES[0]} to copy {L.ER_SAVE_NAME} from in {d}",
			"start Elden Ring normally once with a character, or pass pr launch --seed-save NAME")
	dirs = [p for _, p in m.mgsv_save_dirs() if p.is_dir()]
	if dirs:
		r.add(OK, "MGSV save", ", ".join(str(p) for p in dirs))
	else:
		r.add(WARN, "MGSV save", "no MGSV save folder under Steam's userdata",
			"play MGSV normally until Continue loads a save that can go to Africa (free roam)")
	sets = [p for p in L.BACKUPS.iterdir() if p.is_dir() and (p / "manifest.json").is_file()] if L.BACKUPS.is_dir() else []
	size = sum(f.stat().st_size for p in sets for f in p.rglob("*") if f.is_file())
	if size > BACKUPS_WARN_BYTES:
		r.add(WARN, "save backups", f"{len(sets)} in {L.BACKUPS} ({size / 1e9:.1f} GB)",
			"prune: pr launch --keep-backups N (or PHANTOMRING_KEEP_BACKUPS=N) keeps the newest N per game and the first")
	else:
		r.add(OK, "save backups", f"{len(sets)} in {L.BACKUPS} ({size / 1e9:.1f} GB)" if sets else "none yet (pr launch makes one before every start)")


def check_system(r: Report) -> None:
	py = sys.version_info
	if py >= (3, 11):
		r.add(OK, "Python", f"{py.major}.{py.minor}.{py.micro}")
	else:
		r.add(FAIL, "Python", f"{py.major}.{py.minor} (3.11 or newer needed)", "install a newer python3")
	try:
		st = os.statvfs("/dev/shm")
		free = st.f_bavail * st.f_frsize
		if free >= proto.MAPPING_BYTES:
			r.add(OK, "/dev/shm", f"{free / 2**20:,.0f} MB free (the link needs {proto.MAPPING_BYTES / 2**20:.0f} MB)")
		else:
			r.add(FAIL, "/dev/shm", f"{free / 2**20:,.0f} MB free, the link needs {proto.MAPPING_BYTES / 2**20:.0f} MB",
				"free /dev/shm (or enlarge it); stale /dev/shm/phantomring_* files from crashed runs can be removed while no game runs")
	except OSError as e:
		r.add(FAIL, "/dev/shm", str(e), "PhantomRing's link is a file in /dev/shm")


def diagnose(m: L.Machine | None = None) -> Report:
	r = Report()
	check_system(r)
	m = m or L.Machine()
	if (m.steam / "steamapps").is_dir():
		r.add(OK, "Steam folder", str(m.steam))
	else:
		r.add(FAIL, "Steam folder", f"{m.steam} has no steamapps/", "set PHANTOMRING_STEAM to your Steam folder (the one with steamapps/libraryfolders.vdf)")
	running, detail = L.steam_running(m)
	r.add(OK if running else WARN, "Steam running", detail, "start Steam and sign in before pr launch (both games need it)")
	check_game(r, m, L.ER_APPID, "Elden Ring", "Game/eldenring.exe", L.ER_SHA256, "1.17.1 (eldenring.exe 2.7.1.0)", "eldenring.exe 2.7.1.0")
	mgsv_dir = check_game(r, m, L.MGSV_APPID, "MGSV", "mgsvtpp.exe", L.MGSV_SHA256, "1.0.15.4", None)
	check_proton(r, m)
	check_gamescope(r)
	check_me3(r)
	check_build(r, mgsv_dir)
	check_saves(r, m)
	check_mgsv_extras(r, mgsv_dir)
	games = L.game_processes()
	if games["eldenring"] or games["mgsvtpp"]:
		r.add(INFO, "games running", ", ".join(f"{k} {v}" for k, v in games.items() if v))
	return r


def main(args=None) -> int:
	report = diagnose()
	print(report.text())
	return 1 if report.count(FAIL) else 0
