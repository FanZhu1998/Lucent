#include "tiff_reader.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

#include <lucent/raw/dng_tags.hpp>

namespace lucent::raw::detail {

namespace {

constexpr std::uint64_t kClassicHeaderSize = 8;
constexpr std::uint64_t kBigTiffHeaderSize = 16;
constexpr std::uint16_t kClassicMagic = 42;
constexpr std::uint16_t kBigTiffMagic = 43;
constexpr std::uint16_t kBigTiffOffsetSize = 8;
constexpr std::uint16_t kOrfMagic = 0x4F52;   // "IIRO" / "MMOR"
constexpr std::uint16_t kOrfSMagic = 0x5352;  // "IIRS"
constexpr std::uint16_t kRw2Magic = 0x0055;   // "IIU\0"

// Size of one value of `type`, or 0 when the type is not defined for `variant`.
std::uint32_t type_size_for(std::uint16_t type, TiffVariant variant) noexcept {
    const bool bigtiff_only = type == static_cast<std::uint16_t>(TiffFieldType::Long8) ||
                              type == static_cast<std::uint16_t>(TiffFieldType::SLong8) ||
                              type == static_cast<std::uint16_t>(TiffFieldType::Ifd8);
    if (bigtiff_only && variant != TiffVariant::BigTiff) {
        return 0;
    }
    return tiff_field_type_size(type);
}

std::uint64_t load_le(const std::byte* data, std::size_t size) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = size; i-- > 0;) {
        value = (value << 8) | std::to_integer<std::uint64_t>(data[i]);
    }
    return value;
}

std::uint64_t load_be(const std::byte* data, std::size_t size) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        value = (value << 8) | std::to_integer<std::uint64_t>(data[i]);
    }
    return value;
}

}  // namespace

std::string describe_tag(std::uint16_t tag) {
    const std::string_view name = tiff_tag_name(tag);
    if (name == "unknown") {
        return std::format("{}", tag);
    }
    return std::format("{} ({})", tag, name);
}

std::uint64_t TiffReader::header_size() const noexcept {
    return variant_ == TiffVariant::BigTiff ? kBigTiffHeaderSize : kClassicHeaderSize;
}

std::uint32_t TiffReader::offset_size() const noexcept {
    return variant_ == TiffVariant::BigTiff ? 8u : 4u;
}

std::uint64_t TiffReader::load_unsigned(const std::byte* data, std::size_t size) const noexcept {
    return order_ == TiffByteOrder::LittleEndian ? load_le(data, size) : load_be(data, size);
}

Expected<TiffReader> TiffReader::open(RandomAccessSource& source, TiffSignatures accepted) {
    const std::uint64_t size = source.size();
    if (size == 0) {
        return fail(ErrorCode::Corrupt, "source is empty; a TIFF header needs at least 8 bytes",
                    std::string(kWhereHeader));
    }

    std::array<std::byte, kBigTiffHeaderSize> header{};
    const std::size_t available = static_cast<std::size_t>(std::min<std::uint64_t>(size, header.size()));
    auto read = source.read_at(0, std::span<std::byte>(header.data(), available));
    if (!read) {
        return Unexpected(std::move(read.error()));
    }
    if (*read != available) {
        return fail(ErrorCode::IoError,
                    std::format("source returned {} of {} header bytes", *read, available),
                    std::string(kWhereSource));
    }

    const auto is_order_mark = [](std::byte b) { return b == std::byte{'I'} || b == std::byte{'M'}; };
    if (!is_order_mark(header[0]) || (available >= 2 && header[1] != header[0])) {
        return fail(ErrorCode::Unsupported, "source does not start with a TIFF byte-order mark (II or MM)",
                    std::string(kWhereHeader));
    }
    if (available < 4) {
        return fail(ErrorCode::Corrupt, std::format("source is {} bytes; a TIFF header needs 8", size),
                    std::string(kWhereHeader));
    }
    const TiffByteOrder order =
        header[0] == std::byte{'I'} ? TiffByteOrder::LittleEndian : TiffByteOrder::BigEndian;
    const auto load = [order](const std::byte* data, std::size_t n) {
        return order == TiffByteOrder::LittleEndian ? load_le(data, n) : load_be(data, n);
    };

    const auto magic = static_cast<std::uint16_t>(load(header.data() + 2, 2));

    if (magic == kClassicMagic) {
        if (size < kClassicHeaderSize) {
            return fail(
                ErrorCode::Corrupt,
                std::format("source is {} bytes; a classic TIFF header needs {}", size, kClassicHeaderSize),
                std::string(kWhereHeader));
        }
        const std::uint64_t first = load(header.data() + 4, 4);
        return TiffReader(source, size, order, TiffVariant::Classic, first);
    }

    if (magic == kBigTiffMagic) {
        if (size < kBigTiffHeaderSize) {
            return fail(
                ErrorCode::Corrupt,
                std::format("source is {} bytes; a BigTIFF header needs {}", size, kBigTiffHeaderSize),
                std::string(kWhereHeader));
        }
        const auto offset_bytes = static_cast<std::uint16_t>(load(header.data() + 4, 2));
        const auto reserved = static_cast<std::uint16_t>(load(header.data() + 6, 2));
        if (offset_bytes != kBigTiffOffsetSize) {
            // BigTIFF reserves this field for a possible future offset size.
            return fail(
                ErrorCode::Unsupported,
                std::format("BigTIFF offset size is {}; only 8-byte offsets are defined", offset_bytes),
                std::string(kWhereHeader));
        }
        if (reserved != 0) {
            return fail(ErrorCode::Corrupt,
                        std::format("BigTIFF header reserved field is {}; it must be 0", reserved),
                        std::string(kWhereHeader));
        }
        const std::uint64_t first = load(header.data() + 8, 8);
        return TiffReader(source, size, order, TiffVariant::BigTiff, first);
    }

    // TIFF-structured vendor RAW containers replace the magic number: Olympus ORF "IIRO"/"MMOR"
    // (0x4F52) and "IIRS" (0x5352), Panasonic RW2 "IIU\0" (0x0055). Their IFDs follow the classic
    // TIFF layout; tag meanings are the vendor's (see TiffSignatures).
    const bool vendor_raw = magic == kOrfMagic || magic == kOrfSMagic || magic == kRw2Magic;
    if (vendor_raw && accepted == TiffSignatures::WithVendorRaw) {
        if (size < kClassicHeaderSize) {
            return fail(ErrorCode::Corrupt,
                        std::format("source is {} bytes; a TIFF-structured RAW header needs {}", size,
                                    kClassicHeaderSize),
                        std::string(kWhereHeader));
        }
        const std::uint64_t first = load(header.data() + 4, 4);
        return TiffReader(source, size, order, TiffVariant::Classic, first);
    }
    return fail(ErrorCode::Unsupported,
                std::format("TIFF byte-order mark followed by magic number {} (expected 42 or 43)", magic),
                std::string(kWhereHeader));
}

Status TiffReader::read_exact(std::uint64_t offset, std::span<std::byte> out) {
    auto read = source_->read_at(offset, out);
    if (!read) {
        return Unexpected(std::move(read.error()));
    }
    if (*read != out.size()) {
        return fail(ErrorCode::IoError,
                    std::format("source returned {} of {} bytes at offset {} although its size is {}", *read,
                                out.size(), offset, size_),
                    std::string(kWhereSource));
    }
    return ok();
}

Expected<TiffIfd> TiffReader::read_ifd(std::uint64_t offset, std::uint32_t max_entries,
                                       std::string_view path) {
    const bool big = variant_ == TiffVariant::BigTiff;
    const std::uint64_t count_size = big ? 8 : 2;
    const std::uint64_t entry_size = big ? 20 : 12;
    const std::uint64_t next_size = big ? 8 : 4;
    const std::uint64_t field_size = big ? 8 : 4;

    if (offset < header_size()) {
        return fail(
            ErrorCode::Corrupt,
            std::format("{}: IFD offset {} lies inside the {}-byte header", path, offset, header_size()),
            std::string(kWhereIfd));
    }
    if (!in_bounds(offset, count_size)) {
        return fail(
            ErrorCode::Corrupt,
            std::format("{}: IFD offset {} lies beyond the end of the {}-byte source", path, offset, size_),
            std::string(kWhereIfd));
    }

    std::array<std::byte, 8> count_bytes{};
    if (auto status = read_exact(offset, std::span<std::byte>(count_bytes.data(), count_size)); !status) {
        return Unexpected(std::move(status.error()));
    }
    const std::uint64_t entry_count = load_unsigned(count_bytes.data(), count_size);
    if (entry_count == 0) {
        return fail(ErrorCode::Corrupt, std::format("{}: IFD at offset {} has no entries", path, offset),
                    std::string(kWhereIfd));
    }
    if (entry_count > max_entries) {
        return fail(ErrorCode::Corrupt,
                    std::format("{}: IFD at offset {} declares {} entries; the plausibility limit is {}",
                                path, offset, entry_count, max_entries),
                    std::string(kWhereIfd));
    }

    // entry_count <= 2^32 - 1, so these products cannot overflow 64 bits.
    const std::uint64_t table_bytes = entry_count * entry_size;
    const std::uint64_t block_bytes = table_bytes + next_size;
    if (!in_bounds(offset + count_size, block_bytes)) {
        return fail(
            ErrorCode::Corrupt,
            std::format("{}: IFD at offset {} with {} entries needs {} bytes but the source ends at {}", path,
                        offset, entry_count, count_size + block_bytes, size_),
            std::string(kWhereIfd));
    }

    std::vector<std::byte> block(static_cast<std::size_t>(block_bytes));
    if (auto status = read_exact(offset + count_size, block); !status) {
        return Unexpected(std::move(status.error()));
    }

    TiffIfd ifd;
    ifd.offset = offset;
    ifd.entries.reserve(static_cast<std::size_t>(entry_count));
    for (std::uint64_t i = 0; i < entry_count; ++i) {
        const std::byte* raw = block.data() + i * entry_size;
        TiffEntry entry;
        entry.tag = static_cast<std::uint16_t>(load_unsigned(raw, 2));
        entry.type = static_cast<std::uint16_t>(load_unsigned(raw + 2, 2));
        entry.count = load_unsigned(raw + 4, big ? 8 : 4);
        entry.entry_offset = offset + count_size + i * entry_size;
        const std::byte* field = raw + (big ? 12 : 8);
        std::copy_n(field, field_size, entry.inline_field.begin());

        const std::uint32_t type_size = type_size_for(entry.type, variant_);
        if (type_size == 0) {
            entry.status = EntryStatus::UnknownType;
        } else if (entry.count > std::numeric_limits<std::uint64_t>::max() / type_size) {
            entry.status = EntryStatus::SizeOverflow;
        } else {
            entry.byte_count = entry.count * type_size;
            if (entry.byte_count <= field_size) {
                entry.is_inline = true;
                entry.value_offset = entry.entry_offset + (big ? 12 : 8);
            } else {
                entry.value_offset = load_unsigned(field, field_size);
                if (!in_bounds(entry.value_offset, entry.byte_count)) {
                    entry.status = EntryStatus::OutOfBounds;
                }
            }
        }
        ifd.entries.push_back(entry);
    }
    ifd.next_ifd_offset = load_unsigned(block.data() + table_bytes, next_size);
    return ifd;
}

Error TiffReader::entry_error(const TiffEntry& entry, std::string_view path) const {
    switch (entry.status) {
        case EntryStatus::UnknownType:
            return Error{
                ErrorCode::Corrupt,
                std::format("{}: tag {} has field type {}, which is not defined for this TIFF variant", path,
                            describe_tag(entry.tag), entry.type),
                std::string(kWhereEntry), 0};
        case EntryStatus::SizeOverflow:
            return Error{
                ErrorCode::Corrupt,
                std::format("{}: tag {} declares {} values of type {}; the byte size overflows 64 bits", path,
                            describe_tag(entry.tag), entry.count, tiff_field_type_name(entry.type)),
                std::string(kWhereEntry), 0};
        case EntryStatus::OutOfBounds:
            return Error{
                ErrorCode::Corrupt,
                std::format("{}: tag {} value of {} bytes at offset {} extends beyond the end of the "
                            "{}-byte source",
                            path, describe_tag(entry.tag), entry.byte_count, entry.value_offset, size_),
                std::string(kWhereEntry), 0};
        case EntryStatus::Ok:
            break;
    }
    return Error{ErrorCode::Internal, "entry_error called for a readable entry", std::string(kWhereInspect),
                 0};
}

Expected<std::vector<std::byte>> TiffReader::read_value(const TiffEntry& entry, std::uint64_t max_bytes,
                                                        std::string_view path) {
    if (entry.status != EntryStatus::Ok) {
        return Unexpected(entry_error(entry, path));
    }
    if (entry.byte_count > max_bytes) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("{}: tag {} value is {} bytes; the inspection limit is {}", path,
                                describe_tag(entry.tag), entry.byte_count, max_bytes),
                    std::string(kWhereLimits));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(entry.byte_count));
    if (entry.is_inline) {
        std::copy_n(entry.inline_field.begin(), bytes.size(), bytes.begin());
        return bytes;
    }
    if (auto status = read_exact(entry.value_offset, bytes); !status) {
        return Unexpected(std::move(status.error()));
    }
    return bytes;
}

}  // namespace lucent::raw::detail
