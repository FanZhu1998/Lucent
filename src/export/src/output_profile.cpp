#include "lucent/export/output_profile.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <lucent/image/color_math.hpp>

#include "icc_builder.hpp"
#include "lcms_support.hpp"

namespace lucent::exporter {
namespace {

constexpr std::size_t kProfileCount = 2;  // Srgb, DisplayP3

// Index into the cache; only for validated profiles.
constexpr std::size_t cache_index(OutputProfile profile) noexcept {
    return static_cast<std::size_t>(profile) - 1;
}

image::ColorPrimaries primaries_of(OutputProfile profile) noexcept {
    return profile == OutputProfile::DisplayP3 ? image::ColorPrimaries::DisplayP3
                                               : image::ColorPrimaries::Rec709Srgb;
}

// Process-lifetime cache. Intentionally never destroyed so that spans handed out stay valid
// during static destruction of other objects.
struct IccCache {
    std::mutex mutex;
    std::array<std::unique_ptr<const std::vector<std::uint8_t>>, kProfileCount> bytes;
};

IccCache& icc_cache() {
    static IccCache* cache = new IccCache();  // NOLINT(cppcoreguidelines-owning-memory): leaked on purpose
    return *cache;
}

}  // namespace

std::string_view to_string(OutputProfile profile) noexcept {
    switch (profile) {
        case OutputProfile::Srgb:
            return "srgb";
        case OutputProfile::DisplayP3:
            return "display-p3";
    }
    return "unknown";
}

std::string_view to_string(RenderingIntent intent) noexcept {
    switch (intent) {
        case RenderingIntent::Perceptual:
            return "perceptual";
        case RenderingIntent::RelativeColorimetric:
            return "relative-colorimetric";
        case RenderingIntent::Saturation:
            return "saturation";
        case RenderingIntent::AbsoluteColorimetric:
            return "absolute-colorimetric";
    }
    return "unknown";
}

std::string_view to_string(BitDepth depth) noexcept {
    switch (depth) {
        case BitDepth::Eight:
            return "8";
        case BitDepth::Sixteen:
            return "16";
    }
    return "unknown";
}

Status validate(OutputProfile profile) {
    if (to_string(profile) == "unknown") {
        return fail(ErrorCode::InvalidArgument,
                    "unknown output profile value " + std::to_string(static_cast<int>(profile)),
                    "export.profile.validate");
    }
    return ok();
}

Status validate(RenderingIntent intent) {
    if (to_string(intent) == "unknown") {
        return fail(ErrorCode::InvalidArgument,
                    "unknown rendering intent value " + std::to_string(static_cast<int>(intent)),
                    "export.intent.validate");
    }
    return ok();
}

Status validate(BitDepth depth) {
    if (to_string(depth) == "unknown") {
        return fail(ErrorCode::InvalidArgument,
                    "unknown bit depth value " + std::to_string(static_cast<int>(depth)),
                    "export.bit_depth.validate");
    }
    return ok();
}

Expected<image::ColorEncoding> output_encoding(OutputProfile profile) {
    if (auto status = validate(profile); !status) {
        return Unexpected(std::move(status.error()));
    }
    image::ColorEncoding encoding = image::ColorEncoding::srgb_display();
    encoding.primaries = primaries_of(profile);
    return encoding;
}

std::string_view icc_description(OutputProfile profile) noexcept {
    switch (profile) {
        case OutputProfile::Srgb:
            return "sRGB IEC61966-2.1 (Lucent)";
        case OutputProfile::DisplayP3:
            return "Display P3 (Lucent)";
    }
    return {};
}

Expected<std::span<const std::uint8_t>> icc_profile(OutputProfile profile) {
    if (auto status = validate(profile); !status) {
        return Unexpected(std::move(status.error()));
    }
    IccCache& cache = icc_cache();
    const std::lock_guard lock(cache.mutex);
    auto& slot = cache.bytes[cache_index(profile)];
    if (!slot) {
        auto primaries = image::rgb_primaries(primaries_of(profile));
        if (!primaries) {
            return Unexpected(std::move(primaries.error()));
        }
        auto built = detail::build_display_icc(*primaries, image::kD65White, icc_description(profile));
        if (!built) {
            return Unexpected(std::move(built.error()));
        }
        try {
            slot = std::make_unique<const std::vector<std::uint8_t>>(std::move(*built));
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot cache the ICC profile", "export.profile.icc");
        }
    }
    return std::span<const std::uint8_t>(*slot);
}

}  // namespace lucent::exporter
