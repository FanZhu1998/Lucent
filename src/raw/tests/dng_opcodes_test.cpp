// Unit tests for DNG opcode-list parsing, DNG version handling and compression classification.

#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/raw/dng_opcodes.hpp>
#include <lucent/raw/dng_tags.hpp>
#include <lucent/raw/dng_version.hpp>
#include <lucent/raw/tiff_inspector.hpp>

#include "tiff_test_writer.hpp"

namespace lucent::raw {
namespace {

using test_support::filler;
using test_support::opcode_list;
using test_support::OpcodeSpec;

TEST(DngVersion, PacksAndOrdersFourTiers) {
    constexpr DngVersion v = DngVersion::from_packed(0x01070100);
    static_assert(v.parts == std::array<std::uint8_t, 4>{1, 7, 1, 0});
    static_assert(v.packed() == 0x01070100);
    static_assert(DngVersion{{1, 3, 0, 0}} < DngVersion{{1, 6, 0, 0}});
    static_assert(DngVersion{{1, 6, 0, 0}} < DngVersion{{1, 6, 0, 1}});
    static_assert(DngVersion{{2, 0, 0, 0}} > kDngSpecificationVersion);
    EXPECT_EQ(v.to_string(), "1.7.1.0");
    EXPECT_EQ(kDngSpecificationVersion.to_string(), "1.7.1.0");
}

TEST(DngOpcodes, NamesMatchDng171Chapter7) {
    // Opcode IDs and names as listed in DNG 1.7.1.0 chapter 7.
    const std::array<const char*, 14> names{
        "WarpRectilinear",  "WarpFisheye",     "FixVignetteRadial", "FixBadPixelsConstant",
        "FixBadPixelsList", "TrimBounds",      "MapTable",          "MapPolynomial",
        "GainMap",          "DeltaPerRow",     "DeltaPerColumn",    "ScalePerRow",
        "ScalePerColumn",   "WarpRectilinear2"};
    for (std::uint32_t id = 1; id <= 14; ++id) {
        EXPECT_TRUE(is_known_dng_opcode(id)) << id;
        EXPECT_EQ(dng_opcode_name(id), names[id - 1]) << id;
    }
    EXPECT_FALSE(is_known_dng_opcode(0));
    EXPECT_FALSE(is_known_dng_opcode(15));
    EXPECT_EQ(dng_opcode_name(0), "Unknown");
    EXPECT_EQ(dng_opcode_name(0xFFFFFFFF), "Unknown");
}

TEST(DngOpcodes, ListTagsMatchDng171) {
    EXPECT_EQ(dng_opcode_list_tag(DngOpcodeListId::List1), 51008);
    EXPECT_EQ(dng_opcode_list_tag(DngOpcodeListId::List2), 51009);
    EXPECT_EQ(dng_opcode_list_tag(DngOpcodeListId::List3), 51022);
}

TEST(DngOpcodes, ParsesHeadersFlagsAndParameterLocations) {
    const auto payload = opcode_list({
        {9, 0x01030000, 0, filler(20)},  // GainMap, mandatory
        {8, 0x01030000, 3, filler(12)},  // MapPolynomial, optional + preview-skippable
        {7, 0x01030000, 2, {}},          // MapTable, preview-skippable only, no parameters
    });
    auto parsed = parse_dng_opcode_list(payload, DngOpcodeListId::List2);
    ASSERT_TRUE(parsed) << parsed.error().describe();
    ASSERT_EQ(parsed->size(), 3u);

    const DngOpcode& gain = (*parsed)[0];
    EXPECT_EQ(gain.list, DngOpcodeListId::List2);
    EXPECT_EQ(gain.index, 0u);
    EXPECT_EQ(gain.id, 9u);
    EXPECT_EQ(gain.name(), "GainMap");
    EXPECT_TRUE(gain.is_known());
    EXPECT_EQ(gain.min_dng_version, (DngVersion{{1, 3, 0, 0}}));
    EXPECT_EQ(gain.flags, 0u);
    EXPECT_TRUE(gain.is_mandatory());
    EXPECT_FALSE(gain.is_optional());
    EXPECT_FALSE(gain.is_preview_skippable());
    EXPECT_EQ(gain.parameter_bytes, 20u);
    EXPECT_EQ(gain.parameter_offset, 4u + 16u);

    const DngOpcode& poly = (*parsed)[1];
    EXPECT_EQ(poly.index, 1u);
    EXPECT_TRUE(poly.is_optional());
    EXPECT_TRUE(poly.is_preview_skippable());
    EXPECT_FALSE(poly.is_mandatory());
    EXPECT_EQ(poly.parameter_offset, 4u + 16u + 20u + 16u);

    const DngOpcode& table = (*parsed)[2];
    EXPECT_TRUE(table.is_mandatory());
    EXPECT_TRUE(table.is_preview_skippable());
    EXPECT_EQ(table.parameter_bytes, 0u);
    EXPECT_EQ(table.parameter_offset, payload.size());
}

TEST(DngOpcodes, EmptyListIsValid) {
    const auto payload = opcode_list({});
    auto parsed = parse_dng_opcode_list(payload, DngOpcodeListId::List1);
    ASSERT_TRUE(parsed) << parsed.error().describe();
    EXPECT_TRUE(parsed->empty());
}

TEST(DngOpcodes, UnknownOpcodeKeepsItsRequirement) {
    const auto payload = opcode_list({{99, 0x01080000, 0, filler(4)}, {100, 0x01080000, 1, {}}});
    auto parsed = parse_dng_opcode_list(payload, DngOpcodeListId::List3);
    ASSERT_TRUE(parsed);
    EXPECT_FALSE((*parsed)[0].is_known());
    EXPECT_EQ((*parsed)[0].name(), "Unknown");
    EXPECT_TRUE((*parsed)[0].is_mandatory());  // a reader that cannot apply it must not claim fidelity
    EXPECT_EQ((*parsed)[0].min_dng_version, (DngVersion{{1, 8, 0, 0}}));
    EXPECT_GT((*parsed)[0].min_dng_version, kDngSpecificationVersion);
    EXPECT_TRUE((*parsed)[1].is_optional());
}

TEST(DngOpcodes, ReservedFlagBitsAreReported) {
    const auto payload = opcode_list({{3, 0x01030000, 0x80000001u, {}}});
    auto parsed = parse_dng_opcode_list(payload, DngOpcodeListId::List3);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE((*parsed)[0].has_reserved_flags());
    EXPECT_TRUE((*parsed)[0].is_optional());
}

TEST(DngOpcodes, WarpRectilinear2SkipRule) {
    const auto parse = [](const std::vector<OpcodeSpec>& ops) {
        auto parsed = parse_dng_opcode_list(opcode_list(ops), DngOpcodeListId::List3);
        EXPECT_TRUE(parsed);
        return parsed.value_or(std::vector<DngOpcode>{});
    };
    // Optional WarpRectilinear2 immediately followed by WarpRectilinear / WarpFisheye.
    auto ops = parse({{14, 0x01060000, 1, {}}, {1, 0x01030000, 0, {}}});
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_FALSE(ops[0].follows_optional_warp_rectilinear2);
    EXPECT_TRUE(ops[1].follows_optional_warp_rectilinear2);

    ops = parse({{14, 0x01060000, 1, {}}, {2, 0x01030000, 0, {}}});
    EXPECT_TRUE(ops[1].follows_optional_warp_rectilinear2);

    // Mandatory WarpRectilinear2: the rule does not apply.
    ops = parse({{14, 0x01060000, 0, {}}, {1, 0x01030000, 0, {}}});
    EXPECT_FALSE(ops[1].follows_optional_warp_rectilinear2);

    // Not immediately following.
    ops = parse({{14, 0x01060000, 1, {}}, {3, 0x01030000, 0, {}}, {1, 0x01030000, 0, {}}});
    EXPECT_FALSE(ops[2].follows_optional_warp_rectilinear2);

    // Other opcodes after an optional WarpRectilinear2 are unaffected.
    ops = parse({{14, 0x01060000, 1, {}}, {9, 0x01030000, 0, {}}});
    EXPECT_FALSE(ops[1].follows_optional_warp_rectilinear2);
}

TEST(DngOpcodes, PayloadShorterThanCountIsCorrupt) {
    for (std::size_t size : {0u, 1u, 3u}) {
        const std::vector<std::byte> payload(size);
        auto parsed = parse_dng_opcode_list(payload, DngOpcodeListId::List1);
        ASSERT_FALSE(parsed) << size;
        EXPECT_EQ(parsed.error().code, ErrorCode::Corrupt);
        EXPECT_EQ(parsed.error().where, "raw.dng.opcode_list");
    }
}

TEST(DngOpcodes, CountExceedingPayloadIsCorrupt) {
    for (std::uint32_t count : {2u, 1000u, 0xFFFFFFFFu}) {
        const auto payload = opcode_list({{4, 0x01030000, 0, filler(8)}}, count);
        auto parsed = parse_dng_opcode_list(payload, DngOpcodeListId::List1);
        ASSERT_FALSE(parsed) << count;
        EXPECT_EQ(parsed.error().code, ErrorCode::Corrupt);
        EXPECT_EQ(parsed.error().where, "raw.dng.opcode_list");
    }
}

TEST(DngOpcodes, CountWithinHeaderCapacityButParametersConsumePayloadIsCorrupt) {
    // Two 16-byte headers fit, but the first opcode's parameters leave no room for the second.
    OpcodeSpec first{9, 0x01030000, 0, filler(16)};
    const auto payload = opcode_list({first}, 2);
    ASSERT_EQ(payload.size(), 4u + 16u + 16u);
    auto parsed = parse_dng_opcode_list(payload, DngOpcodeListId::List2);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, ErrorCode::Corrupt);
}

TEST(DngOpcodes, ParameterAreaBeyondPayloadIsCorrupt) {
    for (std::uint32_t declared : {9u, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
        OpcodeSpec op{5, 0x01030000, 0, filler(8)};
        op.parameter_bytes_override = declared;
        auto parsed = parse_dng_opcode_list(opcode_list({op}), DngOpcodeListId::List1);
        ASSERT_FALSE(parsed) << declared;
        EXPECT_EQ(parsed.error().code, ErrorCode::Corrupt);
    }
}

TEST(DngOpcodes, TrailingBytesAreCorrupt) {
    auto payload = opcode_list({{6, 0x01030000, 0, filler(16)}});
    payload.push_back(std::byte{0});
    auto parsed = parse_dng_opcode_list(payload, DngOpcodeListId::List3);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, ErrorCode::Corrupt);
}

TEST(DngOpcodes, OpcodeCountLimit) {
    const auto payload =
        opcode_list({{4, 0x01030000, 0, {}}, {4, 0x01030000, 0, {}}, {4, 0x01030000, 0, {}}});
    auto parsed = parse_dng_opcode_list(payload, DngOpcodeListId::List1, 2);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, ErrorCode::LimitExceeded);
    EXPECT_EQ(parsed.error().where, "raw.dng.opcode_list");
    EXPECT_TRUE(parse_dng_opcode_list(payload, DngOpcodeListId::List1, 3));
    // A count that the payload cannot hold is corrupt, whatever the limit.
    const auto lying = opcode_list({{4, 0x01030000, 0, {}}}, 1000);
    auto corrupt = parse_dng_opcode_list(lying, DngOpcodeListId::List1, 2);
    ASSERT_FALSE(corrupt);
    EXPECT_EQ(corrupt.error().code, ErrorCode::Corrupt);
}

TEST(DngOpcodes, InvalidListNumberIsRejected) {
    const auto payload = opcode_list({});
    for (std::uint8_t list : {std::uint8_t{0}, std::uint8_t{4}}) {
        auto parsed = parse_dng_opcode_list(payload, static_cast<DngOpcodeListId>(list));
        ASSERT_FALSE(parsed);
        EXPECT_EQ(parsed.error().code, ErrorCode::InvalidArgument);
    }
}

TEST(Compression, ClassifiesDng171Codes) {
    const std::vector<std::uint16_t> raw16{16};
    const std::vector<std::uint16_t> rgb8{8, 8, 8};
    const std::vector<std::uint16_t> gray8{8};
    using tiff_photometric::kBlackIsZero;
    using tiff_photometric::kCfa;
    using tiff_photometric::kLinearRaw;
    using tiff_photometric::kRgb;
    using tiff_photometric::kYCbCr;

    auto info = classify_compression(1, kCfa, raw16, std::nullopt);
    EXPECT_EQ(info.name, "Uncompressed");
    EXPECT_EQ(info.lossiness, Lossiness::Lossless);
    EXPECT_TRUE(info.allowed_in_dng);

    info = classify_compression(7, kCfa, raw16, std::nullopt);
    EXPECT_EQ(info.name, "Lossless JPEG");
    EXPECT_FALSE(info.is_lossy());

    // Baseline DCT only for 8-bit YCbCr and 8-bit single-sample BlackIsZero data.
    info = classify_compression(7, kYCbCr, rgb8, std::nullopt);
    EXPECT_EQ(info.name, "Baseline JPEG");
    EXPECT_TRUE(info.is_lossy());
    info = classify_compression(7, kBlackIsZero, gray8, std::nullopt);
    EXPECT_TRUE(info.is_lossy());
    info = classify_compression(7, kLinearRaw, rgb8, std::nullopt);  // 8-bit LinearRaw stays lossless JPEG
    EXPECT_EQ(info.name, "Lossless JPEG");
    info = classify_compression(7, std::nullopt, gray8, std::nullopt);
    EXPECT_FALSE(info.is_lossy());

    info = classify_compression(8, kCfa, raw16, std::nullopt);
    EXPECT_EQ(info.name, "Deflate");
    EXPECT_EQ(info.lossiness, Lossiness::Lossless);

    info = classify_compression(34892, kLinearRaw, rgb8, std::nullopt);
    EXPECT_EQ(info.name, "Lossy JPEG");
    EXPECT_TRUE(info.is_lossy());

    info = classify_compression(52546, kCfa, raw16, std::nullopt);
    EXPECT_EQ(info.name, "JPEG XL");
    EXPECT_EQ(info.lossiness, Lossiness::Unknown);
    EXPECT_EQ(classify_compression(52546, kCfa, raw16, 0.0f).lossiness, Lossiness::Lossless);
    EXPECT_EQ(classify_compression(52546, kCfa, raw16, 0.5f).lossiness, Lossiness::Lossy);
    EXPECT_EQ(classify_compression(52546, kCfa, raw16, -1.0f).lossiness, Lossiness::Unknown);

    info = classify_compression(5, kRgb, rgb8, std::nullopt);
    EXPECT_EQ(info.name, "LZW");
    EXPECT_FALSE(info.allowed_in_dng);

    info = classify_compression(34713, kCfa, raw16, std::nullopt);  // Nikon NEF compression
    EXPECT_EQ(info.name, "Unrecognized");
    EXPECT_FALSE(info.allowed_in_dng);
    EXPECT_EQ(info.lossiness, Lossiness::Unknown);
    EXPECT_EQ(info.code, 34713);
}

TEST(Compression, PhotometricNames) {
    EXPECT_EQ(photometric_name(32803), "CFA");
    EXPECT_EQ(photometric_name(34892), "LinearRaw");
    EXPECT_EQ(photometric_name(6), "YCbCr");
    EXPECT_EQ(photometric_name(9999), "Unrecognized");
}

}  // namespace
}  // namespace lucent::raw
