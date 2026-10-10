#include "tiff_extent.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lucent/raw/dng_tags.hpp>

namespace lucent::raw::detail {

namespace {

constexpr std::uint16_t kTileOffsets = 324;
constexpr std::uint16_t kTileByteCounts = 325;
// Panasonic RW2 / Leica RWL tags of IFD 0 (ExifTool's PanasonicRaw table, LibRaw's parse_tiff_ifd()).
constexpr std::uint16_t kPanasonicSensorWidth = 0x0002;
constexpr std::uint16_t kPanasonicSensorHeight = 0x0003;
constexpr std::uint16_t kPanasonicRawFormat = 0x002D;
constexpr std::uint16_t kPanasonicRawDataOffset = 0x0118;
constexpr std::uint16_t kTiffTypeLong = 4;
// The fixed-rate Panasonic encodings (RawFormat absent, or 1 to 7) store at most 14 pixels in 16
// bytes (the classic encoding exactly; 5: 9 or 10, 6: 11 or 14, 7: 9 or 10 pixels per 16 bytes).
constexpr std::uint64_t kPanasonicPixelsPerBlock = 14;
constexpr std::uint64_t kPanasonicBlockBytes = 16;
constexpr std::uint64_t kPanasonicLastFixedRateFormat = 7;
// LibRaw 0.22.2 decodes RawFormat absent or 1 to 5 with panasonic_load_raw(), which reads the raw
// data in 16 KiB pages with load_flags 0x2008 (set by a LONG RawDataOffset, src/metadata/tiff.cpp):
// each page is read as fread(buf + 0x2008, 0x1FF8) then fread(buf, 0x2008), and the bitstream is
// consumed from buf[0], so a page's first bitstream bytes are its file bytes 0x1FF8 onwards
// (pana_data(), src/decoders/decoders_dcraw.cpp). RawFormat 6 and 7 are read row by row.
constexpr std::uint64_t kPanasonicPageBytes = 0x4000;
constexpr std::uint64_t kPanasonicPageSplit = 0x2008;
constexpr std::uint64_t kPanasonicLastPagedFormat = 5;

// How far into the raw data LibRaw's paged reader consumes the file for `bitstream` bytes of
// bitstream: whole pages, then for a last page holding K bytes, its bytes up to 0x1FF8 + K when
// K <= 0x2008 (the first part LibRaw consumes), otherwise the whole page.
std::uint64_t panasonic_paged_end(std::uint64_t bitstream) noexcept {
    const std::uint64_t rest = bitstream % kPanasonicPageBytes;
    std::uint64_t end = bitstream - rest;
    if (rest != 0) {
        end += rest <= kPanasonicPageSplit ? (kPanasonicPageBytes - kPanasonicPageSplit) + rest : kPanasonicPageBytes;
    }
    return end;
}

struct PendingIfd {
    std::uint64_t offset = 0;
    std::string path;
    std::uint32_t depth = 0;  // 0 for the main chain
};

// True for errors that describe the file's structure (as opposed to a failure to read it).
bool is_structural(ErrorCode code) noexcept {
    return code == ErrorCode::Corrupt || code == ErrorCode::LimitExceeded;
}

// Unsigned integers of an offset/count entry (SHORT, LONG, IFD, LONG8, IFD8), at most
// `max_values` of them (the first ones).
Expected<std::vector<std::uint64_t>> integers(TiffReader& reader, const TiffEntry& entry,
                                              const TiffInspectLimits& limits, const std::string& path,
                                              std::uint64_t max_values) {
    const std::uint32_t size = tiff_field_type_size(entry.type);
    const bool integral =
        entry.type == 3 || entry.type == 4 || entry.type == 13 || entry.type == 16 || entry.type == 18;
    if (!integral || size == 0) {
        return fail(ErrorCode::Corrupt,
                    std::format("{} tag {} has non-integer type {}", path, entry.tag,
                                tiff_field_type_name(entry.type)),
                    "raw.tiff.extent");
    }
    TiffEntry prefix = entry;
    if (prefix.count > max_values) {
        // Read only the first max_values values: the same location, a smaller count. An inline
        // value stays inline (fewer values still fit in the field).
        prefix.count = max_values;
        prefix.byte_count = max_values * size;
    }
    auto bytes = reader.read_value(prefix, limits.max_value_bytes, path);
    if (!bytes) {
        return Unexpected(std::move(bytes.error()));
    }
    std::vector<std::uint64_t> values;
    values.reserve(static_cast<std::size_t>(prefix.count));
    for (std::size_t i = 0; i + size <= bytes->size(); i += size) {
        values.push_back(reader.load_unsigned(bytes->data() + i, size));
    }
    return values;
}

// How an IFD that read_ifd() rejected relates to the end of the source.
enum class BrokenIfd {
    BeyondEnd,  // the IFD, or its plausibly sized entry table, extends past the end: truncated
    Malformed,  // inside the source but malformed (implausible entry count, inside the header)
};

Expected<BrokenIfd> classify_broken_ifd(RandomAccessSource& source, const TiffReader& reader,
                                        std::uint64_t offset, const TiffInspectLimits& limits) {
    const bool big = reader.variant() == TiffVariant::BigTiff;
    const std::uint64_t count_size = big ? 8 : 2;
    const std::uint64_t entry_size = big ? 20 : 12;
    const std::uint64_t next_size = big ? 8 : 4;
    if (!reader.in_bounds(offset, count_size)) {
        return BrokenIfd::BeyondEnd;
    }
    if (offset < reader.header_size()) {
        return BrokenIfd::Malformed;
    }
    std::array<std::byte, 8> count_bytes{};
    auto read = source.read_at(offset, std::span<std::byte>(count_bytes.data(), count_size));
    if (!read) {
        return Unexpected(std::move(read.error()));
    }
    if (*read != count_size) {
        return fail(ErrorCode::IoError,
                    std::format("the source returned {} of {} bytes at offset {}", *read, count_size, offset),
                    "raw.tiff.source");
    }
    const std::uint64_t entries = reader.load_unsigned(count_bytes.data(), count_size);
    if (entries == 0 || entries > limits.max_entries_per_ifd) {
        return BrokenIfd::Malformed;  // garbage rather than a cut-off table
    }
    // entries <= max_entries_per_ifd (32-bit), so the table size cannot overflow.
    return reader.in_bounds(offset, count_size + entries * entry_size + next_size) ? BrokenIfd::Malformed
                                                                                   : BrokenIfd::BeyondEnd;
}

class ExtentScan {
public:
    ExtentScan(RandomAccessSource& source, TiffReader& reader, const TiffInspectLimits& limits, RawDataRule rule)
        : source_(source), reader_(reader), limits_(limits), rule_(rule) {}

    Expected<ImageDataExtent> run();

private:
    // Scans one readable IFD: queues its SubIFDs and checks its strips and tiles.
    Status scan_ifd(const PendingIfd& current, const TiffIfd& ifd);
    // Checks one strip or tile table pair of an IFD.
    Status check_data(const PendingIfd& current, const TiffEntry& offsets, const TiffEntry& counts,
                      const char* what);
    // Checks the Panasonic raw data of IFD 0 at RawDataOffset (RawDataRule::Panasonic).
    Status check_panasonic_data(const PendingIfd& current, const TiffIfd& ifd, const TiffEntry& raw_offset,
                                const TiffEntry* strip_counts);
    // The first value of an integer entry; std::nullopt when absent or unreadable.
    std::optional<std::uint64_t> first_value(const TiffEntry* entry, const std::string& path);
    // Records a structural problem of part of the file as "not verified" and lets the scan go on;
    // passes read failures through.
    Status skip(const std::string& what, Error error);

    RandomAccessSource& source_;
    TiffReader& reader_;
    const TiffInspectLimits& limits_;
    RawDataRule rule_;
    ImageDataExtent extent_;
    std::vector<PendingIfd> pending_;
};

std::optional<std::uint64_t> ExtentScan::first_value(const TiffEntry* entry, const std::string& path) {
    if (entry == nullptr || entry->status != EntryStatus::Ok) {
        return std::nullopt;
    }
    auto values = integers(reader_, *entry, limits_, path, 1);
    if (!values || values->empty()) {
        return std::nullopt;
    }
    return values->front();
}

Status ExtentScan::check_panasonic_data(const PendingIfd& current, const TiffIfd& ifd, const TiffEntry& raw_offset,
                                        const TiffEntry* strip_counts) {
    const std::string what = std::format("{} raw data (RawDataOffset)", current.path);
    if (raw_offset.status == EntryStatus::OutOfBounds) {
        extent_.overruns.push_back({current.path, raw_offset.value_offset + raw_offset.byte_count,
                                    "the raw data offset lies beyond the end of the file"});
        return ok();
    }
    if (raw_offset.status != EntryStatus::Ok) {
        return skip(what, reader_.entry_error(raw_offset, current.path));
    }
    auto start = integers(reader_, raw_offset, limits_, current.path, 1);
    if (!start) {
        return skip(what, std::move(start.error()));
    }
    if (start->empty()) {
        return ok();  // no offset declared: nothing to compare
    }
    const std::uint64_t offset = start->front();
    const auto find = [&ifd](std::uint16_t tag) -> const TiffEntry* {
        for (const TiffEntry& entry : ifd.entries) {
            if (entry.tag == tag) {
                return &entry;
            }
        }
        return nullptr;
    };
    // The length: StripByteCounts when declared (LibRaw's data size), otherwise the least a
    // fixed-rate encoding needs for the sensor area; for the encodings LibRaw reads in pages, at
    // least as far as its page order consumes that bitstream (COR-6, W12).
    const auto format = first_value(find(kPanasonicRawFormat), current.path);
    const auto width = first_value(find(kPanasonicSensorWidth), current.path);
    const auto height = first_value(find(kPanasonicSensorHeight), current.path);
    const bool fixed_rate = !format || (*format >= 1 && *format <= kPanasonicLastFixedRateFormat);
    const bool paged = !format || (*format >= 1 && *format <= kPanasonicLastPagedFormat);
    std::uint64_t encoded = 0;  // 16 bytes per 14 pixels: at most as many pixels as any fixed-rate encoding packs
    if (fixed_rate && width && height && *width > 0 && *height > 0 && *width <= 0xFFFF && *height <= 0xFFFF) {
        encoded = (*width * *height + kPanasonicPixelsPerBlock - 1) / kPanasonicPixelsPerBlock * kPanasonicBlockBytes;
    }
    std::uint64_t length = 0;
    std::string basis;
    if (const auto declared = first_value(strip_counts, current.path); declared && *declared != 0) {
        length = *declared;
        basis = "StripByteCounts";
    } else if (encoded != 0) {
        length = encoded;
        basis = std::format("16 bytes per 14 pixels of the {}x{} sensor", *width, *height);
    }
    if (paged && encoded != 0 && panasonic_paged_end(encoded) > length) {
        length = panasonic_paged_end(encoded);
        basis = std::format("16 bytes per 14 pixels of the {}x{} sensor, read by LibRaw in 16 KiB pages whose last "
                            "{} bytes come first",
                            *width, *height, kPanasonicPageSplit);
    }
    const std::uint64_t size = reader_.source_size();
    if (offset >= size) {
        const std::uint64_t end = offset > std::numeric_limits<std::uint64_t>::max() - std::max<std::uint64_t>(length, 1)
                                      ? std::numeric_limits<std::uint64_t>::max()
                                      : offset + std::max<std::uint64_t>(length, 1);
        extent_.overruns.push_back(
            {current.path, end,
             std::format("the raw data starts at byte {} but the file has {} bytes", offset, size)});
        return ok();
    }
    if (length > size - offset) {
        const std::uint64_t end = length > std::numeric_limits<std::uint64_t>::max() - offset
                                      ? std::numeric_limits<std::uint64_t>::max()
                                      : offset + length;
        extent_.overruns.push_back(
            {current.path, end,
             std::format("raw data ends at byte {} ({}) but the file has {} bytes", end, basis, size)});
    }
    return ok();
}

Status ExtentScan::skip(const std::string& what, Error error) {
    if (!is_structural(error.code)) {
        return Unexpected(std::move(error));
    }
    extent_.warnings.push_back(std::format("{} was not verified: {}", what, error.message));
    return ok();
}

Expected<ImageDataExtent> ExtentScan::run() {
    std::set<std::uint64_t> visited;
    // IFD 0 chain first (iteratively), SubIFDs pushed as they are found and scanned before the
    // next chain IFD.
    std::uint64_t next_main = reader_.first_ifd_offset();
    std::uint32_t main_index = 0;
    while (true) {
        PendingIfd current;
        if (!pending_.empty()) {
            current = std::move(pending_.back());
            pending_.pop_back();
        } else if (next_main != 0) {
            current = PendingIfd{next_main, std::format("ifd{}", main_index++), 0};
            next_main = 0;
        } else {
            break;
        }
        if (!visited.insert(current.offset).second) {
            continue;  // loops and shared IFDs are the inspector's concern; scan each IFD once
        }
        if (visited.size() > limits_.max_ifds) {
            extent_.warnings.push_back(std::format(
                "the file references more than {} IFDs; {} and the IFDs after it were not verified",
                limits_.max_ifds, current.path));
            break;
        }
        auto ifd = reader_.read_ifd(current.offset, limits_.max_entries_per_ifd, current.path);
        if (!ifd) {
            if (ifd.error().code != ErrorCode::Corrupt) {
                return Unexpected(std::move(ifd.error()));
            }
            auto broken = classify_broken_ifd(source_, reader_, current.offset, limits_);
            if (!broken) {
                return Unexpected(std::move(broken.error()));
            }
            if (*broken == BrokenIfd::BeyondEnd) {
                // A truncated copy typically ends before or inside an IFD (IFD 0 of a DNG that
                // stores its IFDs after the image data, a thumbnail IFD, a SubIFD).
                extent_.overruns.push_back(
                    {current.path, current.offset, "the IFD lies (partly) beyond the end of the file"});
            } else if (current.path == "ifd0") {
                return Unexpected(std::move(ifd.error()));  // nothing of the file can be checked
            } else {
                extent_.warnings.push_back(
                    std::format("{} was skipped: {}", current.path, ifd.error().message));
            }
            // A broken main-chain IFD ends the chain (SubIFDs of earlier chain IFDs have already
            // been scanned: they are processed before the next chain IFD).
            continue;
        }
        if (current.depth == 0) {
            next_main = ifd->next_ifd_offset;
        }
        if (auto scanned = scan_ifd(current, *ifd); !scanned) {
            return Unexpected(std::move(scanned.error()));
        }
    }
    return std::move(extent_);
}

Status ExtentScan::scan_ifd(const PendingIfd& current, const TiffIfd& ifd) {
    const TiffEntry* offsets[2] = {nullptr, nullptr};  // strips, tiles
    const TiffEntry* counts[2] = {nullptr, nullptr};
    const TiffEntry* panasonic_raw_offset = nullptr;
    for (const TiffEntry& entry : ifd.entries) {
        switch (entry.tag) {
            case kPanasonicRawDataOffset:
                panasonic_raw_offset = &entry;
                break;
            case tiff_tag::kStripOffsets:
                offsets[0] = &entry;
                break;
            case tiff_tag::kStripByteCounts:
                counts[0] = &entry;
                break;
            case kTileOffsets:
                offsets[1] = &entry;
                break;
            case kTileByteCounts:
                counts[1] = &entry;
                break;
            case tiff_tag::kSubIfds: {
                const std::string what = std::format("{} SubIFDs", current.path);
                if (entry.status == EntryStatus::OutOfBounds) {
                    extent_.overruns.push_back({current.path, entry.value_offset + entry.byte_count,
                                                "the SubIFD offsets lie beyond the end of the file"});
                    break;
                }
                if (entry.status != EntryStatus::Ok) {
                    if (auto skipped = skip(what, reader_.entry_error(entry, current.path)); !skipped) {
                        return skipped;
                    }
                    break;
                }
                if (current.depth + 1 > limits_.max_ifd_depth) {
                    extent_.warnings.push_back(std::format("{} deeper than {} levels were not verified", what,
                                                           limits_.max_ifd_depth));
                    break;
                }
                // Every listed SubIFD is visited, so more than max_ifds of them can never all be
                // scanned; reading the whole list first would let a crafted count allocate
                // max_value_bytes of offsets and one queued path per offset.
                if (entry.count > limits_.max_ifds) {
                    extent_.warnings.push_back(
                        std::format("{} lists {} IFDs; only the first {} were verified", what, entry.count,
                                    limits_.max_ifds));
                }
                auto children = integers(reader_, entry, limits_, current.path, limits_.max_ifds);
                if (!children) {
                    if (auto skipped = skip(what, std::move(children.error())); !skipped) {
                        return skipped;
                    }
                    break;
                }
                for (std::size_t i = children->size(); i-- > 0;) {
                    pending_.push_back(
                        {(*children)[i], std::format("{}.subifd{}", current.path, i), current.depth + 1});
                }
                break;
            }
            default:
                break;
        }
    }
    // RW2 / RWL: the raw data LibRaw reads is at RawDataOffset; IFD 0's strips are not it.
    bool check_strips = true;
    if (rule_ == RawDataRule::Panasonic && current.depth == 0 && current.path == "ifd0" &&
        panasonic_raw_offset != nullptr && panasonic_raw_offset->type == kTiffTypeLong) {
        if (auto checked = check_panasonic_data(current, ifd, *panasonic_raw_offset, counts[0]); !checked) {
            return checked;
        }
        check_strips = false;
    }
    for (int kind = 0; kind < 2; ++kind) {
        if (offsets[kind] == nullptr || counts[kind] == nullptr || (kind == 0 && !check_strips)) {
            continue;
        }
        if (auto checked = check_data(current, *offsets[kind], *counts[kind], kind == 0 ? "strip" : "tile");
            !checked) {
            return checked;
        }
    }
    return ok();
}

Status ExtentScan::check_data(const PendingIfd& current, const TiffEntry& offset_entry,
                              const TiffEntry& count_entry, const char* what) {
    if (offset_entry.status == EntryStatus::OutOfBounds || count_entry.status == EntryStatus::OutOfBounds) {
        extent_.overruns.push_back({current.path,
                                    std::max(offset_entry.value_offset + offset_entry.byte_count,
                                             count_entry.value_offset + count_entry.byte_count),
                                    std::format("the {} tables lie beyond the end of the file", what)});
        return ok();
    }
    const std::string table = std::format("{} {} table", current.path, what);
    for (const TiffEntry* entry : {&offset_entry, &count_entry}) {
        if (entry->status != EntryStatus::Ok) {
            return skip(table, reader_.entry_error(*entry, current.path));
        }
    }
    constexpr std::uint64_t kAll = std::numeric_limits<std::uint64_t>::max();
    auto starts = integers(reader_, offset_entry, limits_, current.path, kAll);
    if (!starts) {
        return skip(table, std::move(starts.error()));
    }
    auto lengths = integers(reader_, count_entry, limits_, current.path, kAll);
    if (!lengths) {
        return skip(table, std::move(lengths.error()));
    }
    std::uint64_t end = 0;
    const std::size_t n = std::min(starts->size(), lengths->size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t start = (*starts)[i];
        const std::uint64_t length = (*lengths)[i];
        const std::uint64_t stop = start > std::numeric_limits<std::uint64_t>::max() - length
                                       ? std::numeric_limits<std::uint64_t>::max()
                                       : start + length;
        end = std::max(end, stop);
    }
    const std::uint64_t size = reader_.source_size();
    if (end > size) {
        extent_.overruns.push_back(
            {current.path, end,
             std::format("{} data ends at byte {} but the file has {} bytes", what, end, size)});
    }
    return ok();
}

}  // namespace

Expected<ImageDataExtent> find_image_data_overruns(RandomAccessSource& source,
                                                   const TiffInspectLimits& limits, TiffSignatures accepted,
                                                   RawDataRule rule) {
    auto opened = TiffReader::open(source, accepted);
    if (!opened) {
        return Unexpected(std::move(opened.error()));
    }
    ExtentScan scan(source, *opened, limits, rule);
    return scan.run();
}

}  // namespace lucent::raw::detail
