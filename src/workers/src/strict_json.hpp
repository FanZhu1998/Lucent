#pragma once

// Strict reading of protocol JSON (protocol.hpp, "Strict reading"): parsing that refuses repeated
// member names and excessive nesting, and an object reader that requires every member it is asked
// for and refuses the members it was not asked for. Private to lucent_workers.
//
// Every failure is Corrupt with the location "workers.protocol.decode" and a message naming the
// member path ("job.source.identity.size: expected an unsigned integer").

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <lucent/core/error.hpp>

namespace lucent::workers::detail {

using Json = nlohmann::json;

inline constexpr std::string_view kWhereDecode = "workers.protocol.decode";
inline constexpr std::size_t kMaxJsonDepth = 8;

[[nodiscard]] Error corrupt(std::string message);

// Parses `text` as one JSON value. Corrupt for invalid JSON (nlohmann also rejects strings that are
// not valid UTF-8), a repeated member name at any level or nesting deeper than kMaxJsonDepth.
[[nodiscard]] Expected<Json> parse_strict(std::string_view text);

class ObjectReader {
public:
    // `object` must outlive the reader. Corrupt (from make) unless it is a JSON object.
    [[nodiscard]] static Expected<ObjectReader> make(const Json& object, std::string path);

    [[nodiscard]] Expected<std::string> string(std::string_view name, std::size_t max_bytes,
                                               bool allow_empty = true);
    // A string or null.
    [[nodiscard]] Expected<std::optional<std::string>> optional_string(std::string_view name,
                                                                       std::size_t max_bytes);
    [[nodiscard]] Expected<std::uint64_t> unsigned_integer(std::string_view name,
                                                           std::uint64_t max = UINT64_MAX);
    [[nodiscard]] Expected<std::uint32_t> uint32(std::string_view name, std::uint32_t min = 0,
                                                 std::uint32_t max = UINT32_MAX);
    [[nodiscard]] Expected<std::int64_t> signed_integer(std::string_view name);
    // A finite number within [min, max].
    [[nodiscard]] Expected<double> number(std::string_view name, double min, double max);
    [[nodiscard]] Expected<bool> boolean(std::string_view name);
    [[nodiscard]] Expected<ObjectReader> object(std::string_view name);
    // An array of at most `max_size` elements.
    [[nodiscard]] Expected<const Json*> array(std::string_view name, std::size_t max_size);
    // True when the member exists and is null (the member counts as read).
    [[nodiscard]] Expected<bool> is_null(std::string_view name);

    // Corrupt when the object has a member none of the accessors above asked for.
    [[nodiscard]] Status finish() const;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::string member_path(std::string_view name) const;

private:
    ObjectReader(const Json& object, std::string path) : object_(&object), path_(std::move(path)) {}
    [[nodiscard]] Expected<const Json*> member(std::string_view name);

    const Json* object_;
    std::string path_;
    std::set<std::string, std::less<>> read_;
};

}  // namespace lucent::workers::detail
