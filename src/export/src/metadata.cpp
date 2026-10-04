#include "lucent/export/metadata.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lucent/core/text.hpp>
#include <lucent/core/version.hpp>

#include "exif_format.hpp"
#include "exif_reader.hpp"

namespace lucent::exporter {
namespace {

bool is_control(char32_t c) noexcept {
    return c < 0x20 || c == 0x7F;
}

// Decodes one code point of well-formed UTF-8 at `i` and advances `i`.
char32_t next_code_point(std::string_view text, std::size_t& i) noexcept {
    const auto lead = static_cast<unsigned char>(text[i]);
    const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    char32_t c = length == 1 ? lead : length == 2 ? (lead & 0x1F) : length == 3 ? (lead & 0x0F) : (lead & 0x07);
    for (std::size_t k = 1; k < length; ++k) {
        c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    }
    i += length;
    return c;
}

std::string trim_spaces(const std::string& text) {
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

// See "Text" in metadata.hpp. `ascii_only` selects the ASCII rule.
std::string clean(const std::string& input, bool ascii_only, std::size_t max_bytes, std::string_view name,
                  std::vector<std::string>& notes) {
    if (input.empty()) {
        return {};
    }
    const std::string valid = core::sanitize_utf8(input);
    bool replaced = valid != input;
    std::string out;
    out.reserve(valid.size());
    for (std::size_t i = 0; i < valid.size();) {
        const std::size_t start = i;
        const char32_t c = next_code_point(valid, i);
        if (is_control(c)) {
            out.push_back(' ');
            replaced = true;
        } else if (ascii_only && c >= 0x80) {
            out.push_back('?');
            replaced = true;
        } else {
            out.append(valid, start, i - start);
        }
    }
    out = trim_spaces(out);
    if (replaced) {
        notes.push_back(std::format("{}: characters that cannot be written were replaced", name));
    }
    if (out.size() > max_bytes) {
        std::size_t cut = max_bytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;  // keep whole UTF-8 sequences
        }
        out = trim_spaces(out.substr(0, cut));
        notes.push_back(std::format("{}: shortened to {} bytes", name, out.size()));
    }
    return out;
}

bool valid_date(int year, int month, int day) noexcept {
    static constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1) {
        return false;
    }
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return day <= kDays[static_cast<std::size_t>(month - 1)] + (month == 2 && leap ? 1 : 0);
}

template <class R>
std::optional<R> positive(const std::optional<R>& value, std::string_view name, std::vector<std::string>& notes) {
    if (value && (value->denominator == 0 || value->numerator == 0)) {
        notes.push_back(std::format("{}: {}/{} is not a positive number; dropped", name, value->numerator,
                                    value->denominator));
        return std::nullopt;
    }
    return value;
}

std::optional<CaptureDateTime> check_time(const std::optional<CaptureDateTime>& value, std::vector<std::string>& notes) {
    if (!value) {
        return std::nullopt;
    }
    CaptureDateTime t = *value;
    if (!valid_date(t.year, t.month, t.day) || t.hour < 0 || t.hour > 23 || t.minute < 0 || t.minute > 59 ||
        t.second < 0 || t.second > 59) {
        notes.push_back("capture time: not a valid date and time; dropped");
        return std::nullopt;
    }
    const bool digits = std::all_of(t.subsec.begin(), t.subsec.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (t.subsec.size() > 9 || !digits) {
        notes.push_back("capture time: sub-seconds are not 0 to 9 digits; dropped");
        t.subsec.clear();
    }
    if (t.utc_offset_minutes && (*t.utc_offset_minutes < -14 * 60 || *t.utc_offset_minutes > 14 * 60)) {
        notes.push_back("capture time: UTC offset outside -14:00..+14:00; dropped");
        t.utc_offset_minutes.reset();
    }
    return t;
}

// Rounds |degrees| to a micro-arcsecond, keeping the sign.
double round_coordinate(double degrees) noexcept {
    constexpr double kScale = 3600.0 * detail::kGpsSecondsDenominator;
    const double magnitude = std::round(std::abs(degrees) * kScale) / kScale;
    return degrees < 0 ? -magnitude : magnitude;
}

std::optional<GpsPosition> check_gps(const std::optional<GpsPosition>& value, std::vector<std::string>& notes) {
    if (!value) {
        return std::nullopt;
    }
    GpsPosition p = *value;
    if (!std::isfinite(p.latitude_deg) || !std::isfinite(p.longitude_deg) || std::abs(p.latitude_deg) > 90.0 ||
        std::abs(p.longitude_deg) > 180.0) {
        notes.push_back("GPS: position not finite or out of range; dropped");
        return std::nullopt;
    }
    p.latitude_deg = round_coordinate(p.latitude_deg);
    p.longitude_deg = round_coordinate(p.longitude_deg);
    if (p.altitude_m) {
        if (!std::isfinite(*p.altitude_m) || std::abs(*p.altitude_m) > kMaxGpsAltitudeMetres) {
            notes.push_back("GPS: altitude not finite or out of range; dropped");
            p.altitude_m.reset();
        } else {
            p.altitude_m = std::round(*p.altitude_m * detail::kAltitudeDenominator) / detail::kAltitudeDenominator;
        }
    }
    if (p.utc) {
        GpsTimestamp& t = *p.utc;
        if (!valid_date(t.year, t.month, t.day) || t.hour < 0 || t.hour > 23 || t.minute < 0 || t.minute > 59 ||
            !std::isfinite(t.second) || t.second < 0.0 || t.second >= 60.0) {
            notes.push_back("GPS: time stamp invalid; dropped");
            p.utc.reset();
        } else {
            // Down to whole milliseconds (the epsilon absorbs binary error in values that already
            // are), never reaching 60 s.
            const double milliseconds =
                std::min(std::floor(t.second * detail::kTimeSecondsDenominator + 1e-6), 59999.0);
            t.second = milliseconds / detail::kTimeSecondsDenominator;
        }
    }
    return p;
}

}  // namespace

bool ExportMetadata::has_capture() const noexcept {
    return !camera_make.empty() || !camera_model.empty() || !lens_make.empty() || !lens_model.empty() ||
           exposure_time_s.has_value() || f_number.has_value() || iso.has_value() || exposure_bias_ev.has_value() ||
           focal_length_mm.has_value() || focal_length_35mm_mm.has_value() || capture_time.has_value();
}

bool ExportMetadata::empty() const noexcept {
    return !has_capture() && artist.empty() && copyright.empty() && !gps.has_value() && software.empty();
}

std::string software_name() {
    return "Lucent " + std::string(core::build_info().version);
}

PreparedMetadata prepare_metadata(const ExportMetadata& source, const MetadataPolicy& policy) {
    PreparedMetadata prepared;
    ExportMetadata& m = prepared.metadata;
    std::vector<std::string>& notes = prepared.notes;
    if (policy.include_capture) {
        m.camera_make = clean(source.camera_make, true, kMaxMetadataAsciiBytes, "Make", notes);
        m.camera_model = clean(source.camera_model, true, kMaxMetadataAsciiBytes, "Model", notes);
        m.lens_make = clean(source.lens_make, true, kMaxMetadataAsciiBytes, "LensMake", notes);
        m.lens_model = clean(source.lens_model, true, kMaxMetadataAsciiBytes, "LensModel", notes);
        m.exposure_time_s = positive(source.exposure_time_s, "ExposureTime", notes);
        m.f_number = positive(source.f_number, "FNumber", notes);
        m.focal_length_mm = positive(source.focal_length_mm, "FocalLength", notes);
        if (source.exposure_bias_ev && source.exposure_bias_ev->denominator == 0) {
            notes.push_back("ExposureBiasValue: zero denominator; dropped");
        } else {
            m.exposure_bias_ev = source.exposure_bias_ev;
        }
        if (source.iso && *source.iso == 0) {
            notes.push_back("ISO: 0; dropped");
        } else {
            m.iso = source.iso;
        }
        if (source.focal_length_35mm_mm && *source.focal_length_35mm_mm == 0) {
            notes.push_back("FocalLengthIn35mmFilm: 0; dropped");
        } else {
            m.focal_length_35mm_mm = source.focal_length_35mm_mm;
        }
        m.capture_time = check_time(source.capture_time, notes);
    }
    if (policy.include_copyright) {
        m.artist = clean(source.artist, false, kMaxMetadataTextBytes, "Artist", notes);
        m.copyright = clean(source.copyright, false, kMaxMetadataTextBytes, "Copyright", notes);
    }
    if (policy.include_gps) {
        m.gps = check_gps(source.gps, notes);
    }
    if (policy.include_software) {
        m.software = clean(software_name(), true, kMaxMetadataAsciiBytes, "Software", notes);
    }
    return prepared;
}

Expected<ExportMetadata> read_export_metadata(const std::filesystem::path& path) {
    auto parsed = detail::read_exif_file(path);
    if (!parsed) {
        return Unexpected(std::move(parsed.error()));
    }
    return std::move(parsed->metadata);
}

}  // namespace lucent::exporter
