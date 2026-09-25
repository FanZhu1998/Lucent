// Unit tests for inspect_tiff() on well-formed synthetic TIFF, BigTIFF and DNG streams.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/raw/dng_tags.hpp>
#include <lucent/raw/tiff_inspector.hpp>

#include "tiff_test_writer.hpp"

namespace lucent::raw {
namespace {

namespace ts = test_support;
namespace t = tiff_tag;
using ts::Built;
using ts::Document;
using ts::Endian;
using ts::Format;
using ts::long_entry;
using ts::short_entry;

Expected<TiffSummary> inspect(const Built& built, const TiffInspectLimits& limits = {}) {
    MemorySource source(built.span());
    return inspect_tiff(source, limits);
}

TiffSummary inspect_ok(const Built& built, const TiffInspectLimits& limits = {}) {
    auto result = inspect(built, limits);
    EXPECT_TRUE(result) << result.error().describe();
    return result ? std::move(*result) : TiffSummary{};
}

// Removes everything that legitimately depends on byte order or variant: offsets and sizes.
TiffSummary normalized(TiffSummary s) {
    s.byte_order = TiffByteOrder::LittleEndian;
    s.variant = TiffVariant::Classic;
    s.source_size = 0;
    const auto clear = [](std::optional<TiffValueLocation>& location) {
        if (location) {
            location->offset = 0;
        }
    };
    for (TiffIfdSummary& ifd : s.ifds) {
        ifd.offset = 0;
        clear(ifd.linearization_table);
        clear(ifd.profile_gain_table_map);
        for (auto& list : ifd.opcode_lists) {
            clear(list);
        }
    }
    if (s.dng) {
        clear(s.dng->profile_gain_table_map2);
        for (auto& data : s.dng->color.illuminant_data) {
            clear(data);
        }
    }
    return s;
}

std::vector<std::byte> slice(const Built& built, const TiffValueLocation& location) {
    const auto begin = built.bytes.begin() + static_cast<std::ptrdiff_t>(location.offset);
    return {begin, begin + static_cast<std::ptrdiff_t>(location.byte_count)};
}

// ---- Byte source ---------------------------------------------------------------------------

TEST(MemorySource, ReadsWithinAndAtTheEndOfTheBuffer) {
    const std::vector<std::byte> data = ts::filler(10);
    MemorySource source(data);
    EXPECT_EQ(source.size(), 10u);

    std::array<std::byte, 4> out{};
    auto read = source.read_at(2, out);
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, 4u);
    EXPECT_EQ(out[0], data[2]);
    EXPECT_EQ(out[3], data[5]);

    read = source.read_at(8, out);  // partial read at the end
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, 2u);
    EXPECT_EQ(out[1], data[9]);

    read = source.read_at(10, out);
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, 0u);
    read = source.read_at(0xFFFFFFFFFFFFFFFFull, out);
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, 0u);

    read = source.read_at(3, std::span<std::byte>{});
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, 0u);
}

// ---- Reference DNG -------------------------------------------------------------------------

TEST(TiffInspector, ReferenceDngLittleEndian) {
    const Built built = ts::build(ts::dng::document(Endian::Little, Format::Classic));
    const TiffSummary s = inspect_ok(built);

    EXPECT_EQ(s.byte_order, TiffByteOrder::LittleEndian);
    EXPECT_EQ(s.variant, TiffVariant::Classic);
    EXPECT_EQ(s.source_size, built.bytes.size());
    EXPECT_TRUE(s.warnings.empty());
    EXPECT_EQ(s.suppressed_warnings, 0u);

    ASSERT_EQ(s.ifds.size(), 4u);
    EXPECT_EQ(s.ifds[0].path, "ifd0");
    EXPECT_EQ(s.ifds[1].path, "ifd0.subifd0");
    EXPECT_EQ(s.ifds[2].path, "ifd0.subifd1");
    EXPECT_EQ(s.ifds[3].path, "ifd0.exif");
    EXPECT_EQ(s.ifds[0].kind, TiffIfdKind::Main);
    EXPECT_EQ(s.ifds[1].kind, TiffIfdKind::SubIfd);
    EXPECT_EQ(s.ifds[3].kind, TiffIfdKind::Exif);
    EXPECT_EQ(s.ifds[0].parent, std::nullopt);
    EXPECT_EQ(s.ifds[1].parent, 0u);
    EXPECT_EQ(s.ifds[3].parent, 0u);
    EXPECT_EQ(s.ifds[1].depth, 1u);
    EXPECT_EQ(s.ifds[0].offset, built.ifd("ifd0"));
    EXPECT_EQ(s.ifds[1].offset, built.ifd("ifd0.subifd0"));
    EXPECT_EQ(s.ifds[3].offset, built.ifd("ifd0.exif"));
    EXPECT_EQ(s.ifds[0].entry_count, 24u);  // 22 listed entries + SubIFDs + ExifIFD
    EXPECT_EQ(s.ifds[3].entry_count, 2u);

    EXPECT_EQ(s.make, "Lucent");  // trailing padding removed
    EXPECT_EQ(s.model, "Synthetic One");

    ASSERT_TRUE(s.is_dng());
    const DngInfo& dng = *s.dng;
    EXPECT_EQ(dng.version, (DngVersion{{1, 6, 0, 0}}));
    EXPECT_EQ(dng.backward_version, (DngVersion{{1, 4, 0, 0}}));
    EXPECT_TRUE(dng.backward_version_present);
    EXPECT_EQ(dng.unique_camera_model, "Lucent Synthetic One");
    EXPECT_EQ(dng.color_planes, 3u);
    EXPECT_FALSE(dng.profile_gain_table_map2);

    // Colour tags: SRATIONAL n/10000 decoded exactly as stored.
    const DngColorInfo& c = dng.color;
    ASSERT_TRUE(c.color_matrix[0]);
    EXPECT_EQ(c.color_matrix[0]->rows, 3u);
    EXPECT_EQ(c.color_matrix[0]->cols, 3u);
    EXPECT_DOUBLE_EQ(c.color_matrix[0]->at(0, 0), 0.8);
    EXPECT_DOUBLE_EQ(c.color_matrix[0]->at(0, 1), -0.2);
    EXPECT_DOUBLE_EQ(c.color_matrix[0]->at(1, 1), 1.2);
    EXPECT_DOUBLE_EQ(c.color_matrix[0]->at(2, 2), 0.55);
    ASSERT_TRUE(c.color_matrix[1]);
    EXPECT_DOUBLE_EQ(c.color_matrix[1]->at(2, 2), 0.65);
    EXPECT_FALSE(c.color_matrix[2]);
    ASSERT_TRUE(c.camera_calibration[0]);
    EXPECT_EQ(c.camera_calibration[0]->values, (std::vector<double>{1, 0, 0, 0, 1, 0, 0, 0, 1}));
    EXPECT_FALSE(c.camera_calibration[1]);
    ASSERT_TRUE(c.forward_matrix[0]);
    EXPECT_EQ(c.forward_matrix[0]->rows, 3u);
    EXPECT_EQ(c.forward_matrix[0]->cols, 3u);
    EXPECT_DOUBLE_EQ(c.forward_matrix[0]->at(1, 1), 0.7);
    EXPECT_EQ(c.analog_balance, (std::vector<double>{1, 1, 1}));
    ASSERT_EQ(c.as_shot_neutral.size(), 3u);
    EXPECT_DOUBLE_EQ(c.as_shot_neutral[0], 0.5);
    EXPECT_DOUBLE_EQ(c.as_shot_neutral[2], 2.0 / 3.0);
    EXPECT_FALSE(c.as_shot_white_xy);
    EXPECT_EQ(c.baseline_exposure, -0.5);
    EXPECT_EQ(c.calibration_illuminant[0], 17);
    EXPECT_EQ(c.calibration_illuminant[1], 21);
    EXPECT_FALSE(c.calibration_illuminant[2]);

    // Raw IFD selection.
    EXPECT_EQ(s.raw_ifd_candidates, (std::vector<std::uint32_t>{1}));
    ASSERT_EQ(s.primary_raw_ifd, 1u);
    const TiffIfdSummary& raw = *s.primary_raw();
    EXPECT_EQ(raw.new_subfile_type, 0u);
    EXPECT_EQ(raw.width, ts::dng::kRawWidth);
    EXPECT_EQ(raw.height, ts::dng::kRawHeight);
    EXPECT_EQ(raw.bits_per_sample, (std::vector<std::uint16_t>{16}));
    EXPECT_EQ(raw.compression, 7);
    EXPECT_EQ(raw.compression_info.name, "Lossless JPEG");
    EXPECT_EQ(raw.compression_info.lossiness, Lossiness::Lossless);
    EXPECT_EQ(raw.photometric, tiff_photometric::kCfa);
    EXPECT_EQ(raw.samples_per_pixel, 1);
    EXPECT_EQ(raw.planar_configuration, 1);
    EXPECT_EQ(raw.cfa_repeat_dim, (std::array<std::uint16_t, 2>{2, 2}));
    EXPECT_EQ(raw.cfa_pattern, (std::vector<std::uint8_t>{0, 1, 1, 2}));
    EXPECT_EQ(raw.cfa_plane_color, (std::vector<std::uint8_t>{0, 1, 2}));
    EXPECT_EQ(raw.cfa_layout, 1);
    EXPECT_EQ(raw.active_area, (DngRect{12, 16, ts::dng::kRawHeight, ts::dng::kRawWidth}));
    EXPECT_EQ(raw.default_crop_origin, (std::array<double, 2>{8.0, 8.0}));
    EXPECT_EQ(raw.default_crop_size, (std::array<double, 2>{4000.0, 2992.0}));
    EXPECT_EQ(raw.black_level_repeat_dim, (std::array<std::uint16_t, 2>{2, 2}));
    EXPECT_EQ(raw.black_level, (std::vector<double>{256.0, 257.0, 258.0, 256.5}));
    EXPECT_EQ(raw.white_level, (std::vector<std::uint32_t>{16383}));
    EXPECT_EQ(raw.noise_profile, (std::vector<double>{2.0e-5, 4.5e-7}));

    ASSERT_TRUE(raw.linearization_table);
    EXPECT_EQ(raw.linearization_table->type, static_cast<std::uint16_t>(TiffFieldType::Short));
    EXPECT_EQ(raw.linearization_table->count, 16u);
    EXPECT_EQ(raw.linearization_table->byte_count, 32u);
    EXPECT_EQ(raw.linearization_table->offset, built.value("ifd0.subifd0", t::kLinearizationTable));
    for (std::uint64_t i = 0; i < 16; ++i) {
        EXPECT_EQ(built.get(raw.linearization_table->offset + 2 * i, 2), i);
    }
    ASSERT_TRUE(raw.profile_gain_table_map);
    EXPECT_EQ(raw.profile_gain_table_map->byte_count, 24u);
    EXPECT_EQ(slice(built, *raw.profile_gain_table_map), ts::filler(24, 0x11));

    // Opcode lists, in list order and processing order.
    EXPECT_EQ(slice(built, *raw.opcode_lists[0]), ts::dng::list1());
    EXPECT_EQ(slice(built, *raw.opcode_lists[1]), ts::dng::list2());
    EXPECT_EQ(slice(built, *raw.opcode_lists[2]), ts::dng::list3());
    ASSERT_EQ(raw.opcodes.size(), 6u);
    const auto expect_opcode = [&](std::size_t i, DngOpcodeListId list, std::uint32_t index,
                                   std::string_view name, bool mandatory, bool preview_skippable) {
        SCOPED_TRACE(i);
        const DngOpcode& op = raw.opcodes[i];
        EXPECT_EQ(op.list, list);
        EXPECT_EQ(op.index, index);
        EXPECT_EQ(op.name(), name);
        EXPECT_EQ(op.is_mandatory(), mandatory);
        EXPECT_EQ(op.is_optional(), !mandatory);
        EXPECT_EQ(op.is_preview_skippable(), preview_skippable);
    };
    expect_opcode(0, DngOpcodeListId::List1, 0, "FixBadPixelsConstant", true, false);
    expect_opcode(1, DngOpcodeListId::List2, 0, "GainMap", true, false);
    expect_opcode(2, DngOpcodeListId::List2, 1, "MapPolynomial", false, true);
    expect_opcode(3, DngOpcodeListId::List3, 0, "WarpRectilinear2", false, false);
    expect_opcode(4, DngOpcodeListId::List3, 1, "WarpRectilinear", true, false);
    expect_opcode(5, DngOpcodeListId::List3, 2, "FixVignetteRadial", true, true);
    EXPECT_EQ(raw.opcodes[3].min_dng_version, (DngVersion{{1, 6, 0, 0}}));
    EXPECT_EQ(raw.opcodes[1].parameter_bytes, 76u);
    EXPECT_TRUE(raw.opcodes[4].follows_optional_warp_rectilinear2);
    EXPECT_TRUE(raw.has_mandatory_opcodes());
    // Parameters are addressable in the file: list offset + parameter offset.
    const DngOpcode& gain_map = raw.opcodes[1];
    EXPECT_EQ(built.bytes.at(raw.opcode_lists[1]->offset + gain_map.parameter_offset), ts::filler(76)[0]);

    const TiffIfdSummary& preview = s.ifds[2];
    EXPECT_EQ(preview.new_subfile_type, dng_subfile_type::kPreview);
    EXPECT_EQ(preview.compression_info.name, "Baseline JPEG");
    EXPECT_TRUE(preview.compression_info.is_lossy());
    EXPECT_TRUE(preview.opcodes.empty());
    EXPECT_FALSE(preview.has_mandatory_opcodes());

    const TiffIfdSummary& exif = s.ifds[3];
    EXPECT_FALSE(exif.width);
    EXPECT_FALSE(exif.compression);
}

TEST(TiffInspector, ByteOrderAndVariantDoNotChangeTheSummary) {
    const TiffSummary reference =
        normalized(inspect_ok(ts::build(ts::dng::document(Endian::Little, Format::Classic))));
    ASSERT_EQ(reference.ifds.size(), 4u);
    for (const Endian endian : {Endian::Little, Endian::Big}) {
        for (const Format format : {Format::Classic, Format::BigTiff}) {
            SCOPED_TRACE(std::string(endian == Endian::Little ? "II" : "MM") +
                         (format == Format::Classic ? " classic" : " BigTIFF"));
            const Built built = ts::build(ts::dng::document(endian, format));
            const TiffSummary s = inspect_ok(built);
            EXPECT_EQ(s.byte_order,
                      endian == Endian::Little ? TiffByteOrder::LittleEndian : TiffByteOrder::BigEndian);
            EXPECT_EQ(s.variant, format == Format::Classic ? TiffVariant::Classic : TiffVariant::BigTiff);
            EXPECT_TRUE(normalized(s) == reference);
            // Opcode lists are big-endian in every file: the stored bytes are identical.
            ASSERT_TRUE(s.primary_raw());
            EXPECT_EQ(slice(built, *s.primary_raw()->opcode_lists[1]), ts::dng::list2());
        }
    }
}

// ---- Raw IFD selection -----------------------------------------------------------------------

TEST(TiffInspector, Ifd0CanBeTheRawIfd) {
    Document doc = ts::dng::document();
    ts::Ifd raw = ts::dng::raw_ifd();
    ts::dng::erase(raw, t::kNewSubFileType);  // absent means 0 (main image)
    ts::Ifd& ifd0 = doc.chain[0];
    for (const ts::Entry& e : ifd0.entries) {
        const bool image_structure = e.tag == t::kNewSubFileType || e.tag == t::kImageWidth ||
                                     e.tag == t::kImageLength || e.tag == t::kBitsPerSample ||
                                     e.tag == t::kCompression || e.tag == t::kPhotometricInterpretation ||
                                     e.tag == t::kSamplesPerPixel;
        if (!image_structure) {
            raw.entries.push_back(e);
        }
    }
    ifd0 = raw;
    const TiffSummary s = inspect_ok(ts::build(doc));
    ASSERT_EQ(s.ifds.size(), 1u);
    EXPECT_EQ(s.primary_raw_ifd, 0u);
    ASSERT_TRUE(s.dng);
    EXPECT_EQ(s.dng->color_planes, 3u);
    EXPECT_EQ(s.primary_raw()->opcodes.size(), 6u);
}

TEST(TiffInspector, SeveralRawCandidatesSelectNone) {
    Document doc = ts::dng::document();
    doc.chain[0].sub_ifds.push_back(ts::dng::raw_ifd());
    const TiffSummary s = inspect_ok(ts::build(doc));
    EXPECT_EQ(s.raw_ifd_candidates, (std::vector<std::uint32_t>{1, 3}));
    EXPECT_FALSE(s.primary_raw_ifd);
    EXPECT_EQ(s.primary_raw(), nullptr);
    ASSERT_TRUE(s.dng);
    EXPECT_FALSE(s.dng->color_planes);
    ASSERT_EQ(s.warnings.size(), 1u);
    EXPECT_EQ(s.warnings[0].where, "raw.tiff.ifd_tree");
}

TEST(TiffInspector, EnhancedAndPreviewIfdsAreNotRawCandidates) {
    Document doc = ts::dng::document();
    ts::Ifd enhanced = ts::dng::raw_ifd();
    ts::dng::find(enhanced, t::kNewSubFileType).integers = {dng_subfile_type::kEnhancedImage};
    ts::dng::find(enhanced, t::kPhotometricInterpretation).integers = {tiff_photometric::kLinearRaw};
    doc.chain[0].sub_ifds.push_back(enhanced);
    const TiffSummary s = inspect_ok(ts::build(doc));
    EXPECT_EQ(s.raw_ifd_candidates, (std::vector<std::uint32_t>{1}));
    EXPECT_EQ(s.primary_raw_ifd, 1u);
}

TEST(TiffInspector, LinearRawColourPlanesComeFromSamplesPerPixel) {
    Document doc = ts::dng::document();
    ts::Ifd& raw = doc.chain[0].sub_ifds[0];
    ts::dng::find(raw, t::kPhotometricInterpretation).integers = {tiff_photometric::kLinearRaw};
    ts::dng::find(raw, t::kSamplesPerPixel).integers = {3};
    ts::dng::find(raw, t::kBitsPerSample).integers = {16, 16, 16};
    ts::dng::find(raw, t::kWhiteLevel).integers = {16383, 16383, 16383};
    ts::dng::find(raw, t::kBlackLevel).rationals.assign(12, {64, 1});
    const TiffSummary s = inspect_ok(ts::build(doc));
    ASSERT_TRUE(s.dng);
    EXPECT_EQ(s.dng->color_planes, 3u);
    EXPECT_EQ(s.primary_raw()->black_level.size(), 12u);
}

TEST(TiffInspector, MonochromeAndFourColourProfilesMatchTheirPlaneCount) {
    {
        Document doc = ts::dng::document();
        ts::Ifd& ifd0 = doc.chain[0];
        ts::Ifd& raw = ifd0.sub_ifds[0];
        ts::dng::find(raw, t::kPhotometricInterpretation).integers = {tiff_photometric::kLinearRaw};
        ts::dng::erase(raw, t::kCfaRepeatPatternDim);
        ts::dng::erase(raw, t::kCfaPattern);
        ts::dng::erase(raw, t::kCfaPlaneColor);
        ts::dng::find(raw, t::kBlackLevelRepeatDim).integers = {1, 1};
        ts::dng::find(raw, t::kBlackLevel).rationals = {{64, 1}};
        ts::dng::find(ifd0, t::kColorMatrix1).rationals = {{3, 10}, {6, 10}, {1, 10}};
        ts::dng::erase(ifd0, t::kColorMatrix2);
        ts::dng::find(ifd0, t::kCameraCalibration1).rationals = {{1, 1}};
        ts::dng::find(ifd0, t::kForwardMatrix1).rationals = {{1, 3}, {1, 3}, {1, 3}};
        ts::dng::find(ifd0, t::kAnalogBalance).rationals = {{1, 1}};
        ts::dng::find(ifd0, t::kAsShotNeutral).rationals = {{1, 1}};
        const TiffSummary s = inspect_ok(ts::build(doc));
        ASSERT_TRUE(s.dng);
        EXPECT_EQ(s.dng->color_planes, 1u);
        EXPECT_EQ(s.dng->color.color_matrix[0]->rows, 1u);
        EXPECT_EQ(s.dng->color.forward_matrix[0]->cols, 1u);
    }
    {
        Document doc = ts::dng::document();
        ts::Ifd& ifd0 = doc.chain[0];
        ts::Ifd& raw = ifd0.sub_ifds[0];
        ts::dng::find(raw, t::kCfaPlaneColor).integers = {0, 1, 2, 6};  // R, G, B, white
        ts::dng::find(raw, t::kCfaPattern).integers = {0, 1, 3, 2};
        ts::dng::find(ifd0, t::kColorMatrix1).rationals.assign(12, {1, 4});
        ts::dng::find(ifd0, t::kColorMatrix2).rationals.assign(12, {1, 5});
        ts::dng::find(ifd0, t::kCameraCalibration1).rationals.assign(16, {0, 1});
        ts::dng::find(ifd0, t::kForwardMatrix1).rationals.assign(12, {1, 4});
        ts::dng::find(ifd0, t::kAnalogBalance).rationals.assign(4, {1, 1});
        ts::dng::find(ifd0, t::kAsShotNeutral).rationals.assign(4, {1, 2});
        const TiffSummary s = inspect_ok(ts::build(doc));
        ASSERT_TRUE(s.dng);
        EXPECT_EQ(s.dng->color_planes, 4u);
        EXPECT_EQ(s.dng->color.color_matrix[0]->rows, 4u);
        EXPECT_EQ(s.dng->color.camera_calibration[0]->rows, 4u);
        EXPECT_EQ(s.dng->color.forward_matrix[0]->cols, 4u);
    }
}

// ---- DNG-level tags --------------------------------------------------------------------------

TEST(TiffInspector, NonDngTiffHasNoDngInfo) {
    Document doc;
    ts::Ifd ifd0;
    ifd0.entries = {
        long_entry(t::kImageWidth, {64}),
        long_entry(t::kImageLength, {48}),
        short_entry(t::kBitsPerSample, {8, 8, 8}),
        short_entry(t::kCompression, {5}),
        short_entry(t::kPhotometricInterpretation, {2}),
        ts::ascii_entry(t::kMake, "Scanner Co"),
        short_entry(t::kSamplesPerPixel, {3}),
    };
    doc.chain = {ifd0};
    const TiffSummary s = inspect_ok(ts::build(doc));
    EXPECT_FALSE(s.is_dng());
    EXPECT_EQ(s.make, "Scanner Co");
    EXPECT_FALSE(s.model);
    EXPECT_TRUE(s.raw_ifd_candidates.empty());
    EXPECT_FALSE(s.primary_raw_ifd);
    EXPECT_EQ(s.ifds[0].compression_info.name, "LZW");
    EXPECT_FALSE(s.ifds[0].compression_info.allowed_in_dng);
}

TEST(TiffInspector, BackwardVersionDefaultsWhenAbsent) {
    Document doc = ts::dng::document();
    ts::dng::erase(doc.chain[0], t::kDngBackwardVersion);
    ts::dng::find(doc.chain[0], t::kDngVersion).integers = {1, 7, 1, 0};
    const TiffSummary s = inspect_ok(ts::build(doc));
    ASSERT_TRUE(s.dng);
    EXPECT_EQ(s.dng->version, (DngVersion{{1, 7, 1, 0}}));
    EXPECT_EQ(s.dng->backward_version, (DngVersion{{1, 7, 0, 0}}));
    EXPECT_FALSE(s.dng->backward_version_present);
}

TEST(TiffInspector, ThirdIlluminantAndDng17Tags) {
    Document doc = ts::dng::document();
    ts::Ifd& ifd0 = doc.chain[0];
    ifd0.entries.push_back(short_entry(t::kCalibrationIlluminant3, {255}));
    ifd0.entries.push_back(ts::srational_entry(
        t::kColorMatrix3, {{1, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 1}}));
    ifd0.entries.push_back(ts::srational_entry(
        t::kCameraCalibration3, {{1, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 1}}));
    ifd0.entries.push_back(ts::srational_entry(
        t::kForwardMatrix3, {{1, 2}, {1, 4}, {1, 4}, {1, 4}, {1, 2}, {1, 4}, {0, 1}, {1, 8}, {7, 8}}));
    ifd0.entries.push_back(ts::undefined_entry(t::kIlluminantData3, ts::filler(12, 0x33)));
    ifd0.entries.push_back(ts::undefined_entry(t::kProfileGainTableMap2, ts::filler(40, 0x44)));
    ifd0.entries.push_back(ts::rational_entry(t::kAsShotWhiteXY, {{3127, 10000}, {3290, 10000}}));
    const Built built = ts::build(doc);
    const TiffSummary s = inspect_ok(built);
    ASSERT_TRUE(s.dng);
    const DngColorInfo& c = s.dng->color;
    EXPECT_EQ(c.calibration_illuminant[2], 255);
    ASSERT_TRUE(c.color_matrix[2]);
    ASSERT_TRUE(c.camera_calibration[2]);
    ASSERT_TRUE(c.forward_matrix[2]);
    EXPECT_DOUBLE_EQ(c.forward_matrix[2]->at(2, 2), 0.875);
    ASSERT_TRUE(c.illuminant_data[2]);
    EXPECT_FALSE(c.illuminant_data[0]);
    EXPECT_EQ(slice(built, *c.illuminant_data[2]), ts::filler(12, 0x33));
    ASSERT_TRUE(s.dng->profile_gain_table_map2);
    EXPECT_EQ(slice(built, *s.dng->profile_gain_table_map2), ts::filler(40, 0x44));
    ASSERT_TRUE(c.as_shot_white_xy);
    EXPECT_DOUBLE_EQ((*c.as_shot_white_xy)[0], 0.3127);
}

TEST(TiffInspector, JpegXlLossinessFollowsJxlDistance) {
    for (const double distance : {0.0, 1.5}) {
        Document doc = ts::dng::document();
        ts::Ifd& raw = doc.chain[0].sub_ifds[0];
        ts::dng::find(raw, t::kCompression).integers = {52546};
        raw.entries.push_back(ts::float_entry(t::kJxlDistance, {distance}));
        const TiffSummary s = inspect_ok(ts::build(doc));
        const TiffIfdSummary& r = *s.primary_raw();
        EXPECT_EQ(r.jxl_distance, static_cast<float>(distance));
        EXPECT_EQ(r.compression_info.name, "JPEG XL");
        EXPECT_EQ(r.compression_info.lossiness, distance == 0.0 ? Lossiness::Lossless : Lossiness::Lossy);
    }
}

TEST(TiffInspector, MakeWithNonAsciiBytesIsSanitised) {
    Document doc = ts::dng::document();
    ts::dng::find(doc.chain[0], t::kMake).raw = {std::byte{'A'},  std::byte{0xE9}, std::byte{'B'},
                                                 std::byte{0x07}, std::byte{0},    std::byte{'X'}};
    const TiffSummary s = inspect_ok(ts::build(doc));
    EXPECT_EQ(s.make, "A?B?");  // stops at the first NUL; result is always valid UTF-8
}

// ---- IFD tree ------------------------------------------------------------------------------

TEST(TiffInspector, VisitsChainAndNestedSubIfdsInPreOrder) {
    Document doc = ts::dng::document();
    ts::Ifd nested = ts::dng::preview_ifd();
    nested.sub_ifds = {ts::dng::preview_ifd()};
    doc.chain[0].sub_ifds[1] = nested;
    doc.chain.push_back(ts::dng::preview_ifd());
    const Built built = ts::build(doc);
    const TiffSummary s = inspect_ok(built);
    std::vector<std::string> paths;
    for (const auto& ifd : s.ifds) {
        paths.push_back(ifd.path);
    }
    EXPECT_EQ(paths, (std::vector<std::string>{"ifd0", "ifd0.subifd0", "ifd0.subifd1", "ifd0.subifd1.subifd0",
                                               "ifd0.exif", "ifd1"}));
    EXPECT_EQ(s.ifds[3].depth, 2u);
    EXPECT_EQ(s.ifds[3].parent, 2u);
    EXPECT_EQ(s.ifds[5].kind, TiffIfdKind::Main);
    EXPECT_EQ(s.ifds[5].depth, 0u);
    EXPECT_EQ(s.ifds[5].offset, built.ifd("ifd1"));
}

// ---- Tolerated irregularities ---------------------------------------------------------------

TEST(TiffInspector, EntriesWithUndefinedFieldTypesAreSkippedWithWarnings) {
    Document doc = ts::dng::document();
    ts::Ifd& ifd0 = doc.chain[0];
    ifd0.entries.push_back(ts::raw_entry(50000, 99, 4, ts::filler(4)));  // undefined type, unknown tag
    ts::dng::erase(ifd0, t::kModel);
    ifd0.entries.push_back(ts::raw_entry(t::kModel, 0, 4, ts::filler(4)));  // undefined type, known tag
    // LONG8 is a BigTIFF type; in a classic TIFF it is undefined.
    ifd0.entries.push_back(ts::integer_entry(50001, TiffFieldType::Long8, {1}));
    const TiffSummary s = inspect_ok(ts::build(doc));
    EXPECT_FALSE(s.model);
    ASSERT_EQ(s.warnings.size(), 3u);
    for (const auto& w : s.warnings) {
        EXPECT_EQ(w.where, "raw.tiff.entry");
    }
    EXPECT_TRUE(s.is_dng());
}

TEST(TiffInspector, UninterpretedEntryOutsideTheSourceIsIgnoredWithWarning) {
    Document doc = ts::dng::document();
    ts::Entry strip = ts::long_entry(t::kStripOffsets, {100, 200, 300});
    strip.offset_override = 0xFFFFFFF0u;
    doc.chain[0].entries.push_back(strip);
    ts::Entry huge = ts::long_entry(t::kStripByteCounts, {1});
    huge.count_override = 0x40000000u;  // 4 GiB of LONGs
    doc.chain[0].entries.push_back(huge);
    const TiffSummary s = inspect_ok(ts::build(doc));
    ASSERT_EQ(s.warnings.size(), 2u);
    EXPECT_NE(s.warnings[0].message.find("StripOffsets"), std::string::npos);
    EXPECT_EQ(s.primary_raw_ifd, 1u);
}

TEST(TiffInspector, UnsortedEntriesAreAcceptedWithOneWarning) {
    Document doc = ts::dng::document();
    ts::Ifd& preview = doc.chain[0].sub_ifds[1];
    preview.sort_entries = false;
    std::reverse(preview.entries.begin(), preview.entries.end());
    const TiffSummary s = inspect_ok(ts::build(doc));
    ASSERT_EQ(s.warnings.size(), 1u);
    EXPECT_EQ(s.warnings[0].where, "raw.tiff.ifd");
    EXPECT_EQ(s.ifds[2].width, 1024u);
}

TEST(TiffInspector, DuplicateUninterpretedTagIsAcceptedWithWarning) {
    Document doc = ts::dng::document();
    doc.chain[0].exif.clear();
    doc.chain[0].entries.push_back(ts::ascii_entry(305, "Software A"));  // Software, not interpreted
    doc.chain[0].entries.push_back(ts::ascii_entry(305, "Software B"));
    const TiffSummary s = inspect_ok(ts::build(doc));
    ASSERT_EQ(s.warnings.size(), 1u);
    EXPECT_EQ(s.warnings[0].where, "raw.tiff.ifd");
}

TEST(TiffInspector, SubIfdNextPointerIsIgnoredWithWarning) {
    Built built = ts::build(ts::dng::document());
    built.put(built.next_field("ifd0.subifd1"), built.ifd("ifd0.subifd0"), 4);
    const TiffSummary s = inspect_ok(built);
    EXPECT_EQ(s.ifds.size(), 4u);
    ASSERT_EQ(s.warnings.size(), 1u);
    EXPECT_EQ(s.warnings[0].where, "raw.tiff.ifd_tree");
}

TEST(TiffInspector, WarningsAreCappedAndCounted) {
    Document doc = ts::dng::document();
    for (std::uint16_t i = 0; i < 5; ++i) {
        doc.chain[0].entries.push_back(
            ts::raw_entry(static_cast<std::uint16_t>(60000 + i), 77, 1, {std::byte{0}}));
    }
    TiffInspectLimits limits;
    limits.max_warnings = 2;
    const TiffSummary s = inspect_ok(ts::build(doc), limits);
    EXPECT_EQ(s.warnings.size(), 2u);
    EXPECT_EQ(s.suppressed_warnings, 3u);
}

}  // namespace
}  // namespace lucent::raw

namespace lucent::raw {
namespace {

namespace ts = test_support;
namespace t = tiff_tag;

TEST(TiffInspectorReview, BlackLevelDeltasAndMaskedAreasAreReadAndChecked) {
    // The reference raw IFD has ActiveArea {12, 16, 3024, 4032}: 4016 columns and 3012 rows.
    const auto with = [](std::vector<std::pair<std::int32_t, std::int32_t>> h,
                         std::vector<std::pair<std::int32_t, std::int32_t>> v,
                         std::vector<std::uint64_t> masked) {
        ts::Document doc = ts::dng::document();
        ts::Ifd& raw = doc.chain[0].sub_ifds[0];
        raw.entries.push_back(ts::srational_entry(t::kBlackLevelDeltaH, h));
        raw.entries.push_back(ts::srational_entry(t::kBlackLevelDeltaV, v));
        raw.entries.push_back(ts::long_entry(t::kMaskedAreas, std::move(masked)));
        return ts::build(doc);
    };
    std::vector<std::pair<std::int32_t, std::int32_t>> h(4016, {1, 4});
    h[0] = {-3, 1};
    const std::vector<std::pair<std::int32_t, std::int32_t>> v(3012, {0, 1});
    const std::vector<std::uint64_t> masked = {0, 0, 12, 4032, 12, 0, 3024, 16};

    const TiffSummary s = inspect_ok(with(h, v, masked));
    const TiffIfdSummary* raw = s.primary_raw();
    ASSERT_NE(raw, nullptr);
    ASSERT_EQ(raw->black_level_delta_h.size(), 4016u);
    EXPECT_DOUBLE_EQ(raw->black_level_delta_h[0], -3.0);
    EXPECT_DOUBLE_EQ(raw->black_level_delta_h[1], 0.25);
    EXPECT_EQ(raw->black_level_delta_v, std::vector<double>(3012, 0.0));
    EXPECT_EQ(raw->masked_areas, (std::vector<DngRect>{{0, 0, 12, 4032}, {12, 0, 3024, 16}}));

    // One value short of the ActiveArea width.
    auto short_h = h;
    short_h.pop_back();
    auto bad = inspect(with(short_h, v, masked));
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::Corrupt);
    EXPECT_NE(bad.error().message.find("BlackLevelDeltaH"), std::string::npos) << bad.error().message;
    // A masked rectangle outside the image, an inverted one, and a count that is not 4 x n.
    for (const std::vector<std::uint64_t>& area :
         {std::vector<std::uint64_t>{0, 0, 12, 5000}, std::vector<std::uint64_t>{12, 0, 10, 16},
          std::vector<std::uint64_t>{0, 0, 12, 4032, 1}}) {
        auto wrong = inspect(with(h, v, area));
        ASSERT_FALSE(wrong);
        EXPECT_EQ(wrong.error().code, ErrorCode::Corrupt);
        EXPECT_NE(wrong.error().message.find("MaskedAreas"), std::string::npos) << wrong.error().message;
    }
}

TEST(TiffInspectorReview, Ifd0NoiseProfileIsVerifiedAgainstTheRawPlanes) {
    // Some writers store NoiseProfile in IFD 0 while the raw image is a SubIFD; the adapter uses
    // that copy when the raw IFD has none, so it is held to the same rule (2 or 2 x planes).
    ts::Document doc = ts::dng::document();
    doc.chain[0].entries.push_back(ts::double_entry(t::kNoiseProfile, {1e-5, 1e-7, 2e-5, 2e-7, 3e-5, 3e-7}));
    const TiffSummary s = inspect_ok(ts::build(doc));
    EXPECT_EQ(s.ifds[0].noise_profile.size(), 6u);
    ASSERT_NE(s.primary_raw(), nullptr);
    EXPECT_EQ(s.primary_raw()->noise_profile, (std::vector<double>{2.0e-5, 4.5e-7}));

    doc.chain[0].entries.back() = ts::double_entry(t::kNoiseProfile, {1e-5, 1e-7, 2e-5, 2e-7});
    auto bad = inspect(ts::build(doc));
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::Corrupt);
    EXPECT_NE(bad.error().message.find("NoiseProfile of ifd0"), std::string::npos) << bad.error().message;
}

}  // namespace
}  // namespace lucent::raw
