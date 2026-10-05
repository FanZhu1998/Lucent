#pragma once

// Test-only builder of small, complete, uncompressed DNG files that LibRaw decodes, so that the
// LibRaw adapter can be tested end to end without downloaded camera files.
//
// The raw image lives in IFD 0 (allowed by DNG), stored as one uncompressed strip. Sample values
// are a deterministic function of position, channel and image (expected_sample), so tests can
// check that every decoded sample is exactly the stored one. With raw_images = 2, a second,
// different raw image of the same kind and size is stored in a SubIFD of IFD 0; both have
// NewSubFileType 0, so LibRaw reports two images (raw_count 2) and Lucent's DNG inspector finds
// two raw-IFD candidates. With raw_in_subifd, IFD 0 is instead a small RGB preview
// (NewSubFileType 1) and the only raw image is SubIFD 0, the layout most cameras write.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lucent/raw/dng_tags.hpp>

#include "tiff_test_writer.hpp"

namespace lucent::raw::test_support {

enum class SyntheticKind {
    Cfa,        // PhotometricInterpretation CFA, 1 sample per pixel
    LinearRgb,  // LinearRaw, 3 samples per pixel (already demosaiced)
    Mono,       // LinearRaw, 1 sample per pixel (monochrome sensor)
};

// A JPEG stream to embed as a preview IFD (see SyntheticDngSpec::jpeg_previews). The declared
// ImageWidth / ImageLength need not match the stream (tests use that to check that the stream's
// own frame header is authoritative).
struct SyntheticJpegPreview {
    std::vector<std::byte> bytes;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t components = 3;
};

struct SyntheticDngSpec {
    std::uint32_t width = 64;  // LibRaw rejects images smaller than 22 x 22
    std::uint32_t height = 48;
    SyntheticKind kind = SyntheticKind::Cfa;
    bool float_samples = false;  // SampleFormat 3, 32-bit IEEE float
    Endian endian = Endian::Little;
    std::array<std::uint8_t, 4> cfa_pattern{0, 1, 1, 2};      // 2x2 CFAPattern, row-major (RGGB)
    std::optional<std::array<std::uint32_t, 4>> active_area;  // top, left, bottom, right
    std::uint16_t orientation = 1;
    std::uint32_t white_level = 16383;
    std::uint32_t black_level = 256;  // BlackLevel (1x1 repeat) as a RATIONAL n/1
    std::array<std::uint8_t, 4> dng_version{1, 4, 0, 0};
    std::optional<std::array<std::uint8_t, 4>> backward_version;
    std::vector<std::byte> opcode_list1;
    std::vector<std::byte> opcode_list2;
    std::vector<std::byte> opcode_list3;
    std::string make = "Lucent";
    std::string model = "Synthetic DNG";
    std::uint32_t raw_images = 1;   // 1, or 2 for a second raw image in a SubIFD of IFD 0
    bool raw_in_subifd = false;     // IFD 0 is an 8x8 RGB preview, the raw image is SubIFD 0
    bool write_white_level = true;  // false omits WhiteLevel (the DNG default then applies)
    // BlackLevelDeltaH / BlackLevelDeltaV as SRATIONAL numerator/denominator pairs; empty omits.
    std::vector<std::pair<std::int32_t, std::int32_t>> black_level_delta_h;
    std::vector<std::pair<std::int32_t, std::int32_t>> black_level_delta_v;
    // NoiseProfile (DOUBLE pairs) in the raw IFD and in IFD 0; empty omits the tag.
    std::vector<double> raw_noise_profile;
    std::vector<double> ifd0_noise_profile;
    // Embedded JPEG previews, each written as a further SubIFD of IFD 0 (after any raw SubIFD) in
    // this order: NewSubFileType 1, Compression 7, YCbCr (or BlackIsZero for 1 component), one
    // strip holding the stream. Used by the embedded-preview tests.
    std::vector<SyntheticJpegPreview> jpeg_previews;
    // A scene instead of expected_sample() (16-bit integer samples only): the stored value of pixel
    // (x, y) for `color`, the CFA colour at (x, y) for Cfa data (cfa_pattern: 0 red, 1 green, 2
    // blue) and the sample's channel otherwise. Every raw image stores it.
    std::function<std::uint16_t(std::uint32_t x, std::uint32_t y, std::uint32_t color)> scene;
};

// Stored value of sample (x, y, channel) of raw image `image`: varied, within 12 bits, never 0.
inline std::uint16_t expected_sample(std::uint32_t x, std::uint32_t y, std::uint32_t channel,
                                     std::uint32_t image = 0) {
    return static_cast<std::uint16_t>(300 + (x * 37 + y * 101 + (channel + 3 * image) * 1009 + x * y) % 3700);
}

// Float counterpart: expected_sample scaled into [0, 1].
inline float expected_float_sample(std::uint32_t x, std::uint32_t y, std::uint32_t channel,
                                   std::uint32_t image = 0) {
    return static_cast<float>(expected_sample(x, y, channel, image)) / 4096.0f;
}

inline std::uint32_t samples_per_pixel(const SyntheticDngSpec& spec) {
    return spec.kind == SyntheticKind::LinearRgb ? 3u : 1u;
}

// Builds the DNG file bytes.
inline std::vector<std::byte> build_synthetic_dng(const SyntheticDngSpec& spec) {
    namespace t = tiff_tag;
    constexpr std::uint16_t kPixelDataTag = 65000;  // private tag holding the strip bytes

    const std::uint32_t spp = samples_per_pixel(spec);
    const std::uint32_t bits = spec.float_samples ? 32 : 16;
    const std::size_t bytes_per_sample = bits / 8;

    // Pixel data of raw image `image` in the file's byte order.
    const auto pixel_data = [&](std::uint32_t image) {
        std::vector<std::byte> data;
        data.reserve(std::size_t{spec.width} * spec.height * spp * bytes_per_sample);
        const auto push = [&](std::uint64_t value, std::size_t size) {
            for (std::size_t i = 0; i < size; ++i) {
                const std::size_t shift = spec.endian == Endian::Little ? 8 * i : 8 * (size - 1 - i);
                data.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
            }
        };
        for (std::uint32_t y = 0; y < spec.height; ++y) {
            for (std::uint32_t x = 0; x < spec.width; ++x) {
                for (std::uint32_t c = 0; c < spp; ++c) {
                    if (spec.scene && !spec.float_samples) {
                        const std::uint32_t color =
                            spec.kind == SyntheticKind::Cfa ? spec.cfa_pattern[(y % 2) * 2 + x % 2] : c;
                        push(spec.scene(x, y, color), 2);
                    } else if (spec.float_samples) {
                        push(std::bit_cast<std::uint32_t>(expected_float_sample(x, y, c, image)), 4);
                    } else {
                        push(expected_sample(x, y, c, image), 2);
                    }
                }
            }
        }
        return data;
    };

    // Tags describing one raw image: IFD 0, and the SubIFD holding a second image.
    const auto image_entries = [&](const std::vector<std::byte>& data) {
        std::vector<Entry> entries = {
            long_entry(t::kNewSubFileType, {0}),
            long_entry(t::kImageWidth, {spec.width}),
            long_entry(t::kImageLength, {spec.height}),
            short_entry(t::kBitsPerSample, std::vector<std::uint64_t>(spp, bits)),
            short_entry(t::kCompression, {1}),
            short_entry(t::kPhotometricInterpretation, {spec.kind == SyntheticKind::Cfa ? 32803u : 34892u}),
            long_entry(t::kStripOffsets, {0}),  // patched below
            short_entry(t::kSamplesPerPixel, {spp}),
            long_entry(t::kRowsPerStrip, {spec.height}),
            long_entry(t::kStripByteCounts, {data.size()}),
            short_entry(t::kPlanarConfiguration, {1}),
            short_entry(t::kSampleFormat, std::vector<std::uint64_t>(spp, spec.float_samples ? 3u : 1u)),
            undefined_entry(kPixelDataTag, data),
        };
        if (spec.float_samples) {
            entries.push_back(rational_entry(t::kBlackLevel, {{0, 1}}));
            if (spec.write_white_level) {
                entries.push_back(long_entry(t::kWhiteLevel, {1}));
            }
        } else {
            entries.push_back(rational_entry(t::kBlackLevel, {{spec.black_level, 1}}));
            if (spec.write_white_level) {
                entries.push_back(
                    long_entry(t::kWhiteLevel, std::vector<std::uint64_t>(spp, spec.white_level)));
            }
        }
        if (!spec.black_level_delta_h.empty()) {
            entries.push_back(srational_entry(t::kBlackLevelDeltaH, spec.black_level_delta_h));
        }
        if (!spec.black_level_delta_v.empty()) {
            entries.push_back(srational_entry(t::kBlackLevelDeltaV, spec.black_level_delta_v));
        }
        if (!spec.raw_noise_profile.empty()) {
            entries.push_back(double_entry(t::kNoiseProfile, spec.raw_noise_profile));
        }
        if (spec.kind == SyntheticKind::Cfa) {
            entries.push_back(short_entry(t::kCfaRepeatPatternDim, {2, 2}));
            entries.push_back(byte_entry(t::kCfaPattern, {spec.cfa_pattern[0], spec.cfa_pattern[1],
                                                          spec.cfa_pattern[2], spec.cfa_pattern[3]}));
            entries.push_back(byte_entry(t::kCfaPlaneColor, {0, 1, 2}));
            entries.push_back(short_entry(t::kCfaLayout, {1}));
        }
        if (spec.active_area) {
            const auto& a = *spec.active_area;
            entries.push_back(long_entry(t::kActiveArea, {a[0], a[1], a[2], a[3]}));
        }
        return entries;
    };

    Ifd ifd0;
    if (spec.raw_in_subifd) {
        // An 8x8 RGB preview: 192 bytes of 8-bit samples.
        std::vector<std::byte> preview(8 * 8 * 3, std::byte{0x40});
        ifd0.entries = {
            long_entry(t::kNewSubFileType, {1}),
            long_entry(t::kImageWidth, {8}),
            long_entry(t::kImageLength, {8}),
            short_entry(t::kBitsPerSample, {8, 8, 8}),
            short_entry(t::kCompression, {1}),
            short_entry(t::kPhotometricInterpretation, {2}),
            long_entry(t::kStripOffsets, {0}),  // patched below
            short_entry(t::kSamplesPerPixel, {3}),
            long_entry(t::kRowsPerStrip, {8}),
            long_entry(t::kStripByteCounts, {preview.size()}),
            short_entry(t::kPlanarConfiguration, {1}),
            undefined_entry(kPixelDataTag, preview),
        };
        Ifd raw;
        raw.entries = image_entries(pixel_data(0));
        ifd0.sub_ifds = {raw};
    } else {
        ifd0.entries = image_entries(pixel_data(0));
    }
    if (!spec.ifd0_noise_profile.empty()) {
        ifd0.entries.push_back(double_entry(t::kNoiseProfile, spec.ifd0_noise_profile));
    }
    const std::vector<Entry> camera_entries = {
        ascii_entry(t::kMake, spec.make),
        ascii_entry(t::kModel, spec.model),
        short_entry(t::kOrientation, {spec.orientation}),
        byte_entry(t::kDngVersion,
                   {spec.dng_version[0], spec.dng_version[1], spec.dng_version[2], spec.dng_version[3]}),
        ascii_entry(t::kUniqueCameraModel, spec.make + " " + spec.model),
        srational_entry(t::kColorMatrix1, {{8000, 10000},
                                           {-2000, 10000},
                                           {-1000, 10000},
                                           {-4000, 10000},
                                           {12000, 10000},
                                           {1500, 10000},
                                           {-500, 10000},
                                           {1800, 10000},
                                           {5500, 10000}}),
        short_entry(t::kCalibrationIlluminant1, {21}),
        rational_entry(t::kAsShotNeutral, {{1, 2}, {1, 1}, {2, 3}}),
    };
    ifd0.entries.insert(ifd0.entries.end(), camera_entries.begin(), camera_entries.end());
    if (spec.backward_version) {
        const auto& v = *spec.backward_version;
        ifd0.entries.push_back(byte_entry(t::kDngBackwardVersion, {v[0], v[1], v[2], v[3]}));
    }
    if (!spec.opcode_list1.empty()) {
        ifd0.entries.push_back(undefined_entry(t::kOpcodeList1, spec.opcode_list1));
    }
    if (!spec.opcode_list2.empty()) {
        ifd0.entries.push_back(undefined_entry(t::kOpcodeList2, spec.opcode_list2));
    }
    if (!spec.opcode_list3.empty()) {
        ifd0.entries.push_back(undefined_entry(t::kOpcodeList3, spec.opcode_list3));
    }

    if (spec.raw_images == 2 && !spec.raw_in_subifd) {
        Ifd second;
        second.entries = image_entries(pixel_data(1));
        ifd0.sub_ifds = {second};
    }

    for (const SyntheticJpegPreview& jpeg : spec.jpeg_previews) {
        Ifd preview;
        const std::uint64_t components = jpeg.components;
        preview.entries = {
            long_entry(t::kNewSubFileType, {1}),
            long_entry(t::kImageWidth, {jpeg.width}),
            long_entry(t::kImageLength, {jpeg.height}),
            short_entry(t::kBitsPerSample, std::vector<std::uint64_t>(components, 8)),
            short_entry(t::kCompression, {7}),
            short_entry(t::kPhotometricInterpretation, {components == 3 ? 6u : 1u}),
            long_entry(t::kStripOffsets, {0}),  // patched below
            short_entry(t::kSamplesPerPixel, {components}),
            long_entry(t::kRowsPerStrip, {jpeg.height}),
            long_entry(t::kStripByteCounts, {jpeg.bytes.size()}),
            short_entry(t::kPlanarConfiguration, {1}),
            undefined_entry(kPixelDataTag, jpeg.bytes),
        };
        ifd0.sub_ifds.push_back(std::move(preview));
    }

    Document doc;
    doc.endian = spec.endian;
    doc.chain = {ifd0};
    Built built = build(doc);
    // StripOffsets holds one LONG, stored inline in its entry.
    built.put(built.value("ifd0", t::kStripOffsets), built.value("ifd0", kPixelDataTag), 4);
    for (std::size_t i = 0; i < ifd0.sub_ifds.size(); ++i) {
        const std::string path = "ifd0.subifd" + std::to_string(i);
        built.put(built.value(path, t::kStripOffsets), built.value(path, kPixelDataTag), 4);
    }
    return std::move(built.bytes);
}

}  // namespace lucent::raw::test_support
