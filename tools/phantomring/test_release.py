"""Offline checks of the release tooling: versions (VERSION, the DLLs' build tags, the mismatch wording), the two
packagers (reproducible, complete, their sha256 lists), `pr doctor` on an empty fake machine, `pr collect-logs`'s
redaction, and recognising any PhantomRing guest build in MGSV's folder. Temp dirs only.

    python3 tools/phantomring/test_release.py [build]     # build: the build dir (default build/); package checks skip without it
"""

from __future__ import annotations

import hashlib
import io
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from phantomring import collect, doctor, proto, version  # noqa: E402
from phantomring import launch as L  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
failures: list[str] = []


def check(cond, what, detail=""):
	print(f"  [{'ok' if cond else 'FAIL'}] {what}{f'  ({detail})' if detail else ''}")
	if not cond:
		failures.append(what)


def versions() -> None:
	print("versions:")
	rel = version.release()
	check(bool(version.RELEASE_RE.match(rel)), "VERSION is MAJOR.MINOR.PATCH[-PRE]", rel)
	cmake = (ROOT / "CMakeLists.txt").read_text()
	check('file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PHANTOM_RELEASE' in cmake and "project(PhantomRing VERSION ${PHANTOM_VERSION_CORE}" in cmake,
		"CMake takes its project version from VERSION")
	check(not re.search(r"project\(PhantomRing VERSION [0-9]", cmake), "no second version number in CMakeLists.txt")
	blob = b"MZ...\0PHANTOMRING_BUILD release=1.2.3-rc.1 commit=0123456789ab-dirty protocol=42\0more"
	with tempfile.TemporaryDirectory() as tmp:
		p = Path(tmp) / "x.dll"
		p.write_bytes(blob)
		tag = version.dll_tag(p)
		check(tag == {"release": "1.2.3-rc.1", "commit": "0123456789ab-dirty", "protocol": 42}, "build tag parsed from DLL bytes", tag)
		check(version.is_phantomring_guest(p), "a tagged DLL is a PhantomRing guest")
		p.write_bytes(b"MZ old guest " + "PhantomRingGuest.log".encode("utf-16-le"))
		check(version.dll_tag(p) is None and version.is_phantomring_guest(p), "an untagged pre-0.2.0 guest is still recognised")
		p.write_bytes(b"MZ somebody else's proxy")
		check(not version.is_phantomring_guest(p), "another mod's dinput8.dll is not")
		(Path(tmp) / "BUILD.txt").write_text("PhantomRing 9.9.9 for Linux\ncommit: feedfacecafe\nprotocol: 11\n")
		check(version.commit(Path(tmp)) == "feedfacecafe", "a package's commit comes from its BUILD.txt")
		check(version.release(Path(tmp)) == "unknown", "no VERSION file: unknown")
	# The wording the player sees: proto.py's copy must say exactly what common/Version.cpp says (test_common checks that one).
	check(proto.mismatch_text("0.2.0-alpha", 11, "0.3.0", 12) ==
		"PhantomRing versions differ: Elden Ring plugin v0.2.0-alpha, MGSV plugin v0.3.0 -- reinstall both", "mismatch: two releases")
	check(proto.mismatch_text("0.2.0-alpha", 11, "0.2.0-alpha", 12) ==
		"PhantomRing versions differ: Elden Ring plugin v0.2.0-alpha (protocol 11), MGSV plugin v0.2.0-alpha (protocol 12) -- reinstall both",
		"mismatch: one release, two protocols")
	check(proto.mismatch_text("", 10, "0.2.0-alpha", 11) ==
		"PhantomRing versions differ: Elden Ring plugin from before v0.2.0-alpha (protocol 10), MGSV plugin v0.2.0-alpha -- reinstall both",
		"mismatch: an older host")
	h = proto.Header()
	h.magic, h.version, h.guestProtocol = proto.MAGIC, proto.VERSION, proto.VERSION
	check(proto.version_mismatch(h) is None, "same protocol: no mismatch")
	h.guestProtocol = proto.VERSION + 1
	proto.set_text(h, "hostRelease", "0.2.0-alpha")
	proto.set_text(h, "guestRelease", "0.9.0")
	check((proto.version_mismatch(h) or "").endswith("MGSV plugin v0.9.0 -- reinstall both"), "header mismatch read", proto.version_mismatch(h))


def guest_is_ours() -> None:
	print("guest install:")
	with tempfile.TemporaryDirectory() as tmp:
		saved = L.GUEST_RECORD
		L.GUEST_RECORD = Path(tmp) / "guest-install.json"  # no records at all: as in a freshly unpacked package
		try:
			p = Path(tmp) / "dinput8.dll"
			p.write_bytes(b"MZ\0PHANTOMRING_BUILD release=0.1.9 commit=abc protocol=10\0")
			check(L.guest_is_ours(p, "dll"), "a guest another checkout/package installed is ours to replace")
			p.write_bytes(b"MZ IHHook")
			check(not L.guest_is_ours(p, "dll"), "IHHook's dinput8.dll is not")
		finally:
			L.GUEST_RECORD = saved


def redaction() -> None:
	print("collect-logs redaction:")
	r = collect.Redactor(home="/home/alice", user="alice", host="workbox", steam_ids=["76561198000000042", "39734314"])
	text = ("loading from Z:\\home\\alice\\PhantomRing\\build\\ and /home/alice/PhantomRing/logs\n"
		"MGS_TPP at /run/media/alice/ext/steam-library, Z:\\run\\media\\alice\\ext\n"
		"userdata/39734314/287700 EldenRing/76561198000000042/ER0000.sl2 ssh alice@workbox user=alice\n"
		"alicette and malice stay; 739734314 stays")
	out = r(text)
	check("alice" not in out.replace("alicette", "").replace("malice", ""), "user name gone from paths", out)
	check("Z:\\~\\PhantomRing" in out and "~/PhantomRing/logs" in out, "home folder becomes ~")
	check("/run/media/<user>/ext" in out and "\\media\\<user>\\ext" in out, "user name inside other paths")
	check("userdata/<account>/" in out and "EldenRing/<steamid>/" in out, "Steam ids")
	check("<user>@<host>" in out and "user=<user>" in out, "address and key=value")
	check("alicette and malice stay; 739734314 stays" in out, "ordinary words and other numbers stay")


def doctor_on_empty_machine() -> None:
	print("pr doctor on an empty machine:")
	with tempfile.TemporaryDirectory() as tmp:
		steam = Path(tmp) / "Steam"
		(steam / "steamapps").mkdir(parents=True)
		saved = L.BUILD, L.BACKUPS
		L.BUILD, L.BACKUPS = Path(tmp) / "build", Path(tmp) / "backups"
		try:
			report = doctor.diagnose(L.Machine(steam=steam))
		finally:
			L.BUILD, L.BACKUPS = saved
		names = {c.name: c for c in report.checks}
		check(names["Elden Ring installed"].level == doctor.FAIL and names["MGSV installed"].level == doctor.FAIL, "missing games FAIL")
		check(names["PhantomRing build"].level == doctor.FAIL and "cmake" in names["PhantomRing build"].fix, "missing build FAIL, with how to build")
		check(names["Elden Ring save"].level == doctor.FAIL, "no Elden Ring save FAIL")
		check(names["D-Walker developed"].level == doctor.INFO, "D-Walker unknown without a guest log")
		text = report.text()
		check("fix:" in text and text.rstrip().endswith("fix the FAIL lines first"), "fixes and the verdict printed")
		log = Path(tmp) / "PhantomRingGuest.log"
		log.write_text("[1] boot: D-Walker: deploying\n[2] boot: D-Walker: missing: not developed in this save\n")
		check(doctor.last_buddy_result(log) == "missing: not developed in this save", "the last run's D-Walker result read")
	exe = b"MZ" + b"\0" * 64 + bytes.fromhex("bd04effe") + b"\0\0\1\0" + (2 << 16 | 7).to_bytes(4, "little") + (1 << 16 | 0).to_bytes(4, "little")
	check(doctor.pe_file_version(exe) == "2.7.1.0", "PE file version read", doctor.pe_file_version(exe))


def packages(build: Path) -> None:
	print("packages:")
	if not all((build / n).is_file() for n in ("PhantomRingHost.dll", "dinput8.dll")):
		print(f"  (skipped: no DLLs in {build})")
		return
	tags = version.built_dlls(build)
	check(all(t and t["release"] == version.release() and t["protocol"] == proto.VERSION for t in tags.values()),
		"both built DLLs carry VERSION and this protocol", tags)
	with tempfile.TemporaryDirectory() as tmp:
		outs = []
		for run in ("a", "b"):
			for script in ("package_linux.py", "package_windows.py"):
				r = subprocess.run([sys.executable, "-I", str(ROOT / "tools" / script), "--no-build", "--allow-other-commit", "--build-dir", str(build),
					"--out", str(Path(tmp) / run)], capture_output=True, text=True)
				check(r.returncode == 0, f"{script} ({run})", (r.stdout + r.stderr).strip().splitlines()[-1:] if r.returncode else "")
			outs.append(sorted((Path(tmp) / run).iterdir()))
		a, b = outs
		check([p.name for p in a] == [p.name for p in b] and len(a) == 4, "two archives and two .sha256 each time", [p.name for p in a])
		check(all(x.read_bytes() == y.read_bytes() for x, y in zip(a, b)), "both packages are reproducible (byte-identical)")
		for sums in (p for p in a if p.name.endswith(".sha256")):
			digest, name = sums.read_text().split()
			check(hashlib.sha256((sums.parent / name).read_bytes()).hexdigest() == digest, f"{sums.name} matches")
		ver = version.release()
		tgz = Path(tmp) / "a" / f"PhantomRing-{ver}-linux.tar.gz"
		with tarfile.open(tgz) as tar:
			members = {m.name: m for m in tar.getmembers()}
			prefix = f"PhantomRing-{ver}-linux/"
			need = ["build/PhantomRingHost.dll", "build/dinput8.dll", "build/PhantomRingHost.ini", "mgsv/PhantomRingGuest.ini", "tools/pr",
				"tools/phantomring/cli.py", "tools/phantomring/profile/phantomring.me3", "LICENSE", "THIRD-PARTY-NOTICES.md", "README.md", "VERSION", "BUILD.txt"]
			check(all(prefix + n in members for n in need), "Linux package complete", [n for n in need if prefix + n not in members])
			check(not any("/test_" in n for n in members), "no tests in the Linux package")
			check(members[prefix + "tools/pr"].mode == 0o755 and all(m.uid == 0 and m.mtime == 1767225600 for m in members.values()),
				"tools/pr executable; fixed owner and time")
			build_txt = tar.extractfile(prefix + "BUILD.txt").read().decode()
			listed = dict(reversed(line.split()) for line in build_txt.split("sha256:\n", 1)[1].splitlines() if line.strip())
			ok = all(hashlib.sha256(tar.extractfile(prefix + n).read()).hexdigest() == h for n, h in listed.items())
			check(ok and len(listed) == len([m for m in members.values() if m.isfile()]) - 1, "BUILD.txt lists every file's sha256", len(listed))
			check(f"PhantomRing {ver} for Linux" in build_txt and "2.7.1.0" in build_txt and "1.0.15.4" in build_txt, "BUILD.txt: version, game versions")
			# The unpacked package's pr knows its version from VERSION + BUILD.txt.
			unpack = Path(tmp) / "unpacked"
			tar.extractall(unpack, filter="data")
		pr = unpack / prefix / "tools/pr"
		r = subprocess.run([str(pr), "--version"], capture_output=True, text=True)
		tag = tags["PhantomRingHost.dll"]
		check(r.returncode == 0 and r.stdout.startswith(f"PhantomRing v{ver} (commit {tag['commit']}") and "note:" not in r.stdout,
			"pr --version from the unpacked package", r.stdout.strip().splitlines()[:1])
		with zipfile.ZipFile(Path(tmp) / "a" / f"PhantomRing-{ver}-windows.zip") as z:
			names = z.namelist()
			build_txt = z.read("PhantomRing/BUILD.txt").decode()
			collect = z.read("PhantomRing/collect-logs.cmd")
		check("PhantomRing/mgsv/dinput8.dll" in names and "PhantomRing/phantomring.me3" in names and "PhantomRing/collect-logs.cmd" in names,
			"Windows package complete")
		check(collect_logs_ok(collect), "collect-logs.cmd: CRLF, the cmd/PowerShell header, every file it gathers", collect_logs_ok(collect, why=True))
		check(f"PhantomRing {ver} for Windows" in build_txt and "\r\n" in build_txt, "Windows BUILD.txt (CRLF)")


def collect_logs_ok(data: bytes, why: bool = False):
	"""tools/windows/collect-logs.cmd as packed: CRLF only (cmd.exe), the batch part hidden from PowerShell in <# ... #>
	(cmd reads line 1 as a label), the batch part hands the file to PowerShell, and the script gathers what
	docs/WINDOWS.md "For testers" promises. True, or (why=True) the reasons it isn't."""
	text = data.decode("utf-8", "replace")
	problems = []
	if b"\n" in data.replace(b"\r\n", b"") or not data.endswith(b"\r\n"):
		problems.append("not CRLF throughout")
	lines = text.split("\r\n")
	if not lines[0].startswith("<# :"):
		problems.append("line 1 is not '<# :'")
	try:
		end = lines.index("#>")
	except ValueError:
		end = -1
		problems.append("no '#>' line")
	batch = lines[1:end] if end > 0 else []
	if not any("powershell.exe" in line and "PR_SELF" in line for line in batch) or not any(line.startswith("exit /b") for line in batch):
		problems.append("the batch part does not run PowerShell and exit")
	if any(c in line for line in batch if line.lower().startswith("rem ") for c in "<>|&"):
		problems.append("a rem line in the batch part has < > | &")
	script = "\n".join(lines[end + 1:]) if end > 0 else ""
	for need in ("PhantomRingHost.log", "PhantomRingHost.ini", "BUILD.txt", "PhantomRingGuest.log", "PhantomRingGuest.ini", "dxdiag.exe",
			"launcher: MGSV is", "MgsvExe", "libraryfolders.vdf", "Compress-Archive", "GetFolderPath('Desktop')", "Redact", "<steamid>"):
		if need not in script:
			problems.append(f"the script lacks {need!r}")
	# Review 2026-10-14: dxdiag's raw report (PC name, user paths) was written into $work, the folder the zip packs whole; a
	# slow dxdiag or a locked file left it there. It must be written outside $work and removed in a finally.
	dx = next((line for line in script.split("\n") if line.lstrip().startswith("$dx =")), "")
	if not dx or "$work" in dx or "GetTempPath()" not in dx:
		problems.append("dxdiag's raw report is not written outside $work")
	if "finally { Remove-Item -LiteralPath $dx" not in script:
		problems.append("dxdiag's raw report is not removed in a finally")
	return problems if why else not problems


def shipped_inis() -> None:
	"""The shipped inis are standard INI (review: a prose line in [Combat] had lost its ';'; common/Ini.cpp skipped it only
	because it had no '='): every line is blank, a comment, a [section] or key = value."""
	print("shipped inis:")
	import configparser
	for rel in ("er/PhantomRingHost.ini", "mgsv/PhantomRingGuest.ini"):
		text = (ROOT / rel).read_text(encoding="utf-8")
		bad = [f"{n}: {line[:60]}" for n, line in enumerate(text.splitlines(), 1)
			if line.strip() and not line.lstrip().startswith((";", "#", "[")) and "=" not in line]
		check(not bad, f"{rel}: every line a comment, a section or a key", "; ".join(bad[:3]))
		c = configparser.ConfigParser(strict=False, interpolation=None)
		try:
			c.read_string(text)
			check(len(c.sections()) > 0, f"{rel}: Python's configparser reads it", f"{len(c.sections())} sections")
		except configparser.Error as e:
			check(False, f"{rel}: Python's configparser reads it", str(e).splitlines()[0])


def main() -> int:
	build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build"
	versions()
	shipped_inis()
	guest_is_ours()
	redaction()
	doctor_on_empty_machine()
	packages(build)
	print("PASS" if not failures else f"FAIL: {failures}")
	return 1 if failures else 0


if __name__ == "__main__":
	sys.exit(main())
