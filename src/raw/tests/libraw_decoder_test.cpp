// End-to-end tests of the LibRaw adapter on synthetic DNG files (no downloaded fixtures).

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/decoder_registry.hpp>
#include <lucent/raw/libraw_decoder.hpp>

#include "raw_test_support.hpp"
#include "synthetic_dng.hpp"

namespace lucent::raw {
namespace {

namespace ts = test_support;
using ts::ScratchDir;
using ts::SyntheticDngSpec;
using ts::SyntheticKind;

class LibRawAdapter : public ::testing::Test {
protected:
    void SetUp() override {
        auto decoder = create_libraw_decoder();
        ASSERT_TRUE(decoder) << decoder.error().describe();
        decoder_ = std::move(*decoder);
    }

    std::filesystem::path write_dng(const SyntheticDngSpec& spec,
                                    const std::wstring& name = L"synthetic.dng") {
        return dir_.write(name, ts::build_synthetic_dng(spec));
    }

    DecodedRaw decode_ok(const std::filesystem::path& path, DecodeOptions options = {}) {
        auto decoded = decoder_->decode(path, options, {});
        EXPECT_TRUE(decoded) << decoded.error().describe();
        if (!decoded) {
            return {};
        }
        EXPECT_TRUE(check_consistency(*decoded));
        return std::move(*decoded);
    }

    ProbeResult probe_ok(const std::filesystem::path& path, ProbeOptions options = {}) {
        auto probed = decoder_->probe(path, options, {});
        EXPECT_TRUE(probed) << probed.error().describe();
        return probed ? std::move(*probed) : ProbeResult{};
    }

    Error decode_error(const std::filesystem::path& path, DecodeOptions options = {},
                       core::CancellationToken cancel = {}) {
        auto decoded = decoder_->decode(path, options, cancel);
        EXPECT_FALSE(decoded) << "decode unexpectedly succeeded";
        return decoded ? Error{} : decoded.error();
    }

    Error probe_error(const std::filesystem::path& path, ProbeOptions options = {},
                      core::CancellationToken cancel = {}) {
        auto probed = decoder_->probe(path, options, cancel);
        EXPECT_FALSE(probed) << "probe unexpectedly succeeded";
        return probed ? Error{} : probed.error();
    }

    std::unique_ptr<RawDecoder> decoder_;
    ScratchDir dir_;
};

// Every decoded sample of single-channel data equals the stored value of raw image `image`.
void expect_mosaic_samples(const RawPixels& pixels, std::uint32_t width, std::uint32_t height,
                           std::uint32_t image = 0) {
    const auto* buffer = std::get_if<image::ImageBuffer<std::uint16_t>>(&pixels);
    ASSERT_NE(buffer, nullptr);
    ASSERT_EQ(buffer->width(), static_cast<std::int32_t>(width));
    ASSERT_EQ(buffer->height(), static_cast<std::int32_t>(height));
    ASSERT_EQ(buffer->channels(), 1);
    std::size_t mismatches = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            if (buffer->at(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 0) !=
                ts::expected_sample(x, y, 0, image)) {
                ++mismatches;
            }
        }
    }
    EXPECT_EQ(mismatches, 0u);
}

TEST_F(LibRawAdapter, DecodesSyntheticCfaDngExactly) {
    for (const ts::Endian endian : {ts::Endian::Little, ts::Endian::Big}) {
        SCOPED_TRACE(endian == ts::Endian::Little ? "little-endian" : "big-endian");
        SyntheticDngSpec spec;
        spec.endian = endian;
        const auto path = write_dng(spec);
        const DecodedRaw decoded = decode_ok(path);
        const RawMetadata& m = decoded.metadata;

        EXPECT_EQ(decoded.contract, kDecodedRawContractVersion);
        EXPECT_EQ(m.stage, MetadataStage::AfterUnpack);
        EXPECT_EQ(m.source.path, path);
        EXPECT_EQ(m.source.size_bytes, std::filesystem::file_size(path));
        EXPECT_EQ(m.source.identity.size_bytes, m.source.size_bytes);
        EXPECT_EQ(m.format.container, ContainerFormat::Dng);
        EXPECT_EQ(m.format.extension_hint, "dng");
        ASSERT_TRUE(m.format.dng_version);
        EXPECT_EQ(m.format.dng_version->to_string(), "1.4.0.0");
        EXPECT_EQ(m.format.compression.tiff_code, std::optional<std::uint16_t>(std::uint16_t{1}));
        EXPECT_EQ(m.format.compression.name, "Uncompressed");
        EXPECT_EQ(m.format.compression.lossiness, Lossiness::Lossless);
        EXPECT_EQ(m.format.compression.provenance, Provenance::DngTag);
        EXPECT_EQ(m.format.compression.basis,
                  "DNG raw IFD ifd0 Compression 1, PhotometricInterpretation 32803");
        EXPECT_EQ(m.subimage_index, 0u);
        EXPECT_EQ(m.subimage_count, 1u);
        EXPECT_EQ(m.subimage_role, SubimageRole::Primary);
        EXPECT_EQ(m.projection.layout, ViewLayout::SingleView);
        EXPECT_FALSE(m.noise);
        EXPECT_EQ(m.camera.make, "Lucent");
        EXPECT_EQ(m.camera.model, "Synthetic DNG");
        EXPECT_EQ(m.camera.unique_camera_model, "Lucent Synthetic DNG");

        EXPECT_EQ(m.geometry.raw_width, 64u);
        EXPECT_EQ(m.geometry.raw_height, 48u);
        EXPECT_EQ(m.geometry.visible_area, (image::PixelRect{0, 0, 64, 48}));
        EXPECT_EQ(m.geometry.orientation, image::Orientation::Normal);
        EXPECT_DOUBLE_EQ(m.geometry.pixel_aspect, 1.0);

        EXPECT_EQ(m.representation.kind, RawRepresentation::Mosaic);
        EXPECT_EQ(m.representation.sample_encoding, SampleEncoding::UInt16);
        EXPECT_EQ(m.representation.channels, 1);
        EXPECT_EQ(m.representation.color_count, 3);
        EXPECT_EQ(m.representation.color_description, "RGBG");
        EXPECT_EQ(m.representation.channel_colors,
                  (std::vector<image::CfaColor>{image::CfaColor::Red, image::CfaColor::Green,
                                                image::CfaColor::Blue, image::CfaColor::Green2}));
        EXPECT_EQ(m.representation.cfa, image::CfaPattern::bayer(image::BayerLayout::Rggb));
        EXPECT_EQ(cfa_at_visible_origin(m), image::CfaPattern::bayer(image::BayerLayout::Rggb));

        // Levels exactly as stored: BlackLevel 256, WhiteLevel 16383, nothing subtracted.
        EXPECT_EQ(m.levels.provenance, Provenance::Decoder);
        for (std::size_t c = 0; c < 4; ++c) {
            EXPECT_DOUBLE_EQ(m.levels.black_level_at(c, 3, 5), 256.0) << "channel " << c;
        }
        EXPECT_DOUBLE_EQ(m.levels.white, 16383.0);

        EXPECT_EQ(m.interpretation.status, InterpretationStatus::Complete);
        EXPECT_TRUE(m.required_operations.empty());
        EXPECT_TRUE(m.prior_processing.empty());
        ASSERT_TRUE(m.dng);
        EXPECT_EQ(m.dng->raw_ifd.path, "ifd0");
        EXPECT_EQ(m.dng->info.color.as_shot_neutral, (std::vector<double>{0.5, 1.0, 2.0 / 3.0}));
        EXPECT_EQ(m.decoder.adapter_id, "libraw");
        EXPECT_EQ(m.decoder.library, "LibRaw");
        EXPECT_FALSE(m.decoder.unpacker.empty());

        ASSERT_TRUE(std::holds_alternative<MosaicFrame>(decoded.image));
        const auto& frame = std::get<MosaicFrame>(decoded.image);
        EXPECT_EQ(frame.cfa, m.representation.cfa);
        EXPECT_EQ(frame.encoding, image::ColorEncoding::camera_native_mosaic());
        expect_mosaic_samples(frame.pixels, 64, 48);

        ASSERT_TRUE(m.decoder.reference_parity);
        const ReferenceParity& p = *m.decoder.reference_parity;
        EXPECT_EQ(p.raw_type, "flat");
        EXPECT_EQ(p.raw_pattern, (std::vector<std::vector<std::int32_t>>{{0, 1}, {3, 2}}));
        EXPECT_EQ(p.black_level_per_channel, (std::array<std::uint32_t, 4>{256, 256, 256, 256}));
        EXPECT_EQ(p.white_level, 16383u);
        EXPECT_EQ(p.raw_width, 64);
        EXPECT_EQ(p.iwidth, 64);
        EXPECT_EQ(p.flip, 0);
    }
}

TEST_F(LibRawAdapter, ProbeReportsMetadataWithoutUnpacking) {
    const auto path = write_dng(SyntheticDngSpec{});
    const ProbeResult probe = probe_ok(path);
    const RawMetadata& m = probe.metadata;
    EXPECT_EQ(probe.contract, kDecodedRawContractVersion);
    EXPECT_EQ(m.stage, MetadataStage::AfterOpen);
    EXPECT_EQ(m.subimage_count, 1u);
    ASSERT_EQ(probe.subimages.size(), 1u);
    EXPECT_EQ(probe.subimages[0].index, 0u);
    EXPECT_FALSE(probe.subimages[0].error);
    EXPECT_EQ(probe.subimages[0].raw_width, 64u);
    EXPECT_EQ(probe.subimages[0].representation, RawRepresentation::Mosaic);

    const DecodedRaw decoded = decode_ok(path);
    EXPECT_EQ(m.geometry.raw_width, decoded.metadata.geometry.raw_width);
    EXPECT_EQ(m.geometry.visible_area, decoded.metadata.geometry.visible_area);
    EXPECT_EQ(m.representation.cfa, decoded.metadata.representation.cfa);
    EXPECT_EQ(m.representation.kind, decoded.metadata.representation.kind);
    ASSERT_TRUE(m.decoder.reference_parity);
    EXPECT_EQ(m.decoder.reference_parity->raw_type, "flat");  // predicted before unpacking

    ProbeOptions quiet;
    quiet.describe_subimages = false;
    EXPECT_TRUE(probe_ok(path, quiet).subimages.empty());
}

TEST_F(LibRawAdapter, ActiveAreaDefinesVisibleAreaAndCfaOrigin) {
    SyntheticDngSpec spec;
    spec.active_area = std::array<std::uint32_t, 4>{3, 5, 45, 61};  // top, left, bottom, right
    const auto path = write_dng(spec);
    const DecodedRaw decoded = decode_ok(path);
    const RawMetadata& m = decoded.metadata;
    EXPECT_EQ(m.geometry.raw_width, 64u);
    EXPECT_EQ(m.geometry.raw_height, 48u);
    // LibRaw moves odd ActiveArea margins to the next even row/column (keeping the 2x2 phase of
    // its filter word), so the visible area starts at (6, 4) and is one pixel smaller each way.
    EXPECT_EQ(m.geometry.visible_area, (image::PixelRect{6, 4, 55, 41}));
    ASSERT_TRUE(m.dng);
    ASSERT_TRUE(m.dng->raw_ifd.active_area);
    EXPECT_EQ(m.dng->raw_ifd.active_area->top, 3u);  // the file's own value stays available
    // DNG CFAPattern is relative to the ActiveArea origin (5, 3): red there, so blue at the
    // raw origin and at the visible origin (6, 4).
    EXPECT_EQ(m.representation.cfa.color_at(3, 5), image::CfaColor::Red);
    EXPECT_EQ(m.representation.cfa.color_at(0, 0), image::CfaColor::Blue);
    EXPECT_EQ(cfa_at_visible_origin(m), image::CfaPattern::bayer(image::BayerLayout::Bggr));
    // The whole frame, margins included, is delivered unchanged.
    expect_mosaic_samples(std::get<MosaicFrame>(decoded.image).pixels, 64, 48);
    ASSERT_TRUE(m.decoder.reference_parity);
    EXPECT_EQ(m.decoder.reference_parity->top_margin, 4);
    EXPECT_EQ(m.decoder.reference_parity->left_margin, 6);
    // rawpy's raw_pattern is anchored at the raw origin: (0, 0) is blue (index 2).
    EXPECT_EQ(m.decoder.reference_parity->raw_pattern,
              (std::vector<std::vector<std::int32_t>>{{2, 3}, {1, 0}}));
}

TEST_F(LibRawAdapter, EveryBayerLayoutKeepsBothGreens) {
    struct Case {
        std::array<std::uint8_t, 4> dng;
        image::BayerLayout layout;
    };
    const Case cases[] = {{{0, 1, 1, 2}, image::BayerLayout::Rggb},
                          {{2, 1, 1, 0}, image::BayerLayout::Bggr},
                          {{1, 0, 2, 1}, image::BayerLayout::Grbg},
                          {{1, 2, 0, 1}, image::BayerLayout::Gbrg}};
    for (const Case& c : cases) {
        SCOPED_TRACE(std::string(image::to_string(c.layout)));
        SyntheticDngSpec spec;
        spec.cfa_pattern = c.dng;
        const DecodedRaw decoded = decode_ok(write_dng(spec));
        const image::CfaPattern cfa = cfa_at_visible_origin(decoded.metadata);
        EXPECT_EQ(cfa, image::CfaPattern::bayer(c.layout));
        EXPECT_EQ(cfa.bayer_layout(), c.layout);
    }
}

TEST_F(LibRawAdapter, OrientationComesFromTheFile) {
    struct Case {
        std::uint16_t exif;
        image::Orientation expected;
    };
    const Case cases[] = {
        {1, image::Orientation::Normal},
        {3, image::Orientation::Rotate180},
        {6, image::Orientation::Rotate90Cw},
        {8, image::Orientation::Rotate90Ccw},
    };
    for (const auto& [exif, expected] : cases) {
        SyntheticDngSpec spec;
        spec.orientation = exif;
        const ProbeResult probe = probe_ok(write_dng(spec));
        EXPECT_EQ(probe.metadata.geometry.orientation, expected) << "EXIF orientation " << exif;
    }
}

TEST_F(LibRawAdapter, LinearDngIsFullColourData) {
    SyntheticDngSpec spec;
    spec.kind = SyntheticKind::LinearRgb;
    const DecodedRaw decoded = decode_ok(write_dng(spec));
    const RawMetadata& m = decoded.metadata;
    EXPECT_EQ(m.representation.kind, RawRepresentation::LinearColor);
    EXPECT_EQ(m.representation.channels, 3);
    EXPECT_TRUE(m.representation.cfa.empty());
    EXPECT_TRUE(m.has_prior_processing(PriorProcessingKind::Demosaiced));
    ASSERT_TRUE(std::holds_alternative<LinearColorImage>(decoded.image));
    const auto& image = std::get<LinearColorImage>(decoded.image);
    EXPECT_EQ(image.encoding, image::ColorEncoding::camera_native_linear());
    const auto* buffer = std::get_if<image::ImageBuffer<std::uint16_t>>(&image.pixels);
    ASSERT_NE(buffer, nullptr);
    ASSERT_EQ(buffer->channels(), 3);
    std::size_t mismatches = 0;
    for (std::uint32_t y = 0; y < 48; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            for (std::uint32_t c = 0; c < 3; ++c) {
                mismatches += buffer->at(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                                         static_cast<std::int32_t>(c)) != ts::expected_sample(x, y, c);
            }
        }
    }
    EXPECT_EQ(mismatches, 0u);
    ASSERT_TRUE(m.decoder.reference_parity);
    EXPECT_EQ(m.decoder.reference_parity->raw_type, "stack");
    EXPECT_FALSE(m.decoder.reference_parity->raw_pattern);
}

TEST_F(LibRawAdapter, MonochromeDngIsSingleChannel) {
    SyntheticDngSpec spec;
    spec.kind = SyntheticKind::Mono;
    const DecodedRaw decoded = decode_ok(write_dng(spec));
    const RawMetadata& m = decoded.metadata;
    EXPECT_EQ(m.representation.kind, RawRepresentation::Monochrome);
    EXPECT_EQ(m.representation.color_count, 1);
    EXPECT_EQ(m.representation.channel_colors, std::vector<image::CfaColor>{image::CfaColor::White});
    ASSERT_TRUE(std::holds_alternative<MonochromeFrame>(decoded.image));
    expect_mosaic_samples(std::get<MonochromeFrame>(decoded.image).pixels, 64, 48);
    ASSERT_TRUE(m.decoder.reference_parity);
    EXPECT_EQ(m.decoder.reference_parity->raw_pattern, (std::vector<std::vector<std::int32_t>>{{6}}));
}

TEST_F(LibRawAdapter, FloatingPointDngKeepsFloatSamples) {
    SyntheticDngSpec spec;
    spec.float_samples = true;
    const DecodedRaw decoded = decode_ok(write_dng(spec));
    const RawMetadata& m = decoded.metadata;
    EXPECT_EQ(m.representation.kind, RawRepresentation::Mosaic);
    EXPECT_EQ(m.representation.sample_encoding, SampleEncoding::Float32);
    EXPECT_EQ(m.levels.provenance, Provenance::DngTag);
    EXPECT_DOUBLE_EQ(m.levels.white, 1.0);
    EXPECT_DOUBLE_EQ(m.levels.black_level_at(0, 0, 0), 0.0);
    const auto* buffer = std::get_if<image::ImageBuffer<float>>(&std::get<MosaicFrame>(decoded.image).pixels);
    ASSERT_NE(buffer, nullptr);
    std::size_t mismatches = 0;
    for (std::uint32_t y = 0; y < 48; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            // Exact: LibRaw copies 32-bit floats without conversion when rescaling is disabled.
            mismatches += buffer->at(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 0) !=
                          ts::expected_float_sample(x, y, 0);
        }
    }
    EXPECT_EQ(mismatches, 0u);
}

TEST_F(LibRawAdapter, MandatoryDngOpcodeRequiresUnsupportedOperation) {
    SyntheticDngSpec spec;
    spec.opcode_list3 = ts::opcode_list({{9, 0x01030000, 0, ts::filler(76)}});  // GainMap, mandatory
    spec.opcode_list1 = ts::opcode_list({{4, 0x01030000, 1, ts::filler(8)}});   // optional
    const auto path = write_dng(spec);
    const ProbeResult probe = probe_ok(path);
    const RawMetadata& m = probe.metadata;
    EXPECT_EQ(m.interpretation.status, InterpretationStatus::RequiresUnsupportedOperation);
    ASSERT_EQ(m.required_operations.size(), 2u);
    const RequiredOperation& list1 = m.required_operations[0];
    EXPECT_EQ(list1.kind, RequiredOperationKind::DngOpcode);
    EXPECT_EQ(list1.stage, OperationStage::StoredRaw);
    EXPECT_EQ(list1.name, "FixBadPixelsConstant");
    EXPECT_FALSE(list1.mandatory);
    const RequiredOperation& gain = m.required_operations[1];
    EXPECT_EQ(gain.stage, OperationStage::AfterDemosaic);
    EXPECT_EQ(gain.name, "GainMap");
    EXPECT_EQ(gain.opcode_id, 9u);
    EXPECT_EQ(gain.min_dng_version.to_string(), "1.3.0.0");
    EXPECT_TRUE(gain.mandatory);
    EXPECT_FALSE(gain.supported_by_lucent);
    EXPECT_FALSE(gain.applied_by_decoder);
    const auto count_kind = [&](InterpretationIssueKind kind) {
        return std::count_if(m.interpretation.issues.begin(), m.interpretation.issues.end(),
                             [kind](const InterpretationIssue& i) { return i.kind == kind; });
    };
    EXPECT_EQ(count_kind(InterpretationIssueKind::MandatoryOperationUnsupported), 1);
    EXPECT_EQ(count_kind(InterpretationIssueKind::OptionalOperationUnsupported), 1);

    // The raw data is still decodable; the status travels with it.
    const DecodedRaw decoded = decode_ok(path);
    EXPECT_EQ(decoded.metadata.interpretation.status, InterpretationStatus::RequiresUnsupportedOperation);
    expect_mosaic_samples(std::get<MosaicFrame>(decoded.image).pixels, 64, 48);
}

TEST_F(LibRawAdapter, OptionalOpcodesOnlySkipOptionalOperations) {
    SyntheticDngSpec spec;
    spec.opcode_list2 = ts::opcode_list({{8, 0x01030000, 3, ts::filler(36)}});  // MapPolynomial, optional
    const ProbeResult probe = probe_ok(write_dng(spec));
    EXPECT_EQ(probe.metadata.interpretation.status, InterpretationStatus::OptionalOperationsSkipped);
    ASSERT_EQ(probe.metadata.required_operations.size(), 1u);
    EXPECT_EQ(probe.metadata.required_operations[0].stage, OperationStage::LinearRaw);
    EXPECT_TRUE(probe.metadata.required_operations[0].preview_skippable);
}

TEST_F(LibRawAdapter, NewerDngBackwardVersionIsUnsupported) {
    SyntheticDngSpec spec;
    spec.dng_version = {1, 9, 0, 0};
    spec.backward_version = std::array<std::uint8_t, 4>{1, 8, 0, 0};
    const ProbeResult probe = probe_ok(write_dng(spec));
    EXPECT_EQ(probe.metadata.interpretation.status, InterpretationStatus::RequiresUnsupportedOperation);
    ASSERT_FALSE(probe.metadata.interpretation.issues.empty());
    EXPECT_EQ(probe.metadata.interpretation.issues[0].kind, InterpretationIssueKind::DngVersionUnsupported);
}

TEST_F(LibRawAdapter, FileSizeLimitIsEnforcedBeforeOpening) {
    const auto path = write_dng(SyntheticDngSpec{});
    const auto size = std::filesystem::file_size(path);
    DecodeOptions options;
    options.limits.max_file_bytes = size - 1;
    const Error decode = decode_error(path, options);
    EXPECT_EQ(decode.code, ErrorCode::LimitExceeded);
    EXPECT_EQ(decode.where, "raw.libraw.limits");
    ProbeOptions probe_options;
    probe_options.limits.max_file_bytes = size - 1;
    EXPECT_EQ(probe_error(path, probe_options).code, ErrorCode::LimitExceeded);
    options.limits.max_file_bytes = size;
    decode_ok(path, options);
}

TEST_F(LibRawAdapter, PixelAndMemoryLimitsAreEnforcedBeforeUnpacking) {
    const auto path = write_dng(SyntheticDngSpec{});
    DecodeOptions options;
    options.limits.max_pixels = 64 * 48 - 1;
    Error error = decode_error(path, options);
    EXPECT_EQ(error.code, ErrorCode::LimitExceeded);
    EXPECT_EQ(error.where, "raw.libraw.limits");
    EXPECT_NE(error.message.find("pixels"), std::string::npos) << error.message;

    // A probe reports the dimensions instead of refusing them.
    ProbeOptions probe_options;
    probe_options.limits.max_pixels = 1;
    EXPECT_EQ(probe_ok(path, probe_options).metadata.geometry.raw_width, 64u);

    options = {};
    options.limits.max_memory_bytes = 1024;  // far below 64 x 48 x 2 bytes, twice
    error = decode_error(path, options);
    EXPECT_EQ(error.code, ErrorCode::LimitExceeded);
    EXPECT_EQ(error.where, "raw.libraw.limits");
    EXPECT_NE(error.message.find("memory"), std::string::npos) << error.message;

    options.limits = DecodeLimits{};
    options.limits.max_pixels = 0;
    EXPECT_EQ(decode_error(path, options).code, ErrorCode::InvalidArgument);
}

TEST_F(LibRawAdapter, WhiteLevelBelowBlackIsCorrupt) {
    // WhiteLevel 200 below BlackLevel 256: the data cannot be normalised. That is a property of
    // the file (Corrupt), not a Lucent defect (the contract check's Internal).
    SyntheticDngSpec spec;
    spec.white_level = 200;
    const auto path = write_dng(spec);
    const Error error = decode_error(path);
    EXPECT_EQ(error.code, ErrorCode::Corrupt) << error.describe();
    EXPECT_EQ(error.where, "raw.libraw.levels") << error.describe();
    // A probe still reports what the file says.
    const ProbeResult probe = probe_ok(path);
    EXPECT_DOUBLE_EQ(probe.metadata.levels.white, 200.0);
    EXPECT_DOUBLE_EQ(probe.metadata.levels.max_black_level(), 256.0);
}

TEST_F(LibRawAdapter, PreCancelledTokenStopsBeforeReading) {
    const auto path = write_dng(SyntheticDngSpec{});
    core::CancellationSource source;
    source.cancel();
    EXPECT_EQ(decode_error(path, {}, source.token()).code, ErrorCode::Cancelled);
    EXPECT_EQ(probe_error(path, {}, source.token()).code, ErrorCode::Cancelled);
    // Even a missing file reports cancellation first: nothing is touched.
    EXPECT_EQ(decode_error(dir_.path() / L"missing.dng", {}, source.token()).code, ErrorCode::Cancelled);
}

TEST_F(LibRawAdapter, NonRawInputsFailCleanly) {
    const Error text =
        decode_error(dir_.write_text(L"notes.txt", "This is not a RAW file, just some text.\n"));
    EXPECT_EQ(text.code, ErrorCode::Unsupported) << text.describe();
    EXPECT_EQ(text.where, "raw.libraw.open");
    EXPECT_NE(text.native_code, 0);  // LibRaw's return code is kept for diagnostics

    const Error empty = probe_error(dir_.write(L"empty.dng", {}));
    EXPECT_EQ(empty.code, ErrorCode::Unsupported);

    const Error missing = probe_error(dir_.path() / L"does-not-exist.dng");
    EXPECT_EQ(missing.code, ErrorCode::NotFound);

    const Error directory = probe_error(dir_.path());
    EXPECT_EQ(directory.code, ErrorCode::InvalidArgument);

    // A JPEG header and random bytes are not RAW data.
    std::vector<std::byte> jpeg(4096, std::byte{0x5A});
    jpeg[0] = std::byte{0xFF};
    jpeg[1] = std::byte{0xD8};
    jpeg[2] = std::byte{0xFF};
    jpeg[3] = std::byte{0xE0};
    const Error not_raw = decode_error(dir_.write(L"photo.jpg", jpeg));
    EXPECT_EQ(not_raw.code, ErrorCode::Unsupported) << not_raw.describe();
    EXPECT_EQ(not_raw.where, "raw.libraw.open") << not_raw.describe();
}

TEST_F(LibRawAdapter, TruncatedDngFailsWithoutCrashing) {
    const auto bytes = ts::build_synthetic_dng(SyntheticDngSpec{});
    // 8: header only; 200: cut inside IFD 0; then cuts inside the pixel data. Whatever LibRaw
    // answers (it calls some of these "unsupported"), a truncated DNG is Corrupt, never
    // "not a format this adapter understands", and the registry does not try other adapters.
    auto registry = create_default_registry();
    ASSERT_TRUE(registry);
    for (const std::size_t length : {std::size_t{8}, std::size_t{200}, bytes.size() / 2, bytes.size() - 1}) {
        SCOPED_TRACE(length);
        const auto path = dir_.write(L"truncated.dng", std::span<const std::byte>(bytes.data(), length));
        auto decoded = decoder_->decode(path, {}, {});
        ASSERT_FALSE(decoded) << "decoding a file truncated to " << length << " bytes succeeded";
        EXPECT_EQ(decoded.error().code, ErrorCode::Corrupt) << decoded.error().describe();
        EXPECT_EQ(decoded.error().where, "raw.libraw.truncated") << decoded.error().describe();
        auto probed = decoder_->probe(path, {}, {});
        ASSERT_FALSE(probed);
        EXPECT_EQ(probed.error().code, ErrorCode::Corrupt) << probed.error().describe();
        auto selected = registry->select(path, {}, {});
        ASSERT_FALSE(selected);
        EXPECT_EQ(selected.error().code, ErrorCode::Corrupt) << selected.error().describe();
    }
}

TEST_F(LibRawAdapter, DngWithIfdsAfterTheImageDataCutBeforeIfdZeroIsCorrupt) {
    // Review finding ROBU-4: cameras such as the Insta360 One X write IFD 0 after the pixel data;
    // a copy cut before it must be reported as truncated, not as an unsupported format.
    ts::Built patch;
    patch.bytes = ts::build_synthetic_dng(SyntheticDngSpec{});
    const std::uint64_t beyond = patch.bytes.size() + 4096;
    patch.put(4, beyond, 4);  // the header now points past the end of the file
    const auto path = dir_.write(L"ifd-after-data.dng", patch.bytes);
    const Error error = decode_error(path);
    EXPECT_EQ(error.code, ErrorCode::Corrupt) << error.describe();
    EXPECT_EQ(error.where, "raw.libraw.truncated") << error.describe();
    EXPECT_NE(error.message.find("ifd0"), std::string::npos) << error.message;
}

TEST_F(LibRawAdapter, DamagedVendorContainersAreCorruptNotUnsupported) {
    // Only RAW files carry the CR3 and RAF signatures, so one LibRaw cannot open because its own
    // structure is broken is a damaged RAW file (review finding ROBU-4), not an unknown format.
    std::vector<std::byte> cr3(64, std::byte{0});
    const std::string_view ftyp(
        "\0\0\0\x18"
        "ftypcrx \0\0\0\x01"
        "crx isom"
        "\0\0\0\x04"
        "moov",
        32);
    std::memcpy(cr3.data(), ftyp.data(), ftyp.size());  // "moov" declares 4 bytes: less than its header
    const Error damaged = decode_error(dir_.write(L"damaged.cr3", cr3));
    EXPECT_EQ(damaged.code, ErrorCode::Corrupt) << damaged.describe();
    EXPECT_EQ(damaged.where, "raw.libraw.container") << damaged.describe();

    std::vector<std::byte> raf(40, std::byte{0x20});
    std::memcpy(raf.data(), "FUJIFILMCCD-RAW 0201", 20);  // the 108-byte RAF header is cut off
    const Error cut = decode_error(dir_.write(L"cut.raf", raf));
    EXPECT_EQ(cut.code, ErrorCode::Corrupt) << cut.describe();
}

TEST_F(LibRawAdapter, FloatingPointDngWithoutWhiteLevelUsesTheDngDefault) {
    // DNG 1.7.1.0, WhiteLevel: "1.0 for floating point images" (review finding NUME-5).
    SyntheticDngSpec spec;
    spec.float_samples = true;
    spec.write_white_level = false;
    const DecodedRaw decoded = decode_ok(write_dng(spec));
    const RawMetadata& m = decoded.metadata;
    EXPECT_EQ(m.representation.sample_encoding, SampleEncoding::Float32);
    EXPECT_EQ(m.levels.provenance, Provenance::DngTag);
    EXPECT_DOUBLE_EQ(m.levels.white, 1.0);
    ASSERT_TRUE(m.dng);
    EXPECT_TRUE(m.dng->raw_ifd.white_level.empty());
    const bool noted =
        std::any_of(m.decoder.warnings.begin(), m.decoder.warnings.end(),
                    [](const std::string& w) { return w.find("no WhiteLevel tag") != std::string::npos; });
    EXPECT_TRUE(noted);
    EXPECT_DOUBLE_EQ(probe_ok(write_dng(spec, L"probe.dng")).metadata.levels.white, 1.0);
}

TEST_F(LibRawAdapter, NoiseProfileComesFromTheRawIfdElseIfdZero) {
    // Review finding NUME-2: several writers store NoiseProfile only in IFD 0 although the raw
    // image is a SubIFD.
    SyntheticDngSpec spec;
    spec.raw_in_subifd = true;
    spec.ifd0_noise_profile = {1.0458e-4, 2.8818e-8};
    {
        const DecodedRaw decoded = decode_ok(write_dng(spec, L"ifd0-noise.dng"));
        const RawMetadata& m = decoded.metadata;
        ASSERT_TRUE(m.dng);
        EXPECT_EQ(m.dng->raw_ifd.path, "ifd0.subifd0");
        ASSERT_TRUE(m.noise);
        EXPECT_EQ(m.noise->provenance, Provenance::DngTag);
        EXPECT_EQ(m.noise->source_ifd, "ifd0");
        EXPECT_EQ(m.noise->planes, (std::vector<NoiseModel>{{1.0458e-4, 2.8818e-8}}));
        EXPECT_EQ(m.interpretation.status, InterpretationStatus::Complete);
        expect_mosaic_samples(std::get<MosaicFrame>(decoded.image).pixels, 64, 48);
    }
    {
        // Both present: the raw IFD's own profile wins.
        spec.raw_noise_profile = {2e-5, 4e-7, 3e-5, 5e-7, 4e-5, 6e-7};
        const DecodedRaw decoded = decode_ok(write_dng(spec, L"both-noise.dng"));
        ASSERT_TRUE(decoded.metadata.noise);
        EXPECT_EQ(decoded.metadata.noise->source_ifd, "ifd0.subifd0");
        EXPECT_EQ(decoded.metadata.noise->planes.size(), 3u);
    }
    {
        // An IFD 0 profile that does not match the colour planes is malformed DNG metadata.
        spec.raw_noise_profile.clear();
        spec.ifd0_noise_profile = {2e-5, 4e-7, 3e-5, 5e-7};  // 2 planes for a 3-plane CFA
        const ProbeResult probe = probe_ok(write_dng(spec, L"bad-noise.dng"));
        EXPECT_EQ(probe.metadata.interpretation.status, InterpretationStatus::Ambiguous);
        EXPECT_FALSE(probe.metadata.noise);
    }
}

TEST_F(LibRawAdapter, BlackLevelDeltasTheContractCannotCarryBlockVerification) {
    // Review finding NUME-6: LibRaw folds BlackLevelDeltaH/V into the common black level.
    SyntheticDngSpec spec;
    spec.black_level_delta_h.assign(64, {0, 1});
    spec.black_level_delta_h[10] = {6, 1};
    spec.black_level_delta_v.assign(48, {-1, 2});
    {
        const DecodedRaw decoded = decode_ok(write_dng(spec, L"deltas.dng"));
        const RawMetadata& m = decoded.metadata;
        ASSERT_TRUE(m.dng);
        EXPECT_EQ(m.dng->raw_ifd.black_level_delta_h.size(), 64u);
        EXPECT_DOUBLE_EQ(m.dng->raw_ifd.black_level_delta_h[10], 6.0);
        EXPECT_EQ(m.dng->raw_ifd.black_level_delta_v, std::vector<double>(48, -0.5));
        EXPECT_EQ(m.interpretation.status, InterpretationStatus::RequiresUnsupportedOperation);
        ASSERT_EQ(m.interpretation.issues.size(), 1u);
        EXPECT_EQ(m.interpretation.issues[0].kind,
                  InterpretationIssueKind::UnrepresentedBlackLevelCorrection);
        EXPECT_NE(m.interpretation.issues[0].message.find("BlackLevelDeltaH (64 values, 0 to 6)"),
                  std::string::npos)
            << m.interpretation.issues[0].message;
    }
    {
        // All-zero deltas change nothing.
        spec.black_level_delta_h.assign(64, {0, 1});
        spec.black_level_delta_v.assign(48, {0, 1});
        const DecodedRaw decoded = decode_ok(write_dng(spec, L"zero-deltas.dng"));
        EXPECT_EQ(decoded.metadata.interpretation.status, InterpretationStatus::Complete);
    }
    {
        // A count that does not match the active area is malformed.
        spec.black_level_delta_h.assign(63, {1, 1});
        const ProbeResult probe = probe_ok(write_dng(spec, L"bad-deltas.dng"));
        EXPECT_EQ(probe.metadata.interpretation.status, InterpretationStatus::Ambiguous);
    }
}

TEST_F(LibRawAdapter, DualFisheye360SourceIsNeverACompletePlanarImage) {
    // Review finding NUME-4: an Insta360 two-lens frame (2:1) is recognised; a 4:3 frame of the
    // same maker (an Ace-series planar camera) is not.
    SyntheticDngSpec spec;
    spec.make = "Arashi Vision";
    spec.model = "Insta360 One2";
    spec.width = 32;
    spec.height = 64;
    const DecodedRaw decoded = decode_ok(write_dng(spec, L"one-x.dng"));
    const RawMetadata& m = decoded.metadata;
    EXPECT_EQ(m.projection.layout, ViewLayout::DualFisheye360);
    EXPECT_EQ(m.projection.provenance, Provenance::LucentRule);
    EXPECT_EQ(m.interpretation.status, InterpretationStatus::RequiresUnsupportedOperation);
    ASSERT_EQ(m.interpretation.issues.size(), 1u);
    EXPECT_EQ(m.interpretation.issues[0].kind, InterpretationIssueKind::MultiViewSource);

    spec.model = "Insta360 Ace Pro";
    spec.width = 64;
    spec.height = 48;
    const DecodedRaw planar = decode_ok(write_dng(spec, L"ace.dng"));
    EXPECT_EQ(planar.metadata.projection.layout, ViewLayout::SingleView);
    EXPECT_EQ(planar.metadata.interpretation.status, InterpretationStatus::Complete);
}

TEST_F(LibRawAdapter, SubimageIndexIsValidated) {
    const auto path = write_dng(SyntheticDngSpec{});
    DecodeOptions explicit_zero;
    explicit_zero.subimage = 0;
    decode_ok(path, explicit_zero);

    DecodeOptions second;
    second.subimage = 1;
    const Error error = decode_error(path, second);
    EXPECT_EQ(error.code, ErrorCode::NotFound);
    EXPECT_EQ(error.where, "raw.libraw.subimage");
    ProbeOptions probe_second;
    probe_second.subimage = 1;
    EXPECT_EQ(probe_error(path, probe_second).code, ErrorCode::NotFound);
}

TEST_F(LibRawAdapter, MultiImageFileNeedsAnExplicitSubimage) {
    // Two raw images (IFD 0 and a SubIFD, both NewSubFileType 0): LibRaw exposes two shots.
    SyntheticDngSpec spec;
    spec.raw_images = 2;
    const auto path = write_dng(spec);

    const ProbeResult probe = probe_ok(path);
    EXPECT_EQ(probe.metadata.subimage_index, 0u);
    EXPECT_EQ(probe.metadata.subimage_count, 2u);
    ASSERT_EQ(probe.subimages.size(), 2u);
    for (std::uint32_t i = 0; i < 2; ++i) {
        EXPECT_EQ(probe.subimages[i].index, i);
        EXPECT_FALSE(probe.subimages[i].error);
        EXPECT_EQ(probe.subimages[i].raw_width, 64u);
        EXPECT_EQ(probe.subimages[i].raw_height, 48u);
        EXPECT_EQ(probe.subimages[i].representation, RawRepresentation::Mosaic);
    }
    // The DNG inspector finds two raw IFDs and cannot tell which one a subimage is: the DNG tags
    // (opcodes, noise profile, ...) are not attached, and the status says why. Nor can Lucent
    // tell what the images are: their role is Unknown.
    EXPECT_EQ(probe.metadata.interpretation.status, InterpretationStatus::Ambiguous);
    EXPECT_FALSE(probe.metadata.dng);
    EXPECT_EQ(probe.metadata.subimage_role, SubimageRole::Unknown);
    for (const SubimageInfo& info : probe.subimages) {
        EXPECT_EQ(info.role, SubimageRole::Unknown);
        EXPECT_EQ(info.interpretation, InterpretationStatus::Ambiguous);
    }
    // The compression still comes from the codec when the raw IFD is not identified.
    EXPECT_EQ(probe.metadata.format.compression.provenance, Provenance::Decoder);
    EXPECT_EQ(probe.metadata.format.compression.name, "Uncompressed");
    ProbeOptions probe_second;
    probe_second.subimage = 1;
    EXPECT_EQ(probe_ok(path, probe_second).metadata.subimage_index, 1u);

    // Never silently the first image: decoding requires a choice, directly and via the registry.
    const Error implicit = decode_error(path);
    EXPECT_EQ(implicit.code, ErrorCode::InvalidArgument);
    EXPECT_EQ(implicit.where, "raw.libraw.subimage");
    auto registry = create_default_registry();
    ASSERT_TRUE(registry);
    auto via_registry = registry->decode(path, {}, {});
    ASSERT_FALSE(via_registry);
    EXPECT_EQ(via_registry.error().code, ErrorCode::InvalidArgument);

    for (std::uint32_t image = 0; image < 2; ++image) {
        SCOPED_TRACE(image);
        DecodeOptions options;
        options.subimage = image;
        const DecodedRaw decoded = decode_ok(path, options);
        EXPECT_EQ(decoded.metadata.subimage_index, image);
        EXPECT_EQ(decoded.metadata.subimage_count, 2u);
        EXPECT_EQ(decoded.metadata.interpretation.status, InterpretationStatus::Ambiguous);
        ASSERT_TRUE(std::holds_alternative<MosaicFrame>(decoded.image));
        expect_mosaic_samples(std::get<MosaicFrame>(decoded.image).pixels, 64, 48, image);
    }

    DecodeOptions third;
    third.subimage = 2;
    const Error missing = decode_error(path, third);
    EXPECT_EQ(missing.code, ErrorCode::NotFound);
    EXPECT_EQ(missing.where, "raw.libraw.subimage");
}

TEST_F(LibRawAdapter, UnicodeAndLongPathsWork) {
    const std::wstring unicode_name = L"Überprüfung 日本語 ñ \U0001F4F7.dng";
    const auto unicode_path = write_dng(SyntheticDngSpec{}, unicode_name);
    const DecodedRaw decoded = decode_ok(unicode_path);
    EXPECT_EQ(decoded.metadata.source.path, unicode_path);
    EXPECT_EQ(decoded.metadata.format.extension_hint, "dng");

    // A path longer than MAX_PATH (260 characters).
    std::filesystem::path deep;
    for (int i = 0; i < 8; ++i) {
        deep /= std::wstring(40, static_cast<wchar_t>(L'a' + i));
    }
    const auto long_path = write_dng(SyntheticDngSpec{}, (deep / L"long.dng").wstring());
    ASSERT_GT(long_path.wstring().size(), 260u);
    decode_ok(long_path);
}

TEST_F(LibRawAdapter, UppercaseAndMisleadingExtensionsAreOnlyHints) {
    // A DNG named .NEF is still decoded as the DNG it is; the extension is only recorded.
    const auto path = write_dng(SyntheticDngSpec{}, L"MISNAMED.NEF");
    const DecodedRaw decoded = decode_ok(path);
    EXPECT_EQ(decoded.metadata.format.container, ContainerFormat::Dng);
    EXPECT_EQ(decoded.metadata.format.extension_hint, "nef");
}

TEST_F(LibRawAdapter, ConcurrentDecodesAreIndependent) {
    const auto path = write_dng(SyntheticDngSpec{});
    constexpr int kThreads = 4;
    std::vector<std::optional<Error>> errors(kThreads);
    std::vector<std::uint16_t> checksums(kThreads, 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int round = 0; round < 3; ++round) {
                auto decoded = decoder_->decode(path, {}, {});
                if (!decoded) {
                    errors[static_cast<std::size_t>(t)] = decoded.error();
                    return;
                }
                const auto& buffer =
                    std::get<image::ImageBuffer<std::uint16_t>>(std::get<MosaicFrame>(decoded->image).pixels);
                std::uint16_t sum = 0;
                for (std::int32_t y = 0; y < buffer.height(); ++y) {
                    for (std::int32_t x = 0; x < buffer.width(); ++x) {
                        sum = static_cast<std::uint16_t>(sum * 31 + buffer.at(x, y, 0));
                    }
                }
                checksums[static_cast<std::size_t>(t)] = sum;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (int t = 0; t < kThreads; ++t) {
        EXPECT_FALSE(errors[static_cast<std::size_t>(t)]) << errors[static_cast<std::size_t>(t)]->describe();
        EXPECT_EQ(checksums[static_cast<std::size_t>(t)], checksums[0]);
    }
}

TEST_F(LibRawAdapter, RegistryDecodesThroughTheAdapter) {
    auto registry = create_default_registry();
    ASSERT_TRUE(registry);
    const auto path = write_dng(SyntheticDngSpec{});
    auto selection = registry->select(path, {}, {});
    ASSERT_TRUE(selection) << selection.error().describe();
    EXPECT_EQ(selection->decoder->id(), "libraw");
    auto decoded = registry->decode(path, {}, {});
    ASSERT_TRUE(decoded) << decoded.error().describe();
    EXPECT_EQ(decoded->metadata.decoder.adapter_id, "libraw");

    auto text = registry->select(dir_.write_text(L"readme.txt", "hello, not a raw file at all"), {}, {});
    ASSERT_FALSE(text);
    EXPECT_EQ(text.error().code, ErrorCode::Unsupported);
}

}  // namespace
}  // namespace lucent::raw
