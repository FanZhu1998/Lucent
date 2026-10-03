#pragma once

// Render warnings that follow from the metadata alone (private; shared by the W2 render path and
// the develop pipeline).

#include <format>
#include <string>
#include <vector>

#include <lucent/image/geometry.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::engine::detail {

inline void metadata_warnings(const raw::RawMetadata& metadata, bool apply_orientation,
                              std::vector<std::string>& warnings) {
    if (metadata.interpretation.status != raw::InterpretationStatus::Complete) {
        warnings.push_back(std::format("interpretation is '{}': this render is not a verified interpretation",
                                       raw::to_string(metadata.interpretation.status)));
        for (const raw::InterpretationIssue& issue : metadata.interpretation.issues) {
            warnings.push_back(std::format("{}: {}", raw::to_string(issue.kind), issue.message));
        }
    }
    if (metadata.geometry.pixel_aspect != 1.0) {
        warnings.push_back(std::format("non-square pixels (aspect {}) are not corrected; the image is distorted",
                                       metadata.geometry.pixel_aspect));
    }
    if (metadata.dng && metadata.dng->raw_ifd.default_crop_origin && metadata.dng->raw_ifd.default_crop_size) {
        const auto& origin = *metadata.dng->raw_ifd.default_crop_origin;
        const auto& size = *metadata.dng->raw_ifd.default_crop_size;
        const image::PixelRect& visible = metadata.geometry.visible_area;
        if (origin[0] != 0.0 || origin[1] != 0.0 || size[0] != static_cast<double>(visible.width) ||
            size[1] != static_cast<double>(visible.height)) {
            warnings.push_back(
                std::format("DNG DefaultCrop (origin {}, {}; size {} x {}) is not applied; the whole visible "
                            "area is rendered",
                            origin[0], origin[1], size[0], size[1]));
        }
    }
    if (!apply_orientation && metadata.geometry.orientation != image::Orientation::Normal) {
        warnings.push_back(std::format("the file's orientation '{}' is not applied (settings); pixels stay in stored "
                                       "order",
                                       image::to_string(metadata.geometry.orientation)));
    }
}

}  // namespace lucent::engine::detail
