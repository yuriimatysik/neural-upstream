// The four things ReShade was providing, implemented for the standalone proxy.
//
// Settings live in neural-upstream.ini next to the game executable, in the same
// [NRPreUpscale] section and under the same key names the ReShade build writes
// into ReShade.ini. Keeping the names identical means a setting worked out in the
// overlay can be copied straight across, and the two builds can be compared
// without wondering whether they were configured the same way.

#include "shim/reshade.hpp"
#include <string>

// ------------------------------------------------------------------- log ---

extern void proxy_log_line(const char *s);   // owned by nrproxy.cpp

namespace reshade { namespace log {
void message(level, const char *text) { proxy_log_line(text); }
}}  // namespace reshade::log

// ------------------------------------------------------------------- ini ---

namespace {

char  g_ini_path[MAX_PATH];
char *g_ini = nullptr;          // whole file, read once
size_t g_ini_len = 0;

void ini_path() {
    if (g_ini_path[0] != 0) return;
    GetModuleFileNameA(nullptr, g_ini_path, MAX_PATH);
    int last = -1;
    for (int i = 0; g_ini_path[i] != 0; ++i) if (g_ini_path[i] == '\\') last = i;
    static const char kName[] = "\\neural-upstream.ini";
    int i = 0;
    for (; kName[i] != 0; ++i) g_ini_path[last + i] = kName[i];
    g_ini_path[last + i] = 0;
}

void ini_load() {
    if (g_ini != nullptr) return;
    ini_path();
    HANDLE h = CreateFileA(g_ini_path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { g_ini = (char *)std::calloc(1, 1); return; }
    const DWORD sz = GetFileSize(h, nullptr);
    g_ini = (char *)std::calloc(1, (size_t)sz + 2);
    DWORD got = 0;
    if (g_ini != nullptr) ReadFile(h, g_ini, sz, &got, nullptr);
    g_ini_len = got;
    CloseHandle(h);
}

// Returns the value text for a key, or nullptr. Section headers are matched but
// not required to be unique: the first [section] that contains the key wins,
// which is the same thing ReShade does and is all this ever needs.
const char *ini_find(const char *section, const char *key) {
    ini_load();
    if (g_ini == nullptr) return nullptr;
    const size_t klen = std::strlen(key);
    const char *p = g_ini;
    bool in_section = false;
    while (*p != 0) {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') ++p;
        if (*p == 0) break;
        if (*p == '[') {
            ++p;
            const char *e = p;
            while (*e != 0 && *e != ']' && *e != '\n') ++e;
            const size_t n = (size_t)(e - p);
            in_section = (n == std::strlen(section) && std::strncmp(p, section, n) == 0);
            p = e;
        } else if (in_section && std::strncmp(p, key, klen) == 0 && p[klen] == '=') {
            return p + klen + 1;
        }
        while (*p != 0 && *p != '\n') ++p;
    }
    return nullptr;
}

// Rewrites one key in place, or appends it. The file is small and written only
// when a setting changes, so simplicity beats cleverness here.
void ini_set(const char *section, const char *key, const char *value) {
    ini_load();
    ini_path();
    const char *old = ini_find(section, key);

    // Build the new contents: everything except the old line, then the new line
    // inside the section (creating the section if it is missing).
    const size_t cap = g_ini_len + std::strlen(key) + std::strlen(value) +
                       std::strlen(section) + 64;
    char *out = (char *)std::calloc(1, cap);
    if (out == nullptr) return;
    size_t o = 0;

    if (old == nullptr) {
        // Append, creating the section if this is the first key.
        if (g_ini_len > 0) { std::memcpy(out, g_ini, g_ini_len); o = g_ini_len; }
        if (ini_find(section, "") == nullptr && std::strstr(g_ini, section) == nullptr) {
            if (o > 0 && out[o - 1] != '\n') out[o++] = '\n';
            o += (size_t)std::snprintf(out + o, cap - o, "[%s]\n", section);
        }
        if (o > 0 && out[o - 1] != '\n') out[o++] = '\n';
        o += (size_t)std::snprintf(out + o, cap - o, "%s=%s\n", key, value);
    } else {
        // Copy up to the value, write the new one, skip the old line's remainder.
        const size_t head = (size_t)(old - g_ini);
        std::memcpy(out, g_ini, head);
        o = head;
        o += (size_t)std::snprintf(out + o, cap - o, "%s", value);
        const char *rest = old;
        while (*rest != 0 && *rest != '\n') ++rest;
        const size_t tail = g_ini_len - (size_t)(rest - g_ini);
        std::memcpy(out + o, rest, tail);
        o += tail;
    }

    HANDLE h = CreateFileA(g_ini_path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, out, (DWORD)o, &w, nullptr);
        CloseHandle(h);
    }
    std::free(g_ini);
    g_ini = out;
    g_ini_len = o;
}

}  // namespace

namespace reshade {

bool get_config_value(api::effect_runtime *, const char *section, const char *key, int &value) {
    const char *v = ini_find(section, key);
    if (v == nullptr) return false;
    value = std::atoi(v);
    return true;
}

bool get_config_value(api::effect_runtime *, const char *section, const char *key, float &value) {
    const char *v = ini_find(section, key);
    if (v == nullptr) return false;
    value = (float)std::atof(v);
    return true;
}

void set_config_value(api::effect_runtime *, const char *section, const char *key, int value) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d", value);
    ini_set(section, key, buf);
}

void set_config_value(api::effect_runtime *, const char *section, const char *key, float value) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", value);
    ini_set(section, key, buf);
}

}  // namespace reshade
