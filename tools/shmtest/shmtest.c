/*
 * shmtest.exe - PhantomRing cross-Proton-prefix IPC probe.
 *
 * Proves (or disproves) that two Windows processes running under DIFFERENT
 * wineservers (different WINEPREFIXes / different Proton containers) can share
 * memory through a file-backed section on a host tmpfs (/dev/shm), and measures
 * what that channel can do: coherence, interlocked atomics, round-trip latency,
 * bulk frame-copy bandwidth, cross-process wakeups (UDP loopback, raw futex) and
 * liveness detection through the host /proc.
 *
 * Build:  x86_64-w64-mingw32-gcc -O2 -std=c17 -Wall -static -o shmtest.exe shmtest.c -lws2_32
 *
 * Paths: a path starting with '/' is a Unix path and is converted with Wine's
 * kernel32!wine_get_dos_file_name (works even if the Z: drive was removed);
 * anything else is used as a Windows path (e.g. Z:\dev\shm\phantomring_test).
 *
 * Modes (run `shmtest.exe` without args for the list):
 *   info                                     wine version, unix pid, path conversion
 *   writer   <path> <seconds>                increments a shared counter every ~1 ms
 *   reader   <path> <seconds>                samples the counter, checks monotonicity
 *   inc      <path> <iters> <nprocs>         barrier, then InterlockedIncrement64 + CAS-add from every process
 *   pingpong <path> <a|b> <iters>            spin-based round trip latency (a=initiator)
 *   bench    <path> <w|r> <frames> <bytes>   triple-buffered frame copy throughput (w=producer)
 *   futex    <path> <wait|wake> [count]      raw Linux futex(2) syscall on a word in the shared file
 *   udp      <server|client> <port> <iters>  loopback UDP ping-pong latency (wakeup channel)
 *   named-create <name> <seconds>            CreateFileMappingW(pagefile, name) + CreateEventW(name+"_evt")
 *   named-open   <name>                      OpenFileMappingW / OpenEventW, prints the result
 *   alive    <unixpid>                       liveness check via Z:\proc\<pid> (host pid namespace)
 *   lock     <path> <hold|try|wait> [secs]   LockFileEx byte-range lock (cross-wineserver via fcntl?)
 *   unixexec <cmdline>                       CreateProcessW on a Unix ELF/script, prints exit code
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MAGIC 0x31544D48534E4850ull /* "PHNSHMT1" */
#define HDR_BYTES 0x10000ull        /* 64 KiB of control words */
#define CL 64

/* Shared control block. Every hot field sits on its own cache line. */
typedef struct {
    volatile uint64_t magic;               /* 0x000 */
    volatile uint64_t layout_bytes;        /* 0x008 total mapping size the creator set */
    uint8_t _p0[CL - 16];
    volatile int64_t counter;              /* 0x040 writer counter */
    volatile int64_t writer_unixpid;
    volatile int64_t writer_qpc;
    uint8_t _p1[CL - 24];
    volatile LONG barrier;                 /* 0x080 inc: arrival counter */
    uint8_t _p2[CL - 4];
    volatile LONG64 xadd_total;            /* 0x0C0 inc: lock xadd target */
    uint8_t _p3[CL - 8];
    volatile LONG64 cas_total;             /* 0x100 inc: lock cmpxchg target */
    uint8_t _p4[CL - 8];
    volatile LONG done;                    /* 0x140 inc: finished counter */
    uint8_t _p5[CL - 4];
    volatile int64_t ping;                 /* 0x180 pingpong a->b */
    uint8_t _p6[CL - 8];
    volatile int64_t pong;                 /* 0x1C0 pingpong b->a */
    uint8_t _p7[CL - 8];
    volatile uint32_t futex_word;          /* 0x200 futex */
    uint8_t _p8[CL - 4];
    volatile int64_t bench_published;      /* 0x240 bench: last frame index written (+1) */
    uint8_t _p9[CL - 8];
    volatile int64_t bench_consumed;       /* 0x280 bench: last frame index read (+1) */
    uint8_t _pa[CL - 8];
    volatile int64_t bench_frame_bytes;    /* 0x2C0 */
    volatile int64_t bench_reader_ready;
} Ctl;

static LARGE_INTEGER g_freq;
static double now_s(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)g_freq.QuadPart; }

static void die(const char *what) {
    fprintf(stderr, "FAIL %s: GetLastError=%lu\n", what, GetLastError());
    fflush(stderr);
    ExitProcess(2);
}

/* ---- Wine helpers (resolved dynamically so the exe also runs on Windows) ---- */
typedef WCHAR *(CDECL *wine_get_dos_file_name_t)(const char *);
typedef const char *(CDECL *wine_get_version_t)(void);

static WCHAR *to_win_path(const char *arg) {
    static WCHAR buf[4096];
    if (arg[0] == '/') {
        wine_get_dos_file_name_t f = (wine_get_dos_file_name_t)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name");
        if (!f) { fprintf(stderr, "wine_get_dos_file_name not available (not Wine?)\n"); ExitProcess(2); }
        WCHAR *w = f(arg);
        if (!w) die("wine_get_dos_file_name");
        wcsncpy(buf, w, 4095);
        HeapFree(GetProcessHeap(), 0, w);
        return buf;
    }
    MultiByteToWideChar(CP_UTF8, 0, arg, -1, buf, 4096);
    return buf;
}

/* ---- raw Linux syscalls from PE code ----------------------------------------
 * Proton's ntdll installs a seccomp BPF filter that TRAPs every syscall whose
 * instruction pointer is below 0x7001'0000'0000 (i.e. issued from PE code) and
 * emulates it as a *Windows* NT syscall. Native .so code lives above that line.
 * So the `syscall` instruction must execute from a page allocated above it.
 * Vanilla Wine has no such filter; the high stub works there too. */
static void *g_sys_stub;
static void *make_syscall_stub(void) {
    static const unsigned char code[] = { 0x0f, 0x05, 0xc3 }; /* syscall; ret */
    for (uint64_t a = 0x710000000000ull; a < 0x7f0000000000ull; a += 0x10000000000ull) {
        void *p = VirtualAlloc((void *)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) { memcpy(p, code, sizeof code); FlushInstructionCache(GetCurrentProcess(), p, 16); return p; }
    }
    return NULL;
}
/* NOTE: Windows is LLP64 (long = 32 bit) -> every argument must be int64_t. */
static int64_t linux_syscall6(int64_t nr, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6) {
    if (!g_sys_stub) g_sys_stub = make_syscall_stub();
    if (!g_sys_stub) return -38; /* ENOSYS */
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    register int64_t r8 __asm__("r8") = a5;
    register int64_t r9 __asm__("r9") = a6;
    __asm__ volatile("call *%[stub]" : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9), [stub] "r"(g_sys_stub)
                     : "rcx", "r11", "memory");
    return ret;
}
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

/* Unix pid of the process that performed the open of /proc/self/stat. NOTE: in
 * Wine the open() is executed by the *wineserver*, so this returns the
 * wineserver's pid, not ours (kept as a demonstration). */
static int64_t proc_self_pid(void) {
    HANDLE h = CreateFileW(L"Z:\\proc\\self\\stat", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    char b[128] = {0}; DWORD n = 0;
    ReadFile(h, b, sizeof b - 1, &n, NULL);
    CloseHandle(h);
    return _atoi64(b);
}
/* Demonstration only: `syscall` executed from the exe's own .text (low address).
 * Vanilla Wine: reaches Linux (returns the pid). Proton: seccomp TRAP -> SIGSYS ->
 * Wine treats rax as a Windows NT syscall number and returns an NTSTATUS. */
static int64_t low_getpid(void) {
    int64_t ret;
    __asm__ volatile("mov $39, %%eax\n\tsyscall" : "=a"(ret) : : "rcx", "r11", "rdx", "rdi", "rsi", "r8", "r9", "r10", "memory");
    return ret;
}
/* Real Unix pid of this process: getpid(2) = 39 through the high stub. */
static int64_t unix_pid(void) { return linux_syscall6(39, 0, 0, 0, 0, 0, 0); }

static uint8_t *g_base;
static uint64_t g_size;

static Ctl *open_shared(const char *arg, uint64_t want_bytes) {
    WCHAR *path = to_win_path(arg);
    if (want_bytes < HDR_BYTES) want_bytes = HDR_BYTES;
    HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) die("CreateFileW");
    DWORD created = GetLastError() != ERROR_ALREADY_EXISTS;
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    if ((uint64_t)sz.QuadPart < want_bytes) {
        LARGE_INTEGER p; p.QuadPart = (LONGLONG)want_bytes;
        if (!SetFilePointerEx(f, p, NULL, FILE_BEGIN) || !SetEndOfFile(f)) die("SetEndOfFile");
        sz.QuadPart = (LONGLONG)want_bytes;
    }
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READWRITE, 0, 0, NULL);
    if (!m) die("CreateFileMappingW");
    void *v = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!v) die("MapViewOfFile");
    CloseHandle(m); CloseHandle(f); /* the view keeps the section alive */
    g_base = v; g_size = (uint64_t)sz.QuadPart;
    Ctl *c = v;
    if (c->magic != MAGIC) {
        uint64_t expect = 0;
        /* first opener initialises the header; racing openers wait for magic */
        if (InterlockedCompareExchange64((volatile LONG64 *)&c->layout_bytes, (LONG64)g_size, 0) == 0) {
            c->magic = MAGIC;
        } else {
            double t0 = now_s();
            while (c->magic != MAGIC && now_s() - t0 < 5.0) Sleep(1);
        }
        (void)expect;
    }
    printf("mapped %ls (%s) size=%llu at %p, unixpid=%lld\n", path, created ? "created" : "existing",
           (unsigned long long)g_size, v, (long long)unix_pid());
    fflush(stdout);
    return c;
}

static void cpu_relax(void) { __builtin_ia32_pause(); }

/* ---------------------------------------------------------------- modes */
static int m_info(void) {
    wine_get_version_t gv = (wine_get_version_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
    printf("wine_get_version: %s\n", gv ? gv() : "(not wine)");
    int64_t up = unix_pid();
    printf("windows pid=%lu  unix pid via getpid stub=%lld (stub at %p)  pid seen by open(/proc/self)=%lld\n",
           GetCurrentProcessId(), (long long)up, g_sys_stub, (long long)proc_self_pid());
    WCHAR pp[64]; swprintf(pp, 64, L"Z:\\proc\\%lld\\comm", (long long)up);
    HANDLE h = CreateFileW(pp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char comm[64] = {0}; DWORD n = 0;
    if (h != INVALID_HANDLE_VALUE) { ReadFile(h, comm, 63, &n, NULL); CloseHandle(h); }
    printf("/proc/<getpid>/comm = %s", n ? comm : "(unreadable)\n");
    printf("/dev/shm -> %ls\n", to_win_path("/dev/shm"));
    WCHAR prefix[512]; DWORD pn = GetEnvironmentVariableW(L"WINEPREFIX", prefix, 512);
    printf("WINEPREFIX env=%ls\n", pn ? prefix : L"(unset)");
    WCHAR sysdir[MAX_PATH]; GetSystemDirectoryW(sysdir, MAX_PATH);
    printf("system dir=%ls\n", sysdir);
    return 0;
}

static int m_writer(const char *path, int seconds) {
    Ctl *c = open_shared(path, 0);
    c->writer_unixpid = unix_pid();
    double t0 = now_s(), last = t0;
    int64_t v = c->counter;
    while (now_s() - t0 < seconds) {
        v++;
        c->writer_qpc = (int64_t)(now_s() * 1e6);
        __atomic_store_n(&c->counter, v, __ATOMIC_RELEASE);
        if (now_s() - last >= 1.0) { printf("writer: counter=%lld\n", (long long)v); fflush(stdout); last = now_s(); }
        Sleep(1);
    }
    printf("writer: done, final counter=%lld\n", (long long)v);
    return 0;
}

static int m_reader(const char *path, int seconds) {
    Ctl *c = open_shared(path, 0);
    double t0 = now_s();
    int64_t prev = -1, first = -1; long samples = 0, changes = 0, regress = 0;
    while (now_s() - t0 < seconds) {
        int64_t v = __atomic_load_n(&c->counter, __ATOMIC_ACQUIRE);
        if (first < 0) first = v;
        if (prev >= 0 && v != prev) changes++;
        if (prev >= 0 && v < prev) regress++;
        if (samples % 4 == 0) { printf("reader: t=%.2fs counter=%lld writer_unixpid=%lld\n", now_s() - t0, (long long)v, (long long)c->writer_unixpid); fflush(stdout); }
        prev = v; samples++;
        Sleep(250);
    }
    printf("reader: samples=%ld changes=%ld regressions=%ld first=%lld last=%lld -> %s\n", samples, changes, regress,
           (long long)first, (long long)prev, (changes > 0 && regress == 0) ? "LIVE+MONOTONIC" : "NOT LIVE");
    return (changes > 0 && regress == 0) ? 0 : 1;
}

static int m_inc(const char *path, long iters, int nprocs) {
    Ctl *c = open_shared(path, 0);
    LONG me = InterlockedIncrement(&c->barrier);
    printf("inc: arrived %ld/%d, waiting for barrier\n", me, nprocs); fflush(stdout);
    double t0 = now_s();
    while (c->barrier < nprocs) { cpu_relax(); if (now_s() - t0 > 60) { printf("inc: barrier timeout\n"); return 1; } }
    double ts = now_s();
    for (long i = 0; i < iters; i++) {
        InterlockedIncrement64(&c->xadd_total);           /* lock xadd */
        LONG64 o, n;
        do { o = c->cas_total; n = o + 3; } while (InterlockedCompareExchange64(&c->cas_total, n, o) != o); /* lock cmpxchg */
    }
    double te = now_s();
    LONG d = InterlockedIncrement(&c->done);
    printf("inc: my %ld iters took %.3f s (%.1f ns per xadd+cas pair)\n", iters, te - ts, (te - ts) * 1e9 / iters);
    if (d == nprocs) {
        LONG64 ex = (LONG64)iters * nprocs;
        printf("inc: LAST FINISHER xadd_total=%lld (expect %lld) cas_total=%lld (expect %lld) -> %s\n",
               (long long)c->xadd_total, (long long)ex, (long long)c->cas_total, (long long)(ex * 3),
               (c->xadd_total == ex && c->cas_total == ex * 3) ? "ATOMIC OK" : "LOST UPDATES");
    }
    fflush(stdout);
    return 0;
}

static int m_pingpong(const char *path, const char *role, long iters) {
    Ctl *c = open_shared(path, 0);
    if (role[0] == 'b') {
        c->pong = 0;
        printf("pingpong b: echoing\n"); fflush(stdout);
        for (int64_t i = 1; i <= iters; i++) {
            double t0 = now_s();
            while (__atomic_load_n(&c->ping, __ATOMIC_ACQUIRE) < i) { cpu_relax(); if (now_s() - t0 > 30) { printf("b: timeout at %lld\n", (long long)i); return 1; } }
            __atomic_store_n(&c->pong, i, __ATOMIC_RELEASE);
        }
        printf("pingpong b: done\n");
        return 0;
    }
    /* a: wait until b has reset pong and is spinning (give it a moment) */
    c->ping = 0; Sleep(500);
    double *lat = malloc(sizeof(double) * iters);
    double tall = now_s();
    for (int64_t i = 1; i <= iters; i++) {
        double t0 = now_s();
        __atomic_store_n(&c->ping, i, __ATOMIC_RELEASE);
        while (__atomic_load_n(&c->pong, __ATOMIC_ACQUIRE) < i) { cpu_relax(); if (now_s() - t0 > 30) { printf("a: timeout at %lld\n", (long long)i); return 1; } }
        lat[i - 1] = now_s() - t0;
    }
    tall = now_s() - tall;
    /* simple stats */
    double mn = 1e9, mx = 0, sum = 0;
    for (long i = 0; i < iters; i++) { sum += lat[i]; if (lat[i] < mn) mn = lat[i]; if (lat[i] > mx) mx = lat[i]; }
    /* median by partial sort (iters small) */
    for (long i = 0; i < iters; i++) for (long j = i + 1; j < iters && i < iters / 2 + 1; j++) if (lat[j] < lat[i]) { double t = lat[i]; lat[i] = lat[j]; lat[j] = t; }
    printf("pingpong a: %ld round trips in %.3f s: min=%.2f us median=%.2f us mean=%.2f us max=%.2f us\n", iters, tall,
           mn * 1e6, lat[iters / 2] * 1e6, sum / iters * 1e6, mx * 1e6);
    return 0;
}

static int m_bench(const char *path, const char *role, long frames, long long fbytes) {
    const int SLOTS = 3;
    uint64_t total = HDR_BYTES + (uint64_t)fbytes * SLOTS;
    Ctl *c = open_shared(path, total);
    uint8_t *slots = g_base + HDR_BYTES;
    uint8_t *priv = VirtualAlloc(NULL, (SIZE_T)fbytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!priv) die("VirtualAlloc");
    memset(priv, 0x5a, (size_t)fbytes);
    if (role[0] == 'w') {
        c->bench_published = 0; c->bench_consumed = 0; c->bench_frame_bytes = fbytes;
        printf("bench w: waiting for reader\n"); fflush(stdout);
        double tw = now_s();
        while (!__atomic_load_n(&c->bench_reader_ready, __ATOMIC_ACQUIRE)) { Sleep(1); if (now_s() - tw > 30) { printf("no reader\n"); return 1; } }
        double t0 = now_s(), stall = 0;
        for (int64_t f = 0; f < frames; f++) {
            double ts = now_s();
            while (f - __atomic_load_n(&c->bench_consumed, __ATOMIC_ACQUIRE) >= SLOTS - 1) cpu_relax(); /* keep one slot free */
            stall += now_s() - ts;
            uint8_t *dst = slots + (uint64_t)(f % SLOTS) * fbytes;
            memcpy(dst, priv, (size_t)fbytes);
            memcpy(dst, &f, 8); memcpy(dst + fbytes - 8, &f, 8);   /* tag both ends */
            __atomic_store_n(&c->bench_published, f + 1, __ATOMIC_RELEASE);
        }
        double t = now_s() - t0;
        printf("bench w: %ld frames x %lld B in %.3f s = %.1f frames/s, %.2f GB/s copy-in (%.3f ms/frame, stalled %.3f s)\n",
               frames, fbytes, t, frames / t, (double)frames * fbytes / t / 1e9, t / frames * 1e3, stall);
        c->bench_reader_ready = 0;
        return 0;
    }
    __atomic_store_n(&c->bench_reader_ready, 1, __ATOMIC_RELEASE);
    printf("bench r: ready\n"); fflush(stdout);
    double t0 = -1; long bad = 0;
    for (int64_t f = 0; f < frames; f++) {
        double tw = now_s();
        while (__atomic_load_n(&c->bench_published, __ATOMIC_ACQUIRE) <= f) { cpu_relax(); if (now_s() - tw > 30) { printf("r: timeout at %lld\n", (long long)f); return 1; } }
        if (t0 < 0) t0 = now_s();
        uint8_t *src = slots + (uint64_t)(f % SLOTS) * fbytes;
        memcpy(priv, src, (size_t)fbytes);
        int64_t a, b; memcpy(&a, priv, 8); memcpy(&b, priv + fbytes - 8, 8);
        if (a != f || b != f) bad++;
        __atomic_store_n(&c->bench_consumed, f + 1, __ATOMIC_RELEASE);
    }
    double t = now_s() - t0;
    printf("bench r: %ld frames in %.3f s = %.1f frames/s, %.2f GB/s copy-out, torn/mismatched frames=%ld -> %s\n",
           frames, t, frames / t, (double)frames * fbytes / t / 1e9, bad, bad ? "CORRUPT" : "OK");
    return bad ? 1 : 0;
}

static int m_futex(const char *path, const char *op, long count) {
    Ctl *c = open_shared(path, 0);
    if (count <= 0) count = 1;
    if (!strcmp(op, "wait")) {
        for (long i = 0; i < count; i++) {
            uint32_t cur = c->futex_word;
            struct { int64_t s, ns; } ts = { 10, 0 };   /* struct timespec (64-bit) */
            double t0 = now_s();
            int64_t r = linux_syscall6(202, (int64_t)(uintptr_t)&c->futex_word, FUTEX_WAIT, (int64_t)cur, (int64_t)(uintptr_t)&ts, 0, 0);
            double t = now_s() - t0;
            double wake_lat_us = now_s() * 1e6 - (double)c->writer_qpc; /* QPC = CLOCK_MONOTONIC in both prefixes */
            printf("futex wait #%ld: ret=%lld (0=woken, -11=EAGAIN, -110=ETIMEDOUT) after %.3f ms, word=%u, wake->resume latency=%.1f us\n",
                   i, (long long)r, t * 1e3, c->futex_word, wake_lat_us);
            fflush(stdout);
        }
        return 0;
    }
    for (long i = 0; i < count; i++) {
        c->writer_qpc = (int64_t)(now_s() * 1e6);
        __atomic_add_fetch(&c->futex_word, 1, __ATOMIC_SEQ_CST);
        int64_t r = linux_syscall6(202, (int64_t)(uintptr_t)&c->futex_word, FUTEX_WAKE, 1, 0, 0, 0);
        printf("futex wake #%ld: ret=%lld (number of waiters woken)\n", i, (long long)r);
        fflush(stdout);
        Sleep(200);
    }
    return 0;
}

static int m_udp(const char *role, int port, long iters) {
    WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w)) die("WSAStartup");
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) die("socket");
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    DWORD tmo = 10000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof tmo);
    char buf[64];
    if (role[0] == 's') {
        a.sin_port = htons((u_short)port);
        if (bind(s, (struct sockaddr *)&a, sizeof a)) die("bind");
        printf("udp server: bound 127.0.0.1:%d\n", port); fflush(stdout);
        for (long i = 0; i < iters; i++) {
            struct sockaddr_in from; int fl = sizeof from;
            int n = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n <= 0) { printf("udp server: recv timeout/err %d\n", WSAGetLastError()); return 1; }
            sendto(s, buf, n, 0, (struct sockaddr *)&from, fl);
        }
        printf("udp server: done\n");
        return 0;
    }
    a.sin_port = htons((u_short)port);
    Sleep(300);
    double mn = 1e9, mx = 0, sum = 0;
    for (long i = 0; i < iters; i++) {
        double t0 = now_s();
        sendto(s, (char *)&i, sizeof i, 0, (struct sockaddr *)&a, sizeof a);
        int n = recvfrom(s, buf, sizeof buf, 0, NULL, NULL);
        double t = now_s() - t0;
        if (n <= 0) { printf("udp client: recv timeout/err %d at %ld\n", WSAGetLastError(), i); return 1; }
        sum += t; if (t < mn) mn = t; if (t > mx) mx = t;
    }
    printf("udp client: %ld blocking round trips: min=%.1f us mean=%.1f us max=%.1f us\n", iters, mn * 1e6, sum / iters * 1e6, mx * 1e6);
    return 0;
}

static int m_named_create(const char *name, int seconds) {
    WCHAR wn[256], we[300];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 256);
    swprintf(we, 300, L"%ls_evt", wn);
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, wn);
    printf("named-create: CreateFileMappingW(%ls) -> %p err=%lu\n", wn, m, GetLastError());
    HANDLE e = CreateEventW(NULL, TRUE, FALSE, we);
    printf("named-create: CreateEventW(%ls) -> %p err=%lu\n", we, e, GetLastError());
    if (m) { volatile uint64_t *p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0); if (p) *p = 0xC0FFEE; }
    fflush(stdout);
    Sleep((DWORD)seconds * 1000);
    printf("named-create: exiting\n");
    return 0;
}

static int m_named_open(const char *name) {
    WCHAR wn[256], we[300];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 256);
    swprintf(we, 300, L"%ls_evt", wn);
    HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wn);
    DWORD em = GetLastError();
    unsigned long long val = 0;
    if (m) { volatile uint64_t *p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0); if (p) val = *p; }
    HANDLE e = OpenEventW(EVENT_ALL_ACCESS, FALSE, we);
    DWORD ee = GetLastError();
    printf("named-open: OpenFileMappingW(%ls) -> %s (err=%lu) value=0x%llx; OpenEventW -> %s (err=%lu)\n", wn,
           m ? "OPENED" : "NOT FOUND", m ? 0 : em, val, e ? "OPENED" : "NOT FOUND", e ? 0 : ee);
    return m ? 0 : 1;
}


/* Cross-prefix liveness / wait primitive: wineserver mirrors Windows byte-range
 * locks onto the Unix fd with fcntl(F_SETLK), so another wineserver sees them. */
static int m_lock(const char *path, const char *op, int seconds) {
    WCHAR *wp = to_win_path(path);
    HANDLE f = CreateFileW(wp, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) die("CreateFileW(lock)");
    OVERLAPPED ov = {0};
    DWORD flags = LOCKFILE_EXCLUSIVE_LOCK | (strcmp(op, "wait") ? LOCKFILE_FAIL_IMMEDIATELY : 0);
    double t0 = now_s();
    BOOL ok = LockFileEx(f, flags, 0, 1, 0, &ov);
    DWORD err = ok ? 0 : GetLastError();
    double t = now_s() - t0;
    printf("lock %s: LockFileEx(%ls, %s) -> %s err=%lu after %.3f s\n", op, wp,
           (flags & LOCKFILE_FAIL_IMMEDIATELY) ? "exclusive|fail_immediately" : "exclusive|blocking",
           ok ? "ACQUIRED" : "BUSY", err, t);
    if (ok && !strcmp(op, "hold")) { Sleep((DWORD)seconds * 1000); printf("lock hold: exiting WITHOUT unlocking\n"); ExitProcess(0); }
    return ok ? 0 : 1;
}

static int m_alive(const char *pid) {
    WCHAR p[64]; swprintf(p, 64, L"Z:\\proc\\%hs\\stat", pid);
    DWORD a = GetFileAttributesW(p);
    printf("alive: %ls -> %s\n", p, a == INVALID_FILE_ATTRIBUTES ? "GONE" : "ALIVE");
    return a == INVALID_FILE_ATTRIBUTES;
}

static int m_unixexec(const char *cmd) {
    WCHAR wc[2048]; MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wc, 2048);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, wc, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("unixexec: CreateProcessW(%ls) FAILED err=%lu\n", wc, GetLastError());
        return 1;
    }
    DWORD w = WaitForSingleObject(pi.hProcess, 10000), code = 0xdead;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("unixexec: CreateProcessW(%ls) OK, wait=%lu exit=%lu\n", wc, w, code);
    return 0;
}

int main(int argc, char **argv) {
    QueryPerformanceFrequency(&g_freq);
    /* Under Proton the exe is started by steam.exe and a console app gets its own
     * conhost, so stdout does not reach the Linux terminal. SHMTEST_OUT=<unix or
     * windows path> appends all output to a file instead (build with -mwindows). */
    const char *out = getenv("SHMTEST_OUT");
    if (out && *out) {
        WCHAR *wp = to_win_path(out);
        if (!_wfreopen(wp, L"a", stdout)) return 3;
        _wfreopen(wp, L"a", stderr);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    {
        char cl[512]; int n = 0;
        for (int i = 0; i < argc && n < 480; i++) n += snprintf(cl + n, sizeof cl - n, "%s ", argv[i]);
        printf("---- %s(unix pid %lld)\n", cl, (long long)unix_pid());
    }
    if (argc < 2) {
        fprintf(stderr, "usage: see header of shmtest.c\n");
        return 2;
    }
    const char *m = argv[1];
#define A(i) (argc > (i) ? argv[i] : "")
    if (!strcmp(m, "info")) return m_info();
    if (!strcmp(m, "lowsyscall")) {
        int64_t hi = unix_pid();
        int64_t lo = low_getpid();
        printf("lowsyscall: getpid via high stub (%p) = %lld ; raw syscall from .text (%p) returned %lld (0x%llx) -> %s\n",
               g_sys_stub, (long long)hi, (void *)low_getpid, (long long)lo, (unsigned long long)lo,
               lo == hi ? "REACHED LINUX" : "INTERCEPTED (seccomp/NT syscall emulation)");
        return 0;
    }
    if (!strcmp(m, "writer")) return m_writer(A(2), atoi(A(3)));
    if (!strcmp(m, "reader")) return m_reader(A(2), atoi(A(3)));
    if (!strcmp(m, "inc")) return m_inc(A(2), atol(A(3)), argc > 4 ? atoi(A(4)) : 2);
    if (!strcmp(m, "pingpong")) return m_pingpong(A(2), A(3), atol(A(4)));
    if (!strcmp(m, "bench")) return m_bench(A(2), A(3), atol(A(4)), _atoi64(A(5)));
    if (!strcmp(m, "futex")) return m_futex(A(2), A(3), argc > 4 ? atol(A(4)) : 1);
    if (!strcmp(m, "udp")) return m_udp(A(2), atoi(A(3)), atol(A(4)));
    if (!strcmp(m, "named-create")) return m_named_create(A(2), atoi(A(3)));
    if (!strcmp(m, "named-open")) return m_named_open(A(2));
    if (!strcmp(m, "alive")) return m_alive(A(2));
    if (!strcmp(m, "lock")) return m_lock(A(2), A(3), atoi(A(4)));
    if (!strcmp(m, "unixexec")) return m_unixexec(A(2));
    fprintf(stderr, "unknown mode %s\n", m);
    return 2;
}
