#!/usr/bin/env python3
"""package_windows.py - pack PhantomRing for a Windows player (docs/WINDOWS.md).

    python3 -I tools/package_windows.py [--build-dir build] [--out dist] [--no-build] [--allow-other-commit]

Builds (unless --no-build) with the MinGW toolchain already configured in --build-dir, checks both DLLs (64-bit PE,
stock Windows imports only, the same build tag, this checkout's commit and protocol: tools/package_common.py) and
writes dist/PhantomRing-<version>-windows.zip plus its .sha256:

    PhantomRing/
      phantomring.me3            Mod Engine 3 profile (Elden Ring offline, isolated save, the host plugin)
      PhantomRingHost.dll        Elden Ring plugin; starts MGSV itself (er/Launcher.h)
      PhantomRingHost.ini
      mgsv/dinput8.dll           MGSV guest; the host copies it into MGSV's folder (er/GuestInstall.h)
      mgsv/PhantomRingGuest.ini
      README-WINDOWS.md          docs/WINDOWS.md
      THIRD-PARTY-NOTICES.md     incl. the MinGW runtime, MinHook and HDE licences the DLLs carry
      LICENSE                    PhantomRing's own (MIT)
      BUILD.txt                  version, commit, protocol, supported game versions, sha256 of every file

The zip is reproducible: entries sorted, fixed timestamps.
"""
import argparse
import sys
import tomllib
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import package_common as P  # noqa: E402
from phantomring import version  # noqa: E402

# me3 0.13 ModProfileV1, as tools/phantomring/profile/phantomring.me3 but with the DLL next to it.
ME3_PROFILE = """# PhantomRing for Windows: Mod Engine 3 (me3 >= 0.13) profile. Double-click it (me3 registers .me3 files)
# or run:  me3 launch --profile phantomring.me3
# Relative paths resolve against this file's folder.
profileVersion = "v1"

# Isolated save in %APPDATA%\\EldenRing\\<steam id>\\: copy your ER0000.sl2 to PhantomRing0000.sl2 first
# (README-WINDOWS.md). Your normal save is never touched.
savefile = "PhantomRing0000.sl2"
# Elden Ring stays offline; Easy Anti-Cheat is never involved (me3 starts eldenring.exe directly).
start_online = false
# me3 neutralises Arxan before natives load (the plugin's hooks need it).
disable_arxan = true

[[supports]]
game = "eldenring"

[[natives]]
path = "PhantomRingHost.dll"
"""

FIXED_TIME = P.FIXED_TIME


def entries(build: Path) -> dict:
	"""zip name -> source path (or bytes)."""
	return {
		"PhantomRing/phantomring.me3": ME3_PROFILE.encode(),
		"PhantomRing/PhantomRingHost.dll": build / "PhantomRingHost.dll",
		"PhantomRing/PhantomRingHost.ini": ROOT / "er/PhantomRingHost.ini",
		"PhantomRing/mgsv/dinput8.dll": build / "dinput8.dll",
		"PhantomRing/mgsv/PhantomRingGuest.ini": ROOT / "mgsv/PhantomRingGuest.ini",
		"PhantomRing/README-WINDOWS.md": ROOT / "docs/WINDOWS.md",
		"PhantomRing/THIRD-PARTY-NOTICES.md": ROOT / "THIRD-PARTY-NOTICES.md",
		"PhantomRing/LICENSE": ROOT / "LICENSE",
	}


def crlf(data: bytes) -> bytes:
	# Notepad copes with LF these days, but older Windows tools don't: text files get CRLF.
	return data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")


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

	profile = tomllib.loads(ME3_PROFILE)  # me3 reads TOML: a typo here would only show on the player's PC
	if profile.get("natives", [{}])[0].get("path") != "PhantomRingHost.dll" or profile.get("start_online") is not False:
		print("me3 profile: natives path / start_online wrong")
		return 1

	files = entries(build)
	for name, src in files.items():
		if isinstance(src, Path) and not src.is_file():
			print(f"missing {src} (build first, or drop --no-build)")
			return 1
	tags, problems = P.check_dlls(build, args.allow_other_commit)
	for p in problems:
		print(p)
	if problems:
		return 1

	ver = version.release()
	data = {}
	for name, src in files.items():
		raw = src if isinstance(src, bytes) else src.read_bytes()
		data[name] = crlf(raw) if name.endswith((".ini", ".md", ".me3", ".txt")) else raw
	listed = {name.removeprefix("PhantomRing/"): raw for name, raw in data.items()}
	data["PhantomRing/BUILD.txt"] = crlf(P.build_txt("Windows", tags, listed).encode())

	out = Path(args.out)
	out.mkdir(parents=True, exist_ok=True)
	zip_path = out / f"PhantomRing-{ver}-windows.zip"
	with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
		for name in sorted(data):
			info = zipfile.ZipInfo(name, FIXED_TIME)
			info.compress_type = zipfile.ZIP_DEFLATED
			info.external_attr = 0o644 << 16
			z.writestr(info, data[name])
	with zipfile.ZipFile(zip_path) as z:
		bad = z.testzip()
		if bad:
			print(f"{zip_path}: {bad} is corrupt")
			return 1
		names = z.namelist()
	sums = P.write_sha256(zip_path)
	print(f"{zip_path} ({zip_path.stat().st_size:,} bytes, sha256 {P.sha256_bytes(zip_path.read_bytes())})")
	print(f"{sums}")
	for name in names:
		print(f"  {name}")
	if "-dirty" in (tags.get("PhantomRingHost.dll") or {}).get("commit", ""):
		print("note: built from uncommitted changes (-dirty): fine for testing, not for a release")
	return 0


if __name__ == "__main__":
	sys.exit(main())
