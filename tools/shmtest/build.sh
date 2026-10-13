#!/bin/bash
# Build the PhantomRing IPC probes into $WORK (default: ./build). Requires x86_64-w64-mingw32-gcc.
set -euo pipefail
HERE=$(dirname "$(readlink -f "$0")")
WORK=${WORK:-$HERE/build}
mkdir -p "$WORK"
CC=x86_64-w64-mingw32-gcc
CFLAGS="-O2 -std=gnu17 -Wall -static"
$CC $CFLAGS          -o "$WORK/shmtest.exe"     "$HERE/shmtest.c"  -lws2_32
$CC $CFLAGS -mwindows -o "$WORK/shmtest-gui.exe" "$HERE/shmtest.c"  -lws2_32          # no conhost under Proton; use SHMTEST_OUT
$CC $CFLAGS -mwindows -o "$WORK/d3dprobe.exe"    "$HERE/d3dprobe.c" -ld3d11 -ldxgi -ldxguid -luuid
$CC $CFLAGS -mwindows -o "$WORK/gpushare.exe"    "$HERE/gpushare.c" -ld3d11 -ld3d12 -ldxgi -ldxguid -luuid
cp "$HERE"/scripts/*.sh "$WORK"/ 2>/dev/null || true
echo "built into $WORK"
