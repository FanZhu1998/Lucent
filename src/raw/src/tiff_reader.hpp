#pragma once

// Private low-level TIFF structure reader used by the inspector: header, IFD entry tables and
// bounds-checked value access. Nothing here interprets tag meaning.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/raw/tiff_inspector.hpp>

namespace lucent::raw::detail {

// Stable failure locations (Error::where).
inline constexpr std::string_view kWhereHeader = "raw.tiff.header";
inline constexpr std::string_view kWhereIfd = "raw.tiff.ifd";
inline constexpr std::string_view kWhereIfdTree = "raw.tiff.ifd_tree";
inline constexpr std::string_view kWhereEntry = "raw.tiff.entry";
inline constexpr std::string_view kWhereTag = "raw.tiff.tag";
inline constexpr std::string_view kWhereLimits = "raw.tiff.limits";
inline constexpr std::string_view kWhereSource = "raw.tiff.source";
inline constexpr std::string_view kWhereInspect = "raw.tiff.inspect";
inline constexpr std::string_view kWhereOpcodeList = "raw.dng.opcode_list";
inline constexpr std::string_view kWhereColor = "raw.dng.color";

// Why an entry's value cannot be accessed.
enum class EntryStatus : std::uint8_t {
    Ok,
    UnknownType,   // field type not defined for this TIFF variant; the entry must be skipped
    SizeOverflow,  // count * type size does not fit in 64 bits
    OutOfBounds,   // value range extends beyond the end of the source
};

// One decoded IFD entry. Offsets are absolute positions in the source.
struct TiffEntry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint64_t count = 0;
    std::uint64_t entry_offset = 0;  // first byte of the 12- or 20-byte entry
    std::uint64_t value_offset = 0;  // first value byte (inline field or out-of-line data)
    std::uint64_t byte_count = 0;    // count * type size; valid when status != UnknownType/SizeOverflow
    std::array<std::byte, 8> inline_field{};  // raw value/offset field, file byte order
    bool is_inline = false;
    EntryStatus status = EntryStatus::Ok;
};

struct TiffIfd {
    std::uint64_t offset = 0;
    std::vector<TiffEntry> entries;  // in file order
    std::uint64_t next_ifd_offset = 0;
};

// Header signatures TiffReader::open accepts.
enum class TiffSignatures : std::uint8_t {
    Standard,  // TIFF (magic 42) and BigTIFF (magic 43)
    // Also the TIFF-structured RAW variants that replace the magic number: Olympus/OM System ORF
    // ("IIRO", "IIRS", "MMOR") and Panasonic RW2 ("IIU\0"). They are read as classic TIFF (32-bit
    // offsets, first IFD offset at byte 4). Their tag semantics are vendor-specific, so only
    // structural checks (tiff_extent.hpp) use this mode, never the DNG inspector.
    WithVendorRaw,
};

class TiffReader {
public:
    // Parses the header. Fails with Unsupported for a signature `accepted` does not include and
    // Corrupt for a truncated or invalid TIFF/BigTIFF header. `source` must outlive the reader.
    [[nodiscard]] static Expected<TiffReader> open(RandomAccessSource& source,
                                                   TiffSignatures accepted = TiffSignatures::Standard);

    [[nodiscard]] TiffByteOrder byte_order() const noexcept { return order_; }
    [[nodiscard]] TiffVariant variant() const noexcept { return variant_; }
    [[nodiscard]] std::uint64_t source_size() const noexcept { return size_; }
    [[nodiscard]] std::uint64_t header_size() const noexcept;
    [[nodiscard]] std::uint64_t first_ifd_offset() const noexcept { return first_ifd_offset_; }

    // Size in bytes of an IFD offset / pointer value in this variant (4 or 8).
    [[nodiscard]] std::uint32_t offset_size() const noexcept;

    // True when [offset, offset + length) lies within the source. Overflow-safe.
    [[nodiscard]] bool in_bounds(std::uint64_t offset, std::uint64_t length) const noexcept {
        return offset <= size_ && length <= size_ - offset;
    }

    // Reads and decodes the IFD at `offset`: entry count, entry table and next-IFD offset.
    // `path` names the IFD in error messages. Fails with Corrupt when the IFD lies (partly)
    // outside the source, starts inside the header, is empty, or declares more than
    // `max_entries` entries.
    [[nodiscard]] Expected<TiffIfd> read_ifd(std::uint64_t offset, std::uint32_t max_entries,
                                             std::string_view path);

    // Reads an entry's value bytes (file byte order). Fails with Corrupt when the entry status
    // is not Ok, and with LimitExceeded when the value is larger than `max_bytes`.
    [[nodiscard]] Expected<std::vector<std::byte>> read_value(const TiffEntry& entry, std::uint64_t max_bytes,
                                                              std::string_view path);

    // Error describing why an entry with a non-Ok status cannot be read. Precondition:
    // entry.status != EntryStatus::Ok.
    [[nodiscard]] Error entry_error(const TiffEntry& entry, std::string_view path) const;

    // Decodes an unsigned integer of `size` bytes (1, 2, 4 or 8) in file byte order.
    [[nodiscard]] std::uint64_t load_unsigned(const std::byte* data, std::size_t size) const noexcept;

private:
    TiffReader(RandomAccessSource& source, std::uint64_t size, TiffByteOrder order, TiffVariant variant,
               std::uint64_t first_ifd_offset) noexcept
        : source_(&source),
          size_(size),
          order_(order),
          variant_(variant),
          first_ifd_offset_(first_ifd_offset) {}

    // Reads exactly out.size() bytes at `offset`. Precondition: in_bounds(offset, out.size()).
    [[nodiscard]] Status read_exact(std::uint64_t offset, std::span<std::byte> out);

    RandomAccessSource* source_;
    std::uint64_t size_;
    TiffByteOrder order_;
    TiffVariant variant_;
    std::uint64_t first_ifd_offset_;
};

// Formats "<tag number> (<name>)" for messages.
[[nodiscard]] std::string describe_tag(std::uint16_t tag);

}  // namespace lucent::raw::detail
