#include "strict_json.hpp"

#include <cmath>
#include <format>
#include <limits>
#include <utility>
#include <vector>

namespace lucent::workers::detail {

Error corrupt(std::string message) {
    return Error{ErrorCode::Corrupt, std::move(message), std::string(kWhereDecode), 0};
}

Expected<Json> parse_strict(std::string_view text) {
    // nlohmann's parser is iterative (no recursion), so nesting costs memory bounded by the frame
    // size, not stack; the depth limit refuses documents no message has. Repeated member names are
    // refused: nlohmann would keep the last one silently.
    std::vector<std::set<std::string, std::less<>>> names;
    std::size_t depth = 0;
    std::string problem;
    const Json::parser_callback_t track = [&](int, Json::parse_event_t event, Json& parsed) {
        switch (event) {
            case Json::parse_event_t::object_start:
                names.emplace_back();
                [[fallthrough]];
            case Json::parse_event_t::array_start:
                if (++depth > kMaxJsonDepth && problem.empty()) {
                    problem = std::format("the message nests deeper than {} levels", kMaxJsonDepth);
                }
                break;
            case Json::parse_event_t::object_end:
                if (!names.empty()) {
                    names.pop_back();
                }
                [[fallthrough]];
            case Json::parse_event_t::array_end:
                if (depth > 0) {
                    --depth;
                }
                break;
            case Json::parse_event_t::key:
                if (!names.empty() && parsed.is_string() &&
                    !names.back().insert(parsed.get<std::string>()).second && problem.empty()) {
                    problem = std::format("the member '{}' is repeated", parsed.get<std::string>());
                }
                break;
            default:
                break;
        }
        return true;
    };
    try {
        Json value = Json::parse(text.begin(), text.end(), track, /*allow_exceptions=*/false);
        if (value.is_discarded()) {
            return Unexpected(corrupt("the message is not valid JSON"));
        }
        if (!problem.empty()) {
            return Unexpected(corrupt(std::move(problem)));
        }
        return value;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot parse the message", std::string(kWhereDecode));
    }
}

Expected<ObjectReader> ObjectReader::make(const Json& object, std::string path) {
    if (!object.is_object()) {
        return Unexpected(corrupt(std::format("{}: expected an object", path)));
    }
    return ObjectReader(object, std::move(path));
}

std::string ObjectReader::member_path(std::string_view name) const {
    return path_.empty() ? std::string(name) : std::format("{}.{}", path_, name);
}

Expected<const Json*> ObjectReader::member(std::string_view name) {
    const auto found = object_->find(name);
    if (found == object_->end()) {
        return Unexpected(corrupt(std::format("{}: the member is missing", member_path(name))));
    }
    read_.emplace(name);
    return &*found;
}

Expected<std::string> ObjectReader::string(std::string_view name, std::size_t max_bytes, bool allow_empty) {
    auto value = member(name);
    if (!value) {
        return Unexpected(std::move(value.error()));
    }
    if (!(*value)->is_string()) {
        return Unexpected(corrupt(std::format("{}: expected a string", member_path(name))));
    }
    const auto& text = (*value)->get_ref<const std::string&>();
    if (text.size() > max_bytes) {
        return Unexpected(corrupt(std::format("{}: longer than {} bytes", member_path(name), max_bytes)));
    }
    if (text.empty() && !allow_empty) {
        return Unexpected(corrupt(std::format("{}: must not be empty", member_path(name))));
    }
    return text;
}

Expected<std::optional<std::string>> ObjectReader::optional_string(std::string_view name, std::size_t max_bytes) {
    auto null = is_null(name);
    if (!null) {
        return Unexpected(std::move(null.error()));
    }
    if (*null) {
        return std::optional<std::string>{};
    }
    auto text = string(name, max_bytes);
    if (!text) {
        return Unexpected(std::move(text.error()));
    }
    return std::optional<std::string>(std::move(*text));
}

Expected<std::uint64_t> ObjectReader::unsigned_integer(std::string_view name, std::uint64_t max) {
    auto value = member(name);
    if (!value) {
        return Unexpected(std::move(value.error()));
    }
    // nlohmann stores every non-negative integer literal as number_unsigned; "1.0", "1e0" and
    // negative numbers are other kinds and refused here.
    if (!(*value)->is_number_unsigned()) {
        return Unexpected(corrupt(std::format("{}: expected an unsigned integer", member_path(name))));
    }
    const auto number = (*value)->get<std::uint64_t>();
    if (number > max) {
        return Unexpected(corrupt(std::format("{}: {} is above {}", member_path(name), number, max)));
    }
    return number;
}

Expected<std::uint32_t> ObjectReader::uint32(std::string_view name, std::uint32_t min, std::uint32_t max) {
    auto value = unsigned_integer(name, max);
    if (!value) {
        return Unexpected(std::move(value.error()));
    }
    if (*value < min) {
        return Unexpected(corrupt(std::format("{}: {} is below {}", member_path(name), *value, min)));
    }
    return static_cast<std::uint32_t>(*value);
}

Expected<std::int64_t> ObjectReader::signed_integer(std::string_view name) {
    auto value = member(name);
    if (!value) {
        return Unexpected(std::move(value.error()));
    }
    if ((*value)->is_number_unsigned()) {
        const auto number = (*value)->get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return Unexpected(corrupt(std::format("{}: out of range", member_path(name))));
        }
        return static_cast<std::int64_t>(number);
    }
    if (!(*value)->is_number_integer()) {
        return Unexpected(corrupt(std::format("{}: expected an integer", member_path(name))));
    }
    return (*value)->get<std::int64_t>();
}

Expected<double> ObjectReader::number(std::string_view name, double min, double max) {
    auto value = member(name);
    if (!value) {
        return Unexpected(std::move(value.error()));
    }
    if (!(*value)->is_number()) {
        return Unexpected(corrupt(std::format("{}: expected a number", member_path(name))));
    }
    const double number = (*value)->get<double>();
    if (!std::isfinite(number) || number < min || number > max) {
        return Unexpected(corrupt(std::format("{}: {} is outside [{}, {}]", member_path(name), number, min, max)));
    }
    return number;
}

Expected<bool> ObjectReader::boolean(std::string_view name) {
    auto value = member(name);
    if (!value) {
        return Unexpected(std::move(value.error()));
    }
    if (!(*value)->is_boolean()) {
        return Unexpected(corrupt(std::format("{}: expected true or false", member_path(name))));
    }
    return (*value)->get<bool>();
}

Expected<ObjectReader> ObjectReader::object(std::string_view name) {
    auto value = member(name);
    if (!value) {
        return Unexpected(std::move(value.error()));
    }
    return make(**value, member_path(name));
}

Expected<const Json*> ObjectReader::array(std::string_view name, std::size_t max_size) {
    auto value = member(name);
    if (!value) {
        return Unexpected(std::move(value.error()));
    }
    if (!(*value)->is_array()) {
        return Unexpected(corrupt(std::format("{}: expected an array", member_path(name))));
    }
    if ((*value)->size() > max_size) {
        return Unexpected(corrupt(std::format("{}: more than {} elements", member_path(name), max_size)));
    }
    return *value;
}

Expected<bool> ObjectReader::is_null(std::string_view name) {
    auto value = member(name);
    if (!value) {
        return Unexpected(std::move(value.error()));
    }
    return (*value)->is_null();
}

Status ObjectReader::finish() const {
    for (const auto& [key, value] : object_->items()) {
        if (!read_.contains(key)) {
            return Unexpected(corrupt(std::format("{}: unknown member", member_path(key))));
        }
    }
    return ok();
}

}  // namespace lucent::workers::detail
