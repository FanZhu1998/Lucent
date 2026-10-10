// The WinHTTP client of the update channel (http_client.hpp; ADR 0022) against a loopback server:
// bodies, statuses, size limits, redirects within and outside the policy, timeouts, refused
// connections and cancellation. Every URL is on 127.0.0.1 (UrlPolicy::LoopbackOnly, no proxy), so
// nothing leaves the machine.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <thread>

#include <lucent/application/http_client.hpp>
#include <lucent/core/cancellation.hpp>

#include "loopback_http_server.hpp"

namespace lucent::app {
namespace {

using test::LoopbackHttpServer;

HttpClientOptions loopback_options() {
    HttpClientOptions options;
    options.policy = UrlPolicy::LoopbackOnly;
    options.system_proxy = false;
    options.resolve_timeout = std::chrono::milliseconds(5000);
    options.connect_timeout = std::chrono::milliseconds(5000);
    options.send_timeout = std::chrono::milliseconds(5000);
    options.receive_timeout = std::chrono::milliseconds(5000);
    return options;
}

std::unique_ptr<HttpClient> loopback_client(HttpClientOptions options = loopback_options()) {
    Expected<std::unique_ptr<HttpClient>> client = create_winhttp_client(options);
    EXPECT_TRUE(client) << (client ? std::string() : client.error().describe());
    return client ? std::move(*client) : nullptr;
}

class HttpClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(server.listening());
        client = loopback_client();
        ASSERT_NE(client, nullptr);
    }

    Expected<std::string> get(const std::string& path, std::uint64_t max_bytes = 1 << 20,
                              core::CancellationToken cancel = {}) {
        return http_get_text(*client, server.url(path), max_bytes, std::move(cancel));
    }

    LoopbackHttpServer server;
    std::unique_ptr<HttpClient> client;
};

std::string pattern(std::size_t size) {
    std::string text(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        text[i] = static_cast<char>((i * 131 + i / 7) & 0xFF);
    }
    return text;
}

TEST_F(HttpClientTest, DownloadsBodiesExactly) {
    const std::string large = pattern(3 * 64 * 1024 + 17);  // several chunks, binary
    server.set("/large.bin", {.body = large});
    server.set("/empty", {.body = ""});
    server.set("/unannounced", {.body = "no length", .send_length = false});
    Expected<std::string> body = get("/large.bin");
    ASSERT_TRUE(body) << body.error().describe();
    EXPECT_EQ(*body, large);
    body = get("/empty");
    ASSERT_TRUE(body) << body.error().describe();
    EXPECT_TRUE(body->empty());
    body = get("/unannounced");
    ASSERT_TRUE(body) << body.error().describe();
    EXPECT_EQ(*body, "no length");
    EXPECT_EQ(server.request_count("/large.bin"), 1u);
}

TEST_F(HttpClientTest, StatusesMapToErrors) {
    server.set("/forbidden", {.status = 403, .body = "no"});
    server.set("/broken", {.status = 500, .body = "oops"});
    Expected<std::string> missing = get("/nothing-here");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
    EXPECT_EQ(missing.error().where, "app.update.http");
    EXPECT_EQ(missing.error().native_code, 404);
    Expected<std::string> forbidden = get("/forbidden");
    ASSERT_FALSE(forbidden);
    EXPECT_EQ(forbidden.error().code, ErrorCode::PermissionDenied);
    Expected<std::string> broken = get("/broken");
    ASSERT_FALSE(broken);
    EXPECT_EQ(broken.error().code, ErrorCode::IoError);
    EXPECT_EQ(broken.error().native_code, 500);
}

TEST_F(HttpClientTest, SizeLimitsAndShortBodiesAreRefused) {
    server.set("/big", {.body = std::string(5000, 'x')});
    server.set("/big-unannounced", {.body = std::string(5000, 'x'), .send_length = false});
    server.set("/short", {.body = "only this", .announced_length = 100});
    Expected<std::string> announced = get("/big", 4096);
    ASSERT_FALSE(announced);
    EXPECT_EQ(announced.error().code, ErrorCode::LimitExceeded);
    Expected<std::string> streamed = get("/big-unannounced", 4096);
    ASSERT_FALSE(streamed);
    EXPECT_EQ(streamed.error().code, ErrorCode::LimitExceeded);
    Expected<std::string> exact = get("/big", 5000);
    ASSERT_TRUE(exact) << exact.error().describe();
    Expected<std::string> truncated = get("/short");
    ASSERT_FALSE(truncated);
    // WinHTTP itself may report the connection that closed early, or the client the missing bytes.
    EXPECT_TRUE(truncated.error().code == ErrorCode::Corrupt || truncated.error().code == ErrorCode::IoError)
        << truncated.error().describe();
}

TEST_F(HttpClientTest, RedirectsAreFollowedOnlyWithinThePolicy) {
    server.set("/target", {.body = "arrived"});
    server.set("/relative", {.status = 302, .headers = {{"Location", "/target"}}});
    server.set("/absolute", {.status = 301, .headers = {{"Location", server.url("/target")}}});
    server.set("/outside", {.status = 302, .headers = {{"Location", "http://updates.example.invalid/x"}}});
    server.set("/loop", {.status = 307, .headers = {{"Location", "/loop"}}});
    server.set("/nowhere", {.status = 302});
    Expected<HttpResult> result = client->get(
        server.url("/relative"), 1024, [](std::span<const std::byte>) { return ok(); }, {});
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->final_url, server.url("/target"));
    Expected<std::string> body = get("/absolute");
    ASSERT_TRUE(body) << body.error().describe();
    EXPECT_EQ(*body, "arrived");

    // Leaving the machine is refused before anything is sent there.
    Expected<std::string> outside = get("/outside");
    ASSERT_FALSE(outside);
    EXPECT_EQ(outside.error().code, ErrorCode::PermissionDenied);
    EXPECT_EQ(outside.error().where, "app.update.url");
    Expected<std::string> loop = get("/loop");
    ASSERT_FALSE(loop);
    EXPECT_EQ(loop.error().code, ErrorCode::Corrupt);
    EXPECT_EQ(server.request_count("/loop"), 6u) << "the first request and five redirects";
    Expected<std::string> nowhere = get("/nowhere");
    ASSERT_FALSE(nowhere);
    EXPECT_EQ(nowhere.error().code, ErrorCode::Corrupt);
}

TEST_F(HttpClientTest, TheHttpsOnlyPolicyRefusesPlainHttpWithoutConnecting) {
    HttpClientOptions options = loopback_options();
    options.policy = UrlPolicy::HttpsOnly;
    std::unique_ptr<HttpClient> https_only = loopback_client(options);
    ASSERT_NE(https_only, nullptr);
    server.set("/x", {.body = "x"});
    Expected<std::string> refused = http_get_text(*https_only, server.url("/x"), 1024, {});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::PermissionDenied);
    EXPECT_EQ(refused.error().where, "app.update.url");
    EXPECT_EQ(server.request_count("/x"), 0u);
}

TEST_F(HttpClientTest, ARefusedConnectionAndASlowServerAreNetworkErrors) {
    // A port nobody listens on: the server's own port after it is gone.
    std::uint16_t closed_port = 0;
    {
        LoopbackHttpServer gone;
        closed_port = gone.port();
    }
    Expected<std::string> refused =
        http_get_text(*client, std::format("http://127.0.0.1:{}/x", closed_port), 1024, {});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::IoError) << refused.error().describe();
    EXPECT_EQ(refused.error().where, "app.update.network");

    HttpClientOptions options = loopback_options();
    options.receive_timeout = std::chrono::milliseconds(500);
    std::unique_ptr<HttpClient> impatient = loopback_client(options);
    ASSERT_NE(impatient, nullptr);
    server.set("/slow", {.body = "late", .header_delay = std::chrono::milliseconds(3000)});
    const auto started = std::chrono::steady_clock::now();
    Expected<std::string> slow = http_get_text(*impatient, server.url("/slow"), 1024, {});
    ASSERT_FALSE(slow);
    EXPECT_EQ(slow.error().code, ErrorCode::Timeout) << slow.error().describe();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(2500));
}

TEST_F(HttpClientTest, CancellationStopsAStalledTransferPromptly) {
    server.set("/stall", {.body = pattern(256 * 1024), .stall_at = 100 * 1024, .stall = std::chrono::milliseconds(20000)});
    core::CancellationSource source;
    std::uint64_t received = 0;
    std::thread canceller;
    const auto started = std::chrono::steady_clock::now();
    Expected<HttpResult> result = client->get(
        server.url("/stall"), 1 << 20,
        [&](std::span<const std::byte> chunk) -> Status {
            received += chunk.size();
            if (received >= 64 * 1024 && !canceller.joinable()) {
                canceller = std::thread([&source] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    source.cancel();
                });
            }
            return ok();
        },
        source.token());
    if (canceller.joinable()) {
        canceller.join();
    }
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Cancelled) << result.error().describe();
    EXPECT_GE(received, 64u * 1024u);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(5000));

    // Cancelled before it starts: nothing is sent.
    core::CancellationSource early;
    early.cancel();
    Expected<std::string> never = get("/stall", 1 << 20, early.token());
    ASSERT_FALSE(never);
    EXPECT_EQ(never.error().code, ErrorCode::Cancelled);
    EXPECT_EQ(server.request_count("/stall"), 1u);
}

TEST_F(HttpClientTest, ASinkErrorStopsTheTransfer) {
    server.set("/data", {.body = pattern(200 * 1024)});
    Expected<HttpResult> result = client->get(
        server.url("/data"), 1 << 20,
        [](std::span<const std::byte>) -> Status { return fail(ErrorCode::DiskFull, "the disk is full", "test.sink"); },
        {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::DiskFull);
    EXPECT_EQ(result.error().where, "test.sink");
}

}  // namespace
}  // namespace lucent::app
