#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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

std::string_view to_string(ImageAreaSource value) noexcept {
    switch (value) {
        case ImageAreaSource::Unknown:
            return "unknown";
        case ImageAreaSource::DngDefaultCrop:
            return "dng_default_crop";
        case ImageAreaSource::DngDefaultWholeImage:
            return "dng_default_whole_image";
        case ImageAreaSource::DecoderInsetCrop:
            return "decoder_inset_crop";
    }
    return "invalid";
}

ImageArea dng_image_area(const TiffIfdSummary& ifd, image::Size raw_frame) {
    ImageArea area;
    const std::uint32_t width = ifd.width.value_or(0);
    const std::uint32_t height = ifd.height.value_or(0);
    if (width == 0 || height == 0 || static_cast<std::int64_t>(width) != raw_frame.width ||
        static_cast<std::int64_t>(height) != raw_frame.height) {
        area.basis = std::format(
            "the DNG raw IFD {} is {} x {}, the decoded raw frame {} x {}: its DefaultCrop "
            "cannot be placed",
            ifd.path, width, height, raw_frame.width, raw_frame.height);
        return area;
    }
    const DngRect active = ifd.active_area.value_or(DngRect{0, 0, height, width});
    const image::PixelRect reference{static_cast<std::int32_t>(active.left),
                                     static_cast<std::int32_t>(active.top),
                                     static_cast<std::int32_t>(active.right - active.left),
                                     static_cast<std::int32_t>(active.bottom - active.top)};
    const std::string frame =
        ifd.active_area
            ? std::format("the ActiveArea {}x{}+{}+{} of {}", reference.width, reference.height, reference.x,
                          reference.y, ifd.path)
            : std::format("the whole {} x {} image of {} (no ActiveArea)", width, height, ifd.path);
    area.reference = reference;
    if (ifd.default_crop_origin || ifd.default_crop_size) {
        // Each tag has its own default (DNG 1.4.0.0 p. 31): DefaultCropOrigin (0, 0), DefaultCropSize
        // ImageWidth x ImageLength. A rectangle that overruns the stage-3 image is moved back inside
        // and cut when the engine places it (image_area.hpp, the DNG SDK's DefaultCropArea()).
        const std::array<double, 2> origin = ifd.default_crop_origin.value_or(std::array<double, 2>{0.0, 0.0});
        const std::array<double, 2> size = ifd.default_crop_size.value_or(
            std::array<double, 2>{static_cast<double>(width), static_cast<double>(height)});
        area.source = ImageAreaSource::DngDefaultCrop;
        area.provenance = Provenance::DngTag;
        area.rect = AreaRect{reference.x + origin[0], reference.y + origin[1], size[0], size[1]};
        area.basis = std::format("DNG DefaultCropOrigin ({}, {}) and DefaultCropSize {} x {}, relative to {}",
                                 origin[0], origin[1], size[0], size[1], frame);
        if (!ifd.default_crop_origin) {
            area.basis += "; DefaultCropOrigin is absent: the specification's default (0, 0)";
        } else if (!ifd.default_crop_size) {
            area.basis += std::format(
                "; DefaultCropSize is absent: the specification's default, the IFD's ImageWidth x ImageLength "
                "{} x {}",
                width, height);
        }
    } else {
        area.source = ImageAreaSource::DngDefaultWholeImage;
        area.provenance = Provenance::LucentRule;
        area.rect = AreaRect{static_cast<double>(reference.x), static_cast<double>(reference.y),
                             static_cast<double>(reference.width), static_cast<double>(reference.height)};
        area.basis = std::format("no DNG DefaultCrop tags: the specification's default, {}", frame);
    }
    if (ifd.default_user_crop) {
        const auto& u = *ifd.default_user_crop;
        if (!(u[0] == 0.0 && u[1] == 0.0 && u[2] == 1.0 && u[3] == 1.0)) {
            area.user_crop = NormalizedCrop{u[0], u[1], u[2], u[3]};
            area.user_crop_provenance = Provenance::DngTag;
            area.basis +=
                std::format("; DefaultUserCrop top {}, left {}, bottom {}, right {}", u[0], u[1], u[2], u[3]);
        }
    }
    return area;
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

std::optional<DngBayerPhase> bayer_phase_of(const image::CfaPattern& cfa) noexcept {
    if (cfa.empty() || !cfa.is_bayer() || cfa.width() != 2 || cfa.height() != 2) {
        return std::nullopt;
    }
    const image::CfaColor first = cfa.color_at(0, 0);
    if (first == image::CfaColor::Red) {
        return DngBayerPhase::Red;
    }
    if (first == image::CfaColor::Blue) {
        return DngBayerPhase::Blue;
    }
    return cfa.color_at(0, 1) == image::CfaColor::Red ? DngBayerPhase::GreenRedRow : DngBayerPhase::GreenBlueRow;
}

DngOpcodeFrame dng_opcode_frame(const RawMetadata& metadata, DngOpcodeListId list) {
    DngOpcodeFrame frame;
    DngOpcodeImage& image = frame.image;
    image.list = list;
    if (!metadata.dng) {
        image.list_unavailable = "the DNG raw IFD was not identified, so its opcodes cannot be located";
        return frame;
    }
    const TiffIfdSummary& ifd = metadata.dng->raw_ifd;
    const RepresentationInfo& rep = metadata.representation;
    const SensorGeometry& geo = metadata.geometry;
    const std::uint32_t ifd_width = ifd.width.value_or(0);
    const std::uint32_t ifd_height = ifd.height.value_or(0);
    const bool mosaic = rep.kind == RawRepresentation::Mosaic;
    image.mosaic = mosaic && list != DngOpcodeListId::List3;
    image.integer_samples = rep.sample_encoding == SampleEncoding::UInt16;
    image.pixel_aspect = geo.pixel_aspect;

    if (list == DngOpcodeListId::List1) {
        image.width = ifd_width;
        image.height = ifd_height;
        image.planes = ifd.samples_per_pixel.value_or(1);
        if (mosaic) {
            image.bayer_phase = bayer_phase_of(rep.cfa);
        }
        if (ifd_width != geo.raw_width || ifd_height != geo.raw_height) {
            image.list_unavailable =
                std::format("the decoded {} x {} frame is not the {} x {} raw IFD that OpcodeList1 processes",
                            geo.raw_width, geo.raw_height, ifd_width, ifd_height);
        } else if (metadata.has_prior_processing(PriorProcessingKind::LinearizationApplied)) {
            image.list_unavailable = "the decoder applied the LinearizationTable while unpacking, so OpcodeList1 "
                                     "can no longer see the stored values";
        } else if (metadata.has_prior_processing(PriorProcessingKind::ToneCurveApplied)) {
            image.list_unavailable = "the decoder mapped the lossy samples through a tone curve while unpacking, "
                                     "so OpcodeList1 can no longer see the stored values";
        }
        return frame;
    }

    // Lists 2 and 3: the ActiveArea (the whole image without one).
    DngRect area{0, 0, ifd_height, ifd_width};
    if (ifd.active_area) {
        area = *ifd.active_area;
    }
    image.width = area.right - area.left;
    image.height = area.bottom - area.top;
    frame.offset_x = geo.visible_area.x - static_cast<std::int32_t>(area.left);
    frame.offset_y = geo.visible_area.y - static_cast<std::int32_t>(area.top);
    const image::PixelRect stage{static_cast<std::int32_t>(area.left), static_cast<std::int32_t>(area.top),
                                 static_cast<std::int32_t>(image.width), static_cast<std::int32_t>(image.height)};
    if (ifd_width != geo.raw_width || ifd_height != geo.raw_height || !stage.contains(geo.visible_area)) {
        image.list_unavailable = std::format(
            "the visible area {}x{}+{}+{} of the {} x {} frame is not inside the DNG ActiveArea {}x{}+{}+{} of the "
            "{} x {} raw IFD",
            geo.visible_area.width, geo.visible_area.height, geo.visible_area.x, geo.visible_area.y, geo.raw_width,
            geo.raw_height, image.width, image.height, area.left, area.top, ifd_width, ifd_height);
        return frame;
    }

    if (list == DngOpcodeListId::List2) {
        image.planes = ifd.samples_per_pixel.value_or(1);
        if (mosaic) {
            image.bayer_phase = bayer_phase_of(
                rep.cfa.shifted(static_cast<std::int32_t>(area.left), static_cast<std::int32_t>(area.top)));
        }
        const bool decoder_ran_list2 =
            std::any_of(metadata.required_operations.begin(), metadata.required_operations.end(),
                        [](const RequiredOperation& op) {
                            return op.kind == RequiredOperationKind::DngOpcode && op.applied_by_decoder &&
                                   op.stage == OperationStage::LinearRaw;
                        });
        if (decoder_ran_list2) {
            image.list_unavailable = "the decoder applied opcodes of OpcodeList2 while unpacking, before black "
                                     "subtraction; Lucent cannot run the list in its specified order";
        }
        return frame;
    }

    // List 3: the demosaiced colour planes (Lucent's channels R, G, B or the monochrome plane).
    if (rep.kind == RawRepresentation::Monochrome) {
        image.planes = 1;
    } else {
        image.planes = 3;
        const std::vector<std::uint8_t>& plane_colors = ifd.cfa_plane_color;
        if (mosaic && !plane_colors.empty() && plane_colors != std::vector<std::uint8_t>{0, 1, 2}) {
            image.list_unavailable = "the CFA colour planes are not red, green, blue in that order";
        } else if (!mosaic && ifd.samples_per_pixel.value_or(1) != 3) {
            image.list_unavailable = "the full-colour image does not have 3 planes";
        }
    }
    return frame;
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

OperationStage operation_stage_of(DngOpcodeListId list) noexcept {
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

RequiredOperation required_operation_for(const DngOpcode& opcode) {
    RequiredOperation op;
    op.kind = RequiredOperationKind::DngOpcode;
    op.stage = operation_stage_of(opcode.list);
    op.name = std::string(opcode.name());
    op.opcode_id = opcode.id;
    op.index = opcode.index;
    op.min_dng_version = opcode.min_dng_version;
    op.flags = opcode.flags;
    op.mandatory = opcode.is_mandatory();
    op.preview_skippable = opcode.is_preview_skippable();
    op.follows_optional_warp_rectilinear2 = opcode.follows_optional_warp_rectilinear2;
    return op;
}

namespace {

DngOpcodeListId list_of(OperationStage stage) noexcept {
    return stage == OperationStage::StoredRaw   ? DngOpcodeListId::List1
           : stage == OperationStage::LinearRaw ? DngOpcodeListId::List2
                                                : DngOpcodeListId::List3;
}

bool matches(const RequiredOperation& op, const DngOpcode& opcode) noexcept {
    return op.kind == RequiredOperationKind::DngOpcode && list_of(op.stage) == opcode.list &&
           op.index == opcode.index && op.opcode_id == opcode.id;
}

// The support of every opcode of metadata.dng->raw_ifd.opcodes (same order), assessed in
// processing order (List 1, 2, 3, each by index) against the stage image the engine's
// DngOpcodeProcessor will have at that point: dng_opcode_frame() for List 1 and at the start of
// List 2; a TrimBounds Lucent applies in List 2 makes its rectangle the stage image of the rest of
// List 2 and of List 3, one in List 3 of the rest of List 3. Lucent's buffer (the visible area)
// is followed too: a TrimBounds that keeps none of it cannot be applied. An opcode the decoder
// applied (an operation matching it is applied_by_decoder) is never Lucent's, so it changes
// nothing.
std::vector<DngOpcodeSupport> assess_in_order(const RawMetadata& m) {
    std::vector<DngOpcodeSupport> supports;
    if (!m.dng) {
        return supports;
    }
    const std::vector<DngOpcode>& opcodes = m.dng->raw_ifd.opcodes;
    supports.resize(opcodes.size());
    const auto decoder_applied = [&](const DngOpcode& opcode) {
        return std::any_of(m.required_operations.begin(), m.required_operations.end(),
                           [&](const RequiredOperation& op) { return op.applied_by_decoder && matches(op, opcode); });
    };
    // The running stage image (Lists 2 and 3) and Lucent's buffer in it.
    struct Running {
        std::int64_t width = 0;
        std::int64_t height = 0;
        std::int64_t buffer_x = 0;
        std::int64_t buffer_y = 0;
        std::int64_t buffer_width = 0;
        std::int64_t buffer_height = 0;
    };
    std::optional<Running> running;
    for (const DngOpcodeListId list : {DngOpcodeListId::List1, DngOpcodeListId::List2, DngOpcodeListId::List3}) {
        const DngOpcodeFrame frame = dng_opcode_frame(m, list);
        if (list == DngOpcodeListId::List2) {
            running = Running{frame.image.width,  frame.image.height,       frame.offset_x,
                              frame.offset_y,     m.geometry.visible_area.width, m.geometry.visible_area.height};
        }
        std::vector<std::size_t> order;
        for (std::size_t k = 0; k < opcodes.size(); ++k) {
            if (opcodes[k].list == list) {
                order.push_back(k);
            }
        }
        std::stable_sort(order.begin(), order.end(),
                         [&](std::size_t a, std::size_t b) { return opcodes[a].index < opcodes[b].index; });
        for (const std::size_t k : order) {
            const DngOpcode& opcode = opcodes[k];
            DngOpcodeImage image = frame.image;
            if (running) {
                image.width = static_cast<std::uint32_t>(running->width);
                image.height = static_cast<std::uint32_t>(running->height);
            }
            DngOpcodeSupport support = assess_dng_opcode(opcode, image);
            const auto* trim = std::get_if<DngTrimBounds>(&opcode.parameters);
            if (support.supported && trim != nullptr && running && !decoder_applied(opcode)) {
                const DngOpcodeRect& b = trim->bounds;
                const std::int64_t x0 = std::max<std::int64_t>(b.left, running->buffer_x);
                const std::int64_t y0 = std::max<std::int64_t>(b.top, running->buffer_y);
                const std::int64_t x1 = std::min<std::int64_t>(b.right, running->buffer_x + running->buffer_width);
                const std::int64_t y1 = std::min<std::int64_t>(b.bottom, running->buffer_y + running->buffer_height);
                if (x1 <= x0 || y1 <= y0) {
                    support = DngOpcodeSupport{
                        false, std::format("the TrimBounds rectangle (top {}, left {}, bottom {}, right {}) keeps no "
                                           "pixel of the {} x {} part of the image that Lucent holds",
                                           b.top, b.left, b.bottom, b.right, running->buffer_width,
                                           running->buffer_height)};
                } else {
                    running = Running{std::int64_t{b.right} - b.left, std::int64_t{b.bottom} - b.top, x0 - b.left,
                                      y0 - b.top, x1 - x0, y1 - y0};
                }
            }
            supports[k] = std::move(support);
        }
    }
    return supports;
}

}  // namespace

void assess_dng_opcode_operations(RawMetadata& m) {
    const std::vector<DngOpcodeSupport> supports = assess_in_order(m);
    for (RequiredOperation& op : m.required_operations) {
        if (op.kind != RequiredOperationKind::DngOpcode) {
            continue;
        }
        op.supported_by_lucent = false;
        op.superseded_by_warp_rectilinear2 = false;
        const DngOpcodeListId list = list_of(op.stage);
        std::optional<std::size_t> found;
        if (m.dng) {
            const std::vector<DngOpcode>& opcodes = m.dng->raw_ifd.opcodes;
            for (std::size_t k = 0; k < opcodes.size(); ++k) {
                if (matches(op, opcodes[k])) {
                    found = k;
                    break;
                }
            }
        }
        const std::string label = std::format("OpcodeList{}[{}] {} (id {}, DNG {}{})", static_cast<int>(list),
                                              op.index, op.name, op.opcode_id, op.min_dng_version.to_string(),
                                              is_known_dng_opcode(op.opcode_id) ? "" : ", unknown to this build");
        if (op.applied_by_decoder) {
            op.lucent_note = "the decoder applied it while unpacking; Lucent does not apply it again";
        } else if (!found) {
            op.lucent_note = "the opcode is not in the identified DNG raw IFD";
        } else if (const DngOpcodeSupport& support = supports[*found]; support.supported) {
            op.supported_by_lucent = true;
            op.lucent_note.clear();
        } else {
            op.lucent_note = support.reason;
        }
        // Skip rule: a WarpRectilinear/WarpFisheye right after an optional WarpRectilinear2 that
        // Lucent applies is skipped (the operation before it in the same list is that opcode).
        if (op.follows_optional_warp_rectilinear2 && op.index > 0) {
            const auto previous = std::find_if(m.required_operations.begin(), m.required_operations.end(),
                                               [&](const RequiredOperation& other) {
                                                   return other.kind == RequiredOperationKind::DngOpcode &&
                                                          other.stage == op.stage && other.index == op.index - 1;
                                               });
            if (previous != m.required_operations.end() && previous->supported_by_lucent) {
                op.supported_by_lucent = false;
                op.superseded_by_warp_rectilinear2 = true;
                op.lucent_note = "skipped: it follows an optional WarpRectilinear2 that Lucent applies (DNG 1.7.1.0 "
                                 "chapter 7)";
                continue;
            }
        }
        if (op.supported_by_lucent) {
            continue;
        }
        if (op.mandatory) {
            m.interpretation.issues.push_back(
                InterpretationIssue{InterpretationIssueKind::MandatoryOperationUnsupported,
                                    std::format("{} is mandatory and Lucent cannot apply it: {}", label, op.lucent_note)});
        } else {
            m.interpretation.issues.push_back(
                InterpretationIssue{InterpretationIssueKind::OptionalOperationUnsupported,
                                    std::format("{} is optional and will be skipped: {}", label, op.lucent_note)});
        }
    }
    m.interpretation.status = derive_interpretation_status(m.interpretation.issues);
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
    {
        const ImageArea& area = m.image_area;
        switch (area.source) {
            case ImageAreaSource::Unknown:
            case ImageAreaSource::DngDefaultCrop:
            case ImageAreaSource::DngDefaultWholeImage:
            case ImageAreaSource::DecoderInsetCrop:
                break;
            default:
                return contract_violation("unknown image area source");
        }
        const bool known = area.source != ImageAreaSource::Unknown;
        if (known != area.rect.has_value() || (known && !frame.contains(area.reference)) ||
            (!known && (area.user_crop || !area.reference.empty()))) {
            return contract_violation(
                std::format("image area ({}) needs a rectangle and a reference inside the frame "
                            "exactly when it is known",
                            to_string(area.source)));
        }
        if (area.rect) {
            const AreaRect& r = *area.rect;
            if (!finite(r.x) || !finite(r.y) || !finite(r.width) || !finite(r.height) || !(r.width > 0.0) ||
                !(r.height > 0.0)) {
                return contract_violation("image area must be finite with a positive size");
            }
            if (area.source == ImageAreaSource::DecoderInsetCrop &&
                (r.x < 0.0 || r.y < 0.0 || r.x + r.width > geo.raw_width ||
                 r.y + r.height > geo.raw_height)) {
                return contract_violation("a decoder inset crop must lie inside the raw frame");
            }
        }
        if (area.user_crop) {
            const NormalizedCrop& u = *area.user_crop;
            const auto unit = [&](double v) { return finite(v) && v >= 0.0 && v <= 1.0; };
            if (!unit(u.top) || !unit(u.left) || !unit(u.bottom) || !unit(u.right) || !(u.top < u.bottom) ||
                !(u.left < u.right)) {
                return contract_violation("image area user crop must be ordered inside [0, 1]");
            }
        }
    }
    // Every operation marked supported is one the engine can apply: a decoded DNG opcode that
    // assess_dng_opcode accepts at its stage, evaluated in processing order (assess_in_order).
    std::vector<DngOpcodeSupport> supports;
    for (const RequiredOperation& op : m.required_operations) {
        if (!op.supported_by_lucent) {
            continue;
        }
        if (op.applied_by_decoder || op.superseded_by_warp_rectilinear2 ||
            op.kind != RequiredOperationKind::DngOpcode || !m.dng) {
            return contract_violation(std::format("operation {} is marked supported but cannot be applied", op.name));
        }
        if (supports.empty()) {
            supports = assess_in_order(m);
        }
        const auto& opcodes = m.dng->raw_ifd.opcodes;
        const auto found = std::find_if(opcodes.begin(), opcodes.end(),
                                        [&](const DngOpcode& opcode) { return matches(op, opcode); });
        if (found == opcodes.end() || !supports[static_cast<std::size_t>(found - opcodes.begin())].supported) {
            return contract_violation(std::format("OpcodeList{}[{}] {} is marked supported but Lucent cannot apply it",
                                                  static_cast<int>(list_of(op.stage)), op.index, op.name));
        }
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
