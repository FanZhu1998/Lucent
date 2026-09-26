#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/raw_decoder.hpp>

namespace lucent::raw {

namespace {

constexpr std::string_view kWhereContract = "raw.contract";

bool starts_with(std::span<const std::byte> bytes, std::size_t offset, std::string_view text) noexcept {
    if (bytes.size() < offset + text.size()) {
        return false;
    }
    return std::memcmp(bytes.data() + offset, text.data(), text.size()) == 0;
}

std::uint8_t byte_at(std::span<const std::byte> bytes, std::size_t index) noexcept {
    return index < bytes.size() ? std::to_integer<std::uint8_t>(bytes[index]) : std::uint8_t{0};
}

Unexpected contract_violation(std::string message) {
    return fail(ErrorCode::Internal, std::move(message), std::string(kWhereContract));
}

// Severity rank of the status an issue implies (higher is worse).
InterpretationStatus status_for(InterpretationIssueKind kind) noexcept {
    switch (kind) {
        case InterpretationIssueKind::OptionalOperationUnsupported:
            return InterpretationStatus::OptionalOperationsSkipped;
        case InterpretationIssueKind::SubimageFramesNotCombined:
            return InterpretationStatus::OptionalOperationsSkipped;
        case InterpretationIssueKind::MandatoryOperationUnsupported:
        case InterpretationIssueKind::DngVersionUnsupported:
        case InterpretationIssueKind::SubimageNotAPhotograph:
        case InterpretationIssueKind::MultiViewSource:
        case InterpretationIssueKind::UnrepresentedBlackLevelCorrection:
            return InterpretationStatus::RequiresUnsupportedOperation;
        case InterpretationIssueKind::DngInspectionFailed:
        case InterpretationIssueKind::DngRawImageAmbiguous:
        case InterpretationIssueKind::SubimageRoleUnknown:
            return InterpretationStatus::Ambiguous;
        case InterpretationIssueKind::DecoderUnsupportedFormat:
        case InterpretationIssueKind::UnsupportedSensorLayout:
        case InterpretationIssueKind::UnsupportedChannelLayout:
            return InterpretationStatus::NotDecodable;
    }
    // An unknown kind (read from newer data) must not be treated as harmless.
    return InterpretationStatus::Ambiguous;
}

}  // namespace

std::string_view to_string(RawRepresentation value) noexcept {
    switch (value) {
        case RawRepresentation::Mosaic:
            return "mosaic";
        case RawRepresentation::LinearColor:
            return "linear_color";
        case RawRepresentation::Monochrome:
            return "monochrome";
    }
    return "invalid";
}

std::string_view to_string(SampleEncoding value) noexcept {
    switch (value) {
        case SampleEncoding::UInt16:
            return "uint16";
        case SampleEncoding::Float32:
            return "float32";
    }
    return "invalid";
}

std::string_view to_string(ContainerFormat value) noexcept {
    switch (value) {
        case ContainerFormat::Unknown:
            return "unknown";
        case ContainerFormat::Tiff:
            return "tiff";
        case ContainerFormat::Dng:
            return "dng";
        case ContainerFormat::Cr3:
            return "cr3";
        case ContainerFormat::Raf:
            return "raf";
        case ContainerFormat::Orf:
            return "orf";
        case ContainerFormat::Rw2:
            return "rw2";
        case ContainerFormat::Crw:
            return "crw";
        case ContainerFormat::X3f:
            return "x3f";
    }
    return "invalid";
}

std::string_view to_string(Provenance value) noexcept {
    switch (value) {
        case Provenance::Decoder:
            return "decoder";
        case Provenance::DngTag:
            return "dng_tag";
        case Provenance::LucentRule:
            return "lucent_rule";
    }
    return "invalid";
}

std::string_view to_string(SubimageRole value) noexcept {
    switch (value) {
        case SubimageRole::Primary:
            return "primary";
        case SubimageRole::DualPixelComponent:
            return "dual_pixel_component";
        case SubimageRole::BurstFrame:
            return "burst_frame";
        case SubimageRole::PixelShiftFrame:
            return "pixel_shift_frame";
        case SubimageRole::Unknown:
            return "unknown";
    }
    return "invalid";
}

std::string_view to_string(ViewLayout value) noexcept {
    switch (value) {
        case ViewLayout::SingleView:
            return "single_view";
        case ViewLayout::DualFisheye360:
            return "dual_fisheye_360";
    }
    return "invalid";
}

std::string_view to_string(MetadataStage value) noexcept {
    switch (value) {
        case MetadataStage::AfterOpen:
            return "after_open";
        case MetadataStage::AfterUnpack:
            return "after_unpack";
    }
    return "invalid";
}

std::string_view to_string(PriorProcessingKind value) noexcept {
    switch (value) {
        case PriorProcessingKind::Demosaiced:
            return "demosaiced";
        case PriorProcessingKind::WhiteBalanceApplied:
            return "white_balance_applied";
        case PriorProcessingKind::YCbCrConverted:
            return "ycbcr_converted";
        case PriorProcessingKind::LinearizationApplied:
            return "linearization_applied";
        case PriorProcessingKind::LossyCompression:
            return "lossy_compression";
        case PriorProcessingKind::ToneCurveApplied:
            return "tone_curve_applied";
    }
    return "invalid";
}

std::string_view to_string(RequiredOperationKind value) noexcept {
    switch (value) {
        case RequiredOperationKind::DngOpcode:
            return "dng_opcode";
        case RequiredOperationKind::ProfileGainTableMap:
            return "profile_gain_table_map";
    }
    return "invalid";
}

std::string_view to_string(OperationStage value) noexcept {
    switch (value) {
        case OperationStage::StoredRaw:
            return "stored_raw";
        case OperationStage::LinearRaw:
            return "linear_raw";
        case OperationStage::AfterDemosaic:
            return "after_demosaic";
        case OperationStage::ProfileRendering:
            return "profile_rendering";
    }
    return "invalid";
}

std::string_view to_string(InterpretationStatus value) noexcept {
    switch (value) {
        case InterpretationStatus::Complete:
            return "complete";
        case InterpretationStatus::OptionalOperationsSkipped:
            return "optional_operations_skipped";
        case InterpretationStatus::RequiresUnsupportedOperation:
            return "requires_unsupported_operation";
        case InterpretationStatus::Ambiguous:
            return "ambiguous";
        case InterpretationStatus::NotDecodable:
            return "not_decodable";
    }
    return "invalid";
}

std::string_view to_string(InterpretationIssueKind value) noexcept {
    switch (value) {
        case InterpretationIssueKind::MandatoryOperationUnsupported:
            return "mandatory_operation_unsupported";
        case InterpretationIssueKind::OptionalOperationUnsupported:
            return "optional_operation_unsupported";
        case InterpretationIssueKind::DngVersionUnsupported:
            return "dng_version_unsupported";
        case InterpretationIssueKind::DngInspectionFailed:
            return "dng_inspection_failed";
        case InterpretationIssueKind::DngRawImageAmbiguous:
            return "dng_raw_image_ambiguous";
        case InterpretationIssueKind::DecoderUnsupportedFormat:
            return "decoder_unsupported_format";
        case InterpretationIssueKind::UnsupportedSensorLayout:
            return "unsupported_sensor_layout";
        case InterpretationIssueKind::UnsupportedChannelLayout:
            return "unsupported_channel_layout";
        case InterpretationIssueKind::SubimageNotAPhotograph:
            return "subimage_not_a_photograph";
        case InterpretationIssueKind::SubimageFramesNotCombined:
            return "subimage_frames_not_combined";
        case InterpretationIssueKind::SubimageRoleUnknown:
            return "subimage_role_unknown";
        case InterpretationIssueKind::MultiViewSource:
            return "multi_view_source";
        case InterpretationIssueKind::UnrepresentedBlackLevelCorrection:
            return "unrepresented_black_level_correction";
    }
    return "invalid";
}

ContainerFormat detect_container_format(std::span<const std::byte> bytes) noexcept {
    // Vendor TIFF variants first: their signatures share the "II"/"MM" prefix with TIFF.
    if (starts_with(bytes, 0, "IIRO") || starts_with(bytes, 0, "IIRS") || starts_with(bytes, 0, "MMOR")) {
        return ContainerFormat::Orf;
    }
    if (starts_with(bytes, 0, "IIU") && byte_at(bytes, 3) == 0 && bytes.size() >= 4) {
        return ContainerFormat::Rw2;
    }
    if (starts_with(bytes, 0, "II") && byte_at(bytes, 2) == 0x1A && byte_at(bytes, 3) == 0 &&
        starts_with(bytes, 6, "HEAPCCDR")) {
        return ContainerFormat::Crw;
    }
    if (bytes.size() >= 4) {
        const bool little = starts_with(bytes, 0, "II") &&
                            (byte_at(bytes, 2) == 42 || byte_at(bytes, 2) == 43) && byte_at(bytes, 3) == 0;
        const bool big = starts_with(bytes, 0, "MM") && byte_at(bytes, 2) == 0 &&
                         (byte_at(bytes, 3) == 42 || byte_at(bytes, 3) == 43);
        if (little || big) {
            return ContainerFormat::Tiff;
        }
    }
    if (starts_with(bytes, 0, "FUJIFILM")) {
        return ContainerFormat::Raf;
    }
    if (starts_with(bytes, 0, "FOVb")) {
        return ContainerFormat::X3f;
    }
    // ISO base media file: size(4) "ftyp" major brand "crx ".
    if (starts_with(bytes, 4, "ftyp") && starts_with(bytes, 8, "crx ")) {
        return ContainerFormat::Cr3;
    }
    return ContainerFormat::Unknown;
}

double SensorLevels::black_level_at(std::size_t channel_index, std::int64_t vx,
                                    std::int64_t vy) const noexcept {
    double value = black + (channel_index < channel_black.size() ? channel_black[channel_index] : 0.0);
    if (pattern_rows != 0 && pattern_cols != 0 &&
        black_pattern.size() == std::size_t{pattern_rows} * std::size_t{pattern_cols}) {
        const auto rows = static_cast<std::int64_t>(pattern_rows);
        const auto cols = static_cast<std::int64_t>(pattern_cols);
        const std::int64_t r = ((vy % rows) + rows) % rows;
        const std::int64_t c = ((vx % cols) + cols) % cols;
        value += black_pattern[static_cast<std::size_t>(r * cols + c)];
    }
    if (vx >= 0 && static_cast<std::uint64_t>(vx) < column_black_delta.size()) {
        value += column_black_delta[static_cast<std::size_t>(vx)];
    }
    if (vy >= 0 && static_cast<std::uint64_t>(vy) < row_black_delta.size()) {
        value += row_black_delta[static_cast<std::size_t>(vy)];
    }
    return value;
}

double SensorLevels::max_black_level() const noexcept {
    double value = black + *std::max_element(channel_black.begin(), channel_black.end());
    if (!black_pattern.empty()) {
        value += *std::max_element(black_pattern.begin(), black_pattern.end());
    }
    if (!column_black_delta.empty()) {
        value += *std::max_element(column_black_delta.begin(), column_black_delta.end());
    }
    if (!row_black_delta.empty()) {
        value += *std::max_element(row_black_delta.begin(), row_black_delta.end());
    }
    return value;
}

InterpretationStatus derive_interpretation_status(std::span<const InterpretationIssue> issues) noexcept {
    InterpretationStatus worst = InterpretationStatus::Complete;
    for (const InterpretationIssue& issue : issues) {
        const InterpretationStatus status = status_for(issue.kind);
        if (static_cast<std::uint8_t>(status) > static_cast<std::uint8_t>(worst)) {
            worst = status;
        }
    }
    return worst;
}

bool RawMetadata::has_prior_processing(PriorProcessingKind kind) const noexcept {
    return std::any_of(prior_processing.begin(), prior_processing.end(),
                       [kind](const PriorProcessingStep& step) { return step.kind == kind; });
}

image::CfaPattern cfa_at_visible_origin(const RawMetadata& metadata) {
    if (metadata.representation.kind != RawRepresentation::Mosaic || metadata.representation.cfa.empty()) {
        return {};
    }
    return metadata.representation.cfa.shifted(metadata.geometry.visible_area.x,
                                               metadata.geometry.visible_area.y);
}

PixelsShape shape_of(const RawPixels& pixels) noexcept {
    return std::visit(
        [](const auto& buffer) {
            using Buffer = std::decay_t<decltype(buffer)>;
            PixelsShape shape;
            shape.width = buffer.width();
            shape.height = buffer.height();
            shape.channels = buffer.channels();
            shape.encoding = std::is_same_v<typename Buffer::value_type, float> ? SampleEncoding::Float32
                                                                                : SampleEncoding::UInt16;
            return shape;
        },
        pixels);
}

const RawPixels& pixels_of(const RawImage& image) noexcept {
    return std::visit([](const auto& frame) -> const RawPixels& { return frame.pixels; }, image);
}

Status check_consistency(const DecodedRaw& decoded) {
    if (auto readable =
            core::check_readable(decoded.contract, kDecodedRawContractVersion, "decoded-RAW contract");
        !readable) {
        return readable;
    }
    const RawMetadata& m = decoded.metadata;
    const RepresentationInfo& rep = m.representation;
    const SensorGeometry& geo = m.geometry;

    if (m.stage != MetadataStage::AfterUnpack) {
        return contract_violation("decoded metadata must have stage after_unpack");
    }
    if (m.subimage_count < 1 || m.subimage_index >= m.subimage_count) {
        return contract_violation(
            std::format("subimage index {} is not below the count {}", m.subimage_index, m.subimage_count));
    }
    const auto has_issue = [&](InterpretationIssueKind kind) {
        return std::any_of(m.interpretation.issues.begin(), m.interpretation.issues.end(),
                           [kind](const InterpretationIssue& issue) { return issue.kind == kind; });
    };
    switch (m.subimage_role) {
        case SubimageRole::Primary:
            break;
        case SubimageRole::BurstFrame:
        case SubimageRole::DualPixelComponent:
        case SubimageRole::PixelShiftFrame:
        case SubimageRole::Unknown:
            if (m.subimage_count < 2) {
                return contract_violation(std::format("a single-image file cannot have subimage role {}",
                                                      to_string(m.subimage_role)));
            }
            break;
        default:
            return contract_violation("unknown subimage role");
    }
    if ((m.subimage_role == SubimageRole::DualPixelComponent &&
         !has_issue(InterpretationIssueKind::SubimageNotAPhotograph)) ||
        (m.subimage_role == SubimageRole::PixelShiftFrame &&
         !has_issue(InterpretationIssueKind::SubimageFramesNotCombined)) ||
        (m.subimage_role == SubimageRole::Unknown &&
         !has_issue(InterpretationIssueKind::SubimageRoleUnknown))) {
        return contract_violation(
            std::format("subimage role {} needs its interpretation issue", to_string(m.subimage_role)));
    }
    if (m.projection.layout != ViewLayout::SingleView &&
        !has_issue(InterpretationIssueKind::MultiViewSource)) {
        return contract_violation(
            std::format("view layout {} needs a multi_view_source issue", to_string(m.projection.layout)));
    }
    if (geo.raw_width == 0 || geo.raw_height == 0) {
        return contract_violation("raw frame is empty");
    }
    const image::PixelRect frame{0, 0, static_cast<std::int32_t>(geo.raw_width),
                                 static_cast<std::int32_t>(geo.raw_height)};
    if (geo.raw_width > 65535 || geo.raw_height > 65535 || !frame.contains(geo.visible_area)) {
        return contract_violation(std::format(
            "visible area {}x{}+{}+{} is not inside the {}x{} raw frame", geo.visible_area.width,
            geo.visible_area.height, geo.visible_area.x, geo.visible_area.y, geo.raw_width, geo.raw_height));
    }
    if (!(geo.pixel_aspect > 0.0) || !std::isfinite(geo.pixel_aspect)) {
        return contract_violation("pixel aspect must be finite and positive");
    }

    const std::size_t expected_alternative = rep.kind == RawRepresentation::Mosaic        ? 0
                                             : rep.kind == RawRepresentation::LinearColor ? 1
                                                                                          : 2;
    if (rep.kind != RawRepresentation::Mosaic && rep.kind != RawRepresentation::LinearColor &&
        rep.kind != RawRepresentation::Monochrome) {
        return contract_violation("unknown representation");
    }
    if (decoded.image.index() != expected_alternative) {
        return contract_violation(std::format("image alternative {} does not match representation {}",
                                              decoded.image.index(), to_string(rep.kind)));
    }

    const PixelsShape shape = shape_of(pixels_of(decoded.image));
    const std::int32_t expected_channels = rep.kind == RawRepresentation::LinearColor ? 3 : 1;
    if (rep.channels != expected_channels || shape.channels != expected_channels) {
        return contract_violation(
            std::format("{} data must have {} channel(s); metadata says {}, buffer has {}",
                        to_string(rep.kind), expected_channels, rep.channels, shape.channels));
    }
    if (shape.width != static_cast<std::int32_t>(geo.raw_width) ||
        shape.height != static_cast<std::int32_t>(geo.raw_height)) {
        return contract_violation(std::format("buffer is {}x{} but the raw frame is {}x{}", shape.width,
                                              shape.height, geo.raw_width, geo.raw_height));
    }
    if (shape.encoding != rep.sample_encoding) {
        return contract_violation(std::format("buffer holds {} samples but metadata says {}",
                                              to_string(shape.encoding), to_string(rep.sample_encoding)));
    }
    if (rep.color_count < 1 || rep.color_count > 4) {
        return contract_violation(std::format("colour count {} is outside 1..4", rep.color_count));
    }
    const std::size_t expected_channel_colors = rep.kind == RawRepresentation::LinearColor  ? 3
                                                : rep.kind == RawRepresentation::Monochrome ? 1
                                                                                            : 0;
    if (rep.channel_colors.empty() || rep.channel_colors.size() > 4 ||
        (expected_channel_colors != 0 && rep.channel_colors.size() != expected_channel_colors)) {
        return contract_violation(std::format("{} data cannot have {} channel colour(s)", to_string(rep.kind),
                                              rep.channel_colors.size()));
    }

    if (rep.kind == RawRepresentation::Mosaic) {
        const auto& frame_data = std::get<MosaicFrame>(decoded.image);
        if (rep.cfa.empty() || frame_data.cfa != rep.cfa) {
            return contract_violation("mosaic data needs a CFA pattern equal to the metadata's");
        }
        for (std::int32_t r = 0; r < rep.cfa.height(); ++r) {
            for (std::int32_t c = 0; c < rep.cfa.width(); ++c) {
                const image::CfaColor color = rep.cfa.color_at(r, c);
                if (std::find(rep.channel_colors.begin(), rep.channel_colors.end(), color) ==
                    rep.channel_colors.end()) {
                    return contract_violation(
                        std::format("CFA colour {} has no channel index", image::to_string(color)));
                }
            }
        }
        if (frame_data.encoding != image::ColorEncoding::camera_native_mosaic()) {
            return contract_violation("mosaic encoding must be camera-native mosaic");
        }
    } else if (!rep.cfa.empty()) {
        return contract_violation("only mosaic data carries a CFA pattern");
    }

    const SensorLevels& lv = m.levels;
    const auto finite = [](double v) { return std::isfinite(v); };
    if (!finite(lv.black) || !finite(lv.white) ||
        !std::all_of(lv.channel_black.begin(), lv.channel_black.end(), finite) ||
        !std::all_of(lv.black_pattern.begin(), lv.black_pattern.end(), finite) ||
        !std::all_of(lv.linear_max.begin(), lv.linear_max.end(), finite) ||
        !std::all_of(lv.column_black_delta.begin(), lv.column_black_delta.end(), finite) ||
        !std::all_of(lv.row_black_delta.begin(), lv.row_black_delta.end(), finite)) {
        return contract_violation("levels must be finite");
    }
    if (lv.black_pattern.size() != std::size_t{lv.pattern_rows} * std::size_t{lv.pattern_cols}) {
        return contract_violation("black pattern size does not match its dimensions");
    }
    if (!lv.column_black_delta.empty() &&
        lv.column_black_delta.size() != static_cast<std::size_t>(geo.visible_area.width)) {
        return contract_violation(std::format("{} column black deltas for a visible area {} columns wide",
                                              lv.column_black_delta.size(), geo.visible_area.width));
    }
    if (!lv.row_black_delta.empty() &&
        lv.row_black_delta.size() != static_cast<std::size_t>(geo.visible_area.height)) {
        return contract_violation(std::format("{} row black deltas for a visible area {} rows high",
                                              lv.row_black_delta.size(), geo.visible_area.height));
    }
    const double max_black = lv.max_black_level();
    if (!(lv.white > max_black)) {
        return contract_violation(
            std::format("white level {} is not above the highest black level {}", lv.white, max_black));
    }
    if (m.interpretation.status != derive_interpretation_status(m.interpretation.issues)) {
        return contract_violation("interpretation status does not match its issues");
    }
    return ok();
}

Status DecodeLimits::validate() const {
    if (max_file_bytes == 0 || max_pixels == 0 || max_memory_bytes == 0) {
        return fail(ErrorCode::InvalidArgument, "decode limits must all be at least 1", "raw.limits");
    }
    return ok();
}

}  // namespace lucent::raw
