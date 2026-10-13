# Proton IPC, guest launch and guest hiding — empirical results

Research date 2026-10-11. Author: a PhantomRing research agent. This is the durable record for one question.
**How can Elden Ring (host, D3D12) and MGSV (guest, D3D11) talk to each other, and how do we start and hide MGSV?**
Each game runs under Proton in its own prefix. Each prefix has its own wineserver and its own pressure-vessel container.

Tags: **VERIFIED** means checked on this machine against local bytes, source or a run.
**UNVERIFIED** means from memory or the web and not checked here.

Test sources, scripts and raw logs:

- `PhantomRing/tools/shmtest/shmtest.c` holds the IPC probe and `PhantomRing/tools/shmtest/d3dprobe.c` holds the window and present probe.
- `PhantomRing/tools/shmtest/gpushare.c` holds the GPU sharing probe. `build.sh` builds all three, and the runner scripts are in `scripts/`.
- Raw logs are under `PhantomRing/docs/research/proton-ipc-logs/{wine-vanilla,proton-slr4}/`.

---

## 0. Answers in one page

| Question | Answer | Status |
|---|---|---|
| Can two Proton games in different prefixes, wineservers and containers share memory? | **Yes.** Map the same file on host `/dev/shm` with `CreateFileW` + `CreateFileMappingW` + `MapViewOfFile`. The mapping is coherent `MAP_SHARED`, with the same inode and `rw-s` in both. | VERIFIED: vanilla Wine 11, two prefixes; and the real Steam stack, SLR 4 container + Proton Experimental, two containers + two prefixes |
| Do `Local\` / `Global\` named mappings and events cross prefixes? | **No.** `OpenFileMappingW` and `OpenEventW` fail with `ERROR_FILE_NOT_FOUND` (2). They do work between processes in the same prefix, including from a *second container* that joins the prefix with Proton verb `run`. | VERIFIED |
| Interlocked ops across prefixes? | **Work.** 2×5 M `lock xadd` + `lock cmpxchg` lost no updates. | VERIFIED |
| Latency across prefixes | Spin round trip: median **0.20 µs**, max 22–110 µs. Raw futex wake: **10–13 µs**. UDP loopback: mean **~90 µs**. | VERIFIED |
| Bulk bandwidth through shm | 1080p RGBA (8.3 MB): **0.55–0.60 ms/frame** per side (14–15 GB/s). RGBA+D32 (16.6 MB): **1.5–1.6 ms** per side. | VERIFIED (measured with 2 Ghidra JVMs loading the box) |
| Does pressure-vessel share `/dev/shm` and `/tmp`? | **Yes, by default.** Containers mount the host `/dev/shm` tmpfs (dev 0:25) and the host `/tmp` (0:42) as slave bind mounts. They also share the host IPC, PID and network namespaces. No env var is needed. | VERIFIED (mountinfo of live containers + pressure-vessel strings) |
| Cross-prefix wakeups and liveness | **Futex:** a raw Linux futex on the shm word works, but *only* from a code page above `0x7001'0000'0000`. Proton's seccomp filter traps lower-address syscalls. **UDP loopback** works. **`LockFileEx`** is mirrored to `fcntl` locks, so it gives cross-prefix liveness and a "wait until peer dies" primitive. Use **unix PIDs** via the getpid stub and `Z:\proc\<pid>`. Never use Windows PIDs; they collide across prefixes. | VERIFIED |
| GPU texture sharing D3D11 (DXVK) → D3D12 (vkd3d-proton) | **Same prefix/wineserver: zero-copy works.** `CreateSharedHandle` → `DuplicateHandle` → `ID3D12Device::OpenSharedHandle`. **Different prefixes: impossible**; the handles are wineserver objects. | VERIFIED |
| Cheapest cross-prefix frame path | **vkd3d-proton `ID3D12Device3::OpenExistingHeapFromAddress` works on a `/dev/shm` file view.** The GPU reads and writes the shared file directly at about **0.36 ms per 16 MiB (~46 GB/s)**, with **no CPU memcpy** on either side. The guest (D3D11) can reach it through an in-process D3D12 device plus a D3D11→D3D12 shared handle. | VERIFIED (host side and both copy directions; guest D3D11 interop in-process is inferred from the cross-process result) |
| Does DXVK keep rendering for hidden or minimized windows? | **Yes.** GPU work always executes. A hidden (never shown) or offscreen window: `Present` = `S_OK` (with vsync paced at 60 Hz). A minimized window: `Present` = `DXGI_STATUS_OCCLUDED` for both flip and blit, and GPU work still runs. | VERIFIED (DXVK v3.1.1-47 source + d3dprobe runs) |
| Best way to hide the guest | **`gamescope --backend headless`** wrapping the guest launch. The window is mapped and focused *inside* the nested Xwayland (`GetForegroundWindow()==hwnd`), presents at 60 Hz, and nothing appears on the desktop. | VERIFIED (d3dprobe under Proton inside headless gamescope) |
| How to start MGSV from ER's running process | (a) **Steam's host-side launcher service:** `steam-runtime-launch-client --alongside-steam -- <cmd>` from inside the ER container runs `<cmd>` in the **host** namespace, as a child of Steam. (b) `steam://rungameid/287700` / `-applaunch` forwarded to Steam by Proton's `steam.exe` → `steam-runtime-steam-remote` → `~/.steam/steam.pipe`. (c) `CreateProcess(mgsvtpp.exe)` directly in ER's prefix. | (a) VERIFIED with a harmless command; (b) mechanism VERIFIED from source/strings, actual MGSV launch not attempted; (c) not attempted |

**Recommended architecture.**

- **Default:** separate prefixes, each game in its own Proton container.
- **IPC:** a file mapping `/dev/shm/phantomring_v1`, opened through `wine_get_dos_file_name("/dev/shm/phantomring_v1")`.
- **Sync:** spin on seqlocks / SPSC rings, plus an optional futex through a high-address syscall stub or a UDP doorbell.
- **Liveness:** unix PIDs + `LockFileEx`.
- **Frames:** carried through the same shm by D3D12 `OpenExistingHeapFromAddress` on both sides.
- **Guest hiding:** headless gamescope.
- **Guest launch:** a Linux-side launcher. The ER plugin can call it with `steam-runtime-launch-client --alongside-steam`, or the user runs it directly. It invokes SLR 4 + Proton for MGSV the way `me3` does for ER.
- **Fallback:** "MGSV as a child process inside ER's prefix" keeps SkyCraft's `Local\` names, named events and shared GPU handles. The price is MGSV running in ER's prefix and container, which is visible on ER's display and has no gamescope isolation (§7.3, §9).

---

## 1. Environment (VERIFIED)

| Item | Value |
|---|---|
| Kernel / GPU | Linux (Gentoo); NVIDIA RTX 50-series (proprietary driver) |
| Desktop | KDE Plasma on Wayland, with XWayland |
| Steam | Native client, `/usr/bin/steam` → `~/.local/share/Steam`. Running. `~/.steam/steam.pipe` (FIFO), `~/.steam/steam.pid`. One library: `~/.local/share/Steam`, whose `steamapps/common` and `steamapps/compatdata` are **symlinks** to a folder on another drive (`<steam-library>/{common,compatdata}`) |
| Compat tool | Global default `proton_experimental` (`config/config.vdf` CompatToolMapping "0"). No per-app override for 1245620/287700. **No launch options set** for either game (`userdata/<account-id>/config/localconfig.vdf`) |
| Proton | `common/Proton - Experimental`, `version` = `1790839227 experimental-11.0-20261001-x86_64`. `toolmanifest.vdf`: `commandline "/proton %verb%"`, `require_tool_appid "4183110"` |
| Container runtime | **Steam Linux Runtime 4.0 (appid 4183110, `common/SteamLinuxRuntime_4`)**, depot 4.0.20260805.254769, pressure-vessel 0.20260805.0. It is *not* sniper: Proton Experimental now requires SLR 4. `toolmanifest.vdf`: `commandline "/_v2-entry-point --verb=%verb% --"` |
| DXVK in Proton | `v3.1.1-47-g68530156` (`files/lib/wine/dxvk/version`, commit 685301564ea3486a7098bd37194020da84299617) |
| vkd3d-proton in Proton | `vkd3d-1.1-5641-g44cf7c20` (`files/lib/wine/vkd3d-proton/version`) |
| Sync | `ntsync: up and running.` (Proton log). `/dev/ntsync` exists (crw-rw-rw- 10,258) |
| System Wine | `/etc/eselect/wine/bin/wine` → `wine-vanilla-11.0` (`wine_get_version()` = "11.0") |
| gamescope | `/usr/bin/gamescope` (supports `--backend headless`, VERIFIED via `--help`) |

---

## 2. Test program

`shmtest.c` (C17, mingw-w64, about 520 lines) has these modes:

`info | writer | reader | inc | pingpong | bench | futex | udp | named-create | named-open | alive | lock | unixexec | lowsyscall`

Paths that start with `/` are converted with **`kernel32!wine_get_dos_file_name`**. That works even if a user deleted the `Z:` drive. Otherwise the path is used as a Windows path, e.g. `Z:\dev\shm\phantomring_test`.

Opening the mapping:

1. `CreateFileW(GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|WRITE|DELETE, OPEN_ALWAYS)`
2. Grow the file with `SetFilePointerEx` + `SetEndOfFile`.
3. `CreateFileMappingW(f, PAGE_READWRITE)`, then `MapViewOfFile(FILE_MAP_ALL_ACCESS)`.

The first opener initializes the header through `InterlockedCompareExchange64`. All hot fields sit on separate 64-byte lines.

Build (`tools/shmtest/build.sh`):

```
x86_64-w64-mingw32-gcc -O2 -std=gnu17 -Wall -static          -o shmtest.exe     shmtest.c -lws2_32
x86_64-w64-mingw32-gcc -O2 -std=gnu17 -Wall -static -mwindows -o shmtest-gui.exe shmtest.c -lws2_32
```

The `-mwindows` build plus `SHMTEST_OUT=<file>` is needed under Proton. Proton starts the exe through its `steam.exe`, so a console exe gets its own `conhost.exe` and its stdout never reaches the Linux terminal.

### 2.1 Runners

- **Vanilla Wine, two prefixes** (`scripts/run.sh`). The prefixes were created with `WINEPREFIX=$WORK/pfxA|pfxB WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -i`. They got separate wineservers: socket dirs `/tmp/.wine-1000/server-2a-66ce` and `server-2a-66cf`, two `wineserver` processes.

  ```
  env -u DISPLAY -u WAYLAND_DISPLAY WINEPREFIX=$WORK/pfx{A,B} WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" wine $WORK/shmtest.exe <args>
  ```

- **The real Steam stack** (`scripts/prun.sh`). This is the same command chain Steam and `me3` use, with a scratch prefix and a scratch pressure-vessel variable dir. It does **not** start Steam or any game.

  ```
  env STEAM_COMPAT_CLIENT_INSTALL_PATH=~/.local/share/Steam \
      STEAM_COMPAT_DATA_PATH=$WORK/proton/compat{A,B} \
      STEAM_COMPAT_TOOL_PATHS="<Proton - Experimental>:<SteamLinuxRuntime_4>" \
      STEAM_COMPAT_LIBRARY_PATHS=<steam-library> STEAM_COMPAT_INSTALL_PATH=$WORK \
      STEAM_COMPAT_APP_ID=0 SteamAppId=0 SteamGameId=0 \
      PRESSURE_VESSEL_VARIABLE_DIR=$WORK/proton/pvvar SHMTEST_OUT=... \
    <steam-library>/common/SteamLinuxRuntime_4/_v2-entry-point --verb=${VERB:-waitforexitandrun} -- \
    "<steam-library>/common/Proton - Experimental/proton" ${VERB:-waitforexitandrun} $WORK/shmtest-gui.exe <args>
  ```

  `PRESSURE_VESSEL_VARIABLE_DIR` keeps pressure-vessel from writing into `SteamLinuxRuntime_4/var`. The `run` script honours it (`--variable-dir="${PRESSURE_VESSEL_VARIABLE_DIR:-"${here}/var"}"`).

  On tmpfs the 631 MB runtime is *copied*, about 582 MB per launch. Hard links fail across filesystems, and the log fills with "Invalid cross-device link" warnings. In a real Steam launch `var/` is on the same fs and is hard-linked.

- `scripts/ppair.sh` starts process X in one container, waits for a marker line, then runs Y in another container.

---

## 3. Results: vanilla Wine 11, two prefixes (VERIFIED)

Raw logs are in `proton-ipc-logs/wine-vanilla/`.

| # | Test | Command (A=pfxA, B=pfxB) | Result |
|---|---|---|---|
| T1 | Coherence | A: `writer Z:\dev\shm\phantomring_test 6`; B: `reader ... 4` | Writer counter about 950 Hz (Sleep(1)). Reader: `samples=16 changes=15 regressions=0 first=1575 last=5140 -> LIVE+MONOTONIC`. A **Linux-native** `python3 mmap` of `/dev/shm/phantomring_test` saw `magic=31544d48534e4850`, counter +475 in 0.5 s, size 65536 |
| T2 | Named objects | A: `named-create Local\PhantomRingTest`; open from A and from B | Same prefix: `OPENED value=0xc0ffee`, event `OPENED`. Other prefix: `NOT FOUND (err=2)` for the mapping **and** the event. `Global\PhantomRingTest`: identical |
| T3 | Interlocked | A and B: `inc /dev/shm/phantomring_inc 5000000 2` | `xadd_total=10000000 (expect 10000000) cas_total=30000000 (expect 30000000) -> ATOMIC OK`, 47.7 ns per contended xadd+CAS pair |
| T4 | Spin RTT | B: `pingpong ... b 200000`; A: `... a 200000` | `min=0.10 us median=0.20 us mean=0.22 us max=22.10 us` |
| T5 | Bulk copy, 3-slot ring | B reader / A writer, 600 frames | 8,294,400 B (1080p RGBA): 1805 fps, **14.97 GB/s, 0.554 ms/frame**. 16,588,800 B (1080p RGBA+D32): 650 fps, 10.78 GB/s, 1.539 ms. 33,177,600 B (4K RGBA): 246 fps, 8.16 GB/s, 4.064 ms. Torn frames: 0 |
| T6 | Raw futex | B: `futex ... wait 4`; A: `futex ... wake 3` | `ret=0` woken; wake→resume **11.5 / 12.8 / 13.3 µs**; 4th wait `-110 ETIMEDOUT` after 10 s. Requires the high-address stub (§5.3) |
| T7 | UDP 127.0.0.1 | A: `udp server 47811`; B: `udp client` | 20 000 blocking RTTs: min 54.6, **mean 87.5**, max 5377 µs |
| T8 | `LockFileEx` | A: `lock ... hold 4` (exits without unlocking); B: `try`, then `wait` | B try: `BUSY err=33` (ERROR_LOCK_VIOLATION). Linux `fcntl.lockf` from python: `HELD ... EAGAIN`, so the lock is mirrored to a POSIX lock. B blocking wait: `ACQUIRED after 1.101 s`, exactly when A exited |
| T9 | Liveness | B: `alive <unix pid of A's writer>` | `Z:\proc\77132\stat -> ALIVE` while running, `GONE` after |
| T10 | Run Unix programs from Wine | A: `unixexec "Z:\usr\bin\touch <f>"`, `"/usr/bin/touch <f>"`, `"Z:\bin\sh -c \"...\""` | All three executed. `CreateProcessW` returned TRUE but gave **no waitable handle**: `WaitForSingleObject` → `WAIT_FAILED`, exit code unavailable. It is fire-and-forget |
| info | PIDs | `info` | Windows pid = **32 in both prefixes** (collision). Unix pid via getpid stub = 73888. `open("/proc/self")` through Wine returned **73890 = the wineserver**, because Wine performs `open()` inside the wineserver |

---

## 4. Results: real Steam stack, two SLR 4 containers + two Proton prefixes (VERIFIED)

Raw logs are in `proton-ipc-logs/proton-slr4/`.

### 4.1 Isolation evidence while writer (A) and reader (B) were running

```
writer pid=84692  mnt_ns=4026534147  ipc_ns=4026531839  pid_ns=4026531836  net_ns=4026531833
reader pid=84980  mnt_ns=4026534249  ipc_ns=4026531839  pid_ns=4026531836  net_ns=4026531833
host shell        mnt_ns=4026531832  ipc_ns=4026531839
both:  0:25 / /dev/shm master:3 | 0:42 / /tmp master:22 | (home and library mounts omitted)
host:  40 38 0:25 / /dev/shm shared:3 ; 34 43 0:42 / /tmp shared:22
/proc/84692/maps: 00900000-00910000 rw-s 00000000 00:19 14741 /dev/shm/phantomring_test
/proc/84980/maps: 00900000-00910000 rw-s 00000000 00:19 14741 /dev/shm/phantomring_test
wineservers: 84542 and 84818 (Proton - Experimental/files/bin/wineserver)
```

So the two containers have separate mount namespaces, but they bind the *same* host `/dev/shm` and `/tmp` superblocks with propagation from the host. They share the host IPC, PID and network namespaces, and the same file inode is mapped `MAP_SHARED`.

The Proton log also shows the test exe loaded from `Z:\\tmp\\...`, which confirms `/tmp` is visible inside the container.

### 4.2 Results

| Test | Result |
|---|---|
| P1 coherence (writer in container A / prefix A, reader in container B / prefix B) | `samples=32 changes=31 regressions=0 -> LIVE+MONOTONIC` |
| Named objects, cross-prefix | `OpenFileMappingW(Local\PhantomRingTest) -> NOT FOUND (err=2)`, `OpenEventW -> NOT FOUND (err=2)` |
| Named objects, same prefix, **second container**, verb `waitforexitandrun` | NOT FOUND. The verb first runs `wineserver -w`, so it waited for the creator's prefix to shut down (Proton `proton` script lines 2214–2218) |
| Named objects, same prefix, **second container**, verb `run` | `OPENED (err=0) value=0xc0ffee`, event `OPENED`. The second container joins the first container's wineserver through the shared `/tmp/.wine-1000/server-<dev>-<ino>` socket |
| Interlocked 2×5 M | `ATOMIC OK`, 35.5 ns/pair |
| Spin RTT 200 k | `min=0.10 median=0.20 mean=0.22 max=110.00 us` |
| Raw futex (high stub) | woken, wake→resume **9.8 / 12.8 / 13.3 µs**; timeout `-110` |
| Raw syscall from `.text` (`lowsyscall`) | **`0xC0000005`**: intercepted by Proton's seccomp/NT-syscall emulation. The high stub returned the real pid 94105 |
| Bulk 1080p RGBA, 1200 frames | 1677 fps, **13.91 GB/s, 0.596 ms/frame**, 0 torn |
| Bulk 1080p RGBA+D32, 1200 frames | 629 fps, 10.43 GB/s, **1.590 ms/frame**, 0 torn |
| UDP loopback 20 000 RTT | min 48.4, **mean 90.2**, max 6914 µs |
| `LockFileEx` wait | `ACQUIRED after 5.407 s`, when the holder exited (it held 12 s without unlocking) |
| Windows pids | 316 in both A and B; 324 for the producer in A and the consumer in B (collision) |

### 4.3 Inside a game container (`unixexec` of a shell script from Wine)

```
pid=97100 mnt_ns=4026533789 ; os-release: PRETTY_NAME="Steam Runtime 4"
~/.steam/steam.pipe  visible (prw-------)
/usr/bin/steam -> steam-runtime-steam-remote ; /usr/bin/xdg-open -> steam-runtime-urlopen   (created by pressure-vessel)
/usr/bin/steam-runtime-launch-client present
DBUS_SESSION_BUS_ADDRESS=unix:path=/run/pressure-vessel/bus
```

From inside the container, `steam-runtime-launch-client --alongside-steam -- /bin/sh -c '...'` ran the command on the **host**:

```
host-side mnt_ns=mnt:[4026531832] pid=110965 ppid=44757    (44757 = "steam-runtime-launcher-service --alongside-steam", child of Steam 44569)
/usr/bin/gamescope visible
```

Steam's console log confirms the service: `Steam Runtime Launch Service: steam-runtime-launcher-service is running pid 44757 / bus_name=com.steampowered.PressureVessel.LaunchAlongsideSteam`.

---

## 5. Gotchas found (all VERIFIED)

1. **Windows PIDs collide across prefixes.** We saw 32/32, 316/316 and 324/324. Each wineserver numbers processes from the same sequence. `OpenProcess(otherPrefixPid)` can open **the wrong process, including yourself**: the cross-prefix gpushare consumer `OpenProcess(324)` opened itself and `DuplicateHandle` failed with error 6. Put **unix PIDs** in the protocol header.
2. **`/proc/self` through Wine file APIs is the wineserver.** Wine executes file `open()` inside the wineserver. `Z:\proc\self\stat` therefore returned the wineserver pid. Get your own unix pid with `getpid` through the stub (§5.3). `Z:\proc\<pid>\...` works fine for checking *another* pid.
3. **Proton traps raw Linux syscalls from PE code.**
   - Proton's `ntdll.so` `signal_init_process` installs a seccomp BPF filter (`filter.3` @ .data `0xa6340`, 12 insns). The log shows `trace:seh:install_bpf Installing seccomp filters.`
   - Decoded, the filter **ALLOWs** syscalls whose IP high dword is `> 0x7000`, i.e. IP ≥ `0x7001'0000'0000`. For x86-64 it also **ALLOWs** IP low dword in `[0x7d400000, 0x7d402000)`. Everything else gets **`SECCOMP_RET_TRAP`** → SIGSYS → emulated as an NT syscall.
   - It is installed unconditionally whenever native libs are above `0x6fff'ffff'ffff`, with no appid gating seen in the code path.
   - Workaround: `VirtualAlloc((void*)0x710000000000, 4096, MEM_RESERVE|MEM_COMMIT, PAGE_EXECUTE_READWRITE)`, write `0F 05 C3` (`syscall; ret`), and call it with the Linux registers set (rax=nr, rdi, rsi, rdx, r10, r8, r9). This worked on vanilla Wine and on Proton.
   - Vanilla Wine 11 has **no** such filter (no "seccomp" string in `/usr/lib/wine-vanilla-11.0/wine/x86_64-unix/ntdll.so`).
   - Caveats: a thread blocked in a raw futex is invisible to Wine (no APCs/suspend), so always use a timeout and handle `EINTR`. And LLP64 means **every syscall argument must be 64-bit**: the first futex attempt passed `long` = 32-bit pointers and got `-EFAULT`.
4. **Proton needs a display.** With `DISPLAY`/`WAYLAND_DISPLAY` unset, the exe ran, but `steam.exe`/`explorer.exe` never shut down ("Application tried to create a window, but no driver could be loaded", "The explorer process failed to start"). `proton waitforexitandrun` hung until the 180/300 s timeout.
5. **Proton verbs** (VERIFIED in the `proton` script, lines 2209–2221):
   - `waitforexitandrun` = `wineserver -w`, then run.
   - `run` = run immediately and join a live wineserver.
   - `runinprefix` = plain `wine <args>` without `steam.exe`.

   A container started with `run` that joins another container's prefix **stays alive until the shared wineserver exits**. consume11 finished at t=322.25 s, but its container only exited at 395.78 s, 0.12 s after the producer in the first container exited. Nothing was killed: the producer's 5 s heartbeats continued to the end.
6. **Console exes under Proton** get a new `conhost.exe`, so stdout is lost. Use GUI-subsystem helpers + file logging.
7. **Accidental side effect (disclosed).** While exploring, `SteamLinuxRuntime_4/pressure-vessel/bin/steam-runtime-steam-remote --help` was run once on the host. That tool forwards its argv to the running Steam via `~/.steam/steam.pipe`, so Steam received a `--help` command line. Steam's console log shows no reaction, and nothing was launched. Do not run that tool for "help" again.

---

## 6. pressure-vessel / SLR 4 sharing (VERIFIED unless marked)

- **`/dev/shm`.** `pressure-vessel-wrap` has Flatpak-derived `/dev/shm` handling: strings `/dev/shm`, `/proc/self/root/run/shm`, `Unexpected /dev/shm symlink %s`. It also has the warning `/dev/shm not shared between app instances (flatpak#4214). The Steam Overlay will not work.`, which only applies when Steam is a Flatpak. Here Steam is native. The overlay and steamclient IPC rely on `/dev/shm/u1000-Shm_*` / `ValveIPCSharedObj` files, and many `u1000-Shm_*` files exist in host `/dev/shm`. Live containers mount the host `/dev/shm` (§4.1).
- **`/tmp`.** Live game and steamwebhelper containers bind the host `/tmp`. Note that Valve's `steamlinuxruntime-known-issues.md` list of shared dirs does not mention `/tmp`. That list (home, Steam library, Proton dir, Steam install dir, shader cache, `/home`, `/media`, `/mnt`, `/opt`, `/run/media`, `/srv`) is fetched from GitHub. Do not rely on `/tmp` for the protocol; use `/dev/shm`. Wine's `/tmp/.wine-1000/server-*` sockets *are* shared, which is why same-prefix joining across containers works.
- **Extra paths.** `PRESSURE_VESSEL_FILESYSTEMS_RO` / `PRESSURE_VESSEL_FILESYSTEMS_RW` (colon-separated) are present as strings in `pressure-vessel-wrap` and documented in Valve's known-issues doc. `STEAM_COMPAT_MOUNTS`, `STEAM_COMPAT_LIBRARY_PATHS`, `STEAM_COMPAT_TOOL_PATHS` and `STEAM_COMPAT_INSTALL_PATH` are present as strings. me3 passes `STEAM_COMPAT_MOUNTS`. Not needed for `/dev/shm`.
- **Namespaces.** Shared with the host: IPC (4026531839), PID (4026531836) and network (4026531833). Private: mount and user. The pid namespace stays shared unless `--unshare-pid` / `PRESSURE_VESSEL_SHARE_PID=0` is used.
- **`run` hard-codes `PRESSURE_VESSEL_COPY_RUNTIME=1`** and passes `--variable-dir="${PRESSURE_VESSEL_VARIABLE_DIR:-$here/var}"`. `_v2-entry-point` honours `STEAM_LINUX_RUNTIME_LOG`, `STEAM_LINUX_RUNTIME_VERBOSE`, `STEAM_COMPAT_MACHINE_ARCHITECTURE` etc.

---

## 7. Launching the guest (MGSV, appid 287700)

### 7.1 MGSV facts relevant to launching (VERIFIED from local bytes)

- **Files.** `MGS_TPP/steam_appid.txt` = `287700` (6 bytes) and `version_info.txt` = `Tpp_steam_mst_en_day3900Mgo_patch_0707_1632`.
- **No SteamStub DRM.** `mgsvtpp.exe` has no `.bind` section. Its sections are `.xtext .edata .data2 .didata .bss .text .impdata .trace .xdata(0x6f74df4, CODE) .00cfg .xtls .debug$P .ecode`, i.e. it is obfuscated or protected (the vendor is UNVERIFIED).
- **steam_api64 imports.** `SteamAPI_Init`, `SteamAPI_RunCallbacks`, `SteamAPI_RegisterCallback/CallResult(+Unregister)`, `SteamUser`, `SteamUserStats`, `SteamUtils`, `SteamFriends`, `SteamRemoteStorage`, `SteamApps`, `SteamNetworking`. **No `SteamAPI_RestartAppIfNecessary`**, so a non-Steam launch is not bounced back through Steam.
- **Other imports that matter.**
  - **`DINPUT8.dll`**: the natural proxy injection point; IHHook uses `dinput8.dll` for MGSV (UNVERIFIED).
  - `XINPUT1_3.dll`, `dxgi!CreateDXGIFactory1`, `d3d11!D3D11CreateDevice`.
  - `USER32!GetForegroundWindow/SetActiveWindow/SetFocus/ShowWindow/CreateWindowExW/RegisterRawInputDevices/GetRawInputData/SetCursorPos/SetForegroundWindow`.
  - `KERNEL32!CreateMutexW/CreateFileMappingW/CreateProcessA`, `MFPlat`/`MFReadWrite` (video), `WS2_32`/`WINHTTP`/`wininet` (online).
- **Saves.** `userdata/<account-id>/311340/remote/{TPP_GAME_DATA, TPP_CONFIG_DATA, PERSONAL_DATA, MGO_GAME_DATA, mgsv_gz_save_data_gl_en0/1}` are on the **Linux** side (2018 timestamps, synced 2026-10-09 09:51). `userdata/.../287700/remotecache.vdf` lists the same files.
  - The exe contains bare names `TPP_GAME_DATA`, `TPP_CONFIG_DATA`, `PERSONAL_DATA`, `MGO_MAIN_DATA`, `MGO_GAME_DATA`, `TPP_GRAPHICS_CONFIG`. Together with the `SteamRemoteStorage` import, saves go **through the Steam client (ISteamRemoteStorage)** and are **prefix-independent**. That is a strong inference (UNVERIFIED that every save uses FileWrite).
  - The MGSV prefix `compatdata/287700/pfx/drive_c` (created 2026-10-09 09:51) contains no MGSV save or config folders. Listing was read-only.
- **Proton has an MGSV-specific hack.** `files/lib/wine/x86_64-unix/winedmo.so` contains `SteamGameId` near `287700 543900` and the strings `placeholder-video-used` / `MEDIACONV_BLANK_VIDEO_FILE` (a media-playback workaround). If MGSV runs with ER's `SteamGameId=1245620`, that hack is lost. **Always set `SteamGameId=287700`** for the guest. Neither game id appears in `ntdll.so` or the `proton` script.

### 7.2 Launch paths

**A. Linux-side launcher (recommended).**

- **ER** is started with me3, as the old project does: `MGS/scripts/project.py launch`, which runs `tools/me3/bin/me3 --windows-binaries-dir .../win64 launch --game eldenring --exe .../eldenring.exe --profile ... --online=false --savefile MGS_ER0000.sl2`.
- **How me3 runs Proton** (VERIFIED in `garyttierney/me3` `crates/cli/src/commands/launch/strategy/compat_tool.rs` and in local `me3` strings):
  - It walks `toolmanifest.vdf` → `require_tool_appid` and builds `<SLR>/_v2-entry-point --verb=waitforexitandrun -- <Proton>/proton waitforexitandrun <me3-launcher.exe> -- <args>`. `ME3_PROTON_LAUNCH_VERB` overrides the verb.
  - It sets `STEAM_COMPAT_CLIENT_INSTALL_PATH`, `STEAM_COMPAT_INSTALL_PATH`, `STEAM_COMPAT_TOOL_PATH`, `STEAM_COMPAT_LIBRARY_PATHS`, `STEAM_COMPAT_MOUNTS`, `STEAM_COMPAT_DATA_PATH=<lib>/steamapps/compatdata/<appid>` and `STEAM_COMPAT_APP_ID`, plus `SteamAppId`/`SteamGameId`/`SteamOverlayGameId` and `LD_PRELOAD=<steam>/ubuntu12_64/gameoverlayrenderer.so`.
  - The old project's Proton log (`MGS/reports/launch-diagnostics/20261009T192838768053Z/steam-1245620.log`) shows this exact path: `SteamGameId: 1245620`, `Command: ['.../me3-launcher.exe', '--']`, `Options: {'forcelgadd', 'gamedrive'}`, `ntsync: up and running`. ER ran for more than 30 min that way.
- **MGSV** is started the same way. Our own `prun.sh` proved the chain works.

  ```
  gamescope --backend headless -w 1920 -h 1080 -W 1920 -H 1080 -- \
    env STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam \
        STEAM_COMPAT_DATA_PATH=<steam-library>/compatdata/287700 \
        STEAM_COMPAT_INSTALL_PATH=<steam-library>/common/MGS_TPP \
        STEAM_COMPAT_TOOL_PATHS="<Proton - Experimental>:<SteamLinuxRuntime_4>" \
        STEAM_COMPAT_LIBRARY_PATHS=$HOME/.local/share/Steam/steamapps \
        STEAM_COMPAT_APP_ID=287700 SteamAppId=287700 SteamGameId=287700 SteamOverlayGameId=287700 \
        WINEDLLOVERRIDES="dinput8=n,b"   # only if a dinput8 proxy is used; alternative: our own injector exe as the target
        PHANTOMRING_ROLE=guest \
    <SteamLinuxRuntime_4>/_v2-entry-point --verb=waitforexitandrun -- "<Proton - Experimental>/proton" waitforexitandrun \
        <PhantomRing guest injector exe or .../MGS_TPP/mgsvtpp.exe> [args]
  ```

  - Per-launch env vars (`WINEDLLOVERRIDES`, `PROTON_LOG`, `DXVK_*`) need no edit to Steam's launch options.
  - Steam must be running for `SteamAPI_Init`.
  - The injector-exe variant avoids modifying the game dir: `CreateProcess(mgsvtpp.exe, CREATE_SUSPENDED)` + `LoadLibrary` injection, like me3-launcher. Placing `dinput8.dll` in `MGS_TPP` would be a user-approved install step.
  - The `SteamAppId` env vs `steam_appid.txt` precedence is UNVERIFIED, so set both consistently.
  - `waitforexitandrun` for 287700 does not interfere with ER, which uses another prefix.

**B. Started by the ER plugin at runtime (SkyCraft-style auto-launch).** From ER's PE plugin, spawn a Unix command. There are three ways:

1. **`ntdll!__wine_unix_spawnvp(char *const argv[], int wait)`**. Proton's `steam.exe` uses it.
2. `CreateProcessW` on a Unix path (VERIFIED in §3 T10 and §4.3).
3. **`steam-runtime-launch-client --alongside-steam -- <host command>`** inside ER's container. This is VERIFIED to execute on the host, so the host launcher can then run gamescope + SLR + Proton for MGSV. That gives MGSV its own container and prefix. This is the best match for "the guest runs hidden in the background, started by the host game".

**C. Through Steam.**

- `steam -applaunch 287700 <args>` from the host. From inside a container, `/usr/bin/steam` (= `steam-runtime-steam-remote`) writes to `~/.steam/steam.pipe`. Alternatively, `ShellExecuteW(L"steam://rungameid/287700")` from ER does the same.
- How the `steam://` URL is forwarded:
  - ER's prefix registry has `[Software\Classes\steam\shell\open\command] @="\"C:\Program Files (x86)\Steam\Steam.exe\" -- \"%1\""` (`compatdata/1245620/pfx/system.reg` line 35913).
  - That `steam.exe` is Proton's helper (`files/lib/wine/x86_64-windows/steam.exe`, strings `steam://`, `steam://rungameid/%s`, `__wine_unix_spawnvp`).
  - Its `steam_command_handler()` (Proton `steam_helper/steam.c`, branch experimental_11.0, lines 650–745) forwards any `steam://…` or `-option` argv. It tries `steam-runtime-steam-remote`, then `steam`, through `__wine_unix_spawnvp`. The one exception is `steam://rungameid/<own SteamGameId>`, which signals `PROTON_STEAM_EXE_RESTART_APP`.
- Limitations:
  - Steam then launches MGSV with **the user's saved launch options only**. Env vars cannot be passed through `-applaunch` (UNVERIFIED); extra args are appended to the game command line (UNVERIFIED).
  - A `steam://run/<id>//<args>/` launch shows a confirmation dialog (UNVERIFIED, forum reports).
  - Making the guest headless or modded via Steam would need persistent launch options. One example is `/path/phantomring-guest-wrap %command%`, which decides at runtime: if `/dev/shm/phantomring_v1` requests a guest, it execs `gamescope --backend headless ... -- "$@"` with guest env; otherwise it execs `"$@"`. The user must set this once in Steam.
  - Running two different Steam games at once is normally allowed (UNVERIFIED here, not attempted).

**D. MGSV as a child inside ER's prefix.** Not attempted. Implications:

- MGSV gets ER's Proton env. Override `SteamAppId/SteamGameId/SteamOverlayGameId=287700` in the child env so the winedmo hack and the Steam app identity are right.
- It shares ER's wineserver, so SkyCraft's `Local\` names, named events and zero-copy D3D11→D3D12 handles all work (§9.1).
- Saves still go to Steam userdata (inferred).
- It runs on ER's X display, so it cannot use gamescope. Hide its window by never showing it (hook `ShowWindow`/`CreateWindowExW` in the guest plugin) and spoof focus.
- When ER exits, Proton's `waitforexitandrun` for ER waits for MGSV too, because they share one wineserver.

### 7.3 Choosing between "separate prefixes" (A/B) and "same prefix" (D)

| | Separate prefixes (A/B) | Same prefix (D) |
|---|---|---|
| IPC | `/dev/shm` file mapping (VERIFIED). No named events, so use spin/futex-stub/UDP | Everything SkyCraft uses works as on Windows |
| GPU frames | Through shm with D3D12 `OpenExistingHeapFromAddress` (VERIFIED zero CPU copy) or a CPU copy | Zero-copy shared texture + shared fences (VERIFIED texture; fence UNVERIFIED) |
| Hiding | gamescope headless: nested focus, nothing on screen (VERIFIED) | Hidden window + focus hooks (Present OK on hidden windows, VERIFIED) |
| Isolation | MGSV keeps its own prefix, DLL overrides and Proton per-app settings | MGSV inherits ER's prefix and env; EAC is not involved (ER runs via me3 offline) |
| Launch | Linux launcher or the alongside-steam service | `CreateProcess` from the ER plugin |

---

## 8. Keeping the guest hidden but rendering

### 8.1 DXVK source (VERIFIED, DXVK commit 68530156 = the one in Proton)

`src/dxgi/dxgi_swapchain.cpp` `DxgiSwapChain::PresentBase`:

```cpp
if ((m_desc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD || m_desc.SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL) && wsi::isMinimized(m_window))
  return DXGI_STATUS_OCCLUDED;
bool occluded = !m_descFs.Windowed && wsi::isOccluded(m_window) && !wsi::isMinimized(m_window);
...
if (occluded) { if (!(PresentFlags & DXGI_PRESENT_TEST)) SetFullscreenState(FALSE, nullptr); hr = DXGI_STATUS_OCCLUDED; }
```

`src/wsi/win32/wsi_window_win32.cpp`:

- `isMinimized` = `GWL_STYLE & WS_MINIMIZE`.
- `isOccluded` = not foreground for more than 100 ms.

So an exclusive-fullscreen guest that loses foreground is **forced out of fullscreen**. The guest must run windowed or borderless.

`src/d3d11/d3d11_swapchain.cpp`: `acquireNextImage` → `VK_NOT_READY` → `DXGI_STATUS_OCCLUDED`. The frame-latency semaphore is still released on failed presents, so apps that wait on it will not deadlock.

### 8.2 d3dprobe measurements (VERIFIED)

d3dprobe ran under Proton + DXVK in the SLR 4 container. Every frame it clears an offscreen RT with a frame-encoded colour, copies it to staging, reads back the 2-frame-old staging and checks the colour.

| Mode | Swap | Sync | fps | Present results | GPU work verified |
|---|---|---|---|---|---|
| noswap (no window), 1920×1080 | — | — | 1845 | — | 9223/9223 |
| hidden (never `ShowWindow`), 640×360 | FLIP_DISCARD | 0 | 6467 | S_OK ×25867 | yes |
| hidden | DISCARD (blit) | 0 | 6377 | S_OK ×25507 | yes |
| hidden | FLIP_DISCARD | 1 | **59** | S_OK, avg 16.7 ms | yes |
| minimized (`SW_SHOWMINNOACTIVE`, rect −32000) | FLIP_DISCARD | 0 | 7169 | **DXGI_STATUS_OCCLUDED ×28678 (all)** | yes |
| minimized | DISCARD | 0 | 8848 | **OCCLUDED ×35391 (all)** | yes |
| offscreen (−6000,−6000), shown no-activate | FLIP_DISCARD | 0 / 1 | 5158 / 59 | S_OK | yes |
| **inside `gamescope --backend headless`**, shown + activated | FLIP_DISCARD | 1 | **60** | S_OK ×301 | yes, and **`GetForegroundWindow()==hwnd` = 1** |

Readback and upload costs at 1920×1080 RGBA8 (8,294,400 B) under DXVK:

- `Map(staging, READ)` of a 2-frame-old copy: **avg 0.043 ms** (max 2.06).
- Row-wise memcpy out: **0.455 ms**.
- Upload proxy (`Map(WRITE_DISCARD)` dynamic + memcpy + `CopyResource`): **0.306 ms** CPU (max 0.56).

Under headless gamescope, the gamescope WSI layer engaged ("Made gamescope surface for xid …", 16.67 ms refresh cycle). At shutdown gamescope logged `NVVM compilation failed: 3` / `vkCreateComputePipelines failed (VkResult: -13)` on the NVIDIA driver. It did not affect the client. Watch this when the guest runs for long periods (UNVERIFIED impact).

### 8.3 Recommendations

- **Use headless gamescope for the guest** (path A/B). MGSV truly has focus inside its nested X server, so its `GetForegroundWindow`/`WM_ACTIVATEAPP`/raw-input focus logic stays happy. No real input reaches it, which is what we want: ER owns real input and forwards it through shm, as SkyCraft's InputBridge does. Nothing appears on the user's desktop.
- **In same-prefix mode (D):** never show the guest window and spoof focus. Hook `GetForegroundWindow`, `GetActiveWindow`, `WM_ACTIVATEAPP` and the `RegisterRawInputDevices` flags, and feed input from shm.
- **Avoid minimizing.** DXVK returns OCCLUDED, and many engines pause on OCCLUDED or on focus loss. Avoid exclusive fullscreen for the same reason.
- **Audio** (UNVERIFIED options): the guest must be silent. Run it with `PULSE_SINK=<null sink>`, set its PipeWire stream volume to 0 after start, or mute Wwise/XAudio2 inside the guest plugin.

---

## 9. GPU frame transport

### 9.1 Same wineserver: zero-copy shared textures (VERIFIED with `gpushare.exe`)

The producer used D3D11 (DXVK):

- `CreateTexture2D(MISC_SHARED|MISC_SHARED_NTHANDLE)` → `S_OK`.
- `IDXGIResource1::CreateSharedHandle(name)` → `S_OK`, handle `0x9c`.
- `IDXGIResource::GetSharedHandle` (KMT) on an NT-handle resource → `E_INVALIDARG` (0x80070057).

Consumers ran in the same prefix but a **different container** (verb `run`). The handle was passed through `/dev/shm` with `OpenProcess(PROCESS_DUP_HANDLE)` + `DuplicateHandle`:

- D3D11 (DXVK) `ID3D11Device1::OpenSharedResource1` → `S_OK`, pixel `(64,128,191,255)` correct. `OpenSharedResourceByName` → `E_NOTIMPL` (DXVK source line 1631 also says "Not implemented").
- **D3D12 (vkd3d-proton) `ID3D12Device::OpenSharedHandle`** → `S_OK`, a 256×256 `DXGI_FORMAT_R8G8B8A8_UNORM` (28) texture; `CopyTextureRegion` readback pixel correct. **Zero-copy D3D11→D3D12 across processes works.** `ID3D12Device::OpenSharedHandleByName` → `E_NOTIMPL`.
- DXVK supports **shared D3D11 fences** (`D3D11_FENCE_FLAG_SHARED`, `D3D11Fence::CreateSharedHandle`, `D3D11Device::OpenSharedFence`; VERIFIED in source at the Proton commit). Opening one from D3D12 is UNVERIFIED.

### 9.2 Different wineservers: no handle sharing (VERIFIED)

The cross-prefix consumer failed:

- `OpenProcess(324)` opened *itself*, because of the PID collision.
- `DuplicateHandle` → error 6.
- `OpenSharedHandle` → `0x80070006`.
- `OpenSharedHandleByName` → `E_NOTIMPL`.

NT, KMT and D3DKMT objects live in a wineserver. The underlying Vulkan fds are not reachable from PE code without a custom Wine unixlib, which would mean modifying Proton. That is forbidden and fragile.

### 9.3 Cross-prefix fast path: GPU ↔ `/dev/shm` directly (VERIFIED)

`gpushare.exe heap12` (vkd3d-proton, Proton container) ran these steps:

1. Map a 16 MiB view of `/dev/shm/phantomring_heap` (view address `0xcf0000`).
2. `ID3D12Device3::OpenExistingHeapFromAddress(view)` → `S_OK`. A control call on `VirtualAlloc` memory also gave `S_OK`.
3. `CreatePlacedResource` (buffer, `D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER`) on that heap → `S_OK`.
4. **shm → VRAM** `CopyBufferRegion` 16 MiB: best **0.352–0.364 ms (~46–48 GB/s)**; the data verified.
5. **VRAM → shm** `CopyBufferRegion` 16 MiB: best **0.359 ms (46.8 GB/s)**. The CPU view, and later a Linux-side python read of `/dev/shm/phantomring_heap` after exit, saw the new data with 0 mismatches.

Proposed frame path:

- **Guest (MGSV process).** The plugin creates its own D3D12 device (vkd3d-proton loads fine next to DXVK).
  - It shares MGSV's chosen render targets (HUD layer, Snake layer, depth if needed) D3D11 → D3D12 in-process through NT handles; §9.1 proved this cross-process, so in-process should work.
  - Each frame it runs `CopyTextureRegion` into placed buffers on an `OpenExistingHeapFromAddress` heap over the frame slots of `/dev/shm/phantomring_v1`, then publishes `frameId` after the D3D12 fence completes.
  - D3D11→D3D12 ordering: a shared D3D11 fence (UNVERIFIED in vkd3d) or `Flush` + event query.
- **Host (ER process).** The plugin opens the same slots with `OpenExistingHeapFromAddress` and does `CopyTextureRegion` buffer→texture in its own command list before compositing.
- **Cost per 1080p RGBA layer:** about 0.2 ms of GPU copy each way and **no CPU memcpy**. Latency is 1–2 frames. Fallback without D3D12 on the guest: staging `Map` + memcpy straight into the shm slot, about 0.5 ms CPU.
- **Budget for the CPU-only path at 60 fps** (measured numbers):
  - Guest readback ≈ 0.04 ms Map + 0.46 ms copy into shm.
  - Host upload ≈ 0.3–0.6 ms.
  - About **1–1.2 ms CPU per RGBA layer per frame**, doubled for RGBA+D32.
  - Bandwidth: 0.5 GB/s per copy at 60 fps (8.3 MB), well under the measured 10–15 GB/s.
- Alignment: Windows views are 64 KiB aligned. Keep frame slots 64 KiB aligned and sized to multiples of 64 KiB.

---

## 10. Concrete recommendations for the PhantomRing protocol

These differ from SkyCraft (`protocol/skycraft_protocol.h` uses `Local\SkyCraft_v1` + named events + `GetTickCount64` heartbeats).

1. **Mapping.** Create a file `/dev/shm/phantomring_v1` (mode 0600). Open it from both sides with `CreateFileW(wine_get_dos_file_name("/dev/shm/phantomring_v1"), …, OPEN_ALWAYS)` + `CreateFileMappingW` + `MapViewOfFile`.
   - The creator sizes the file with `SetEndOfFile`.
   - Keep the header layout identical to SkyCraft's idea (magic, version, seqlock slots, SPSC rings), but **add** `hostUnixPid`, `guestUnixPid`, `hostWinePrefix[256]`, `guestWinePrefix[256]` and a per-boot `sessionNonce`. Then a stale file from a crashed run is detected: the PID is gone, or `/proc/<pid>/stat` starttime differs.
   - A Linux-side launcher or monitor can `mmap` the same file natively (VERIFIED with python).
2. **No named events.** Replace them in this order of preference:
   - Bounded **spin + `_mm_pause` + `SwitchToThread`/`Sleep(0)`** on seqlock/ring counters, for per-frame data where both sides are busy anyway. RTT is 0.2 µs.
   - An optional **futex** through a stub at ≥ `0x7001'0000'0000` (wait with a 2–10 ms timeout; handle EINTR/EAGAIN/ETIMEDOUT) for idle waits: 10–13 µs wake.
   - Or a **UDP 127.0.0.1 doorbell** (~90 µs mean, 5–7 ms worst case) as a portable fallback that needs no syscall tricks.
3. **Liveness.**
   - Each side holds `LockFileEx(LOCKFILE_EXCLUSIVE_LOCK)` on its own byte in a lock file (`/dev/shm/phantomring_v1.lock`, bytes 0 = host, 1 = guest) for its whole life.
   - The peer probes with `LOCKFILE_FAIL_IMMEDIATELY`. "BUSY" means alive, "ACQUIRED" means dead; unlock immediately after probing.
   - A watchdog thread can also block on the peer's byte to be woken when it dies (VERIFIED, wakes on exit). Back this up with `Z:\proc\<unixpid>` existence and heartbeats.
4. **Clocks.** Use `QueryPerformanceCounter` for heartbeats and latency. QPC readings from different prefixes were directly comparable: futex wake latencies of 10–13 µs were computed across prefixes from QPC timestamps. Don't use Windows PIDs or `GetTickCount64` across prefixes; GetTickCount64 is UNVERIFIED but probably fine.
5. **Unix pid.** `getpid` through the high stub, or have the launcher write PIDs into the header. Never use `/proc/self` through Wine.
6. **Frames.** §9.3. Frame slots go in the same file or a second file, `/dev/shm/phantomring_v1.frames`, 64 KiB aligned.
7. **Debug/test stand-ins.** `shmtest.exe` modes double as a fake peer. The same file can be driven by a native Linux python "fake_host"/"fake_guest", like SkyCraft's `tools/fake_skyrim.py`.

---

## 11. Open items / UNVERIFIED

- MGSV itself was not launched (forbidden here). Still to verify on the first real run:
  - SteamAPI_Init when started by our launcher, and env vs `steam_appid.txt` precedence.
  - The protector's tolerance for suspended-start injection.
  - Behaviour on focus loss and on OCCLUDED.
  - Online/FOB/MGO connection prompts when offline.
  - Where `TPP_GRAPHICS_CONFIG` lives.
- `steam -applaunch` argument and env semantics, and the `steam://run//args` confirmation dialog. These come from forum reports.
- D3D12 opening a DXVK shared fence. vkd3d-proton `OpenExistingHeapFromAddress` with sizes or offsets not aligned to 64 KiB.
- The headless-gamescope `vkCreateComputePipelines` error at teardown on NVIDIA 595. Also gamescope headless performance at 1920×1080 with a real game.
- Raw-syscall stub address `0x710000000000`: it is free in our test processes. In MGSV and ER, also try other addresses in `[0x7001'0000'0000, 0x7fff'0000'0000)` and fall back to UDP if `VirtualAlloc` fails.
- Proton's seccomp filter constants come from this Proton build (experimental-11.0-20261001). Re-check them after Proton updates; the `lowsyscall` mode is the quick test.

## 12. Sources

- Local, read-only:
  - `.../Proton - Experimental/proton` (verb dispatch lines 2205–2232) and `.../files/lib/wine/x86_64-unix/ntdll.so` (seccomp filter at .data `0xa6340`, `signal_init_process` around `0x5088b`).
  - `.../x86_64-windows/steam.exe` (strings) and `winedmo.so` (strings).
  - `SteamLinuxRuntime_4/{_v2-entry-point,run,toolmanifest.vdf,pressure-vessel/bin/pressure-vessel-wrap}`.
  - `compatdata/1245620/pfx/system.reg` (steam: handler) and `MGS_TPP/{mgsvtpp.exe,steam_appid.txt,version_info.txt}`.
  - `~/.local/share/Steam/{config/config.vdf,userdata/<account-id>/...,logs/console-linux.txt}`.
  - The old project: `MGS/scripts/project.py`, `MGS/tools/me3/launch-eldenring-mods.sh`, `MGS/tools/me3/bin/me3` (strings), `MGS/reports/launch-diagnostics/*/steam-1245620.log`.
- Fetched with `gh api` (raw):
  - [ValveSoftware/Proton `steam_helper/steam.c` (experimental_11.0)](https://github.com/ValveSoftware/Proton/blob/experimental_11.0/steam_helper/steam.c).
  - [doitsujin/dxvk `src/dxgi/dxgi_swapchain.cpp`, `src/d3d11/d3d11_swapchain.cpp`, `src/wsi/win32/wsi_window_win32.cpp`, `src/d3d11/d3d11_fence.cpp`, `src/d3d11/d3d11_device.cpp` @685301564ea3](https://github.com/doitsujin/dxvk/tree/685301564ea3486a7098bd37194020da84299617).
  - [garyttierney/me3 `crates/cli/src/commands/launch/strategy/compat_tool.rs`](https://github.com/garyttierney/me3/blob/main/crates/cli/src/commands/launch/strategy/compat_tool.rs).
- Web:
  - [Valve steam-runtime `doc/steamlinuxruntime-known-issues.md`](https://github.com/ValveSoftware/steam-runtime/blob/master/doc/steamlinuxruntime-known-issues.md) (shared-directory list, `PRESSURE_VESSEL_FILESYSTEMS_RO/RW`).
  - [Valve Developer Wiki: Command line options (Steam)](https://developer.valvesoftware.com/wiki/Command_line_options_(Steam)).
  - Steam Community threads on `steam://run//args` prompts: [3830914462336948405](https://steamcommunity.com/discussions/forum/1/3830914462336948405), [1744478429684418112](https://steamcommunity.com/discussions/forum/1/1744478429684418112).
  - `gitlab.steamos.cloud` (steam-runtime-tools docs) was blocked by Anubis and was not read.
