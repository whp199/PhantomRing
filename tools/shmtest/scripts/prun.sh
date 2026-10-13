#!/bin/bash
# usage: prun.sh A|B <shmtest args...>
: "${WORK:?export WORK=<scratch dir holding the built exes>}"
: "${STEAM_LIBRARY:?export STEAM_LIBRARY=<the Steam library folder holding Proton and the runtime, e.g. ~/.local/share/Steam/steamapps>}"
# Runs shmtest.exe exactly the way Steam runs a Proton game: SLR4 (pressure-vessel) container -> Proton Experimental -> wine,
# but with a scratch STEAM_COMPAT_DATA_PATH (prefix) and a scratch pressure-vessel variable dir. Does NOT start Steam or any game.
p=$1; shift
mkdir -p "${WORK}/proton/compat$p" "${WORK}/proton/pvvar" "${WORK}/proton/logs/$p"
PROTON="${PROTON:-$STEAM_LIBRARY/common/Proton - Experimental}"
SLR="${SLR:-$STEAM_LIBRARY/common/SteamLinuxRuntime_4}"
exec env \
  STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_ROOT:-$HOME/.local/share/Steam}" \
  STEAM_COMPAT_DATA_PATH=${WORK}/proton/compat$p \
  STEAM_COMPAT_TOOL_PATHS="$PROTON:$SLR" \
  STEAM_COMPAT_LIBRARY_PATHS="$STEAM_LIBRARY" \
  STEAM_COMPAT_INSTALL_PATH=${WORK} \
  STEAM_COMPAT_APP_ID=0 SteamAppId=0 SteamGameId=0 \
  PRESSURE_VESSEL_VARIABLE_DIR=${WORK}/proton/pvvar \
  SHMTEST_OUT=${SHMTEST_OUT:-${WORK}/proton/out_$p.log} PROTON_LOG=${PROTON_LOG:-0} PROTON_LOG_DIR=${WORK}/proton/logs/$p \
  "$SLR/_v2-entry-point" --verb=${VERB:-waitforexitandrun} -- "$PROTON/proton" ${VERB:-waitforexitandrun} ${EXE:-${WORK}/shmtest-gui.exe} "$@"
