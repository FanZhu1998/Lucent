// LibRaw adapter tests that need downloaded camera fixtures (tools\fetch-fixtures.ps1). Each test
// skips with a clear message when its fixture is not in fixtures/cache.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/libraw_decoder.hpp>

#include "raw_test_support.hpp"

namespace lucent::raw {
namespace {

namespace ts = test_support;

class LibRawFixtures : public ::testing::Test {
protected:
    void SetUp() override {
        auto decoder = create_libraw_decoder();
        ASSERT_TRUE(decoder);
        decoder_ = std::move(*decoder);
    }

    std::unique_ptr<RawDecoder> decoder_;
    ts::ScratchDir dir_;
};

// Mean of (sample - black level) over the visible area of a single-channel frame.
double mean_above_black(const DecodedRaw& decoded) {
    const RawMetadata& m = decoded.metadata;
    const auto& buffer = std::get<image::ImageBuffer<std::uint16_t>>(pixels_of(decoded.image));
    const image::PixelRect& v = m.geometry.visible_area;
    const auto& colors = m.representation.channel_colors;
    const image::CfaPattern& cfa = m.representation.cfa;
    // Black level of every cell of one CFA tile (the levels have no spatial pattern here).
    EXPECT_EQ(m.levels.pattern_rows, 0u);
    std::vector<double> tile_black(static_cast<std::size_t>(cfa.width() * cfa.height()));
    for (std::int32_t r = 0; r < cfa.height(); ++r) {
        for (std::int32_t c = 0; c < cfa.width(); ++c) {
            const auto channel = static_cast<std::size_t>(
                std::find(colors.begin(), colors.end(), cfa.color_at(r, c)) - colors.begin());
            tile_black[static_cast<std::size_t>(r * cfa.width() + c)] =
                m.levels.black_level_at(channel, 0, 0);
        }
    }
    double sum = 0.0;
    for (std::int32_t y = v.y; y < v.y + v.height; ++y) {
        const std::uint16_t* row = buffer.row(y);
        const std::size_t tile_row = static_cast<std::size_t>((y % cfa.height()) * cfa.width());
        for (std::int32_t x = v.x; x < v.x + v.width; ++x) {
            sum += row[x] - tile_black[tile_row + static_cast<std::size_t>(x % cfa.width())];
        }
    }
    return sum / (static_cast<double>(v.width) * static_cast<double>(v.height));
}

// Order-sensitive checksum of all samples of a single-channel frame.
std::uint64_t checksum(const DecodedRaw& decoded) {
    const auto& pixels = pixels_of(decoded.image);
    const auto& buffer = std::get<image::ImageBuffer<std::uint16_t>>(pixels);
    std::uint64_t sum = 1469598103934665603ull;
    for (std::int32_t y = 0; y < buffer.height(); ++y) {
        const std::uint16_t* row = buffer.row(y);
        for (std::int32_t i = 0; i < buffer.width() * buffer.channels(); ++i) {
            sum = (sum ^ row[i]) * 1099511628211ull;
        }
    }
    return sum;
}

TEST_F(LibRawFixtures, UnicodeNamedCopyDecodesIdentically) {
    LUCENT_REQUIRE_FIXTURE(original, "dji-fc4382-dng");
    const auto copy = dir_.path() / L"été 写真 šćž.DNG";
    std::filesystem::copy_file(original, copy);

    auto a = decoder_->decode(original, {}, {});
    auto b = decoder_->decode(copy, {}, {});
    ASSERT_TRUE(a) << a.error().describe();
    ASSERT_TRUE(b) << b.error().describe();
    EXPECT_EQ(b->metadata.source.path, copy);
    EXPECT_EQ(b->metadata.source.size_bytes, a->metadata.source.size_bytes);
    EXPECT_EQ(b->metadata.format.extension_hint, "dng");
    EXPECT_EQ(checksum(*a), checksum(*b));
    EXPECT_EQ(a->metadata.representation.cfa, b->metadata.representation.cfa);
}

TEST_F(LibRawFixtures, TruncatedCopiesAreCorrupt) {
    // Cuts at 4096 bytes, half and 90 % of every recognised container, and cuts of the last 2
    // bytes where the container's structure reaches the end of the file. Whatever LibRaw answers
    // (it reports some of these as unsupported, or decodes them with wrong pixels), a truncated
    // copy of a supported RAW file is Corrupt (review findings ROBU-1 and ROBU-4).
    struct Case {
        const char* id;
        bool tail_is_structural;  // the file's declared structure ends exactly at its last byte
    };
    const Case cases[] = {
        {"dji-fc4382-dng", false},
        {"insta360-one-x-dng", false},
        {"nikon-z-6-nef-lossless", true},
        {"canon-eos-r5-cr3-raw", true},
        {"sony-ilce-7m4-arw-compressed", false},
        {"fujifilm-x-t4-raf-lossless", true},
        {"om-system-om-1-orf", true},
        {"panasonic-dc-s5m2-rw2", false},
    };
    int tested = 0;
    for (const Case& c : cases) {
        const auto original = ts::fixture_file(c.id);
        if (!original) {
            continue;
        }
        const std::vector<std::byte> bytes = ts::read_all(*original);
        std::vector<std::size_t> lengths = {4096, bytes.size() / 2, bytes.size() / 10 * 9};
        if (c.tail_is_structural) {
            lengths.push_back(bytes.size() - 2);
        }
        for (const std::size_t length : lengths) {
            SCOPED_TRACE(std::string(c.id) + " truncated to " + std::to_string(length));
            const auto path = dir_.write(std::filesystem::path(*original).filename().wstring() + L".cut",
                                         std::span<const std::byte>(bytes.data(), length));
            DecodeOptions options;
            options.subimage = 0;
            auto decoded = decoder_->decode(path, options, {});
            ASSERT_FALSE(decoded) << "a truncated file decoded successfully";
            EXPECT_EQ(decoded.error().code, ErrorCode::Corrupt) << decoded.error().describe();
        }
        ++tested;
    }
    if (tested == 0) {
        GTEST_SKIP() << "no fixture of this test is downloaded; run tools\\fetch-fixtures.ps1";
    }
}

TEST_F(LibRawFixtures, OrfCutByTwoBytesIsTruncatedNotDecoded) {
    // Review finding ROBU-1: LibRaw decodes this ORF with altered pixels in the last row when its
    // last 1 to ~45 bytes are missing; the strip extent check of the ORF's TIFF structure
    // catches it.
    LUCENT_REQUIRE_FIXTURE(original, "om-system-om-1-orf");
    const std::vector<std::byte> bytes = ts::read_all(original);
    for (const std::size_t cut : {std::size_t{1}, std::size_t{2}, std::size_t{17}, std::size_t{32}}) {
        SCOPED_TRACE(cut);
        const auto path =
            dir_.write(L"cut.ORF", std::span<const std::byte>(bytes.data(), bytes.size() - cut));
        auto decoded = decoder_->decode(path, {}, {});
        ASSERT_FALSE(decoded);
        EXPECT_EQ(decoded.error().code, ErrorCode::Corrupt) << decoded.error().describe();
        EXPECT_EQ(decoded.error().where, "raw.libraw.truncated") << decoded.error().describe();
        EXPECT_NE(decoded.error().message.find("strip data ends"), std::string::npos)
            << decoded.error().message;
    }
    // The complete file still decodes.
    auto complete = decoder_->decode(original, {}, {});
    ASSERT_TRUE(complete) << complete.error().describe();
    EXPECT_EQ(complete->metadata.format.container, ContainerFormat::Orf);
}

TEST_F(LibRawFixtures, CompressionModesAreDistinguished) {
    // Review finding NUME-1: C-RAW, lossy NEF, lossy ARW and lossy RAF must not look like their
    // lossless counterparts.
    struct Case {
        const char* id;
        Lossiness lossiness;
        const char* name;
    };
    const Case cases[] = {
        {"canon-eos-r5-cr3-raw", Lossiness::Lossless, "Canon CRX lossless (RAW)"},
        {"canon-eos-r5-cr3-craw", Lossiness::Lossy, "Canon CRX lossy (C-RAW)"},
        {"canon-eos-5d-mark-iv-cr2-raw", Lossiness::Lossless, "Lossless JPEG"},
        {"nikon-z-6-nef-lossless", Lossiness::Lossless, "Nikon NEF lossless"},
        {"nikon-z-6-nef-compressed", Lossiness::Lossy, "Nikon NEF lossy (type 2)"},
        {"nikon-z-8-nef-he", Lossiness::Lossy, "Nikon NEF high efficiency (HE)"},
        {"nikon-z-8-nef-he-star", Lossiness::Lossy, "Nikon NEF high efficiency (HE*)"},
        {"sony-ilce-7m4-arw-uncompressed", Lossiness::Lossless, "Uncompressed"},
        {"sony-ilce-7m4-arw-compressed", Lossiness::Lossy, "Sony ARW compressed (lossy)"},
        {"sony-ilce-7m4-arw-lossless-l", Lossiness::Lossless, "Sony ARW lossless compressed"},
        {"fujifilm-x-t4-raf-lossless", Lossiness::Lossless, "Fujifilm RAF lossless compressed"},
        {"fujifilm-x-t4-raf-compressed", Lossiness::Lossy, "Fujifilm RAF compressed (lossy)"},
        {"om-system-om-1-orf", Lossiness::Lossless, "Olympus ORF lossless compressed"},
        {"pentax-k-3-mark-iii-pef", Lossiness::Lossless, "Pentax PEF lossless compressed"},
        {"dji-fc4382-dng", Lossiness::Lossless, "Lossless JPEG"},
    };
    int tested = 0;
    for (const Case& c : cases) {
        const auto path = ts::fixture_file(c.id);
        if (!path) {
            continue;
        }
        SCOPED_TRACE(c.id);
        auto probe = decoder_->probe(*path, {}, {});
        ASSERT_TRUE(probe) << probe.error().describe();
        const RawMetadata& m = probe->metadata;
        EXPECT_EQ(m.format.compression.name, c.name) << m.format.compression.basis;
        EXPECT_EQ(m.format.compression.lossiness, c.lossiness) << m.format.compression.basis;
        EXPECT_FALSE(m.format.compression.basis.empty());
        EXPECT_EQ(m.has_prior_processing(PriorProcessingKind::LossyCompression),
                  c.lossiness == Lossiness::Lossy);
        ++tested;
    }
    if (tested == 0) {
        GTEST_SKIP() << "no fixture of this test is downloaded; run tools\\fetch-fixtures.ps1";
    }
}

TEST_F(LibRawFixtures, NoiseProfileStoredOnlyInIfdZeroIsReported) {
    // Review finding NUME-2: the DJI Mavic 3 Pro writes NoiseProfile to IFD 0 only.
    LUCENT_REQUIRE_FIXTURE(path, "dji-fc4382-dng");
    auto probe = decoder_->probe(path, {}, {});
    ASSERT_TRUE(probe) << probe.error().describe();
    const RawMetadata& m = probe->metadata;
    ASSERT_TRUE(m.dng);
    EXPECT_EQ(m.dng->raw_ifd.path, "ifd0.subifd0");
    ASSERT_TRUE(m.noise);
    EXPECT_EQ(m.noise->source_ifd, "ifd0");
    EXPECT_EQ(m.noise->provenance, Provenance::DngTag);
    ASSERT_FALSE(m.noise->planes.empty());
    // Values as stored (DOUBLE), compared with the tag dump of the review: S 1.0458e-4, O 2.8818e-8.
    EXPECT_NEAR(m.noise->planes[0].scale, 1.0458e-4, 1e-8);
    EXPECT_NEAR(m.noise->planes[0].offset, 2.8818e-8, 1e-12);
}

TEST_F(LibRawFixtures, DualFisheye360SourceIsRecognised) {
    // Review finding NUME-4: the Insta360 One X stores both fisheye views in one 3040 x 6080 frame.
    LUCENT_REQUIRE_FIXTURE(path, "insta360-one-x-dng");
    auto probe = decoder_->probe(path, {}, {});
    ASSERT_TRUE(probe) << probe.error().describe();
    const RawMetadata& m = probe->metadata;
    EXPECT_EQ(m.projection.layout, ViewLayout::DualFisheye360);
    EXPECT_EQ(m.interpretation.status, InterpretationStatus::RequiresUnsupportedOperation);
    const bool flagged = std::any_of(m.interpretation.issues.begin(), m.interpretation.issues.end(),
                                     [](const InterpretationIssue& issue) {
                                         return issue.kind == InterpretationIssueKind::MultiViewSource;
                                     });
    EXPECT_TRUE(flagged);
}

TEST_F(LibRawFixtures, DualPixelFileRequiresAnExplicitSubimage) {
    LUCENT_REQUIRE_FIXTURE(path, "canon-eos-r5-cr3-craw-crop-dualpixel");
    auto probe = decoder_->probe(path, {}, {});
    ASSERT_TRUE(probe) << probe.error().describe();
    EXPECT_EQ(probe->metadata.subimage_count, 2u);
    EXPECT_EQ(probe->metadata.subimage_index, 0u);
    ASSERT_EQ(probe->subimages.size(), 2u);
    for (const SubimageInfo& info : probe->subimages) {
        EXPECT_FALSE(info.error);
        EXPECT_EQ(info.raw_width, probe->metadata.geometry.raw_width);
        EXPECT_EQ(info.representation, RawRepresentation::Mosaic);
    }
    // Review finding NUME-3: the listing says which image is the photograph.
    EXPECT_EQ(probe->metadata.subimage_role, SubimageRole::Primary);
    EXPECT_EQ(probe->subimages[0].role, SubimageRole::Primary);
    EXPECT_EQ(probe->subimages[0].interpretation, InterpretationStatus::Complete);
    EXPECT_EQ(probe->subimages[1].role, SubimageRole::DualPixelComponent);
    EXPECT_EQ(probe->subimages[1].interpretation, InterpretationStatus::RequiresUnsupportedOperation);

    // Never silently the first image.
    auto implicit = decoder_->decode(path, {}, {});
    ASSERT_FALSE(implicit);
    EXPECT_EQ(implicit.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(implicit.error().where, "raw.libraw.subimage");

    DecodeOptions first;
    first.subimage = 0;
    DecodeOptions second;
    second.subimage = 1;
    auto a = decoder_->decode(path, first, {});
    auto b = decoder_->decode(path, second, {});
    ASSERT_TRUE(a) << a.error().describe();
    ASSERT_TRUE(b) << b.error().describe();
    EXPECT_EQ(a->metadata.subimage_index, 0u);
    EXPECT_EQ(b->metadata.subimage_index, 1u);
    EXPECT_NE(checksum(*a), checksum(*b));  // the two Dual Pixel images differ
    EXPECT_EQ(a->metadata.subimage_role, SubimageRole::Primary);
    EXPECT_EQ(a->metadata.interpretation.status, InterpretationStatus::Complete);
    EXPECT_EQ(b->metadata.subimage_role, SubimageRole::DualPixelComponent);
    EXPECT_EQ(b->metadata.interpretation.status, InterpretationStatus::RequiresUnsupportedOperation);
    // The component frame holds about half the signal of the main image (review measurement:
    // 0.54); this is what makes it unsuitable as the photograph.
    const double ratio = mean_above_black(*b) / mean_above_black(*a);
    EXPECT_GT(ratio, 0.4);
    EXPECT_LT(ratio, 0.7);

    DecodeOptions third;
    third.subimage = 2;
    auto missing = decoder_->decode(path, third, {});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
}

TEST_F(LibRawFixtures, UndecodableCompressionIsReportedAndRefused) {
    LUCENT_REQUIRE_FIXTURE(path, "nikon-z-8-nef-he");
    auto probe = decoder_->probe(path, {}, {});
    ASSERT_TRUE(probe) << probe.error().describe();
    EXPECT_EQ(probe->metadata.interpretation.status, InterpretationStatus::NotDecodable);
    EXPECT_EQ(probe->metadata.camera.normalized_make, "Nikon");
    auto decoded = decoder_->decode(path, {}, {});
    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code, ErrorCode::Unsupported);
    EXPECT_EQ(decoded.error().where, "raw.libraw.interpret");
}

// The drone DNGs of the corpus carry mandatory opcodes (ADR 0014); Lucent applies every one of
// them, so their interpretation is complete (W1 to W8 reported them as unsupported).
TEST_F(LibRawFixtures, DroneDngOpcodesAreSupported) {
    struct Expected {
        const char* id;
        std::vector<std::pair<OperationStage, std::string>> opcodes;
    };
    const std::vector<Expected> cases = {
        {"dji-fc7303-dng",
         {{OperationStage::AfterDemosaic, "FixVignetteRadial"},
          {OperationStage::AfterDemosaic, "WarpRectilinear"},
          {OperationStage::AfterDemosaic, "GainMap"}}},
        {"dji-fc4382-dng", {{OperationStage::AfterDemosaic, "WarpRectilinear"}}},
        {"dji-fc220-dng",
         {{OperationStage::StoredRaw, "FixBadPixelsList"}, {OperationStage::AfterDemosaic, "FixVignetteRadial"}}},
        {"hasselblad-l2d-20c-dng",
         {{OperationStage::AfterDemosaic, "GainMap"}, {OperationStage::AfterDemosaic, "WarpRectilinear"}}},
    };
    int checked = 0;
    for (const Expected& expected : cases) {
        SCOPED_TRACE(expected.id);
        const auto path = test_support::fixture_file(expected.id);
        if (!path) {
            continue;
        }
        auto probe = decoder_->probe(*path, {}, {});
        ASSERT_TRUE(probe) << probe.error().describe();
        const RawMetadata& m = probe->metadata;
        EXPECT_EQ(m.interpretation.status, InterpretationStatus::Complete);
        std::vector<std::pair<OperationStage, std::string>> found;
        for (const RequiredOperation& op : m.required_operations) {
            EXPECT_TRUE(op.mandatory) << op.name;
            EXPECT_TRUE(op.supported_by_lucent) << op.name << ": " << op.lucent_note;
            found.emplace_back(op.stage, op.name);
        }
        EXPECT_EQ(found, expected.opcodes);
        ++checked;
    }
    if (checked == 0) {
        GTEST_SKIP() << "no drone DNG fixture is downloaded; run tools\\fetch-fixtures.ps1 -Tier all";
    }
}

TEST_F(LibRawFixtures, FullColourModesCarryTheirPriorProcessing) {
    LUCENT_REQUIRE_FIXTURE(path, "sony-ilce-7m4-arw-lossless-m");
    auto decoded = decoder_->decode(path, {}, {});
    ASSERT_TRUE(decoded) << decoded.error().describe();
    const RawMetadata& m = decoded->metadata;
    EXPECT_EQ(m.representation.kind, RawRepresentation::LinearColor);
    EXPECT_TRUE(m.has_prior_processing(PriorProcessingKind::Demosaiced));
    EXPECT_TRUE(m.has_prior_processing(PriorProcessingKind::YCbCrConverted));
    EXPECT_TRUE(m.has_prior_processing(PriorProcessingKind::WhiteBalanceApplied));
    EXPECT_TRUE(std::holds_alternative<LinearColorImage>(decoded->image));
}

}  // namespace
}  // namespace lucent::raw

namespace lucent::raw {
namespace {

// The camera image area of every corpus fixture (contract 1.4, ADR 0017), from the probe. The
// values were read 2026-10-05 with LibRaw 0.22.2 and agree with the cited sources of
// fixtures/qualification/expectations.json (manufacturer sizes, the files' own crop tags) except
// where noted; Unknown means the decoder reads no crop declaration for the file.
TEST_F(LibRawFixtures, CameraImageAreasOfTheCorpus) {
    using S = ImageAreaSource;
    using P = Provenance;
    struct Case {
        const char* id;
        S source;
        P provenance;
        AreaRect rect;
    };
    const Case cases[] = {
        // Canon CR3: 8192 x 5464 (Canon's recorded size), one column and row beyond LibRaw's visible
        // area (the engine clips it to the visible area).
        {"canon-eos-r5-cr3-raw", S::DecoderInsetCrop, P::Decoder, {144, 112, 8192, 5464}},
        {"canon-eos-r5-cr3-craw", S::DecoderInsetCrop, P::Decoder, {144, 112, 8192, 5464}},
        {"canon-eos-r5-cr3-raw-crop", S::DecoderInsetCrop, P::Decoder, {144, 108, 5088, 3392}},
        {"canon-eos-r5-cr3-craw-crop-dualpixel", S::DecoderInsetCrop, P::Decoder, {144, 108, 5088, 3392}},
        {"canon-eos-5d-mark-iv-cr2-raw", S::DecoderInsetCrop, P::Decoder, {148, 54, 6720, 4480}},
        {"canon-eos-5d-mark-iv-cr2-sraw", S::DecoderInsetCrop, P::Decoder, {0, 0, 3360, 2240}},
        {"sony-ilce-7m4-arw-compressed", S::DecoderInsetCrop, P::Decoder, {12, 8, 7008, 4672}},
        {"sony-ilce-7m4-arw-uncompressed", S::DecoderInsetCrop, P::Decoder, {12, 8, 7008, 4672}},
        {"sony-ilce-7m4-arw-lossless-l", S::DecoderInsetCrop, P::Decoder, {12, 8, 7008, 4672}},
        {"sony-ilce-7m4-arw-lossless-m", S::DecoderInsetCrop, P::Decoder, {8, 4, 4608, 3072}},
        {"sony-ilce-7m4-arw-lossless-s", S::DecoderInsetCrop, P::Decoder, {6, 4, 3504, 2336}},
        {"sony-ilce-7m4-arw-compressed-apsc", S::DecoderInsetCrop, P::Decoder, {44, 30, 4608, 3072}},
        {"sony-ilce-7rm5-arw-compressed", S::DecoderInsetCrop, P::Decoder, {32, 20, 9504, 6336}},
        {"nikon-z-6-nef-lossless", S::DecoderInsetCrop, P::Decoder, {8, 8, 6048, 4024}},
        {"nikon-z-6-nef-compressed", S::DecoderInsetCrop, P::Decoder, {8, 8, 6048, 4024}},
        {"nikon-z-8-nef-he", S::DecoderInsetCrop, P::Decoder, {12, 8, 8256, 5504}},
        {"nikon-z-8-nef-he-star", S::DecoderInsetCrop, P::Decoder, {12, 8, 8256, 5504}},
        // X-Trans: LibRaw moves the RAF's top edge 13 to 18 (its X-Trans period); Lucent restores
        // the file's RawImageCropTopLeft / RawImageCroppedSize.
        {"fujifilm-x-t4-raf-lossless", S::DecoderInsetCrop, P::LucentRule, {6, 13, 6240, 4160}},
        {"fujifilm-x-t4-raf-compressed", S::DecoderInsetCrop, P::LucentRule, {6, 13, 6240, 4160}},
        {"fujifilm-gfx-50s-ii-raf-compressed", S::DecoderInsetCrop, P::Decoder, {12, 8, 8256, 6192}},
        {"panasonic-dc-s5m2-rw2", S::DecoderInsetCrop, P::Decoder, {4, 4, 6000, 4000}},
        {"om-system-om-1-orf", S::DecoderInsetCrop, P::Decoder, {12, 12, 5184, 3888}},
        // LibRaw reads no crop for the K-3 Mark III PEF (no maker-note tags 0x0038/0x0039) and does
        // not read the 3FR raw IFD's DefaultCrop tags (it promotes them for Sony only).
        {"pentax-k-3-mark-iii-pef", S::Unknown, P::LucentRule, {}},
        {"hasselblad-x1d-ii-50c-3fr", S::Unknown, P::LucentRule, {}},
        {"dji-fc4382-dng", S::DngDefaultCrop, P::DngTag, {4, 4, 4024, 3016}},
        {"dji-fc220-dng", S::DngDefaultCrop, P::DngTag, {4, 4, 3992, 2992}},
        {"dji-fc7303-dng", S::DngDefaultCrop, P::DngTag, {0, 0, 4000, 3000}},
        {"hasselblad-l2d-20c-dng", S::DngDefaultCrop, P::DngTag, {4, 4, 5272, 3948}},
        {"insta360-one-x-dng", S::DngDefaultWholeImage, P::LucentRule, {0, 0, 3040, 6080}},
        {"pentax-k-3-mark-iii-monochrome-dng", S::DngDefaultCrop, P::DngTag, {54, 58, 6192, 4128}},
        {"leica-m10-dng", S::DngDefaultCrop, P::DngTag, {4, 4, 5976, 3984}},
        {"leica-m-monochrom-typ-246-dng", S::DngDefaultCrop, P::DngTag, {4, 4, 5976, 3992}},
        {"adobe-dng-canon-eos-5d-mark-iii-linear-dng", S::DngDefaultCrop, P::DngTag, {0, 0, 3960, 2640}},
        {"canon-eos-7d-mark-ii-dng-jxl", S::DngDefaultCrop, P::DngTag, {84, 50, 5472, 3648}},
    };
    int tested = 0;
    for (const Case& c : cases) {
        const auto path = ts::fixture_file(c.id);
        if (!path) {
            continue;
        }
        SCOPED_TRACE(c.id);
        ProbeOptions options;
        options.subimage = 0;
        options.describe_subimages = false;
        auto probe = decoder_->probe(*path, options, {});
        ASSERT_TRUE(probe) << probe.error().describe();
        const ImageArea& a = probe->metadata.image_area;
        EXPECT_EQ(a.source, c.source) << a.basis;
        EXPECT_FALSE(a.basis.empty());
        if (c.source == S::Unknown) {
            EXPECT_FALSE(a.rect);
        } else {
            EXPECT_EQ(a.provenance, c.provenance) << a.basis;
            ASSERT_TRUE(a.rect) << a.basis;
            EXPECT_EQ(*a.rect, c.rect)
                << a.rect->x << "," << a.rect->y << " " << a.rect->width << "x" << a.rect->height;
        }
        ++tested;
    }
    if (tested == 0) {
        GTEST_SKIP() << "no fixture of this test is downloaded; run tools\fetch-fixtures.ps1";
    }
}

}  // namespace
}  // namespace lucent::raw
