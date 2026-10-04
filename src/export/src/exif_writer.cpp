#include "exif_writer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "exif_format.hpp"

namespace lucent::exporter::detail {
namespace {

constexpr const char* kWhere = "export.metadata.write";

struct Entry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::vector<std::uint8_t> value;  // little-endian value bytes
};

void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

void set32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) {
        out[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

class Ifd {
public:
    void ascii(std::uint16_t tag, std::string_view text) {
        if (text.empty()) {
            return;
        }
        Entry e{tag, exif_type::kAscii, static_cast<std::uint32_t>(text.size() + 1), {}};
        e.value.assign(text.begin(), text.end());
        e.value.push_back(0);
        entries_.push_back(std::move(e));
    }
    void bytes(std::uint16_t tag, std::uint16_t type, const void* data, std::uint32_t count) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        entries_.push_back(Entry{tag, type, count, std::vector<std::uint8_t>(p, p + count)});
    }
    void short_value(std::uint16_t tag, std::uint16_t v) {
        Entry e{tag, exif_type::kShort, 1, {}};
        put16(e.value, v);
        entries_.push_back(std::move(e));
    }
    void long_value(std::uint16_t tag, std::uint32_t v) {
        Entry e{tag, exif_type::kLong, 1, {}};
        put32(e.value, v);
        entries_.push_back(std::move(e));
    }
    void rationals(std::uint16_t tag, std::initializer_list<ExportRational> values) {
        rationals(tag, std::span<const ExportRational>(values.begin(), values.size()));
    }
    void rationals(std::uint16_t tag, std::span<const ExportRational> values) {
        Entry e{tag, exif_type::kRational, static_cast<std::uint32_t>(values.size()), {}};
        for (const ExportRational& r : values) {
            put32(e.value, r.numerator);
            put32(e.value, r.denominator);
        }
        entries_.push_back(std::move(e));
    }
    void srational(std::uint16_t tag, ExportSignedRational r) {
        Entry e{tag, exif_type::kSRational, 1, {}};
        put32(e.value, static_cast<std::uint32_t>(r.numerator));
        put32(e.value, static_cast<std::uint32_t>(r.denominator));
        entries_.push_back(std::move(e));
    }

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    // Bytes of the IFD table plus its out-of-line values (each word-aligned); 0 when empty.
    [[nodiscard]] std::size_t size() const noexcept {
        if (entries_.empty()) {
            return 0;  // an empty IFD is not written
        }
        std::size_t total = 2 + entries_.size() * 12 + 4;
        for (const Entry& e : entries_) {
            if (e.value.size() > 4) {
                total += e.value.size() + (e.value.size() & 1);
            }
        }
        return total;
    }

    // Appends the IFD at the end of `out`; value offsets are relative to `base` (the TIFF header).
    // Precondition: out.size() - base is even.
    void write(std::vector<std::uint8_t>& out, std::size_t base) {
        std::stable_sort(entries_.begin(), entries_.end(),
                         [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
        std::size_t data = out.size() - base + 2 + entries_.size() * 12 + 4;
        put16(out, static_cast<std::uint32_t>(entries_.size()));
        std::vector<const Entry*> deferred;
        for (const Entry& e : entries_) {
            put16(out, e.tag);
            put16(out, e.type);
            put32(out, e.count);
            if (e.value.size() <= 4) {
                std::vector<std::uint8_t> field = e.value;
                field.resize(4, 0);
                out.insert(out.end(), field.begin(), field.end());
            } else {
                put32(out, static_cast<std::uint32_t>(data));
                data += e.value.size() + (e.value.size() & 1);
                deferred.push_back(&e);
            }
        }
        put32(out, 0);  // next IFD: none (no IFD 1, no thumbnail)
        for (const Entry* e : deferred) {
            out.insert(out.end(), e->value.begin(), e->value.end());
            if (e->value.size() & 1) {
                out.push_back(0);
            }
        }
    }

    // Sets the inline value of entry `tag` (a LONG) after write(). `ifd` is the IFD's position
    // in `out`. Returns false when the IFD has no such entry.
    static bool patch_long(std::vector<std::uint8_t>& out, std::size_t ifd, std::uint16_t tag, std::uint32_t value) {
        const std::size_t count = static_cast<std::size_t>(out[ifd]) | (static_cast<std::size_t>(out[ifd + 1]) << 8);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t entry = ifd + 2 + i * 12;
            const auto entry_tag = static_cast<std::uint16_t>(out[entry] | (out[entry + 1] << 8));
            if (entry_tag == tag) {
                set32(out, entry + 8, value);
                return true;
            }
        }
        return false;
    }

private:
    std::vector<Entry> entries_;
};

std::string two_digits(int value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02d", value);
    return buffer;
}

}  // namespace

Expected<std::vector<std::uint8_t>> build_exif_app1(const ExportMetadata& m, const ExifImageInfo& image) {
    if (m.empty()) {
        return std::vector<std::uint8_t>{};
    }
    if (image.width < 1 || image.height < 1) {
        return fail(ErrorCode::Internal, "EXIF image dimensions must be positive", kWhere);
    }

    Ifd ifd0;
    ifd0.ascii(exif_tag::kMake, m.camera_make);
    ifd0.ascii(exif_tag::kModel, m.camera_model);
    ifd0.short_value(exif_tag::kOrientation, 1);
    ifd0.rationals(exif_tag::kXResolution, {ExportRational{kExifResolutionDpi, 1}});
    ifd0.rationals(exif_tag::kYResolution, {ExportRational{kExifResolutionDpi, 1}});
    ifd0.short_value(exif_tag::kResolutionUnit, kResolutionUnitInch);
    ifd0.ascii(exif_tag::kSoftware, m.software);
    ifd0.ascii(exif_tag::kArtist, m.artist);
    ifd0.short_value(exif_tag::kYCbCrPositioning, kYCbCrCentered);
    ifd0.ascii(exif_tag::kCopyright, m.copyright);

    Ifd exif;
    if (m.has_capture()) {
        exif.bytes(exif_tag::kExifVersion, exif_type::kUndefined, kExifVersion, 4);
        exif.bytes(exif_tag::kComponentsConfiguration, exif_type::kUndefined, kComponentsConfiguration, 4);
        exif.bytes(exif_tag::kFlashpixVersion, exif_type::kUndefined, kFlashpixVersion, 4);
        exif.short_value(exif_tag::kColorSpace, exif_color_space(image.profile));
        exif.long_value(exif_tag::kPixelXDimension, static_cast<std::uint32_t>(image.width));
        exif.long_value(exif_tag::kPixelYDimension, static_cast<std::uint32_t>(image.height));
        if (m.exposure_time_s) {
            exif.rationals(exif_tag::kExposureTime, {*m.exposure_time_s});
        }
        if (m.f_number) {
            exif.rationals(exif_tag::kFNumber, {*m.f_number});
        }
        if (m.iso) {
            exif.short_value(exif_tag::kPhotographicSensitivity,
                             static_cast<std::uint16_t>(std::min<std::uint32_t>(*m.iso, 65535)));
            if (*m.iso >= 65535) {
                exif.short_value(exif_tag::kSensitivityType, kSensitivityTypeIsoSpeed);
                exif.long_value(exif_tag::kIsoSpeed, *m.iso);
            }
        }
        if (m.capture_time) {
            const CaptureDateTime& t = *m.capture_time;
            char text[32];
            std::snprintf(text, sizeof(text), "%04d:%02d:%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour,
                          t.minute, t.second);
            exif.ascii(exif_tag::kDateTimeOriginal, text);
            exif.ascii(exif_tag::kSubSecTimeOriginal, t.subsec);
            if (t.utc_offset_minutes) {
                const int minutes = *t.utc_offset_minutes;
                const int magnitude = minutes < 0 ? -minutes : minutes;
                exif.ascii(exif_tag::kOffsetTimeOriginal, std::string(minutes < 0 ? "-" : "+") +
                                                              two_digits(magnitude / 60) + ":" +
                                                              two_digits(magnitude % 60));
            }
        }
        if (m.exposure_bias_ev) {
            exif.srational(exif_tag::kExposureBias, *m.exposure_bias_ev);
        }
        if (m.focal_length_mm) {
            exif.rationals(exif_tag::kFocalLength, {*m.focal_length_mm});
        }
        if (m.focal_length_35mm_mm) {
            exif.short_value(exif_tag::kFocalLength35mm, *m.focal_length_35mm_mm);
        }
        exif.ascii(exif_tag::kLensMake, m.lens_make);
        exif.ascii(exif_tag::kLensModel, m.lens_model);
    }

    Ifd gps;
    if (m.gps) {
        const GpsPosition& p = *m.gps;
        gps.bytes(exif_tag::kGpsVersionId, exif_type::kByte, kGpsVersion, 4);
        gps.ascii(exif_tag::kGpsLatitudeRef, p.latitude_deg < 0 ? "S" : "N");
        gps.rationals(exif_tag::kGpsLatitude, coordinate_rationals(p.latitude_deg));
        gps.ascii(exif_tag::kGpsLongitudeRef, p.longitude_deg < 0 ? "W" : "E");
        gps.rationals(exif_tag::kGpsLongitude, coordinate_rationals(p.longitude_deg));
        if (p.altitude_m) {
            const std::uint8_t below = *p.altitude_m < 0 ? 1 : 0;
            gps.bytes(exif_tag::kGpsAltitudeRef, exif_type::kByte, &below, 1);
            gps.rationals(exif_tag::kGpsAltitude, {altitude_rational(*p.altitude_m)});
        }
        if (p.utc) {
            const GpsTimestamp& t = *p.utc;
            gps.rationals(exif_tag::kGpsTimeStamp, time_rationals(t));
            char date[16];
            std::snprintf(date, sizeof(date), "%04d:%02d:%02d", t.year, t.month, t.day);
            gps.ascii(exif_tag::kGpsDateStamp, date);
        }
    }

    // Pointer entries are inline LONGs, so the IFD sizes do not depend on their values.
    if (!exif.empty()) {
        ifd0.long_value(exif_tag::kExifIfd, 0);
    }
    if (!gps.empty()) {
        ifd0.long_value(exif_tag::kGpsIfd, 0);
    }
    const std::size_t total = 6 + 8 + ifd0.size() + exif.size() + gps.size();
    if (total > kMaxApp1Payload) {
        return fail(ErrorCode::LimitExceeded,
                    "EXIF metadata needs " + std::to_string(total) + " bytes; a JPEG APP1 segment holds " +
                        std::to_string(kMaxApp1Payload),
                    kWhere);
    }

    std::vector<std::uint8_t> out = {'E', 'x', 'i', 'f', 0, 0};
    constexpr std::size_t kTiffStart = 6;
    out.insert(out.end(), {'I', 'I', 42, 0});
    put32(out, 8);  // IFD 0 follows the header
    const std::size_t exif_offset = 8 + ifd0.size();
    const std::size_t gps_offset = exif_offset + exif.size();
    ifd0.write(out, kTiffStart);
    if (!exif.empty() &&
        !Ifd::patch_long(out, kTiffStart + 8, exif_tag::kExifIfd, static_cast<std::uint32_t>(exif_offset))) {
        return fail(ErrorCode::Internal, "EXIF IFD pointer missing", kWhere);
    }
    if (!gps.empty() &&
        !Ifd::patch_long(out, kTiffStart + 8, exif_tag::kGpsIfd, static_cast<std::uint32_t>(gps_offset))) {
        return fail(ErrorCode::Internal, "GPS IFD pointer missing", kWhere);
    }
    if (!exif.empty()) {
        exif.write(out, kTiffStart);
    }
    if (!gps.empty()) {
        gps.write(out, kTiffStart);
    }
    if (out.size() != total || out.size() - kTiffStart != gps_offset + gps.size()) {
        return fail(ErrorCode::Internal, "EXIF layout size mismatch", kWhere);
    }
    return out;
}

}  // namespace lucent::exporter::detail
