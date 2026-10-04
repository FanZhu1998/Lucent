#pragma once

// Framed protocol messages over a pair of byte-stream handles (protocol.hpp, "Transport"): one
// handle this side reads, one it writes. The host and the worker each hold one MessageChannel over
// their ends of the two pipes.
//
// Thread-safety: receive() must be called by one thread at a time (a dedicated reader thread);
// send() and close_write() may be called from any thread concurrently with each other and with
// receive(): frames are written whole under a lock and never interleave. The handles are
// synchronous; a thread blocked in receive() returns when the peer closes its end (the process
// ends or closes the pipe) or when another thread cancels the read with CancelSynchronousIo.

#include <memory>
#include <mutex>
#include <optional>

#include <lucent/core/error.hpp>
#include <lucent/workers/protocol.hpp>

namespace lucent::workers {

// Location of the errors that mean "the peer closed its end" (send() after the peer is gone).
inline constexpr std::string_view kWhereChannelClosed = "workers.channel.closed";

// True for the IoError send() reports once the peer has closed its end.
[[nodiscard]] bool is_channel_closed(const Error& error) noexcept;

class MessageChannel {
public:
    // Takes ownership of two Win32 handles (HANDLE values; either may be null, which makes that
    // direction unusable) of synchronous pipe ends or files. `incoming` is the direction of the
    // messages this side receives: Direction::WorkerToHost for the host.
    MessageChannel(void* read_handle, void* write_handle, Direction incoming) noexcept;
    // Closes both handles. Must not race with receive() or send().
    ~MessageChannel();
    MessageChannel(const MessageChannel&) = delete;
    MessageChannel& operator=(const MessageChannel&) = delete;

    // Blocks until one whole message has arrived and returns it, decoded and validated for
    // `incoming` (decode_message). std::nullopt when the peer closed its end at a frame boundary
    // (end of file, broken pipe): the normal end of a conversation. Errors: Corrupt
    // ("workers.protocol.*") for a frame header announcing a bad length, a frame cut short by the
    // peer closing, or a message decode_message refuses (the stream cannot be trusted afterwards;
    // stop reading); Cancelled when the read was cancelled with CancelSynchronousIo; IoError for
    // other read failures; OutOfMemory.
    [[nodiscard]] Expected<std::optional<Message>> receive();

    // Encodes `message` (encode_frame) and writes the whole frame. Errors: those of encode_frame;
    // IoError at kWhereChannelClosed when the peer has closed its end or close_write() was called;
    // IoError for other write failures.
    [[nodiscard]] Status send(const Message& message);

    // Closes the write handle: the peer's receive() returns std::nullopt after the frames already
    // sent. Idempotent.
    void close_write() noexcept;

private:
    void* read_ = nullptr;
    void* write_ = nullptr;
    Direction incoming_;
    std::mutex write_mutex_;
};

// Two connected channels over two new anonymous pipes (non-inheritable): `host` receives what
// `worker` sends and vice versa. Used by tests that run a worker server in-process. Errors: IoError
// or OutOfMemory from CreatePipe.
struct ChannelPair {
    std::unique_ptr<MessageChannel> host;
    std::unique_ptr<MessageChannel> worker;
};

[[nodiscard]] Expected<ChannelPair> create_channel_pair();

}  // namespace lucent::workers
