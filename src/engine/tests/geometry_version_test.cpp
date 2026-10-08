// Geometry operation versions and the camera's image area (image_area.hpp, develop_geometry.hpp,
// develop.hpp, render.hpp; ADR 0017).
//
// Pinned renders: every hash below was produced by the engine BEFORE the camera image area and
// geometry version 2 existed (the W10 phase-1 engine, commit 3a836ac: a `git archive` of it built
// with a test rendering the same synthetic scenes and settings, without the contract-1.4 image
// area; checked 2026-10-07). Settings whose geometry operation is version 1, version-1 settings and
// the W2 render with the camera area switched off must keep rendering exactly those bytes, now
// that the metadata declares an image area (an inset crop, or DNG DefaultCrop tags with odd
// ActiveArea margins and OpcodeList3 opcodes).

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/engine/develop.hpp>
#include <lucent/engine/image_area.hpp>
#include <lucent/engine/orientation.hpp>
#include <lucent/engine/render.hpp>

#include "develop_test_support.hpp"
#include "dng_opcode_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;

// FNV-1a 64 over the size, the channel count and every sample's bit pattern.
std::uint64_t pixel_hash(const image::ImageBuffer<float>& pixels) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto mix = [&h](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (8 * i)) & 0xffu;
            h *= 0x100000001b3ull;
        }
    };
    mix(static_cast<std::uint32_t>(pixels.width()));
    mix(static_cast<std::uint32_t>(pixels.height()));
    mix(static_cast<std::uint32_t>(pixels.channels()));
    for (std::int32_t y = 0; y < pixels.height(); ++y) {
        const float* row = pixels.row(y);
        for (std::int32_t k = 0; k < pixels.width() * pixels.channels(); ++k) {
            mix(std::bit_cast<std::uint32_t>(row[k]));
        }
    }
    return h;
}

// A textured, coloured RGGB mosaic of 100 x 72 with a 2-pixel masked margin (visible 96 x 68 at
// (2, 2)), R5 decoder matrix.
raw::DecodedRaw plain_scene(image::Orientation orientation = image::Orientation::Normal) {
    const image::CfaPattern cfa = image::CfaPattern::bayer(image::BayerLayout::Rggb);
    const image::PixelRect visible{2, 2, 96, 68};
    constexpr std::array<double, 3> as_shot{2.0, 1.0, 1.5};
    auto decoded = ts::make_mosaic<std::uint16_t>(
        100, 72, visible, cfa, ts::simple_levels(ts::kDevBlack, ts::kDevWhite),
        [&](std::int32_t x, std::int32_t y, std::int32_t) {
            const std::int32_t vx = x - visible.x;
            const std::int32_t vy = y - visible.y;
            const auto color = cfa.color_at(vy, vx);
            const std::size_t c = color == image::CfaColor::Red ? 0 : color == image::CfaColor::Blue ? 2 : 1;
            const double base = 0.03 + 0.25 * (0.5 + 0.5 * std::sin(0.09 * vx) * std::cos(0.07 * vy)) +
                                0.01 * ((vx * 7 + vy * 13) % 5);
            const double n = 0.03 * std::sin(12.9898 * vx + 78.233 * vy);
            const std::array<double, 3> rgb{0.6 * base * (1.0 + n), base, 0.35 * base * (1.0 - n)};
            return std::round(ts::kDevBlack + rgb[c] * (ts::kDevWhite - ts::kDevBlack));
        });
    ts::set_decoder_matrix(decoded.metadata, ts::kR5CamXyz);
    ts::set_as_shot(decoded.metadata, as_shot);
    decoded.metadata.geometry.orientation = orientation;
    return decoded;
}

// A decoder inset crop of 88 x 60 at raw-frame (6, 5): visible-area pixels (4, 3) .. (92, 63).
constexpr image::PixelRect kInsetInVisible{4, 3, 88, 60};

raw::ImageArea inset_area() {
    raw::ImageArea a;
    a.source = raw::ImageAreaSource::DecoderInsetCrop;
    a.provenance = raw::Provenance::Decoder;
    a.rect = raw::AreaRect{6.0, 5.0, 88.0, 60.0};
    a.reference = image::PixelRect{0, 0, 100, 72};
    a.basis = "synthetic inset crop";
    return a;
}

raw::DecodedRaw pinned_scene() {
    raw::DecodedRaw d = plain_scene();
    d.metadata.image_area = inset_area();
    return d;
}

DevelopSettings masked_settings() {
    DevelopSettings s;
    s.exposure_ev = 0.3;
    s.shadows = 0.3;
    s.chroma_denoise.strength = 0.6;
    s.chroma_denoise.radius = 4.0;
    LocalAdjustment radial;
    radial.id = 1;
    RadialGradientMask r;
    r.center = {0.4, 0.45};
    r.radius_x = 0.22;
    r.radius_y = 0.14;
    r.angle_degrees = 25.0;
    r.feather = 0.6;
    radial.shape = r;
    radial.controls.exposure_ev = 0.6;
    radial.controls.saturation = 0.3;
    LocalAdjustment brush;
    brush.id = 4;
    BrushStroke stroke;
    stroke.points = {{0.1, 0.8}, {0.45, 0.7}, {0.85, 0.88}};
    stroke.radius = 0.07;
    stroke.hardness = 0.4;
    stroke.flow = 0.9;
    brush.shape = BrushMask{{stroke}};
    brush.controls.exposure_ev = -0.4;
    brush.controls.contrast = 0.3;
    s.local_adjustments = {radial, brush};
    return s;
}

DevelopSettings pinned_settings() {
    DevelopSettings s = masked_settings();
    s.geometry.straighten_degrees = 3.5;
    s.geometry.crop = CropRect{0.08, 0.06, 0.93, 0.9};
    s.geometry.quarter_turns = 1;
    s.geometry.flip_horizontal = true;
    s.operations.geometry = 1;
    return s;
}

StageOneImage stage_one(raw::DecodedRaw&& decoded, std::int32_t proxy = 0) {
    StageOneRequest request;
    request.proxy_long_edge = proxy;
    auto one = render_stage_one(std::move(decoded), request, DevelopOptions{});
    EXPECT_TRUE(one) << one.error().describe();
    return std::move(*one);
}

void expect_hash(const char* name, const image::ImageBuffer<float>& pixels, std::uint64_t expected) {
    const std::uint64_t h = pixel_hash(pixels);
    EXPECT_EQ(h, expected) << name << " " << pixels.width() << "x" << pixels.height() << " 0x" << std::hex
                           << h;
}

// `big` holds `small` at (x0, y0), bit for bit.
void expect_sub_image(const image::ImageBuffer<float>& big, const image::ImageBuffer<float>& small,
                      std::int32_t x0, std::int32_t y0) {
    ASSERT_EQ(big.channels(), small.channels());
    ASSERT_LE(x0 + small.width(), big.width());
    ASSERT_LE(y0 + small.height(), big.height());
    for (std::int32_t y = 0; y < small.height(); ++y) {
        for (std::int32_t k = 0; k < small.width() * small.channels(); ++k) {
            ASSERT_EQ(std::bit_cast<std::uint32_t>(small.row(y)[k]),
                      std::bit_cast<std::uint32_t>(big.row(y + y0)[k + x0 * big.channels()]))
                << "at " << k / small.channels() << "," << y;
        }
    }
}

// ---- Version 1: pinned -------------------------------------------------------------------------

TEST(GeometryVersions, Version1RendersArePinned) {
    // Straightened, cropped, turned and mirrored, with masks and chroma denoising.
    {
        StageOneImage one = stage_one(pinned_scene());
        ASSERT_TRUE(one.image_area.has_value());  // the area is known, and version 1 ignores it
        auto r = render_stage_two(one, pinned_settings());
        ASSERT_TRUE(r) << r.error().describe();
        expect_hash("interactive_v1_straighten", r->display.pixels, 0x59ae8a99415df989ull);
        EXPECT_EQ(r->report.geometry_version, 1u);
        EXPECT_EQ(r->report.geometry_frame, image::PixelRect::from_size(one.pixels.size()));
        EXPECT_EQ(r->report.operations.back().version, 1u);
    }
    // A whole-pixel crop and three quarter turns.
    {
        StageOneImage one = stage_one(pinned_scene());
        DevelopSettings s = pinned_settings();
        s.geometry.straighten_degrees = 0.0;
        s.geometry.crop = CropRect{0.1, 0.2, 0.8, 0.95};
        s.geometry.quarter_turns = 3;
        s.geometry.flip_horizontal = false;
        auto r = render_stage_two(one, s);
        ASSERT_TRUE(r) << r.error().describe();
        expect_hash("interactive_v1_crop", r->display.pixels, 0x7719aa64288ffd0bull);
    }
    // The identity geometry of version 1: the whole visible area.
    {
        StageOneImage one = stage_one(pinned_scene());
        DevelopSettings s;
        s.operations.geometry = 1;
        auto r = render_stage_two(one, s);
        ASSERT_TRUE(r) << r.error().describe();
        expect_hash("interactive_v1_identity", r->display.pixels, 0x4d881ca61c97501cull);
        EXPECT_FALSE(r->report.operations.back().applied);
    }
    // A proxy.
    {
        StageOneImage one = stage_one(pinned_scene(), 40);
        auto r = render_stage_two(one, pinned_settings());
        ASSERT_TRUE(r) << r.error().describe();
        expect_hash("proxy_v1_straighten", r->display.pixels, 0x5d5d9ae2c99f3224ull);
    }
    // The export render.
    {
        auto r = render_develop(pinned_scene(), pinned_settings());
        ASSERT_TRUE(r) << r.error().describe();
        expect_hash("export_v1_straighten", r->display.pixels, 0x59ae8a99415df989ull);
    }
    // Version-1 settings (no geometry member at all; their operations.geometry is the default 2,
    // unused: they render as geometry version 1).
    {
        DevelopSettings v1;
        v1.version = 1;
        v1.exposure_ev = 0.4;
        v1.shadows = 0.3;
        ASSERT_EQ(v1.operations.geometry, 2u);
        auto r = render_develop(pinned_scene(), v1);
        ASSERT_TRUE(r) << r.error().describe();
        expect_hash("export_settings_v1", r->display.pixels, 0x55fbc92351f0bd19ull);
        EXPECT_EQ(r->report.geometry_version, 1u);
        auto upgraded = upgrade_develop_settings(v1);
        ASSERT_TRUE(upgraded);
        auto u = render_develop(pinned_scene(), *upgraded);
        ASSERT_TRUE(u);
        expect_hash("export_settings_v1_upgraded", u->display.pixels, 0x55fbc92351f0bd19ull);
    }
}

TEST(GeometryVersions, W2RenderWithoutTheCameraAreaIsPinned) {
    RenderSettings settings;
    settings.apply_camera_area = false;
    RenderOptions options;
    options.keep_scene_linear = true;
    auto r = render_decoded(pinned_scene(), settings, options);
    ASSERT_TRUE(r) << r.error().describe();
    expect_hash("w2_display", r->display.pixels, 0xc24fd2ef44873f6eull);
    ASSERT_TRUE(r->scene_linear.has_value());
    expect_hash("w2_scene_linear", r->scene_linear->pixels, 0xc24fd2ef44873f6eull);
    EXPECT_FALSE(r->report.camera_area_applied);
    ASSERT_TRUE(r->report.image_area.has_value());
    EXPECT_EQ(r->report.image_area->rect, kInsetInVisible);
    bool warned = false;
    for (const std::string& w : r->report.warnings) {
        warned = warned || w.find("is not applied (settings)") != std::string::npos;
    }
    EXPECT_TRUE(warned);
    for (const StageRecord& s : r->report.stages) {
        EXPECT_NE(s.stage, RenderStage::CameraArea);
    }
}

// ---- Version 2 ------------------------------------------------------------------------------------

TEST(GeometryVersions, Version2RendersTheCameraAreaOfTheSameRendering) {
    // Masks, denoising and the guided filters work on the whole upright image in both versions,
    // so the version-2 identity is exactly the camera area's pixels of the version-1 rendering:
    // every mask stays on the same content.
    StageOneImage one = stage_one(pinned_scene());
    ASSERT_TRUE(one.image_area.has_value());
    EXPECT_EQ(one.image_area->rect, kInsetInVisible);
    EXPECT_EQ(one.image_area->source, raw::ImageAreaSource::DecoderInsetCrop);
    DevelopSettings v1 = masked_settings();
    v1.operations.geometry = 1;
    DevelopSettings v2 = masked_settings();
    ASSERT_EQ(v2.operations.geometry, 2u);
    auto whole = render_stage_two(one, v1);
    auto area = render_stage_two(one, v2);
    ASSERT_TRUE(whole && area);
    EXPECT_EQ(area->display.pixels.size(), kInsetInVisible.size());
    EXPECT_EQ(area->report.output_size, kInsetInVisible.size());
    EXPECT_EQ(area->report.geometry_frame, kInsetInVisible);
    EXPECT_EQ(area->report.geometry_version, 2u);
    ASSERT_TRUE(area->report.geometry.has_value());
    EXPECT_EQ(area->report.geometry->frame, kInsetInVisible);
    EXPECT_EQ(area->report.geometry->version, 2u);
    EXPECT_TRUE(area->report.operations.back().applied);
    expect_sub_image(whole->display.pixels, area->display.pixels, kInsetInVisible.x, kInsetInVisible.y);

    // A crop is relative to the area: 0.25..0.75 of 88 columns = 22..66, 0.1..0.9 of 60 rows = 6..54.
    v2.geometry.crop = CropRect{0.25, 0.1, 0.75, 0.9};
    auto cropped = render_stage_two(one, v2);
    ASSERT_TRUE(cropped);
    EXPECT_EQ(cropped->display.pixels.size(), (image::Size{44, 48}));
    expect_sub_image(whole->display.pixels, cropped->display.pixels, kInsetInVisible.x + 22,
                     kInsetInVisible.y + 6);

    // Quarter turns turn the same pixels.
    v2.geometry.quarter_turns = 1;
    auto turned = render_stage_two(one, v2);
    ASSERT_TRUE(turned);
    auto expected = apply_orientation(std::move(cropped->display.pixels), image::Orientation::Rotate90Cw);
    ASSERT_TRUE(expected);
    EXPECT_TRUE(ts::bit_identical(turned->display.pixels, *expected));

    // The export render agrees with the interactive one at full resolution here (no clipping).
    DevelopSettings exported_settings = masked_settings();
    auto exported = render_develop(pinned_scene(), exported_settings);
    ASSERT_TRUE(exported);
    EXPECT_EQ(exported->display.pixels.size(), kInsetInVisible.size());
}

TEST(GeometryVersions, MasksStayOnTheirContentAcrossVersions) {
    // Masks are normalised in the whole upright image for both versions (ADR 0017): a mask point
    // names the same upright pixel, so without a crop or straightening its output position moves
    // by exactly the area's origin, and every version maps it back to itself.
    StageOneImage one = stage_one(pinned_scene());
    DevelopSettings v1 = masked_settings();
    v1.operations.geometry = 1;
    const DevelopSettings v2 = masked_settings();
    auto m1 = geometry_mapping(v1, one);
    auto m2 = geometry_mapping(v2, one);
    ASSERT_TRUE(m1 && m2);
    EXPECT_EQ(m1->frame(), image::PixelRect::from_size(one.pixels.size()));
    EXPECT_EQ(m2->frame(), kInsetInVisible);
    for (const MaskPoint p : {MaskPoint{0.4, 0.45}, MaskPoint{0.1, 0.8}, MaskPoint{0.85, 0.88}}) {
        const image::PointD o1 = m1->output_from_normalized(p);
        const image::PointD o2 = m2->output_from_normalized(p);
        EXPECT_DOUBLE_EQ(o2.x, o1.x - kInsetInVisible.x);
        EXPECT_DOUBLE_EQ(o2.y, o1.y - kInsetInVisible.y);
        for (const GeometryMapping* m : {&*m1, &*m2}) {
            const MaskPoint back = m->normalized_from_output(m->output_from_normalized(p));
            EXPECT_NEAR(back.x, p.x, 1e-12);
            EXPECT_NEAR(back.y, p.y, 1e-12);
        }
    }
    // Straightened, cropped and turned within the area: the mask point still maps back to itself,
    // and the rendered mask effect is where the version-1 rendering of the same content has it
    // (the version-2 identity render is the area of the version-1 render, bit for bit: see
    // Version2RendersTheCameraAreaOfTheSameRendering).
    DevelopSettings turned = v2;
    turned.geometry.straighten_degrees = -2.5;
    turned.geometry.crop = CropRect{0.1, 0.05, 0.9, 0.95};
    turned.geometry.quarter_turns = 2;
    auto m3 = geometry_mapping(turned, one);
    ASSERT_TRUE(m3);
    const MaskPoint centre{0.4, 0.45};
    const MaskPoint back = m3->normalized_from_output(m3->output_from_normalized(centre));
    EXPECT_NEAR(back.x, centre.x, 1e-12);
    EXPECT_NEAR(back.y, centre.y, 1e-12);
}

TEST(GeometryVersions, StraighteningStaysInsideTheAreaAndTheMappingIsConsistent) {
    StageOneImage one = stage_one(pinned_scene());
    DevelopSettings s = masked_settings();
    s.geometry.straighten_degrees = 4.0;
    auto mapping = geometry_mapping(s, one);
    ASSERT_TRUE(mapping) << mapping.error().describe();
    EXPECT_EQ(mapping->frame(), kInsetInVisible);
    EXPECT_EQ(mapping->upright_size(), one.pixels.size());
    // The largest crop inside the straightened area covers every output pixel.
    s.geometry.crop = largest_inside_crop(4.0, mapping->frame_size());
    auto r = render_stage_two(one, s);
    ASSERT_TRUE(r) << r.error().describe();
    ASSERT_TRUE(r->report.geometry.has_value());
    EXPECT_EQ(r->report.geometry->uncovered_pixels, 0u);
    EXPECT_TRUE(crop_inside_image(s.geometry, mapping->frame_size()));
    auto m2 = geometry_mapping(s, one);
    ASSERT_TRUE(m2);
    EXPECT_EQ(r->display.pixels.size(), m2->output_size());
    // Output <-> upright round trip, and mask points land where the upright frame puts them.
    for (const image::PointD p :
         {image::PointD{10.5, 7.25}, image::PointD{40.0, 30.0}, image::PointD{80.5, 55.5}}) {
        const image::PointD o = m2->output_from_upright(p);
        const image::PointD back = m2->upright_from_output(o);
        EXPECT_NEAR(back.x, p.x, 1e-9);
        EXPECT_NEAR(back.y, p.y, 1e-9);
        const MaskPoint n{p.x / one.pixels.width(), p.y / one.pixels.height()};
        const image::PointD on = m2->output_from_normalized(n);
        EXPECT_NEAR(on.x, o.x, 1e-9);
        EXPECT_NEAR(on.y, o.y, 1e-9);
    }
    // Without straightening the area's centre maps to the output centre.
    DevelopSettings plain;
    auto m3 = geometry_mapping(plain, one);
    ASSERT_TRUE(m3);
    const image::PointD c = m3->output_from_upright(image::PointD{
        kInsetInVisible.x + 0.5 * kInsetInVisible.width, kInsetInVisible.y + 0.5 * kInsetInVisible.height});
    EXPECT_DOUBLE_EQ(c.x, 0.5 * kInsetInVisible.width);
    EXPECT_DOUBLE_EQ(c.y, 0.5 * kInsetInVisible.height);
    EXPECT_FALSE(m3->identity());
    EXPECT_NE(m3->revision(), GeometryMapping::create(plain.geometry, one.pixels.size())->revision());
}

TEST(GeometryVersions, TheMemoryEstimateCountsTheVersion2Output) {
    // With a camera area, version 2's identity geometry writes a new image of the area's size; the
    // stage-two estimate counts it (version 1's identity writes nothing).
    const raw::DecodedRaw d = pinned_scene();
    const StageOneRequest request;
    DevelopSettings v1;
    v1.operations.geometry = 1;
    auto e1 = estimate_develop_memory(d.metadata, request, v1);
    auto e2 = estimate_develop_memory(d.metadata, request, DevelopSettings{});
    ASSERT_TRUE(e1 && e2);
    const std::uint64_t area_plane = std::uint64_t{88} * 60 * 3 * sizeof(float);
    const std::uint64_t whole_plane = std::uint64_t{96} * 68 * 3 * sizeof(float);
    EXPECT_GE(e2->stage_two_bytes, e1->stage_two_bytes + area_plane);
    EXPECT_LT(e2->stage_two_bytes, e1->stage_two_bytes + whole_plane);
    // Without an area both versions estimate the same.
    const raw::DecodedRaw plain = plain_scene();
    auto p1 = estimate_develop_memory(plain.metadata, request, v1);
    auto p2 = estimate_develop_memory(plain.metadata, request, DevelopSettings{});
    ASSERT_TRUE(p1 && p2);
    EXPECT_EQ(p1->stage_two_bytes, p2->stage_two_bytes);
}

TEST(GeometryVersions, AFrameOfTheWholeImageIsVersion1Exactly) {
    // GeometryMapping with the whole image as its frame is the version-1 mapping, bit for bit.
    const image::Size size{96, 68};
    GeometrySetting g;
    g.straighten_degrees = -6.0;
    g.crop = CropRect{0.1, 0.05, 0.85, 0.9};
    g.quarter_turns = 3;
    auto a = GeometryMapping::create(g, size);
    auto b = GeometryMapping::create(g, size, image::PixelRect::from_size(size));
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->revision(), b->revision());
    EXPECT_EQ(a->crop_pixels(), b->crop_pixels());
    for (const image::PointD p : {image::PointD{0.5, 0.5}, image::PointD{33.3, 21.7}}) {
        EXPECT_EQ(a->upright_from_output(p).x, b->upright_from_output(p).x);
        EXPECT_EQ(a->upright_from_output(p).y, b->upright_from_output(p).y);
    }
    // A frame outside the image is refused.
    ts::expect_error(GeometryMapping::create(g, size, image::PixelRect{90, 0, 10, 10}),
                     ErrorCode::InvalidArgument, "engine.develop.geometry");
}

TEST(GeometryVersions, AnUnknownAreaRendersVersion2AsVersion1) {
    StageOneImage one = stage_one(plain_scene());
    EXPECT_FALSE(one.image_area.has_value());
    DevelopSettings v2 = pinned_settings();
    v2.operations.geometry = 2;
    auto a = render_stage_two(one, v2);
    ASSERT_TRUE(a);
    expect_hash("unknown_area_v2", a->display.pixels, 0x59ae8a99415df989ull);
    EXPECT_EQ(a->report.geometry_version, 2u);
    EXPECT_EQ(a->report.geometry_frame, image::PixelRect::from_size(one.pixels.size()));
    DevelopSettings identity;
    auto b = render_stage_two(one, identity);
    ASSERT_TRUE(b);
    expect_hash("unknown_area_identity", b->display.pixels, 0x4d881ca61c97501cull);
    EXPECT_FALSE(b->report.operations.back().applied);
}

TEST(GeometryVersions, ProxiesScaleTheAreaPerAxis) {
    StageOneImage full = stage_one(pinned_scene());
    StageOneImage proxy = stage_one(pinned_scene(), 48);
    ASSERT_EQ(proxy.pixels.size(), (image::Size{48, 34}));
    ASSERT_TRUE(proxy.image_area.has_value());
    EXPECT_EQ(proxy.image_area->rect, kInsetInVisible);  // kept in full-resolution pixels
    DevelopSettings s;
    auto r = render_stage_two(proxy, s);
    ASSERT_TRUE(r) << r.error().describe();
    // 4..92 x 3..63 of 96 x 68 at 48 x 34: columns 2..46, rows 1.5..31.5 -> 2..32 (halves up).
    EXPECT_EQ(r->report.geometry_frame, (image::PixelRect{2, 2, 44, 30}));
    EXPECT_EQ(scale_area(kInsetInVisible, full.pixels.size(), proxy.pixels.size()),
              (image::PixelRect{2, 2, 44, 30}));
    EXPECT_EQ(r->display.pixels.size(), (image::Size{44, 30}));
    // A stage one cut to part of the image cannot carry the area.
    StageOneImage cut = stage_one(pinned_scene());
    {
        auto part = image::ImageBuffer<float>::create(40, 30, 3);
        ASSERT_TRUE(part);
        for (std::int32_t y = 0; y < 30; ++y) {
            for (std::int32_t k = 0; k < 120; ++k) {
                part->row(y)[k] = cut.pixels.row(y + 10)[k + 30];
            }
        }
        cut.pixels = std::move(*part);
        cut.id = 0;
    }
    ASSERT_TRUE(cut.image_area.has_value());
    ts::expect_error(render_stage_two(cut, s), ErrorCode::InvalidArgument, "engine.develop.geometry");
    cut.image_area.reset();  // what code that cuts a stage one must do (develop.hpp)
    auto rendered_cut = render_stage_two(cut, s);
    ASSERT_TRUE(rendered_cut) << rendered_cut.error().describe();
    EXPECT_EQ(rendered_cut->display.pixels.size(), (image::Size{40, 30}));
}

TEST(GeometryVersions, TheAreaTurnsWithTheCameraOrientation) {
    // The version-1 renders are pinned too (the pre-change engine's identity renders of the same
    // oriented scenes).
    const std::array<std::pair<image::Orientation, std::uint64_t>, 4> cases{{
        {image::Orientation::Rotate90Cw, 0x4e532c4411e49becull},
        {image::Orientation::Rotate180, 0xce8badfa60d4987cull},
        {image::Orientation::Rotate90Ccw, 0x19165f36e97e92f4ull},
        {image::Orientation::Transpose, 0x6a0e5049dbebeffcull},
    }};
    for (const auto& [o, v1_hash] : cases) {
        raw::DecodedRaw d = plain_scene(o);
        d.metadata.image_area = inset_area();
        StageOneImage one = stage_one(std::move(d));
        ASSERT_TRUE(one.image_area.has_value());
        const image::PixelRect expected = image::map_to_oriented(kInsetInVisible, image::Size{96, 68}, o);
        EXPECT_EQ(one.image_area->rect, expected) << image::to_string(o);
        DevelopSettings v1;
        v1.operations.geometry = 1;
        auto whole = render_stage_two(one, v1);
        auto area = render_stage_two(one, DevelopSettings{});
        ASSERT_TRUE(whole && area);
        expect_hash(std::string(image::to_string(o)).c_str(), whole->display.pixels, v1_hash);
        EXPECT_EQ(area->display.pixels.size(), expected.size());
        expect_sub_image(whole->display.pixels, area->display.pixels, expected.x, expected.y);
    }
}

// ---- Placement ---------------------------------------------------------------------------------------

// A DNG of 100 x 72 with an ActiveArea of odd margins (LibRaw starts the visible area one row and
// column inside it), DefaultCrop as given, and `opcodes`.
raw::DecodedRaw dng_scene(std::array<double, 2> origin, std::array<double, 2> size,
                          const std::vector<ts::TestOpcode>& opcodes = {}) {
    raw::DecodedRaw d = plain_scene();
    // ActiveArea rows 1..71, columns 1..99 (odd margins); visible area (2, 2) 96 x 68 inside it.
    ts::attach_dng(d, opcodes, raw::DngRect{1, 1, 71, 99});
    d.metadata.dng->raw_ifd.default_crop_origin = origin;
    d.metadata.dng->raw_ifd.default_crop_size = size;
    d.metadata.image_area = raw::dng_image_area(d.metadata.dng->raw_ifd, image::Size{100, 72});
    return d;
}

TEST(ImageAreaPlacement, DngDefaultCropIsRelativeToTheActiveAreaWithOddMargins) {
    // Origin (3, 2) of the ActiveArea at (1, 1): raw-frame (4, 3); the visible area starts at (2, 2).
    raw::DecodedRaw d = dng_scene({3.0, 2.0}, {90.0, 60.0});
    EXPECT_EQ(d.metadata.image_area.source, raw::ImageAreaSource::DngDefaultCrop);
    EXPECT_EQ(d.metadata.image_area.provenance, raw::Provenance::DngTag);
    ASSERT_TRUE(d.metadata.image_area.rect);
    EXPECT_EQ(*d.metadata.image_area.rect, (raw::AreaRect{4.0, 3.0, 90.0, 60.0}));
    EXPECT_EQ(d.metadata.image_area.reference, (image::PixelRect{1, 1, 98, 70}));
    auto placed = place_image_area(d.metadata, image::Size{96, 68});
    ASSERT_TRUE(placed);
    EXPECT_EQ(placed->rect, (image::PixelRect{2, 1, 90, 60}));
    EXPECT_EQ(placed->clipped, (std::array<std::int32_t, 4>{0, 0, 0, 0}));
    EXPECT_FALSE(placed->moved_inside);

    // The whole ActiveArea: one row and column of it lie outside the visible area on each side.
    raw::DecodedRaw whole = dng_scene({0.0, 0.0}, {98.0, 70.0});
    auto w = place_image_area(whole.metadata, image::Size{96, 68});
    ASSERT_TRUE(w);
    EXPECT_EQ(w->rect, (image::PixelRect{0, 0, 96, 68}));
    EXPECT_EQ(w->clipped, (std::array<std::int32_t, 4>{1, 1, 1, 1}));

    // Fractional values round halves away from zero, origin and size separately.
    raw::DecodedRaw half = dng_scene({3.5, 2.5}, {80.5, 50.4});
    auto h = place_image_area(half.metadata, image::Size{96, 68});
    ASSERT_TRUE(h);
    EXPECT_EQ(h->rect, (image::PixelRect{3, 2, 81, 50}));

    // An overrunning area moves back inside the stage image (dng_negative::DefaultCropArea()).
    raw::DecodedRaw over = dng_scene({10.0, 12.0}, {95.0, 65.0});
    auto o = place_image_area(over.metadata, image::Size{96, 68});
    ASSERT_TRUE(o);
    EXPECT_TRUE(o->moved_inside);
    // left 10 - (105 - 98) = 3, top 12 - (77 - 70) = 5: buffer (2, 4), cut on the right and bottom.
    EXPECT_EQ(o->rect, (image::PixelRect{2, 4, 94, 64}));
    EXPECT_EQ(o->clipped, (std::array<std::int32_t, 4>{0, 0, 1, 1}));

    // Without DefaultCrop tags: the whole ActiveArea (the specification's default).
    raw::DecodedRaw none = plain_scene();
    ts::attach_dng(none, {}, raw::DngRect{1, 1, 71, 99});
    none.metadata.image_area = raw::dng_image_area(none.metadata.dng->raw_ifd, image::Size{100, 72});
    EXPECT_EQ(none.metadata.image_area.source, raw::ImageAreaSource::DngDefaultWholeImage);
    EXPECT_EQ(none.metadata.image_area.provenance, raw::Provenance::LucentRule);

    // Unknown: nothing to place.
    EXPECT_FALSE(place_image_area(plain_scene().metadata, image::Size{96, 68}));

    // Version 1 renders this DNG exactly as the pre-change engine did (which read the same
    // DefaultCrop tags and did not apply them): pinned.
    StageOneImage one = stage_one(dng_scene({3.0, 2.0}, {90.0, 60.0}));
    ASSERT_TRUE(one.image_area.has_value());
    EXPECT_EQ(one.image_area->rect, (image::PixelRect{2, 1, 90, 60}));
    DevelopSettings v1;
    v1.operations.geometry = 1;
    auto identity = render_stage_two(one, v1);
    auto straightened = render_stage_two(one, pinned_settings());
    ASSERT_TRUE(identity && straightened);
    expect_hash("dng_odd_margins_v1_identity", identity->display.pixels, 0x4d881ca61c97501cull);
    expect_hash("dng_odd_margins_v1_straighten", straightened->display.pixels, 0x59ae8a99415df989ull);
}

TEST(ImageAreaPlacement, DngDefaultCropAppliesAfterTheOpcodes) {
    // OpcodeList3: a gentle vignette (pixels change, geometry does not), then a TrimBounds of the
    // stage image (rows 2..60, columns 3..90 of the ActiveArea), which re-bases the stage image
    // (dng_simple_image::Trim): the DefaultCrop is relative to the trimmed image.
    raw::DngFixVignetteRadial vignette;
    vignette.k = {0.05, 0.0, 0.0, 0.0, 0.0};
    vignette.center_x = 0.5;
    vignette.center_y = 0.5;
    raw::DngTrimBounds trim;
    trim.bounds = raw::DngOpcodeRect{2, 3, 60, 90};
    raw::DecodedRaw d = dng_scene({4.0, 3.0}, {80.0, 50.0},
                                  {ts::TestOpcode{raw::DngOpcodeListId::List3, 3, vignette},
                                   ts::TestOpcode{raw::DngOpcodeListId::List3, 6, trim}});
    ASSERT_EQ(d.metadata.interpretation.status, raw::InterpretationStatus::Complete);
    StageOneImage one = stage_one(std::move(d));
    // The trim keeps stage columns 3..90 and rows 2..60: the buffer (stage offset (1, 1)) becomes
    // 87 x 58 and is the stage image; the area is (4, 3) 80 x 50 of it.
    ASSERT_EQ(one.pixels.size(), (image::Size{87, 58}));
    ASSERT_TRUE(one.image_area.has_value());
    EXPECT_EQ(one.image_area->rect, (image::PixelRect{4, 3, 80, 50}));
    DevelopSettings v1;
    v1.operations.geometry = 1;
    auto whole = render_stage_two(one, v1);
    auto area = render_stage_two(one, DevelopSettings{});
    ASSERT_TRUE(whole && area);
    expect_sub_image(whole->display.pixels, area->display.pixels, 4, 3);
    // Version 1 is the pre-change engine's rendering of this DNG, bit for bit (pinned).
    expect_hash("dng_opcodes_v1_identity", whole->display.pixels, 0x75bab5615f8f4a93ull);
    auto straightened = render_stage_two(one, pinned_settings());
    ASSERT_TRUE(straightened) << straightened.error().describe();
    expect_hash("dng_opcodes_v1_straighten", straightened->display.pixels, 0xdac58c9c64e03428ull);

    // The W2 path cuts at its camera_area stage, after the opcode stage, to the same pixels.
    raw::DecodedRaw w2 = dng_scene({4.0, 3.0}, {80.0, 50.0},
                                   {ts::TestOpcode{raw::DngOpcodeListId::List3, 3, vignette},
                                    ts::TestOpcode{raw::DngOpcodeListId::List3, 6, trim}});
    RenderOptions options;
    options.keep_scene_linear = true;
    auto cut = render_decoded(std::move(w2), RenderSettings{}, options);
    ASSERT_TRUE(cut) << cut.error().describe();
    EXPECT_TRUE(cut->report.camera_area_applied);
    EXPECT_EQ(cut->display.pixels.size(), (image::Size{80, 50}));
    std::vector<RenderStage> order;
    for (const StageRecord& s : cut->report.stages) {
        order.push_back(s.stage);
    }
    const auto list3 = std::find(order.begin(), order.end(), RenderStage::DngOpcodeList3);
    ASSERT_NE(list3, order.end());
    ASSERT_NE(list3 + 1, order.end());
    EXPECT_EQ(*(list3 + 1), RenderStage::CameraArea);
    RenderSettings off;
    off.apply_camera_area = false;
    raw::DecodedRaw w2_off = dng_scene({4.0, 3.0}, {80.0, 50.0},
                                       {ts::TestOpcode{raw::DngOpcodeListId::List3, 3, vignette},
                                        ts::TestOpcode{raw::DngOpcodeListId::List3, 6, trim}});
    auto uncut = render_decoded(std::move(w2_off), off, options);
    ASSERT_TRUE(uncut);
    EXPECT_EQ(uncut->display.pixels.size(), (image::Size{87, 58}));
    // Without the area the W2 render is the pre-change engine's, bit for bit (pinned).
    expect_hash("dng_opcodes_w2_display", uncut->display.pixels, 0xc4aeeb2d7f49c91aull);
    expect_hash("dng_opcodes_w2_scene_linear", uncut->scene_linear->pixels, 0xc4aeeb2d7f49c91aull);
    expect_sub_image(uncut->display.pixels, cut->display.pixels, 4, 3);
    expect_sub_image(uncut->scene_linear->pixels, cut->scene_linear->pixels, 4, 3);
}

TEST(ImageAreaPlacement, OrientationKeepsTheClippedSides) {
    ImageAreaPlacement p;
    p.source = raw::ImageAreaSource::DecoderInsetCrop;
    p.rect = image::PixelRect{0, 2, 10, 5};
    p.clipped = {1, 0, 0, 2};  // one column cut on the left, two rows at the bottom
    const image::Size stored{12, 8};
    const ImageAreaPlacement r = oriented_placement(p, stored, image::Orientation::Rotate90Cw);
    // Rotate 90 clockwise: the stored left side becomes the top, the bottom the left.
    EXPECT_EQ(r.rect, image::map_to_oriented(p.rect, stored, image::Orientation::Rotate90Cw));
    EXPECT_EQ(r.clipped, (std::array<std::int32_t, 4>{2, 1, 0, 0}));
    EXPECT_EQ(oriented_placement(p, stored, image::Orientation::Normal), p);
}

TEST(ImageAreaPlacement, ScaleAreaRoundsEdgesToTheProxy) {
    const image::Size full{8192, 5464};
    const image::PixelRect area{12, 8, 8160, 5448};
    EXPECT_EQ(scale_area(area, full, full), area);
    const image::PixelRect p = scale_area(area, full, image::Size{2048, 1366});
    EXPECT_EQ(p, (image::PixelRect{3, 2, 2040, 1362}));
    // Never empty and always inside.
    const image::PixelRect tiny = scale_area(image::PixelRect{8190, 5462, 2, 2}, full, image::Size{64, 43});
    EXPECT_FALSE(tiny.empty());
    EXPECT_TRUE(image::PixelRect::from_size(image::Size{64, 43}).contains(tiny));
}

}  // namespace
}  // namespace lucent::engine
