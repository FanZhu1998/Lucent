#include "lucent/application/http_client.hpp"

// clang-format off: <winhttp.h> needs the types of <windows.h>.
#include <windows.h>
#include <winhttp.h>
// clang-format on

#include <algorithm>
#include <array>
#include <charconv>
#include <cwctype>
#include <format>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include <lucent/core/text.hpp>
#include <lucent/core/version.hpp>
#include <lucent/core/win32_error.hpp>

// Older SDKs lack the TLS 1.3 flag; its value is fixed by WinHTTP.
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

namespace lucent::app {

namespace {

constexpr std::string_view kWhereUrl = "app.update.url";
constexpr std::string_view kWhereNetwork = "app.update.network";
constexpr std::string_view kWhereTls = "app.update.tls";
constexpr std::string_view kWhereHttp = "app.update.http";

constexpr std::size_t kChunkBytes = 64 * 1024;
constexpr DWORD kPollMilliseconds = 50;
// How long a closed request may take to be released by WinHTTP before its state is leaked rather
// than freed under a callback (never observed; a safety net, not a timeout of the transfer).
constexpr DWORD kCloseWaitMilliseconds = 30000;

// ---- URLs ---------------------------------------------------------------------------------------

struct ParsedUrl {
    bool secure = false;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path;  // path and query, never empty
};

std::wstring lower(std::wstring text) {
    for (wchar_t& c : text) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return text;
}

bool is_loopback_host(const std::wstring& host) {
    const std::wstring h = lower(host);
    return h == L"127.0.0.1" || h == L"::1" || h == L"[::1]";
}

Expected<ParsedUrl> parse_url(std::string_view url, UrlPolicy policy) {
    if (url.empty() || url.size() > 2048) {
        return fail(ErrorCode::InvalidArgument, "an update URL has 1 to 2048 characters", std::string(kWhereUrl));
    }
    for (const char c : url) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7F) {
            return fail(ErrorCode::InvalidArgument, "an update URL has printable ASCII characters only",
                        std::string(kWhereUrl));
        }
    }
    // Fragments are never sent.
    const std::string_view without_fragment = url.substr(0, url.find('#'));
    Expected<std::wstring> wide = core::utf8_to_wide(without_fragment);
    if (!wide) {
        return fail(ErrorCode::InvalidArgument, "the URL is not text", std::string(kWhereUrl));
    }
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUserNameLength = static_cast<DWORD>(-1);
    parts.dwPasswordLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(wide->c_str(), static_cast<DWORD>(wide->size()), 0, &parts)) {
        return fail(ErrorCode::InvalidArgument, std::format("'{}' is not an absolute http(s) URL", url),
                    std::string(kWhereUrl), static_cast<std::int64_t>(::GetLastError()));
    }
    ParsedUrl parsed;
    if (parts.nScheme == INTERNET_SCHEME_HTTPS) {
        parsed.secure = true;
    } else if (parts.nScheme != INTERNET_SCHEME_HTTP) {
        return fail(ErrorCode::PermissionDenied, std::format("'{}' is neither https nor http", url),
                    std::string(kWhereUrl));
    }
    if (parts.dwUserNameLength > 0 || parts.dwPasswordLength > 0) {
        return fail(ErrorCode::PermissionDenied, "update URLs never carry user names or passwords",
                    std::string(kWhereUrl));
    }
    parsed.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    if (parsed.host.empty()) {
        return fail(ErrorCode::InvalidArgument, std::format("'{}' names no host", url), std::string(kWhereUrl));
    }
    parsed.port = parts.nPort;
    parsed.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    parsed.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (parsed.path.empty()) {
        parsed.path = L"/";
    }
    switch (policy) {
        case UrlPolicy::HttpsOnly:
            if (!parsed.secure) {
                return fail(ErrorCode::PermissionDenied,
                            std::format("'{}' is not an https URL; updates are fetched over https only", url),
                            std::string(kWhereUrl));
            }
            break;
        case UrlPolicy::LoopbackOnly:
            if (!is_loopback_host(parsed.host)) {
                return fail(ErrorCode::PermissionDenied,
                            std::format("'{}' is not on this computer (127.0.0.1 or ::1); this client only fetches "
                                        "loopback URLs",
                                        url),
                            std::string(kWhereUrl));
            }
            break;
    }
    return parsed;
}

// `location` (a Location header) resolved against `base`: absolute URLs as they are, absolute paths
// ("/x") on the same scheme, host and port. Anything else is refused.
Expected<std::string> resolve_location(std::string_view base, std::string_view location) {
    if (location.starts_with("https://") || location.starts_with("http://")) {
        return std::string(location);
    }
    if (location.starts_with("/") && !location.starts_with("//")) {
        const std::size_t scheme_end = base.find("://");
        if (scheme_end == std::string_view::npos) {
            return fail(ErrorCode::Corrupt, "cannot resolve a redirect", std::string(kWhereHttp));
        }
        const std::size_t authority_end = base.find('/', scheme_end + 3);
        return std::string(base.substr(0, authority_end)) + std::string(location);
    }
    return fail(ErrorCode::Corrupt, std::format("the server redirected to '{}', which is not a usable address", location),
                std::string(kWhereHttp));
}

// ---- WinHTTP handles ----------------------------------------------------------------------------

class InternetHandle {
public:
    InternetHandle() noexcept = default;
    explicit InternetHandle(HINTERNET handle) noexcept : handle_(handle) {}
    ~InternetHandle() { reset(); }
    InternetHandle(InternetHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }
    void reset() noexcept {
        if (handle_ != nullptr) {
            ::WinHttpCloseHandle(handle_);
            handle_ = nullptr;
        }
    }

private:
    HINTERNET handle_ = nullptr;
};

Error winhttp_error(DWORD code, std::string_view what) {
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT:
            return Error{ErrorCode::Timeout, std::format("{}: the server did not answer in time", what),
                         std::string(kWhereNetwork), code};
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
        case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
        case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
            return Error{ErrorCode::PermissionDenied,
                         std::format("{}: the server's certificate or secure connection was refused ({})", what,
                                     core::win32_message(code)),
                         std::string(kWhereTls), code};
        case ERROR_WINHTTP_OPERATION_CANCELLED:
            return Error{ErrorCode::Cancelled, std::format("{}: cancelled", what), std::string(kWhereNetwork), code};
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return Error{ErrorCode::OutOfMemory, std::format("{}: out of memory", what), std::string(kWhereNetwork), code};
        default:
            return Error{ErrorCode::IoError, std::format("{}: {}", what, core::win32_message(code)),
                         std::string(kWhereNetwork), code};
    }
}

// The completion state of one asynchronous request. Heap-allocated: WinHTTP's callbacks reference it
// until WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING, after which it is freed.
struct RequestState {
    HANDLE completed = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset: one per operation
    HANDLE closed = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);      // manual-reset: handle released
    std::mutex mutex;
    DWORD error = 0;     // of the last operation (0: success)
    DWORD bytes = 0;     // READ_COMPLETE: bytes read

    ~RequestState() {
        if (completed != nullptr) {
            ::CloseHandle(completed);
        }
        if (closed != nullptr) {
            ::CloseHandle(closed);
        }
    }

    void finish(DWORD operation_error, DWORD operation_bytes) {
        {
            const std::lock_guard lock(mutex);
            error = operation_error;
            bytes = operation_bytes;
        }
        ::SetEvent(completed);
    }
};

void CALLBACK on_request_status(HINTERNET, DWORD_PTR context, DWORD status, LPVOID information, DWORD length) {
    auto* state = reinterpret_cast<RequestState*>(context);
    if (state == nullptr) {
        return;
    }
    switch (status) {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            state->finish(0, 0);
            break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            state->finish(0, length);
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
            const auto* result = static_cast<const WINHTTP_ASYNC_RESULT*>(information);
            state->finish(result != nullptr && result->dwError != 0 ? result->dwError : ERROR_WINHTTP_INTERNAL_ERROR, 0);
            break;
        }
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
            ::SetEvent(state->closed);
            break;
        default:
            break;
    }
}

// One open request: its handle and completion state. The destructor closes the handle and waits for
// WinHTTP to release it before freeing the state.
class Request {
public:
    Request() = default;
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    ~Request() { close(); }

    Status open(HINTERNET connection, const ParsedUrl& url) {
        state_ = new (std::nothrow) RequestState();
        if (state_ == nullptr || state_->completed == nullptr || state_->closed == nullptr) {
            return fail(ErrorCode::OutOfMemory, "cannot prepare an update request", std::string(kWhereNetwork));
        }
        handle_ = ::WinHttpOpenRequest(connection, L"GET", url.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0);
        if (handle_ == nullptr) {
            return Unexpected(winhttp_error(::GetLastError(), "opening the request"));
        }
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(state_);
        if (!::WinHttpSetOption(handle_, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
            return Unexpected(winhttp_error(::GetLastError(), "preparing the request"));
        }
        if (::WinHttpSetStatusCallback(handle_, &on_request_status,
                                       WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
                                       0) == WINHTTP_INVALID_STATUS_CALLBACK) {
            return Unexpected(winhttp_error(::GetLastError(), "preparing the request"));
        }
        watched_ = true;  // from now on WinHTTP reports this handle's closing to the state
        // Redirects are followed by get() itself, checking each target against the policy.
        DWORD feature = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        if (!::WinHttpSetOption(handle_, WINHTTP_OPTION_DISABLE_FEATURE, &feature, sizeof(feature))) {
            return Unexpected(winhttp_error(::GetLastError(), "preparing the request"));
        }
        return ok();
    }

    [[nodiscard]] HINTERNET handle() const noexcept { return handle_; }
    // The callbacks' context (WinHttpSendRequest passes it on as well).
    [[nodiscard]] DWORD_PTR context() const noexcept { return reinterpret_cast<DWORD_PTR>(state_); }

    // Waits for the operation just started to complete, checking `cancel` every kPollMilliseconds.
    // Returns the bytes it transferred (READ_COMPLETE) or its error.
    Expected<DWORD> wait(const core::CancellationToken& cancel, std::string_view what) {
        for (;;) {
            const DWORD waited = ::WaitForSingleObject(state_->completed, kPollMilliseconds);
            if (waited == WAIT_OBJECT_0) {
                const std::lock_guard lock(state_->mutex);
                if (state_->error != 0) {
                    return Unexpected(winhttp_error(state_->error, what));
                }
                return state_->bytes;
            }
            if (waited != WAIT_TIMEOUT) {
                return fail(ErrorCode::Internal, "waiting for WinHTTP failed", std::string(kWhereNetwork),
                            static_cast<std::int64_t>(::GetLastError()));
            }
            if (cancel.is_cancelled()) {
                close();  // the operation in progress ends; the state lives until WinHTTP released it
                return fail(ErrorCode::Cancelled, std::format("{}: cancelled", what), std::string(kWhereNetwork));
            }
        }
    }

    void close() noexcept {
        if (handle_ != nullptr) {
            ::WinHttpCloseHandle(handle_);
            handle_ = nullptr;
            if (watched_ && state_ != nullptr &&
                ::WaitForSingleObject(state_->closed, kCloseWaitMilliseconds) != WAIT_OBJECT_0) {
                state_ = nullptr;  // still referenced by WinHTTP: leaked rather than freed under a callback
            }
        }
        delete state_;
        state_ = nullptr;
    }

private:
    HINTERNET handle_ = nullptr;
    RequestState* state_ = nullptr;
    bool watched_ = false;  // the callback with the state as context is installed
};

std::optional<std::wstring> query_header_text(HINTERNET request, DWORD info) {
    DWORD size = 0;
    ::WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return std::nullopt;
    }
    std::wstring text(size / sizeof(wchar_t) + 1, L'\0');
    if (!::WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &size,
                               WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }
    text.resize(size / sizeof(wchar_t));
    return text;
}

Error status_error(DWORD status, std::string_view url) {
    const std::string message = std::format("the server answered {} for {}", status, url);
    if (status == 404 || status == 410) {
        return Error{ErrorCode::NotFound, message, std::string(kWhereHttp), status};
    }
    if (status == 401 || status == 403) {
        return Error{ErrorCode::PermissionDenied, message, std::string(kWhereHttp), status};
    }
    return Error{ErrorCode::IoError, message, std::string(kWhereHttp), status};
}

bool is_redirect(DWORD status) noexcept {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

class WinHttpClient final : public HttpClient {
public:
    WinHttpClient(InternetHandle session, HttpClientOptions options)
        : session_(std::move(session)), options_(std::move(options)) {}

    Expected<HttpResult> get(std::string_view url, std::uint64_t max_bytes, const HttpSink& sink,
                             core::CancellationToken cancel) override {
        try {
            std::string current(url);
            for (std::uint32_t hop = 0;; ++hop) {
                if (Status cancelled = cancel.check(kWhereNetwork); !cancelled) {
                    return Unexpected(std::move(cancelled.error()));
                }
                Expected<ParsedUrl> parsed = parse_url(current, options_.policy);
                if (!parsed) {
                    return Unexpected(std::move(parsed.error()));
                }
                InternetHandle connection(::WinHttpConnect(session_.get(), parsed->host.c_str(), parsed->port, 0));
                if (!connection) {
                    return Unexpected(winhttp_error(::GetLastError(), "connecting to the update server"));
                }
                Request request;
                if (Status opened = request.open(connection.get(), *parsed); !opened) {
                    return Unexpected(std::move(opened.error()));
                }
                static constexpr wchar_t kHeaders[] = L"Cache-Control: no-cache\r\nPragma: no-cache\r\n";
                if (!::WinHttpSendRequest(request.handle(), kHeaders, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA,
                                          0, 0, request.context())) {
                    return Unexpected(winhttp_error(::GetLastError(), "sending the request"));
                }
                if (Expected<DWORD> sent = request.wait(cancel, "sending the request"); !sent) {
                    return Unexpected(std::move(sent.error()));
                }
                if (!::WinHttpReceiveResponse(request.handle(), nullptr)) {
                    return Unexpected(winhttp_error(::GetLastError(), "receiving the answer"));
                }
                if (Expected<DWORD> received = request.wait(cancel, "receiving the answer"); !received) {
                    return Unexpected(std::move(received.error()));
                }
                DWORD status = 0;
                DWORD size = sizeof(status);
                if (!::WinHttpQueryHeaders(request.handle(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
                    return Unexpected(winhttp_error(::GetLastError(), "reading the answer"));
                }
                if (is_redirect(status)) {
                    if (hop >= options_.max_redirects) {
                        return fail(ErrorCode::Corrupt, std::format("more than {} redirects", options_.max_redirects),
                                    std::string(kWhereHttp), status);
                    }
                    const std::optional<std::wstring> location = query_header_text(request.handle(), WINHTTP_QUERY_LOCATION);
                    if (!location || location->empty()) {
                        return fail(ErrorCode::Corrupt, "a redirect without a Location", std::string(kWhereHttp),
                                    status);
                    }
                    Expected<std::string> next = resolve_location(current, core::wide_to_utf8_lossy(*location));
                    if (!next) {
                        return Unexpected(std::move(next.error()));
                    }
                    current = std::move(*next);
                    continue;  // checked against the policy at the top of the loop
                }
                if (status != 200) {
                    return Unexpected(status_error(status, current));
                }
                std::optional<std::uint64_t> announced;
                if (const std::optional<std::wstring> length =
                        query_header_text(request.handle(), WINHTTP_QUERY_CONTENT_LENGTH)) {
                    const std::string digits = core::wide_to_utf8_lossy(*length);
                    std::uint64_t value = 0;
                    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
                    if (error == std::errc() && end == digits.data() + digits.size()) {
                        announced = value;
                    }
                }
                if (announced && *announced > max_bytes) {
                    return fail(ErrorCode::LimitExceeded,
                                std::format("the server announced {} bytes, more than the {} allowed", *announced,
                                            max_bytes),
                                std::string(kWhereHttp));
                }
                std::vector<std::byte> buffer(kChunkBytes);
                HttpResult result;
                result.final_url = current;
                for (;;) {
                    if (!::WinHttpReadData(request.handle(), buffer.data(), static_cast<DWORD>(buffer.size()), nullptr)) {
                        return Unexpected(winhttp_error(::GetLastError(), "downloading"));
                    }
                    Expected<DWORD> read = request.wait(cancel, "downloading");
                    if (!read) {
                        return Unexpected(std::move(read.error()));
                    }
                    if (*read == 0) {
                        break;
                    }
                    if (result.bytes + *read > max_bytes) {
                        return fail(ErrorCode::LimitExceeded,
                                    std::format("the server sent more than the {} bytes allowed", max_bytes),
                                    std::string(kWhereHttp));
                    }
                    if (Status taken = sink(std::span<const std::byte>(buffer.data(), *read)); !taken) {
                        return Unexpected(std::move(taken.error()));
                    }
                    result.bytes += *read;
                }
                if (announced && result.bytes != *announced) {
                    return fail(ErrorCode::Corrupt,
                                std::format("the server announced {} bytes and sent {}", *announced, result.bytes),
                                std::string(kWhereHttp));
                }
                return result;
            }
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "out of memory while downloading", std::string(kWhereNetwork));
        }
    }

private:
    InternetHandle session_;
    HttpClientOptions options_;
};

DWORD timeout_of(std::chrono::milliseconds value) {
    return static_cast<DWORD>(std::clamp<std::int64_t>(value.count(), 1, 600000));
}

}  // namespace

Status check_url(std::string_view url, UrlPolicy policy) {
    Expected<ParsedUrl> parsed = parse_url(url, policy);
    if (!parsed) {
        return Unexpected(std::move(parsed.error()));
    }
    return ok();
}

Expected<std::string> http_get_text(HttpClient& client, std::string_view url, std::uint64_t max_bytes,
                                    core::CancellationToken cancel) {
    std::string body;
    Expected<HttpResult> result = client.get(
        url, max_bytes,
        [&](std::span<const std::byte> chunk) -> Status {
            body.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
            return ok();
        },
        std::move(cancel));
    if (!result) {
        return Unexpected(std::move(result.error()));
    }
    return body;
}

Expected<std::unique_ptr<HttpClient>> create_winhttp_client(const HttpClientOptions& options) {
    const std::string agent =
        options.user_agent.empty() ? std::format("Lucent/{}", core::build_info().version) : options.user_agent;
    const std::wstring wide_agent = core::utf8_to_wide_lossy(agent);
    InternetHandle session(::WinHttpOpen(wide_agent.c_str(),
                                         options.system_proxy ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
                                                              : WINHTTP_ACCESS_TYPE_NO_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!session && options.system_proxy) {
        // Before Windows 8.1 there is no automatic proxy type: the default (registry) configuration.
        session = InternetHandle(::WinHttpOpen(wide_agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    }
    if (!session) {
        return Unexpected(winhttp_error(::GetLastError(), "opening an HTTP session"));
    }
    if (!::WinHttpSetTimeouts(session.get(), static_cast<int>(timeout_of(options.resolve_timeout)),
                              static_cast<int>(timeout_of(options.connect_timeout)),
                              static_cast<int>(timeout_of(options.send_timeout)),
                              static_cast<int>(timeout_of(options.receive_timeout)))) {
        return Unexpected(winhttp_error(::GetLastError(), "setting the timeouts"));
    }
    // TLS 1.2 and 1.3; Windows versions whose WinHTTP has no TLS 1.3 refuse that flag.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!::WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        if (!::WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            return Unexpected(winhttp_error(::GetLastError(), "selecting TLS 1.2 or later"));
        }
    }
    try {
        return std::unique_ptr<HttpClient>(new WinHttpClient(std::move(session), options));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot create the HTTP client", std::string(kWhereNetwork));
    }
}

}  // namespace lucent::app
