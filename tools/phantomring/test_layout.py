"""Checks proto.py against protocol/phantomring_protocol.h, byte for byte.

Two independent checks, both run under wine (the layout that matters is the one MinGW compiles into the
plugins):
  1. build/layout_dump.exe (the cmake target tests/layout_dump.cpp): every offset/size/constant it
     prints must equal proto.py's.
  2. A dump generated from proto.py itself: every field of every ctypes struct, every constant and every
     enum value, compiled against the header with the same MinGW toolchain. This catches fields the
     hand-written dump does not list.

    python3 tools/phantomring/test_layout.py [-v]
"""

from __future__ import annotations

import ctypes
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from phantomring import proto  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
# The wine prefix the dumps run in: $PHANTOMRING_WINEPREFIX, else $WINEPREFIX, else wine's own default (as CMakeLists.txt's
# PHANTOM_WINEPREFIX). A throwaway one keeps the tests off your desktop prefix: PHANTOMRING_WINEPREFIX=$(mktemp -d).
WINEPREFIX = os.environ.get("PHANTOMRING_WINEPREFIX") or os.environ.get("WINEPREFIX") or None
CXX = os.environ.get("PHANTOMRING_CXX", "x86_64-w64-mingw32-g++")


def run_wine(exe: Path) -> str:
	env = dict(os.environ, WINEDEBUG="-all")
	if WINEPREFIX:
		env["WINEPREFIX"] = WINEPREFIX
	out = subprocess.run(["wine", str(exe)], env=env, capture_output=True, text=True, timeout=300, check=True).stdout
	# wine may print a CR before LF
	return out.replace("\r", "")


def ensure_layout_dump(tmp: Path) -> Path:
	"""build/layout_dump.exe via its cmake target. If the build tree can't regenerate right now (e.g. a
	plugin directory is half-written), compile tests/layout_dump.cpp directly with the same toolchain:
	it only needs the protocol header."""
	exe = BUILD / "layout_dump.exe"
	try:
		if not (BUILD / "build.ninja").exists():
			subprocess.run(["cmake", "-S", str(ROOT), "-B", str(BUILD), "-G", "Ninja",
				f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'cmake/mingw-w64-x86_64.cmake'}", "-DCMAKE_BUILD_TYPE=Release"],
				check=True, capture_output=True, text=True)
		subprocess.run(["cmake", "--build", str(BUILD), "--target", "layout_dump"], check=True, capture_output=True, text=True)
		return exe
	except subprocess.CalledProcessError as e:
		reason = (e.stdout + e.stderr).strip().splitlines()
		print(f"note: cmake target layout_dump not buildable now ({reason[-1] if reason else e}); compiling tests/layout_dump.cpp directly")
	exe = tmp / "layout_dump.exe"
	subprocess.run([CXX, "-std=c++23", "-O0", "-static", f"-I{ROOT / 'protocol'}", str(ROOT / "tests/layout_dump.cpp"), "-o", str(exe)],
		check=True, capture_output=True, text=True)
	return exe


def python_value(key: str):
	"""proto.py's value for one key of layout_dump's JSON."""
	consts = {v: k for k, v in proto.ALL_CONSTANTS.items()}
	if key in consts:
		return getattr(proto, consts[key])
	if key.startswith("sizeof(") and key.endswith(")"):
		return ctypes.sizeof(getattr(proto, key[7:-1]))
	if "." in key:
		t, field = key.split(".", 1)
		return getattr(getattr(proto, t), field).offset
	raise KeyError(key)


def check_layout_dump(verbose: bool) -> list[str]:
	tmp = Path(tempfile.mkdtemp(prefix="phantomring_layout_"))
	try:
		data = json.loads(run_wine(ensure_layout_dump(tmp)))
	finally:
		shutil.rmtree(tmp, ignore_errors=True)
	data.pop("end", None)
	errors = []
	for key, want in data.items():
		try:
			got = python_value(key)
		except (KeyError, AttributeError):
			errors.append(f"layout_dump: {key} = {want}, proto.py has no such thing")
			continue
		if got != want:
			errors.append(f"layout_dump: {key}: header {want}, proto.py {got}")
		elif verbose:
			print(f"  ok {key} = {want}")
	print(f"layout_dump.exe: {len(data)} values compared, {len(errors)} mismatches")
	return errors


def generated_source() -> tuple[str, dict[str, int]]:
	"""C++ that prints `name value` lines, and what proto.py expects for each name."""
	lines = ['#include "phantomring_protocol.h"', "#include <cstdio>", "#include <cstddef>", "using namespace phantom::proto;",
		"#define P(name, expr) std::printf(\"%s %llu\\n\", name, static_cast<unsigned long long>(expr))", "int main() {"]
	expect: dict[str, int] = {}
	for t in proto.ALL_STRUCTS:
		name = t.__name__
		lines.append(f'P("sizeof({name})", sizeof({name}));')
		expect[f"sizeof({name})"] = ctypes.sizeof(t)
		lines.append(f'P("alignof({name})", alignof({name}));')
		expect[f"alignof({name})"] = ctypes.alignment(t)
		for field, _ in t._fields_:
			lines.append(f'P("{name}.{field}", offsetof({name}, {field}));')
			lines.append(f'P("sizeof({name}.{field})", sizeof({name}::{field}));')
			expect[f"{name}.{field}"] = getattr(t, field).offset
			expect[f"sizeof({name}.{field})"] = getattr(t, field).size
	for py, cpp in proto.ALL_CONSTANTS.items():
		lines.append(f'P("{cpp}", {cpp});')
		expect[cpp] = getattr(proto, py)
	for enum_type, (scope, prefix) in proto.ALL_ENUMS.items():
		for member in enum_type:
			cpp = f"{scope}{prefix}{member.name}"
			lines.append(f'P("{cpp}", {cpp});')
			expect[cpp] = member.value
	# Derived slot-header positions: must at least fit where the header's static_asserts say.
	lines.append('P("sizeof(DrawCtl)+3*sizeof(DrawSlotHdr)", sizeof(DrawCtl) + 3 * sizeof(DrawSlotHdr));')
	expect["sizeof(DrawCtl)+3*sizeof(DrawSlotHdr)"] = proto.OFF_DRAW_SLOT_HDRS - proto.OFF_DRAW_CTL + 3 * ctypes.sizeof(proto.DrawSlotHdr)
	lines.append("return 0; }")
	return "\n".join(lines) + "\n", expect


def check_generated(verbose: bool) -> list[str]:
	src, expect = generated_source()
	tmp = Path(tempfile.mkdtemp(prefix="phantomring_layout_"))
	try:
		(tmp / "dump.cpp").write_text(src)
		exe = tmp / "dump.exe"
		subprocess.run([CXX, "-std=c++23", "-O0", "-static", f"-I{ROOT / 'protocol'}", str(tmp / "dump.cpp"), "-o", str(exe)],
			check=True, capture_output=True, text=True)
		out = run_wine(exe)
	except subprocess.CalledProcessError as e:
		return [f"generated dump failed to build/run: {e.stderr or e}"]
	got = {}
	for line in out.splitlines():
		if line.strip():
			k, v = line.rsplit(" ", 1)
			got[k] = int(v)
	shutil.rmtree(tmp, ignore_errors=True)
	errors = []
	for k, want in expect.items():
		if k not in got:
			errors.append(f"generated: {k} missing from the dump output")
		elif got[k] != want:
			errors.append(f"generated: {k}: header {got[k]}, proto.py {want}")
		elif verbose:
			print(f"  ok {k} = {want}")
	print(f"generated dump: {len(expect)} values compared ({len(proto.ALL_STRUCTS)} structs, every field), {len(errors)} mismatches")
	return errors


def main() -> int:
	verbose = "-v" in sys.argv
	errors = check_layout_dump(verbose) + check_generated(verbose)
	for e in errors:
		print("MISMATCH", e)
	print("PASS" if not errors else f"FAIL ({len(errors)})")
	return 1 if errors else 0


if __name__ == "__main__":
	sys.exit(main())
