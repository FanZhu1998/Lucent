#include "readback.hpp"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <jpeglib.h>
#include <tiffio.h>

namespace lucent::qualify {

namespace {

struct TiffCloser {
    void operator()(TIFF* t) const noexcept { TIFFClose(t); }
};

#pragma warning(push)
#pragma warning(disable : 4324)  // padded: jmp_buf alignment
struct JpegError {
    jpeg_error_mgr pub;  // first member
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};
#pragma warning(pop)

[[noreturn]] void on_jpeg_error(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegError*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->message);
    std::longjmp(err->jump, 1);
}

void on_jpeg_message(j_common_ptr) {}

// Everything the setjmp frame touches is plain data (no destructors can be skipped). The ICC
// buffer is malloc'ed by libjpeg-turbo and freed by the caller.
struct JpegJob {
    jpeg_decompress_struct cinfo;
    JpegError err;
    std::FILE* file;
    unsigned char* row;
    std::size_t row_size;
    void (*deliver)(void*, std::int32_t, const unsigned char*, std::size_t);
    void* context;
    std::int32_t width;
    std::int32_t height;
    std::int32_t channels;
    std::int32_t bits;
    JOCTET* icc;
    unsigned int icc_size;
    long warnings;
};

#pragma warning(push)
#pragma warning(disable : 4611)  // setjmp and C++ destruction: the frame holds no such objects
bool run_jpeg(JpegJob* job, std::vector<unsigned char>* buffer) {
    job->cinfo.err = jpeg_std_error(&job->err.pub);
    job->err.pub.error_exit = on_jpeg_error;
    job->err.pub.output_message = on_jpeg_message;
    if (setjmp(job->err.jump) != 0) {
        jpeg_destroy_decompress(&job->cinfo);
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
    job->width = static_cast<std::int32_t>(job->cinfo.output_width);
    job->height = static_cast<std::int32_t>(job->cinfo.output_height);
    job->channels = job->cinfo.output_components;
    job->bits = job->cinfo.data_precision;
    job->row_size = static_cast<std::size_t>(job->cinfo.output_width) * job->cinfo.output_components;
    if (buffer->size() < job->row_size) {
        // Resizing may throw; this happens before any longjmp can target this frame's objects,
        // and `buffer` lives in the caller.
        buffer->resize(job->row_size);
    }
    job->row = buffer->data();
    while (job->cinfo.output_scanline < job->cinfo.output_height) {
        const auto y = static_cast<std::int32_t>(job->cinfo.output_scanline);
        JSAMPROW row = job->row;
        jpeg_read_scanlines(&job->cinfo, &row, 1);
        job->deliver(job->context, y, job->row, job->row_size);
    }
    jpeg_finish_decompress(&job->cinfo);
    job->warnings = job->err.pub.num_warnings;
    jpeg_destroy_decompress(&job->cinfo);
    return true;
}
#pragma warning(pop)

}  // namespace

bool read_jpeg(const std::filesystem::path& path, ReadbackHeader& header, std::string& error,
               const std::function<void(std::int32_t, std::span<const std::uint8_t>)>& row) {
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
        error = "cannot open the JPEG";
        return false;
    }
    auto job = std::make_unique<JpegJob>();
    std::memset(job.get(), 0, sizeof(JpegJob));
    job->file = file;
    job->context = const_cast<void*>(static_cast<const void*>(&row));
    job->deliver = [](void* context, std::int32_t y, const unsigned char* data, std::size_t size) {
        const auto& callback =
            *static_cast<const std::function<void(std::int32_t, std::span<const std::uint8_t>)>*>(context);
        callback(y, std::span<const std::uint8_t>(data, size));
    };
    std::vector<unsigned char> buffer;
    const bool decoded = run_jpeg(job.get(), &buffer);
    std::fclose(file);
    if (job->icc != nullptr) {
        header.icc.assign(job->icc, job->icc + job->icc_size);
        std::free(job->icc);
    }
    if (!decoded) {
        error = std::string("JPEG does not decode: ") + job->err.message;
        return false;
    }
    header.width = job->width;
    header.height = job->height;
    header.channels = job->channels;
    header.bits = job->bits;
    if (job->warnings != 0) {
        error = "JPEG decoder warnings: " + std::to_string(job->warnings);
        return false;
    }
    return true;
}

bool read_tiff16(const std::filesystem::path& path, ReadbackHeader& header, std::string& error,
                 const std::function<void(std::int32_t, std::span<const std::uint16_t>)>& row) {
    std::unique_ptr<TIFF, TiffCloser> tiff(TIFFOpenW(path.c_str(), "r"));
    if (!tiff) {
        error = "cannot open the TIFF";
        return false;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t bits = 0;
    std::uint16_t samples = 0;
    std::uint16_t planar = 0;
    if (!TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width) ||
        !TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height) ||
        !TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &bits) ||
        !TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samples) ||
        !TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PLANARCONFIG, &planar)) {
        error = "missing TIFF fields";
        return false;
    }
    header.width = static_cast<std::int32_t>(width);
    header.height = static_cast<std::int32_t>(height);
    header.channels = samples;
    header.bits = bits;
    std::uint32_t icc_size = 0;
    void* icc = nullptr;
    if (TIFFGetField(tiff.get(), TIFFTAG_ICCPROFILE, &icc_size, &icc) && icc != nullptr) {
        const auto* bytes = static_cast<const std::uint8_t*>(icc);
        header.icc.assign(bytes, bytes + icc_size);
    }
    if (bits != 16 || samples != 3 || planar != PLANARCONFIG_CONTIG) {
        error = "not 16-bit chunky RGB";
        return false;
    }
    std::vector<std::uint16_t> buffer(static_cast<std::size_t>(width) * 3);
    for (std::uint32_t y = 0; y < height; ++y) {
        if (TIFFReadScanline(tiff.get(), buffer.data(), y, 0) < 0) {
            error = "TIFF row " + std::to_string(y) + " does not decode";
            return false;
        }
        row(static_cast<std::int32_t>(y), buffer);
    }
    return true;
}

}  // namespace lucent::qualify
