#pragma once

// A minimal HTTP/1.1 server on 127.0.0.1 for the update tests (ADR 0022: tests never contact the
// internet). It binds an ephemeral port, answers GET requests with registered responses (404 for
// anything else), one request per connection ("Connection: close"), each connection on its own
// thread, and records every path requested. Responses can delay their headers, stall part-way
// through the body, announce a Content-Length that differs from the body, or redirect.
//
// Thread-safety: set(), requests() and request_count() may be called while requests are served.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace lucent::app::test {

class LoopbackHttpServer {
public:
    struct Response {
        int status = 200;
        std::string body;
        std::vector<std::pair<std::string, std::string>> headers;  // extra response headers
        bool send_length = true;                       // send Content-Length
        std::optional<std::uint64_t> announced_length;  // a Content-Length other than the body's size
        std::chrono::milliseconds header_delay{0};     // before the status line
        std::uint64_t stall_at = UINT64_MAX;           // body offset where the response stalls
        std::chrono::milliseconds stall{0};            // how long it stalls (until stopped at most)
    };

    LoopbackHttpServer();
    ~LoopbackHttpServer();
    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    [[nodiscard]] bool listening() const noexcept { return listening_; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    // "http://127.0.0.1:<port><path>"
    [[nodiscard]] std::string url(std::string_view path) const;

    void set(const std::string& path, Response response);
    [[nodiscard]] std::vector<std::string> requests() const;
    [[nodiscard]] std::size_t request_count(std::string_view path) const;

private:
    void accept_loop();
    void serve(std::uintptr_t connection);
    bool sleep_unless_stopped(std::chrono::milliseconds duration) const;

    std::uintptr_t listener_;
    std::uint16_t port_ = 0;
    bool listening_ = false;
    std::atomic<bool> stopping_{false};
    mutable std::mutex mutex_;
    std::map<std::string, Response, std::less<>> responses_;
    std::vector<std::string> requests_;
    std::vector<std::thread> connections_;
    std::thread acceptor_;
};

}  // namespace lucent::app::test
