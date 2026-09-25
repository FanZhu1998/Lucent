// Conversion of LibRaw's state into the decoded-RAW contract (decoded_raw.hpp).
//
// Field sources refer to LibRaw 0.22.2 (libraw/libraw_types.h). Colour and level fields are read
// from imgdata.color after open_datastream() and from imgdata.rawdata.color, LibRaw's snapshot
// taken at the end of unpack(), afterwards (the same source rawpy uses).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lucent/core/text.hpp>
#include <lucent/raw/dng_opcodes.hpp>
#include <lucent/raw/libraw_decoder.hpp>

#include "libraw_support.hpp"

namespace lucent::raw::detail {

namespace {

const libraw_colordata_t& colordata(const LibRaw& raw, MetadataStage stage) noexcept {
    return stage == MetadataStage::AfterUnpack ? raw.imgdata.rawdata.color : raw.imgdata.color;
}

std::int32_t euclidean_mod(std::int32_t value, std::int32_t period) noexcept {
    const std::int32_t r = value % period;
    return r < 0 ? r + period : r;
}

// LibRaw colour index at visible-area coordinates (row, col), which may be negative. LibRaw's
// own COLOR() indexes arrays with (row + 6) % 6 for X-Trans and is only defined for row >= -6,
// so the coordinates are first reduced into one period of the pattern, which gives the same
// value wherever LibRaw's is defined (FC() masks rows mod 8 and columns mod 2; the Leaf table
// is indexed (row + top_margin) & 15).
int color_index_at(LibRaw& raw, std::int32_t row, std::int32_t col) {
    if (raw.is_fuji_rotated() != 0) {
        return raw.COLOR(row, col);
    }
    const unsigned filters = raw.imgdata.idata.filters;
    std::int32_t period_rows = 8;
    std::int32_t period_cols = 2;
    if (filters == 1) {
        period_rows = period_cols = 16;
    } else if (filters == LIBRAW_XTRANS) {
        period_rows = period_cols = 6;
    }
    return raw.COLOR(euclidean_mod(row, period_rows), euclidean_mod(col, period_cols));
}

// Tile size over which LibRaw's colour index repeats, for a mosaic.
std::pair<std::int32_t, std::int32_t> cfa_period(unsigned filters) noexcept {
    if (filters == 1) {
        return {16, 16};  // Leaf CatchLight table
    }
    if (filters == LIBRAW_XTRANS) {
        return {6, 6};
    }
    return {8, 2};  // FC(): 8 rows x 2 columns of 2-bit indices
}

// Colour letters of cdesc mapped to CfaColor; the second 'G' is Green2 (LibRaw's index 3 rule).
std::array<std::optional<image::CfaColor>, 4> cdesc_colors(const char (&cdesc)[5]) noexcept {
    std::array<std::optional<image::CfaColor>, 4> colors{};
    bool seen_green = false;
    std::array<bool, 8> seen{};
    for (std::size_t i = 0; i < 4 && cdesc[i] != '\0'; ++i) {
        std::optional<image::CfaColor> color;
        switch (cdesc[i]) {
            case 'R':
                color = image::CfaColor::Red;
                break;
            case 'G':
                color = seen_green ? image::CfaColor::Green2 : image::CfaColor::Green;
                seen_green = true;
                break;
            case 'B':
                color = image::CfaColor::Blue;
                break;
            case 'C':
                color = image::CfaColor::Cyan;
                break;
            case 'M':
                color = image::CfaColor::Magenta;
                break;
            case 'Y':
                color = image::CfaColor::Yellow;
                break;
            case 'W':
                color = image::CfaColor::White;
                break;
            default:
                break;  // e.g. 'E' (emerald): no CfaColor
        }
        if (color) {
            const auto value = static_cast<std::size_t>(*color);
            if (seen[value]) {
                color.reset();  // a repeated letter other than the second green is ambiguous
            } else {
                seen[value] = true;
            }
        }
        colors[i] = color;
    }
    return colors;
}

std::optional<float> positive(float value) noexcept {
    if (std::isfinite(value) && value > 0.0f) {
        return value;
    }
    return std::nullopt;
}

std::optional<std::string> local_time_text(time_t timestamp) {
    if (timestamp <= 0) {
        return std::nullopt;
    }
    std::tm parts{};
    if (localtime_s(&parts, &timestamp) != 0) {
        return std::nullopt;
    }
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", parts.tm_year + 1900, parts.tm_mon + 1,
                       parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
}

std::string extension_hint(const std::filesystem::path& path) {
    std::wstring ext = path.extension().wstring();
    if (!ext.empty() && ext.front() == L'.') {
        ext.erase(ext.begin());
    }
    for (wchar_t& c : ext) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return core::wide_to_utf8_lossy(ext);
}

void add_issue(Interpretation& interpretation, InterpretationIssueKind kind, std::string message) {
    interpretation.issues.push_back(InterpretationIssue{kind, std::move(message)});
}

char ascii_lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Case-insensitive (ASCII) substring test.
bool contains_ci(std::string_view text, std::string_view needle) noexcept {
    if (needle.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size() && match; ++j) {
            match = ascii_lower(text[i + j]) == ascii_lower(needle[j]);
        }
        if (match) {
            return true;
        }
    }
    return false;
}

// Minimum and maximum of a non-empty list, for messages.
std::pair<double, double> value_range(const std::vector<double>& values) {
    const auto [low, high] = std::minmax_element(values.begin(), values.end());
    return {*low, *high};
}

OperationStage stage_of(DngOpcodeListId list) noexcept {
    switch (list) {
        case DngOpcodeListId::List1:
            return OperationStage::StoredRaw;
        case DngOpcodeListId::List2:
            return OperationStage::LinearRaw;
        case DngOpcodeListId::List3:
            return OperationStage::AfterDemosaic;
    }
    return OperationStage::StoredRaw;
}

// Representation from LibRaw's state. After unpack the buffer LibRaw filled decides; before, the
// same rules LibRaw's unpack() uses to choose that buffer (filters or a single colour give the
// one-sample raw_image, anything else a multi-channel image).
struct RepresentationChoice {
    RawRepresentation kind = RawRepresentation::Mosaic;
    SampleEncoding encoding = SampleEncoding::UInt16;
};

Expected<RepresentationChoice> choose_representation(LibRaw& raw, MetadataStage stage) {
    const unsigned filters = raw.imgdata.idata.filters;
    const int colors = raw.imgdata.idata.colors;
    RepresentationChoice choice;
    if (stage == MetadataStage::AfterOpen) {
        choice.kind = filters != 0  ? RawRepresentation::Mosaic
                      : colors == 1 ? RawRepresentation::Monochrome
                                    : RawRepresentation::LinearColor;
        choice.encoding = raw.is_floating_point() != 0 ? SampleEncoding::Float32 : SampleEncoding::UInt16;
        return choice;
    }
    const LibRawBuffer buffer = unpacked_buffer(raw);
    switch (buffer) {
        case LibRawBuffer::Raw:
        case LibRawBuffer::Float1:
            if (filters == 0 && colors != 1) {
                return fail(
                    ErrorCode::Internal,
                    std::format("LibRaw produced single-sample data for {} unfiltered colours", colors),
                    "raw.libraw.unpack");
            }
            choice.kind = filters != 0 ? RawRepresentation::Mosaic : RawRepresentation::Monochrome;
            break;
        case LibRawBuffer::Color3:
        case LibRawBuffer::Color4:
        case LibRawBuffer::Float3:
        case LibRawBuffer::Float4:
            choice.kind = RawRepresentation::LinearColor;
            break;
        case LibRawBuffer::None:
            return fail(ErrorCode::Internal, "LibRaw reported success but produced no pixel buffer",
                        "raw.libraw.unpack");
    }
    choice.encoding =
        (buffer == LibRawBuffer::Float1 || buffer == LibRawBuffer::Float3 || buffer == LibRawBuffer::Float4)
            ? SampleEncoding::Float32
            : SampleEncoding::UInt16;
    return choice;
}

// Fills representation.channel_colors and (for a mosaic) the CFA. Adds a NotDecodable issue when
// the layout cannot be expressed.
Status fill_channels(LibRaw& raw, const SensorGeometry& geometry, RepresentationInfo& rep,
                     Interpretation& interpretation) {
    const auto letters = cdesc_colors(raw.imgdata.idata.cdesc);
    rep.channel_colors.clear();
    rep.cfa = {};
    switch (rep.kind) {
        case RawRepresentation::Monochrome:
            rep.channels = 1;
            rep.channel_colors = {image::CfaColor::White};
            return ok();
        case RawRepresentation::LinearColor: {
            rep.channels = 3;
            if (rep.color_count != 3) {
                add_issue(interpretation, InterpretationIssueKind::UnsupportedChannelLayout,
                          std::format("full-colour data with {} colours; the contract supports 3",
                                      rep.color_count));
                rep.channel_colors = {image::CfaColor::Red, image::CfaColor::Green, image::CfaColor::Blue};
                return ok();
            }
            for (std::size_t i = 0; i < 3; ++i) {
                if (!letters[i]) {
                    add_issue(interpretation, InterpretationIssueKind::UnsupportedChannelLayout,
                              std::format("colour description '{}' has no colour for channel {}",
                                          rep.color_description, i));
                    rep.channel_colors = {image::CfaColor::Red, image::CfaColor::Green,
                                          image::CfaColor::Blue};
                    return ok();
                }
                rep.channel_colors.push_back(*letters[i]);
            }
            return ok();
        }
        case RawRepresentation::Mosaic:
            break;
    }

    rep.channels = 1;
    if (raw.is_fuji_rotated() != 0) {
        add_issue(interpretation, InterpretationIssueKind::UnsupportedSensorLayout,
                  "Fujifilm SuperCCD data in LibRaw's rotated (diagonal) layout is not supported");
        rep.channel_colors = {image::CfaColor::Red, image::CfaColor::Green, image::CfaColor::Blue};
        return ok();
    }
    const auto [rows, cols] = cfa_period(raw.imgdata.idata.filters);
    std::vector<image::CfaColor> tile;
    tile.reserve(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols));
    int max_index = -1;
    for (std::int32_t r = 0; r < rows; ++r) {
        for (std::int32_t c = 0; c < cols; ++c) {
            const int index = color_index_at(raw, r, c);
            if (index < 0 || index > 3 || !letters[static_cast<std::size_t>(index)]) {
                add_issue(interpretation, InterpretationIssueKind::UnsupportedChannelLayout,
                          std::format("CFA colour index {} has no colour in description '{}'", index,
                                      rep.color_description));
                rep.channel_colors = {image::CfaColor::Red, image::CfaColor::Green, image::CfaColor::Blue};
                return ok();
            }
            max_index = std::max(max_index, index);
            tile.push_back(*letters[static_cast<std::size_t>(index)]);
        }
    }
    for (int i = 0; i <= max_index; ++i) {
        if (!letters[static_cast<std::size_t>(i)]) {
            add_issue(
                interpretation, InterpretationIssueKind::UnsupportedChannelLayout,
                std::format("channel index {} has no colour in description '{}'", i, rep.color_description));
            rep.channel_colors = {image::CfaColor::Red, image::CfaColor::Green, image::CfaColor::Blue};
            return ok();
        }
        rep.channel_colors.push_back(*letters[static_cast<std::size_t>(i)]);
    }
    auto visible_pattern = image::CfaPattern::create(cols, rows, tile);
    if (!visible_pattern) {
        return Unexpected(std::move(visible_pattern.error()));
    }
    // LibRaw evaluates colours at visible-area coordinates; the contract's CFA is relative to
    // the raw-frame origin, which lies at (-left_margin, -top_margin) in visible coordinates.
    rep.cfa = visible_pattern->shifted(-geometry.visible_area.x, -geometry.visible_area.y);
    return ok();
}

SensorLevels levels_from_libraw(const libraw_colordata_t& color) {
    SensorLevels levels;
    levels.provenance = Provenance::Decoder;
    levels.black = color.black;
    for (std::size_t c = 0; c < 4; ++c) {
        levels.channel_black[c] = color.cblack[c];
        levels.linear_max[c] = color.linear_max[c];
    }
    const std::uint64_t rows = color.cblack[4];
    const std::uint64_t cols = color.cblack[5];
    if (rows != 0 && cols != 0 && rows * cols <= LIBRAW_CBLACK_SIZE - 6) {
        levels.pattern_rows = static_cast<std::uint32_t>(rows);
        levels.pattern_cols = static_cast<std::uint32_t>(cols);
        levels.black_pattern.assign(color.cblack + 6, color.cblack + 6 + rows * cols);
    }
    levels.white = color.maximum;
    return levels;
}

// Levels of floating-point DNG data, from the DNG raw IFD (LibRaw's integer level fields cannot
// hold fractional float levels). Absent tags take their DNG 1.7.1.0 defaults: BlackLevel 0 and,
// for floating-point data, WhiteLevel 1.0 (a note is added to `warnings`).
Expected<SensorLevels> levels_from_dng(const TiffIfdSummary& ifd, std::vector<std::string>& warnings) {
    SensorLevels levels;
    levels.provenance = Provenance::DngTag;
    const std::uint32_t samples = ifd.samples_per_pixel.value_or(1);
    const std::uint32_t rows = ifd.black_level_repeat_dim ? (*ifd.black_level_repeat_dim)[0] : 1;
    const std::uint32_t cols = ifd.black_level_repeat_dim ? (*ifd.black_level_repeat_dim)[1] : 1;
    if (!ifd.black_level.empty()) {
        if (samples == 1) {
            if (rows * cols == 1) {
                levels.black = ifd.black_level[0];
            } else {
                levels.pattern_rows = rows;
                levels.pattern_cols = cols;
                levels.black_pattern = ifd.black_level;
            }
        } else if (rows * cols == 1 && samples <= 4) {
            for (std::uint32_t s = 0; s < samples; ++s) {
                levels.channel_black[s] = ifd.black_level[s];
            }
        } else {
            return fail(ErrorCode::Unsupported,
                        "floating-point data with a per-sample black-level pattern is not supported",
                        "raw.libraw.levels");
        }
    }
    if (ifd.white_level.empty()) {
        levels.white = 1.0;
        warnings.push_back(
            std::format("DNG raw IFD {} has no WhiteLevel tag; the DNG default 1.0 for "
                        "floating-point data applies",
                        ifd.path));
        return levels;
    }
    levels.white = *std::min_element(ifd.white_level.begin(), ifd.white_level.end());
    return levels;
}

CameraColorData color_from_libraw(const libraw_colordata_t& color) {
    CameraColorData out;
    out.provenance = Provenance::Decoder;
    for (std::size_t c = 0; c < 4; ++c) {
        out.as_shot_multipliers[c] = color.cam_mul[c];
        out.daylight_multipliers[c] = color.pre_mul[c];
        for (std::size_t j = 0; j < 3; ++j) {
            out.xyz_to_camera[c][j] = color.cam_xyz[c][j];
        }
    }
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            out.camera_to_srgb_from_file[r][c] = color.cmatrix[r][c];
            out.camera_to_srgb[r][c] = color.rgb_cam[r][c];
        }
    }
    return out;
}

std::string as_shot_wb_detail(int flags) {
    std::string detail = "as-shot white balance was applied before decoding";
    const std::pair<int, std::string_view> vendors[] = {
        {LIBRAW_ASWB_CANON, "Canon multi-shot"},   {LIBRAW_ASWB_NIKON, "Nikon"},
        {LIBRAW_ASWB_NIKON_SRAW, "Nikon sRAW"},    {LIBRAW_ASWB_PENTAX, "Pentax multi-exposure"},
        {LIBRAW_ASWB_SONY, "Sony YCC pseudo-RAW"},
    };
    for (const auto& [bit, name] : vendors) {
        if ((flags & bit) != 0) {
            detail += std::format(" ({})", name);
        }
    }
    return detail;
}

// DNG-specific metadata, required operations and interpretation issues.
void apply_dng(const ExtractionContext& context, std::string_view unpacker, RawMetadata& m) {
    if (context.tiff == nullptr) {
        add_issue(m.interpretation, InterpretationIssueKind::DngInspectionFailed,
                  std::format(
                      "the DNG structure could not be inspected: {}",
                      context.tiff_error ? context.tiff_error->describe() : std::string("not a TIFF file")));
        return;
    }
    const TiffSummary& tiff = *context.tiff;
    if (!tiff.dng) {
        add_issue(m.interpretation, InterpretationIssueKind::DngInspectionFailed,
                  "the decoder reports a DNG but IFD 0 carries no DNGVersion tag");
        return;
    }
    if (!tiff.primary_raw_ifd) {
        add_issue(m.interpretation, InterpretationIssueKind::DngRawImageAmbiguous,
                  std::format("the DNG has {} raw image candidates; the decoded one cannot be identified",
                              tiff.raw_ifd_candidates.size()));
        return;
    }
    const TiffIfdSummary& ifd = tiff.ifds[*tiff.primary_raw_ifd];
    const std::uint32_t expected_width =
        context.open_raw_width != 0 ? context.open_raw_width : m.geometry.raw_width;
    const std::uint32_t expected_height =
        context.open_raw_height != 0 ? context.open_raw_height : m.geometry.raw_height;
    if (ifd.width.value_or(0) != expected_width || ifd.height.value_or(0) != expected_height ||
        m.subimage_index != 0) {
        add_issue(m.interpretation, InterpretationIssueKind::DngRawImageAmbiguous,
                  std::format("DNG raw IFD {} is {}x{} but the decoder read a {}x{} image (subimage {})",
                              ifd.path, ifd.width.value_or(0), ifd.height.value_or(0), expected_width,
                              expected_height, m.subimage_index));
        return;
    }

    DngMetadata dng;
    dng.info = *tiff.dng;
    dng.raw_ifd_index = *tiff.primary_raw_ifd;
    dng.raw_ifd = ifd;
    dng.warnings = tiff.warnings;

    if (ifd.compression) {
        DataCompression& compression = m.format.compression;
        compression.tiff_code = ifd.compression;
        compression.name = std::string(ifd.compression_info.name);
        compression.lossiness = ifd.compression_info.lossiness;
        compression.provenance = Provenance::DngTag;
        compression.basis = std::format("DNG raw IFD {} Compression {}", ifd.path, *ifd.compression);
        if (ifd.photometric) {
            compression.basis += std::format(", PhotometricInterpretation {}", *ifd.photometric);
        }
        if (ifd.jxl_distance) {
            compression.basis += std::format(", JXLDistance {}", *ifd.jxl_distance);
        }
    }
    if (!m.camera.unique_camera_model.empty() && dng.info.unique_camera_model &&
        *dng.info.unique_camera_model != m.camera.unique_camera_model) {
        m.decoder.warnings.push_back(
            std::format("UniqueCameraModel differs between decoder ('{}') and inspector ('{}')",
                        m.camera.unique_camera_model, *dng.info.unique_camera_model));
    }

    if (dng.info.backward_version > kDngSpecificationVersion) {
        add_issue(m.interpretation, InterpretationIssueKind::DngVersionUnsupported,
                  std::format("DNGBackwardVersion {} is newer than the supported DNG specification {}",
                              dng.info.backward_version.to_string(), kDngSpecificationVersion.to_string()));
    }

    const bool lossy_dng_decoder = unpacker == "lossy_dng_load_raw()";
    for (const DngOpcode& opcode : ifd.opcodes) {
        RequiredOperation op;
        op.kind = RequiredOperationKind::DngOpcode;
        op.stage = stage_of(opcode.list);
        op.name = std::string(opcode.name());
        op.opcode_id = opcode.id;
        op.index = opcode.index;
        op.min_dng_version = opcode.min_dng_version;
        op.flags = opcode.flags;
        op.mandatory = opcode.is_mandatory();
        op.preview_skippable = opcode.is_preview_skippable();
        op.follows_optional_warp_rectilinear2 = opcode.follows_optional_warp_rectilinear2;
        op.supported_by_lucent = false;
        // LibRaw's lossy_dng_load_raw() maps samples through MapPolynomial opcodes of
        // OpcodeList2 (LibRaw 0.22.2 src/decoders/dng.cpp:200-231).
        op.applied_by_decoder = lossy_dng_decoder && opcode.list == DngOpcodeListId::List2 &&
                                opcode.id == static_cast<std::uint32_t>(DngOpcodeId::MapPolynomial);
        const std::string label =
            std::format("OpcodeList{}[{}] {} (id {}, DNG {}{})", static_cast<int>(opcode.list), opcode.index,
                        op.name, opcode.id, opcode.min_dng_version.to_string(),
                        opcode.is_known() ? "" : ", unknown to this build");
        if (op.mandatory) {
            add_issue(m.interpretation, InterpretationIssueKind::MandatoryOperationUnsupported,
                      std::format("{} is mandatory and Lucent cannot apply it", label));
        } else {
            add_issue(m.interpretation, InterpretationIssueKind::OptionalOperationUnsupported,
                      std::format("{} is optional and will be skipped", label));
        }
        m.required_operations.push_back(std::move(op));
    }
    const auto add_gain_table = [&](std::string_view name) {
        RequiredOperation op;
        op.kind = RequiredOperationKind::ProfileGainTableMap;
        op.stage = OperationStage::ProfileRendering;
        op.name = std::string(name);
        op.mandatory = false;
        m.required_operations.push_back(op);
        add_issue(m.interpretation, InterpretationIssueKind::OptionalOperationUnsupported,
                  std::format("{} (camera-profile local tone map) will not be applied", name));
    };
    if (ifd.profile_gain_table_map) {
        add_gain_table("ProfileGainTableMap");
    }
    if (dng.info.profile_gain_table_map2) {
        add_gain_table("ProfileGainTableMap2");
    }

    // NoiseProfile of the raw IFD, else the copy in IFD 0 that some writers store instead (both
    // are verified against the colour planes by the inspector).
    const TiffIfdSummary& ifd0 = tiff.ifds.front();
    const TiffIfdSummary& noise_ifd = !ifd.noise_profile.empty() ? ifd : ifd0;
    if (!noise_ifd.noise_profile.empty() && noise_ifd.noise_profile.size() % 2 == 0) {
        NoiseProfile noise;
        noise.provenance = Provenance::DngTag;
        noise.source_ifd = noise_ifd.path;
        for (std::size_t i = 0; i + 1 < noise_ifd.noise_profile.size(); i += 2) {
            noise.planes.push_back(NoiseModel{noise_ifd.noise_profile[i], noise_ifd.noise_profile[i + 1]});
        }
        m.noise = std::move(noise);
    }

    // BlackLevelDeltaH/V are part of the black level (DNG 1.7.1.0 chapter 5), but SensorLevels
    // has no per-row or per-column term and LibRaw adds their rounded mean to its common black.
    std::string deltas;
    const auto describe_deltas = [&deltas](std::string_view tag, const std::vector<double>& values) {
        if (std::none_of(values.begin(), values.end(), [](double v) { return v != 0.0; })) {
            return;  // absent or all zero: nothing is lost
        }
        const auto [low, high] = value_range(values);
        deltas += std::format("{}{} ({} values, {} to {})", deltas.empty() ? "" : " and ", tag, values.size(),
                              low, high);
    };
    describe_deltas("BlackLevelDeltaH", ifd.black_level_delta_h);
    describe_deltas("BlackLevelDeltaV", ifd.black_level_delta_v);
    if (!deltas.empty()) {
        add_issue(m.interpretation, InterpretationIssueKind::UnrepresentedBlackLevelCorrection,
                  std::format("DNG raw IFD {} has {}; the contract cannot represent per-column or per-row "
                              "black levels, and the decoder folded their mean into the common black level",
                              ifd.path, deltas));
    }
    if (ifd.linearization_table) {
        m.prior_processing.push_back(PriorProcessingStep{
            PriorProcessingKind::LinearizationApplied,
            std::format("the decoder mapped stored values through the DNG LinearizationTable ({} entries)",
                        ifd.linearization_table->count)});
    }
    if (lossy_dng_decoder) {
        m.prior_processing.push_back(PriorProcessingStep{PriorProcessingKind::ToneCurveApplied,
                                                         "LibRaw mapped the 8-bit lossy JPEG samples through "
                                                         "OpcodeList2 MapPolynomial curves, or through its "
                                                         "own sRGB-like curve when there are none"});
    }
    m.dng = std::move(dng);
}

// The compression facts LibRaw holds after open_datastream(). The Fujifilm lossless flag and the
// Panasonic encoding exist only in LibRaw's unpacker state (libraw_internal.h, reached through
// the public get_internal_data_pointer()); they are only read. Field meanings are those of
// LibRaw 0.22.2 and are re-checked against the fixtures on every LibRaw upgrade (ADR 0005).
CompressionFacts compression_facts(LibRaw& raw, std::string_view unpacker) {
    const libraw_makernotes_t& notes = raw.imgdata.makernotes;
    const unpacker_data_t& unpacker_data = raw.get_internal_data_pointer()->unpacker_data;
    CompressionFacts facts;
    facts.unpacker = unpacker;
    facts.canon_quality = notes.canon.Quality;
    facts.nikon_nef_compression = notes.nikon.NEFCompression;
    facts.sony_raw_file_type = notes.sony.SonyRawFileType;
    facts.fuji_lossless = unpacker_data.fuji_lossless;
    facts.panasonic_encoding = unpacker_data.pana_encoding;
    return facts;
}

// The facts about a multi-image file. For CR3, LibRaw's own track selection (selectCRXTrack in
// src/metadata/cr3_parser.cpp) is mirrored: RAW tracks (MediaType 1) of the largest bit count are
// the image candidates; several of them are the tracks of a Dual Pixel RAW, and one track with
// several frames (samples) is a RAW burst.
SubimageFacts subimage_facts(LibRaw& raw, ContainerFormat container, std::uint32_t index) {
    SubimageFacts facts;
    facts.count = std::max(1u, raw.imgdata.idata.raw_count);
    facts.index = index;
    const unsigned maker = raw.imgdata.idata.maker_index;
    facts.canon_cr3 = maker == LIBRAW_CAMERAMAKER_Canon && container == ContainerFormat::Cr3;
    if (facts.canon_cr3) {
        const unpacker_data_t& data = raw.get_internal_data_pointer()->unpacker_data;
        const int last = std::min<int>(data.crx_track_count, LIBRAW_CRXTRACKS_MAXCOUNT - 1);
        std::int64_t max_bits = 0;
        for (int i = 0; i <= last; ++i) {
            const crx_data_header_t& track = data.crx_header[i];
            if (track.MediaType == 1) {
                max_bits = std::max(max_bits, std::int64_t{track.nBits} * track.f_width * track.f_height);
            }
        }
        for (int i = 0; i <= last && max_bits > 0; ++i) {
            const crx_data_header_t& track = data.crx_header[i];
            if (track.MediaType == 1 &&
                std::int64_t{track.nBits} * track.f_width * track.f_height == max_bits) {
                ++facts.cr3_full_size_raw_tracks;
                facts.cr3_max_frames_per_track = std::max(facts.cr3_max_frames_per_track, track.sample_count);
            }
        }
    }
    const unsigned quality = raw.imgdata.makernotes.pentax.Quality;
    facts.pentax_pixel_shift = maker == LIBRAW_CAMERAMAKER_Pentax && (quality == 7 || quality == 8);
    return facts;
}

}  // namespace

DataCompression classify_vendor_compression(const CompressionFacts& facts) {
    DataCompression out;
    out.provenance = Provenance::Decoder;
    const std::string_view codec = facts.unpacker;
    if (codec.empty()) {
        out.basis = "the decoder reported no codec";
        return out;  // name "unknown"
    }
    out.basis = std::format("decoder {}", codec);
    const auto set = [&out](std::string_view name, Lossiness lossiness, std::string_view evidence = {}) {
        out.name = std::string(name);
        out.lossiness = lossiness;
        if (!evidence.empty()) {
            out.basis += std::format("; {}", evidence);
        }
    };

    // Codecs whose lossiness is fixed by the codec itself.
    struct Fixed {
        std::string_view codec;
        std::string_view name;
        Lossiness lossiness;
    };
    static constexpr Fixed kFixed[] = {
        // Samples stored as plain or bit-packed integers.
        {"unpacked_load_raw()", "Uncompressed", Lossiness::Lossless},
        {"unpacked_load_raw_reversed()", "Uncompressed", Lossiness::Lossless},
        {"packed_load_raw()", "Uncompressed", Lossiness::Lossless},
        {"fuji_14bit_load_raw()", "Uncompressed", Lossiness::Lossless},
        {"unpacked_load_raw_FujiDBP()", "Uncompressed", Lossiness::Lossless},
        {"unpacked_load_raw_fuji_f700s20()", "Uncompressed", Lossiness::Lossless},
        {"nikon_load_striped_packed_raw()", "Uncompressed", Lossiness::Lossless},
        {"nikon_load_padded_packed_raw()", "Uncompressed", Lossiness::Lossless},
        {"nikon_14bit_load_raw()", "Uncompressed", Lossiness::Lossless},
        {"sony_arq_load_raw()", "Uncompressed", Lossiness::Lossless},
        {"imacon_full_load_raw()", "Uncompressed", Lossiness::Lossless},
        {"hasselblad_full_load_raw()", "Uncompressed", Lossiness::Lossless},
        {"panasonicC7_load_raw()", "Uncompressed", Lossiness::Lossless},
        // DNG codecs, used here only when the DNG raw IFD could not be identified.
        {"packed_dng_load_raw()", "Uncompressed", Lossiness::Lossless},
        {"uncompressed_fp_dng_load_raw()", "Uncompressed", Lossiness::Lossless},
        {"lossless_dng_load_raw()", "Lossless JPEG", Lossiness::Lossless},
        {"deflate_dng_load_raw()", "Deflate", Lossiness::Lossless},
        {"lossy_dng_load_raw()", "Lossy JPEG", Lossiness::Lossy},
        // Entropy coders without a lossy mode.
        {"lossless_jpeg_load_raw()", "Lossless JPEG", Lossiness::Lossless},
        {"canon_sraw_load_raw()", "Lossless JPEG (Canon sRAW/mRAW, subsampled YCbCr)", Lossiness::Lossless},
        {"canon_load_raw()", "Canon CRW lossless", Lossiness::Lossless},
        {"sony_ljpeg_load_raw()", "Sony ARW lossless compressed", Lossiness::Lossless},
        {"sony_ycbcr_load_raw()", "Sony ARW lossless compressed (YCbCr, M/S sizes)", Lossiness::Lossless},
        {"olympus_load_raw()", "Olympus ORF lossless compressed", Lossiness::Lossless},
        {"pentax_load_raw()", "Pentax PEF lossless compressed", Lossiness::Lossless},
        {"hasselblad_load_raw()", "Hasselblad lossless compressed", Lossiness::Lossless},
        // Sony's ARW 2 compression (a 7-bit delta code) has no lossless mode.
        {"sony_arw2_load_raw()", "Sony ARW compressed (lossy)", Lossiness::Lossy},
    };

    if (codec == "crxLoadRaw()") {
        // Canon maker note Quality: 4 RAW (lossless CRX), 7 C-RAW (lossy CRX).
        const std::string evidence = std::format("Canon maker note Quality {}", facts.canon_quality);
        if (facts.canon_quality == 4) {
            set("Canon CRX lossless (RAW)", Lossiness::Lossless, evidence);
        } else if (facts.canon_quality == 7) {
            set("Canon CRX lossy (C-RAW)", Lossiness::Lossy, evidence);
        } else {
            set("Canon CRX (mode not recorded)", Lossiness::Unknown, evidence);
        }
        return out;
    }
    if (codec == "nikon_load_raw()" || codec == "nikon_he_load_raw()") {
        // Nikon NEFCompression (ExifTool 0x0093): 1 lossy type 1, 3 lossless, 4 lossy type 2,
        // 13 high efficiency, 14 high efficiency* (both lossy).
        const std::string evidence =
            std::format("Nikon maker note NEFCompression {}", facts.nikon_nef_compression);
        switch (facts.nikon_nef_compression) {
            case 1:
                set("Nikon NEF lossy (type 1)", Lossiness::Lossy, evidence);
                break;
            case 3:
                set("Nikon NEF lossless", Lossiness::Lossless, evidence);
                break;
            case 4:
                set("Nikon NEF lossy (type 2)", Lossiness::Lossy, evidence);
                break;
            case 13:
                set("Nikon NEF high efficiency (HE)", Lossiness::Lossy, evidence);
                break;
            case 14:
                set("Nikon NEF high efficiency (HE*)", Lossiness::Lossy, evidence);
                break;
            default:
                set("Nikon NEF compressed (mode not recorded)", Lossiness::Unknown, evidence);
                break;
        }
        return out;
    }
    if (codec == "fuji_compressed_load_raw()") {
        // Flag byte 2 of the compressed-data header (LibRaw parse_fuji_compressed_header).
        const std::string evidence =
            std::format("RAF compressed-data header lossless flag {}", facts.fuji_lossless);
        if (facts.fuji_lossless == 1) {
            set("Fujifilm RAF lossless compressed", Lossiness::Lossless, evidence);
        } else if (facts.fuji_lossless == 0) {
            set("Fujifilm RAF compressed (lossy)", Lossiness::Lossy, evidence);
        } else {
            set("Fujifilm RAF compressed (mode not recorded)", Lossiness::Unknown, evidence);
        }
        return out;
    }
    if (codec == "panasonic_load_raw()") {
        // Encoding 5 stores samples bit-packed in 16-byte blocks; older encodings use a delta code
        // whose lossiness is not established here.
        const std::string evidence = std::format("Panasonic encoding {}", facts.panasonic_encoding);
        if (facts.panasonic_encoding == 5) {
            set("Uncompressed", Lossiness::Lossless, evidence);
        } else {
            set("Panasonic RW2 compressed", Lossiness::Unknown, evidence);
        }
        return out;
    }
    for (const Fixed& fixed : kFixed) {
        if (fixed.codec == codec) {
            std::string evidence;
            if (codec.starts_with("sony_") && facts.sony_raw_file_type != 0xFFFF) {
                evidence = std::format("Sony maker note SonyRawFileType {}", facts.sony_raw_file_type);
            }
            set(fixed.name, fixed.lossiness, evidence);
            return out;
        }
    }
    // Any other codec: named, lossiness not established.
    std::string_view name = codec;
    if (name.ends_with("()")) {
        name.remove_suffix(2);
    }
    set(std::format("{} (lossiness not established)", name), Lossiness::Unknown);
    return out;
}

SubimageClassification classify_subimage(const SubimageFacts& facts) {
    SubimageClassification result;
    if (facts.count <= 1) {
        return result;  // Primary
    }
    const std::string of = std::format("subimage {} of {}", facts.index, facts.count);
    if (facts.canon_cr3 && facts.cr3_max_frames_per_track > 1 && facts.cr3_full_size_raw_tracks == 1) {
        result.role = SubimageRole::BurstFrame;  // each frame is a complete exposure
        return result;
    }
    if (facts.canon_cr3 && facts.cr3_max_frames_per_track <= 1 && facts.cr3_full_size_raw_tracks == 2 &&
        facts.count == 2) {
        // Canon Dual Pixel RAW: the first track is the ordinary image (both photodiodes), the
        // second the frame of one photodiode (LibRaw documents the first as "Canon dual pixel,
        // 1st frame"; measured on the R5 fixture: about 0.54 x the signal above black).
        if (facts.index == 0) {
            result.role = SubimageRole::Primary;
            return result;
        }
        result.role = SubimageRole::DualPixelComponent;
        result.issue = InterpretationIssue{
            InterpretationIssueKind::SubimageNotAPhotograph,
            std::format("{} is the single-photodiode frame of a Canon Dual Pixel RAW (about half the signal "
                        "of subimage 0); Dual Pixel processing is not supported, and the frame is not a "
                        "photograph on its own",
                        of)};
        return result;
    }
    if (facts.pentax_pixel_shift) {
        result.role = SubimageRole::PixelShiftFrame;
        result.issue = InterpretationIssue{
            InterpretationIssueKind::SubimageFramesNotCombined,
            std::format("{} is one exposure of a Pentax pixel-shift set; the frames are not combined", of)};
        return result;
    }
    result.role = SubimageRole::Unknown;
    result.issue = InterpretationIssue{
        InterpretationIssueKind::SubimageRoleUnknown,
        std::format("the file holds {} images and Lucent cannot tell what {} is", facts.count, of)};
    return result;
}

ProjectionInfo classify_projection(std::string_view make, std::string_view model, std::uint32_t width,
                                   std::uint32_t height) {
    ProjectionInfo info;
    const bool insta360 =
        contains_ci(make, "Insta360") || contains_ci(make, "Arashi Vision") || contains_ci(model, "Insta360");
    const bool theta = contains_ci(make, "Ricoh") && contains_ci(model, "THETA");
    const bool two_to_one = width != 0 && height != 0 &&
                            (std::uint64_t{width} == 2 * std::uint64_t{height} ||
                             std::uint64_t{height} == 2 * std::uint64_t{width});
    if ((insta360 || theta) && two_to_one) {
        info.layout = ViewLayout::DualFisheye360;
        info.provenance = Provenance::LucentRule;
        info.basis = std::format(
            "{} camera (make '{}', model '{}') with a {}x{} frame: two-lens 360-degree "
            "bodies store both fisheye views in one 2:1 frame",
            insta360 ? "Insta360" : "Ricoh THETA", make, model, width, height);
    }
    return info;
}

ReferenceParity extract_reference_parity(LibRaw& raw, MetadataStage stage,
                                         const RepresentationInfo& representation) {
    const libraw_image_sizes_t& s = raw.imgdata.sizes;
    const libraw_iparams_t& id = raw.imgdata.idata;
    const libraw_colordata_t& color = colordata(raw, stage);

    ReferenceParity p;
    p.raw_width = s.raw_width;
    p.raw_height = s.raw_height;
    p.width = s.width;
    p.height = s.height;
    p.top_margin = s.top_margin;
    p.left_margin = s.left_margin;
    p.iwidth = s.iwidth;
    p.iheight = s.iheight;
    p.flip = s.flip;
    p.num_colors = id.colors;
    std::size_t cdesc_length = 0;
    while (cdesc_length < sizeof(id.cdesc) && id.cdesc[cdesc_length] != '\0') {
        ++cdesc_length;
    }
    p.color_desc = core::sanitize_utf8(std::string_view(id.cdesc, cdesc_length));

    // rawpy: raw_type is "flat" when rawdata.raw_image is set. Before unpack that is predicted
    // from the representation LibRaw will produce.
    const bool flat = stage == MetadataStage::AfterUnpack
                          ? raw.imgdata.rawdata.raw_image != nullptr
                          : representation.kind != RawRepresentation::LinearColor &&
                                representation.sample_encoding == SampleEncoding::UInt16;
    p.raw_type = flat ? "flat" : "stack";

    // rawpy raw_pattern: n x n of COLOR(y - top_margin, x - left_margin); n = 1 (filters 0),
    // 16 (filters 1), 6 (X-Trans) or 4 (filters >= 1000, reduced to 2x2 when periodic).
    if (flat) {
        int n = 0;
        if (id.filters >= 1000) {
            n = 4;
        } else if (id.filters == 0) {
            n = 1;
        } else if (id.filters == 1) {
            n = 16;
        } else if (id.filters == LIBRAW_XTRANS) {
            n = 6;
        }
        if (n > 0) {
            std::vector<std::vector<std::int32_t>> pattern(
                static_cast<std::size_t>(n), std::vector<std::int32_t>(static_cast<std::size_t>(n)));
            for (int y = 0; y < n; ++y) {
                for (int x = 0; x < n; ++x) {
                    pattern[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] =
                        id.filters == 0 ? raw.COLOR(0, 0)
                                        : color_index_at(raw, y - s.top_margin, x - s.left_margin);
                }
            }
            if (n == 4) {
                bool periodic = true;
                for (std::size_t y = 0; y < 2 && periodic; ++y) {
                    for (std::size_t x = 0; x < 2; ++x) {
                        const auto v = pattern[y][x];
                        if (pattern[y][x + 2] != v || pattern[y + 2][x] != v || pattern[y + 2][x + 2] != v) {
                            periodic = false;
                            break;
                        }
                    }
                }
                if (periodic) {
                    pattern.resize(2);
                    for (auto& row : pattern) {
                        row.resize(2);
                    }
                }
            }
            p.raw_pattern = std::move(pattern);
        }
    }

    // rawpy adjust_bl_() (rawpy/data_helper.h), itself a copy of LibRaw::adjust_bl(), applied to
    // a copy of the colour data. The unsigned arithmetic of the original is kept: its int
    // temporaries are always compared with and subtracted from unsigned values.
    std::vector<std::uint32_t> cblack(color.cblack, color.cblack + LIBRAW_CBLACK_SIZE);
    std::uint32_t black = color.black;
    const unsigned filters = id.filters;
    const std::size_t pattern_capacity = LIBRAW_CBLACK_SIZE - 6;
    if (filters > 1000 && (cblack[4] + 1) / 2 == 1 && (cblack[5] + 1) / 2 == 1) {
        int clrs[4];
        int lastg = -1;
        int gcnt = 0;
        for (int c = 0; c < 4; ++c) {
            clrs[c] = raw.FC(c / 2, c % 2);
            if (clrs[c] == 1) {
                ++gcnt;
                lastg = c;
            }
        }
        if (gcnt > 1 && lastg >= 0) {
            clrs[lastg] = 3;
        }
        for (unsigned c = 0; c < 4; ++c) {
            cblack[static_cast<std::size_t>(clrs[c])] +=
                cblack[6 + c / 2 % cblack[4] * cblack[5] + c % 2 % cblack[5]];
        }
        cblack[4] = cblack[5] = 0;
    } else if (filters <= 1000 && cblack[4] == 1 && cblack[5] == 1) {
        for (std::size_t c = 0; c < 4; ++c) {
            cblack[c] += cblack[6];
        }
        cblack[4] = cblack[5] = 0;
    }
    std::uint32_t common = cblack[3];
    for (std::size_t c = 0; c < 3; ++c) {
        if (common > cblack[c]) {
            common = cblack[c];
        }
    }
    for (std::size_t c = 0; c < 4; ++c) {
        cblack[c] -= common;
    }
    black += common;
    const std::uint64_t cells = std::uint64_t{cblack[4]} * cblack[5];
    if (cblack[4] != 0 && cblack[5] != 0 && cells <= pattern_capacity) {
        common = cblack[6];
        for (std::size_t c = 1; c < cells; ++c) {
            if (common > cblack[6 + c]) {
                common = cblack[6 + c];
            }
        }
        std::size_t nonzero = 0;
        for (std::size_t c = 0; c < cells; ++c) {
            cblack[6 + c] -= common;
            if (cblack[6 + c] != 0) {
                ++nonzero;
            }
        }
        black += common;
        if (nonzero == 0) {
            cblack[4] = cblack[5] = 0;
        }
    }
    for (std::size_t c = 0; c < 4; ++c) {
        p.black_level_per_channel[c] = cblack[c] + black;
    }

    p.white_level = color.maximum;
    if (std::all_of(std::begin(color.linear_max), std::end(color.linear_max),
                    [](unsigned v) { return v > 0; })) {
        p.camera_white_level_per_channel = std::array<std::uint32_t, 4>{
            color.linear_max[0], color.linear_max[1], color.linear_max[2], color.linear_max[3]};
    }
    for (std::size_t c = 0; c < 4; ++c) {
        p.camera_whitebalance[c] = color.cam_mul[c];
        p.daylight_whitebalance[c] = color.pre_mul[c];
        for (std::size_t j = 0; j < 3; ++j) {
            p.rgb_xyz_matrix[c][j] = color.cam_xyz[c][j];
        }
    }
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            p.color_matrix[r][c] = color.cmatrix[r][c];
        }
    }
    return p;
}

Expected<RawMetadata> extract_metadata(LibRaw& raw, const ExtractionContext& context) {
    const libraw_iparams_t& id = raw.imgdata.idata;
    const libraw_image_sizes_t& sizes = raw.imgdata.sizes;
    const libraw_colordata_t& color = colordata(raw, context.stage);

    RawMetadata m;
    m.stage = context.stage;
    m.source = context.source;
    m.subimage_index = context.subimage_index;
    m.subimage_count = std::max(1u, id.raw_count);

    // ---- Format ----------------------------------------------------------------------------
    m.format.container = context.sniffed_container;
    if (id.dng_version != 0) {
        m.format.dng_version = DngVersion::from_packed(id.dng_version);
        if (context.sniffed_container == ContainerFormat::Tiff) {
            m.format.container = ContainerFormat::Dng;
        }
    }
    m.format.extension_hint = extension_hint(context.source.path);

    // ---- Identity and capture ----------------------------------------------------------------
    m.camera.make = text_field(id.make, sizeof(id.make));
    m.camera.model = text_field(id.model, sizeof(id.model));
    m.camera.normalized_make = text_field(id.normalized_make, sizeof(id.normalized_make));
    m.camera.normalized_model = text_field(id.normalized_model, sizeof(id.normalized_model));
    m.camera.software = text_field(id.software, sizeof(id.software));
    if (id.dng_version != 0) {
        m.camera.unique_camera_model = text_field(color.UniqueCameraModel, sizeof(color.UniqueCameraModel));
    }

    const libraw_lensinfo_t& lens = raw.imgdata.lens;
    m.lens.make = text_field(lens.LensMake, sizeof(lens.LensMake));
    m.lens.model = text_field(lens.Lens, sizeof(lens.Lens));
    if (m.lens.model.empty()) {
        m.lens.model = text_field(lens.makernotes.Lens, sizeof(lens.makernotes.Lens));
    }
    m.lens.min_focal_length_mm = positive(lens.MinFocal);
    m.lens.max_focal_length_mm = positive(lens.MaxFocal);
    m.lens.max_aperture_at_min_focal = positive(lens.MaxAp4MinFocal);
    m.lens.max_aperture_at_max_focal = positive(lens.MaxAp4MaxFocal);
    m.lens.focal_length_35mm_equivalent_mm = positive(static_cast<float>(lens.FocalLengthIn35mmFormat));

    const libraw_imgother_t& other = raw.imgdata.other;
    m.capture.iso = positive(other.iso_speed);
    m.capture.exposure_time_s = positive(other.shutter);
    m.capture.f_number = positive(other.aperture);
    m.capture.focal_length_mm = positive(other.focal_len);
    m.capture.capture_time_local = local_time_text(other.timestamp);

    // ---- Geometry ----------------------------------------------------------------------------
    m.geometry.raw_width = sizes.raw_width;
    m.geometry.raw_height = sizes.raw_height;
    const image::PixelRect frame{0, 0, static_cast<std::int32_t>(sizes.raw_width),
                                 static_cast<std::int32_t>(sizes.raw_height)};
    const image::PixelRect visible{sizes.left_margin, sizes.top_margin, sizes.width, sizes.height};
    m.geometry.visible_area = frame.intersect(visible);
    if (m.geometry.visible_area.empty()) {
        return fail(
            ErrorCode::Corrupt,
            std::format("the decoder's visible area {}x{}+{}+{} lies outside the {}x{} raw frame",
                        visible.width, visible.height, visible.x, visible.y, frame.width, frame.height),
            "raw.libraw.geometry");
    }
    if (m.geometry.visible_area != visible) {
        m.decoder.warnings.push_back(std::format(
            "the decoder's visible area {}x{}+{}+{} extends beyond the {}x{} raw frame and was clipped",
            visible.width, visible.height, visible.x, visible.y, frame.width, frame.height));
    }
    if (std::isfinite(sizes.pixel_aspect) && sizes.pixel_aspect > 0.0) {
        m.geometry.pixel_aspect = sizes.pixel_aspect;
    } else {
        m.decoder.warnings.push_back(
            std::format("invalid pixel aspect {} replaced by 1", sizes.pixel_aspect));
    }
    if (auto orientation = image::from_libraw_flip(sizes.flip); orientation) {
        m.geometry.orientation = *orientation;
    } else {
        m.decoder.warnings.push_back(
            std::format("invalid LibRaw flip {}; orientation left normal", sizes.flip));
    }

    // ---- Decoder -----------------------------------------------------------------------------
    libraw_decoder_info_t decoder_info{};
    if (raw.get_decoder_info(&decoder_info) == LIBRAW_SUCCESS && decoder_info.decoder_name != nullptr) {
        m.decoder.unpacker = decoder_info.decoder_name;
    }
    m.decoder.adapter_id = std::string(kLibRawDecoderId);
    m.decoder.library = "LibRaw";
    m.decoder.library_version = LibRaw::version();
    m.decoder.capabilities = capability_names(LibRaw::capabilities());
    m.decoder.unpacker_flags = decoder_flag_names(decoder_info.decoder_flags);
    m.decoder.process_warnings = process_warning_names(raw.imgdata.process_warnings);
    if ((decoder_info.decoder_flags & LIBRAW_DECODER_UNSUPPORTED_FORMAT) != 0) {
        add_issue(
            m.interpretation, InterpretationIssueKind::DecoderUnsupportedFormat,
            std::format("LibRaw {} recognises the file but cannot decode its data ({})", LibRaw::version(),
                        m.decoder.unpacker.empty() ? std::string("no decoder") : m.decoder.unpacker));
    }

    // ---- Representation ----------------------------------------------------------------------
    auto choice = choose_representation(raw, context.stage);
    if (!choice) {
        return Unexpected(std::move(choice.error()));
    }
    m.representation.kind = choice->kind;
    m.representation.sample_encoding = choice->encoding;
    m.representation.color_count = id.colors;
    {
        std::size_t length = 0;
        while (length < sizeof(id.cdesc) && id.cdesc[length] != '\0') {
            ++length;
        }
        m.representation.color_description = core::sanitize_utf8(std::string_view(id.cdesc, length));
    }
    if (auto filled = fill_channels(raw, m.geometry, m.representation, m.interpretation); !filled) {
        return Unexpected(std::move(filled.error()));
    }

    // ---- DNG ---------------------------------------------------------------------------------
    if (id.dng_version != 0) {
        apply_dng(context, m.decoder.unpacker, m);
    }

    // ---- Compression -------------------------------------------------------------------------
    // DNG: the raw IFD's Compression tag (apply_dng). Otherwise, or when the DNG raw IFD could
    // not be identified: the decoder's codec and the camera's recorded mode.
    if (m.format.compression.provenance != Provenance::DngTag) {
        m.format.compression = classify_vendor_compression(compression_facts(raw, m.decoder.unpacker));
    }
    if (m.format.compression.lossiness == Lossiness::Lossy) {
        m.prior_processing.push_back(PriorProcessingStep{
            PriorProcessingKind::LossyCompression,
            std::format("{} compression ({})", m.format.compression.name, m.format.compression.basis)});
    }

    // ---- Subimage role and view layout -------------------------------------------------------
    const SubimageClassification subimage =
        classify_subimage(subimage_facts(raw, m.format.container, context.subimage_index));
    m.subimage_role = subimage.role;
    if (subimage.issue) {
        m.interpretation.issues.push_back(*subimage.issue);
    }
    m.projection = classify_projection(m.camera.make, m.camera.model,
                                       static_cast<std::uint32_t>(m.geometry.visible_area.width),
                                       static_cast<std::uint32_t>(m.geometry.visible_area.height));
    if (m.projection.layout != ViewLayout::SingleView) {
        add_issue(m.interpretation, InterpretationIssueKind::MultiViewSource,
                  std::format("unstitched {} source ({}); developing it as one planar image would show the "
                              "separate lens views, and Lucent cannot stitch them",
                              to_string(m.projection.layout), m.projection.basis));
    }

    // ---- Levels and colour ------------------------------------------------------------------
    if (m.representation.sample_encoding == SampleEncoding::Float32) {
        if (!m.dng) {
            return fail(
                ErrorCode::Unsupported,
                "floating-point data needs the DNG level tags, but the DNG raw IFD could not be identified",
                "raw.libraw.levels");
        }
        auto levels = levels_from_dng(m.dng->raw_ifd, m.decoder.warnings);
        if (!levels) {
            return Unexpected(std::move(levels.error()));
        }
        m.levels = std::move(*levels);
    } else {
        m.levels = levels_from_libraw(color);
    }
    m.color = color_from_libraw(color);

    // ---- Prior processing --------------------------------------------------------------------
    if (m.representation.kind == RawRepresentation::LinearColor) {
        m.prior_processing.insert(
            m.prior_processing.begin(),
            PriorProcessingStep{PriorProcessingKind::Demosaiced,
                                id.dng_version != 0
                                    ? "the DNG stores full-colour (LinearRaw) data"
                                    : "the file stores full-colour data (not a sensor mosaic)"});
    }
    if (raw.is_sraw() != 0) {
        m.prior_processing.push_back(
            PriorProcessingStep{PriorProcessingKind::YCbCrConverted,
                                "the file stores YCbCr (sRAW/mRAW or YCC pseudo-RAW); LibRaw converted it to "
                                "camera RGB while unpacking"});
    }
    if ((color.as_shot_wb_applied & LIBRAW_ASWB_APPLIED) != 0) {
        m.prior_processing.push_back(PriorProcessingStep{PriorProcessingKind::WhiteBalanceApplied,
                                                         as_shot_wb_detail(color.as_shot_wb_applied)});
    }

    m.interpretation.status = derive_interpretation_status(m.interpretation.issues);
    m.decoder.reference_parity = extract_reference_parity(raw, context.stage, m.representation);
    return m;
}

}  // namespace lucent::raw::detail
