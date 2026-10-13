#!/bin/bash
# usage: run.sh A|B <shmtest args...>   (headless, separate prefix => separate wineserver)
: "${WORK:?export WORK=<scratch dir holding the built exes>}"
p=$1; shift
exec env -u DISPLAY -u WAYLAND_DISPLAY WINEPREFIX=${WORK}/pfx$p WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" wine ${WORK}/shmtest.exe "$@"
