// Output profiles: ICC generation, determinism and colorimetry checked with an independent
// parse through Little CMS and against lucent_image's colour math.

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <lcms2.h>

#include <lucent/export/output_profile.hpp>
#include <lucent/image/color_math.hpp>

#include "icc_builder.hpp"

namespace lucent::exporter {
namespace {

std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (std::uint32_t{bytes[offset]} << 24) | (std::uint32_t{bytes[offset + 1]} << 16) |
           (std::uint32_t{bytes[offset + 2]} << 8) | std::uint32_t{bytes[offset + 3]};
}

std::uint16_t be16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}

struct ProfileCloser {
    void operator()(void* profile) const noexcept { cmsCloseProfile(profile); }
};
using Profile = std::unique_ptr<void, ProfileCloser>;

Profile open(std::span<const std::uint8_t> bytes) {
    return Profile(cmsOpenProfileFromMem(bytes.data(), static_cast<cmsUInt32Number>(bytes.size())));
}

class OutputProfileTest : public ::testing::TestWithParam<OutputProfile> {};

TEST_P(OutputProfileTest, CachedBytesAreStableAndIdenticalToAFreshBuild) {
    const OutputProfile profile = GetParam();
    auto first = icc_profile(profile);
    ASSERT_TRUE(first) << first.error().describe();
    auto second = icc_profile(profile);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->data(), second->data()) << "the cache must hand out the same storage";

    // A fresh build in a new Little CMS context (a different creation time) gives the same bytes.
    auto encoding = output_encoding(profile);
    ASSERT_TRUE(encoding);
    auto primaries = image::rgb_primaries(encoding->primaries);
    ASSERT_TRUE(primaries);
    auto rebuilt = detail::build_display_icc(*primaries, image::kD65White, icc_description(profile));
    ASSERT_TRUE(rebuilt) << rebuilt.error().describe();
    ASSERT_EQ(rebuilt->size(), first->size());
    EXPECT_EQ(std::memcmp(rebuilt->data(), first->data(), first->size()), 0);
}

TEST_P(OutputProfileTest, HeaderDeclaresAV4DisplayRgbProfileWithFixedDate) {
    auto bytes = icc_profile(GetParam());
    ASSERT_TRUE(bytes);
    ASSERT_GE(bytes->size(), 132u);
    EXPECT_EQ(be32(*bytes, 0), bytes->size()) << "profile size field";
    EXPECT_EQ((*bytes)[8], 4) << "major version";
    EXPECT_EQ((*bytes)[9], 0x30) << "minor.bugfix version 3.0";
    EXPECT_EQ(be32(*bytes, 12), 0x6D6E7472u) << "'mntr' display class";
    EXPECT_EQ(be32(*bytes, 16), 0x52474220u) << "'RGB ' colour space";
    EXPECT_EQ(be32(*bytes, 20), 0x58595A20u) << "'XYZ ' PCS";
    EXPECT_EQ(be32(*bytes, 36), 0x61637370u) << "'acsp' signature";
    const std::uint16_t expected_date[6] = {2026, 1, 1, 0, 0, 0};
    for (std::size_t i = 0; i < 6; ++i) {
        EXPECT_EQ(be16(*bytes, 24 + 2 * i), expected_date[i]) << "date field " << i;
    }
    for (std::size_t i = 84; i < 100; ++i) {
        EXPECT_EQ((*bytes)[i], 0) << "profile ID must be left zero (not computed)";
    }
}

TEST_P(OutputProfileTest, IndependentParseMatchesColorimetry) {
    const OutputProfile profile = GetParam();
    auto bytes = icc_profile(profile);
    ASSERT_TRUE(bytes);
    Profile parsed = open(*bytes);
    ASSERT_TRUE(parsed);

    char description[256] = {};
    ASSERT_GT(cmsGetProfileInfoASCII(parsed.get(), cmsInfoDescription, "en", "US", description, sizeof(description)),
              0u);
    EXPECT_EQ(std::string(description), std::string(icc_description(profile)));
    char copyright[256] = {};
    ASSERT_GT(cmsGetProfileInfoASCII(parsed.get(), cmsInfoCopyright, "en", "US", copyright, sizeof(copyright)), 0u);
    EXPECT_EQ(std::string(copyright), "No copyright, use freely");

    // Expected colorants: lucent_image's normalised primary matrix for D65, Bradford-adapted to
    // the ICC D50 PCS white (0.9642, 1, 0.8249). Tolerance 1e-4: colorants are stored as
    // s15Fixed16 (resolution 1.5e-5) and Little CMS rounds its own adaptation matrix.
    auto encoding = output_encoding(profile);
    ASSERT_TRUE(encoding);
    auto to_xyz = image::to_xyz_matrix(*encoding);
    ASSERT_TRUE(to_xyz);
    auto d65 = image::xy_to_XYZ(image::kD65White);
    ASSERT_TRUE(d65);
    auto adapt = image::chromatic_adaptation(*d65, image::Vec3d{0.9642, 1.0, 0.8249});
    ASSERT_TRUE(adapt);
    const image::Matrix3d expected = *adapt * *to_xyz;
    const cmsTagSignature tags[3] = {cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag};
    for (std::size_t c = 0; c < 3; ++c) {
        const auto* xyz = static_cast<const cmsCIEXYZ*>(cmsReadTag(parsed.get(), tags[c]));
        ASSERT_NE(xyz, nullptr);
        EXPECT_NEAR(xyz->X, expected(0, c), 1e-4) << "channel " << c;
        EXPECT_NEAR(xyz->Y, expected(1, c), 1e-4) << "channel " << c;
        EXPECT_NEAR(xyz->Z, expected(2, c), 1e-4) << "channel " << c;
    }

    // Tone curves: parametric sRGB on every channel. Parameters are s15Fixed16 (1.5e-5).
    const cmsTagSignature trcs[3] = {cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag};
    for (const cmsTagSignature trc : trcs) {
        const auto* curve = static_cast<const cmsToneCurve*>(cmsReadTag(parsed.get(), trc));
        ASSERT_NE(curve, nullptr);
        EXPECT_EQ(cmsGetToneCurveParametricType(curve), 4);
        const cmsCurveSegment* segment = cmsGetToneCurveSegment(0, curve);
        ASSERT_NE(segment, nullptr);
        const cmsFloat64Number* params = segment->Params;
        EXPECT_NEAR(params[0], 2.4, 2e-5);
        EXPECT_NEAR(params[1], 1.0 / 1.055, 2e-5);
        EXPECT_NEAR(params[2], 0.055 / 1.055, 2e-5);
        EXPECT_NEAR(params[3], 1.0 / 12.92, 2e-5);
        EXPECT_NEAR(params[4], 0.04045, 2e-5);
        for (const float v : {0.0f, 0.02f, 0.2f, 0.5f, 0.9f, 1.0f}) {
            EXPECT_NEAR(cmsEvalToneCurveFloat(curve, v), image::srgb_decode(v), 1e-4) << "at " << v;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Profiles, OutputProfileTest,
                         ::testing::Values(OutputProfile::Srgb, OutputProfile::DisplayP3),
                         [](const auto& info) { return info.param == OutputProfile::Srgb ? "Srgb" : "DisplayP3"; });

TEST(OutputProfile, ProfilesDiffer) {
    auto srgb = icc_profile(OutputProfile::Srgb);
    auto p3 = icc_profile(OutputProfile::DisplayP3);
    ASSERT_TRUE(srgb);
    ASSERT_TRUE(p3);
    EXPECT_FALSE(srgb->size() == p3->size() && std::memcmp(srgb->data(), p3->data(), srgb->size()) == 0);
}

TEST(OutputProfile, OutputEncodingDescribesDisplayReferredOutput) {
    auto encoding = output_encoding(OutputProfile::DisplayP3);
    ASSERT_TRUE(encoding);
    EXPECT_EQ(encoding->primaries, image::ColorPrimaries::DisplayP3);
    EXPECT_EQ(encoding->white_point, image::WhitePoint::D65);
    EXPECT_EQ(encoding->transfer, image::TransferFunction::Srgb);
    EXPECT_EQ(encoding->range, image::NumericRange::UnitClamped);
    EXPECT_EQ(encoding->reference, image::ImageReference::Display);
    EXPECT_EQ(*output_encoding(OutputProfile::Srgb), image::ColorEncoding::srgb_display());
}

TEST(OutputProfile, PersistedValuesAndNamesAreStable) {
    EXPECT_EQ(static_cast<int>(OutputProfile::Srgb), 1);
    EXPECT_EQ(static_cast<int>(OutputProfile::DisplayP3), 2);
    EXPECT_EQ(static_cast<int>(RenderingIntent::Perceptual), 1);
    EXPECT_EQ(static_cast<int>(RenderingIntent::RelativeColorimetric), 2);
    EXPECT_EQ(static_cast<int>(RenderingIntent::Saturation), 3);
    EXPECT_EQ(static_cast<int>(RenderingIntent::AbsoluteColorimetric), 4);
    EXPECT_EQ(static_cast<int>(BitDepth::Eight), 8);
    EXPECT_EQ(static_cast<int>(BitDepth::Sixteen), 16);
    EXPECT_EQ(to_string(OutputProfile::DisplayP3), "display-p3");
    EXPECT_EQ(to_string(RenderingIntent::RelativeColorimetric), "relative-colorimetric");
    EXPECT_EQ(to_string(BitDepth::Sixteen), "16");
}

TEST(OutputProfile, UnknownValuesAreRejected) {
    const auto bogus_profile = static_cast<OutputProfile>(9);
    EXPECT_EQ(icc_profile(bogus_profile).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(output_encoding(bogus_profile).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(validate(bogus_profile).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(validate(static_cast<RenderingIntent>(0)).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(validate(static_cast<BitDepth>(12)).error().code, ErrorCode::InvalidArgument);
    EXPECT_TRUE(icc_description(bogus_profile).empty());
    EXPECT_EQ(to_string(bogus_profile), "unknown");
}

}  // namespace
}  // namespace lucent::exporter
