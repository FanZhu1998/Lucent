#include "exif_reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lucent/core/file.hpp>
#include <lucent/core/text.hpp>

#include "exif_format.hpp"

namespace lucent::exporter::detail {
namespace {

constexpr const char* kWhereRead = "export.metadata.read";
constexpr std::uint32_t kMaxEntries = 1024;
constexpr std::uint64_t kMaxValueBytes = 1u << 20;
constexpr std::int32_t kMaxJpegSegments = 4096;

Error corrupt_error(std::string message) {
    return Error{ErrorCode::Corrupt, std::move(message), kWhereRead, 0};
}

// Random access to the bytes of a file or of a buffer.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    // Reads exactly out.size() bytes at `offset`; Corrupt when the range lies outside.
    [[nodiscard]] virtual Status read(std::uint64_t offset, std::span<std::uint8_t> out) = 0;
};

class SpanSource final : public ByteSource {
public:
    explicit SpanSource(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
    std::uint64_t size() const noexcept override { return bytes_.size(); }
    Status read(std::uint64_t offset, std::span<std::uint8_t> out) override {
        if (offset > bytes_.size() || out.size() > bytes_.size() - offset) {
            return Unexpected(corrupt_error(std::format("EXIF reference to bytes {}..{} lies outside {} bytes",
                                                        offset, offset + out.size(), bytes_.size())));
        }
        std::memcpy(out.data(), bytes_.data() + offset, out.size());
        return ok();
    }

private:
    std::span<const std::uint8_t> bytes_;
};

class FileByteSource final : public ByteSource {
public:
    FileByteSource(const core::ReadOnlyFile& file, std::uint64_t size) noexcept : file_(&file), size_(size) {}
    std::uint64_t size() const noexcept override { return size_; }
    Status read(std::uint64_t offset, std::span<std::uint8_t> out) override {
        if (offset > size_ || out.size() > size_ - offset) {
            return Unexpected(corrupt_error(
                std::format("reference to bytes {}..{} lies outside the {}-byte file", offset, offset + out.size(), size_)));
        }
        auto read = file_->read_at(offset, std::as_writable_bytes(out));
        if (!read) {
            return Unexpected(std::move(read.error()));
        }
        if (*read != out.size()) {
            return Unexpected(corrupt_error(std::format("the file ended while reading byte {}", offset + *read)));
        }
        return ok();
    }

private:
    const core::ReadOnlyFile* file_;
    std::uint64_t size_;
};

std::uint32_t type_size(std::uint16_t type) noexcept {
    switch (type) {
        case 1:  // BYTE
        case 2:  // ASCII
        case 6:  // SBYTE
        case 7:  // UNDEFINED
            return 1;
        case 3:  // SHORT
        case 8:  // SSHORT
            return 2;
        case 4:   // LONG
        case 9:   // SLONG
        case 11:  // FLOAT
        case 13:  // IFD
            return 4;
        case 5:   // RATIONAL
        case 10:  // SRATIONAL
        case 12:  // DOUBLE
            return 8;
        default:
            return 0;
    }
}

struct RawEntry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::array<std::uint8_t, 4> field{};
};

// A classic TIFF structure at the start of a ByteSource.
class TiffParser {
public:
    explicit TiffParser(ByteSource& source) noexcept : source_(&source) {}

    Status open() {
        std::array<std::uint8_t, 8> header{};
        if (source_->size() < header.size()) {
            return Unexpected(corrupt_error("the TIFF header is truncated"));
        }
        if (auto s = source_->read(0, header); !s) {
            return s;
        }
        if (header[0] == 'I' && header[1] == 'I') {
            little_ = true;
        } else if (header[0] == 'M' && header[1] == 'M') {
            little_ = false;
        } else {
            return Unexpected(corrupt_error("no TIFF byte-order mark"));
        }
        if (u16(header.data() + 2) != 42) {
            return Unexpected(corrupt_error("not a classic TIFF structure (magic number)"));
        }
        ifd0_ = u32(header.data() + 4);
        return ok();
    }

    [[nodiscard]] std::uint32_t ifd0() const noexcept { return ifd0_; }

    std::uint16_t u16(const std::uint8_t* p) const noexcept {
        return little_ ? static_cast<std::uint16_t>(p[0] | (p[1] << 8)) : static_cast<std::uint16_t>((p[0] << 8) | p[1]);
    }
    std::uint32_t u32(const std::uint8_t* p) const noexcept {
        return little_ ? (std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) |
                          (std::uint32_t{p[3]} << 24))
                       : ((std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
                          std::uint32_t{p[3]});
    }

    // Entries and next-IFD offset of the IFD at `offset`.
    Status read_ifd(std::uint32_t offset, std::string_view name, std::vector<RawEntry>& entries,
                    std::uint32_t& next) {
        if (offset < 8 || offset % 2 != 0) {
            return Unexpected(corrupt_error(std::format("{} offset {} is invalid", name, offset)));
        }
        std::array<std::uint8_t, 2> count_bytes{};
        if (auto s = source_->read(offset, count_bytes); !s) {
            return s;
        }
        const std::uint32_t count = u16(count_bytes.data());
        if (count == 0 || count > kMaxEntries) {
            return Unexpected(corrupt_error(std::format("{} declares {} entries", name, count)));
        }
        std::vector<std::uint8_t> table(static_cast<std::size_t>(count) * 12 + 4);
        if (auto s = source_->read(std::uint64_t{offset} + 2, table); !s) {
            return s;
        }
        entries.clear();
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint8_t* p = table.data() + std::size_t{i} * 12;
            RawEntry e;
            e.tag = u16(p);
            e.type = u16(p + 2);
            e.count = u32(p + 4);
            std::memcpy(e.field.data(), p + 8, 4);
            if (!entries.empty() && e.tag <= entries.back().tag) {
                return Unexpected(corrupt_error(std::format("{} tags are not in ascending order at tag {}", name, e.tag)));
            }
            entries.push_back(e);
        }
        next = u32(table.data() + std::size_t{count} * 12);
        return ok();
    }

    Expected<std::vector<std::uint8_t>> value(const RawEntry& e, std::string_view name) {
        const std::uint32_t size = type_size(e.type);
        if (size == 0) {
            return Unexpected(corrupt_error(std::format("{} tag {} has unknown type {}", name, e.tag, e.type)));
        }
        const std::uint64_t bytes = std::uint64_t{size} * e.count;
        if (bytes > kMaxValueBytes) {
            return Unexpected(corrupt_error(std::format("{} tag {} value of {} bytes is too large", name, e.tag, bytes)));
        }
        std::vector<std::uint8_t> out(static_cast<std::size_t>(bytes));
        if (bytes <= 4) {
            std::memcpy(out.data(), e.field.data(), out.size());
            return out;
        }
        if (auto s = source_->read(u32(e.field.data()), out); !s) {
            return Unexpected(std::move(s.error()));
        }
        return out;
    }

private:
    ByteSource* source_;
    bool little_ = true;
    std::uint32_t ifd0_ = 0;
};

// Typed, strict access to one IFD's entries.
class Fields {
public:
    Fields(TiffParser& parser, std::vector<RawEntry> entries, std::string name)
        : parser_(&parser), entries_(std::move(entries)), name_(std::move(name)) {}

    [[nodiscard]] const RawEntry* find(std::uint16_t tag) const noexcept {
        for (const RawEntry& e : entries_) {
            if (e.tag == tag) {
                return &e;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::vector<std::uint16_t> tags() const {
        std::vector<std::uint16_t> out;
        for (const RawEntry& e : entries_) {
            out.push_back(e.tag);
        }
        return out;
    }

    Error error(std::uint16_t tag, std::string_view what) const {
        return corrupt_error(std::format("{} tag {}: {}", name_, tag, what));
    }

    // Value bytes of `tag` with type `type` and, unless 0, exactly `count` values.
    Expected<std::optional<std::vector<std::uint8_t>>> raw(std::uint16_t tag, std::uint16_t type,
                                                           std::uint32_t count) {
        const RawEntry* e = find(tag);
        if (e == nullptr) {
            return std::optional<std::vector<std::uint8_t>>{};
        }
        if (e->type != type) {
            return Unexpected(error(tag, std::format("type {} instead of {}", e->type, type)));
        }
        if (count != 0 ? e->count != count : e->count == 0) {
            return Unexpected(error(tag, std::format("{} values", e->count)));
        }
        auto value = parser_->value(*e, name_);
        if (!value) {
            return Unexpected(std::move(value.error()));
        }
        return std::optional<std::vector<std::uint8_t>>(std::move(*value));
    }

    // ASCII value: NUL-terminated, well-formed UTF-8, no embedded NUL.
    Expected<std::string> text(std::uint16_t tag) {
        auto value = raw(tag, exif_type::kAscii, 0);
        if (!value) {
            return Unexpected(std::move(value.error()));
        }
        if (!*value) {
            return std::string();
        }
        const std::vector<std::uint8_t>& bytes = **value;
        if (bytes.back() != 0) {
            return Unexpected(error(tag, "ASCII value is not NUL-terminated"));
        }
        std::string out(bytes.begin(), bytes.end() - 1);
        if (out.find('\0') != std::string::npos) {
            return Unexpected(error(tag, "ASCII value contains NUL"));
        }
        if (!core::is_valid_utf8(out)) {
            return Unexpected(error(tag, "text is not well-formed UTF-8"));
        }
        if (out.empty()) {
            return Unexpected(error(tag, "empty text"));
        }
        return out;
    }

    Expected<std::optional<std::vector<ExportRational>>> rationals(std::uint16_t tag, std::uint32_t count) {
        auto value = raw(tag, exif_type::kRational, count);
        if (!value) {
            return Unexpected(std::move(value.error()));
        }
        if (!*value) {
            return std::optional<std::vector<ExportRational>>{};
        }
        std::vector<ExportRational> out;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint8_t* p = (*value)->data() + std::size_t{i} * 8;
            const ExportRational r{parser_->u32(p), parser_->u32(p + 4)};
            if (r.denominator == 0) {
                return Unexpected(error(tag, "zero denominator"));
            }
            out.push_back(r);
        }
        return std::optional<std::vector<ExportRational>>(std::move(out));
    }

    Expected<std::optional<ExportRational>> rational(std::uint16_t tag) {
        auto values = rationals(tag, 1);
        if (!values) {
            return Unexpected(std::move(values.error()));
        }
        if (!*values) {
            return std::optional<ExportRational>{};
        }
        return std::optional<ExportRational>((*values)->front());
    }

    Expected<std::optional<ExportSignedRational>> signed_rational(std::uint16_t tag) {
        auto value = raw(tag, exif_type::kSRational, 1);
        if (!value) {
            return Unexpected(std::move(value.error()));
        }
        if (!*value) {
            return std::optional<ExportSignedRational>{};
        }
        const ExportSignedRational r{static_cast<std::int32_t>(parser_->u32((*value)->data())),
                                     static_cast<std::int32_t>(parser_->u32((*value)->data() + 4))};
        if (r.denominator == 0) {
            return Unexpected(error(tag, "zero denominator"));
        }
        return std::optional<ExportSignedRational>(r);
    }

    // A SHORT or LONG (`allow_long`) single value.
    Expected<std::optional<std::uint32_t>> integer(std::uint16_t tag, bool allow_long) {
        const RawEntry* e = find(tag);
        if (e == nullptr) {
            return std::optional<std::uint32_t>{};
        }
        const bool is_short = e->type == exif_type::kShort;
        const bool is_long = e->type == exif_type::kLong || e->type == exif_type::kIfd;
        if (!is_short && !(allow_long && is_long)) {
            return Unexpected(error(tag, std::format("unexpected type {}", e->type)));
        }
        if (e->count != 1) {
            return Unexpected(error(tag, std::format("{} values", e->count)));
        }
        return std::optional<std::uint32_t>(is_short ? parser_->u16(e->field.data()) : parser_->u32(e->field.data()));
    }

private:
    TiffParser* parser_;
    std::vector<RawEntry> entries_;
    std::string name_;
};

// Unwraps an Expected or returns its error from the enclosing function (returning Status or
// Expected<...>).
#define LUCENT_EXIF_TRY(target, expression)               \
    do {                                                    \
        auto lucent_try_result = (expression);              \
        if (!lucent_try_result) {                           \
            return Unexpected(std::move(lucent_try_result.error())); \
        }                                                   \
        target = std::move(*lucent_try_result);             \
    } while (false)

bool digits(std::string_view text) noexcept {
    return std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int number(std::string_view text) noexcept {
    int value = 0;
    for (const char c : text) {
        value = value * 10 + (c - '0');
    }
    return value;
}

bool valid_date(int year, int month, int day) noexcept {
    static constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1) {
        return false;
    }
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return day <= kDays[static_cast<std::size_t>(month - 1)] + (month == 2 && leap ? 1 : 0);
}

// "YYYY:MM:DD" -> year, month, day.
bool parse_date(std::string_view text, int& year, int& month, int& day) noexcept {
    if (text.size() != 10 || text[4] != ':' || text[7] != ':' || !digits(text.substr(0, 4)) ||
        !digits(text.substr(5, 2)) || !digits(text.substr(8, 2))) {
        return false;
    }
    year = number(text.substr(0, 4));
    month = number(text.substr(5, 2));
    day = number(text.substr(8, 2));
    return valid_date(year, month, day);
}

Status parse_ifd0(Fields& f, ParsedExif& out) {
    LUCENT_EXIF_TRY(out.metadata.camera_make, f.text(exif_tag::kMake));
    LUCENT_EXIF_TRY(out.metadata.camera_model, f.text(exif_tag::kModel));
    LUCENT_EXIF_TRY(out.metadata.software, f.text(exif_tag::kSoftware));
    LUCENT_EXIF_TRY(out.metadata.artist, f.text(exif_tag::kArtist));
    LUCENT_EXIF_TRY(out.metadata.copyright, f.text(exif_tag::kCopyright));
    std::optional<std::uint32_t> orientation;
    LUCENT_EXIF_TRY(orientation, f.integer(exif_tag::kOrientation, false));
    if (orientation) {
        out.orientation = static_cast<std::uint16_t>(*orientation);
    }
    return ok();
}

Status parse_exif(Fields& f, ParsedExif& out) {
    ExportMetadata& m = out.metadata;
    std::optional<std::vector<std::uint8_t>> version;
    LUCENT_EXIF_TRY(version, f.raw(exif_tag::kExifVersion, exif_type::kUndefined, 4));
    if (version) {
        out.exif_version.assign(version->begin(), version->end());
    }
    std::optional<std::uint32_t> value;
    LUCENT_EXIF_TRY(value, f.integer(exif_tag::kColorSpace, false));
    if (value) {
        out.color_space = static_cast<std::uint16_t>(*value);
    }
    LUCENT_EXIF_TRY(out.pixel_x, f.integer(exif_tag::kPixelXDimension, true));
    LUCENT_EXIF_TRY(out.pixel_y, f.integer(exif_tag::kPixelYDimension, true));

    LUCENT_EXIF_TRY(m.exposure_time_s, f.rational(exif_tag::kExposureTime));
    LUCENT_EXIF_TRY(m.f_number, f.rational(exif_tag::kFNumber));
    LUCENT_EXIF_TRY(m.focal_length_mm, f.rational(exif_tag::kFocalLength));

    LUCENT_EXIF_TRY(m.exposure_bias_ev, f.signed_rational(exif_tag::kExposureBias));

    std::optional<std::uint32_t> iso;
    LUCENT_EXIF_TRY(iso, f.integer(exif_tag::kPhotographicSensitivity, false));
    if (iso && *iso == 65535) {
        std::optional<std::uint32_t> precise;
        LUCENT_EXIF_TRY(precise, f.integer(exif_tag::kIsoSpeed, true));
        if (!precise) {
            return Unexpected(f.error(exif_tag::kIsoSpeed, "missing for PhotographicSensitivity 65535"));
        }
        iso = precise;
    }
    m.iso = iso;
    std::optional<std::uint32_t> focal35;
    LUCENT_EXIF_TRY(focal35, f.integer(exif_tag::kFocalLength35mm, false));
    if (focal35) {
        m.focal_length_35mm_mm = static_cast<std::uint16_t>(*focal35);
    }
    LUCENT_EXIF_TRY(m.lens_make, f.text(exif_tag::kLensMake));
    LUCENT_EXIF_TRY(m.lens_model, f.text(exif_tag::kLensModel));

    std::string date_time;
    std::string subsec;
    std::string offset;
    LUCENT_EXIF_TRY(date_time, f.text(exif_tag::kDateTimeOriginal));
    LUCENT_EXIF_TRY(subsec, f.text(exif_tag::kSubSecTimeOriginal));
    LUCENT_EXIF_TRY(offset, f.text(exif_tag::kOffsetTimeOriginal));
    if (date_time.empty()) {
        if (!subsec.empty() || !offset.empty()) {
            return Unexpected(f.error(exif_tag::kDateTimeOriginal, "sub-seconds or offset without a time"));
        }
        return ok();
    }
    CaptureDateTime t;
    if (date_time.size() != 19 || date_time[10] != ' ' || date_time[13] != ':' || date_time[16] != ':' ||
        !parse_date(std::string_view(date_time).substr(0, 10), t.year, t.month, t.day) ||
        !digits(date_time.substr(11, 2)) || !digits(date_time.substr(14, 2)) || !digits(date_time.substr(17, 2))) {
        return Unexpected(f.error(exif_tag::kDateTimeOriginal, "malformed \"" + date_time + "\""));
    }
    t.hour = number(date_time.substr(11, 2));
    t.minute = number(date_time.substr(14, 2));
    t.second = number(date_time.substr(17, 2));
    if (t.hour > 23 || t.minute > 59 || t.second > 59) {
        return Unexpected(f.error(exif_tag::kDateTimeOriginal, "time out of range"));
    }
    if (!subsec.empty() && (subsec.size() > 9 || !digits(subsec))) {
        return Unexpected(f.error(exif_tag::kSubSecTimeOriginal, "malformed"));
    }
    t.subsec = subsec;
    if (!offset.empty()) {
        if (offset.size() != 6 || (offset[0] != '+' && offset[0] != '-') || offset[3] != ':' ||
            !digits(offset.substr(1, 2)) || !digits(offset.substr(4, 2))) {
            return Unexpected(f.error(exif_tag::kOffsetTimeOriginal, "malformed"));
        }
        const int minutes = number(offset.substr(1, 2)) * 60 + number(offset.substr(4, 2));
        t.utc_offset_minutes = offset[0] == '-' ? -minutes : minutes;
    }
    m.capture_time = std::move(t);
    return ok();
}

Expected<double> dms(Fields& f, std::uint16_t tag) {
    std::optional<std::vector<ExportRational>> values;
    LUCENT_EXIF_TRY(values, f.rationals(tag, 3));
    if (!values) {
        return Unexpected(f.error(tag, "missing"));
    }
    return (*values)[0].value() + (*values)[1].value() / 60.0 + (*values)[2].value() / 3600.0;
}

Expected<char> reference(Fields& f, std::uint16_t tag, char positive, char negative) {
    std::string text;
    LUCENT_EXIF_TRY(text, f.text(tag));
    if (text.size() != 1 || (text[0] != positive && text[0] != negative)) {
        return Unexpected(f.error(tag, "invalid reference \"" + text + "\""));
    }
    return text[0];
}

Status parse_gps(Fields& f, ParsedExif& out) {
    GpsPosition p;
    double latitude = 0.0;
    double longitude = 0.0;
    char lat_ref = 0;
    char lon_ref = 0;
    LUCENT_EXIF_TRY(lat_ref, reference(f, exif_tag::kGpsLatitudeRef, 'N', 'S'));
    LUCENT_EXIF_TRY(latitude, dms(f, exif_tag::kGpsLatitude));
    LUCENT_EXIF_TRY(lon_ref, reference(f, exif_tag::kGpsLongitudeRef, 'E', 'W'));
    LUCENT_EXIF_TRY(longitude, dms(f, exif_tag::kGpsLongitude));
    p.latitude_deg = lat_ref == 'S' ? -latitude : latitude;
    p.longitude_deg = lon_ref == 'W' ? -longitude : longitude;

    std::optional<std::vector<std::uint8_t>> version;
    LUCENT_EXIF_TRY(version, f.raw(exif_tag::kGpsVersionId, exif_type::kByte, 4));
    if (!version || !std::equal(version->begin(), version->end(), std::begin(kGpsVersion))) {
        return Unexpected(f.error(exif_tag::kGpsVersionId, "missing or not 2.3.0.0"));
    }

    std::optional<std::vector<std::uint8_t>> altitude_ref;
    std::optional<ExportRational> altitude;
    LUCENT_EXIF_TRY(altitude_ref, f.raw(exif_tag::kGpsAltitudeRef, exif_type::kByte, 1));
    LUCENT_EXIF_TRY(altitude, f.rational(exif_tag::kGpsAltitude));
    if (altitude_ref.has_value() != altitude.has_value() || (altitude_ref && altitude_ref->front() > 1)) {
        return Unexpected(f.error(exif_tag::kGpsAltitudeRef, "altitude and its reference do not match"));
    }
    if (altitude) {
        p.altitude_m = altitude_ref->front() == 1 ? -altitude->value() : altitude->value();
    }

    std::optional<std::vector<ExportRational>> time;
    std::string date;
    LUCENT_EXIF_TRY(time, f.rationals(exif_tag::kGpsTimeStamp, 3));
    LUCENT_EXIF_TRY(date, f.text(exif_tag::kGpsDateStamp));
    if (time.has_value() != !date.empty()) {
        return Unexpected(f.error(exif_tag::kGpsTimeStamp, "time stamp and date stamp must come together"));
    }
    if (time) {
        GpsTimestamp t;
        if (!parse_date(date, t.year, t.month, t.day)) {
            return Unexpected(f.error(exif_tag::kGpsDateStamp, "malformed \"" + date + "\""));
        }
        const double hour = (*time)[0].value();
        const double minute = (*time)[1].value();
        t.second = (*time)[2].value();
        if (hour != std::floor(hour) || hour > 23 || minute != std::floor(minute) || minute > 59 || t.second >= 60.0) {
            return Unexpected(f.error(exif_tag::kGpsTimeStamp, "time out of range"));
        }
        t.hour = static_cast<std::int32_t>(hour);
        t.minute = static_cast<std::int32_t>(minute);
        p.utc = t;
    }
    out.metadata.gps = p;
    return ok();
}

Status parse_tiff_structure(ByteSource& source, ParsedExif& out) {
    TiffParser parser(source);
    if (auto s = parser.open(); !s) {
        return s;
    }
    std::vector<RawEntry> entries;
    if (auto s = parser.read_ifd(parser.ifd0(), "ifd0", entries, out.ifd0_next); !s) {
        return s;
    }
    Fields ifd0(parser, std::move(entries), "ifd0");
    out.ifd0_tags = ifd0.tags();
    out.has_tiff_structure = true;
    if (auto s = parse_ifd0(ifd0, out); !s) {
        return s;
    }
    std::optional<std::uint32_t> exif_offset;
    std::optional<std::uint32_t> gps_offset;
    LUCENT_EXIF_TRY(exif_offset, ifd0.integer(exif_tag::kExifIfd, true));
    LUCENT_EXIF_TRY(gps_offset, ifd0.integer(exif_tag::kGpsIfd, true));
    if (exif_offset) {
        std::uint32_t next = 0;
        if (auto s = parser.read_ifd(*exif_offset, "exif", entries, next); !s) {
            return s;
        }
        Fields exif(parser, std::move(entries), "exif");
        out.exif_tags = exif.tags();
        out.has_exif_ifd = true;
        if (auto s = parse_exif(exif, out); !s) {
            return s;
        }
    }
    if (gps_offset) {
        std::uint32_t next = 0;
        if (auto s = parser.read_ifd(*gps_offset, "gps", entries, next); !s) {
            return s;
        }
        Fields gps(parser, std::move(entries), "gps");
        out.gps_tags = gps.tags();
        out.has_gps_ifd = true;
        if (auto s = parse_gps(gps, out); !s) {
            return s;
        }
    }
    return ok();
}

// Walks the marker segments before the first scan; parses the first APP1 "Exif" segment and
// counts all of them.
Status parse_jpeg(ByteSource& source, ParsedExif& out) {
    std::uint64_t pos = 2;
    std::vector<std::uint8_t> first_exif;
    for (std::int32_t segment = 0; segment < kMaxJpegSegments; ++segment) {
        std::array<std::uint8_t, 2> marker{};
        if (auto s = source.read(pos, marker); !s) {
            return s;
        }
        if (marker[0] != 0xFF) {
            return Unexpected(corrupt_error(std::format("expected a JPEG marker at offset {}", pos)));
        }
        pos += 2;
        const std::uint8_t code = marker[1];
        if (code == 0xDA || code == 0xD9) {
            break;
        }
        if (code == 0xFF || code == 0x00 || (code >= 0xD0 && code <= 0xD8) || code == 0x01) {
            return Unexpected(corrupt_error(std::format("unexpected JPEG marker 0x{:02X} at offset {}", code, pos - 2)));
        }
        std::array<std::uint8_t, 2> length_bytes{};
        if (auto s = source.read(pos, length_bytes); !s) {
            return s;
        }
        const std::uint32_t length = (std::uint32_t{length_bytes[0]} << 8) | length_bytes[1];
        if (length < 2 || pos + length > source.size()) {
            return Unexpected(corrupt_error(std::format("JPEG segment 0x{:02X} at offset {} overruns the file", code, pos - 2)));
        }
        if (code == 0xE1 && length >= 2 + 6) {
            std::array<std::uint8_t, 6> signature{};
            if (auto s = source.read(pos + 2, signature); !s) {
                return s;
            }
            if (std::memcmp(signature.data(), "Exif\0\0", 6) == 0) {
                ++out.exif_segments;
                if (out.exif_segments == 1) {
                    first_exif.resize(length - 2 - 6);
                    if (auto s = source.read(pos + 8, first_exif); !s) {
                        return s;
                    }
                }
            }
        }
        pos += length;
    }
    if (out.exif_segments == 0) {
        return ok();
    }
    SpanSource tiff(first_exif);
    return parse_tiff_structure(tiff, out);
}

Expected<ParsedExif> read_impl(const std::filesystem::path& path) {
    auto file = core::ReadOnlyFile::open(path);
    if (!file) {
        return Unexpected(std::move(file.error()));
    }
    auto size = file->size();
    if (!size) {
        return Unexpected(std::move(size.error()));
    }
    FileByteSource source(*file, *size);
    std::array<std::uint8_t, 4> lead{};
    if (*size < lead.size()) {
        return fail(ErrorCode::Unsupported, "the file is neither JPEG nor TIFF", kWhereRead);
    }
    if (auto s = source.read(0, lead); !s) {
        return Unexpected(std::move(s.error()));
    }
    ParsedExif out;
    if (lead[0] == 0xFF && lead[1] == 0xD8 && lead[2] == 0xFF) {
        out.container = ExifContainer::Jpeg;
        if (auto s = parse_jpeg(source, out); !s) {
            return Unexpected(std::move(s.error()));
        }
        return out;
    }
    const bool tiff = (lead[0] == 'I' && lead[1] == 'I' && lead[2] == 42 && lead[3] == 0) ||
                      (lead[0] == 'M' && lead[1] == 'M' && lead[2] == 0 && lead[3] == 42);
    if (!tiff) {
        return fail(ErrorCode::Unsupported, "the file is neither JPEG nor classic TIFF", kWhereRead);
    }
    out.container = ExifContainer::Tiff;
    if (auto s = parse_tiff_structure(source, out); !s) {
        return Unexpected(std::move(s.error()));
    }
    return out;
}

#undef LUCENT_EXIF_TRY

bool close(double a, double b, double tolerance) noexcept {
    return std::abs(a - b) <= tolerance;
}

bool contains(std::span<const std::uint16_t> allowed, std::uint16_t tag) noexcept {
    return std::find(allowed.begin(), allowed.end(), tag) != allowed.end();
}

}  // namespace

Expected<ParsedExif> read_exif_file(const std::filesystem::path& path) {
    try {
        return read_impl(path);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while reading EXIF", kWhereRead);
    }
}

std::optional<std::string> metadata_difference(const ExportMetadata& a, const ExportMetadata& e) {
    const auto text = [](std::string_view name, const std::string& actual,
                         const std::string& expected) -> std::optional<std::string> {
        if (actual == expected) {
            return std::nullopt;
        }
        return std::format("{} is \"{}\" instead of \"{}\"", name, actual, expected);
    };
    for (auto difference : {text("Make", a.camera_make, e.camera_make), text("Model", a.camera_model, e.camera_model),
                            text("LensMake", a.lens_make, e.lens_make), text("LensModel", a.lens_model, e.lens_model),
                            text("Artist", a.artist, e.artist), text("Copyright", a.copyright, e.copyright),
                            text("Software", a.software, e.software)}) {
        if (difference) {
            return difference;
        }
    }
    if (a.exposure_time_s != e.exposure_time_s) {
        return "ExposureTime differs";
    }
    if (a.f_number != e.f_number) {
        return "FNumber differs";
    }
    if (a.focal_length_mm != e.focal_length_mm) {
        return "FocalLength differs";
    }
    if (a.exposure_bias_ev != e.exposure_bias_ev) {
        return "ExposureBiasValue differs";
    }
    if (a.iso != e.iso) {
        return "ISO differs";
    }
    if (a.focal_length_35mm_mm != e.focal_length_35mm_mm) {
        return "FocalLengthIn35mmFilm differs";
    }
    if (a.capture_time != e.capture_time) {
        return "capture time differs";
    }
    if (a.gps.has_value() != e.gps.has_value()) {
        return a.gps ? "a GPS position is present although none was expected" : "the GPS position is missing";
    }
    if (a.gps) {
        constexpr double degrees = 1e-9;
        constexpr double metres = 1e-6;
        constexpr double seconds = 1e-6;
        if (!close(a.gps->latitude_deg, e.gps->latitude_deg, degrees) ||
            !close(a.gps->longitude_deg, e.gps->longitude_deg, degrees)) {
            return "GPS coordinates differ";
        }
        if (a.gps->altitude_m.has_value() != e.gps->altitude_m.has_value() ||
            (a.gps->altitude_m && !close(*a.gps->altitude_m, *e.gps->altitude_m, metres))) {
            return "GPS altitude differs";
        }
        const auto& at = a.gps->utc;
        const auto& et = e.gps->utc;
        if (at.has_value() != et.has_value() ||
            (at && (at->year != et->year || at->month != et->month || at->day != et->day || at->hour != et->hour ||
                    at->minute != et->minute || !close(at->second, et->second, seconds)))) {
            return "GPS time stamp differs";
        }
    }
    return std::nullopt;
}

Status verify_metadata(const std::filesystem::path& path, ExportFormat format, const ExportMetadata& expected,
                       const ExifImageInfo& image) {
    const bool jpeg = format == ExportFormat::Jpeg;
    const char* where = jpeg ? "export.jpeg.verify" : "export.tiff.verify";
    const auto bad = [where](std::string what) {
        return fail(ErrorCode::Corrupt, "written file failed metadata verification: " + std::move(what), where);
    };
    auto parsed = read_exif_file(path);
    if (!parsed) {
        if (parsed.error().code == ErrorCode::Corrupt || parsed.error().code == ErrorCode::Unsupported) {
            return bad(parsed.error().message);
        }
        return Unexpected(std::move(parsed.error()));
    }
    const ParsedExif& p = *parsed;
    if (p.container != (jpeg ? ExifContainer::Jpeg : ExifContainer::Tiff)) {
        return bad("unexpected container");
    }
    if (jpeg) {
        if (expected.empty()) {
            return p.exif_segments == 0 ? ok() : bad("EXIF present although no metadata was to be written");
        }
        if (p.exif_segments != 1) {
            return bad(std::format("{} EXIF segments instead of 1", p.exif_segments));
        }
        if (p.ifd0_next != 0) {
            return bad("IFD 1 (a thumbnail) is present");
        }
        if (p.orientation != std::optional<std::uint16_t>(std::uint16_t{1})) {
            return bad("Orientation is not 1");
        }
        for (const std::uint16_t tag : p.ifd0_tags) {
            if (!contains(kJpegIfd0Tags, tag)) {
                return bad(std::format("unexpected IFD 0 tag {}", tag));
            }
        }
    } else {
        for (const std::uint16_t tag : {exif_tag::kSubIfds, exif_tag::kJpegInterchangeFormat,
                                        exif_tag::kJpegInterchangeFormatLength}) {
            if (std::find(p.ifd0_tags.begin(), p.ifd0_tags.end(), tag) != p.ifd0_tags.end()) {
                return bad(std::format("unexpected IFD 0 tag {}", tag));
            }
        }
        if (p.ifd0_next != 0) {
            return bad("the TIFF has more than one main IFD");
        }
    }
    if (p.has_exif_ifd != expected.has_capture()) {
        return bad(p.has_exif_ifd ? "Exif IFD present without capture metadata" : "Exif IFD missing");
    }
    for (const std::uint16_t tag : p.exif_tags) {
        if (!contains(kExifIfdTags, tag)) {
            return bad(std::format("unexpected Exif IFD tag {}", tag));
        }
    }
    if (p.has_exif_ifd) {
        if (p.exif_version != std::string_view(kExifVersion, 4)) {
            return bad("ExifVersion is not 0232");
        }
        if (p.color_space != std::optional<std::uint16_t>(exif_color_space(image.profile))) {
            return bad("ColorSpace does not match the output profile");
        }
        if (p.pixel_x != std::optional<std::uint32_t>(static_cast<std::uint32_t>(image.width)) ||
            p.pixel_y != std::optional<std::uint32_t>(static_cast<std::uint32_t>(image.height))) {
            return bad("PixelX/YDimension do not match the image");
        }
    }
    if (p.has_gps_ifd != expected.gps.has_value()) {
        return bad(p.has_gps_ifd ? "GPS IFD present although GPS was not to be written" : "GPS IFD missing");
    }
    for (const std::uint16_t tag : p.gps_tags) {
        if (!contains(kGpsIfdTags, tag)) {
            return bad(std::format("unexpected GPS IFD tag {}", tag));
        }
    }
    if (auto difference = metadata_difference(p.metadata, expected)) {
        return bad(*difference);
    }
    return ok();
}

}  // namespace lucent::exporter::detail
