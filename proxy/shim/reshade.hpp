// Enough of ReShade's add-on API for src/addon.cpp to compile against nothing.
//
// The point of this file is that the add-on is not rewritten. It is 2700 lines
// that work, and retyping them into a proxy would be a fresh chance to introduce
// the bugs it took a day to remove. So the proxy supplies the four things ReShade
// was supplying -- a device, a queue, a per-frame call and a settings store --
// and the add-on compiles unchanged on top of them.
//
// The surface really is this small: get_api, get_native, get_device, a settings
// pair, and a log line. Everything else ReShade offers went unused.

#pragma once

#include <windows.h>
#include <psapi.h>   // the add-on enumerates modules to find every NGX provider
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace reshade {

namespace api {

enum class device_api { d3d9 = 0, d3d10, d3d11, d3d12, opengl, vulkan };

struct rect { int32_t left, top, right, bottom; };

// The proxy fills these in and hands them to the add-on's entry points. They are
// plain structs rather than interfaces because nothing here is ever implemented
// twice: there is one device and one queue, both D3D12.
struct device {
    void *native = nullptr;
    device_api api = device_api::d3d12;
    device_api get_api() const { return api; }
    void *get_native() const { return native; }
};

struct command_queue {
    device *dev = nullptr;
    void   *native = nullptr;          // the ID3D12CommandQueue the fences signal on
    device *get_device() const { return dev; }
    void   *get_native() const { return native; }
};

struct swapchain {};
struct effect_runtime {};

}  // namespace api

namespace log {
enum class level { error = 1, warning = 2, info = 3, debug = 4 };
// Defined by the proxy: it owns the file.
void message(level lvl, const char *text);
}  // namespace log

// ---------------------------------------------------------------- settings ---
//
// A flat key/value file next to the game executable, read once and written on
// change. ReShade kept these in ReShade.ini under [NRPreUpscale]; the section is
// carried through so the two builds read the same names and a setting learned in
// the overlay can be copied across by hand.

bool get_config_value(api::effect_runtime *, const char *section, const char *key, int &value);
bool get_config_value(api::effect_runtime *, const char *section, const char *key, float &value);
void set_config_value(api::effect_runtime *, const char *section, const char *key, int value);
void set_config_value(api::effect_runtime *, const char *section, const char *key, float value);

// ------------------------------------------------------------------ events ---
//
// The proxy drives its own callbacks, so registration has nothing to do. Kept as
// no-ops purely so the add-on's DllMain compiles untouched.

enum class addon_event {
    init_device, destroy_device, init_swapchain, init_effect_runtime, present
};

template <addon_event E, typename F> inline void register_event(F) {}
template <addon_event E, typename F> inline void unregister_event(F) {}

// The add-on's DllMain announces itself to ReShade. Standalone there is nobody to
// announce to, and the proxy calls the entry points directly.
inline bool register_addon(HMODULE) { return true; }
inline void unregister_addon(HMODULE) {}
inline void register_overlay(const char *, void *) {}

}  // namespace reshade
