// Unit tests of the LibRaw adapter's classification rules (src/libraw_support.hpp): compression
// of non-DNG data, subimage roles and view layout. The rules are pure functions of the facts
// LibRaw reports, so every branch is tested here without camera files; the fixture tests
// (libraw_fixture_test.cpp) check the facts LibRaw extracts from real files.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/raw/decoded_raw.hpp>

#include "libraw_support.hpp"

namespace lucent::raw::detail {
namespace {

DataCompression classify(std::string_view unpacker, CompressionFacts facts = {}) {
    facts.unpacker = unpacker;
    return classify_vendor_compression(facts);
}

TEST(CompressionClassification, CanonCrxFollowsTheRecordedQuality) {
    CompressionFacts facts;
    facts.canon_quality = 4;
    const DataCompression raw = classify("crxLoadRaw()", facts);
    EXPECT_EQ(raw.name, "Canon CRX lossless (RAW)");
    EXPECT_EQ(raw.lossiness, Lossiness::Lossless);
    EXPECT_EQ(raw.provenance, Provenance::Decoder);
    EXPECT_FALSE(raw.tiff_code);
    EXPECT_NE(raw.basis.find("Quality 4"), std::string::npos) << raw.basis;
    EXPECT_NE(raw.basis.find("crxLoadRaw()"), std::string::npos) << raw.basis;

    facts.canon_quality = 7;
    const DataCompression craw = classify("crxLoadRaw()", facts);
    EXPECT_EQ(craw.name, "Canon CRX lossy (C-RAW)");
    EXPECT_EQ(craw.lossiness, Lossiness::Lossy);

    facts.canon_quality = -1;  // not recorded
    EXPECT_EQ(classify("crxLoadRaw()", facts).lossiness, Lossiness::Unknown);
}

TEST(CompressionClassification, NikonFollowsNefCompression) {
    const struct {
        unsigned code;
        std::string_view name;
        Lossiness lossiness;
    } cases[] = {
        {1, "Nikon NEF lossy (type 1)", Lossiness::Lossy},
        {3, "Nikon NEF lossless", Lossiness::Lossless},
        {4, "Nikon NEF lossy (type 2)", Lossiness::Lossy},
        {13, "Nikon NEF high efficiency (HE)", Lossiness::Lossy},
        {14, "Nikon NEF high efficiency (HE*)", Lossiness::Lossy},
        {0, "Nikon NEF compressed (mode not recorded)", Lossiness::Unknown},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.code);
        CompressionFacts facts;
        facts.nikon_nef_compression = c.code;
        const DataCompression result =
            classify(c.code >= 13 ? "nikon_he_load_raw()" : "nikon_load_raw()", facts);
        EXPECT_EQ(result.name, c.name);
        EXPECT_EQ(result.lossiness, c.lossiness);
    }
}

TEST(CompressionClassification, FujifilmFollowsTheCompressedHeaderFlag) {
    CompressionFacts facts;
    facts.fuji_lossless = 1;
    EXPECT_EQ(classify("fuji_compressed_load_raw()", facts).lossiness, Lossiness::Lossless);
    facts.fuji_lossless = 0;
    const DataCompression lossy = classify("fuji_compressed_load_raw()", facts);
    EXPECT_EQ(lossy.name, "Fujifilm RAF compressed (lossy)");
    EXPECT_EQ(lossy.lossiness, Lossiness::Lossy);
    facts.fuji_lossless = -1;
    EXPECT_EQ(classify("fuji_compressed_load_raw()", facts).lossiness, Lossiness::Unknown);
}

TEST(CompressionClassification, SonyCodecsDecideTheirLossiness) {
    CompressionFacts facts;
    facts.sony_raw_file_type = 2;
    const DataCompression lossy = classify("sony_arw2_load_raw()", facts);
    EXPECT_EQ(lossy.name, "Sony ARW compressed (lossy)");
    EXPECT_EQ(lossy.lossiness, Lossiness::Lossy);
    EXPECT_NE(lossy.basis.find("SonyRawFileType 2"), std::string::npos) << lossy.basis;
    EXPECT_EQ(classify("sony_ljpeg_load_raw()").lossiness, Lossiness::Lossless);
    EXPECT_EQ(classify("sony_ycbcr_load_raw()").lossiness, Lossiness::Lossless);
    // Uncompressed ARW uses LibRaw's generic unpacker.
    EXPECT_EQ(classify("unpacked_load_raw()").name, "Uncompressed");
}

TEST(CompressionClassification, FixedCodecsAndUnknownCodecs) {
    EXPECT_EQ(classify("lossless_jpeg_load_raw()").name, "Lossless JPEG");
    EXPECT_EQ(classify("olympus_load_raw()").lossiness, Lossiness::Lossless);
    EXPECT_EQ(classify("pentax_load_raw()").lossiness, Lossiness::Lossless);
    EXPECT_EQ(classify("lossy_dng_load_raw()").lossiness, Lossiness::Lossy);

    CompressionFacts panasonic;
    panasonic.panasonic_encoding = 5;
    EXPECT_EQ(classify("panasonic_load_raw()", panasonic).name, "Uncompressed");
    panasonic.panasonic_encoding = 4;
    EXPECT_EQ(classify("panasonic_load_raw()", panasonic).lossiness, Lossiness::Unknown);

    // A codec without a rule is named but its lossiness is not claimed.
    const DataCompression other = classify("panasonicC8_load_raw()");
    EXPECT_EQ(other.name, "panasonicC8_load_raw (lossiness not established)");
    EXPECT_EQ(other.lossiness, Lossiness::Unknown);

    // Nothing known: say so explicitly.
    const DataCompression none = classify("");
    EXPECT_EQ(none.name, "unknown");
    EXPECT_EQ(none.lossiness, Lossiness::Unknown);
    EXPECT_FALSE(none.basis.empty());
}

SubimageFacts cr3(std::uint32_t count, std::uint32_t index, std::uint32_t tracks, std::uint32_t frames) {
    SubimageFacts facts;
    facts.count = count;
    facts.index = index;
    facts.canon_cr3 = true;
    facts.cr3_full_size_raw_tracks = tracks;
    facts.cr3_max_frames_per_track = frames;
    return facts;
}

TEST(SubimageClassification, SingleImagesArePrimary) {
    const SubimageClassification single = classify_subimage(SubimageFacts{});
    EXPECT_EQ(single.role, SubimageRole::Primary);
    EXPECT_FALSE(single.issue);
    EXPECT_EQ(classify_subimage(cr3(1, 0, 1, 1)).role, SubimageRole::Primary);
}

TEST(SubimageClassification, CanonDualPixelSecondFrameIsNotAPhotograph) {
    const SubimageClassification main = classify_subimage(cr3(2, 0, 2, 1));
    EXPECT_EQ(main.role, SubimageRole::Primary);
    EXPECT_FALSE(main.issue);

    const SubimageClassification component = classify_subimage(cr3(2, 1, 2, 1));
    EXPECT_EQ(component.role, SubimageRole::DualPixelComponent);
    ASSERT_TRUE(component.issue);
    EXPECT_EQ(component.issue->kind, InterpretationIssueKind::SubimageNotAPhotograph);
    EXPECT_EQ(derive_interpretation_status(std::vector<InterpretationIssue>{*component.issue}),
              InterpretationStatus::RequiresUnsupportedOperation);
}

TEST(SubimageClassification, CanonRawBurstFramesArePhotographs) {
    for (std::uint32_t index : {0u, 5u}) {
        const SubimageClassification frame = classify_subimage(cr3(12, index, 1, 12));
        EXPECT_EQ(frame.role, SubimageRole::BurstFrame);
        EXPECT_FALSE(frame.issue);
    }
}

TEST(SubimageClassification, PentaxPixelShiftFramesAreNotCombined) {
    SubimageFacts facts;
    facts.count = 4;
    facts.index = 2;
    facts.pentax_pixel_shift = true;
    const SubimageClassification frame = classify_subimage(facts);
    EXPECT_EQ(frame.role, SubimageRole::PixelShiftFrame);
    ASSERT_TRUE(frame.issue);
    EXPECT_EQ(derive_interpretation_status(std::vector<InterpretationIssue>{*frame.issue}),
              InterpretationStatus::OptionalOperationsSkipped);
}

TEST(SubimageClassification, AnythingElseIsUnknownAndAmbiguous) {
    SubimageFacts dng;
    dng.count = 2;
    dng.index = 0;
    const SubimageClassification unknown = classify_subimage(dng);
    EXPECT_EQ(unknown.role, SubimageRole::Unknown);
    ASSERT_TRUE(unknown.issue);
    EXPECT_EQ(unknown.issue->kind, InterpretationIssueKind::SubimageRoleUnknown);
    // A CR3 whose track layout matches neither Dual Pixel nor burst.
    EXPECT_EQ(classify_subimage(cr3(3, 1, 3, 1)).role, SubimageRole::Unknown);
    EXPECT_EQ(classify_subimage(cr3(4, 1, 2, 2)).role, SubimageRole::Unknown);
}

TEST(ProjectionClassification, TwoLens360BodiesWithTwoToOneFramesAreDualFisheye) {
    const ProjectionInfo one_x = classify_projection("Arashi Vision", "Insta360 One2", 3040, 6080);
    EXPECT_EQ(one_x.layout, ViewLayout::DualFisheye360);
    EXPECT_EQ(one_x.provenance, Provenance::LucentRule);
    EXPECT_NE(one_x.basis.find("Insta360 One2"), std::string::npos) << one_x.basis;
    EXPECT_EQ(classify_projection("INSTA360", "X4", 11904, 5952).layout, ViewLayout::DualFisheye360);
    EXPECT_EQ(classify_projection("RICOH", "RICOH THETA Z1", 7296, 3648).layout, ViewLayout::DualFisheye360);
}

TEST(ProjectionClassification, PlanarCamerasStaySingleView) {
    // Insta360's single-lens cameras (Ace Pro: 4:3) are ordinary planar sources.
    EXPECT_EQ(classify_projection("Arashi Vision", "Insta360 Ace Pro", 8192, 6144).layout,
              ViewLayout::SingleView);
    // A 2:1 frame from another maker says nothing about lenses.
    const ProjectionInfo other = classify_projection("Canon", "EOS R5", 6000, 3000);
    EXPECT_EQ(other.layout, ViewLayout::SingleView);
    EXPECT_TRUE(other.basis.empty());
    EXPECT_EQ(classify_projection("RICOH", "GR III", 6000, 3000).layout, ViewLayout::SingleView);
    EXPECT_EQ(classify_projection("Arashi Vision", "Insta360 One2", 0, 0).layout, ViewLayout::SingleView);
}

}  // namespace
}  // namespace lucent::raw::detail
