#pragma once

// Helpers shared by the repository implementation files (repository.cpp, jobs.cpp), private to
// lucent_catalog: SQL assembly, parameter binding, input validation, row decoding and the
// active-transaction check. Every value reaches SQLite as a bound parameter.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "lucent/catalog/location.hpp"
#include "lucent/catalog/types.hpp"
#include "lucent/core/error.hpp"
#include "lucent/core/hash.hpp"
#include "lucent/core/text.hpp"

#include "codec.hpp"
#include "sqlite.hpp"

namespace lucent::catalog::detail {

inline std::string sql(std::initializer_list<std::string_view> parts) {
    std::string text;
    for (const std::string_view part : parts) {
        text.append(part);
    }
    return text;
}

// A bound parameter of dynamically assembled queries.
using Parameter = std::variant<std::int64_t, std::string>;

inline void bind_all(Statement& statement, const std::vector<Parameter>& parameters) {
    int index = 1;
    for (const Parameter& parameter : parameters) {
        if (const auto* number = std::get_if<std::int64_t>(&parameter)) {
            statement.bind_int64(index, *number);
        } else {
            statement.bind_text(index, std::get<std::string>(parameter));
        }
        ++index;
    }
}

inline std::span<const std::uint8_t> bytes_of(const core::Digest256& digest) noexcept {
    return digest;
}
inline std::span<const std::uint8_t> bytes_of(const std::array<std::uint8_t, 16>& id) noexcept {
    return id;
}

inline Status invalid(std::string_view where, std::string message) {
    return fail(ErrorCode::InvalidArgument, std::move(message), std::string(where));
}

inline Status check_limit(std::uint32_t limit, std::string_view where) {
    if (limit < 1 || limit > kMaxPageSize) {
        return invalid(where, std::format("limit must be 1 ... {}", kMaxPageSize));
    }
    return ok();
}

inline Status check_text(std::string_view text, std::string_view field, std::string_view where) {
    if (!core::is_valid_utf8(text)) {
        return invalid(where, std::format("{} is not valid UTF-8", field));
    }
    return ok();
}

inline Expected<std::string> path_text(const std::filesystem::path& path, std::string_view where) {
    Expected<std::string> text = core::path_to_utf8(path);
    if (!text) {
        return fail(ErrorCode::InvalidArgument,
                    "path is not valid UTF-16: '" + core::path_to_utf8_lossy(path) + "'", std::string(where));
    }
    return text;
}

inline Expected<std::string> key_of(const std::filesystem::path& path, std::string_view where) {
    Expected<std::string> key = make_path_key(path);
    if (!key) {
        Error error = std::move(key.error());
        error.where = std::string(where);
        return Unexpected(std::move(error));
    }
    return key;
}

// Errors for stored values this build cannot interpret.
class RowDecoder {
public:
    RowDecoder(bool newer_schema, std::string_view where) noexcept
        : newer_schema_(newer_schema), where_(where) {}

    [[nodiscard]] Error bad(std::string_view column, std::string_view value) const {
        if (newer_schema_) {
            return Error{ErrorCode::VersionMismatch,
                         std::format("{} holds '{}', a value written by a newer Lucent", column,
                                     core::sanitize_utf8(value)),
                         std::string(where_), 0};
        }
        return Error{ErrorCode::Corrupt,
                     std::format("{} holds an invalid value '{}'", column, core::sanitize_utf8(value)),
                     std::string(where_), 0};
    }

    template <class T>
    [[nodiscard]] Expected<T> parse(std::optional<T> parsed, std::string_view column,
                                    std::string_view value) const {
        if (!parsed) {
            return Unexpected(bad(column, value));
        }
        return *parsed;
    }

    [[nodiscard]] Expected<std::filesystem::path> path(std::string_view column,
                                                       std::string_view value) const {
        Expected<std::filesystem::path> parsed = core::path_from_utf8(value);
        if (!parsed) {
            return Unexpected(bad(column, value));
        }
        return parsed;
    }

    template <std::size_t N>
    [[nodiscard]] Expected<std::array<std::uint8_t, N>> blob(std::string_view column,
                                                             std::span<const std::uint8_t> value) const {
        std::array<std::uint8_t, N> out{};
        if (value.size() != N) {
            return Unexpected(bad(column, std::format("<{} bytes>", value.size())));
        }
        std::copy(value.begin(), value.end(), out.begin());
        return out;
    }

    [[nodiscard]] Expected<std::uint32_t> u32(std::string_view column, std::int64_t value) const {
        if (value < 0 || value > std::numeric_limits<std::uint32_t>::max()) {
            return Unexpected(bad(column, std::to_string(value)));
        }
        return static_cast<std::uint32_t>(value);
    }

private:
    bool newer_schema_;
    std::string_view where_;
};

inline Expected<core::Digest256> sha256_of(std::string_view text) {
    return core::sha256(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

template <class Record, class Reader>
inline Expected<std::vector<Record>> collect(Statement& statement, const RowDecoder& decode, Reader reader) {
    std::vector<Record> records;
    for (;;) {
        Expected<bool> row = statement.step();
        if (!row) {
            return Unexpected(std::move(row.error()));
        }
        if (!*row) {
            return records;
        }
        Expected<Record> record = reader(statement, decode);
        if (!record) {
            return Unexpected(std::move(record.error()));
        }
        records.push_back(std::move(*record));
    }
}

template <class Record, class Reader>
inline Expected<std::optional<Record>> first(Statement& statement, const RowDecoder& decode, Reader reader) {
    Expected<bool> row = statement.step();
    if (!row) {
        return Unexpected(std::move(row.error()));
    }
    if (!*row) {
        return std::optional<Record>();
    }
    Expected<Record> record = reader(statement, decode);
    if (!record) {
        return Unexpected(std::move(record.error()));
    }
    return std::optional<Record>(std::move(*record));
}

inline Status require_changed(Connection& connection, std::string_view what, std::int64_t id,
                       std::string_view where) {
    if (connection.changes() == 0) {
        return fail(ErrorCode::NotFound, std::format("no {} with id {}", what, id), std::string(where));
    }
    return ok();
}

inline Expected<std::int64_t> count_to_db(std::uint64_t value, std::string_view where) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return fail(ErrorCode::InvalidArgument, "count too large", std::string(where));
    }
    return static_cast<std::int64_t>(value);
}

// Every call runs inside the transaction of read() / write(). SQLite rolls a transaction back on
// its own after some errors (I/O, disk full, out of memory); a later call would then run in
// autocommit mode and commit on its own, so it is refused instead.
inline Status check_active(const Connection* connection, std::string_view where) {
    if (connection == nullptr) {
        return fail(ErrorCode::Internal, "the catalog transaction is no longer active", std::string(where));
    }
    if (!connection->in_transaction()) {
        return fail(ErrorCode::IoError, "the catalog transaction was rolled back after an earlier error",
                    std::string(where));
    }
    return ok();
}

}  // namespace lucent::catalog::detail
