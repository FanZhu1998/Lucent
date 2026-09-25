// LibRaw error mapping, callbacks and name tables for the LibRaw adapter.

#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lucent/core/text.hpp>
#include <lucent/raw/libraw_decoder.hpp>

#include "libraw_support.hpp"

namespace lucent::raw {

ErrorCode libraw_error_code(int code) noexcept {
    if (code > 0) {  // errno values from LibRaw::open_datastream and friends
        return code == ENOENT ? ErrorCode::NotFound : ErrorCode::IoError;
    }
    switch (code) {
        case LIBRAW_FILE_UNSUPPORTED:
        case LIBRAW_NOT_IMPLEMENTED:
        case LIBRAW_UNSUPPORTED_THUMBNAIL:
            return ErrorCode::Unsupported;
        case LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE:
        case LIBRAW_NO_THUMBNAIL:
        case LIBRAW_REQUEST_FOR_NONEXISTENT_THUMBNAIL:
            return ErrorCode::NotFound;
        case LIBRAW_DATA_ERROR:
            return ErrorCode::Corrupt;
        case LIBRAW_IO_ERROR:
            return ErrorCode::IoError;
        case LIBRAW_UNSUFFICIENT_MEMORY:
            return ErrorCode::OutOfMemory;
        case LIBRAW_TOO_BIG:
        case LIBRAW_MEMPOOL_OVERFLOW:
            return ErrorCode::LimitExceeded;
        case LIBRAW_CANCELLED_BY_CALLBACK:
            return ErrorCode::Cancelled;
        case LIBRAW_OUT_OF_ORDER_CALL:
        case LIBRAW_INPUT_CLOSED:
        case LIBRAW_BAD_CROP:
        case LIBRAW_SUCCESS:  // not an error; asking for its category is an adapter defect
            return ErrorCode::Internal;
        default:
            return ErrorCode::Unknown;
    }
}

std::string_view libraw_error_text(int code) noexcept {
    const char* text = LibRaw::strerror(code);
    return text != nullptr ? std::string_view(text) : std::string_view("unknown LibRaw error");
}

}  // namespace lucent::raw

namespace lucent::raw::detail {

namespace {

int progress_callback(void* data, enum LibRaw_progress /*stage*/, int /*iteration*/, int /*expected*/) {
    auto* state = static_cast<LibRawCallState*>(data);
    if (state->cancel.is_cancelled()) {
        state->cancel_observed = true;
        return 1;  // LibRaw throws LIBRAW_EXCEPTION_CANCELLED_BY_CALLBACK internally
    }
    return 0;
}

void data_error_callback(void* data, const char* /*file*/, const INT64 offset) {
    auto* state = static_cast<LibRawCallState*>(data);
    if (state->data_errors == 0) {
        state->first_data_error_offset = offset;
    }
    if (state->data_errors < UINT32_MAX) {
        ++state->data_errors;
    }
}

struct FlagName {
    unsigned bit;
    std::string_view name;
};

std::vector<std::string> names_of(unsigned bits, std::span<const FlagName> table) {
    std::vector<std::string> names;
    unsigned known = 0;
    for (const FlagName& entry : table) {
        known |= entry.bit;
        if ((bits & entry.bit) != 0) {
            names.emplace_back(entry.name);
        }
    }
    for (unsigned bit = 1; bit != 0; bit <<= 1) {
        if ((bits & bit) != 0 && (known & bit) == 0) {
            names.push_back(std::format("bit{}", std::countr_zero(bit)));
        }
    }
    return names;
}

}  // namespace

void install_callbacks(LibRaw& raw, LibRawCallState& state) noexcept {
    raw.set_progress_handler(&progress_callback, &state);
    raw.set_dataerror_handler(&data_error_callback, &state);
}

Error libraw_failure(int code, std::string_view stage, const LibRawFileStream& stream,
                     const LibRawCallState& state) {
    const std::string where = std::format("raw.libraw.{}", stage);
    const std::string library_text(libraw_error_text(code));

    if (state.cancel_observed || stream.cancel_observed() || state.cancel.is_cancelled()) {
        return Error{ErrorCode::Cancelled, std::format("{} was cancelled", stage), where, code};
    }
    if (const auto& read_error = stream.read_error(); read_error) {
        Error error = *read_error;
        error.message = std::format("{} failed reading the file: {}", stage, error.message);
        if (error.where.empty()) {
            error.where = where;
        }
        return error;
    }
    if (code == LIBRAW_IO_ERROR) {
        // Every OS read failure is recorded by the stream, so LibRaw's "I/O error" here means it
        // ran out of data or rejected the structure (LibRaw maps both its EOF and CORRUPT
        // exceptions to LIBRAW_IO_ERROR).
        const std::string reason = stream.read_past_end() ? "the file ends before the data it describes"
                                                          : "the decoder rejected the file structure";
        return Error{ErrorCode::Corrupt, std::format("{} failed: {} ({})", stage, reason, library_text),
                     where, code};
    }
    return Error{libraw_error_code(code), std::format("{} failed: {}", stage, library_text), where, code};
}

std::vector<std::string> capability_names(unsigned capabilities) {
    static constexpr FlagName kTable[] = {
        {LIBRAW_CAPS_RAWSPEED, "rawspeed"},   {LIBRAW_CAPS_DNGSDK, "dngsdk"},
        {LIBRAW_CAPS_GPRSDK, "gprsdk"},       {LIBRAW_CAPS_UNICODEPATHS, "unicodepaths"},
        {LIBRAW_CAPS_X3FTOOLS, "x3ftools"},   {LIBRAW_CAPS_RPI6BY9, "rpi6by9"},
        {LIBRAW_CAPS_ZLIB, "zlib"},           {LIBRAW_CAPS_JPEG, "jpeg"},
        {LIBRAW_CAPS_RAWSPEED3, "rawspeed3"}, {LIBRAW_CAPS_RAWSPEED_BITS, "rawspeed_bits"},
    };
    return names_of(capabilities, kTable);
}

std::vector<std::string> decoder_flag_names(unsigned flags) {
    // LIBRAW_DECODER_3CHANNEL and LIBRAW_DECODER_SINAR4SHOT share bit 11 in LibRaw 0.22.
    static constexpr FlagName kTable[] = {
        {LIBRAW_DECODER_HASCURVE, "hascurve"},
        {LIBRAW_DECODER_SONYARW2, "sonyarw2"},
        {LIBRAW_DECODER_TRYRAWSPEED, "tryrawspeed"},
        {LIBRAW_DECODER_OWNALLOC, "ownalloc"},
        {LIBRAW_DECODER_FIXEDMAXC, "fixedmaxc"},
        {LIBRAW_DECODER_ADOBECOPYPIXEL, "adobecopypixel"},
        {LIBRAW_DECODER_LEGACY_WITH_MARGINS, "legacy_with_margins"},
        {LIBRAW_DECODER_3CHANNEL, "3channel_or_sinar4shot"},
        {LIBRAW_DECODER_FLATDATA, "flatdata"},
        {LIBRAW_DECODER_FLAT_BG2_SWAPPED, "flat_bg2_swapped"},
        {LIBRAW_DECODER_UNSUPPORTED_FORMAT, "unsupported_format"},
        {LIBRAW_DECODER_NOTSET, "notset"},
        {LIBRAW_DECODER_TRYRAWSPEED3, "tryrawspeed3"},
    };
    return names_of(flags, kTable);
}

std::vector<std::string> process_warning_names(unsigned warnings) {
    static constexpr FlagName kTable[] = {
        {LIBRAW_WARN_BAD_CAMERA_WB, "bad_camera_wb"},
        {LIBRAW_WARN_NO_METADATA, "no_metadata"},
        {LIBRAW_WARN_NO_JPEGLIB, "no_jpeglib"},
        {LIBRAW_WARN_NO_EMBEDDED_PROFILE, "no_embedded_profile"},
        {LIBRAW_WARN_NO_INPUT_PROFILE, "no_input_profile"},
        {LIBRAW_WARN_BAD_OUTPUT_PROFILE, "bad_output_profile"},
        {LIBRAW_WARN_NO_BADPIXELMAP, "no_badpixelmap"},
        {LIBRAW_WARN_BAD_DARKFRAME_FILE, "bad_darkframe_file"},
        {LIBRAW_WARN_BAD_DARKFRAME_DIM, "bad_darkframe_dim"},
        {LIBRAW_WARN_RAWSPEED_PROBLEM, "rawspeed_problem"},
        {LIBRAW_WARN_RAWSPEED_UNSUPPORTED, "rawspeed_unsupported"},
        {LIBRAW_WARN_RAWSPEED_PROCESSED, "rawspeed_processed"},
        {LIBRAW_WARN_FALLBACK_TO_AHD, "fallback_to_ahd"},
        {LIBRAW_WARN_PARSEFUJI_PROCESSED, "parsefuji_processed"},
        {LIBRAW_WARN_DNGSDK_PROCESSED, "dngsdk_processed"},
        {LIBRAW_WARN_DNG_IMAGES_REORDERED, "dng_images_reordered"},
        {LIBRAW_WARN_DNG_STAGE2_APPLIED, "dng_stage2_applied"},
        {LIBRAW_WARN_DNG_STAGE3_APPLIED, "dng_stage3_applied"},
        {LIBRAW_WARN_RAWSPEED3_PROBLEM, "rawspeed3_problem"},
        {LIBRAW_WARN_RAWSPEED3_UNSUPPORTED, "rawspeed3_unsupported"},
        {LIBRAW_WARN_RAWSPEED3_PROCESSED, "rawspeed3_processed"},
        {LIBRAW_WARN_RAWSPEED3_NOTLISTED, "rawspeed3_notlisted"},
        {LIBRAW_WARN_VENDOR_CROP_SUGGESTED, "vendor_crop_suggested"},
        {LIBRAW_WARN_DNG_NOT_PROCESSED, "dng_not_processed"},
        {LIBRAW_WARN_DNG_NOT_PARSED, "dng_not_parsed"},
    };
    return names_of(warnings, kTable);
}

std::string text_field(const char* field, std::size_t capacity) {
    if (field == nullptr || capacity == 0) {
        return {};
    }
    std::size_t length = 0;
    while (length < capacity && field[length] != '\0') {
        ++length;
    }
    std::string_view text(field, length);
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return core::sanitize_utf8(text);
}

LibRawBuffer unpacked_buffer(const LibRaw& raw) noexcept {
    const libraw_rawdata_t& data = raw.imgdata.rawdata;
    if (data.raw_image != nullptr) {
        return LibRawBuffer::Raw;
    }
    if (data.color3_image != nullptr) {
        return LibRawBuffer::Color3;
    }
    if (data.color4_image != nullptr) {
        return LibRawBuffer::Color4;
    }
    if (data.float_image != nullptr) {
        return LibRawBuffer::Float1;
    }
    if (data.float3_image != nullptr) {
        return LibRawBuffer::Float3;
    }
    if (data.float4_image != nullptr) {
        return LibRawBuffer::Float4;
    }
    return LibRawBuffer::None;
}

}  // namespace lucent::raw::detail
