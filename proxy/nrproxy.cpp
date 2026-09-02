// A version.dll that runs DLSS Neural Rendering without ReShade.
//
// Step one of three. This build proves the three things that decide whether the
// rest is possible, and deliberately does nothing else -- it never runs the
// network and never touches a pixel:
//
//   1. the proxy loads and forwards version.dll's exports without breaking the game
//   2. the D3D12 device and the direct command queue can both be captured
//   3. the DLSSNR snippet accepts feature creation from us
//
// (3) is what shapes the design. The snippet refuses any caller whose module path
// lacks "nvngx.dll" -- a plain substring test on the calling module -- which is
// why the ReShade add-on is named nvngx.dll.addon64. A version.dll asking
// directly gets 0xBAD00002. So the NGX calls go through a satellite whose file
// name carries the substring, and everything else stays here.
//
// The queue matters because the exposure and histogram readbacks are fenced
// against it. Without one, half the automatic calibration cannot run -- and it is
// exactly what ReShade was handing over for free.

#include <windows.h>
#include <winternl.h>
#include <d3d12.h>
#include <cstdint>
#include <MinHook.h>

// ------------------------------------------------------------------- log ---
// Raw file calls, no CRT: some of this runs under the loader lock.

static wchar_t g_log[MAX_PATH];

static void log_init() {
    GetModuleFileNameW(nullptr, g_log, MAX_PATH);
    int last = -1;
    for (int i = 0; g_log[i] != 0; ++i) if (g_log[i] == L'\\') last = i;
    static const wchar_t kName[] = L"\\neural-proxy.log";
    int i = 0;
    for (; kName[i] != 0; ++i) g_log[last + i] = kName[i];
    g_log[last + i] = 0;
    DeleteFileW(g_log);
}

static void log_line(const char *s) {
    HANDLE h = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    SetFilePointer(h, 0, nullptr, FILE_END);
    size_t n = 0;
    while (s[n] != 0) ++n;
    WriteFile(h, s, (DWORD)n, &w, nullptr);
    WriteFile(h, "\r\n", 2, &w, nullptr);
    CloseHandle(h);
}

// The add-on logs through reshade::log::message; the shim routes it here so both
// builds write the same lines, just to different files.
void proxy_log_line(const char *s) { log_line(s); }

static void log_num(const char *prefix, unsigned long long v, int hex) {
    char buf[256];
    int i = 0;
    while (prefix[i] != 0 && i < 200) { buf[i] = prefix[i]; ++i; }
    if (hex) { buf[i++] = '0'; buf[i++] = 'x'; }
    char d[32];
    int n = 0;
    const unsigned base = hex ? 16u : 10u;
    if (v == 0) d[n++] = '0';
    while (v > 0) {
        unsigned r = (unsigned)(v % base);
        d[n++] = (char)(r < 10 ? '0' + r : 'a' + (r - 10));
        v /= base;
    }
    while (n > 0) buf[i++] = d[--n];
    buf[i] = 0;
    log_line(buf);
}

static void log_wide(const char *prefix, const wchar_t *w) {
    char buf[512];
    int i = 0;
    while (prefix[i] != 0 && i < 200) { buf[i] = prefix[i]; ++i; }
    for (int k = 0; w[k] != 0 && i < 500; ++k, ++i)
        buf[i] = (w[k] >= 32 && w[k] < 127) ? (char)w[k] : '?';
    buf[i] = 0;
    log_line(buf);
}

// --------------------------------------------------------------- capture ---
//
// The device comes free later: every evaluate hands us a command list, and a
// command list knows its device. The queue does not -- nothing in the NGX path
// mentions one. D3D12CreateDevice is exported and runs before anything else, so
// hook that, then hook the device's own CreateCommandQueue through its vtable
// and keep the first DIRECT queue. Slot 8 is CreateCommandQueue and is fixed by
// the COM contract, not by a version.

static ID3D12Device       *g_device = nullptr;
static ID3D12CommandQueue *g_queue  = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *PFN_CCQ)(ID3D12Device *,
        const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);
static PFN_CCQ g_orig_ccq = nullptr;

static HRESULT STDMETHODCALLTYPE hk_ccq(ID3D12Device *self,
        const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **out)
{
    HRESULT hr = g_orig_ccq(self, desc, riid, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && desc != nullptr &&
        desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT && g_queue == nullptr)
    {
        g_queue  = reinterpret_cast<ID3D12CommandQueue *>(*out);
        g_device = self;
        log_num("direct command queue captured: ", (unsigned long long)(uintptr_t)g_queue, 1);
        log_num("  device: ", (unsigned long long)(uintptr_t)g_device, 1);
    }
    return hr;
}

typedef HRESULT (WINAPI *PFN_D3D12CD)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
static PFN_D3D12CD g_orig_cd = nullptr;

static HRESULT WINAPI hk_cd(IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void **out)
{
    HRESULT hr = g_orig_cd(adapter, fl, riid, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && g_orig_ccq == nullptr) {
        void **vt = *reinterpret_cast<void ***>(*out);
        log_line("D3D12 device created; hooking CreateCommandQueue");
        if (MH_CreateHook(vt[8], reinterpret_cast<void *>(&hk_ccq),
                          reinterpret_cast<void **>(&g_orig_ccq)) == MH_OK &&
            MH_EnableHook(vt[8]) == MH_OK) {
            log_line("  hooked");
        } else {
            log_line("  FAILED");
            g_orig_ccq = nullptr;
        }
    }
    return hr;
}

// ------------------------------------------------------------- hand-over ---
//
// This file installs no hook on the NGX evaluate, and that is deliberate. The
// add-on installs its own, and two MinHook instances detouring one address means
// the second writes over the first's trampoline -- which hangs the process
// somewhere unrelated and looks like anything but what it is.
//
// So the bootstrap waits, on its own thread, until there is a device, a queue and
// an NGX to hand over. The add-on does everything after that.

static HMODULE g_sat = nullptr;
static void (*g_host_device)(void *) = nullptr;
static void (*g_host_queue_fn)(void *) = nullptr;
static bool g_handed_over = false;

static void load_satellite() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    int last = -1;
    for (int i = 0; path[i] != 0; ++i) if (path[i] == L'\\') last = i;
    static const wchar_t kSat[] = L"\\nvngx.dll.nr";
    int i = 0;
    for (; kSat[i] != 0; ++i) path[last + i] = kSat[i];
    path[last + i] = 0;

    log_wide("loading the add-on: ", path);
    g_sat = LoadLibraryW(path);
    if (g_sat == nullptr) { log_num("  LoadLibrary failed, err ", GetLastError(), 0); return; }
    g_host_device   = (void (*)(void *))GetProcAddress(g_sat, "nr_host_device");
    g_host_queue_fn = (void (*)(void *))GetProcAddress(g_sat, "nr_host_queue");
    log_line((g_host_device && g_host_queue_fn) ? "  add-on entry points resolved"
                                                : "  ADD-ON ENTRY POINTS MISSING");
}

static void hand_over_when_ready() {
    for (int i = 0; i < 600 && !g_handed_over; ++i) {      // up to ~60 s, then give up
        if (g_device != nullptr && g_queue != nullptr && g_host_device != nullptr &&
            GetModuleHandleW(L"_nvngx.dll") != nullptr) {
            g_handed_over = true;
            log_line("handing device and queue to the add-on");
            g_host_queue_fn(g_queue);      // queue first: the add-on reads it on its first frame
            g_host_device(g_device);
            return;
        }
        Sleep(100);
    }
    if (!g_handed_over) log_line("gave up waiting for a device and _nvngx.dll");
}
typedef NTSTATUS (NTAPI *PFN_Register)(ULONG, PVOID, PVOID, PVOID *);

static DWORD WINAPI boot(LPVOID) {
    log_line("--- neural-upstream proxy (step 1: bootstrap only) ---");
    if (MH_Initialize() != MH_OK) { log_line("MH_Initialize failed"); return 0; }

    if (HMODULE d3d12 = LoadLibraryW(L"d3d12.dll")) {
        FARPROC p = GetProcAddress(d3d12, "D3D12CreateDevice");
        if (p != nullptr &&
            MH_CreateHook(reinterpret_cast<void *>(p), reinterpret_cast<void *>(&hk_cd),
                          reinterpret_cast<void **>(&g_orig_cd)) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void *>(p)) == MH_OK)
            log_line("hooked D3D12CreateDevice");
        else
            log_line("could not hook D3D12CreateDevice");
    }

    load_satellite();       // here, where the loader lock is not held
    hand_over_when_ready();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        log_init();
        // Never do this work under the loader lock.
        HANDLE t = CreateThread(nullptr, 0, boot, nullptr, 0, nullptr);
        if (t != nullptr) CloseHandle(t);
    }
    return TRUE;
}

// ------------------------------------------------------------ forwarding ---
// Resolved lazily: calling LoadLibrary from DllMain is how proxies deadlock.

static HMODULE g_real = nullptr;

static FARPROC real(const char *name) {
    if (g_real == nullptr) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 16) return nullptr;
        static const wchar_t kTail[] = L"\\version.dll";
        UINT i = 0;
        for (; kTail[i] != 0; ++i) path[n + i] = kTail[i];
        path[n + i] = 0;
        g_real = LoadLibraryW(path);
        if (g_real == nullptr) return nullptr;
    }
    return GetProcAddress(g_real, name);
}

#define FORWARD(ret, name, params, args)                                  \
    extern "C" __declspec(dllexport) ret WINAPI name params {             \
        using fn = ret(WINAPI *) params;                                  \
        auto p = reinterpret_cast<fn>(real(#name));                       \
        return p ? p args : (ret)0;                                       \
    }

FORWARD(BOOL,  GetFileVersionInfoA,       (LPCSTR a, DWORD b, DWORD c, LPVOID d), (a,b,c,d))
FORWARD(BOOL,  GetFileVersionInfoW,       (LPCWSTR a, DWORD b, DWORD c, LPVOID d), (a,b,c,d))
FORWARD(BOOL,  GetFileVersionInfoExA,     (DWORD f, LPCSTR a, DWORD b, DWORD c, LPVOID d), (f,a,b,c,d))
FORWARD(BOOL,  GetFileVersionInfoExW,     (DWORD f, LPCWSTR a, DWORD b, DWORD c, LPVOID d), (f,a,b,c,d))
FORWARD(DWORD, GetFileVersionInfoSizeA,   (LPCSTR a, LPDWORD b), (a,b))
FORWARD(DWORD, GetFileVersionInfoSizeW,   (LPCWSTR a, LPDWORD b), (a,b))
FORWARD(DWORD, GetFileVersionInfoSizeExA, (DWORD f, LPCSTR a, LPDWORD b), (f,a,b))
FORWARD(DWORD, GetFileVersionInfoSizeExW, (DWORD f, LPCWSTR a, LPDWORD b), (f,a,b))
FORWARD(BOOL,  VerQueryValueA,            (LPCVOID a, LPCSTR b, LPVOID *c, PUINT d), (a,b,c,d))
FORWARD(BOOL,  VerQueryValueW,            (LPCVOID a, LPCWSTR b, LPVOID *c, PUINT d), (a,b,c,d))
FORWARD(DWORD, VerFindFileA,              (DWORD a, LPSTR b, LPSTR c, LPSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h), (a,b,c,d,e,f,g,h))
FORWARD(DWORD, VerFindFileW,              (DWORD a, LPWSTR b, LPWSTR c, LPWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h), (a,b,c,d,e,f,g,h))
FORWARD(DWORD, VerInstallFileA,           (DWORD a, LPSTR b, LPSTR c, LPSTR d, LPSTR e, LPSTR f, LPSTR g, PUINT h), (a,b,c,d,e,f,g,h))
FORWARD(DWORD, VerInstallFileW,           (DWORD a, LPWSTR b, LPWSTR c, LPWSTR d, LPWSTR e, LPWSTR f, LPWSTR g, PUINT h), (a,b,c,d,e,f,g,h))
FORWARD(DWORD, VerLanguageNameA,          (DWORD a, LPSTR b, DWORD c), (a,b,c))
FORWARD(DWORD, VerLanguageNameW,          (DWORD a, LPWSTR b, DWORD c), (a,b,c))
FORWARD(BOOL,  GetFileVersionInfoByHandle,(DWORD a, HANDLE b, DWORD c, LPVOID d), (a,b,c,d))
