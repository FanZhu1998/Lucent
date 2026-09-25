#pragma once

// Builds lucent.probe-report (schema version 1) documents: the report format shared by
// lucent-probe and the rawpy reference (research/README.md, "Probe report contract").
//
// A report holds the file's identity, the generator, the "parity" block (values directly
// comparable with the rawpy reference, from lucent::raw::ReferenceParity) and Lucent's full
// structured metadata. Parity semantics follow rawpy 0.27.1 exactly. Two points to note:
//   * "color_matrix" is LibRaw's cmatrix, because that is what rawpy's color_matrix reads (the
//     shared contract text names rgb_cam, which rawpy does not report; research/README.md).
//   * Floating-point DNG data: Lucent keeps it as float (LIBRAW_RAWOPTIONS_CONVERTFLOAT_TO_INT
//     cleared), while rawpy lets LibRaw convert it to integers. For such files "raw_type" is
//     "stack" (LibRaw's raw_image is not used), "raw_pattern" is null and no raw_data_sha256 is
//     produced, so their parity blocks are not comparable with rawpy's. The corpus has no such
//     file (docs/adr/0005-raw-decoder-boundary.md).
//
// JSON numbers: floats are LibRaw's float values widened exactly to double; non-finite values
// are written as the strings "nan", "inf" and "-inf".
//
// This is a developer-tool library (apps/), not part of any module's public API.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/decoder_registry.hpp>

namespace lucent::probe {

using Json = nlohmann::ordered_json;

inline constexpr std::string_view kReportSchema = "lucent.probe-report";
inline constexpr int kReportSchemaVersion = 1;
inline constexpr std::string_view kGeneratorName = "lucent-probe";

struct ReportOptions {
    // Unpack the pixel data: parity values are then taken after unpacking and raw_data_sha256
    // is added for single-channel ("flat") data. Without it, parity values are those known after
    // opening the file (see "parity_stage").
    bool decode = false;
    // Image to probe/decode. Required by decoding for multi-image files.
    std::optional<std::uint32_t> subimage;
    // Add file_sha256 (a full read of the file).
    bool hash_file = true;
};

struct Report {
    Json document;
    bool ok = false;  // "status" == "ok"
};

// Probes (and optionally decodes) `path` with `registry` and returns its report. Never fails:
// every failure is described inside the report ("status": "error").
// The file is stat'ed, hashed, probed and decoded through separate opens. The FileIdentity each
// step observed must equal the one captured while hashing (or stat'ing, without hashing);
// otherwise the report fails with source_changed at "probe.report", so file_sha256 always
// describes the bytes the metadata and parity values were read from.
[[nodiscard]] Report build_report(const raw::DecoderRegistry& registry, const std::filesystem::path& path,
                                  const ReportOptions& options, core::CancellationToken cancel);

// Building blocks, exposed for tests.
[[nodiscard]] Json generator_json();
[[nodiscard]] Json error_json(const Error& error);
[[nodiscard]] Json metadata_json(const raw::RawMetadata& metadata);
[[nodiscard]] Json subimages_json(const std::vector<raw::SubimageInfo>& subimages);
// The parity block; `raw_data_sha256` is added when set.
[[nodiscard]] Json parity_json(const raw::ReferenceParity& parity,
                               const std::optional<std::string>& raw_data_sha256);

// SHA-256 (lowercase hex) of single-channel uint16 decoded data as raw_height rows of raw_width
// little-endian samples without padding: the parity block's raw_data_sha256. std::nullopt for
// other data (full-colour or floating-point).
[[nodiscard]] Expected<std::optional<std::string>> raw_data_sha256(const raw::DecodedRaw& decoded);

}  // namespace lucent::probe
