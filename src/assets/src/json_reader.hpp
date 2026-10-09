#pragma once

// Private: strict JSON reading for the asset documents (asset_manifest.cpp, camera_support.cpp).
// Readers throw DocumentError from inside guarded(), which turns it (and nlohmann and allocation
// failures) into lucent::Error at the module boundary; nothing escapes the module.

#include <cstdint>
#include <exception>
#include <format>
#include <initializer_list>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <lucent/core/error.hpp>

namespace lucent::assets::detail {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

struct DocumentError {
    ErrorCode code = ErrorCode::Corrupt;
    std::string message;
};

[[noreturn]] inline void corrupt(std::string message) {
    throw DocumentError{ErrorCode::Corrupt, std::move(message)};
}

// Parses `text` as one JSON value (duplicate keys keep the last value, as nlohmann does; an
// invalid document is Corrupt).
inline Json parse(std::string_view text) {
    Json value = Json::parse(text.begin(), text.end(), nullptr, false);
    if (value.is_discarded()) {
        corrupt("not a JSON document");
    }
    return value;
}

inline void require_object(const Json& value, std::string_view what) {
    if (!value.is_object()) {
        corrupt(std::format("{} is not an object", what));
    }
}

// Every member of `object` is one of `known` (strict documents: an unknown member is an error).
inline void only_members(const Json& object, std::initializer_list<std::string_view> known,
                         std::string_view what) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool listed = false;
        for (const std::string_view key : known) {
            listed = listed || it.key() == key;
        }
        if (!listed) {
            corrupt(std::format("{} has an unknown member '{}'", what, it.key()));
        }
    }
}

inline const Json& member(const Json& object, std::string_view key, std::string_view what) {
    const auto found = object.find(key);
    if (found == object.end()) {
        corrupt(std::format("{} has no member '{}'", what, key));
    }
    return *found;
}

inline std::string text(const Json& object, std::string_view key, std::string_view what) {
    const Json& value = member(object, key, what);
    if (!value.is_string()) {
        corrupt(std::format("{}.{} is not a string", what, key));
    }
    return value.get<std::string>();
}

inline std::uint64_t unsigned_number(const Json& object, std::string_view key, std::string_view what) {
    const Json& value = member(object, key, what);
    if (!value.is_number_unsigned()) {
        corrupt(std::format("{}.{} is not a non-negative integer", what, key));
    }
    return value.get<std::uint64_t>();
}

inline std::uint32_t small_number(const Json& object, std::string_view key, std::string_view what) {
    const std::uint64_t value = unsigned_number(object, key, what);
    if (value > 0xFFFFFFFFu) {
        corrupt(std::format("{}.{} is out of range", what, key));
    }
    return static_cast<std::uint32_t>(value);
}

inline const Json& array(const Json& object, std::string_view key, std::string_view what) {
    const Json& value = member(object, key, what);
    if (!value.is_array()) {
        corrupt(std::format("{}.{} is not an array", what, key));
    }
    return value;
}

inline std::vector<std::string> strings(const Json& object, std::string_view key, std::string_view what) {
    std::vector<std::string> out;
    for (const Json& value : array(object, key, what)) {
        if (!value.is_string()) {
            corrupt(std::format("{}.{} holds a value that is not a string", what, key));
        }
        out.push_back(value.get<std::string>());
    }
    return out;
}

inline std::vector<std::uint32_t> small_numbers(const Json& object, std::string_view key, std::string_view what) {
    std::vector<std::uint32_t> out;
    for (const Json& value : array(object, key, what)) {
        if (!value.is_number_unsigned() || value.get<std::uint64_t>() > 0xFFFFFFFFu) {
            corrupt(std::format("{}.{} holds a value that is not a version number", what, key));
        }
        out.push_back(static_cast<std::uint32_t>(value.get<std::uint64_t>()));
    }
    return out;
}

// Runs `body`, converting DocumentError, nlohmann exceptions and allocation failures into errors
// at `where`.
template <class F>
auto guarded(std::string_view where, std::string_view document, F&& body) -> decltype(body()) {
    try {
        return body();
    } catch (const DocumentError& e) {
        return fail(e.code, std::format("{}: {}", document, e.message), std::string(where));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, std::format("{}: out of memory", document), std::string(where));
    } catch (const std::exception& e) {
        return fail(ErrorCode::Corrupt, std::format("{}: {}", document, e.what()), std::string(where));
    }
}

}  // namespace lucent::assets::detail
