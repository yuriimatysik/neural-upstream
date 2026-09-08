// DLSS5 NR Pre-Upscale - stage 1: observe the game's NGX DLSS contract (read-only)
#include <windows.h>
#define ImTextureID ImU64
// The overlay is ReShade's to draw. The standalone proxy compiles this same
// file with NR_STANDALONE and takes its settings from an ini instead, so the
// one block that needs ImGui is the one block excluded.
#ifndef NR_STANDALONE
#include <imgui.h>
#endif
#include <d3d12.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include <reshade.hpp>
#include <MinHook.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>
#include "codec.hlsl.h"
#include "submission_tracker.h"
#include "descriptor_lifetime.h"
#include "feature_registry.h"
#include <atomic>
#include <new>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <mutex>
#include <unordered_map>

extern "C" __declspec(dllexport) const char *NAME        = "DLSS5 NR Pre-Upscale";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Runs DLSS 5 Neural Rendering at render resolution before the game's DLSS upscale.";

static void logf(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    reshade::log::message(reshade::log::level::info, buf);
}

// ID3D12Resource::GetDesc is vtable slot 10 and returns a struct by value.
// Call it manually with the MSVC x64 sret convention so the MinGW ABI can't bite us.
static bool res_desc(ID3D12Resource *res, D3D12_RESOURCE_DESC *out) {
    if (res == nullptr) return false;
    using pfn_t = D3D12_RESOURCE_DESC *(STDMETHODCALLTYPE *)(ID3D12Resource *, D3D12_RESOURCE_DESC *);
    auto **vtbl = *reinterpret_cast<void ***>(res);
    auto fn = reinterpret_cast<pfn_t>(vtbl[10]);
    fn(res, out);
    return true;
}

// MSVC emits same-name virtual overloads in REVERSE declaration order; MinGW does not.
// Call the vtable slot directly instead of trusting C++ overload resolution.
typedef NVSDK_NGX_Result (STDMETHODCALLTYPE *PFN_GetPtr)(const NVSDK_NGX_Parameter *, const char *, void **);
static NVSDK_NGX_Result param_get_slot(const NVSDK_NGX_Parameter *p, unsigned slot,
                                       const char *key, void **out) {
    auto **vtbl = *reinterpret_cast<void ***>(const_cast<NVSDK_NGX_Parameter *>(p));
    return reinterpret_cast<PFN_GetPtr>(vtbl[slot])(p, key, out);
}

static bool looks_like_com(void *pv) {
    if (pv == nullptr || (reinterpret_cast<uintptr_t>(pv) & 7) != 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(pv, &mbi, sizeof mbi) == 0 || mbi.State != MEM_COMMIT) return false;
    void *vt = *reinterpret_cast<void **>(pv);            // vtable pointer
    if (vt == nullptr) return false;
    return VirtualQuery(vt, &mbi, sizeof mbi) != 0 && mbi.State == MEM_COMMIT;
}

// One-shot probe: find which vtable slot is the real Get(name, ID3D12Resource**).
static int g_ptr_slot = -1;
static void probe_slots(const NVSDK_NGX_Parameter *p, const char *key) {
    char line[480]; int off = 0;
    off += std::snprintf(line + off, sizeof line - off, "[NRPRE] probe '%s':", key);
    for (unsigned slot = 8; slot <= 15 && off < 400; ++slot) {
        void *pv = nullptr;
        NVSDK_NGX_Result r = param_get_slot(p, slot, key, &pv);
        bool ok = (r == NVSDK_NGX_Result_Success) && looks_like_com(pv);
        off += std::snprintf(line + off, sizeof line - off, " [%u]%s%s",
                             slot, r == NVSDK_NGX_Result_Success ? "OK" : "--", ok ? "*COM" : "");
        if (ok && g_ptr_slot < 0) g_ptr_slot = (int)slot;
    }
    reshade::log::message(reshade::log::level::info, line);
}

typedef void (STDMETHODCALLTYPE *PFN_SetU)(NVSDK_NGX_Parameter *, const char *, unsigned);
static void pset_u(NVSDK_NGX_Parameter *p, const char *k, unsigned v) {
    auto **vt = *reinterpret_cast<void ***>(p);          // MSVC slot 4 = Set(name, unsigned int)
    reinterpret_cast<PFN_SetU>(vt[4])(p, k, v);
}

typedef void (STDMETHODCALLTYPE *PFN_SetI)(NVSDK_NGX_Parameter *, const char *, int);
static void pset_i(NVSDK_NGX_Parameter *p, const char *k, int v) {
    auto **vt = *reinterpret_cast<void ***>(p);          // MSVC slot 3 = Set(name, int)
    reinterpret_cast<PFN_SetI>(vt[3])(p, k, v);
}

typedef void (STDMETHODCALLTYPE *PFN_SetF)(NVSDK_NGX_Parameter *, const char *, float);
static void pset_f(NVSDK_NGX_Parameter *p, const char *k, float v) {
    auto **vt = *reinterpret_cast<void ***>(p);          // MSVC slot 6 = Set(name, float)
    reinterpret_cast<PFN_SetF>(vt[6])(p, k, v);
}

static void describe(const NVSDK_NGX_Parameter *p, const char *key, char *out, size_t n) {
    if (g_ptr_slot < 0) { std::snprintf(out, n, "%s=<no slot>", key); return; }
    void *pv = nullptr;
    if (param_get_slot(p, (unsigned)g_ptr_slot, key, &pv) != NVSDK_NGX_Result_Success || !looks_like_com(pv)) {
        std::snprintf(out, n, "%s=<none>", key);
        return;
    }
    D3D12_RESOURCE_DESC d{};
    if (!res_desc(reinterpret_cast<ID3D12Resource *>(pv), &d)) {
        std::snprintf(out, n, "%s=<desc failed>", key); return;
    }
    std::snprintf(out, n, "%s=%llux%u fmt=%d", key,
                  (unsigned long long)d.Width, d.Height, (int)d.Format);
}

// NVSDK_NGX_Handle is opaque in the public SDK; its first field is the feature id.
struct NgxHandleLayout { unsigned int Id; };
static unsigned feat_id(const NVSDK_NGX_Handle *h) {
    return h ? reinterpret_cast<const NgxHandleLayout *>(h)->Id : 0u;
}

static void setup_nr(unsigned w, unsigned h, ID3D12GraphicsCommandList *cmd);   // defined below
static bool g_nr_enabled = true;    // starts in the optimal mode; F7 toggles
static unsigned g_net_w, g_net_h;
static unsigned g_setup_w = 0, g_setup_h = 0;
static std::recursive_mutex g_state_mutex;
static bool g_resize_pending = false;
static bool g_submission_failed = false;
static bool install_submission_hook(ID3D12CommandQueue *queue);
static bool prepare_recording(ID3D12GraphicsCommandList *cmd);
static void track_recording(ID3D12GraphicsCommandList *cmd);
static void service_pending_cleanup();
static uint64_t recording_id(ID3D12CommandList *list);
static int g_force_reset_frames = 0;
// A Reset=1 from the game is only consumed on frames where NR actually runs.
// At a cadence below 1:1 it can land on a skipped frame and be overwritten by
// the next running frame's rst=0, so the network keeps a stale history --
// that is the broken image after alt-tab. Latch it until NR consumes it.
static bool g_reset_pending = false;
// Diagnostics: after a swapchain reset, log the first N evaluates in full so the
// cadence-2 breakage can be read off the log instead of guessed at.
static int g_diag_frames = 0;
// Every logf() writes to the log file, with a lock and a flush, on the thread that
// called it -- and the F10 experiment showed a stall on the present thread is
// visible on screen. prof_report() additionally Maps and Unmaps a readback buffer
// every present. None of that belongs in a session that is being played rather
// than measured, so it all hangs off one switch.
static bool g_diagnostics = true;
// ReShade can recreate the effect runtime while the game is running. Settings
// are loaded through that callback, but Enabled is also changed live by the
// overlay and F7; re-reading the INI on every callback silently undid either
// action a moment later.
static bool g_enabled_setting_loaded = false;

// ReShade 6.8 exposes its underlying COM object through this optional IID.
// Older versions and native objects fall back to the original interface.
// Private-data recording IDs below also work across wrappers without this IID.
static const GUID kUnwrappedObject =
    {0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
template <typename T> static T *native_ref(T *object) {
    T *native = nullptr;
    if (FAILED(object->QueryInterface(kUnwrappedObject, reinterpret_cast<void **>(&native)))) {
        native = object;
        native->AddRef();
    }
    return native;
}
static bool same_device(ID3D12Device *a, ID3D12Device *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    auto *na = native_ref(a);
    auto *nb = native_ref(b);
    const bool same = na == nb;
    na->Release(); nb->Release();
    return same;
}

#ifdef NR_STANDALONE
static reshade::api::device        g_host_dev;
static reshade::api::command_queue g_host_queue;
static void on_present(reshade::api::command_queue *, reshade::api::swapchain *,
                       const reshade::api::rect *, const reshade::api::rect *,
                       uint32_t, const reshade::api::rect *);
#endif

// ---- perfilador de GPU -------------------------------------------------------
// El fps total no dice donde se va el tiempo: si NR es el 5% del frame, una
// mejora del 20% en la red se ve como 1% en pantalla. Con timestamps de GPU
// alrededor de cada etapa se mide lo que realmente cuesta cada una.
struct Prof {
    ID3D12QueryHeap *heap = nullptr;
    ID3D12Resource  *rb   = nullptr;      // readback: kSlots x kMarks timestamps
    bool  ready = false;
    UINT64 freq = 0;                      // ticks por segundo de la cola
    unsigned slot = 0, frames = 0;
    double sum_enc = 0, sum_net = 0, sum_dec = 0, sum_all = 0;
    double max_all = 0, max_net = 0;   // el promedio esconde justo el pico que rompe el pacing
    double sum_snap = 0, max_snap = 0; // las copias no estaban dentro de la ventana medida
    unsigned samples = 0;
};
static const unsigned kMarks = 6;         // 0 inicio, 1 post-encode, 2 post-red, 3 post-decode,
                                          // 4/5 alrededor de las copias de entrada
static const unsigned kPSlots = 4;        // rotacion para no leer lo que aun se ejecuta
static Prof g_prof;
static bool g_profiling = true;

static bool prof_init(ID3D12Device *dev) {
    if (g_prof.ready) return true;
    if (dev == nullptr) return false;
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kMarks * kPSlots;
    if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_prof.heap)))) return false;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = sizeof(UINT64) * kMarks * kPSlots;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&g_prof.rb)))) {
        if (g_prof.heap) { g_prof.heap->Release(); g_prof.heap = nullptr; }
        return false;
    }
    g_prof.ready = true;
    logf("[NRPRE] perfilador de GPU listo");
    return true;
}

static inline void prof_mark(ID3D12GraphicsCommandList *cmd, unsigned m) {
    if (!g_diagnostics || !g_profiling || !g_prof.ready || cmd == nullptr) return;
    cmd->EndQuery(g_prof.heap, D3D12_QUERY_TYPE_TIMESTAMP, g_prof.slot * kMarks + m);
}

static void prof_resolve(ID3D12GraphicsCommandList *cmd) {
    if (!g_profiling || !g_prof.ready || cmd == nullptr) return;
    cmd->ResolveQueryData(g_prof.heap, D3D12_QUERY_TYPE_TIMESTAMP,
                          g_prof.slot * kMarks, kMarks, g_prof.rb,
                          sizeof(UINT64) * g_prof.slot * kMarks);
    g_prof.slot = (g_prof.slot + 1) % kPSlots;
}

static void prof_report() {
    if (!g_diagnostics) return;   // Map/Unmap per present is not free
    if (!g_profiling || !g_prof.ready || g_prof.freq == 0) return;
    // leer el slot mas viejo: ya se ejecuto seguro
    const unsigned old = (g_prof.slot + 1) % kPSlots;
    D3D12_RANGE r{ sizeof(UINT64) * old * kMarks, sizeof(UINT64) * (old + 1) * kMarks };
    void *p = nullptr;
    if (FAILED(g_prof.rb->Map(0, &r, &p)) || p == nullptr) return;
    const UINT64 *t = reinterpret_cast<const UINT64 *>(p) + old * kMarks;
    static UINT64 last_t0 = 0;
    if (t[3] > t[0] && t[0] != 0 && t[0] != last_t0) {   // no volver a contar el mismo slot
        last_t0 = t[0];
        const double k = 1000.0 / (double)g_prof.freq;   // ticks -> ms
        g_prof.sum_enc += (double)(t[1] - t[0]) * k;
        g_prof.sum_net += (double)(t[2] - t[1]) * k;
        g_prof.sum_dec += (double)(t[3] - t[2]) * k;
        const double all = (double)(t[3] - t[0]) * k;
        const double net = (double)(t[2] - t[1]) * k;
        g_prof.sum_all += all;
        if (all > g_prof.max_all) g_prof.max_all = all;
        if (net > g_prof.max_net) g_prof.max_net = net;
        if (t[5] > t[4] && t[4] != 0) {
            const double sn = (double)(t[5] - t[4]) * k;
            g_prof.sum_snap += sn;
            if (sn > g_prof.max_snap) g_prof.max_snap = sn;
        }
        ++g_prof.samples;
    }
    D3D12_RANGE w{ 0, 0 }; g_prof.rb->Unmap(0, &w);
    if (g_prof.samples >= 120) {
        const double n = (double)g_prof.samples;
        logf("[NRPRE] PERFIL  encode=%.3f  red=%.3f  decode=%.3f  total=%.3f ms  | PICO red=%.3f total=%.3f ms  (n=%u)",
             g_prof.sum_enc / n, g_prof.sum_net / n, g_prof.sum_dec / n, g_prof.sum_all / n,
             g_prof.max_net, g_prof.max_all, g_prof.samples);
        logf("[NRPRE] PERFIL  copias=%.3f ms  PICO copias=%.3f ms",
             g_prof.sum_snap / n, g_prof.max_snap);
        g_prof.sum_enc = g_prof.sum_net = g_prof.sum_dec = g_prof.sum_all = 0;
        g_prof.max_all = g_prof.max_net = 0;
        g_prof.sum_snap = g_prof.max_snap = 0;
        g_prof.samples = 0;
    }
}


// Which side of the cadence actually runs NR. An alt-tab can drop or add a single
// evaluate, flipping this relative to DLSS's jitter sequence; that is the thing
// F11 lets us flip back by hand to prove whether parity is the cause.
static unsigned g_phase = 0;
static bool  g_delta_reuse = true;      // skipped frames reapply the last NR delta
static bool  g_delta_valid = false;
static bool  g_final_valid = false;   // final_tex holds a real frame
// How transformative the result is belongs to the network, not to us: blending its
// output or shifting exposure changes the picture globally, it does not make the
// network less inventive. So the presets drive its own controls. Reference for the
// values: the community baseline is 1.00 across the board (intensity 1.00-1.05)
// and going above that is reported to look worse, so the axis runs downward from
// what the network intends. Local structure is the micro-detail term -- it is the
// one that invents texture, so it leads the way down.
// Encode divides by paper white, so a *larger* paper white dims what the network
// is shown. Expressing this as a gain the value is divided by keeps the control
// reading the way it behaves: above 1 is a brighter input, and the network acts
// harder on it. Auto exposure still owns the absolute level; this only biases it.
static float g_input_gain = 1.0f;
static int   g_preset = 3;              // 0 custom, 1 subtle, 2 natural, 3 strong, 4 max
static float g_chroma_transfer = 0.0f;  // adopt the network's own colour, not just its light
static float g_effect_strength = 1.0f;  // blend back toward the game's own image
static float g_struct_gate  = 1.0f;     // delta allowed, relative to local contrast
static float g_depth_reject = 0.02f;    // relative depth gap that reads as a disocclusion
static float g_reproject = 1.0f;        // follow motion vectors when reusing the delta
// How many frames old the stored delta is. The motion vector describes one frame of
// movement, but above cadence 1 the delta can be two or three frames behind, and
// following a single frame of motion then lands it short -- by more the faster the
// camera moves and the higher the cadence. Scaling the step by the delta's age
// assumes velocity held roughly constant across those frames, which is wrong under
// hard acceleration but far closer than pretending the delta is one frame old.
static unsigned g_delta_age = 1;
// Scaling the step by the delta's age assumes the velocity held constant across
// those frames. Under a turning camera it does not, and overshooting reads worse
// than landing short -- so this is off, and a flat one-frame step is what ships.
static bool g_delta_age_scale = false;
// With advection on, the delta is carried forward every frame and is already in
// position, so the apply pass samples it where it stands and the age multiplier is
// not used at all. Off, the old behaviour returns: reproject by age, extrapolated.
static bool g_delta_advect = false;   // measured worse than leaving the delta in place
static float g_mv_scale_x = 1.0f, g_mv_scale_y = 1.0f;   // the game's own, captured live
static float g_delta_clamp = 0.25f;     // cap in encoded units; 0 disables clamping
static bool g_skip_passthrough = true;  // skipped frames pass the game's colour, not a stale one
static int g_jit_burst = 40;      // F11 arms a burst that dumps the jitter sequence
static unsigned long long g_eval_tick = 0;

// The game evaluates DLSS twice per rendered frame with the *same* jitter (measured:
// 233 of 240 runs were exactly 2 long). Counting evaluates therefore never gave a
// real per-frame cadence, and an alt-tab that drops or adds one evaluate flips which
// of the two passes gets NR -- that is the image breaking at cadence 2.
// Anchor on the jitter instead: it comes from the game, is identical within a frame,
// and changes on every new one, so nothing we count can drift out of step.
// Roughly seven threads call the hook, so claim the frame with one atomic swap:
// only the thread that wins the exchange is the frame's first evaluate.
//
// The swap assumes the jitter changes on every new frame. It does not when the
// game holds it still (a paused scene, or a renderer that never animates the
// TAA pattern), and then a plain prev != key says "repeat" for the rest of the
// session: the network would run once and never again. A frozen key makes
// frames indistinguishable, so the best a key can do then is claim one thread
// per run of evaluates longer than any single frame's thread count: the run
// counter restarts on every claim, so a frozen key yields one claimed frame
// every ~kMaxClaimThreads/7 frames instead of none, and a live key (the common
// case) still claims exactly one thread on the first transition.
static volatile LONG64 g_jit_key = 0;
static volatile LONG64 g_jit_run = 0;             // consecutive same-key repeats
static volatile LONG64 g_frame_seq = 0;
static const unsigned kMaxClaimThreads = 16;      // ~7 call today; headroom, not a limit
static bool claim_frame(float jx, float jy, unsigned long long *out_frame) {
    unsigned kx, ky;
    std::memcpy(&kx, &jx, 4);
    std::memcpy(&ky, &jy, 4);
    LONG64 key = (LONG64)(((unsigned long long)kx << 32) | (unsigned long long)ky);
    if (key == 0) key = 1;                       // 0 is the "nothing seen yet" marker
    // Unconditional atomic swap: whichever thread gets back a different key is
    // the first evaluate of this frame, and exactly one thread can.
    const LONG64 prev = InterlockedExchange64(&g_jit_key, key);
    bool first = (prev != key);
    if (first) {
        InterlockedExchange64(&g_jit_run, 0);
    } else {
        const LONG64 run = InterlockedIncrement64(&g_jit_run);
        // A run longer than any frame's thread count cannot be one frame.
        if (run > (LONG64)kMaxClaimThreads) {
            first = true;
            InterlockedExchange64(&g_jit_run, 0); // restart: one claim per long run
        }
    }
    LONG64 seq = first ? InterlockedIncrement64(&g_frame_seq)
                       : InterlockedCompareExchange64(&g_frame_seq, 0, 0);
    *out_frame = (unsigned long long)seq;
    return first;
}
static void release_state(const char *why);
static void record_exposure_sample(ID3D12GraphicsCommandList *, ID3D12Device *,
                                   ID3D12Resource *, ID3D12Resource *, unsigned, unsigned);
static ID3D12Device *g_device = nullptr;
static ID3D12Resource *make_uav_tex(ID3D12Device *, unsigned, unsigned, DXGI_FORMAT);
static void codec_dispatch(ID3D12GraphicsCommandList *, ID3D12Device *, ID3D12PipelineState *,
                           ID3D12Resource *, ID3D12Resource *, ID3D12Resource *,
                           ID3D12Resource *, unsigned, unsigned,
                           ID3D12Resource * = nullptr, ID3D12Resource * = nullptr);
static void advect_delta(ID3D12GraphicsCommandList *, ID3D12Device *, ID3D12Resource *,
                         ID3D12Resource *, ID3D12Resource *, unsigned, unsigned);
static void snapshot_dispatch(ID3D12GraphicsCommandList *, ID3D12Device *,
                              ID3D12Resource *, ID3D12Resource *, DXGI_FORMAT,
                              unsigned, unsigned);
static void uav_barrier(ID3D12GraphicsCommandList *, ID3D12Resource *);
static void barrier_transition(ID3D12GraphicsCommandList *, ID3D12Resource *,
                               D3D12_RESOURCE_STATES, D3D12_RESOURCE_STATES);
static bool  g_codec_on = true;     // F4: run the colour codec around NR
static float g_paper_white = 1.0f;     // neutral: scene-linear 1.0 = reference white
static float g_transfer = 1.0f;
static float g_color_strength = 1.0f;   // shader reads 0 as 1.0, so this is identical
static unsigned g_hdr_mode = 2;        // 2 = scene-linear; auto-guides may drop it to 0
static float g_knee = 0.75f;           // shoulder start of the development curve

// Live status shown in the overlay.
static float g_ui_ms = 0.0f, g_ui_fps = 0.0f, g_ui_nr_hz = 0.0f;
static float g_ui_base_ms = 0.0f;      // last frame time measured with NR off

static const unsigned kHistBins = 132;   // 0..127 luminance, 128 HDR, 129/130 depth

// ---- automatic tonemap calibration ---------------------------------------
// The generic paper-white/soft-clip curve is borrowed from another title. The
// game itself shows us the answer every frame: the DLSS colour buffer is the
// scene pre-tonemap and the backbuffer is that same scene post-tonemap. Match
// their luminance percentiles and the transfer curve falls out, whatever the
// game's tonemapper happens to be.
// Source priority, resolved automatically: the game's own exposure is the real
// measurement, the scene histogram is the fallback for titles that do not hand
// one over, and the manual sliders are the floor. The user picks "Auto" or not;
// which source serves it is not a decision worth exposing.
static bool  g_use_exp_tex = true;      // internal: disabled on repeated failure
static unsigned g_exp_fresh = 0;        // frames since the last game-exposure read
static bool  g_exp_from_game = false;   // read the game's exposure texture (opt-in: needs its resource state)
static bool  g_auto_pw = false;            // UI: derive paper white from the scene
static bool  g_pw_valid = false;
static unsigned g_pw_updates = 0;
static float g_pw_measured = 0.0f;
// Derived alongside paper white from the same histogram -- no extra passes.
static bool  g_auto_guides = true;     // derive HDR + shoulder
static bool  g_hdr_detected = true;    // latched: proven HDR, never unlatched
static unsigned g_hdr_samples = 0;
static bool  g_depth_reversed = true;  // fixed fallback when NGX flags are unavailable
static bool  g_depth_from_game = false;
// Creation flags may be absent from evaluate. Keep the full creation contract
// across NR-only cleanup; retire it when its own provider releases the handle.
static nr::FeatureRegistry g_features;
static thread_local unsigned t_ngx_provider = 0;
static unsigned long long g_fg_bypass = 0, g_unknown_bypass = 0;
static unsigned long long g_nested_dlss_bypass = 0;
static_assert(NVSDK_NGX_Feature_SuperSampling == 1 && NVSDK_NGX_Feature_RayReconstruction == 13);
static bool  g_guides_valid = false;
static float g_knee_measured = 0.75f;

static bool read_dlss_flags(const NVSDK_NGX_Parameter *params, unsigned &flags) {
    if (!params) return false;
    auto **vt = *reinterpret_cast<void ***>(const_cast<NVSDK_NGX_Parameter *>(params));
    using GetI = NVSDK_NGX_Result (STDMETHODCALLTYPE *)(const NVSDK_NGX_Parameter *, const char *, int *);
    using GetU = NVSDK_NGX_Result (STDMETHODCALLTYPE *)(const NVSDK_NGX_Parameter *, const char *, unsigned *);
    int signed_flags = 0;
    // Official DLSS/DLSSD helpers use SetI; retain the unsigned proxy fallback.
    if (reinterpret_cast<GetI>(vt[11])(params, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                                     &signed_flags) == NVSDK_NGX_Result_Success) {
        flags = static_cast<unsigned>(signed_flags);
    } else if (reinterpret_cast<GetU>(vt[12])(params, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                                            &flags) != NVSDK_NGX_Result_Success) {
        return false;
    }
    return (flags & static_cast<unsigned>(NVSDK_NGX_DLSS_Feature_Flags_IsInvalid)) == 0;
}

static void update_depth_guide(const NVSDK_NGX_Handle *feat, const NVSDK_NGX_Parameter *params) {
    unsigned flags = 0;
    bool known = false;
    const auto *saved = g_features.find(t_ngx_provider, feat);
    if (saved && saved->flags_valid) {
        flags = saved->flags;
        known = true;
    } else {
        known = read_dlss_flags(params, flags);
    }
    const bool reversed = !known || (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    if (reversed != g_depth_reversed) {
        g_reset_pending = true;
        g_delta_valid = false;
        g_final_valid = false;
    }
    if (reversed != g_depth_reversed || known != g_depth_from_game)
        logf("[NRPRE] depth guide: %s source=%s flags=0x%08X",
             reversed ? "reversed" : "normal", known ? "game" : "fixed fallback", flags);
    g_depth_reversed = reversed;
    g_depth_from_game = known;
}

// Network-side controls. We were leaving all of these unset, which meant the
// final post block fell through to its simple_blend kernel instead of the
// mask-aware one. Defaults follow the baseline the community settled on: 1.00
// across the board, with higher values reported to look worse.
static bool  g_auto_mask = true;        // picks the control_mask kernel path
static float g_intensity = 1.0f;        // reported as harmful above 1.0
static float g_local_tone = 0.15f;
static float g_local_structure = 0.70f;
static float g_skin_structure = -1.0f;  // -1 = leave to the network
static int   g_style = 0;
// The network is 4.9 ms of the 8.9 ms frame -- 56% of the budget, measured, and
// at cadence 2 it lands on every other frame, so the rendered interval swings
// 8.9/13.9 ms and DLSS-G cannot pace through that. The feature already carries a
// scaling knob; if it means what it says, running the net below render resolution
// cuts the cost by area and the delta -- which is low frequency, and already
// sampled bilinearly -- is the right thing to compute coarsely. 1.0 keeps today's
// behaviour exactly; try 0.6 and watch PERFIL's red= before trusting it.
static float g_net_scale = 1.0f;
// Stage 1 of taking the network off the critical path: it reads our own copies of
// Color/Depth/MVec instead of the game's textures, still on the graphics queue and
// still in order. That settles the two unknowns -- whether the copies come out
// right for a typeless depth buffer, and whether the network is happy reading
// them -- before any second queue exists to hang on. Off by default.
// Stage 1 validated that the network reads our copies correctly, which is what
// stage 2 needed to know. Without stage 2 the network still runs in order on the
// graphics queue and can read the game's textures directly, so the three copies
// buy nothing and cost 0.07 ms a frame. Off until there is a second queue to feed.
static bool g_async_net = false;
static int  g_stall_ms = 120;   // F8: matches the >100 ms frames Streamline reports
// Above cadence 1 the image alternates between two ways of being built: the frame
// that runs the network gets its output developed directly, the frames between get
// the stored effect reprojected onto them. Those two do not land in the same place
// once anything moves, and a difference that alternates every frame is a flicker --
// which is exactly what is left in Balanced and Performance.
//
// So stop giving one frame in N the special treatment. Build *every* frame the
// same way, from the stored delta, and let the network's fresh result only ever
// arrive as the next frame's delta. The reconstruction is then uniform: what varies
// is how old the delta is, and that is a far smaller difference than which of two
// different paths drew the frame.
static bool g_uniform_delta = true;

// ---- exposure read over a dedicated COPY queue ----------------------------
// On a COPY queue D3D12 treats every resource as COMMON, so a copy needs no
// barrier and we never have to declare a StateBefore we cannot know. That is
// the whole class of bug that kept removing the device. The game's texture is
// AddRef'd while the copy is in flight, because it hands us a different 1x1
// texture every frame and would otherwise free it under us.
struct ExpQueue {
    ID3D12CommandQueue      *queue = nullptr;
    ID3D12CommandAllocator  *alloc = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Resource          *readback = nullptr;
    ID3D12Fence             *fence = nullptr;
    UINT64 value = 0, wait_at = 0;
    ID3D12Resource *held = nullptr;     // AddRef'd game texture, released after read
    DXGI_FORMAT     held_fmt = DXGI_FORMAT_R32_FLOAT;
    bool inflight = false, ready = false;
} g_eq;

static ID3D12Resource *g_pending_exp_tex = nullptr;   // captured in the eval hook

static bool exp_queue_init(ID3D12Device *dev) {
    if (g_eq.ready) return true;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_eq.queue)))) return false;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                           IID_PPV_ARGS(&g_eq.alloc)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_eq.alloc,
                                      nullptr, IID_PPV_ARGS(&g_eq.list)))) return false;
    g_eq.list->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_eq.fence)))) return false;

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_eq.readback)))) return false;

    g_eq.ready = true;
    logf("[NRPRE] exposure: copy queue ready");
    return true;
}

// Runs at present, on the graphics queue we are handed, so the game's frame is
// already submitted: signal there, make the copy queue wait on that, and only
// then read. Never blocks.
static void exp_queue_tick(ID3D12Device *dev, ID3D12CommandQueue *gfx) {
    if (!g_use_exp_tex || dev == nullptr || gfx == nullptr) return;
    if (!exp_queue_init(dev)) return;

    if (g_eq.inflight && g_eq.fence->GetCompletedValue() >= g_eq.wait_at) {
        g_eq.inflight = false;
        void *m = nullptr; D3D12_RANGE r{ 0, 64 };
        if (SUCCEEDED(g_eq.readback->Map(0, &r, &m)) && m) {
            // Whatever the channel count, exposure lives in the first component.
            float e = 0.0f;
            switch (g_eq.held_fmt) {
            case DXGI_FORMAT_R16_FLOAT: {
                const unsigned short h = *static_cast<const unsigned short *>(m);
                const unsigned sg = (h >> 15) & 1, ex = (h >> 10) & 0x1F, mant = h & 0x3FF;
                e = (ex == 0) ? ldexpf((float)mant / 1024.0f, -14)
                              : ldexpf(1.0f + (float)mant / 1024.0f, (int)ex - 15);
                if (sg) e = -e;
                break;
            }
            default:
                e = *static_cast<const float *>(m);      // R32_FLOAT / R32G32B32A32_FLOAT
                break;
            }
            if ((g_pw_updates % 40) == 0)
                if (g_diagnostics)
                    logf("[NRPRE] exposure read: %.6f -> paper white %.4f", e, e > 1e-5f ? 1.0f / e : 0.0f);
            D3D12_RANGE w{ 0, 0 }; g_eq.readback->Unmap(0, &w);
            // Measured on GTA V: the value rises as the scene darkens (day ~2.4-3.8,
            // night ~11.3), so it is the exposure gain the game applies to reach
            // display range. Normalising the network's input the same way means
            // dividing by its reciprocal. Independent corroboration: at night the
            // scene histogram lands on 0.100 and this gives 1/11.31 = 0.088.
            if (e > 1e-5f && e < 1e5f) {
                float pw = 1.0f / e;
                if (pw < 0.10f) pw = 0.10f;
                if (pw > 4.0f)  pw = 4.0f;
                g_pw_measured = pw;
                g_exp_fresh = 0;
                g_exp_from_game = true;
                if (g_auto_pw) {
                    g_paper_white = g_pw_valid ? (g_paper_white * 0.85f + pw * 0.15f) : pw;
                    g_pw_valid = true;
                }
                ++g_pw_updates;
            }
        }
        if (g_eq.held) { g_eq.held->Release(); g_eq.held = nullptr; }
    }

    if (++g_exp_fresh > 600) g_exp_from_game = false;   // ~10 s without a reading
    if (g_eq.inflight || g_pending_exp_tex == nullptr) return;
    static unsigned n = 0;
    if ((n++ % 15) != 0) return;

    ID3D12Resource *tex = g_pending_exp_tex;
    D3D12_RESOURCE_DESC td{};
    if (!res_desc(tex, &td)) return;

    // Look before touching it. Every crash so far came from assuming the shape
    // of a resource the game owns; report what it actually is, once.
    static bool described = false;
    if (!described) {
        described = true;
        logf("[NRPRE] exposure texture: dim=%d %llux%u mips=%u fmt=%d layout=%d flags=0x%X",
             (int)td.Dimension, (unsigned long long)td.Width, td.Height,
             td.MipLevels, (int)td.Format, (int)td.Layout, (unsigned)td.Flags);
    }
    if (td.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        td.Width == 0 || td.Width > 16 || td.Height > 16) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logf("[NRPRE] exposure: not the 1x1 texture we expected -- not copying");
        }
        return;                       // refuse rather than guess
    }
    // Measured on GTA V: 1x1 R32G32B32A32_FLOAT (16 bytes), not the single float
    // I had assumed. The footprint format must match the source exactly.
    g_eq.held_fmt = td.Format;
    tex->AddRef();                       // keep it alive for the whole copy
    g_eq.held = tex;

    g_eq.alloc->Reset();
    g_eq.list->Reset(g_eq.alloc, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = g_eq.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = g_eq.held_fmt;
    dst.PlacedFootprint.Footprint.Width = 1;
    dst.PlacedFootprint.Footprint.Height = 1;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = 256;
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    g_eq.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);   // no barriers needed
    g_eq.list->Close();

    const UINT64 after_gfx = ++g_eq.value;
    gfx->Signal(g_eq.fence, after_gfx);            // frame is submitted
    g_eq.queue->Wait(g_eq.fence, after_gfx);       // copy waits for it
    ID3D12CommandList *lists[1] = { g_eq.list };
    g_eq.queue->ExecuteCommandLists(1, lists);
    g_eq.wait_at = ++g_eq.value;
    g_eq.queue->Signal(g_eq.fence, g_eq.wait_at);
    g_eq.inflight = true;
}



// ---- colour-codec plumbing ------------------------------------------------
// Two compute passes wrap the NR evaluate: encode the game's scene-linear HDR
// into the bounded sRGB domain DLSSNR was trained on, then decode back.
// Each recording owns descriptor pages. Pages are recycled only after Reset or
// destruction AND completion of every queue that submitted the recording.
static const unsigned kCxSlots = 64;
static const unsigned kCxDescriptorsPerSlot = 7;
// At most nine slots: three snapshots, histogram clear+dispatch, encode,
// advect, decode/apply and delta capture. Reserve before recording any of them.
static const unsigned kCxBatchSlots = 12;

struct Codec {
    ID3D12RootSignature *root = nullptr;
    ID3D12PipelineState *pso_enc = nullptr, *pso_dec = nullptr;
    ID3D12PipelineState *pso_delta = nullptr, *pso_apply = nullptr;
    // Private copies of the game's Color/Depth/MVec. See CSSnapshot: the network
    // cannot keep reading the game's own textures once it stops running in order
    // on the graphics queue, because those are being redrawn for the next frame.
    ID3D12PipelineState *pso_snap = nullptr, *pso_advect = nullptr;
    ID3D12Resource *delta2 = nullptr;   // ping-pong: advection cannot read and write one texture
    ID3D12Resource *snap_color = nullptr, *snap_depth = nullptr, *snap_mv = nullptr;
    bool snap_ready = false;
    ID3D12DescriptorHeap *heap = nullptr;
    ID3D12Resource *proxy = nullptr, *final_tex = nullptr;
    ID3D12Resource *delta = nullptr;      // what the network added, reused on skipped frames
    ID3D12PipelineState *pso_hist = nullptr;
    ID3D12Resource *exp_rb = nullptr;      // readback for the game's 1x1 exposure texture
    ID3D12Resource *hist = nullptr;        // 128 bins, GPU-side
    ID3D12Resource *hist_rb = nullptr;     // readback, CPU-visible
    ID3D12Fence *fence = nullptr;
    UINT64 fence_val = 0;
    UINT64 pending_at = 0;                 // fence value the last copy waits on
    bool   pending = false;
    bool   copy_recorded = false;          // set in eval, signalled at present
    bool   exp_is_texture = false;         // the pending copy is the exposure texel
    ID3D12DescriptorHeap *clear_heap = nullptr;   // non-shader-visible, for the UAV clear
    UINT inc = 0;
    unsigned slot = 0;
    bool ready = false;
} g_cx;

struct DescriptorPage {
    nr::DescriptorLifetime lifetime;
    ID3D12DescriptorHeap *heap = nullptr;
    unsigned used = 0;
};
static std::vector<DescriptorPage> g_descriptor_pages;
static size_t g_descriptor_page = 0;
static uint64_t g_descriptor_recording = 0;
static unsigned g_descriptor_end = 0;
static unsigned g_descriptor_bypass = 0, g_descriptor_invariant = 0;
static bool g_descriptor_failed = false;
static uint64_t descriptor_completed(uint64_t fence) {
    return reinterpret_cast<ID3D12Fence *>(fence)->GetCompletedValue();
}
static void cx_discard_recording(uint64_t id) {
    for (auto &page : g_descriptor_pages) page.lifetime.discard(id);
}

static bool cx_begin_batch(ID3D12GraphicsCommandList *cmd, ID3D12Device *dev) {
    g_descriptor_failed = false;
    const uint64_t id = recording_id(cmd);
    if (!id || g_submission_failed) return false;
    size_t chosen = g_descriptor_pages.size();
    for (size_t i = 0; i < g_descriptor_pages.size(); ++i) {
        auto &page = g_descriptor_pages[i];
        if (page.lifetime.owner == id && page.used + kCxBatchSlots <= kCxSlots) {
            chosen = i;
            break;
        }
    }
    if (chosen == g_descriptor_pages.size()) {
        for (size_t i = 0; i < g_descriptor_pages.size(); ++i) {
            auto &page = g_descriptor_pages[i];
            if (page.lifetime.reusable(descriptor_completed)) {
                page.lifetime.assign(id);
                page.used = 0;
                chosen = i;
                break;
            }
        }
    }
    if (chosen == g_descriptor_pages.size()) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kCxSlots * kCxDescriptorsPerSlot;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        DescriptorPage page;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&page.heap)))) return false;
        page.lifetime.assign(id);
        g_descriptor_pages.push_back(std::move(page));
    }
    auto &page = g_descriptor_pages[chosen];
    g_descriptor_page = chosen;
    g_descriptor_recording = id;
    g_cx.heap = page.heap; // borrowed; the page pool owns the COM reference
    g_cx.slot = page.used;
    g_descriptor_end = page.used + kCxBatchSlots;
    return true;
}

static bool cx_slot_acquire(ID3D12GraphicsCommandList *cmd, unsigned &slot) {
    if (!g_cx.heap || g_cx.slot >= g_descriptor_end ||
        recording_id(cmd) != g_descriptor_recording) {
        g_descriptor_failed = true;
        ++g_descriptor_invariant;
        logf("[NRPRE] descriptor batch invariant failed; discarding codec output");
        return false;
    }
    slot = g_cx.slot++;
    g_descriptor_pages[g_descriptor_page].used = g_cx.slot;
    return true;
}

// These two return a struct by value: MSVC uses the hidden-sret convention that
// MinGW does not match, so call them through the vtable explicitly.
static D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu(ID3D12DescriptorHeap *h) {
    using pfn = D3D12_CPU_DESCRIPTOR_HANDLE *(STDMETHODCALLTYPE *)(void *, D3D12_CPU_DESCRIPTOR_HANDLE *);
    auto **vt = *reinterpret_cast<void ***>(h);
    D3D12_CPU_DESCRIPTOR_HANDLE out{};
    reinterpret_cast<pfn>(vt[9])(h, &out);
    return out;
}
static D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu(ID3D12DescriptorHeap *h) {
    using pfn = D3D12_GPU_DESCRIPTOR_HANDLE *(STDMETHODCALLTYPE *)(void *, D3D12_GPU_DESCRIPTOR_HANDLE *);
    auto **vt = *reinterpret_cast<void ***>(h);
    D3D12_GPU_DESCRIPTOR_HANDLE out{};
    reinterpret_cast<pfn>(vt[10])(h, &out);
    return out;
}

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void *, void *,
        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static bool codec_init(ID3D12Device *dev, unsigned w, unsigned h) {
    if (g_cx.ready) return true;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto D3DCompileFn = dc ? reinterpret_cast<PFN_D3DCompile>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!D3DCompileFn) { logf("[NRPRE] codec: d3dcompiler_47 unavailable"); return false; }

    ID3DBlob *enc = nullptr, *dec = nullptr, *err = nullptr;
    HRESULT h1 = D3DCompileFn(kCodecHLSL, strlen(kCodecHLSL), "codec", nullptr, nullptr,
                              "CSEncode", "cs_5_0", 0, 0, &enc, &err);
    if (FAILED(h1)) {
        logf("[NRPRE] codec: CSEncode failed 0x%08lX %s", (unsigned long)h1,
             err ? (const char *)err->GetBufferPointer() : "");
        return false;
    }
    HRESULT h2 = D3DCompileFn(kCodecHLSL, strlen(kCodecHLSL), "codec", nullptr, nullptr,
                              "CSDecode", "cs_5_0", 0, 0, &dec, &err);
    if (FAILED(h2)) {
        logf("[NRPRE] codec: CSDecode failed 0x%08lX %s", (unsigned long)h2,
             err ? (const char *)err->GetBufferPointer() : "");
        return false;
    }

    ID3DBlob *dlt = nullptr, *apl = nullptr;
    HRESULT h4 = D3DCompileFn(kCodecHLSL, strlen(kCodecHLSL), "codec", nullptr, nullptr,
                              "CSDelta", "cs_5_0", 0, 0, &dlt, &err);
    if (FAILED(h4)) {
        logf("[NRPRE] codec: CSDelta failed 0x%08lX %s", (unsigned long)h4,
             err ? (const char *)err->GetBufferPointer() : "");
        return false;
    }
    HRESULT h5 = D3DCompileFn(kCodecHLSL, strlen(kCodecHLSL), "codec", nullptr, nullptr,
                              "CSApplyDelta", "cs_5_0", 0, 0, &apl, &err);
    if (FAILED(h5)) {
        logf("[NRPRE] codec: CSApplyDelta failed 0x%08lX %s", (unsigned long)h5,
             err ? (const char *)err->GetBufferPointer() : "");
        return false;
    }
    ID3DBlob *hist_cs = nullptr;
    HRESULT h3 = D3DCompileFn(kCodecHLSL, strlen(kCodecHLSL), "codec", nullptr, nullptr,
                              "CSHistogram", "cs_5_0", 0, 0, &hist_cs, &err);
    if (FAILED(h3)) {
        logf("[NRPRE] codec: CSHistogram failed 0x%08lX %s", (unsigned long)h3,
             err ? (const char *)err->GetBufferPointer() : "");
        return false;
    }
    // Optional: without it only the async path is unavailable, so a failure here
    // must not take the whole codec down with it.
    ID3DBlob *adv_cs = nullptr;
    HRESULT hadv = D3DCompileFn(kCodecHLSL, strlen(kCodecHLSL), "codec", nullptr, nullptr,
                                "CSAdvect", "cs_5_0", 0, 0, &adv_cs, &err);
    if (FAILED(hadv))
        logf("[NRPRE] codec: CSAdvect failed 0x%08lX (delta will age in place)",
             (unsigned long)hadv);
    ID3DBlob *snap_cs = nullptr;
    HRESULT hsnap = D3DCompileFn(kCodecHLSL, strlen(kCodecHLSL), "codec", nullptr, nullptr,
                                 "CSSnapshot", "cs_5_0", 0, 0, &snap_cs, &err);
    if (FAILED(hsnap))
        logf("[NRPRE] codec: CSSnapshot failed 0x%08lX (async path unavailable)",
             (unsigned long)hsnap);

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 5; ranges[0].BaseShaderRegister = 0;   // t3 mvec, t4 depth
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2; ranges[1].BaseShaderRegister = 0;   // u0 image, u1 histogram
    ranges[1].OffsetInDescriptorsFromTableStart = 5;

    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 2;
    rp[0].DescriptorTable.pDescriptorRanges = ranges;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.ShaderRegister = 0; rp[1].Constants.Num32BitValues = 16;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2; rsd.pParameters = rp;
    ID3DBlob *sig = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        logf("[NRPRE] codec: root signature serialise failed"); return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&g_cx.root)))) {
        logf("[NRPRE] codec: CreateRootSignature failed"); return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_cx.root;
    pd.CS.pShaderBytecode = enc->GetBufferPointer(); pd.CS.BytecodeLength = enc->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_cx.pso_enc)))) {
        logf("[NRPRE] codec: encode PSO failed"); return false;
    }
    pd.CS.pShaderBytecode = dec->GetBufferPointer(); pd.CS.BytecodeLength = dec->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_cx.pso_dec)))) {
        logf("[NRPRE] codec: decode PSO failed"); return false;
    }

    g_cx.inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    pd.CS.pShaderBytecode = hist_cs->GetBufferPointer();
    pd.CS.BytecodeLength = hist_cs->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_cx.pso_hist)))) {
        logf("[NRPRE] codec: histogram PSO failed"); return false;
    }
    pd.CS.pShaderBytecode = dlt->GetBufferPointer(); pd.CS.BytecodeLength = dlt->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_cx.pso_delta)))) {
        logf("[NRPRE] codec: delta PSO failed"); return false;
    }
    if (adv_cs != nullptr) {
        pd.CS.pShaderBytecode = adv_cs->GetBufferPointer();
        pd.CS.BytecodeLength  = adv_cs->GetBufferSize();
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_cx.pso_advect))))
            logf("[NRPRE] codec: advect PSO failed");
    }
    // snapshot: same root signature, its own PSO
    if (snap_cs != nullptr) {
        pd.CS.pShaderBytecode = snap_cs->GetBufferPointer();
        pd.CS.BytecodeLength  = snap_cs->GetBufferSize();
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_cx.pso_snap))))
            logf("[NRPRE] codec: snapshot PSO failed (async path unavailable)");
    }
    pd.CS.pShaderBytecode = apl->GetBufferPointer(); pd.CS.BytecodeLength = apl->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_cx.pso_apply)))) {
        logf("[NRPRE] codec: apply PSO failed"); return false;
    }

    // histogram buffer (GPU) + readback buffer (CPU) + fence to know when it landed
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kHistBins * sizeof(UINT); bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_cx.hist));

        hp.Type = D3D12_HEAP_TYPE_READBACK;
        bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_cx.hist_rb));

        bd.Width = 256;                    // one texel, padded to the copy alignment
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_cx.exp_rb));

        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_cx.fence));

        // ClearUnorderedAccessViewUint needs a CPU handle from a heap that is
        // NOT shader visible, in addition to the shader-visible GPU handle.
        D3D12_DESCRIPTOR_HEAP_DESC cd{};
        cd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        cd.NumDescriptors = 1;
        cd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        dev->CreateDescriptorHeap(&cd, IID_PPV_ARGS(&g_cx.clear_heap));
        D3D12_UNORDERED_ACCESS_VIEW_DESC hv{};
        hv.Format = DXGI_FORMAT_UNKNOWN;
        hv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        hv.Buffer.NumElements = kHistBins;
        hv.Buffer.StructureByteStride = sizeof(UINT);
        dev->CreateUnorderedAccessView(g_cx.hist, nullptr, &hv, heap_cpu(g_cx.clear_heap));
    }

    g_cx.proxy     = make_uav_tex(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    g_cx.final_tex = make_uav_tex(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    // signed: the delta is a difference and goes negative wherever NR darkens
    g_cx.delta = make_uav_tex(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    g_cx.delta2 = make_uav_tex(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    g_cx.ready = (g_cx.proxy && g_cx.final_tex && g_cx.delta);
    logf("[NRPRE] codec: ready=%d proxy=%p final=%p", (int)g_cx.ready,
         (void *)g_cx.proxy, (void *)g_cx.final_tex);
    return g_cx.ready;
}
static bool g_want_hi = false;   // F8 asks for the output-res feature; built on demand
static void build_hi(ID3D12GraphicsCommandList *cmd);
typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_Eval)(ID3D12GraphicsCommandList *,
        const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *,
        PFN_NVSDK_NGX_ProgressCallback);
static NVSDK_NGX_Parameter *g_nr_params = nullptr;

static NVSDK_NGX_Handle *g_nr_handle = nullptr;
static ID3D12Resource   *g_nr_out = nullptr;      // NR result at render resolution
// Second feature at OUTPUT resolution, so we can A/B our own implementation
// against itself: identical code path, only the network size differs.
static NVSDK_NGX_Handle *g_nr_handle_hi = nullptr;
static ID3D12Resource   *g_nr_out_hi = nullptr;
static NVSDK_NGX_Parameter *g_nr_params_hi = nullptr;
static bool              g_use_hi = false;        // F8
static int               g_repeat = 1;            // how many times the network is applied
// Chained passes stage each result into this buffer and the next pass reads the
// copy. A texture cannot read and write itself at once, and the network's
// D3D12 backend did not tolerate a buffer it had just written being handed
// back as an input in the same frame (E_INVALIDARG on the first multi-pass
// frame), so the copy is what makes the chain safe: the network's output stays
// 'out' on every pass, and 'stage' is only ever an input.
static ID3D12Resource   *g_nr_stage = nullptr;
static ID3D12Resource   *g_nr_stage_hi = nullptr;
static bool              g_rebind = true;         // F10: feed NR output into DLSS (visible)
// Every frame is the default: since the cadence anchors on the jitter, this is one
// NR pass per frame in the pass that is actually shown. The old evaluate counter ran
// it twice per frame here, so this is now both better looking and cheaper.
static int               g_skip_n = 1;
static unsigned long long g_frame_no = 0;

// ---- interleaved auto-benchmark (F12) ------------------------------------
// Sequential A/B blocks lose to scene drift: each block sees a different scene.
// Instead cycle the states every 20 frames and bucket the timings, so drift is
// shared equally by every state and cancels in the comparison.
static bool   g_bench = false;
static int    g_bench_state = 0;          // 0=off 1=every1 2=every2 3=every3
static int    g_bench_frames = 0;
static double g_bench_sum[4] = {0,0,0,0};
static unsigned g_bench_n[4] = {0,0,0,0};
static int    g_bench_rounds = 0;

static void bench_apply() {
    switch (g_bench_state) {
    case 0: g_nr_enabled = false; g_skip_n = 1; break;
    case 1: g_nr_enabled = true;  g_skip_n = 1; break;
    case 2: g_nr_enabled = true;  g_skip_n = 2; break;
    case 3: g_nr_enabled = true;  g_skip_n = 3; break;
    }
}

static void bench_report() {
    const char *nm[4] = { "NR off      ", "NR every 1  ", "NR every 2  ", "NR every 3  " };
    logf("[NRPRE] ===== AUTO-BENCH (interleaved, %d rounds) =====", g_bench_rounds);
    double base = g_bench_n[0] ? g_bench_sum[0] / g_bench_n[0] : 0.0;
    for (int i = 0; i < 4; ++i) {
        if (!g_bench_n[i]) continue;
        double ms = g_bench_sum[i] / g_bench_n[i];
        if (i == 0)
            logf("[NRPRE]   %s %6.2f ms  %6.1f fps   (n=%u)", nm[i], ms, 1000.0 / ms, g_bench_n[i]);
        else
            logf("[NRPRE]   %s %6.2f ms  %6.1f fps   (n=%u)  NR cost %.2f ms  vs off %+.1f%% fps",
                 nm[i], ms, 1000.0 / ms, g_bench_n[i], ms - base, 100.0 * (base / ms - 1.0));
    }
}


// ID3D12GraphicsCommandList::ResourceBarrier is vtable slot 26.
static void barrier_transition(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res,
                               D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource   = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    using pfn_t = void (STDMETHODCALLTYPE *)(void *, UINT, const D3D12_RESOURCE_BARRIER *);
    auto **vt = *reinterpret_cast<void ***>(cmd);
    reinterpret_cast<pfn_t>(vt[26])(cmd, 1, &b);
}
static unsigned          g_eval_count = 0;
static unsigned          g_out_w = 2560, g_out_h = 1440;
static PFN_Eval          g_snip_eval = nullptr;
static PFN_Eval          g_nr_eval = nullptr;
static bool              g_nr_core_owned = false;
typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_ReleaseFeature)(NVSDK_NGX_Handle *);
static PFN_ReleaseFeature g_nr_release = nullptr;
static PFN_ReleaseFeature g_hi_release = nullptr;
typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_DestroyParams)(NVSDK_NGX_Parameter *);
static PFN_DestroyParams g_destroy_params = nullptr;
static unsigned          g_eval_logged = 0;

// ---- rolling-window frame timing: reports only the last ~2 s, so each phase of
// a manual A/B (stock NR on, then ours) is measured independently instead of
// being smeared into one cumulative mean.
static void frame_tick() {
    static LARGE_INTEGER freq{}, prev{}, last_report{};
    static double ring[512]; static unsigned head = 0, count = 0;
    static int skip = 0;
    static bool prev_state = false;

    if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); prev_state = g_nr_enabled; }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);

    if (g_nr_enabled != prev_state) {          // our NR toggled -> drop the window
        prev_state = g_nr_enabled; head = count = 0; skip = 30;
    }
    if (prev.QuadPart != 0) {
        double ms = (double)(now.QuadPart - prev.QuadPart) * 1000.0 / (double)freq.QuadPart;
        if (skip > 0) --skip;
        else if (ms > 0.2 && ms < 200.0) {
            ring[head] = ms; head = (head + 1) % 512; if (count < 512) ++count;
            if (g_bench) {
                // discard the first 6 frames of each slice so the pipeline settles
                if (g_bench_frames >= 6) {
                    g_bench_sum[g_bench_state] += ms;
                    ++g_bench_n[g_bench_state];
                }
                if (++g_bench_frames >= 26) {
                    g_bench_frames = 0;
                    g_bench_state = (g_bench_state + 1) & 3;
                    if (g_bench_state == 0) {
                        ++g_bench_rounds;
                        if (g_bench_rounds % 5 == 0) bench_report();
                        if (g_bench_rounds >= 30) { g_bench = false; bench_report();
                            logf("[NRPRE] AUTO-BENCH done"); }
                    }
                    bench_apply();
                }
            }
        }
    }
    prev = now;

    if (last_report.QuadPart == 0) last_report = now;        // arm the timer once
    double since = (double)(now.QuadPart - last_report.QuadPart) / (double)freq.QuadPart;
    if (count >= 120 && since >= 10.0) {
        last_report = now;
        double sum = 0.0; unsigned n = count < 240 ? count : 240;
        for (unsigned i = 0; i < n; ++i) sum += ring[(head + 512 - 1 - i) % 512];
        double avg = sum / n;
        static unsigned last_count = 0;
        unsigned evals = g_eval_count - last_count; last_count = g_eval_count;
        g_ui_ms = (float)avg;
        g_ui_fps = (float)(1000.0 / avg);
        g_ui_nr_hz = (float)evals / 10.0f;          // window is 10 s
        if (!g_nr_enabled) g_ui_base_ms = (float)avg;
    }
}

// MSVC slot 1 = Set(const char*, ID3D12Resource*)
typedef void (STDMETHODCALLTYPE *PFN_SetRes)(NVSDK_NGX_Parameter *, const char *, ID3D12Resource *);
static void pset_res(NVSDK_NGX_Parameter *p, const char *k, ID3D12Resource *v) {
    auto **vt = *reinterpret_cast<void ***>(p);
    reinterpret_cast<PFN_SetRes>(vt[1])(p, k, v);
}

// ID3D12Device::CreateCommittedResource is vtable slot 27; every argument is a
// pointer, so the MinGW/MSVC struct-return mismatch cannot bite here.
static ID3D12Resource *make_uav_tex(ID3D12Device *dev, unsigned w, unsigned h, DXGI_FORMAT fmt) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    static const GUID iid_res = {0x696442be,0xa72e,0x4059,{0xbc,0x79,0x5b,0x5c,0x98,0x04,0x0f,0xad}};
    using pfn_t = HRESULT (STDMETHODCALLTYPE *)(void *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS,
            const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
    auto **vt = *reinterpret_cast<void ***>(dev);
    ID3D12Resource *out = nullptr;
    HRESULT hr = reinterpret_cast<pfn_t>(vt[27])(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, iid_res, reinterpret_cast<void **>(&out));
    logf("[NRPRE] 2c: CreateCommittedResource %ux%u fmt=%d -> hr=0x%08lX res=%p",
         w, h, (int)fmt, (unsigned long)hr, (void *)out);
    return out;
}

static bool g_setup_done = false;

// Every diagnostic in here is capped to the first handful of frames, so by the
// time anything goes wrong the logging has already stopped and the log says
// nothing at all about the moment that matters. These are the two results that
// can turn an evaluate into garbage on screen -- the network's own, and DLSS's --
// and until now a failure in either was silent after frame five. Count them for
// the whole session and say so the first times and then periodically: a flash
// that lines up with a failure is a different problem from one that does not.
static unsigned g_er_fail = 0, g_gr_fail = 0, g_pass_frames = 0;
static unsigned long long g_ev_total = 0;

// F10 marks the log *after* the event, and nobody reacts inside a frame -- by the
// time the key goes down the flash is several frames gone. So record every
// evaluate into a small ring and have F10 dump what came *before* it: at roughly
// 240 evaluates a second, 1024 slots hold about four seconds, which is far more
// than any human delay. The same dump fires by itself on the first few NGX
// failures, so the interesting case needs no key at all.
struct EvRec {
    unsigned long long tick;
    double t;                 // wall clock, so a dump lines up with sl.log
    const void *feed, *src;
    unsigned er, gr;
    unsigned short w, h;
    unsigned char run, codec, fvalid, dvalid, pass, skip;
};
static EvRec g_ring[1024];
static volatile LONG g_ring_seq = 0;

static void ring_push(unsigned long long tick, const void *feed, const void *src,
                      unsigned er, unsigned gr, unsigned w, unsigned h,
                      bool run, bool codec, bool fvalid, bool dvalid, bool pass)
{
    if (!g_diagnostics) return;   // nothing reads it unless F10 is going to be pressed
    const LONG i = InterlockedIncrement(&g_ring_seq) - 1;
    EvRec &r = g_ring[(unsigned)i & 1023u];
    r.tick = tick; r.feed = feed; r.src = src; r.er = er; r.gr = gr;
    SYSTEMTIME st; GetLocalTime(&st);
    r.t = st.wHour * 3600.0 + st.wMinute * 60.0 + st.wSecond + st.wMilliseconds / 1000.0;
    r.w = (unsigned short)w; r.h = (unsigned short)h;
    r.run = run; r.codec = codec; r.fvalid = fvalid; r.dvalid = dvalid;
    r.pass = pass; r.skip = (unsigned char)g_skip_n;
}

// The first version of this wrote 1024 lines through ReShade's logger, which locks
// and flushes per message, on the present thread. That stalled the frame hard
// enough to *produce* the very artefact it was meant to catch -- the instrument was
// perturbing the experiment. Build the whole text in memory and put it on disk in
// one write, to a file of our own, so observing costs about as much as one log line.
static void ring_dump(const char *why)
{
    const LONG end = g_ring_seq;
    const LONG start = (end > 1024) ? end - 1024 : 0;
    static char *buf = nullptr;
    static const size_t kCap = 1024u * 224u;
    if (buf == nullptr) buf = static_cast<char *>(std::malloc(kCap));
    if (buf == nullptr) return;
    size_t o = 0;
    o += (size_t)std::snprintf(buf + o, kCap - o,
                               "==== %s: %ld evaluates hasta el #%ld ====\r\n",
                               why, (long)(end - start), (long)end);
    for (LONG i = start; i < end && o + 256 < kCap; ++i) {
        const EvRec &r = g_ring[(unsigned)i & 1023u];
        o += (size_t)std::snprintf(buf + o, kCap - o,
                 "R %6ld %02d:%02d:%06.3f tick=%llu run=%d cad=%d codec=%d fvalid=%d dvalid=%d pass=%d "
                 "%ux%u feed=%p src=%p er=0x%08X gr=0x%08X\r\n",
                 (long)i, (int)(r.t / 3600), (int)((r.t / 60)) % 60, r.t - ((long long)(r.t / 60)) * 60.0,
                 (unsigned long long)r.tick, (int)r.run, (int)r.skip, (int)r.codec,
                 (int)r.fvalid, (int)r.dvalid, (int)r.pass, (unsigned)r.w, (unsigned)r.h,
                 r.feed, r.src, r.er, r.gr);
    }
    HANDLE h = CreateFileW(L"nrpre-ring.txt", FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        SetFilePointer(h, 0, nullptr, FILE_END);
        WriteFile(h, buf, (DWORD)o, &wrote, nullptr);
        CloseHandle(h);
    }
    logf("[NRPRE] anillo volcado a nrpre-ring.txt (%s, %ld entradas)",
         why, (long)(end - start));
}




// One trampoline per module we hook. Which module is the live one cannot be told
// apart by inspection -- an NGX proxy and the driver both export the same entry
// point and both stay loaded -- so rather than guess, hook every module that
// exports it and let whichever is actually called identify itself by being
// called. t_orig carries the trampoline belonging to the hook we entered
// through, so the body always continues down the right chain.
static const unsigned kMaxNgx = 8;
static PFN_Eval g_orig_eval_n[kMaxNgx] = {};
static thread_local PFN_Eval t_orig_eval = nullptr;
static PFN_Eval g_orig_eval = nullptr;          // kept for the single-hook paths
static unsigned g_logged = 0;

static NVSDK_NGX_Result NVSDK_CONV eval_body(ID3D12GraphicsCommandList *cmd,
        const NVSDK_NGX_Handle *feat, const NVSDK_NGX_Parameter *params,
        PFN_NVSDK_NGX_ProgressCallback cb)
{
#ifdef NR_STANDALONE
    // The frame tick, at the top of the hook and unconditional.
    //
    // Standalone there is no ReShade present event, and the proxy must not put a
    // second detour on this same function -- two MinHook instances on one address
    // is a trampoline written over a trampoline. So it runs from here.
    //
    // It has to sit before every early return rather than inside the block that
    // does the work. This is also where the keys are read, and buried further down
    // it stopped running the moment F7 switched the network off, which left no way
    // to switch it back on.
    if (g_host_queue.native != nullptr) {
        g_host_queue.dev = &g_host_dev;
        on_present(&g_host_queue, nullptr, nullptr, nullptr, 0, nullptr);
    }
#endif

    // Resize detection must precede setup, snapshots, codec allocation and any
    // cached output selection. A pending reset always passes the game through.
    if (g_resize_pending || g_submission_failed)
        return t_orig_eval(cmd, feat, params, cb);
    unsigned color_w = 0, color_h = 0;
    if (params && cmd) {
        if (g_ptr_slot < 0) probe_slots(params, NVSDK_NGX_Parameter_Color);
        ID3D12Resource *color = nullptr;
        if (g_ptr_slot >= 0)
            param_get_slot(params, (unsigned)g_ptr_slot, NVSDK_NGX_Parameter_Color,
                           reinterpret_cast<void **>(&color));
        D3D12_RESOURCE_DESC desc{};
        if (color && res_desc(color, &desc)) {
            color_w = (unsigned)desc.Width;
            color_h = desc.Height;
        }
        ID3D12Device *command_device = nullptr;
        if (SUCCEEDED(cmd->GetDevice(IID_PPV_ARGS(&command_device)))) {
            const bool changed = g_setup_w && g_device && !same_device(command_device, g_device);
            command_device->Release();
            if (changed) {
                release_state("command list device changed");
                return t_orig_eval(cmd, feat, params, cb);
            }
        }
        if (g_setup_w && color_w &&
            (g_setup_w != color_w || g_setup_h != color_h)) {
            release_state("Color resource resolution changed");
            return t_orig_eval(cmd, feat, params, cb);
        }
    }
    // Discover the implementation used by NGX, which can bypass the presentation
    // queue's wrapper entirely. Do this before feature creation can record work.
    if (!cmd || !params || !color_w || !color_h || !prepare_recording(cmd))
        return t_orig_eval(cmd, feat, params, cb);

    update_depth_guide(feat, params);

    {   // heartbeat: proves whether the hook survives a device/swapchain recreation
        static unsigned long long hb = 0;
        if ((hb++ % 60ull) == 0ull)
            if (g_diagnostics)
            logf("[NRPRE] HB #%llu handle=%p | pw=%.4f meas=%.4f valid=%d upd=%u "
                 "| hdr=%u det=%d knee=%.3f kmeas=%.3f | expfresh=%u fromgame=%d "
                 "| net=%ux%u finalvalid=%d | enabled=%d strength=%.3f transfer=%.3f "
                 "| cad=%d rep=%d async=%d | erfail=%u grfail=%u passthru=%u "
                 "| depthinv=%d depthsrc=%s | descpages=%zu descbypass=%u descerr=%u "
                 "| fg_bypass=%llu unknown_bypass=%llu nested_dlss_bypass=%llu",
                 (unsigned long long)hb, (void *)g_nr_handle,
                 g_paper_white, g_pw_measured, (int)g_pw_valid, g_pw_updates,
                 g_hdr_mode, (int)g_hdr_detected, g_knee, g_knee_measured,
                 g_exp_fresh, (int)g_exp_from_game,
                 g_net_w, g_net_h, (int)g_final_valid,
                 (int)g_nr_enabled, g_effect_strength, g_transfer,
                 g_skip_n, (int)g_repeat, (int)g_async_net,   // which mode was actually running, always
                 g_er_fail, g_gr_fail, g_pass_frames,
                 (int)g_depth_reversed, g_depth_from_game ? "game" : "fallback",
                 g_descriptor_pages.size(), g_descriptor_bypass, g_descriptor_invariant,
                 g_fg_bypass, g_unknown_bypass, g_nested_dlss_bypass);
    }
    if (params != nullptr && (g_logged < 12 || !g_setup_done)) {
        if (g_logged == 0) probe_slots(params, NVSDK_NGX_Parameter_Color);
        char color[128], output[128], depth[128], mvec[128];
        describe(params, NVSDK_NGX_Parameter_Color,         color,  sizeof color);
        describe(params, NVSDK_NGX_Parameter_Output,        output, sizeof output);
        describe(params, NVSDK_NGX_Parameter_Depth,         depth,  sizeof depth);
        describe(params, NVSDK_NGX_Parameter_MotionVectors, mvec,   sizeof mvec);
        unsigned rw = 0, rh = 0;
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,  &rw);
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &rh);
        if (g_logged < 12) {
            logf("[NRPRE] eval feat=%u  render_subrect=%ux%u | %s | %s | %s | %s",
                 feat_id(feat), rw, rh, color, output, depth, mvec);
            ++g_logged;
        }
        {
            ID3D12Resource *o = nullptr;
            if (g_ptr_slot >= 0) param_get_slot(params, (unsigned)g_ptr_slot,
                    NVSDK_NGX_Parameter_Output, reinterpret_cast<void **>(&o));
            D3D12_RESOURCE_DESC od{};
            if (o && res_desc(o, &od)) { g_out_w = (unsigned)od.Width; g_out_h = od.Height; }
        }
        if (!g_setup_done)
            logf("[NRPRE] setup gate: rw=%u rh=%u cmd=%p", rw, rh, (void *)cmd);
        if (color_w > 0 && color_h > 0) setup_nr(color_w, color_h, cmd);
    }
    // ---- 2c: run NR at render resolution on the DLSS input colour, discard result ----
    if (g_nr_enabled && g_nr_handle && g_nr_out && g_nr_eval && params) {
        ID3D12Resource *src = nullptr, *dep = nullptr, *mv = nullptr;
        if (g_ptr_slot >= 0) {
            param_get_slot(params, (unsigned)g_ptr_slot, NVSDK_NGX_Parameter_Color,
                           reinterpret_cast<void **>(&src));
            param_get_slot(params, (unsigned)g_ptr_slot, NVSDK_NGX_Parameter_Depth,
                           reinterpret_cast<void **>(&dep));
            param_get_slot(params, (unsigned)g_ptr_slot, NVSDK_NGX_Parameter_MotionVectors,
                           reinterpret_cast<void **>(&mv));
        }
        unsigned w_net = 0, h_net = 0;
        {
            D3D12_RESOURCE_DESC sd{};
            if (src && res_desc(src, &sd)) { w_net = (unsigned)sd.Width; h_net = sd.Height; }
        }
        ID3D12Resource *dst_col = nullptr;
        if (g_use_hi && g_ptr_slot >= 0)
            param_get_slot(params, (unsigned)g_ptr_slot, NVSDK_NGX_Parameter_Output,
                           reinterpret_cast<void **>(&dst_col));

        if (g_use_hi && g_nr_handle_hi == nullptr) build_hi(cmd);
        const bool hi = g_use_hi && g_nr_handle_hi && g_nr_out_hi && dst_col;
        NVSDK_NGX_Parameter *pp  = hi ? g_nr_params_hi : g_nr_params;
        NVSDK_NGX_Handle    *hh  = hi ? g_nr_handle_hi : g_nr_handle;
        ID3D12Resource      *out = hi ? g_nr_out_hi    : g_nr_out;
        ID3D12Resource      *in  = hi ? dst_col        : src;

        if (in && dep && mv) {
            const bool use_codec = g_codec_on && !hi && g_device != nullptr
                                   && codec_init(g_device, w_net, h_net);
            // Preflight the entire evaluation before snapshots, encode or NR.
            // Allocation failure must pass the current game input through, never
            // skip an individual pass and pretend its old output is current.
            if ((g_codec_on && !hi && !use_codec) ||
                (g_cx.ready && !cx_begin_batch(cmd, g_device))) {
                ++g_descriptor_bypass;
                g_final_valid = g_delta_valid = false;
                g_reset_pending = true;
                if (g_descriptor_bypass <= 3 || g_descriptor_bypass % 120 == 0)
                    logf("[NRPRE] descriptor preflight failed; current input passthrough (%u)",
                         g_descriptor_bypass);
                return t_orig_eval(cmd, feat, params, cb);
            }
            // Track only lists where this add-on is about to write GPU work.
            // Tracking every Color evaluate also tracked passthrough/disabled
            // evaluates; if one of those lists was discarded, resize cleanup
            // waited forever and NR could no longer be toggled.
            bool recording_tracked = false;
            auto ensure_recording = [&]() {
                if (!recording_tracked) {
                    track_recording(cmd);
                    recording_tracked = true;
                }
            };
            ID3D12Resource *net_color = in, *net_depth = dep, *net_mv = mv;
            if (g_async_net && g_cx.pso_snap != nullptr && dep != nullptr && mv != nullptr) {
                if (!g_cx.snap_ready) {
                    g_cx.snap_color = make_uav_tex(g_device, w_net, h_net, DXGI_FORMAT_R16G16B16A16_FLOAT);
                    g_cx.snap_depth = make_uav_tex(g_device, w_net, h_net, DXGI_FORMAT_R32_FLOAT);
                    g_cx.snap_mv    = make_uav_tex(g_device, w_net, h_net, DXGI_FORMAT_R16G16_FLOAT);
                    g_cx.snap_ready = (g_cx.snap_color && g_cx.snap_depth && g_cx.snap_mv);
                    logf("[NRPRE] snapshot: %ux%u ready=%d", w_net, h_net, (int)g_cx.snap_ready);
                }
                if (g_cx.snap_ready) {
                    ensure_recording();
                    prof_mark(cmd, 4);
                    snapshot_dispatch(cmd, g_device, in,  g_cx.snap_color,
                                      DXGI_FORMAT_R16G16B16A16_FLOAT, w_net, h_net);
                    snapshot_dispatch(cmd, g_device, dep, g_cx.snap_depth,
                                      DXGI_FORMAT_R32_FLOAT, w_net, h_net);
                    snapshot_dispatch(cmd, g_device, mv,  g_cx.snap_mv,
                                      DXGI_FORMAT_R16G16_FLOAT, w_net, h_net);
                    uav_barrier(cmd, g_cx.snap_color);
                    uav_barrier(cmd, g_cx.snap_depth);
                    uav_barrier(cmd, g_cx.snap_mv);
                    // the network reads them the way it reads the game's own
                    barrier_transition(cmd, g_cx.snap_color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    barrier_transition(cmd, g_cx.snap_depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    barrier_transition(cmd, g_cx.snap_mv,    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    prof_mark(cmd, 5);
                    net_color = g_cx.snap_color;
                    net_depth = g_cx.snap_depth;
                    net_mv    = g_cx.snap_mv;
                }
            }
            pset_res(pp, "DLSSNR.Color",  net_color);
            pset_res(pp, "DLSSNR.Depth",  net_depth);
            pset_res(pp, "DLSSNR.MVec",   net_mv);
            // Forward the game's own motion-vector scale instead of assuming 1.0:
            // a wrong scale makes the network reproject onto the wrong pixels,
            // which shows up as smearing and dirty edges in motion.
            {
                float msx = 0.0f, msy = 0.0f;
                unsigned rst = 0;
                auto **vt = *reinterpret_cast<void ***>(const_cast<NVSDK_NGX_Parameter *>(params));
                typedef NVSDK_NGX_Result (STDMETHODCALLTYPE *PFN_GF)(const NVSDK_NGX_Parameter *, const char *, float *);
                typedef NVSDK_NGX_Result (STDMETHODCALLTYPE *PFN_GU)(const NVSDK_NGX_Parameter *, const char *, unsigned *);
                NVSDK_NGX_Result mrx = reinterpret_cast<PFN_GF>(vt[14])(params, NVSDK_NGX_Parameter_MV_Scale_X, &msx);
                NVSDK_NGX_Result mry = reinterpret_cast<PFN_GF>(vt[14])(params, NVSDK_NGX_Parameter_MV_Scale_Y, &msy);
                // 1.0 here may be the game's value or our own fallback below; the
                // reprojection needs to know which, so record the raw answer.
                if (g_jit_burst > 0)
                    logf("[NRPRE] MV raw=(%.6f, %.6f) res=(0x%08X, 0x%08X) net=%ux%u",
                         msx, msy, (unsigned)mrx, (unsigned)mry, w_net, h_net);
                // The official helper sets Reset with SetI, so a uint read may simply
                // fail and leave rst at 0 -- read both and keep whichever answered.
                typedef NVSDK_NGX_Result (STDMETHODCALLTYPE *PFN_GI)(const NVSDK_NGX_Parameter *, const char *, int *);
                int rst_i = 0; unsigned rst_u = 0;
                NVSDK_NGX_Result ri = reinterpret_cast<PFN_GI>(vt[11])(params, NVSDK_NGX_Parameter_Reset, &rst_i);
                NVSDK_NGX_Result ru = reinterpret_cast<PFN_GU>(vt[12])(params, NVSDK_NGX_Parameter_Reset, &rst_u);
                if (ri == NVSDK_NGX_Result_Success && rst_i != 0) rst = 1;
                else if (ru == NVSDK_NGX_Result_Success && rst_u != 0) rst = 1;
                if (msx == 0.0f) msx = 1.0f;
                if (msy == 0.0f) msy = 1.0f;
                pset_f(pp, "DLSSNR.MVecScaleX", msx);
                pset_f(pp, "DLSSNR.MVecScaleY", msy);
                g_mv_scale_x = msx; g_mv_scale_y = msy;   // the codec reprojects with these
                if (g_force_reset_frames > 0) { rst = 1; --g_force_reset_frames; }
                if (rst != 0) g_reset_pending = true;
                pset_u(pp, "DLSSNR.Reset", g_reset_pending ? 1u : 0u); // honour camera cuts
                pset_i(pp, "DLSSNR.Reset", g_reset_pending ? 1 : 0);   // ...and as int
                if (g_diag_frames > 0)
                    logf("[NRPRE] RST gameI=%d(0x%08X) gameU=%u(0x%08X) force=%d pending=%d",
                         rst_i, (unsigned)ri, rst_u, (unsigned)ru,
                         g_force_reset_frames, (int)g_reset_pending);
                pset_u(pp, "DLSSNR.DepthInverted", g_depth_reversed ? 1u : 0u);
                pset_u(pp, "DLSSNR.UseAutoMask", g_auto_mask ? 1u : 0u);
                pset_u(pp, "DLSSNR.Style", (unsigned)g_style);
                pset_f(pp, "DLSSNR.Intensity", g_intensity);
                pset_f(pp, "DLSSNR.LocalToneStrength", g_local_tone);
                pset_f(pp, "DLSSNR.LocalStructureStrength", g_local_structure);
                pset_f(pp, "DLSSNR.SkinStructureStrength", g_skin_structure);
                if (g_eval_logged < 3)
                            {
                    // DLSS solves this exact problem by having the game hand it the
                    // exposure it already computed. If GTA V supplies it, that beats
                    // any histogram estimate we could make.
                    float pre = -1.0f, escale = -1.0f;
                    void *etex = nullptr;
                    reinterpret_cast<PFN_GF>(vt[14])(params, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &pre);
                    reinterpret_cast<PFN_GF>(vt[14])(params, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, &escale);
                    if (g_ptr_slot >= 0)
                        param_get_slot(params, (unsigned)g_ptr_slot,
                                       NVSDK_NGX_Parameter_ExposureTexture, &etex);
                    logf("[NRPRE] exposure: PreExposure=%.4f Scale=%.4f ExposureTexture=%p",
                         pre, escale, etex);
                }
                if (g_eval_logged < 3)
                    logf("[NRPRE] guides: MVecScale=(%.3f, %.3f) reset=%u", msx, msy, rst);
            }
            pset_res(pp, "DLSSNR.Output", out);
            NVSDK_NGX_Result er = NVSDK_NGX_Result_Success;
            // Count evaluates, not presents: ReShade calls on_present about twice
            // per DLSS evaluate, so a present-based counter made "every 2" step by 2
            // and fire on every single frame.
    const unsigned long long tick = g_eval_tick++;
            // Identify the frame by its jitter, not by how many times we were called.
            float cjx = 0.0f, cjy = 0.0f;
            {
                auto **cvt = *reinterpret_cast<void ***>(const_cast<NVSDK_NGX_Parameter *>(params));
                typedef NVSDK_NGX_Result (STDMETHODCALLTYPE *PFN_GFc)(const NVSDK_NGX_Parameter *, const char *, float *);
                reinterpret_cast<PFN_GFc>(cvt[14])(params, NVSDK_NGX_Parameter_Jitter_Offset_X, &cjx);
                reinterpret_cast<PFN_GFc>(cvt[14])(params, NVSDK_NGX_Parameter_Jitter_Offset_Y, &cjy);
            }
            unsigned long long frame = 0;
            const bool first_of_frame = claim_frame(cjx, cjy, &frame);
            // One NR pass per selected frame: the frame's other evaluate reuses the
            // result. Skipped frames keep showing the last one, which is what makes
            // a lower cadence visible at all.
            const bool run_now = first_of_frame
                              && ((g_skip_n <= 1)
                                  || (((frame + g_phase) % (unsigned)g_skip_n) == 0));
            if (g_jit_burst > 0) {
                --g_jit_burst;
                float jx = 0.0f, jy = 0.0f;
                auto **jvt = *reinterpret_cast<void ***>(const_cast<NVSDK_NGX_Parameter *>(params));
                typedef NVSDK_NGX_Result (STDMETHODCALLTYPE *PFN_GFj)(const NVSDK_NGX_Parameter *, const char *, float *);
                NVSDK_NGX_Result jrx = reinterpret_cast<PFN_GFj>(jvt[14])(params, NVSDK_NGX_Parameter_Jitter_Offset_X, &jx);
                NVSDK_NGX_Result jry = reinterpret_cast<PFN_GFj>(jvt[14])(params, NVSDK_NGX_Parameter_Jitter_Offset_Y, &jy);
                ID3D12Resource *jout = nullptr;
                if (g_ptr_slot >= 0)
                    param_get_slot(params, (unsigned)g_ptr_slot, NVSDK_NGX_Parameter_Output,
                                   reinterpret_cast<void **>(&jout));
                D3D12_RESOURCE_DESC jod{}, jsd{};
                if (jout) res_desc(jout, &jod);
                if (src)  res_desc(src, &jsd);
                logf("[NRPRE] JIT tick=%llu frame=%llu first=%d run=%d ph=%u jx=%+.6f jy=%+.6f | feat=%u "
                     "| src=%p %llux%u | out=%p %llux%u | dep=%p mv=%p",
                     (unsigned long long)tick, frame, (int)first_of_frame, (int)run_now,
                     g_phase, jx, jy, feat_id(feat),
                     (void *)src, (unsigned long long)jsd.Width, jsd.Height,
                     (void *)jout, (unsigned long long)jod.Width, jod.Height,
                     (void *)dep, (void *)mv);
            }
            const D3D12_RESOURCE_STATES kUAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            const D3D12_RESOURCE_STATES kSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            const D3D12_RESOURCE_STATES kCS  = D3D12_RESOURCE_STATE_COPY_SOURCE;
            const D3D12_RESOURCE_STATES kCD  = D3D12_RESOURCE_STATE_COPY_DEST;

            g_net_w = w_net; g_net_h = h_net;

            if (use_codec && g_auto_pw && run_now) {
                ensure_recording();
                ID3D12Resource *etex = nullptr;
                if (g_ptr_slot >= 0)
                    param_get_slot(params, (unsigned)g_ptr_slot,
                                   NVSDK_NGX_Parameter_ExposureTexture,
                                   reinterpret_cast<void **>(&etex));
                if (etex != nullptr && g_use_exp_tex) g_pending_exp_tex = etex;
                else record_exposure_sample(cmd, g_device, in, dep, w_net, h_net);
            }

            if (run_now) {
                ensure_recording();
                prof_init(g_device);
                prof_mark(cmd, 0);
                if (use_codec) {
                    // scene-linear HDR -> bounded sRGB proxy that DLSSNR expects
                    codec_dispatch(cmd, g_device, g_cx.pso_enc, in, nullptr, nullptr,
                                   g_cx.proxy, w_net, h_net);
                    uav_barrier(cmd, g_cx.proxy);
                    barrier_transition(cmd, g_cx.proxy, kUAV, kSRV);   // NGX reads inputs as SRV
                }

                // Passes are chained, not repeated: each pass reads the previous
                // pass's result, so the effect compounds instead of re-running the
                // same input N times. The result is copied to 'stage' between
                // passes: a texture cannot be its own input and output at once, and
                // handing the network a buffer it just wrote in the same frame is
                // what broke the D3D12 backend, so 'stage' is only ever an input
                // and the network's output stays 'out' on every pass.
                ID3D12Resource *stage = hi ? g_nr_stage_hi : g_nr_stage;
                if (stage == nullptr) g_repeat = 1;
                ID3D12Resource *cur_in = use_codec ? g_cx.proxy : in;
                bool staged = false;
                for (int k = 0; k < g_repeat; ++k) {
                    pset_res(pp, "DLSSNR.Color",  cur_in);
                    pset_res(pp, "DLSSNR.Output", out);
                    prof_mark(cmd, 1);
                    er = (hi ? g_snip_eval : g_nr_eval)(cmd, hh, pp, nullptr);
                    uav_barrier(cmd, out);
                    prof_mark(cmd, 2);
                    if (er != NVSDK_NGX_Result_Success) break;
                    if (k + 1 < g_repeat) {
                        barrier_transition(cmd, out,   kUAV, kCS);
                        barrier_transition(cmd, stage, staged ? kSRV : kUAV, kCD);
                        cmd->CopyResource(stage, out);
                        barrier_transition(cmd, out,   kCS, kUAV);
                        barrier_transition(cmd, stage, kCD, kSRV);
                        cur_in = stage;
                        staged = true;
                    }
                }
                // After a staged pass the stage buffer sits in SRV (it was the
                // last input); park it in UAV so the next frame can stage into it.
                if (staged) barrier_transition(cmd, stage, kSRV, kUAV);
                if (g_async_net && g_cx.snap_ready) {
                    // back to UAV so the next frame's snapshot can write them again
                    barrier_transition(cmd, g_cx.snap_color,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    barrier_transition(cmd, g_cx.snap_depth,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    barrier_transition(cmd, g_cx.snap_mv,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
                ++g_eval_count;
                g_reset_pending = (er != NVSDK_NGX_Result_Success);
                if (er != NVSDK_NGX_Result_Success) {
                    ++g_er_fail;
                    g_final_valid = g_delta_valid = false;
                    if (g_er_fail <= 3) ring_dump("NR evaluate failed before rebind");
                    if (g_er_fail <= 20 || g_er_fail % 200 == 0)
                        logf("[NRPRE] NR Evaluate failed 0x%08X; current input passthrough (%u)",
                             (unsigned)er, g_er_fail);
                    if (use_codec) barrier_transition(cmd, g_cx.proxy, kSRV, kUAV);
                }

                if (use_codec && er == NVSDK_NGX_Result_Success) {
                    barrier_transition(cmd, out, kUAV, kSRV);          // decode reads NR output
                    // Uniform mode: this frame is developed from the delta that is
                    // already stored, exactly as the frames between are, so no frame
                    // is built differently from its neighbours. The result the
                    // network just produced becomes the *next* frame's delta, below.
                    // Needs a delta to exist, so the first run frame still decodes.
                    const bool uniform = g_uniform_delta && g_delta_reuse
                                      && g_delta_valid && g_skip_n > 1;
                    if (uniform) {
                        if (g_delta_advect)
                            advect_delta(cmd, g_device, in, mv, dep, w_net, h_net);
                        barrier_transition(cmd, g_cx.delta, kUAV, kSRV);
                        codec_dispatch(cmd, g_device, g_cx.pso_apply, in, g_cx.proxy,
                                       g_cx.delta, g_cx.final_tex, w_net, h_net, mv, dep);
                        uav_barrier(cmd, g_cx.final_tex);
                        barrier_transition(cmd, g_cx.delta, kSRV, kUAV);
                    } else {
                        codec_dispatch(cmd, g_device, g_cx.pso_dec, in, g_cx.proxy, out,
                                       g_cx.final_tex, w_net, h_net);
                        uav_barrier(cmd, g_cx.final_tex);
                    }
                    // Capture what the network changed, while both inputs are still SRV.
                    if (g_delta_reuse && g_skip_n > 1) {
                        codec_dispatch(cmd, g_device, g_cx.pso_delta, in, g_cx.proxy, out,
                                       g_cx.delta, w_net, h_net, nullptr, dep);
                        uav_barrier(cmd, g_cx.delta);
                        g_delta_valid = true;
                        g_delta_age = 1;   // fresh again
                    }
                    barrier_transition(cmd, g_cx.proxy, kSRV, kUAV);   // restore for next frame
                    barrier_transition(cmd, out,        kSRV, kUAV);
                    g_final_valid = true;         // only now does final_tex hold an image
                }
                prof_mark(cmd, 3);
                prof_resolve(cmd);
            } else if (use_codec && g_delta_reuse && g_delta_valid) {
                ensure_recording();
                // Skipped frame: develop the *current* colour and re-apply the stored
                // effect to it. No network, so the saving stands, but the image being
                // handed on is built from this frame rather than the previous one.
                codec_dispatch(cmd, g_device, g_cx.pso_enc, in, nullptr, nullptr,
                               g_cx.proxy, w_net, h_net);
                uav_barrier(cmd, g_cx.proxy);
                barrier_transition(cmd, g_cx.proxy, kUAV, kSRV);
                if (g_delta_advect) advect_delta(cmd, g_device, in, mv, dep, w_net, h_net);
                barrier_transition(cmd, g_cx.delta, kUAV, kSRV);
                codec_dispatch(cmd, g_device, g_cx.pso_apply, in, g_cx.proxy, g_cx.delta,
                               g_cx.final_tex, w_net, h_net, mv, dep);
                uav_barrier(cmd, g_cx.final_tex);
                barrier_transition(cmd, g_cx.proxy, kSRV, kUAV);
                barrier_transition(cmd, g_cx.delta, kSRV, kUAV);
                g_final_valid = true;
                if (g_delta_age < 8) ++g_delta_age;   // one more frame behind
            }

            // --- 2d: hand the processed image to the game's DLSS as its colour input.
            // Never feed a texture we have not written yet: uninitialised memory
            // shows up as a black screen.
            //
            // On a frame we skipped, the cached NR result belongs to the previous
            // frame while the motion vectors and jitter belong to this one. Feeding
            // it anyway is stale colour against fresh motion, which DLSS resolves as
            // ghosting and judder. Passing the game's own colour through instead
            // costs the NR look on that frame but keeps the input aligned.
            if (g_descriptor_failed) {
                g_final_valid = g_delta_valid = false;
                g_reset_pending = true;
            }
            ID3D12Resource *feed = (er != NVSDK_NGX_Result_Success || g_descriptor_failed)
                ? nullptr : (use_codec ? (g_final_valid ? g_cx.final_tex : nullptr) : out);
            const bool delta_fed = use_codec && g_delta_reuse && g_delta_valid && g_skip_n > 1;
            // !run_now covers two different situations and they need opposite
            // answers. One is a frame the cadence genuinely skipped. The other is a
            // *repeat* evaluate of the frame we just processed -- the game issues
            // more than one per frame and only the first claims it -- and there
            // final_tex already holds this very frame, current and enhanced.
            // Passing the raw colour on a repeat threw the work away on 43% of
            // evaluates here, leaving whether the effect is seen at all depending on
            // which evaluate's output reached the screen.
            const bool repeat_of_this_frame = !first_of_frame && g_final_valid;
            if (!run_now && !repeat_of_this_frame && g_skip_passthrough && !delta_fed) {
                feed = nullptr;                                   // let src go through untouched
                ++g_pass_frames;   // the effect is fully off on this frame: count it
            }
            if (g_rebind && !hi && feed != nullptr) {
                ensure_recording();
                barrier_transition(cmd, feed, kUAV, kSRV);
                pset_res(const_cast<NVSDK_NGX_Parameter *>(params),
                         NVSDK_NGX_Parameter_Color, feed);
                NVSDK_NGX_Result gr = t_orig_eval(cmd, feat, params, cb);
                if (gr != NVSDK_NGX_Result_Success) {
                    ++g_gr_fail;
                    if (g_gr_fail <= 3) ring_dump("fallo del evaluate de DLSS");
                    if (g_gr_fail <= 20 || (g_gr_fail % 200) == 0)
                        logf("[NRPRE] *** DLSS Evaluate FAILED -> 0x%08X  (fallo #%u, tick %llu, feed=%p run=%d)",
                             (unsigned)gr, g_gr_fail, (unsigned long long)tick,
                             (void *)feed, (int)run_now);
                }
                pset_res(const_cast<NVSDK_NGX_Parameter *>(params),
                         NVSDK_NGX_Parameter_Color, src);
                barrier_transition(cmd, feed, kSRV, kUAV);
                if (g_diag_frames > 0) {
                    --g_diag_frames;
                    D3D12_RESOURCE_DESC fd{}, sd2{};
                    res_desc(feed, &fd); res_desc(src, &sd2);
                    logf("[NRPRE] DIAG tick=%llu run=%d skip=%d codec=%d finalvalid=%d "
                         "feed=%p %llux%u fmt=%d | src=%p %llux%u fmt=%d | nr=%p reset=%d DLSS=0x%08X",
                         (unsigned long long)tick, (int)run_now, g_skip_n, (int)use_codec,
                         (int)g_final_valid,
                         (void *)feed, (unsigned long long)fd.Width, fd.Height, (int)fd.Format,
                         (void *)src, (unsigned long long)sd2.Width, sd2.Height, (int)sd2.Format,
                         (void *)hh, g_force_reset_frames, (unsigned)gr);
                } else if (g_eval_logged < 6) {
                    logf("[NRPRE] 3: codec=%d ran=%d rebound colour, DLSS -> 0x%08X",
                         (int)use_codec, (int)run_now, (unsigned)gr);
                    ++g_eval_logged;
                }
                ring_push(tick, feed, src, (unsigned)er, (unsigned)gr, w_net, h_net,
                          run_now, use_codec, g_final_valid, g_delta_valid, false);
                return gr;
            }
            if (g_diag_frames > 0) {
                --g_diag_frames;
                logf("[NRPRE] DIAG tick=%llu run=%d skip=%d codec=%d finalvalid=%d NO REBIND (feed=%p)",
                     (unsigned long long)tick, (int)run_now, g_skip_n, (int)use_codec,
                     (int)g_final_valid, (void *)feed);
            }
            ring_push(tick, feed, src, (unsigned)er, 0u, w_net, h_net,
                      run_now, use_codec, g_final_valid, g_delta_valid, feed == nullptr);
            if (er == NVSDK_NGX_Result_Success && g_eval_logged < 5) {
                logf("[NRPRE] 2c: NR Evaluate(%s) -> 0x%08X", hi ? "HI" : "LO", (unsigned)er);
                ++g_eval_logged;
            }
        }
    }
    return t_orig_eval(cmd, feat, params, cb);
}

// A proxy forwards to the driver, so one game call can enter our hooks twice.
// Only the outermost interception should do any work; the inner one is just a
// link in the chain and must pass straight through, or the network would run
// twice on the same frame and the colour would be developed twice over.
static thread_local int t_depth = 0;
static thread_local unsigned t_bypassed_feature = ~0u;

static NVSDK_NGX_Result eval_dispatch(unsigned idx, ID3D12GraphicsCommandList *cmd,
        const NVSDK_NGX_Handle *feat, const NVSDK_NGX_Parameter *params,
        PFN_NVSDK_NGX_ProgressCallback cb)
{
    PFN_Eval orig = g_orig_eval_n[idx];
    if (orig == nullptr) return NVSDK_NGX_Result_Fail;
    if (t_depth > 0) {
        // Observe whether the non-DLSS nesting guard hides an actual upscaler.
        // Do not infer that from repeated Color parameters on FG evaluations.
        if (t_bypassed_feature != ~0u) {
            std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
            const auto *nested = g_features.find(idx, feat);
            if (nested && nested->allows_nr()) {
                ++g_nested_dlss_bypass;
                if (g_nested_dlss_bypass <= 3)
                    logf("[NRPRE] nested DLSS bypass: outer feature=%u inner feature=%u module=%u handle=%p",
                         t_bypassed_feature, nested->feature, idx, (void *)feat);
            }
        }
        return orig(cmd, feat, params, cb);
    }

    std::unique_lock<std::recursive_mutex> lock(g_state_mutex);
    auto *contract = g_features.find(idx, feat);
    if (!contract || !contract->allows_nr()) {
        if (!contract) {
            ++g_unknown_bypass;
            static bool reported[kMaxNgx] = {};
            if (!reported[idx]) {
                reported[idx] = true;
                logf("[NRPRE] feature gate: unknown handle=%p module=%u; passthrough until creation is observed",
                     (void *)feat, idx);
            }
        } else {
            if (contract->feature == NVSDK_NGX_Feature_FrameGeneration) ++g_fg_bypass;
            if (!contract->reported) {
                contract->reported = true;
                logf("[NRPRE] feature gate: bypass feature=%u handle=%p module=%u (no NR work or Color rebind)",
                     contract->feature, (void *)feat, idx);
            }
        }
        // An unknown outer proxy may forward to a known core handle. Let that
        // inner hook decide. A known non-DLSS feature suppresses the whole chain.
        const bool known_other = contract != nullptr;
        const unsigned bypassed_feature = contract ? contract->feature : ~0u;
        lock.unlock();
        const unsigned saved_bypassed_feature = t_bypassed_feature;
        if (known_other) { ++t_depth; t_bypassed_feature = bypassed_feature; }
        const NVSDK_NGX_Result result = orig(cmd, feat, params, cb);
        if (known_other) { --t_depth; t_bypassed_feature = saved_bypassed_feature; }
        return result;
    }
    if (!contract->reported) {
        contract->reported = true;
        logf("[NRPRE] feature gate: allow feature=%u handle=%p module=%u",
             contract->feature, (void *)feat, idx);
    }
    PFN_Eval saved = t_orig_eval;
    const unsigned saved_provider = t_ngx_provider;
    t_orig_eval = orig;
    t_ngx_provider = idx;
    ++t_depth;
    NVSDK_NGX_Result r = eval_body(cmd, feat, params, cb);
    --t_depth;
    t_orig_eval = saved;
    t_ngx_provider = saved_provider;
    return r;
}

template <unsigned N>
static NVSDK_NGX_Result NVSDK_CONV hk_eval_t(ID3D12GraphicsCommandList *cmd,
        const NVSDK_NGX_Handle *feat, const NVSDK_NGX_Parameter *params,
        PFN_NVSDK_NGX_ProgressCallback cb)
{
    return eval_dispatch(N, cmd, feat, params, cb);
}

static void *const kEvalThunks[kMaxNgx] = {
    (void *)&hk_eval_t<0>, (void *)&hk_eval_t<1>, (void *)&hk_eval_t<2>, (void *)&hk_eval_t<3>,
    (void *)&hk_eval_t<4>, (void *)&hk_eval_t<5>, (void *)&hk_eval_t<6>, (void *)&hk_eval_t<7>,
};

// ---- CreateFeature hook: learn the DLSSNR feature id and the params it is given ----
typedef NVSDK_NGX_Result (STDMETHODCALLTYPE *PFN_GetU)(const NVSDK_NGX_Parameter *, const char *, unsigned *);
typedef NVSDK_NGX_Result (STDMETHODCALLTYPE *PFN_GetF)(const NVSDK_NGX_Parameter *, const char *, float *);
static unsigned pget_u(const NVSDK_NGX_Parameter *p, const char *k) {
    auto **v = *reinterpret_cast<void ***>(const_cast<NVSDK_NGX_Parameter *>(p));
    unsigned out = 0;                                   // MSVC slot 12 = Get(unsigned int*)
    if (reinterpret_cast<PFN_GetU>(v[12])(p, k, &out) != NVSDK_NGX_Result_Success) return 0xFFFFFFFFu;
    return out;
}
static float pget_f(const NVSDK_NGX_Parameter *p, const char *k) {
    auto **v = *reinterpret_cast<void ***>(const_cast<NVSDK_NGX_Parameter *>(p));
    float out = -1.0f;                                  // MSVC slot 14 = Get(float*)
    reinterpret_cast<PFN_GetF>(v[14])(p, k, &out);
    return out;
}

typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_Create)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature,
        NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
static PFN_Create g_orig_create_n[kMaxNgx] = {};
static PFN_ReleaseFeature g_orig_release_n[kMaxNgx] = {};

template <unsigned Index>
static NVSDK_NGX_Result NVSDK_CONV hk_create_t(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Feature id,
        NVSDK_NGX_Parameter *params, NVSDK_NGX_Handle **out)
{
    const bool dlss = id == NVSDK_NGX_Feature_SuperSampling || id == NVSDK_NGX_Feature_RayReconstruction;
    unsigned flags = 0;
    const bool flags_valid = dlss && read_dlss_flags(params, flags);
    NVSDK_NGX_Result r = g_orig_create_n[Index](cmd, id, params, out);
    if (r == NVSDK_NGX_Result_Success && out && *out) {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        g_features.created(Index, *out, static_cast<unsigned>(id), flags, flags_valid);
        logf("[NRPRE] feature contract: module=%u feature=%u handle=%p id=%u flags=0x%08X valid=%d",
             Index, (unsigned)id, (void *)*out, feat_id(*out), flags, (int)flags_valid);
    }
    char color[128], output[128], depth[128], mvec[128];
    describe(params, "DLSSNR.Color",  color,  sizeof color);
    describe(params, "DLSSNR.Output", output, sizeof output);
    describe(params, "DLSSNR.Depth",  depth,  sizeof depth);
    describe(params, "DLSSNR.MVec",   mvec,   sizeof mvec);
    logf("[NRPRE] CreateFeature id=%d -> res=0x%08X handle=%u | NR W=%u H=%u ratio=%.3f preset=%d | %s | %s | %s | %s",
         (int)id, (unsigned)r, (r == NVSDK_NGX_Result_Success && out && *out) ? feat_id(*out) : 0u,
         pget_u(params, "DLSSNR.Width"), pget_u(params, "DLSSNR.Height"),
         pget_f(params, "DLSSNR.ScalingRatio"), (int)pget_u(params, "DLSSNR.Hint.Render.Preset"),
         color, output, depth, mvec);
    return r;
}

template <unsigned Index>
static NVSDK_NGX_Result NVSDK_CONV hk_release_t(NVSDK_NGX_Handle *handle) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    const NVSDK_NGX_Result result = g_orig_release_n[Index](handle);
    if (result == NVSDK_NGX_Result_Success) g_features.released(Index, handle);
    return result;
}

static const PFN_Create kCreateThunks[kMaxNgx] = {
    hk_create_t<0>, hk_create_t<1>, hk_create_t<2>, hk_create_t<3>,
    hk_create_t<4>, hk_create_t<5>, hk_create_t<6>, hk_create_t<7>,
};
static const PFN_ReleaseFeature kReleaseThunks[kMaxNgx] = {
    hk_release_t<0>, hk_release_t<1>, hk_release_t<2>, hk_release_t<3>,
    hk_release_t<4>, hk_release_t<5>, hk_release_t<6>, hk_release_t<7>,
};

// ---- snippet-level hook: NR is created via nvngx_dlssnr.dll directly ----
static PFN_Create g_orig_snip_create = nullptr;

static NVSDK_NGX_Result NVSDK_CONV hk_snip_create(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Feature id,
        NVSDK_NGX_Parameter *params, NVSDK_NGX_Handle **out)
{
    NVSDK_NGX_Result r = g_orig_snip_create(cmd, id, params, out);
    char c[128], o[128], dp[128], mv[128];
    describe(params, "DLSSNR.Color",  c,  sizeof c);
    describe(params, "DLSSNR.Output", o,  sizeof o);
    describe(params, "DLSSNR.Depth",  dp, sizeof dp);
    describe(params, "DLSSNR.MVec",   mv, sizeof mv);
    logf("[NRPRE] SNIPPET CreateFeature id=%d res=0x%08X handle=%u | W=%u H=%u ratio=%.3f preset=%d style=%d "
         "automask=%u depthinv=%u mvsx=%.3f mvsy=%.3f | %s | %s | %s | %s",
         (int)id, (unsigned)r, (out && *out) ? feat_id(*out) : 0u,
         pget_u(params, "DLSSNR.Width"), pget_u(params, "DLSSNR.Height"),
         pget_f(params, "DLSSNR.ScalingRatio"), (int)pget_u(params, "DLSSNR.Hint.Render.Preset"),
         (int)pget_u(params, "DLSSNR.Style"), pget_u(params, "DLSSNR.UseAutoMask"),
         pget_u(params, "DLSSNR.DepthInverted"),
         pget_f(params, "DLSSNR.MVecScaleX"), pget_f(params, "DLSSNR.MVecScaleY"),
         c, o, dp, mv);
    return r;
}

static bool g_snip_hooked = false;
static void install_snippet_hook() {
    if (g_snip_hooked) return;
    HMODULE m = GetModuleHandleW(L"nvngx_dlssnr.dll");
    if (m == nullptr) return;                     // the module loads lazily when NR is on
    g_snip_hooked = true;
    auto ct = GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature");
    if (ct == nullptr) { logf("[NRPRE] snippet CreateFeature export missing"); return; }
    if (MH_CreateHook(reinterpret_cast<LPVOID>(ct), reinterpret_cast<LPVOID>(&hk_snip_create),
                      reinterpret_cast<LPVOID *>(&g_orig_snip_create)) != MH_OK) {
        logf("[NRPRE] snippet MH_CreateHook failed"); return;
    }
    MH_STATUS s = MH_EnableHook(reinterpret_cast<LPVOID>(ct));
    logf("[NRPRE] hook on nvngx_dlssnr!NVSDK_NGX_D3D12_CreateFeature: %s (mod=%p)",
         s == MH_OK ? "OK" : "FAILED", (void *)m);
}

// ================= stage 2a: bring up our own DLSSNR runtime =================
// The snippet (nvngx_dlssnr.dll) exports Init/CreateFeature/EvaluateFeature but NOT
// AllocateParameters -- parameter objects come from the NGX core (_nvngx.dll).
static const NVSDK_NGX_Feature kFeatureDLSSNR = (NVSDK_NGX_Feature)18;   // Reserved18

typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_AllocParams)(NVSDK_NGX_Parameter **);
// The snippet's Init_Ext (5 args) only accepts this app id:
//   mov ecx, 0x876232c ; r9d = 0x15 ; [rsp+0x20] = nullptr ; call NVSDK_NGX_D3D12_Init_Ext
#ifdef NR_STANDALONE
// Standalone this file is compiled into nvngx.dll.nr and calls the snippet
// directly, exactly as the ReShade build calls it from nvngx.dll.addon64. The
// file name is the gate, and this module carries it.
//
// A thin forwarder was tried first -- the add-on in version.dll and four exports
// in a satellite -- and it fails in a way worth remembering: Init_Ext is accepted
// and CreateFeature enters and never returns. The snippet is not merely checking
// a return address; whatever it reconciles at creation has to belong to the same
// module that initialised it.
extern HMODULE g_satellite_module;
#endif
#define SNIP_ENTRY(mod, name, sat_name) GetProcAddress(mod, name)
static const unsigned long long kNgxAppId = 0x0876232Cull;
typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_SnipInitExt)(unsigned long long, const wchar_t *,
                                                       ID3D12Device *, NVSDK_NGX_Version,
                                                       const NVSDK_NGX_Parameter *);
typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_ScratchSize)(NVSDK_NGX_Feature,
                                                       const NVSDK_NGX_Parameter *, size_t *);

static HMODULE            g_self_module = nullptr;
static size_t g_scratch_size = 0;

static void setup_nr(unsigned w, unsigned h, ID3D12GraphicsCommandList *cmd) {
    if (g_setup_done) return;
    // The DLSSNR runtime initialises lazily, ~150ms after the module appears.
    // Keep retrying Init_Ext frame by frame until it takes (or we run out of patience).
    // Everything goes through the NGX core (_nvngx.dll), which the game already
    // initialised -- the snippet DLL is loaded and driven by the core, not by us.

    // Prefer the device that owns the command list DLSS is actually recording into.
    ID3D12Device *dev_from_cmd = nullptr;
    if (cmd != nullptr) {
        using pfn_getdev = HRESULT (STDMETHODCALLTYPE *)(void *, REFIID, void **);
        auto **vt = *reinterpret_cast<void ***>(cmd);      // slot 7 = GetDevice
        static const GUID iid_dev = {0x189819f1,0x1db6,0x4b57,{0xbe,0x54,0x18,0x21,0x33,0x9b,0x85,0xf7}};
        reinterpret_cast<pfn_getdev>(vt[7])(cmd, iid_dev, reinterpret_cast<void **>(&dev_from_cmd));
    }
    logf("[NRPRE] 2a: device reshade=%p cmdlist=%p", (void *)g_device, (void *)dev_from_cmd);
    if (dev_from_cmd != nullptr) g_device = dev_from_cmd;
    if (g_device == nullptr) { logf("[NRPRE] 2a: no ID3D12Device"); return; }

    HMODULE core = GetModuleHandleW(L"_nvngx.dll");
    auto alloc_params = reinterpret_cast<PFN_AllocParams>(
        core ? GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters") : nullptr);
    logf("[NRPRE] 2b: core=%p alloc=%p", (void *)core, (void *)alloc_params);
    if (core == nullptr || alloc_params == nullptr) {
        logf("[NRPRE] 2b: core exports missing"); g_setup_done = true; return;
    }

    NVSDK_NGX_Result r = NVSDK_NGX_Result_Success;
    if (g_nr_params == nullptr) {
        r = alloc_params(&g_nr_params);
        logf("[NRPRE] 2a: AllocateParameters -> 0x%08X params=%p", (unsigned)r, (void *)g_nr_params);
        if (r != NVSDK_NGX_Result_Success || g_nr_params == nullptr) return;
    }

    pset_u(g_nr_params, "DLSSNR.Width",  w);
    pset_u(g_nr_params, "DLSSNR.Height", h);
    pset_u(g_nr_params, "DLSSNR.Enabled", 1);

    // The game already initialised the NGX core for this device, so we piggyback on it:
    // a SECOND Init is exactly what kept returning 0xBAD00002.
    auto core_scratch = reinterpret_cast<PFN_ScratchSize>(GetProcAddress(core, "NVSDK_NGX_D3D12_GetScratchBufferSize"));
    auto core_create  = reinterpret_cast<PFN_Create>(GetProcAddress(core, "NVSDK_NGX_D3D12_CreateFeature"));
    g_destroy_params = reinterpret_cast<PFN_DestroyParams>(GetProcAddress(core, "NVSDK_NGX_D3D12_DestroyParameters"));
    if (!core_scratch || !core_create) { logf("[NRPRE] 2b: core exports missing"); g_setup_done = true; return; }

    // The DLSSNR snippet refuses any caller whose module path lacks "nvngx.dll"
    // ("Error: Not called from NGX runtime"), so the CORE has to make the call.
    // The core only failed before because it could not locate a DLSSNR snippet:
    // the driver store ships nvngx_dlssg.dll only. Point it at the game folder.
    {
        wchar_t exe[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wchar_t *slash = wcsrchr(exe, (wchar_t)92);   // backslash
        if (slash) *slash = (wchar_t)0;
        static wchar_t dir[MAX_PATH];
        wcscpy(dir, exe);
        static const wchar_t *plist[1] = { dir };

        struct { const wchar_t *const *Path; unsigned Length; } pathlist = { plist, 1 };
        struct { decltype(pathlist) PathListInfo; void *InternalData; char Logging[32]; } info{};
        info.PathListInfo = pathlist;

        typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_CoreInit)(unsigned long long, const wchar_t *,
                ID3D12Device *, const void *, NVSDK_NGX_Version);
        auto core_init = reinterpret_cast<PFN_CoreInit>(GetProcAddress(core, "NVSDK_NGX_D3D12_Init"));
        if (core_init) {
            NVSDK_NGX_Result ri = core_init(kNgxAppId, dir, g_device, &info, (NVSDK_NGX_Version)0x15);
            logf("[NRPRE] 2b: CORE Init(pathlist=%S) -> 0x%08X", dir, (unsigned)ri);
        }
    }

    size_t bytes = 0;
    r = core_scratch(kFeatureDLSSNR, g_nr_params, &bytes);
    g_scratch_size = bytes;
    logf("[NRPRE] 2b: scratch(18) -> 0x%08X size=%llu", (unsigned)r, (unsigned long long)bytes);
    // A community Ada runtime is not vetted by the NGX core, so the core refuses
    // feature 18 and this query fails. That is expected there, and it is not a
    // reason to stop: the snippet route below needs neither, and it is the only
    // path that works on those runtimes. g_scratch_size is never read anywhere.
    if (r != NVSDK_NGX_Result_Success) {
        logf("[NRPRE] 2b: core scratch unavailable (0x%08X) - continuing to the snippet route", (unsigned)r);
    }

    if (cmd == nullptr) { logf("[NRPRE] 2b: setup deferred, no command list"); return; }

    pset_u(g_nr_params, "DLSSNR.Hint.Render.Preset", 0);
    pset_u(g_nr_params, "DLSSNR.DepthInverted", g_depth_reversed ? 1u : 0u);
    pset_f(g_nr_params, "DLSSNR.ScalingRatio", g_net_scale);
    logf("[NRPRE] 2b: ScalingRatio=%.3f (1.0 = misma resolucion dentro y fuera)", g_net_scale);
    pset_u(g_nr_params, "DLSSNR.UseAutoMask", g_auto_mask ? 1u : 0u);
    pset_u(g_nr_params, "DLSSNR.Style", (unsigned)g_style);
    // Geometry. Without these the subrect defaults to zero and the network is asked
    // to process a 0x0 region: every evaluate returns success and does no work.
    // On an RTX 4090 with the 310.8.0-RTX40 runtime this is the difference between
    // 0.000 ms and 8.0 ms of real network time.
    pset_u(g_nr_params, "DLSSNR.InputWidth",   w);
    pset_u(g_nr_params, "DLSSNR.InputHeight",  h);
    pset_u(g_nr_params, "DLSSNR.OutputWidth",  w);
    pset_u(g_nr_params, "DLSSNR.OutputHeight", h);
    static const char *const kSubW[] = { "DLSSNR.ColorSubrectWidth",  "DLSSNR.DepthSubrectWidth",
                                         "DLSSNR.MVecSubrectWidth",   "DLSSNR.OutputSubrectWidth" };
    static const char *const kSubH[] = { "DLSSNR.ColorSubrectHeight", "DLSSNR.DepthSubrectHeight",
                                         "DLSSNR.MVecSubrectHeight",  "DLSSNR.OutputSubrectHeight" };
    static const char *const kSubB[] = { "DLSSNR.ColorSubrectBaseX",  "DLSSNR.ColorSubrectBaseY",
                                         "DLSSNR.DepthSubrectBaseX",  "DLSSNR.DepthSubrectBaseY",
                                         "DLSSNR.MVecSubrectBaseX",   "DLSSNR.MVecSubrectBaseY",
                                         "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY" };
    for (int i = 0; i < 4; ++i) { pset_u(g_nr_params, kSubW[i], w); pset_u(g_nr_params, kSubH[i], h); }
    for (int i = 0; i < 8; ++i) pset_u(g_nr_params, kSubB[i], 0);

    pset_u(g_nr_params, "CreationNodeMask", 1);
    pset_u(g_nr_params, "VisibilityNodeMask", 1);

    if (!g_nr_out)
        g_nr_out = make_uav_tex(g_device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!g_nr_out) { logf("[NRPRE] NR output allocation failed"); return; }
    pset_res(g_nr_params, "DLSSNR.Output", g_nr_out);
    if (!g_nr_stage)
        g_nr_stage = make_uav_tex(g_device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!g_nr_stage) logf("[NRPRE] NR staging buffer allocation failed; chained passes limited to one");
    g_setup_w = w; g_setup_h = h;

    auto core_eval = reinterpret_cast<PFN_Eval>(GetProcAddress(core, "NVSDK_NGX_D3D12_EvaluateFeature"));
    auto core_release = reinterpret_cast<PFN_ReleaseFeature>(GetProcAddress(core, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if (!core_eval || !core_release) {
        logf("[NRPRE] core evaluate/release exports missing");
        g_setup_done = true;
        return;
    }
    track_recording(cmd); // CreateFeature may record GPU initialization too.
    r = core_create(cmd, kFeatureDLSSNR, g_nr_params, &g_nr_handle);
    if (g_nr_handle) {
        g_nr_eval = core_eval;
        g_nr_release = core_release;
        g_nr_core_owned = true;
    }
    logf("[NRPRE] 2b: CORE CreateFeature(18, %ux%u) -> 0x%08X handle=%p",
         w, h, (unsigned)r, (void *)g_nr_handle);

    if (g_nr_handle == nullptr) {
        // Core route is dead (it will not instantiate feature 18 from a snippet it
        // did not vet). Go straight to the snippet instead: its only gate is a
        // wcsstr(module_path_of_caller, L"nvngx.dll") -- and this addon is named
        // nvngx.dll.addon64 precisely so that substring is present.
        HMODULE snip = GetModuleHandleW(L"nvngx_dlssnr.dll");
        if (snip == nullptr) snip = LoadLibraryW(L"nvngx_dlssnr.dll");
        wchar_t self[MAX_PATH] = L"";
    #ifdef NR_STANDALONE
    // Without its own DllMain there is no self-handle; the satellite's is ours.
    GetModuleFileNameW(g_satellite_module, self, MAX_PATH);
#else
    GetModuleFileNameW(g_self_module, self, MAX_PATH);
#endif
        logf("[NRPRE] 2b: snippet route, self=%S", self);
        if (snip != nullptr) {
            auto si = reinterpret_cast<PFN_SnipInitExt>(SNIP_ENTRY(snip, "NVSDK_NGX_D3D12_Init_Ext", "nr_init"));
            auto sc = reinterpret_cast<PFN_Create>(SNIP_ENTRY(snip, "NVSDK_NGX_D3D12_CreateFeature", "nr_create"));
            auto se = reinterpret_cast<PFN_Eval>(SNIP_ENTRY(snip, "NVSDK_NGX_D3D12_EvaluateFeature", "nr_eval"));
            auto sr = reinterpret_cast<PFN_ReleaseFeature>(SNIP_ENTRY(snip, "NVSDK_NGX_D3D12_ReleaseFeature", "nr_release"));
            // Initialise the snippet at most once per process. The retry loop above
            // exists because the runtime comes up lazily, but re-initialising one that
            // is already up is a different thing entirely -- it hung the game to a
            // black screen the first time this ran standalone.
            static bool s_init_done = false;
            static NVSDK_NGX_Result s_init_res = NVSDK_NGX_Result_Fail;
            if (si && sc && se && sr) {
                wchar_t lp[MAX_PATH] = L"";
                GetEnvironmentVariableW(L"LOCALAPPDATA", lp, MAX_PATH);
                NVSDK_NGX_Result ri = s_init_res;
                if (!s_init_done) {
                    s_init_done = true;
                    ri = s_init_res = si(kNgxAppId, lp[0] ? lp : L".", g_device,
                                         (NVSDK_NGX_Version)0x15, nullptr);
                    logf("[NRPRE] 2b: SNIPPET Init_Ext -> 0x%08X", (unsigned)ri);
                }
                if (ri == NVSDK_NGX_Result_Success) {
                    // Logged on both sides of the call: standalone this is where the
                    // process stops without a word, and the pair of lines is what
                    // distinguishes "never entered" from "entered and never returned".
                    logf("[NRPRE] 2b: -> SNIPPET CreateFeature sc=%p cmd=%p params=%p",
                         (void *)sc, (void *)cmd, (void *)g_nr_params);
                    r = sc(cmd, kFeatureDLSSNR, g_nr_params, &g_nr_handle);
                    if (g_nr_handle) {
                        g_nr_eval = g_snip_eval = se;
                        g_nr_release = sr;
                        g_nr_core_owned = false;
                    }
                    logf("[NRPRE] 2b: <- returned");
                    logf("[NRPRE] 2b: SNIPPET CreateFeature(18, %ux%u) -> 0x%08X handle=%p",
                         w, h, (unsigned)r, (void *)g_nr_handle);
                }
                // Whatever happened, stop here. Retrying a creation that failed once
                // has never produced a handle, and doing it every frame is how a
                // failure becomes a hang instead of simply no effect.
                if (g_nr_handle == nullptr) { g_setup_done = true; return; }
            }
        }
    }
    if (g_nr_handle != nullptr) {
        HMODULE snip = GetModuleHandleW(L"nvngx_dlssnr.dll");
        if (snip) g_snip_eval = reinterpret_cast<PFN_Eval>(
                      SNIP_ENTRY(snip, "NVSDK_NGX_D3D12_EvaluateFeature", "nr_eval"));
        logf("[NRPRE] 2c: ready out=%p eval=%p", (void *)g_nr_out, (void *)g_snip_eval);
    }
    g_setup_done = true;
}


// Built only when F8 is first pressed: keeping two 147 MB weight heaps alive at
// once is a benchmark artefact, not how this would ship.
static void build_hi(ID3D12GraphicsCommandList *cmd) {
    if (g_nr_handle_hi != nullptr || g_device == nullptr || cmd == nullptr) return;
    HMODULE core = GetModuleHandleW(L"_nvngx.dll");
    auto alloc_params = reinterpret_cast<PFN_AllocParams>(
        core ? GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters") : nullptr);
        HMODULE snip2 = GetModuleHandleW(L"nvngx_dlssnr.dll");
    auto sc2 = snip2 ? reinterpret_cast<PFN_Create>(
                       GetProcAddress(snip2, "NVSDK_NGX_D3D12_CreateFeature")) : nullptr;
    auto sr2 = snip2 ? reinterpret_cast<PFN_ReleaseFeature>(
                       SNIP_ENTRY(snip2, "NVSDK_NGX_D3D12_ReleaseFeature", "nr_release")) : nullptr;
        if (sc2 && sr2 && g_snip_eval && alloc_params &&
            alloc_params(&g_nr_params_hi) == NVSDK_NGX_Result_Success && g_nr_params_hi) {
            pset_u(g_nr_params_hi, "DLSSNR.Width",  g_out_w);
            pset_u(g_nr_params_hi, "DLSSNR.Height", g_out_h);
            pset_u(g_nr_params_hi, "DLSSNR.Enabled", 1);
            pset_u(g_nr_params_hi, "DLSSNR.Hint.Render.Preset", 0);
            pset_u(g_nr_params_hi, "DLSSNR.DepthInverted", g_depth_reversed ? 1u : 0u);
            pset_f(g_nr_params_hi, "DLSSNR.ScalingRatio", 1.0f);
            pset_u(g_nr_params_hi, "CreationNodeMask", 1);
            pset_u(g_nr_params_hi, "VisibilityNodeMask", 1);
            g_nr_out_hi = make_uav_tex(g_device, g_out_w, g_out_h, DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (!g_nr_out_hi) return;
            pset_res(g_nr_params_hi, "DLSSNR.Output", g_nr_out_hi);
            g_nr_stage_hi = make_uav_tex(g_device, g_out_w, g_out_h, DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (!g_nr_stage_hi) logf("[NRPRE] HI staging buffer allocation failed; chained passes limited to one");
            pset_u(g_nr_params_hi, "DLSSNR.InputWidth", g_out_w);
            pset_u(g_nr_params_hi, "DLSSNR.InputHeight", g_out_h);
            pset_u(g_nr_params_hi, "DLSSNR.OutputWidth", g_out_w);
            pset_u(g_nr_params_hi, "DLSSNR.OutputHeight", g_out_h);
            const char *widths[] = {"DLSSNR.ColorSubrectWidth", "DLSSNR.DepthSubrectWidth", "DLSSNR.MVecSubrectWidth", "DLSSNR.OutputSubrectWidth"};
            const char *heights[] = {"DLSSNR.ColorSubrectHeight", "DLSSNR.DepthSubrectHeight", "DLSSNR.MVecSubrectHeight", "DLSSNR.OutputSubrectHeight"};
            for (int i = 0; i < 4; ++i) {
                pset_u(g_nr_params_hi, widths[i], g_out_w);
                pset_u(g_nr_params_hi, heights[i], g_out_h);
            }
            track_recording(cmd);
            NVSDK_NGX_Result rh = sc2(cmd, kFeatureDLSSNR, g_nr_params_hi, &g_nr_handle_hi);
            g_hi_release = sr2;
            logf("[NRPRE] 2c: HI feature (%ux%u) -> 0x%08X handle=%p out=%p",
                 g_out_w, g_out_h, (unsigned)rh, (void *)g_nr_handle_hi, (void *)g_nr_out_hi);
        }
}

// Records one compute pass: writes SRVs/UAV into this frame's descriptor slot,
// binds our root signature + PSO, and dispatches over the image.
static void codec_dispatch(ID3D12GraphicsCommandList *cmd, ID3D12Device *dev,
                           ID3D12PipelineState *pso,
                           ID3D12Resource *t0, ID3D12Resource *t1, ID3D12Resource *t2,
                           ID3D12Resource *u0, unsigned w, unsigned h,
                           ID3D12Resource *t3, ID3D12Resource *t4)
{
    unsigned slot = 0;
    if (!cx_slot_acquire(cmd, slot)) return;   // preflight guarantees room

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_cpu(g_cx.heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_gpu(g_cx.heap);
    cpu.ptr += (SIZE_T)slot * kCxDescriptorsPerSlot * g_cx.inc;
    gpu.ptr += (UINT64)slot * kCxDescriptorsPerSlot * g_cx.inc;

    // The depth buffer is typeless (GTA V: R32G8X24_TYPELESS), so a fixed colour
    // format here creates an invalid view and removes the device. Derive each
    // view's format from the resource it actually points at.
    auto srv_format = [](ID3D12Resource *r) -> DXGI_FORMAT {
        D3D12_RESOURCE_DESC d{};
        if (!res_desc(r, &d)) return DXGI_FORMAT_R16G16B16A16_FLOAT;
        switch (d.Format) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:    return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R24G8_TYPELESS:       return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:         return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R16_TYPELESS:         return DXGI_FORMAT_R16_UNORM;
        // Colour and motion vectors can be typeless too. Dead Space (2023) hands
        // over R16G16B16A16_TYPELESS colour and R16G16_TYPELESS motion vectors;
        // without these they reach default, a view is built with a typeless
        // format, and the device is removed on the first evaluate.
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS:       return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R8G8_TYPELESS:         return DXGI_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8_TYPELESS:           return DXGI_FORMAT_R8_UNORM;
        default:                               return d.Format;
        }
    };

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    ID3D12Resource *srvs[5] = { t0, t1, t2, t3, t4 };
    for (int i = 0; i < 5; ++i) {
        ID3D12Resource *r = srvs[i] ? srvs[i] : t0;
        sd.Format = srv_format(r);
        D3D12_CPU_DESCRIPTOR_HANDLE hnd = cpu; hnd.ptr += (SIZE_T)i * g_cx.inc;
        dev->CreateShaderResourceView(r, &sd, hnd);
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE uh = cpu; uh.ptr += (SIZE_T)5 * g_cx.inc;   // u0 after t0..t4
    dev->CreateUnorderedAccessView(u0, nullptr, &ud, uh);

    D3D12_UNORDERED_ACCESS_VIEW_DESC hv{};                 // u1: histogram buffer
    hv.Format = DXGI_FORMAT_UNKNOWN;
    hv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    hv.Buffer.NumElements = kHistBins;
    hv.Buffer.StructureByteStride = sizeof(UINT);
    D3D12_CPU_DESCRIPTOR_HANDLE hh2 = cpu; hh2.ptr += (SIZE_T)6 * g_cx.inc;
    dev->CreateUnorderedAccessView(g_cx.hist, nullptr, &hv, hh2);

    // With auto guides on, an SDR buffer skips the paper-white divide and the
    // soft clip entirely -- developing an already display-referred image only
    // crushes it, so an unmarked buffer is passed through untouched.
    const UINT hdr_mode = (g_auto_guides && g_guides_valid && !g_hdr_detected) ? 0u : g_hdr_mode;
    // Bias rides on the measured value, then is held inside the range the
    // measurement itself is clamped to.
    float pw_effective = g_paper_white / (g_input_gain > 0.01f ? g_input_gain : 0.01f);
    if (pw_effective < 0.10f) pw_effective = 0.10f;
    if (pw_effective > 4.00f) pw_effective = 4.00f;
    struct { UINT w, h; float pw, ts, cs; UINT hdr; float knee, dclamp;
             float mvx, mvy, reproj, dreject, sgate, strength, chroma, pad4; } k{
        w, h, pw_effective, g_transfer, g_color_strength, hdr_mode, g_knee, g_delta_clamp,
        g_mv_scale_x, g_mv_scale_y,
        // advected: it is already in place, so do not move it again
        (g_delta_advect && g_cx.pso_advect) ? 0.0f
            : (g_delta_age_scale ? g_reproject * (float)g_delta_age : g_reproject),
        g_depth_reject, g_struct_gate,
        g_effect_strength, g_chroma_transfer, 0.0f };

    ID3D12DescriptorHeap *heaps[1] = { g_cx.heap };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(g_cx.root);
    cmd->SetPipelineState(pso);
    cmd->SetComputeRootDescriptorTable(0, gpu);
    cmd->SetComputeRoot32BitConstants(1, 16, &k, 0);
    cmd->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
}

// One texture into one of our own, with the UAV typed for the destination.
//
// codec_dispatch cannot serve here: it hardcodes an RGBA16F UAV, which is right
// for colour and wrong for a single-channel depth or a two-channel motion vector.
// Everything else -- the ring slot, the root signature, the SRV format mapping --
// is deliberately the same, so this stays one small departure and not a fork.
static void snapshot_dispatch(ID3D12GraphicsCommandList *cmd, ID3D12Device *dev,
                              ID3D12Resource *srcTex, ID3D12Resource *dstTex,
                              DXGI_FORMAT uav_fmt, unsigned w, unsigned h)
{
    if (cmd == nullptr || dev == nullptr || srcTex == nullptr || dstTex == nullptr) return;
    if (g_cx.pso_snap == nullptr || g_cx.heap == nullptr) return;
    unsigned slot = 0;
    if (!cx_slot_acquire(cmd, slot)) return;   // preflight guarantees room
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_cpu(g_cx.heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_gpu(g_cx.heap);
    cpu.ptr += (SIZE_T)slot * kCxDescriptorsPerSlot * g_cx.inc;
    gpu.ptr += (UINT64)slot * kCxDescriptorsPerSlot * g_cx.inc;

    D3D12_RESOURCE_DESC sd0{};
    DXGI_FORMAT srv_fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (res_desc(srcTex, &sd0)) {
        switch (sd0.Format) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:    srv_fmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: srv_fmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        case DXGI_FORMAT_R24G8_TYPELESS:       srv_fmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;    break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:    srv_fmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;    break;
        case DXGI_FORMAT_R32_TYPELESS:         srv_fmt = DXGI_FORMAT_R32_FLOAT;                break;
        case DXGI_FORMAT_D32_FLOAT:            srv_fmt = DXGI_FORMAT_R32_FLOAT;                break;
        case DXGI_FORMAT_D16_UNORM:            srv_fmt = DXGI_FORMAT_R16_UNORM;                break;
        case DXGI_FORMAT_R16_TYPELESS:         srv_fmt = DXGI_FORMAT_R16_UNORM;                break;
        default:                               srv_fmt = sd0.Format;                           break;
        }
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = srv_fmt;
    for (int i = 0; i < 5; ++i) {            // t1..t4 unused, but must be valid
        D3D12_CPU_DESCRIPTOR_HANDLE hnd = cpu; hnd.ptr += (SIZE_T)i * g_cx.inc;
        dev->CreateShaderResourceView(srcTex, &sd, hnd);
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = uav_fmt;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE uh = cpu; uh.ptr += (SIZE_T)5 * g_cx.inc;
    dev->CreateUnorderedAccessView(dstTex, nullptr, &ud, uh);
    D3D12_CPU_DESCRIPTOR_HANDLE uh1 = cpu; uh1.ptr += (SIZE_T)6 * g_cx.inc;
    dev->CreateUnorderedAccessView(dstTex, nullptr, &ud, uh1);

    struct { UINT w, h; float f[14]; } k{}; k.w = w; k.h = h;
    ID3D12DescriptorHeap *heaps[1] = { g_cx.heap };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(g_cx.root);
    cmd->SetPipelineState(g_cx.pso_snap);
    cmd->SetComputeRootDescriptorTable(0, gpu);
    cmd->SetComputeRoot32BitConstants(1, 16, &k, 0);
    cmd->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
}

// Move the stored delta forward one frame, so it is never reused out of position.
// Reads the current delta, writes the moved one into the spare, then swaps: a
// compute shader cannot read and write the same texture safely, and the swap costs
// nothing since only the pointers move.
static void advect_delta(ID3D12GraphicsCommandList *cmd, ID3D12Device *dev,
                         ID3D12Resource *in_tex, ID3D12Resource *mvec,
                         ID3D12Resource *depth, unsigned w, unsigned h)
{
    if (g_cx.pso_advect == nullptr || g_cx.delta2 == nullptr) return;
    barrier_transition(cmd, g_cx.delta, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    codec_dispatch(cmd, dev, g_cx.pso_advect, in_tex, nullptr, g_cx.delta,
                   g_cx.delta2, w, h, mvec, depth);
    uav_barrier(cmd, g_cx.delta2);
    barrier_transition(cmd, g_cx.delta, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *t = g_cx.delta; g_cx.delta = g_cx.delta2; g_cx.delta2 = t;
}



static void uav_barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    using pfn_t = void (STDMETHODCALLTYPE *)(void *, UINT, const D3D12_RESOURCE_BARRIER *);
    auto **vt = *reinterpret_cast<void ***>(cmd);
    reinterpret_cast<pfn_t>(vt[26])(cmd, 1, &b);
}

// ---- settings persistence ------------------------------------------------
// ReShade keeps one ReShade.ini per game, so this is per-game config for free.
static void load_settings(reshade::api::effect_runtime *rt) {
    int v = 0; float f = 0.0f;
    if (!g_enabled_setting_loaded) {
        if (reshade::get_config_value(rt, "NRPreUpscale", "Enabled", v))
            g_nr_enabled = (v != 0);
        g_enabled_setting_loaded = true;
    }
    if (reshade::get_config_value(rt, "NRPreUpscale", "AsyncNetwork", v)) g_async_net = (v != 0);
    if (reshade::get_config_value(rt, "NRPreUpscale", "StallMs", v)) g_stall_ms = (v < 1 ? 1 : (v > 500 ? 500 : v));
    if (reshade::get_config_value(rt, "NRPreUpscale", "UniformDelta", v)) g_uniform_delta = (v != 0);
    if (reshade::get_config_value(rt, "NRPreUpscale", "Diagnostics", v)) g_diagnostics = (v != 0);
    if (reshade::get_config_value(rt, "NRPreUpscale", "DeltaAdvect", v)) g_delta_advect = (v != 0);
    if (reshade::get_config_value(rt, "NRPreUpscale", "DeltaAgeScale", v)) g_delta_age_scale = (v != 0);
    if (reshade::get_config_value(rt, "NRPreUpscale", "NetScale", f))
        g_net_scale = (f < 0.25f ? 0.25f : (f > 1.0f ? 1.0f : f));
    if (reshade::get_config_value(rt, "NRPreUpscale", "Cadence", v))       g_skip_n = (v < 1 ? 1 : (v > 3 ? 3 : v));
    if (reshade::get_config_value(rt, "NRPreUpscale", "Codec", v))         g_codec_on = (v != 0);
    if (reshade::get_config_value(rt, "NRPreUpscale", "AutoMask", v)) g_auto_mask = (v != 0);
    // ReShade re-creates the effect runtime several times per session; do not let
    // a reload stomp the value the auto-exposure has already converged on.
    if (!g_auto_pw || !g_pw_valid)
        if (reshade::get_config_value(rt, "NRPreUpscale", "PaperWhite", f)) g_paper_white = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "Shoulder", f))      g_knee = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "SkinStructure", f)) g_skin_structure = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "LocalStructure", f)) g_local_structure = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "LocalTone", f)) g_local_tone = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "Intensity", f)) g_intensity = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "SkipPassthrough", v)) g_skip_passthrough = (v != 0);
    if (reshade::get_config_value(rt, "NRPreUpscale", "DeltaReuse", v)) g_delta_reuse = (v != 0);
    if (reshade::get_config_value(rt, "NRPreUpscale", "DeltaClamp", f)) g_delta_clamp = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "Reproject", f)) g_reproject = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "DepthReject", f)) g_depth_reject = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "StructGate", f)) g_struct_gate = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "ChromaTransfer", f)) g_chroma_transfer = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "InputGain", f)) g_input_gain = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "Preset", v)) g_preset = v;
    if (reshade::get_config_value(rt, "NRPreUpscale", "Transfer", f)) g_transfer = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "Saturation", f)) g_color_strength = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "EffectStrength", f)) g_effect_strength = f;
    if (reshade::get_config_value(rt, "NRPreUpscale", "Passes", v)) g_repeat = (v < 1 ? 1 : (v > 4 ? 4 : v));
    if (reshade::get_config_value(rt, "NRPreUpscale", "AutoPaperWhite", v)) g_auto_pw = (v != 0);
    if (g_auto_pw && g_pw_valid) return;   // already converged, keep it
    logf("[NRPRE] settings loaded: enabled=%d cadence=%d codec=%d pw=%.3f knee=%.2f "
         "strength=%.3f transfer=%.3f intensity=%.3f rebind=%d",
         (int)g_nr_enabled, g_skip_n, (int)g_codec_on, g_paper_white, g_knee,
         g_effect_strength, g_transfer, g_intensity, (int)g_rebind);
}
// Paper white shifts what the network is shown, so a preset nudges it rather than
// setting it: auto exposure owns the absolute value, this only biases it.
static void apply_preset(int p) {
    switch (p) {
    // intensity, local tone, local structure, skin structure (-1 = follow structure)
    case 1: g_intensity = 0.45f; g_local_tone = 0.55f; g_local_structure = 0.25f;
            g_skin_structure = 0.15f; break;                       // Light
    case 2: g_intensity = 0.70f; g_local_tone = 0.80f; g_local_structure = 0.55f;
            g_skin_structure = 0.40f; break;                       // Moderate
    case 3: g_intensity = 1.00f; g_local_tone = 1.00f; g_local_structure = 1.00f;
            g_skin_structure = -1.0f; break;                       // Reference
    // Mas alla de lo que la red pretende. La linea base de la comunidad es 1.00 y
    // subir de ahi esta reportado como peor -- que es justamente el punto de estos
    // dos: empujar el realce hasta que se note que es artificial.
    case 4: g_intensity = 1.30f; g_local_tone = 1.25f; g_local_structure = 1.45f;
            g_skin_structure = 1.20f; break;                       // Overdrive
    case 5: g_intensity = 1.80f; g_local_tone = 1.60f; g_local_structure = 2.00f;
            g_skin_structure = 1.90f; break;                       // AI slop
    default: break;                     // custom: leave whatever the user set
    }
}

static void save_settings(reshade::api::effect_runtime *rt) {
    reshade::set_config_value(rt, "NRPreUpscale", "Enabled", g_nr_enabled ? 1 : 0);
    reshade::set_config_value(rt, "NRPreUpscale", "Cadence", g_skip_n);
    reshade::set_config_value(rt, "NRPreUpscale", "NetScale", g_net_scale);
    reshade::set_config_value(rt, "NRPreUpscale", "AsyncNetwork", g_async_net ? 1 : 0);
    reshade::set_config_value(rt, "NRPreUpscale", "UniformDelta", g_uniform_delta ? 1 : 0);
    reshade::set_config_value(rt, "NRPreUpscale", "Diagnostics", g_diagnostics ? 1 : 0);
    reshade::set_config_value(rt, "NRPreUpscale", "DeltaAdvect", g_delta_advect ? 1 : 0);
    reshade::set_config_value(rt, "NRPreUpscale", "DeltaAgeScale", g_delta_age_scale ? 1 : 0);
    reshade::set_config_value(rt, "NRPreUpscale", "SkipPassthrough", g_skip_passthrough ? 1 : 0);
    reshade::set_config_value(rt, "NRPreUpscale", "DeltaReuse", g_delta_reuse ? 1 : 0);
    reshade::set_config_value(rt, "NRPreUpscale", "DeltaClamp", g_delta_clamp);
    reshade::set_config_value(rt, "NRPreUpscale", "Reproject", g_reproject);
    reshade::set_config_value(rt, "NRPreUpscale", "DepthReject", g_depth_reject);
    reshade::set_config_value(rt, "NRPreUpscale", "StructGate", g_struct_gate);
    reshade::set_config_value(rt, "NRPreUpscale", "ChromaTransfer", g_chroma_transfer);
    reshade::set_config_value(rt, "NRPreUpscale", "InputGain", g_input_gain);
    reshade::set_config_value(rt, "NRPreUpscale", "Preset", g_preset);
    reshade::set_config_value(rt, "NRPreUpscale", "Transfer", g_transfer);
    reshade::set_config_value(rt, "NRPreUpscale", "Saturation", g_color_strength);
    reshade::set_config_value(rt, "NRPreUpscale", "EffectStrength", g_effect_strength);
    reshade::set_config_value(rt, "NRPreUpscale", "Passes", g_repeat);
    reshade::set_config_value(rt, "NRPreUpscale", "Codec", g_codec_on ? 1 : 0);
    reshade::set_config_value(rt, "NRPreUpscale", "AutoMask", g_auto_mask ? 1 : 0);
    reshade::set_config_value(rt, "NRPreUpscale", "PaperWhite", g_paper_white);
    reshade::set_config_value(rt, "NRPreUpscale", "Shoulder", g_knee);
    reshade::set_config_value(rt, "NRPreUpscale", "SkinStructure", g_skin_structure);
    reshade::set_config_value(rt, "NRPreUpscale", "LocalStructure", g_local_structure);
    reshade::set_config_value(rt, "NRPreUpscale", "LocalTone", g_local_tone);
    reshade::set_config_value(rt, "NRPreUpscale", "Intensity", g_intensity);
    reshade::set_config_value(rt, "NRPreUpscale", "AutoPaperWhite", g_auto_pw ? 1 : 0);
}

#ifndef NR_STANDALONE
static void draw_overlay(reshade::api::effect_runtime *rt) {
    bool changed = false;

    changed |= ImGui::Checkbox("Enable DLSS-NR", &g_nr_enabled);
    ImGui::SetItemTooltip("Runs Neural Rendering on the DLSS input at render resolution, "
                          "instead of on the DLSS output, then feeds the result back in.");

    ImGui::Spacing();
    ImGui::SeparatorText("Neural rendering");
    ImGui::TextDisabled("How often it runs");
    int cad = g_skip_n - 1;
    changed |= ImGui::RadioButton("Quality", &cad, 0);
    ImGui::SetItemTooltip("The network runs on every frame.");
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Balanced", &cad, 1);
    ImGui::SetItemTooltip("The network runs on every 2nd frame.");
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Performance", &cad, 2);
    ImGui::SetItemTooltip("The network runs on every 3rd frame.");
    g_skip_n = cad + 1;

    // Each mode states what it does and what it costs: the cheaper ones are not
    // free, they reconstruct the skipped frames and that shows on moving edges.
    switch (g_skip_n) {
    case 1:
        ImGui::TextDisabled("Runs the network on every frame. Nothing is reconstructed.");
        ImGui::TextDisabled("Side effect: none. This is the reference image.");
        break;
    case 2:
        ImGui::TextDisabled("Runs the network every 2nd frame. The frame in between reuses "
                            "the last effect, moved along the motion vectors.");
        ImGui::TextDisabled("Side effect: some trailing at edges that move, and mild shimmer "
                            "where the reconstruction and the real result differ.");
        break;
    default:
        ImGui::TextDisabled("Runs the network every 3rd frame. Two frames in between reuse an "
                            "effect that is up to two frames old.");
        ImGui::TextDisabled("Side effect: the same trailing and shimmer as Balanced, stronger, "
                            "since the reused effect is older.");
        break;
    }

    ImGui::Spacing();
    ImGui::TextDisabled("How far it may go");
    const char *presets[] = { "Custom", "Light", "Moderate", "Reference",
                              "Overdrive", "AI slop" };
    if (ImGui::Combo("How transformative", &g_preset, presets, 6)) {
        apply_preset(g_preset); changed = true;
    }
    ImGui::SetItemTooltip("How far the network is allowed to reinterpret the image. "
                          "Reference is what it produces on its own; the lower settings pull "
                          "back its micro-detail first, which is where invented texture "
                          "comes from.");
    switch (g_preset) {
    case 1: ImGui::TextDisabled("Keeps the lighting work, holds back invented detail."); break;
    case 2: ImGui::TextDisabled("Half way: detail is enhanced but not rebuilt."); break;
    case 3: ImGui::TextDisabled("Everything the network wants to do. Most detail, most "
                                "reinterpretation."); break;
    case 4: ImGui::TextDisabled("Past what the network intends. Detail is pushed until it "
                                "starts looking drawn rather than photographed."); break;
    case 5: ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "Deliberately overcooked. Skin turns waxy and surfaces grow "
                               "detail that was never in the scene."); break;
    default: ImGui::TextDisabled("Set by hand in Advanced."); break;
    }

    ImGui::TextDisabled("How much of it is kept");
    changed |= ImGui::SliderFloat("Effect strength", &g_effect_strength, 0.0f, 2.0f, "%.2f");
    ImGui::SetItemTooltip("How far to carry the network's result. 0 is the game's own image, "
                          "1 is the full effect, and above 1 pushes past what the network "
                          "produced -- which sharpens, and can ring on hard edges. This "
                          "dilutes everything evenly; the setting above shapes what the "
                          "network does instead.");
    if (g_effect_strength == 0.0f && g_codec_on)
        ImGui::TextDisabled("Effect strength is zero: the NR effect is not visible.");

    changed |= ImGui::SliderInt("Network passes", &g_repeat, 1, 4, "%d");
    ImGui::SetItemTooltip("How many times the network is applied to the image in one frame. "
                          "Each pass re-runs the network on the previous pass's result, so the "
                          "effect compounds: 2 is roughly the network pushing itself twice as "
                          "far. Each extra pass costs a full run of the network (~3.3 ms at "
                          "1280x720), so at 4 the frame budget roughly quadruples. F9 cycles it.");

    ImGui::Spacing();
    ImGui::SeparatorText("Exposure");
    ImGui::BeginDisabled(!g_codec_on);
    changed |= ImGui::Checkbox("Auto", &g_auto_pw);
    ImGui::SameLine();
    ImGui::TextDisabled("(uses the game's own exposure, or the scene if it has none)");
    ImGui::SetItemTooltip("Measures the scene's own brightness and sets the reference white "
                          "from it, instead of a fixed value borrowed from another game.");
    if (g_auto_pw) {
        if (g_pw_valid || g_guides_valid) {
            ImGui::TextDisabled("source: %s", g_exp_from_game ? "game exposure"
                                                             : "scene histogram (game has none)");
            ImGui::TextDisabled("paper white %.3f   shoulder %.2f   (%u samples)",
                                g_pw_measured, g_knee_measured, g_pw_updates);
            ImGui::TextDisabled("buffer: %s      depth: %s (%s)",
                                g_hdr_detected ? "HDR (developing)" : "SDR (passthrough)",
                                g_depth_reversed ? "reversed" : "normal",
                                g_depth_from_game ? "game" : "fixed fallback");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "sampling...");
        }
    }
    ImGui::BeginDisabled(g_auto_pw);
    changed |= ImGui::SliderFloat("Paper white", &g_paper_white, 0.10f, 2.00f, "%.3f");
    ImGui::SetItemTooltip("Where the game's reference white lands.");
    changed |= ImGui::SliderFloat("Shoulder", &g_knee, 0.10f, 0.95f, "%.2f");
    ImGui::SetItemTooltip("Where highlight roll-off begins.");
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Advanced")) {
        ImGui::TextDisabled("Defaults are what the tuning converged on. Change them only to "
                            "trade one artefact for another.");
        ImGui::Spacing();

        ImGui::SeparatorText("Colour development");
        changed |= ImGui::Checkbox("Enable codec", &g_codec_on);
        ImGui::SetItemTooltip("The network expects a bounded, display-referred image and this "
                              "game hands DLSS a scene-linear HDR buffer, so it is developed "
                              "first and the range restored afterwards. An SDR buffer is "
                              "detected and passed through on its own, so turning this off is "
                              "a diagnostic, not a setting.");
        changed |= ImGui::SliderFloat("Transfer", &g_transfer, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("How much of the development curve to apply before the network "
                              "sees the image. 1 is the full curve.");
        changed |= ImGui::SliderFloat("Saturation", &g_color_strength, 0.0f, 1.5f, "%.2f");
        ImGui::SetItemTooltip("1 keeps the colour as developed. Below 1 desaturates toward "
                              "luminance, above 1 pushes it further.");
        changed |= ImGui::SliderFloat("Chroma transfer", &g_chroma_transfer, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("Restoring the range carries the network's light but not its "
                              "colour, since hue is preserved exactly by construction. This "
                              "adopts its colour as well, at the luminance already restored.");
        changed |= ImGui::SliderFloat("Input gain", &g_input_gain, 0.60f, 1.60f, "%.2f");
        ImGui::SetItemTooltip("How bright the image is when the network sees it. Above 1 shows "
                              "it a brighter image and it acts harder. Mostly cancels out when "
                              "the range is restored, so it shows up in the highlights.");


        ImGui::Spacing();
        ImGui::SeparatorText("Skipped-frame reconstruction");
        ImGui::BeginDisabled(g_skip_n <= 1);
        changed |= ImGui::Checkbox("Reuse the network's effect", &g_delta_reuse);
        ImGui::SetItemTooltip("Stores what the network changed and re-applies it to the current "
                              "frame instead of reusing the whole stale image.");
        ImGui::BeginDisabled(!g_delta_reuse);
        changed |= ImGui::SliderFloat("Follow motion", &g_reproject, -1.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("Walks the motion vectors back to where each pixel was, so the "
                              "effect lands on the moving geometry instead of trailing behind "
                              "it. 0 reapplies it in place.");
        changed |= ImGui::SliderFloat("Disocclusion reject", &g_depth_reject, 0.0f, 0.2f, "%.4f");
        ImGui::SetItemTooltip("Drops the stored effect where the depth no longer matches, which "
                              "is what uncovered pixels look like. Lower rejects more.");
        changed |= ImGui::SliderFloat("Structure gate", &g_struct_gate, 0.0f, 4.0f, "%.2f");
        ImGui::SetItemTooltip("Holds the effect to what the local contrast can account for, "
                              "removing silhouettes left behind by things that moved. Applies "
                              "only where the depth test already found the history doubtful.");
        changed |= ImGui::SliderFloat("Effect clamp", &g_delta_clamp, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("Caps the reused effect's magnitude. 0 removes the cap.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(g_delta_reuse);
        changed |= ImGui::Checkbox("Pass the game's colour instead", &g_skip_passthrough);
        ImGui::SetItemTooltip("Fallback: drops the effect on skipped frames rather than "
                              "reconstructing it.");
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::SeparatorText("Network parameters");
        ImGui::TextDisabled("What the preset above sets. 1.00 across the board is the "
                            "reference; above that is where Overdrive and AI slop live, "
                            "and it is reported to look worse -- on purpose.");
        bool net = false;
        changed |= ImGui::Checkbox("Automatic mask", &g_auto_mask);
        ImGui::SetItemTooltip("Lets the network decide per pixel where to apply its result. "
                              "Without it the final blend is uniform.");
        net |= ImGui::SliderFloat("Intensity", &g_intensity, 0.0f, 2.0f, "%.2f");
        net |= ImGui::SliderFloat("Local tone", &g_local_tone, 0.0f, 2.0f, "%.2f");
        net |= ImGui::SliderFloat("Local structure", &g_local_structure, 0.0f, 2.0f, "%.2f");
        net |= ImGui::SliderFloat("Skin structure", &g_skin_structure, -1.0f, 2.0f, "%.2f");
        ImGui::SetItemTooltip("-1 follows Local structure.");
        if (net) { g_preset = 0; changed = true; }
        ImGui::Spacing();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Status");
    if (g_ui_fps > 0.0f) {
        ImGui::Text("%.2f ms   %.0f fps", g_ui_ms, g_ui_fps);
        if (g_nr_enabled && g_ui_base_ms > 0.0f)
            ImGui::Text("NR cost %.2f ms   network at %.0f Hz",
                        g_ui_ms - g_ui_base_ms, g_ui_nr_hz);
        else if (g_nr_enabled)
            ImGui::TextDisabled("Toggle off once to measure the NR cost.");
    } else {
        ImGui::TextDisabled("Waiting for frames...");
    }
    if (g_resize_pending)
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "NR paused while previous GPU work finishes");
    else if (g_nr_handle == nullptr)
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Network not created yet");

    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults")) {
        g_nr_enabled = true; g_skip_n = 1; g_codec_on = true;
        g_paper_white = 1.0f; g_knee = 0.75f;
        g_repeat = 1;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Saved per game in ReShade.ini");

    if (changed) save_settings(rt);
}
#endif  // NR_STANDALONE

// Exposure is a global, slowly-varying property, so sample it a few times a
// second and read the result back a few frames later -- never stalling the GPU.
// Recording happens here; the fence is signalled at present, once the game has
// actually submitted the command list this was recorded into.
static void record_exposure_sample(ID3D12GraphicsCommandList *cmd, ID3D12Device *dev,
                                   ID3D12Resource *src, ID3D12Resource *depth,
                                   unsigned w, unsigned h)
{
    if (!g_cx.hist || !g_cx.pso_hist || !g_cx.clear_heap ||
        !g_cx.heap || !dev || g_cx.inc == 0) return;
    static unsigned n = 0;
    const unsigned period = g_exp_from_game ? 60u : 20u;   // guides only, when the game covers exposure
    if ((n++ % period) != 0 || g_cx.pending || g_cx.copy_recorded) return;

    // The Clear below binds this slot's u1, so the slot must be reserved before
    // any view is written: writing into one the GPU still reads corrupts it.
    // If it is in flight the sample is dropped for this frame; the histogram
    // just keeps its last values.
    unsigned slot = 0;
    if (!cx_slot_acquire(cmd, slot)) return;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_gpu(g_cx.heap);
    // The root table is t0..t4, u0, u1. The old slot*5+4 address pointed at
    // another slot's SRV (and even slot 0 was t4), so the clear could remove
    // the D3D12 device after the histogram had run for a while. The slot was
    // just reserved, so its u1 has never been written: create it before use.
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_cpu(g_cx.heap);
    cpu.ptr += ((SIZE_T)slot * kCxDescriptorsPerSlot + 6u) * g_cx.inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC hv{};
    hv.Format = DXGI_FORMAT_UNKNOWN;
    hv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    hv.Buffer.NumElements = kHistBins;
    hv.Buffer.StructureByteStride = sizeof(UINT);
    dev->CreateUnorderedAccessView(g_cx.hist, nullptr, &hv, cpu);
    gpu.ptr += ((UINT64)slot * kCxDescriptorsPerSlot + 6u) * g_cx.inc; // u1

    ID3D12DescriptorHeap *heaps[1] = { g_cx.heap };
    cmd->SetDescriptorHeaps(1, heaps);
    const UINT zero[4] = { 0, 0, 0, 0 };
    cmd->ClearUnorderedAccessViewUint(gpu, heap_cpu(g_cx.clear_heap), g_cx.hist, zero, 0, nullptr);
    uav_barrier(cmd, g_cx.hist); // histogram atomics must follow the clear

    codec_dispatch(cmd, dev, g_cx.pso_hist, src, depth, nullptr, g_cx.proxy, w, h);
    uav_barrier(cmd, g_cx.hist);

    barrier_transition(cmd, g_cx.hist, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyResource(g_cx.hist_rb, g_cx.hist);
    barrier_transition(cmd, g_cx.hist, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_cx.copy_recorded = true;
}

// Called at present, where the frame's work is already queued.
static void tick_exposure(ID3D12CommandQueue *queue) {
    if (g_cx.copy_recorded && queue != nullptr) {
        g_cx.copy_recorded = false;
        g_cx.pending_at = ++g_cx.fence_val;
        queue->Signal(g_cx.fence, g_cx.pending_at);
        g_cx.pending = true;
    }
    if (!g_cx.pending || !g_cx.fence) return;
    if (g_cx.fence->GetCompletedValue() < g_cx.pending_at) return;

    g_cx.pending = false;

    if (g_cx.exp_is_texture) {
        g_cx.exp_is_texture = false;
        void *m = nullptr;
        D3D12_RANGE r0{ 0, sizeof(float) };
        if (SUCCEEDED(g_cx.exp_rb->Map(0, &r0, &m)) && m) {
            const float e = *static_cast<const float *>(m);
            D3D12_RANGE w0{ 0, 0 };
            g_cx.exp_rb->Unmap(0, &w0);
            if (e > 1e-5f && e < 1e5f) {
                // Exposure is the factor the game applies to reach display range,
                // so the reference white in scene-linear terms is its reciprocal.
                float pw = 1.0f / e;
                if (pw < 0.05f) pw = 0.05f;
                if (pw > 8.0f)  pw = 8.0f;
                g_pw_measured = pw;
                if (g_auto_pw) {
                    g_paper_white = g_pw_valid ? (g_paper_white * 0.85f + pw * 0.15f) : pw;
                    g_pw_valid = true;
                }
                ++g_pw_updates;
            }
        }
        return;
    }

    void *mapped = nullptr;
    D3D12_RANGE rd{ 0, kHistBins * sizeof(UINT) };
    if (FAILED(g_cx.hist_rb->Map(0, &rd, &mapped)) || mapped == nullptr) return;
    const UINT *bins = static_cast<const UINT *>(mapped);
    // Luminance only: bins 128..131 carry HDR and depth-convention samples,
    // not scene brightness, and would skew the percentile target.
    UINT64 total = 0;
    for (unsigned i = 0; i < 128; ++i) total += bins[i];
    if (total > 1000) {
        const UINT64 target = (UINT64)(total * 0.99);      // 99th percentile
        UINT64 acc = 0; unsigned bin = 127;
        for (unsigned i = 0; i < 128; ++i) {
            acc += bins[i];
            if (acc >= target) { bin = i; break; }
        }
        // Undo the CSHistogram mapping: bin = ((log2(y) + 10) / 16) * 128
        // --- derived guides, same data, no extra dispatch -------------------
        if (g_auto_guides) {
            // Asymmetric evidence: seeing values above 1.0 proves the buffer is
            // HDR, but not seeing them proves nothing -- a night scene in an HDR
            // buffer never exceeds 1.0 either. So latch on, never off, and start
            // assuming HDR so a dark scene cannot flip the codec mid-session.
            const UINT64 over = bins[128];
            if (over * 1000ull > total) g_hdr_detected = true;
            ++g_hdr_samples;
            if (!g_hdr_detected && g_hdr_samples < 400) g_hdr_detected = true;
            // Depth convention comes from the game's NGX flags. A screen sample
            // measures distance, not convention: sky and nearby geometry must
            // never flip DepthInverted while the network retains its history.
            // Shoulder: put the knee just under the 99th percentile, so the roll
            // off starts where the scene's own highlights begin.
            UINT64 acc99 = 0; unsigned b99 = 127;
            const UINT64 t99 = (UINT64)(total * 0.99);
            for (unsigned i = 0; i < 128; ++i) {
                acc99 += bins[i];
                if (acc99 >= t99) { b99 = i; break; }
            }
            float k = 0.60f + 0.35f * ((float)b99 / 127.0f);
            if (k < 0.50f) k = 0.50f;
            if (k > 0.92f) k = 0.92f;
            g_knee_measured = k;
            g_knee = g_guides_valid ? (g_knee * 0.9f + k * 0.1f) : k;
            g_guides_valid = true;
        }

        const float lg = ((bin + 0.5f) / 128.0f) * 16.0f - 10.0f;
        const float p90 = powf(2.0f, lg);

        // Anchor on the scene's genuine highlights, not its average. Normalising a
        // dark scene by its own mid-tones amplifies it ~10x, which both blows the
        // encode past 1.0 and drags the anti-clipping guard across the whole frame
        // -- measured as a heavy loss of saturation at night. Let a night scene
        // stay dark: only real highlights define white.
        float pw = p90;
        if (pw < 0.35f) pw = 0.35f;
        if (pw > 4.0f)  pw = 4.0f;
        // Fallback only: if the game handed us a real exposure recently, its
        // number wins and the histogram just keeps the guides up to date.
        if (!g_exp_from_game) {
            g_pw_measured = pw;
            if (g_auto_pw) {
                g_paper_white = g_pw_valid ? (g_paper_white * 0.90f + pw * 0.10f) : pw;
                g_pw_valid = true;
            }
            ++g_pw_updates;
        }
    }
    D3D12_RANGE wr{ 0, 0 };
    g_cx.hist_rb->Unmap(0, &wr);
}

// Alt-tab, a resolution change or a windowed/fullscreen switch can recreate the
// device or resize the render target. Everything below was built once and never
// rebuilt, so afterwards we kept feeding DLSS textures that belonged to a dead
// device or held a stale frame -- visible as the image "breaking" until another
// mode switch happened to land somewhere consistent.
// A resize is a request, never proof of GPU completion. Track actual submission
// of every list on which we record, then fence every queue which executes it.
// This also works in standalone mode, whose "present" tick is an evaluate hook.
struct SubmissionFence {
    ID3D12CommandQueue *queue;
    ID3D12Fence *fence;
    UINT64 value;
};
static nr::SubmissionTracker g_recorded_lists;
static std::vector<SubmissionFence> g_submission_fences;
using PFN_ExecuteLists = void (STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
using PFN_ResetList = HRESULT (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *);
static constexpr unsigned kMaxSubmissionHooks = 8;
static PFN_ExecuteLists g_orig_execute_lists[kMaxSubmissionHooks] = {};
static PFN_ResetList g_orig_reset_lists[kMaxSubmissionHooks] = {};
static void *g_execute_targets[kMaxSubmissionHooks] = {};
static void *g_reset_targets[kMaxSubmissionHooks] = {};
static std::vector<void *> g_submission_device_targets;

static const GUID kRecordingToken =
    {0xdc8d11a3,0x52a1,0x49bb,{0xb2,0x2d,0x18,0x89,0xda,0x97,0x36,0x04}};

// Private data is forwarded by command-list wrappers. The list owns this token;
// the tracker keeps only its ID, so it cannot prevent abandoned list destruction.
class RecordingToken final : public IUnknown {
    std::atomic<ULONG> refs{1};
public:
    const uint64_t id;
    explicit RecordingToken(uint64_t value) : id(value) {}
    ~RecordingToken() {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        g_recorded_lists.discard(id);
        cx_discard_recording(id);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!IsEqualIID(iid, __uuidof(IUnknown))) return E_NOINTERFACE;
        *out = static_cast<IUnknown *>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --refs;
        if (!remaining) delete this;
        return remaining;
    }
};

static uint64_t recording_id(ID3D12CommandList *list) {
    RecordingToken *token = nullptr;
    UINT size = sizeof(token);
    if (FAILED(list->GetPrivateData(kRecordingToken, &size, &token)) || !token) return 0;
    const uint64_t id = token->id;
    token->Release(); // GetPrivateData AddRefs stored COM interfaces.
    return id;
}

static void track_recording(ID3D12GraphicsCommandList *cmd) {
    if (const uint64_t id = recording_id(cmd)) {
        g_recorded_lists.record(id);
    } else {
        // prepare_recording must have succeeded before any GPU commands.
        g_submission_failed = true;
        logf("[NRPRE] recording identity lost; retaining resources");
    }
}

template <unsigned Index>
static void STDMETHODCALLTYPE hk_execute_lists(ID3D12CommandQueue *queue, UINT count,
                                               ID3D12CommandList *const *lists) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    // A closed list can be submitted more than once. Serialize the submission
    // itself with cleanup, not only its following fence bookkeeping.
    g_orig_execute_lists[Index](queue, count, lists);
    bool referenced = false;
    for (UINT i = 0; i < count; ++i) {
        if (g_recorded_lists.submit(recording_id(lists[i]))) referenced = true;
    }
    if (!referenced) return;
    SubmissionFence *use = nullptr;
    for (auto &entry : g_submission_fences) if (entry.queue == queue) use = &entry;
    if (!use) {
        ID3D12Device *device = nullptr;
        ID3D12Fence *fence = nullptr;
        HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) {
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
            device->Release();
        }
        if (FAILED(hr)) {
            g_submission_failed = true;
            logf("[NRPRE] cleanup fence allocation failed: 0x%08lX; retaining resources", (unsigned long)hr);
            return;
        }
        queue->AddRef();
        g_submission_fences.push_back({queue, fence, 0});
        use = &g_submission_fences.back();
        logf("[NRPRE] tracking submitted NR work on queue=%p", (void *)queue);
    }
    const UINT64 value = ++use->value;
    for (UINT i = 0; i < count; ++i) {
        const uint64_t id = recording_id(lists[i]);
        for (auto &page : g_descriptor_pages)
            page.lifetime.submit(id, reinterpret_cast<uint64_t>(use->fence), value);
    }
    // Every queue has its own timeline. Keep waits even after list Reset; a
    // later submission on another queue cannot prove this queue has completed.
    if (FAILED(queue->Signal(use->fence, value))) {
        g_submission_failed = true;
        logf("[NRPRE] cleanup fence signal failed; retaining resources");
    }
}

template <unsigned Index>
static HRESULT STDMETHODCALLTYPE hk_reset_list(ID3D12GraphicsCommandList *list,
        ID3D12CommandAllocator *allocator, ID3D12PipelineState *state) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    const HRESULT result = g_orig_reset_lists[Index](list, allocator, state);
    // ReShade's reset event precedes Reset and cannot establish success.
    // Discarding a recording never discards fences from its earlier executions.
    if (SUCCEEDED(result)) {
        const std::uint64_t id = recording_id(list);
        g_recorded_lists.discard(id);
        cx_discard_recording(id);
    }
    return result;
}

static const PFN_ExecuteLists kExecuteHooks[] = {
    hk_execute_lists<0>, hk_execute_lists<1>, hk_execute_lists<2>, hk_execute_lists<3>,
    hk_execute_lists<4>, hk_execute_lists<5>, hk_execute_lists<6>, hk_execute_lists<7>
};
static const PFN_ResetList kResetHooks[] = {
    hk_reset_list<0>, hk_reset_list<1>, hk_reset_list<2>, hk_reset_list<3>,
    hk_reset_list<4>, hk_reset_list<5>, hk_reset_list<6>, hk_reset_list<7>
};

template <typename Fn>
static bool install_lifetime_hook(void *target, void **targets, Fn *originals,
                                   const Fn *hooks, const char *name) {
    unsigned slot = 0;
    while (slot < kMaxSubmissionHooks && targets[slot]) {
        if (targets[slot] == target) return true;
        ++slot;
    }
    if (slot == kMaxSubmissionHooks) return false;
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    const MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void *>(hooks[slot]),
                                          reinterpret_cast<void **>(&originals[slot]));
    if (status != MH_OK) {
        logf("[NRPRE] %s hook failed: %d", name, (int)status);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        originals[slot] = nullptr;
        return false;
    }
    targets[slot] = target;
    logf("[NRPRE] %s hook %u installed at %p", name, slot, target);
    return true;
}

static bool install_submission_hook(ID3D12CommandQueue *queue) {
    if (!queue) return false;
    auto *native = native_ref(queue);
    auto **vt = *reinterpret_cast<void ***>(native);
    // ID3D12CommandQueue::ExecuteCommandLists is slot 10.
    const bool ok = install_lifetime_hook(vt[10], g_execute_targets, g_orig_execute_lists,
                                          kExecuteHooks, "ExecuteCommandLists");
    native->Release();
    return ok;
}

static bool prepare_recording(ID3D12GraphicsCommandList *cmd) {
    if (recording_id(cmd)) return true;
    auto *native = native_ref(cmd);
    auto **vt = *reinterpret_cast<void ***>(native);
    // ID3D12GraphicsCommandList::Reset is slot 10.
    bool ok = install_lifetime_hook(vt[10], g_reset_targets, g_orig_reset_lists,
                                    kResetHooks, "command list Reset");
    ID3D12Device *device = nullptr;
    if (ok) ok = SUCCEEDED(native->GetDevice(IID_PPV_ARGS(&device)));
    if (device) {
        auto *native_device = native_ref(device);
        device->Release();
        void *create_target = (*reinterpret_cast<void ***>(native_device))[8];
        if (std::find(g_submission_device_targets.begin(), g_submission_device_targets.end(),
                      create_target) == g_submission_device_targets.end()) {
            // Discover both queue implementations on the device used by NGX.
            // Merely hooking the first presentation queue misses native/compute
            // submissions in games with intermediate queue wrappers.
            for (auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE}) {
                D3D12_COMMAND_QUEUE_DESC desc{};
                desc.Type = type;
                ID3D12CommandQueue *probe = nullptr;
                if (FAILED(native_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&probe)))) {
                    ok = false;
                    break;
                }
                const bool hooked = install_submission_hook(probe);
                probe->Release();
                if (!hooked) { ok = false; break; }
            }
            if (ok) g_submission_device_targets.push_back(create_target);
        }
        native_device->Release();
    }
    if (ok) {
        static uint64_t next_id = 0;
        auto *token = new (std::nothrow) RecordingToken(++next_id);
        ok = token && SUCCEEDED(native->SetPrivateDataInterface(kRecordingToken, token));
        if (token) token->Release();
        // Verify the NGX-facing wrapper shares the native object's private data.
        ok = ok && recording_id(cmd) != 0;
    }
    native->Release();
    if (!ok) {
        static bool reported = false;
        if (!reported) logf("[NRPRE] submission tracking unavailable; skipping NR before recording");
        reported = true;
    }
    return ok;
}

static void release_state(const char *why) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (!g_resize_pending) logf("[NRPRE] cleanup requested (%s)", why);
    g_resize_pending = true;
}

static void service_pending_cleanup() {
    if (!g_resize_pending || g_submission_failed) return;
    if (!g_recorded_lists.ready()) return;
    for (const auto &entry : g_submission_fences) {
        const UINT64 completed = entry.fence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < entry.value) return;
    }
    // Exposure owns a separate copy queue; graphics completion alone is not enough.
    if (g_eq.inflight && g_eq.fence) {
        const UINT64 completed = g_eq.fence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < g_eq.wait_at) return;
    }
    logf("[NRPRE] GPU submissions complete; releasing NR state");
    if (g_nr_handle && g_nr_release) g_nr_release(g_nr_handle);
    if (g_nr_handle_hi && g_hi_release) g_hi_release(g_nr_handle_hi);
    g_nr_handle = g_nr_handle_hi = nullptr;

    auto rls = [](auto *&p) { if (p) { p->Release(); p = nullptr; } };
    rls(g_nr_out); rls(g_nr_stage); rls(g_nr_out_hi); rls(g_nr_stage_hi);
    rls(g_cx.proxy); rls(g_cx.final_tex); rls(g_cx.delta); rls(g_cx.hist); rls(g_cx.hist_rb);
    for (auto &page : g_descriptor_pages) rls(page.heap);
    g_descriptor_pages.clear();
    g_cx.heap = nullptr;
    g_descriptor_recording = 0;
    g_descriptor_end = 0;
    rls(g_cx.clear_heap); rls(g_cx.pso_enc); rls(g_cx.pso_dec);
    rls(g_cx.pso_hist); rls(g_cx.pso_delta); rls(g_cx.pso_apply); rls(g_cx.pso_snap);
    rls(g_cx.pso_advect); rls(g_cx.delta2);
    rls(g_cx.snap_color); rls(g_cx.snap_depth); rls(g_cx.snap_mv);
    g_cx.snap_ready = false;
    rls(g_cx.root); rls(g_cx.fence);
    g_cx.ready = false; g_cx.pending = false; g_cx.copy_recorded = false;
    g_cx.slot = 0; g_cx.fence_val = 0; g_cx.pending_at = 0;

    if (g_eq.held) { g_eq.held->Release(); g_eq.held = nullptr; }
    rls(g_eq.list); rls(g_eq.alloc); rls(g_eq.queue); rls(g_eq.readback); rls(g_eq.fence);
    g_eq.ready = false; g_eq.inflight = false; g_eq.value = 0; g_eq.wait_at = 0;

    rls(g_prof.heap); rls(g_prof.rb);
    g_prof = {};
    if (g_destroy_params) {
        if (g_nr_params) g_destroy_params(g_nr_params);
        if (g_nr_params_hi) g_destroy_params(g_nr_params_hi);
    }
    g_nr_params = nullptr; g_nr_params_hi = nullptr;
    g_pending_exp_tex = nullptr;
    g_setup_done = false;
    g_final_valid = false;
    g_delta_valid = false;
    g_pw_valid = false; g_guides_valid = false;
    g_net_w = g_net_h = 0;
    g_setup_w = g_setup_h = 0;
    g_nr_eval = g_snip_eval = nullptr;
    g_nr_release = g_hi_release = nullptr;
    g_nr_core_owned = false;
    g_destroy_params = nullptr;
    g_reset_pending = false;
    g_recorded_lists.clear();
    for (auto &entry : g_submission_fences) {
        entry.fence->Release();
        entry.queue->Release();
    }
    g_submission_fences.clear();
    g_resize_pending = false;
}

static bool g_hooked = false;

// Avoid dedicated FG providers when gathering modules. This is only a discovery
// filter: the shared core and sl.common also dispatch FG, so eval_dispatch must
// enforce the feature's creation contract before inspecting Color or jitter.
static bool is_framegen_snippet(HMODULE m) {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(m, path, MAX_PATH) == 0) return false;
    for (wchar_t *p = path; *p; ++p)
        if (*p >= L'A' && *p <= L'Z') *p += 32;
    for (const wchar_t *p = path; *p; ++p) {
        const wchar_t *n = L"dlssg", *q = p;
        while (*n && *q == *n) { ++q; ++n; }
        if (!*n) return true;
    }
    return false;
}

static void install_hook() {
    if (g_hooked) return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&install_hook), &self);

    // Gather every loaded module that offers the NGX evaluate. The driver's own
    // _nvngx.dll is the usual provider, but a proxy (OptiScaler and friends)
    // answers the same calls in games that have no native DLSS, and both are
    // loaded at once. Waiting for the driver alone leaves those games unhooked;
    // picking by name or by a timer picks wrong. Hook them all instead.
    HMODULE cands[kMaxNgx];
    unsigned n = 0;
    if (HMODULE drv = GetModuleHandleW(L"_nvngx.dll"))
        if (GetProcAddress(drv, "NVSDK_NGX_D3D12_EvaluateFeature")) cands[n++] = drv;

    HMODULE mods[512];
    DWORD needed = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
        const unsigned total = needed / sizeof(HMODULE);
        for (unsigned i = 0; i < total && n < kMaxNgx; ++i) {
            if (mods[i] == self) continue;                 // never hook ourselves
            bool dup = false;
            for (unsigned k = 0; k < n; ++k) dup |= (cands[k] == mods[i]);
            if (dup) continue;
            if (is_framegen_snippet(mods[i])) continue;      // see above
            if (GetProcAddress(mods[i], "NVSDK_NGX_D3D12_EvaluateFeature")) cands[n++] = mods[i];
        }
    }
    if (n == 0) return;                                    // nothing loaded yet, retry next present

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        logf("[NRPRE] MH_Initialize failed"); return;
    }
    g_hooked = true;                                       // only ever try once

    unsigned ok = 0;
    for (unsigned i = 0; i < n; ++i) {
        auto target = GetProcAddress(cands[i], "NVSDK_NGX_D3D12_EvaluateFeature");
        if (target == nullptr) continue;
        // Observe the complete lifecycle at each provider before permitting NR
        // on its evaluate path. A proxy and its core can expose different handles.
        const auto create = GetProcAddress(cands[i], "NVSDK_NGX_D3D12_CreateFeature");
        const auto release = GetProcAddress(cands[i], "NVSDK_NGX_D3D12_ReleaseFeature");
        if (!create || !release) {
            logf("[NRPRE] feature gate: module %u lacks lifecycle exports; leaving evaluate untouched", i);
            continue;
        }
        const auto install_contract_hook = [](FARPROC address, void *hook, void **original) {
            if (MH_CreateHook(reinterpret_cast<void *>(address), hook, original) != MH_OK)
                return false;
            if (MH_EnableHook(reinterpret_cast<void *>(address)) == MH_OK) return true;
            MH_RemoveHook(reinterpret_cast<void *>(address));
            *original = nullptr;
            return false;
        };
        if (!install_contract_hook(release, reinterpret_cast<void *>(kReleaseThunks[i]),
                                   reinterpret_cast<void **>(&g_orig_release_n[i])) ||
            !install_contract_hook(create, reinterpret_cast<void *>(kCreateThunks[i]),
                                   reinterpret_cast<void **>(&g_orig_create_n[i]))) {
            logf("[NRPRE] feature gate: lifecycle hook unavailable for module %u; leaving evaluate untouched", i);
            continue;
        }
        logf("[NRPRE] feature gate: CreateFeature/ReleaseFeature hooks active (module %u)", i);
        if (MH_CreateHook(reinterpret_cast<LPVOID>(target),
                          const_cast<LPVOID>(kEvalThunks[i]),
                          reinterpret_cast<LPVOID *>(&g_orig_eval_n[i])) != MH_OK) continue;
        const bool on = MH_EnableHook(reinterpret_cast<LPVOID>(target)) == MH_OK;
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(cands[i], path, MAX_PATH);
        logf("[NRPRE] hook %u on NVSDK_NGX_D3D12_EvaluateFeature: %s  %S",
             i, on ? "OK" : "FAILED", path);
        if (on) { ++ok; if (g_orig_eval == nullptr) g_orig_eval = g_orig_eval_n[i]; }
    }
    if (ok == 0) { logf("[NRPRE] no NGX evaluate could be hooked"); return; }

}

// Alt-tab keeps the D3D12 device alive and only resizes the swapchain, so the
// device hooks never fire. This is the event that actually marks the boundary,
// and the network's temporal history has to be dropped across it.
static void on_init_swapchain(reshade::api::swapchain *, bool resize) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (!resize) return;
    release_state("swapchain recreated");
    g_force_reset_frames = 8;       // flush the network's history afterwards
    g_diag_frames = 90;
}

static void on_destroy_device(reshade::api::device *dev) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (dev->get_api() == reshade::api::device_api::d3d12 &&
        same_device(reinterpret_cast<ID3D12Device *>(dev->get_native()), g_device)) {
        release_state("device destroyed");
        g_device = nullptr;
    }
}

static void on_init_device(reshade::api::device *dev) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (dev->get_api() != reshade::api::device_api::d3d12) return;
    auto *d = reinterpret_cast<ID3D12Device *>(dev->get_native());
    if (g_device != nullptr && !same_device(d, g_device)) release_state("new device");
    if (!same_device(g_device, d)) {
        g_device = d;
        logf("[NRPRE] captured ID3D12Device=%p", (void *)g_device);
    }
}

#ifndef NR_STANDALONE
static void on_init_command_queue(reshade::api::command_queue *queue) {
    if (queue->get_device()->get_api() != reshade::api::device_api::d3d12) return;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    install_submission_hook(reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()));
}
#endif

static void on_present(reshade::api::command_queue *queue, reshade::api::swapchain *,
                       const reshade::api::rect *, const reshade::api::rect *,
                       uint32_t, const reshade::api::rect *)
{
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (queue && queue->get_device() &&
        queue->get_device()->get_api() == reshade::api::device_api::d3d12)
        install_submission_hook(reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()));
    service_pending_cleanup();
    static bool prev_down = false;
    const bool down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (down && !prev_down) {
        g_nr_enabled = !g_nr_enabled;
        logf("[NRPRE] F7: DLSS-NR %s", g_nr_enabled ? "ON" : "OFF");
    }
    prev_down = down;

    // F10 stamps the log so a visual event can be located exactly: the breakage
    // leaves no DXGI or state trace, so the timestamp has to come from the user.
    // F6 cycles the effect between normal, exaggerated and off.
    //
    // Without an overlay there is no way to see whether the network is doing
    // anything, and "is it on?" is not a question a subtle effect can answer. The
    // strength is a blend factor toward the network's result, so pushing it past 1
    // extrapolates and turns a subtle change into an obvious one. If 3.0 looks
    // wrong in an obvious way, the whole pipeline is alive; if it looks identical,
    // nothing is reaching the screen and the strength is not the problem.
    static bool ex_prev = false;
    const bool ex_down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (ex_down && !ex_prev) {
        g_effect_strength = (g_effect_strength > 2.0f)  ? 0.0f
                          : (g_effect_strength < 0.01f) ? 1.0f
                                                        : 3.0f;
        logf("[NRPRE] F6: EffectStrength = %.1f  (%s)", g_effect_strength,
             g_effect_strength > 2.0f  ? "EXAGERADO -- si no se nota, no llega a pantalla" :
             g_effect_strength < 0.01f ? "apagado" : "normal");
    }
    ex_prev = ex_down;

    static bool mark_prev = false;
    const bool mark_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (mark_down && !mark_prev) {
        static unsigned mark_no = 0;
        logf("[NRPRE] ======== MARK #%u ======== pw=%.4f meas=%.4f valid=%d upd=%u "
             "| hdr=%u det=%d knee=%.3f | expfresh=%u fromgame=%d | cadence=%d "
             "| net=%ux%u finalvalid=%d codec=%d nr=%d phase=%u",
             ++mark_no, g_paper_white, g_pw_measured, (int)g_pw_valid, g_pw_updates,
             g_hdr_mode, (int)g_hdr_detected, g_knee, g_exp_fresh, (int)g_exp_from_game,
             g_skip_n, g_net_w, g_net_h, (int)g_final_valid, (int)g_codec_on,
             (int)g_nr_enabled, g_phase);
        ring_dump("F10");   // what came BEFORE the key: no one reacts inside a frame
        g_jit_burst = 40;
    }
    mark_prev = mark_down;

    // Control for the F10 observation: the dump made the artefact appear, and the
    // only thing it did was block this thread. Reproduce the block by itself --
    // no logging, no state touched, nothing but time -- and see whether the
    // artefact still comes. If it does, the artefact is a pacing failure and not
    // anything this add-on draws.
    static bool stall_prev = false;
    const bool stall_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (stall_down && !stall_prev) {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        // Streamline reports the real events as frames over 100 ms, so reproduce
        // that length, not a token pause. If this brings the black flash on with
        // the add-on disabled, the flash is simply what a hitch of that size looks
        // like at 4x, and NR's only part in it is making hitches more likely.
        const long long target = f.QuadPart * g_stall_ms / 1000;
        do { QueryPerformanceCounter(&t1); } while (t1.QuadPart - t0.QuadPart < target);
        logf("[NRPRE] F8: %d ms de stall en el hilo de presentacion, sin I/O", g_stall_ms);
    }
    stall_prev = stall_down;

    static bool ph_prev = false;
    const bool ph_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (ph_down && !ph_prev) {
        g_phase = (g_phase + 1) % (unsigned)(g_skip_n > 0 ? g_skip_n : 1);
        logf("[NRPRE] F11: cadence phase -> %u/%d", g_phase, g_skip_n);
        g_jit_burst = 40;
    }
    ph_prev = ph_down;

    static bool rp_prev = false;
    const bool rp_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (rp_down && !rp_prev) {
        g_repeat = (g_repeat >= 4) ? 1 : g_repeat + 1;
        logf("[NRPRE] F9: network passes -> %d", g_repeat);
        g_jit_burst = 40;
    }
    rp_prev = rp_down;

    // Keep controls alive while old GPU work drains. Previously this return sat
    // above all hotkeys, so one never-submitted list made F7 appear dead forever.
    static unsigned pending_report = 0;
    if (g_resize_pending) {
        if (g_diagnostics && (++pending_report % 120u) == 0u)
            logf("[NRPRE] cleanup still pending: lists=%zu unsubmitted=%zu fences=%zu signal_failed=%d",
                 g_recorded_lists.size(), g_recorded_lists.pending_count(), g_submission_fences.size(),
                 (int)g_submission_failed);
        return;
    }
    pending_report = 0;

    prof_report();   // estaba definido y nunca se llamaba: nunca hubo una linea PERFIL
    frame_tick();
    // Signal here, not in the eval hook: only at present has the game actually
    // submitted the command list the histogram copy was recorded into.
    if (queue != nullptr && queue->get_device() != nullptr &&
        queue->get_device()->get_api() == reshade::api::device_api::d3d12)
    {
        auto *gq = reinterpret_cast<ID3D12CommandQueue *>(queue->get_native());
        // g_prof.freq was declared and never assigned, so prof_report() bailed on
        // its first line even once it was being called. The queue is the only thing
        // that knows the tick rate, and this is the first place we hold one.
        if (g_prof.ready && g_prof.freq == 0) {
            if (SUCCEEDED(gq->GetTimestampFrequency(&g_prof.freq)))
                logf("[NRPRE] perfilador: %llu ticks/s", (unsigned long long)g_prof.freq);
        }
        tick_exposure(gq);
        exp_queue_tick(g_device, gq);
    }
    install_hook();
    // NOTE: nvngx_dlssnr!NVSDK_NGX_D3D12_CreateFeature is ALREADY detoured by
    // another add-on. A second MinHook trampoline on the same prologue makes the
    // snippet create fail with 0xBAD00002 (PlatformError). Do not hook it.
    // The DLSSNR feature id is 18 (Reserved18), established by probing.
    // install_snippet_hook();
}

// Standalone the proxy owns DllMain; all this one does is announce the add-on
// to ReShade and register its events, none of which exists there.
#ifndef NR_STANDALONE
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_self_module = hModule;
        if (!reshade::register_addon(hModule)) return FALSE;
        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::init_command_queue>(on_init_command_queue);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
#ifndef NR_STANDALONE
        reshade::register_overlay("NR Pre-Upscale", draw_overlay);
#endif
        reshade::register_event<reshade::addon_event::init_effect_runtime>(load_settings);
        reshade::register_event<reshade::addon_event::present>(on_present);
        logf("[NRPRE] addon registered (submission tracking v2; stable depth guides v1; descriptor lifetime v3; NGX feature isolation v1; visibility diagnostics v1 -- configure in the ReShade overlay)");
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
        reshade::unregister_event<reshade::addon_event::init_command_queue>(on_init_command_queue);
        reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::unregister_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(load_settings);
        reshade::unregister_event<reshade::addon_event::present>(on_present);
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
#endif  // NR_STANDALONE

#ifdef NR_STANDALONE
// The standalone proxy drives the same four entry points ReShade used to call.
// Only the caller changes: the logic below this line is the add-on's, unchanged.
//
// The present tick runs from the evaluate hook rather than a real Present hook.
// What it actually requires is that the *previous* frame's command list has been
// submitted before its fences are signalled -- and by the time the next frame's
// first evaluate arrives, it has. That is the whole reason the original comment
// says "signal here, not in the eval hook", and it is satisfied either way,
// without hooking the DXGI swapchain to find out.

extern "C" __declspec(dllexport) void nr_host_device(void *d3d12_device) {
    g_host_dev.native = d3d12_device;
    g_host_dev.api = reshade::api::device_api::d3d12;
    on_init_device(&g_host_dev);
    load_settings(nullptr);
    install_hook();
}

extern "C" __declspec(dllexport) void nr_host_queue(void *d3d12_queue) {
    // The queue the readback fences signal on. Handed over once; the frame tick
    // itself runs from the add-on's own evaluate hook, so nothing else is needed.
    g_host_queue.native = d3d12_queue;
    g_host_queue.dev = &g_host_dev;
}

extern "C" __declspec(dllexport) void nr_host_swapchain_resized() {
    on_init_swapchain(nullptr, true);
}
#endif  // NR_STANDALONE
