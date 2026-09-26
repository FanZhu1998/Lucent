#include "float_tiff.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace lucent::render_tool {

static_assert(std::endian::native == std::endian::little,
              "the float TIFF writer assumes a little-endian host");

namespace {

constexpr const char* kWhere = "lucent-render.tiff";

constexpr std::uint16_t kTypeAscii = 2;
constexpr std::uint16_t kTypeShort = 3;
constexpr std::uint16_t kTypeLong = 4;

constexpr std::uint16_t kTagWidth = 256;
constexpr std::uint16_t kTagLength = 257;
constexpr std::uint16_t kTagBitsPerSample = 258;
constexpr std::uint16_t kTagCompression = 259;
constexpr std::uint16_t kTagPhotometric = 262;
constexpr std::uint16_t kTagDescription = 270;
constexpr std::uint16_t kTagStripOffsets = 273;
constexpr std::uint16_t kTagSamplesPerPixel = 277;
constexpr std::uint16_t kTagRowsPerStrip = 278;
constexpr std::uint16_t kTagStripByteCounts = 279;
constexpr std::uint16_t kTagPlanar = 284;
constexpr std::uint16_t kTagSoftware = 305;
constexpr std::uint16_t kTagSampleFormat = 339;

struct Entry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::vector<std::uint8_t> value;  // little-endian bytes
    std::uint32_t offset = 0;         // where value lives when it does not fit in 4 bytes
};

void append16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}

void append32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

Entry shorts(std::uint16_t tag, std::initializer_list<std::uint16_t> values) {
    Entry e{tag, kTypeShort, static_cast<std::uint32_t>(values.size()), {}, 0};
    for (const std::uint16_t v : values) {
        append16(e.value, v);
    }
    return e;
}

Entry longs(std::uint16_t tag, const std::vector<std::uint32_t>& values) {
    Entry e{tag, kTypeLong, static_cast<std::uint32_t>(values.size()), {}, 0};
    for (const std::uint32_t v : values) {
        append32(e.value, v);
    }
    return e;
}

Entry ascii(std::uint16_t tag, std::string_view text) {
    Entry e{tag, kTypeAscii, static_cast<std::uint32_t>(text.size() + 1), {}, 0};
    for (const char c : text) {
        e.value.push_back(static_cast<std::uint8_t>(c == '\0' ? ' ' : c));
    }
    e.value.push_back(0);
    return e;
}

std::uint16_t read16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t read32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

Status write_float_tiff(const std::filesystem::path& path, image::ConstImageView<float> rgb,
                        std::string_view description) {
    if (rgb.empty() || rgb.channels() != 3) {
        return fail(ErrorCode::InvalidArgument, "the float TIFF writer needs a non-empty 3-channel image",
                    kWhere);
    }
    const auto width = static_cast<std::uint64_t>(rgb.width());
    const auto height = static_cast<std::uint64_t>(rgb.height());
    const std::uint64_t row_bytes = width * 3 * sizeof(float);
    const auto strips = static_cast<std::uint32_t>((height + kRowsPerStrip - 1) / kRowsPerStrip);

    std::vector<Entry> entries;
    entries.push_back(longs(kTagWidth, {static_cast<std::uint32_t>(width)}));
    entries.push_back(longs(kTagLength, {static_cast<std::uint32_t>(height)}));
    entries.push_back(shorts(kTagBitsPerSample, {32, 32, 32}));
    entries.push_back(shorts(kTagCompression, {1}));
    entries.push_back(shorts(kTagPhotometric, {2}));
    entries.push_back(ascii(kTagDescription, description));
    entries.push_back(longs(kTagStripOffsets, std::vector<std::uint32_t>(strips, 0)));
    entries.push_back(shorts(kTagSamplesPerPixel, {3}));
    entries.push_back(longs(kTagRowsPerStrip, {static_cast<std::uint32_t>(kRowsPerStrip)}));
    entries.push_back(longs(kTagStripByteCounts, std::vector<std::uint32_t>(strips, 0)));
    entries.push_back(shorts(kTagPlanar, {1}));
    entries.push_back(ascii(kTagSoftware, "lucent-render"));
    entries.push_back(shorts(kTagSampleFormat, {3, 3, 3}));

    // Layout: header, IFD, out-of-line values (word aligned), pixel data (16-byte aligned).
    std::uint64_t cursor = 8 + 2 + 12 * entries.size() + 4;
    for (Entry& e : entries) {
        if (e.value.size() > 4) {
            cursor += cursor & 1;
            e.offset = static_cast<std::uint32_t>(cursor);
            cursor += e.value.size();
        }
    }
    const std::uint64_t data_offset = (cursor + 15) / 16 * 16;
    const std::uint64_t file_size = data_offset + row_bytes * height;
    if (file_size > std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("a {}x{} float TIFF needs {} bytes, beyond classic TIFF's 4 GiB", width,
                                height, file_size),
                    kWhere);
    }
    std::vector<std::uint32_t> offsets(strips);
    std::vector<std::uint32_t> counts(strips);
    for (std::uint32_t s = 0; s < strips; ++s) {
        const std::uint64_t first_row = static_cast<std::uint64_t>(s) * kRowsPerStrip;
        const std::uint64_t rows = std::min<std::uint64_t>(kRowsPerStrip, height - first_row);
        offsets[s] = static_cast<std::uint32_t>(data_offset + first_row * row_bytes);
        counts[s] = static_cast<std::uint32_t>(rows * row_bytes);
    }
    for (Entry& e : entries) {
        if (e.tag == kTagStripOffsets || e.tag == kTagStripByteCounts) {
            const std::uint32_t offset = e.offset;
            e = longs(e.tag, e.tag == kTagStripOffsets ? offsets : counts);
            e.offset = offset;
        }
    }

    std::vector<std::uint8_t> head;
    head.reserve(static_cast<std::size_t>(data_offset));
    head.push_back('I');
    head.push_back('I');
    append16(head, 42);
    append32(head, 8);
    append16(head, static_cast<std::uint16_t>(entries.size()));
    for (const Entry& e : entries) {
        append16(head, e.tag);
        append16(head, e.type);
        append32(head, e.count);
        if (e.value.size() <= 4) {
            for (std::size_t i = 0; i < 4; ++i) {
                head.push_back(i < e.value.size() ? e.value[i] : 0);
            }
        } else {
            append32(head, e.offset);
        }
    }
    append32(head, 0);  // no next IFD
    for (const Entry& e : entries) {
        if (e.value.size() > 4) {
            head.resize(e.offset, 0);
            head.insert(head.end(), e.value.begin(), e.value.end());
        }
    }
    head.resize(static_cast<std::size_t>(data_offset), 0);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail(ErrorCode::IoError, "cannot create the float TIFF", kWhere);
    }
    out.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
    for (std::int32_t y = 0; y < rgb.height() && out; ++y) {
        out.write(reinterpret_cast<const char*>(rgb.row(y)), static_cast<std::streamsize>(row_bytes));
    }
    out.close();
    if (!out) {
        return fail(ErrorCode::IoError, "writing the float TIFF failed", kWhere);
    }
    return ok();
}

Expected<image::ImageBuffer<float>> read_float_tiff(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uint64_t file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        return fail(ErrorCode::NotFound, "cannot stat the float TIFF: " + ec.message(), kWhere);
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail(ErrorCode::IoError, "cannot open the float TIFF", kWhere);
    }
    const auto read_at = [&](std::uint64_t offset,
                             std::size_t size) -> std::optional<std::vector<std::uint8_t>> {
        if (offset > file_size || size > file_size - offset) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> bytes(size);
        in.seekg(static_cast<std::streamoff>(offset));
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!in) {
            return std::nullopt;
        }
        return bytes;
    };
    const auto header = read_at(0, 8);
    if (!header || (*header)[0] != 'I' || (*header)[1] != 'I' || read16(header->data() + 2) != 42) {
        return fail(ErrorCode::Unsupported, "not a little-endian classic TIFF", kWhere);
    }
    const std::uint32_t ifd = read32(header->data() + 4);
    const auto count_bytes = read_at(ifd, 2);
    if (!count_bytes) {
        return fail(ErrorCode::Corrupt, "the IFD lies outside the file", kWhere);
    }
    const std::uint16_t count = read16(count_bytes->data());
    const auto table = read_at(static_cast<std::uint64_t>(ifd) + 2, static_cast<std::size_t>(count) * 12);
    if (!table) {
        return fail(ErrorCode::Corrupt, "the IFD is truncated", kWhere);
    }
    std::vector<std::uint32_t> width, height, bps, spp, compression, photometric, offsets, rows_per_strip,
        counts, planar, format;
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint8_t* e = table->data() + static_cast<std::size_t>(i) * 12;
        const std::uint16_t tag = read16(e);
        const std::uint16_t type = read16(e + 2);
        const std::uint32_t n = read32(e + 4);
        if (type != kTypeShort && type != kTypeLong) {
            continue;  // ASCII and others are not interpreted
        }
        const std::size_t size = static_cast<std::size_t>(n) * (type == kTypeShort ? 2 : 4);
        std::vector<std::uint8_t> bytes;
        if (size <= 4) {
            bytes.assign(e + 8, e + 8 + size);
        } else {
            auto value = read_at(read32(e + 8), size);
            if (!value) {
                return fail(ErrorCode::Corrupt, std::format("tag {} value lies outside the file", tag),
                            kWhere);
            }
            bytes = std::move(*value);
        }
        std::vector<std::uint32_t> values(n);
        for (std::uint32_t k = 0; k < n; ++k) {
            values[k] = type == kTypeShort ? read16(bytes.data() + 2 * k) : read32(bytes.data() + 4 * k);
        }
        switch (tag) {
            case kTagWidth:
                width = std::move(values);
                break;
            case kTagLength:
                height = std::move(values);
                break;
            case kTagBitsPerSample:
                bps = std::move(values);
                break;
            case kTagCompression:
                compression = std::move(values);
                break;
            case kTagPhotometric:
                photometric = std::move(values);
                break;
            case kTagStripOffsets:
                offsets = std::move(values);
                break;
            case kTagSamplesPerPixel:
                spp = std::move(values);
                break;
            case kTagRowsPerStrip:
                rows_per_strip = std::move(values);
                break;
            case kTagStripByteCounts:
                counts = std::move(values);
                break;
            case kTagPlanar:
                planar = std::move(values);
                break;
            case kTagSampleFormat:
                format = std::move(values);
                break;
            default:
                break;
        }
    }
    const std::vector<std::uint32_t> triple32{32, 32, 32};
    const std::vector<std::uint32_t> triple3{3, 3, 3};
    if (width.size() != 1 || height.size() != 1 || spp != std::vector<std::uint32_t>{3} || bps != triple32 ||
        format != triple3 || (!compression.empty() && compression != std::vector<std::uint32_t>{1}) ||
        photometric != std::vector<std::uint32_t>{2} ||
        (!planar.empty() && planar != std::vector<std::uint32_t>{1})) {
        return fail(ErrorCode::Unsupported, "only uncompressed chunky 3 x 32-bit float RGB TIFFs are read",
                    kWhere);
    }
    if (width[0] == 0 || height[0] == 0 || width[0] > 65535 || height[0] > 65535) {
        return fail(ErrorCode::Corrupt, "invalid image dimensions", kWhere);
    }
    const std::uint32_t rps = rows_per_strip.empty() ? height[0] : std::min(rows_per_strip[0], height[0]);
    if (rps == 0) {
        return fail(ErrorCode::Corrupt, "RowsPerStrip is 0", kWhere);
    }
    const std::uint32_t strips = (height[0] + rps - 1) / rps;
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(width[0]) * 12;
    if (offsets.size() != strips || counts.size() != strips) {
        return fail(ErrorCode::Corrupt, "strip tables do not match the image height", kWhere);
    }
    auto buffer = image::ImageBuffer<float>::create_uninitialized(static_cast<std::int32_t>(width[0]),
                                                                  static_cast<std::int32_t>(height[0]), 3);
    if (!buffer) {
        return Unexpected(std::move(buffer.error()));
    }
    for (std::uint32_t s = 0; s < strips; ++s) {
        const std::uint32_t first = s * rps;
        const std::uint32_t rows = std::min(rps, height[0] - first);
        if (counts[s] != rows * row_bytes || offsets[s] > file_size || counts[s] > file_size - offsets[s]) {
            return fail(ErrorCode::Corrupt, std::format("strip {} is inconsistent or truncated", s), kWhere);
        }
        in.seekg(static_cast<std::streamoff>(offsets[s]));
        for (std::uint32_t r = 0; r < rows; ++r) {
            in.read(reinterpret_cast<char*>(buffer->row(static_cast<std::int32_t>(first + r))),
                    static_cast<std::streamsize>(row_bytes));
        }
        if (!in) {
            return fail(ErrorCode::IoError, std::format("reading strip {} failed", s), kWhere);
        }
    }
    return std::move(*buffer);
}

}  // namespace lucent::render_tool
