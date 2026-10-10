#include "lucent/application/update_channel.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

#include <lucent/application/update_manifest.hpp>

#include "update_channel_config.hpp"

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.update.channel";

std::string trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

// The non-empty, trimmed elements of a ';'-separated list.
std::vector<std::string> split_list(std::string_view text) {
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(';', start), text.size());
        std::string item = trimmed(text.substr(start, end - start));
        if (!item.empty()) {
            items.push_back(std::move(item));
        }
        start = end + 1;
    }
    return items;
}

std::string lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return text;
}

bool is_hex(std::string_view text) noexcept {
    return !text.empty() && std::all_of(text.begin(), text.end(),
                                        [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

}  // namespace

Expected<UpdateChannel> make_update_channel(std::string_view name, std::string_view manifest_url,
                                            std::string_view keys, std::string_view thumbprints, UrlPolicy policy) {
    const std::string channel_name = trimmed(name);
    const std::string url = trimmed(manifest_url);
    const std::vector<std::string> key_texts = split_list(keys);
    const std::vector<std::string> thumbprint_texts = split_list(thumbprints);
    UpdateChannel channel;
    if (channel_name.empty() && url.empty() && key_texts.empty() && thumbprint_texts.empty()) {
        return channel;  // not configured
    }
    if (channel_name.empty() || url.empty() || key_texts.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "an update channel needs its name, its manifest URL and at least one public key",
                    std::string(kWhere));
    }
    if (!is_valid_channel_name(channel_name)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("'{}' is not a channel name ([a-z0-9] words joined by '-')", channel_name),
                    std::string(kWhere));
    }
    if (Status allowed = check_url(url, policy); !allowed) {
        return fail(ErrorCode::InvalidArgument, "the update manifest URL is refused: " + allowed.error().message,
                    std::string(kWhere));
    }
    channel.name = channel_name;
    channel.manifest_url = url;
    for (const std::string& text : key_texts) {
        Expected<core::EcdsaP256PublicKey> key = core::parse_ecdsa_p256_public_key(text);
        if (!key) {
            return fail(ErrorCode::InvalidArgument, "an update public key is refused: " + key.error().message,
                        std::string(kWhere));
        }
        if (std::find(channel.keys.begin(), channel.keys.end(), *key) == channel.keys.end()) {
            channel.keys.push_back(*key);
        }
    }
    for (const std::string& text : thumbprint_texts) {
        std::string thumbprint = lower_ascii(text);
        if (!is_hex(thumbprint) || (thumbprint.size() != 40 && thumbprint.size() != 64)) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("'{}' is not a certificate thumbprint (40 or 64 hex digits)", text),
                        std::string(kWhere));
        }
        if (std::find(channel.publisher_thumbprints.begin(), channel.publisher_thumbprints.end(), thumbprint) ==
            channel.publisher_thumbprints.end()) {
            channel.publisher_thumbprints.push_back(std::move(thumbprint));
        }
    }
    return channel;
}

const BuildUpdateChannel& build_update_channel() {
    static const BuildUpdateChannel channel = [] {
        const detail::UpdateChannelText& text = detail::configured_update_channel();
        BuildUpdateChannel result;
        Expected<UpdateChannel> made =
            make_update_channel(text.name, text.manifest_url, text.public_keys, text.publisher_thumbprints);
        if (made) {
            result.channel = std::move(*made);
        } else {
            result.problem = std::move(made.error());
        }
        return result;
    }();
    return channel;
}

}  // namespace lucent::app
