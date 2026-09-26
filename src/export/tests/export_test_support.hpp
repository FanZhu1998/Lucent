#pragma once

// Helpers shared by lucent_export's unit tests.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

#include <lucent/image/pixel_buffer.hpp>

namespace lucent::exporter::test_support {

// "\\?\" form of an absolute path so helpers work beyond MAX_PATH.
inline std::filesystem::path verbatim(const std::filesystem::path& path) {
    const std::wstring& native = path.native();
    if (native.starts_with(L"\\\\?\\")) {
        return path;
    }
    return std::filesystem::path(L"\\\\?\\" + native);
}

// A unique, initially empty directory under %TEMP%\lucent-export-tests, removed with its
// contents on destruction.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        std::random_device random;
        const std::filesystem::path base = std::filesystem::temp_directory_path() / L"lucent-export-tests";
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
        std::filesystem::remove_all(verbatim(path_), error);
        std::filesystem::remove(path_.parent_path(), error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path operator/(const std::filesystem::path& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

// Names of the entries directly inside `directory`, sorted.
inline std::vector<std::wstring> list_directory(const std::filesystem::path& directory) {
    std::vector<std::wstring> names;
    for (const auto& entry : std::filesystem::directory_iterator(verbatim(directory))) {
        names.push_back(entry.path().filename().native());
    }
    std::sort(names.begin(), names.end());
    return names;
}

inline std::vector<char> read_all(const std::filesystem::path& path) {
    std::ifstream in(verbatim(path), std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void write_all(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(verbatim(path), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// A float RGB image filled by f(x, y, channel).
template <class F>
image::ImageBuffer<float> make_rgb(std::int32_t width, std::int32_t height, F f) {
    auto buffer = image::ImageBuffer<float>::create(width, height, 3);
    if (!buffer) {
        throw std::runtime_error("cannot allocate a test image");
    }
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                buffer->at(x, y, c) = f(x, y, c);
            }
        }
    }
    return std::move(*buffer);
}

// A smooth, colourful display-referred test image with values in [0, 1] (compresses well and
// has little high-frequency content, so JPEG error stays small).
inline image::ImageBuffer<float> make_gradient(std::int32_t width, std::int32_t height) {
    return make_rgb(width, height, [width, height](std::int32_t x, std::int32_t y, std::int32_t c) {
        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
        switch (c) {
            case 0:
                return u * u;
            case 1:
                return v * v * 0.8f + 0.1f;
            default:
                return 0.5f * (1.0f - u) * v + 0.05f;
        }
    });
}

}  // namespace lucent::exporter::test_support
