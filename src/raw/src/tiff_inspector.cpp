#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <exception>
#include <format>
#include <iterator>
#include <limits>
#include <new>
#include <string>
#include <utility>

#include <lucent/raw/dng_tags.hpp>
#include <lucent/raw/tiff_inspector.hpp>

#include "tiff_reader.hpp"

namespace lucent::raw {

namespace {

using detail::describe_tag;
using detail::EntryStatus;
using detail::TiffEntry;
using detail::TiffIfd;
using detail::TiffReader;

constexpr std::uint64_t kMaxU16 = std::numeric_limits<std::uint16_t>::max();
constexpr std::uint64_t kMaxU32 = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t kAnyCount = std::numeric_limits<std::uint64_t>::max();

// DNG readers, including the reference DNG SDK, handle at most four colour planes.
constexpr std::uint32_t kMaxColorPlanes = 4;

// Accepted field types per value class. Integer tags accept every unsigned integer type and are
// then range-checked, so a writer's choice between SHORT and LONG never matters. Real-valued tags
// accept every numeric type; each value is decoded according to its declared type.
constexpr std::array kUnsignedTypes{TiffFieldType::Byte, TiffFieldType::Short, TiffFieldType::Long,
                                    TiffFieldType::Long8};
constexpr std::array kPointerTypes{TiffFieldType::Long, TiffFieldType::Ifd, TiffFieldType::Long8,
                                   TiffFieldType::Ifd8};
constexpr std::array kRealTypes{TiffFieldType::Byte,     TiffFieldType::Short,     TiffFieldType::Long,
                                TiffFieldType::Rational, TiffFieldType::SByte,     TiffFieldType::SShort,
                                TiffFieldType::SLong,    TiffFieldType::SRational, TiffFieldType::Float,
                                TiffFieldType::Double,   TiffFieldType::Long8,     TiffFieldType::SLong8};
constexpr std::array kAsciiTypes{TiffFieldType::Ascii};
constexpr std::array kByteTypes{TiffFieldType::Byte, TiffFieldType::Undefined};
constexpr std::array kShortTypes{TiffFieldType::Short};

using FieldTypes = std::span<const TiffFieldType>;

// An entry being interpreted, with the path of its IFD for messages.
struct TagRef {
    std::string_view path;
    const TiffEntry& entry;
};

// Pointers to child IFDs found while interpreting an IFD.
struct ChildIfds {
    std::vector<std::uint64_t> sub_ifds;
    std::optional<std::uint64_t> exif;
};

// Tags that DNG places in IFD 0, collected while IFD 0 is interpreted.
struct Ifd0Tags {
    std::optional<std::string> make;
    std::optional<std::string> model;
    std::optional<DngVersion> dng_version;
    std::optional<DngVersion> dng_backward_version;
    std::optional<std::string> unique_camera_model;
    DngColorInfo color;
    std::optional<TiffValueLocation> profile_gain_table_map2;
};

// An IFD waiting to be visited.
struct PendingIfd {
    std::uint64_t offset = 0;
    TiffIfdKind kind = TiffIfdKind::Main;
    std::optional<std::uint32_t> parent;
    std::uint32_t depth = 0;
    std::uint32_t chain_index = 0;
    std::string path;
};

enum class MatrixShape {
    ThreeColumns,  // ColorPlanes x 3 (ColorMatrix)
    ThreeRows,     // 3 x ColorPlanes (ForwardMatrix)
    Square,        // ColorPlanes x ColorPlanes (CameraCalibration)
};

// Decodes one numeric value of `type`. Returns nullopt for a rational with a zero denominator
// and for a non-finite floating-point value.
std::optional<double> decode_real(const TiffReader& reader, std::uint16_t type, const std::byte* p) {
    switch (static_cast<TiffFieldType>(type)) {
        case TiffFieldType::Byte:
            return static_cast<double>(std::to_integer<std::uint8_t>(p[0]));
        case TiffFieldType::SByte:
            return static_cast<double>(static_cast<std::int8_t>(std::to_integer<std::uint8_t>(p[0])));
        case TiffFieldType::Short:
            return static_cast<double>(reader.load_unsigned(p, 2));
        case TiffFieldType::SShort:
            return static_cast<double>(static_cast<std::int16_t>(reader.load_unsigned(p, 2)));
        case TiffFieldType::Long:
            return static_cast<double>(reader.load_unsigned(p, 4));
        case TiffFieldType::SLong:
            return static_cast<double>(static_cast<std::int32_t>(reader.load_unsigned(p, 4)));
        case TiffFieldType::Long8:
            return static_cast<double>(reader.load_unsigned(p, 8));
        case TiffFieldType::SLong8:
            return static_cast<double>(static_cast<std::int64_t>(reader.load_unsigned(p, 8)));
        case TiffFieldType::Rational: {
            const std::uint64_t denominator = reader.load_unsigned(p + 4, 4);
            if (denominator == 0) {
                return std::nullopt;
            }
            return static_cast<double>(reader.load_unsigned(p, 4)) / static_cast<double>(denominator);
        }
        case TiffFieldType::SRational: {
            const auto denominator = static_cast<std::int32_t>(reader.load_unsigned(p + 4, 4));
            if (denominator == 0) {
                return std::nullopt;
            }
            const auto numerator = static_cast<std::int32_t>(reader.load_unsigned(p, 4));
            return static_cast<double>(numerator) / static_cast<double>(denominator);
        }
        case TiffFieldType::Float: {
            const auto value = std::bit_cast<float>(static_cast<std::uint32_t>(reader.load_unsigned(p, 4)));
            if (!std::isfinite(value)) {
                return std::nullopt;
            }
            return static_cast<double>(value);
        }
        case TiffFieldType::Double: {
            const auto value = std::bit_cast<double>(reader.load_unsigned(p, 8));
            if (!std::isfinite(value)) {
                return std::nullopt;
            }
            return value;
        }
        default:
            return std::nullopt;
    }
}

// TIFF ASCII: text up to the first NUL. Bytes outside printable 7-bit ASCII become '?', so the
// result is always valid UTF-8; trailing spaces (common padding in Make/Model) are removed.
std::string to_text(std::span<const std::byte> bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const std::byte b : bytes) {
        const auto c = std::to_integer<unsigned char>(b);
        if (c == 0) {
            break;
        }
        text.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
    }
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
}

// In-memory size of a numeric value decoded to 8-byte elements: the larger of the stored size
// and 8 bytes per value (saturating). Precondition: the entry size did not overflow.
std::uint64_t decoded_size(const TiffEntry& entry) {
    constexpr std::uint64_t kDecodedBytes = 8;
    const std::uint64_t decoded =
        entry.count > kAnyCount / kDecodedBytes ? kAnyCount : entry.count * kDecodedBytes;
    return std::max(decoded, entry.byte_count);
}

// Stores a successfully decoded value through `store`; forwards the error otherwise.
template <class T, class Store>
Expected<bool> assign(Expected<T> value, Store&& store) {
    if (!value) {
        return Unexpected(std::move(value.error()));
    }
    std::forward<Store>(store)(std::move(*value));
    return true;
}

// Reports a successfully completed interpretation step; forwards the error otherwise.
Expected<bool> completed(Status status) {
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    return true;
}

class Inspector {
public:
    Inspector(TiffReader& reader, const TiffInspectLimits& limits) : reader_(reader), limits_(limits) {}

    Expected<TiffSummary> run();

private:
    Status walk();
    Status interpret_ifd(const TiffIfd& ifd, bool is_ifd0, TiffIfdSummary& summary, ChildIfds& children);
    Expected<bool> interpret_entry(const TagRef& tag, TiffIfdSummary& summary, ChildIfds& children);
    Expected<bool> interpret_ifd0_entry(const TagRef& tag);
    Status parse_opcode_list(const TagRef& tag, DngOpcodeListId list, TiffIfdSummary& summary);
    Status validate_ifd(TiffIfdSummary& summary) const;
    Status finish();
    Status validate_color(const DngInfo& dng, const TiffIfdSummary& raw) const;

    // Typed access. Each call verifies the field type and count, then reads and decodes the
    // value; any violation is a Corrupt error naming the IFD and tag.
    Status check_shape(const TagRef& tag, FieldTypes types, std::uint64_t min_count,
                       std::uint64_t max_count) const;
    Expected<std::vector<std::uint64_t>> unsigneds(const TagRef& tag, FieldTypes types,
                                                   std::uint64_t min_count, std::uint64_t max_count,
                                                   std::uint64_t max_value);
    Expected<std::uint64_t> unsigned_one(const TagRef& tag, FieldTypes types, std::uint64_t max_value);
    Expected<std::vector<double>> reals(const TagRef& tag, std::uint64_t min_count, std::uint64_t max_count);
    Expected<std::vector<double>> positive_reals(const TagRef& tag, std::uint64_t min_count,
                                                 std::uint64_t max_count);
    Expected<std::string> ascii(const TagRef& tag);
    Expected<std::vector<std::byte>> bytes(const TagRef& tag, FieldTypes types, std::uint64_t min_count,
                                           std::uint64_t max_count);
    Expected<TiffValueLocation> locate(const TagRef& tag, FieldTypes types, std::uint64_t min_count,
                                       std::uint64_t max_count) const;
    Expected<DngMatrix> matrix(const TagRef& tag, MatrixShape shape);

    // Charges the in-memory size of a value against the per-tag and total value budgets.
    Status charge(const TagRef& tag, std::uint64_t bytes);

    Unexpected tag_error(const TagRef& tag, std::string_view what) const;
    void warn(std::string_view where, std::string message);

    TiffReader& reader_;
    const TiffInspectLimits& limits_;
    TiffSummary summary_;
    Ifd0Tags ifd0_;
    std::uint64_t charged_bytes_ = 0;
};

Unexpected Inspector::tag_error(const TagRef& tag, std::string_view what) const {
    return fail(ErrorCode::Corrupt, std::format("{}: tag {} {}", tag.path, describe_tag(tag.entry.tag), what),
                std::string(detail::kWhereTag));
}

void Inspector::warn(std::string_view where, std::string message) {
    if (summary_.warnings.size() < limits_.max_warnings) {
        summary_.warnings.push_back(TiffWarning{std::string(where), std::move(message)});
    } else if (summary_.suppressed_warnings < std::numeric_limits<std::uint32_t>::max()) {
        ++summary_.suppressed_warnings;
    }
}

Status Inspector::charge(const TagRef& tag, std::uint64_t bytes) {
    if (bytes > limits_.max_value_bytes) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("{}: tag {} value needs {} bytes in memory; the per-tag limit is {}",
                                tag.path, describe_tag(tag.entry.tag), bytes, limits_.max_value_bytes),
                    std::string(detail::kWhereLimits));
    }
    if (bytes > limits_.max_total_value_bytes - charged_bytes_) {
        return fail(
            ErrorCode::LimitExceeded,
            std::format("{}: tag {} value needs {} bytes in memory; {} of the {}-byte total budget remain",
                        tag.path, describe_tag(tag.entry.tag), bytes,
                        limits_.max_total_value_bytes - charged_bytes_, limits_.max_total_value_bytes),
            std::string(detail::kWhereLimits));
    }
    charged_bytes_ += bytes;
    return ok();
}

Status Inspector::check_shape(const TagRef& tag, FieldTypes types, std::uint64_t min_count,
                              std::uint64_t max_count) const {
    const auto type = static_cast<TiffFieldType>(tag.entry.type);
    if (std::find(types.begin(), types.end(), type) == types.end()) {
        return tag_error(tag, std::format("has field type {} ({}), which this tag does not allow",
                                          tag.entry.type, tiff_field_type_name(tag.entry.type)));
    }
    const std::uint64_t count = tag.entry.count;
    if (count < min_count || count > max_count) {
        if (min_count == max_count) {
            return tag_error(tag, std::format("has {} values; expected {}", count, min_count));
        }
        if (max_count == kAnyCount) {
            return tag_error(tag, std::format("has {} values; expected at least {}", count, min_count));
        }
        return tag_error(tag, std::format("has {} values; expected {} to {}", count, min_count, max_count));
    }
    if (tag.entry.status != EntryStatus::Ok) {
        return Unexpected(reader_.entry_error(tag.entry, tag.path));
    }
    return ok();
}

Expected<std::vector<std::uint64_t>> Inspector::unsigneds(const TagRef& tag, FieldTypes types,
                                                          std::uint64_t min_count, std::uint64_t max_count,
                                                          std::uint64_t max_value) {
    if (auto shape = check_shape(tag, types, min_count, max_count); !shape) {
        return Unexpected(std::move(shape.error()));
    }
    if (auto budget = charge(tag, decoded_size(tag.entry)); !budget) {
        return Unexpected(std::move(budget.error()));
    }
    auto raw = reader_.read_value(tag.entry, limits_.max_value_bytes, tag.path);
    if (!raw) {
        return Unexpected(std::move(raw.error()));
    }
    const std::size_t size = tiff_field_type_size(tag.entry.type);
    const std::size_t count = raw->size() / size;
    std::vector<std::uint64_t> values;
    values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t value = reader_.load_unsigned(raw->data() + i * size, size);
        if (value > max_value) {
            return tag_error(tag,
                             std::format("value {} at index {} exceeds the maximum {}", value, i, max_value));
        }
        values.push_back(value);
    }
    return values;
}

Expected<std::uint64_t> Inspector::unsigned_one(const TagRef& tag, FieldTypes types,
                                                std::uint64_t max_value) {
    auto values = unsigneds(tag, types, 1, 1, max_value);
    if (!values) {
        return Unexpected(std::move(values.error()));
    }
    return values->front();
}

Expected<std::vector<double>> Inspector::reals(const TagRef& tag, std::uint64_t min_count,
                                               std::uint64_t max_count) {
    if (auto shape = check_shape(tag, kRealTypes, min_count, max_count); !shape) {
        return Unexpected(std::move(shape.error()));
    }
    if (auto budget = charge(tag, decoded_size(tag.entry)); !budget) {
        return Unexpected(std::move(budget.error()));
    }
    auto raw = reader_.read_value(tag.entry, limits_.max_value_bytes, tag.path);
    if (!raw) {
        return Unexpected(std::move(raw.error()));
    }
    const std::size_t size = tiff_field_type_size(tag.entry.type);
    const std::size_t count = raw->size() / size;
    std::vector<double> values;
    values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto value = decode_real(reader_, tag.entry.type, raw->data() + i * size);
        if (!value) {
            return tag_error(tag,
                             std::format("value at index {} is not a finite number (zero denominator, NaN "
                                         "or infinity)",
                                         i));
        }
        values.push_back(*value);
    }
    return values;
}

Expected<std::vector<double>> Inspector::positive_reals(const TagRef& tag, std::uint64_t min_count,
                                                        std::uint64_t max_count) {
    auto values = reals(tag, min_count, max_count);
    if (!values) {
        return values;
    }
    for (std::size_t i = 0; i < values->size(); ++i) {
        if (!((*values)[i] > 0.0)) {
            return tag_error(tag,
                             std::format("value {} at index {} must be greater than zero", (*values)[i], i));
        }
    }
    return values;
}

Expected<std::string> Inspector::ascii(const TagRef& tag) {
    auto raw = bytes(tag, kAsciiTypes, 1, kAnyCount);
    if (!raw) {
        return Unexpected(std::move(raw.error()));
    }
    return to_text(*raw);
}

Expected<std::vector<std::byte>> Inspector::bytes(const TagRef& tag, FieldTypes types,
                                                  std::uint64_t min_count, std::uint64_t max_count) {
    if (auto shape = check_shape(tag, types, min_count, max_count); !shape) {
        return Unexpected(std::move(shape.error()));
    }
    if (auto budget = charge(tag, tag.entry.byte_count); !budget) {
        return Unexpected(std::move(budget.error()));
    }
    return reader_.read_value(tag.entry, limits_.max_value_bytes, tag.path);
}

Expected<TiffValueLocation> Inspector::locate(const TagRef& tag, FieldTypes types, std::uint64_t min_count,
                                              std::uint64_t max_count) const {
    if (auto shape = check_shape(tag, types, min_count, max_count); !shape) {
        return Unexpected(std::move(shape.error()));
    }
    return TiffValueLocation{tag.entry.type, tag.entry.count, tag.entry.value_offset, tag.entry.byte_count};
}

Expected<DngMatrix> Inspector::matrix(const TagRef& tag, MatrixShape shape) {
    const std::uint64_t max_count =
        shape == MatrixShape::Square ? kMaxColorPlanes * kMaxColorPlanes : 3 * kMaxColorPlanes;
    const std::uint64_t min_count = shape == MatrixShape::Square ? 1 : 3;
    auto values = reals(tag, min_count, max_count);
    if (!values) {
        return Unexpected(std::move(values.error()));
    }
    const auto count = static_cast<std::uint32_t>(values->size());
    DngMatrix m;
    switch (shape) {
        case MatrixShape::ThreeColumns:
        case MatrixShape::ThreeRows: {
            if (count % 3 != 0) {
                return tag_error(
                    tag, std::format("has {} values; expected 3 x ColorPlanes (3, 6, 9 or 12)", count));
            }
            const bool three_columns = shape == MatrixShape::ThreeColumns;
            m.rows = three_columns ? count / 3 : 3;
            m.cols = three_columns ? 3 : count / 3;
            break;
        }
        case MatrixShape::Square: {
            std::uint32_t side = 1;
            while (side * side < count) {
                ++side;
            }
            if (side * side != count) {
                return tag_error(
                    tag,
                    std::format("has {} values; expected ColorPlanes x ColorPlanes (1, 4, 9 or 16)", count));
            }
            m.rows = side;
            m.cols = side;
            break;
        }
    }
    m.values = std::move(*values);
    return m;
}

Status Inspector::parse_opcode_list(const TagRef& tag, DngOpcodeListId list, TiffIfdSummary& summary) {
    auto location = locate(tag, kByteTypes, 0, kAnyCount);
    if (!location) {
        return Unexpected(std::move(location.error()));
    }
    auto payload = bytes(tag, kByteTypes, 0, kAnyCount);
    if (!payload) {
        return Unexpected(std::move(payload.error()));
    }
    auto opcodes = parse_dng_opcode_list(*payload, list, limits_.max_opcodes_per_list);
    if (!opcodes) {
        Error error = std::move(opcodes.error());
        error.message = std::format("{}: {} (list at offset {})", tag.path, error.message, location->offset);
        return Unexpected(std::move(error));
    }
    // The decoded parameters stay in the summary: charge about their size (at most the payload's)
    // a second time, besides the payload read above.
    if (auto budget = charge(tag, tag.entry.byte_count); !budget) {
        return Unexpected(std::move(budget.error()));
    }
    if (auto decoded = decode_dng_opcode_parameters(*opcodes, *payload); !decoded) {
        Error error = std::move(decoded.error());
        error.message = std::format("{}: {} (list at offset {})", tag.path, error.message, location->offset);
        return Unexpected(std::move(error));
    }
    const auto index = static_cast<std::size_t>(list) - 1;
    summary.opcode_lists[index] = *location;
    summary.opcodes.insert(summary.opcodes.end(), std::make_move_iterator(opcodes->begin()),
                           std::make_move_iterator(opcodes->end()));
    return ok();
}

Expected<bool> Inspector::interpret_entry(const TagRef& tag, TiffIfdSummary& s, ChildIfds& children) {
    namespace t = tiff_tag;
    const auto u16 = [](std::uint64_t v) { return static_cast<std::uint16_t>(v); };
    const auto u32 = [](std::uint64_t v) { return static_cast<std::uint32_t>(v); };

    switch (tag.entry.tag) {
        case t::kNewSubFileType:
            return assign(unsigned_one(tag, kUnsignedTypes, kMaxU32),
                          [&](std::uint64_t v) { s.new_subfile_type = u32(v); });
        case t::kImageWidth:
            return assign(unsigned_one(tag, kUnsignedTypes, kMaxU32),
                          [&](std::uint64_t v) { s.width = u32(v); });
        case t::kImageLength:
            return assign(unsigned_one(tag, kUnsignedTypes, kMaxU32),
                          [&](std::uint64_t v) { s.height = u32(v); });
        case t::kBitsPerSample:
            return assign(unsigneds(tag, kUnsignedTypes, 1, kMaxU16, kMaxU16),
                          [&](std::vector<std::uint64_t> v) {
                              std::transform(v.begin(), v.end(), std::back_inserter(s.bits_per_sample), u16);
                          });
        case t::kCompression:
            return assign(unsigned_one(tag, kUnsignedTypes, kMaxU16),
                          [&](std::uint64_t v) { s.compression = u16(v); });
        case t::kPhotometricInterpretation:
            return assign(unsigned_one(tag, kUnsignedTypes, kMaxU16),
                          [&](std::uint64_t v) { s.photometric = u16(v); });
        case t::kOrientation:
            return assign(unsigned_one(tag, kUnsignedTypes, kMaxU16),
                          [&](std::uint64_t v) { s.orientation = u16(v); });
        case t::kSamplesPerPixel: {
            auto value = unsigned_one(tag, kUnsignedTypes, kMaxU16);
            if (value && *value == 0) {
                return tag_error(tag, "is 0; at least one sample per pixel is required");
            }
            return assign(std::move(value), [&](std::uint64_t v) { s.samples_per_pixel = u16(v); });
        }
        case t::kPlanarConfiguration:
            return assign(unsigned_one(tag, kUnsignedTypes, kMaxU16),
                          [&](std::uint64_t v) { s.planar_configuration = u16(v); });
        case t::kSampleFormat:
            return assign(unsigneds(tag, kUnsignedTypes, 1, kMaxU16, kMaxU16),
                          [&](std::vector<std::uint64_t> v) {
                              std::transform(v.begin(), v.end(), std::back_inserter(s.sample_format), u16);
                          });
        case t::kJxlDistance: {
            // Narrowing an out-of-range double to float is undefined behaviour; reject it first.
            auto distance = reals(tag, 1, 1);
            if (distance && std::abs(distance->front()) > std::numeric_limits<float>::max()) {
                return tag_error(tag,
                                 std::format("value {} is outside the range of FLOAT", distance->front()));
            }
            return assign(std::move(distance),
                          [&](std::vector<double> v) { s.jxl_distance = static_cast<float>(v.front()); });
        }
        case t::kSubIfds:
            // Each SubIFD is visited, so more pointers than the IFD budget can never be valid.
            return assign(unsigneds(tag, kPointerTypes, 1, limits_.max_ifds, kAnyCount),
                          [&](std::vector<std::uint64_t> v) { children.sub_ifds = std::move(v); });
        case t::kExifIfd:
            return assign(unsigned_one(tag, kPointerTypes, kAnyCount),
                          [&](std::uint64_t v) { children.exif = v; });
        case t::kCfaRepeatPatternDim: {
            auto dims = unsigneds(tag, kUnsignedTypes, 2, 2, kMaxU16);
            if (dims && ((*dims)[0] == 0 || (*dims)[1] == 0)) {
                return tag_error(tag, std::format("is {} x {}; both dimensions must be at least 1",
                                                  (*dims)[0], (*dims)[1]));
            }
            return assign(std::move(dims), [&](std::vector<std::uint64_t> v) {
                s.cfa_repeat_dim = std::array<std::uint16_t, 2>{u16(v[0]), u16(v[1])};
            });
        }
        case t::kCfaPattern:
            return assign(bytes(tag, kByteTypes, 1, kAnyCount), [&](std::vector<std::byte> v) {
                std::transform(v.begin(), v.end(), std::back_inserter(s.cfa_pattern),
                               [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
            });
        case t::kCfaPlaneColor:
            return assign(bytes(tag, kByteTypes, 1, kAnyCount), [&](std::vector<std::byte> v) {
                std::transform(v.begin(), v.end(), std::back_inserter(s.cfa_plane_color),
                               [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
            });
        case t::kCfaLayout:
            return assign(unsigned_one(tag, kUnsignedTypes, kMaxU16),
                          [&](std::uint64_t v) { s.cfa_layout = u16(v); });
        case t::kActiveArea: {
            auto area = unsigneds(tag, kUnsignedTypes, 4, 4, kMaxU32);
            if (area && ((*area)[0] >= (*area)[2] || (*area)[1] >= (*area)[3])) {
                return tag_error(tag, std::format("top {}, left {}, bottom {}, right {} is empty or inverted",
                                                  (*area)[0], (*area)[1], (*area)[2], (*area)[3]));
            }
            return assign(std::move(area), [&](std::vector<std::uint64_t> v) {
                s.active_area = DngRect{u32(v[0]), u32(v[1]), u32(v[2]), u32(v[3])};
            });
        }
        case t::kDefaultCropOrigin: {
            auto origin = reals(tag, 2, 2);
            if (origin && ((*origin)[0] < 0.0 || (*origin)[1] < 0.0)) {
                return tag_error(tag, "has a negative component");
            }
            return assign(std::move(origin), [&](std::vector<double> v) {
                s.default_crop_origin = std::array<double, 2>{v[0], v[1]};
            });
        }
        case t::kDefaultCropSize:
            return assign(positive_reals(tag, 2, 2), [&](std::vector<double> v) {
                s.default_crop_size = std::array<double, 2>{v[0], v[1]};
            });
        case t::kDefaultUserCrop: {
            // Advisory (a user crop's initial value): a malformed tag is skipped with a warning.
            auto crop = reals(tag, 4, 4);
            if (!crop && crop.error().code != ErrorCode::Corrupt) {
                return Unexpected(std::move(crop.error()));  // limits and I/O errors stay errors
            }
            if (!crop) {
                warn(detail::kWhereTag, std::format("{}: tag {} skipped: {}", tag.path,
                                                    describe_tag(tag.entry.tag), crop.error().message));
                return true;
            }
            const std::vector<double>& v = *crop;
            const auto unit = [](double x) { return std::isfinite(x) && x >= 0.0 && x <= 1.0; };
            if (!std::all_of(v.begin(), v.end(), unit) || !(v[0] < v[2]) || !(v[1] < v[3])) {
                warn(detail::kWhereTag,
                     std::format("{}: tag {} skipped: top {}, left {}, bottom {}, right {} is not ordered "
                                 "inside [0, 1]",
                                 tag.path, describe_tag(tag.entry.tag), v[0], v[1], v[2], v[3]));
                return true;
            }
            s.default_user_crop = std::array<double, 4>{v[0], v[1], v[2], v[3]};
            return true;
        }
        case t::kBlackLevelRepeatDim: {
            auto dims = unsigneds(tag, kUnsignedTypes, 1, 2, kMaxU16);
            if (dims && dims->size() == 1) {
                // DNG requires two values. Some writers (the Pentax K-3 Mark III Monochrome) store
                // the single value 1, which has only one consistent reading: no repeat pattern.
                // Any other single value is ambiguous and stays an error.
                if ((*dims)[0] != 1) {
                    return tag_error(tag, std::format("has 1 value ({}); expected 2", (*dims)[0]));
                }
                warn(detail::kWhereTag, std::format("{}: tag {} has 1 value (1) instead of 2; read as 1 x 1",
                                                    tag.path, describe_tag(tag.entry.tag)));
                dims->push_back(1);
            }
            if (dims && ((*dims)[0] == 0 || (*dims)[1] == 0)) {
                return tag_error(tag, std::format("is {} x {}; both dimensions must be at least 1",
                                                  (*dims)[0], (*dims)[1]));
            }
            return assign(std::move(dims), [&](std::vector<std::uint64_t> v) {
                s.black_level_repeat_dim = std::array<std::uint16_t, 2>{u16(v[0]), u16(v[1])};
            });
        }
        case t::kBlackLevel:
            return assign(reals(tag, 1, kAnyCount),
                          [&](std::vector<double> v) { s.black_level = std::move(v); });
        case t::kWhiteLevel:
            return assign(unsigneds(tag, kUnsignedTypes, 1, kMaxU16, kMaxU32),
                          [&](std::vector<std::uint64_t> v) {
                              std::transform(v.begin(), v.end(), std::back_inserter(s.white_level), u32);
                          });
        case t::kBlackLevelDeltaH:
            // Counts are verified against the ActiveArea in validate_ifd().
            return assign(reals(tag, 1, kAnyCount),
                          [&](std::vector<double> v) { s.black_level_delta_h = std::move(v); });
        case t::kBlackLevelDeltaV:
            return assign(reals(tag, 1, kAnyCount),
                          [&](std::vector<double> v) { s.black_level_delta_v = std::move(v); });
        case t::kMaskedAreas: {
            auto values = unsigneds(tag, kUnsignedTypes, 4, kAnyCount, kMaxU32);
            if (values && values->size() % 4 != 0) {
                return tag_error(tag, std::format("has {} values; expected groups of 4 (top, left, bottom, "
                                                  "right)",
                                                  values->size()));
            }
            if (values) {
                for (std::size_t i = 0; i < values->size(); i += 4) {
                    const auto& v = *values;
                    if (v[i] >= v[i + 2] || v[i + 1] >= v[i + 3]) {
                        return tag_error(tag,
                                         std::format("rectangle {} (top {}, left {}, bottom {}, right {}) is "
                                                     "empty or inverted",
                                                     i / 4, v[i], v[i + 1], v[i + 2], v[i + 3]));
                    }
                }
            }
            return assign(std::move(values), [&](std::vector<std::uint64_t> v) {
                for (std::size_t i = 0; i < v.size(); i += 4) {
                    s.masked_areas.push_back(DngRect{u32(v[i]), u32(v[i + 1]), u32(v[i + 2]), u32(v[i + 3])});
                }
            });
        }
        case t::kLinearizationTable:
            return assign(locate(tag, kShortTypes, 1, kAnyCount),
                          [&](TiffValueLocation v) { s.linearization_table = v; });
        case t::kProfileGainTableMap:
            return assign(locate(tag, kByteTypes, 1, kAnyCount),
                          [&](TiffValueLocation v) { s.profile_gain_table_map = v; });
        case t::kNoiseProfile: {
            auto profile = reals(tag, 2, 2 * kMaxColorPlanes);
            if (profile) {
                if (profile->size() % 2 != 0) {
                    return tag_error(tag,
                                     std::format("has {} values; expected pairs (S, O)", profile->size()));
                }
                for (std::size_t i = 0; i < profile->size(); i += 2) {
                    if (!((*profile)[i] > 0.0) || (*profile)[i + 1] < 0.0) {
                        return tag_error(tag, std::format("pair {} (S {}, O {}) violates S > 0 and O >= 0",
                                                          i / 2, (*profile)[i], (*profile)[i + 1]));
                    }
                }
            }
            return assign(std::move(profile), [&](std::vector<double> v) { s.noise_profile = std::move(v); });
        }
        case t::kOpcodeList1:
            return completed(parse_opcode_list(tag, DngOpcodeListId::List1, s));
        case t::kOpcodeList2:
            return completed(parse_opcode_list(tag, DngOpcodeListId::List2, s));
        case t::kOpcodeList3:
            return completed(parse_opcode_list(tag, DngOpcodeListId::List3, s));
        default:
            return false;
    }
}

Expected<bool> Inspector::interpret_ifd0_entry(const TagRef& tag) {
    namespace t = tiff_tag;
    DngColorInfo& color = ifd0_.color;
    const auto version = [](std::vector<std::byte> v) {
        return DngVersion{{std::to_integer<std::uint8_t>(v[0]), std::to_integer<std::uint8_t>(v[1]),
                           std::to_integer<std::uint8_t>(v[2]), std::to_integer<std::uint8_t>(v[3])}};
    };
    const auto illuminant = [&](std::size_t i) {
        return assign(unsigned_one(tag, kUnsignedTypes, kMaxU16), [&](std::uint64_t v) {
            color.calibration_illuminant[i] = static_cast<std::uint16_t>(v);
        });
    };
    const auto matrix_into = [&](std::array<std::optional<DngMatrix>, 3>& target, std::size_t i,
                                 MatrixShape shape) {
        return assign(matrix(tag, shape), [&](DngMatrix m) { target[i] = std::move(m); });
    };
    const auto illuminant_data = [&](std::size_t i) {
        return assign(locate(tag, kByteTypes, 1, kAnyCount),
                      [&](TiffValueLocation v) { color.illuminant_data[i] = v; });
    };

    switch (tag.entry.tag) {
        case t::kMake:
            return assign(ascii(tag), [&](std::string v) { ifd0_.make = std::move(v); });
        case t::kModel:
            return assign(ascii(tag), [&](std::string v) { ifd0_.model = std::move(v); });
        case t::kDngVersion:
            return assign(bytes(tag, std::array{TiffFieldType::Byte}, 4, 4),
                          [&](std::vector<std::byte> v) { ifd0_.dng_version = version(std::move(v)); });
        case t::kDngBackwardVersion:
            return assign(bytes(tag, std::array{TiffFieldType::Byte}, 4, 4), [&](std::vector<std::byte> v) {
                ifd0_.dng_backward_version = version(std::move(v));
            });
        case t::kUniqueCameraModel:
            return assign(ascii(tag), [&](std::string v) { ifd0_.unique_camera_model = std::move(v); });
        case t::kCalibrationIlluminant1:
            return illuminant(0);
        case t::kCalibrationIlluminant2:
            return illuminant(1);
        case t::kCalibrationIlluminant3:
            return illuminant(2);
        case t::kColorMatrix1:
            return matrix_into(color.color_matrix, 0, MatrixShape::ThreeColumns);
        case t::kColorMatrix2:
            return matrix_into(color.color_matrix, 1, MatrixShape::ThreeColumns);
        case t::kColorMatrix3:
            return matrix_into(color.color_matrix, 2, MatrixShape::ThreeColumns);
        case t::kCameraCalibration1:
            return matrix_into(color.camera_calibration, 0, MatrixShape::Square);
        case t::kCameraCalibration2:
            return matrix_into(color.camera_calibration, 1, MatrixShape::Square);
        case t::kCameraCalibration3:
            return matrix_into(color.camera_calibration, 2, MatrixShape::Square);
        case t::kForwardMatrix1:
            return matrix_into(color.forward_matrix, 0, MatrixShape::ThreeRows);
        case t::kForwardMatrix2:
            return matrix_into(color.forward_matrix, 1, MatrixShape::ThreeRows);
        case t::kForwardMatrix3:
            return matrix_into(color.forward_matrix, 2, MatrixShape::ThreeRows);
        case t::kIlluminantData1:
            return illuminant_data(0);
        case t::kIlluminantData2:
            return illuminant_data(1);
        case t::kIlluminantData3:
            return illuminant_data(2);
        case t::kAnalogBalance:
            return assign(positive_reals(tag, 1, kMaxColorPlanes),
                          [&](std::vector<double> v) { color.analog_balance = std::move(v); });
        case t::kAsShotNeutral:
            return assign(positive_reals(tag, 1, kMaxColorPlanes),
                          [&](std::vector<double> v) { color.as_shot_neutral = std::move(v); });
        case t::kAsShotWhiteXY:
            return assign(positive_reals(tag, 2, 2), [&](std::vector<double> v) {
                color.as_shot_white_xy = std::array<double, 2>{v[0], v[1]};
            });
        case t::kBaselineExposure:
            return assign(reals(tag, 1, 1),
                          [&](std::vector<double> v) { color.baseline_exposure = v.front(); });
        case t::kProfileGainTableMap2:
            return assign(locate(tag, kByteTypes, 1, kAnyCount),
                          [&](TiffValueLocation v) { ifd0_.profile_gain_table_map2 = v; });
        default:
            return false;
    }
}

Status Inspector::interpret_ifd(const TiffIfd& ifd, bool is_ifd0, TiffIfdSummary& summary,
                                ChildIfds& children) {
    std::vector<std::uint16_t> interpreted;
    bool reported_unsorted = false;
    std::optional<std::uint16_t> previous_tag;

    for (const TiffEntry& entry : ifd.entries) {
        if (previous_tag && entry.tag <= *previous_tag && !reported_unsorted) {
            warn(detail::kWhereIfd,
                 std::format("{}: entries are not in strictly ascending tag order (tag {} follows {})",
                             summary.path, entry.tag, *previous_tag));
            reported_unsorted = true;
        }
        previous_tag = entry.tag;

        if (entry.status == EntryStatus::UnknownType) {
            warn(detail::kWhereEntry, std::format("{}: skipped tag {} with undefined field type {}",
                                                  summary.path, describe_tag(entry.tag), entry.type));
            continue;
        }

        const TagRef tag{summary.path, entry};
        auto handled = interpret_entry(tag, summary, children);
        if (handled && !*handled && is_ifd0) {
            handled = interpret_ifd0_entry(tag);
        }
        if (!handled) {
            return Unexpected(std::move(handled.error()));
        }
        if (*handled) {
            if (std::find(interpreted.begin(), interpreted.end(), entry.tag) != interpreted.end()) {
                return tag_error(tag, "appears more than once in the IFD");
            }
            interpreted.push_back(entry.tag);
        } else if (entry.status != EntryStatus::Ok) {
            warn(detail::kWhereEntry, std::format("{}; the tag is not interpreted and was ignored",
                                                  reader_.entry_error(entry, summary.path).message));
        }
    }
    return ok();
}

Status Inspector::validate_ifd(TiffIfdSummary& s) const {
    const auto fail_here = [&](std::string message) {
        return fail(ErrorCode::Corrupt, std::format("{}: {}", s.path, message),
                    std::string(detail::kWhereTag));
    };

    if (s.cfa_repeat_dim && !s.cfa_pattern.empty()) {
        const std::size_t expected = std::size_t{(*s.cfa_repeat_dim)[0]} * (*s.cfa_repeat_dim)[1];
        if (s.cfa_pattern.size() != expected) {
            return fail_here(std::format("CFAPattern has {} values but CFARepeatPatternDim is {} x {}",
                                         s.cfa_pattern.size(), (*s.cfa_repeat_dim)[0],
                                         (*s.cfa_repeat_dim)[1]));
        }
    }

    if (s.active_area) {
        if (s.height && s.active_area->bottom > *s.height) {
            return fail_here(
                std::format("ActiveArea bottom {} exceeds ImageLength {}", s.active_area->bottom, *s.height));
        }
        if (s.width && s.active_area->right > *s.width) {
            return fail_here(
                std::format("ActiveArea right {} exceeds ImageWidth {}", s.active_area->right, *s.width));
        }
    }

    const std::uint64_t samples = s.samples_per_pixel.value_or(1);
    if (!s.black_level.empty()) {
        const std::uint64_t rows = s.black_level_repeat_dim ? (*s.black_level_repeat_dim)[0] : 1;
        const std::uint64_t cols = s.black_level_repeat_dim ? (*s.black_level_repeat_dim)[1] : 1;
        const std::uint64_t expected = rows * cols * samples;
        if (s.black_level.size() != expected) {
            return fail_here(std::format(
                "BlackLevel has {} values; BlackLevelRepeatDim {} x {} and SamplesPerPixel {} require {}",
                s.black_level.size(), rows, cols, samples, expected));
        }
    }
    if (!s.white_level.empty() && s.white_level.size() != samples) {
        return fail_here(std::format("WhiteLevel has {} values; SamplesPerPixel {} requires {}",
                                     s.white_level.size(), samples, samples));
    }

    // DNG 1.7.1.0: BlackLevelDeltaH has "ActiveArea width" values, BlackLevelDeltaV "ActiveArea
    // length" values; without an ActiveArea the whole image is active.
    const std::optional<std::uint64_t> active_width =
        s.active_area ? std::optional<std::uint64_t>(s.active_area->right - s.active_area->left)
                      : std::optional<std::uint64_t>(s.width);
    const std::optional<std::uint64_t> active_height =
        s.active_area ? std::optional<std::uint64_t>(s.active_area->bottom - s.active_area->top)
                      : std::optional<std::uint64_t>(s.height);
    if (!s.black_level_delta_h.empty() && s.black_level_delta_h.size() != active_width.value_or(0)) {
        return fail_here(std::format("BlackLevelDeltaH has {} values; the active area is {} columns wide",
                                     s.black_level_delta_h.size(), active_width.value_or(0)));
    }
    if (!s.black_level_delta_v.empty() && s.black_level_delta_v.size() != active_height.value_or(0)) {
        return fail_here(std::format("BlackLevelDeltaV has {} values; the active area is {} rows high",
                                     s.black_level_delta_v.size(), active_height.value_or(0)));
    }
    for (const DngRect& area : s.masked_areas) {
        if ((s.height && area.bottom > *s.height) || (s.width && area.right > *s.width)) {
            return fail_here(
                std::format("MaskedAreas rectangle (bottom {}, right {}) exceeds the {} x {} image",
                            area.bottom, area.right, s.width.value_or(0), s.height.value_or(0)));
        }
    }

    if (s.compression) {
        s.compression_info =
            classify_compression(*s.compression, s.photometric, s.bits_per_sample, s.jxl_distance);
    }

    // Tags normally appear in ascending order; keep the documented list order regardless.
    std::stable_sort(s.opcodes.begin(), s.opcodes.end(),
                     [](const DngOpcode& a, const DngOpcode& b) { return a.list < b.list; });
    return ok();
}

Status Inspector::walk() {
    if (reader_.first_ifd_offset() == 0) {
        return fail(ErrorCode::Corrupt,
                    "header's first IFD offset is 0; a TIFF file contains at least one IFD",
                    std::string(detail::kWhereHeader));
    }

    std::vector<PendingIfd> pending;
    pending.push_back(PendingIfd{reader_.first_ifd_offset(), TiffIfdKind::Main, std::nullopt, 0, 0, "ifd0"});
    std::vector<std::pair<std::uint64_t, std::uint32_t>> visited;  // IFD offset, index in summary_.ifds

    while (!pending.empty()) {
        PendingIfd next = std::move(pending.back());
        pending.pop_back();

        const auto seen = std::find_if(visited.begin(), visited.end(),
                                       [&](const auto& v) { return v.first == next.offset; });
        if (seen != visited.end()) {
            return fail(
                ErrorCode::Corrupt,
                std::format("{}: IFD offset {} was already visited as {}; the IFD structure contains a loop "
                            "or a shared IFD",
                            next.path, next.offset, summary_.ifds[seen->second].path),
                std::string(detail::kWhereIfdTree));
        }
        if (summary_.ifds.size() >= limits_.max_ifds) {
            return fail(ErrorCode::Corrupt,
                        std::format("{}: the file references more than {} IFDs", next.path, limits_.max_ifds),
                        std::string(detail::kWhereIfdTree));
        }
        if (next.depth > limits_.max_ifd_depth) {
            return fail(ErrorCode::Corrupt,
                        std::format("{}: IFD nesting depth {} exceeds the limit {}", next.path, next.depth,
                                    limits_.max_ifd_depth),
                        std::string(detail::kWhereIfdTree));
        }

        auto ifd = reader_.read_ifd(next.offset, limits_.max_entries_per_ifd, next.path);
        if (!ifd) {
            return Unexpected(std::move(ifd.error()));
        }

        const auto index = static_cast<std::uint32_t>(summary_.ifds.size());
        visited.emplace_back(next.offset, index);

        TiffIfdSummary summary;
        summary.kind = next.kind;
        summary.path = next.path;
        summary.offset = next.offset;
        summary.parent = next.parent;
        summary.depth = next.depth;
        summary.entry_count = static_cast<std::uint32_t>(ifd->entries.size());

        ChildIfds children;
        if (next.kind != TiffIfdKind::Exif) {
            const bool is_ifd0 = index == 0;
            if (auto status = interpret_ifd(*ifd, is_ifd0, summary, children); !status) {
                return status;
            }
            if (auto status = validate_ifd(summary); !status) {
                return status;
            }
        }
        summary_.ifds.push_back(std::move(summary));

        // Push in reverse visiting order: SubIFDs in order, then the Exif IFD, then the next
        // main-chain IFD (depth-first pre-order).
        if (next.kind == TiffIfdKind::Main) {
            if (ifd->next_ifd_offset != 0) {
                const std::uint32_t chain_index = next.chain_index + 1;
                pending.push_back(PendingIfd{ifd->next_ifd_offset, TiffIfdKind::Main, std::nullopt, 0,
                                             chain_index, std::format("ifd{}", chain_index)});
            }
        } else if (next.kind == TiffIfdKind::SubIfd && ifd->next_ifd_offset != 0) {
            warn(detail::kWhereIfdTree,
                 std::format("{}: next-IFD offset {} ignored; DNG does not support SubIFD chains", next.path,
                             ifd->next_ifd_offset));
        }
        if (children.exif) {
            pending.push_back(
                PendingIfd{*children.exif, TiffIfdKind::Exif, index, next.depth + 1, 0, next.path + ".exif"});
        }
        for (std::size_t i = children.sub_ifds.size(); i-- > 0;) {
            pending.push_back(PendingIfd{children.sub_ifds[i], TiffIfdKind::SubIfd, index, next.depth + 1, 0,
                                         std::format("{}.subifd{}", next.path, i)});
        }
    }
    return ok();
}

Status Inspector::validate_color(const DngInfo& dng, const TiffIfdSummary& raw) const {
    if (!dng.color_planes) {
        return ok();
    }
    const std::uint32_t planes = *dng.color_planes;
    const auto mismatch = [&](std::string_view tag, std::string_view what, std::size_t actual) {
        return fail(ErrorCode::Corrupt,
                    std::format("{} has {} {} but the primary raw IFD {} has {} colour planes", tag, actual,
                                what, raw.path, planes),
                    std::string(detail::kWhereColor));
    };

    const DngColorInfo& c = dng.color;
    for (std::size_t i = 0; i < 3; ++i) {
        const std::string suffix = std::to_string(i + 1);
        if (c.color_matrix[i] && c.color_matrix[i]->rows != planes) {
            return mismatch("ColorMatrix" + suffix, "rows", c.color_matrix[i]->rows);
        }
        if (c.forward_matrix[i] && c.forward_matrix[i]->cols != planes) {
            return mismatch("ForwardMatrix" + suffix, "columns", c.forward_matrix[i]->cols);
        }
        if (c.camera_calibration[i] && c.camera_calibration[i]->rows != planes) {
            return mismatch("CameraCalibration" + suffix, "rows", c.camera_calibration[i]->rows);
        }
    }
    if (!c.analog_balance.empty() && c.analog_balance.size() != planes) {
        return mismatch("AnalogBalance", "values", c.analog_balance.size());
    }
    if (!c.as_shot_neutral.empty() && c.as_shot_neutral.size() != planes) {
        return mismatch("AsShotNeutral", "values", c.as_shot_neutral.size());
    }
    if (!raw.noise_profile.empty() && raw.noise_profile.size() != 2 &&
        raw.noise_profile.size() != 2 * planes) {
        return mismatch("NoiseProfile", "values (expected 2 or 2 x planes)", raw.noise_profile.size());
    }
    // Some writers store NoiseProfile in IFD 0 although the raw image is in a SubIFD; readers use
    // that copy when the raw IFD has none, so it must describe the same planes.
    const TiffIfdSummary& ifd0 = summary_.ifds.front();
    if (&ifd0 != &raw && !ifd0.noise_profile.empty() && ifd0.noise_profile.size() != 2 &&
        ifd0.noise_profile.size() != 2 * planes) {
        return mismatch("NoiseProfile of ifd0", "values (expected 2 or 2 x planes)",
                        ifd0.noise_profile.size());
    }
    return ok();
}

Status Inspector::finish() {
    for (std::uint32_t i = 0; i < summary_.ifds.size(); ++i) {
        const TiffIfdSummary& ifd = summary_.ifds[i];
        const bool main_image =
            ifd.new_subfile_type.value_or(dng_subfile_type::kMainImage) == dng_subfile_type::kMainImage;
        const bool raw_photometric =
            ifd.photometric == tiff_photometric::kCfa || ifd.photometric == tiff_photometric::kLinearRaw;
        if (ifd.kind != TiffIfdKind::Exif && main_image && raw_photometric) {
            summary_.raw_ifd_candidates.push_back(i);
        }
    }
    if (summary_.raw_ifd_candidates.size() == 1) {
        summary_.primary_raw_ifd = summary_.raw_ifd_candidates.front();
    } else if (summary_.raw_ifd_candidates.size() > 1) {
        warn(detail::kWhereIfdTree,
             std::format("{} IFDs qualify as the main raw image; no primary raw IFD is selected",
                         summary_.raw_ifd_candidates.size()));
    }

    summary_.make = std::move(ifd0_.make);
    summary_.model = std::move(ifd0_.model);

    if (!ifd0_.dng_version) {
        return ok();
    }
    DngInfo dng;
    dng.version = *ifd0_.dng_version;
    if (ifd0_.dng_backward_version) {
        dng.backward_version = *ifd0_.dng_backward_version;
        dng.backward_version_present = true;
    } else {
        dng.backward_version = DngVersion{{dng.version.parts[0], dng.version.parts[1], 0, 0}};
    }
    dng.unique_camera_model = std::move(ifd0_.unique_camera_model);
    dng.color = std::move(ifd0_.color);
    dng.profile_gain_table_map2 = ifd0_.profile_gain_table_map2;

    if (const TiffIfdSummary* raw = summary_.primary_raw()) {
        if (raw->photometric == tiff_photometric::kCfa) {
            dng.color_planes =
                raw->cfa_plane_color.empty() ? 3u : static_cast<std::uint32_t>(raw->cfa_plane_color.size());
        } else {
            dng.color_planes = raw->samples_per_pixel.value_or(1);
        }
        if (auto status = validate_color(dng, *raw); !status) {
            return status;
        }
    }
    summary_.dng = std::move(dng);
    return ok();
}

Expected<TiffSummary> Inspector::run() {
    summary_.byte_order = reader_.byte_order();
    summary_.variant = reader_.variant();
    summary_.source_size = reader_.source_size();
    if (auto status = walk(); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = finish(); !status) {
        return Unexpected(std::move(status.error()));
    }
    return std::move(summary_);
}

}  // namespace

std::string_view to_string(Lossiness value) noexcept {
    switch (value) {
        case Lossiness::Unknown:
            return "unknown";
        case Lossiness::Lossless:
            return "lossless";
        case Lossiness::Lossy:
            return "lossy";
    }
    return "invalid";
}

bool TiffIfdSummary::has_mandatory_opcodes() const noexcept {
    return std::any_of(opcodes.begin(), opcodes.end(), [](const DngOpcode& op) { return op.is_mandatory(); });
}

CompressionInfo classify_compression(std::uint16_t compression, std::optional<std::uint16_t> photometric,
                                     std::span<const std::uint16_t> bits_per_sample,
                                     std::optional<float> jxl_distance) noexcept {
    namespace c = tiff_compression;
    switch (compression) {
        case c::kUncompressed:
            return {compression, "Uncompressed", true, Lossiness::Lossless};
        case c::kJpeg: {
            // DNG 1.7.1.0 chapter 3: baseline DCT for 8-bit YCbCr or 8-bit single-sample
            // BlackIsZero data (previews); lossless Huffman JPEG in every other case.
            const bool all_8_bit =
                !bits_per_sample.empty() && std::all_of(bits_per_sample.begin(), bits_per_sample.end(),
                                                        [](std::uint16_t bits) { return bits == 8; });
            const bool ycbcr_8 = photometric == tiff_photometric::kYCbCr && all_8_bit &&
                                 (bits_per_sample.size() == 3 || bits_per_sample.size() == 1);
            const bool gray_8 =
                photometric == tiff_photometric::kBlackIsZero && all_8_bit && bits_per_sample.size() == 1;
            if (ycbcr_8 || gray_8) {
                return {compression, "Baseline JPEG", true, Lossiness::Lossy};
            }
            return {compression, "Lossless JPEG", true, Lossiness::Lossless};
        }
        case c::kDeflate:
            return {compression, "Deflate", true, Lossiness::Lossless};
        case c::kLossyJpeg:
            return {compression, "Lossy JPEG", true, Lossiness::Lossy};
        case c::kJpegXl: {
            Lossiness lossiness = Lossiness::Unknown;
            if (jxl_distance && *jxl_distance == 0.0f) {
                lossiness = Lossiness::Lossless;
            } else if (jxl_distance && *jxl_distance > 0.0f) {
                lossiness = Lossiness::Lossy;
            }
            return {compression, "JPEG XL", true, lossiness};
        }
        case c::kLzw:
            return {compression, "LZW", false, Lossiness::Lossless};
        case c::kPackBits:
            return {compression, "PackBits", false, Lossiness::Lossless};
        default:
            return {compression, "Unrecognized", false, Lossiness::Unknown};
    }
}

std::string_view photometric_name(std::uint16_t photometric) noexcept {
    namespace p = tiff_photometric;
    switch (photometric) {
        case p::kWhiteIsZero:
            return "WhiteIsZero";
        case p::kBlackIsZero:
            return "BlackIsZero";
        case p::kRgb:
            return "RGB";
        case p::kTransparencyMask:
            return "TransparencyMask";
        case p::kYCbCr:
            return "YCbCr";
        case p::kCfa:
            return "CFA";
        case p::kLinearRaw:
            return "LinearRaw";
        case p::kDepth:
            return "Depth";
        case p::kPhotometricMask:
            return "PhotometricMask";
        default:
            return "Unrecognized";
    }
}

Expected<TiffSummary> inspect_tiff(RandomAccessSource& source, const TiffInspectLimits& limits) {
    try {
        if (limits.max_ifds == 0 || limits.max_entries_per_ifd == 0 || limits.max_value_bytes == 0 ||
            limits.max_total_value_bytes == 0) {
            return fail(
                ErrorCode::InvalidArgument,
                "max_ifds, max_entries_per_ifd, max_value_bytes and max_total_value_bytes must each be "
                "at least 1",
                std::string(detail::kWhereLimits));
        }
        auto reader = TiffReader::open(source);
        if (!reader) {
            return Unexpected(std::move(reader.error()));
        }
        Inspector inspector(*reader, limits);
        return inspector.run();
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "allocation failed while inspecting the TIFF structure",
                    std::string(detail::kWhereInspect));
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("unexpected exception: ") + e.what(),
                    std::string(detail::kWhereInspect));
    } catch (...) {
        return fail(ErrorCode::Internal, "unexpected non-standard exception",
                    std::string(detail::kWhereInspect));
    }
}

}  // namespace lucent::raw
