/*
 * gpushare.exe - can a D3D11 texture (DXVK, i.e. MGSV's API) be opened zero-copy
 * by another process through D3D12 (vkd3d-proton, i.e. Elden Ring's API) under
 * Proton? Producer and consumer exchange the handle values through a small file
 * mapping on /dev/shm (which works across prefixes); the GPU handle itself is a
 * wineserver object, so it can only be duplicated within ONE wineserver/prefix.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -std=gnu17 -Wall -static -mwindows -o gpushare.exe gpushare.c   (WIDL_C_INLINE_WRAPPERS needed for struct-returning methods) -ld3d11 -ld3d12 -ldxgi -ldxguid -luuid
 * Usage:
 *   gpushare.exe produce11 <unix-shm-path> <seconds>   D3D11: shared texture (NT + KMT handle), cleared to (64,128,191,255)
 *   gpushare.exe consume12 <unix-shm-path>             D3D12: DuplicateHandle + OpenSharedHandle (+ByName) + readback
 *   gpushare.exe consume11 <unix-shm-path>             D3D11.1: OpenSharedResource1(NT) and OpenSharedResource(KMT) + readback
 *   gpushare.exe heap12    <unix-shm-path>             D3D12: OpenExistingHeapFromAddress on a /dev/shm file view, GPU copy, verify
 * Output: appended to $GPUSHARE_OUT (unix path) if set.
 */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef WCHAR *(CDECL *wine_get_dos_file_name_t)(const char *);
static FILE *g_out;
static WCHAR *dos(const char *p) {
    static WCHAR b[1024];
    wine_get_dos_file_name_t f = (wine_get_dos_file_name_t)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name");
    WCHAR *w = (p[0] == '/' && f) ? f(p) : NULL;
    if (w) { wcsncpy(b, w, 1023); b[1023] = 0; } else MultiByteToWideChar(CP_UTF8, 0, p, -1, b, 1024);
    return b;
}
static double wall(void) { FILETIME ft; GetSystemTimePreciseAsFileTime(&ft); return (double)(((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull) / 1e7; }
#define LOG(...) do { fprintf(g_out, "[%.3f] ", wall()); fprintf(g_out, __VA_ARGS__); fflush(g_out); } while (0)
#define CHECK(hr, what) do { HRESULT _h = (hr); LOG("  %-48s hr=0x%08lx %s\n", what, (unsigned long)_h, FAILED(_h) ? "FAILED" : "ok"); if (FAILED(_h)) return 1; } while (0)

typedef struct { volatile LONG ready; DWORD win_pid; uint64_t nt_handle; uint64_t kmt_handle; uint64_t unix_pid; } Shared;

static Shared *map_shared(const char *path) {
    HANDLE f = CreateFileW(dos(path), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    if (sz.QuadPart < 4096) { LARGE_INTEGER p; p.QuadPart = 4096; SetFilePointerEx(f, p, NULL, FILE_BEGIN); SetEndOfFile(f); }
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READWRITE, 0, 0, NULL);
    return m ? MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
}

static int check_pixel(const uint8_t *p, const char *who) {
    int ok = abs(p[0] - 64) <= 1 && abs(p[1] - 128) <= 1 && abs(p[2] - 191) <= 1 && p[3] == 255;
    LOG("  %s pixel = (%u,%u,%u,%u) expected (64,128,191,255) -> %s\n", who, p[0], p[1], p[2], p[3], ok ? "ZERO-COPY SHARE WORKS" : "WRONG CONTENT");
    return ok ? 0 : 1;
}

static int produce11(const char *path, int secs) {
    Shared *s = map_shared(path);
    if (!s) { LOG("map failed\n"); return 1; }
    s->ready = 0;
    ID3D11Device *dev; ID3D11DeviceContext *ctx; D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1, got;
    CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, &got, &ctx), "D3D11CreateDevice(11_1)");
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = 256; td.Height = 256; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ID3D11Texture2D *tex = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex);
    LOG("  %-48s hr=0x%08lx\n", "CreateTexture2D(MISC_SHARED|SHARED_NTHANDLE)", (unsigned long)hr);
    if (FAILED(hr)) { td.MiscFlags = D3D11_RESOURCE_MISC_SHARED; CHECK(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex), "CreateTexture2D(MISC_SHARED only)"); }
    ID3D11RenderTargetView *rtv; CHECK(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv), "CreateRenderTargetView");
    float col[4] = { 64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1.0f };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, col);
    /* wait for the GPU */
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 }; ID3D11Query *q; ID3D11Device_CreateQuery(dev, &qd, &q);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q);
    BOOL done = FALSE; while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q, &done, sizeof done, 0) != S_OK) Sleep(1);
    IDXGIResource1 *r1; HANDLE nt = NULL, kmt = NULL;
    CHECK(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&r1), "QI IDXGIResource1");
    hr = IDXGIResource1_CreateSharedHandle(r1, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, L"PhantomRingSharedTex", &nt);
    LOG("  %-48s hr=0x%08lx handle=%p\n", "IDXGIResource1::CreateSharedHandle(named)", (unsigned long)hr, nt);
    if (FAILED(hr)) {
        hr = IDXGIResource1_CreateSharedHandle(r1, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &nt);
        LOG("  %-48s hr=0x%08lx handle=%p\n", "IDXGIResource1::CreateSharedHandle(unnamed)", (unsigned long)hr, nt);
    }
    hr = IDXGIResource_GetSharedHandle((IDXGIResource *)r1, &kmt);
    LOG("  %-48s hr=0x%08lx handle=%p\n", "IDXGIResource::GetSharedHandle (KMT)", (unsigned long)hr, kmt);
    s->win_pid = GetCurrentProcessId(); s->nt_handle = (uint64_t)(uintptr_t)nt; s->kmt_handle = (uint64_t)(uintptr_t)kmt;
    InterlockedExchange(&s->ready, 1);
    LOG("  producer ready (win pid %lu), holding for %d s\n", GetCurrentProcessId(), secs);
    for (int i = 0; i < secs; i++) { Sleep(1000); if (i % 5 == 4) LOG("  producer alive t=%ds\n", i + 1); }
    LOG("  producer exiting normally\n");
    s->ready = 0;
    return 0;
}

static HANDLE dup_from_producer(Shared *s) {
    HANDLE proc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, s->win_pid), h = NULL;
    if (!proc) { LOG("  OpenProcess(win pid %lu) FAILED err=%lu (different wineserver?)\n", s->win_pid, GetLastError()); return NULL; }
    if (!DuplicateHandle(proc, (HANDLE)(uintptr_t)s->nt_handle, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        LOG("  DuplicateHandle FAILED err=%lu\n", GetLastError()); h = NULL;
    } else LOG("  DuplicateHandle(producer %p) -> %p ok\n", (void *)(uintptr_t)s->nt_handle, h);
    CloseHandle(proc);
    return h;
}

static int wait_ready(Shared *s) {
    for (int i = 0; i < 300 && !s->ready; i++) Sleep(100);
    if (!s->ready) { LOG("  producer not ready\n"); return 0; }
    LOG("  producer: win pid=%lu nt=%p kmt=%p (my win pid=%lu)\n", s->win_pid, (void *)(uintptr_t)s->nt_handle, (void *)(uintptr_t)s->kmt_handle, GetCurrentProcessId());
    return 1;
}

static int consume11(const char *path) {
    Shared *s = map_shared(path);
    if (!s || !wait_ready(s)) return 1;
    ID3D11Device *dev; ID3D11DeviceContext *ctx; D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1, got;
    CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, &got, &ctx), "D3D11CreateDevice(11_1)");
    ID3D11Device1 *dev1; CHECK(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, (void **)&dev1), "QI ID3D11Device1");
    int rc = 1;
    for (int pass = 0; pass < 3; pass++) {
        ID3D11Texture2D *t = NULL; HRESULT hr; const char *what;
        if (pass == 0) { HANDLE h = dup_from_producer(s); what = "OpenSharedResource1(dup NT handle)"; hr = h ? ID3D11Device1_OpenSharedResource1(dev1, h, &IID_ID3D11Texture2D, (void **)&t) : E_HANDLE; }
        else if (pass == 1) { what = "OpenSharedResourceByName(PhantomRingSharedTex)"; hr = ID3D11Device1_OpenSharedResourceByName(dev1, L"PhantomRingSharedTex", DXGI_SHARED_RESOURCE_READ, &IID_ID3D11Texture2D, (void **)&t); }
        else { what = "OpenSharedResource(KMT handle)"; hr = ID3D11Device_OpenSharedResource(dev, (HANDLE)(uintptr_t)s->kmt_handle, &IID_ID3D11Texture2D, (void **)&t); }
        LOG("  %-48s hr=0x%08lx\n", what, (unsigned long)hr);
        if (FAILED(hr) || !t) continue;
        D3D11_TEXTURE2D_DESC td; ID3D11Texture2D_GetDesc(t, &td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        ID3D11Texture2D *st; ID3D11Device_CreateTexture2D(dev, &td, NULL, &st);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)t);
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &ms))) {
            if (check_pixel((uint8_t *)ms.pData + 128 * ms.RowPitch + 128 * 4, what) == 0) rc = 0;
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        }
    }
    return rc;
}

static int consume12(const char *path) {
    Shared *s = map_shared(path);
    if (!s || !wait_ready(s)) return 1;
    ID3D12Device *dev;
    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev), "D3D12CreateDevice");
    ID3D12Resource *res = NULL; HRESULT hr;
    HANDLE h = dup_from_producer(s);
    hr = h ? ID3D12Device_OpenSharedHandle(dev, h, &IID_ID3D12Resource, (void **)&res) : E_HANDLE;
    LOG("  %-48s hr=0x%08lx\n", "ID3D12Device::OpenSharedHandle(dup NT handle)", (unsigned long)hr);
    if (!res) {
        HANDLE hn = NULL;
        hr = ID3D12Device_OpenSharedHandleByName(dev, L"PhantomRingSharedTex", GENERIC_ALL, &hn);
        LOG("  %-48s hr=0x%08lx handle=%p\n", "ID3D12Device::OpenSharedHandleByName", (unsigned long)hr, hn);
        if (SUCCEEDED(hr)) { hr = ID3D12Device_OpenSharedHandle(dev, hn, &IID_ID3D12Resource, (void **)&res); LOG("  %-48s hr=0x%08lx\n", "OpenSharedHandle(by-name handle)", (unsigned long)hr); }
    }
    if (!res) return 1;
    /* readback: copy texture -> readback buffer */
    D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(res);
    LOG("  opened D3D12 resource: %llux%u fmt=%d dim=%d\n", (unsigned long long)rd.Width, rd.Height, rd.Format, rd.Dimension);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rowsz, total;
    ID3D12Device_GetCopyableFootprints(dev, &rd, 0, 1, 0, &fp, &rows, &rowsz, &total);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    D3D12_RESOURCE_DESC bd = {0}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *rb; CHECK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb), "CreateCommittedResource(readback)");
    D3D12_COMMAND_QUEUE_DESC qd = {0}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue; CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue), "CreateCommandQueue");
    ID3D12CommandAllocator *alloc; CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc), "CreateCommandAllocator");
    ID3D12GraphicsCommandList *cl; CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl), "CreateCommandList");
    D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    src.pResource = res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    ID3D12GraphicsCommandList_Close(cl);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl);
    ID3D12Fence *fence; CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence), "CreateFence");
    ID3D12CommandQueue_Signal(queue, fence, 1);
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL); ID3D12Fence_SetEventOnCompletion(fence, 1, ev); WaitForSingleObject(ev, 5000);
    void *p; D3D12_RANGE r = { 0, (SIZE_T)total };
    CHECK(ID3D12Resource_Map(rb, 0, &r, &p), "Map(readback)");
    return check_pixel((uint8_t *)p + fp.Offset + 128 * fp.Footprint.RowPitch + 128 * 4, "D3D12 consumer");
}


/* D3D12 OpenExistingHeapFromAddress on a view of the /dev/shm file mapping: if it
 * works, the host can let the GPU read the guest's frame straight out of shared
 * memory (no CPU memcpy on the host side). */
static int heap12(const char *path) {
    const SIZE_T sz = 16u << 20;                     /* 16 MiB, multiple of 64 KiB */
    HANDLE f = CreateFileW(dos(path), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { LOG("open failed\n"); return 1; }
    LARGE_INTEGER p; p.QuadPart = sz; SetFilePointerEx(f, p, NULL, FILE_BEGIN); SetEndOfFile(f);
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READWRITE, 0, 0, NULL);
    uint8_t *view = m ? MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sz) : NULL;
    if (!view) { LOG("map failed %lu\n", GetLastError()); return 1; }
    for (SIZE_T i = 0; i < sz; i += 4) *(uint32_t *)(view + i) = 0xC0DE0000u | (uint32_t)((i >> 2) & 0xffff);
    LOG("  file view at %p (%zu bytes), pattern written\n", view, sz);
    ID3D12Device *dev; CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev), "D3D12CreateDevice");
    ID3D12Device3 *dev3; CHECK(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device3, (void **)&dev3), "QI ID3D12Device3");
    ID3D12Heap *heap = NULL;
    HRESULT hr = ID3D12Device3_OpenExistingHeapFromAddress(dev3, view, &IID_ID3D12Heap, (void **)&heap);
    LOG("  %-48s hr=0x%08lx\n", "OpenExistingHeapFromAddress(file-mapping view)", (unsigned long)hr);
    void *priv = VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ID3D12Heap *heap2 = NULL;
    HRESULT hr2 = ID3D12Device3_OpenExistingHeapFromAddress(dev3, priv, &IID_ID3D12Heap, (void **)&heap2);
    LOG("  %-48s hr=0x%08lx (control: private VirtualAlloc)\n", "OpenExistingHeapFromAddress(VirtualAlloc)", (unsigned long)hr2);
    if (FAILED(hr) || !heap) return 1;
    D3D12_RESOURCE_DESC bd = {0}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = sz; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    ID3D12Resource *src; CHECK(ID3D12Device_CreatePlacedResource(dev, heap, 0, &bd, D3D12_RESOURCE_STATE_COPY_SOURCE, NULL, &IID_ID3D12Resource, (void **)&src), "CreatePlacedResource(on shm heap)");
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES hpd = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    D3D12_HEAP_PROPERTIES hpr = { D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    ID3D12Resource *vram, *rb;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &hpd, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&vram), "CreateCommittedResource(DEFAULT)");
    CHECK(ID3D12Device_CreateCommittedResource(dev, &hpr, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb), "CreateCommittedResource(READBACK)");
    D3D12_COMMAND_QUEUE_DESC qd = {0}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue; ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
    ID3D12CommandAllocator *alloc; ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
    ID3D12GraphicsCommandList *cl; ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl);
    ID3D12Fence *fence; ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    /* time: shm-heap -> VRAM copy of the whole 16 MiB (what the host would do per frame) */
    LARGE_INTEGER fq, t0, t1; QueryPerformanceFrequency(&fq);
    double best = 1e9;
    for (int it = 1; it <= 20; it++) {
        ID3D12CommandAllocator_Reset(alloc); ID3D12GraphicsCommandList_Reset(cl, alloc, NULL);
        D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = vram;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (it > 1) { b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST; ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b); }
        ID3D12GraphicsCommandList_CopyBufferRegion(cl, vram, 0, src, 0, sz);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
        if (it == 20) ID3D12GraphicsCommandList_CopyBufferRegion(cl, rb, 0, vram, 0, sz);
        ID3D12GraphicsCommandList_Close(cl);
        QueryPerformanceCounter(&t0);
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl);
        ID3D12CommandQueue_Signal(queue, fence, (UINT64)it);
        ID3D12Fence_SetEventOnCompletion(fence, (UINT64)it, ev); WaitForSingleObject(ev, 5000);
        QueryPerformanceCounter(&t1);
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1e3 / (double)fq.QuadPart;
        if (it < 20 && ms < best) best = ms;
    }
    LOG("  shm-heap -> VRAM CopyBufferRegion of %zu bytes: best submit-to-complete %.3f ms (%.1f GB/s)\n", sz, best, sz / best / 1e6);
    uint32_t *r; D3D12_RANGE rr = { 0, sz };
    CHECK(ID3D12Resource_Map(rb, 0, &rr, (void **)&r), "Map(readback)");
    size_t bad = 0; for (size_t i = 0; i < sz / 4; i++) if (r[i] != (0xC0DE0000u | (uint32_t)(i & 0xffff))) bad++;
    LOG("  verify VRAM copy of shm contents: mismatched words=%zu -> %s\n", bad, bad ? "WRONG" : "GPU READ SHM DIRECTLY (zero host memcpy)");
    ID3D12Resource_Unmap(rb, 0, NULL);
    /* reverse direction (what the guest would do): GPU writes VRAM -> shm heap. Fill VRAM from an
     * UPLOAD buffer with a new pattern 0xBEEFxxxx, then copy VRAM -> shm placed buffer, time it. */
    D3D12_HEAP_PROPERTIES hpu = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    ID3D12Resource *up; CHECK(ID3D12Device_CreateCommittedResource(dev, &hpu, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&up), "CreateCommittedResource(UPLOAD)");
    uint32_t *u; CHECK(ID3D12Resource_Map(up, 0, NULL, (void **)&u), "Map(upload)");
    for (size_t i = 0; i < sz / 4; i++) u[i] = 0xBEEF0000u | (uint32_t)(i & 0xffff);
    ID3D12Resource_Unmap(up, 0, NULL);
    best = 1e9;
    for (int it = 21; it <= 40; it++) {
        ID3D12CommandAllocator_Reset(alloc); ID3D12GraphicsCommandList_Reset(cl, alloc, NULL);
        D3D12_RESOURCE_BARRIER b[2] = {0};
        b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition.Subresource = b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (it == 21) {
            /* vram: COPY_SOURCE -> COPY_DEST, fill from upload, back to COPY_SOURCE; shm buffer: COPY_SOURCE -> COPY_DEST */
            b[0].Transition.pResource = vram; b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, b);
            ID3D12GraphicsCommandList_CopyBufferRegion(cl, vram, 0, up, 0, sz);
            b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b[1].Transition.pResource = src; b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            ID3D12GraphicsCommandList_ResourceBarrier(cl, 2, b);
            ID3D12GraphicsCommandList_Close(cl);
            ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl);
            ID3D12CommandQueue_Signal(queue, fence, (UINT64)it);
            ID3D12Fence_SetEventOnCompletion(fence, (UINT64)it, ev); WaitForSingleObject(ev, 5000);
            continue;
        }
        ID3D12GraphicsCommandList_CopyBufferRegion(cl, src, 0, vram, 0, sz);
        ID3D12GraphicsCommandList_Close(cl);
        QueryPerformanceCounter(&t0);
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl);
        ID3D12CommandQueue_Signal(queue, fence, (UINT64)it);
        ID3D12Fence_SetEventOnCompletion(fence, (UINT64)it, ev); WaitForSingleObject(ev, 5000);
        QueryPerformanceCounter(&t1);
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1e3 / (double)fq.QuadPart;
        if (ms < best) best = ms;
    }
    size_t bad2 = 0; for (size_t i = 0; i < sz / 4; i++) if (((uint32_t *)view)[i] != (0xBEEF0000u | (uint32_t)(i & 0xffff))) bad2++;
    LOG("  VRAM -> shm-heap CopyBufferRegion of %zu bytes: best submit-to-complete %.3f ms (%.1f GB/s); CPU sees new data in the file view: mismatched=%zu -> %s\n",
        sz, best, sz / best / 1e6, bad2, bad2 ? "WRONG" : "GPU WROTE SHM DIRECTLY (zero guest memcpy)");
    return (bad || bad2) ? 1 : 0;
}

int main(int argc, char **argv) {
    g_out = stdout;
    const char *o = getenv("GPUSHARE_OUT");
    if (o && *o) { FILE *f = _wfopen(dos(o), L"a"); if (f) g_out = f; }
    if (argc < 3) { LOG("usage: gpushare produce11|consume11|consume12 <shm> [secs]\n"); return 2; }
    LOG("---- gpushare %s %s (win pid %lu)\n", argv[1], argv[2], GetCurrentProcessId());
    if (!strcmp(argv[1], "produce11")) return produce11(argv[2], argc > 3 ? atoi(argv[3]) : 20);
    if (!strcmp(argv[1], "consume11")) return consume11(argv[2]);
    if (!strcmp(argv[1], "consume12")) return consume12(argv[2]);
    if (!strcmp(argv[1], "heap12")) return heap12(argv[2]);
    return 2;
}
