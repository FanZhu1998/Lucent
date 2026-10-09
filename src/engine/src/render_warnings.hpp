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
    if (!apply_orientation && metadata.geometry.orientation != image::Orientation::Normal) {
        warnings.push_back(std::format("the file's orientation '{}' is not applied (settings); pixels stay in stored "
                                       "order",
                                       image::to_string(metadata.geometry.orientation)));
    }
}

}  // namespace lucent::engine::detail
