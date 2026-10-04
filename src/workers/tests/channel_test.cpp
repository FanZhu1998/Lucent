// Framed messages over real anonymous pipes (channel.hpp).

#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/workers/channel.hpp>

#include "workers_test_support.hpp"

namespace lucent::workers {
namespace {

// A channel reading from a pipe whose write end the test keeps, to inject raw bytes.
struct RawPipe {
    std::unique_ptr<MessageChannel> reader;
    HANDLE write = nullptr;

    explicit RawPipe(Direction incoming) {
        HANDLE read = nullptr;
        EXPECT_TRUE(::CreatePipe(&read, &write, nullptr, 0));
        reader = std::make_unique<MessageChannel>(read, nullptr, incoming);
    }
    ~RawPipe() { close(); }
    void put(const std::string& bytes) {
        DWORD written = 0;
        ASSERT_TRUE(::WriteFile(write, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
        ASSERT_EQ(written, bytes.size());
    }
    void close() {
        if (write != nullptr) {
            ::CloseHandle(write);
            write = nullptr;
        }
    }
};

std::string header(std::uint32_t length) {
    std::string h(4, '\0');
    for (int i = 0; i < 4; ++i) {
        h[i] = static_cast<char>((length >> (8 * i)) & 0xFF);
    }
    return h;
}

TEST(WorkerChannel, MessagesCrossThePipesBothWays) {
    auto pair = create_channel_pair();
    ASSERT_TRUE(pair) << pair.error().describe();
    ASSERT_TRUE(pair->host->send(test_support::sample_job()));
    auto job = pair->worker->receive();
    ASSERT_TRUE(job && *job) << (job ? "closed" : job.error().describe());
    EXPECT_EQ(std::get<JobRequest>(**job).job_id, "job-1");

    ASSERT_TRUE(pair->worker->send(test_support::sample_result()));
    auto result = pair->host->receive();
    ASSERT_TRUE(result && *result);
    EXPECT_EQ(std::get<JobResult>(**result).output.width, 8192u);
}

TEST(WorkerChannel, ClosingTheWriteEndEndsTheStreamAfterPendingFrames) {
    auto pair = create_channel_pair();
    ASSERT_TRUE(pair);
    ASSERT_TRUE(pair->host->send(Ping{1}));
    ASSERT_TRUE(pair->host->send(Ping{2}));
    pair->host->close_write();
    pair->host->close_write();  // idempotent
    for (std::uint64_t nonce : {1u, 2u}) {
        auto ping = pair->worker->receive();
        ASSERT_TRUE(ping && *ping);
        EXPECT_EQ(std::get<Ping>(**ping).nonce, nonce);
    }
    auto end = pair->worker->receive();
    ASSERT_TRUE(end);
    EXPECT_FALSE(end->has_value());

    auto closed = pair->host->send(Ping{3});
    ASSERT_FALSE(closed);
    EXPECT_TRUE(is_channel_closed(closed.error()));
}

TEST(WorkerChannel, SendingToAClosedPeerReportsTheClosedChannel) {
    auto pair = create_channel_pair();
    ASSERT_TRUE(pair);
    pair->worker.reset();  // closes both of the worker's ends
    auto sent = pair->host->send(Ping{1});
    ASSERT_FALSE(sent);
    EXPECT_TRUE(is_channel_closed(sent.error())) << sent.error().describe();
    auto received = pair->host->receive();
    ASSERT_TRUE(received);
    EXPECT_FALSE(received->has_value());
}

TEST(WorkerChannel, DamagedFramesAreCorrupt) {
    {
        RawPipe pipe(Direction::HostToWorker);
        pipe.put(header(kMaxMessageBytes + 1));
        auto received = pipe.reader->receive();
        ASSERT_FALSE(received);
        EXPECT_EQ(received.error().code, ErrorCode::Corrupt);
    }
    {
        RawPipe pipe(Direction::HostToWorker);
        pipe.put(header(100) + std::string(10, '{'));
        pipe.close();
        auto received = pipe.reader->receive();
        ASSERT_FALSE(received);
        EXPECT_EQ(received.error().code, ErrorCode::Corrupt);
        EXPECT_EQ(received.error().where, "workers.protocol.frame");
    }
    {
        RawPipe pipe(Direction::HostToWorker);
        pipe.put(std::string(2, '\x05'));
        pipe.close();
        auto received = pipe.reader->receive();
        ASSERT_FALSE(received);
        EXPECT_EQ(received.error().code, ErrorCode::Corrupt);
    }
    {
        const std::string text = R"({"type":"pong","nonce":1})";  // a worker message sent to the worker
        RawPipe pipe(Direction::HostToWorker);
        pipe.put(header(static_cast<std::uint32_t>(text.size())) + text);
        auto received = pipe.reader->receive();
        ASSERT_FALSE(received);
        EXPECT_EQ(received.error().code, ErrorCode::Corrupt);
    }
}

TEST(WorkerChannel, ConcurrentSendersNeverInterleaveFrames) {
    auto pair = create_channel_pair();
    ASSERT_TRUE(pair);
    constexpr int kPerThread = 300;
    std::atomic<int> failures{0};
    std::vector<std::thread> senders;
    for (int t = 0; t < 3; ++t) {
        senders.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                JobResult result = test_support::sample_result(std::format("job-{}-{}", t, i));
                if (!pair->worker->send(result)) {
                    ++failures;
                }
            }
        });
    }
    int received = 0;
    for (; received < 3 * kPerThread; ++received) {
        auto message = pair->host->receive();
        ASSERT_TRUE(message && *message) << (message ? "closed" : message.error().describe());
        ASSERT_TRUE(std::holds_alternative<JobResult>(**message));
    }
    for (auto& sender : senders) {
        sender.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(received, 3 * kPerThread);
}

TEST(WorkerChannel, ABlockedReceiveCanBeCancelled) {
    auto pair = create_channel_pair();
    ASSERT_TRUE(pair);
    std::atomic<bool> done{false};
    Expected<std::optional<Message>> outcome = std::optional<Message>{};
    std::thread reader([&] {
        outcome = pair->worker->receive();
        done = true;
    });
    for (int i = 0; i < 500 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ::CancelSynchronousIo(reader.native_handle());
    }
    reader.join();
    ASSERT_FALSE(outcome);
    EXPECT_EQ(outcome.error().code, ErrorCode::Cancelled);
}

}  // namespace
}  // namespace lucent::workers
