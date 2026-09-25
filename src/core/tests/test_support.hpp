#pragma once

// Helpers shared by lucent_core's unit tests.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <windows.h>

namespace lucent::core::test_support {

// "\\?\" form of an absolute drive path, so helpers work beyond MAX_PATH.
inline std::wstring verbatim(const std::filesystem::path& path) {
    const std::wstring& native = path.native();
    if (native.starts_with(L"\\\\?\\")) {
        return native;
    }
    return L"\\\\?\\" + native;
}

// A unique, initially empty directory under %TEMP%\lucent-core-tests, removed with its
// contents on destruction. Unique across the parallel test processes CTest starts.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        std::random_device random;
        const std::filesystem::path base = std::filesystem::temp_directory_path() / L"lucent-core-tests";
        for (int attempt = 0; attempt < 64; ++attempt) {
            std::error_code error;
            std::filesystem::create_directories(base, error);
            const std::filesystem::path candidate =
                base / std::format(L"{}-{}-{:08x}", ::GetCurrentProcessId(), counter++, random());
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
        }
        throw std::runtime_error("cannot create a temporary test directory");
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path(verbatim(path_)), error);
        // Remove the shared parent too when no other test process is using it.
        std::filesystem::remove(path_.parent_path(), error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path operator/(const std::filesystem::path& name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

// Creates or truncates `path` and writes `bytes` (works for long paths).
[[nodiscard]] inline bool write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    const HANDLE handle =
        ::CreateFileW(verbatim(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = true;
    std::size_t offset = 0;
    while (ok && offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
        DWORD written = 0;
        ok = ::WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) && written == chunk;
        offset += chunk;
    }
    ::CloseHandle(handle);
    return ok;
}

[[nodiscard]] inline bool write_file(const std::filesystem::path& path, std::string_view text) {
    return write_file(path, std::as_bytes(std::span(text.data(), text.size())));
}

// Reads a whole file with the standard library (independent of the code under test).
[[nodiscard]] inline std::string read_text(const std::filesystem::path& path) {
    const HANDLE handle = ::CreateFileW(verbatim(path).c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string text;
    char buffer[4096];
    DWORD read = 0;
    while (::ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        text.append(buffer, read);
    }
    ::CloseHandle(handle);
    return text;
}

// Deterministic pseudo-random bytes (fixed seed per call site).
[[nodiscard]] inline std::vector<std::byte> pseudo_random_bytes(std::size_t count, std::uint32_t seed) {
    std::mt19937 engine(seed);
    std::vector<std::byte> bytes(count);
    for (std::byte& b : bytes) {
        b = static_cast<std::byte>(engine() & 0xFFu);
    }
    return bytes;
}

}  // namespace lucent::core::test_support
