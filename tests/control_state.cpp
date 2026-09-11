#include "control_state.h"

#include <cstdio>
#include <cstdlib>

static void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main() {
    nr::ControlState state;
    require(state.enabled && state.effect_strength == 1.0f,
            "fresh controls use a visible enabled default");

    state.enabled = false;
    state.effect_strength = 0.0f;
    const auto enabled = state.set_enabled(true);
    require(enabled.changed && enabled.reset_history,
            "off to on requests a temporal history reset");
    require(enabled.restored_strength && state.effect_strength == 1.0f,
            "enabling restores a zero visual strength");

    const auto unchanged = state.set_enabled(true);
    require(!unchanged.changed && !unchanged.reset_history,
            "reapplying the enabled state does not reset history");
    const auto disabled = state.set_enabled(false);
    require(disabled.changed && !disabled.reset_history,
            "turning NR off does not request a history reset");

    state.enabled = true;
    state.effect_strength = 0.0f;
    require(state.ensure_visible_strength() && state.effect_strength == 1.0f,
            "loading enabled NR repairs a saved zero strength");
    require(!state.ensure_visible_strength(),
            "an already visible strength needs no repair");

    state.enabled = false;
    state.effect_strength = 0.0f;
    state.reset_defaults();
    require(state.enabled && state.effect_strength == 1.0f,
            "reset defaults restores enabled visible controls");

    std::puts("control state: all regression tests passed");
}
