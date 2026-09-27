// Embedded camera previews (embedded_preview.hpp) through LibRaw. Flow of one call:
//   open the file (ReadOnlyFile) -> capture identity -> enforce the file-size limit
//   -> LibRaw::open_datastream over a LibRawFileStream (identify only; nothing is unpacked)
//   -> classify every entry of LibRaw's thumbnail list, reading each JPEG's header through the
//      same handle -> rank the usable ones (largest first, or smallest large enough for the
//      requested size) -> unpack_thumb_ex() the first (falling back to the next on failure)
//      -> verify the file identity is unchanged.

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
#include <vector>

#include <lucent/core/file.hpp>
#include <lucent/raw/embedded_preview.hpp>
#include <lucent/raw/libraw_decoder.hpp>

#include "jpeg_header.hpp"
#include "libraw_stream.hpp"
#include "libraw_support.hpp"

namespace lucent::raw {

namespace {

constexpr std::string_view kWherePreview = "raw.preview";
constexpr std::string_view kWhereLimits = "raw.preview.limits";
constexpr std::string_view kWhereSource = "raw.preview.source";
constexpr std::string_view kWhereCopy = "raw.preview.copy";
constexpr std::string_view kStageUnpackThumb = "unpack_thumb";

// Header prefixes read to find a JPEG frame header: the first covers every camera preview seen
// (APP segments before SOF are at most a few tens of kilobytes); the second covers several large
// APP segments; the last resort is the whole candidate (bounded by max_bytes).
constexpr std::array<std::uint64_t, 2> kHeaderPrefixBytes = {std::uint64_t{64} << 10, std::uint64_t{1} << 20};

std::string where(std::string_view suffix) {
    return std::format("{}.{}", kWherePreview, suffix);
}

std::string_view internal_format_name(int format) noexcept {
    switch (format) {
        case LIBRAW_INTERNAL_THUMBNAIL_UNKNOWN:
            return "unknown";
        case LIBRAW_INTERNAL_THUMBNAIL_KODAK_THUMB:
        case LIBRAW_INTERNAL_THUMBNAIL_KODAK_YCBCR:
        case LIBRAW_INTERNAL_THUMBNAIL_KODAK_RGB:
            return "Kodak bitmap";
        case LIBRAW_INTERNAL_THUMBNAIL_JPEG:
            return "JPEG";
        case LIBRAW_INTERNAL_THUMBNAIL_LAYER:
            return "planar bitmap";
        case LIBRAW_INTERNAL_THUMBNAIL_ROLLEI:
            return "Rollei bitmap";
        case LIBRAW_INTERNAL_THUMBNAIL_PPM:
            return "8-bit bitmap";
        case LIBRAW_INTERNAL_THUMBNAIL_PPM16:
            return "16-bit bitmap";
        case LIBRAW_INTERNAL_THUMBNAIL_X3F:
            return "Sigma X3F bitmap";
        case LIBRAW_INTERNAL_THUMBNAIL_DNG_YCBCR:
            return "DNG YCbCr bitmap";
        case LIBRAW_INTERNAL_THUMBNAIL_JPEGXL:
            return "JPEG XL";
        default:
            return "unrecognised";
    }
}

// ---------------------------------------------------------------------------------------------
// Source file
// ---------------------------------------------------------------------------------------------

struct OpenedSource {
    core::ReadOnlyFile file;
    core::FileIdentity identity;
};

Expected<OpenedSource> open_source(const std::filesystem::path& path, std::uint64_t max_file_bytes) {
    auto file = core::ReadOnlyFile::open(path, core::AccessHint::Random);
    if (!file) {
        return Unexpected(std::move(file.error()));
    }
    auto identity = file->identity();
    if (!identity) {
        return Unexpected(std::move(identity.error()));
    }
    if (identity->size_bytes > max_file_bytes) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("file has {} bytes; the limit is {}", identity->size_bytes, max_file_bytes),
                    std::string(kWhereLimits));
    }
    if (identity->size_bytes == 0) {
        return fail(ErrorCode::Unsupported, "the file is empty", where("open"));
    }
    OpenedSource source;
    source.identity = *identity;
    source.file = std::move(*file);
    return source;
}

// SourceChanged unless the file object and its content version are exactly as captured when the
// call started, through the handle and through the path (the rule of the LibRaw adapter and
// core::hash_file).
Status verify_unchanged(const OpenedSource& source, const std::filesystem::path& path) {
    const std::string at(kWhereSource);
    auto after = source.file.identity();
    if (!after) {
        return Unexpected(std::move(after.error()));
    }
    if (*after != source.identity) {
        return fail(ErrorCode::SourceChanged, "the file was modified while its preview was being read", at);
    }
    auto deleted = source.file.is_delete_pending();
    if (!deleted) {
        return Unexpected(std::move(deleted.error()));
    }
    if (*deleted) {
        return fail(ErrorCode::SourceChanged, "the file was deleted while its preview was being read", at);
    }
    auto at_path = core::stat_file(path);
    if (!at_path) {
        if (at_path.error().code == ErrorCode::NotFound) {
            return fail(ErrorCode::SourceChanged,
                        "the file was moved or deleted while its preview was being read", at);
        }
        return Unexpected(std::move(at_path.error()));
    }
    if (*at_path != source.identity) {
        return fail(ErrorCode::SourceChanged, "the path now names a different or modified file", at);
    }
    return ok();
}

// ---------------------------------------------------------------------------------------------
// Candidates
// ---------------------------------------------------------------------------------------------

struct Classified {
    PreviewCandidate candidate;
    detail::JpegFrame frame;  // valid when candidate.problem is empty
};

void set_problem(PreviewCandidate& candidate, PreviewProblem problem, std::string detail) {
    candidate.problem = problem;
    candidate.detail = std::move(detail);
}

// Reads the header of the JPEG candidate at [offset, offset + length) through the open handle and
// scans it for the frame header, reading longer prefixes only when the header needs them.
Expected<detail::JpegScan> scan_candidate_header(const core::ReadOnlyFile& file, std::uint64_t offset,
                                                 std::uint64_t length,
                                                 const core::CancellationToken& cancel) {
    std::vector<std::byte> prefix;
    std::uint64_t have = 0;
    std::size_t step = 0;
    for (;;) {
        if (auto status = cancel.check(where("scan")); !status) {
            return Unexpected(std::move(status.error()));
        }
        const std::uint64_t want =
            step < kHeaderPrefixBytes.size() ? std::min(length, kHeaderPrefixBytes[step]) : length;
        ++step;
        if (want <= have) {
            continue;  // this prefix size is not larger than what was read already
        }
        prefix.resize(static_cast<std::size_t>(want));
        auto read = file.read_exact_at(
            offset + have, std::span<std::byte>(prefix.data() + have, static_cast<std::size_t>(want - have)));
        if (!read) {
            return Unexpected(std::move(read.error()));
        }
        have = want;
        const bool complete = have == length;
        detail::JpegScan scan = detail::scan_jpeg_header(prefix, complete);
        if (scan.status != detail::JpegScanStatus::NeedMoreData || complete) {
            return scan;
        }
    }
}

Expected<Classified> classify_candidate(const libraw_thumbnail_item_t& item, std::uint32_t index,
                                        const OpenedSource& source, const EmbeddedPreviewLimits& limits,
                                        const core::CancellationToken& cancel) {
    Classified classified;
    PreviewCandidate& c = classified.candidate;
    c.list_index = index;
    c.file_offset = item.toffset < 0 ? 0 : static_cast<std::uint64_t>(item.toffset);
    c.byte_length = item.tlength;
    c.width = item.twidth;
    c.height = item.theight;

    if (item.tformat != LIBRAW_INTERNAL_THUMBNAIL_JPEG) {
        set_problem(
            c, PreviewProblem::NotJpeg,
            std::format("{} ({})", internal_format_name(item.tformat), static_cast<int>(item.tformat)));
        return classified;
    }
    const std::uint64_t size = source.identity.size_bytes;
    if (item.toffset < 0 || item.tlength == 0 || c.file_offset > size ||
        c.byte_length > size - c.file_offset) {
        set_problem(c, PreviewProblem::OutsideFile,
                    std::format("declared bytes {}..{} lie outside the {}-byte file", item.toffset,
                                static_cast<std::int64_t>(item.toffset) + item.tlength, size));
        return classified;
    }
    if (c.byte_length > limits.max_bytes) {
        set_problem(c, PreviewProblem::TooLarge,
                    std::format("{} bytes; the limit is {}", c.byte_length, limits.max_bytes));
        return classified;
    }
    auto scan = scan_candidate_header(source.file, c.file_offset, c.byte_length, cancel);
    if (!scan) {
        return Unexpected(std::move(scan.error()));
    }
    if (scan->status != detail::JpegScanStatus::Frame) {
        set_problem(c, PreviewProblem::InvalidJpeg, scan->message);
        return classified;
    }
    const detail::JpegFrame& frame = scan->frame;
    c.width = frame.width;
    c.height = frame.height;
    if (const std::string problem = detail::displayable_jpeg_problem(frame); !problem.empty()) {
        set_problem(c, PreviewProblem::UnsupportedJpeg, problem);
        return classified;
    }
    const std::uint64_t pixels = std::uint64_t{frame.width} * frame.height;
    if (pixels > limits.max_pixels) {
        set_problem(
            c, PreviewProblem::TooLarge,
            std::format("{} x {} pixels; the limit is {}", frame.width, frame.height, limits.max_pixels));
        return classified;
    }
    classified.frame = frame;
    return classified;
}

std::uint64_t pixel_count(const PreviewCandidate& c) {
    return std::uint64_t(std::max(c.width, 0)) * std::uint64_t(std::max(c.height, 0));
}

std::int32_t long_edge(const PreviewCandidate& c) {
    return std::max(c.width, c.height);
}

// Long edge / short edge; 0 for a candidate without dimensions.
double aspect(const PreviewCandidate& c) {
    const std::int32_t short_edge = std::min(c.width, c.height);
    return short_edge > 0 ? double(long_edge(c)) / double(short_edge) : 0.0;
}

// Usable candidates in the order they are tried (embedded_preview.hpp, "Selection"): largest
// first (most pixels, then most bytes, then earlier list position); with a requested size, the
// candidates that qualify for it come first, smallest first (fewest pixels, then most bytes, then
// earlier list position).
std::vector<std::size_t> ranking(const std::vector<Classified>& classified, std::int32_t min_long_edge) {
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < classified.size(); ++i) {
        if (!classified[i].candidate.problem) {
            order.push_back(i);
        }
    }
    const auto more_bytes = [&](std::size_t a, std::size_t b) {
        return classified[a].candidate.byte_length > classified[b].candidate.byte_length;
    };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const std::uint64_t pa = pixel_count(classified[a].candidate);
        const std::uint64_t pb = pixel_count(classified[b].candidate);
        return pa != pb ? pa > pb : more_bytes(a, b);
    });
    if (min_long_edge <= 0 || order.empty()) {
        return order;
    }
    const double reference = aspect(classified[order.front()].candidate);
    const auto qualifies = [&](std::size_t i) {
        const PreviewCandidate& c = classified[i].candidate;
        const double a = aspect(c);
        return long_edge(c) >= min_long_edge && reference > 0.0 && a > 0.0 &&
               std::abs(a / reference - 1.0) <= EmbeddedPreviewOptions::kAspectTolerance;
    };
    std::vector<std::size_t> preferred;
    std::vector<std::size_t> rest;
    for (const std::size_t i : order) {
        (qualifies(i) ? preferred : rest).push_back(i);
    }
    // Back to list order, then smallest first: stable, so equal candidates keep list order.
    std::sort(preferred.begin(), preferred.end());
    std::stable_sort(preferred.begin(), preferred.end(), [&](std::size_t a, std::size_t b) {
        const std::uint64_t pa = pixel_count(classified[a].candidate);
        const std::uint64_t pb = pixel_count(classified[b].candidate);
        return pa != pb ? pa < pb : more_bytes(a, b);
    });
    preferred.insert(preferred.end(), rest.begin(), rest.end());
    return preferred;
}

// ---------------------------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------------------------

class Session {
public:
    Session(const OpenedSource& source, core::CancellationToken cancel)
        : stream_(
              std::make_unique<detail::LibRawFileStream>(source.file, source.identity.size_bytes, cancel)),
          raw_(std::make_unique<LibRaw>(LIBRAW_OPTIONS_NO_DATAERR_CALLBACK)) {
        state_.cancel = std::move(cancel);
        stream_->set_owner(raw_.get());
        detail::install_callbacks(*raw_, state_);
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() = default;  // raw_ (declared last) is destroyed before the stream it reads

    [[nodiscard]] LibRaw& raw() noexcept { return *raw_; }

    // Identifies the file (LibRaw::open_datastream): metadata and the thumbnail list, no pixels.
    [[nodiscard]] Status open() { return run(detail::kStageOpen, raw_->open_datastream(stream_.get())); }

    // Ok, or the refined error of a LibRaw call at `stage` (see libraw_decoder.hpp).
    [[nodiscard]] Status run(std::string_view stage, int code) {
        if (code != LIBRAW_SUCCESS) {
            return Unexpected(detail::libraw_failure(code, stage, *stream_, state_));
        }
        if (state_.cancel_observed || stream_->cancel_observed()) {
            return fail(ErrorCode::Cancelled, std::format("{} was cancelled", stage),
                        std::format("raw.libraw.{}", stage));
        }
        if (const auto& read_error = stream_->read_error(); read_error) {
            Error error = *read_error;
            error.message = std::format("{} failed reading the file: {}", stage, error.message);
            return Unexpected(std::move(error));
        }
        return ok();
    }

private:
    detail::LibRawCallState state_;
    std::unique_ptr<detail::LibRawFileStream> stream_;
    std::unique_ptr<LibRaw> raw_;
};

// unpack_thumb_ex() for one classified candidate and a copy of its bytes. std::nullopt with the
// candidate's problem set when LibRaw refuses or returns something other than the stream that was
// classified; an error only for reading, cancellation and allocation failures.
Expected<std::optional<EmbeddedPreview>> unpack_candidate(Session& session, Classified& chosen,
                                                          const core::CancellationToken& cancel) {
    if (auto status = cancel.check(where("unpack")); !status) {
        return Unexpected(std::move(status.error()));
    }
    PreviewCandidate& c = chosen.candidate;
    LibRaw& raw = session.raw();
    const int code = raw.unpack_thumb_ex(static_cast<int>(c.list_index));
    if (auto status = session.run(kStageUnpackThumb, code); !status) {
        const ErrorCode error = status.error().code;
        if (error == ErrorCode::Cancelled || error == ErrorCode::IoError ||
            error == ErrorCode::SourceChanged || error == ErrorCode::OutOfMemory ||
            error == ErrorCode::PermissionDenied) {
            return Unexpected(std::move(status.error()));
        }
        set_problem(c, PreviewProblem::InvalidJpeg,
                    std::format("the decoder refused it: {}", status.error().message));
        return std::optional<EmbeddedPreview>{};
    }
    const libraw_thumbnail_t& thumb = raw.imgdata.thumbnail;
    if (thumb.tformat != LIBRAW_THUMBNAIL_JPEG) {
        set_problem(
            c, PreviewProblem::NotJpeg,
            std::format("the decoder delivered format {} instead of JPEG", static_cast<int>(thumb.tformat)));
        return std::optional<EmbeddedPreview>{};
    }
    if (thumb.thumb == nullptr || thumb.tlength != c.byte_length) {
        set_problem(
            c, PreviewProblem::InvalidJpeg,
            std::format("the decoder delivered {} bytes instead of {}", thumb.tlength, c.byte_length));
        return std::optional<EmbeddedPreview>{};
    }

    EmbeddedPreview preview;
    preview.bytes.resize(thumb.tlength);
    std::memcpy(preview.bytes.data(), thumb.thumb, thumb.tlength);
    // The copy must still be the stream that was classified (LibRaw rewrites the first two bytes
    // as SOI; the classification already required them to be SOI). A difference means the file
    // changed in between, which the final identity check reports; the candidate is not used.
    const detail::JpegScan scan = detail::scan_jpeg_header(preview.bytes, true);
    if (scan.status != detail::JpegScanStatus::Frame || scan.frame.width != chosen.frame.width ||
        scan.frame.height != chosen.frame.height || scan.frame.components != chosen.frame.components) {
        set_problem(c, PreviewProblem::InvalidJpeg,
                    std::format("the decoder's copy differs from the header read directly ({})", kWhereCopy));
        return std::optional<EmbeddedPreview>{};
    }
    preview.format = EmbeddedPreviewFormat::Jpeg;
    preview.width = chosen.frame.width;
    preview.height = chosen.frame.height;
    preview.components = chosen.frame.components;
    preview.list_index = c.list_index;
    preview.file_offset = c.file_offset;
    return std::optional<EmbeddedPreview>(std::move(preview));
}

Expected<EmbeddedPreviewResult> extract_impl(const std::filesystem::path& path,
                                             const EmbeddedPreviewOptions& options,
                                             const core::CancellationToken& cancel) {
    if (auto status = cancel.check(std::string(kWherePreview)); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto valid = options.limits.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    if (options.max_file_bytes == 0) {
        return fail(ErrorCode::InvalidArgument, "max_file_bytes must be at least 1",
                    std::string(kWhereLimits));
    }
    if (options.min_long_edge < 0) {
        return fail(ErrorCode::InvalidArgument, "min_long_edge must not be negative", std::string(kWhereLimits));
    }
    auto source = open_source(path, options.max_file_bytes);
    if (!source) {
        return Unexpected(std::move(source.error()));
    }
    Session session(*source, cancel);
    if (auto opened = session.open(); !opened) {
        return Unexpected(std::move(opened.error()));
    }
    LibRaw& raw = session.raw();

    EmbeddedPreviewResult result;
    result.decoder = std::string(kLibRawDecoderId);
    result.source_identity = source->identity;

    const libraw_thumbnail_list_t& list = raw.imgdata.thumbs_list;
    const int listed = std::clamp(list.thumbcount, 0, LIBRAW_THUMBNAIL_MAXCOUNT);
    std::vector<Classified> classified;
    classified.reserve(static_cast<std::size_t>(listed));
    bool any_listed = false;
    for (int i = 0; i < listed; ++i) {
        // LibRaw appends its "single thumbnail" to the list even when the file has none, as an
        // entry with offset and length 0 (unpack_thumb answers LIBRAW_NO_THUMBNAIL for it).
        if (list.thumblist[i].toffset == 0 && list.thumblist[i].tlength == 0) {
            continue;
        }
        any_listed = true;
        auto candidate = classify_candidate(list.thumblist[i], static_cast<std::uint32_t>(i), *source,
                                            options.limits, cancel);
        if (!candidate) {
            return Unexpected(std::move(candidate.error()));
        }
        classified.push_back(std::move(*candidate));
    }

    // LibRaw reports the flip after identification (0..7); an out-of-range value would be a
    // decoder defect, reported rather than guessed.
    auto orientation = image::from_libraw_flip(raw.imgdata.sizes.flip);
    if (!orientation) {
        return fail(
            ErrorCode::Internal,
            std::format("the decoder reported the invalid orientation code {}", raw.imgdata.sizes.flip),
            where("orientation"));
    }

    for (const std::size_t index : ranking(classified, options.min_long_edge)) {
        auto unpacked = unpack_candidate(session, classified[index], cancel);
        if (!unpacked) {
            return Unexpected(std::move(unpacked.error()));
        }
        if (*unpacked) {
            result.preview = std::move(**unpacked);
            result.preview->orientation = *orientation;
            break;
        }
    }
    for (Classified& c : classified) {
        result.candidates.push_back(std::move(c.candidate));
    }
    if (result.preview) {
        result.status = EmbeddedPreviewStatus::Found;
    } else {
        result.status = any_listed ? EmbeddedPreviewStatus::NoneUsable : EmbeddedPreviewStatus::NoneInFile;
    }
    if (auto unchanged = verify_unchanged(*source, path); !unchanged) {
        return Unexpected(std::move(unchanged.error()));
    }
    return result;
}

}  // namespace

std::string_view to_string(EmbeddedPreviewFormat format) noexcept {
    switch (format) {
        case EmbeddedPreviewFormat::Jpeg:
            return "jpeg";
    }
    return "unknown";
}

std::string_view to_string(PreviewProblem problem) noexcept {
    switch (problem) {
        case PreviewProblem::NotJpeg:
            return "not_jpeg";
        case PreviewProblem::OutsideFile:
            return "outside_file";
        case PreviewProblem::TooLarge:
            return "too_large";
        case PreviewProblem::InvalidJpeg:
            return "invalid_jpeg";
        case PreviewProblem::UnsupportedJpeg:
            return "unsupported_jpeg";
    }
    return "unknown";
}

std::string_view to_string(EmbeddedPreviewStatus status) noexcept {
    switch (status) {
        case EmbeddedPreviewStatus::Found:
            return "found";
        case EmbeddedPreviewStatus::NoneInFile:
            return "none_in_file";
        case EmbeddedPreviewStatus::NoneUsable:
            return "none_usable";
    }
    return "unknown";
}

Status EmbeddedPreviewLimits::validate() const {
    if (max_bytes == 0 || max_pixels == 0) {
        return fail(ErrorCode::InvalidArgument, "embedded preview limits must be at least 1",
                    std::string(kWhereLimits));
    }
    return ok();
}

Expected<EmbeddedPreviewResult> extract_embedded_preview(const std::filesystem::path& path,
                                                         const EmbeddedPreviewOptions& options,
                                                         core::CancellationToken cancel) {
    try {
        return extract_impl(path, options, cancel);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "extracting the embedded preview ran out of memory",
                    where("adapter"));
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::format("extracting the embedded preview raised: {}", e.what()),
                    where("adapter"));
    } catch (...) {
        return fail(ErrorCode::Internal, "extracting the embedded preview raised an unknown exception",
                    where("adapter"));
    }
}

}  // namespace lucent::raw
