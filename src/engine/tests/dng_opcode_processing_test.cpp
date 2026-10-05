// Unit tests of DNG opcode processing (engine/dng_opcode_processing.hpp, ADR 0014): exact
// expected outputs on small synthetic images for every implemented opcode, area/plane/pitch and
// frame-offset edge cases, the WarpRectilinear2 skip rule, white-balance and saturation handling,
// stage ordering through the render and develop paths, records and determinism.

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/engine/develop.hpp>
#include <lucent/engine/dng_opcode_processing.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/engine/sensor.hpp>

#include "develop_test_support.hpp"
#include "dng_opcode_test_support.hpp"
#include "sensor_test_support.hpp"

namespace lucent::engine {
namespace {

using namespace test_support;
using raw::DngOpcodeListId;
using L = raw::DngOpcodeListId;
using image::CfaPattern;
using image::PixelRect;

constexpr std::uint32_t kGainMap = 9;
constexpr std::uint32_t kVignette = 3;
constexpr std::uint32_t kBadConstant = 4;
constexpr std::uint32_t kBadList = 5;
constexpr std::uint32_t kTrim = 6;
constexpr std::uint32_t kMapTable = 7;
constexpr std::uint32_t kMapPolynomial = 8;
constexpr std::uint32_t kDeltaRow = 10;
constexpr std::uint32_t kDeltaColumn = 11;
constexpr std::uint32_t kScaleRow = 12;
constexpr std::uint32_t kScaleColumn = 13;
constexpr std::uint32_t kWarp = 1;
constexpr std::uint32_t kWarp2 = 14;

const CfaPattern kRggb = CfaPattern::bayer(image::BayerLayout::Rggb);

std::uint16_t u16_at(const raw::DecodedRaw& d, std::int32_t x, std::int32_t y, std::int32_t c = 0) {
    return std::visit(
        [&](const auto& frame) {
            return std::get<image::ImageBuffer<std::uint16_t>>(frame.pixels).at(x, y, c);
        },
        d.image);
}

// A 16-bit mosaic whose every visible pixel is `value(x, y)`.
raw::DecodedRaw mosaic16(std::int32_t w, std::int32_t h, const SampleFn& value) {
    return make_mosaic<std::uint16_t>(w, h, PixelRect{0, 0, w, h}, kRggb, simple_levels(0.0, 65535.0), value);
}

const DngOpcodeRecord& record(const DngOpcodeProcessor& p, std::size_t i) { return p.report().opcodes.at(i); }

// ---- List 1: stored 16-bit samples ----------------------------------------------------------------

TEST(DngOpcodeList1, GainMapInterpolatesAtPixelCentresAndLimitsToTheStoredRange) {
    raw::DecodedRaw d = mosaic16(4, 2, [](auto x, auto, auto) { return x == 3 ? 60000.0 : 1000.0; });
    // One map row, two map columns at h = 0 and h = 1 (relative to the width): column c sits at
    // h = (c + 0.5) / 4, so its gain is 1 + (c + 0.5) / 4. ColPitch 2 from column 1: columns 1, 3.
    attach_dng(d, {{L::List1, kGainMap, gain_map(area(0, 1, 2, 4, 0, 1, 1, 2), 1, 2, 1.0, 1.0, 0.0, 0.0, 1, {1.0f, 2.0f})}});
    ASSERT_EQ(d.metadata.interpretation.status, raw::InterpretationStatus::Complete);
    DngOpcodeProcessor p(d.metadata);
    ASSERT_TRUE(p.run_list1(d.image));
    for (std::int32_t y = 0; y < 2; ++y) {
        EXPECT_EQ(u16_at(d, 0, y), 1000);
        EXPECT_EQ(u16_at(d, 1, y), 1375);   // 1000 x 1.375
        EXPECT_EQ(u16_at(d, 2, y), 1000);   // not on the column pitch
        EXPECT_EQ(u16_at(d, 3, y), 65535);  // 60000 x 1.875 = 112500, limited
    }
    const DngOpcodeRecord& r = record(p, 0);
    EXPECT_EQ(r.outcome, OpcodeOutcome::Applied);
    EXPECT_EQ(r.affected_samples, 4u);
    EXPECT_EQ(r.limited_samples, 2u);
}

TEST(DngOpcodeList1, ValueMapsDeltasAndScalesUseTheSixteenBitLogicalRange) {
    // The DNG SDK runs these on its stage-1 image converted to [0, 1] floats (v / 65535) but
    // rescales a polynomial's coefficients (c_k x 65535^(k - 1)) and a delta (d / 65535) for it,
    // so in stored units a polynomial is evaluated on the stored value and a delta adds stored
    // units (dng_misc_opcodes.cpp, BufferPixelType of MapPolynomial and DeltaPerRow).
    raw::DecodedRaw d = mosaic16(5, 4, [](auto x, auto, auto) {
        const double v[5] = {0.0, 1.0, 1000.0, 40000.0, 100.0};
        return v[x];
    });
    attach_dng(d, {
                      // Row 0: table of 4 entries; 100 is beyond it and takes the last entry.
                      {L::List1, kMapTable, raw::DngMapTable{area(0, 0, 1, 5), {10, 20, 30, 40}}},
                      // Row 1: y = 100 + 2 v - v^2 / 65535 on the stored value v.
                      {L::List1, kMapPolynomial, raw::DngMapPolynomial{area(1, 0, 2, 5), {100.0, 2.0, -1.0 / 65535.0}}},
                      // Row 2: - 50 stored units.
                      {L::List1, kDeltaRow, values(area(2, 0, 3, 5), {-50.0f})},
                      // Row 3: x 1.5.
                      {L::List1, kScaleRow, values(area(3, 0, 4, 5), {1.5f})},
                  });
    DngOpcodeProcessor p(d.metadata);
    ASSERT_TRUE(p.run_list1(d.image));
    const std::array<std::uint16_t, 5> table{10, 20, 40, 40, 40};
    for (std::int32_t x = 0; x < 5; ++x) {
        EXPECT_EQ(u16_at(d, x, 0), table[static_cast<std::size_t>(x)]) << x;
    }
    // 0 -> 100; 1 -> 101.99998; 1000 -> 2084.741; 40000 -> 55685.596; 100 -> 299.847.
    const std::array<std::uint16_t, 5> polynomial{100, 102, 2085, 55686, 300};
    const std::array<std::uint16_t, 5> delta{0, 0, 950, 39950, 50};  // 0 and 1 limited at 0
    for (std::int32_t x = 0; x < 5; ++x) {
        EXPECT_EQ(u16_at(d, x, 1), polynomial[static_cast<std::size_t>(x)]) << x;
        EXPECT_EQ(u16_at(d, x, 2), delta[static_cast<std::size_t>(x)]) << x;
    }
    EXPECT_EQ(u16_at(d, 2, 3), 1500);
    EXPECT_EQ(u16_at(d, 3, 3), 60000);
    EXPECT_EQ(record(p, 1).limited_samples, 0u);
    EXPECT_EQ(record(p, 2).limited_samples, 2u);
}

TEST(DngOpcodeList1, ADeltaOfFiveStoredUnitsAddsFive) {
    // The review's probe: DeltaPerRow 5.0 on stored ~1000 must give ~1005, not saturate.
    raw::DecodedRaw d = mosaic16(4, 4, [](auto, auto, auto) { return 1000.0; });
    attach_dng(d, {{L::List1, kDeltaRow, values(area(0, 0, 4, 4), {5.0f, 5.0f, 5.0f, 5.0f})}});
    DngOpcodeProcessor p(d.metadata);
    ASSERT_TRUE(p.run_list1(d.image));
    EXPECT_EQ(u16_at(d, 2, 3), 1005);
    EXPECT_EQ(record(p, 0).limited_samples, 0u);
}

TEST(DngOpcodeList1, BadPixelListUsesTheNearestSameColourNeighbours) {
    // RGGB, BayerPhase 0. v = 10 x^2 + y (not linear, so the mean is not the original value).
    const auto v = [](std::int32_t x, std::int32_t y) { return 10.0 * x * x + y; };
    raw::DecodedRaw d = mosaic16(8, 8, [&](auto x, auto y, auto) {
        if ((x == 3 && y == 3) || (x == 3 && y == 2) || (x >= 5 && x < 7 && y >= 5 && y < 7)) {
            return 9999.0;
        }
        return v(x, y);
    });
    raw::DngFixBadPixelsList list;
    list.bayer_phase = 0;
    list.points = {{3, 3}, {2, 3}, {100, 100}};  // {row, column}; the last lies outside the image
    list.rects = {{5, 5, 7, 7}};
    attach_dng(d, {{L::List1, kBadList, list}});
    ASSERT_EQ(d.metadata.interpretation.status, raw::InterpretationStatus::Complete);
    DngOpcodeProcessor p(d.metadata);
    ASSERT_TRUE(p.run_list1(d.image));
    // (3, 3) is blue: ring (+-2, 0), (0, +-2): 13 + 253 + 91 + 95 = 452 -> 113.
    EXPECT_EQ(u16_at(d, 3, 3), 113);
    // (3, 2) is green: diagonals (2, 1), (4, 1), (2, 3), (4, 3): 41 + 161 + 43 + 163 = 408 -> 102.
    EXPECT_EQ(u16_at(d, 3, 2), 102);
    // (5, 5) blue, inside the rectangle: (3, 5), (7, 5), (5, 3), (5, 7) are good: 95 + 495 + 253
    // + 257 = 1100 -> 275.
    EXPECT_EQ(u16_at(d, 5, 5), 275);
    // (6, 5) green: diagonals (5, 4), (7, 4), (5, 6) bad, (7, 6): 254 + 494 + 496 = 1244 / 3 -> 415.
    EXPECT_EQ(u16_at(d, 6, 5), 415);
    EXPECT_EQ(u16_at(d, 0, 0), 0);  // untouched
    const DngOpcodeRecord& r = record(p, 0);
    EXPECT_EQ(r.affected_samples, 6u);
    EXPECT_EQ(r.unrepaired_pixels, 0u);
    EXPECT_NE(r.detail.find("1 point(s) outside"), std::string::npos) << r.detail;
}

TEST(DngOpcodeList1, BadPixelConstantAndUnrepairablePixels) {
    raw::DecodedRaw d = mosaic16(6, 6, [](auto x, auto y, auto) { return x == 2 && y == 2 ? 0.0 : 100.0 + x; });
    attach_dng(d, {{L::List1, kBadConstant, raw::DngFixBadPixelsConstant{0, 0}}});
    DngOpcodeProcessor p(d.metadata);
    ASSERT_TRUE(p.run_list1(d.image));
    // (2, 2) red: (0, 2), (4, 2), (2, 0), (2, 4): 100 + 104 + 102 + 102 = 408 -> 102.
    EXPECT_EQ(u16_at(d, 2, 2), 102);

    // A rectangle over the whole image leaves nothing to interpolate from.
    raw::DecodedRaw all = mosaic16(6, 6, [](auto, auto, auto) { return 7.0; });
    raw::DngFixBadPixelsList list;
    list.rects = {{0, 0, 6, 6}};
    attach_dng(all, {{L::List1, kBadList, list}});
    DngOpcodeProcessor q(all.metadata);
    ASSERT_TRUE(q.run_list1(all.image));
    EXPECT_EQ(record(q, 0).unrepaired_pixels, 36u);
    EXPECT_EQ(u16_at(all, 3, 3), 7);
}

TEST(DngOpcodeList1, BadPixelPhaseThatContradictsTheCfaIsNotSupported) {
    raw::DecodedRaw d = mosaic16(6, 6, [](auto, auto, auto) { return 1.0; });
    attach_dng(d, {{L::List1, kBadConstant, raw::DngFixBadPixelsConstant{0, 1}}});  // green first, CFA is RGGB
    const raw::RequiredOperation* op = operation(d.metadata, L::List1, 0);
    ASSERT_NE(op, nullptr);
    EXPECT_FALSE(op->supported_by_lucent);
    EXPECT_NE(op->lucent_note.find("BayerPhase"), std::string::npos) << op->lucent_note;
    EXPECT_EQ(d.metadata.interpretation.status, raw::InterpretationStatus::RequiresUnsupportedOperation);
    DngOpcodeProcessor p(d.metadata);
    ASSERT_TRUE(p.run_list1(d.image));
    EXPECT_EQ(record(p, 0).outcome, OpcodeOutcome::NotApplied);
    EXPECT_EQ(u16_at(d, 0, 0), 1);
}

// ---- List 2: normalised samples, frame offsets, saturation, trims ---------------------------------

Expected<SensorImage> normalized(const raw::DecodedRaw& d) { return normalize_sensor(d, {}, {}); }

TEST(DngOpcodeList2, CoordinatesAreActiveAreaCoordinatesWhenTheVisibleAreaIsShifted) {
    // ActiveArea starts at (1, 1); the decoder's visible area at (2, 2) (LibRaw moves an odd
    // margin): buffer pixel (0, 0) is stage pixel (1, 1).
    raw::DecodedRaw d = make_mosaic<std::uint16_t>(8, 8, PixelRect{2, 2, 6, 6}, kRggb, simple_levels(0.0, 1000.0),
                                                   [](auto, auto, auto) { return 100.0; });
    std::vector<float> per_column;
    for (int c = 0; c < 7; ++c) {
        per_column.push_back(0.01f * static_cast<float>(c));
    }
    attach_dng(d,
               {
                   {L::List2, kDeltaColumn, values(area(0, 0, 7, 7), per_column)},
                   {L::List2, kDeltaRow, values(area(0, 0, 2, 7), {0.5f, 0.25f})},  // stage rows 0, 1
               },
               raw::DngRect{1, 1, 8, 8});
    ASSERT_EQ(d.metadata.interpretation.status, raw::InterpretationStatus::Complete);
    const raw::DngOpcodeFrame frame = raw::dng_opcode_frame(d.metadata, L::List2);
    EXPECT_EQ(frame.image.width, 7u);
    EXPECT_EQ(frame.offset_x, 1);
    EXPECT_EQ(frame.offset_y, 1);
    auto sensor = normalized(d);
    ASSERT_TRUE(sensor) << sensor.error().describe();
    DngOpcodeProcessor p(d.metadata);
    ASSERT_TRUE(p.run_list2(*sensor));
    for (std::int32_t x = 0; x < 6; ++x) {
        const float column = 0.01f * static_cast<float>(x + 1);
        EXPECT_FLOAT_EQ(sensor->pixels.at(x, 0, 0), 0.1f + column + 0.25f) << x;  // stage row 1
        EXPECT_FLOAT_EQ(sensor->pixels.at(x, 1, 0), 0.1f + column) << x;          // stage row 2
    }
}

TEST(DngOpcodeList2, AreaPlanesAndPitchSelectExactlyTheirSamples) {
    raw::DecodedRaw d = make_linear<std::uint16_t>(6, 5, PixelRect{0, 0, 6, 5}, simple_levels(0.0, 1000.0),
                                                   [](auto, auto, auto) { return 100.0; });
    // ScalePerRow: rows 1 and 3 (pitch 2 from 1, bottom 5 excluded -> rows 1, 3), plane 1 only,
    // columns 2 and 5 (pitch 3 from 2), values 2 and 3. A second opcode names planes 2..5 of a
    // 3-plane image: only plane 2 exists.
    attach_dng(d, {
                      {L::List2, kScaleRow, values(area(1, 2, 5, 6, 1, 1, 2, 3), {2.0f, 3.0f})},
                      {L::List2, kScaleColumn, values(area(0, 0, 1, 6, 2, 4, 1, 1), {4, 4, 4, 4, 4, 4})},
                  });
    auto sensor = normalized(d);
    ASSERT_TRUE(sensor);
    DngOpcodeProcessor p(d.metadata);
    ASSERT_TRUE(p.run_list2(*sensor));
    for (std::int32_t y = 0; y < 5; ++y) {
        for (std::int32_t x = 0; x < 6; ++x) {
            const bool hit = (y == 1 || y == 3) && (x == 2 || x == 5);
            const float expected1 = hit ? (y == 1 ? 0.2f : 0.3f) : 0.1f;
            EXPECT_FLOAT_EQ(sensor->pixels.at(x, y, 0), 0.1f) << x << "," << y;
            EXPECT_FLOAT_EQ(sensor->pixels.at(x, y, 1), expected1) << x << "," << y;
            EXPECT_FLOAT_EQ(sensor->pixels.at(x, y, 2), y == 0 ? 0.4f : 0.1f) << x << "," << y;
        }
    }
    EXPECT_EQ(record(p, 0).affected_samples, 4u);
    EXPECT_EQ(record(p, 1).affected_samples, 6u);
}

TEST(DngOpcodeList2, SaturatedSamplesStayAtTheirSaturationLevel) {
    raw::DecodedRaw d = make_mosaic<std::uint16_t>(4, 4, PixelRect{0, 0, 4, 4}, kRggb, simple_levels(0.0, 1000.0),
                                                   [](auto x, auto y, auto) { return x == 1 && y == 1 ? 1000.0 : 400.0; });
    attach_dng(d, {{L::List2, kGainMap, gain_map(area(0, 0, 4, 4), 1, 1, 1.0, 1.0, 0.0, 0.0, 1, {0.5f})}});
    auto sensor = normalized(d);
    ASSERT_TRUE(sensor);
    EXPECT_EQ(sensor->normalization.saturated_samples, 1u);
    DngOpcodeProcessor p(d.metadata);
    ASSERT_TRUE(p.run_list2(*sensor));
    EXPECT_FLOAT_EQ(sensor->pixels.at(0, 0, 0), 0.2f);
    EXPECT_FLOAT_EQ(sensor->pixels.at(1, 1, 0), 1.0f);  // 0.5 after the gain, raised back
    EXPECT_EQ(p.report().list2_resaturated_samples, 1u);
}

TEST(DngOpcodeList2, TrimBoundsCutsTheSensorImageAndBecomesListThreesFrame) {
    raw::DecodedRaw d = make_mosaic<std::uint16_t>(6, 6, PixelRect{0, 0, 6, 6}, kRggb, simple_levels(0.0, 1000.0),
                                                   [](auto x, auto y, auto) { return 10.0 * x + y; });
    attach_dng(d, {
                      {L::List2, kTrim, raw::DngTrimBounds{{1, 1, 5, 5}}},
                      {L::List3, kVignette, raw::DngFixVignetteRadial{{1.0, 0, 0, 0, 0}, 0.5, 0.5}},
                  });
    ASSERT_EQ(d.metadata.interpretation.status, raw::InterpretationStatus::Complete);
    auto sensor = normalized(d);
    ASSERT_TRUE(sensor);
    DngOpcodeProcessor p(d.metadata);
    ASSERT_TRUE(p.run_list2(*sensor));
    ASSERT_EQ(sensor->pixels.size(), (image::Size{4, 4}));
    EXPECT_FLOAT_EQ(sensor->pixels.at(0, 0, 0), 0.011f);  // raw (1, 1) = 11
    EXPECT_EQ(sensor->cfa.color_at(0, 0), image::CfaColor::Blue);
    EXPECT_EQ(sensor->saturation.width(), 4);
    EXPECT_EQ(sensor->normalization.source_area, (PixelRect{1, 1, 4, 4}));
    ASSERT_EQ(p.report().geometry.size(), 1u);
    EXPECT_EQ(p.report().geometry[0].kind, GeometryChangeKind::Trim);
    EXPECT_EQ(p.report().geometry[0].source_of({0.5, 0.5}), (image::PointD{1.5, 1.5}));

    // List 3 runs on the 4 x 4 image: centre (1.5, 1.5), m^2 = 4.5; pixel (0, 0) has r^2 = 1.
    auto rgb = image::ImageBuffer<float>::create(4, 4, 3);
    ASSERT_TRUE(rgb);
    for (std::int32_t y = 0; y < 4; ++y) {
        for (std::int32_t x = 0; x < 4; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                rgb->at(x, y, c) = 1.0f;
            }
        }
    }
    ASSERT_TRUE(p.run_list3(*rgb, {1.0, 1.0, 1.0}));
    EXPECT_FLOAT_EQ(rgb->at(0, 0, 1), 2.0f);
    EXPECT_FLOAT_EQ(rgb->at(1, 1, 1), static_cast<float>(1.0 + 0.5 / 4.5));
}

// ---- List 3: warps, vignetting, white balance -----------------------------------------------------

// A LinearColor image of w x h whose List 3 holds `opcodes`, and a 3-plane float image filled by
// `value`.
struct List3Case {
    raw::DecodedRaw decoded;
    image::ImageBuffer<float> pixels;
};

List3Case list3_case(std::int32_t w, std::int32_t h, const std::vector<TestOpcode>& opcodes,
                     const std::function<double(double, double, std::int32_t)>& value) {
    List3Case c;
    c.decoded = make_linear<std::uint16_t>(w, h, PixelRect{0, 0, w, h}, simple_levels(0.0, 1000.0),
                                           [](auto, auto, auto) { return 1.0; });
    attach_dng(c.decoded, opcodes);
    auto buffer = image::ImageBuffer<float>::create(w, h, 3);
    EXPECT_TRUE(buffer);
    c.pixels = std::move(*buffer);
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            for (std::int32_t p = 0; p < 3; ++p) {
                c.pixels.at(x, y, p) = static_cast<float>(value(x, y, p));
            }
        }
    }
    return c;
}

TEST(DngOpcodeList3, IdentityWarpReturnsTheImageExactly) {
    List3Case c = list3_case(7, 5, {{L::List3, kWarp, warp({{1.0, 0, 0, 0, 0, 0}}, 0.3, 0.8)}},
                             [](double x, double y, std::int32_t p) { return x * 0.37 + y * y + p; });
    const image::ImageBuffer<float> before = *c.pixels.clone();
    DngOpcodeProcessor p(c.decoded.metadata);
    ASSERT_TRUE(p.run_list3(c.pixels, {1, 1, 1}));
    EXPECT_TRUE(bit_identical(before, c.pixels));
    EXPECT_EQ(record(p, 0).outside_samples, 0u);
}

TEST(DngOpcodeList3, RadialScaleResamplesWithCatmullRom) {
    // 5 x 5, centre (2, 2), kr0 = 0.5: output (x, y) shows input (2 + (x - 2) / 2, 2 + (y - 2) / 2).
    List3Case c = list3_case(5, 5, {{L::List3, kWarp, warp({{0.5, 0, 0, 0, 0, 0}})}},
                             [](double x, double y, std::int32_t) { return x * x + 10.0 * y; });
    DngOpcodeProcessor p(c.decoded.metadata);
    ASSERT_TRUE(p.run_list3(c.pixels, {1, 1, 1}));
    EXPECT_FLOAT_EQ(c.pixels.at(0, 0, 0), 1.0f + 10.0f);  // input (1, 1)
    EXPECT_FLOAT_EQ(c.pixels.at(4, 4, 0), 9.0f + 30.0f);  // input (3, 3)
    EXPECT_FLOAT_EQ(c.pixels.at(2, 2, 2), 4.0f + 20.0f);  // the centre is fixed
    // Input (1.5, 2): Catmull-Rom weights (-1, 9, 9, -1) / 16 on columns 0..3 of row 2
    // reproduce the quadratic exactly: 1.5^2 + 20 = 22.25.
    EXPECT_FLOAT_EQ(c.pixels.at(1, 2, 1), 22.25f);
    // Input (2.5, 2.5) in x and y: 2.5^2 + 25 = 31.25.
    EXPECT_FLOAT_EQ(c.pixels.at(3, 3, 0), 31.25f);
    ASSERT_EQ(p.report().geometry.size(), 1u);
    const GeometryChange& g = p.report().geometry[0];
    EXPECT_EQ(g.kind, GeometryChangeKind::Warp);
    EXPECT_DOUBLE_EQ(g.center_x, 2.0);
    EXPECT_DOUBLE_EQ(g.max_distance, std::sqrt(8.0));
    // Continuous coordinates: the centre of output pixel (0, 0) shows the centre of input (1, 1).
    const image::PointD s = g.source_of({0.5, 0.5});
    EXPECT_DOUBLE_EQ(s.x, 1.5);
    EXPECT_DOUBLE_EQ(s.y, 1.5);
}

// The DNG 1.7.1.0 WarpRectilinear formulas, written out independently of the implementation.
image::PointD spec_source(double x, double y, double w, double h, const std::array<double, 6>& k, double chx,
                          double chy) {
    const double cx = 0.0 + chx * (w - 1 - 0.0);
    const double cy = 0.0 + chy * (h - 1 - 0.0);
    const double mx = std::max(std::abs(0.0 - cx), std::abs(w - 1 - cx));
    const double my = std::max(std::abs(0.0 - cy), std::abs(h - 1 - cy));
    const double m = std::sqrt(mx * mx + my * my);
    const double dx = (x - cx) / m;
    const double dy = (y - cy) / m;
    const double r = std::sqrt(dx * dx + dy * dy);
    const double f = k[0] + k[1] * std::pow(r, 2) + k[2] * std::pow(r, 4) + k[3] * std::pow(r, 6);
    const double dxr = f * dx;
    const double dyr = f * dy;
    const double dxt = k[4] * (2 * dx * dy) + k[5] * (r * r + 2 * dx * dx);
    const double dyt = k[5] * (2 * dx * dy) + k[4] * (r * r + 2 * dy * dy);
    return {cx + m * (dxr + dxt), cy + m * (dyr + dyt)};
}

TEST(DngOpcodeList3, WarpFollowsTheSpecificationFormulasWithTangentialTerms) {
    const std::array<double, 6> k{0.98, 0.02, -0.01, 0.005, 0.003, -0.002};
    const double chx = 0.4;
    const double chy = 0.6;
    const std::int32_t w = 31;
    const std::int32_t h = 21;
    // A quadratic field: Catmull-Rom reproduces it exactly away from the clamped borders.
    const auto field = [](double x, double y, std::int32_t p) { return 0.5 * x * x + 3.0 * x + 7.0 * y + p; };
    List3Case c = list3_case(w, h, {{L::List3, kWarp, warp({k}, chx, chy)}}, field);
    DngOpcodeProcessor p(c.decoded.metadata);
    ASSERT_TRUE(p.run_list3(c.pixels, {1, 1, 1}));
    int checked = 0;
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            const image::PointD s = spec_source(x, y, w, h, k, chx, chy);
            if (s.x < 1.0 || s.y < 1.0 || s.x > w - 3.0 || s.y > h - 3.0) {
                continue;  // taps would be clamped at the border
            }
            for (std::int32_t pl = 0; pl < 3; ++pl) {
                EXPECT_NEAR(c.pixels.at(x, y, pl), field(s.x, s.y, pl), 2e-4) << x << "," << y;
            }
            ++checked;
        }
    }
    EXPECT_GT(checked, 300);
    // The recorded mapping is the same function (continuous coordinates).
    const image::PointD mapped = p.report().geometry.at(0).source_of({10.5, 7.5});
    const image::PointD spec = spec_source(10, 7, w, h, k, chx, chy);
    EXPECT_NEAR(mapped.x, spec.x + 0.5, 1e-12);
    EXPECT_NEAR(mapped.y, spec.y + 0.5, 1e-12);
}

TEST(DngOpcodeList3, PerPlaneCoefficientsCorrectLateralChromaticAberration) {
    List3Case c = list3_case(9, 9,
                             {{L::List3, kWarp,
                               warp({{1.0, 0, 0, 0, 0, 0}, {0.75, 0, 0, 0, 0, 0}, {0.5, 0, 0, 0, 0, 0}})}},
                             [](double x, double y, std::int32_t) { return x + 100.0 * y; });
    DngOpcodeProcessor p(c.decoded.metadata);
    ASSERT_TRUE(p.run_list3(c.pixels, {1, 1, 1}));
    // Output (0, 4): plane 0 input x = 0; plane 1 x = 4 - 4 x 0.75 = 1; plane 2 x = 2.
    EXPECT_FLOAT_EQ(c.pixels.at(0, 4, 0), 400.0f);
    EXPECT_FLOAT_EQ(c.pixels.at(0, 4, 1), 401.0f);
    EXPECT_FLOAT_EQ(c.pixels.at(0, 4, 2), 402.0f);
    const CarriedLensCorrections carried = carried_lens_corrections(c.decoded.metadata);
    EXPECT_TRUE(carried.distortion);
    EXPECT_TRUE(carried.lateral_chromatic_aberration);
    EXPECT_FALSE(carried.vignetting);
}

TEST(DngOpcodeList3, WarpRectilinear2MatchesTheEquivalentWarpRectilinear) {
    const std::array<double, 6> k{0.98, 0.02, -0.01, 0.005, 0.001, 0.002};
    raw::DngWarpRectilinear w2;
    w2.extended = true;
    raw::DngWarpPlane plane;
    plane.radial[0] = k[0];
    plane.radial[2] = k[1];
    plane.radial[4] = k[2];
    plane.radial[6] = k[3];
    plane.tangential = {k[4], k[5]};
    w2.planes = {plane};
    const auto field = [](double x, double y, std::int32_t p) { return std::sin(x * 0.3) + y * 0.1 * (p + 1); };
    List3Case a = list3_case(17, 13, {{L::List3, kWarp, warp({k})}}, field);
    List3Case b = list3_case(17, 13, {{L::List3, kWarp2, w2, 0, raw::DngVersion{{1, 6, 0, 0}}}}, field);
    ASSERT_EQ(b.decoded.metadata.interpretation.status, raw::InterpretationStatus::Complete);
    DngOpcodeProcessor pa(a.decoded.metadata);
    DngOpcodeProcessor pb(b.decoded.metadata);
    ASSERT_TRUE(pa.run_list3(a.pixels, {1, 1, 1}));
    ASSERT_TRUE(pb.run_list3(b.pixels, {1, 1, 1}));
    EXPECT_LT(max_abs_difference(a.pixels, b.pixels), 1e-5);

    // reciprocalRadial with f = 2 is WarpRectilinear with kr0 = 0.5.
    raw::DngWarpRectilinear r2;
    r2.extended = true;
    r2.reciprocal_radial = true;
    raw::DngWarpPlane two;
    two.radial[0] = 2.0;
    r2.planes = {two};
    List3Case c = list3_case(17, 13, {{L::List3, kWarp, warp({{0.5, 0, 0, 0, 0, 0}})}}, field);
    List3Case d = list3_case(17, 13, {{L::List3, kWarp2, r2, 0, raw::DngVersion{{1, 6, 0, 0}}}}, field);
    DngOpcodeProcessor pc(c.decoded.metadata);
    DngOpcodeProcessor pd(d.decoded.metadata);
    ASSERT_TRUE(pc.run_list3(c.pixels, {1, 1, 1}));
    ASSERT_TRUE(pd.run_list3(d.pixels, {1, 1, 1}));
    EXPECT_LT(max_abs_difference(c.pixels, d.pixels), 1e-6);
}

TEST(DngOpcodeList3, WarpRectilinear2ClampsTheRadiusToItsValidRange) {
    raw::DngWarpRectilinear w2;
    w2.extended = true;
    raw::DngWarpPlane plane;
    plane.radial[0] = 1.0;
    plane.radial[1] = -0.5;  // f(r) = 1 - r / 2, evaluated at min(r, 0.2): 0.9 beyond r = 0.2
    plane.max_valid_radius = 0.2;
    w2.planes = {plane};
    List3Case c = list3_case(21, 21, {{L::List3, kWarp2, w2, 0, raw::DngVersion{{1, 6, 0, 0}}}},
                             [](double x, double y, std::int32_t) { return x + y; });
    DngOpcodeProcessor p(c.decoded.metadata);
    ASSERT_TRUE(p.run_list3(c.pixels, {1, 1, 1}));
    const GeometryChange& g = p.report().geometry.at(0);
    // Output pixel (0, 10): dx = -10 / m, r = 10 / m > 0.2 (m = 14.14): input x = 10 - 0.9 x 10.
    const image::PointD s = g.source_of({0.5, 10.5});
    EXPECT_NEAR(s.x - 0.5, 1.0, 1e-12);
    EXPECT_NEAR(s.y - 0.5, 10.0, 1e-12);
    // Output pixel (9, 10): r = 1 / m < 0.2: f = 1 - r / 2.
    const double r = 1.0 / std::sqrt(200.0);
    EXPECT_NEAR(g.source_of({9.5, 10.5}).x - 0.5, 10.0 - (1.0 - r / 2.0), 1e-12);
}

TEST(DngOpcodeList3, ReciprocalWarpThroughZeroIsCorrupt) {
    raw::DngWarpRectilinear w2;
    w2.extended = true;
    w2.reciprocal_radial = true;
    w2.planes = {raw::DngWarpPlane{}};  // f = 0 everywhere
    w2.planes[0].radial = {};
    List3Case c = list3_case(5, 5, {{L::List3, kWarp2, w2, 0, raw::DngVersion{{1, 6, 0, 0}}}},
                             [](double, double, std::int32_t) { return 1.0; });
    DngOpcodeProcessor p(c.decoded.metadata);
    const Status status = p.run_list3(c.pixels, {1, 1, 1});
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::Corrupt);
    EXPECT_EQ(status.error().where, "engine.dng_opcode");
}

TEST(DngOpcodeList3, VignetteGainFollowsTheRadialPolynomial) {
    // 5 x 3: centre (2, 1), m^2 = 5. Pixel (0, 0): r^2 = 1, g = 1 + k0 + k1 = 1.75.
    List3Case c = list3_case(5, 3, {{L::List3, kVignette, raw::DngFixVignetteRadial{{0.5, 0.25, 0, 0, 0}, 0.5, 0.5}}},
                             [](double, double, std::int32_t) { return 0.4; });
    DngOpcodeProcessor p(c.decoded.metadata);
    ASSERT_TRUE(p.run_list3(c.pixels, {2.0, 1.0, 1.5}));
    EXPECT_FLOAT_EQ(c.pixels.at(0, 0, 0), static_cast<float>(0.4 * 1.75));
    EXPECT_FLOAT_EQ(c.pixels.at(2, 1, 2), 0.4f);
    const double r2 = 1.0 / 5.0;
    EXPECT_FLOAT_EQ(c.pixels.at(1, 1, 1), static_cast<float>(0.4 * (1.0 + 0.5 * r2 + 0.25 * r2 * r2)));
    EXPECT_TRUE(carried_lens_corrections(c.decoded.metadata).vignetting);
}

TEST(DngOpcodeList3, DeltasAndValueMapsActOnUnbalancedValues) {
    // Plane multipliers (2, 1, 1.5): the image holds 0.25 x m in every plane, i.e. 0.25 before
    // white balance.
    std::vector<std::uint16_t> invert(65536);
    for (std::size_t i = 0; i < invert.size(); ++i) {
        invert[i] = static_cast<std::uint16_t>(65535 - i);
    }
    const std::array<double, 3> m{2.0, 1.0, 1.5};
    List3Case c = list3_case(3, 3,
                             {
                                 {L::List3, kDeltaRow, values(area(0, 0, 1, 3, 0, 3), {0.1f})},
                                 {L::List3, kMapTable, raw::DngMapTable{area(1, 0, 2, 3, 0, 3), invert}},
                                 {L::List3, kMapPolynomial, raw::DngMapPolynomial{area(2, 0, 3, 3, 0, 3), {0.0, 0.0, 4.0}}},
                             },
                             [&](double, double, std::int32_t p) { return 0.25 * m[static_cast<std::size_t>(p)]; });
    DngOpcodeProcessor p(c.decoded.metadata);
    ASSERT_TRUE(p.run_list3(c.pixels, m));
    for (std::int32_t pl = 0; pl < 3; ++pl) {
        const double mp = m[static_cast<std::size_t>(pl)];
        EXPECT_FLOAT_EQ(c.pixels.at(1, 0, pl), static_cast<float>((0.25 + static_cast<double>(0.1f)) * mp));
        // round(0.25 x 65535) = 16384 -> 65535 - 16384 = 49151.
        EXPECT_FLOAT_EQ(c.pixels.at(1, 1, pl), static_cast<float>(49151.0 / 65535.0 * mp));
        // 4 x 0.25^2 = 0.25.
        EXPECT_FLOAT_EQ(c.pixels.at(1, 2, pl), static_cast<float>(0.25 * mp));
    }
}

TEST(DngOpcodeList3, ValueMapsLimitToTheLogicalRange) {
    List3Case c = list3_case(2, 1, {{L::List3, kMapPolynomial, raw::DngMapPolynomial{area(0, 0, 1, 2, 0, 3), {0.5, 1.0}}}},
                             [](double x, double, std::int32_t) { return x == 0 ? -0.2 : 1.7; });
    DngOpcodeProcessor p(c.decoded.metadata);
    ASSERT_TRUE(p.run_list3(c.pixels, {1, 1, 1}));
    EXPECT_FLOAT_EQ(c.pixels.at(0, 0, 0), 0.5f);  // input limited to 0
    EXPECT_FLOAT_EQ(c.pixels.at(1, 0, 0), 1.0f);  // input limited to 1, output 1.5 limited to 1
    EXPECT_EQ(record(p, 0).limited_samples, 9u);  // 3 inputs below 0, 3 above 1, 3 outputs above 1
}

TEST(DngOpcodeList3, TrimBoundsCutsTheImageAndRecordsTheCrop) {
    List3Case c = list3_case(6, 4, {{L::List3, kTrim, raw::DngTrimBounds{{1, 2, 3, 5}}}},
                             [](double x, double y, std::int32_t) { return 10 * x + y; });
    DngOpcodeProcessor p(c.decoded.metadata);
    ASSERT_TRUE(p.run_list3(c.pixels, {1, 1, 1}));
    ASSERT_EQ(c.pixels.size(), (image::Size{3, 2}));
    EXPECT_FLOAT_EQ(c.pixels.at(0, 0, 0), 21.0f);
    EXPECT_FLOAT_EQ(c.pixels.at(2, 1, 2), 42.0f);
    ASSERT_EQ(p.report().geometry.size(), 1u);
    EXPECT_EQ(p.report().geometry[0].crop, (PixelRect{2, 1, 3, 2}));
}

TEST(DngOpcodeList3, TrimBoundsOutsideTheImageIsNotSupported) {
    List3Case c = list3_case(6, 4, {{L::List3, kTrim, raw::DngTrimBounds{{0, 0, 5, 6}}}},
                             [](double, double, std::int32_t) { return 1.0; });
    const raw::RequiredOperation* op = operation(c.decoded.metadata, L::List3, 0);
    ASSERT_NE(op, nullptr);
    EXPECT_FALSE(op->supported_by_lucent);
    EXPECT_EQ(c.decoded.metadata.interpretation.status, raw::InterpretationStatus::RequiresUnsupportedOperation);
}

// ---- Support decisions, the skip rule and outcomes -----------------------------------------------

TEST(DngOpcodeOutcomes, OptionalWarpRectilinear2SupersedesTheFollowingWarp) {
    raw::DngWarpRectilinear w2;
    w2.extended = true;
    w2.planes = {raw::DngWarpPlane{}};
    w2.planes[0].radial[0] = 0.9;
    List3Case c = list3_case(9, 9,
                             {
                                 {L::List3, kWarp2, w2, raw::DngOpcode::kFlagOptional, raw::DngVersion{{1, 6, 0, 0}}},
                                 {L::List3, kWarp, warp({{0.5, 0, 0, 0, 0, 0}})},
                             },
                             [](double x, double, std::int32_t) { return x; });
    const raw::RawMetadata& m = c.decoded.metadata;
    EXPECT_EQ(m.interpretation.status, raw::InterpretationStatus::Complete);
    EXPECT_TRUE(operation(m, L::List3, 0)->supported_by_lucent);
    EXPECT_TRUE(operation(m, L::List3, 1)->superseded_by_warp_rectilinear2);
    EXPECT_FALSE(operation(m, L::List3, 1)->supported_by_lucent);
    DngOpcodeProcessor p(m);
    ASSERT_TRUE(p.run_list3(c.pixels, {1, 1, 1}));
    EXPECT_EQ(record(p, 0).outcome, OpcodeOutcome::Applied);
    EXPECT_EQ(record(p, 1).outcome, OpcodeOutcome::SkippedByWarpRule);
    EXPECT_FLOAT_EQ(c.pixels.at(2, 4, 0), static_cast<float>(4.0 - 0.9 * 2.0));  // only the 0.9 warp

    // When Lucent declines the WarpRectilinear2 (malformed parameters), the warp after it applies.
    List3Case d = list3_case(9, 9,
                             {
                                 {L::List3, kWarp2, std::monostate{}, raw::DngOpcode::kFlagOptional,
                                  raw::DngVersion{{1, 6, 0, 0}}, "WarpRectilinear2 (id 14) parameters: truncated"},
                                 {L::List3, kWarp, warp({{0.5, 0, 0, 0, 0, 0}})},
                             },
                             [](double x, double, std::int32_t) { return x; });
    EXPECT_EQ(d.decoded.metadata.interpretation.status, raw::InterpretationStatus::OptionalOperationsSkipped);
    DngOpcodeProcessor q(d.decoded.metadata);
    ASSERT_TRUE(q.run_list3(d.pixels, {1, 1, 1}));
    EXPECT_EQ(record(q, 0).outcome, OpcodeOutcome::SkippedOptional);
    EXPECT_EQ(record(q, 1).outcome, OpcodeOutcome::Applied);
    EXPECT_FLOAT_EQ(d.pixels.at(0, 4, 0), 2.0f);
}

TEST(DngOpcodeOutcomes, UnsupportedOpcodesKeepTheFileUnverified) {
    List3Case optional = list3_case(4, 4, {{L::List3, 2 /* WarpFisheye */, std::monostate{}, raw::DngOpcode::kFlagOptional}},
                                    [](double, double, std::int32_t) { return 1.0; });
    EXPECT_EQ(optional.decoded.metadata.interpretation.status, raw::InterpretationStatus::OptionalOperationsSkipped);
    DngOpcodeProcessor p(optional.decoded.metadata);
    ASSERT_TRUE(p.run_list3(optional.pixels, {1, 1, 1}));
    EXPECT_EQ(record(p, 0).outcome, OpcodeOutcome::SkippedOptional);

    List3Case mandatory = list3_case(4, 4, {{L::List3, 2, std::monostate{}}, {L::List3, 99, std::monostate{}}},
                                     [](double, double, std::int32_t) { return 1.0; });
    EXPECT_EQ(mandatory.decoded.metadata.interpretation.status,
              raw::InterpretationStatus::RequiresUnsupportedOperation);
    EXPECT_EQ(mandatory.decoded.metadata.interpretation.issues.size(), 2u);
    DngOpcodeProcessor q(mandatory.decoded.metadata);
    ASSERT_TRUE(q.run_list3(mandatory.pixels, {1, 1, 1}));
    EXPECT_EQ(record(q, 0).outcome, OpcodeOutcome::NotApplied);
    EXPECT_EQ(record(q, 1).outcome, OpcodeOutcome::NotApplied);

    // An opcode newer than DNG 1.7.1.0 is never processed, even with an ID Lucent knows.
    List3Case newer = list3_case(4, 4,
                                 {{L::List3, kVignette, raw::DngFixVignetteRadial{{1, 0, 0, 0, 0}, 0.5, 0.5}, 0,
                                   raw::DngVersion{{1, 8, 0, 0}}}},
                                 [](double, double, std::int32_t) { return 1.0; });
    EXPECT_EQ(newer.decoded.metadata.interpretation.status, raw::InterpretationStatus::RequiresUnsupportedOperation);
}

TEST(DngOpcodeOutcomes, DecoderAppliedOpcodesAreNeverAppliedTwice) {
    raw::DecodedRaw d = make_mosaic<std::uint16_t>(4, 4, PixelRect{0, 0, 4, 4}, kRggb, simple_levels(0.0, 1000.0),
                                                   [](auto, auto, auto) { return 500.0; });
    attach_dng(d, {{L::List2, kMapPolynomial, raw::DngMapPolynomial{area(0, 0, 4, 4), {0.0, 2.0}}}});
    // As the LibRaw adapter records a lossy DNG's MapPolynomial: applied while unpacking.
    d.metadata.required_operations[0].applied_by_decoder = true;
    d.metadata.interpretation = {};
    raw::assess_dng_opcode_operations(d.metadata);
    EXPECT_FALSE(d.metadata.required_operations[0].supported_by_lucent);
    auto sensor = normalized(d);
    ASSERT_TRUE(sensor);
    DngOpcodeProcessor p(d.metadata);
    ASSERT_TRUE(p.run_list2(*sensor));
    EXPECT_EQ(record(p, 0).outcome, OpcodeOutcome::AppliedByDecoder);
    EXPECT_FLOAT_EQ(sensor->pixels.at(1, 1, 0), 0.5f);
}

TEST(DngOpcodeOutcomes, DisabledProcessingChangesNothingAndSaysSo) {
    List3Case c = list3_case(5, 5, {{L::List3, kWarp, warp({{0.5, 0, 0, 0, 0, 0}})}},
                             [](double x, double, std::int32_t) { return x; });
    const image::ImageBuffer<float> before = *c.pixels.clone();
    DngOpcodeOptions options;
    options.enabled = false;
    DngOpcodeProcessor p(c.decoded.metadata, options);
    ASSERT_TRUE(p.run_list3(c.pixels, {1, 1, 1}));
    EXPECT_TRUE(bit_identical(before, c.pixels));
    EXPECT_EQ(record(p, 0).outcome, OpcodeOutcome::Disabled);
    EXPECT_FALSE(p.report().any_applied());
}

// ---- Stage order through the pipelines ------------------------------------------------------------

// A 16 x 16 RGGB flat field (stored 1000 of white 10000, black 0) with as-shot multipliers
// (2, 1, 1.5) and a decoder colour matrix, carrying one opcode per list.
raw::DecodedRaw ordered_case() {
    raw::DecodedRaw d = make_mosaic<std::uint16_t>(16, 16, PixelRect{0, 0, 16, 16}, kRggb, simple_levels(0.0, 10000.0),
                                                   [](auto, auto, auto) { return 1000.0; });
    set_decoder_matrix(d.metadata, kR5CamXyz);
    d.metadata.color.as_shot_multipliers = {2.0f, 1.0f, 1.5f, 1.0f};
    d.metadata.color.daylight_multipliers = {2.0f, 1.0f, 1.5f, 1.0f};
    attach_dng(d, {
                      // List 1: + 655 stored units -> 1655.
                      {L::List1, kDeltaRow, values(area(0, 0, 16, 16), std::vector<float>(16, 655.0f))},
                      // List 2: + 0.05 on the normalised values (white = 1).
                      {L::List2, kDeltaRow, values(area(0, 0, 16, 16), std::vector<float>(16, 0.05f))},
                      // List 3: green x 2.
                      {L::List3, kScaleRow, values(area(0, 0, 16, 16, 1, 1), std::vector<float>(16, 2.0f))},
                  });
    return d;
}

TEST(DngOpcodeStages, StageOneAppliesTheListsAtTheirStagesAndInOrder) {
    raw::DecodedRaw d = ordered_case();
    ASSERT_EQ(d.metadata.interpretation.status, raw::InterpretationStatus::Complete);
    StageOneRequest request;
    request.kind = StageOneKind::Interactive;
    auto one = render_stage_one(std::move(d), request);
    ASSERT_TRUE(one) << one.error().describe();
    std::vector<RenderStage> order;
    for (const StageRecord& s : one->stages) {
        order.push_back(s.stage);
    }
    EXPECT_EQ(order, (std::vector<RenderStage>{RenderStage::DngOpcodeList1, RenderStage::Normalize,
                                               RenderStage::DngOpcodeList2, RenderStage::WhiteBalance,
                                               RenderStage::Demosaic, RenderStage::DngOpcodeList3,
                                               RenderStage::Orientation}));
    // 1655 / 10000 = 0.1655 + 0.05 = 0.2155, balanced (2, 1, 1.5), then green x 2. A different
    // order or unit (the List 1 delta after normalisation would add 655; the List 2 delta before
    // it 0.05 stored units) would give other values.
    const double v = 0.1655 + static_cast<double>(0.05f);
    EXPECT_NEAR(one->pixels.at(8, 8, 0), v * 2.0, 1e-5);
    EXPECT_NEAR(one->pixels.at(8, 8, 1), v * 2.0, 1e-5);
    EXPECT_NEAR(one->pixels.at(8, 8, 2), v * 1.5, 1e-5);
    ASSERT_EQ(one->dng_opcodes.opcodes.size(), 3u);
    for (const DngOpcodeRecord& r : one->dng_opcodes.opcodes) {
        EXPECT_EQ(r.outcome, OpcodeOutcome::Applied) << r.name;
    }
    EXPECT_EQ(one->stages[0].algorithm, "dng-opcode-list-1");
    EXPECT_EQ(one->stages[0].version, kDngOpcodeProcessingVersion);

    // Stage two carries the records into the develop report.
    auto two = render_stage_two(*one, DevelopSettings{});
    ASSERT_TRUE(two) << two.error().describe();
    EXPECT_EQ(two->report.dng_opcodes.opcodes.size(), 3u);
}

TEST(DngOpcodeStages, RenderPathRecordsOpcodeStagesAndHonoursTheSwitch) {
    RenderOptions options;
    options.keep_scene_linear = true;
    auto applied = render_decoded(ordered_case(), RenderSettings{}, options);
    ASSERT_TRUE(applied) << applied.error().describe();
    std::vector<RenderStage> order;
    for (const StageRecord& s : applied->report.stages) {
        order.push_back(s.stage);
    }
    EXPECT_EQ(order, (std::vector<RenderStage>{RenderStage::DngOpcodeList1, RenderStage::Normalize,
                                               RenderStage::DngOpcodeList2, RenderStage::WhiteBalance,
                                               RenderStage::Demosaic, RenderStage::DngOpcodeList3,
                                               RenderStage::CameraColor, RenderStage::WorkingSpace,
                                               RenderStage::DisplayRendering, RenderStage::Orientation}));
    EXPECT_EQ(applied->report.interpretation.status, raw::InterpretationStatus::Complete);
    EXPECT_TRUE(applied->report.dng_opcodes.any_applied());

    RenderSettings off;
    off.apply_dng_opcodes = false;
    auto skipped = render_decoded(ordered_case(), off, options);
    ASSERT_TRUE(skipped) << skipped.error().describe();
    EXPECT_EQ(skipped->report.interpretation.status, raw::InterpretationStatus::RequiresUnsupportedOperation);
    EXPECT_EQ(skipped->report.interpretation.issues.size(), 3u);
    for (const DngOpcodeRecord& r : skipped->report.dng_opcodes.opcodes) {
        EXPECT_EQ(r.outcome, OpcodeOutcome::Disabled);
    }
    EXPECT_FALSE(skipped->report.warnings.empty());
    // The metadata of the file is unchanged: the file itself is fully supported.
    EXPECT_EQ(skipped->metadata.interpretation.status, raw::InterpretationStatus::Complete);
    EXPECT_GT(max_abs_difference(applied->scene_linear->pixels, skipped->scene_linear->pixels), 0.01);
}

TEST(DngOpcodeStages, MemoryEstimateCountsTheWarpBuffer) {
    raw::DecodedRaw d = make_mosaic<std::uint16_t>(64, 48, PixelRect{0, 0, 64, 48}, kRggb, simple_levels(0.0, 1000.0),
                                                   [](auto, auto, auto) { return 100.0; });
    raw::DecodedRaw plain = make_mosaic<std::uint16_t>(64, 48, PixelRect{0, 0, 64, 48}, kRggb,
                                                       simple_levels(0.0, 1000.0), [](auto, auto, auto) { return 100.0; });
    attach_dng(d, {{L::List3, kWarp, warp({{0.99, 0, 0, 0, 0, 0}})}});
    auto with = estimate_render_memory(d.metadata, RenderSettings{}, RenderOptions{});
    auto without = estimate_render_memory(plain.metadata, RenderSettings{}, RenderOptions{});
    ASSERT_TRUE(with && without);
    EXPECT_EQ(with->opcode_peak, 2 * with->rgb_bytes);
    EXPECT_EQ(without->opcode_peak, 0u);
    RenderSettings off;
    off.apply_dng_opcodes = false;
    EXPECT_EQ(estimate_render_memory(d.metadata, off, RenderOptions{})->opcode_peak, 0u);
}

TEST(DngOpcodeStages, ResultsDoNotDependOnParallelism) {
    const auto field = [](double x, double y, std::int32_t p) { return std::cos(x * 0.21) * std::sin(y * 0.13) + p; };
    const std::vector<TestOpcode> opcodes = {
        {L::List3, kGainMap,
         gain_map(area(0, 0, 40, 50, 0, 3), 3, 4, 0.5, 1.0 / 3.0, 0.0, 0.0, 2,
                  {1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f, 1.7f, 1.8f, 1.9f, 2.0f, 2.1f, 2.2f, 2.3f, 2.4f, 2.5f, 2.6f,
                   2.7f, 2.8f, 2.9f, 3.0f, 3.1f, 3.2f, 3.3f})},
        {L::List3, kWarp, warp({{0.97, 0.03, -0.02, 0.01, 0.001, -0.001}}, 0.45, 0.55)},
    };
    List3Case a = list3_case(50, 40, opcodes, field);
    List3Case b = list3_case(50, 40, opcodes, field);
    DngOpcodeProcessor pa(a.decoded.metadata);
    DngOpcodeOptions serial;
    serial.parallel = false;
    DngOpcodeProcessor pb(b.decoded.metadata, serial);
    ASSERT_TRUE(pa.run_list3(a.pixels, {1.2, 1.0, 0.8}));
    ASSERT_TRUE(pb.run_list3(b.pixels, {1.2, 1.0, 0.8}));
    EXPECT_TRUE(bit_identical(a.pixels, b.pixels));
}

TEST(DngOpcodeStages, GainMapPlanesBeyondTheMapUseItsLastPlane) {
    // Planes 0..2 modified, a 2-plane map: plane 2 uses map plane 1.
    List3Case c = list3_case(2, 2, {{L::List3, kGainMap, gain_map(area(0, 0, 2, 2, 0, 3), 1, 1, 1, 1, 0, 0, 2, {2.0f, 3.0f})}},
                             [](double, double, std::int32_t) { return 1.0; });
    DngOpcodeProcessor p(c.decoded.metadata);
    ASSERT_TRUE(p.run_list3(c.pixels, {1, 1, 1}));
    EXPECT_FLOAT_EQ(c.pixels.at(1, 1, 0), 2.0f);
    EXPECT_FLOAT_EQ(c.pixels.at(1, 1, 1), 3.0f);
    EXPECT_FLOAT_EQ(c.pixels.at(1, 1, 2), 3.0f);
    EXPECT_TRUE(carried_lens_corrections(c.decoded.metadata).vignetting);
}

// ---- Review findings (W9 phase-1 closure) ----------------------------------------------------------

TEST(DngOpcodeList2, BadPixelRepairOfASaturatedPixelIsKept) {
    // 8 x 8 RGGB, white 1000. A hot red pixel (2, 2) reads white among red neighbours of 400; a
    // second listed red pixel (6, 6) has only saturated red neighbours (4, 6) and (6, 4) inside
    // the image. A gain of 0.5 follows the repair.
    raw::DecodedRaw d = make_mosaic<std::uint16_t>(8, 8, PixelRect{0, 0, 8, 8}, kRggb, simple_levels(0.0, 1000.0),
                                                   [](auto x, auto y, auto) {
                                                       if ((x == 2 && y == 2) || (x == 6 && y == 6) ||
                                                           (x == 4 && y == 6) || (x == 6 && y == 4)) {
                                                           return 1000.0;
                                                       }
                                                       return 400.0;
                                                   });
    raw::DngFixBadPixelsList list;
    list.bayer_phase = 0;
    list.points = {{2, 2}, {6, 6}};
    attach_dng(d, {
                      {L::List2, kBadList, list},
                      {L::List2, kGainMap, gain_map(area(0, 0, 8, 8), 1, 1, 1.0, 1.0, 0.0, 0.0, 1, {0.5f})},
                  });
    ASSERT_EQ(d.metadata.interpretation.status, raw::InterpretationStatus::Complete);
    auto sensor = normalized(d);
    ASSERT_TRUE(sensor);
    EXPECT_EQ(sensor->normalization.saturated_samples, 4u);
    DngOpcodeProcessor p(d.metadata);
    ASSERT_TRUE(p.run_list2(*sensor));
    // The repaired hot pixel keeps its interpolated value (0.4, then the gain): its stored value
    // was discarded, and with it the saturation flag.
    EXPECT_FLOAT_EQ(sensor->pixels.at(2, 2, 0), 0.2f);
    EXPECT_EQ(sensor->saturation.at(2, 2), 0u);
    // A pixel repaired from saturated neighbours reaches the saturation level and stays flagged.
    EXPECT_FLOAT_EQ(sensor->pixels.at(6, 6, 0), 1.0f);
    EXPECT_NE(sensor->saturation.at(6, 6), 0u);
    EXPECT_FLOAT_EQ(sensor->pixels.at(4, 6, 0), 1.0f);
    EXPECT_EQ(p.report().list2_resaturated_samples, 3u);  // (6, 6), (4, 6), (6, 4)
    EXPECT_EQ(record(p, 0).affected_samples, 2u);
}

TEST(DngOpcodeList3, EmptyAreasCoverTheWholeImage) {
    // The DNG SDK reads an empty area rectangle as the whole image (dng_area_spec::Overlap).
    const raw::DngOpcodeArea empty = area(0, 0, 0, 0, 0, 3);
    List3Case c = list3_case(4, 3,
                             {
                                 {L::List3, kGainMap, gain_map(empty, 1, 1, 1.0, 1.0, 0.0, 0.0, 1, {2.0f})},
                                 {L::List3, kMapPolynomial, raw::DngMapPolynomial{empty, {0.05, 1.0}}},
                             },
                             [](double, double, std::int32_t) { return 0.25; });
    ASSERT_EQ(c.decoded.metadata.interpretation.status, raw::InterpretationStatus::Complete);
    DngOpcodeProcessor p(c.decoded.metadata);
    ASSERT_TRUE(p.run_list3(c.pixels, {1, 1, 1}));
    for (std::int32_t y = 0; y < 3; ++y) {
        for (std::int32_t x = 0; x < 4; ++x) {
            for (std::int32_t pl = 0; pl < 3; ++pl) {
                EXPECT_FLOAT_EQ(c.pixels.at(x, y, pl), 0.55f) << x << "," << y << "," << pl;
            }
        }
    }
    EXPECT_EQ(record(p, 0).affected_samples, 36u);
    EXPECT_NE(record(p, 0).detail.find("the whole image"), std::string::npos) << record(p, 0).detail;
    EXPECT_TRUE(carried_lens_corrections(c.decoded.metadata).vignetting);

    // A per-row opcode with an empty area holds no values for the image: not supported.
    List3Case rows = list3_case(4, 3, {{L::List3, kDeltaRow, values(empty, {})}},
                                [](double, double, std::int32_t) { return 0.25; });
    EXPECT_EQ(rows.decoded.metadata.interpretation.status, raw::InterpretationStatus::RequiresUnsupportedOperation);
}

TEST(DngOpcodeList3, GainMapPlanesAreIndexedByTheImagePlane) {
    // Planes 1..2 modified with a 2-plane map: the DNG SDK takes map plane min(p, MapPlanes - 1)
    // for image plane p, so both use map plane 1 (gain 3); plane 0 is not modified.
    List3Case c = list3_case(2, 2,
                             {{L::List3, kGainMap, gain_map(area(0, 0, 2, 2, 1, 2), 1, 1, 1, 1, 0, 0, 2, {2.0f, 3.0f})}},
                             [](double, double, std::int32_t) { return 1.0; });
    DngOpcodeProcessor p(c.decoded.metadata);
    ASSERT_TRUE(p.run_list3(c.pixels, {1, 1, 1}));
    EXPECT_FLOAT_EQ(c.pixels.at(1, 1, 0), 1.0f);
    EXPECT_FLOAT_EQ(c.pixels.at(1, 1, 1), 3.0f);
    EXPECT_FLOAT_EQ(c.pixels.at(1, 1, 2), 3.0f);
}

TEST(DngOpcodeList3, HugeFiniteCoefficientsAreBounded) {
    // A malformed but finite vignette (k0 = 1e300) cannot push values beyond the bound (or to
    // infinity), so every later stage sees finite values.
    List3Case c = list3_case(5, 3, {{L::List3, kVignette, raw::DngFixVignetteRadial{{1e300, 0, 0, 0, 0}, 0.5, 0.5}}},
                             [](double, double, std::int32_t) { return 0.4; });
    DngOpcodeProcessor p(c.decoded.metadata);
    ASSERT_TRUE(p.run_list3(c.pixels, {1, 1, 1}));
    EXPECT_FLOAT_EQ(c.pixels.at(2, 1, 0), 0.4f);  // the centre: r = 0, g = 1
    EXPECT_FLOAT_EQ(c.pixels.at(0, 0, 0), static_cast<float>(kDngOpcodeValueBound));
    for (std::int32_t y = 0; y < 3; ++y) {
        for (std::int32_t x = 0; x < 5; ++x) {
            EXPECT_TRUE(std::isfinite(c.pixels.at(x, y, 1)));
            EXPECT_LE(c.pixels.at(x, y, 1), kDngOpcodeValueBound);
        }
    }
    EXPECT_EQ(record(p, 0).limited_samples, 3u * (15u - 1u));  // all but the centre pixel
    // Results inside the bound are not changed: Lucent still does not clip at white.
    List3Case bright = list3_case(2, 1, {{L::List3, kScaleRow, values(area(0, 0, 1, 2, 0, 3), {3.0f})}},
                                  [](double, double, std::int32_t) { return 0.9; });
    DngOpcodeProcessor q(bright.decoded.metadata);
    ASSERT_TRUE(q.run_list3(bright.pixels, {1, 1, 1}));
    EXPECT_FLOAT_EQ(bright.pixels.at(0, 0, 0), 2.7f);
    EXPECT_EQ(record(q, 0).limited_samples, 0u);
}

TEST(DngOpcodeList3, AnInconsistentGainMapMarkedSupportedIsAnErrorNotARead) {
    // Metadata altered after the assessment (the parser and the assessment never produce it): the
    // processor refuses instead of reading past the gains.
    List3Case c = list3_case(4, 4, {{L::List3, kGainMap, gain_map(area(0, 0, 4, 4, 0, 3), 1, 1, 1, 1, 0, 0, 1, {2.0f})}},
                             [](double, double, std::int32_t) { return 1.0; });
    ASSERT_TRUE(operation(c.decoded.metadata, L::List3, 0)->supported_by_lucent);
    ASSERT_TRUE(raw::check_consistency(c.decoded));
    auto& map = std::get<raw::DngGainMap>(c.decoded.metadata.dng->raw_ifd.opcodes[0].parameters);
    map.points_v = 1u << 22;
    map.points_h = 1u << 21;
    map.map_planes = 1u << 21;
    map.gains.clear();
    DngOpcodeProcessor p(c.decoded.metadata);
    const Status status = p.run_list3(c.pixels, {1, 1, 1});
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::Internal);
    EXPECT_EQ(status.error().where, "engine.dng_opcode");
    // check_consistency (the decoded-RAW contract) rejects such metadata as well.
    const Status consistent = raw::check_consistency(c.decoded);
    ASSERT_FALSE(consistent);
    EXPECT_NE(consistent.error().message.find("GainMap is marked supported"), std::string::npos)
        << consistent.error().message;
}

TEST(DngOpcodeOutcomes, TrimBoundsAreAssessedAgainstTheTrimmedImage) {
    // 6 x 4: the first TrimBounds makes the image 5 x 3. A second one that fits the original image
    // but not the trimmed one cannot be applied: the file is not Complete (it used to be, and the
    // render then failed with Corrupt).
    List3Case two = list3_case(6, 4,
                               {
                                   {L::List3, kTrim, raw::DngTrimBounds{{0, 0, 3, 5}}},
                                   {L::List3, kTrim, raw::DngTrimBounds{{0, 0, 4, 6}}},
                               },
                               [](double x, double y, std::int32_t) { return 10 * x + y; });
    const raw::RawMetadata& m = two.decoded.metadata;
    EXPECT_TRUE(operation(m, L::List3, 0)->supported_by_lucent);
    const raw::RequiredOperation* second = operation(m, L::List3, 1);
    ASSERT_NE(second, nullptr);
    EXPECT_FALSE(second->supported_by_lucent);
    EXPECT_NE(second->lucent_note.find("5 x 3"), std::string::npos) << second->lucent_note;
    EXPECT_EQ(m.interpretation.status, raw::InterpretationStatus::RequiresUnsupportedOperation);
    EXPECT_TRUE(raw::check_consistency(two.decoded));
    DngOpcodeProcessor p(m);
    ASSERT_TRUE(p.run_list3(two.pixels, {1, 1, 1}));  // the first trim applies, the second is not applied
    EXPECT_EQ(two.pixels.size(), (image::Size{5, 3}));
    EXPECT_EQ(record(p, 1).outcome, OpcodeOutcome::NotApplied);

    // One that fits the trimmed image applies after it.
    List3Case nested = list3_case(6, 4,
                                  {
                                      {L::List3, kTrim, raw::DngTrimBounds{{0, 0, 3, 5}}},
                                      {L::List3, kTrim, raw::DngTrimBounds{{1, 1, 3, 5}}},
                                  },
                                  [](double x, double y, std::int32_t) { return 10 * x + y; });
    ASSERT_EQ(nested.decoded.metadata.interpretation.status, raw::InterpretationStatus::Complete);
    DngOpcodeProcessor q(nested.decoded.metadata);
    ASSERT_TRUE(q.run_list3(nested.pixels, {1, 1, 1}));
    ASSERT_EQ(nested.pixels.size(), (image::Size{4, 2}));
    EXPECT_FLOAT_EQ(nested.pixels.at(0, 0, 0), 11.0f);

    // A List 2 TrimBounds makes the image smaller for List 3 as well.
    raw::DecodedRaw d = make_mosaic<std::uint16_t>(6, 6, PixelRect{0, 0, 6, 6}, kRggb, simple_levels(0.0, 1000.0),
                                                   [](auto, auto, auto) { return 100.0; });
    attach_dng(d, {
                      {L::List2, kTrim, raw::DngTrimBounds{{1, 1, 5, 5}}},
                      {L::List3, kTrim, raw::DngTrimBounds{{0, 0, 5, 5}}},
                  });
    EXPECT_TRUE(operation(d.metadata, L::List2, 0)->supported_by_lucent);
    EXPECT_FALSE(operation(d.metadata, L::List3, 0)->supported_by_lucent);
    EXPECT_NE(operation(d.metadata, L::List3, 0)->lucent_note.find("4 x 4"), std::string::npos)
        << operation(d.metadata, L::List3, 0)->lucent_note;
}

TEST(DngOpcodeOutcomes, TrimBoundsThatKeepNoHeldPixelIsNotSupported) {
    // ActiveArea (1, 1)..(8, 8); the decoder's visible area starts at (2, 2): stage pixel (0, 0)
    // is not in Lucent's buffer, so a TrimBounds keeping only it cannot be applied.
    raw::DecodedRaw d = make_mosaic<std::uint16_t>(8, 8, PixelRect{2, 2, 6, 6}, kRggb, simple_levels(0.0, 1000.0),
                                                   [](auto, auto, auto) { return 100.0; });
    attach_dng(d, {{L::List2, kTrim, raw::DngTrimBounds{{0, 0, 1, 1}}}}, raw::DngRect{1, 1, 8, 8});
    const raw::RequiredOperation* op = operation(d.metadata, L::List2, 0);
    ASSERT_NE(op, nullptr);
    EXPECT_FALSE(op->supported_by_lucent);
    EXPECT_NE(op->lucent_note.find("keeps no pixel"), std::string::npos) << op->lucent_note;
}

TEST(DngOpcodeOutcomes, RadialOpcodesOnNonSquarePixelsAreNotSupported) {
    List3Case c = list3_case(6, 4, {}, [](double, double, std::int32_t) { return 1.0; });
    c.decoded.metadata.geometry.pixel_aspect = 0.5;
    c.decoded.metadata.required_operations.clear();
    c.decoded.metadata.interpretation = {};
    attach_dng(c.decoded, {{L::List3, kVignette, raw::DngFixVignetteRadial{{0.5, 0, 0, 0, 0}, 0.5, 0.5}},
                           {L::List3, kWarp, warp({{0.99, 0, 0, 0, 0, 0}})}});
    EXPECT_FALSE(operation(c.decoded.metadata, L::List3, 0)->supported_by_lucent);
    EXPECT_FALSE(operation(c.decoded.metadata, L::List3, 1)->supported_by_lucent);
    EXPECT_NE(operation(c.decoded.metadata, L::List3, 0)->lucent_note.find("non-square"), std::string::npos);
    EXPECT_EQ(c.decoded.metadata.interpretation.status, raw::InterpretationStatus::RequiresUnsupportedOperation);
    DngOpcodeProcessor p(c.decoded.metadata);
    const image::ImageBuffer<float> before = *c.pixels.clone();
    ASSERT_TRUE(p.run_list3(c.pixels, {1, 1, 1}));
    EXPECT_TRUE(bit_identical(before, c.pixels));
}

}  // namespace
}  // namespace lucent::engine
