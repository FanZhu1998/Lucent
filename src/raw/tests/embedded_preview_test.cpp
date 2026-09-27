// Tests of extract_embedded_preview (embedded_preview.hpp): synthetic DNGs with JPEG preview IFDs
// (selection, limits, problems, orientation, failure paths) and the smoke-tier camera fixtures
// (skipped when not downloaded).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/raw/embedded_preview.hpp>
#include <lucent/raw/libraw_decoder.hpp>

#include "raw_test_support.hpp"
#include "synthetic_dng.hpp"
#include "test_jpegs.hpp"

namespace lucent::raw {
namespace {

namespace ts = test_support;

ts::SyntheticJpegPreview preview_of(std::span<const std::uint8_t> jpeg, std::uint32_t width,
                                    std::uint32_t height, std::uint32_t components = 3) {
    return ts::SyntheticJpegPreview{ts::jpeg_bytes(jpeg), width, height, components};
}

class EmbeddedPreviewTest : public ::testing::Test {
protected:
    std::filesystem::path write_dng(const ts::SyntheticDngSpec& spec,
                                    const std::string& name = "preview.dng") {
        return dir_.write(name, ts::build_synthetic_dng(spec));
    }

    ts::ScratchDir dir_;
};

TEST_F(EmbeddedPreviewTest, ReturnsTheLargestJpegExactlyAsStored) {
    ts::SyntheticDngSpec spec;
    spec.jpeg_previews = {preview_of(ts::kJpegRgb48x32, 48, 32), preview_of(ts::kJpegRgb96x64, 96, 64)};
    const auto path = write_dng(spec);

    auto result = extract_embedded_preview(path);
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->status, EmbeddedPreviewStatus::Found);
    ASSERT_TRUE(result->preview);
    const EmbeddedPreview& p = *result->preview;
    EXPECT_EQ(p.format, EmbeddedPreviewFormat::Jpeg);
    EXPECT_EQ(p.width, 96);
    EXPECT_EQ(p.height, 64);
    EXPECT_EQ(p.components, 3);
    EXPECT_EQ(p.orientation, image::Orientation::Normal);
    EXPECT_EQ(p.bytes, ts::jpeg_bytes(ts::kJpegRgb96x64));
    EXPECT_EQ(result->decoder, "libraw");

    // The bytes are those at the reported offset of the file.
    const std::vector<std::byte> file = ts::read_all(path);
    ASSERT_LE(p.file_offset + p.bytes.size(), file.size());
    EXPECT_TRUE(std::equal(p.bytes.begin(), p.bytes.end(),
                           file.begin() + static_cast<std::ptrdiff_t>(p.file_offset)));

    ASSERT_EQ(result->candidates.size(), 2u);
    for (const PreviewCandidate& c : result->candidates) {
        EXPECT_FALSE(c.problem) << c.detail;
    }
    EXPECT_EQ(result->source_identity, *core::stat_file(path));
}

TEST_F(EmbeddedPreviewTest, ARequestedSizeChoosesTheSmallestPreviewThatCoversIt) {
    ts::SyntheticDngSpec spec;
    spec.jpeg_previews = {preview_of(ts::kJpegRgb96x64, 96, 64), preview_of(ts::kJpegRgb48x32, 48, 32)};
    const auto path = write_dng(spec);
    const auto chosen_width = [&](std::int32_t min_long_edge) {
        EmbeddedPreviewOptions options;
        options.min_long_edge = min_long_edge;
        auto result = extract_embedded_preview(path, options);
        EXPECT_TRUE(result) << result.error().describe();
        return result && result->preview ? result->preview->width : -1;
    };
    EXPECT_EQ(chosen_width(0), 96);    // no size: the largest
    EXPECT_EQ(chosen_width(1), 48);    // the smallest that covers the size
    EXPECT_EQ(chosen_width(48), 48);   // exactly the long edge
    EXPECT_EQ(chosen_width(49), 96);   // the next larger one
    EXPECT_EQ(chosen_width(96), 96);
    EXPECT_EQ(chosen_width(500), 96);  // none is large enough: the largest

    EmbeddedPreviewOptions negative;
    negative.min_long_edge = -1;
    EXPECT_EQ(extract_embedded_preview(path, negative).error().code, ErrorCode::InvalidArgument);
}

TEST_F(EmbeddedPreviewTest, ASmallerPreviewWithAnotherAspectRatioIsNeverPreferred) {
    // 40 x 24 (5:3) and 32 x 16 (2:1) beside the 96 x 64 (3:2) main preview: letterboxed or cropped
    // thumbnails do not stand in for it, however small the requested size.
    ts::SyntheticDngSpec spec;
    spec.jpeg_previews = {preview_of(ts::kJpegGray40x24, 40, 24, 1), preview_of(ts::kJpegProgressive32x16, 32, 16),
                          preview_of(ts::kJpegRgb96x64, 96, 64)};
    EmbeddedPreviewOptions options;
    options.min_long_edge = 16;
    auto result = extract_embedded_preview(write_dng(spec), options);
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_TRUE(result->preview);
    EXPECT_EQ(result->preview->width, 96);
    EXPECT_EQ(result->candidates.size(), 3u);
}

TEST_F(EmbeddedPreviewTest, TheStreamsFrameHeaderDecidesTheDimensions) {
    // The IFDs declare the sizes the other way round; the JPEG frame headers are authoritative.
    ts::SyntheticDngSpec spec;
    spec.jpeg_previews = {preview_of(ts::kJpegRgb48x32, 4000, 3000), preview_of(ts::kJpegRgb96x64, 10, 10)};
    auto result = extract_embedded_preview(write_dng(spec));
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_TRUE(result->preview);
    EXPECT_EQ(result->preview->width, 96);
    EXPECT_EQ(result->preview->height, 64);
    EXPECT_EQ(result->candidates[0].width, 48);
    EXPECT_EQ(result->candidates[0].height, 32);
}

TEST_F(EmbeddedPreviewTest, ReportsTheRawOrientationAsTheDisplayRule) {
    ts::SyntheticDngSpec spec;
    spec.orientation = 6;  // EXIF: rotate 90 degrees clockwise
    spec.jpeg_previews = {preview_of(ts::kJpegRgb96x64, 96, 64)};
    auto result = extract_embedded_preview(write_dng(spec));
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_TRUE(result->preview);
    EXPECT_EQ(result->preview->orientation, image::Orientation::Rotate90Cw);
    // Pixels are unrotated; the upright size swaps the axes.
    EXPECT_EQ(result->preview->width, 96);
    EXPECT_EQ(result->preview->upright_size(), (image::Size{64, 96}));
}

TEST_F(EmbeddedPreviewTest, GreyscaleAndProgressiveJpegsAreUsable) {
    ts::SyntheticDngSpec spec;
    spec.jpeg_previews = {preview_of(ts::kJpegGray40x24, 40, 24, 1)};
    auto grey = extract_embedded_preview(write_dng(spec, "grey.dng"));
    ASSERT_TRUE(grey) << grey.error().describe();
    ASSERT_TRUE(grey->preview);
    EXPECT_EQ(grey->preview->components, 1);
    EXPECT_EQ(grey->preview->width, 40);

    spec.jpeg_previews = {preview_of(ts::kJpegProgressive32x16, 32, 16)};
    auto progressive = extract_embedded_preview(write_dng(spec, "progressive.dng"));
    ASSERT_TRUE(progressive) << progressive.error().describe();
    ASSERT_TRUE(progressive->preview);
    EXPECT_EQ(progressive->preview->width, 32);
}

TEST_F(EmbeddedPreviewTest, AFileWithoutPreviewsSaysSoExplicitly) {
    auto result = extract_embedded_preview(write_dng(ts::SyntheticDngSpec{}));
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->status, EmbeddedPreviewStatus::NoneInFile);
    EXPECT_FALSE(result->preview);
    EXPECT_TRUE(result->candidates.empty());
}

TEST_F(EmbeddedPreviewTest, BitmapPreviewsAreListedButNotReturned) {
    ts::SyntheticDngSpec spec;
    spec.raw_in_subifd = true;  // IFD 0 is an uncompressed 8x8 RGB preview
    auto only_bitmap = extract_embedded_preview(write_dng(spec, "bitmap.dng"));
    ASSERT_TRUE(only_bitmap) << only_bitmap.error().describe();
    EXPECT_EQ(only_bitmap->status, EmbeddedPreviewStatus::NoneUsable);
    ASSERT_EQ(only_bitmap->candidates.size(), 1u);
    EXPECT_EQ(only_bitmap->candidates[0].problem, PreviewProblem::NotJpeg);
    EXPECT_NE(only_bitmap->candidates[0].detail.find("bitmap"), std::string::npos)
        << only_bitmap->candidates[0].detail;

    spec.jpeg_previews = {preview_of(ts::kJpegRgb48x32, 48, 32)};
    auto mixed = extract_embedded_preview(write_dng(spec, "mixed.dng"));
    ASSERT_TRUE(mixed) << mixed.error().describe();
    ASSERT_EQ(mixed->status, EmbeddedPreviewStatus::Found);
    EXPECT_EQ(mixed->preview->width, 48);
    EXPECT_EQ(mixed->candidates.size(), 2u);
}

TEST_F(EmbeddedPreviewTest, InvalidAndUndisplayableJpegsAreSkipped) {
    std::vector<std::byte> garbage(300, std::byte{0x5A});  // not a JPEG stream
    std::vector<std::byte> lossless = ts::jpeg_bytes(ts::kJpegRgb96x64);
    // Turn the SOF0 marker into SOF3 (lossless): a valid structure no display decoder handles.
    for (std::size_t i = 2; i + 1 < lossless.size(); ++i) {
        if (lossless[i] == std::byte{0xFF} && lossless[i + 1] == std::byte{0xC0}) {
            lossless[i + 1] = std::byte{0xC3};
            break;
        }
    }
    ts::SyntheticDngSpec spec;
    spec.jpeg_previews = {ts::SyntheticJpegPreview{garbage, 500, 500, 3},
                          ts::SyntheticJpegPreview{lossless, 96, 64, 3},
                          preview_of(ts::kJpegRgb48x32, 48, 32)};
    auto result = extract_embedded_preview(write_dng(spec));
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->status, EmbeddedPreviewStatus::Found);
    EXPECT_EQ(result->preview->width, 48);
    ASSERT_EQ(result->candidates.size(), 3u);
    EXPECT_EQ(result->candidates[0].problem, PreviewProblem::InvalidJpeg);
    EXPECT_EQ(result->candidates[1].problem, PreviewProblem::UnsupportedJpeg);
    EXPECT_NE(result->candidates[1].detail.find("lossless"), std::string::npos);
    EXPECT_FALSE(result->candidates[2].problem);

    spec.jpeg_previews = {ts::SyntheticJpegPreview{garbage, 500, 500, 3}};
    auto none = extract_embedded_preview(write_dng(spec, "none.dng"));
    ASSERT_TRUE(none) << none.error().describe();
    EXPECT_EQ(none->status, EmbeddedPreviewStatus::NoneUsable);
    EXPECT_FALSE(none->preview);
}

TEST_F(EmbeddedPreviewTest, LimitsRejectLargePreviewsBeforeReadingThem) {
    ts::SyntheticDngSpec spec;
    spec.jpeg_previews = {preview_of(ts::kJpegRgb48x32, 48, 32), preview_of(ts::kJpegRgb96x64, 96, 64)};
    const auto path = write_dng(spec);

    EmbeddedPreviewOptions by_bytes;
    by_bytes.limits.max_bytes = ts::kJpegRgb48x32.size();  // the 96 x 64 stream is larger
    auto small = extract_embedded_preview(path, by_bytes);
    ASSERT_TRUE(small) << small.error().describe();
    ASSERT_TRUE(small->preview);
    EXPECT_EQ(small->preview->width, 48);
    EXPECT_EQ(small->candidates[1].problem, PreviewProblem::TooLarge);

    EmbeddedPreviewOptions by_pixels;
    by_pixels.limits.max_pixels = 48 * 32;
    auto fewer = extract_embedded_preview(path, by_pixels);
    ASSERT_TRUE(fewer) << fewer.error().describe();
    ASSERT_TRUE(fewer->preview);
    EXPECT_EQ(fewer->preview->width, 48);
    EXPECT_EQ(fewer->candidates[1].problem, PreviewProblem::TooLarge);

    EmbeddedPreviewOptions tiny;
    tiny.limits.max_bytes = 16;
    auto nothing = extract_embedded_preview(path, tiny);
    ASSERT_TRUE(nothing) << nothing.error().describe();
    EXPECT_EQ(nothing->status, EmbeddedPreviewStatus::NoneUsable);

    EmbeddedPreviewOptions file_limit;
    file_limit.max_file_bytes = 100;
    auto refused = extract_embedded_preview(path, file_limit);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::LimitExceeded);

    EmbeddedPreviewOptions invalid;
    invalid.limits.max_pixels = 0;
    auto rejected = extract_embedded_preview(path, invalid);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidArgument);
}

TEST_F(EmbeddedPreviewTest, NeverWritesToTheSource) {
    ts::SyntheticDngSpec spec;
    spec.jpeg_previews = {preview_of(ts::kJpegRgb96x64, 96, 64)};
    const auto path = write_dng(spec);
    const auto before = core::hash_file(path, {});
    ASSERT_TRUE(before);
    ASSERT_TRUE(extract_embedded_preview(path));
    const auto after = core::hash_file(path, {});
    ASSERT_TRUE(after);
    EXPECT_EQ(before->digest, after->digest);
    EXPECT_EQ(before->identity, after->identity);
}

TEST_F(EmbeddedPreviewTest, FailurePaths) {
    const auto missing = extract_embedded_preview(dir_.path() / "missing.dng");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);

    const auto empty = extract_embedded_preview(dir_.write("empty.dng", std::vector<std::byte>{}));
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::Unsupported);

    const auto text = extract_embedded_preview(dir_.write_text("notes.txt", std::string(4096, 'x')));
    ASSERT_FALSE(text);
    EXPECT_EQ(text.error().code, ErrorCode::Unsupported) << text.error().describe();

    ts::SyntheticDngSpec spec;
    spec.jpeg_previews = {preview_of(ts::kJpegRgb96x64, 96, 64)};
    core::CancellationSource source;
    source.cancel();
    const auto cancelled = extract_embedded_preview(write_dng(spec), {}, source.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);
}

TEST(EmbeddedPreviewNames, AreStable) {
    EXPECT_EQ(to_string(EmbeddedPreviewFormat::Jpeg), "jpeg");
    EXPECT_EQ(to_string(PreviewProblem::NotJpeg), "not_jpeg");
    EXPECT_EQ(to_string(PreviewProblem::OutsideFile), "outside_file");
    EXPECT_EQ(to_string(PreviewProblem::TooLarge), "too_large");
    EXPECT_EQ(to_string(PreviewProblem::InvalidJpeg), "invalid_jpeg");
    EXPECT_EQ(to_string(PreviewProblem::UnsupportedJpeg), "unsupported_jpeg");
    EXPECT_EQ(to_string(EmbeddedPreviewStatus::Found), "found");
    EXPECT_EQ(to_string(EmbeddedPreviewStatus::NoneInFile), "none_in_file");
    EXPECT_EQ(to_string(EmbeddedPreviewStatus::NoneUsable), "none_usable");
}

// ---- Camera fixtures (smoke tier) -----------------------------------------------------------

class EmbeddedPreviewFixtures : public ::testing::TestWithParam<const char*> {};

TEST_P(EmbeddedPreviewFixtures, ExtractsALargeCameraJpegWithoutUnpackingTheRawData) {
    LUCENT_REQUIRE_FIXTURE(path, GetParam());
    const auto start = std::chrono::steady_clock::now();
    auto result = extract_embedded_preview(path);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(result) << result.error().describe();
    for (const PreviewCandidate& c : result->candidates) {
        std::printf("  candidate %u: %d x %d, %llu bytes at %llu: %s %s\n", c.list_index, c.width, c.height,
                    static_cast<unsigned long long>(c.byte_length),
                    static_cast<unsigned long long>(c.file_offset),
                    c.problem ? std::string(to_string(*c.problem)).c_str() : "usable", c.detail.c_str());
    }
    ASSERT_EQ(result->status, EmbeddedPreviewStatus::Found);
    const EmbeddedPreview& p = *result->preview;
    std::printf("  chosen: %d x %d, %zu bytes, orientation %s, %.3f s\n", p.width, p.height, p.bytes.size(),
                std::string(image::to_string(p.orientation)).c_str(), seconds);
    RecordProperty("preview_width", p.width);
    RecordProperty("preview_height", p.height);
    RecordProperty("milliseconds", static_cast<int>(seconds * 1000.0));

    ASSERT_GE(p.bytes.size(), 4u);
    EXPECT_EQ(p.bytes[0], std::byte{0xFF});
    EXPECT_EQ(p.bytes[1], std::byte{0xD8});
    EXPECT_TRUE(p.components == 1 || p.components == 3);

    // A browsing-size preview whose aspect ratio matches the RAW image (within the rounding of
    // preview dimensions and small sensor-margin differences).
    auto decoder = create_libraw_decoder();
    ASSERT_TRUE(decoder);
    auto probe = (*decoder)->probe(path, ProbeOptions{}, {});
    ASSERT_TRUE(probe) << probe.error().describe();
    const image::PixelRect& visible = probe->metadata.geometry.visible_area;
    EXPECT_GE(std::max(p.width, p.height), 640);
    const double raw_aspect =
        double(std::max(visible.width, visible.height)) / std::min(visible.width, visible.height);
    const double preview_aspect = double(std::max(p.width, p.height)) / std::min(p.width, p.height);
    EXPECT_NEAR(preview_aspect, raw_aspect, 0.1);
    EXPECT_EQ(p.orientation, probe->metadata.geometry.orientation);
    EXPECT_EQ(result->source_identity, probe->metadata.source.identity);

    // For a thumbnail (the contact sheet asks for 512 px, 768 px at 150 % display scale): the
    // smallest preview at least that large, with the RAW's aspect ratio; never larger than the
    // largest one.
    EmbeddedPreviewOptions thumbnail;
    thumbnail.min_long_edge = 768;
    auto small = extract_embedded_preview(path, thumbnail);
    ASSERT_TRUE(small) << small.error().describe();
    ASSERT_EQ(small->status, EmbeddedPreviewStatus::Found);
    const EmbeddedPreview& t = *small->preview;
    std::printf("  for a 768 px thumbnail: %d x %d, %zu bytes\n", t.width, t.height, t.bytes.size());
    EXPECT_GE(std::max(t.width, t.height), 768);
    EXPECT_LE(std::uint64_t(t.width) * std::uint64_t(t.height), std::uint64_t(p.width) * std::uint64_t(p.height));
    EXPECT_NEAR(double(std::max(t.width, t.height)) / std::min(t.width, t.height), preview_aspect,
                preview_aspect * EmbeddedPreviewOptions::kAspectTolerance);
    EXPECT_EQ(t.orientation, p.orientation);
}

INSTANTIATE_TEST_SUITE_P(Smoke, EmbeddedPreviewFixtures,
                         ::testing::Values("canon-eos-r5-cr3-raw", "sony-ilce-7m4-arw-compressed",
                                           "dji-fc4382-dng", "fujifilm-x-t4-raf-lossless",
                                           "nikon-z-6-nef-lossless"),
                         [](const ::testing::TestParamInfo<const char*>& info) {
                             std::string name = info.param;
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });

}  // namespace
}  // namespace lucent::raw
