#pragma once

// Helpers shared by the decoder tests: scratch directories, file writing and fixture lookup.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

namespace lucent::raw::test_support {

// Verbatim ("\\?\"-prefixed) form of an absolute path, so that Win32 calls work beyond MAX_PATH.
inline std::wstring verbatim(const std::filesystem::path& path) {
    constexpr std::wstring_view kPrefix = LR"(\\?\)";
    const std::wstring native = std::filesystem::absolute(path).wstring();
    return native.starts_with(kPrefix) ? native : std::wstring(kPrefix) + native;
}

// Creates `path` and its missing parents (any length).
inline void make_directories(const std::filesystem::path& path) {
    std::filesystem::path partial;
    for (const auto& part : std::filesystem::absolute(path)) {
        partial /= part;
        if (!partial.has_relative_path()) {
            continue;  // drive or root: exists
        }
        if (!::CreateDirectoryW(verbatim(partial).c_str(), nullptr)) {
            const DWORD error = ::GetLastError();
            EXPECT_TRUE(error == ERROR_ALREADY_EXISTS || error == ERROR_ACCESS_DENIED)
                << "cannot create " << partial.string() << ": " << error;
        }
    }
}

// A fresh directory under %TEMP%\lucent-raw-tests, removed (with its contents) on destruction.
// The shared lucent-raw-tests directory itself is left in place: ctest runs test processes in
// parallel, and removing it when it happens to be empty raced with another process creating its
// own scratch directory inside it (CreateDirectoryW then failed with ERROR_PATH_NOT_FOUND).
class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<unsigned> counter{0};
        const auto base = std::filesystem::temp_directory_path() / L"lucent-raw-tests";
        path_ =
            base / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(counter.fetch_add(1)));
        make_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(verbatim(path_)), ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    // Writes `bytes` to <dir>/<name> and returns the path.
    std::filesystem::path write(const std::filesystem::path& name, std::span<const std::byte> bytes) const {
        const auto target = path_ / name;
        make_directories(target.parent_path());
        std::ofstream out(std::filesystem::path(verbatim(target)), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        EXPECT_TRUE(out.good()) << "cannot write " << target.string();
        return target;
    }

    std::filesystem::path write_text(const std::filesystem::path& name, const std::string& text) const {
        return write(name, std::as_bytes(std::span<const char>(text.data(), text.size())));
    }

private:
    std::filesystem::path path_;
};

inline std::vector<std::byte> read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::memcpy(bytes.data(), chars.data(), chars.size());
    return bytes;
}

// The cached file of fixture `id` (fixtures/cache/<id>/<file>), or std::nullopt when it has not
// been downloaded (tools\fetch-fixtures.ps1). Each cache directory holds exactly one file.
inline std::optional<std::filesystem::path> fixture_file(const std::string& id) {
    const std::filesystem::path dir = std::filesystem::path(LUCENT_FIXTURE_DIR) / "cache" / id;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return std::nullopt;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() != L".partial") {
            return entry.path();
        }
    }
    return std::nullopt;
}

}  // namespace lucent::raw::test_support

// Skips the current test when fixture `id` is not cached.
#define LUCENT_REQUIRE_FIXTURE(var, id)                                                              \
    const auto var##_optional = ::lucent::raw::test_support::fixture_file(id);                       \
    if (!var##_optional) {                                                                           \
        GTEST_SKIP() << "fixture '" << (id) << "' is not downloaded; run tools\\fetch-fixtures.ps1"; \
    }                                                                                                \
    const std::filesystem::path var = *var##_optional
