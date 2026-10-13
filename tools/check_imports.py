#!/usr/bin/env python3
"""check_imports.py - the PhantomRing DLLs may only import DLLs every Windows 10/11 has.

    python3 -I tools/check_imports.py build/PhantomRingHost.dll build/dinput8.dll

Both are linked statically (no libgcc/libstdc++/libwinpthread DLL): a MinGW runtime DLL, or anything
outside the list below, would make the DLL fail to load on a player's Windows (or be a dependency we did
not mean to ship). Reads the import table with `x86_64-w64-mingw32-objdump -p`. Exit 0 when all are fine.
"""
import re
import shutil
import subprocess
import sys

# System DLLs on every Windows 10/11 install (and in Proton/Wine prefixes).
ALLOWED = {
	"kernel32.dll", "user32.dll", "advapi32.dll", "ole32.dll", "oleaut32.dll", "shell32.dll", "bcrypt.dll",
	"ws2_32.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "d3dcompiler_47.dll", "msvcrt.dll", "ntdll.dll",
	"gdi32.dll", "version.dll", "winmm.dll", "shlwapi.dll",
}
# The UCRT API sets that UCRT-based MinGW toolchains link against (part of Windows 10+).
API_SET = re.compile(r"^api-ms-win-crt-[a-z0-9-]+\.dll$")
RUNTIME_HINTS = ("libgcc", "libstdc++", "libwinpthread", "mingw", "msvcp", "vcruntime")


def imports(path):
	objdump = shutil.which("x86_64-w64-mingw32-objdump") or shutil.which("objdump")
	if not objdump:
		raise SystemExit("no objdump (x86_64-w64-mingw32-objdump) in PATH")
	out = subprocess.run([objdump, "-p", path], check=True, capture_output=True, text=True).stdout
	return sorted({m.lower() for m in re.findall(r"DLL Name: (\S+)", out)})


def check(paths):
	bad = 0
	for path in paths:
		dlls = imports(path)
		print(f"{path}: {', '.join(dlls)}")
		for dll in dlls:
			if dll in ALLOWED or API_SET.match(dll):
				continue
			why = "a MinGW/MSVC runtime DLL (link statically)" if any(h in dll for h in RUNTIME_HINTS) else "not on the allowed list"
			print(f"  BAD {dll}: {why}")
			bad += 1
	print("imports ok" if not bad else f"{bad} unexpected imports")
	return bad


if __name__ == "__main__":
	if len(sys.argv) < 2:
		print(__doc__)
		sys.exit(2)
	sys.exit(1 if check(sys.argv[1:]) else 0)
