#pragma once

// Private: a LibRaw datastream that reads through lucent::core::ReadOnlyFile.
//
// Why not LibRaw::open_file(const wchar_t*)? LibRaw's own Windows stream opens the file a second
// time with FILE_SHARE_READ only (blocking writers, unlike Lucent's READ|WRITE|DELETE sharing),
// cannot open paths longer than MAX_PATH, reports every open failure as LIBRAW_IO_ERROR without
// the OS error, and reads a file object whose identity Lucent never captured. Reading through
// the ReadOnlyFile whose FileIdentity the adapter records guarantees that the decoded bytes are
// the bytes of that identity (ADR 0005).
//
// The observable semantics deliberately mirror LibRaw_bigfile_buffered_datastream, the stream
// LibRaw (and therefore the rawpy reference) uses for files on Windows, so that both decode
// identically (LibRaw 0.22.2 src/libraw_datastream.cpp):
//   * read() returns the number of complete items read (floor), advancing by every byte read;
//   * seek() never clamps; reads at negative or past-the-end positions return 0 bytes;
//   * get_char() returns 0, not EOF, when no byte can be read;
//   * gets() and scanf_one() fail when fewer than `sz` (gets) or 24 (scanf_one) bytes remain.
//
// On top of LibRaw's behaviour the stream records what happened, so the adapter can report
// precise errors: OS read failures, reads beyond the end of the file, and cancellation. When the
// CancellationToken is cancelled, every later read returns 0 bytes and the owning LibRaw
// instance's cancel flag is set, so LibRaw stops at its next check or on the resulting end of
// data.
//
// Not thread-safe: one stream serves one LibRaw instance on one thread (LibRaw is built without
// OpenMP, so it never reads concurrently).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <libraw.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/file.hpp>

namespace lucent::raw::detail {

class LibRawFileStream final : public LibRaw_abstract_datastream {
public:
    // Size of the read-ahead buffer. Large enough to amortise system calls, small enough that
    // cancellation (checked on every refill) stays responsive.
    static constexpr std::size_t kBufferBytes = std::size_t{256} << 10;

    // `file` must stay open and outlive the stream; `size` is its size when it was opened.
    // Throws std::bad_alloc when the buffer cannot be allocated.
    LibRawFileStream(const core::ReadOnlyFile& file, std::uint64_t size, core::CancellationToken cancel);

    // The LibRaw instance reading from this stream, whose cancel flag is raised on cancellation.
    void set_owner(LibRaw* owner) noexcept { owner_ = owner; }

    int valid() override { return 1; }
    int read(void* ptr, size_t size, size_t nmemb) override;
    int seek(INT64 offset, int whence) override;
    INT64 tell() override { return position_; }
    INT64 size() override { return static_cast<INT64>(size_); }
    int get_char() override;
    char* gets(char* destination, int size) override;
    int scanf_one(const char* format, void* value) override;
    int eof() override { return position_ >= static_cast<INT64>(size_) ? 1 : 0; }
    void buffering_off() override { buffered_ = false; }
    void buffering_on() override { buffered_ = true; }
    bool is_buffered() override { return buffered_; }

    // True once a read was cut short because it extended beyond the end of the file.
    [[nodiscard]] bool read_past_end() const noexcept { return read_past_end_; }

    // Bytes requested beyond the end of the file since the last reset_past_end().
    [[nodiscard]] std::uint64_t past_end_bytes() const noexcept { return past_end_bytes_; }
    // Reads since the last reset_past_end() that started at or beyond the end of the file.
    [[nodiscard]] std::uint64_t reads_at_end() const noexcept { return reads_at_end_; }
    void reset_past_end() noexcept {
        read_past_end_ = false;
        past_end_bytes_ = 0;
        reads_at_end_ = 0;
    }

    // True once the token was seen cancelled (all later reads return no data).
    [[nodiscard]] bool cancel_observed() const noexcept { return cancel_observed_; }

    // The first OS read failure (or a file that shrank while being read), if any.
    [[nodiscard]] const std::optional<Error>& read_error() const noexcept { return read_error_; }

private:
    // False when reading must stop (cancelled or a previous read error).
    bool may_read();
    // Loads the buffer with up to kBufferBytes starting at `position` (0 <= position < size_).
    bool fill_buffer(std::uint64_t position);
    // Copies up to `count` bytes at the current position; returns the number copied.
    std::uint64_t copy_out(std::byte* destination, std::uint64_t count);
    // Bytes of the buffer available at the current position (0 when not buffered there).
    [[nodiscard]] std::uint64_t buffered_at_position() const noexcept;
    // Makes at least `count` bytes at the current position available in the buffer.
    bool ensure_buffered(std::uint64_t count);
    void record_error(Error error);

    const core::ReadOnlyFile* file_ = nullptr;
    std::uint64_t size_ = 0;
    core::CancellationToken cancel_;
    LibRaw* owner_ = nullptr;

    INT64 position_ = 0;
    std::vector<std::byte> buffer_;
    std::uint64_t buffer_start_ = 0;
    std::uint64_t buffer_length_ = 0;  // valid bytes in buffer_
    bool buffered_ = true;             // LibRaw's buffering switch; data is identical either way

    bool read_past_end_ = false;
    std::uint64_t past_end_bytes_ = 0;
    std::uint64_t reads_at_end_ = 0;
    bool cancel_observed_ = false;
    std::optional<Error> read_error_;
};

}  // namespace lucent::raw::detail
