#include "container_extent.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace lucent::raw::detail {

namespace {

constexpr std::string_view kWhere = "raw.container.extent";

// Top-level boxes scanned in a CR3 before giving up with a warning (real files have about six).
constexpr std::uint32_t kMaxTopLevelBoxes = 4096;

std::uint64_t load_be(const std::byte* data, std::size_t size) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        value = (value << 8) | std::to_integer<std::uint64_t>(data[i]);
    }
    return value;
}

// Reads exactly out.size() bytes at `offset`, which the caller has checked lie inside the source.
Status read_exact(RandomAccessSource& source, std::uint64_t offset, std::span<std::byte> out) {
    auto read = source.read_at(offset, out);
    if (!read) {
        return Unexpected(std::move(read.error()));
    }
    if (*read != out.size()) {
        return fail(ErrorCode::IoError,
                    std::format("the source returned {} of {} bytes at offset {}", *read, out.size(), offset),
                    std::string(kWhere));
    }
    return ok();
}

// Printable form of a four-character box type.
std::string box_type(const std::byte* type) {
    std::string text;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto c = std::to_integer<unsigned char>(type[i]);
        text.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
    }
    return text;
}

// ISO/IEC 14496-12 box headers: 32-bit size and type; size 1 means a 64-bit size follows, size 0
// means the box extends to the end of the file.
Expected<ImageDataExtent> bmff_overruns(RandomAccessSource& source) {
    const std::uint64_t size = source.size();
    ImageDataExtent extent;
    std::uint64_t offset = 0;
    for (std::uint32_t boxes = 0; offset < size; ++boxes) {
        if (boxes == kMaxTopLevelBoxes) {
            extent.warnings.push_back(std::format(
                "more than {} top-level boxes; the rest of the file from byte {} was not verified",
                kMaxTopLevelBoxes, offset));
            break;
        }
        const std::string at = std::format("box at offset {}", offset);
        std::array<std::byte, 16> header{};
        if (size - offset < 8) {
            extent.overruns.push_back({at, offset + 8, "the box header is cut off by the end of the file"});
            break;
        }
        if (auto status = read_exact(source, offset, std::span<std::byte>(header.data(), 8)); !status) {
            return Unexpected(std::move(status.error()));
        }
        std::uint64_t box_size = load_be(header.data(), 4);
        const std::string name = std::format("box '{}' at offset {}", box_type(header.data() + 4), offset);
        std::uint64_t header_size = 8;
        if (box_size == 1) {
            if (size - offset < 16) {
                extent.overruns.push_back(
                    {name, offset + 16, "the 64-bit box size is cut off by the end of the file"});
                break;
            }
            if (auto status = read_exact(source, offset + 8, std::span<std::byte>(header.data() + 8, 8));
                !status) {
                return Unexpected(std::move(status.error()));
            }
            box_size = load_be(header.data() + 8, 8);
            header_size = 16;
        } else if (box_size == 0) {
            break;  // the last box, extending to the end of the file
        }
        if (box_size < header_size) {
            return fail(ErrorCode::Corrupt,
                        std::format("{} declares {} bytes, less than its {}-byte header", name, box_size,
                                    header_size),
                        std::string(kWhere));
        }
        if (box_size > size - offset) {
            const std::uint64_t end = offset > std::numeric_limits<std::uint64_t>::max() - box_size
                                          ? std::numeric_limits<std::uint64_t>::max()
                                          : offset + box_size;
            extent.overruns.push_back(
                {name, end, std::format("the box ends at byte {} but the file has {} bytes", end, size)});
            break;
        }
        offset += box_size;
    }
    return extent;
}

// RAF header (big-endian): "FUJIFILM" signature; at byte 84 the offset and length of the JPEG
// preview, at 92 those of the CFA header (the metadata directory), at 100 those of the CFA data.
// Ranges with a zero offset or length are not declared.
Expected<ImageDataExtent> raf_overruns(RandomAccessSource& source) {
    constexpr std::uint64_t kHeaderBytes = 108;
    const std::uint64_t size = source.size();
    ImageDataExtent extent;
    if (size < kHeaderBytes) {
        extent.overruns.push_back(
            {"RAF header", kHeaderBytes,
             std::format("the {}-byte header is cut off at byte {}", kHeaderBytes, size)});
        return extent;
    }
    std::array<std::byte, kHeaderBytes> header{};
    if (auto status = read_exact(source, 0, header); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (std::memcmp(header.data(), "FUJIFILM", 8) != 0) {
        return fail(ErrorCode::Unsupported, "the data does not start with the RAF signature",
                    std::string(kWhere));
    }
    struct Range {
        const char* name;
        std::size_t field;
    };
    for (const Range range : {Range{"JPEG preview", 84}, Range{"CFA header", 92}, Range{"CFA data", 100}}) {
        const std::uint64_t start = load_be(header.data() + range.field, 4);
        const std::uint64_t length = load_be(header.data() + range.field + 4, 4);
        if (start == 0 || length == 0) {
            continue;
        }
        const std::uint64_t end = start + length;  // both are 32-bit: no overflow
        if (end > size) {
            extent.overruns.push_back(
                {std::format("RAF {}", range.name), end,
                 std::format("{} ends at byte {} but the file has {} bytes", range.name, end, size)});
        }
    }
    return extent;
}

}  // namespace

bool has_structural_check(ContainerFormat container) noexcept {
    switch (container) {
        case ContainerFormat::Tiff:
        case ContainerFormat::Dng:
        case ContainerFormat::Orf:
        case ContainerFormat::Rw2:
        case ContainerFormat::Cr3:
        case ContainerFormat::Raf:
            return true;
        case ContainerFormat::Unknown:
        case ContainerFormat::Crw:
        case ContainerFormat::X3f:
            return false;
    }
    return false;
}

Expected<ImageDataExtent> find_container_overruns(RandomAccessSource& source, ContainerFormat container,
                                                  const TiffInspectLimits& limits) {
    switch (container) {
        case ContainerFormat::Tiff:
        case ContainerFormat::Dng:
        case ContainerFormat::Orf:
        case ContainerFormat::Rw2:
            return find_image_data_overruns(source, limits, TiffSignatures::WithVendorRaw);
        case ContainerFormat::Cr3:
            return bmff_overruns(source);
        case ContainerFormat::Raf:
            return raf_overruns(source);
        case ContainerFormat::Unknown:
        case ContainerFormat::Crw:
        case ContainerFormat::X3f:
            break;
    }
    return fail(ErrorCode::Unsupported,
                std::format("no structural check exists for {} containers", to_string(container)),
                std::string(kWhere));
}

}  // namespace lucent::raw::detail
