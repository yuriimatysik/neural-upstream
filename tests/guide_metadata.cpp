#include "guide_metadata.h"

#include <cassert>
#include <cmath>

static bool near(float left, float right) {
    return std::fabs(left - right) < 0.0001f;
}

int main() {
    {
        nr::GuideMetadataInput input{};
        input.work_width = input.render_width = input.depth_width = 1920;
        input.work_height = input.render_height = input.depth_height = 1080;
        input.output_width = input.motion_width = 3840;
        input.output_height = input.motion_height = 2160;
        input.motion_low_resolution = false;
        const nr::GuideMetadata metadata = nr::resolve_guide_metadata(input);
        assert(metadata.depth.width == 1920 && metadata.depth.height == 1080);
        assert(metadata.motion.width == 3840 && metadata.motion.height == 2160);
        assert(near(metadata.motion_scale_x, 0.5f));
        assert(near(metadata.motion_scale_y, 0.5f));
    }
    {
        nr::GuideMetadataInput input{};
        input.work_width = input.render_width = 1280;
        input.work_height = input.render_height = 720;
        input.output_width = 2560;
        input.output_height = 960;
        input.depth_width = input.motion_width = 1344;
        input.depth_height = input.motion_height = 768;
        input.depth_base_x = input.motion_base_x = 32;
        input.depth_base_y = input.motion_base_y = 24;
        input.motion_low_resolution = true;
        input.motion_scale_x = 2.0f;
        input.motion_scale_y = -2.0f;
        const nr::GuideMetadata metadata = nr::resolve_guide_metadata(input);
        assert(metadata.depth.base_x == 32 && metadata.depth.base_y == 24);
        assert(metadata.motion.base_x == 32 && metadata.motion.base_y == 24);
        assert(metadata.motion.width == 1280 && metadata.motion.height == 720);
        assert(near(metadata.motion_scale_x, 1.0f));
        assert(near(metadata.motion_scale_y, -1.5f));
    }
    {
        nr::GuideMetadataInput input{};
        input.work_width = input.render_width = 100;
        input.work_height = input.render_height = 80;
        input.output_width = 200;
        input.output_height = 160;
        input.depth_width = input.motion_width = 110;
        input.depth_height = input.motion_height = 90;
        input.depth_base_x = input.motion_base_x = 20;
        input.depth_base_y = input.motion_base_y = 20;
        input.motion_low_resolution = true;
        const nr::GuideMetadata metadata = nr::resolve_guide_metadata(input);
        assert(metadata.depth.width == 90 && metadata.depth.height == 70);
        assert(metadata.motion.width == 90 && metadata.motion.height == 70);
    }
    {
        nr::GuideMetadataInput input{};
        input.work_width = input.render_width = 100;
        input.work_height = input.render_height = 80;
        input.output_width = input.motion_width = 200;
        input.output_height = input.motion_height = 160;
        input.depth_width = 90;
        input.depth_height = 70;
        input.motion_low_resolution = true;
        const nr::GuideMetadata metadata = nr::resolve_guide_metadata(input);
        assert(metadata.depth.width == 90 && metadata.depth.height == 70);
        assert(metadata.motion.width == 90 && metadata.motion.height == 70);
    }
}
