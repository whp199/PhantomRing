"""package_common.py - what tools/package_windows.py and tools/package_linux.py share.

Both packages are reproducible (sorted entries, fixed timestamps and owners, no build paths): the same commit built
with the same toolchain gives byte-identical archives. Each carries a BUILD.txt (version, commit, protocol, the DLLs'
own build tags, the supported game versions, the sha256 of every file) and gets a `<archive>.sha256` next to it in
sha256sum's format (the release job concatenates them into SHA256SUMS).
"""
from __future__ import annotations

import hashlib
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import check_imports  # noqa: E402
from phantomring import launch as L  # noqa: E402
from phantomring import proto, version  # noqa: E402

# 2026-01-01 00:00:00 UTC: every file in both archives gets this time (zip: as local time, the format has no zone).
FIXED_TIME = (2026, 1, 1, 0, 0, 0)
FIXED_EPOCH = 1767225600
DLLS = ("PhantomRingHost.dll", "dinput8.dll")

SUPPORTED = [
	f"ELDEN RING 1.17.1 (eldenring.exe 2.7.1.0, sha256 {L.ER_SHA256})",
	f"METAL GEAR SOLID V: THE PHANTOM PAIN 1.0.15.4 (mgsvtpp.exe sha256 {L.MGSV_SHA256})",
	f"Mod Engine 3 (me3) 0.13.0 (not included: the player downloads it; tools/pr install-me3 checks sha256 {L.ME3_SHA256})",
]


def sha256_bytes(data: bytes) -> str:
	return hashlib.sha256(data).hexdigest()


def build(build_dir: Path) -> None:
	"""Both DLLs, through the shared build lock (other builds may use the same tree)."""
	lock = build_dir / ".buildlock"
	lock.touch(exist_ok=True)
	subprocess.run(["flock", str(lock), "cmake", "--build", str(build_dir), "--target", "PhantomRingHost", "PhantomRingGuest"], check=True)


def is_pe64_dll(path: Path) -> bool:
	data = path.read_bytes()[:4096]
	if data[:2] != b"MZ" or len(data) < 0x40:
		return False
	pe = int.from_bytes(data[0x3C:0x40], "little")
	if data[pe:pe + 4] != b"PE\0\0":
		return False
	machine = int.from_bytes(data[pe + 4:pe + 6], "little")
	characteristics = int.from_bytes(data[pe + 22:pe + 24], "little")
	return machine == 0x8664 and bool(characteristics & 0x2000)  # AMD64, IMAGE_FILE_DLL


def check_dlls(build_dir: Path, allow_other_commit: bool = False) -> tuple[dict, list[str]]:
	"""(the DLLs' tags, problems). The DLLs must be 64-bit PE DLLs importing only stock Windows DLLs, carry the same
	build tag, speak these tools' protocol and (unless allowed) come from the checkout's current commit, so that the
	package's DLLs, inis and tools belong together."""
	problems = []
	for name in DLLS:
		p = build_dir / name
		if not p.is_file():
			problems.append(f"missing {p} (build first, or drop --no-build)")
		elif not is_pe64_dll(p):
			problems.append(f"{p} is not a 64-bit Windows DLL")
	if problems:
		return {}, problems
	if check_imports.check([str(build_dir / n) for n in DLLS]):
		problems.append("unexpected DLL imports (above)")
	tags = version.built_dlls(build_dir)
	problems += version.dll_problems(build_dir)
	host = tags.get("PhantomRingHost.dll") or {}
	rel = version.release()
	if host and host["release"] != rel:
		problems.append(f"the DLLs are {host['release']}, VERSION says {rel}: rebuild")
	here = version.commit()
	if host and host.get("commit") != here and not allow_other_commit:
		problems.append(f"the DLLs were built from {host.get('commit')}, the checkout is at {here}: rebuild (or --allow-other-commit)")
	return tags, problems


def build_txt(platform: str, tags: dict, files: dict[str, bytes]) -> str:
	"""BUILD.txt: what this package is, and the sha256 of each of its files (names relative to the package folder)."""
	host = tags.get("PhantomRingHost.dll") or {}
	lines = [
		f"PhantomRing {version.release()} for {platform}",
		f"commit: {host.get('commit', version.commit())}",
		f"protocol: {proto.VERSION}",
	]
	for name in DLLS:
		lines.append(f"{name}: {version.describe_tag(tags.get(name))}")
	lines += ["", "Supported game versions (the plugins do nothing on any other):"]
	lines += [f"  {s}" for s in SUPPORTED]
	lines += ["", "sha256:"]
	lines += [f"  {sha256_bytes(data)}  {name}" for name, data in sorted(files.items())]
	return "\n".join(lines) + "\n"


def write_sha256(archive: Path) -> Path:
	"""<archive>.sha256 in sha256sum's format (`sha256sum -c` checks it next to the archive)."""
	out = archive.with_name(archive.name + ".sha256")
	out.write_text(f"{sha256_bytes(archive.read_bytes())}  {archive.name}\n")
	return out
