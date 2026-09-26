// Unit tests for the decoded-RAW data contract (decoded_raw.hpp, raw_decoder.hpp).

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/raw_decoder.hpp>

namespace lucent::raw {
namespace {

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

TEST(DecodedRawContract, EnumIdentifiersAreStable) {
    EXPECT_EQ(to_string(RawRepresentation::Mosaic), "mosaic");
    EXPECT_EQ(to_string(RawRepresentation::LinearColor), "linear_color");
    EXPECT_EQ(to_string(RawRepresentation::Monochrome), "monochrome");
    EXPECT_EQ(to_string(SampleEncoding::Float32), "float32");
    EXPECT_EQ(to_string(ContainerFormat::Cr3), "cr3");
    EXPECT_EQ(to_string(Provenance::DngTag), "dng_tag");
    EXPECT_EQ(to_string(MetadataStage::AfterUnpack), "after_unpack");
    EXPECT_EQ(to_string(PriorProcessingKind::WhiteBalanceApplied), "white_balance_applied");
    EXPECT_EQ(to_string(RequiredOperationKind::ProfileGainTableMap), "profile_gain_table_map");
    EXPECT_EQ(to_string(OperationStage::AfterDemosaic), "after_demosaic");
    EXPECT_EQ(to_string(InterpretationStatus::RequiresUnsupportedOperation),
              "requires_unsupported_operation");
    EXPECT_EQ(to_string(InterpretationIssueKind::DngRawImageAmbiguous), "dng_raw_image_ambiguous");
    EXPECT_EQ(to_string(InterpretationIssueKind::SubimageNotAPhotograph), "subimage_not_a_photograph");
    EXPECT_EQ(to_string(InterpretationIssueKind::SubimageFramesNotCombined), "subimage_frames_not_combined");
    EXPECT_EQ(to_string(InterpretationIssueKind::SubimageRoleUnknown), "subimage_role_unknown");
    EXPECT_EQ(to_string(InterpretationIssueKind::MultiViewSource), "multi_view_source");
    EXPECT_EQ(to_string(InterpretationIssueKind::UnrepresentedBlackLevelCorrection),
              "unrepresented_black_level_correction");
    EXPECT_EQ(to_string(Provenance::LucentRule), "lucent_rule");
    EXPECT_EQ(to_string(SubimageRole::Primary), "primary");
    EXPECT_EQ(to_string(SubimageRole::DualPixelComponent), "dual_pixel_component");
    EXPECT_EQ(to_string(SubimageRole::BurstFrame), "burst_frame");
    EXPECT_EQ(to_string(SubimageRole::PixelShiftFrame), "pixel_shift_frame");
    EXPECT_EQ(to_string(SubimageRole::Unknown), "unknown");
    EXPECT_EQ(to_string(ViewLayout::SingleView), "single_view");
    EXPECT_EQ(to_string(ViewLayout::DualFisheye360), "dual_fisheye_360");
    EXPECT_EQ(to_string(Lossiness::Unknown), "unknown");
    EXPECT_EQ(to_string(Lossiness::Lossless), "lossless");
    EXPECT_EQ(to_string(Lossiness::Lossy), "lossy");
    EXPECT_EQ(to_string(static_cast<RawRepresentation>(99)), "invalid");
    EXPECT_EQ(to_string(static_cast<SubimageRole>(99)), "invalid");
}

TEST(DecodedRawContract, PersistedEnumValuesNeverChange) {
    EXPECT_EQ(static_cast<int>(RawRepresentation::Mosaic), 1);
    EXPECT_EQ(static_cast<int>(RawRepresentation::Monochrome), 3);
    EXPECT_EQ(static_cast<int>(SampleEncoding::Float32), 2);
    EXPECT_EQ(static_cast<int>(ContainerFormat::X3f), 9);
    EXPECT_EQ(static_cast<int>(InterpretationStatus::NotDecodable), 5);
    EXPECT_EQ(static_cast<int>(InterpretationIssueKind::UnsupportedChannelLayout), 8);
    EXPECT_EQ(static_cast<int>(PriorProcessingKind::ToneCurveApplied), 6);
    EXPECT_EQ(static_cast<int>(Provenance::LucentRule), 3);
    EXPECT_EQ(static_cast<int>(SubimageRole::Primary), 1);
    EXPECT_EQ(static_cast<int>(SubimageRole::Unknown), 5);
    EXPECT_EQ(static_cast<int>(ViewLayout::SingleView), 1);
    EXPECT_EQ(static_cast<int>(ViewLayout::DualFisheye360), 2);
    EXPECT_EQ(static_cast<int>(InterpretationIssueKind::SubimageNotAPhotograph), 9);
    EXPECT_EQ(static_cast<int>(InterpretationIssueKind::UnrepresentedBlackLevelCorrection), 13);
    EXPECT_EQ(static_cast<int>(Lossiness::Unknown), 0);
    EXPECT_EQ(static_cast<int>(Lossiness::Lossy), 2);
    // 1.1 added fields (subimage role, projection, compression provenance) and 1.2 the black
    // deltas: minor changes.
    EXPECT_EQ(kDecodedRawContractVersion.major, 1u);
    EXPECT_EQ(kDecodedRawContractVersion.minor, 2u);
}

TEST(DecodedRawContract, DefaultsSayNothingIsKnown) {
    const RawMetadata m;
    EXPECT_EQ(m.format.compression.name, "unknown");
    EXPECT_EQ(m.format.compression.lossiness, Lossiness::Unknown);
    EXPECT_FALSE(m.format.compression.tiff_code);
    EXPECT_EQ(m.subimage_role, SubimageRole::Primary);
    EXPECT_EQ(m.projection.layout, ViewLayout::SingleView);
    EXPECT_TRUE(m.projection.basis.empty());
    EXPECT_EQ(SubimageInfo{}.role, SubimageRole::Unknown);
}

TEST(ContainerDetection, RecognisesSignaturesNotNames) {
    EXPECT_EQ(detect_container_format(bytes_of(std::string("II*\0\x08\0\0\0", 8))), ContainerFormat::Tiff);
    EXPECT_EQ(detect_container_format(bytes_of(std::string("MM\0*\0\0\0\x08", 8))), ContainerFormat::Tiff);
    EXPECT_EQ(detect_container_format(bytes_of(std::string("II+\0\x08\0\0\0", 8))), ContainerFormat::Tiff);
    EXPECT_EQ(detect_container_format(bytes_of(std::string("IIRO\x08\0\0\0", 8))), ContainerFormat::Orf);
    EXPECT_EQ(detect_container_format(bytes_of(std::string("IIRS\x08\0\0\0", 8))), ContainerFormat::Orf);
    EXPECT_EQ(detect_container_format(bytes_of(std::string("MMOR\0\0\0\x08", 8))), ContainerFormat::Orf);
    EXPECT_EQ(detect_container_format(bytes_of(std::string("IIU\0\x18\0\0\0", 8))), ContainerFormat::Rw2);
    EXPECT_EQ(detect_container_format(bytes_of(std::string("II\x1a\0\0\0HEAPCCDR", 14))),
              ContainerFormat::Crw);
    EXPECT_EQ(detect_container_format(bytes_of("FUJIFILMCCD-RAW 0201")), ContainerFormat::Raf);
    EXPECT_EQ(detect_container_format(bytes_of("FOVb\x02\0\x02\0")), ContainerFormat::X3f);
    EXPECT_EQ(detect_container_format(bytes_of(std::string("\0\0\0\x18"
                                                           "ftypcrx \0\0\0\x01",
                                                           16))),
              ContainerFormat::Cr3);
    // Other ISO base media files (HEIF, MP4) are not CR3.
    EXPECT_EQ(detect_container_format(bytes_of(std::string("\0\0\0\x18"
                                                           "ftypheic\0\0\0\0",
                                                           16))),
              ContainerFormat::Unknown);
    EXPECT_EQ(detect_container_format(bytes_of("\xFF\xD8\xFF\xE1 jpeg")), ContainerFormat::Unknown);
    EXPECT_EQ(detect_container_format(bytes_of("plain text file")), ContainerFormat::Unknown);
    EXPECT_EQ(detect_container_format({}), ContainerFormat::Unknown);
    EXPECT_EQ(detect_container_format(bytes_of("II")), ContainerFormat::Unknown);
    // A TIFF signature with a wrong magic number is not TIFF.
    EXPECT_EQ(detect_container_format(bytes_of(std::string("II\x2c\0\x08\0\0\0", 8))),
              ContainerFormat::Unknown);
}

TEST(SensorLevels, BlackLevelCombinesCommonChannelAndPattern) {
    SensorLevels levels;
    levels.black = 100.0;
    levels.channel_black = {1.0, 2.0, 3.0, 4.0};
    levels.pattern_rows = 2;
    levels.pattern_cols = 3;
    levels.black_pattern = {10, 20, 30, 40, 50, 60};
    EXPECT_DOUBLE_EQ(levels.black_level_at(0, 0, 0), 111.0);
    EXPECT_DOUBLE_EQ(levels.black_level_at(3, 2, 1), 100.0 + 4.0 + 60.0);
    // The pattern repeats, negative positions included.
    EXPECT_DOUBLE_EQ(levels.black_level_at(1, 3, 2), levels.black_level_at(1, 0, 0));
    EXPECT_DOUBLE_EQ(levels.black_level_at(1, -1, -1), 100.0 + 2.0 + 60.0);
    // Without a pattern only the common and channel parts apply.
    SensorLevels flat;
    flat.black = 64.0;
    flat.channel_black = {0, 1, 0, 1};
    EXPECT_DOUBLE_EQ(flat.black_level_at(1, 5, 7), 65.0);
    // The upper bound over all sites: common + largest channel part + largest pattern value.
    EXPECT_DOUBLE_EQ(levels.max_black_level(), 100.0 + 4.0 + 60.0);
    EXPECT_DOUBLE_EQ(flat.max_black_level(), 65.0);
}

TEST(SensorLevels, RowAndColumnDeltasAddAtVisiblePositionsOnly) {
    SensorLevels levels;
    levels.black = 100.0;
    levels.channel_black = {1.0, 2.0, 0.0, 0.0};
    levels.pattern_rows = 1;
    levels.pattern_cols = 2;
    levels.black_pattern = {0.0, 10.0};
    levels.column_black_delta = {0.5, -1.0, 3.0};
    levels.row_black_delta = {-0.25, 2.0};
    // DNG: BlackLevel + BlackLevelDeltaH[col] + BlackLevelDeltaV[row], plus the channel term.
    EXPECT_DOUBLE_EQ(levels.black_level_at(0, 0, 0), 100.0 + 1.0 + 0.0 + 0.5 - 0.25);
    EXPECT_DOUBLE_EQ(levels.black_level_at(1, 1, 1), 100.0 + 2.0 + 10.0 - 1.0 + 2.0);
    EXPECT_DOUBLE_EQ(levels.black_level_at(0, 2, 1), 100.0 + 1.0 + 0.0 + 3.0 + 2.0);
    // Outside the vectors (masked margins) no delta applies, but the pattern still wraps.
    EXPECT_DOUBLE_EQ(levels.black_level_at(0, -1, 5), 100.0 + 1.0 + 10.0);
    EXPECT_DOUBLE_EQ(levels.black_level_at(0, 3, -1), 100.0 + 1.0 + 10.0);
    // The upper bound adds the largest delta of each kind.
    EXPECT_DOUBLE_EQ(levels.max_black_level(), 100.0 + 2.0 + 10.0 + 3.0 + 2.0);
    // All-negative deltas lower the bound.
    levels.column_black_delta = {-1.0, -2.0, -3.0};
    levels.row_black_delta.clear();
    EXPECT_DOUBLE_EQ(levels.max_black_level(), 100.0 + 2.0 + 10.0 - 1.0);
    EXPECT_EQ(SensorLevels{}.delta_provenance, Provenance::DngTag);
}

TEST(Interpretation, StatusIsTheWorstIssue) {
    std::vector<InterpretationIssue> issues;
    EXPECT_EQ(derive_interpretation_status(issues), InterpretationStatus::Complete);
    issues.push_back({InterpretationIssueKind::OptionalOperationUnsupported, "optional"});
    EXPECT_EQ(derive_interpretation_status(issues), InterpretationStatus::OptionalOperationsSkipped);
    issues.push_back({InterpretationIssueKind::SubimageFramesNotCombined, "pixel-shift frame"});
    EXPECT_EQ(derive_interpretation_status(issues), InterpretationStatus::OptionalOperationsSkipped);
    issues.push_back({InterpretationIssueKind::MandatoryOperationUnsupported, "mandatory"});
    EXPECT_EQ(derive_interpretation_status(issues), InterpretationStatus::RequiresUnsupportedOperation);
    issues.push_back({InterpretationIssueKind::DngRawImageAmbiguous, "ambiguous"});
    EXPECT_EQ(derive_interpretation_status(issues), InterpretationStatus::Ambiguous);
    issues.push_back({InterpretationIssueKind::DecoderUnsupportedFormat, "cannot decode"});
    EXPECT_EQ(derive_interpretation_status(issues), InterpretationStatus::NotDecodable);

    EXPECT_EQ(derive_interpretation_status(std::vector<InterpretationIssue>{
                  {InterpretationIssueKind::DngVersionUnsupported, "newer DNG"}}),
              InterpretationStatus::RequiresUnsupportedOperation);
    // A frame that is not a photograph, an unstitched 360 source and black corrections the
    // contract cannot carry can never be verified; an unknown subimage is ambiguous.
    for (const InterpretationIssueKind kind :
         {InterpretationIssueKind::SubimageNotAPhotograph, InterpretationIssueKind::MultiViewSource,
          InterpretationIssueKind::UnrepresentedBlackLevelCorrection}) {
        EXPECT_EQ(derive_interpretation_status(std::vector<InterpretationIssue>{{kind, "x"}}),
                  InterpretationStatus::RequiresUnsupportedOperation)
            << to_string(kind);
    }
    EXPECT_EQ(derive_interpretation_status(
                  std::vector<InterpretationIssue>{{InterpretationIssueKind::SubimageRoleUnknown, "?"}}),
              InterpretationStatus::Ambiguous);
    // An issue kind from newer data is never treated as harmless.
    EXPECT_EQ(derive_interpretation_status(
                  std::vector<InterpretationIssue>{{static_cast<InterpretationIssueKind>(200), "future"}}),
              InterpretationStatus::Ambiguous);
}

TEST(DecodeLimits, DefaultsAreValidAndZeroIsRejected) {
    DecodeLimits limits;
    EXPECT_TRUE(limits.validate());
    EXPECT_EQ(limits.max_file_bytes, std::uint64_t{2} << 30);
    EXPECT_EQ(limits.max_pixels, std::uint64_t{1} << 28);
    EXPECT_EQ(limits.max_memory_bytes, std::uint64_t{2} << 30);
    for (int field = 0; field < 3; ++field) {
        DecodeLimits bad;
        (field == 0 ? bad.max_file_bytes : field == 1 ? bad.max_pixels : bad.max_memory_bytes) = 0;
        const Status status = bad.validate();
        ASSERT_FALSE(status);
        EXPECT_EQ(status.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(status.error().where, "raw.limits");
    }
}

// A minimal consistent decoded mosaic: 8x6 frame, visible 6x4 at (2, 1), RGGB at the visible
// origin.
DecodedRaw make_mosaic() {
    DecodedRaw d;
    RawMetadata& m = d.metadata;
    m.stage = MetadataStage::AfterUnpack;
    m.geometry.raw_width = 8;
    m.geometry.raw_height = 6;
    m.geometry.visible_area = image::PixelRect{2, 1, 6, 4};
    m.representation.kind = RawRepresentation::Mosaic;
    m.representation.color_count = 3;
    m.representation.color_description = "RGBG";
    m.representation.channel_colors = {image::CfaColor::Red, image::CfaColor::Green, image::CfaColor::Blue,
                                       image::CfaColor::Green2};
    m.representation.cfa = image::CfaPattern::bayer(image::BayerLayout::Rggb).shifted(-2, -1);
    m.levels.black = 64;
    m.levels.white = 4095;
    MosaicFrame frame;
    frame.pixels = std::move(*image::ImageBuffer<std::uint16_t>::create(8, 6, 1));
    frame.cfa = m.representation.cfa;
    d.image = std::move(frame);
    return d;
}

TEST(DecodedRawContract, ConsistentMosaicPasses) {
    const DecodedRaw d = make_mosaic();
    EXPECT_TRUE(check_consistency(d)) << check_consistency(d).error().describe();
    EXPECT_EQ(cfa_at_visible_origin(d.metadata), image::CfaPattern::bayer(image::BayerLayout::Rggb));
    const PixelsShape shape = shape_of(pixels_of(d.image));
    EXPECT_EQ(shape.width, 8);
    EXPECT_EQ(shape.height, 6);
    EXPECT_EQ(shape.channels, 1);
    EXPECT_EQ(shape.encoding, SampleEncoding::UInt16);
}

void expect_violation(const DecodedRaw& d, std::string_view fragment) {
    const Status status = check_consistency(d);
    ASSERT_FALSE(status) << "expected a violation mentioning '" << fragment << "'";
    EXPECT_EQ(status.error().code, ErrorCode::Internal);
    EXPECT_EQ(status.error().where, "raw.contract");
    EXPECT_NE(status.error().message.find(fragment), std::string::npos) << status.error().message;
}

TEST(DecodedRawContract, ViolationsAreReported) {
    {
        DecodedRaw d = make_mosaic();
        d.metadata.geometry.visible_area = image::PixelRect{4, 1, 6, 4};  // extends to x = 10 > 8
        expect_violation(d, "visible area");
    }
    {
        DecodedRaw d = make_mosaic();
        d.metadata.geometry.raw_width = 9;  // buffer is 8 wide
        expect_violation(d, "buffer is 8x6");
    }
    {
        DecodedRaw d = make_mosaic();
        d.metadata.representation.kind = RawRepresentation::Monochrome;
        expect_violation(d, "image alternative");
    }
    {
        DecodedRaw d = make_mosaic();
        d.metadata.representation.sample_encoding = SampleEncoding::Float32;
        expect_violation(d, "float32");
    }
    {
        DecodedRaw d = make_mosaic();
        std::get<MosaicFrame>(d.image).cfa = image::CfaPattern::bayer(image::BayerLayout::Bggr);
        expect_violation(d, "CFA pattern");
    }
    {
        DecodedRaw d = make_mosaic();
        d.metadata.representation.channel_colors = {image::CfaColor::Red, image::CfaColor::Green,
                                                    image::CfaColor::Blue};  // no Green2
        expect_violation(d, "no channel index");
    }
    {
        DecodedRaw d = make_mosaic();
        d.metadata.levels.white = 60;  // below black 64
        expect_violation(d, "white level");
    }
    {
        DecodedRaw d = make_mosaic();
        d.metadata.levels.channel_black = {0, 0, 5000, 0};  // total black above white on blue
        expect_violation(d, "white level");
    }
    {
        DecodedRaw d = make_mosaic();
        d.metadata.levels.pattern_rows = 2;
        d.metadata.levels.pattern_cols = 2;
        d.metadata.levels.black_pattern = {1, 2, 3};
        expect_violation(d, "pattern size");
    }
    {
        DecodedRaw d = make_mosaic();
        d.metadata.levels.column_black_delta = std::vector<double>(5, 1.0);  // visible area is 6 wide
        expect_violation(d, "column black deltas");
        d.metadata.levels.column_black_delta = std::vector<double>(6, 1.0);
        EXPECT_TRUE(check_consistency(d)) << check_consistency(d).error().describe();
        d.metadata.levels.row_black_delta = std::vector<double>(6, 1.0);  // visible area is 4 high
        expect_violation(d, "row black deltas");
        d.metadata.levels.row_black_delta = {1.0, 1.0, std::numeric_limits<double>::quiet_NaN(), 1.0};
        expect_violation(d, "finite");
        d.metadata.levels.row_black_delta = {0.0, 0.0, 4100.0, 0.0};  // black above white on row 2
        expect_violation(d, "white level");
    }
    {
        DecodedRaw d = make_mosaic();
        d.metadata.stage = MetadataStage::AfterOpen;
        expect_violation(d, "after_unpack");
    }
    {
        DecodedRaw d = make_mosaic();
        d.metadata.subimage_index = 1;  // count is 1
        expect_violation(d, "subimage");
    }
    {
        DecodedRaw d = make_mosaic();
        d.metadata.interpretation.issues.push_back(
            {InterpretationIssueKind::MandatoryOperationUnsupported, "status not updated"});
        expect_violation(d, "interpretation status");
    }
    {
        DecodedRaw d = make_mosaic();
        d.metadata.subimage_role = SubimageRole::DualPixelComponent;  // but a single-image file
        expect_violation(d, "single-image file");
    }
    {
        DecodedRaw d = make_mosaic();
        d.metadata.subimage_count = 2;
        d.metadata.subimage_index = 1;
        d.metadata.subimage_role = SubimageRole::DualPixelComponent;  // without its issue
        expect_violation(d, "needs its interpretation issue");
        d.metadata.interpretation.issues.push_back(
            {InterpretationIssueKind::SubimageNotAPhotograph, "frame"});
        d.metadata.interpretation.status = InterpretationStatus::RequiresUnsupportedOperation;
        EXPECT_TRUE(check_consistency(d)) << check_consistency(d).error().describe();
    }
    {
        DecodedRaw d = make_mosaic();
        d.metadata.projection.layout = ViewLayout::DualFisheye360;  // without its issue
        expect_violation(d, "multi_view_source");
    }
    {
        DecodedRaw d = make_mosaic();
        d.contract = core::ContractVersion{2, 0};
        const Status status = check_consistency(d);
        ASSERT_FALSE(status);
        EXPECT_EQ(status.error().code, ErrorCode::VersionMismatch);
    }
}

TEST(DecodedRawContract, LinearColorNeedsThreeChannels) {
    DecodedRaw d;
    RawMetadata& m = d.metadata;
    m.stage = MetadataStage::AfterUnpack;
    m.geometry.raw_width = 4;
    m.geometry.raw_height = 4;
    m.geometry.visible_area = image::PixelRect{0, 0, 4, 4};
    m.representation.kind = RawRepresentation::LinearColor;
    m.representation.channels = 3;
    m.representation.color_count = 3;
    m.representation.color_description = "RGBG";
    m.representation.channel_colors = {image::CfaColor::Red, image::CfaColor::Green, image::CfaColor::Blue};
    m.levels.white = 1000;
    LinearColorImage linear;
    linear.pixels = std::move(*image::ImageBuffer<std::uint16_t>::create(4, 4, 3));
    d.image = std::move(linear);
    EXPECT_TRUE(check_consistency(d)) << check_consistency(d).error().describe();

    m.representation.cfa = image::CfaPattern::bayer(image::BayerLayout::Rggb);
    expect_violation(d, "only mosaic");
    m.representation.cfa = {};

    std::get<LinearColorImage>(d.image).pixels =
        std::move(*image::ImageBuffer<std::uint16_t>::create(4, 4, 1));
    expect_violation(d, "3 channel");
}

TEST(DecodedRawContract, MonochromeEncodingIsSingleChannelCameraNative) {
    const image::ColorEncoding e = camera_native_monochrome();
    EXPECT_EQ(e.layout, image::ChannelLayout::Mono);
    EXPECT_EQ(e.primaries, image::ColorPrimaries::CameraNative);
    EXPECT_EQ(e.transfer, image::TransferFunction::Linear);
    EXPECT_TRUE(e.validate());
    EXPECT_EQ(MonochromeFrame{}.encoding, e);
    EXPECT_EQ(MosaicFrame{}.encoding, image::ColorEncoding::camera_native_mosaic());
}

TEST(DecodedRawContract, PriorProcessingLookup) {
    RawMetadata m;
    EXPECT_FALSE(m.has_prior_processing(PriorProcessingKind::Demosaiced));
    m.prior_processing.push_back({PriorProcessingKind::Demosaiced, "linear DNG"});
    EXPECT_TRUE(m.has_prior_processing(PriorProcessingKind::Demosaiced));
    EXPECT_FALSE(m.has_prior_processing(PriorProcessingKind::WhiteBalanceApplied));
}

}  // namespace
}  // namespace lucent::raw
