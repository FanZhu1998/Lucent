#pragma once

// HTTP GET for the update channel (ADR 0022; blueprint section 8: no network connection is needed for
// opening, editing or exporting; the channel is contacted only when the photographer asks).
//
// The WinHTTP client (create_winhttp_client) fetches one URL at a time on the caller's thread:
//   * URL policy, checked for the first URL and for every redirect before anything is sent:
//     UrlPolicy::HttpsOnly (Lucent.exe) accepts only https URLs; UrlPolicy::LoopbackOnly (tests)
//     accepts only URLs whose host is 127.0.0.1 or ::1, so a test can never reach another machine.
//     A URL with user information ("user:password@") is refused under both;
//   * TLS 1.2 or later (TLS 1.3 where Windows offers it) and WinHTTP's certificate validation
//     against the machine's trusted roots, never relaxed;
//   * redirects are followed by the client itself (WinHTTP's own following is disabled), at most
//     HttpClientOptions::max_redirects, each target checked against the policy, so an https
//     address can never lead to an http one;
//   * proxies: the system's automatic proxy configuration (WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY), or
//     direct connections (tests);
//   * timeouts per step (name resolution, connection, sending, each read);
//   * the body is streamed to a sink in chunks of at most 64 KiB; more than `max_bytes` stops the
//     transfer (LimitExceeded), as does a body shorter than the Content-Length the server announced
//     (Corrupt);
//   * no cache: requests carry "Cache-Control: no-cache", and no cookies or credentials are sent.
// Cancellation: the token is checked at least every 50 ms while WinHTTP works; a cancelled request's
// handle is closed (WinHTTP's asynchronous mode, so closing never races a call in progress) and get()
// returns Cancelled once WinHTTP has released it.
//
// Errors (Error::where):
//   "app.update.url"      PermissionDenied: a URL the policy refuses; InvalidArgument: not a URL
//   "app.update.network"  IoError: the name did not resolve, the connection failed or broke;
//                         Timeout: a step outlasted its timeout
//   "app.update.tls"      PermissionDenied: the server's certificate or TLS connection was refused
//   "app.update.http"     the server's answer: NotFound (404, 410), PermissionDenied (401, 403),
//                         IoError (any other status but 200; native_code holds it), LimitExceeded (a
//                         larger body than allowed), Corrupt (a body shorter than announced, a
//                         redirect without a usable Location, too many redirects)
//   and the sink's own errors, and Cancelled.
//
// Thread-safety: one client may serve concurrent get() calls (each has its own WinHTTP request).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>

namespace lucent::app {

enum class UrlPolicy : std::uint8_t {
    HttpsOnly = 1,     // https only (Lucent.exe)
    LoopbackOnly = 2,  // http or https to 127.0.0.1 or ::1 only (tests)
};

// ok() when `url` may be fetched under `policy` (see the file comment).
[[nodiscard]] Status check_url(std::string_view url, UrlPolicy policy);

struct HttpClientOptions {
    UrlPolicy policy = UrlPolicy::HttpsOnly;
    // The system's automatic proxy configuration; false: direct connections (tests).
    bool system_proxy = true;
    std::chrono::milliseconds resolve_timeout{15000};
    std::chrono::milliseconds connect_timeout{15000};
    std::chrono::milliseconds send_timeout{30000};
    std::chrono::milliseconds receive_timeout{30000};  // for the response headers and each read
    std::uint32_t max_redirects = 5;
    std::string user_agent;  // empty: "Lucent/<version>"
};

struct HttpResult {
    std::string final_url;   // the URL that answered (after redirects)
    std::uint64_t bytes = 0; // body bytes delivered to the sink
};

// Receives the body in order. An error stops the transfer and is returned by get().
using HttpSink = std::function<Status(std::span<const std::byte> chunk)>;

class HttpClient {
public:
    virtual ~HttpClient() = default;

    // GETs `url` and streams its body (status 200) to `sink`; see the file comment.
    [[nodiscard]] virtual Expected<HttpResult> get(std::string_view url, std::uint64_t max_bytes, const HttpSink& sink,
                                                   core::CancellationToken cancel) = 0;
};

// The body of `url` in memory (at most `max_bytes`).
[[nodiscard]] Expected<std::string> http_get_text(HttpClient& client, std::string_view url, std::uint64_t max_bytes,
                                                  core::CancellationToken cancel);

// Errors ("app.update.network"): IoError when WinHTTP cannot open a session.
[[nodiscard]] Expected<std::unique_ptr<HttpClient>> create_winhttp_client(const HttpClientOptions& options = {});

}  // namespace lucent::app
