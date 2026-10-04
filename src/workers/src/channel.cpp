#include <lucent/workers/channel.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <new>
#include <span>
#include <string>

#include <windows.h>

#include <lucent/core/win32_error.hpp>

#include "win32_util.hpp"

namespace lucent::workers {

namespace {

constexpr std::string_view kWhereRead = "workers.channel.read";
constexpr std::string_view kWhereWrite = "workers.channel.write";
constexpr DWORD kPipeBufferBytes = 64 * 1024;

bool is_end_of_stream(DWORD error) noexcept {
    return error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF || error == ERROR_PIPE_NOT_CONNECTED ||
           error == ERROR_NO_DATA;
}

// Reads until `buffer` is full or the stream ends; returns the number of bytes read.
Expected<std::size_t> read_fully(HANDLE handle, std::span<std::byte> buffer) {
    std::size_t total = 0;
    while (total < buffer.size()) {
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(buffer.size() - total, 1u << 20));
        DWORD got = 0;
        if (!::ReadFile(handle, buffer.data() + total, want, &got, nullptr)) {
            const DWORD error = ::GetLastError();
            if (is_end_of_stream(error)) {
                return total;
            }
            if (error == ERROR_OPERATION_ABORTED) {
                return fail(ErrorCode::Cancelled, "the read was cancelled", std::string(kWhereRead),
                            static_cast<std::int64_t>(error));
            }
            return Unexpected(core::win32_error(error, std::string(kWhereRead), "cannot read from the worker pipe"));
        }
        if (got == 0) {
            return total;  // end of file
        }
        total += got;
    }
    return total;
}

}  // namespace

bool is_channel_closed(const Error& error) noexcept {
    return error.code == ErrorCode::IoError && error.where == kWhereChannelClosed;
}

MessageChannel::MessageChannel(void* read_handle, void* write_handle, Direction incoming) noexcept
    : read_(read_handle), write_(write_handle), incoming_(incoming) {}

MessageChannel::~MessageChannel() {
    close_write();
    if (read_ != nullptr) {
        ::CloseHandle(read_);
    }
}

Expected<std::optional<Message>> MessageChannel::receive() {
    if (read_ == nullptr) {
        return fail(ErrorCode::Internal, "the channel cannot read", std::string(kWhereRead));
    }
    std::array<std::byte, kFrameHeaderBytes> header{};
    auto got = read_fully(read_, header);
    if (!got) {
        return Unexpected(std::move(got.error()));
    }
    if (*got == 0) {
        return std::optional<Message>{};
    }
    if (*got < header.size()) {
        return fail(ErrorCode::Corrupt, "the pipe closed inside a frame header", "workers.protocol.frame");
    }
    auto length = decode_frame_length(header);
    if (!length) {
        return Unexpected(std::move(length.error()));
    }
    std::string payload;
    try {
        payload.resize(*length);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate a frame", std::string(kWhereRead));
    }
    got = read_fully(read_, std::as_writable_bytes(std::span(payload.data(), payload.size())));
    if (!got) {
        return Unexpected(std::move(got.error()));
    }
    if (*got < payload.size()) {
        return fail(ErrorCode::Corrupt,
                    std::format("the pipe closed inside a frame ({} of {} bytes)", *got, payload.size()),
                    "workers.protocol.frame");
    }
    auto message = decode_message(payload, incoming_);
    if (!message) {
        return Unexpected(std::move(message.error()));
    }
    return std::optional<Message>(std::move(*message));
}

Status MessageChannel::send(const Message& message) {
    auto frame = encode_frame(message);
    if (!frame) {
        return Unexpected(std::move(frame.error()));
    }
    std::lock_guard lock(write_mutex_);
    if (write_ == nullptr) {
        return fail(ErrorCode::IoError, "the channel is closed for writing", std::string(kWhereChannelClosed));
    }
    std::size_t written = 0;
    while (written < frame->size()) {
        DWORD wrote = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(frame->size() - written, 1u << 20));
        if (!::WriteFile(write_, frame->data() + written, want, &wrote, nullptr)) {
            const DWORD error = ::GetLastError();
            if (is_end_of_stream(error)) {
                return fail(ErrorCode::IoError, "the peer closed the pipe", std::string(kWhereChannelClosed),
                            static_cast<std::int64_t>(error));
            }
            return Unexpected(core::win32_error(error, std::string(kWhereWrite), "cannot write to the worker pipe"));
        }
        written += wrote;
    }
    return ok();
}

void MessageChannel::close_write() noexcept {
    std::lock_guard lock(write_mutex_);
    if (write_ != nullptr) {
        ::CloseHandle(write_);
        write_ = nullptr;
    }
}

Expected<ChannelPair> create_channel_pair() {
    HANDLE request_read = nullptr;
    HANDLE request_write = nullptr;
    HANDLE response_read = nullptr;
    HANDLE response_write = nullptr;
    if (!::CreatePipe(&request_read, &request_write, nullptr, kPipeBufferBytes)) {
        return Unexpected(core::last_error("workers.channel.create", "cannot create a pipe"));
    }
    detail::UniqueHandle rr(request_read);
    detail::UniqueHandle rw(request_write);
    if (!::CreatePipe(&response_read, &response_write, nullptr, kPipeBufferBytes)) {
        return Unexpected(core::last_error("workers.channel.create", "cannot create a pipe"));
    }
    detail::UniqueHandle sr(response_read);
    detail::UniqueHandle sw(response_write);
    try {
        ChannelPair pair;
        pair.host = std::make_unique<MessageChannel>(sr.get(), rw.get(), Direction::WorkerToHost);
        (void)sr.release();
        (void)rw.release();
        pair.worker = std::make_unique<MessageChannel>(rr.get(), sw.get(), Direction::HostToWorker);
        (void)rr.release();
        (void)sw.release();
        return pair;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot create a channel", "workers.channel.create");
    }
}

}  // namespace lucent::workers
