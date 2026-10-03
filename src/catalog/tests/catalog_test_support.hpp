#pragma once

// Helpers shared by lucent_catalog's unit tests: temporary folders, synthetic file identities and
// probe summaries, raw SQL access to a catalog file and status assertions.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "lucent/catalog/catalog.hpp"
#include "lucent/core/text.hpp"

#include "sqlite.hpp"

namespace lucent::catalog::test {

// Formats an unexpected error for assertion messages.
template <class T>
std::string describe(const Expected<T>& value) {
    return value ? std::string("ok") : value.error().describe();
}

#define LUCENT_ASSERT_OK(expr)                                                            \
    do {                                                                                  \
        const auto& lucent_result_ = (expr);                                              \
        ASSERT_TRUE(lucent_result_) << ::lucent::catalog::test::describe(lucent_result_); \
    } while (false)

#define LUCENT_EXPECT_OK(expr)                                                            \
    do {                                                                                  \
        const auto& lucent_result_ = (expr);                                              \
        EXPECT_TRUE(lucent_result_) << ::lucent::catalog::test::describe(lucent_result_); \
    } while (false)

inline std::wstring verbatim(const std::filesystem::path& path) {
    const std::wstring& native = path.native();
    if (native.starts_with(L"\\\\?\\")) {
        return native;
    }
    return L"\\\\?\\" + native;
}

// A unique, initially empty directory under %TEMP%\lucent-catalog-tests, removed with its contents
// on destruction. Unique across the parallel test processes CTest starts.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        std::random_device random;
        const std::filesystem::path base = std::filesystem::temp_directory_path() / L"lucent-catalog-tests";
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

inline OpenOptions options_for(const std::filesystem::path& path) {
    OpenOptions options;
    options.path = path;
    options.app_version = "0.1.0-test";
    return options;
}

inline std::unique_ptr<Catalog> open_or_fail(const std::filesystem::path& path) {
    Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options_for(path));
    if (!catalog) {
        ADD_FAILURE() << catalog.error().describe();
        return nullptr;
    }
    return std::move(*catalog);
}

// Synthetic identity number `n`: unique file id, size and mtime derived from n.
inline core::FileIdentity identity(std::uint64_t n, std::uint64_t volume = 0x1234'5678'9ABC'DEF0ull) {
    core::FileIdentity id;
    id.volume_serial = volume;
    for (int i = 0; i < 8; ++i) {
        id.file_id[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(n >> (8 * i));
    }
    id.file_id[15] = 0x5A;
    id.size_bytes = 1'000'000 + n;
    id.last_write_time = 133'000'000'000'000'000 + static_cast<std::int64_t>(n) * 10'000;
    return id;
}

inline core::Digest256 digest(std::uint8_t seed) {
    core::Digest256 d{};
    for (std::size_t i = 0; i < d.size(); ++i) {
        d[i] = static_cast<std::uint8_t>(seed * 31 + i);
    }
    return d;
}

inline Timestamp at(std::int64_t milliseconds) {
    return Timestamp(std::chrono::milliseconds(milliseconds));
}

inline SourceObservation observation(const std::filesystem::path& path, std::uint64_t n) {
    SourceObservation o;
    o.path = path;
    o.identity = identity(n);
    o.storage_kind = StorageKind::Fixed;
    o.seen_at = at(1'700'000'000'000 + static_cast<std::int64_t>(n));
    return o;
}

inline ItemMetadata item(std::uint32_t subimage = 0, std::string capture_time = "2026-09-01T10:00:00") {
    ItemMetadata m;
    m.subimage_index = subimage;
    m.make = "Canon";
    m.model = "EOS R5";
    m.lens = "RF24-105mm F4 L IS USM";
    if (!capture_time.empty()) {
        m.capture_time = std::move(capture_time);
    }
    m.width = 8192;
    m.height = 5464;
    m.orientation = 1;
    m.format = "cr3";
    m.support_status = SupportStatus::Supported;
    m.support_reasons_json = "[]";
    m.decoder_id = "libraw";
    m.decoder_version = "0.22.0";
    m.probed_at = at(1'700'000'500'000);
    return m;
}

// Raw SQL access to a catalog file, bypassing Catalog (for tampering and inspection).
inline std::unique_ptr<detail::Connection> raw_connection(const std::filesystem::path& path,
                                                          detail::Access access = detail::Access::ReadWrite) {
    Expected<std::unique_ptr<detail::Connection>> connection =
        detail::Connection::open(path, access, std::chrono::milliseconds(5000), "test");
    if (!connection) {
        ADD_FAILURE() << connection.error().describe();
        return nullptr;
    }
    return std::move(*connection);
}

inline std::int64_t query_int(detail::Connection& connection, std::string_view sql) {
    Expected<std::int64_t> value = connection.query_int64(sql, "test");
    if (!value) {
        ADD_FAILURE() << value.error().describe();
        return -1;
    }
    return *value;
}

inline std::string query_text(detail::Connection& connection, std::string_view sql) {
    Expected<std::string> value = connection.query_text(sql, "test");
    if (!value) {
        ADD_FAILURE() << value.error().describe();
        return {};
    }
    return *value;
}

inline std::vector<char> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void write_bytes(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Inserts `count` sources (paths <dir>\IMG_<n>.CR3) with one item each, in one transaction.
inline Status populate(Catalog& catalog, const std::filesystem::path& folder, std::uint64_t first,
                       std::uint64_t count) {
    return catalog.write([&](WriteTransaction& tx) -> Status {
        for (std::uint64_t n = first; n < first + count; ++n) {
            Expected<SourceUpsertResult> source =
                tx.upsert_source(observation(folder / std::format(L"IMG_{:05}.CR3", n), n));
            if (!source) {
                return Unexpected(source.error());
            }
            const ItemMetadata metadata =
                item(0, std::format("2026-09-{:02}T10:{:02}:{:02}", 1 + n % 28, (n / 60) % 60, n % 60));
            Expected<std::vector<ItemId>> items = tx.upsert_items(source->id, std::span(&metadata, 1));
            if (!items) {
                return Unexpected(items.error());
            }
        }
        return ok();
    });
}

}  // namespace lucent::catalog::test
