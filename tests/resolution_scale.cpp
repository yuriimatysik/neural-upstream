#include "resolution_scale.h"
#include "guide_metadata.h"

#include <cassert>
#include <cmath>
#include <limits>

static bool near(float left, float right) {
    return std::fabs(left - right) < 0.0001f;
}

int main() {
    // The same decision serves live UI changes and effect-runtime config reloads.
    assert(nr::codec_requires_resolution_cleanup(true, false, 0.75f));
    assert(!nr::codec_requires_resolution_cleanup(true, false, 1.0f));
    assert(!nr::codec_requires_resolution_cleanup(false, true, 0.75f));
    assert(!nr::codec_requires_resolution_cleanup(false, false, 0.75f));
    assert(near(nr::clamp_resolution_scale(0.25f), 0.50f));
    assert(near(nr::clamp_resolution_scale(0.85f), 0.85f));
    assert(near(nr::clamp_resolution_scale(2.0f), 1.00f));
    assert(near(nr::clamp_resolution_scale(std::numeric_limits<float>::quiet_NaN()), 1.00f));

    const nr::ResolutionPlan native = nr::resolve_resolution_plan(1920, 1080, 1.0f);
    assert(!native.scaled && native.width == 1920 && native.height == 1080);

    const nr::ResolutionPlan almost_native = nr::resolve_resolution_plan(1920, 1080, 0.9995f);
    assert(!almost_native.scaled && near(almost_native.scale, 1.0f));

    const nr::ResolutionPlan three_quarters = nr::resolve_resolution_plan(1920, 1080, 0.75f);
    assert(three_quarters.scaled);
    assert(three_quarters.width == 1440 && three_quarters.height == 810);

    const nr::ResolutionPlan eighty_five = nr::resolve_resolution_plan(1920, 1080, 0.85f);
    assert(eighty_five.scaled);
    assert(eighty_five.width == 1632 && eighty_five.height == 918);

    const nr::ResolutionPlan odd = nr::resolve_resolution_plan(1365, 767, 0.73f);
    assert((odd.width & 1u) == 0 && (odd.height & 1u) == 0);

    const nr::ResolutionPlan tiny = nr::resolve_resolution_plan(80, 70, 0.50f);
    assert(tiny.width == 64 && tiny.height == 64);

    nr::GuideMetadataInput guide{};
    guide.work_width = eighty_five.width;
    guide.work_height = eighty_five.height;
    guide.render_width = 1920;
    guide.render_height = 1080;
    guide.output_width = 3840;
    guide.output_height = 2160;
    guide.depth_width = 1920;
    guide.depth_height = 1080;
    guide.motion_width = 3840;
    guide.motion_height = 2160;
    guide.motion_low_resolution = false;
    guide.motion_scale_x = 2.0f;
    guide.motion_scale_y = -2.0f;
    const nr::GuideMetadata metadata = nr::resolve_guide_metadata(guide);
    assert(near(metadata.motion_scale_x, 2.0f * 1632.0f / 3840.0f));
    assert(near(metadata.motion_scale_y, -2.0f * 918.0f / 2160.0f));
}
