"""`pr collect-logs`: one zip to attach to a bug report (.github/ISSUE_TEMPLATE/bug_report.yml).

It holds, as text: both plugins' logs, the last launch's logs (pr launch: me3, gamescope, Proton; the tail of big
ones), the inis in use, BUILD.txt (or the build's versions), `pr doctor` and `pr status`. Personal paths are
redacted: your home folder becomes ~, your user name, host name, Steam id and account id become <user>, <host>,
<steamid>, <account>. It never holds saves, `pr record` captures, screenshots or game files. Look it over before you
post it.
"""

from __future__ import annotations

import getpass
import io
import os
import re
import socket
import time
import zipfile
from pathlib import Path

from . import launch as L
from . import proto, version

TAIL_BYTES = 4 << 20       # per file: the end of a big Proton log is what matters
SKIP_SUFFIXES = (".sl2", ".bak", ".prrec", ".part", ".png", ".bmp", ".jpg", ".dds", ".bin", ".exe", ".dll")


class Redactor:
	"""Replaces personal identifiers in text. Longest first, so a home path goes before the user name inside it."""

	def __init__(self, home: str | None = None, user: str | None = None, host: str | None = None, steam_ids: list[str] | None = None):
		self.pairs: list[tuple[re.Pattern, str]] = []
		home = home if home is not None else str(Path.home())
		user = user if user is not None else _user()
		host = host if host is not None else _host()
		if home and home not in ("/", ""):
			# /home/<user>, and as Wine shows it: Z:\home\<user>
			win = "Z:" + home.replace("/", "\\")
			self.pairs.append((re.compile(re.escape(win), re.I), "Z:\\~"))
			self.pairs.append((re.compile(re.escape(home) + r"(?=/|\b|$)"), "~"))
		for sid in steam_ids or []:
			if sid:
				self.pairs.append((re.compile(rf"(?<![0-9]){re.escape(sid)}(?![0-9])"), "<steamid>" if len(sid) >= 17 else "<account>"))
		if user and len(user) >= 2:
			# Only where it is a path component or an address: a short name must not eat ordinary words.
			self.pairs.append((re.compile(rf"(?<=[/\\]){re.escape(user)}(?=[/\\]|$|\s|[\"'),;:])"), "<user>"))
			self.pairs.append((re.compile(rf"\b{re.escape(user)}@"), "<user>@"))
			self.pairs.append((re.compile(rf"(?i)\b(user(?:name)?\s*[=:]\s*){re.escape(user)}\b"), r"\1<user>"))
		if host and len(host) >= 3:
			self.pairs.append((re.compile(rf"\b{re.escape(host)}\b"), "<host>"))

	def __call__(self, text: str) -> str:
		for pattern, repl in self.pairs:
			text = pattern.sub(repl, text)
		return text


def _user() -> str:
	try:
		return getpass.getuser()
	except Exception:  # noqa: BLE001 - no user database entry in a container
		return os.environ.get("USER", "")


def _host() -> str:
	try:
		return socket.gethostname()
	except OSError:
		return ""


def steam_ids(m: L.Machine) -> list[str]:
	sid = m.steamid64()
	if not sid:
		return []
	try:
		return [sid, str(int(sid) - L.STEAMID64_BASE)]
	except ValueError:
		return [sid]


def read_tail(path: Path, limit: int = TAIL_BYTES) -> str:
	size = path.stat().st_size
	with open(path, "rb") as f:
		if size > limit:
			f.seek(size - limit)
			data = f.read()
			return f"[... first {size - limit:,} of {size:,} bytes left out ...]\n" + data.decode("utf-8", "replace").split("\n", 1)[-1]
		return f.read().decode("utf-8", "replace")


def sources(m: L.Machine) -> dict[str, Path]:
	"""Name in the zip -> file. Only text: logs, inis, BUILD.txt, the launch record."""
	out: dict[str, Path] = {}
	current = L.LOGS / "current"
	if current.exists():
		for p in sorted(current.resolve().iterdir()):
			if p.is_file() and not p.name.endswith(SKIP_SUFFIXES):
				out[f"launch/{p.name}"] = p
	for cand in L.plugin_log_candidates(m):
		if cand.is_dir():  # me3's log folder: the newest
			logs = sorted(cand.glob("*.log"), key=lambda p: p.stat().st_mtime)
			if logs:
				out[f"me3/{logs[-1].name}"] = logs[-1]
		elif cand.is_file():
			out[f"plugins/{cand.name}"] = cand
	# The host reads the ini next to its DLL (build/); the repository's copies are what a rebuild would install.
	for name, p in (("ini/PhantomRingHost.ini", L.BUILD / "PhantomRingHost.ini"), ("ini/repo-er-PhantomRingHost.ini", L.ROOT / "er/PhantomRingHost.ini"),
			("ini/repo-mgsv-PhantomRingGuest.ini", L.ROOT / "mgsv/PhantomRingGuest.ini")):
		if p.is_file():
			out[name] = p
	mgsv = m.app_dir(L.MGSV_APPID)
	if mgsv and (mgsv / "PhantomRingGuest.ini").is_file():
		out["ini/PhantomRingGuest.ini"] = mgsv / "PhantomRingGuest.ini"  # the one MGSV's guest reads
	if (L.ROOT / "BUILD.txt").is_file():
		out["BUILD.txt"] = L.ROOT / "BUILD.txt"
	return out


def status_snapshot() -> str:
	try:
		from .cli import status_text
		link = proto.Link(proto.default_path())
		try:
			return status_text(link)
		finally:
			link.close()
	except Exception as e:  # noqa: BLE001 - no mapping, or an older one: say so in the zip
		return f"no status: {type(e).__name__}: {e}"


def collect(out: Path | None = None, m: L.Machine | None = None, log=print) -> Path:
	from . import doctor
	m = m or L.Machine()
	redact = Redactor(steam_ids=steam_ids(m))
	ts = L.timestamp()
	out = out or (L.LOGS / f"phantomring-logs-{ts}.zip")
	out.parent.mkdir(parents=True, exist_ok=True)
	texts: dict[str, str] = {
		"version.txt": version.report(L.BUILD) + "\n",
		"doctor.txt": doctor.diagnose(m).text() + "\n",
		"status.txt": status_snapshot() + "\n",
	}
	for name, path in sources(m).items():
		try:
			texts[name] = read_tail(path)
		except OSError as e:
			texts[name] = f"<could not read: {e}>"
	listing = [f"PhantomRing logs collected {ts} by `pr collect-logs` ({version.describe()})",
		"Personal paths and ids are redacted (~, <user>, <host>, <steamid>, <account>); no saves, captures or game files.", ""]
	listing += [f"  {name}" for name in sorted(texts)]
	texts["README.txt"] = "\n".join(listing) + "\n"
	buf = io.BytesIO()
	with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
		for name in sorted(texts):
			info = zipfile.ZipInfo(name, time.localtime()[:6])
			info.compress_type = zipfile.ZIP_DEFLATED
			z.writestr(info, redact(texts[name]))
	out.write_bytes(buf.getvalue())
	log(f"{out} ({out.stat().st_size:,} bytes, {len(texts)} files): attach it to the issue -- look it over first")
	for name in sorted(texts):
		log(f"  {name}")
	return out


def main(args=None) -> int:
	collect(Path(args.output) if args and getattr(args, "output", None) else None)
	return 0
