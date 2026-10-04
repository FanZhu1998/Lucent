#include "lucent/raw/capture_metadata.hpp"

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
#include <lucent/raw/dng_tags.hpp>
#include <lucent/raw/file_source.hpp>

#include "tiff_reader.hpp"

namespace lucent::raw {

namespace {

constexpr std::string_view kWhereContainer = "raw.exif.container";
constexpr std::string_view kWhereRead = "raw.exif.read";

// Tags read (EXIF 2.32 / 3.0 numbering).
namespace tag {
constexpr std::uint16_t kMake = 271;
constexpr std::uint16_t kModel = 272;
constexpr std::uint16_t kArtist = 315;
constexpr std::uint16_t kCopyright = 33432;
constexpr std::uint16_t kExifIfd = 34665;
constexpr std::uint16_t kGpsIfd = 34853;
constexpr std::uint16_t kExposureTime = 33434;
constexpr std::uint16_t kFNumber = 33437;
constexpr std::uint16_t kPhotographicSensitivity = 34855;
constexpr std::uint16_t kRecommendedExposureIndex = 34866;
constexpr std::uint16_t kIsoSpeed = 34867;
constexpr std::uint16_t kDateTimeOriginal = 36867;
constexpr std::uint16_t kOffsetTimeOriginal = 36881;
constexpr std::uint16_t kExposureBias = 37380;
constexpr std::uint16_t kFocalLength = 37386;
constexpr std::uint16_t kSubSecTimeOriginal = 37521;
constexpr std::uint16_t kFocalLength35mm = 41989;
constexpr std::uint16_t kLensMake = 42035;
constexpr std::uint16_t kLensModel = 42036;
constexpr std::uint16_t kGpsLatitudeRef = 1;
constexpr std::uint16_t kGpsLatitude = 2;
constexpr std::uint16_t kGpsLongitudeRef = 3;
constexpr std::uint16_t kGpsLongitude = 4;
constexpr std::uint16_t kGpsAltitudeRef = 5;
constexpr std::uint16_t kGpsAltitude = 6;
constexpr std::uint16_t kGpsTimeStamp = 7;
constexpr std::uint16_t kGpsDateStamp = 29;
}  // namespace tag

constexpr std::uint16_t kTypeByte = 1;
constexpr std::uint16_t kTypeAscii = 2;
constexpr std::uint16_t kTypeShort = 3;
constexpr std::uint16_t kTypeLong = 4;
constexpr std::uint16_t kTypeRational = 5;
constexpr std::uint16_t kTypeUndefined = 7;
constexpr std::uint16_t kTypeSRational = 10;
constexpr std::uint16_t kTypeIfd = 13;
constexpr std::uint16_t kTypeLong8 = 16;
constexpr std::uint16_t kTypeIfd8 = 18;

// Which IFD the first IFD of a TIFF stream is.
enum class StreamRole : std::uint8_t {
    Main,     // a whole TIFF/EXIF stream: IFD 0, following its Exif and GPS pointers
    Ifd0,     // CR3 CMT1: IFD 0 only (pointers there are relative to another stream)
    ExifIfd,  // CR3 CMT2
    GpsIfd,   // CR3 CMT4
};

std::uint64_t load_be(const std::byte* data, std::size_t size) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        value = (value << 8) | std::to_integer<std::uint64_t>(data[i]);
    }
    return value;
}

bool in_bounds(std::uint64_t size, std::uint64_t offset, std::uint64_t length) noexcept {
    return offset <= size && length <= size - offset;
}

// Reads exactly `length` bytes at `offset`. Precondition: in bounds of the source.
Expected<std::vector<std::byte>> read_bytes(RandomAccessSource& source, std::uint64_t offset,
                                            std::uint64_t length) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    if (bytes.empty()) {
        return bytes;
    }
    auto read = source.read_at(offset, bytes);
    if (!read) {
        return Unexpected(std::move(read.error()));
    }
    if (*read != bytes.size()) {
        return fail(ErrorCode::IoError,
                    std::format("source returned {} of {} bytes at offset {}", *read, bytes.size(), offset),
                    std::string(kWhereRead));
    }
    return bytes;
}

bool is_control(unsigned char c) noexcept {
    return c < 0x20 || c == 0x7F;
}

// See "Text" in the header. `bytes` is the raw tag value.
std::string clean_text(std::span<const std::byte> bytes, bool copyright) {
    const auto* begin = reinterpret_cast<const char*>(bytes.data());
    std::string_view raw(begin, bytes.size());
    std::string_view part = raw.substr(0, raw.find('\0'));
    if (copyright) {
        // "photographer\0editor\0": a lone space for the photographer means "editor only".
        std::string_view first = part;
        while (!first.empty() && first.front() == ' ') {
            first.remove_prefix(1);
        }
        if (first.empty() && part.size() < raw.size()) {
            std::string_view rest = raw.substr(part.size() + 1);
            part = rest.substr(0, rest.find('\0'));
        }
    }
    std::string text = core::sanitize_utf8(part);
    for (char& c : text) {
        if (is_control(static_cast<unsigned char>(c))) {
            c = ' ';
        }
    }
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(' ');
    return text.substr(first, last - first + 1);
}

bool all_digits(std::string_view text) noexcept {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int parse_int(std::string_view digits) noexcept {
    int value = 0;
    for (const char c : digits) {
        value = value * 10 + (c - '0');
    }
    return value;
}

bool is_leap(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

bool valid_date(int year, int month, int day) noexcept {
    static constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1) {
        return false;
    }
    const int days = kDays[static_cast<std::size_t>(month - 1)] + (month == 2 && is_leap(year) ? 1 : 0);
    return day <= days;
}

enum class ParseOutcome : std::uint8_t { Ok, Unknown, Invalid };

// "YYYY:MM:DD HH:MM:SS" (also "YYYY-MM-DDTHH:MM:SS", the decoder's form). Blank or all-zero
// values are Unknown, as EXIF specifies for an unknown time.
ParseOutcome parse_date_time(std::string_view text, ExifDateTime& out) {
    if (text.find_first_not_of(" :0") == std::string_view::npos) {
        return ParseOutcome::Unknown;
    }
    if (text.size() != 19) {
        return ParseOutcome::Invalid;
    }
    const auto date_sep = [](char c) { return c == ':' || c == '-'; };
    if (!date_sep(text[4]) || text[7] != text[4] || (text[10] != ' ' && text[10] != 'T') || text[13] != ':' ||
        text[16] != ':') {
        return ParseOutcome::Invalid;
    }
    const std::array<std::string_view, 6> fields = {text.substr(0, 4),  text.substr(5, 2),  text.substr(8, 2),
                                                    text.substr(11, 2), text.substr(14, 2), text.substr(17, 2)};
    for (const std::string_view field : fields) {
        if (!all_digits(field)) {
            return ParseOutcome::Invalid;
        }
    }
    ExifDateTime value;
    value.year = parse_int(fields[0]);
    value.month = parse_int(fields[1]);
    value.day = parse_int(fields[2]);
    value.hour = parse_int(fields[3]);
    value.minute = parse_int(fields[4]);
    value.second = parse_int(fields[5]);
    if (!valid_date(value.year, value.month, value.day) || value.hour > 23 || value.minute > 59 ||
        value.second > 59) {
        return ParseOutcome::Invalid;
    }
    out = std::move(value);
    return ParseOutcome::Ok;
}

// "+HH:MM" / "-HH:MM".
ParseOutcome parse_offset(std::string_view text, std::int32_t& minutes) {
    if (text.find_first_not_of(" :") == std::string_view::npos) {
        return ParseOutcome::Unknown;
    }
    if (text.size() != 6 || (text[0] != '+' && text[0] != '-') || text[3] != ':' || !all_digits(text.substr(1, 2)) ||
        !all_digits(text.substr(4, 2))) {
        return ParseOutcome::Invalid;
    }
    const int hours = parse_int(text.substr(1, 2));
    const int mins = parse_int(text.substr(4, 2));
    if (mins > 59 || hours * 60 + mins > 14 * 60) {
        return ParseOutcome::Invalid;
    }
    minutes = (text[0] == '-' ? -1 : 1) * (hours * 60 + mins);
    return ParseOutcome::Ok;
}

// One IFD with typed, tolerant accessors: a malformed value is reported as a warning and
// treated as absent.
class IfdFields {
public:
    IfdFields(detail::TiffReader& reader, detail::TiffIfd ifd, std::string name, const CaptureMetadataLimits& limits,
              std::vector<std::string>& warnings)
        : reader_(&reader), ifd_(std::move(ifd)), name_(std::move(name)), limits_(&limits), warnings_(&warnings) {}

    [[nodiscard]] const detail::TiffEntry* find(std::uint16_t tag) const noexcept {
        for (const detail::TiffEntry& entry : ifd_.entries) {
            if (entry.tag == tag) {
                return &entry;
            }
        }
        return nullptr;
    }

    void warn(std::uint16_t tag, std::string_view what) {
        warnings_->push_back(std::format("{} tag {}: {}", name_, tag, what));
    }

    // The value bytes of `tag` when it has one of `types` and at least `min_count` values.
    std::optional<std::vector<std::byte>> bytes(std::uint16_t tag, std::initializer_list<std::uint16_t> types,
                                                std::uint64_t min_count) {
        const detail::TiffEntry* entry = find(tag);
        if (entry == nullptr) {
            return std::nullopt;
        }
        if (std::find(types.begin(), types.end(), entry->type) == types.end()) {
            warn(tag, std::format("unexpected field type {}", entry->type));
            return std::nullopt;
        }
        if (entry->count < min_count) {
            warn(tag, std::format("{} values where at least {} are required", entry->count, min_count));
            return std::nullopt;
        }
        if (entry->status != detail::EntryStatus::Ok) {
            warn(tag, "the value lies outside the source");
            return std::nullopt;
        }
        auto value = reader_->read_value(*entry, limits_->max_value_bytes, name_);
        if (!value) {
            warn(tag, value.error().message);
            return std::nullopt;
        }
        return std::move(*value);
    }

    std::string text(std::uint16_t tag, bool copyright = false) {
        if (const detail::TiffEntry* entry = find(tag); entry != nullptr && entry->count == 0) {
            return {};  // an empty value (Leica writes Artist and Copyright with count 0)
        }
        auto value = bytes(tag, {kTypeAscii, kTypeUndefined, kTypeByte}, 1);
        return value ? clean_text(*value, copyright) : std::string();
    }

    // First value of an unsigned integer tag (BYTE, SHORT, LONG and the IFD pointer types).
    std::optional<std::uint64_t> unsigned_value(std::uint16_t tag) {
        const detail::TiffEntry* entry = find(tag);
        if (entry == nullptr) {
            return std::nullopt;
        }
        const std::uint32_t size = tiff_field_type_size(entry->type);
        auto value = bytes(tag, {kTypeByte, kTypeShort, kTypeLong, kTypeIfd, kTypeLong8, kTypeIfd8}, 1);
        if (!value) {
            return std::nullopt;
        }
        return reader_->load_unsigned(value->data(), size);
    }

    // `count` RATIONALs; std::nullopt (with a warning) when any denominator is zero.
    std::optional<std::vector<ExifRational>> rationals(std::uint16_t tag, std::uint64_t count) {
        auto value = bytes(tag, {kTypeRational}, count);
        if (!value) {
            return std::nullopt;
        }
        std::vector<ExifRational> out;
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::byte* p = value->data() + i * 8;
            out.push_back({static_cast<std::uint32_t>(reader_->load_unsigned(p, 4)),
                           static_cast<std::uint32_t>(reader_->load_unsigned(p + 4, 4))});
        }
        return out;
    }

    // A positive RATIONAL. A zero numerator means "unknown" (absent, no warning).
    std::optional<ExifRational> positive_rational(std::uint16_t tag) {
        auto values = rationals(tag, 1);
        if (!values) {
            return std::nullopt;
        }
        const ExifRational r = values->front();
        if (r.numerator == 0) {
            return std::nullopt;
        }
        if (r.denominator == 0) {
            warn(tag, "zero denominator");
            return std::nullopt;
        }
        return r;
    }

    std::optional<ExifSignedRational> signed_rational(std::uint16_t tag) {
        auto value = bytes(tag, {kTypeSRational}, 1);
        if (!value) {
            return std::nullopt;
        }
        const ExifSignedRational r{static_cast<std::int32_t>(reader_->load_unsigned(value->data(), 4)),
                                   static_cast<std::int32_t>(reader_->load_unsigned(value->data() + 4, 4))};
        if (r.denominator == 0) {
            if (r.numerator != 0) {
                warn(tag, "zero denominator");
            }
            return std::nullopt;
        }
        return r;
    }

private:
    detail::TiffReader* reader_;
    detail::TiffIfd ifd_;
    std::string name_;
    const CaptureMetadataLimits* limits_;
    std::vector<std::string>* warnings_;
};

void read_ifd0_fields(IfdFields& ifd, CaptureMetadata& out) {
    out.make = ifd.text(tag::kMake);
    out.model = ifd.text(tag::kModel);
    out.artist = ifd.text(tag::kArtist);
    out.copyright = ifd.text(tag::kCopyright, true);
}

void read_exif_fields(IfdFields& ifd, CaptureMetadata& out) {
    out.exposure_time_s = ifd.positive_rational(tag::kExposureTime);
    out.f_number = ifd.positive_rational(tag::kFNumber);
    out.focal_length_mm = ifd.positive_rational(tag::kFocalLength);
    out.exposure_bias_ev = ifd.signed_rational(tag::kExposureBias);
    out.lens_make = ifd.text(tag::kLensMake);
    out.lens_model = ifd.text(tag::kLensModel);

    if (auto focal35 = ifd.unsigned_value(tag::kFocalLength35mm); focal35 && *focal35 != 0) {
        if (*focal35 <= std::numeric_limits<std::uint16_t>::max()) {
            out.focal_length_35mm_mm = static_cast<std::uint16_t>(*focal35);
        } else {
            ifd.warn(tag::kFocalLength35mm, "value out of range");
        }
    }

    std::optional<std::uint64_t> iso = ifd.unsigned_value(tag::kPhotographicSensitivity);
    if (iso && *iso == 65535) {
        // EXIF: 65535 means "65535 or more"; the exact value is in one of these.
        for (const std::uint16_t precise : {tag::kRecommendedExposureIndex, tag::kIsoSpeed}) {
            if (auto value = ifd.unsigned_value(precise); value && *value != 0) {
                iso = value;
                break;
            }
        }
    }
    if (iso && *iso != 0 && *iso <= std::numeric_limits<std::uint32_t>::max()) {
        out.iso = static_cast<std::uint32_t>(*iso);
    }

    if (ifd.find(tag::kDateTimeOriginal) != nullptr) {
        ExifDateTime time;
        const std::string text = ifd.text(tag::kDateTimeOriginal);
        const ParseOutcome outcome = parse_date_time(text, time);
        if (outcome == ParseOutcome::Invalid) {
            ifd.warn(tag::kDateTimeOriginal, std::format("\"{}\" is not a valid date and time", text));
        } else if (outcome == ParseOutcome::Ok) {
            const std::string subsec = ifd.text(tag::kSubSecTimeOriginal);
            if (!subsec.empty()) {
                if (all_digits(subsec) && subsec.size() <= 9) {
                    time.subsec = subsec;
                } else {
                    ifd.warn(tag::kSubSecTimeOriginal, std::format("\"{}\" is not 1 to 9 digits", subsec));
                }
            }
            if (ifd.find(tag::kOffsetTimeOriginal) != nullptr) {
                const std::string offset = ifd.text(tag::kOffsetTimeOriginal);
                std::int32_t minutes = 0;
                const ParseOutcome parsed = parse_offset(offset, minutes);
                if (parsed == ParseOutcome::Ok) {
                    time.utc_offset_minutes = minutes;
                } else if (parsed == ParseOutcome::Invalid) {
                    ifd.warn(tag::kOffsetTimeOriginal, std::format("\"{}\" is not a UTC offset", offset));
                }
            }
            out.capture_time = std::move(time);
        }
    }
}

// Degrees from three RATIONALs (degrees, minutes, seconds); std::nullopt when malformed.
std::optional<double> dms_degrees(IfdFields& ifd, std::uint16_t tag) {
    auto values = ifd.rationals(tag, 3);
    if (!values) {
        return std::nullopt;
    }
    for (const ExifRational& r : *values) {
        if (r.denominator == 0) {
            ifd.warn(tag, "zero denominator");
            return std::nullopt;
        }
    }
    return (*values)[0].value() + (*values)[1].value() / 60.0 + (*values)[2].value() / 3600.0;
}

void read_gps_fields(IfdFields& ifd, CaptureMetadata& out) {
    if (ifd.find(tag::kGpsLatitude) == nullptr && ifd.find(tag::kGpsLongitude) == nullptr) {
        return;  // a GPS IFD without a position (common: GPSVersionID only)
    }
    const std::optional<double> latitude = dms_degrees(ifd, tag::kGpsLatitude);
    const std::optional<double> longitude = dms_degrees(ifd, tag::kGpsLongitude);
    const std::string lat_ref = ifd.text(tag::kGpsLatitudeRef);
    const std::string lon_ref = ifd.text(tag::kGpsLongitudeRef);
    if (!latitude || !longitude) {
        ifd.warn(tag::kGpsLatitude, "the GPS position is incomplete; it is dropped");
        return;
    }
    if ((lat_ref != "N" && lat_ref != "S") || (lon_ref != "E" && lon_ref != "W")) {
        ifd.warn(tag::kGpsLatitudeRef, "missing or invalid latitude/longitude reference; the position is dropped");
        return;
    }
    if (*latitude > 90.0 || *longitude > 180.0) {
        ifd.warn(tag::kGpsLatitude, "position out of range; it is dropped");
        return;
    }
    if (*latitude == 0.0 && *longitude == 0.0) {
        // Cameras without a fix write zeros (seen in DJI files); 0,0 is open ocean.
        ifd.warn(tag::kGpsLatitude, "position 0,0 (no fix); it is dropped");
        return;
    }
    GpsLocation location;
    location.latitude_deg = lat_ref == "S" ? -*latitude : *latitude;
    location.longitude_deg = lon_ref == "W" ? -*longitude : *longitude;

    if (ifd.find(tag::kGpsAltitude) != nullptr) {
        auto altitude = ifd.rationals(tag::kGpsAltitude, 1);
        if (altitude && altitude->front().denominator != 0) {
            const std::uint64_t ref = ifd.unsigned_value(tag::kGpsAltitudeRef).value_or(0);
            if (ref == 0 || ref == 1) {
                location.altitude_m = ref == 1 ? -altitude->front().value() : altitude->front().value();
            } else {
                ifd.warn(tag::kGpsAltitudeRef, std::format("unknown altitude reference {}", ref));
            }
        } else if (altitude) {
            ifd.warn(tag::kGpsAltitude, "zero denominator");
        }
    }

    if (ifd.find(tag::kGpsTimeStamp) != nullptr && ifd.find(tag::kGpsDateStamp) != nullptr) {
        auto time = ifd.rationals(tag::kGpsTimeStamp, 3);
        const std::string date = ifd.text(tag::kGpsDateStamp);
        ExifDateTime parsed;
        const bool date_ok = parse_date_time(date + " 00:00:00", parsed) == ParseOutcome::Ok;
        bool time_ok = time.has_value() &&
                       std::all_of(time->begin(), time->end(), [](const ExifRational& r) { return r.denominator != 0; });
        if (date_ok && time_ok) {
            const double hour = (*time)[0].value();
            const double minute = (*time)[1].value();
            const double second = (*time)[2].value();
            time_ok = hour == std::floor(hour) && hour <= 23 && minute == std::floor(minute) && minute <= 59 &&
                      second < 60.0;
            if (time_ok) {
                location.utc = GpsTimestamp{parsed.year, parsed.month, parsed.day, static_cast<std::int32_t>(hour),
                                            static_cast<std::int32_t>(minute), second};
            }
        }
        if (!date_ok || !time_ok) {
            ifd.warn(tag::kGpsTimeStamp, "invalid GPS date or time stamp; it is dropped");
        }
    }
    out.gps = location;
}

// Reads one IFD of a stream, or records a warning and returns std::nullopt.
std::optional<IfdFields> open_ifd(detail::TiffReader& reader, std::uint64_t offset, std::string name,
                                  const CaptureMetadataLimits& limits, CaptureMetadata& out) {
    auto ifd = reader.read_ifd(offset, limits.max_entries_per_ifd, name);
    if (!ifd) {
        out.warnings.push_back(std::format("{}: {}", name, ifd.error().message));
        return std::nullopt;
    }
    return IfdFields(reader, std::move(*ifd), std::move(name), limits, out.warnings);
}

// Parses a TIFF/EXIF stream. Failures to open the stream or read its first IFD are returned
// (the caller decides whether they are fatal); everything below is tolerated with warnings.
Status parse_tiff_stream(RandomAccessSource& source, StreamRole role, std::string_view name,
                         const CaptureMetadataLimits& limits, CaptureMetadata& out) {
    auto reader = detail::TiffReader::open(source, detail::TiffSignatures::WithVendorRaw);
    if (!reader) {
        return Unexpected(std::move(reader.error()));
    }
    const std::string first_name = std::string(name) + (role == StreamRole::ExifIfd  ? ".exif"
                                                         : role == StreamRole::GpsIfd ? ".gps"
                                                                                       : ".ifd0");
    auto first = reader->read_ifd(reader->first_ifd_offset(), limits.max_entries_per_ifd, first_name);
    if (!first) {
        return Unexpected(std::move(first.error()));
    }
    IfdFields ifd0(*reader, std::move(*first), first_name, limits, out.warnings);
    switch (role) {
        case StreamRole::ExifIfd:
            read_exif_fields(ifd0, out);
            return ok();
        case StreamRole::GpsIfd:
            read_gps_fields(ifd0, out);
            return ok();
        case StreamRole::Ifd0:
            read_ifd0_fields(ifd0, out);
            return ok();
        case StreamRole::Main:
            break;
    }
    read_ifd0_fields(ifd0, out);
    if (auto exif = ifd0.unsigned_value(tag::kExifIfd); exif && *exif != 0) {
        if (auto fields = open_ifd(*reader, *exif, std::string(name) + ".exif", limits, out)) {
            read_exif_fields(*fields, out);
        }
    }
    if (auto gps = ifd0.unsigned_value(tag::kGpsIfd); gps && *gps != 0) {
        if (auto fields = open_ifd(*reader, *gps, std::string(name) + ".gps", limits, out)) {
            read_gps_fields(*fields, out);
        }
    }
    return ok();
}

// Loads [offset, offset + length) and parses it as an embedded TIFF stream; problems become
// warnings.
Status parse_embedded(RandomAccessSource& source, std::uint64_t offset, std::uint64_t length, StreamRole role,
                      std::string_view name, const CaptureMetadataLimits& limits, CaptureMetadata& out) {
    if (length > limits.max_embedded_bytes) {
        out.warnings.push_back(std::format("{}: {} bytes exceed the {}-byte limit; skipped", name, length,
                                           limits.max_embedded_bytes));
        return ok();
    }
    auto bytes = read_bytes(source, offset, length);
    if (!bytes) {
        return Unexpected(std::move(bytes.error()));
    }
    MemorySource memory(*bytes);
    if (auto status = parse_tiff_stream(memory, role, name, limits, out); !status) {
        out.warnings.push_back(std::format("{}: {}", name, status.error().message));
    }
    return ok();
}

Error container_error(std::string message) {
    return Error{ErrorCode::Corrupt, std::move(message), std::string(kWhereContainer), 0};
}

// Walks the JPEG marker segments of [base, base + length) up to the first scan and parses the
// first APP1 "Exif\0\0" segment.
Status read_jpeg_exif(RandomAccessSource& source, std::uint64_t base, std::uint64_t length, std::string_view name,
                      const CaptureMetadataLimits& limits, CaptureMetadata& out) {
    const std::uint64_t end = base + length;
    std::uint64_t pos = base + 2;  // after SOI
    for (std::uint32_t segment = 0; segment < limits.max_jpeg_segments; ++segment) {
        // Marker: 0xFF, optional 0xFF fill bytes, then the code.
        std::uint8_t code = 0;
        bool marker = false;
        for (;;) {
            if (pos >= end) {
                return Unexpected(container_error(std::format("{}: JPEG ends before its first scan", name)));
            }
            auto b = read_bytes(source, pos, 1);
            if (!b) {
                return Unexpected(std::move(b.error()));
            }
            const auto value = std::to_integer<std::uint8_t>((*b)[0]);
            ++pos;
            if (value == 0xFF) {
                marker = true;
                continue;
            }
            if (!marker || value == 0x00) {
                return Unexpected(container_error(std::format("{}: expected a JPEG marker at offset {}", name, pos - 1)));
            }
            code = value;
            break;
        }
        if (code == 0xD9 || code == 0xDA) {
            return ok();  // EOI or SOS: no EXIF before the image data
        }
        if ((code >= 0xD0 && code <= 0xD7) || code == 0x01) {
            continue;  // RSTn / TEM carry no length
        }
        if (!in_bounds(end, pos, 2)) {
            return Unexpected(container_error(std::format("{}: truncated JPEG segment header", name)));
        }
        auto len_bytes = read_bytes(source, pos, 2);
        if (!len_bytes) {
            return Unexpected(std::move(len_bytes.error()));
        }
        const std::uint64_t seg_length = load_be(len_bytes->data(), 2);
        if (seg_length < 2 || !in_bounds(end, pos, seg_length)) {
            return Unexpected(container_error(
                std::format("{}: JPEG segment 0x{:02X} of {} bytes overruns the stream", name, code, seg_length)));
        }
        if (code == 0xE1 && seg_length >= 2 + 6 + 8) {
            auto signature = read_bytes(source, pos + 2, 6);
            if (!signature) {
                return Unexpected(std::move(signature.error()));
            }
            if (std::memcmp(signature->data(), "Exif\0\0", 6) == 0) {
                return parse_embedded(source, pos + 8, seg_length - 8, StreamRole::Main, name, limits, out);
            }
        }
        pos += seg_length;
    }
    out.warnings.push_back(std::format("{}: no EXIF within the first {} JPEG segments", name,
                                       limits.max_jpeg_segments));
    return ok();
}

// RAF (big-endian header): "FUJIFILM" ... at byte 84 the offset and length of the JPEG preview.
Status read_raf(RandomAccessSource& source, const CaptureMetadataLimits& limits, CaptureMetadata& out) {
    if (source.size() < 92) {
        return Unexpected(container_error("RAF header is truncated"));
    }
    auto header = read_bytes(source, 84, 8);
    if (!header) {
        return Unexpected(std::move(header.error()));
    }
    const std::uint64_t offset = load_be(header->data(), 4);
    const std::uint64_t length = load_be(header->data() + 4, 4);
    if (length < 4 || !in_bounds(source.size(), offset, length)) {
        return Unexpected(container_error("RAF JPEG preview lies outside the file"));
    }
    auto soi = read_bytes(source, offset, 2);
    if (!soi) {
        return Unexpected(std::move(soi.error()));
    }
    if (std::to_integer<int>((*soi)[0]) != 0xFF || std::to_integer<int>((*soi)[1]) != 0xD8) {
        out.warnings.push_back("raf: the preview is not a JPEG stream; no EXIF read");
        return ok();
    }
    return read_jpeg_exif(source, offset, length, "raf.jpeg", limits, out);
}

struct Box {
    std::uint64_t start = 0;    // first byte of the header
    std::uint64_t payload = 0;  // first byte after the header
    std::uint64_t end = 0;      // one past the last byte
    std::array<char, 4> type{};
};

// Reads the box header at `pos` inside [pos, parent_end).
Expected<Box> read_box(RandomAccessSource& source, std::uint64_t pos, std::uint64_t parent_end) {
    if (!in_bounds(parent_end, pos, 8)) {
        return Unexpected(container_error(std::format("CR3 box header at {} is truncated", pos)));
    }
    auto header = read_bytes(source, pos, 8);
    if (!header) {
        return Unexpected(std::move(header.error()));
    }
    Box box;
    box.start = pos;
    std::memcpy(box.type.data(), header->data() + 4, 4);
    std::uint64_t size = load_be(header->data(), 4);
    box.payload = pos + 8;
    if (size == 1) {
        if (!in_bounds(parent_end, pos, 16)) {
            return Unexpected(container_error(std::format("CR3 box header at {} is truncated", pos)));
        }
        auto large = read_bytes(source, pos + 8, 8);
        if (!large) {
            return Unexpected(std::move(large.error()));
        }
        size = load_be(large->data(), 8);
        box.payload = pos + 16;
    } else if (size == 0) {
        size = parent_end - pos;
    }
    if (size < box.payload - pos || !in_bounds(parent_end, pos, size)) {
        return Unexpected(container_error(std::format("CR3 box at {} with size {} overruns its parent", pos, size)));
    }
    box.end = pos + size;
    return box;
}

bool is_type(const Box& box, std::string_view type) noexcept {
    return std::memcmp(box.type.data(), type.data(), 4) == 0;
}

constexpr std::array<std::uint8_t, 16> kCanonUuid = {0x85, 0xc0, 0xb6, 0x87, 0x82, 0x0f, 0x11, 0xe0,
                                                     0x81, 0x11, 0xf4, 0xce, 0x46, 0x2b, 0x6a, 0x48};

Status read_cr3(RandomAccessSource& source, const CaptureMetadataLimits& limits, CaptureMetadata& out) {
    std::uint32_t visited = 0;
    const auto next_box = [&](std::uint64_t pos, std::uint64_t end) -> Expected<Box> {
        if (++visited > limits.max_boxes) {
            return Unexpected(container_error(std::format("more than {} CR3 boxes", limits.max_boxes)));
        }
        return read_box(source, pos, end);
    };

    const std::uint64_t file_end = source.size();
    for (std::uint64_t pos = 0; pos < file_end;) {
        auto top = next_box(pos, file_end);
        if (!top) {
            return Unexpected(std::move(top.error()));
        }
        if (is_type(*top, "moov")) {
            for (std::uint64_t child = top->payload; child < top->end;) {
                auto box = next_box(child, top->end);
                if (!box) {
                    return Unexpected(std::move(box.error()));
                }
                if (is_type(*box, "uuid") && in_bounds(box->end, box->payload, 16)) {
                    auto uuid = read_bytes(source, box->payload, 16);
                    if (!uuid) {
                        return Unexpected(std::move(uuid.error()));
                    }
                    if (std::memcmp(uuid->data(), kCanonUuid.data(), 16) == 0) {
                        for (std::uint64_t inner = box->payload + 16; inner < box->end;) {
                            auto item = next_box(inner, box->end);
                            if (!item) {
                                return Unexpected(std::move(item.error()));
                            }
                            const std::uint64_t length = item->end - item->payload;
                            Status status = ok();
                            if (is_type(*item, "CMT1")) {
                                status = parse_embedded(source, item->payload, length, StreamRole::Ifd0, "cr3.cmt1",
                                                        limits, out);
                            } else if (is_type(*item, "CMT2")) {
                                status = parse_embedded(source, item->payload, length, StreamRole::ExifIfd,
                                                        "cr3.cmt2", limits, out);
                            } else if (is_type(*item, "CMT4")) {
                                status = parse_embedded(source, item->payload, length, StreamRole::GpsIfd,
                                                        "cr3.cmt4", limits, out);
                            }
                            if (!status) {
                                return status;
                            }
                            inner = item->end;
                        }
                        return ok();
                    }
                }
                child = box->end;
            }
            out.warnings.push_back("cr3: no Canon metadata box in moov");
            return ok();
        }
        pos = top->end;
    }
    out.warnings.push_back("cr3: no moov box");
    return ok();
}

Expected<CaptureMetadata> read_impl(RandomAccessSource& source, const CaptureMetadataLimits& limits) {
    std::array<std::byte, 16> lead{};
    const std::uint64_t lead_size = std::min<std::uint64_t>(source.size(), lead.size());
    auto read = source.read_at(0, std::span<std::byte>(lead.data(), static_cast<std::size_t>(lead_size)));
    if (!read) {
        return Unexpected(std::move(read.error()));
    }
    const std::span<const std::byte> leading(lead.data(), *read);

    CaptureMetadata out;
    Status status = ok();
    if (leading.size() >= 3 && std::to_integer<int>(leading[0]) == 0xFF && std::to_integer<int>(leading[1]) == 0xD8 &&
        std::to_integer<int>(leading[2]) == 0xFF) {
        status = read_jpeg_exif(source, 0, source.size(), "jpeg", limits, out);
    } else {
        switch (detect_container_format(leading)) {
            case ContainerFormat::Tiff:
            case ContainerFormat::Dng:
            case ContainerFormat::Orf:
            case ContainerFormat::Rw2:
                status = parse_tiff_stream(source, StreamRole::Main, "tiff", limits, out);
                break;
            case ContainerFormat::Cr3:
                status = read_cr3(source, limits, out);
                break;
            case ContainerFormat::Raf:
                status = read_raf(source, limits, out);
                break;
            default:
                return fail(ErrorCode::Unsupported, "no EXIF reader for this container", std::string(kWhereContainer));
        }
    }
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    return out;
}

}  // namespace

std::optional<ExifRational> ExifRational::approximate(double value, std::uint32_t max_denominator) {
    constexpr double kMax = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    if (!std::isfinite(value) || value < 0.0 || value > kMax) {
        return std::nullopt;
    }
    max_denominator = std::max<std::uint32_t>(max_denominator, 1);
    if (value > 0.0 && value < 1.0) {
        const double reciprocal = 1.0 / value;
        const double n = std::round(reciprocal);
        if (n >= 1.0 && n <= kMax && std::abs(reciprocal - n) <= 0.005 * n) {
            return ExifRational{1, static_cast<std::uint32_t>(n)};
        }
    }
    // Continued-fraction convergents h/k, stopping before the denominator limit.
    std::uint64_t h_prev = 1;
    std::uint64_t h = static_cast<std::uint64_t>(std::floor(value));
    std::uint64_t k_prev = 0;
    std::uint64_t k = 1;
    double x = value;
    for (int i = 0; i < 64; ++i) {
        const double fraction = x - std::floor(x);
        if (fraction < 1e-12) {
            break;
        }
        x = 1.0 / fraction;
        if (x > kMax) {
            break;
        }
        const auto a = static_cast<std::uint64_t>(std::floor(x));
        const std::uint64_t h_next = a * h + h_prev;
        const std::uint64_t k_next = a * k + k_prev;
        if (k_next > max_denominator || h_next > std::numeric_limits<std::uint32_t>::max()) {
            break;
        }
        h_prev = h;
        h = h_next;
        k_prev = k;
        k = k_next;
    }
    return ExifRational{static_cast<std::uint32_t>(h), static_cast<std::uint32_t>(k)};
}

Expected<CaptureMetadata> read_capture_metadata(RandomAccessSource& source, const CaptureMetadataLimits& limits) {
    try {
        return read_impl(source, limits);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while reading EXIF", std::string(kWhereRead));
    }
}

Expected<CaptureMetadata> read_capture_metadata(const std::filesystem::path& path,
                                                const CaptureMetadataLimits& limits) {
    auto file = core::ReadOnlyFile::open(path);
    if (!file) {
        return Unexpected(std::move(file.error()));
    }
    auto source = FileSource::create(*file);
    if (!source) {
        return Unexpected(std::move(source.error()));
    }
    return read_capture_metadata(*source, limits);
}

void complete_from_decoder(CaptureMetadata& metadata, const RawMetadata& decoded) {
    const auto fill_text = [](std::string& target, const std::string& value) {
        if (target.empty()) {
            target = value;
        }
    };
    fill_text(metadata.make, decoded.camera.make);
    fill_text(metadata.model, decoded.camera.model);
    fill_text(metadata.lens_make, decoded.lens.make);
    fill_text(metadata.lens_model, decoded.lens.model);

    const auto positive = [](const std::optional<float>& value) -> std::optional<ExifRational> {
        if (!value || !(*value > 0.0f)) {
            return std::nullopt;
        }
        return ExifRational::approximate(static_cast<double>(*value));
    };
    if (!metadata.exposure_time_s) {
        metadata.exposure_time_s = positive(decoded.capture.exposure_time_s);
    }
    if (!metadata.f_number) {
        metadata.f_number = positive(decoded.capture.f_number);
    }
    if (!metadata.focal_length_mm) {
        metadata.focal_length_mm = positive(decoded.capture.focal_length_mm);
    }
    if (!metadata.iso && decoded.capture.iso && *decoded.capture.iso >= 0.5f && *decoded.capture.iso < 4.0e9f) {
        metadata.iso = static_cast<std::uint32_t>(std::lround(static_cast<double>(*decoded.capture.iso)));
    }
    if (!metadata.focal_length_35mm_mm && decoded.lens.focal_length_35mm_equivalent_mm) {
        const double mm = std::round(static_cast<double>(*decoded.lens.focal_length_35mm_equivalent_mm));
        if (mm >= 1.0 && mm <= 65535.0) {
            metadata.focal_length_35mm_mm = static_cast<std::uint16_t>(mm);
        }
    }
    if (!metadata.capture_time && decoded.capture.capture_time_local) {
        ExifDateTime time;
        if (parse_date_time(*decoded.capture.capture_time_local, time) == ParseOutcome::Ok) {
            metadata.capture_time = std::move(time);
        }
    }
}

}  // namespace lucent::raw
