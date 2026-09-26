#pragma once

// Private construction of ICC profiles with Little CMS.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/image/color_math.hpp>

#include "lcms_support.hpp"

namespace lucent::exporter::detail {

// Fixed ICC header creation date (UTC) written into every generated profile so the bytes are
// identical run to run: 2026-01-01 00:00:00.
inline constexpr std::uint16_t kIccCreationDate[6] = {2026, 1, 1, 0, 0, 0};

// Offset of the dateTimeNumber field in the ICC header (ICC.1:2022 section 7.2.1).
inline constexpr std::size_t kIccDateOffset = 24;

// Copyright text written into every generated profile.
inline constexpr std::string_view kIccCopyright = "No copyright, use freely";

// Serialised ICC v4.3 display profile: matrix/TRC with the given primaries and white, the
// parametric sRGB curve on all three channels, 'desc' = description, 'cprt' = kIccCopyright,
// creation date kIccCreationDate, profile ID zero. Deterministic for a given Little CMS
// version. Internal when Little CMS fails; OutOfMemory.
[[nodiscard]] Expected<std::vector<std::uint8_t>> build_display_icc(const image::RgbPrimaries& primaries,
                                                                    image::Chromaticity white,
                                                                    std::string_view description);

// In-memory (never serialised) RGB profile with the given primaries and white and identity
// (gamma 1.0) curves, for use as a float transform input. Internal when Little CMS fails.
[[nodiscard]] Expected<UniqueProfile> create_linear_rgb_profile(const LcmsContext& context,
                                                                const image::RgbPrimaries& primaries,
                                                                image::Chromaticity white);

}  // namespace lucent::exporter::detail
