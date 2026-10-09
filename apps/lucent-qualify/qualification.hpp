#pragma once

// The camera qualification check suite (ADR 0016; blueprint section 3 "User-visible
// compatibility states" and section 10 "Camera correctness" gate).
//
// qualify_fixture() runs one corpus fixture through Lucent's real decode, render, develop and
// export code and records, per check, a status (pass, fail, not_applicable, not_run), the
// strength of its evidence and the measured values:
//
//   fixture_integrity     the cached file has the manifest's size and SHA-256
//   decode_parity         the decoded file matches the committed rawpy reference
//                         (fixtures/reference/<id>.parity.json) field for field, including the
//                         SHA-256 of single-channel raw data; a file both decoders refuse passes
//                         when Lucent refuses it as Unsupported
//   mode_key              the mode key derived from the decode equals the expected one
//   subimages             roles as probed; a multi-image file refuses a decode without an explicit
//                         subimage (blueprint section 3: never the first image silently)
//   interpretation        the contract's interpretation status is Complete or
//                         OptionalOperationsSkipped (a mandatory step is available)
//   cfa                   representation and CFA at the visible-area origin equal the expectation;
//                         for Bayer data, the two sites labelled green have the closest means of
//                         the two diagonals (an independent physical check of the CFA phase)
//   black_white           white above every black level; for a DNG, exactly the file's
//                         WhiteLevel; no clip plateau below the white level (which would leave
//                         clipped highlights short of white); at most 0.1 % of samples more than
//                         2 % of the range below black. Samples above white are recorded, not
//                         failed: the engine treats them as saturated
//   colour                the W2 render without DNG opcodes (like the reference decoder) against
//                         the committed rawpy region statistics
//                         (tests/integration/data/render_reference.json): the outer gross-error
//                         gates and the per-fixture regression gates of ADR 0006. Evidence is
//                         strong only when both use the same calibration (decoder matrix or
//                         monochrome); DNG camera-profile files are weak (gross errors only)
//   dimensions            the develop render's upright size equals the expectation; the camera's
//                         image area lies inside it (at most 1 px smaller per side, at most 3 %
//                         larger); the outer 4-pixel bands are not dark or garbage
//   orientation           decoded orientation equals the expectation and was baked into the
//                         pixels
//   required_corrections  every mandatory DNG opcode the contract lists was applied, at its
//                         stage, and they are exactly the expectation's mandatory opcode steps
//   numerical_validity    W2 scene-linear and display images and the develop display are finite;
//                         display max(R, G, B) <= 1
//   export_round_trip     the develop render exported as JPEG (sRGB, q95) and 16-bit TIFF
//                         (Display P3, uncompressed) with capture metadata; the files decode with
//                         independent libjpeg / libtiff readers to the quantised samples (TIFF
//                         exactly, JPEG within quantisation: mean |difference| < 3 codes, max 80),
//                         carry the profile's exact ICC bytes and read back the written metadata
//   determinism           a second develop render is bit-identical and its JPEG byte-identical
//
// Limitations (known optional capabilities that are missing) are collected alongside, and
// propose_state() turns checks, limitations and the expectation's evidence into a proposed state.
//
// Memory: the stages run one after another and release their images; the peak is one
// full-resolution develop render plus export buffers (about 1.2 GB for 45 MP). Not thread-safe
// with respect to the work directory; run one fixture at a time.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <lucent/core/error.hpp>

#include "expectations.hpp"
#include "mode_key.hpp"

namespace lucent::qualify {

using OrderedJson = nlohmann::ordered_json;

// Version of the check suite and of propose_state(); recorded in every report.
//   2: the develop render applies the camera's image area (geometry version 2, ADR 0017): the
//      dimensions check measures that output, a DNG DefaultCropSize counts as applied when the
//      output has its size, the orientation check compares the stage-one image with the visible
//      area, and the colour check renders without the area (like the rawpy reference).
inline constexpr std::uint32_t kCheckSuiteVersion = 2;
inline constexpr std::uint32_t kStateRulesVersion = 1;

// Permissible geometry difference (ADR 0016): Lucent's upright output may be at most this many
// pixels smaller than the camera's image area per dimension, and at most kMaxAreaExcess larger.
inline constexpr std::int32_t kAreaShortfallTolerancePx = 1;
inline constexpr double kMaxAreaExcess = 0.03;

enum class CheckStatus : std::uint8_t { Pass = 1, Fail = 2, NotApplicable = 3, NotRun = 4 };
enum class Evidence : std::uint8_t { Strong = 1, Weak = 2, None = 3 };
enum class SupportState : std::uint8_t {
    Verified = 1,
    CompatibleUnverified = 2,
    Limited = 3,
    Unsupported = 4,
    NotQualified = 5,  // the harness could not qualify the fixture (missing or altered file)
};

[[nodiscard]] std::string_view to_string(CheckStatus status) noexcept;  // "pass", ...
[[nodiscard]] std::string_view to_string(Evidence evidence) noexcept;   // "strong", ...
[[nodiscard]] std::string_view to_string(SupportState state) noexcept;  // "verified", ...
// User-facing name ("Verified", "Compatible, unverified", "Limited", "Unsupported").
[[nodiscard]] std::string_view display_name(SupportState state) noexcept;

struct CheckResult {
    std::string id;
    CheckStatus status = CheckStatus::NotRun;
    Evidence evidence = Evidence::None;
    std::string summary;  // one line, developer-facing English
    OrderedJson details = OrderedJson::object();
    double seconds = 0.0;
};

struct Limitation {
    std::string id;  // limitation_ids()
    std::string detail;
};

struct Measurements {
    double total_seconds = 0.0;
    std::map<std::string, double> phase_seconds;  // probe, decode, w2_render, develop_1, ...
    std::uint64_t peak_working_set_bytes = 0;     // of the process when the fixture finished
    std::uint64_t peak_commit_bytes = 0;
    std::uint64_t w2_estimate_bytes = 0;  // engine estimate of its own pixel buffers
    std::uint64_t develop_estimate_bytes = 0;
};

struct FixtureResult {
    std::string id;
    std::string tier;
    std::string file_name;
    std::uint64_t file_size = 0;
    std::string file_sha256;
    // Camera as decoded.
    std::string camera_make;
    std::string camera_model;
    std::string firmware;
    std::string lens;
    std::optional<ModeKey> mode_key;
    // The mode key of the file's probe (stage AfterOpen): the key the application derives when it
    // imports the file and looks it up in the camera support database (ADR 0018). It equals the text
    // of `mode_key` unless the probe sees a provisional value (the Canon S-RAW white level before
    // unpacking); std::nullopt when the file could not be probed.
    std::optional<std::string> lookup_key;
    // The probe's facts beside the key (assets::derive_mode_variant(): the image area's source and
    // size, the DNG opcodes), which a Verified database entry also requires of a file (QUA-08):
    // {"image_area": "...", "dng_opcodes": [...]}; null when the file could not be probed.
    OrderedJson lookup_variant = nullptr;
    // From the expectation (empty without one): product name, qualification family, mode label.
    std::string product;
    std::string family;
    std::string mode_label;
    bool decoded = false;
    std::string decode_error;  // "<code> at <where>: <message>" when the decoder refused
    std::string interpretation_status;
    std::vector<std::string> interpretation_issues;
    std::string decoder;  // "LibRaw 0.22.2-Release; decoded-RAW 1.3"
    std::vector<CheckResult> checks;
    std::vector<Limitation> limitations;
    // The camera's image area (ADR 0017): {"source", "provenance", "declared" ({x, y, width, height}
    // in raw-frame pixels, or null), "user_crop", "basis", "applied" (the develop render's geometry
    // frame in its upright image, or null when the whole image is the frame), "output" ([w, h])}.
    OrderedJson camera_area = nullptr;
    // Filled by propose_state().
    SupportState state = SupportState::NotQualified;
    std::vector<std::string> state_reasons;
    std::vector<std::string> evidence_gaps;
    std::optional<std::string> expected_state;
    // Differences from the expectation's expected outcome (state, check statuses, limitations,
    // mode key); empty when the result is exactly as expected or there is no expectation.
    std::vector<std::string> expectation_mismatches;
    Measurements measurements;
    std::string harness_error;  // set when the fixture could not be qualified at all

    [[nodiscard]] const CheckResult* check(std::string_view check_id) const noexcept;
    [[nodiscard]] OrderedJson to_json() const;
};

struct ManifestFixture {
    std::string id;
    std::string tier;
    std::string make;
    std::string model;
    std::string format;
    std::string mode;
    std::string file_name;
    std::uint64_t size_bytes = 0;
    std::string sha256;
};

// fixtures/manifest.json (lucent.fixture-manifest v1). VersionMismatch for a newer schema;
// InvalidArgument when malformed. Where "qualify.manifest".
[[nodiscard]] Expected<std::vector<ManifestFixture>> load_manifest(
    const std::filesystem::path& manifest_path);

// Paths of the repository data the suite reads.
struct CorpusPaths {
    std::filesystem::path root;                                 // repository root
    [[nodiscard]] std::filesystem::path manifest() const;       // fixtures/manifest.json
    [[nodiscard]] std::filesystem::path cache_dir() const;      // fixtures/cache
    [[nodiscard]] std::filesystem::path reference_dir() const;  // fixtures/reference
    [[nodiscard]] std::filesystem::path expectations() const;   // fixtures/qualification/expectations.json
    [[nodiscard]] std::filesystem::path families() const;       // fixtures/qualification/families.json
    [[nodiscard]] std::filesystem::path render_reference()
        const;  // tests/integration/data/render_reference.json
    [[nodiscard]] std::filesystem::path cached_file(const ManifestFixture& fixture) const;
};

struct QualifyOptions {
    // Exports are written below this directory (created; a fixture subdirectory is removed
    // afterwards unless keep_exports).
    std::filesystem::path work_dir;
    bool keep_exports = false;
    // When set, a reduced sRGB JPEG of the develop render (long edge preview_long_edge) is
    // written to <previews_dir>/<id>.jpg for the reviewer's visual check.
    std::optional<std::filesystem::path> previews_dir;
    std::int32_t preview_long_edge = 1024;
};

// Shared inputs of a run (loaded once).
struct QualifyCorpus {
    CorpusPaths paths;
    std::vector<ManifestFixture> manifest;
    std::optional<Expectations> expectations;
    nlohmann::json render_reference;  // lucent.render-reference v2

    // Loads manifest, expectations (optional: a run without them proposes states only from
    // checks and reports every fact as unverified) and the render reference.
    [[nodiscard]] static Expected<QualifyCorpus> load(const CorpusPaths& paths, bool require_expectations);
    [[nodiscard]] const ManifestFixture* find(std::string_view id) const noexcept;
};

// Runs every check on one fixture and proposes its state. Never fails: problems are recorded in
// the result (harness_error, failed checks).
[[nodiscard]] FixtureResult qualify_fixture(const QualifyCorpus& corpus, const ManifestFixture& fixture,
                                            const QualifyOptions& options);

// The state rules (version kStateRulesVersion), applied to `result` (its checks and limitations)
// and the fixture's expectation (may be null). Sets state, state_reasons, evidence_gaps and
// expected_state:
//   not_qualified          a harness error or a failed fixture_integrity check;
//   unsupported            the decoder refused the file, or any check failed (a failed
//                          interpretation check means a mandatory step is unavailable);
//   limited                every check passed (or does not apply) but a known optional capability
//                          is missing (a limitation);
//   verified               additionally: every check that applies passed with strong evidence, and
//                          the expectation exists, its mode key equals the derived one, and its
//                          identity, compression, camera image area, orientation and
//                          representation facts are all verified;
//   compatible_unverified  otherwise (decoding and interpretation succeed, the evidence is
//                          insufficient; the gaps are listed).
void propose_state(FixtureResult& result, const FixtureExpectation* expectation);

// Process memory counters (GetProcessMemoryInfo): peak working set and peak commit.
struct ProcessMemory {
    std::uint64_t peak_working_set_bytes = 0;
    std::uint64_t peak_commit_bytes = 0;
};
[[nodiscard]] ProcessMemory process_memory() noexcept;

}  // namespace lucent::qualify
