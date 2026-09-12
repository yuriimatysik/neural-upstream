#pragma once

#include <algorithm>
#include <cmath>

namespace nr {

constexpr float kMinResolutionScale = 0.50f;
constexpr float kMaxResolutionScale = 1.00f;
constexpr float kNativeResolutionThreshold = 0.999f;

struct ResolutionPlan {
    float scale = 1.0f;
    unsigned width = 0;
    unsigned height = 0;
    bool scaled = false;
};

inline float clamp_resolution_scale(float value) {
    if (!std::isfinite(value)) return 1.0f;
    const float clamped = std::clamp(value, kMinResolutionScale, kMaxResolutionScale);
    return clamped >= kNativeResolutionThreshold ? 1.0f : clamped;
}

inline bool codec_requires_resolution_cleanup(bool previous, bool enabled, float active_scale) {
    return previous && !enabled && active_scale < kNativeResolutionThreshold;
}

inline unsigned scaled_even_dimension(unsigned native, float scale) {
    if (native == 0) return 0;
    if (scale >= kNativeResolutionThreshold) return native;
    unsigned value = static_cast<unsigned>(std::lround(native * scale));
    value &= ~1u;
    const unsigned minimum = std::min(native, 64u);
    value = std::max(value, minimum);
    return std::min(value, native);
}

inline ResolutionPlan resolve_resolution_plan(unsigned width, unsigned height, float requested) {
    ResolutionPlan result{};
    result.scale = clamp_resolution_scale(requested);
    if (result.scale >= kNativeResolutionThreshold) {
        result.scale = 1.0f;
        result.width = width;
        result.height = height;
        return result;
    }
    result.width = scaled_even_dimension(width, result.scale);
    result.height = scaled_even_dimension(height, result.scale);
    result.scaled = result.width != width || result.height != height;
    if (!result.scaled) result.scale = 1.0f;
    return result;
}

} // namespace nr
