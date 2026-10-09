#pragma once

// DNG opcode processing (DNG 1.7.1.0 chapter 7 "Opcode List Processing"; blueprint section 3,
// "mandatory DNG opcode cannot silently become a verified image", and section 5 step 4,
// "Required DNG operations follow their specified stage/order"; ADR 0014).
//
// The raw module decodes the opcode parameters and decides which opcodes Lucent applies
// (raw::assess_dng_opcode, RequiredOperation::supported_by_lucent). This module applies exactly
// those, in list order, at the stages the DNG processing model defines:
//
//   List | DNG stage                                  | Lucent stage (render and develop paths)
//   -----+--------------------------------------------+------------------------------------------
//    1   | stored raw values, before linearisation    | decoded raw frame, before normalisation
//    2   | linear reference values (black/white       | normalised visible area (SensorImage),
//        | mapped), before demosaicing                | before white balance
//    3   | after demosaicing                          | demosaiced camera RGB / monochrome, before
//        |                                            | camera colour (render) / orientation
//
// Coordinates. Each list processes a "stage image" (raw::dng_opcode_frame): the stored raw image
// for List 1, the ActiveArea for Lists 2 and 3. Lucent's buffer for the stage covers the raw
// frame (List 1) or the visible area (Lists 2 and 3), which LibRaw may start one row or column
// inside an odd ActiveArea margin; every position, area, pitch, map coordinate, optical centre and
// radius is evaluated in stage-image coordinates and mapped to the buffer, so the parts of the
// stage image that Lucent does not hold affect nothing but are still part of the geometry. A List
// 2 TrimBounds makes the trimmed rectangle the stage image of the rest of List 2 and of List 3.
//
// Values. List 1 works on the stored 16-bit integers with the specification's logical range: each
// result is rounded to nearest (halves away from zero) and limited to [0, 65535]. Its MapTable
// maps the stored value, its MapPolynomial is evaluated on the stored value (y = sum c_k v^k)
// and its deltas add stored units, which is what the DNG SDK computes: it converts the stage-1
// image to [0, 1] floats (v / 65535) but rescales the polynomial coefficients (c_k x 65535^(k-1))
// and the deltas (d / 65535) to match. Lists 2 and 3 work on Lucent's normalised floats (1.0 =
// white). DNG clips the image to [0, 1] after every opcode; Lucent does not clip linear
// operations (gains, scales, deltas, warps), keeping its unclipped convention (conventions.md;
// results inside [0, 1] are identical), but bounds their results to +-kDngOpcodeValueBound; value
// maps (MapTable, MapPolynomial) are defined on the logical range only: their input and output
// are limited to [0, 1]. Limited samples are counted (DngOpcodeRecord::limited_samples).
//
// Areas. An empty area rectangle stands for the whole stage image (the DNG SDK's
// dng_area_spec::Overlap) for MapTable, MapPolynomial and GainMap; the per-row and per-column
// opcodes then hold no values for it and are not supported (raw::assess_dng_opcode).
//
//   * White balance. Lucent balances the mosaic before demosaicing, so List 3 sees camera RGB
//     multiplied by the stage-one multipliers m_p. Multiplicative opcodes and warps commute with
//     that; Delta* add delta x m_p, and value maps run on value / m_p and are scaled back by m_p,
//     so every opcode acts on the unbalanced values the DNG model defines.
//   * Saturation. List 2 opcodes correct measured values; a sample that was saturated in the raw
//     data has no measured value beyond "at least white", so after List 2 it is kept at or above
//     its channel's saturation level (max(value, level)). Highlights clipped in the sensor
//     therefore stay clipped (neutral after the highlight clip) whatever gain an opcode applies.
//     A bad-pixel repair is not a correction of the measured value but its replacement: the
//     repaired pixel's flag is derived again from its new value (saturated when it reaches the
//     channel's saturation level), so the repair is kept. In List 1, saturation is judged
//     afterwards on the corrected stored values, as DNG applies WhiteLevel after List 1.
//
// Per opcode:
//   GainMap            bilinear in the map grid at the pixel centre, map coordinates
//                      v = ((row + 0.5) / H - MapOriginV) / MapSpacingV (likewise h with the
//                      width), indices clamped to the grid (edge values replicated); image plane
//                      p (the absolute plane index, Plane <= p < Plane + Planes) uses map plane
//                      min(p, MapPlanes - 1), as the DNG SDK indexes it.
//   FixVignetteRadial  g = 1 + k0 r^2 + ... + k4 r^10 on every plane; r and the optical centre as
//                      the specification defines them (below).
//   WarpRectilinear,   output pixel (x, y) of plane i takes input position (x', y') from the
//   WarpRectilinear2   specification's formulas with coefficient set min(i, N - 1); (x0, y0) =
//                      (0, 0) and (x1, y1) = (W - 1, H - 1) are the stage image's corner pixel
//                      indices, so the centre is c = c^ (W - 1, H - 1) and m is the distance from
//                      it to the farthest corner pixel. Resampled with the Keys cubic convolution
//                      kernel a = -0.5 (Catmull-Rom; interpolating, third-order accurate), 4 x 4
//                      taps, coordinates beyond the buffer clamped to its edge pixels. An input
//                      position outside the buffer's pixel area is counted (outside_samples). The
//                      image size is unchanged.
//   TrimBounds         the image becomes the rectangle (Lucent's buffer is cut to it).
//   MapTable           16-bit table; List 1 maps the stored value, Lists 2 and 3 map
//                      round(clamp(v, 0, 1) x 65535) and divide by 65535 (the SDK's 16-bit path);
//                      values past the table take its last entry.
//   MapPolynomial      y = sum c_k x^k with x = the stored value (List 1) or clamp(v, 0, 1).
//   DeltaPerRow/Column, ScalePerRow/Column: v + delta (List 1: stored units), v x scale on the
//                      affected rows/columns.
//   FixBadPixelsConstant/List (Bayer CFA): a bad pixel (value == Constant, or listed) becomes the
//                      mean of its nearest same-colour good neighbours: green sites search rings
//                      of increasing distance on the green quincunx (sqrt 2, 2, sqrt 8, ...), red
//                      and blue sites on their 2-pixel lattice (2, sqrt 8, 4, ...), up to 16
//                      pixels; the first ring with a good neighbour inside the stage image and
//                      Lucent's buffer gives the mean (integers rounded half up). A bad pixel
//                      without one within 16 pixels keeps its value (counted as unrepaired).
//
// Never twice: an opcode the decoder applied (RequiredOperation::applied_by_decoder) is recorded
// and not applied; an opcode superseded by an applied optional WarpRectilinear2 is skipped; and
// carried_lens_corrections() tells a future lens-profile (Lensfun) stage which corrections the
// file's opcodes already made, so that stage must leave them off (ADR 0014).
//
// Thread-safety: a DngOpcodeProcessor is used by one thread at a time; pixel loops use the
// parallel algorithms per row, and results do not depend on scheduling.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/sensor.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/image/pixel_buffer.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/dng_opcodes.hpp>

namespace lucent::engine {

// Semantic version of opcode processing (bump whenever the same input would give other values).
inline constexpr std::uint32_t kDngOpcodeProcessingVersion = 1;

// Lists 2 and 3 do not clip linear opcodes to [0, 1] (see "Values" above) but limit their float
// results to [-kDngOpcodeValueBound, kDngOpcodeValueBound] (2^16 x white): no physical correction
// comes near it, and a huge but finite coefficient (a vignette k0 of 1e300) can never make a later
// stage overflow, so the render and develop paths treat such a file alike. Limited results are
// counted in DngOpcodeRecord::limited_samples.
inline constexpr double kDngOpcodeValueBound = 65536.0;

// What happened to one opcode (persisted; append only).
enum class OpcodeOutcome : std::uint8_t {
    Applied = 1,
    // Optional and not supported by Lucent: skipped, as DNG permits.
    SkippedOptional = 2,
    // Mandatory and not supported: the result is not a faithful interpretation (the metadata's
    // interpretation already says so).
    NotApplied = 3,
    // Follows an optional WarpRectilinear2 that was applied (DNG 1.7.1.0 chapter 7 skip rule).
    SkippedByWarpRule = 4,
    // The decoder applied it while unpacking; never applied a second time.
    AppliedByDecoder = 5,
    // Opcode processing was switched off (RenderSettings::apply_dng_opcodes, reference
    // comparisons with decoders that do not apply opcodes).
    Disabled = 6,
};

[[nodiscard]] std::string_view to_string(OpcodeOutcome outcome) noexcept;  // "applied", ...

struct DngOpcodeRecord {
    raw::DngOpcodeListId list = raw::DngOpcodeListId::List1;
    std::uint32_t index = 0;  // position in its list
    std::uint32_t id = 0;     // opcode ID
    std::string name;         // "GainMap", ...
    bool mandatory = true;
    OpcodeOutcome outcome = OpcodeOutcome::NotApplied;
    // Developer-facing English: the parameters as applied (Applied), or why not.
    std::string detail;
    std::uint64_t affected_samples = 0;  // samples written
    std::uint64_t limited_samples = 0;   // results limited to the logical range (see the file comment)
    std::uint64_t outside_samples = 0;   // warps: input positions outside Lucent's buffer
    std::uint64_t unrepaired_pixels = 0; // bad-pixel opcodes: bad pixels without a good neighbour
    double seconds = 0.0;
};

enum class GeometryChangeKind : std::uint8_t {
    Warp = 1,  // WarpRectilinear / WarpRectilinear2: same size, pixels moved
    Trim = 2,  // TrimBounds: the image is cut to a rectangle
};

// A change of image geometry made by an opcode, recorded so that masks and diagnostics can be
// mapped between the coordinates before and after it (blueprint section 5, "Noise, masks, and
// geometry"). Changes are listed in execution order; all are in stored (unrotated) orientation,
// before the orientation stage.
struct GeometryChange {
    GeometryChangeKind kind = GeometryChangeKind::Warp;
    raw::DngOpcodeListId list = raw::DngOpcodeListId::List3;
    std::uint32_t index = 0;
    image::Size input_size;   // Lucent's buffer before the opcode
    image::Size output_size;  // and after it
    // Trim: output pixel (x, y) is input pixel (x + crop.x, y + crop.y).
    image::PixelRect crop;
    // Warp: the parameters and the frame they were evaluated in: the stage image size, Lucent's
    // buffer origin in it, the optical centre in stage pixel indices and m (see the file comment).
    raw::DngWarpRectilinear warp;
    image::Size stage_size;
    std::int32_t offset_x = 0;
    std::int32_t offset_y = 0;
    double center_x = 0.0;
    double center_y = 0.0;
    double max_distance = 0.0;

    // The point of the input image whose content the output point `output` shows, in plane
    // `plane`; continuous coordinates as image/geometry.hpp defines them (pixel centres at
    // i + 0.5). A Trim adds the crop origin; a Warp evaluates the opcode's inverse mapping.
    [[nodiscard]] image::PointD source_of(image::PointD output, std::uint32_t plane = 0) const noexcept;
};

// Everything opcode processing did to one image.
struct DngOpcodeReport {
    std::uint32_t version = kDngOpcodeProcessingVersion;
    std::vector<DngOpcodeRecord> opcodes;  // every opcode of the file, in execution order
    std::vector<GeometryChange> geometry;  // in execution order
    // Samples raised back to their saturation level after List 2 (see "Saturation" above).
    std::uint64_t list2_resaturated_samples = 0;

    [[nodiscard]] bool any_applied() const noexcept;
};

// Optical corrections that a file's applied opcodes already made. A lens-profile correction
// stage (Lensfun, not yet part of Lucent) must not apply a correction a second time: it must
// leave distortion off when `distortion`, lateral chromatic aberration off when
// `lateral_chromatic_aberration`, and vignetting off when `vignetting` (ADR 0014).
struct CarriedLensCorrections {
    bool distortion = false;                    // a WarpRectilinear/WarpRectilinear2 is applied
    bool lateral_chromatic_aberration = false;  // ... whose coefficient sets differ per plane
    bool vignetting = false;  // FixVignetteRadial, or a GainMap of List 2 or 3 over the whole image
    std::vector<std::string> sources;  // "OpcodeList3[1] WarpRectilinear", ...
};

// From the metadata's supported opcodes (those the engine applies). Opcodes the decoder applied
// count as well; opcodes Lucent skips do not (the file is then not verified anyway).
[[nodiscard]] CarriedLensCorrections carried_lens_corrections(const raw::RawMetadata& metadata);

struct DngOpcodeOptions {
    // false: record every opcode as Disabled and change no pixel.
    bool enabled = true;
    image::AllocationLimits limits;  // for warp and trim buffers
    bool parallel = true;
};

// Runs the opcode lists of one decoded image at their stages. Construct it from the decoded
// metadata, then call run_list1() before normalisation, run_list2() before white balance and
// run_list3() after demosaicing; each records every opcode of its list (or nothing for an empty
// list). `metadata` must outlive the processor and stay unchanged.
//
// Errors (Error::where "engine.dng_opcode"): Internal when an opcode marked supported cannot be
// applied to the data it is given (a contract violation: wrong buffer size or sample type, or
// parameters that break the parser's invariants, e.g. gains that do not fill a GainMap's grid);
// Corrupt when a warp's radial term divides by zero or yields a non-finite position;
// LimitExceeded / OutOfMemory for buffers; Cancelled. After an error the image must be
// discarded.
class DngOpcodeProcessor {
public:
    explicit DngOpcodeProcessor(const raw::RawMetadata& metadata, DngOpcodeOptions options = {});

    // True when the list holds at least one opcode (so the stage has a record).
    [[nodiscard]] bool has_list(raw::DngOpcodeListId list) const noexcept;

    // List 1 on the decoded frame (16-bit samples for any applied opcode).
    [[nodiscard]] Status run_list1(raw::RawImage& image, const core::CancellationToken& cancel = {});

    // List 2 on the normalised, not yet white-balanced sensor image of the visible area.
    [[nodiscard]] Status run_list2(SensorImage& sensor, const core::CancellationToken& cancel = {});

    // List 3 on the demosaiced image (3 channels R, G, B, or 1 monochrome) whose channel c was
    // multiplied by plane_multipliers[c] before demosaicing (white balance; 1 when unbalanced).
    [[nodiscard]] Status run_list3(image::ImageBuffer<float>& pixels, const std::array<double, 3>& plane_multipliers,
                                   const core::CancellationToken& cancel = {});

    [[nodiscard]] const DngOpcodeReport& report() const noexcept { return report_; }

    // The records of one list (a view into report().opcodes).
    [[nodiscard]] std::vector<DngOpcodeRecord> records_of(raw::DngOpcodeListId list) const;

    // The stage image of List 3 as it stands now (raw::dng_opcode_frame(), then re-based by every
    // TrimBounds applied so far: its size and Lucent's buffer origin in it). After run_list3() (or
    // without a List 3) this is the DNG SDK's stage-3 image, to which the DNG default crop applies
    // (image_area.hpp).
    [[nodiscard]] const raw::DngOpcodeFrame& stage3_frame() const noexcept { return list3_frame_; }

private:
    const raw::RawMetadata* metadata_;
    DngOpcodeOptions options_;
    raw::DngOpcodeFrame list3_frame_;
    DngOpcodeReport report_;
};

}  // namespace lucent::engine
