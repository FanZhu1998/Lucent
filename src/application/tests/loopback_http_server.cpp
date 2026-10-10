#include "loopback_http_server.hpp"

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on

#include <algorithm>
#include <format>

namespace lucent::app::test {

namespace {

std::string_view reason_of(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 307:
            return "Temporary Redirect";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 500:
            return "Internal Server Error";
        default:
            return "Status";
    }
}

bool send_all(SOCKET socket, std::string_view data) {
    while (!data.empty()) {
        const int sent = ::send(socket, data.data(), static_cast<int>(std::min<std::size_t>(data.size(), 65536)), 0);
        if (sent <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

}  // namespace

LoopbackHttpServer::LoopbackHttpServer() : listener_(static_cast<std::uintptr_t>(INVALID_SOCKET)) {
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return;
    }
    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    int length = sizeof(address);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, SOMAXCONN) != 0 ||
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::closesocket(listener);
        return;
    }
    listener_ = static_cast<std::uintptr_t>(listener);
    port_ = ntohs(address.sin_port);
    listening_ = true;
    acceptor_ = std::thread([this] { accept_loop(); });
}

LoopbackHttpServer::~LoopbackHttpServer() {
    stopping_ = true;
    if (listening_) {
        ::closesocket(static_cast<SOCKET>(listener_));  // ends accept()
    }
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
    std::vector<std::thread> connections;
    {
        const std::lock_guard lock(mutex_);
        connections.swap(connections_);
    }
    for (std::thread& connection : connections) {
        connection.join();
    }
    ::WSACleanup();
}

std::string LoopbackHttpServer::url(std::string_view path) const {
    return std::format("http://127.0.0.1:{}{}", port_, path);
}

void LoopbackHttpServer::set(const std::string& path, Response response) {
    const std::lock_guard lock(mutex_);
    responses_[path] = std::move(response);
}

std::vector<std::string> LoopbackHttpServer::requests() const {
    const std::lock_guard lock(mutex_);
    return requests_;
}

std::size_t LoopbackHttpServer::request_count(std::string_view path) const {
    const std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::count(requests_.begin(), requests_.end(), path));
}

bool LoopbackHttpServer::sleep_unless_stopped(std::chrono::milliseconds duration) const {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        if (stopping_) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !stopping_;
}

void LoopbackHttpServer::accept_loop() {
    for (;;) {
        const SOCKET connection = ::accept(static_cast<SOCKET>(listener_), nullptr, nullptr);
        if (connection == INVALID_SOCKET) {
            return;  // the listener was closed
        }
        // A client that connects and sends nothing cannot hold a connection thread forever.
        const DWORD timeout_ms = 10000;
        ::setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        const std::lock_guard lock(mutex_);
        if (stopping_) {
            ::closesocket(connection);
            return;
        }
        connections_.emplace_back([this, connection] { serve(static_cast<std::uintptr_t>(connection)); });
    }
}

void LoopbackHttpServer::serve(std::uintptr_t handle) {
    const SOCKET connection = static_cast<SOCKET>(handle);
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
        const int received = ::recv(connection, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            ::closesocket(connection);
            return;
        }
        request.append(buffer, static_cast<std::size_t>(received));
    }
    // "GET <path> HTTP/1.1"
    std::string path;
    const std::size_t first_space = request.find(' ');
    const std::size_t second_space = first_space == std::string::npos ? std::string::npos : request.find(' ', first_space + 1);
    if (second_space != std::string::npos) {
        path = request.substr(first_space + 1, second_space - first_space - 1);
    }
    Response response;
    {
        const std::lock_guard lock(mutex_);
        requests_.push_back(path);
        const auto found = responses_.find(path);
        if (found != responses_.end()) {
            response = found->second;
        } else {
            response.status = 404;
            response.body = "not found";
        }
    }
    if (response.header_delay.count() > 0 && !sleep_unless_stopped(response.header_delay)) {
        ::closesocket(connection);
        return;
    }
    std::string head = std::format("HTTP/1.1 {} {}\r\n", response.status, reason_of(response.status));
    if (response.send_length) {
        head += std::format("Content-Length: {}\r\n", response.announced_length.value_or(response.body.size()));
    }
    for (const auto& [name, value] : response.headers) {
        head += std::format("{}: {}\r\n", name, value);
    }
    head += "Connection: close\r\n\r\n";
    bool alive = send_all(connection, head);
    if (alive) {
        const std::string_view body = response.body;
        const std::size_t stall_at = static_cast<std::size_t>(std::min<std::uint64_t>(response.stall_at, body.size()));
        alive = send_all(connection, body.substr(0, stall_at));
        if (alive && stall_at < body.size()) {
            alive = sleep_unless_stopped(response.stall) && send_all(connection, body.substr(stall_at));
        } else if (alive && response.stall_at != UINT64_MAX && response.stall.count() > 0) {
            sleep_unless_stopped(response.stall);
        }
    }
    ::shutdown(connection, SD_SEND);
    ::closesocket(connection);
}

}  // namespace lucent::app::test
