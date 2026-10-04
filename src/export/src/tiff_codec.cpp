// TIFF writer and verification reader on libtiff.
//
// Errors and warnings are captured per handle with TIFFOpenOptions ExtR handlers (libtiff
// >= 4.5), which return 1 so the process-global handlers (stderr) are never invoked.

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <string>
#include <vector>

#include <tiffio.h>
#include <tiffvers.h>

#include "codecs.hpp"
#include "exif_format.hpp"
#include "file_util.hpp"

namespace lucent::exporter::detail {
namespace {

// Target uncompressed strip size. Larger strips compress better with Deflate; 1 MiB keeps the
// codec's working buffers small.
constexpr std::size_t kStripBytes = std::size_t{1} << 20;

// zlib level for Deflate: libtiff's default, fixed explicitly so output bytes never depend on
// a library default changing.
constexpr int kDeflateLevel = 6;

// Nominal resolution written into the baseline resolution tags.
constexpr double kResolutionDpi = 300.0;

struct Diagnostics {
    int errors = 0;
    int warnings = 0;
    char first_error[512] = {};
    char first_warning[512] = {};
};

void record(char* buffer, std::size_t size, const char* module, const char* fmt, va_list ap) {
    char text[448];
    std::vsnprintf(text, sizeof(text), fmt, ap);
    std::snprintf(buffer, size, "%s: %s", module != nullptr ? module : "libtiff", text);
}

int on_error(TIFF*, void* user_data, const char* module, const char* fmt, va_list ap) {
    auto* diagnostics = static_cast<Diagnostics*>(user_data);
    if (diagnostics->errors++ == 0) {
        record(diagnostics->first_error, sizeof(diagnostics->first_error), module, fmt, ap);
    }
    return 1;  // handled: do not call the global handler
}

int on_warning(TIFF*, void* user_data, const char* module, const char* fmt, va_list ap) {
    auto* diagnostics = static_cast<Diagnostics*>(user_data);
    if (diagnostics->warnings++ == 0) {
        record(diagnostics->first_warning, sizeof(diagnostics->first_warning), module, fmt, ap);
    }
    return 1;
}

struct TiffCloser {
    void operator()(TIFF* tiff) const noexcept { TIFFClose(tiff); }
};
using UniqueTiff = std::unique_ptr<TIFF, TiffCloser>;

// Opens with per-handle diagnostics. `diagnostics` must outlive the handle.
Expected<UniqueTiff> open_tiff(const std::filesystem::path& path, const char* mode, Diagnostics& diagnostics,
                               const char* where, ErrorCode open_failure) {
    auto native = api_path(path, where);
    if (!native) {
        return Unexpected(std::move(native.error()));
    }
    TIFFOpenOptions* options = TIFFOpenOptionsAlloc();
    if (options == nullptr) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate TIFF open options", where);
    }
    TIFFOpenOptionsSetErrorHandlerExtR(options, on_error, &diagnostics);
    TIFFOpenOptionsSetWarningHandlerExtR(options, on_warning, &diagnostics);
    UniqueTiff tiff(TIFFOpenWExt(native->c_str(), mode, options));
    TIFFOpenOptionsFree(options);
    if (!tiff) {
        return fail(open_failure, std::string("cannot open TIFF file: ") + diagnostics.first_error, where);
    }
    return tiff;
}

// Sets an ASCII field when `text` is not empty.
bool set_text(TIFF* t, std::uint32_t tag, const std::string& text) {
    return text.empty() || TIFFSetField(t, tag, text.c_str()) != 0;
}

// The Exif IFD fields of `m` (the current directory must be a fresh EXIF custom directory).
// Rationals are passed as double; libtiff stores EXIF rationals as float.
bool set_exif_fields(TIFF* t, const ExportMetadata& m, const ExifImageInfo& image) {
    bool ok = TIFFSetField(t, EXIFTAG_EXIFVERSION, static_cast<const void*>(kExifVersion)) &&
              TIFFSetField(t, EXIFTAG_COLORSPACE, static_cast<int>(exif_color_space(image.profile))) &&
              TIFFSetField(t, EXIFTAG_PIXELXDIMENSION, static_cast<std::uint32_t>(image.width)) &&
              TIFFSetField(t, EXIFTAG_PIXELYDIMENSION, static_cast<std::uint32_t>(image.height));
    if (ok && m.exposure_time_s) {
        ok = TIFFSetField(t, EXIFTAG_EXPOSURETIME, m.exposure_time_s->value()) != 0;
    }
    if (ok && m.f_number) {
        ok = TIFFSetField(t, EXIFTAG_FNUMBER, m.f_number->value()) != 0;
    }
    if (ok && m.iso) {
        std::uint16_t iso = static_cast<std::uint16_t>(std::min<std::uint32_t>(*m.iso, 65535));
        ok = TIFFSetField(t, EXIFTAG_ISOSPEEDRATINGS, 1, &iso) != 0;
        if (ok && *m.iso >= 65535) {
            ok = TIFFSetField(t, EXIFTAG_SENSITIVITYTYPE, static_cast<int>(kSensitivityTypeIsoSpeed)) &&
                 TIFFSetField(t, EXIFTAG_ISOSPEED, static_cast<std::uint32_t>(*m.iso));
        }
    }
    if (ok && m.capture_time) {
        const CaptureDateTime& c = *m.capture_time;
        char text[32];
        std::snprintf(text, sizeof(text), "%04d:%02d:%02d %02d:%02d:%02d", c.year, c.month, c.day, c.hour, c.minute,
                      c.second);
        ok = TIFFSetField(t, EXIFTAG_DATETIMEORIGINAL, text) && set_text(t, EXIFTAG_SUBSECTIMEORIGINAL, c.subsec);
        if (ok && c.utc_offset_minutes) {
            const int minutes = *c.utc_offset_minutes;
            const int magnitude = minutes < 0 ? -minutes : minutes;
            std::snprintf(text, sizeof(text), "%c%02d:%02d", minutes < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
            ok = TIFFSetField(t, EXIFTAG_OFFSETTIMEORIGINAL, text) != 0;
        }
    }
    if (ok && m.exposure_bias_ev) {
        ok = TIFFSetField(t, EXIFTAG_EXPOSUREBIASVALUE, m.exposure_bias_ev->value()) != 0;
    }
    if (ok && m.focal_length_mm) {
        ok = TIFFSetField(t, EXIFTAG_FOCALLENGTH, m.focal_length_mm->value()) != 0;
    }
    if (ok && m.focal_length_35mm_mm) {
        ok = TIFFSetField(t, EXIFTAG_FOCALLENGTHIN35MMFILM, static_cast<int>(*m.focal_length_35mm_mm)) != 0;
    }
    return ok && set_text(t, EXIFTAG_LENSMAKE, m.lens_make) && set_text(t, EXIFTAG_LENSMODEL, m.lens_model);
}

// The GPS IFD fields of `p` (the current directory must be a fresh GPS custom directory).
bool set_gps_fields(TIFF* t, const GpsPosition& p) {
    const DmsRationals lat = to_dms(p.latitude_deg);
    const DmsRationals lon = to_dms(p.longitude_deg);
    double latitude[3] = {static_cast<double>(lat.degrees), static_cast<double>(lat.minutes),
                          static_cast<double>(lat.micro_seconds) / kGpsSecondsDenominator};
    double longitude[3] = {static_cast<double>(lon.degrees), static_cast<double>(lon.minutes),
                           static_cast<double>(lon.micro_seconds) / kGpsSecondsDenominator};
    bool ok = TIFFSetField(t, GPSTAG_VERSIONID, static_cast<const void*>(kGpsVersion)) &&
              TIFFSetField(t, GPSTAG_LATITUDEREF, p.latitude_deg < 0 ? "S" : "N") &&
              TIFFSetField(t, GPSTAG_LATITUDE, latitude) &&
              TIFFSetField(t, GPSTAG_LONGITUDEREF, p.longitude_deg < 0 ? "W" : "E") &&
              TIFFSetField(t, GPSTAG_LONGITUDE, longitude);
    if (ok && p.altitude_m) {
        ok = TIFFSetField(t, GPSTAG_ALTITUDEREF, *p.altitude_m < 0 ? 1 : 0) &&
             TIFFSetField(t, GPSTAG_ALTITUDE,
                          static_cast<double>(altitude_millimetres(*p.altitude_m)) / kAltitudeDenominator);
    }
    if (ok && p.utc) {
        const GpsTimestamp& u = *p.utc;
        double time[3] = {static_cast<double>(u.hour), static_cast<double>(u.minute),
                          static_cast<double>(second_milliseconds(u.second)) / kTimeSecondsDenominator};
        char date[16];
        std::snprintf(date, sizeof(date), "%04d:%02d:%02d", u.year, u.month, u.day);
        ok = TIFFSetField(t, GPSTAG_TIMESTAMP, time) && TIFFSetField(t, GPSTAG_DATESTAMP, date);
    }
    return ok;
}

// Values to store in place in an IFD libtiff has written: the inline value of a LONG/IFD
// pointer entry, or the out-of-line words of a RATIONAL/SRATIONAL entry (numerator and
// denominator pairs; signed values as two's complement).
struct ValuePatch {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;  // the type the entry must have (kLong also accepts kIfd)
    std::vector<std::uint32_t> words;
};

void add_rationals(std::vector<ValuePatch>& patches, std::uint16_t tag, std::span<const ExportRational> values) {
    ValuePatch patch{tag, exif_type::kRational, {}};
    for (const ExportRational& r : values) {
        patch.words.push_back(r.numerator);
        patch.words.push_back(r.denominator);
    }
    patches.push_back(std::move(patch));
}

// Patches for the Exif IFD: libtiff stores EXIF rationals as float and writes its own fraction
// approximation (1/8000 became 376336/3010687857), so the exact fractions are written over them.
std::vector<ValuePatch> exif_patches(const ExportMetadata& m) {
    std::vector<ValuePatch> patches;
    for (const auto& [tag, value] : {std::pair{exif_tag::kExposureTime, m.exposure_time_s},
                                     std::pair{exif_tag::kFNumber, m.f_number},
                                     std::pair{exif_tag::kFocalLength, m.focal_length_mm}}) {
        if (value) {
            add_rationals(patches, tag, std::span<const ExportRational>(&*value, 1));
        }
    }
    if (m.exposure_bias_ev) {
        patches.push_back(ValuePatch{exif_tag::kExposureBias, exif_type::kSRational,
                                     {static_cast<std::uint32_t>(m.exposure_bias_ev->numerator),
                                      static_cast<std::uint32_t>(m.exposure_bias_ev->denominator)}});
    }
    return patches;
}

// Patches for the GPS IFD (libtiff converts the doubles it is given into fractions itself).
std::vector<ValuePatch> gps_patches(const GpsPosition& p) {
    std::vector<ValuePatch> patches;
    add_rationals(patches, exif_tag::kGpsLatitude, coordinate_rationals(p.latitude_deg));
    add_rationals(patches, exif_tag::kGpsLongitude, coordinate_rationals(p.longitude_deg));
    if (p.altitude_m) {
        const ExportRational altitude = altitude_rational(*p.altitude_m);
        add_rationals(patches, exif_tag::kGpsAltitude, std::span<const ExportRational>(&altitude, 1));
    }
    if (p.utc) {
        add_rationals(patches, exif_tag::kGpsTimeStamp, time_rationals(*p.utc));
    }
    return patches;
}

// In-place editor for values of a classic TIFF file written by libtiff. Never changes a size or
// an offset, so the layout libtiff produced stays intact.
class TiffPatcher {
public:
    static constexpr const char* kWhere = "export.tiff.write";

    ~TiffPatcher() {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    Status open(const std::filesystem::path& path) {
        auto native = api_path(path, kWhere);
        if (!native) {
            return Unexpected(std::move(native.error()));
        }
        _set_doserrno(0);
        _set_errno(0);
        if (_wfopen_s(&file_, native->c_str(), L"r+b") != 0 || file_ == nullptr) {
            file_ = nullptr;
            return Unexpected(crt_file_error(kWhere, "cannot reopen the TIFF file to complete its EXIF directories"));
        }
        std::uint8_t header[8] = {};
        if (auto s = read(0, header); !s) {
            return s;
        }
        if (header[0] != header[1] || (header[0] != 'I' && header[0] != 'M')) {
            return internal("libtiff wrote an unexpected TIFF header");
        }
        little_ = header[0] == 'I';
        if (load(header + 2, 2) != 42) {
            return internal("libtiff wrote a TIFF variant other than classic TIFF");
        }
        ifd0_ = load(header + 4, 4);
        return ok();
    }

    [[nodiscard]] std::uint32_t ifd0() const noexcept { return ifd0_; }

    // Applies every patch to the IFD at `ifd`; each must match exactly one entry with the
    // expected type and a count equal to its number of values.
    Status apply(std::uint32_t ifd, std::span<const ValuePatch> patches) {
        std::uint8_t count_bytes[2] = {};
        if (auto s = read(ifd, count_bytes); !s) {
            return s;
        }
        const std::uint32_t count = load(count_bytes, 2);
        std::vector<std::uint8_t> table(std::size_t{count} * 12);
        if (auto s = read(std::uint64_t{ifd} + 2, table); !s) {
            return s;
        }
        for (const ValuePatch& patch : patches) {
            const bool pointer = patch.type == exif_type::kLong;
            const std::size_t values = pointer ? patch.words.size() : patch.words.size() / 2;
            bool found = false;
            for (std::uint32_t i = 0; i < count && !found; ++i) {
                const std::uint8_t* entry = table.data() + std::size_t{i} * 12;
                if (load(entry, 2) != patch.tag) {
                    continue;
                }
                const std::uint32_t type = load(entry + 2, 2);
                const bool type_ok = type == patch.type || (pointer && type == exif_type::kIfd);
                if (!type_ok || load(entry + 4, 4) != values) {
                    return internal("libtiff wrote tag " + std::to_string(patch.tag) + " with an unexpected type or count");
                }
                // Pointers are inline (4 bytes); rationals (8 bytes each) are out of line.
                const std::uint64_t at = pointer ? std::uint64_t{ifd} + 2 + std::uint64_t{i} * 12 + 8 : load(entry + 8, 4);
                std::vector<std::uint8_t> bytes(patch.words.size() * 4);
                for (std::size_t w = 0; w < patch.words.size(); ++w) {
                    store(bytes.data() + w * 4, patch.words[w]);
                }
                if (auto s = write(at, bytes); !s) {
                    return s;
                }
                found = true;
            }
            if (!found) {
                return internal("libtiff did not write tag " + std::to_string(patch.tag));
            }
        }
        return ok();
    }

    Status close() {
        const bool flushed = std::fflush(file_) == 0;
        const bool closed = std::fclose(file_) == 0;
        file_ = nullptr;
        if (!flushed || !closed) {
            return Unexpected(crt_file_error(kWhere, "cannot write the TIFF file"));
        }
        return ok();
    }

private:
    static Status internal(std::string message) { return fail(ErrorCode::Internal, std::move(message), kWhere); }

    std::uint32_t load(const std::uint8_t* p, int n) const noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            v |= std::uint32_t{p[little_ ? i : n - 1 - i]} << (8 * i);
        }
        return v;
    }
    void store(std::uint8_t* p, std::uint32_t v) const noexcept {
        for (int i = 0; i < 4; ++i) {
            p[little_ ? i : 3 - i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        }
    }
    Status read(std::uint64_t at, std::span<std::uint8_t> out) {
        if (_fseeki64(file_, static_cast<long long>(at), SEEK_SET) != 0 ||
            std::fread(out.data(), 1, out.size(), file_) != out.size()) {
            return Unexpected(crt_file_error(kWhere, "cannot read back the TIFF structure"));
        }
        return ok();
    }
    Status write(std::uint64_t at, std::span<const std::uint8_t> bytes) {
        if (_fseeki64(file_, static_cast<long long>(at), SEEK_SET) != 0 ||
            std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) {
            return Unexpected(crt_file_error(kWhere, "cannot complete the EXIF directories"));
        }
        return ok();
    }

    std::FILE* file_ = nullptr;
    bool little_ = true;
    std::uint32_t ifd0_ = 0;
};

// Completes the metadata libtiff wrote: stores the Exif / GPS IFD offsets in IFD 0's placeholder
// pointer entries and writes the exact fractions over libtiff's approximations.
Status complete_tiff_metadata(const std::filesystem::path& path, const ExportMetadata& m,
                              std::optional<std::uint64_t> exif_offset, std::optional<std::uint64_t> gps_offset) {
    TiffPatcher patcher;
    if (auto s = patcher.open(path); !s) {
        return s;
    }
    std::vector<ValuePatch> pointers;
    for (const auto& [tag, offset] : {std::pair{exif_tag::kExifIfd, exif_offset}, std::pair{exif_tag::kGpsIfd, gps_offset}}) {
        if (offset) {
            if (*offset == 0 || *offset > 0xFFFFFFFFu) {
                return fail(ErrorCode::Internal, "invalid EXIF/GPS directory offset", TiffPatcher::kWhere);
            }
            pointers.push_back(ValuePatch{tag, exif_type::kLong, {static_cast<std::uint32_t>(*offset)}});
        }
    }
    if (auto s = patcher.apply(patcher.ifd0(), pointers); !s) {
        return s;
    }
    if (exif_offset) {
        if (auto s = patcher.apply(static_cast<std::uint32_t>(*exif_offset), exif_patches(m)); !s) {
            return s;
        }
    }
    if (gps_offset) {
        if (auto s = patcher.apply(static_cast<std::uint32_t>(*gps_offset), gps_patches(*m.gps)); !s) {
            return s;
        }
    }
    return patcher.close();
}

}  // namespace

Status write_tiff(const std::filesystem::path& path, const OutputImage& image, const TiffSettings& settings,
                  std::span<const std::uint8_t> icc, const core::CancellationToken& cancel,
                  const ExportMetadata* metadata, const ExifImageInfo& image_info) {
    constexpr const char* kWhere = "export.tiff.write";
    if (image.bit_depth != BitDepth::Sixteen) {
        return fail(ErrorCode::InvalidArgument, "TIFF output requires 16-bit samples", kWhere);
    }
    Diagnostics diagnostics;
    _set_doserrno(0);  // so crt_file_error() reports this operation's failure, not a stale one
    _set_errno(0);
    auto opened = open_tiff(path, "w", diagnostics, kWhere, ErrorCode::IoError);
    if (!opened) {
        return Unexpected(std::move(opened.error()));
    }
    UniqueTiff tiff = std::move(*opened);
    TIFF* t = tiff.get();

    const std::size_t row_samples = image.row_samples();
    const std::size_t row_bytes = row_samples * sizeof(std::uint16_t);
    const auto rows_per_strip =
        static_cast<std::uint32_t>(std::clamp<std::size_t>(kStripBytes / row_bytes, 1, static_cast<std::size_t>(image.height)));
    const bool deflate = settings.compression == TiffCompression::Deflate;

    bool fields_ok = TIFFSetField(t, TIFFTAG_IMAGEWIDTH, static_cast<std::uint32_t>(image.width)) &&
                     TIFFSetField(t, TIFFTAG_IMAGELENGTH, static_cast<std::uint32_t>(image.height)) &&
                     TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 16) && TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 3) &&
                     TIFFSetField(t, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT) &&
                     TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB) &&
                     TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) &&
                     TIFFSetField(t, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) &&
                     TIFFSetField(t, TIFFTAG_COMPRESSION, deflate ? COMPRESSION_ADOBE_DEFLATE : COMPRESSION_NONE) &&
                     TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, rows_per_strip) &&
                     TIFFSetField(t, TIFFTAG_XRESOLUTION, kResolutionDpi) &&
                     TIFFSetField(t, TIFFTAG_YRESOLUTION, kResolutionDpi) &&
                     TIFFSetField(t, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH) &&
                     TIFFSetField(t, TIFFTAG_ICCPROFILE, static_cast<std::uint32_t>(icc.size()), icc.data());
    if (fields_ok && deflate) {
        fields_ok = TIFFSetField(t, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL) &&
                    TIFFSetField(t, TIFFTAG_ZIPQUALITY, kDeflateLevel);
    }
    const bool with_exif = metadata != nullptr && metadata->has_capture();
    const bool with_gps = metadata != nullptr && metadata->gps.has_value();
    if (fields_ok && metadata != nullptr) {
        fields_ok = set_text(t, TIFFTAG_MAKE, metadata->camera_make) &&
                    set_text(t, TIFFTAG_MODEL, metadata->camera_model) &&
                    set_text(t, TIFFTAG_SOFTWARE, metadata->software) && set_text(t, TIFFTAG_ARTIST, metadata->artist) &&
                    set_text(t, TIFFTAG_COPYRIGHT, metadata->copyright);
        // Placeholders reserve the pointer entries; the real offsets are set once the custom
        // directories exist, and libtiff then rewrites IFD 0 in place (same size).
        if (fields_ok && with_exif) {
            fields_ok = TIFFSetField(t, TIFFTAG_EXIFIFD, std::uint64_t{0}) != 0;
        }
        if (fields_ok && with_gps) {
            fields_ok = TIFFSetField(t, TIFFTAG_GPSIFD, std::uint64_t{0}) != 0;
        }
    }
    if (!fields_ok) {
        return fail(ErrorCode::Internal, std::string("cannot set TIFF fields: ") + diagnostics.first_error, kWhere);
    }

    // libtiff's predictor may modify the buffer passed to TIFFWriteScanline, so each row is
    // copied into scratch memory first.
    std::vector<std::uint16_t> scratch;
    try {
        scratch.resize(row_samples);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate a TIFF row", kWhere);
    }
    for (std::int32_t y = 0; y < image.height; ++y) {
        if (y % kCodecCancelRows == 0) {
            if (auto status = cancel.check(kWhere); !status) {
                return status;  // the caller removes the partial file
            }
        }
        std::memcpy(scratch.data(), image.samples16.data() + row_samples * static_cast<std::size_t>(y), row_bytes);
        if (TIFFWriteScanline(t, scratch.data(), static_cast<std::uint32_t>(y), 0) < 0) {
            return Unexpected(crt_file_error(kWhere, std::string("cannot write TIFF data: ") + diagnostics.first_error));
        }
    }
    if (!with_exif && !with_gps) {
        if (!TIFFFlush(t)) {
            return Unexpected(
                crt_file_error(kWhere, std::string("cannot flush the TIFF file: ") + diagnostics.first_error));
        }
    } else {
        // Finish IFD 0 (with placeholder pointers), then write the Exif / GPS IFDs as libtiff
        // custom directories. Rewriting IFD 0 through libtiff to store their offsets would
        // append a second copy of IFD 0's values (libtiff relocates them), leaving stale bytes in
        // the file, so complete_tiff_metadata patches the pointers in place after closing (and
        // replaces libtiff's float-derived fractions with the exact ones).
        if (!TIFFWriteDirectory(t)) {
            return Unexpected(
                crt_file_error(kWhere, std::string("cannot write the TIFF directory: ") + diagnostics.first_error));
        }
        std::uint64_t exif_offset = 0;
        std::uint64_t gps_offset = 0;
        if (with_exif) {
            if (TIFFCreateEXIFDirectory(t) != 0 || !set_exif_fields(t, *metadata, image_info)) {
                return fail(ErrorCode::Internal, std::string("cannot set EXIF fields: ") + diagnostics.first_error,
                            kWhere);
            }
            if (!TIFFWriteCustomDirectory(t, &exif_offset)) {
                return Unexpected(
                    crt_file_error(kWhere, std::string("cannot write the EXIF directory: ") + diagnostics.first_error));
            }
        }
        if (with_gps) {
            if (TIFFCreateGPSDirectory(t) != 0 || !set_gps_fields(t, *metadata->gps)) {
                return fail(ErrorCode::Internal, std::string("cannot set GPS fields: ") + diagnostics.first_error,
                            kWhere);
            }
            if (!TIFFWriteCustomDirectory(t, &gps_offset)) {
                return Unexpected(
                    crt_file_error(kWhere, std::string("cannot write the GPS directory: ") + diagnostics.first_error));
            }
        }
        // A written custom directory stays "dirty" in libtiff and TIFFClose would write it again
        // as an image directory; reloading the (already written) IFD 0 leaves nothing to write.
        if (!TIFFSetDirectory(t, 0)) {
            return fail(ErrorCode::Internal, std::string("cannot reload the TIFF directory: ") + diagnostics.first_error,
                        kWhere);
        }
        tiff.reset();  // TIFFClose; failures surface through the error handler
        if (diagnostics.errors != 0) {
            return fail(ErrorCode::IoError, std::string("libtiff reported an error: ") + diagnostics.first_error,
                        kWhere);
        }
        return complete_tiff_metadata(path, *metadata,
                                      with_exif ? std::optional<std::uint64_t>(exif_offset) : std::nullopt,
                                      with_gps ? std::optional<std::uint64_t>(gps_offset) : std::nullopt);
    }
    tiff.reset();  // TIFFClose; failures surface through the error handler
    if (diagnostics.errors != 0) {
        return fail(ErrorCode::IoError, std::string("libtiff reported an error: ") + diagnostics.first_error, kWhere);
    }
    return ok();
}

Expected<DecodedFile> read_tiff(const std::filesystem::path& path, const core::CancellationToken& cancel,
                                const OutputImage* expected) {
    constexpr const char* kWhere = "export.tiff.verify";
    Diagnostics diagnostics;
    auto opened = open_tiff(path, "r", diagnostics, kWhere, ErrorCode::Corrupt);
    if (!opened) {
        return Unexpected(std::move(opened.error()));
    }
    TIFF* t = opened->get();

    DecodedFile decoded;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t bits = 0;
    std::uint16_t samples = 0;
    std::uint16_t photometric = 0;
    std::uint16_t planar = 0;
    std::uint16_t orientation = 0;
    std::uint16_t sample_format = 0;
    std::uint16_t compression = 0;
    if (!TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &width) || !TIFFGetField(t, TIFFTAG_IMAGELENGTH, &height) ||
        !TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &bits) ||
        !TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &samples) ||
        !TIFFGetField(t, TIFFTAG_PHOTOMETRIC, &photometric) ||
        !TIFFGetFieldDefaulted(t, TIFFTAG_PLANARCONFIG, &planar) ||
        !TIFFGetFieldDefaulted(t, TIFFTAG_ORIENTATION, &orientation) ||
        !TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLEFORMAT, &sample_format) ||
        !TIFFGetFieldDefaulted(t, TIFFTAG_COMPRESSION, &compression)) {
        return fail(ErrorCode::Corrupt, std::string("written TIFF lacks required fields: ") + diagnostics.first_error,
                    kWhere);
    }
    if (width > static_cast<std::uint32_t>(kMaxOutputDimension) ||
        height > static_cast<std::uint32_t>(kMaxOutputDimension)) {
        return fail(ErrorCode::Corrupt, "written TIFF has implausible dimensions", kWhere);
    }
    decoded.width = static_cast<std::int32_t>(width);
    decoded.height = static_cast<std::int32_t>(height);
    decoded.channels = samples;
    decoded.bits_per_sample = bits;
    decoded.tiff_photometric = photometric;
    decoded.tiff_planar = planar;
    decoded.tiff_orientation = orientation;
    decoded.tiff_sample_format = sample_format;
    decoded.tiff_compression = compression;
    decoded.tiff_directories = static_cast<std::int32_t>(TIFFNumberOfDirectories(t));

    std::uint32_t icc_size = 0;
    void* icc_data = nullptr;
    try {
        if (TIFFGetField(t, TIFFTAG_ICCPROFILE, &icc_size, &icc_data) && icc_data != nullptr) {
            const auto* bytes = static_cast<const std::uint8_t*>(icc_data);
            decoded.icc.assign(bytes, bytes + icc_size);
        }
        if (bits == 16 && samples == 3 && planar == PLANARCONFIG_CONTIG && sample_format == SAMPLEFORMAT_UINT) {
            const std::size_t row_samples = std::size_t{width} * 3;
            if (TIFFScanlineSize64(t) != static_cast<std::uint64_t>(row_samples * sizeof(std::uint16_t))) {
                return fail(ErrorCode::Corrupt, "written TIFF has an unexpected scanline size", kWhere);
            }
            // Verification compares row by row through one row buffer (no second full copy).
            const bool compare = expected != nullptr && expected->bit_depth == BitDepth::Sixteen &&
                                 expected->width == static_cast<std::int32_t>(width) &&
                                 expected->height == static_cast<std::int32_t>(height) &&
                                 expected->samples16.size() == row_samples * height;
            std::vector<std::uint16_t> row;
            if (expected == nullptr) {
                decoded.samples16.resize(row_samples * height);
            } else {
                row.resize(row_samples);
            }
            if (expected == nullptr || compare) {
                for (std::uint32_t y = 0; y < height; ++y) {
                    if (y % static_cast<std::uint32_t>(kCodecCancelRows) == 0) {
                        if (auto status = cancel.check(kWhere); !status) {
                            return Unexpected(std::move(status.error()));
                        }
                    }
                    std::uint16_t* target =
                        expected == nullptr ? decoded.samples16.data() + row_samples * y : row.data();
                    if (TIFFReadScanline(t, target, y, 0) < 0) {
                        return fail(ErrorCode::Corrupt,
                                    std::string("written TIFF does not decode: ") + diagnostics.first_error,
                                    kWhere);
                    }
                    if (compare && std::memcmp(target, expected->samples16.data() + row_samples * y,
                                               row_samples * sizeof(std::uint16_t)) != 0) {
                        decoded.first_mismatched_row = y;
                        break;
                    }
                }
                decoded.samples_compared = compare;
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the TIFF verification buffer", kWhere);
    }
    opened->reset();
    if (diagnostics.errors != 0) {
        return fail(ErrorCode::Corrupt, std::string("written TIFF does not decode: ") + diagnostics.first_error,
                    kWhere);
    }
    decoded.warnings = diagnostics.warnings;
    decoded.first_warning = diagnostics.first_warning;
    return decoded;
}

std::string tiff_library_version() {
    return std::string("libtiff ") + TIFFLIB_VERSION_STR_MAJ_MIN_MIC;
}

}  // namespace lucent::exporter::detail
