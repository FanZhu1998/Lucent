// Native crops (native_crops.hpp): the measurements read only NativeCrop::measured, so settings
// without local tone, rendered on the halo-free crop, measure exactly as on the crop with the
// local-tone halo (the recheck speed-up of ADR 0011 phase 2); with local tone the halo matters.

#include <cmath>
#include <cstdio>

#include <gtest/gtest.h>

#include <lucent/analysis/native_crops.hpp>
#include <lucent/engine/develop.hpp>

#include "../src/stage_one_copy.hpp"
#include "analysis_test_support.hpp"

namespace lucent::analysis {
namespace {

void expect_same(const CropMeasurement& a, const CropMeasurement& b) {
    EXPECT_EQ(a.kind, b.kind);
    EXPECT_EQ(a.pixels, b.pixels);
    EXPECT_EQ(a.texture_lstar, b.texture_lstar);
    EXPECT_EQ(a.noise_lstar, b.noise_lstar);
    EXPECT_EQ(a.edge_samples, b.edge_samples);
    EXPECT_EQ(a.halo_lstar, b.halo_lstar);
    EXPECT_EQ(a.highlight_clipped, b.highlight_clipped);
    EXPECT_EQ(a.new_highlight_clipped, b.new_highlight_clipped);
    EXPECT_EQ(a.nonfinite_samples, b.nonfinite_samples);
}

struct Fixture {
    engine::StageOneImage full;
    NativeCrop crop;
    engine::StageOneImage halo;
    engine::StageOneImage halo_free;
};

Fixture make_fixture(image::PixelRect evaluation_origin) {
    Fixture f;
    engine::StageOneRequest request;
    request.kind = engine::StageOneKind::Interactive;
    auto one = engine::render_stage_one(test_support::make_scene(test_support::chart_scene(0.6, 1.05, 0.95)),
                                        request);
    EXPECT_TRUE(one) << one.error().describe();
    f.full = std::move(*one);
    const image::Size size = f.full.full_size;
    f.crop.kind = CropKind::Edges;
    f.crop.evaluation = evaluation_origin;
    const std::int32_t halo = native_crop_halo(size);
    f.crop.rendered = image::PixelRect{evaluation_origin.x - halo, evaluation_origin.y - halo,
                                       evaluation_origin.width + 2 * halo, evaluation_origin.height + 2 * halo}
                          .intersect(image::PixelRect::from_size(size));
    f.crop.measured =
        image::PixelRect{evaluation_origin.x - kNativeMeasureMargin, evaluation_origin.y - kNativeMeasureMargin,
                         evaluation_origin.width + 2 * kNativeMeasureMargin,
                         evaluation_origin.height + 2 * kNativeMeasureMargin}
            .intersect(image::PixelRect::from_size(size));
    auto a = detail::crop_stage_one(f.full, f.crop.rendered, {});
    auto b = detail::crop_stage_one(f.full, f.crop.measured, {});
    EXPECT_TRUE(a && b);
    f.halo = std::move(*a);
    f.halo_free = std::move(*b);
    return f;
}

Expected<CropMeasurement> measure(const engine::StageOneImage& image, const image::PixelRect& rect,
                                  const NativeCrop& crop, const engine::DevelopSettings& settings,
                                  const CropFeatures* baseline, CropFeatures* features) {
    auto rendered = engine::render_stage_two(image, settings);
    if (!rendered) {
        return Unexpected(std::move(rendered.error()));
    }
    return measure_crop(rendered->display, rect, crop, baseline, features);
}

class HaloFreeCrops : public ::testing::TestWithParam<image::PixelRect> {};

TEST_P(HaloFreeCrops, MeasureExactlyAsWithTheHaloWithoutLocalTone) {
    const Fixture f = make_fixture(GetParam());
    ASSERT_TRUE(f.crop.rendered.contains(f.crop.measured));
    ASSERT_LT(f.crop.measured.size().area(), f.crop.rendered.size().area());
    const engine::DevelopSettings baseline;
    CropFeatures features_halo;
    CropFeatures features_free;
    auto base_halo = measure(f.halo, f.crop.rendered, f.crop, baseline, nullptr, &features_halo);
    auto base_free = measure(f.halo_free, f.crop.measured, f.crop, baseline, nullptr, &features_free);
    ASSERT_TRUE(base_halo && base_free);
    expect_same(*base_halo, *base_free);
    ASSERT_EQ(features_halo.edges.size(), features_free.edges.size());
    EXPECT_GT(features_halo.edges.size(), 16u);
    EXPECT_EQ(features_halo.clipped, features_free.clipped);
    for (std::size_t k = 0; k < features_halo.edges.size(); ++k) {
        EXPECT_EQ(features_halo.edges[k].x, features_free.edges[k].x);
        EXPECT_EQ(features_halo.edges[k].near_plus, features_free.edges[k].near_plus);
        EXPECT_EQ(features_halo.edges[k].far_minus, features_free.edges[k].far_minus);
    }

    // Exposure, white balance, contrast and saturation: per pixel.
    engine::DevelopSettings edit;
    edit.exposure_ev = 0.8;
    edit.contrast = 0.12;
    edit.saturation = 0.1;
    edit.white_balance = engine::CameraNeutralWhiteBalance{2.0 * 1.04, 1.0, 1.5 * 0.96};
    auto halo = measure(f.halo, f.crop.rendered, f.crop, edit, &features_halo, nullptr);
    auto free = measure(f.halo_free, f.crop.measured, f.crop, edit, &features_halo, nullptr);
    ASSERT_TRUE(halo && free);
    expect_same(*halo, *free);
    EXPECT_GT(halo->texture_lstar, 0.0);
    std::printf("[ CROPS    ] %dx%d halo crop, %dx%d halo-free: texture %.4f L*, halo %.4f L*, %llu edges\n",
                f.crop.rendered.width, f.crop.rendered.height, f.crop.measured.width, f.crop.measured.height,
                halo->texture_lstar, halo->halo_lstar, static_cast<unsigned long long>(halo->edge_samples));

    // Local tone needs the halo: the guided filter sees beyond the measured region.
    engine::DevelopSettings local = edit;
    local.shadows = 0.3;
    local.highlights = -0.3;
    auto with_halo = measure(f.halo, f.crop.rendered, f.crop, local, &features_halo, nullptr);
    auto without = measure(f.halo_free, f.crop.measured, f.crop, local, &features_halo, nullptr);
    ASSERT_TRUE(with_halo && without);
    EXPECT_NE(with_halo->texture_lstar, without->texture_lstar);
}

// An interior crop and one against the image's left border (where both rectangles are clipped by
// the image border), each over chart patch edges.
INSTANTIATE_TEST_SUITE_P(Positions, HaloFreeCrops,
                         ::testing::Values(image::PixelRect{400, 260, 128, 128}, image::PixelRect{0, 150, 160, 128}));

TEST(NativeCrops, RefusesARenderThatMissesTheMeasuredRegion) {
    const Fixture f = make_fixture(image::PixelRect{400, 260, 128, 128});
    auto rendered = engine::render_stage_two(f.halo_free, engine::DevelopSettings{});
    ASSERT_TRUE(rendered);
    // Claiming the halo-free render is the full crop: wrong size.
    auto wrong = measure_crop(rendered->display, f.crop, nullptr, nullptr);
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, ErrorCode::InvalidArgument);
    // A rectangle that does not cover the measured region.
    image::PixelRect shifted = f.crop.measured;
    shifted.x += 8;
    auto missing = measure_crop(rendered->display, shifted, f.crop, nullptr, nullptr);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace lucent::analysis
