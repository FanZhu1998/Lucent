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
#include <string>
#include <vector>

#include <tiffio.h>
#include <tiffvers.h>

#include "codecs.hpp"
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

}  // namespace

Status write_tiff(const std::filesystem::path& path, const OutputImage& image, const TiffSettings& settings,
                  std::span<const std::uint8_t> icc, const core::CancellationToken& cancel) {
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
                     TIFFSetField(t, TIFFTAG_SOFTWARE, "Lucent") &&
                     TIFFSetField(t, TIFFTAG_ICCPROFILE, static_cast<std::uint32_t>(icc.size()), icc.data());
    if (fields_ok && deflate) {
        fields_ok = TIFFSetField(t, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL) &&
                    TIFFSetField(t, TIFFTAG_ZIPQUALITY, kDeflateLevel);
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
    if (!TIFFFlush(t)) {
        return Unexpected(crt_file_error(kWhere, std::string("cannot flush the TIFF file: ") + diagnostics.first_error));
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
