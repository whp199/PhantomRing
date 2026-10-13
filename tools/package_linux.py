#!/usr/bin/env python3
"""package_linux.py - pack PhantomRing for a Linux player (README "Install").

    python3 -I tools/package_linux.py [--build-dir build] [--out dist] [--no-build] [--allow-other-commit]

Builds (unless --no-build) with the MinGW toolchain already configured in --build-dir, checks both DLLs (64-bit PE,
stock Windows imports only, the same build tag, this checkout's commit and protocol) and writes
dist/PhantomRing-<version>-linux.tar.gz plus its .sha256. The tarball unpacks to the layout `tools/pr` expects in a
checkout, so every pr command works from it as it does from a clone:

    PhantomRing-<version>-linux/
      build/PhantomRingHost.dll     Elden Ring plugin (me3 native), and its ini (read from next to the DLL)
      build/PhantomRingHost.ini
      build/dinput8.dll             MGSV guest; `pr launch` copies it into MGSV's folder
      mgsv/PhantomRingGuest.ini
      tools/pr                      the launcher and consoles (Python 3.11+, standard library only)
      tools/phantomring/            ...its modules (no tests) and profile/phantomring.me3 (Mod Engine 3)
      README.md  docs/TOOLS.md  LICENSE  THIRD-PARTY-NOTICES.md  VERSION
      BUILD.txt                     version, commit, protocol, supported game versions, sha256 of every file

me3 itself is not included (tools/pr install-me3 checks the archive the player downloads). The tarball is
reproducible: sorted entries, fixed time/owner/mode, gzip without a name or time.
"""
from __future__ import annotations

import argparse
import gzip
import io
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import package_common as P  # noqa: E402
from phantomring import launch as L  # noqa: E402
from phantomring import version  # noqa: E402

EXECUTABLE = {"tools/pr"}


def entries(build: Path) -> dict[str, Path]:
	"""Path inside the package -> source file."""
	out = {
		"build/PhantomRingHost.dll": build / "PhantomRingHost.dll",
		"build/PhantomRingHost.ini": ROOT / "er/PhantomRingHost.ini",
		"build/dinput8.dll": build / "dinput8.dll",
		"mgsv/PhantomRingGuest.ini": ROOT / "mgsv/PhantomRingGuest.ini",
		"tools/pr": ROOT / "tools/pr",
		"tools/phantomring/profile/phantomring.me3": ROOT / "tools/phantomring/profile/phantomring.me3",
		"README.md": ROOT / "README.md",
		"docs/TOOLS.md": ROOT / "docs/TOOLS.md",
		"LICENSE": ROOT / "LICENSE",
		"THIRD-PARTY-NOTICES.md": ROOT / "THIRD-PARTY-NOTICES.md",
		"VERSION": ROOT / "VERSION",
	}
	for py in sorted((ROOT / "tools/phantomring").glob("*.py")):
		if not py.name.startswith("test_"):
			out[f"tools/phantomring/{py.name}"] = py
	return out


def tar_bytes(prefix: str, files: dict[str, bytes]) -> bytes:
	"""A reproducible .tar.gz of files under prefix/: directories first in sorted order, fixed metadata."""
	dirs = {prefix}
	for name in files:
		parts = name.split("/")[:-1]
		for i in range(1, len(parts) + 1):
			dirs.add(f"{prefix}/{'/'.join(parts[:i])}")
	raw = io.BytesIO()
	with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as tar:
		for d in sorted(dirs):
			info = tarfile.TarInfo(d)
			info.type, info.mode = tarfile.DIRTYPE, 0o755
			info.mtime, info.uid, info.gid, info.uname, info.gname = P.FIXED_EPOCH, 0, 0, "", ""
			tar.addfile(info)
		for name in sorted(files):
			data = files[name]
			info = tarfile.TarInfo(f"{prefix}/{name}")
			info.size, info.mode = len(data), 0o755 if name in EXECUTABLE else 0o644
			info.mtime, info.uid, info.gid, info.uname, info.gname = P.FIXED_EPOCH, 0, 0, "", ""
			tar.addfile(info, io.BytesIO(data))
	out = io.BytesIO()
	with gzip.GzipFile(filename="", mode="wb", fileobj=out, compresslevel=9, mtime=0) as gz:
		gz.write(raw.getvalue())
	return out.getvalue()


def main() -> int:
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--build-dir", default=str(ROOT / "build"))
	ap.add_argument("--out", default=str(ROOT / "dist"))
	ap.add_argument("--no-build", action="store_true")
	ap.add_argument("--allow-other-commit", action="store_true", help="package DLLs built from another commit than the checkout's")
	args = ap.parse_args()
	build = Path(args.build_dir).resolve()

	if not args.no_build:
		P.build(build)
	problems = L.check_profile()  # the shipped profile, against this checkout's build/ (the package keeps that layout)
	if problems:
		print("me3 profile: " + "; ".join(problems))
		return 1
	tags, problems = P.check_dlls(build, args.allow_other_commit)
	for p in problems:
		print(p)
	if problems:
		return 1

	srcs = entries(build)
	missing = [str(s) for s in srcs.values() if not s.is_file()]
	if missing:
		print("missing: " + ", ".join(missing))
		return 1
	files = {name: src.read_bytes() for name, src in srcs.items()}
	files["BUILD.txt"] = P.build_txt("Linux", tags, files).encode()

	ver = version.release()
	prefix = f"PhantomRing-{ver}-linux"
	out = Path(args.out)
	out.mkdir(parents=True, exist_ok=True)
	archive = out / f"{prefix}.tar.gz"
	archive.write_bytes(tar_bytes(prefix, files))
	with tarfile.open(archive) as tar:  # read it back: every file, byte for byte
		for name, data in files.items():
			member = tar.extractfile(f"{prefix}/{name}")
			if member is None or member.read() != data:
				print(f"{archive}: {name} did not read back")
				return 1
	sums = P.write_sha256(archive)
	print(f"{archive} ({archive.stat().st_size:,} bytes, sha256 {P.sha256_bytes(archive.read_bytes())})")
	print(f"{sums}")
	for name in sorted(files):
		print(f"  {prefix}/{name}")
	if "-dirty" in (tags.get("PhantomRingHost.dll") or {}).get("commit", ""):
		print("note: built from uncommitted changes (-dirty): fine for testing, not for a release")
	return 0


if __name__ == "__main__":
	sys.exit(main())
