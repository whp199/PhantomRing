"""Which PhantomRing this is: the VERSION file (the one version source, also CMake's project version and the DLLs'),
the commit, and what the built DLLs say about themselves (common/Version.h BuildTag, a plain string in each DLL).

In a git checkout the commit comes from git (with "-dirty" for uncommitted changes to tracked files); in an unpacked
release package, from its BUILD.txt.
"""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # PhantomRing/ (a checkout or an unpacked Linux package)
TAG_PREFIX = b"PHANTOMRING_BUILD release="
RELEASE_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$")


def release(root: Path = ROOT) -> str:
	"""The VERSION file's text, e.g. '0.2.0-alpha' ('unknown' when it is missing or malformed)."""
	try:
		text = (root / "VERSION").read_text().strip()
	except OSError:
		return "unknown"
	return text if RELEASE_RE.match(text) else "unknown"


def _build_txt_field(root: Path, key: str) -> str | None:
	try:
		for line in (root / "BUILD.txt").read_text(errors="replace").splitlines():
			k, sep, v = line.partition(":")
			if sep and k.strip().lower() == key:
				return v.strip()
	except OSError:
		pass
	return None


def commit(root: Path = ROOT) -> str:
	"""'0123456789ab', '0123456789ab-dirty', or 'unknown'; $PHANTOMRING_COMMIT overrides (as in cmake/version.cmake)."""
	if os.environ.get("PHANTOMRING_COMMIT"):
		return os.environ["PHANTOMRING_COMMIT"]
	if (root / ".git").exists():
		try:
			head = subprocess.run(["git", "-C", str(root), "rev-parse", "--short=12", "HEAD"], capture_output=True, text=True,
				timeout=20, check=True).stdout.strip()
			status = subprocess.run(["git", "-C", str(root), "status", "--porcelain", "--untracked-files=no", "--ignore-submodules=dirty"],
				capture_output=True, text=True, timeout=60, check=True).stdout.strip()
			if head:
				return head + ("-dirty" if status else "")
		except (OSError, subprocess.SubprocessError):
			pass
	return _build_txt_field(root, "commit") or "unknown"


def dll_tag(path: Path) -> dict | None:
	"""{'release', 'commit', 'protocol'} from a PhantomRing DLL's build tag; None if the file has none (missing, or
	built before 0.2.0-alpha)."""
	try:
		data = Path(path).read_bytes()
	except OSError:
		return None
	i = data.find(TAG_PREFIX)
	if i < 0:
		return None
	end = data.find(b"\0", i, i + 256)
	text = data[i:end if end > 0 else i + 256].decode("ascii", "replace")
	out: dict = {}
	for item in text.split()[1:]:
		k, sep, v = item.partition("=")
		if sep:
			out[k] = v
	try:
		out["protocol"] = int(out.get("protocol", "0"))
	except ValueError:
		out["protocol"] = 0
	return out if out.get("release") else None


def is_phantomring_guest(path: Path) -> bool:
	"""A PhantomRing guest dinput8.dll, of any version: its build tag (0.2.0-alpha on), or the name of its log, a UTF-16
	string every guest build carries. Not another mod's dinput8.dll (IHHook, Infinite Heaven), which is never replaced."""
	try:
		data = Path(path).read_bytes()
	except OSError:
		return False
	return data[:2] == b"MZ" and (TAG_PREFIX in data or "PhantomRingGuest.log".encode("utf-16-le") in data)


def describe_tag(tag: dict | None) -> str:
	if not tag:
		return "no version tag (built before 0.2.0-alpha, or not a PhantomRing DLL)"
	return f"v{tag['release']} (commit {tag.get('commit', '?')}, protocol {tag['protocol']})"


def describe(root: Path = ROOT) -> str:
	from . import proto
	return f"v{release(root)} (commit {commit(root)}, protocol {proto.VERSION})"


def built_dlls(build: Path) -> dict[str, dict | None]:
	"""The tags of the two built plugins in a build dir (None for a missing or untagged one)."""
	return {name: dll_tag(build / name) for name in ("PhantomRingHost.dll", "dinput8.dll")}


def dll_problems(build: Path) -> list[str]:
	"""Why the built DLLs don't go with these tools or with each other (empty: they do)."""
	from . import proto
	tags = built_dlls(build)
	out = []
	for name, tag in tags.items():
		if not (build / name).is_file():
			out.append(f"{name} is not built")
		elif not tag:
			out.append(f"{name} has no version tag (built before 0.2.0-alpha)")
		elif tag["protocol"] != proto.VERSION:
			out.append(f"{name} speaks protocol {tag['protocol']}, these tools {proto.VERSION}")
	host, guest = tags["PhantomRingHost.dll"], tags["dinput8.dll"]
	if host and guest and (host["release"], host.get("commit"), host["protocol"]) != (guest["release"], guest.get("commit"), guest["protocol"]):
		out.append(f"the two DLLs come from different builds: PhantomRingHost.dll {describe_tag(host)}, dinput8.dll {describe_tag(guest)}")
	return out


def report(build: Path, root: Path = ROOT) -> str:
	"""`pr --version`: the tools' version and the built DLLs'."""
	lines = [f"PhantomRing {describe(root)}"]
	for name, tag in built_dlls(build).items():
		where = build / name
		lines.append(f"  {name:<20} {describe_tag(tag) if where.is_file() else 'not built'}")
	for problem in dll_problems(build):
		lines.append(f"  note: {problem} -- rebuild both (cmake --build build) or reinstall the package")
	return "\n".join(lines)
