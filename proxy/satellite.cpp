// nvngx.dll.nr -- the satellite that gets past the snippet's front door.
//
// The DLSSNR snippet decides whether to serve a caller by looking at the module
// path the call came from and testing it for the substring "nvngx.dll". Nothing
// else about the caller matters. A version.dll therefore gets 0xBAD00002 no
// matter what it asks for, and the ReShade add-on works only because it is named
// nvngx.dll.addon64.
//
// So this file exists to have a name. It carries no logic of its own: the proxy
// keeps the state, the settings and the pipeline, and hands the two NGX calls
// through here so that the return address lands in a module the snippet accepts.
//
// Step one asks it a single question -- will the snippet create the feature for
// us at all -- because a "no" here means the whole version.dll route needs a
// different shape, and there is no point writing the rest until that is known.

#include <windows.h>
#include <cstdint>
#include <cstring>

// The snippet's own exports. Signatures kept opaque on purpose: this file has to
// forward calls, not understand them.
// Order and types copied from the add-on, which is the version known to work:
// (app id, writable directory, device, sdk version, optional parameters).
typedef int (__cdecl *PFN_Init)(unsigned long long id, const wchar_t *path, void *dev,
                                int ver, const void *params);
typedef int (__cdecl *PFN_GetParams)(void **out);
typedef int (__cdecl *PFN_Create)(void *cmd, unsigned feature, void *params, void **handle);

static void write_log(const char *prefix, const char *s);
static void probe_log(const char *s) { write_log("    [shim] ", s); }
static void probe_log_raw(const char *s) { write_log(nullptr, s); }

static void write_log(const char *prefix, const char *s) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    int last = -1;
    for (int i = 0; path[i] != 0; ++i) if (path[i] == L'\\') last = i;
    static const wchar_t kName[] = L"\\neural-proxy.log";
    int i = 0;
    for (; kName[i] != 0; ++i) path[last + i] = kName[i];
    path[last + i] = 0;

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    SetFilePointer(h, 0, nullptr, FILE_END);
    size_t n = 0;
    while (s[n] != 0) ++n;
    if (prefix != nullptr) WriteFile(h, prefix, (DWORD)std::strlen(prefix), &w, nullptr);
    WriteFile(h, s, (DWORD)n, &w, nullptr);
    WriteFile(h, "\r\n", 2, &w, nullptr);
    CloseHandle(h);
}

// Returns the snippet's own result code for Init_Ext, or a negative marker if we
// could not get far enough to ask.
//
// Init_Ext is the right question, and the cheapest one. Feature creation needs
// parameters, and parameters come from the NGX *core* (_nvngx.dll), not from the
// snippet -- the snippet exports no allocator at all, which is what the first
// attempt got wrong. But the admission test happens at Init_Ext: that is where
// the snippet looks at who is calling. If it lets us initialise, the gate is open
// and the rest is plumbing we already have working elsewhere.
extern "C" __declspec(dllexport) int nr_probe(void *device) {
    if (device == nullptr) { probe_log("no device yet"); return -1; }

    HMODULE snip = GetModuleHandleW(L"nvngx_dlssnr.dll");
    if (snip == nullptr) snip = LoadLibraryW(L"nvngx_dlssnr.dll");
    if (snip == nullptr) { probe_log("nvngx_dlssnr.dll not present"); return -2; }

    // Confirm the module this call appears to come from really does carry the
    // substring. If the file is ever renamed, this line says so instead of
    // leaving an unexplained 0xBAD00002 behind.
    wchar_t self[MAX_PATH] = L"";
    HMODULE me = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&nr_probe), &me);
    GetModuleFileNameW(me, self, MAX_PATH);
    {
        bool ok = false;
        for (int i = 0; self[i] != 0 && !ok; ++i) {
            static const wchar_t kWant[] = L"nvngx.dll";
            int j = 0;
            while (kWant[j] != 0) {
                wchar_t c = self[i + j];
                if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
                if (c != kWant[j]) break;
                ++j;
            }
            ok = (kWant[j] == 0);
        }
        probe_log(ok ? "own path carries \"nvngx.dll\": the gate should open"
                     : "own path LACKS \"nvngx.dll\": the snippet will refuse us");
    }

    auto init = reinterpret_cast<PFN_Init>(GetProcAddress(snip, "NVSDK_NGX_D3D12_Init_Ext"));
    if (init == nullptr) { probe_log("NVSDK_NGX_D3D12_Init_Ext not exported"); return -3; }

    // Same application id and version the add-on uses, and LOCALAPPDATA for the
    // snippet's own scratch -- it wants somewhere writable and will refuse a path
    // it cannot use.
    wchar_t lp[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"LOCALAPPDATA", lp, MAX_PATH);
    // The same application id the add-on registers with. Guessing one gets a
    // refusal that looks like a gate failure and is not.
    const int r = init(0x0876232Cull, lp[0] ? lp : L".", device, 0x15, nullptr);
    probe_log(r == 1 ? "Init_Ext accepted us: the snippet is serving this module"
                     : "Init_Ext refused; see the code the proxy logs");
    return r;
}


// The add-on is compiled into this same module, so it needs a handle to itself --
// the ReShade build got one from its own DllMain and standalone there was none,
// which is why the log kept saying self=GTA5_Enhanced.exe.
HMODULE g_satellite_module = nullptr;


// The add-on logs through reshade::log::message, which the shim routes here. Both
// binaries write to the same file so the bootstrap and the add-on read as one
// story rather than two.
void proxy_log_line(const char *s) { probe_log_raw(s); }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_satellite_module = (HMODULE)h;
    return TRUE;
}
