#pragma once

// Preview value types shared by the preview service and its cache (blueprint section 4: "Each
// preview request carries image ID, recipe revision, geometry revision, quality level, and
// generation token").
//
// All types are plain values; a PreviewImage is immutable once published and shared through
// std::shared_ptr<const PreviewImage>, so readers on any thread need no locking.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/application/display_image.hpp>
#include <lucent/application/source_item.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::app {

// Quality level of a preview (append only).
enum class PreviewQualityKind : std::uint8_t {
    Fit = 1,   // the whole image within a square of long_edge pixels (never enlarged)
    Full = 2,  // every rendered pixel (100% view)
};

struct PreviewQuality {
    static constexpr std::int32_t kMinLongEdge = 16;
    static constexpr std::int32_t kMaxLongEdge = 16384;

    PreviewQualityKind kind = PreviewQualityKind::Fit;
    std::int32_t long_edge = 2048;  // Fit only; kMinLongEdge..kMaxLongEdge (ignored for Full)

    [[nodiscard]] static constexpr PreviewQuality fit(std::int32_t long_edge) noexcept {
        return PreviewQuality{PreviewQualityKind::Fit, long_edge};
    }
    [[nodiscard]] static constexpr PreviewQuality full() noexcept {
        return PreviewQuality{PreviewQualityKind::Full, 0};
    }

    // InvalidArgument ("app.preview.quality") for an unknown kind or a Fit edge out of range.
    [[nodiscard]] Status validate() const;

    // Semantic equality: long_edge is compared for Fit only.
    friend bool operator==(const PreviewQuality& a, const PreviewQuality& b) noexcept {
        return a.kind == b.kind && (a.kind != PreviewQualityKind::Fit || a.long_edge == b.long_edge);
    }
};

// "fit-2048", "full".
[[nodiscard]] std::string to_string(const PreviewQuality& quality);

// What the render did, in the terms the editor and diagnostics show.
// What a render did, in the terms the editor and diagnostics show (develop pipeline, ADR 0010).
struct RenderSummary {
    std::uint32_t pipeline_version = 0;  // engine::kDevelopPipelineVersion
    std::string look;                    // base look id, e.g. "neutral"
    std::uint32_t look_version = 0;      // e.g. 1
    // A provisional stage was used (the W2 Neutral v0 rendering; never in the develop pipeline).
    bool provisional = false;
    // Interpretation of the decoded image (after unpacking) and its issues.
    raw::InterpretationStatus interpretation = raw::InterpretationStatus::Complete;
    std::vector<raw::InterpretationIssue> issues;
    std::vector<std::string> warnings;  // DevelopReport::warnings
    // The size of the developed image at full resolution: after the geometry (crop, straighten,
    // quarter turns), i.e. DevelopReport::output_size of a full-resolution render
    // (rendered_full_size()).
    image::Size rendered_size;
    image::Orientation orientation = image::Orientation::Normal;  // baked into the pixels
    std::string demosaic;      // demosaic algorithm id, e.g. "malvar-he-cutler-2004"
    std::string camera_color;  // camera-colour algorithm id, e.g. "camera-to-xyz-dng_color_matrix"
    std::uint64_t clipped_raw_samples = 0;     // samples stage one's white balance clipped (export kind)
    std::uint64_t limited_display_pixels = 0;  // pixels the base look mapped to display white
    std::uint64_t estimated_peak_bytes = 0;    // the memory admitted for the render
    double render_seconds = 0.0;               // stage one + stage two
};

// The full-resolution size of a develop report's image: its output_size when the render was at full
// resolution (upright_size == full_size), otherwise the settings' geometry mapped for full_size.
[[nodiscard]] image::Size rendered_full_size(const engine::DevelopReport& report);

// The summary of a develop report; `interpretation` is the decoded image's.
[[nodiscard]] RenderSummary summarize_render(const engine::DevelopReport& report,
                                             const raw::Interpretation& interpretation);

// Identifies a cached preview: (item, settings, quality). Settings are identified by the SHA-256 of
// their canonical recipe document (recipe.hpp), so equal settings share a preview whatever
// revision or transient edit they come from, and changed settings never hit an old one.
struct PreviewKey {
    ItemId item;
    core::Digest256 settings{};
    PreviewQuality quality;

    friend bool operator==(const PreviewKey&, const PreviewKey&) = default;
};

struct PreviewKeyHash {
    [[nodiscard]] std::size_t operator()(const PreviewKey& key) const noexcept;
};

// A finished preview.
struct PreviewImage {
    PreviewKey key;
    DisplayImage image;   // upright 8-bit sRGB
    Histogram histogram;  // of `image`
    RenderSummary summary;
    std::shared_ptr<const engine::DevelopReport> report;  // the complete render record
    double seconds = 0.0;  // time to produce it: estimate, admission wait, render, fit, encode

    // Memory charged to the preview cache: the pixels plus a fixed allowance for the records.
    [[nodiscard]] std::uint64_t byte_size() const noexcept;
};

}  // namespace lucent::app
