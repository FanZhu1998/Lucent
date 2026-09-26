#include "lucent/export/export.hpp"

#include <algorithm>
#include <cstdint>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lucent/core/text.hpp>

#include "codecs.hpp"
#include "fault_injection.hpp"
#include "file_util.hpp"
#include "lcms_support.hpp"

namespace lucent::exporter {

// ---------------------------------------------------------------------------------------------
// Test seam

namespace detail {
namespace {

struct HookSlot {
    FaultHook hook = nullptr;
    void* context = nullptr;
};

thread_local HookSlot g_hook;

}  // namespace

void set_fault_hook(FaultHook hook, void* context) noexcept {
    g_hook = HookSlot{hook, context};
}

Status run_fault_hook(ExportStage stage, const std::filesystem::path& temp_path) {
    if (g_hook.hook == nullptr) {
        return ok();
    }
    return g_hook.hook(stage, temp_path, g_hook.context);
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------
// Settings

std::string_view to_string(ExportFormat format) noexcept {
    switch (format) {
        case ExportFormat::Jpeg:
            return "jpeg";
        case ExportFormat::Tiff:
            return "tiff";
    }
    return "unknown";
}

std::string_view to_string(ChromaSubsampling subsampling) noexcept {
    switch (subsampling) {
        case ChromaSubsampling::Yuv444:
            return "4:4:4";
        case ChromaSubsampling::Yuv420:
            return "4:2:0";
    }
    return "unknown";
}

std::string_view to_string(TiffCompression compression) noexcept {
    switch (compression) {
        case TiffCompression::None:
            return "none";
        case TiffCompression::Deflate:
            return "deflate";
    }
    return "unknown";
}

BitDepth bit_depth_for(ExportFormat format) noexcept {
    return format == ExportFormat::Tiff ? BitDepth::Sixteen : BitDepth::Eight;
}

ChromaSubsampling jpeg_chroma_subsampling(std::int32_t quality) noexcept {
    return quality >= kJpegFullChromaMinQuality ? ChromaSubsampling::Yuv444 : ChromaSubsampling::Yuv420;
}

Status ExportSettings::validate() const {
    constexpr const char* kWhere = "export.settings.validate";
    if (to_string(format) == "unknown") {
        return fail(ErrorCode::InvalidArgument, "unknown export format value " + std::to_string(static_cast<int>(format)),
                    kWhere);
    }
    if (auto status = exporter::validate(profile); !status) {
        return status;
    }
    if (auto status = exporter::validate(transform.intent); !status) {
        return status;
    }
    if (format == ExportFormat::Jpeg && (jpeg.quality < 1 || jpeg.quality > 100)) {
        return fail(ErrorCode::InvalidArgument, "JPEG quality must be in 1..100, got " + std::to_string(jpeg.quality),
                    kWhere);
    }
    if (format == ExportFormat::Tiff && to_string(tiff.compression) == "unknown") {
        return fail(ErrorCode::InvalidArgument,
                    "unknown TIFF compression value " + std::to_string(static_cast<int>(tiff.compression)), kWhere);
    }
    return ok();
}

// ---------------------------------------------------------------------------------------------
// Verification

namespace {

// TIFF tag values (TIFF 6.0) checked by verification.
constexpr std::int32_t kTiffPhotometricRgb = 2;
constexpr std::int32_t kTiffPlanarContig = 1;
constexpr std::int32_t kTiffOrientationTopLeft = 1;
constexpr std::int32_t kTiffSampleFormatUint = 1;
constexpr std::int32_t kTiffCompressionNone = 1;
constexpr std::int32_t kTiffCompressionAdobeDeflate = 8;

Status corrupt(std::string what, const char* where) {
    return fail(ErrorCode::Corrupt, "written file failed verification: " + std::move(what), where);
}

Status verify_decoded(const detail::DecodedFile& decoded, const OutputImage& image, const ExportSettings& settings,
                      std::span<const std::uint8_t> icc) {
    const char* where = settings.format == ExportFormat::Jpeg ? "export.jpeg.verify" : "export.tiff.verify";
    if (decoded.width != image.width || decoded.height != image.height) {
        return corrupt("dimensions " + std::to_string(decoded.width) + "x" + std::to_string(decoded.height) +
                           " differ from " + std::to_string(image.width) + "x" + std::to_string(image.height),
                       where);
    }
    const std::int32_t bits = static_cast<std::int32_t>(image.bit_depth);
    if (decoded.channels != 3 || decoded.bits_per_sample != bits) {
        return corrupt(std::to_string(decoded.channels) + " channels of " + std::to_string(decoded.bits_per_sample) +
                           " bits instead of 3 of " + std::to_string(bits),
                       where);
    }
    if (!std::ranges::equal(decoded.icc, icc)) {
        return corrupt(decoded.icc.empty() ? "no embedded ICC profile" : "embedded ICC profile differs", where);
    }
    if (decoded.warnings != 0) {
        return corrupt("decoder warning: " + decoded.first_warning, where);
    }
    if (settings.format == ExportFormat::Jpeg) {
        // The file's bytes were already compared with the encoder's output (compare_file_bytes);
        // this checks that the stream decodes completely, without warnings, as intended.
        const std::int32_t luma = jpeg_chroma_subsampling(settings.jpeg.quality) == ChromaSubsampling::Yuv444 ? 1 : 2;
        if (!decoded.jpeg_ycbcr || decoded.jpeg_luma_h_samp != luma || decoded.jpeg_luma_v_samp != luma) {
            return corrupt("unexpected JPEG colour space or sampling factors", where);
        }
        return ok();
    }
    const std::int32_t compression = settings.tiff.compression == TiffCompression::Deflate
                                         ? kTiffCompressionAdobeDeflate
                                         : kTiffCompressionNone;
    if (decoded.tiff_directories != 1 || decoded.tiff_photometric != kTiffPhotometricRgb ||
        decoded.tiff_planar != kTiffPlanarContig || decoded.tiff_orientation != kTiffOrientationTopLeft ||
        decoded.tiff_sample_format != kTiffSampleFormatUint || decoded.tiff_compression != compression) {
        return corrupt("unexpected TIFF layout tags", where);
    }
    if (!decoded.samples_compared) {
        return corrupt("the TIFF samples could not be compared with the encoded samples", where);
    }
    if (decoded.first_mismatched_row >= 0) {
        return corrupt("decoded TIFF row " + std::to_string(decoded.first_mismatched_row) +
                           " differs from the encoded samples",
                       where);
    }
    return ok();
}

Status check_image_matches(const OutputImage& image, const ExportSettings& settings) {
    if (auto status = image.validate(); !status) {
        return status;
    }
    if (image.bit_depth != bit_depth_for(settings.format)) {
        return fail(ErrorCode::InvalidArgument,
                    std::string(to_string(settings.format)) + " export requires " +
                        std::string(to_string(bit_depth_for(settings.format))) + "-bit samples",
                    "export.write");
    }
    if (image.profile != settings.profile) {
        return fail(ErrorCode::InvalidArgument, "image was quantised for a different output profile than requested",
                    "export.write");
    }
    return ok();
}

std::string encoder_description(ExportFormat format) {
    const std::string codec =
        format == ExportFormat::Jpeg ? detail::jpeg_library_version() : detail::tiff_library_version();
    return codec + "; lcms " + detail::lcms_library_version();
}

Expected<ExportReceipt> write_verified_impl(const OutputImage& image, const std::filesystem::path& destination,
                                            const ExportSettings& settings, const core::CancellationToken& cancel) {
    if (auto status = settings.validate(); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = check_image_matches(image, settings); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (settings.format == ExportFormat::Jpeg &&
        (image.width > kJpegMaxDimension || image.height > kJpegMaxDimension)) {
        return fail(ErrorCode::LimitExceeded,
                    "JPEG sides are limited to " + std::to_string(kJpegMaxDimension) + " pixels", "export.write");
    }
    auto icc = icc_profile(settings.profile);
    if (!icc) {
        return Unexpected(std::move(icc.error()));
    }
    if (auto status = detail::check_destination(destination); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = cancel.check("export.write"); !status) {
        return Unexpected(std::move(status.error()));
    }

    // JPEG is encoded into memory before any file exists, so the written file can be compared
    // byte for byte with the encoder's output.
    std::vector<std::uint8_t> jpeg_stream;
    if (settings.format == ExportFormat::Jpeg) {
        auto encoded = detail::encode_jpeg(image, settings.jpeg, *icc, cancel);
        if (!encoded) {
            return Unexpected(std::move(encoded.error()));
        }
        jpeg_stream = std::move(*encoded);
    }

    auto reserved = detail::reserve_temporary_sibling(detail::destination_directory(destination));
    if (!reserved) {
        return Unexpected(std::move(reserved.error()));
    }
    detail::TemporaryFile temp(std::move(*reserved));
    if (auto status = detail::run_fault_hook(detail::ExportStage::TempReserved, temp.path()); !status) {
        return Unexpected(std::move(status.error()));
    }

    // Write (and for TIFF encode), then flush to the device.
    if (settings.format == ExportFormat::Jpeg) {
        if (auto status = detail::write_file_bytes(temp.path(), jpeg_stream, cancel); !status) {
            return Unexpected(std::move(status.error()));
        }
    } else {
        if (auto status = detail::write_tiff(temp.path(), image, settings.tiff, *icc, cancel); !status) {
            return Unexpected(std::move(status.error()));
        }
        if (auto status = detail::flush_to_disk(temp.path()); !status) {
            return Unexpected(std::move(status.error()));
        }
    }
    if (auto status = detail::run_fault_hook(detail::ExportStage::Written, temp.path()); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = cancel.check("export.write"); !status) {
        return Unexpected(std::move(status.error()));
    }

    // Re-open and verify: JPEG byte for byte against the encoder's output, then a full decode;
    // TIFF by a full decode compared row by row with the encoded samples.
    if (settings.format == ExportFormat::Jpeg) {
        if (auto status = detail::compare_file_bytes(temp.path(), jpeg_stream, cancel); !status) {
            return Unexpected(std::move(status.error()));
        }
        jpeg_stream = {};  // release before decoding
    }
    auto decoded = settings.format == ExportFormat::Jpeg ? detail::read_jpeg(temp.path(), cancel, false)
                                                         : detail::read_tiff(temp.path(), cancel, &image);
    if (!decoded) {
        return Unexpected(std::move(decoded.error()));
    }
    if (auto status = verify_decoded(*decoded, image, settings, *icc); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = detail::run_fault_hook(detail::ExportStage::Verified, temp.path()); !status) {
        return Unexpected(std::move(status.error()));
    }

    auto hashed = core::hash_file(temp.path(), cancel);
    if (!hashed) {
        return Unexpected(std::move(hashed.error()));
    }
    auto icc_hash = core::sha256(std::as_bytes(*icc));
    if (!icc_hash) {
        return Unexpected(std::move(icc_hash.error()));
    }
    if (auto status = detail::run_fault_hook(detail::ExportStage::Hashed, temp.path()); !status) {
        return Unexpected(std::move(status.error()));
    }
    // Last point of no return: a cancelled export never publishes.
    if (auto status = cancel.check("export.write"); !status) {
        return Unexpected(std::move(status.error()));
    }

    if (auto status = detail::publish_no_clobber(temp.path(), destination); !status) {
        return Unexpected(std::move(status.error()));
    }
    temp.release();

    ExportReceipt receipt;
    receipt.path = destination;
    receipt.format = settings.format;
    receipt.profile = settings.profile;
    receipt.bit_depth = image.bit_depth;
    receipt.width = image.width;
    receipt.height = image.height;
    receipt.byte_size = hashed->identity.size_bytes;
    receipt.sha256 = hashed->digest;
    receipt.icc_sha256 = *icc_hash;
    receipt.settings = settings;
    receipt.chroma_subsampling = jpeg_chroma_subsampling(settings.jpeg.quality);
    receipt.encoder = encoder_description(settings.format);
    return receipt;
}

}  // namespace

Expected<ExportReceipt> write_verified(const OutputImage& image, const std::filesystem::path& destination,
                                       const ExportSettings& settings, core::CancellationToken cancel) {
    try {
        return write_verified_impl(image, destination, settings, cancel);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while writing " + core::path_to_utf8_lossy(destination),
                    "export.write");
    }
}

Expected<ExportReceipt> export_image(image::ConstImageView<float> rgb, const image::ColorEncoding& source,
                                     const std::filesystem::path& destination, const ExportSettings& settings,
                                     core::CancellationToken cancel) {
    if (auto status = settings.validate(); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = detail::check_destination(destination); !status) {
        return Unexpected(std::move(status.error()));
    }
    auto encoded =
        encode_for_output(rgb, source, settings.profile, bit_depth_for(settings.format), settings.transform, cancel);
    if (!encoded) {
        return Unexpected(std::move(encoded.error()));
    }
    return write_verified(*encoded, destination, settings, std::move(cancel));
}

}  // namespace lucent::exporter
