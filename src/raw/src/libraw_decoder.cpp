// The LibRaw adapter (libraw_decoder.hpp). Flow of one call:
//   open the file (ReadOnlyFile) -> capture identity -> enforce the file-size limit
//   -> LibRaw::open_datastream over a LibRawFileStream -> subimage policy
//   -> DNG inspection (TIFF-based DNGs) -> metadata
//   [decode only: refuse undecodable layouts -> pixel/memory limits -> unpack -> final metadata
//    -> refuse levels that cannot be normalised -> copy pixels into Lucent-owned buffers]
//   -> verify the file identity is unchanged.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <lucent/core/file.hpp>
#include <lucent/image/pixel_buffer.hpp>
#include <lucent/raw/file_source.hpp>
#include <lucent/raw/libraw_decoder.hpp>
#include <lucent/raw/tiff_inspector.hpp>

#include "container_extent.hpp"
#include "libraw_stream.hpp"
#include "libraw_support.hpp"

namespace lucent::raw {

namespace {

using detail::ExtractionContext;
using detail::LibRawBuffer;
using detail::LibRawCallState;
using detail::LibRawFileStream;

constexpr std::string_view kWhereLimits = "raw.libraw.limits";
constexpr std::string_view kWhereSubimage = "raw.libraw.subimage";
constexpr std::string_view kWhereSource = "raw.libraw.source";
constexpr std::string_view kWhereCopy = "raw.libraw.copy";
constexpr std::string_view kWhereInterpret = "raw.libraw.interpret";
constexpr std::string_view kWhereAdapter = "raw.libraw.adapter";
constexpr std::string_view kWhereTruncated = "raw.libraw.truncated";
constexpr std::string_view kWhereContainer = "raw.libraw.container";
constexpr std::string_view kWhereLevels = "raw.libraw.levels";

// Reads starting at the end of the file that unpacking may make before the data is considered
// missing (read-ahead of complete files; see decode_impl).
constexpr std::uint64_t kMaxReadsAtEnd = 16;

// Rows copied between cancellation checks.
constexpr std::int32_t kRowsPerCancelCheck = 256;

std::string where_of(std::string_view stage) {
    return std::format("raw.libraw.{}", stage);
}

// ---------------------------------------------------------------------------------------------
// Source file
// ---------------------------------------------------------------------------------------------

struct OpenedSource {
    core::ReadOnlyFile file;
    core::FileIdentity identity;
    ContainerFormat container = ContainerFormat::Unknown;
};

Expected<OpenedSource> open_source(const std::filesystem::path& path, const DecodeLimits& limits) {
    auto file = core::ReadOnlyFile::open(path, core::AccessHint::Normal);
    if (!file) {
        return Unexpected(std::move(file.error()));
    }
    auto identity = file->identity();
    if (!identity) {
        return Unexpected(std::move(identity.error()));
    }
    if (identity->size_bytes > limits.max_file_bytes) {
        return fail(
            ErrorCode::LimitExceeded,
            std::format("file has {} bytes; the limit is {}", identity->size_bytes, limits.max_file_bytes),
            std::string(kWhereLimits));
    }
    if (identity->size_bytes == 0) {
        return fail(ErrorCode::Unsupported, "the file is empty", where_of(detail::kStageOpen));
    }
    std::array<std::byte, 16> header{};
    const auto header_size =
        static_cast<std::size_t>(std::min<std::uint64_t>(header.size(), identity->size_bytes));
    auto read = file->read_at(0, std::span<std::byte>(header.data(), header_size));
    if (!read) {
        return Unexpected(std::move(read.error()));
    }
    OpenedSource source;
    source.container = detect_container_format(std::span<const std::byte>(header.data(), *read));
    source.identity = *identity;
    source.file = std::move(*file);
    return source;
}

// SourceChanged unless the file object and its content version are exactly as captured when the
// call started: checked through the handle (writes, truncation, deletion) and through the path
// (renamed away or replaced), like core::hash_file's stable read.
Status verify_unchanged(const OpenedSource& source, const std::filesystem::path& path) {
    const std::string where(kWhereSource);
    auto after = source.file.identity();
    if (!after) {
        return Unexpected(std::move(after.error()));
    }
    if (*after != source.identity) {
        return fail(ErrorCode::SourceChanged, "the file was modified while it was being decoded", where);
    }
    auto deleted = source.file.is_delete_pending();
    if (!deleted) {
        return Unexpected(std::move(deleted.error()));
    }
    if (*deleted) {
        return fail(ErrorCode::SourceChanged, "the file was deleted while it was being decoded", where);
    }
    auto at_path = core::stat_file(path);
    if (!at_path) {
        if (at_path.error().code == ErrorCode::NotFound) {
            return fail(ErrorCode::SourceChanged, "the file was moved or deleted while it was being decoded",
                        where);
        }
        return Unexpected(std::move(at_path.error()));
    }
    if (*at_path != source.identity) {
        return fail(ErrorCode::SourceChanged, "the path now names a different or modified file", where);
    }
    return ok();
}

// ---------------------------------------------------------------------------------------------
// One LibRaw instance reading the source through its own stream
// ---------------------------------------------------------------------------------------------

class Session {
public:
    Session(const OpenedSource& source, core::CancellationToken cancel, std::uint32_t shot,
            const DecodeLimits& limits)
        : stream_(std::make_unique<LibRawFileStream>(source.file, source.identity.size_bytes, cancel)),
          raw_(std::make_unique<LibRaw>(LIBRAW_OPTIONS_NO_DATAERR_CALLBACK)) {
        state_.cancel = std::move(cancel);
        stream_->set_owner(raw_.get());
        detail::install_callbacks(*raw_, state_);
        libraw_raw_unpack_params_t& params = raw_->imgdata.rawparams;
        params.shot_select = shot;
        const std::uint64_t mb = std::max<std::uint64_t>(1, limits.max_memory_bytes >> 20);
        params.max_raw_memory_mb =
            static_cast<unsigned>(std::min<std::uint64_t>(mb, std::numeric_limits<unsigned>::max()));
        // Keep floating-point DNG data as float instead of LibRaw's default rescaling to integers.
        params.options &= ~static_cast<unsigned>(LIBRAW_RAWOPTIONS_CONVERTFLOAT_TO_INT);
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() = default;  // raw_ (declared last) is destroyed before the stream it reads

    [[nodiscard]] LibRaw& raw() noexcept { return *raw_; }

    // Runs a LibRaw call and converts its result, the stream's observations and LibRaw's data
    // error count into a Status.
    [[nodiscard]] Status run(std::string_view stage, int code) {
        if (code != LIBRAW_SUCCESS) {
            return Unexpected(detail::libraw_failure(code, stage, *stream_, state_));
        }
        if (state_.cancel_observed || stream_->cancel_observed()) {
            return fail(ErrorCode::Cancelled, std::format("{} was cancelled", stage), where_of(stage));
        }
        if (const auto& read_error = stream_->read_error(); read_error) {
            Error error = *read_error;
            error.message = std::format("{} failed reading the file: {}", stage, error.message);
            return Unexpected(std::move(error));
        }
        const int data_errors = raw_->error_count();
        if (data_errors > 0 || state_.data_errors > 0) {
            const std::string position =
                state_.first_data_error_offset < 0
                    ? std::string("unexpected end of file")
                    : std::format("first near byte {}", state_.first_data_error_offset);
            return fail(ErrorCode::Corrupt,
                        std::format("LibRaw reported {} data error(s) during {} ({})",
                                    std::max<std::int64_t>(data_errors, state_.data_errors), stage, position),
                        where_of(stage), LIBRAW_DATA_ERROR);
        }
        return ok();
    }

    [[nodiscard]] Status open() { return run(detail::kStageOpen, raw_->open_datastream(stream_.get())); }
    // Unpacks the pixel data; afterwards past_end_bytes() tells how far the decoder tried to read
    // beyond the end of the file while doing so.
    [[nodiscard]] Status unpack() {
        stream_->reset_past_end();
        return run(detail::kStageUnpack, raw_->unpack());
    }
    [[nodiscard]] std::uint64_t past_end_bytes() const noexcept { return stream_->past_end_bytes(); }
    [[nodiscard]] std::uint64_t reads_at_end() const noexcept { return stream_->reads_at_end(); }

private:
    LibRawCallState state_;
    std::unique_ptr<LibRawFileStream> stream_;
    std::unique_ptr<LibRaw> raw_;
};

// ---------------------------------------------------------------------------------------------
// DNG inspection
// ---------------------------------------------------------------------------------------------

struct Inspection {
    std::optional<TiffSummary> summary;
    std::optional<Error> error;
    // Fujifilm RAF: the crop the file declares, read by Lucent (detail::read_raf_crop).
    std::optional<detail::InsetCrop> raf_crop;
};

// The DNG structure (TIFF inspector) of a DNG, or the declared crop of a RAF (which restores the
// edges LibRaw moves to its CFA period, libraw_support.hpp); nothing for other files. A RAF whose
// crop cannot be read simply has none: LibRaw's own reading remains.
Inspection inspect_source(const OpenedSource& source, LibRaw& raw) {
    Inspection inspection;
    if (source.container == ContainerFormat::Raf) {
        if (auto file_source = FileSource::create(source.file); file_source) {
            if (auto crop = detail::read_raf_crop(*file_source); crop && *crop) {
                inspection.raf_crop =
                    detail::InsetCrop{(*crop)->left, (*crop)->top, (*crop)->width, (*crop)->height};
            }
        }
        return inspection;
    }
    if (raw.imgdata.idata.dng_version == 0) {
        return inspection;
    }
    if (source.container != ContainerFormat::Tiff) {
        inspection.error =
            Error{ErrorCode::Unsupported, "the file does not start with a TIFF header", "raw.tiff.header", 0};
        return inspection;
    }
    auto file_source = FileSource::create(source.file);
    if (!file_source) {
        inspection.error = std::move(file_source.error());
        return inspection;
    }
    auto summary = inspect_tiff(*file_source);
    if (!summary) {
        inspection.error = std::move(summary.error());
        return inspection;
    }
    inspection.summary = std::move(*summary);
    return inspection;
}

ExtractionContext make_context(const std::filesystem::path& path, const OpenedSource& source,
                               const Inspection& inspection, std::uint32_t subimage) {
    ExtractionContext context;
    context.stage = MetadataStage::AfterOpen;
    context.source.path = path;
    context.source.size_bytes = source.identity.size_bytes;
    context.source.identity = source.identity;
    context.sniffed_container = source.container;
    context.subimage_index = subimage;
    context.tiff = inspection.summary ? &*inspection.summary : nullptr;
    context.tiff_error = inspection.error;
    context.raf_crop = inspection.raf_crop;
    return context;
}

Status require_decodable(const RawMetadata& metadata) {
    if (metadata.interpretation.status != InterpretationStatus::NotDecodable) {
        return ok();
    }
    std::string reasons;
    for (const InterpretationIssue& issue : metadata.interpretation.issues) {
        if (issue.kind == InterpretationIssueKind::DecoderUnsupportedFormat ||
            issue.kind == InterpretationIssueKind::UnsupportedSensorLayout ||
            issue.kind == InterpretationIssueKind::UnsupportedChannelLayout) {
            reasons += reasons.empty() ? issue.message : "; " + issue.message;
        }
    }
    return fail(ErrorCode::Unsupported, reasons, std::string(kWhereInterpret));
}

// Levels that cannot be normalised (white at or below the black level of some site) come from
// the file or the decoder's reading of it, so they are Corrupt data rather than the contract
// violation (Internal) that check_consistency() would report.
Status check_levels(const SensorLevels& levels) {
    const double max_black = levels.max_black_level();
    if (std::isfinite(levels.white) && std::isfinite(max_black) && levels.white > max_black) {
        return ok();
    }
    return fail(
        ErrorCode::Corrupt,
        std::format("the white level {} is not above the highest black level {}", levels.white, max_black),
        std::string(kWhereLevels));
}

// Errors of the structural check that concern the file's content (as opposed to reading it).
bool is_structural_error(ErrorCode code) noexcept {
    return code == ErrorCode::Corrupt || code == ErrorCode::LimitExceeded || code == ErrorCode::Unsupported;
}

// The structural completeness check of container_extent.hpp for the source's container.
Expected<detail::ImageDataExtent> scan_container(const OpenedSource& source) {
    auto file_source = FileSource::create(source.file);
    if (!file_source) {
        return Unexpected(std::move(file_source.error()));
    }
    return detail::find_container_overruns(*file_source, source.container);
}

Error truncation_error(const detail::ImageDataExtent& extent, std::string_view decoder_message) {
    constexpr std::size_t kListed = 4;  // a damaged file can declare thousands of overruns
    std::string details;
    for (std::size_t i = 0; i < extent.overruns.size() && i < kListed; ++i) {
        const detail::ImageDataOverrun& overrun = extent.overruns[i];
        details += std::format("{}{}: {}", details.empty() ? "" : "; ", overrun.ifd_path, overrun.detail);
    }
    if (extent.overruns.size() > kListed) {
        details += std::format("; and {} more", extent.overruns.size() - kListed);
    }
    std::string message = std::format("the file is truncated or damaged ({})", details);
    if (!decoder_message.empty()) {
        message += std::format("; LibRaw: {}", decoder_message);
    }
    return Error{ErrorCode::Corrupt, std::move(message), std::string(kWhereTruncated), 0};
}

// Refines a failed LibRaw open with what Lucent can establish about the file itself:
//   * a recognised container whose structure extends beyond the end of the file is a truncated
//     copy (Corrupt at raw.libraw.truncated), whatever LibRaw answered; LibRaw reports several
//     truncated TIFF files as LIBRAW_FILE_UNSUPPORTED, which would otherwise read as "not a
//     supported format";
//   * a vendor RAW container (ORF, RW2, CR3, RAF: signatures only RAW files carry) whose own
//     structure is malformed is a damaged RAW file (Corrupt). A plain TIFF signature is shared by
//     non-RAW images, so a TIFF that LibRaw rejects keeps LibRaw's answer;
//   * a file whose signature Lucent does not recognise and that LibRaw cannot parse is "not a
//     RAW file" (Unsupported) rather than a damaged RAW file (Corrupt).
// Cancellation, read failures and resource errors of the open are kept unchanged.
Error refine_open_error(Error error, const OpenedSource& source) {
    if (error.code != ErrorCode::Unsupported && error.code != ErrorCode::Corrupt) {
        return error;
    }
    if (!detail::has_structural_check(source.container)) {
        if (source.container == ContainerFormat::Unknown && error.code == ErrorCode::Corrupt) {
            error.code = ErrorCode::Unsupported;
            error.message = std::format("not a RAW file this build recognises ({})", error.message);
        }
        return error;
    }
    auto extent = scan_container(source);
    if (!extent) {
        if (!is_structural_error(extent.error().code)) {
            return std::move(extent.error());  // reading the file failed: that is the better answer
        }
        if (source.container != ContainerFormat::Tiff && extent.error().code == ErrorCode::Corrupt) {
            return Error{ErrorCode::Corrupt,
                         std::format("the {} structure is damaged ({}); LibRaw: {}",
                                     to_string(source.container), extent.error().message, error.message),
                         std::string(kWhereContainer), error.native_code};
        }
        return error;
    }
    if (!extent->overruns.empty()) {
        Error truncated = truncation_error(*extent, error.message);
        truncated.native_code = error.native_code;
        return truncated;
    }
    return error;
}

// Corrupt when a recognised container declares structure or image data beyond its end (a
// truncated copy); see container_extent.hpp. When the structure cannot be scanned the check is
// skipped with a warning: LibRaw's own parser accepted the file, and the DNG inspector reports
// DNG structure problems. Read failures (and a file that changed) are errors: the check must not
// be skipped silently.
Status check_image_data_complete(const OpenedSource& source, std::vector<std::string>& warnings) {
    if (!detail::has_structural_check(source.container)) {
        return ok();
    }
    auto extent = scan_container(source);
    if (!extent) {
        if (!is_structural_error(extent.error().code)) {
            return Unexpected(std::move(extent.error()));
        }
        warnings.push_back(
            std::format("the extent of the image data could not be verified: {}", extent.error().describe()));
        return ok();
    }
    for (const std::string& warning : extent->warnings) {
        warnings.push_back(std::format("image data extent: {}", warning));
    }
    if (extent->overruns.empty()) {
        return ok();
    }
    return Unexpected(truncation_error(*extent, {}));
}

// ---------------------------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------------------------

// Pixel memory of a decode: LibRaw's raw buffer (its allocation formula in unpack(): (height + 8)
// rows, and for legacy full-colour decoders a 4 x uint16 image) plus Lucent's copy.
std::uint64_t estimated_decode_bytes(const RawMetadata& m, unsigned decoder_flags) {
    const std::uint64_t w = m.geometry.raw_width;
    const std::uint64_t h = m.geometry.raw_height;
    const std::uint64_t sample = m.representation.sample_encoding == SampleEncoding::Float32 ? 4 : 2;
    std::uint64_t decoder_planes = 1;
    std::uint64_t output_channels = 1;
    if (m.representation.kind == RawRepresentation::LinearColor) {
        decoder_planes = (decoder_flags & LIBRAW_DECODER_3CHANNEL) != 0 ? 3 : 4;
        output_channels = 3;
    }
    const std::uint64_t decoder_bytes = w * (h + 8) * decoder_planes * sample;
    const std::uint64_t row_bytes = (w * output_channels * sample + image::kRowAlignment - 1) /
                                    image::kRowAlignment * image::kRowAlignment;
    return decoder_bytes + row_bytes * h;
}

Status check_decode_limits(const RawMetadata& m, LibRaw& raw, const DecodeLimits& limits) {
    const std::uint64_t pixels = std::uint64_t{m.geometry.raw_width} * m.geometry.raw_height;
    if (pixels > limits.max_pixels) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("raw frame {}x{} has {} pixels; the limit is {}", m.geometry.raw_width,
                                m.geometry.raw_height, pixels, limits.max_pixels),
                    std::string(kWhereLimits));
    }
    libraw_decoder_info_t info{};
    raw.get_decoder_info(&info);
    const std::uint64_t bytes = estimated_decode_bytes(m, info.decoder_flags);
    if (bytes > limits.max_memory_bytes) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("decoding needs about {} bytes of pixel memory; the limit is {}", bytes,
                                limits.max_memory_bytes),
                    std::string(kWhereLimits));
    }
    return ok();
}

// ---------------------------------------------------------------------------------------------
// Pixel copy
// ---------------------------------------------------------------------------------------------

// Copies `height` rows of `width` pixels with `source_channels` interleaved samples each, rows
// `pitch_bytes` apart, keeping the first `channels` samples of every pixel.
template <image::PixelSample Sample>
Expected<image::ImageBuffer<Sample>> copy_pixels(const void* base, std::uint64_t pitch_bytes,
                                                 std::int32_t width, std::int32_t height,
                                                 std::int32_t source_channels, std::int32_t channels,
                                                 const DecodeLimits& limits,
                                                 const core::CancellationToken& cancel) {
    const std::string where(kWhereCopy);
    const std::uint64_t min_pitch = std::uint64_t(width) * std::uint64_t(source_channels) * sizeof(Sample);
    if (base == nullptr || width < 1 || height < 1 || pitch_bytes < min_pitch ||
        pitch_bytes % sizeof(Sample) != 0 || channels > source_channels) {
        return fail(ErrorCode::Internal,
                    std::format("unexpected LibRaw buffer layout: {}x{} x{} samples of {} bytes, pitch {}",
                                width, height, source_channels, sizeof(Sample), pitch_bytes),
                    where);
    }
    image::AllocationLimits allocation;
    allocation.max_bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(limits.max_memory_bytes, std::numeric_limits<std::size_t>::max()));
    auto buffer = image::ImageBuffer<Sample>::create_uninitialized(width, height, channels, allocation);
    if (!buffer) {
        return Unexpected(std::move(buffer.error()));
    }
    const auto* bytes = static_cast<const std::byte*>(base);
    for (std::int32_t y = 0; y < height; ++y) {
        if (y % kRowsPerCancelCheck == 0 && cancel.is_cancelled()) {
            return fail(ErrorCode::Cancelled, "copying the decoded pixels was cancelled", where);
        }
        const auto* source =
            reinterpret_cast<const Sample*>(bytes + static_cast<std::uint64_t>(y) * pitch_bytes);
        Sample* destination = buffer->row(y);
        if (channels == source_channels) {
            std::memcpy(
                destination, source,
                static_cast<std::size_t>(width) * static_cast<std::size_t>(channels) * sizeof(Sample));
        } else {
            for (std::int32_t x = 0; x < width; ++x) {
                for (std::int32_t c = 0; c < channels; ++c) {
                    destination[x * channels + c] = source[x * source_channels + c];
                }
            }
        }
    }
    return buffer;
}

Expected<RawImage> copy_image(LibRaw& raw, const RawMetadata& m, const DecodeLimits& limits,
                              const core::CancellationToken& cancel) {
    const libraw_rawdata_t& data = raw.imgdata.rawdata;
    const std::uint64_t pitch = raw.imgdata.sizes.raw_pitch;
    const auto width = static_cast<std::int32_t>(m.geometry.raw_width);
    const auto height = static_cast<std::int32_t>(m.geometry.raw_height);

    Expected<RawPixels> pixels = fail(ErrorCode::Internal, "no pixel buffer", std::string(kWhereCopy));
    const auto wrap = [](auto&& buffer) -> Expected<RawPixels> {
        if (!buffer) {
            return Unexpected(std::move(buffer.error()));
        }
        return RawPixels(std::move(*buffer));
    };
    switch (detail::unpacked_buffer(raw)) {
        case LibRawBuffer::Raw:
            pixels =
                wrap(copy_pixels<std::uint16_t>(data.raw_image, pitch, width, height, 1, 1, limits, cancel));
            break;
        case LibRawBuffer::Color3:
            pixels = wrap(
                copy_pixels<std::uint16_t>(data.color3_image, pitch, width, height, 3, 3, limits, cancel));
            break;
        case LibRawBuffer::Color4:
            pixels = wrap(
                copy_pixels<std::uint16_t>(data.color4_image, pitch, width, height, 4, 3, limits, cancel));
            break;
        case LibRawBuffer::Float1:
            pixels = wrap(copy_pixels<float>(data.float_image, pitch, width, height, 1, 1, limits, cancel));
            break;
        case LibRawBuffer::Float3:
            pixels = wrap(copy_pixels<float>(data.float3_image, pitch, width, height, 3, 3, limits, cancel));
            break;
        case LibRawBuffer::Float4:
            pixels = wrap(copy_pixels<float>(data.float4_image, pitch, width, height, 4, 3, limits, cancel));
            break;
        case LibRawBuffer::None:
            break;
    }
    if (!pixels) {
        return Unexpected(std::move(pixels.error()));
    }
    switch (m.representation.kind) {
        case RawRepresentation::Mosaic: {
            MosaicFrame frame;
            frame.pixels = std::move(*pixels);
            frame.cfa = m.representation.cfa;
            return RawImage(std::move(frame));
        }
        case RawRepresentation::LinearColor: {
            LinearColorImage image;
            image.pixels = std::move(*pixels);
            return RawImage(std::move(image));
        }
        case RawRepresentation::Monochrome: {
            MonochromeFrame frame;
            frame.pixels = std::move(*pixels);
            return RawImage(std::move(frame));
        }
    }
    return fail(ErrorCode::Internal, "unknown representation", std::string(kWhereCopy));
}

// ---------------------------------------------------------------------------------------------
// The adapter
// ---------------------------------------------------------------------------------------------

class LibRawDecoder final : public RawDecoder {
public:
    [[nodiscard]] std::string_view id() const noexcept override { return kLibRawDecoderId; }

    [[nodiscard]] Expected<ProbeResult> probe(const std::filesystem::path& path, const ProbeOptions& options,
                                              core::CancellationToken cancel) const override {
        return guarded("probe", [&] { return probe_impl(path, options, cancel); });
    }

    [[nodiscard]] Expected<DecodedRaw> decode(const std::filesystem::path& path, const DecodeOptions& options,
                                              core::CancellationToken cancel) const override {
        return guarded("decode", [&] { return decode_impl(path, options, cancel); });
    }

private:
    // No exception crosses the adapter boundary (docs/development/conventions.md).
    template <class F>
    static auto guarded(std::string_view operation, F&& body) -> decltype(body()) {
        try {
            return body();
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, std::format("{} ran out of memory", operation),
                        std::string(kWhereAdapter));
        } catch (const std::exception& e) {
            return fail(ErrorCode::Internal, std::format("{} raised an exception: {}", operation, e.what()),
                        std::string(kWhereAdapter));
        } catch (...) {
            return fail(ErrorCode::Internal, std::format("{} raised an unknown exception", operation),
                        std::string(kWhereAdapter));
        }
    }

    static Status check_subimage(std::optional<std::uint32_t> requested, std::uint32_t count,
                                 bool must_choose) {
        if (must_choose && !requested && count > 1) {
            return fail(
                ErrorCode::InvalidArgument,
                std::format("the file holds {} images; DecodeOptions::subimage must select one", count),
                std::string(kWhereSubimage));
        }
        const std::uint32_t index = requested.value_or(0);
        if (index >= count) {
            return fail(ErrorCode::NotFound,
                        std::format("subimage {} was requested but the file holds {} image(s)", index, count),
                        std::string(kWhereSubimage), LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE);
        }
        return ok();
    }

    static Expected<ProbeResult> probe_impl(const std::filesystem::path& path, const ProbeOptions& options,
                                            const core::CancellationToken& cancel) {
        if (auto status = cancel.check("raw.libraw.probe"); !status) {
            return Unexpected(std::move(status.error()));
        }
        if (auto valid = options.limits.validate(); !valid) {
            return Unexpected(std::move(valid.error()));
        }
        auto source = open_source(path, options.limits);
        if (!source) {
            return Unexpected(std::move(source.error()));
        }
        const std::uint32_t shot = options.subimage.value_or(0);
        Session session(*source, cancel, shot, options.limits);
        if (auto opened = session.open(); !opened) {
            return Unexpected(refine_open_error(std::move(opened.error()), *source));
        }
        std::vector<std::string> open_warnings;
        if (auto complete = check_image_data_complete(*source, open_warnings); !complete) {
            return Unexpected(std::move(complete.error()));
        }
        const std::uint32_t count = std::max(1u, session.raw().imgdata.idata.raw_count);
        if (auto chosen = check_subimage(options.subimage, count, false); !chosen) {
            return Unexpected(std::move(chosen.error()));
        }
        const Inspection inspection = inspect_source(*source, session.raw());
        auto metadata =
            detail::extract_metadata(session.raw(), make_context(path, *source, inspection, shot));
        if (!metadata) {
            return Unexpected(std::move(metadata.error()));
        }

        ProbeResult result;
        result.metadata = std::move(*metadata);
        result.metadata.decoder.warnings.insert(result.metadata.decoder.warnings.end(), open_warnings.begin(),
                                                open_warnings.end());
        if (options.describe_subimages) {
            const std::uint32_t described = std::min(count, ProbeOptions::kMaxDescribedSubimages);
            for (std::uint32_t index = 0; index < described; ++index) {
                auto info = describe_subimage(path, *source, inspection, index, shot, result.metadata,
                                              options, cancel);
                if (!info) {
                    return Unexpected(std::move(info.error()));
                }
                result.subimages.push_back(std::move(*info));
            }
        }
        if (auto unchanged = verify_unchanged(*source, path); !unchanged) {
            return Unexpected(std::move(unchanged.error()));
        }
        return result;
    }

    // Cancellation and source failures end the probe; any other failure to open one subimage is
    // recorded in its entry.
    static Expected<SubimageInfo> describe_subimage(const std::filesystem::path& path,
                                                    const OpenedSource& source, const Inspection& inspection,
                                                    std::uint32_t index, std::uint32_t described_shot,
                                                    const RawMetadata& described, const ProbeOptions& options,
                                                    const core::CancellationToken& cancel) {
        SubimageInfo info;
        info.index = index;
        const auto fill = [&info](const RawMetadata& m) {
            info.role = m.subimage_role;
            info.interpretation = m.interpretation.status;
            info.raw_width = m.geometry.raw_width;
            info.raw_height = m.geometry.raw_height;
            info.visible_area = m.geometry.visible_area;
            info.representation = m.representation.kind;
            info.unpacker = m.decoder.unpacker;
        };
        if (index == described_shot) {
            fill(described);
            return info;
        }
        if (auto status = cancel.check("raw.libraw.probe"); !status) {
            return Unexpected(std::move(status.error()));
        }
        Session session(source, cancel, index, options.limits);
        Expected<RawMetadata> metadata = session.open().and_then([&]() {
            return detail::extract_metadata(session.raw(), make_context(path, source, inspection, index));
        });
        if (!metadata) {
            const ErrorCode code = metadata.error().code;
            if (code == ErrorCode::Cancelled || code == ErrorCode::SourceChanged ||
                code == ErrorCode::IoError || code == ErrorCode::OutOfMemory) {
                return Unexpected(std::move(metadata.error()));
            }
            info.error = std::move(metadata.error());
            return info;
        }
        fill(*metadata);
        return info;
    }

    static Expected<DecodedRaw> decode_impl(const std::filesystem::path& path, const DecodeOptions& options,
                                            const core::CancellationToken& cancel) {
        if (auto status = cancel.check("raw.libraw.decode"); !status) {
            return Unexpected(std::move(status.error()));
        }
        if (auto valid = options.limits.validate(); !valid) {
            return Unexpected(std::move(valid.error()));
        }
        auto source = open_source(path, options.limits);
        if (!source) {
            return Unexpected(std::move(source.error()));
        }
        const std::uint32_t shot = options.subimage.value_or(0);
        Session session(*source, cancel, shot, options.limits);
        if (auto opened = session.open(); !opened) {
            return Unexpected(refine_open_error(std::move(opened.error()), *source));
        }
        std::vector<std::string> open_warnings;
        if (auto complete = check_image_data_complete(*source, open_warnings); !complete) {
            return Unexpected(std::move(complete.error()));
        }
        LibRaw& raw = session.raw();
        const std::uint32_t count = std::max(1u, raw.imgdata.idata.raw_count);
        if (auto chosen = check_subimage(options.subimage, count, true); !chosen) {
            return Unexpected(std::move(chosen.error()));
        }

        const Inspection inspection = inspect_source(*source, raw);
        ExtractionContext context = make_context(path, *source, inspection, shot);
        auto opened_metadata = detail::extract_metadata(raw, context);
        if (!opened_metadata) {
            return Unexpected(std::move(opened_metadata.error()));
        }
        if (auto decodable = require_decodable(*opened_metadata); !decodable) {
            return Unexpected(std::move(decodable.error()));
        }
        if (auto within = check_decode_limits(*opened_metadata, raw, options.limits); !within) {
            return Unexpected(std::move(within.error()));
        }
        if (auto status = cancel.check("raw.libraw.unpack"); !status) {
            return Unexpected(std::move(status.error()));
        }
        if (auto unpacked = session.unpack(); !unpacked) {
            return Unexpected(std::move(unpacked.error()));
        }

        // Several LibRaw decoders report success when the file ends inside the pixel data: reads
        // past the end return nothing and the decoder keeps going. Complete files may still be
        // read one buffer beyond their end by read-ahead (observed: OM System ORF, one read), so
        // only repeated reads at the end of the file are taken as missing data.
        std::vector<std::string> unpack_warnings;
        if (session.reads_at_end() > kMaxReadsAtEnd) {
            return fail(
                ErrorCode::Corrupt,
                std::format(
                    "the decoder tried {} times to read beyond the end of the file ({} bytes missing); "
                    "the file is truncated or damaged",
                    session.reads_at_end(), session.past_end_bytes()),
                where_of(detail::kStageUnpack));
        }
        if (session.past_end_bytes() > 0) {
            unpack_warnings.push_back(
                std::format("the decoder read {} byte(s) of read-ahead beyond the end of the file",
                            session.past_end_bytes()));
        }
        context.stage = MetadataStage::AfterUnpack;
        context.open_raw_width = opened_metadata->geometry.raw_width;
        context.open_raw_height = opened_metadata->geometry.raw_height;
        auto metadata = detail::extract_metadata(raw, context);
        if (!metadata) {
            return Unexpected(std::move(metadata.error()));
        }
        if (auto decodable = require_decodable(*metadata); !decodable) {
            return Unexpected(std::move(decodable.error()));
        }
        if (auto levels = check_levels(metadata->levels); !levels) {
            return Unexpected(std::move(levels.error()));
        }
        auto& warnings = metadata->decoder.warnings;
        warnings.insert(warnings.end(), open_warnings.begin(), open_warnings.end());
        warnings.insert(warnings.end(), unpack_warnings.begin(), unpack_warnings.end());
        if (metadata->representation.kind != opened_metadata->representation.kind ||
            metadata->representation.sample_encoding != opened_metadata->representation.sample_encoding) {
            metadata->decoder.warnings.push_back(std::format(
                "the representation predicted after opening ({} {}) differs from the unpacked data ({} {})",
                to_string(opened_metadata->representation.kind),
                to_string(opened_metadata->representation.sample_encoding),
                to_string(metadata->representation.kind),
                to_string(metadata->representation.sample_encoding)));
        }

        auto image = copy_image(raw, *metadata, options.limits, cancel);
        if (!image) {
            return Unexpected(std::move(image.error()));
        }
        if (auto unchanged = verify_unchanged(*source, path); !unchanged) {
            return Unexpected(std::move(unchanged.error()));
        }
        DecodedRaw decoded;
        decoded.metadata = std::move(*metadata);
        decoded.image = std::move(*image);
        if (auto consistent = check_consistency(decoded); !consistent) {
            return Unexpected(std::move(consistent.error()));
        }
        return decoded;
    }
};

}  // namespace

Expected<std::unique_ptr<RawDecoder>> create_libraw_decoder() {
    try {
        return std::unique_ptr<RawDecoder>(std::make_unique<LibRawDecoder>());
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the LibRaw adapter", std::string(kWhereAdapter));
    }
}

LibRawBuildInfo libraw_build_info() {
    LibRawBuildInfo info;
    info.version = LibRaw::version();
    info.version_number = static_cast<std::uint32_t>(LibRaw::versionNumber());
    info.capabilities = detail::capability_names(LibRaw::capabilities());
    return info;
}

}  // namespace lucent::raw
