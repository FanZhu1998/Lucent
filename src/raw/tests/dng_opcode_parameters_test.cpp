// Unit tests for DNG opcode parameter decoding (parse_dng_opcode_parameters,
// decode_dng_opcode_parameters) and Lucent's support table (assess_dng_opcode), DNG 1.7.1.0
// chapter 7: exact layouts, bounds, version checks and malformed payloads.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/raw/dng_opcodes.hpp>
#include <lucent/raw/dng_tags.hpp>
#include <lucent/raw/tiff_inspector.hpp>

#include "tiff_test_writer.hpp"

namespace lucent::raw {
namespace {

namespace ts = test_support;
using ts::AreaSpec;
using ts::ParameterWriter;

template <class T>
T parse_as(std::uint32_t id, const std::vector<std::byte>& bytes) {
    auto parsed = parse_dng_opcode_parameters(id, bytes);
    EXPECT_TRUE(parsed) << (parsed ? std::string() : parsed.error().describe());
    if (!parsed || !std::holds_alternative<T>(*parsed)) {
        ADD_FAILURE() << "unexpected alternative";
        return T{};
    }
    return std::get<T>(*parsed);
}

void expect_corrupt(std::uint32_t id, const std::vector<std::byte>& bytes, std::string_view mention = {}) {
    auto parsed = parse_dng_opcode_parameters(id, bytes);
    ASSERT_FALSE(parsed) << "id " << id << " (" << bytes.size() << " bytes) parsed";
    EXPECT_EQ(parsed.error().code, ErrorCode::Corrupt) << parsed.error().describe();
    EXPECT_EQ(parsed.error().where, "raw.dng.opcode_parameters");
    if (!mention.empty()) {
        EXPECT_NE(parsed.error().message.find(mention), std::string::npos) << parsed.error().message;
    }
}

std::vector<std::byte> without_last(std::vector<std::byte> bytes) {
    bytes.pop_back();
    return bytes;
}

std::vector<std::byte> with_extra(std::vector<std::byte> bytes) {
    bytes.push_back(std::byte{0});
    return bytes;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---- Layouts -------------------------------------------------------------------------------------

TEST(DngOpcodeParameters, WarpRectilinear) {
    const auto bytes = ts::warp_rectilinear_parameters(
        {{0.99, -0.01, 0.002, 0.0003, 1e-4, -2e-4}, {1.0, 0, 0, 0, 0, 0}, {0.98, 0, 0, 0, 0, 0}}, 0.25, 0.75);
    ASSERT_EQ(bytes.size(), 4u + 48u * 3 + 16u);
    const auto w = parse_as<DngWarpRectilinear>(1, bytes);
    EXPECT_FALSE(w.extended);
    ASSERT_EQ(w.planes.size(), 3u);
    EXPECT_EQ(w.planes[0].radial[0], 0.99);
    EXPECT_EQ(w.planes[0].radial[2], -0.01);
    EXPECT_EQ(w.planes[0].radial[4], 0.002);
    EXPECT_EQ(w.planes[0].radial[6], 0.0003);
    EXPECT_EQ(w.planes[0].radial[1], 0.0);  // odd powers are not part of WarpRectilinear
    EXPECT_EQ(w.planes[0].tangential, (std::array<double, 2>{1e-4, -2e-4}));
    EXPECT_EQ(w.planes[0].min_valid_radius, 0.0);
    EXPECT_EQ(w.planes[0].max_valid_radius, 1.0);
    EXPECT_EQ(w.planes[2].radial[0], 0.98);
    EXPECT_EQ(w.center_x, 0.25);
    EXPECT_EQ(w.center_y, 0.75);

    expect_corrupt(1, without_last(bytes), "need 160 more bytes but 159 remain");
    expect_corrupt(1, with_extra(bytes), "need 160 more bytes but 161 remain");
    expect_corrupt(1, ParameterWriter().u32(1).f64(1.0).bytes(), "need 64 more bytes");
    expect_corrupt(1, ts::warp_rectilinear_parameters({}), "N = 0");
    expect_corrupt(1, ts::warp_rectilinear_parameters(std::vector<std::array<double, 6>>(5, {1, 0, 0, 0, 0, 0})),
                   "N = 5");
    expect_corrupt(1, ts::warp_rectilinear_parameters({{1, 0, 0, 0, 0, 0}}, 1.5, 0.5), "optical centre");
    expect_corrupt(1, ts::warp_rectilinear_parameters({{kNaN, 0, 0, 0, 0, 0}}), "not finite");
    // A huge N is rejected before anything is allocated.
    expect_corrupt(1, ParameterWriter().u32(0xFFFFFFFF).bytes(), "N = 4294967295");
}

TEST(DngOpcodeParameters, WarpRectilinear2) {
    ts::Warp2Set set;
    for (std::size_t p = 0; p < 15; ++p) {
        set.radial[p] = 0.01 * static_cast<double>(p);
    }
    set.radial[0] = 1.0;
    set.tangential = {0.001, 0.002};
    set.min_valid_radius = 0.1;
    set.max_valid_radius = 0.9;
    const auto bytes = ts::warp_rectilinear2_parameters({set, set}, 0.4, 0.6, 7);
    ASSERT_EQ(bytes.size(), 4u + 152u * 2 + 20u);
    const auto w = parse_as<DngWarpRectilinear>(14, bytes);
    EXPECT_TRUE(w.extended);
    ASSERT_EQ(w.planes.size(), 2u);
    EXPECT_EQ(w.planes[1].radial[14], 0.14);
    EXPECT_EQ(w.planes[1].radial[0], 1.0);
    EXPECT_EQ(w.planes[0].tangential[1], 0.002);
    EXPECT_EQ(w.planes[0].min_valid_radius, 0.1);
    EXPECT_EQ(w.planes[0].max_valid_radius, 0.9);
    EXPECT_TRUE(w.reciprocal_radial);
    EXPECT_EQ(w.center_y, 0.6);
    EXPECT_FALSE(parse_as<DngWarpRectilinear>(14, ts::warp_rectilinear2_parameters({set})).reciprocal_radial);

    expect_corrupt(14, without_last(bytes));
    for (const auto& [lo, hi] : std::vector<std::pair<double, double>>{{0.5, 0.5}, {0.6, 0.5}, {-0.1, 1.0}, {0.0, 1.1}}) {
        ts::Warp2Set bad = set;
        bad.min_valid_radius = lo;
        bad.max_valid_radius = hi;
        expect_corrupt(14, ts::warp_rectilinear2_parameters({bad}), "valid radius");
    }
}

TEST(DngOpcodeParameters, FixVignetteRadial) {
    const auto bytes = ts::vignette_parameters({0.5, -0.25, 0.125, 0.0625, 0.03125}, 0.5, 0.45);
    ASSERT_EQ(bytes.size(), 56u);
    const auto v = parse_as<DngFixVignetteRadial>(3, bytes);
    EXPECT_EQ(v.k, (std::array<double, 5>{0.5, -0.25, 0.125, 0.0625, 0.03125}));
    EXPECT_EQ(v.center_y, 0.45);
    expect_corrupt(3, without_last(bytes));
    expect_corrupt(3, with_extra(bytes));
    expect_corrupt(3, ts::vignette_parameters({0, 0, 0, 0, 0}, -0.01, 0.5), "optical centre");
    expect_corrupt(3, ts::vignette_parameters({0, 0, std::numeric_limits<double>::infinity(), 0, 0}), "not finite");
}

TEST(DngOpcodeParameters, BadPixels) {
    const auto c = parse_as<DngFixBadPixelsConstant>(4, ts::bad_pixels_constant_parameters(65535, 3));
    EXPECT_EQ(c.constant, 65535u);
    EXPECT_EQ(c.bayer_phase, 3u);
    expect_corrupt(4, ts::bad_pixels_constant_parameters(0, 4), "BayerPhase 4");
    expect_corrupt(4, ParameterWriter().u32(0).bytes());

    const auto bytes = ts::bad_pixels_list_parameters(2, {{1, 2}, {3, 4}}, {{10, 20, 11, 25}});
    ASSERT_EQ(bytes.size(), 12u + 16u + 16u);
    const auto list = parse_as<DngFixBadPixelsList>(5, bytes);
    EXPECT_EQ(list.bayer_phase, 2u);
    ASSERT_EQ(list.points.size(), 2u);
    EXPECT_EQ(list.points[1].row, 3u);
    EXPECT_EQ(list.points[1].column, 4u);
    ASSERT_EQ(list.rects.size(), 1u);
    EXPECT_EQ(list.rects[0], (DngOpcodeRect{10, 20, 11, 25}));
    expect_corrupt(5, without_last(bytes));
    expect_corrupt(5, ts::bad_pixels_list_parameters(0, {}, {{5, 5, 5, 6}}), "empty");
    expect_corrupt(5, ts::bad_pixels_list_parameters(9, {}), "BayerPhase");
    // Counts far beyond the payload are rejected before allocation.
    expect_corrupt(5, ParameterWriter().u32(0).u32(0xFFFFFFFF).u32(0xFFFFFFFF).bytes(), "points");
    EXPECT_TRUE(parse_as<DngFixBadPixelsList>(5, ts::bad_pixels_list_parameters(1, {})).points.empty());
}

TEST(DngOpcodeParameters, TrimBounds) {
    const auto t = parse_as<DngTrimBounds>(6, ts::trim_bounds_parameters(1, 2, 30, 40));
    EXPECT_EQ(t.bounds, (DngOpcodeRect{1, 2, 30, 40}));
    expect_corrupt(6, ts::trim_bounds_parameters(5, 0, 5, 10), "empty");
    expect_corrupt(6, ts::trim_bounds_parameters(0, 10, 5, 2), "empty");
    expect_corrupt(6, with_extra(ts::trim_bounds_parameters(0, 0, 1, 1)));
}

TEST(DngOpcodeParameters, MapTableAndPolynomial) {
    const AreaSpec a{2, 3, 10, 12, 1, 2, 2, 3};
    const auto table = parse_as<DngMapTable>(7, ts::map_table_parameters(a, {0, 100, 65535}));
    EXPECT_EQ(table.area.rect, (DngOpcodeRect{2, 3, 10, 12}));
    EXPECT_EQ(table.area.plane, 1u);
    EXPECT_EQ(table.area.planes, 2u);
    EXPECT_EQ(table.area.row_pitch, 2u);
    EXPECT_EQ(table.area.col_pitch, 3u);
    EXPECT_EQ(table.area.affected_rows(), 4u);
    EXPECT_EQ(table.area.affected_columns(), 3u);
    EXPECT_EQ(table.table, (std::vector<std::uint16_t>{0, 100, 65535}));
    expect_corrupt(7, ts::map_table_parameters(a, {}), "TableSize 0");
    expect_corrupt(7, ParameterWriter(ts::with_area(a)).u32(65537).bytes(), "TableSize 65537");
    EXPECT_EQ(parse_as<DngMapTable>(7, ts::map_table_parameters(a, std::vector<std::uint16_t>(65536, 1))).table.size(),
              65536u);

    const auto poly = parse_as<DngMapPolynomial>(8, ts::map_polynomial_parameters(a, {0.1, 0.9, -0.05}));
    EXPECT_EQ(poly.coefficients, (std::vector<double>{0.1, 0.9, -0.05}));
    expect_corrupt(8, ts::map_polynomial_parameters(a, std::vector<double>(10, 0.1)), "Degree 9");
    expect_corrupt(8, without_last(ts::map_polynomial_parameters(a, {0.0, 1.0})));
    expect_corrupt(8, ts::map_polynomial_parameters(a, {0.0, kNaN}), "not finite");
    EXPECT_EQ(parse_as<DngMapPolynomial>(8, ts::map_polynomial_parameters(a, std::vector<double>(9, 0.1)))
                  .coefficients.size(),
              9u);
}

TEST(DngOpcodeParameters, AreaChecks) {
    expect_corrupt(7, ts::map_table_parameters({5, 0, 4, 10}, {1}), "inverted");
    expect_corrupt(7, ts::map_table_parameters({0, 0, 4, 10, 0, 0}, {1}), "Planes 0");
    expect_corrupt(7, ts::map_table_parameters({0, 0, 4, 10, 0, 1, 0, 1}, {1}), "RowPitch 0");
    expect_corrupt(7, ts::map_table_parameters({0, 0, 4, 10, 0, 1, 1, 0}, {1}), "ColPitch 0");
    // An empty area is valid with pitch 1 (it stands for the whole image, as in the DNG SDK, whose
    // dng_area_spec rejects an empty area with another pitch).
    const auto empty = parse_as<DngMapTable>(7, ts::map_table_parameters({4, 4, 4, 9}, {1}));
    EXPECT_TRUE(empty.area.rect.empty());
    EXPECT_EQ(empty.area.affected_rows(), 0u);
    expect_corrupt(7, ts::map_table_parameters({4, 4, 4, 9, 0, 1, 2, 1}, {1}), "an empty area");
    expect_corrupt(9, ts::gain_map_parameters({0, 0, 0, 0, 0, 3, 1, 2}, 1, 1, 1, 1, 0, 0, 1, {2.0f}), "ColPitch 2");
    // Extents near the top of the 32-bit range do not overflow.
    const auto wide =
        parse_as<DngMapTable>(7, ts::map_table_parameters({0, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFF0, 0xFFFFFFFF}, {1}));
    EXPECT_EQ(wide.area.affected_rows(), 0xFFFFFFFFull);
}

TEST(DngOpcodeParameters, GainMap) {
    const AreaSpec a{0, 0, 100, 200, 0, 3, 1, 1};
    std::vector<float> gains;
    for (int i = 0; i < 2 * 3 * 2; ++i) {
        gains.push_back(1.0f + 0.1f * static_cast<float>(i));
    }
    const auto bytes = ts::gain_map_parameters(a, 2, 3, 0.5, 0.25, 0.1, 0.2, 2, gains);
    ASSERT_EQ(bytes.size(), 76u + 4u * 12u);
    const auto g = parse_as<DngGainMap>(9, bytes);
    EXPECT_EQ(g.points_v, 2u);
    EXPECT_EQ(g.points_h, 3u);
    EXPECT_EQ(g.spacing_v, 0.5);
    EXPECT_EQ(g.spacing_h, 0.25);
    EXPECT_EQ(g.origin_v, 0.1);
    EXPECT_EQ(g.origin_h, 0.2);
    EXPECT_EQ(g.map_planes, 2u);
    // Row by row, the planes of a point together: point (v = 1, h = 2), plane 1 is entry 11.
    EXPECT_EQ(g.gain(1, 2, 1), gains[11]);
    EXPECT_EQ(g.gain(0, 1, 0), gains[2]);

    expect_corrupt(9, without_last(bytes));
    expect_corrupt(9, with_extra(bytes));
    expect_corrupt(9, ts::gain_map_parameters(a, 0, 3, 0.5, 0.25, 0, 0, 2, {}), "at least 1");
    expect_corrupt(9, ts::gain_map_parameters(a, 2, 1, 0.0, 0.0, 0, 0, 1, {1, 1}), "positive");
    // One point needs no spacing.
    EXPECT_EQ(parse_as<DngGainMap>(9, ts::gain_map_parameters(a, 1, 1, 0.0, 0.0, 0, 0, 1, {2.0f})).gains.size(), 1u);
    expect_corrupt(9, ts::gain_map_parameters(a, 1, 1, 1, 1, 0, 0, 1, {std::numeric_limits<float>::quiet_NaN()}),
                   "not finite");
    // Grid dimensions whose product exceeds the payload are rejected before allocation.
    expect_corrupt(9, ts::gain_map_parameters(a, 0xFFFF, 0xFFFF, 1, 1, 0, 0, 0xFFFF, {}), "gains");
    // 2^22 x 2^21 x 2^21 = 2^64 wraps a 64-bit product to 0: without the overflow check the map
    // would parse with no gains at all (and a renderer would read past them).
    EXPECT_FALSE(dng_gain_map_count(1u << 22, 1u << 21, 1u << 21));
    EXPECT_FALSE(dng_gain_map_count(0xFFFFFFFF, 0xFFFFFFFF, 2));
    EXPECT_EQ(dng_gain_map_count(0xFFFFFFFF, 0xFFFFFFFF, 1), 0xFFFFFFFE00000001ull);
    EXPECT_EQ(dng_gain_map_count(32, 32, 3), 3072u);
    expect_corrupt(9, ts::gain_map_parameters(a, 1u << 22, 1u << 21, 1, 1, 0, 0, 1u << 21, {}), "more than the 0 bytes");
    expect_corrupt(9, ts::gain_map_parameters(a, 1u << 22, 1u << 21, 1, 1, 0, 0, 1u << 21, {1.0f, 1.0f}),
                   "more than the 8 bytes");
    // A product that fits 64 bits but not the payload, and one whose byte count would wrap.
    expect_corrupt(9, ts::gain_map_parameters(a, 1u << 31, 1u << 31, 1, 1, 0, 0, 2, {}), "more than");
    expect_corrupt(9, ts::gain_map_parameters(a, 1u << 31, 1u << 31, 1, 1, 0, 0, 1, {1.0f}), "more than the 4 bytes");
    EXPECT_TRUE(g.complete());
    DngGainMap short_map = g;
    short_map.gains.pop_back();
    EXPECT_FALSE(short_map.complete());
}

TEST(DngOpcodeParameters, PerRowAndColumnValues) {
    // Rows 1, 4, 7 of [1, 9) with pitch 3: three values; columns 0..9 with pitch 4: 0, 4, 8.
    const AreaSpec a{1, 0, 9, 10, 0, 1, 3, 4};
    for (const std::uint32_t id : {10u, 12u}) {
        const auto v = parse_as<DngRowColumnValues>(id, ts::row_column_parameters(a, {0.1f, 0.2f, 0.3f}));
        EXPECT_EQ(v.values, (std::vector<float>{0.1f, 0.2f, 0.3f}));
        expect_corrupt(id, ts::row_column_parameters(a, {0.1f, 0.2f}), "Count 2");
    }
    for (const std::uint32_t id : {11u, 13u}) {
        EXPECT_EQ(parse_as<DngRowColumnValues>(id, ts::row_column_parameters(a, {1, 2, 3})).values.size(), 3u);
        expect_corrupt(id, ts::row_column_parameters(a, {1, 2, 3, 4}), "Count 4");
    }
    expect_corrupt(10, ts::row_column_parameters(a, {0.1f, std::numeric_limits<float>::infinity(), 0.3f}), "not finite");
}

TEST(DngOpcodeParameters, OpcodesWithoutDecodedParameters) {
    // WarpFisheye and unknown IDs are not decoded (whatever their bytes).
    auto fisheye = parse_dng_opcode_parameters(2, ts::filler(7));
    ASSERT_TRUE(fisheye);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(*fisheye));
    auto unknown = parse_dng_opcode_parameters(99, ts::filler(3));
    ASSERT_TRUE(unknown);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(*unknown));
    EXPECT_FALSE(decodes_dng_opcode_parameters(2));
    EXPECT_FALSE(decodes_dng_opcode_parameters(15));
    EXPECT_TRUE(decodes_dng_opcode_parameters(14));
    EXPECT_EQ(dng_opcode_defined_version(14), (DngVersion{{1, 6, 0, 0}}));
    EXPECT_EQ(dng_opcode_defined_version(9), (DngVersion{{1, 3, 0, 0}}));
    EXPECT_FALSE(dng_opcode_defined_version(15));
}

// ---- Lists: mandatory versus optional, versions -------------------------------------------------------

TEST(DngOpcodeParameters, DecodeListRecordsMalformedParameters) {
    const auto valid = ts::vignette_parameters({0.1, 0, 0, 0, 0});
    const auto payload = ts::opcode_list({
        {3, 0x01030000, 0, valid},             // decoded
        {3, 0x01030000, 1, ts::filler(10)},    // optional, malformed: recorded
        {3, 0x01080000, 0, ts::filler(10)},    // newer than DNG 1.7.1.0: not decoded at all
        {99, 0x01030000, 0, ts::filler(5)},    // unknown: not decoded
        {2, 0x01030000, 0, ts::filler(5)},     // WarpFisheye: not decoded
    });
    auto opcodes = parse_dng_opcode_list(payload, DngOpcodeListId::List3);
    ASSERT_TRUE(opcodes);
    ASSERT_TRUE(decode_dng_opcode_parameters(*opcodes, payload));
    EXPECT_TRUE(std::holds_alternative<DngFixVignetteRadial>((*opcodes)[0].parameters));
    EXPECT_TRUE((*opcodes)[0].parameter_error.empty());
    EXPECT_TRUE(std::holds_alternative<std::monostate>((*opcodes)[1].parameters));
    EXPECT_NE((*opcodes)[1].parameter_error.find("FixVignetteRadial"), std::string::npos);
    EXPECT_FALSE((*opcodes)[2].version_supported());
    EXPECT_TRUE((*opcodes)[2].parameter_error.empty());
    EXPECT_TRUE(std::holds_alternative<std::monostate>((*opcodes)[3].parameters));
    EXPECT_TRUE(std::holds_alternative<std::monostate>((*opcodes)[4].parameters));

    // A malformed mandatory opcode is recorded the same way (and cannot be applied: the
    // assessment makes it an unsupported mandatory operation).
    const auto bad = ts::opcode_list({{3, 0x01030000, 1, valid}, {9, 0x01030000, 0, ts::filler(80)}});
    auto bad_opcodes = parse_dng_opcode_list(bad, DngOpcodeListId::List2);
    ASSERT_TRUE(bad_opcodes);
    ASSERT_TRUE(decode_dng_opcode_parameters(*bad_opcodes, bad));
    const DngOpcode& gain = (*bad_opcodes)[1];
    EXPECT_TRUE(gain.is_mandatory());
    EXPECT_TRUE(std::holds_alternative<std::monostate>(gain.parameters));
    EXPECT_NE(gain.parameter_error.find("raw.dng.opcode_parameters"), std::string::npos) << gain.parameter_error;
    EXPECT_NE(gain.parameter_error.find("(corrupt)"), std::string::npos) << gain.parameter_error;
    const DngOpcodeSupport support = assess_dng_opcode(gain, DngOpcodeImage{DngOpcodeListId::List2, 10, 10});
    EXPECT_FALSE(support.supported);
    EXPECT_NE(support.reason.find("malformed"), std::string::npos) << support.reason;
}

TEST(DngOpcodeParameters, InspectorRecordsMalformedParameters) {
    namespace t = tiff_tag;
    for (const ts::Endian endian : {ts::Endian::Little, ts::Endian::Big}) {
        ts::Document doc = ts::dng::document(endian, ts::Format::Classic);
        // A GainMap whose MapPointsV is zero.
        ts::dng::find(doc.chain[0].sub_ifds[0], t::kOpcodeList2).raw = ts::opcode_list(
            {{9, 0x01030000, 0, ts::gain_map_parameters({0, 0, 10, 10}, 0, 1, 1, 1, 0, 0, 1, {})}});
        const ts::Built built = ts::build(doc);
        MemorySource source(built.span());
        auto summary = inspect_tiff(source);
        ASSERT_TRUE(summary) << summary.error().describe();
        ASSERT_TRUE(summary->primary_raw());
        const auto& opcodes = summary->primary_raw()->opcodes;
        const auto gain = std::find_if(opcodes.begin(), opcodes.end(),
                                       [](const DngOpcode& op) { return op.list == DngOpcodeListId::List2; });
        ASSERT_NE(gain, opcodes.end());
        EXPECT_TRUE(std::holds_alternative<std::monostate>(gain->parameters));
        EXPECT_NE(gain->parameter_error.find("MapPointsV 0"), std::string::npos) << gain->parameter_error;
    }
}

// ---- Support table -----------------------------------------------------------------------------------

DngOpcode decoded(std::uint32_t id, DngOpcodeListId list, const std::vector<std::byte>& parameters,
                  std::uint32_t version = 0x01030000) {
    auto opcodes = parse_dng_opcode_list(ts::opcode_list({{id, version, 0, parameters}}), list);
    EXPECT_TRUE(opcodes);
    const auto payload = ts::opcode_list({{id, version, 0, parameters}});
    EXPECT_TRUE(decode_dng_opcode_parameters(*opcodes, payload));
    return opcodes->front();
}

DngOpcodeImage image_for(DngOpcodeListId list) {
    DngOpcodeImage image;
    image.list = list;
    image.width = 100;
    image.height = 80;
    image.planes = list == DngOpcodeListId::List3 ? 3 : 1;
    image.mosaic = list != DngOpcodeListId::List3;
    if (image.mosaic) {
        image.bayer_phase = DngBayerPhase::Red;
    }
    return image;
}

TEST(DngOpcodeSupport, StageTable) {
    using L = DngOpcodeListId;
    const AreaSpec a{0, 0, 80, 100};
    const std::vector<std::pair<std::uint32_t, std::vector<std::byte>>> opcodes = {
        {1, ts::warp_rectilinear_parameters({{1, 0, 0, 0, 0, 0}})},
        {3, ts::vignette_parameters({0.1, 0, 0, 0, 0})},
        {4, ts::bad_pixels_constant_parameters(0, 0)},
        {5, ts::bad_pixels_list_parameters(0, {{1, 1}})},
        {6, ts::trim_bounds_parameters(0, 0, 80, 100)},
        {7, ts::map_table_parameters(a, {1, 2})},
        {8, ts::map_polynomial_parameters(a, {0, 1})},
        {9, ts::gain_map_parameters(a, 1, 1, 1, 1, 0, 0, 1, {1.0f})},
        {10, ts::row_column_parameters(a, std::vector<float>(80, 0.0f))},
        {11, ts::row_column_parameters(a, std::vector<float>(100, 0.0f))},
        {12, ts::row_column_parameters(a, std::vector<float>(80, 1.0f))},
        {13, ts::row_column_parameters(a, std::vector<float>(100, 1.0f))},
        {14, ts::warp_rectilinear2_parameters({ts::Warp2Set{}})},
    };
    // Expected support per list (ADR 0014).
    const auto expected = [](std::uint32_t id, L list) {
        switch (id) {
            case 1:
            case 14:
                return list == L::List3;
            case 4:
                return list == L::List1;
            case 5:
                return list != L::List3;
            case 6:
                return list != L::List1;
            default:
                return true;
        }
    };
    for (const L list : {L::List1, L::List2, L::List3}) {
        for (const auto& [id, parameters] : opcodes) {
            const DngOpcode opcode = decoded(id, list, parameters, id == 14 ? 0x01060000 : 0x01030000);
            const DngOpcodeSupport support = assess_dng_opcode(opcode, image_for(list));
            EXPECT_EQ(support.supported, expected(id, list))
                << dng_opcode_name(id) << " in list " << static_cast<int>(list) << ": " << support.reason;
            EXPECT_EQ(support.reason.empty(), support.supported);
        }
    }
}

TEST(DngOpcodeSupport, ConditionsBeyondTheTable) {
    using L = DngOpcodeListId;
    // Warps need 1 or one coefficient set per plane.
    const DngOpcode two_sets =
        decoded(1, L::List3, ts::warp_rectilinear_parameters({{1, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0}}));
    EXPECT_FALSE(assess_dng_opcode(two_sets, image_for(L::List3)).supported);
    // Bad pixels need a Bayer CFA whose green sites agree with BayerPhase (red/blue may swap).
    DngOpcodeImage image = image_for(L::List1);
    EXPECT_TRUE(assess_dng_opcode(decoded(4, L::List1, ts::bad_pixels_constant_parameters(0, 3)), image).supported);
    EXPECT_FALSE(assess_dng_opcode(decoded(4, L::List1, ts::bad_pixels_constant_parameters(0, 2)), image).supported);
    image.bayer_phase.reset();
    EXPECT_FALSE(assess_dng_opcode(decoded(4, L::List1, ts::bad_pixels_constant_parameters(0, 0)), image).supported);
    // List 1 on floating-point samples, an unavailable list, a TrimBounds beyond the image, a
    // version newer than DNG 1.7.1.0, WarpFisheye and unknown IDs are not supported.
    DngOpcodeImage float_image = image_for(L::List1);
    float_image.integer_samples = false;
    EXPECT_FALSE(assess_dng_opcode(decoded(3, L::List1, ts::vignette_parameters({0, 0, 0, 0, 0})), float_image).supported);
    DngOpcodeImage unavailable = image_for(L::List2);
    unavailable.list_unavailable = "the decoder linearised the samples";
    const DngOpcodeSupport no_list =
        assess_dng_opcode(decoded(3, L::List2, ts::vignette_parameters({0, 0, 0, 0, 0})), unavailable);
    EXPECT_FALSE(no_list.supported);
    EXPECT_EQ(no_list.reason, "the decoder linearised the samples");
    EXPECT_FALSE(assess_dng_opcode(decoded(6, L::List3, ts::trim_bounds_parameters(0, 0, 81, 100)), image_for(L::List3))
                     .supported);
    EXPECT_FALSE(
        assess_dng_opcode(decoded(3, L::List3, ts::vignette_parameters({0, 0, 0, 0, 0}), 0x01070200), image_for(L::List3))
            .supported);
    EXPECT_FALSE(assess_dng_opcode(decoded(2, L::List3, ts::filler(8)), image_for(L::List3)).supported);
    EXPECT_FALSE(assess_dng_opcode(decoded(42, L::List3, {}), image_for(L::List3)).supported);
}

TEST(DngOpcodeSupport, EmptyAreasStandForTheWholeImage) {
    using L = DngOpcodeListId;
    const AreaSpec empty{0, 0, 0, 0, 0, 3};
    // Value maps and gain maps over an empty area apply to the whole image (the DNG SDK).
    EXPECT_TRUE(assess_dng_opcode(decoded(7, L::List3, ts::map_table_parameters(empty, {1, 2})), image_for(L::List3))
                    .supported);
    EXPECT_TRUE(
        assess_dng_opcode(decoded(8, L::List3, ts::map_polynomial_parameters(empty, {0, 1})), image_for(L::List3))
            .supported);
    EXPECT_TRUE(assess_dng_opcode(decoded(9, L::List3, ts::gain_map_parameters(empty, 1, 1, 1, 1, 0, 0, 1, {2.0f})),
                                  image_for(L::List3))
                    .supported);
    // Per-row and per-column opcodes then hold no value for any row or column: not supported.
    for (const std::uint32_t id : {10u, 11u, 12u, 13u}) {
        const DngOpcodeSupport support =
            assess_dng_opcode(decoded(id, L::List3, ts::row_column_parameters(empty, {})), image_for(L::List3));
        EXPECT_FALSE(support.supported) << id;
        EXPECT_NE(support.reason.find("empty"), std::string::npos) << support.reason;
    }
}

TEST(DngOpcodeSupport, ParametersThatBreakTheParserInvariantsAreNotSupported) {
    using L = DngOpcodeListId;
    const AreaSpec a{0, 0, 80, 100, 0, 3};
    // A gain map whose gains do not fill its grid (as a payload that overflowed the product would
    // have left it): never applied.
    DngOpcode gain = decoded(9, L::List3, ts::gain_map_parameters(a, 2, 2, 1, 1, 0, 0, 1, {1, 1, 1, 1}));
    ASSERT_TRUE(assess_dng_opcode(gain, image_for(L::List3)).supported);
    auto& map = std::get<DngGainMap>(gain.parameters);
    map.points_v = 1u << 22;
    map.points_h = 1u << 21;
    map.map_planes = 1u << 21;
    map.gains.clear();
    const DngOpcodeSupport overflowing = assess_dng_opcode(gain, image_for(L::List3));
    EXPECT_FALSE(overflowing.supported);
    EXPECT_NE(overflowing.reason.find("do not fill"), std::string::npos) << overflowing.reason;
    // Per-row values that do not match the area, an empty table, no coefficients.
    DngOpcode rows = decoded(10, L::List3, ts::row_column_parameters(a, std::vector<float>(80, 0.0f)));
    std::get<DngRowColumnValues>(rows.parameters).values.pop_back();
    EXPECT_FALSE(assess_dng_opcode(rows, image_for(L::List3)).supported);
    DngOpcode table = decoded(7, L::List3, ts::map_table_parameters(a, {1}));
    std::get<DngMapTable>(table.parameters).table.clear();
    EXPECT_FALSE(assess_dng_opcode(table, image_for(L::List3)).supported);
    DngOpcode poly = decoded(8, L::List3, ts::map_polynomial_parameters(a, {0, 1}));
    std::get<DngMapPolynomial>(poly.parameters).coefficients.clear();
    EXPECT_FALSE(assess_dng_opcode(poly, image_for(L::List3)).supported);
}

TEST(DngOpcodeSupport, RadialOpcodesNeedSquarePixels) {
    using L = DngOpcodeListId;
    DngOpcodeImage image = image_for(L::List3);
    image.pixel_aspect = 2.0;
    for (const auto& [id, parameters] : std::vector<std::pair<std::uint32_t, std::vector<std::byte>>>{
             {1, ts::warp_rectilinear_parameters({{0.99, 0, 0, 0, 0, 0}})},
             {3, ts::vignette_parameters({0.1, 0, 0, 0, 0})},
             {14, ts::warp_rectilinear2_parameters({ts::Warp2Set{}})}}) {
        const DngOpcodeSupport support =
            assess_dng_opcode(decoded(id, L::List3, parameters, id == 14 ? 0x01060000 : 0x01030000), image);
        EXPECT_FALSE(support.supported) << id;
        EXPECT_NE(support.reason.find("non-square pixels"), std::string::npos) << support.reason;
    }
    // Opcodes without a radius do not depend on the pixel shape.
    EXPECT_TRUE(assess_dng_opcode(decoded(9, L::List3, ts::gain_map_parameters({0, 0, 80, 100, 0, 3}, 1, 1, 1, 1, 0, 0,
                                                                              1, {2.0f})),
                                  image)
                    .supported);
}

}  // namespace
}  // namespace lucent::raw
