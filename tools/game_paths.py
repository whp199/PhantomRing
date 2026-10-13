"""Default game paths for the offline reverse-engineering scripts.

$ER_EXE / $MGSV_EXE (and $ER_GAME_DIR) win; otherwise the installs are found the way `pr launch` finds them
(Steam's library folders, tools/phantomring/launch.py Machine). None when neither works: the scripts then ask
for the path.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

ER_APPID = 1245620
MGSV_APPID = 287700


def _app_dir(appid: int) -> Path | None:
	sys.path.insert(0, str(Path(__file__).resolve().parent))
	try:
		from phantomring.launch import Machine
		return Machine().app_dir(appid)
	except Exception:
		return None
	finally:
		sys.path.pop(0)


def er_game_dir() -> str | None:
	if os.environ.get("ER_GAME_DIR"):
		return os.environ["ER_GAME_DIR"]
	if os.environ.get("ER_EXE"):
		return str(Path(os.environ["ER_EXE"]).parent)
	d = _app_dir(ER_APPID)
	return str(d / "Game") if d else None


def er_exe() -> str | None:
	if os.environ.get("ER_EXE"):
		return os.environ["ER_EXE"]
	d = er_game_dir()
	return str(Path(d) / "eldenring.exe") if d else None


def mgsv_exe() -> str | None:
	if os.environ.get("MGSV_EXE"):
		return os.environ["MGSV_EXE"]
	d = _app_dir(MGSV_APPID)
	return str(d / "mgsvtpp.exe") if d else None


def require(path: str | None, what: str, env: str) -> str:
	if not path:
		sys.exit(f"{what} not found: set {env} or pass its path")
	return path
