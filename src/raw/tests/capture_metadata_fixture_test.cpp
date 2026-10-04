// read_capture_metadata() on the downloaded camera fixtures (tools\fetch-fixtures.ps1),
// cross-checked against the values LibRaw reports for the same files. Every test skips with a
// clear message when its fixtures are not in fixtures/cache.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/raw/capture_metadata.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/libraw_decoder.hpp>

#include "raw_test_support.hpp"

namespace lucent::raw {
namespace {

std::vector<std::filesystem::path> cached_fixtures() {
    std::vector<std::filesystem::path> files;
    const std::filesystem::path root = std::filesystem::path(LUCENT_FIXTURE_DIR) / "cache";
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return files;
    }
    for (const auto& dir : std::filesystem::directory_iterator(root, ec)) {
        if (auto file = test_support::fixture_file(dir.path().filename().string())) {
            files.push_back(*file);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// LibRaw reports floats; the EXIF fractions must agree to 0.1 % (exposure times such as 1/3200
// are stored by LibRaw as the float nearest the fraction).
void expect_close(const std::optional<ExifRational>& exif, const std::optional<float>& libraw, const char* what) {
    if (!exif || !libraw) {
        return;
    }
    EXPECT_NEAR(exif->value(), static_cast<double>(*libraw), 1e-3 * static_cast<double>(*libraw)) << what;
}

TEST(CaptureMetadataFixtures, EveryCachedFixtureAgreesWithLibRaw) {
    const auto files = cached_fixtures();
    if (files.empty()) {
        GTEST_SKIP() << "no fixture is downloaded; run tools\\fetch-fixtures.ps1";
    }
    auto decoder = create_libraw_decoder();
    ASSERT_TRUE(decoder);
    int with_time = 0;
    for (const auto& path : files) {
        SCOPED_TRACE(path.parent_path().filename().string());
        auto m = read_capture_metadata(path);
        ASSERT_TRUE(m) << m.error().describe();
        auto probe = (*decoder)->probe(path, {}, {});
        ASSERT_TRUE(probe) << probe.error().describe();
        const RawMetadata& p = probe->metadata;

        // Every container Lucent supports records at least the camera.
        EXPECT_FALSE(m->make.empty());
        EXPECT_FALSE(m->model.empty());
        if (m->iso && p.capture.iso) {
            EXPECT_EQ(static_cast<float>(*m->iso), *p.capture.iso);
        }
        expect_close(m->exposure_time_s, p.capture.exposure_time_s, "exposure time");
        expect_close(m->f_number, p.capture.f_number, "f-number");
        expect_close(m->focal_length_mm, p.capture.focal_length_mm, "focal length");
        if (m->capture_time && p.capture.capture_time_local) {
            const ExifDateTime& t = *m->capture_time;
            EXPECT_EQ(std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", t.year, t.month, t.day, t.hour, t.minute,
                                  t.second),
                      *p.capture.capture_time_local);
            ++with_time;
        }
        if (m->gps) {
            EXPECT_LE(std::abs(m->gps->latitude_deg), 90.0);
            EXPECT_LE(std::abs(m->gps->longitude_deg), 180.0);
        }

        // The decoder fallback never replaces what the file's EXIF says.
        CaptureMetadata completed = *m;
        complete_from_decoder(completed, p);
        EXPECT_EQ(completed.make, m->make);
        if (m->exposure_time_s) {
            EXPECT_EQ(completed.exposure_time_s, m->exposure_time_s);
        }
    }
    EXPECT_GT(with_time, 0);
}

TEST(CaptureMetadataFixtures, CanonR5Cr3) {
    LUCENT_REQUIRE_FIXTURE(path, "canon-eos-r5-cr3-raw");
    auto m = read_capture_metadata(path);
    ASSERT_TRUE(m) << m.error().describe();
    EXPECT_EQ(m->make, "Canon");
    EXPECT_EQ(m->model, "Canon EOS R5");
    EXPECT_EQ(m->lens_model, "50mm F1.4 DG HSM | Art 014");
    EXPECT_EQ(m->artist, "Benjamin Grimm-Lebsanft");
    EXPECT_EQ(m->copyright, "CC0");
    EXPECT_EQ(m->iso, 100u);
    ASSERT_TRUE(m->exposure_time_s);
    EXPECT_DOUBLE_EQ(m->exposure_time_s->value(), 0.3);
    ASSERT_TRUE(m->capture_time);
    EXPECT_EQ(*m->capture_time, (ExifDateTime{2021, 5, 15, 13, 2, 2, "01", 60}));
    EXPECT_FALSE(m->gps);
    EXPECT_TRUE(m->warnings.empty());
}

TEST(CaptureMetadataFixtures, DjiDngGpsAndNoFixZeros) {
    LUCENT_REQUIRE_FIXTURE(path, "dji-fc4382-dng");
    auto m = read_capture_metadata(path);
    ASSERT_TRUE(m) << m.error().describe();
    ASSERT_TRUE(m->gps);
    EXPECT_NEAR(m->gps->latitude_deg, 50.9684856, 1e-6);
    EXPECT_NEAR(m->gps->longitude_deg, 10.3145608, 1e-6);
    ASSERT_TRUE(m->gps->altitude_m);
    EXPECT_NEAR(*m->gps->altitude_m, 446.648, 1e-3);

    const auto zeros = test_support::fixture_file("dji-fc220-dng");
    if (zeros) {
        auto z = read_capture_metadata(*zeros);
        ASSERT_TRUE(z) << z.error().describe();
        EXPECT_FALSE(z->gps);  // the camera wrote 0,0 without a fix
    }
}

TEST(CaptureMetadataFixtures, DecoderFillsWhatTheFileLacks) {
    LUCENT_REQUIRE_FIXTURE(path, "insta360-one-x-dng");
    auto m = read_capture_metadata(path);
    ASSERT_TRUE(m) << m.error().describe();
    EXPECT_FALSE(m->exposure_time_s);  // no Exif IFD in IFD 0
    auto decoder = create_libraw_decoder();
    ASSERT_TRUE(decoder);
    auto probe = (*decoder)->probe(path, {}, {});
    ASSERT_TRUE(probe) << probe.error().describe();
    complete_from_decoder(*m, probe->metadata);
    EXPECT_TRUE(m->exposure_time_s);
    EXPECT_TRUE(m->capture_time);
}

}  // namespace
}  // namespace lucent::raw
