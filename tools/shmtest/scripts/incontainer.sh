#!/bin/sh
# executed (via Wine CreateProcessW on a Unix path) from inside a Proton/SLR4 container
o=${WORK:-/tmp}/proton/incontainer.out
{
echo "pid=$$ ppid=$PPID mnt_ns=$(readlink /proc/self/ns/mnt)"
echo "os-release: $(grep PRETTY_NAME /etc/os-release)"
echo "steam.pipe:"; ls -la "$HOME/.steam/steam.pipe" 2>&1
echo "from-host bin:"; ls /usr/lib/pressure-vessel/from-host/bin/ 2>&1 | tr '\n' ' '; echo
echo "which: steam=$(command -v steam) xdg-open=$(command -v xdg-open) steam-runtime-steam-remote=$(command -v steam-runtime-steam-remote) steam-runtime-launch-client=$(command -v steam-runtime-launch-client)"
echo "xdg-open is: $(ls -la $(command -v xdg-open) 2>&1)"
echo "env: SteamAppId=$SteamAppId STEAM_COMPAT_DATA_PATH=$STEAM_COMPAT_DATA_PATH WINEPREFIX=$WINEPREFIX DBUS_SESSION_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS"
echo "env SRT/PV: $(env | grep -E '^(SRT_|PRESSURE_VESSEL_|STEAM_COMPAT_LAUNCHER)' | tr '\n' ' ')"
} > "$o" 2>&1
