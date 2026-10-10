#pragma once

// Private: the update channel's configuration text as CMake recorded it at configure time
// (src/application/CMakeLists.txt generates update_channel_config.cpp from
// update_channel_config.cpp.in). update_channel.cpp checks and parses it.

#include <string_view>

namespace lucent::app::detail {

struct UpdateChannelText {
    std::string_view name;
    std::string_view manifest_url;
    std::string_view public_keys;            // ';'-separated
    std::string_view publisher_thumbprints;  // ';'-separated
};

[[nodiscard]] const UpdateChannelText& configured_update_channel() noexcept;

}  // namespace lucent::app::detail
