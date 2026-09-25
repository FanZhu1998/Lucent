#include "libraw_stream.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <format>
#include <limits>
#include <span>
#include <utility>

namespace lucent::raw::detail {

namespace {

constexpr std::string_view kWhereRead = "raw.libraw.read";

// Saturating a + b for the stream position (LibRaw passes arbitrary 64-bit offsets).
INT64 saturating_add(INT64 a, INT64 b) noexcept {
    if (b > 0 && a > std::numeric_limits<INT64>::max() - b) {
        return std::numeric_limits<INT64>::max();
    }
    if (b < 0 && a < std::numeric_limits<INT64>::min() - b) {
        return std::numeric_limits<INT64>::min();
    }
    return a + b;
}

}  // namespace

LibRawFileStream::LibRawFileStream(const core::ReadOnlyFile& file, std::uint64_t size,
                                   core::CancellationToken cancel)
    : file_(&file), size_(size), cancel_(std::move(cancel)), buffer_(kBufferBytes) {}

void LibRawFileStream::record_error(Error error) {
    if (!read_error_) {
        read_error_ = std::move(error);
    }
}

bool LibRawFileStream::may_read() {
    if (cancel_observed_ || read_error_) {
        return false;
    }
    if (cancel_.is_cancelled()) {
        cancel_observed_ = true;
        if (owner_ != nullptr) {
            owner_->setCancelFlag();  // LibRaw stops at its next checkCancel()
        }
        return false;
    }
    return true;
}

bool LibRawFileStream::fill_buffer(std::uint64_t position) {
    if (!may_read()) {
        return false;
    }
    const std::uint64_t wanted = std::min<std::uint64_t>(buffer_.size(), size_ - position);
    auto read =
        file_->read_at(position, std::span<std::byte>(buffer_.data(), static_cast<std::size_t>(wanted)));
    if (!read) {
        buffer_length_ = 0;
        record_error(std::move(read.error()));
        return false;
    }
    buffer_start_ = position;
    buffer_length_ = *read;
    if (*read != wanted) {
        // The file is shorter than when it was opened: its content version changed.
        record_error(Error{ErrorCode::SourceChanged,
                           std::format("file ended at byte {} although it had {} bytes when it was opened",
                                       position + *read, size_),
                           std::string(kWhereRead), 0});
        return false;
    }
    return true;
}

std::uint64_t LibRawFileStream::buffered_at_position() const noexcept {
    if (position_ < 0) {
        return 0;
    }
    const auto pos = static_cast<std::uint64_t>(position_);
    if (pos >= buffer_start_ && pos < buffer_start_ + buffer_length_) {
        return buffer_start_ + buffer_length_ - pos;
    }
    return 0;
}

bool LibRawFileStream::ensure_buffered(std::uint64_t count) {
    if (buffered_at_position() >= count) {
        return true;
    }
    const auto pos = static_cast<std::uint64_t>(position_);
    if (count > buffer_.size() || pos >= size_ || size_ - pos < count) {
        return false;
    }
    return fill_buffer(pos) && buffered_at_position() >= count;
}

std::uint64_t LibRawFileStream::copy_out(std::byte* destination, std::uint64_t count) {
    std::uint64_t copied = 0;
    while (copied < count) {
        const std::uint64_t in_buffer = buffered_at_position();
        if (in_buffer > 0) {
            const std::uint64_t n = std::min(in_buffer, count - copied);
            const auto offset =
                static_cast<std::size_t>(static_cast<std::uint64_t>(position_) - buffer_start_);
            std::memcpy(destination + copied, buffer_.data() + offset, static_cast<std::size_t>(n));
            copied += n;
            position_ += static_cast<INT64>(n);
            continue;
        }
        const std::uint64_t remaining = count - copied;
        const auto pos = static_cast<std::uint64_t>(position_);
        if (remaining >= buffer_.size()) {
            // Large request: read straight into the caller's memory.
            if (!may_read()) {
                break;
            }
            auto read = file_->read_at(
                pos, std::span<std::byte>(destination + copied, static_cast<std::size_t>(remaining)));
            if (!read) {
                record_error(std::move(read.error()));
                break;
            }
            copied += *read;
            position_ += static_cast<INT64>(*read);
            if (*read != remaining) {
                record_error(
                    Error{ErrorCode::SourceChanged,
                          std::format("file ended at byte {} although it had {} bytes when it was opened",
                                      pos + *read, size_),
                          std::string(kWhereRead), 0});
                break;
            }
            continue;
        }
        if (!fill_buffer(pos)) {
            break;
        }
    }
    return copied;
}

int LibRawFileStream::read(void* ptr, size_t item_size, size_t items) {
    if (item_size < 1 || items < 1 || ptr == nullptr || position_ < 0) {
        return 0;
    }
    // Requested bytes, limited so that the item count fits LibRaw's int return value.
    const std::uint64_t max_items = static_cast<std::uint64_t>(INT_MAX);
    const std::uint64_t clamped_items = std::min<std::uint64_t>(items, max_items);
    std::uint64_t requested = 0;
    if (clamped_items > std::numeric_limits<std::uint64_t>::max() / item_size) {
        requested = std::numeric_limits<std::uint64_t>::max();
    } else {
        requested = clamped_items * item_size;
    }
    const auto pos = static_cast<std::uint64_t>(position_);
    const std::uint64_t available = pos >= size_ ? 0 : size_ - pos;
    if (requested > available) {
        if (available == 0) {
            ++reads_at_end_;
        }
        read_past_end_ = true;
        const std::uint64_t missing = requested - available;
        past_end_bytes_ = missing > std::numeric_limits<std::uint64_t>::max() - past_end_bytes_
                              ? std::numeric_limits<std::uint64_t>::max()
                              : past_end_bytes_ + missing;
        requested = available;
    }
    if (requested == 0 || !may_read()) {
        return 0;
    }
    const std::uint64_t copied = copy_out(static_cast<std::byte*>(ptr), requested);
    return static_cast<int>(copied / item_size);
}

int LibRawFileStream::seek(INT64 offset, int whence) {
    switch (whence) {
        case SEEK_SET:
            position_ = offset;
            break;
        case SEEK_CUR:
            position_ = saturating_add(position_, offset);
            break;
        case SEEK_END:
            // As LibRaw_bigfile_buffered_datastream: a positive offset lands exactly at the end.
            position_ =
                offset > 0 ? static_cast<INT64>(size_) : saturating_add(static_cast<INT64>(size_), offset);
            break;
        default:
            break;
    }
    return 0;
}

int LibRawFileStream::get_char() {
    if (buffered_at_position() > 0) {
        const auto offset = static_cast<std::size_t>(static_cast<std::uint64_t>(position_) - buffer_start_);
        ++position_;
        return std::to_integer<unsigned char>(buffer_[offset]);
    }
    unsigned char c = 0;
    const int r = read(&c, 1, 1);
    return r > 0 ? c : r;  // 0 when nothing could be read, exactly as LibRaw's Windows stream
}

char* LibRawFileStream::gets(char* destination, int size) {
    if (destination == nullptr || size < 1) {
        return nullptr;
    }
    if (size < 2) {
        destination[0] = 0;
        return destination;
    }
    if (position_ < 0 || !ensure_buffered(static_cast<std::uint64_t>(size))) {
        const auto pos = static_cast<std::uint64_t>(std::max<INT64>(position_, 0));
        const std::uint64_t remaining = pos < size_ ? size_ - pos : 0;
        if (position_ >= 0 && remaining < static_cast<std::uint64_t>(size)) {
            read_past_end_ = true;
            past_end_bytes_ += static_cast<std::uint64_t>(size) - remaining;
        }
        return nullptr;
    }
    // LibRaw_bigfile_buffered_datastream::gets, reproduced step by step: copy up to size - 1
    // characters, stopping after a newline; always consume one character past the copied run.
    const auto* source = reinterpret_cast<const unsigned char*>(buffer_.data()) +
                         (static_cast<std::uint64_t>(position_) - buffer_start_);
    const auto available = static_cast<std::ptrdiff_t>(buffered_at_position());
    auto* out = reinterpret_cast<unsigned char*>(destination);
    std::ptrdiff_t in = 0;
    std::ptrdiff_t written = 0;
    while (in < available && written < size - 1) {
        out[written] = source[in];
        if (source[in] == '\n') {
            break;
        }
        ++in;
        ++written;
    }
    if (in < available) {
        ++in;
    }
    if (written < size - 1) {
        out[written + 1] = 0;
    } else {
        destination[size - 1] = 0;
    }
    position_ += static_cast<INT64>(in);
    return destination;
}

int LibRawFileStream::scanf_one(const char* format, void* value) {
    // LibRaw_bigfile_buffered_datastream::scanf_one: needs 24 buffered bytes, parses one value
    // with sscanf, then skips to the next space, tab, newline or NUL (at most 25 characters).
    constexpr std::uint64_t kWindow = 24;
    if (format == nullptr || value == nullptr || position_ < 0 || !ensure_buffered(kWindow)) {
        return -1;
    }
    const std::uint64_t available = buffered_at_position();
    const auto* source = reinterpret_cast<const char*>(buffer_.data()) +
                         (static_cast<std::uint64_t>(position_) - buffer_start_);
    // sscanf needs a terminated string; 63 characters exceed every number LibRaw parses this way.
    char text[64] = {};
    const auto copy = static_cast<std::size_t>(std::min<std::uint64_t>(available, sizeof(text) - 1));
    std::memcpy(text, source, copy);
    const int result = sscanf_s(text, format, value);
    if (result > 0) {
        std::uint64_t step = 0;
        int count = 0;
        while (step < available) {
            ++step;
            ++count;
            const char c = step < available ? source[step] : '\0';
            if (c == 0 || c == ' ' || c == '\t' || c == '\n' || count > 24) {
                break;
            }
        }
        position_ += static_cast<INT64>(step);
        return result;
    }
    return -1;
}

}  // namespace lucent::raw::detail
