#pragma once

// Test-only writer for synthetic TIFF, BigTIFF and DNG streams.
//
// A Document describes IFDs abstractly (tag, type and typed values); build() lays them out in the
// requested byte order and variant, generates the SubIFDs / Exif pointer entries, and records
// where every IFD, entry and value landed so that tests can corrupt precise bytes afterwards.
// Opcode lists are produced separately by opcode_list(), always big-endian as DNG requires.
//
// This is deliberately a small, obviously-correct writer, not a general TIFF encoder: values are
// written exactly as given, including deliberately invalid counts or offsets.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <lucent/raw/dng_tags.hpp>

namespace lucent::raw::test_support {

enum class Endian { Little, Big };
enum class Format { Classic, BigTiff };

// One IFD entry. Exactly one of the value vectors is used, according to `type`.
struct Entry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::vector<std::uint64_t> integers;  // BYTE..LONG8, signed as two's complement
    std::vector<std::pair<std::uint32_t, std::uint32_t>> rationals;  // RATIONAL / SRATIONAL words
    std::vector<double> reals;                                       // FLOAT / DOUBLE
    std::vector<std::byte> raw;                                      // ASCII, UNDEFINED, unknown types
    std::optional<std::uint64_t> count_override;   // count written instead of the natural count
    std::optional<std::uint64_t> offset_override;  // value offset written instead of the real one
};

inline constexpr std::uint16_t type_code(TiffFieldType type) {
    return static_cast<std::uint16_t>(type);
}

inline Entry integer_entry(std::uint16_t tag, TiffFieldType type, std::vector<std::uint64_t> values) {
    Entry e;
    e.tag = tag;
    e.type = type_code(type);
    e.integers = std::move(values);
    return e;
}
inline Entry byte_entry(std::uint16_t tag, std::vector<std::uint64_t> values) {
    return integer_entry(tag, TiffFieldType::Byte, std::move(values));
}
inline Entry short_entry(std::uint16_t tag, std::vector<std::uint64_t> values) {
    return integer_entry(tag, TiffFieldType::Short, std::move(values));
}
inline Entry long_entry(std::uint16_t tag, std::vector<std::uint64_t> values) {
    return integer_entry(tag, TiffFieldType::Long, std::move(values));
}
inline Entry ascii_entry(std::uint16_t tag, const std::string& text) {
    Entry e;
    e.tag = tag;
    e.type = type_code(TiffFieldType::Ascii);
    for (const char c : text) {
        e.raw.push_back(static_cast<std::byte>(c));
    }
    e.raw.push_back(std::byte{0});
    return e;
}
inline Entry rational_entry(std::uint16_t tag, std::vector<std::pair<std::uint32_t, std::uint32_t>> values) {
    Entry e;
    e.tag = tag;
    e.type = type_code(TiffFieldType::Rational);
    e.rationals = std::move(values);
    return e;
}
inline Entry srational_entry(std::uint16_t tag,
                             const std::vector<std::pair<std::int32_t, std::int32_t>>& values) {
    Entry e;
    e.tag = tag;
    e.type = type_code(TiffFieldType::SRational);
    for (const auto& [n, d] : values) {
        e.rationals.emplace_back(static_cast<std::uint32_t>(n), static_cast<std::uint32_t>(d));
    }
    return e;
}
inline Entry double_entry(std::uint16_t tag, std::vector<double> values) {
    Entry e;
    e.tag = tag;
    e.type = type_code(TiffFieldType::Double);
    e.reals = std::move(values);
    return e;
}
inline Entry float_entry(std::uint16_t tag, std::vector<double> values) {
    Entry e;
    e.tag = tag;
    e.type = type_code(TiffFieldType::Float);
    e.reals = std::move(values);
    return e;
}
inline Entry undefined_entry(std::uint16_t tag, std::vector<std::byte> bytes) {
    Entry e;
    e.tag = tag;
    e.type = type_code(TiffFieldType::Undefined);
    e.raw = std::move(bytes);
    return e;
}
// Entry of any (possibly undefined) type with the given raw value bytes and count.
inline Entry raw_entry(std::uint16_t tag, std::uint16_t type, std::uint64_t count,
                       std::vector<std::byte> bytes) {
    Entry e;
    e.tag = tag;
    e.type = type;
    e.raw = std::move(bytes);
    e.count_override = count;
    return e;
}

struct Ifd {
    std::vector<Entry> entries;
    std::vector<Ifd> sub_ifds;  // written as a SubIFDs (330) entry when non-empty
    std::vector<Ifd> exif;      // at most one; written as an ExifIFD (34665) entry
    std::vector<Ifd> gps;       // at most one; written as a GPSInfo IFD (34853) entry, path ".gps"
    bool sort_entries = true;   // TIFF requires ascending tags; false keeps the given order
};

struct Document {
    Endian endian = Endian::Little;
    Format format = Format::Classic;
    std::vector<Ifd> chain;  // IFD0, IFD1, ...
};

// A built stream plus the layout needed to corrupt it precisely. Keys are IFD paths as the
// inspector names them ("ifd0", "ifd0.subifd1", "ifd0.exif") and "<path>/<tag>" for entries.
struct Built {
    std::vector<std::byte> bytes;
    Endian endian = Endian::Little;
    Format format = Format::Classic;
    std::map<std::string, std::uint64_t> ifd_offsets;
    std::map<std::string, std::uint64_t> next_field_offsets;  // position of each IFD's next-IFD offset
    std::map<std::string, std::uint64_t> entry_offsets;
    std::map<std::string, std::uint64_t> value_offsets;              // inline field or out-of-line data
    std::vector<std::pair<std::uint64_t, std::uint64_t>> structure;  // [begin, end): header, IFD tables

    static std::string key(const std::string& path, std::uint16_t tag) {
        return path + "/" + std::to_string(tag);
    }

    std::uint64_t ifd(const std::string& path) const { return ifd_offsets.at(path); }
    std::uint64_t next_field(const std::string& path) const { return next_field_offsets.at(path); }
    std::uint64_t entry(const std::string& path, std::uint16_t tag) const {
        return entry_offsets.at(key(path, tag));
    }
    std::uint64_t value(const std::string& path, std::uint16_t tag) const {
        return value_offsets.at(key(path, tag));
    }

    bool big_tiff() const { return format == Format::BigTiff; }
    std::size_t offset_size() const { return big_tiff() ? 8 : 4; }
    std::size_t count_field_offset() const { return 4; }  // entry-relative position of the count
    std::size_t value_field_offset() const { return big_tiff() ? 12 : 8; }

    // Writes `value` as a `size`-byte integer in the stream's byte order.
    void put(std::uint64_t offset, std::uint64_t value, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            const std::size_t shift = endian == Endian::Little ? 8 * i : 8 * (size - 1 - i);
            bytes.at(static_cast<std::size_t>(offset) + i) = static_cast<std::byte>((value >> shift) & 0xFF);
        }
    }

    std::uint64_t get(std::uint64_t offset, std::size_t size) const {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < size; ++i) {
            const std::size_t shift = endian == Endian::Little ? 8 * i : 8 * (size - 1 - i);
            value |= std::to_integer<std::uint64_t>(bytes.at(static_cast<std::size_t>(offset) + i)) << shift;
        }
        return value;
    }

    std::span<const std::byte> span() const { return bytes; }
};

namespace detail {

inline constexpr std::uint16_t kGpsIfdTag = 34853;  // GPSInfo IFD pointer (EXIF)

class Writer {
public:
    explicit Writer(const Document& doc) {
        built_.endian = doc.endian;
        built_.format = doc.format;
        doc_ = &doc;
    }

    Built run() {
        const bool big = built_.big_tiff();
        const std::byte order = built_.endian == Endian::Little ? std::byte{'I'} : std::byte{'M'};
        append({order, order});
        append_int(big ? 43 : 42, 2);
        std::uint64_t link = 0;
        if (big) {
            append_int(8, 2);
            append_int(0, 2);
            link = built_.bytes.size();
            append_int(0, 8);
        } else {
            link = built_.bytes.size();
            append_int(0, 4);
        }
        built_.structure.emplace_back(0, built_.bytes.size());

        for (std::size_t i = 0; i < doc_->chain.size(); ++i) {
            const std::string path = "ifd" + std::to_string(i);
            const std::uint64_t offset = write_ifd(doc_->chain[i], path);
            built_.put(link, offset, built_.offset_size());
            link = built_.next_field(path);
        }
        return std::move(built_);
    }

private:
    void append(std::initializer_list<std::byte> bytes) {
        built_.bytes.insert(built_.bytes.end(), bytes.begin(), bytes.end());
    }
    void append_int(std::uint64_t value, std::size_t size) {
        const std::size_t at = built_.bytes.size();
        built_.bytes.resize(at + size);
        built_.put(at, value, size);
    }
    void align() {
        if (built_.bytes.size() % 2 != 0) {
            built_.bytes.push_back(std::byte{0});
        }
    }

    static std::uint64_t natural_count(const Entry& e) {
        if (!e.integers.empty()) {
            return e.integers.size();
        }
        if (!e.rationals.empty()) {
            return e.rationals.size();
        }
        if (!e.reals.empty()) {
            return e.reals.size();
        }
        return e.raw.size();
    }

    std::vector<std::byte> encode(const Entry& e) const {
        std::vector<std::byte> out;
        const auto push = [&](std::uint64_t value, std::size_t size) {
            for (std::size_t i = 0; i < size; ++i) {
                const std::size_t shift = built_.endian == Endian::Little ? 8 * i : 8 * (size - 1 - i);
                out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
            }
        };
        const std::uint32_t size = tiff_field_type_size(e.type);
        if (!e.integers.empty()) {
            for (const std::uint64_t v : e.integers) {
                push(v, size);
            }
        } else if (!e.rationals.empty()) {
            for (const auto& [n, d] : e.rationals) {
                push(n, 4);
                push(d, 4);
            }
        } else if (!e.reals.empty()) {
            for (const double v : e.reals) {
                if (e.type == type_code(TiffFieldType::Float)) {
                    push(std::bit_cast<std::uint32_t>(static_cast<float>(v)), 4);
                } else {
                    push(std::bit_cast<std::uint64_t>(v), 8);
                }
            }
        } else {
            out = e.raw;
        }
        return out;
    }

    std::uint64_t write_ifd(const Ifd& spec, const std::string& path) {
        const bool big = built_.big_tiff();
        const std::size_t count_size = big ? 8 : 2;
        const std::size_t entry_size = big ? 20 : 12;
        const std::size_t field_size = big ? 8 : 4;
        const std::size_t pointer_size = big ? 8 : 4;
        const auto pointer_type = big ? TiffFieldType::Ifd8 : TiffFieldType::Long;

        std::vector<Entry> entries = spec.entries;
        if (!spec.sub_ifds.empty()) {
            entries.push_back(integer_entry(tiff_tag::kSubIfds, pointer_type,
                                            std::vector<std::uint64_t>(spec.sub_ifds.size(), 0)));
        }
        if (!spec.exif.empty()) {
            entries.push_back(integer_entry(tiff_tag::kExifIfd, pointer_type, {0}));
        }
        if (!spec.gps.empty()) {
            entries.push_back(integer_entry(kGpsIfdTag, pointer_type, {0}));
        }
        if (spec.sort_entries) {
            std::stable_sort(entries.begin(), entries.end(),
                             [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
        }

        align();
        const std::uint64_t ifd_offset = built_.bytes.size();
        const std::size_t table_bytes = count_size + entries.size() * entry_size + field_size;
        built_.bytes.resize(built_.bytes.size() + table_bytes);
        built_.put(ifd_offset, entries.size(), count_size);
        built_.ifd_offsets[path] = ifd_offset;
        built_.next_field_offsets[path] = ifd_offset + count_size + entries.size() * entry_size;
        built_.structure.emplace_back(ifd_offset, ifd_offset + table_bytes);

        for (std::size_t k = 0; k < entries.size(); ++k) {
            const Entry& e = entries[k];
            const std::uint64_t position = ifd_offset + count_size + k * entry_size;
            const std::uint64_t field = position + (big ? 12 : 8);
            const std::string key = Built::key(path, e.tag);
            const std::vector<std::byte> data = encode(e);

            built_.put(position, e.tag, 2);
            built_.put(position + 2, e.type, 2);
            built_.put(position + 4, e.count_override.value_or(natural_count(e)), big ? 8 : 4);
            built_.entry_offsets[key] = position;

            if (data.size() <= field_size && !e.offset_override) {
                std::copy(data.begin(), data.end(),
                          built_.bytes.begin() + static_cast<std::ptrdiff_t>(field));
                built_.value_offsets[key] = field;
            } else if (e.offset_override) {
                built_.put(field, *e.offset_override, field_size);
                built_.value_offsets[key] = *e.offset_override;
            } else {
                align();
                const std::uint64_t value_offset = built_.bytes.size();
                built_.bytes.insert(built_.bytes.end(), data.begin(), data.end());
                built_.put(field, value_offset, field_size);
                built_.value_offsets[key] = value_offset;
            }
        }

        for (std::size_t i = 0; i < spec.sub_ifds.size(); ++i) {
            const std::uint64_t child = write_ifd(spec.sub_ifds[i], path + ".subifd" + std::to_string(i));
            built_.put(built_.value(path, tiff_tag::kSubIfds) + i * pointer_size, child, pointer_size);
        }
        if (!spec.exif.empty()) {
            const std::uint64_t child = write_ifd(spec.exif.front(), path + ".exif");
            built_.put(built_.value(path, tiff_tag::kExifIfd), child, pointer_size);
        }
        if (!spec.gps.empty()) {
            const std::uint64_t child = write_ifd(spec.gps.front(), path + ".gps");
            built_.put(built_.value(path, kGpsIfdTag), child, pointer_size);
        }
        return ifd_offset;
    }

    const Document* doc_ = nullptr;
    Built built_;
};

}  // namespace detail

inline Built build(const Document& doc) {
    return detail::Writer(doc).run();
}

// ---- Opcode lists (always big-endian, DNG 1.7.1.0 chapter 7) -------------------------------

struct OpcodeSpec {
    std::uint32_t id = 0;
    std::uint32_t version = 0x01030000;  // packed DNG version, 1.3.0.0
    std::uint32_t flags = 0;
    std::vector<std::byte> parameters;
    std::optional<std::uint32_t> parameter_bytes_override;
};

inline void put_be32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
    }
}

inline std::vector<std::byte> opcode_list(const std::vector<OpcodeSpec>& opcodes,
                                          std::optional<std::uint32_t> count_override = std::nullopt) {
    std::vector<std::byte> out;
    put_be32(out, count_override.value_or(static_cast<std::uint32_t>(opcodes.size())));
    for (const OpcodeSpec& op : opcodes) {
        put_be32(out, op.id);
        put_be32(out, op.version);
        put_be32(out, op.flags);
        put_be32(out, op.parameter_bytes_override.value_or(static_cast<std::uint32_t>(op.parameters.size())));
        out.insert(out.end(), op.parameters.begin(), op.parameters.end());
    }
    return out;
}

inline std::vector<std::byte> filler(std::size_t size, std::uint8_t seed = 0x5A) {
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((seed + 31 * i) & 0xFF);
    }
    return out;
}

// ---- Reference DNG ---------------------------------------------------------------------------
//
// A small but complete DNG: IFD 0 is an RGB thumbnail carrying the camera-profile tags, SubIFD 0
// is a 2x2 RGGB CFA raw image with levels, crops and all three opcode lists, SubIFD 1 is a YCbCr
// JPEG preview, and an Exif IFD hangs off IFD 0. The expected values used by the tests are the
// constants below.

namespace dng {
inline constexpr std::uint32_t kRawWidth = 4032;
inline constexpr std::uint32_t kRawHeight = 3024;

inline std::vector<std::byte> list1() {
    return opcode_list({{4, 0x01030000, 0, filler(8)}});  // FixBadPixelsConstant, mandatory
}
inline std::vector<std::byte> list2() {
    return opcode_list({
        {9, 0x01030000, 0, filler(76)},  // GainMap, mandatory
        {8, 0x01030000, 3, filler(36)},  // MapPolynomial, optional + preview-skippable
    });
}
inline std::vector<std::byte> list3() {
    return opcode_list({
        {14, 0x01060000, 1, filler(52)},  // WarpRectilinear2, optional
        {1, 0x01030000, 0, filler(52)},   // WarpRectilinear, mandatory, follows optional WR2
        {3, 0x01030000, 2, filler(56)},   // FixVignetteRadial, preview-skippable
    });
}

inline Ifd raw_ifd() {
    namespace t = tiff_tag;
    Ifd raw;
    raw.entries = {
        long_entry(t::kNewSubFileType, {0}),
        long_entry(t::kImageWidth, {kRawWidth}),
        long_entry(t::kImageLength, {kRawHeight}),
        short_entry(t::kBitsPerSample, {16}),
        short_entry(t::kCompression, {7}),
        short_entry(t::kPhotometricInterpretation, {32803}),
        short_entry(t::kSamplesPerPixel, {1}),
        short_entry(t::kPlanarConfiguration, {1}),
        short_entry(t::kCfaRepeatPatternDim, {2, 2}),
        byte_entry(t::kCfaPattern, {0, 1, 1, 2}),
        byte_entry(t::kCfaPlaneColor, {0, 1, 2}),
        short_entry(t::kCfaLayout, {1}),
        short_entry(t::kLinearizationTable, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}),
        short_entry(t::kBlackLevelRepeatDim, {2, 2}),
        rational_entry(t::kBlackLevel, {{256, 1}, {257, 1}, {258, 1}, {513, 2}}),
        long_entry(t::kWhiteLevel, {16383}),
        rational_entry(t::kDefaultCropOrigin, {{8, 1}, {8, 1}}),
        long_entry(t::kDefaultCropSize, {4000, 2992}),
        long_entry(t::kActiveArea, {12, 16, kRawHeight, kRawWidth}),
        undefined_entry(t::kOpcodeList1, list1()),
        undefined_entry(t::kOpcodeList2, list2()),
        undefined_entry(t::kOpcodeList3, list3()),
        double_entry(t::kNoiseProfile, {2.0e-5, 4.5e-7}),
        undefined_entry(t::kProfileGainTableMap, filler(24, 0x11)),
    };
    return raw;
}

inline Ifd preview_ifd() {
    namespace t = tiff_tag;
    Ifd preview;
    preview.entries = {
        long_entry(t::kNewSubFileType, {1}),   long_entry(t::kImageWidth, {1024}),
        long_entry(t::kImageLength, {768}),    short_entry(t::kBitsPerSample, {8, 8, 8}),
        short_entry(t::kCompression, {7}),     short_entry(t::kPhotometricInterpretation, {6}),
        short_entry(t::kSamplesPerPixel, {3}),
    };
    return preview;
}

inline Ifd exif_ifd() {
    Ifd exif;
    exif.entries = {
        rational_entry(33434, {{1, 250}}),  // ExposureTime
        short_entry(34855, {400}),          // ISOSpeedRatings
    };
    return exif;
}

inline Ifd ifd0() {
    namespace t = tiff_tag;
    Ifd ifd0;
    ifd0.entries = {
        long_entry(t::kNewSubFileType, {1}),
        long_entry(t::kImageWidth, {256}),
        long_entry(t::kImageLength, {192}),
        short_entry(t::kBitsPerSample, {8, 8, 8}),
        short_entry(t::kCompression, {1}),
        short_entry(t::kPhotometricInterpretation, {2}),
        ascii_entry(t::kMake, "Lucent  "),
        ascii_entry(t::kModel, "Synthetic One"),
        short_entry(t::kOrientation, {1}),
        short_entry(t::kSamplesPerPixel, {3}),
        byte_entry(t::kDngVersion, {1, 6, 0, 0}),
        byte_entry(t::kDngBackwardVersion, {1, 4, 0, 0}),
        ascii_entry(t::kUniqueCameraModel, "Lucent Synthetic One"),
        srational_entry(t::kColorMatrix1, {{8000, 10000},
                                           {-2000, 10000},
                                           {-1000, 10000},
                                           {-4000, 10000},
                                           {12000, 10000},
                                           {1500, 10000},
                                           {-500, 10000},
                                           {1800, 10000},
                                           {5500, 10000}}),
        srational_entry(t::kColorMatrix2, {{7000, 10000},
                                           {-1500, 10000},
                                           {-700, 10000},
                                           {-4500, 10000},
                                           {12500, 10000},
                                           {2000, 10000},
                                           {-800, 10000},
                                           {1500, 10000},
                                           {6500, 10000}}),
        srational_entry(t::kCameraCalibration1,
                        {{1, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 1}}),
        rational_entry(t::kAnalogBalance, {{1, 1}, {1, 1}, {1, 1}}),
        rational_entry(t::kAsShotNeutral, {{1, 2}, {1, 1}, {2, 3}}),
        srational_entry(t::kBaselineExposure, {{-1, 2}}),
        short_entry(t::kCalibrationIlluminant1, {17}),
        short_entry(t::kCalibrationIlluminant2, {21}),
        srational_entry(t::kForwardMatrix1, {{6000, 10000},
                                             {3000, 10000},
                                             {1000, 10000},
                                             {2500, 10000},
                                             {7000, 10000},
                                             {500, 10000},
                                             {0, 10000},
                                             {500, 10000},
                                             {7500, 10000}}),
    };
    ifd0.sub_ifds = {raw_ifd(), preview_ifd()};
    ifd0.exif = {exif_ifd()};
    return ifd0;
}

inline Document document(Endian endian = Endian::Little, Format format = Format::Classic) {
    Document doc;
    doc.endian = endian;
    doc.format = format;
    doc.chain = {ifd0()};
    return doc;
}

// Finds an entry in `ifd` by tag; throws when absent (a test-authoring error).
inline Entry& find(Ifd& ifd, std::uint16_t tag) {
    for (Entry& e : ifd.entries) {
        if (e.tag == tag) {
            return e;
        }
    }
    throw std::logic_error("tag not in test IFD: " + std::to_string(tag));
}

// Removes an entry from `ifd` by tag, if present.
inline void erase(Ifd& ifd, std::uint16_t tag) {
    std::erase_if(ifd.entries, [tag](const Entry& e) { return e.tag == tag; });
}

}  // namespace dng

}  // namespace lucent::raw::test_support
