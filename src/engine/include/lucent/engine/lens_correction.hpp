#pragma once

// Lens-profile corrections: distortion, lateral chromatic aberration and vignetting from the Lensfun
// database (blueprint section 5, step 4: "Correct optics in the appropriate sensor/color domain. For
// the initial conventional Bayer route, use linear camera RGB for profile-based vignetting and
// channel-specific lateral chromatic aberration correction, combine geometric resampling where
// practical, and then transform camera color. ... never apply the same DNG/vendor correction again
// through Lensfun"; section 3: "An optional lens-profile miss can leave correction off"; ADR 0023).
//
// Lensfun (LGPL-3.0, linked dynamically) is a private dependency of this module: none of its types
// appears in this header. It is used for what it is the reference of: reading the database (data
// format version 1, from bytes the caller verified, lucent_assets), finding cameras and lenses
// (lfDatabase::FindCamerasExt, FindLenses), interpolating a lens's calibrations for the photograph
// (lfLens::Interpolate*) and the automatic scale that keeps a corrected frame filled
// (lfModifier::GetAutoScale). The correction itself evaluates Lensfun's models (below) in double
// precision per pixel and resamples with Lucent's own kernel; the unit tests check every position
// and gain against Lensfun's lfModifier (lens_correction_test.cpp).
//
// 1. Matching (LensDatabase::match, kLensProfileMatchVersion 1). From the decoded metadata
//    (lens_query()): camera make and model (as LibRaw reports them and normalised), lens make and
//    model, the lens's focal range, the focal length, the f-number and the 35 mm equivalent focal
//    length. The decoded-RAW contract 1.4 carries no lens identifier number and no focus distance.
//    * Camera: Lensfun's FindCamerasExt (every word of the file's name must occur in the database's)
//      for the make and model as reported and as normalised; a camera is identified when one of its
//      maker spellings and one of its model spellings equal the file's (case and runs of spaces
//      ignored, as Lensfun compares names) and all such entries agree on mount and crop factor.
//    * Lens: Lensfun's FindLenses for the identified camera (its mount and the mounts compatible
//      with it, its crop factor), strict word matching (every word of the file's lens name must
//      occur in the candidate's), with the file's lens maker when it names one. A candidate is the
//      same lens when its name (any spelling) and the file's name hold the same words once the
//      words of the lens maker are removed from both (Lensfun's word rules: digit runs with '.',
//      punctuation runs and letter runs, case folded; a lone punctuation mark or a lone "f" is not a
//      word), and its focal range agrees with the file's within 1 % when the file records one.
//      A fixed-lens camera (its mount is not one of the database's interchangeable mounts and
//      exactly one lens has it) is matched by that lens when the file names none.
//    * Kinds: Exact when the camera is identified and exactly one lens (by name) is the same lens;
//      Fuzzy when Lensfun returns candidates (strict or, failing those, loose search) but none is
//      exact or several are; None otherwise (no database entry, no lens named). The reasons say
//      which step decided. Automatic corrections use Exact matches only: "never a guessed profile".
// 2. Planning (plan_lens_correction, LensCorrectionPlan). Per correction, in this order of
//    precedence: Disabled (the setting), CarriedByFile (the file's applied DNG opcodes already
//    corrected it, carried_lens_corrections(); never applied twice), Withheld (automatic mode and
//    the file carries some correction of its own: no third-party profile is mixed in
//    automatically), NoProfile, NotApplicable (lateral CA of monochrome data), NoCalibration (the
//    profile has none for it, or none that covers the photograph: the focal length within the
//    calibrated focal lengths (1 % tolerance; any focal length for a prime), for vignetting also the
//    f-number within the calibrated apertures (2 %), the image sensor no larger than the
//    calibration's, a model Lucent implements), else Applied with the calibration Lensfun
//    interpolates for the focal length (and f-number and focus distance: unknown in contract 1.4, so
//    the calibration for 1000 m, Lensfun's "infinity" entries, is interpolated).
//    The image's crop factor is the database camera's, unless the file's 35 mm equivalent focal
//    length says otherwise by more than 6 % (a crop mode of the sensor, e.g. APS-C on a full-frame
//    body): an automatic match is then not exact, and a named profile uses the file's value.
// 3. Model (lens-correction operation version 1), in Lensfun's coordinates (lfModifier): the
//    stored-orientation full-resolution image of stage one (W x H, after the DNG opcodes and before
//    the camera orientation, so a lens centre offset keeps its sensor axes); pixel coordinates with
//    pixel centres at integers, p = Lucent's continuous point - 0.5; normalised n = p * s - c with
//        s = 2 / (min(W, H) - 1) * k,  c = ((W - 1, H - 1) / (min(W, H) - 1) + lens centre) * k,
//        k = cf_calibration / cf_image * sqrt(a_cal^2 + 1) / sqrt(a_image^2 + 1)
//    (a: long side / short side), so n's unit is half the short side of the calibration's sensor.
//    For an output (corrected) pixel n, the source position of camera channel c is
//        d = D(n / scale), then TCA_c(d) (green: d), back to pixels by (x + c) / s + 0.5,
//    with D: poly3 r_d = r_u (1 - k1 + k1 r_u^2), poly5 r_d = r_u (1 + k1 r_u^2 + k2 r_u^4),
//    ptlens r_d = r_u (a r_u^3 + b r_u^2 + c r_u + 1 - a - b - c); TCA_c: linear r_c = k_c r, poly3
//    r_c = r (b_c r^2 + c_c r + v_c); scale: Lensfun's GetAutoScale (the corrected frame is filled,
//    1 per mille margin; 1.001 for TCA alone), raised where needed so that the pixel centres of the
//    whole boundary read inside the image in every channel (Lensfun checks eight points of a frame
//    it takes to be symmetric about the lens centre), 1 without distortion or TCA. Vignetting ("pa"): the
//    value is divided by 1 + k1 r^2 + k2 r^4 + k3 r^6 at the source position, r = |d_c| / sqrt(a_cal^2
//    + 1) (1 at the corner of the calibration's sensor), as Lensfun's ModifyColor_DeVignetting_PA.
// 4. Application (develop.hpp, stage two): in linear white-balanced camera RGB (white balance is a
//    per-channel gain and commutes with both), fused into stage two's first pass before camera colour
//    (the blueprint's "combine geometric resampling where practical"): each output pixel's three
//    source positions are evaluated, each channel is sampled there with the Keys cubic convolution
//    kernel a = -0.5 (Catmull-Rom, 4 x 4 taps, sums in double, taps beyond the image clamped to its
//    edge, as the DNG warps of ADR 0014) and divided by the vignetting at its own source position.
//    Positions are full-resolution coordinates scaled per axis, so a proxy is corrected with the
//    same model as the full image. Vignetting alone does not resample.
// 5. Frames. U: stage one's upright image (uncorrected); U': the corrected canvas, U's size. Masks
//    stay defined in U and are evaluated at each pixel's source position in U (the green channel's,
//    LensGeometryMap::source_of), so enabling, disabling or changing a correction never moves a mask
//    relative to the content (as the DNG opcodes' GeometryChange maps points, ADR 0014). The
//    geometry's frame F (the camera's image area for geometry version 2, ADR 0017) is the same pixel
//    rectangle of U', applied after the correction, as the DNG specification applies DefaultCrop to
//    the stage-3 image after the opcode lists' warps; the model's frame is the whole image, whose
//    centre and size Lensfun's normalisation assumes, as for a WarpRectilinear's optical centre.
//
// Thread-safety: a LensDatabase is immutable after load() and may be shared; its lookups are
// serialised internally (Lensfun's search writes scores into the database and switches LC_NUMERIC
// to "C" while it parses names, which Lucent, running in the C locale, does not notice). Plans,
// maps and records are plain values.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/engine/dng_opcode_processing.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/image/pixel_buffer.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::engine {

// Version of the matching rules (section 1 above). It is one of kDevelopEngineVersions: the same
// recipe and database can match another profile under other rules.
inline constexpr std::uint32_t kLensProfileMatchVersion = 1;

// The Lensfun database format Lucent reads (Lensfun 0.3: LF_MAX_DATABASE_VERSION 1).
inline constexpr std::uint32_t kLensfunDataFormatVersion = 1;

// Fuzzy candidates a match lists, best first.
inline constexpr std::size_t kMaxLensCandidates = 5;

// Tolerances of the rules above.
inline constexpr double kLensFocalTolerance = 0.01;     // focal length against the calibrated ones
inline constexpr double kLensApertureTolerance = 0.02;  // f-number against the calibrated ones
inline constexpr double kLensCropModeTolerance = 0.06;  // 35 mm equivalent against the camera's crop factor
// The focus distance used when the file records none: Lensfun's "infinity" calibrations (metres).
inline constexpr double kLensDefaultFocusDistanceM = 1000.0;

// ---- Database -----------------------------------------------------------------------------------

// One file of the database: its name (for messages; files load in the given order, a later entry
// replacing an equal earlier one, as Lensfun loads) and its XML bytes.
struct LensDatabaseFile {
    std::string name;
    std::string_view data;
};

// Where a database came from (the verified asset, lucent_assets lens_database.hpp).
struct LensDatabaseIdentity {
    std::string asset_id;         // "lens-database"
    std::string asset_version;    // SemVer of the asset
    std::string manifest_sha256;  // lower-case hex

    friend bool operator==(const LensDatabaseIdentity&, const LensDatabaseIdentity&) = default;
};

struct LensDatabaseInfo {
    LensDatabaseIdentity identity;
    std::string library;  // "Lensfun 0.3.4"
    std::uint32_t data_format = kLensfunDataFormatVersion;
    std::size_t files = 0;
    std::size_t mounts = 0;
    std::size_t cameras = 0;
    std::size_t lenses = 0;
};

// What the file says about its camera and lens (section 1).
struct LensQuery {
    std::string camera_make;  // raw::CameraIdentity::make
    std::string camera_model;
    std::string camera_normalized_make;
    std::string camera_normalized_model;
    std::string lens_make;  // raw::LensIdentity
    std::string lens_model;
    std::optional<double> lens_min_focal_mm;
    std::optional<double> lens_max_focal_mm;
    std::optional<double> focal_length_mm;  // raw::CaptureSettings
    std::optional<double> f_number;
    std::optional<double> focal_length_35mm_mm;  // raw::LensIdentity::focal_length_35mm_equivalent_mm
    std::optional<double> focus_distance_m;      // not in the decoded-RAW contract 1.4: always empty

    friend bool operator==(const LensQuery&, const LensQuery&) = default;
};

[[nodiscard]] LensQuery lens_query(const raw::RawMetadata& metadata);

// The crop factor the file's focal lengths imply: 35 mm equivalent / actual, when both are known
// and positive.
[[nodiscard]] std::optional<double> crop_factor_from_focal_lengths(const LensQuery& query) noexcept;

// Persisted match kinds (append only).
enum class LensMatchKind : std::uint8_t {
    None = 1,
    Fuzzy = 2,
    Exact = 3,
};

[[nodiscard]] std::string_view to_string(LensMatchKind kind) noexcept;  // "none", "fuzzy", "exact"

// A camera of the database.
struct LensCameraProfile {
    std::string maker;  // the database's default spellings
    std::string model;
    std::string mount;
    double crop_factor = 0.0;

    friend bool operator==(const LensCameraProfile&, const LensCameraProfile&) = default;
};

// A lens profile of the database (one Lensfun lens entry: a lens's calibrations for one sensor
// size), as an application lists and searches them so that the photographer can name a profile
// (LensCorrectionSetting::profile).
struct LensProfileSummary {
    LensProfileId id;       // the database's default spellings and the calibration's crop factor
    std::string mounts;     // the lens's mounts, ", " separated
    std::string lens_type;  // its projection ("rectilinear", "fisheye", ...)
    double min_focal_mm = 0.0;  // the focal range (0: unknown)
    double max_focal_mm = 0.0;
    double min_f_number = 0.0;  // the largest aperture (smallest f-number; 0: unknown)
    // The profile holds at least one calibration of each kind.
    bool distortion = false;
    bool lateral_chromatic_aberration = false;
    bool vignetting = false;

    friend bool operator==(const LensProfileSummary&, const LensProfileSummary&) = default;
};

// A lens the search returned.
struct LensCandidate {
    LensProfileId lens;
    std::string mounts;      // the lens's mounts, ", " separated
    int score = 0;           // Lensfun's score (higher is better)
    bool same_name = false;  // the name rule of section 1 holds

    friend bool operator==(const LensCandidate&, const LensCandidate&) = default;
};

struct LensMatch {
    std::uint32_t version = kLensProfileMatchVersion;
    LensMatchKind kind = LensMatchKind::None;
    std::optional<LensCameraProfile> camera;  // the identified camera
    std::optional<LensProfileId> lens;        // the exact match
    std::vector<LensCandidate> candidates;    // best first, at most kMaxLensCandidates
    std::vector<std::string> reasons;         // developer-facing English, in the order decided

    friend bool operator==(const LensMatch&, const LensMatch&) = default;
};

// Persisted model identifiers (append only).
enum class LensDistortionModel : std::uint8_t { Poly3 = 1, Poly5 = 2, PTLens = 3 };
enum class LensTcaModel : std::uint8_t { Linear = 1, Poly3 = 2 };
enum class LensVignettingModel : std::uint8_t { PabloDAngelo = 1 };  // Lensfun "pa"

[[nodiscard]] std::string_view to_string(LensDistortionModel model) noexcept;  // "poly3", "poly5", "ptlens"
[[nodiscard]] std::string_view to_string(LensTcaModel model) noexcept;         // "linear", "poly3"
[[nodiscard]] std::string_view to_string(LensVignettingModel model) noexcept;  // "pa"

// A calibration as interpolated for the photograph, with the database entries it came from.
struct LensDistortionCalibration {
    LensDistortionModel model = LensDistortionModel::Poly3;
    std::array<double, 3> terms{};  // k1 | k1, k2 | a, b, c
    double focal_mm = 0.0;          // the focal length it was interpolated for
    std::vector<double> calibrated_focal_mm;

    friend bool operator==(const LensDistortionCalibration&, const LensDistortionCalibration&) = default;
};

struct LensTcaCalibration {
    LensTcaModel model = LensTcaModel::Linear;
    // Lensfun's order: linear kr, kb; poly3 vr, vb, cr, cb, br, bb.
    std::array<double, 6> terms{};
    double focal_mm = 0.0;
    std::vector<double> calibrated_focal_mm;

    friend bool operator==(const LensTcaCalibration&, const LensTcaCalibration&) = default;
};

struct LensVignettingCalibration {
    LensVignettingModel model = LensVignettingModel::PabloDAngelo;
    std::array<double, 3> terms{};  // k1, k2, k3
    double focal_mm = 0.0;
    double f_number = 0.0;
    double distance_m = 0.0;
    std::vector<double> calibrated_focal_mm;
    std::vector<double> calibrated_f_numbers;

    friend bool operator==(const LensVignettingCalibration&, const LensVignettingCalibration&) = default;
};

// ---- The model ----------------------------------------------------------------------------------

// Lensfun's normalisation of one image (section 3; lfModifier's constructor in double precision).
struct LensfunFrame {
    image::Size size;          // stored-orientation full-resolution image W x H
    double crop_factor = 1.0;  // the image's
    double calibration_crop_factor = 1.0;
    double calibration_aspect_ratio = 1.5;
    double lens_center_x = 0.0;  // the lens's centre offset (normalised units)
    double lens_center_y = 0.0;
    double norm_scale = 0.0;  // s
    double center_x = 0.0;    // c
    double center_y = 0.0;
    double aspect_ratio_correction = 1.0;  // sqrt(a_cal^2 + 1)

    // From the image size and the calibration (InvalidArgument for an image smaller than 2 x 2 or
    // values that are not finite and positive).
    [[nodiscard]] static Expected<LensfunFrame> create(image::Size stored_size, double image_crop,
                                                       double calibration_crop, double calibration_aspect,
                                                       double centre_x, double centre_y);

    // Lucent continuous stored coordinates <-> normalised coordinates.
    [[nodiscard]] image::PointD normalized(image::PointD stored_point) const noexcept;
    [[nodiscard]] image::PointD stored(image::PointD normalized) const noexcept;

    friend bool operator==(const LensfunFrame&, const LensfunFrame&) = default;
};

// The geometric part of a correction (distortion and/or lateral CA, the scale) for one image, as a
// point mapping in full-resolution upright continuous coordinates (image/geometry.hpp).
struct LensGeometryMap {
    LensfunFrame frame;
    image::Orientation orientation = image::Orientation::Normal;  // stage one's camera orientation
    image::Size upright_size;                                     // full resolution
    double scale = 1.0;
    // Lensfun's GetAutoScale for the same callbacks; `scale` is larger only where Lensfun's scale
    // leaves a boundary pixel reading beyond the image (a lens centre offset, strong lateral CA).
    double lensfun_scale = 1.0;
    std::optional<LensDistortionCalibration> distortion;
    std::optional<LensTcaCalibration> tca;

    // The source point in the uncorrected image U of camera channel `channel` (0 R, 1 G, 2 B; G for
    // monochrome) for the corrected point `corrected` (section 3).
    [[nodiscard]] image::PointD source_of(image::PointD corrected, std::uint32_t channel = 1) const noexcept;
    // The corrected point whose green source is `source` (the inverse of source_of for channel 1:
    // a radial Newton iteration, to 1e-9 normalised units); std::nullopt where it does not converge
    // (beyond the model's monotone range).
    [[nodiscard]] std::optional<image::PointD> corrected_of(image::PointD source) const noexcept;
    // The normalised source position of `channel` for a normalised corrected position (the model in
    // Lensfun's units; source_of() wraps it).
    [[nodiscard]] image::PointD normalized_source(image::PointD normalized_corrected,
                                                  std::uint32_t channel) const noexcept;
    // Identifies the map (FNV-1a over every value).
    [[nodiscard]] std::uint64_t revision() const noexcept;

    friend bool operator==(const LensGeometryMap&, const LensGeometryMap&) = default;
};

// The vignetting part of a correction.
struct LensVignettingCorrection {
    LensfunFrame frame;
    image::Orientation orientation = image::Orientation::Normal;
    image::Size upright_size;
    LensVignettingCalibration calibration;

    // The gain at a normalised source position: 1 / (1 + k1 r^2 + k2 r^4 + k3 r^6).
    [[nodiscard]] double gain_at_normalized(image::PointD normalized_source) const noexcept;
    // The gain at a full-resolution upright continuous point of U.
    [[nodiscard]] double gain_at(image::PointD upright_source) const noexcept;

    friend bool operator==(const LensVignettingCorrection&, const LensVignettingCorrection&) = default;
};

// ---- Plans --------------------------------------------------------------------------------------

// Persisted identifiers (append only).
enum class LensCorrectionKind : std::uint8_t {
    Distortion = 1,
    LateralChromaticAberration = 2,
    Vignetting = 3,
};

enum class LensCorrectionOutcome : std::uint8_t {
    Applied = 1,
    Disabled = 2,       // the setting leaves it off (or lens-correction operation version 0)
    CarriedByFile = 3,  // the file's applied DNG opcodes already corrected it: never applied twice
    Withheld = 4,       // automatic: the file carries a lens correction of its own
    NoProfile = 5,      // no database, no exact match (automatic) or the named profile is not in it
    NotApplicable = 6,  // lateral CA of monochrome data, or an image that is not the whole frame
    NoCalibration = 7,  // the profile does not calibrate it for this photograph (see section 2)
};

[[nodiscard]] std::string_view to_string(LensCorrectionKind kind) noexcept;
// "distortion", "lateral_chromatic_aberration", "vignetting"
[[nodiscard]] std::string_view to_string(LensCorrectionOutcome outcome) noexcept;
// "applied", "disabled", "carried_by_file", "withheld", "no_profile", "not_applicable", "no_calibration"

struct LensCorrectionDecision {
    LensCorrectionKind kind = LensCorrectionKind::Distortion;
    LensCorrectionOutcome outcome = LensCorrectionOutcome::Disabled;
    std::string detail;  // developer-facing English

    friend bool operator==(const LensCorrectionDecision&, const LensCorrectionDecision&) = default;
};

// What a develop does for the lens (section 2), a plain value: stage two applies it, the report
// records it and GeometryMapping maps points through its geometry.
struct LensCorrectionPlan {
    std::uint32_t operation_version = kOldestLensCorrectionOperationVersion;  // 0: nothing
    std::uint32_t match_version = kLensProfileMatchVersion;
    LensCorrectionSetting setting;  // the effective setting (the default for version 0)
    bool automatic = true;
    std::optional<LensDatabaseIdentity> database;
    LensQuery query;
    LensMatch match;                       // the automatic match (also made for a named profile)
    std::optional<LensProfileId> profile;  // the profile used
    std::optional<LensCameraProfile> camera;
    std::string lens_type;     // the profile's projection ("rectilinear", "fisheye", ...)
    double crop_factor = 0.0;  // the image's crop factor used (0: unknown)
    std::string crop_factor_basis;
    CarriedLensCorrections carried;
    // Distortion, lateral chromatic aberration, vignetting.
    std::array<LensCorrectionDecision, 3> decisions{
        LensCorrectionDecision{LensCorrectionKind::Distortion, LensCorrectionOutcome::Disabled, {}},
        LensCorrectionDecision{
            LensCorrectionKind::LateralChromaticAberration, LensCorrectionOutcome::Disabled, {}},
        LensCorrectionDecision{LensCorrectionKind::Vignetting, LensCorrectionOutcome::Disabled, {}}};
    std::optional<LensGeometryMap> geometry;             // distortion and/or lateral CA applied
    std::optional<LensVignettingCorrection> vignetting;  // vignetting applied

    [[nodiscard]] const LensCorrectionDecision& decision(LensCorrectionKind kind) const noexcept;
    [[nodiscard]] bool applied(LensCorrectionKind kind) const noexcept;
    [[nodiscard]] bool any_applied() const noexcept { return geometry.has_value() || vignetting.has_value(); }
    // Identifies the pixels the plan produces (FNV-1a over the operation version, the maps and the
    // gains): equal revisions render equally. 0 when nothing is applied.
    [[nodiscard]] std::uint64_t revision() const noexcept;

    friend bool operator==(const LensCorrectionPlan&, const LensCorrectionPlan&) = default;
};

// The database. Immutable after load(); share it with std::shared_ptr<const LensDatabase>.
class LensDatabase {
public:
    // Parses `files` (in order) with Lensfun. Errors (engine.lens.database): InvalidArgument for no
    // file; Corrupt when Lensfun refuses a file (not XML it understands, a newer data format) or the
    // result holds no lens; OutOfMemory.
    [[nodiscard]] static Expected<std::shared_ptr<const LensDatabase>> load(
        std::span<const LensDatabaseFile> files, LensDatabaseIdentity identity);

    ~LensDatabase();
    LensDatabase(const LensDatabase&) = delete;
    LensDatabase& operator=(const LensDatabase&) = delete;

    [[nodiscard]] const LensDatabaseInfo& info() const noexcept;

    // Every lens profile of the database, sorted by maker, model and crop factor (listed once by
    // load(); every entry is one contains() finds).
    [[nodiscard]] const std::vector<LensProfileSummary>& profiles() const noexcept;

    // The match for `query` (section 1). Results are cached (the last 64 queries).
    [[nodiscard]] LensMatch match(const LensQuery& query) const;

    // True when the database holds the profile `id` (maker and model compared as Lensfun compares
    // names, crop factors within 0.01).
    [[nodiscard]] bool contains(const LensProfileId& id) const;

    // The plan for a photograph (section 2): `setting` std::nullopt for lens-correction operation
    // version 0; `upright_size` the full-resolution upright size of stage one's image and
    // `orientation` its camera orientation; `monochrome` for one-channel data. Plans are cached (the
    // last 64). Errors: Internal when Lensfun returns values that are not finite.
    [[nodiscard]] Expected<LensCorrectionPlan> plan(const raw::RawMetadata& metadata,
                                                    const std::optional<LensCorrectionSetting>& setting,
                                                    image::Size upright_size, image::Orientation orientation,
                                                    bool monochrome) const;

private:
    LensDatabase();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// LensDatabase::plan(), or without a database (`database` null) the plan that applies nothing and
// says why (NoProfile: "no lens database").
[[nodiscard]] Expected<LensCorrectionPlan> plan_lens_correction(
    const LensDatabase* database, const raw::RawMetadata& metadata,
    const std::optional<LensCorrectionSetting>& setting, image::Size upright_size,
    image::Orientation orientation, bool monochrome);

// What the correction did in one render (DevelopReport::lens_correction).
struct LensCorrectionRecord {
    LensCorrectionPlan plan;
    image::Size render_size;            // the stage-one image it was applied to
    bool resampled = false;             // distortion or lateral CA applied
    std::uint64_t outside_samples = 0;  // source positions outside the image (taps clamped to its edge)
    double seconds = 0.0;
};

// Applies `plan` to `pixels` (white-balanced or unbalanced camera RGB, 3 channels, or 1 channel:
// the whole upright image of the stage one the plan was made for, at full resolution or any proxy
// scale of `full_size`) as section 4 defines: a new buffer of the same size under `limits` (the input
// is released on success); nothing changes when the plan applies nothing. Stage two fuses exactly
// this computation into its first pass (develop.hpp); tests and tools use it on its own.
// Errors (engine.lens.apply): InvalidArgument for an empty image, 2 or more than 3 channels, an
// empty full size; LimitExceeded / OutOfMemory; Cancelled (the image is unchanged).
[[nodiscard]] Expected<LensCorrectionRecord> apply_lens_correction(const LensCorrectionPlan& plan,
                                                                   image::ImageBuffer<float>& pixels,
                                                                   image::Size full_size,
                                                                   const image::AllocationLimits& limits = {},
                                                                   const core::CancellationToken& cancel = {},
                                                                   bool parallel = true);

// Words of a name under Lensfun's rules (section 1), sorted: for tests and diagnostics.
[[nodiscard]] std::vector<std::string> lens_name_words(std::string_view name);

}  // namespace lucent::engine
