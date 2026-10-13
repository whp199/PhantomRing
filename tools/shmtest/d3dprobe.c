/*
 * d3dprobe.exe - does a D3D11 (DXVK under Proton) app keep rendering when its
 * window is hidden / minimized / offscreen, what does Present return, and what
 * does a per-frame GPU->CPU readback (and CPU->GPU upload) of a WxH RGBA8 frame
 * cost? Used to size the "guest renders, host composites" path of PhantomRing.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -std=gnu17 -Wall -static -mwindows -o d3dprobe.exe d3dprobe.c -ld3d11 -ldxgi -ldxguid -luuid
 * Usage: d3dprobe.exe <hidden|minimized|offscreen|visible|active|noswap> <seconds> <width> <height> <flip|blit> [vsync0|vsync1]
 * Output: appended to the file named by $D3DPROBE_OUT (unix or windows path), else stdout.
 *
 * Per frame: ClearRenderTargetView(offscreen RT, colour encoding the frame number)
 *            -> CopyResource(RT -> staging[f%3]) -> Map(staging[f-2]) + memcpy whole frame + verify colour
 *            -> CopyResource(RT -> backbuffer) -> Present.
 * Every 8th frame additionally times an upload: Map(WRITE_DISCARD dynamic tex) + memcpy + CopyResource.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LARGE_INTEGER g_freq;
static double now_s(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)g_freq.QuadPart; }

typedef WCHAR *(CDECL *wine_get_dos_file_name_t)(const char *);
static FILE *g_out;

#define CHECK(hr, what) do { HRESULT _h = (hr); if (FAILED(_h)) { fprintf(g_out, "FAIL %s hr=0x%08lx\n", what, (unsigned long)_h); fflush(g_out); return 1; } } while (0)

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int main(int argc, char **argv) {
    QueryPerformanceFrequency(&g_freq);
    g_out = stdout;
    const char *outp = getenv("D3DPROBE_OUT");
    if (outp && *outp) {
        WCHAR wbuf[1024];
        if (outp[0] == '/') {
            wine_get_dos_file_name_t f = (wine_get_dos_file_name_t)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name");
            WCHAR *w = f ? f(outp) : NULL;
            if (w) { wcsncpy(wbuf, w, 1023); wbuf[1023] = 0; } else MultiByteToWideChar(CP_UTF8, 0, outp, -1, wbuf, 1024);
        } else MultiByteToWideChar(CP_UTF8, 0, outp, -1, wbuf, 1024);
        FILE *f = _wfopen(wbuf, L"a");
        if (f) g_out = f;
    }
    setvbuf(g_out, NULL, _IONBF, 0);
    if (argc < 6) { fprintf(g_out, "usage: d3dprobe <hidden|minimized|offscreen|visible|noswap> <seconds> <w> <h> <flip|blit> [vsync0|vsync1]\n"); return 2; }
    const char *mode = argv[1];
    double secs = atof(argv[2]);
    UINT W = (UINT)atoi(argv[3]), H = (UINT)atoi(argv[4]);
    int flip = !strcmp(argv[5], "flip");
    UINT sync = (argc > 6 && !strcmp(argv[6], "vsync1")) ? 1 : 0;
    int noswap = !strcmp(mode, "noswap");
    fprintf(g_out, "---- d3dprobe mode=%s secs=%.1f %ux%u swap=%s sync=%u\n", mode, secs, W, H, flip ? "FLIP_DISCARD" : "DISCARD(blit)", sync);

    HWND hwnd = NULL;
    if (!noswap) {
        WNDCLASSW wc = {0}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"d3dprobe";
        RegisterClassW(&wc);
        int x = 0, y = 0;
        if (!strcmp(mode, "offscreen")) { x = -6000; y = -6000; }
        int active = !strcmp(mode, "active"); /* only sensible inside a nested/headless gamescope */
        hwnd = CreateWindowExW(active ? 0 : (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE), L"d3dprobe", L"d3dprobe", WS_OVERLAPPEDWINDOW,
                               x, y, (int)W, (int)H, NULL, NULL, wc.hInstance, NULL);
        if (!hwnd) { fprintf(g_out, "FAIL CreateWindowExW %lu\n", GetLastError()); return 1; }
        if (!strcmp(mode, "minimized")) ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
        else if (!strcmp(mode, "offscreen") || !strcmp(mode, "visible")) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        else if (active) { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
        /* hidden: never shown */
        RECT r; GetWindowRect(hwnd, &r);
        fprintf(g_out, "window: visible=%d iconic=%d rect=(%ld,%ld)-(%ld,%ld) foreground=%d\n", IsWindowVisible(hwnd), IsIconic(hwnd),
                r.left, r.top, r.right, r.bottom, GetForegroundWindow() == hwnd);
    }

    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; IDXGISwapChain *sc = NULL;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
    if (noswap) {
        CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, &got, &ctx), "D3D11CreateDevice");
    } else {
        DXGI_SWAP_CHAIN_DESC sd = {0};
        sd.BufferDesc.Width = W; sd.BufferDesc.Height = H; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = flip ? 2 : 1; sd.OutputWindow = hwnd; sd.Windowed = TRUE;
        sd.SwapEffect = flip ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
        CHECK(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &sc, &dev, &got, &ctx), "D3D11CreateDeviceAndSwapChain");
    }
    { IDXGIDevice *dd; IDXGIAdapter *ad; DXGI_ADAPTER_DESC adesc;
      if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dd)) && SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad)) && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &adesc)))
          fprintf(g_out, "adapter: %ls\n", adesc.Description); }

    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *rt; CHECK(ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt), "CreateTexture2D(rt)");
    ID3D11RenderTargetView *rtv; CHECK(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv), "CreateRTV");
    ID3D11Texture2D *gpucopy; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; CHECK(ID3D11Device_CreateTexture2D(dev, &td, NULL, &gpucopy), "CreateTexture2D(gpucopy)");
    D3D11_TEXTURE2D_DESC sdsc = td; sdsc.Usage = D3D11_USAGE_STAGING; sdsc.BindFlags = 0; sdsc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *stg[3];
    for (int i = 0; i < 3; i++) CHECK(ID3D11Device_CreateTexture2D(dev, &sdsc, NULL, &stg[i]), "CreateTexture2D(staging)");
    D3D11_TEXTURE2D_DESC ddsc = td; ddsc.Usage = D3D11_USAGE_DYNAMIC; ddsc.BindFlags = D3D11_BIND_SHADER_RESOURCE; ddsc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D *dyn; CHECK(ID3D11Device_CreateTexture2D(dev, &ddsc, NULL, &dyn), "CreateTexture2D(dynamic)");
    ID3D11Texture2D *bb = NULL;
    if (sc) CHECK(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb), "GetBuffer");

    size_t frame_bytes = (size_t)W * H * 4;
    uint8_t *cpu = VirtualAlloc(NULL, frame_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    memset(cpu, 0x33, frame_bytes);

    long frames = 0, ok_present = 0, occl_present = 0, other_present = 0, verified = 0, mismatched = 0, uploads = 0;
    HRESULT last_other = S_OK;
    double t_present = 0, t_present_max = 0, t_map = 0, t_map_max = 0, t_copy = 0, t_upload = 0, t_upload_max = 0;
    double t0 = now_s(), tlast = t0;
    long frames_last = 0;
    while (now_s() - t0 < secs) {
        MSG msg; while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        long f = frames;
        float col[4] = { (float)(f & 0xff) / 255.0f, (float)((f >> 8) & 0xff) / 255.0f, 0.5f, 1.0f };
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, col);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stg[f % 3], (ID3D11Resource *)rt);
        if (f >= 2) {
            D3D11_MAPPED_SUBRESOURCE ms;
            double a = now_s();
            HRESULT hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg[(f - 2) % 3], 0, D3D11_MAP_READ, 0, &ms);
            double b = now_s();
            if (SUCCEEDED(hr)) {
                for (UINT y = 0; y < H; y++) memcpy(cpu + (size_t)y * W * 4, (uint8_t *)ms.pData + (size_t)y * ms.RowPitch, (size_t)W * 4);
                double c = now_s();
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg[(f - 2) % 3], 0);
                long g = f - 2;
                uint8_t er = (uint8_t)((g & 0xff) * 255 / 255), eg = (uint8_t)(((g >> 8) & 0xff));
                uint8_t *px = cpu + ((size_t)(H / 2) * W + W / 2) * 4;
                if (abs((int)px[0] - er) <= 1 && abs((int)px[1] - eg) <= 1) verified++; else mismatched++;
                t_map += b - a; if (b - a > t_map_max) t_map_max = b - a; t_copy += c - b;
            }
        }
        if ((f & 7) == 0) {
            double a = now_s();
            D3D11_MAPPED_SUBRESOURCE ms;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)dyn, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                for (UINT y = 0; y < H; y++) memcpy((uint8_t *)ms.pData + (size_t)y * ms.RowPitch, cpu + (size_t)y * W * 4, (size_t)W * 4);
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)dyn, 0);
                ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)gpucopy, (ID3D11Resource *)dyn);
                double b = now_s(); t_upload += b - a; if (b - a > t_upload_max) t_upload_max = b - a; uploads++;
            }
        }
        if (sc) {
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)bb, (ID3D11Resource *)rt);
            double a = now_s();
            HRESULT hr = IDXGISwapChain_Present(sc, sync, 0);
            double d = now_s() - a; t_present += d; if (d > t_present_max) t_present_max = d;
            if (hr == S_OK) ok_present++; else if (hr == DXGI_STATUS_OCCLUDED) occl_present++; else { other_present++; last_other = hr; }
        } else {
            ID3D11DeviceContext_Flush(ctx);
        }
        frames++;
        if (now_s() - tlast >= 1.0) {
            fprintf(g_out, "  t=%.1fs fps=%.0f present ok=%ld occluded=%ld other=%ld verified=%ld mismatched=%ld foreground=%d\n", now_s() - t0,
                    (frames - frames_last) / (now_s() - tlast), ok_present, occl_present, other_present, verified, mismatched,
                    hwnd ? GetForegroundWindow() == hwnd : -1);
            tlast = now_s(); frames_last = frames;
        }
    }
    double T = now_s() - t0;
    long maps = verified + mismatched;
    fprintf(g_out, "RESULT mode=%s swap=%s %ux%u: frames=%ld in %.2fs (%.0f fps) | Present: ok=%ld occluded=%ld other=%ld (last 0x%08lx) avg=%.3f ms max=%.3f ms\n",
            mode, noswap ? "none" : (flip ? "flip" : "blit"), W, H, frames, T, frames / T, ok_present, occl_present, other_present,
            (unsigned long)last_other, frames ? t_present / frames * 1e3 : 0, t_present_max * 1e3);
    fprintf(g_out, "RESULT readback (2-frame-old staging): Map avg=%.3f ms max=%.3f ms, memcpy %zu B avg=%.3f ms; verified=%ld mismatched=%ld -> %s\n",
            maps ? t_map / maps * 1e3 : 0, t_map_max * 1e3, frame_bytes, maps ? t_copy / maps * 1e3 : 0, verified, mismatched,
            (verified > 0 && mismatched == 0) ? "GPU WORK EXECUTES" : "NO/WRONG GPU OUTPUT");
    fprintf(g_out, "RESULT upload (Map WRITE_DISCARD + memcpy + CopyResource, CPU time): avg=%.3f ms max=%.3f ms over %ld uploads\n",
            uploads ? t_upload / uploads * 1e3 : 0, t_upload_max * 1e3, uploads);
    if (hwnd) DestroyWindow(hwnd);
    return 0;
}
