// write_verified / export_image: JPEG and TIFF round trips, embedded ICC, receipts, no-clobber
// publication, temporary-file cleanup on injected failures and cancellation, byte-exact JPEG and
// row-wise TIFF verification, codec cancellation, Unicode and long paths.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <lcms2.h>

#include <lucent/core/hash.hpp>
#include <lucent/export/export.hpp>

#include "codecs.hpp"
#include "export_test_support.hpp"
#include "fault_injection.hpp"

namespace lucent::exporter {
namespace {

using test_support::list_directory;
using test_support::TempDir;

image::ColorEncoding rec2020_display() {
    image::ColorEncoding e = image::ColorEncoding::scene_linear_rec2020_d65();
    e.reference = image::ImageReference::Display;
    return e;
}

OutputImage quantise(std::int32_t width, std::int32_t height, const ExportSettings& settings) {
    auto rgb = test_support::make_gradient(width, height);
    auto out = encode_for_output(rgb.const_view(), rec2020_display(), settings.profile, bit_depth_for(settings.format),
                                 settings.transform);
    if (!out) {
        ADD_FAILURE() << out.error().describe();
        return {};
    }
    return std::move(*out);
}

std::vector<std::uint8_t> expected_icc(OutputProfile profile) {
    auto icc = icc_profile(profile);
    EXPECT_TRUE(icc);
    return std::vector<std::uint8_t>(icc->begin(), icc->end());
}

// Installs a fault hook for the lifetime of the object (on this thread).
class ScopedHook {
public:
    ScopedHook(detail::FaultHook hook, void* context) { detail::set_fault_hook(hook, context); }
    ~ScopedHook() { detail::set_fault_hook(nullptr, nullptr); }
    ScopedHook(const ScopedHook&) = delete;
    ScopedHook& operator=(const ScopedHook&) = delete;
};

void check_receipt(const ExportReceipt& receipt, const std::filesystem::path& path, const OutputImage& image,
                   const ExportSettings& settings) {
    EXPECT_EQ(receipt.path, path);
    EXPECT_EQ(receipt.format, settings.format);
    EXPECT_EQ(receipt.profile, settings.profile);
    EXPECT_EQ(receipt.bit_depth, image.bit_depth);
    EXPECT_EQ(receipt.width, image.width);
    EXPECT_EQ(receipt.height, image.height);
    EXPECT_EQ(receipt.byte_size, std::filesystem::file_size(test_support::verbatim(path)));
    auto hashed = core::hash_file(path, {});
    ASSERT_TRUE(hashed) << hashed.error().describe();
    EXPECT_EQ(receipt.sha256, hashed->digest);
    const auto icc = expected_icc(settings.profile);
    auto icc_hash = core::sha256(std::as_bytes(std::span<const std::uint8_t>(icc)));
    ASSERT_TRUE(icc_hash);
    EXPECT_EQ(receipt.icc_sha256, *icc_hash);
    EXPECT_NE(receipt.encoder.find(settings.format == ExportFormat::Jpeg ? "libjpeg-turbo " : "libtiff "),
              std::string::npos)
        << receipt.encoder;
    EXPECT_NE(receipt.encoder.find("lcms 2."), std::string::npos) << receipt.encoder;
}

// ---------------------------------------------------------------------------------------------
// Round trips

TEST(ExportJpeg, RoundTripWithinQuantisationTolerance) {
    TempDir dir;
    const ExportSettings settings = ExportSettings::jpeg_srgb_q95();
    const OutputImage image = quantise(160, 96, settings);
    const auto path = dir / L"round_trip.jpg";
    auto receipt = write_verified(image, path, settings);
    ASSERT_TRUE(receipt) << receipt.error().describe();
    check_receipt(*receipt, path, image, settings);
    EXPECT_EQ(receipt->chroma_subsampling, ChromaSubsampling::Yuv444);

    auto decoded = detail::read_jpeg(path);
    ASSERT_TRUE(decoded) << decoded.error().describe();
    EXPECT_EQ(decoded->width, 160);
    EXPECT_EQ(decoded->height, 96);
    EXPECT_EQ(decoded->jpeg_luma_h_samp, 1);
    EXPECT_EQ(decoded->jpeg_luma_v_samp, 1);
    EXPECT_EQ(decoded->icc, expected_icc(OutputProfile::Srgb));
    ASSERT_EQ(decoded->samples8.size(), image.samples8.size());
    // Quality 95, 4:4:4, smooth content: JPEG error is DCT quantisation plus YCbCr rounding.
    // Bounds for the IJG q95 tables on this smooth gradient: mean < 1.5 codes; max <= 16 codes
    // (single samples where the steep corner of the gradient meets 8x8 block edges).
    double sum = 0.0;
    int worst = 0;
    for (std::size_t i = 0; i < image.samples8.size(); ++i) {
        const int diff = std::abs(int{decoded->samples8[i]} - int{image.samples8[i]});
        sum += diff;
        worst = std::max(worst, diff);
    }
    EXPECT_LT(sum / static_cast<double>(image.samples8.size()), 1.5);
    EXPECT_LE(worst, 16);
}

TEST(ExportJpeg, LowQualityUsesChromaSubsampling) {
    TempDir dir;
    ExportSettings settings = ExportSettings::jpeg_srgb_q95();
    settings.jpeg.quality = 60;
    settings.profile = OutputProfile::DisplayP3;
    const OutputImage image = quantise(33, 17, settings);  // odd sizes exercise partial MCUs
    const auto path = dir / L"q60.jpg";
    auto receipt = write_verified(image, path, settings);
    ASSERT_TRUE(receipt) << receipt.error().describe();
    EXPECT_EQ(receipt->chroma_subsampling, ChromaSubsampling::Yuv420);
    auto decoded = detail::read_jpeg(path);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->jpeg_luma_h_samp, 2);
    EXPECT_EQ(decoded->jpeg_luma_v_samp, 2);
    EXPECT_EQ(decoded->icc, expected_icc(OutputProfile::DisplayP3));
}

TEST(ExportJpeg, ChromaSubsamplingThreshold) {
    EXPECT_EQ(jpeg_chroma_subsampling(100), ChromaSubsampling::Yuv444);
    EXPECT_EQ(jpeg_chroma_subsampling(90), ChromaSubsampling::Yuv444);
    EXPECT_EQ(jpeg_chroma_subsampling(89), ChromaSubsampling::Yuv420);
    EXPECT_EQ(jpeg_chroma_subsampling(1), ChromaSubsampling::Yuv420);
}

TEST(ExportTiff, LosslessRoundTripForEachCompression) {
    for (const TiffCompression compression : {TiffCompression::Deflate, TiffCompression::None}) {
        TempDir dir;
        ExportSettings settings = ExportSettings::tiff16(OutputProfile::DisplayP3);
        settings.tiff.compression = compression;
        const OutputImage image = quantise(129, 65, settings);
        const auto path = dir / L"round_trip.tif";
        auto receipt = write_verified(image, path, settings);
        ASSERT_TRUE(receipt) << receipt.error().describe();
        check_receipt(*receipt, path, image, settings);

        auto decoded = detail::read_tiff(path);
        ASSERT_TRUE(decoded) << decoded.error().describe();
        EXPECT_EQ(decoded->bits_per_sample, 16);
        EXPECT_EQ(decoded->channels, 3);
        EXPECT_EQ(decoded->tiff_orientation, 1);
        EXPECT_EQ(decoded->tiff_photometric, 2);
        EXPECT_EQ(decoded->tiff_compression, compression == TiffCompression::Deflate ? 8 : 1);
        EXPECT_EQ(decoded->tiff_directories, 1);
        EXPECT_EQ(decoded->samples16, image.samples16);
        EXPECT_EQ(decoded->icc, expected_icc(OutputProfile::DisplayP3));
    }
}

TEST(Export, EmbeddedProfileIsExtractableAndParses) {
    TempDir dir;
    const ExportSettings settings = ExportSettings::tiff16(OutputProfile::Srgb);
    const auto path = dir / L"icc.tif";
    auto receipt = write_verified(quantise(8, 8, settings), path, settings);
    ASSERT_TRUE(receipt) << receipt.error().describe();
    auto decoded = detail::read_tiff(path);
    ASSERT_TRUE(decoded);
    cmsHPROFILE profile = cmsOpenProfileFromMem(decoded->icc.data(), static_cast<cmsUInt32Number>(decoded->icc.size()));
    ASSERT_NE(profile, nullptr);
    char description[128] = {};
    cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", description, sizeof(description));
    EXPECT_STREQ(description, "sRGB IEC61966-2.1 (Lucent)");
    EXPECT_EQ(cmsGetColorSpace(profile), cmsSigRgbData);
    cmsCloseProfile(profile);
}

TEST(Export, OutputIsDeterministic) {
    TempDir dir;
    for (const ExportSettings& settings : {ExportSettings::jpeg_srgb_q95(), ExportSettings::tiff16(OutputProfile::Srgb)}) {
        const OutputImage image = quantise(64, 48, settings);
        auto first = write_verified(image, dir / (std::string(to_string(settings.format)) + "_a"), settings);
        auto second = write_verified(image, dir / (std::string(to_string(settings.format)) + "_b"), settings);
        ASSERT_TRUE(first) << first.error().describe();
        ASSERT_TRUE(second) << second.error().describe();
        EXPECT_EQ(first->sha256, second->sha256) << to_string(settings.format);
    }
}

TEST(Export, ExportImageConvertsAndPublishes) {
    TempDir dir;
    auto rgb = test_support::make_gradient(50, 30);
    const auto path = dir / L"end_to_end.tif";
    auto receipt = export_image(rgb.const_view(), rec2020_display(), path, ExportSettings::tiff16(OutputProfile::DisplayP3));
    ASSERT_TRUE(receipt) << receipt.error().describe();
    auto direct = encode_for_output(rgb.const_view(), rec2020_display(), OutputProfile::DisplayP3, BitDepth::Sixteen);
    ASSERT_TRUE(direct);
    auto decoded = detail::read_tiff(path);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->samples16, direct->samples16);
    EXPECT_EQ(list_directory(dir.path()), std::vector<std::wstring>{L"end_to_end.tif"});
}

// ---------------------------------------------------------------------------------------------
// Destinations and publication

TEST(ExportPublication, ExistingTargetIsNeverOverwritten) {
    TempDir dir;
    const auto path = dir / L"taken.jpg";
    test_support::write_all(path, "original bytes");
    const ExportSettings settings = ExportSettings::jpeg_srgb_q95();
    auto receipt = write_verified(quantise(8, 8, settings), path, settings);
    ASSERT_FALSE(receipt);
    EXPECT_EQ(receipt.error().code, ErrorCode::AlreadyExists);
    const auto bytes = test_support::read_all(path);
    EXPECT_EQ(std::string(bytes.begin(), bytes.end()), "original bytes");
    EXPECT_EQ(list_directory(dir.path()), std::vector<std::wstring>{L"taken.jpg"});

    // export_image fails before converting anything.
    auto rgb = test_support::make_gradient(4, 4);
    auto again = export_image(rgb.const_view(), rec2020_display(), path, settings);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, ErrorCode::AlreadyExists);

    // A case-only variant is the same file on NTFS.
    auto upper = write_verified(quantise(8, 8, settings), dir / L"TAKEN.JPG", settings);
    ASSERT_FALSE(upper);
    EXPECT_EQ(upper.error().code, ErrorCode::AlreadyExists);
}

Status create_target_during_publication(detail::ExportStage stage, const std::filesystem::path&, void* context) {
    if (stage == detail::ExportStage::Hashed) {
        test_support::write_all(*static_cast<std::filesystem::path*>(context), "racing writer");
    }
    return ok();
}

TEST(ExportPublication, TargetCreatedDuringTheExportIsNotClobbered) {
    TempDir dir;
    std::filesystem::path path = dir / L"race.tif";
    ScopedHook hook(create_target_during_publication, &path);
    const ExportSettings settings = ExportSettings::tiff16(OutputProfile::Srgb);
    auto receipt = write_verified(quantise(8, 8, settings), path, settings);
    ASSERT_FALSE(receipt);
    EXPECT_EQ(receipt.error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(receipt.error().where, "export.publish");
    const auto bytes = test_support::read_all(path);
    EXPECT_EQ(std::string(bytes.begin(), bytes.end()), "racing writer");
    EXPECT_EQ(list_directory(dir.path()), std::vector<std::wstring>{L"race.tif"}) << "temporary file left behind";
}

struct FailAt {
    detail::ExportStage stage;
    std::filesystem::path seen_temp;
};

Status fail_at_stage(detail::ExportStage stage, const std::filesystem::path& temp, void* context) {
    auto* fail_at = static_cast<FailAt*>(context);
    fail_at->seen_temp = temp;
    if (stage == fail_at->stage) {
        return fail(ErrorCode::IoError, "injected failure", "test.inject");
    }
    return ok();
}

TEST(ExportPublication, TemporaryFileIsRemovedOnEveryInjectedFailure) {
    for (const ExportSettings& settings : {ExportSettings::jpeg_srgb_q95(), ExportSettings::tiff16(OutputProfile::Srgb)}) {
        for (const auto stage : {detail::ExportStage::TempReserved, detail::ExportStage::Written,
                                 detail::ExportStage::Verified, detail::ExportStage::Hashed}) {
            TempDir dir;
            FailAt fail_at{stage, {}};
            ScopedHook hook(fail_at_stage, &fail_at);
            auto receipt = write_verified(quantise(16, 16, settings), dir / L"out.bin", settings);
            ASSERT_FALSE(receipt);
            EXPECT_EQ(receipt.error().where, "test.inject");
            EXPECT_FALSE(fail_at.seen_temp.empty());
            EXPECT_EQ(fail_at.seen_temp.parent_path(), dir.path()) << "temporary file must be a sibling";
            EXPECT_TRUE(list_directory(dir.path()).empty())
                << "stage " << static_cast<int>(stage) << " left files behind";
        }
    }
}

Status truncate_written_file(detail::ExportStage stage, const std::filesystem::path& temp, void*) {
    if (stage == detail::ExportStage::Written) {
        std::filesystem::resize_file(test_support::verbatim(temp), std::filesystem::file_size(test_support::verbatim(temp)) / 2);
    }
    return ok();
}

TEST(ExportVerification, TruncatedOutputIsDetectedAndRemoved) {
    for (const ExportSettings& settings : {ExportSettings::jpeg_srgb_q95(), ExportSettings::tiff16(OutputProfile::Srgb)}) {
        TempDir dir;
        ScopedHook hook(truncate_written_file, nullptr);
        auto receipt = write_verified(quantise(64, 64, settings), dir / L"truncated", settings);
        ASSERT_FALSE(receipt);
        EXPECT_EQ(receipt.error().code, ErrorCode::Corrupt) << receipt.error().describe();
        EXPECT_TRUE(list_directory(dir.path()).empty());
    }
}

Status flip_pixel_byte(detail::ExportStage stage, const std::filesystem::path& temp, void*) {
    if (stage == detail::ExportStage::Written) {
        // Uncompressed TIFF: libtiff writes the 8-byte header, then the strip data; byte 100 is
        // pixel data.
        std::fstream file(test_support::verbatim(temp), std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(100);
        file.put('\x5A');
    }
    return ok();
}

TEST(ExportVerification, AlteredTiffPixelsAreDetected) {
    TempDir dir;
    ScopedHook hook(flip_pixel_byte, nullptr);
    ExportSettings settings = ExportSettings::tiff16(OutputProfile::Srgb);
    settings.tiff.compression = TiffCompression::None;
    auto receipt = write_verified(quantise(32, 32, settings), dir / L"altered.tif", settings);
    ASSERT_FALSE(receipt);
    EXPECT_EQ(receipt.error().code, ErrorCode::Corrupt) << receipt.error().describe();
    EXPECT_TRUE(list_directory(dir.path()).empty());
}

// Flips one byte of the entropy-coded data of the written JPEG (never producing or touching a
// 0xFF marker byte), at a position chosen per call.
struct ScanCorruption {
    double position = 0.5;  // fraction of the scan data
    std::uint8_t original = 0;
    std::uint8_t replaced = 0;
};

Status corrupt_jpeg_scan(detail::ExportStage stage, const std::filesystem::path& temp, void* context) {
    if (stage != detail::ExportStage::Written) {
        return ok();
    }
    auto* corruption = static_cast<ScanCorruption*>(context);
    std::fstream file(test_support::verbatim(temp), std::ios::in | std::ios::out | std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // Start of scan: the last SOS marker (FF DA) and its header length; scan data runs to EOI.
    std::size_t sos = 0;
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        if (static_cast<std::uint8_t>(bytes[i]) == 0xFF && static_cast<std::uint8_t>(bytes[i + 1]) == 0xDA) {
            sos = i;
        }
    }
    EXPECT_GT(sos, 0u);
    const std::size_t header = (static_cast<std::uint8_t>(bytes[sos + 2]) << 8) | static_cast<std::uint8_t>(bytes[sos + 3]);
    const std::size_t first = sos + 2 + header;
    const std::size_t last = bytes.size() - 2;  // EOI
    std::size_t at = first + static_cast<std::size_t>(corruption->position * static_cast<double>(last - first - 1));
    while (static_cast<std::uint8_t>(bytes[at]) == 0xFF || (at > 0 && static_cast<std::uint8_t>(bytes[at - 1]) == 0xFF)) {
        ++at;
    }
    corruption->original = static_cast<std::uint8_t>(bytes[at]);
    corruption->replaced = static_cast<std::uint8_t>(corruption->original == 0xFE ? 0xFC : corruption->original ^ 0x01);
    file.seekp(static_cast<std::streamoff>(at));
    file.put(static_cast<char>(corruption->replaced));
    return ok();
}

TEST(ExportVerification, EveryCorruptedJpegScanByteIsDetected) {
    // Most single-byte corruptions of JPEG scan data decode without any warning; verification
    // must still reject every one of them (the file is compared with the encoder's output).
    const ExportSettings settings = ExportSettings::jpeg_srgb_q95();
    const OutputImage image = quantise(96, 64, settings);
    for (int trial = 0; trial < 40; ++trial) {
        TempDir dir;
        ScanCorruption corruption{(trial + 0.5) / 40.0};
        ScopedHook hook(corrupt_jpeg_scan, &corruption);
        auto receipt = write_verified(image, dir / L"corrupt.jpg", settings);
        ASSERT_FALSE(receipt) << "trial " << trial;
        EXPECT_EQ(receipt.error().code, ErrorCode::Corrupt) << receipt.error().describe();
        EXPECT_EQ(receipt.error().where, "export.file.compare");
        EXPECT_NE(corruption.original, corruption.replaced);
        EXPECT_TRUE(list_directory(dir.path()).empty());
    }
}

struct CancelAt {
    detail::ExportStage stage;
    core::CancellationSource* source;
};

Status cancel_at_stage(detail::ExportStage stage, const std::filesystem::path&, void* context) {
    auto* cancel_at = static_cast<CancelAt*>(context);
    if (stage == cancel_at->stage) {
        cancel_at->source->cancel();
    }
    return ok();
}

TEST(ExportPublication, CancellationDuringTheProtocolNeverPublishesAndCleansUp) {
    // Cancellation requested while the export runs (from the protocol hook), at every step: the
    // next check inside the writer, the verifier, the hash or before publication must stop it.
    for (const ExportSettings& settings : {ExportSettings::jpeg_srgb_q95(), ExportSettings::tiff16(OutputProfile::Srgb)}) {
        for (const auto stage : {detail::ExportStage::TempReserved, detail::ExportStage::Written,
                                 detail::ExportStage::Verified, detail::ExportStage::Hashed}) {
            TempDir dir;
            core::CancellationSource source;
            CancelAt cancel_at{stage, &source};
            ScopedHook hook(cancel_at_stage, &cancel_at);
            auto receipt = write_verified(quantise(40, 24, settings), dir / L"out.bin", settings, source.token());
            ASSERT_FALSE(receipt) << static_cast<int>(stage);
            EXPECT_EQ(receipt.error().code, ErrorCode::Cancelled) << receipt.error().describe();
            EXPECT_TRUE(list_directory(dir.path()).empty())
                << "stage " << static_cast<int>(stage) << " left files behind";
        }
    }
}

TEST(ExportCodecs, EncodersAndReadersHonourCancellation) {
    TempDir dir;
    core::CancellationSource source;
    source.cancel();
    const ExportSettings jpeg_settings = ExportSettings::jpeg_srgb_q95();
    const OutputImage jpeg_image = quantise(16, 16, jpeg_settings);
    const auto icc = expected_icc(OutputProfile::Srgb);
    auto encoded = detail::encode_jpeg(jpeg_image, jpeg_settings.jpeg, icc, source.token());
    ASSERT_FALSE(encoded);
    EXPECT_EQ(encoded.error().code, ErrorCode::Cancelled);

    const ExportSettings tiff_settings = ExportSettings::tiff16(OutputProfile::Srgb);
    const OutputImage tiff_image = quantise(16, 16, tiff_settings);
    test_support::write_all(dir / L"t.tif", "");
    auto written = detail::write_tiff(dir / L"t.tif", tiff_image, tiff_settings.tiff, icc, source.token());
    ASSERT_FALSE(written);
    EXPECT_EQ(written.error().code, ErrorCode::Cancelled);

    // Readers: valid files, cancelled token.
    ASSERT_TRUE(write_verified(jpeg_image, dir / L"ok.jpg", jpeg_settings));
    ASSERT_TRUE(write_verified(tiff_image, dir / L"ok.tif", tiff_settings));
    auto jpeg_read = detail::read_jpeg(dir / L"ok.jpg", source.token());
    ASSERT_FALSE(jpeg_read);
    EXPECT_EQ(jpeg_read.error().code, ErrorCode::Cancelled);
    auto tiff_read = detail::read_tiff(dir / L"ok.tif", source.token(), &tiff_image);
    ASSERT_FALSE(tiff_read);
    EXPECT_EQ(tiff_read.error().code, ErrorCode::Cancelled);
}

TEST(ExportCodecs, TiffVerificationComparesRowByRow) {
    TempDir dir;
    const ExportSettings settings = ExportSettings::tiff16(OutputProfile::Srgb);
    const OutputImage image = quantise(24, 20, settings);
    ASSERT_TRUE(write_verified(image, dir / L"rows.tif", settings));
    auto same = detail::read_tiff(dir / L"rows.tif", {}, &image);
    ASSERT_TRUE(same) << same.error().describe();
    EXPECT_TRUE(same->samples_compared);
    EXPECT_EQ(same->first_mismatched_row, -1);
    EXPECT_TRUE(same->samples16.empty());  // no full-size copy while verifying

    OutputImage changed = image;
    changed.samples16[changed.row_samples() * 13 + 5] ^= 1;
    auto different = detail::read_tiff(dir / L"rows.tif", {}, &changed);
    ASSERT_TRUE(different);
    EXPECT_TRUE(different->samples_compared);
    EXPECT_EQ(different->first_mismatched_row, 13);
}

TEST(ExportPublication, UnicodeDestination) {
    TempDir dir;
    const std::filesystem::path folder = dir / L"Ünïcødé 写真 \U0001F4F7";
    std::filesystem::create_directory(folder);
    const std::filesystem::path path = folder / L"фото_Lucent_1.jpg";
    const ExportSettings settings = ExportSettings::jpeg_srgb_q95();
    auto receipt = write_verified(quantise(20, 10, settings), path, settings);
    ASSERT_TRUE(receipt) << receipt.error().describe();
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(list_directory(folder), std::vector<std::wstring>{L"фото_Lucent_1.jpg"});
}

TEST(ExportPublication, LongDestinationPath) {
    TempDir dir;
    std::filesystem::path folder = dir.path();
    while (folder.native().size() < 300) {
        folder /= std::wstring(40, L'd');
    }
    std::filesystem::create_directories(test_support::verbatim(folder));
    const auto path = folder / L"long_path_export.tif";
    ASSERT_GT(path.native().size(), 260u);
    const ExportSettings settings = ExportSettings::tiff16(OutputProfile::Srgb);
    auto receipt = write_verified(quantise(8, 8, settings), path, settings);
    ASSERT_TRUE(receipt) << receipt.error().describe();
    EXPECT_EQ(list_directory(folder), std::vector<std::wstring>{L"long_path_export.tif"});
}

TEST(ExportPublication, InvalidDestinations) {
    TempDir dir;
    const ExportSettings settings = ExportSettings::jpeg_srgb_q95();
    const OutputImage image = quantise(8, 8, settings);

    auto missing_dir = write_verified(image, dir / L"no such folder" / L"x.jpg", settings);
    ASSERT_FALSE(missing_dir);
    EXPECT_EQ(missing_dir.error().code, ErrorCode::NotFound);

    std::filesystem::create_directory(dir / L"folder.jpg");
    auto is_directory = write_verified(image, dir / L"folder.jpg", settings);
    ASSERT_FALSE(is_directory);
    EXPECT_EQ(is_directory.error().code, ErrorCode::AlreadyExists);

    auto no_name = write_verified(image, dir.path() / L"", settings);
    ASSERT_FALSE(no_name);
    EXPECT_EQ(no_name.error().code, ErrorCode::InvalidArgument);

    auto empty = write_verified(image, std::filesystem::path{}, settings);
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::InvalidArgument);

    test_support::write_all(dir / L"file", "x");
    auto parent_is_file = write_verified(image, dir / L"file" / L"x.jpg", settings);
    ASSERT_FALSE(parent_is_file);
    EXPECT_EQ(parent_is_file.error().code, ErrorCode::NotFound);
}

TEST(ExportSettingsValidation, InvalidSettingsAndMismatchedImagesAreRejected) {
    TempDir dir;
    const auto path = dir / L"x";
    const OutputImage jpeg_image = quantise(8, 8, ExportSettings::jpeg_srgb_q95());

    for (const int quality : {0, 101, -5}) {
        ExportSettings settings = ExportSettings::jpeg_srgb_q95();
        settings.jpeg.quality = quality;
        auto receipt = write_verified(jpeg_image, path, settings);
        ASSERT_FALSE(receipt);
        EXPECT_EQ(receipt.error().code, ErrorCode::InvalidArgument) << quality;
    }
    ExportSettings bad_format;
    bad_format.format = static_cast<ExportFormat>(5);
    EXPECT_EQ(bad_format.validate().error().code, ErrorCode::InvalidArgument);
    ExportSettings bad_compression = ExportSettings::tiff16(OutputProfile::Srgb);
    bad_compression.tiff.compression = static_cast<TiffCompression>(0);
    EXPECT_EQ(bad_compression.validate().error().code, ErrorCode::InvalidArgument);

    // 8-bit image for a 16-bit TIFF; sRGB image declared as P3.
    auto wrong_depth = write_verified(jpeg_image, path, ExportSettings::tiff16(OutputProfile::Srgb));
    ASSERT_FALSE(wrong_depth);
    EXPECT_EQ(wrong_depth.error().code, ErrorCode::InvalidArgument);
    ExportSettings p3 = ExportSettings::jpeg_srgb_q95();
    p3.profile = OutputProfile::DisplayP3;
    auto wrong_profile = write_verified(jpeg_image, path, p3);
    ASSERT_FALSE(wrong_profile);
    EXPECT_EQ(wrong_profile.error().code, ErrorCode::InvalidArgument);

    OutputImage inconsistent = jpeg_image;
    inconsistent.samples8.pop_back();
    auto bad_buffer = write_verified(inconsistent, path, ExportSettings::jpeg_srgb_q95());
    ASSERT_FALSE(bad_buffer);
    EXPECT_EQ(bad_buffer.error().code, ErrorCode::InvalidArgument);
    EXPECT_TRUE(list_directory(dir.path()).empty());
}

TEST(ExportSettingsValidation, CancelledExportLeavesNothingBehind) {
    TempDir dir;
    core::CancellationSource source;
    source.cancel();
    const ExportSettings settings = ExportSettings::jpeg_srgb_q95();
    auto receipt = write_verified(quantise(8, 8, settings), dir / L"c.jpg", settings, source.token());
    ASSERT_FALSE(receipt);
    EXPECT_EQ(receipt.error().code, ErrorCode::Cancelled);
    EXPECT_TRUE(list_directory(dir.path()).empty());
}

TEST(ExportSettingsValidation, PersistedValuesAreStable) {
    EXPECT_EQ(static_cast<int>(ExportFormat::Jpeg), 1);
    EXPECT_EQ(static_cast<int>(ExportFormat::Tiff), 2);
    EXPECT_EQ(static_cast<int>(ChromaSubsampling::Yuv444), 1);
    EXPECT_EQ(static_cast<int>(ChromaSubsampling::Yuv420), 2);
    EXPECT_EQ(static_cast<int>(TiffCompression::None), 1);
    EXPECT_EQ(static_cast<int>(TiffCompression::Deflate), 2);
    EXPECT_EQ(bit_depth_for(ExportFormat::Jpeg), BitDepth::Eight);
    EXPECT_EQ(bit_depth_for(ExportFormat::Tiff), BitDepth::Sixteen);
    EXPECT_EQ(ExportSettings::jpeg_srgb_q95().jpeg.quality, 95);
}

}  // namespace
}  // namespace lucent::exporter

namespace lucent::exporter {
namespace {

// Manual timing check (not run by default): --gtest_also_run_disabled_tests
// --gtest_filter=*DISABLED_Timing*. Prints conversion and export times for a 24-megapixel frame.
TEST(ExportTiming, DISABLED_Timing24Megapixels) {
    TempDir dir;
    auto rgb = test_support::make_gradient(6000, 4000);
    for (const ExportSettings& settings : {ExportSettings::jpeg_srgb_q95(), ExportSettings::tiff16(OutputProfile::DisplayP3)}) {
        const auto start = std::chrono::steady_clock::now();
        auto encoded = encode_for_output(rgb.const_view(), rec2020_display(), settings.profile,
                                         bit_depth_for(settings.format), settings.transform);
        ASSERT_TRUE(encoded);
        const auto converted = std::chrono::steady_clock::now();
        auto receipt = write_verified(*encoded, dir / (std::string("timing.") + std::string(to_string(settings.format))),
                                      settings);
        ASSERT_TRUE(receipt) << receipt.error().describe();
        const auto written = std::chrono::steady_clock::now();
        const auto ms = [](auto d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
        std::printf("%s: convert %lld ms, write+verify+hash+publish %lld ms, %llu bytes\n",
                    std::string(to_string(settings.format)).c_str(), static_cast<long long>(ms(converted - start)),
                    static_cast<long long>(ms(written - converted)),
                    static_cast<unsigned long long>(receipt->byte_size));
    }
}

}  // namespace
}  // namespace lucent::exporter
