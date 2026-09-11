#pragma once

#include <algorithm>

namespace nr {

struct GuideRegion {
    unsigned base_x = 0;
    unsigned base_y = 0;
    unsigned width = 0;
    unsigned height = 0;
};

struct GuideMetadataInput {
    unsigned work_width = 0;
    unsigned work_height = 0;
    unsigned output_width = 0;
    unsigned output_height = 0;
    unsigned render_width = 0;
    unsigned render_height = 0;
    unsigned depth_width = 0;
    unsigned depth_height = 0;
    unsigned motion_width = 0;
    unsigned motion_height = 0;
    unsigned depth_base_x = 0;
    unsigned depth_base_y = 0;
    unsigned motion_base_x = 0;
    unsigned motion_base_y = 0;
    bool motion_low_resolution = true;
    float motion_scale_x = 1.0f;
    float motion_scale_y = 1.0f;
};

struct GuideMetadata {
    GuideRegion depth;
    GuideRegion motion;
    float motion_scale_x = 1.0f;
    float motion_scale_y = 1.0f;
};

inline GuideRegion clamp_guide_region(unsigned base_x, unsigned base_y,
                                      unsigned wanted_width, unsigned wanted_height,
                                      unsigned allocation_width, unsigned allocation_height) {
    GuideRegion result{};
    result.base_x = std::min(base_x, allocation_width);
    result.base_y = std::min(base_y, allocation_height);
    result.width = std::min(wanted_width, allocation_width - result.base_x);
    result.height = std::min(wanted_height, allocation_height - result.base_y);
    return result;
}

inline GuideMetadata resolve_guide_metadata(const GuideMetadataInput &input) {
    GuideMetadata result{};
    const unsigned render_width = input.render_width ? input.render_width : input.work_width;
    const unsigned render_height = input.render_height ? input.render_height : input.work_height;
    const unsigned output_width = input.output_width ? input.output_width : input.work_width;
    const unsigned output_height = input.output_height ? input.output_height : input.work_height;
    result.depth = clamp_guide_region(input.depth_base_x, input.depth_base_y,
                                      render_width, render_height,
                                      input.depth_width, input.depth_height);
    const unsigned motion_width = input.motion_low_resolution ? render_width : output_width;
    const unsigned motion_height = input.motion_low_resolution ? render_height : output_height;
    result.motion = clamp_guide_region(input.motion_base_x, input.motion_base_y,
                                       motion_width, motion_height,
                                       input.motion_width, input.motion_height);
    result.motion_scale_x = input.motion_scale_x * static_cast<float>(input.work_width) /
                            static_cast<float>(output_width ? output_width : 1u);
    result.motion_scale_y = input.motion_scale_y * static_cast<float>(input.work_height) /
                            static_cast<float>(output_height ? output_height : 1u);
    return result;
}

} // namespace nr
