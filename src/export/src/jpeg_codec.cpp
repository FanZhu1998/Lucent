// JPEG encoder (to memory), verification reader and in-memory decoder (thumbnails) on
// libjpeg-turbo's libjpeg API.
//
// Error model: libjpeg reports fatal errors through error_exit, which must not return. We
// longjmp back to a setjmp in run_compress / run_decompress_*. Those functions, and everything
// between them and libjpeg, hold only trivially destructible state (plain structs reached
// through a pointer; the cancellation token is only read through a pointer), so the jump never
// skips a C++ destructor. All C++ resource management (FILE*, vectors, errors) happens in the
// callers, outside the setjmp frames. The compressed stream lives in a malloc'ed buffer that the
// caller always frees (MemoryDestination), whether or not the encoder finished.

#include <algorithm>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>

// jpeglib.h needs FILE and size_t declared first.
#include <jpeglib.h>
#include <jerror.h>

#include "codecs.hpp"
#include "file_util.hpp"

namespace lucent::exporter::detail {
namespace {

#pragma warning(push)
#pragma warning(disable : 4324)  // padded: jmp_buf is 16-byte aligned on x64
struct ErrorManager {
    jpeg_error_mgr pub;  // must be first: libjpeg hands back a jpeg_error_mgr*
    std::jmp_buf jump;
    int code;                          // msg_code of the fatal error
    char message[JMSG_LENGTH_MAX];     // fatal error text
    char warning[JMSG_LENGTH_MAX];     // first warning text
};

ErrorManager* manager_of(j_common_ptr cinfo) noexcept {
    return reinterpret_cast<ErrorManager*>(cinfo->err);  // pub is the first member
}

[[noreturn]] void on_error_exit(j_common_ptr cinfo) {
    ErrorManager* err = manager_of(cinfo);
    err->code = cinfo->err->msg_code;
    (*cinfo->err->format_message)(cinfo, err->message);
    std::longjmp(err->jump, 1);
}

// Replaces libjpeg's default (printing to stderr): keep the first warning for diagnostics.
void on_output_message(j_common_ptr cinfo) {
    ErrorManager* err = manager_of(cinfo);
    if (err->warning[0] == '\0') {
        (*cinfo->err->format_message)(cinfo, err->warning);
    }
}

void install_error_manager(ErrorManager* err) noexcept {
    jpeg_std_error(&err->pub);
    err->pub.error_exit = on_error_exit;
    err->pub.output_message = on_output_message;
}

// Growable in-memory destination. `buffer` is malloc'ed by the caller before compression and
// freed by the caller afterwards in every case (libjpeg never frees it).
struct MemoryDestination {
    jpeg_destination_mgr pub;  // must be first: libjpeg hands back a jpeg_destination_mgr*
    JOCTET* buffer;
    std::size_t capacity;
    std::size_t size;  // bytes of the finished stream (set by term_destination)
};

MemoryDestination* destination_of(j_compress_ptr cinfo) noexcept {
    return reinterpret_cast<MemoryDestination*>(cinfo->dest);
}

void init_memory_destination(j_compress_ptr cinfo) {
    MemoryDestination* d = destination_of(cinfo);
    d->pub.next_output_byte = d->buffer;
    d->pub.free_in_buffer = d->capacity;
}

// Called when the whole buffer is full: double it (realloc keeps the old block on failure, so
// the caller can still free it).
boolean empty_memory_destination(j_compress_ptr cinfo) {
    MemoryDestination* d = destination_of(cinfo);
    const std::size_t used = d->capacity;
    if (used > std::numeric_limits<std::size_t>::max() / 2) {
        ERREXIT1(cinfo, JERR_OUT_OF_MEMORY, 1);
    }
    auto* grown = static_cast<JOCTET*>(std::realloc(d->buffer, used * 2));
    if (grown == nullptr) {
        ERREXIT1(cinfo, JERR_OUT_OF_MEMORY, 2);
    }
    d->buffer = grown;
    d->capacity = used * 2;
    d->pub.next_output_byte = grown + used;
    d->pub.free_in_buffer = d->capacity - used;
    return TRUE;
}

void term_memory_destination(j_compress_ptr cinfo) {
    MemoryDestination* d = destination_of(cinfo);
    d->size = d->capacity - d->pub.free_in_buffer;
}

struct CompressJob {
    jpeg_compress_struct cinfo;
    ErrorManager err;
    MemoryDestination dest;
    const core::CancellationToken* cancel;
    bool cancelled;
    const std::uint8_t* samples;
    std::size_t row_samples;
    JDIMENSION width;
    JDIMENSION height;
    int quality;
    bool full_chroma;
    const JOCTET* icc;
    unsigned int icc_size;
};

struct DecompressJob {
    jpeg_decompress_struct cinfo;
    ErrorManager err;
    std::FILE* file;
    const core::CancellationToken* cancel;
    bool cancelled;
    JOCTET* icc;  // malloc'ed by jpeg_read_icc_profile; the caller frees it
    unsigned int icc_size;
    std::uint8_t* samples;  // provided by the caller between the two phases
    std::size_t row_samples;
    std::size_t row_stride;  // samples between rows in `samples`; 0 reuses one row buffer
};

// Decoding a stream held in memory (decode_jpeg_stream): header, then start with a DCT scale,
// then the rows.
struct MemoryDecompressJob {
    jpeg_decompress_struct cinfo;
    ErrorManager err;
    const unsigned char* data;
    unsigned long data_size;
    unsigned int scale_denom;
    const core::CancellationToken* cancel;
    bool cancelled;
    JOCTET* icc;  // malloc'ed by jpeg_read_icc_profile; the caller frees it
    unsigned int icc_size;
    std::uint8_t* samples;
    std::size_t row_stride;
};
#pragma warning(pop)  // C4324

#pragma warning(push)
// C4611: "interaction between '_setjmp' and C++ object destruction is non-portable". The setjmp
// frames below contain no objects with non-trivial destructors (see the file comment).
#pragma warning(disable : 4611)

bool run_compress(CompressJob* job) {
    job->cinfo.err = &job->err.pub;
    install_error_manager(&job->err);
    if (setjmp(job->err.jump) != 0) {
        jpeg_destroy_compress(&job->cinfo);
        return false;
    }
    jpeg_create_compress(&job->cinfo);
    job->dest.pub.init_destination = init_memory_destination;
    job->dest.pub.empty_output_buffer = empty_memory_destination;
    job->dest.pub.term_destination = term_memory_destination;
    job->cinfo.dest = &job->dest.pub;
    job->cinfo.image_width = job->width;
    job->cinfo.image_height = job->height;
    job->cinfo.input_components = 3;
    job->cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&job->cinfo);
    jpeg_set_quality(&job->cinfo, job->quality, TRUE);
    job->cinfo.optimize_coding = TRUE;
    job->cinfo.dct_method = JDCT_ISLOW;
    // jpeg_set_defaults selects 2x2 luma sampling (4:2:0); chroma components are 1x1.
    const int luma_samp = job->full_chroma ? 1 : 2;
    job->cinfo.comp_info[0].h_samp_factor = luma_samp;
    job->cinfo.comp_info[0].v_samp_factor = luma_samp;
    jpeg_start_compress(&job->cinfo, TRUE);
    if (job->icc != nullptr && job->icc_size > 0) {
        jpeg_write_icc_profile(&job->cinfo, job->icc, job->icc_size);
    }
    while (job->cinfo.next_scanline < job->cinfo.image_height) {
        if (job->cinfo.next_scanline % static_cast<JDIMENSION>(kCodecCancelRows) == 0 &&
            job->cancel->is_cancelled()) {
            job->cancelled = true;
            jpeg_destroy_compress(&job->cinfo);  // aborts the unfinished compression
            return false;
        }
        JSAMPROW row = const_cast<JSAMPROW>(job->samples + job->row_samples * job->cinfo.next_scanline);
        jpeg_write_scanlines(&job->cinfo, &row, 1);
    }
    jpeg_finish_compress(&job->cinfo);
    jpeg_destroy_compress(&job->cinfo);
    return true;
}

// Phase 1: header, ICC profile and decompressor start-up. On success the decompressor stays
// alive for run_decompress_body() or abort_decompress().
bool run_decompress_header(DecompressJob* job) {
    job->cinfo.err = &job->err.pub;
    install_error_manager(&job->err);
    if (setjmp(job->err.jump) != 0) {
        jpeg_destroy_decompress(&job->cinfo);
        std::free(job->icc);
        job->icc = nullptr;
        job->icc_size = 0;
        return false;
    }
    jpeg_create_decompress(&job->cinfo);
    jpeg_stdio_src(&job->cinfo, job->file);
    jpeg_save_markers(&job->cinfo, JPEG_APP0 + 2, 0xFFFF);
    jpeg_read_header(&job->cinfo, TRUE);
    if (!jpeg_read_icc_profile(&job->cinfo, &job->icc, &job->icc_size)) {
        job->icc = nullptr;
        job->icc_size = 0;
    }
    job->cinfo.out_color_space = JCS_RGB;
    job->cinfo.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&job->cinfo);
    return true;
}

// Header and ICC profile of a stream in memory. On success the decompressor stays alive for
// run_memory_start() or abort; on failure it is destroyed and the profile freed.
bool run_memory_header(MemoryDecompressJob* job) {
    job->cinfo.err = &job->err.pub;
    install_error_manager(&job->err);
    if (setjmp(job->err.jump) != 0) {
        jpeg_destroy_decompress(&job->cinfo);
        std::free(job->icc);
        job->icc = nullptr;
        job->icc_size = 0;
        return false;
    }
    jpeg_create_decompress(&job->cinfo);
    jpeg_mem_src(&job->cinfo, job->data, job->data_size);
    jpeg_save_markers(&job->cinfo, JPEG_APP0 + 2, 0xFFFF);
    jpeg_read_header(&job->cinfo, TRUE);
    if (!jpeg_read_icc_profile(&job->cinfo, &job->icc, &job->icc_size)) {
        job->icc = nullptr;
        job->icc_size = 0;
    }
    return true;
}

// Starts decompression to RGB at the job's DCT scale. On failure the decompressor is destroyed
// (the profile stays for the caller to free).
bool run_memory_start(MemoryDecompressJob* job) {
    if (setjmp(job->err.jump) != 0) {
        jpeg_destroy_decompress(&job->cinfo);
        return false;
    }
    job->cinfo.out_color_space = JCS_RGB;
    job->cinfo.scale_num = 1;
    job->cinfo.scale_denom = job->scale_denom;
    job->cinfo.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&job->cinfo);
    return true;
}

// Phase 2: all scanlines into job->samples, then finish and destroy.
bool run_decompress_body(DecompressJob* job) {
    if (setjmp(job->err.jump) != 0) {
        jpeg_destroy_decompress(&job->cinfo);
        return false;
    }
    while (job->cinfo.output_scanline < job->cinfo.output_height) {
        if (job->cinfo.output_scanline % static_cast<JDIMENSION>(kCodecCancelRows) == 0 &&
            job->cancel->is_cancelled()) {
            job->cancelled = true;
            jpeg_destroy_decompress(&job->cinfo);
            return false;
        }
        JSAMPROW row = job->samples + job->row_stride * job->cinfo.output_scanline;
        jpeg_read_scanlines(&job->cinfo, &row, 1);
    }
    jpeg_finish_decompress(&job->cinfo);
    jpeg_destroy_decompress(&job->cinfo);
    return true;
}

// All scanlines of a stream in memory into job->samples, then finish and destroy.
bool run_memory_rows(MemoryDecompressJob* job) {
    if (setjmp(job->err.jump) != 0) {
        jpeg_destroy_decompress(&job->cinfo);
        return false;
    }
    while (job->cinfo.output_scanline < job->cinfo.output_height) {
        if (job->cinfo.output_scanline % static_cast<JDIMENSION>(kCodecCancelRows) == 0 &&
            job->cancel->is_cancelled()) {
            job->cancelled = true;
            jpeg_destroy_decompress(&job->cinfo);
            return false;
        }
        JSAMPROW row = job->samples + job->row_stride * job->cinfo.output_scanline;
        jpeg_read_scanlines(&job->cinfo, &row, 1);
    }
    jpeg_finish_decompress(&job->cinfo);
    jpeg_destroy_decompress(&job->cinfo);
    return true;
}

#pragma warning(pop)

template <class Job>
void abort_decompress(Job* job) noexcept {
    jpeg_destroy_decompress(&job->cinfo);  // releases everything; cannot fail
}

// Closes a FILE* on scope exit (read path).
struct FileCloser {
    std::FILE* file = nullptr;
    ~FileCloser() {
        if (file != nullptr) {
            std::fclose(file);
        }
    }
};

// Frees the compressed stream on scope exit.
struct BufferFree {
    void operator()(JOCTET* p) const noexcept { std::free(p); }
};

void reset_crt_errors() noexcept {
    _set_doserrno(0);
    _set_errno(0);
}

}  // namespace

Expected<std::vector<std::uint8_t>> encode_jpeg(const OutputImage& image, const JpegSettings& settings,
                                                std::span<const std::uint8_t> icc,
                                                const core::CancellationToken& cancel) {
    constexpr const char* kWhere = "export.jpeg.encode";
    if (image.bit_depth != BitDepth::Eight) {
        return fail(ErrorCode::InvalidArgument, "JPEG output requires 8-bit samples", kWhere);
    }
    if (image.width > kJpegMaxDimension || image.height > kJpegMaxDimension) {
        return fail(ErrorCode::LimitExceeded, "JPEG sides are limited to " + std::to_string(kJpegMaxDimension) +
                                                  " pixels",
                    kWhere);
    }
    if (auto status = cancel.check(kWhere); !status) {
        return Unexpected(std::move(status.error()));
    }

    // Heap-allocated: jpeg_compress_struct plus two message buffers are a few KB.
    auto job = std::make_unique<CompressJob>();
    std::memset(job.get(), 0, sizeof(CompressJob));
    job->cancel = &cancel;
    job->samples = image.samples8.data();
    job->row_samples = image.row_samples();
    job->width = static_cast<JDIMENSION>(image.width);
    job->height = static_cast<JDIMENSION>(image.height);
    job->quality = settings.quality;
    job->full_chroma = jpeg_chroma_subsampling(settings.quality) == ChromaSubsampling::Yuv444;
    job->icc = icc.data();
    job->icc_size = static_cast<unsigned int>(icc.size());

    // A quarter of the raw size plus the profile covers typical q95 photographs without growing.
    job->dest.capacity = std::max<std::size_t>(image.samples8.size() / 4 + icc.size() + 65536, 65536);
    job->dest.buffer = static_cast<JOCTET*>(std::malloc(job->dest.capacity));
    if (job->dest.buffer == nullptr) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the JPEG output buffer", kWhere);
    }
    const bool encoded = run_compress(job.get());
    std::unique_ptr<JOCTET, BufferFree> stream(job->dest.buffer);
    if (job->cancelled) {
        return Unexpected(cancel.check(kWhere).error());
    }
    if (!encoded) {
        return fail(job->err.code == JERR_OUT_OF_MEMORY ? ErrorCode::OutOfMemory : ErrorCode::Internal,
                    std::string("libjpeg: ") + job->err.message, kWhere, job->err.code);
    }
    try {
        return std::vector<std::uint8_t>(stream.get(), stream.get() + job->dest.size);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot copy the JPEG stream", kWhere);
    }
}

Expected<DecodedFile> read_jpeg(const std::filesystem::path& path, const core::CancellationToken& cancel,
                                bool keep_samples) {
    constexpr const char* kWhere = "export.jpeg.verify";
    auto native = api_path(path, kWhere);
    if (!native) {
        return Unexpected(std::move(native.error()));
    }
    auto job = std::make_unique<DecompressJob>();
    std::memset(job.get(), 0, sizeof(DecompressJob));
    job->cancel = &cancel;

    reset_crt_errors();
    FileCloser closer;
    if (_wfopen_s(&closer.file, native->c_str(), L"rb") != 0 || closer.file == nullptr) {
        return Unexpected(crt_file_error(kWhere, "cannot reopen the written JPEG file"));
    }
    job->file = closer.file;
    if (!run_decompress_header(job.get())) {
        return fail(ErrorCode::Corrupt, std::string("written JPEG does not decode: ") + job->err.message, kWhere,
                    job->err.code);
    }

    DecodedFile decoded;
    std::vector<std::uint8_t> row;
    try {
        if (job->icc != nullptr) {
            decoded.icc.assign(job->icc, job->icc + job->icc_size);
        }
        std::free(job->icc);
        job->icc = nullptr;
        decoded.width = static_cast<std::int32_t>(job->cinfo.output_width);
        decoded.height = static_cast<std::int32_t>(job->cinfo.output_height);
        decoded.channels = job->cinfo.output_components;
        decoded.bits_per_sample = job->cinfo.data_precision;
        decoded.jpeg_ycbcr = job->cinfo.jpeg_color_space == JCS_YCbCr;
        if (job->cinfo.num_components > 0) {
            decoded.jpeg_luma_h_samp = job->cinfo.comp_info[0].h_samp_factor;
            decoded.jpeg_luma_v_samp = job->cinfo.comp_info[0].v_samp_factor;
        }
        job->row_samples = static_cast<std::size_t>(job->cinfo.output_width) *
                           static_cast<std::size_t>(job->cinfo.output_components);
        if (keep_samples) {
            decoded.samples8.resize(job->row_samples * job->cinfo.output_height);
            job->samples = decoded.samples8.data();
            job->row_stride = job->row_samples;
        } else {
            row.resize(job->row_samples);
            job->samples = row.data();
            job->row_stride = 0;
        }
    } catch (const std::bad_alloc&) {
        std::free(job->icc);
        abort_decompress(job.get());
        return fail(ErrorCode::OutOfMemory, "cannot allocate the JPEG verification buffer", kWhere);
    }
    if (!run_decompress_body(job.get())) {
        if (job->cancelled) {
            return Unexpected(cancel.check(kWhere).error());
        }
        return fail(ErrorCode::Corrupt, std::string("written JPEG does not decode: ") + job->err.message, kWhere,
                    job->err.code);
    }
    decoded.warnings = static_cast<std::int32_t>(job->err.pub.num_warnings);
    decoded.first_warning = job->err.warning;
    return decoded;
}

namespace {

// A heap job over `jpeg` with its header read, or the error (Corrupt / InvalidArgument).
Expected<std::unique_ptr<MemoryDecompressJob>> open_memory_job(std::span<const std::uint8_t> jpeg,
                                                               const core::CancellationToken& cancel,
                                                               const char* where) {
    if (jpeg.empty() || jpeg.size() > std::numeric_limits<unsigned long>::max()) {
        return fail(ErrorCode::InvalidArgument, "the JPEG stream is empty or too large", where);
    }
    auto job = std::make_unique<MemoryDecompressJob>();
    std::memset(job.get(), 0, sizeof(MemoryDecompressJob));
    job->data = jpeg.data();
    job->data_size = static_cast<unsigned long>(jpeg.size());
    job->cancel = &cancel;
    job->scale_denom = 1;
    if (!run_memory_header(job.get())) {
        return fail(ErrorCode::Corrupt, std::string("the JPEG stream does not decode: ") + job->err.message,
                    where, job->err.code);
    }
    return job;
}

}  // namespace

Expected<std::pair<std::int32_t, std::int32_t>> read_jpeg_stream_size(std::span<const std::uint8_t> jpeg) {
    constexpr const char* kWhere = "export.jpeg.decode";
    try {
        const core::CancellationToken never;
        auto job = open_memory_job(jpeg, never, kWhere);
        if (!job) {
            return Unexpected(std::move(job.error()));
        }
        std::free((*job)->icc);
        const auto width =
            static_cast<std::int32_t>(std::min<JDIMENSION>((*job)->cinfo.image_width, INT32_MAX));
        const auto height =
            static_cast<std::int32_t>(std::min<JDIMENSION>((*job)->cinfo.image_height, INT32_MAX));
        abort_decompress(job->get());
        return std::pair{width, height};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the JPEG decoder", kWhere);
    }
}

Expected<DecodedJpegStream> decode_jpeg_stream(std::span<const std::uint8_t> jpeg, std::int32_t scale_denom,
                                               const core::CancellationToken& cancel) {
    constexpr const char* kWhere = "export.jpeg.decode";
    if (scale_denom != 1 && scale_denom != 2 && scale_denom != 4 && scale_denom != 8) {
        return fail(ErrorCode::InvalidArgument, "the DCT scale must be 1/1, 1/2, 1/4 or 1/8", kWhere);
    }
    if (auto status = cancel.check(kWhere); !status) {
        return Unexpected(std::move(status.error()));
    }
    std::unique_ptr<MemoryDecompressJob> job;
    try {
        auto opened = open_memory_job(jpeg, cancel, kWhere);
        if (!opened) {
            return Unexpected(std::move(opened.error()));
        }
        job = std::move(*opened);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the JPEG decoder", kWhere);
    }
    // From here on the profile belongs to this function.
    std::unique_ptr<JOCTET, BufferFree> icc(job->icc);
    job->icc = nullptr;
    const J_COLOR_SPACE space = job->cinfo.jpeg_color_space;
    if (space != JCS_GRAYSCALE && space != JCS_RGB && space != JCS_YCbCr) {
        abort_decompress(job.get());
        return fail(ErrorCode::Unsupported, "the JPEG stream is not gray, RGB or YCbCr (CMYK?)", kWhere);
    }
    if (job->cinfo.image_width > static_cast<JDIMENSION>(kJpegMaxDimension) ||
        job->cinfo.image_height > static_cast<JDIMENSION>(kJpegMaxDimension)) {
        abort_decompress(job.get());
        return fail(ErrorCode::LimitExceeded, "the JPEG stream is larger than the JPEG limits", kWhere);
    }
    job->scale_denom = static_cast<unsigned int>(scale_denom);
    if (!run_memory_start(job.get())) {
        return fail(ErrorCode::Corrupt, std::string("the JPEG stream does not decode: ") + job->err.message,
                    kWhere, job->err.code);
    }
    DecodedJpegStream decoded;
    try {
        if (icc) {
            decoded.icc.assign(icc.get(), icc.get() + job->icc_size);
        }
        decoded.width = static_cast<std::int32_t>(job->cinfo.output_width);
        decoded.height = static_cast<std::int32_t>(job->cinfo.output_height);
        if (job->cinfo.output_components != 3) {
            abort_decompress(job.get());
            return fail(ErrorCode::Internal, "libjpeg did not produce RGB", kWhere);
        }
        job->row_stride = static_cast<std::size_t>(job->cinfo.output_width) * 3;
        decoded.rgb.resize(job->row_stride * job->cinfo.output_height);
        job->samples = decoded.rgb.data();
    } catch (const std::bad_alloc&) {
        abort_decompress(job.get());
        return fail(ErrorCode::OutOfMemory, "cannot allocate the decoded JPEG", kWhere);
    }
    if (!run_memory_rows(job.get())) {
        if (job->cancelled) {
            return Unexpected(cancel.check(kWhere).error());
        }
        return fail(ErrorCode::Corrupt, std::string("the JPEG stream does not decode: ") + job->err.message,
                    kWhere, job->err.code);
    }
    return decoded;
}

std::string jpeg_library_version() {
    const int v = LIBJPEG_TURBO_VERSION_NUMBER;  // e.g. 3002000
    return "libjpeg-turbo " + std::to_string(v / 1000000) + "." + std::to_string((v / 1000) % 1000) + "." +
           std::to_string(v % 1000);
}

}  // namespace lucent::exporter::detail
