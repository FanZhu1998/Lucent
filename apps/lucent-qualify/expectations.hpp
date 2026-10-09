#pragma once

// Qualification expectations (fixtures/qualification/expectations.json, schema
// "lucent.qualification-expectations" version 1; ADR 0016).
//
// For every corpus fixture the file records facts a reviewer can check independently of Lucent
// and of LibRaw, each with its sources, and the outcome the qualification suite is expected to
// produce:
//   * camera identity (file make/model, product, qualification family);
//   * the expected mode key (mode_key.hpp) and the compression mode;
//   * dimensions: the upright size Lucent's default develop render outputs (the camera's image
//     area the file declares, ADR 0017, or the decoder's visible area when it declares none), the
//     camera's own image area (manufacturer specification, or the file's declared default crop),
//     how the two relate, and where Lucent's area comes from ("image_area_source": the decoded-RAW
//     contract's ImageAreaSource name, optional);
//   * orientation (EXIF value) and representation (CFA at the visible-area origin, linear or
//     monochrome);
//   * mandatory interpretation steps (DNG opcodes Lucent applies, steps the decoder performs,
//     steps Lucent cannot perform);
//   * the expected proposed state, check statuses and limitations, which the GTest wrapper
//     asserts so that a regression in camera handling fails CI.
//
// Evidence rule (enforced by load_expectations): a fact whose status is "verified" must cite at
// least one source of an independent kind: manufacturer_spec (a manufacturer's page or manual),
// file_tag (a value the file declares, read by tests/qualification/tools/file_facts.py, which
// uses neither LibRaw nor Lucent), independent_decoder (another decoder's camera database, e.g.
// RawSpeed's cameras.xml) or reviewer_visual (a dated human inspection of Lucent's output).
// decoder_reference (LibRaw / rawpy) and archive_metadata (the fixture archive's labels) are
// recorded but never suffice on their own. Anything that could not be checked stays
// "unverified".

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <lucent/assets/camera_support.hpp>
#include <lucent/core/error.hpp>

namespace lucent::qualify {

inline constexpr std::string_view kExpectationsSchema = "lucent.qualification-expectations";
inline constexpr int kExpectationsSchemaVersion = 1;

// Stable identifiers used in expectations and reports.
//   Checks (in execution order).
inline constexpr std::string_view kCheckFixtureIntegrity = "fixture_integrity";
inline constexpr std::string_view kCheckDecodeParity = "decode_parity";
inline constexpr std::string_view kCheckModeKey = "mode_key";
inline constexpr std::string_view kCheckSubimages = "subimages";
inline constexpr std::string_view kCheckInterpretation = "interpretation";
inline constexpr std::string_view kCheckCfa = "cfa";
inline constexpr std::string_view kCheckBlackWhite = "black_white";
inline constexpr std::string_view kCheckColour = "colour";
// ADR 0020: the develop path's colours against a chart's published values (chart fixtures only).
inline constexpr std::string_view kCheckColourReference = "colour_reference";
inline constexpr std::string_view kCheckDimensions = "dimensions";
inline constexpr std::string_view kCheckOrientation = "orientation";
inline constexpr std::string_view kCheckRequiredCorrections = "required_corrections";
inline constexpr std::string_view kCheckNumericalValidity = "numerical_validity";
inline constexpr std::string_view kCheckExportRoundTrip = "export_round_trip";
inline constexpr std::string_view kCheckDeterminism = "determinism";
[[nodiscard]] const std::vector<std::string_view>& check_ids();

//   Limitations (known optional capabilities that are missing): the codes the camera support
//   database and the application share (lucent/assets/camera_support.hpp).
inline constexpr std::string_view kLimitDefaultCrop = assets::kLimitationDefaultCrop;
inline constexpr std::string_view kLimitXTransBaseline = assets::kLimitationXTransBaseline;
inline constexpr std::string_view kLimitDualPixel = assets::kLimitationDualPixel;
inline constexpr std::string_view kLimitEnhancedImage = assets::kLimitationEnhancedImage;
inline constexpr std::string_view kLimitOptionalOperations = assets::kLimitationOptionalOperations;
inline constexpr std::string_view kLimitNonSquarePixels = assets::kLimitationNonSquarePixels;
inline constexpr std::string_view kLimitPixelShift = assets::kLimitationPixelShift;
[[nodiscard]] const std::vector<std::string_view>& limitation_ids();

//   States and check statuses as written in JSON.
[[nodiscard]] bool is_state_id(std::string_view id) noexcept;         // "verified", ...
[[nodiscard]] bool is_check_status_id(std::string_view id) noexcept;  // "pass", ...

// Source kinds; the first four are independent of Lucent and LibRaw.
[[nodiscard]] bool is_independent_source_kind(std::string_view kind) noexcept;
[[nodiscard]] bool is_source_kind(std::string_view kind) noexcept;

struct Source {
    std::string id;
    std::string kind;  // see above
    std::string title;
    std::string url;        // may be empty (file_tag, reviewer_visual)
    std::string retrieved;  // YYYY-MM-DD
    std::string quote;      // the exact text or value relied on
};

struct Fact {
    std::string status = "unverified";  // "verified", "unverified", "not_applicable"
    std::vector<std::string> sources;   // Source ids
    std::string note;

    [[nodiscard]] bool verified() const noexcept { return status == "verified"; }
};

struct Size2 {
    std::int32_t width = 0;
    std::int32_t height = 0;
    friend bool operator==(const Size2&, const Size2&) = default;
};

struct MandatoryStep {
    // "dng_opcode:<list>:<index>:<name>" (checked against the develop report), or a documentary
    // id such as "decoder:ycbcr_to_rgb", "stitch:dual_fisheye_360", "subimage:select_primary".
    std::string id;
    // "applied" (Lucent's engine performs it), "decoder" (the decoder performs it while
    // unpacking), "unsupported" (nobody in Lucent performs it: the file cannot be verified).
    std::string lucent_support;
    std::string description;
};

struct FixtureExpectation {
    std::string id;
    // Camera.
    std::string family;  // "canon", "sony", "dji", "insta360", "nikon", "fujifilm", "other"
    std::string file_make;
    std::string file_model;
    std::string product;
    Fact identity;
    // Mode.
    std::string mode_key;  // canonical text, mode_key.hpp
    std::string mode_label;
    Fact compression;
    // Dimensions, in upright pixels (a file shot in portrait lists the manufacturer's landscape sizes
    // with width and height exchanged).
    std::optional<Size2> rendered;           // absent when the decoder refuses the file
    std::optional<Size2> camera_image_area;  // the camera's own image size for this mode
    Fact camera_image_area_fact;
    std::string dimensions_note;
    // raw::to_string(ImageAreaSource) the decoder reports ("dng_default_crop", "decoder_inset_crop",
    // "dng_default_whole_image", "unknown"); absent: not asserted.
    std::optional<std::string> image_area_source;
    // QUA-07: where the develop render placed the camera's area in its upright image
    // ("image_area_applied": [x, y, width, height], or null when the frame is the whole image). An
    // outcome recorded from a reviewed run, like "rendered" (its origin is the declared crop origin
    // the file's tags give, cross-checked in the note); a placement that moves while the size stays
    // fails. Not asserted when absent (image_area_applied_asserted false).
    bool image_area_applied_asserted = false;
    std::optional<std::array<std::int32_t, 4>> image_area_applied;
    // Orientation.
    int orientation_exif = 1;
    Fact orientation;
    // Representation ("mosaic:RGGB", "linear-rgb", "monochrome"), as in the mode key.
    std::string representation;
    Fact representation_fact;
    std::vector<MandatoryStep> mandatory_steps;
    // Expected outcome.
    std::string expected_state;
    std::map<std::string, std::string> expected_checks;  // check id -> status
    std::vector<std::string> expected_limitations;       // sorted
    std::string notes;
};

struct Expectations {
    int schema_version = kExpectationsSchemaVersion;
    std::uint32_t mode_key_version = 0;
    std::string reviewed;  // YYYY-MM-DD
    std::map<std::string, Source> sources;
    std::vector<FixtureExpectation> fixtures;  // file order

    [[nodiscard]] const FixtureExpectation* find(std::string_view id) const noexcept;
};

// Parses and validates an expectations document. Errors (where "qualify.expectations"):
// VersionMismatch for another schema name, a newer schema_version or a mode_key_version other
// than kModeKeyVersion; InvalidArgument for a malformed document, an unknown source id, state,
// check, status or limitation id, a duplicate fixture, or a verified fact without an independent
// source (the message names the fixture and the fact).
[[nodiscard]] Expected<Expectations> parse_expectations(const nlohmann::json& document);
[[nodiscard]] Expected<Expectations> load_expectations(const std::filesystem::path& path);

// Reads a JSON file. NotFound / InvalidArgument (malformed JSON), where "qualify.json".
[[nodiscard]] Expected<nlohmann::json> read_json_file(const std::filesystem::path& path);

}  // namespace lucent::qualify
