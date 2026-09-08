#pragma once

namespace nr {

struct EnableResult {
    bool changed = false;
    bool reset_history = false;
    bool restored_strength = false;
};

class ControlState {
public:
    bool enabled = true;
    float effect_strength = 1.0f;

    EnableResult set_enabled(bool next) {
        EnableResult result;
        if (enabled == next) return result;

        result.changed = true;
        enabled = next;
        if (enabled) {
            result.reset_history = true;
            if (effect_strength <= 0.01f) {
                effect_strength = 1.0f;
                result.restored_strength = true;
            }
        }
        return result;
    }

    void cycle_diagnostic_strength() {
        effect_strength = effect_strength > 2.0f ? 1.0f
                        : effect_strength < 0.01f ? 1.0f
                                                  : 3.0f;
    }

    bool ensure_visible_strength() {
        if (!enabled || effect_strength > 0.01f) return false;
        effect_strength = 1.0f;
        return true;
    }

    void reset_defaults() {
        enabled = true;
        effect_strength = 1.0f;
    }
};

} // namespace nr
