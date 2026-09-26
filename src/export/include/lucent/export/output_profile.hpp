#pragma once

// Output colour profiles for exported files (blueprint section 5 steps 7-8, section 7 "Export
// integrity and user-selected destinations").
//
// Namespace: `export` is a C++ keyword, so the export module's namespace is lucent::exporter
// (headers stay under lucent/export/, the library is lucent_export / Lucent::export).
//
// An OutputProfile names a display-referred RGB encoding together with the ICC profile that is
// embedded in every file written with it. The ICC bytes are generated with Little CMS on first
// use, are deterministic (fixed description, copyright and creation date; identical bytes on
// every run of the same build), and are cached for the lifetime of the process.
//
// Profiles are ICC v4.3 display ("mntr") matrix/TRC profiles with the parametric sRGB curve
// (ICC parametric type 3: g = 2.4, a = 1/1.055, b = 0.055/1.055, c = 1/12.92, d = 0.04045).
// Colorants are chromatically adapted from D65 to the D50 PCS with Bradford (recorded in the
// 'chad' tag), as the ICC specification requires. The profile ID field is left zero ("not
// computed"), which the ICC specification permits.
//
// All enum values are persisted (recipes, job records, receipts) and must never change; append
// new values only.
//
// Thread-safety: every function here is safe to call concurrently. icc_profile() initialises
// its cache under a lock; the returned bytes are immutable and live until process exit.

#include <cstdint>
#include <span>
#include <string_view>

#include <lucent/core/error.hpp>
#include <lucent/image/color_encoding.hpp>

namespace lucent::exporter {

// Output colour space of an exported file. Planned additions (appended, never renumbered):
// Rec2020 = 3, ProPhotoRomm = 4.
enum class OutputProfile : std::uint8_t {
    Srgb = 1,       // IEC 61966-2-1 sRGB: BT.709 primaries, D65 white, sRGB curve
    DisplayP3 = 2,  // Display P3: SMPTE RP 431-2 (P3) primaries, D65 white, sRGB curve
};

// ICC rendering intent used for the output conversion. The numeric values are Lucent's own
// persisted values, not the ICC header codes. For the matrix/TRC profiles above, Perceptual and
// Saturation have no dedicated tables and behave like RelativeColorimetric (Little CMS falls
// back to the colorimetric matrix path); AbsoluteColorimetric additionally preserves the source
// white instead of mapping it to the destination white.
enum class RenderingIntent : std::uint8_t {
    Perceptual = 1,
    RelativeColorimetric = 2,
    Saturation = 3,
    AbsoluteColorimetric = 4,
};

// Bits per sample of quantised output. The numeric value is the bit count.
enum class BitDepth : std::uint8_t {
    Eight = 8,
    Sixteen = 16,
};

// Stable lowercase identifiers for logs, receipts and reports ("srgb", "display-p3",
// "relative-colorimetric", "8", "16"). Unknown values yield "unknown".
[[nodiscard]] std::string_view to_string(OutputProfile profile) noexcept;
[[nodiscard]] std::string_view to_string(RenderingIntent intent) noexcept;
[[nodiscard]] std::string_view to_string(BitDepth depth) noexcept;

// InvalidArgument for values not listed above (for example read from a newer record).
[[nodiscard]] Status validate(OutputProfile profile);
[[nodiscard]] Status validate(RenderingIntent intent);
[[nodiscard]] Status validate(BitDepth depth);

// The colour encoding of pixels quantised for `profile`: RGB, the profile's primaries, D65,
// TransferFunction::Srgb, NumericRange::UnitClamped, ImageReference::Display.
// InvalidArgument for an unknown profile.
[[nodiscard]] Expected<image::ColorEncoding> output_encoding(OutputProfile profile);

// The 'desc' text written into the profile, e.g. "sRGB IEC61966-2.1 (Lucent)".
// Empty for an unknown profile.
[[nodiscard]] std::string_view icc_description(OutputProfile profile) noexcept;

// The ICC profile bytes embedded for `profile`. The span refers to process-lifetime storage
// owned by the cache; callers must not free it. Every call (in any thread) for the same profile
// returns the same bytes. Errors: InvalidArgument for an unknown profile; Internal when Little
// CMS fails to build or serialise the profile (a failed build is not cached, so a later call
// retries); OutOfMemory.
[[nodiscard]] Expected<std::span<const std::uint8_t>> icc_profile(OutputProfile profile);

}  // namespace lucent::exporter
