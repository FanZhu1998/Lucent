// The camera's intended image area in the decoded-RAW contract (decoded_raw.hpp ImageArea,
// contract 1.4; ADR 0017): the DNG rule (dng_image_area), LibRaw's inset crops
// (image_area_from_inset_crops, libraw_adjusted_inset_crop), the RAF crop reader, the inspector's
// DefaultUserCrop and the contract's consistency rules, on synthetic data; then end to end through
// the LibRaw adapter on synthetic DNGs with odd ActiveArea margins.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/libraw_decoder.hpp>
#include <lucent/raw/tiff_inspector.hpp>

#include "container_extent.hpp"
#include "libraw_support.hpp"
#include "raw_test_support.hpp"
#include "synthetic_dng.hpp"

namespace lucent::raw {
namespace {

namespace ts = test_support;
using detail::InsetCrop;
using detail::InsetCropFacts;

// ---- DNG ---------------------------------------------------------------------------------------

TiffIfdSummary dng_ifd(std::uint32_t width, std::uint32_t height,
                       std::optional<DngRect> active = std::nullopt) {
    TiffIfdSummary ifd;
    ifd.path = "ifd0.subifd0";
    ifd.width = width;
    ifd.height = height;
    ifd.active_area = active;
    return ifd;
}

TEST(DngImageArea, DefaultCropIsRelativeToTheActiveArea) {
    TiffIfdSummary ifd = dng_ifd(6304, 4224, DngRect{34, 26, 4194, 6250});  // Pentax K-3 III Monochrome
    ifd.default_crop_origin = std::array<double, 2>{28.0, 24.0};
    ifd.default_crop_size = std::array<double, 2>{6192.0, 4128.0};
    const ImageArea a = dng_image_area(ifd, image::Size{6304, 4224});
    EXPECT_EQ(a.source, ImageAreaSource::DngDefaultCrop);
    EXPECT_EQ(a.provenance, Provenance::DngTag);
    ASSERT_TRUE(a.rect);
    EXPECT_EQ(*a.rect, (AreaRect{54.0, 58.0, 6192.0, 4128.0}));
    EXPECT_EQ(a.reference, (image::PixelRect{26, 34, 6224, 4160}));
    EXPECT_FALSE(a.user_crop);
    EXPECT_NE(a.basis.find("DefaultCropOrigin (28, 24)"), std::string::npos) << a.basis;
    EXPECT_NE(a.basis.find("ActiveArea 6224x4160+26+34"), std::string::npos) << a.basis;
}

TEST(DngImageArea, FractionalValuesAreKeptAsStored) {
    TiffIfdSummary ifd = dng_ifd(100, 80);
    ifd.default_crop_origin = std::array<double, 2>{2.5, 3.25};
    ifd.default_crop_size = std::array<double, 2>{90.5, 70.0};
    const ImageArea a = dng_image_area(ifd, image::Size{100, 80});
    ASSERT_TRUE(a.rect);
    EXPECT_EQ(*a.rect, (AreaRect{2.5, 3.25, 90.5, 70.0}));
    EXPECT_EQ(a.reference, (image::PixelRect{0, 0, 100, 80}));
}

TEST(DngImageArea, WithoutTheTagsTheWholeActiveAreaIsTheDefault) {
    const ImageArea a = dng_image_area(dng_ifd(100, 80, DngRect{1, 3, 79, 99}), image::Size{100, 80});
    EXPECT_EQ(a.source, ImageAreaSource::DngDefaultWholeImage);
    EXPECT_EQ(a.provenance, Provenance::LucentRule);
    ASSERT_TRUE(a.rect);
    EXPECT_EQ(*a.rect, (AreaRect{3.0, 1.0, 96.0, 78.0}));
}

// QUA-09: each tag has its own default (DNG 1.4.0.0 p. 31): DefaultCropOrigin (0, 0), DefaultCropSize
// ImageWidth x ImageLength.
TEST(DngImageArea, ALoneTagTakesTheSpecificationsDefaultForTheOther) {
    TiffIfdSummary lone = dng_ifd(100, 80, DngRect{2, 4, 78, 96});
    lone.default_crop_size = std::array<double, 2>{90.0, 70.0};
    const ImageArea a = dng_image_area(lone, image::Size{100, 80});
    EXPECT_EQ(a.source, ImageAreaSource::DngDefaultCrop);
    ASSERT_TRUE(a.rect);
    EXPECT_EQ(*a.rect, (AreaRect{4.0, 2.0, 90.0, 70.0})) << "origin (0, 0) of the ActiveArea";
    EXPECT_EQ(a.reference, (image::PixelRect{4, 2, 92, 76}));
    EXPECT_NE(a.basis.find("DefaultCropOrigin is absent"), std::string::npos) << a.basis;

    // A lone origin: the size defaults to the IFD's size, which the placement moves back inside the
    // stage-3 image and cuts (the DNG SDK's DefaultCropArea()).
    TiffIfdSummary origin_only = dng_ifd(100, 80);
    origin_only.default_crop_origin = std::array<double, 2>{3.0, 5.0};
    const ImageArea b = dng_image_area(origin_only, image::Size{100, 80});
    EXPECT_EQ(b.source, ImageAreaSource::DngDefaultCrop);
    ASSERT_TRUE(b.rect);
    EXPECT_EQ(*b.rect, (AreaRect{3.0, 5.0, 100.0, 80.0}));
    EXPECT_NE(b.basis.find("DefaultCropSize is absent"), std::string::npos) << b.basis;
}

TEST(DngImageArea, AnotherFrameSizeIsUnknown) {
    TiffIfdSummary other = dng_ifd(100, 80);
    other.default_crop_origin = std::array<double, 2>{0.0, 0.0};
    other.default_crop_size = std::array<double, 2>{90.0, 70.0};
    EXPECT_EQ(dng_image_area(other, image::Size{120, 80}).source, ImageAreaSource::Unknown);
}

TEST(DngImageArea, DefaultUserCropIsCarriedUnlessItIsTheWholeArea) {
    TiffIfdSummary ifd = dng_ifd(100, 80);
    ifd.default_crop_origin = std::array<double, 2>{4.0, 4.0};
    ifd.default_crop_size = std::array<double, 2>{92.0, 72.0};
    ifd.default_user_crop = std::array<double, 4>{0.125, 0.0, 0.875, 1.0};  // a 16:9 crop of the area
    const ImageArea a = dng_image_area(ifd, image::Size{100, 80});
    ASSERT_TRUE(a.user_crop);
    EXPECT_EQ(*a.user_crop, (NormalizedCrop{0.125, 0.0, 0.875, 1.0}));
    EXPECT_EQ(a.user_crop_provenance, Provenance::DngTag);
    ifd.default_user_crop = std::array<double, 4>{0.0, 0.0, 1.0, 1.0};
    EXPECT_FALSE(dng_image_area(ifd, image::Size{100, 80}).user_crop);
}

// ---- LibRaw inset crops ---------------------------------------------------------------------------

InsetCropFacts facts(InsetCrop crop, image::Size frame, image::PixelRect visible, std::uint32_t period = 2) {
    InsetCropFacts f;
    f.crop = crop;
    f.raw_frame = frame;
    f.visible = visible;
    f.margin_period = period;
    return f;
}

TEST(InsetCrops, ARecordedCropInsideTheFrameIsTheArea) {
    // Sony A7 IV: DefaultCrop 7008 x 4672 at (12, 8) of a 7040 x 4688 frame.
    const ImageArea a = image_area_from_inset_crops(
        facts(InsetCrop{12, 8, 7008, 4672}, image::Size{7040, 4688}, image::PixelRect{0, 0, 7028, 4688}));
    EXPECT_EQ(a.source, ImageAreaSource::DecoderInsetCrop);
    EXPECT_EQ(a.provenance, Provenance::Decoder);
    ASSERT_TRUE(a.rect);
    EXPECT_EQ(*a.rect, (AreaRect{12.0, 8.0, 7008.0, 4672.0}));
    EXPECT_EQ(a.reference, (image::PixelRect{0, 0, 7040, 4688}));
    EXPECT_NE(a.basis.find("raw_inset_crops[0]"), std::string::npos) << a.basis;
}

TEST(InsetCrops, UnrecordedIncompleteOrImplausibleCropsAreUnknown) {
    const image::Size frame{6000, 4000};
    const image::PixelRect visible{0, 0, 6000, 4000};
    // Nothing recorded (LibRaw's initial values).
    const ImageArea none = image_area_from_inset_crops(facts(InsetCrop{}, frame, visible));
    EXPECT_EQ(none.source, ImageAreaSource::Unknown);
    EXPECT_NE(none.basis.find("not set"), std::string::npos) << none.basis;
    // A size without an origin (Sony tag 0xb02b fills only the size).
    const ImageArea size_only =
        image_area_from_inset_crops(facts(InsetCrop{0xffff, 0xffff, 5900, 3900}, frame, visible));
    EXPECT_EQ(size_only.source, ImageAreaSource::Unknown);
    EXPECT_NE(size_only.basis.find("incomplete"), std::string::npos) << size_only.basis;
    // Beyond the raw frame.
    EXPECT_EQ(image_area_from_inset_crops(facts(InsetCrop{200, 0, 5900, 3900}, frame, visible)).source,
              ImageAreaSource::Unknown);
    // Keeping less than kMinInsetCropFraction of the visible area (a misread maker note).
    const ImageArea small = image_area_from_inset_crops(facts(InsetCrop{0, 0, 3000, 3900}, frame, visible));
    EXPECT_EQ(small.source, ImageAreaSource::Unknown);
    EXPECT_NE(small.basis.find("not used"), std::string::npos) << small.basis;
    // Exactly at the bound it is used (LibRaw: cwidth >= int(width * maxcrop)), one pixel less not.
    EXPECT_EQ(image_area_from_inset_crops(facts(InsetCrop{0, 0, 3300, 2200}, frame, visible)).source,
              ImageAreaSource::DecoderInsetCrop);
    EXPECT_EQ(image_area_from_inset_crops(facts(InsetCrop{0, 0, 3299, 2200}, frame, visible)).source,
              ImageAreaSource::Unknown);
    EXPECT_EQ(image_area_from_inset_crops(facts(InsetCrop{0, 0, 3300, 2199}, frame, visible)).source,
              ImageAreaSource::Unknown);
}

TEST(InsetCrops, LibRawsMarginAdjustmentIsReproduced) {
    // src/utils/open.cpp: odd (Bayer) or non-multiple-of-6 (X-Trans) left/top edges move inward.
    EXPECT_EQ(detail::libraw_adjusted_inset_crop(InsetCrop{13, 7, 100, 50}, 2).left, 14u);
    const InsetCrop bayer = detail::libraw_adjusted_inset_crop(InsetCrop{13, 7, 100, 50}, 2);
    EXPECT_EQ(bayer.width, 99u);
    EXPECT_EQ(bayer.top, 8u);
    EXPECT_EQ(bayer.height, 49u);
    const InsetCrop xtrans = detail::libraw_adjusted_inset_crop(InsetCrop{6, 13, 6240, 4160}, 6);
    EXPECT_EQ(xtrans.left, 6u);
    EXPECT_EQ(xtrans.width, 6240u);
    EXPECT_EQ(xtrans.top, 18u);
    EXPECT_EQ(xtrans.height, 4155u);
    // An edge at 0, an unrecorded edge or period 0 stays.
    const InsetCrop zero = detail::libraw_adjusted_inset_crop(InsetCrop{0, 0xffff, 100, 50}, 6);
    EXPECT_EQ(zero.left, 0u);
    EXPECT_EQ(zero.top, 0xffffu);
    EXPECT_EQ(detail::libraw_adjusted_inset_crop(InsetCrop{13, 7, 100, 50}, 0).left, 13u);
}

TEST(InsetCrops, TheFilesOwnDeclarationRestoresEdgesLibRawMoved) {
    // Fujifilm X-T4: RawImageCropTopLeft (13, 6) and RawImageCroppedSize 6240 x 4160; LibRaw reports
    // top 18 and height 4155 after moving the top edge to the X-Trans period.
    InsetCropFacts f =
        facts(InsetCrop{6, 18, 6240, 4155}, image::Size{6384, 4182}, image::PixelRect{0, 6, 6246, 4170}, 6);
    f.declared = InsetCrop{6, 13, 6240, 4160};
    f.declared_name = "RAF RawImageCropTopLeft / RawImageCroppedSize";
    const ImageArea a = image_area_from_inset_crops(f);
    EXPECT_EQ(a.source, ImageAreaSource::DecoderInsetCrop);
    EXPECT_EQ(a.provenance, Provenance::LucentRule);
    ASSERT_TRUE(a.rect);
    EXPECT_EQ(*a.rect, (AreaRect{6.0, 13.0, 6240.0, 4160.0}));
    EXPECT_NE(a.basis.find("moved to its CFA period of 6"), std::string::npos) << a.basis;

    // A declaration that is not LibRaw's adjusted form of its crop is not used.
    f.declared = InsetCrop{0, 0, 6000, 4000};
    const ImageArea b = image_area_from_inset_crops(f);
    EXPECT_EQ(b.provenance, Provenance::Decoder);
    EXPECT_EQ(*b.rect, (AreaRect{6.0, 18.0, 6240.0, 4155.0}));
    EXPECT_NE(b.basis.find("differs"), std::string::npos) << b.basis;

    // Equal declarations: LibRaw's values, noted.
    f.crop = InsetCrop{12, 8, 8256, 6192};
    f.declared = f.crop;
    f.raw_frame = image::Size{8472, 6210};
    f.visible = image::PixelRect{0, 0, 8280, 6208};
    f.margin_period = 2;
    const ImageArea c = image_area_from_inset_crops(f);
    EXPECT_EQ(c.provenance, Provenance::Decoder);
    EXPECT_NE(c.basis.find("equal to the file's"), std::string::npos) << c.basis;
}

TEST(InsetCrops, TheAspectRatioCropBecomesTheUserCrop) {
    InsetCropFacts f =
        facts(InsetCrop{0, 0, 6000, 4000}, image::Size{6000, 4000}, image::PixelRect{0, 0, 6000, 4000});
    f.user_crop = InsetCrop{0, 312, 6000, 3376};  // 16:9 inside 3:2, centred
    const ImageArea a = image_area_from_inset_crops(f);
    ASSERT_TRUE(a.user_crop);
    EXPECT_DOUBLE_EQ(a.user_crop->top, 312.0 / 4000.0);
    EXPECT_DOUBLE_EQ(a.user_crop->bottom, 3688.0 / 4000.0);
    EXPECT_DOUBLE_EQ(a.user_crop->left, 0.0);
    EXPECT_DOUBLE_EQ(a.user_crop->right, 1.0);
    EXPECT_EQ(a.user_crop_provenance, Provenance::Decoder);
    // A user crop outside the area, or the whole area, is not carried.
    f.user_crop = InsetCrop{0, 0, 6000, 4000};
    EXPECT_FALSE(image_area_from_inset_crops(f).user_crop);
}

// QUA-01: Canon's in-camera aspect-ratio setting. LibRaw 0.22 puts the AspectInfo rectangle (plus the
// SensorInfo origin) in raw_inset_crops[0]; the camera records the whole 3:2 image and SensorInfo's
// DefaultCropAbsolute is its area. A Canon EOS R5 set to 1:1: SensorInfo (144, 112) - (8335, 5575),
// AspectInfo 5464 x 5464 at (1364, 0) of it, so raw_inset_crops[0] = 5464 x 5464 at (1508, 112).
detail::CanonCropFacts canon_r5(std::uint32_t raw_aspect) {
    detail::CanonCropFacts canon;
    canon.canon_non_dng = true;
    canon.raw_aspect = raw_aspect;
    canon.sensor_left = 144;
    canon.sensor_top = 112;
    canon.sensor_right = 8335;
    canon.sensor_bottom = 5575;
    return canon;
}

InsetCropFacts r5_facts(InsetCrop crop) {
    // LibRaw's visible area of the R5 (identify.cpp: SensorInfo with the margins rounded to 2).
    return facts(crop, image::Size{8352, 5586}, image::PixelRect{144, 112, 8191, 5463});
}

TEST(InsetCrops, ACanonAspectRatioSettingIsTheUserCropOfTheWholeSensorArea) {
    InsetCropFacts f = r5_facts(InsetCrop{1508, 112, 5464, 5464});
    detail::canon_aspect_crops(f, canon_r5(1000));  // LIBRAW_IMAGE_ASPECT_1to1
    const ImageArea a = image_area_from_inset_crops(f);
    EXPECT_EQ(a.source, ImageAreaSource::DecoderInsetCrop);
    ASSERT_TRUE(a.rect);
    EXPECT_EQ(*a.rect, (AreaRect{144.0, 112.0, 8192.0, 5464.0})) << "the area is the whole 3:2 image";
    ASSERT_TRUE(a.user_crop) << "the 1:1 setting is exposed as the user crop";
    EXPECT_DOUBLE_EQ(a.user_crop->left, 1364.0 / 8192.0);
    EXPECT_DOUBLE_EQ(a.user_crop->right, (1364.0 + 5464.0) / 8192.0);
    EXPECT_DOUBLE_EQ(a.user_crop->top, 0.0);
    EXPECT_DOUBLE_EQ(a.user_crop->bottom, 1.0);
    EXPECT_NE(a.basis.find("SensorInfo"), std::string::npos) << a.basis;
    EXPECT_NE(a.basis.find("AspectInfo 1:1"), std::string::npos) << a.basis;

    // 16:9 (8192 x 4608 centred) and an aspect value LibRaw does not know (LIBRAW_IMAGE_ASPECT_OTHER)
    // are user crops too.
    InsetCropFacts wide = r5_facts(InsetCrop{144, 540, 8192, 4608});
    detail::canon_aspect_crops(wide, canon_r5((1000 * 16) / 9));
    const ImageArea w = image_area_from_inset_crops(wide);
    ASSERT_TRUE(w.rect);
    EXPECT_EQ(*w.rect, (AreaRect{144.0, 112.0, 8192.0, 5464.0}));
    ASSERT_TRUE(w.user_crop);
    EXPECT_DOUBLE_EQ(w.user_crop->top, 428.0 / 5464.0);
    InsetCropFacts other = r5_facts(InsetCrop{1508, 112, 5464, 5464});
    detail::canon_aspect_crops(other, canon_r5(1));
    EXPECT_EQ(*image_area_from_inset_crops(other).rect, (AreaRect{144.0, 112.0, 8192.0, 5464.0}));
}

// Contract 1.5: Canon's crop modes. The corpus's R5 crop files record AspectInfo [13, 5088, 3392, 0,
// 0] ("3:2 (APS-C crop)") with a visible area of 5087 x 3391; its full-frame files [0, 8192, 5464, 0,
// 0] with 8191 x 5463 (read with research's maker-note reader, 2026-10-10).
TEST(SensorCrops, CanonCropModesAreSensorCropsWithCanonsFactor) {
    const image::PixelRect crop_visible{144, 108, 5087, 3391};
    const auto aps_c = detail::canon_sensor_crop(true, detail::CanonAspectInfo{13, 5088, 3392, 0, 0}, crop_visible);
    ASSERT_TRUE(aps_c);
    ASSERT_TRUE(aps_c->factor);
    EXPECT_DOUBLE_EQ(*aps_c->factor, 1.6);
    EXPECT_EQ(aps_c->provenance, Provenance::LucentRule);
    EXPECT_NE(aps_c->basis.find("APS-C crop"), std::string::npos) << aps_c->basis;
    EXPECT_NE(aps_c->basis.find("0x009a"), std::string::npos) << aps_c->basis;
    const auto aps_h = detail::canon_sensor_crop(true, detail::CanonAspectInfo{12, 6400, 4266, 0, 0},
                                                 image::PixelRect{0, 0, 6397, 4265});
    ASSERT_TRUE(aps_h);
    EXPECT_DOUBLE_EQ(*aps_h->factor, 1.3);
}

TEST(SensorCrops, WholeSensorImagesAndOtherFilesAreNoSensorCrop) {
    const image::PixelRect full_visible{144, 112, 8191, 5463};
    // A 3:2 (or any other plain aspect-ratio) setting.
    EXPECT_FALSE(detail::canon_sensor_crop(true, detail::CanonAspectInfo{0, 8192, 5464, 0, 0}, full_visible));
    EXPECT_FALSE(detail::canon_sensor_crop(true, detail::CanonAspectInfo{1, 5464, 5464, 1364, 0}, full_visible));
    // A crop code on a whole-sensor image (the crop only attached): the image covers the sensor.
    EXPECT_FALSE(detail::canon_sensor_crop(true, detail::CanonAspectInfo{13, 5088, 3392, 1552, 1036}, full_visible));
    // No AspectInfo, not Canon or a DNG.
    EXPECT_FALSE(detail::canon_sensor_crop(true, std::nullopt, full_visible));
    EXPECT_FALSE(detail::canon_sensor_crop(false, detail::CanonAspectInfo{13, 5088, 3392, 0, 0},
                                           image::PixelRect{0, 0, 5087, 3391}));
}

TEST(InsetCrops, CanonThreeToTwoFilesAndOtherMakersKeepLibRawsCrop) {
    // 3:2 (the corpus R5 files): AspectInfo is the whole area, nothing changes.
    InsetCropFacts f = r5_facts(InsetCrop{144, 112, 8192, 5464});
    detail::canon_aspect_crops(f, canon_r5(1500));
    EXPECT_EQ(f.crop.left, 144u);
    EXPECT_EQ(f.user_crop.left, 0xffffu);
    EXPECT_TRUE(f.crop_name.empty());
    // No AspectInfo (LIBRAW_IMAGE_ASPECT_UNKNOWN): LibRaw's crop is SensorInfo already.
    InsetCropFacts unknown = r5_facts(InsetCrop{144, 112, 8192, 5464});
    detail::canon_aspect_crops(unknown, canon_r5(0));
    EXPECT_TRUE(unknown.crop_name.empty());
    // Not Canon, or a DNG: unchanged.
    InsetCropFacts nikon = r5_facts(InsetCrop{1508, 112, 5464, 5464});
    detail::CanonCropFacts not_canon = canon_r5(1000);
    not_canon.canon_non_dng = false;
    detail::canon_aspect_crops(nikon, not_canon);
    EXPECT_EQ(nikon.crop.left, 1508u);
    // SensorInfo not parsed, or outside the raw frame: the aspect crop cannot be told apart.
    InsetCropFacts no_sensor = r5_facts(InsetCrop{1508, 112, 5464, 5464});
    detail::CanonCropFacts missing = canon_r5(1000);
    missing.sensor_left = -1;
    detail::canon_aspect_crops(no_sensor, missing);
    EXPECT_EQ(no_sensor.crop.left, 1508u);
    InsetCropFacts beyond = r5_facts(InsetCrop{1508, 112, 5464, 5464});
    detail::CanonCropFacts huge = canon_r5(1000);
    huge.sensor_right = 9000;
    detail::canon_aspect_crops(beyond, huge);
    EXPECT_EQ(beyond.crop.left, 1508u);
}

// ---- RAF crop reader ------------------------------------------------------------------------------

std::vector<std::byte> raf_bytes(const std::vector<std::array<std::uint16_t, 4>>& entries) {
    // Header (108 bytes) with the CFA header (metadata directory) range at byte 92.
    std::vector<std::byte> out(108, std::byte{0});
    std::memcpy(out.data(), "FUJIFILMCCD-RAW ", 16);
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out[at + static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (24 - 8 * i)) & 0xff);
        }
    };
    const auto push16 = [&](std::uint16_t v) {
        out.push_back(static_cast<std::byte>(v >> 8));
        out.push_back(static_cast<std::byte>(v & 0xff));
    };
    const std::size_t dir = out.size();
    out.resize(out.size() + 4);
    put32(dir, static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
        push16(e[0]);  // tag
        push16(4);     // size
        push16(e[1]);
        push16(e[2]);
    }
    put32(92, static_cast<std::uint32_t>(dir));
    put32(96, static_cast<std::uint32_t>(out.size() - dir));
    return out;
}

TEST(RafCrop, ReadsTheCropTagsOfTheMetadataDirectory) {
    const std::vector<std::byte> bytes = raf_bytes(
        {{0x100, 4182, 6384, 0}, {0x110, 13, 6, 0}, {0x111, 4160, 6240, 0}, {0x121, 4170, 6246, 0}});
    MemorySource source(bytes);
    auto crop = detail::read_raf_crop(source);
    ASSERT_TRUE(crop) << crop.error().describe();
    ASSERT_TRUE(*crop);
    EXPECT_EQ((*crop)->top, 13u);
    EXPECT_EQ((*crop)->left, 6u);
    EXPECT_EQ((*crop)->height, 4160u);
    EXPECT_EQ((*crop)->width, 6240u);

    // A missing tag, another signature or a directory beyond the file give nothing.
    const std::vector<std::byte> half = raf_bytes({{0x110, 13, 6, 0}});
    MemorySource half_source(half);
    auto none = detail::read_raf_crop(half_source);
    ASSERT_TRUE(none);
    EXPECT_FALSE(*none);
    std::vector<std::byte> other = bytes;
    other[0] = std::byte{'X'};
    MemorySource other_source(other);
    EXPECT_FALSE(*detail::read_raf_crop(other_source));
    std::vector<std::byte> cut(bytes.begin(), bytes.begin() + 120);
    MemorySource cut_source(cut);
    auto truncated = detail::read_raf_crop(cut_source);
    ASSERT_TRUE(truncated);
    EXPECT_FALSE(*truncated);
}

// ---- Inspector: DefaultUserCrop -------------------------------------------------------------------

class ImageAreaAdapter : public ::testing::Test {
protected:
    void SetUp() override {
        auto decoder = create_libraw_decoder();
        ASSERT_TRUE(decoder);
        decoder_ = std::move(*decoder);
    }

    DecodedRaw decode(const ts::SyntheticDngSpec& spec, const wchar_t* name) {
        const auto path = dir_.write(name, ts::build_synthetic_dng(spec));
        auto decoded = decoder_->decode(path, {}, {});
        EXPECT_TRUE(decoded) << decoded.error().describe();
        return std::move(*decoded);
    }

    std::unique_ptr<RawDecoder> decoder_;
    ts::ScratchDir dir_;
};

TEST_F(ImageAreaAdapter, DngDefaultCropWithOddActiveAreaMargins) {
    ts::SyntheticDngSpec spec;
    spec.active_area = std::array<std::uint32_t, 4>{3, 5, 45, 61};  // top, left, bottom, right
    spec.default_crop_origin = std::array<std::pair<std::uint32_t, std::uint32_t>, 2>{{{2, 1}, {1, 1}}};
    spec.default_crop_size = std::array<std::pair<std::uint32_t, std::uint32_t>, 2>{{{50, 1}, {38, 1}}};
    spec.default_user_crop =
        std::array<std::pair<std::uint32_t, std::uint32_t>, 4>{{{1, 8}, {0, 1}, {7, 8}, {1, 1}}};
    const DecodedRaw d = decode(spec, L"crop.dng");
    const RawMetadata& m = d.metadata;
    ASSERT_TRUE(check_consistency(d));
    // LibRaw starts the visible area at (6, 4), one row and column inside the ActiveArea (5, 3).
    EXPECT_EQ(m.geometry.visible_area, (image::PixelRect{6, 4, 55, 41}));
    EXPECT_EQ(m.image_area.source, ImageAreaSource::DngDefaultCrop);
    ASSERT_TRUE(m.image_area.rect);
    EXPECT_EQ(*m.image_area.rect, (AreaRect{7.0, 4.0, 50.0, 38.0}));
    EXPECT_EQ(m.image_area.reference, (image::PixelRect{5, 3, 56, 42}));
    ASSERT_TRUE(m.image_area.user_crop);
    EXPECT_EQ(*m.image_area.user_crop, (NormalizedCrop{0.125, 0.0, 0.875, 1.0}));
    ASSERT_TRUE(m.dng);
    EXPECT_EQ(m.dng->raw_ifd.default_user_crop, (std::array<double, 4>{0.125, 0.0, 0.875, 1.0}));

    // The probe reports the same area.
    const auto path = dir_.path() / L"crop.dng";
    auto probe = decoder_->probe(path, {}, {});
    ASSERT_TRUE(probe);
    EXPECT_EQ(probe->metadata.image_area.rect, m.image_area.rect);
}

TEST_F(ImageAreaAdapter, AMalformedDefaultUserCropIsSkippedWithAWarning) {
    ts::SyntheticDngSpec spec;
    spec.default_crop_origin = std::array<std::pair<std::uint32_t, std::uint32_t>, 2>{{{0, 1}, {0, 1}}};
    spec.default_crop_size = std::array<std::pair<std::uint32_t, std::uint32_t>, 2>{{{64, 1}, {48, 1}}};
    // bottom above top
    spec.default_user_crop =
        std::array<std::pair<std::uint32_t, std::uint32_t>, 4>{{{3, 4}, {0, 1}, {1, 4}, {1, 1}}};
    const DecodedRaw d = decode(spec, L"bad.dng");
    const RawMetadata& m = d.metadata;
    EXPECT_EQ(m.interpretation.status, InterpretationStatus::Complete);
    EXPECT_EQ(m.image_area.source, ImageAreaSource::DngDefaultCrop);
    EXPECT_FALSE(m.image_area.user_crop);
    ASSERT_TRUE(m.dng);
    EXPECT_FALSE(m.dng->raw_ifd.default_user_crop);
    bool warned = false;
    for (const TiffWarning& w : m.dng->warnings) {
        warned = warned || w.message.find("DefaultUserCrop") != std::string::npos;
    }
    EXPECT_TRUE(warned);
}

TEST_F(ImageAreaAdapter, ADngWithoutDefaultCropHasTheWholeActiveArea) {
    ts::SyntheticDngSpec spec;
    const DecodedRaw d = decode(spec, L"plain.dng");
    EXPECT_EQ(d.metadata.image_area.source, ImageAreaSource::DngDefaultWholeImage);
    ASSERT_TRUE(d.metadata.image_area.rect);
    EXPECT_EQ(*d.metadata.image_area.rect, (AreaRect{0.0, 0.0, 64.0, 48.0}));
}

// ---- Contract ------------------------------------------------------------------------------------

TEST(ImageAreaContract, ConsistencyRules) {
    EXPECT_EQ(kDecodedRawContractVersion.major, 1u);
    EXPECT_GE(kDecodedRawContractVersion.minor, 4u);  // the image area since 1.4
    EXPECT_EQ(to_string(ImageAreaSource::Unknown), "unknown");
    EXPECT_EQ(to_string(ImageAreaSource::DngDefaultCrop), "dng_default_crop");
    EXPECT_EQ(to_string(ImageAreaSource::DngDefaultWholeImage), "dng_default_whole_image");
    EXPECT_EQ(to_string(ImageAreaSource::DecoderInsetCrop), "decoder_inset_crop");
    EXPECT_EQ(to_string(static_cast<ImageAreaSource>(99)), "invalid");
}

}  // namespace
}  // namespace lucent::raw
