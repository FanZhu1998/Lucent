#include "catalog_mapping.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <lucent/core/log.hpp>
#include <lucent/core/text.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::app::detail {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kLogCategory = "app.catalog";

// The enumerator of E whose to_string() spelling is `text`, searching the values 1 ... 255.
template <class E>
std::optional<E> parse_enum(std::string_view text) {
    for (int value = 1; value <= 255; ++value) {
        const auto candidate = static_cast<E>(value);
        if (raw::to_string(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<ErrorCode> parse_error_code(std::string_view text) {
    for (int value = 1; value <= 255; ++value) {
        const auto candidate = static_cast<ErrorCode>(value);
        if (to_string(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::string string_field(const Json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        return {};
    }
    return found->get<std::string>();
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

}  // namespace

catalog::SupportStatus to_catalog(SupportStatus status) noexcept {
    return static_cast<catalog::SupportStatus>(status);  // same values (source_item.hpp, types.hpp)
}

SupportStatus from_catalog(catalog::SupportStatus status) noexcept {
    switch (status) {
        case catalog::SupportStatus::Supported:
            return SupportStatus::Supported;
        case catalog::SupportStatus::Limited:
            return SupportStatus::Limited;
        case catalog::SupportStatus::Unsupported:
            return SupportStatus::Unsupported;
    }
    return SupportStatus::Unsupported;
}

catalog::SourceState to_catalog(SourceState state) noexcept {
    return static_cast<catalog::SourceState>(state);
}

SourceState from_catalog(catalog::SourceState state) noexcept {
    switch (state) {
        case catalog::SourceState::Present:
            return SourceState::Present;
        case catalog::SourceState::Missing:
            return SourceState::Missing;
        case catalog::SourceState::Changed:
            return SourceState::Changed;
    }
    return SourceState::Changed;  // unknown: treat the probe as stale
}

catalog::ImportMode to_catalog(ImportMode mode) noexcept {
    return static_cast<catalog::ImportMode>(mode);
}

ImportMode from_catalog(catalog::ImportMode mode) noexcept {
    return mode == catalog::ImportMode::Copied ? ImportMode::Copied : ImportMode::Referenced;
}

catalog::ItemFlags to_catalog(const ItemFlags& flags) noexcept {
    return catalog::ItemFlags{static_cast<catalog::PickFlag>(flags.pick), flags.rating};
}

ItemFlags from_catalog(const catalog::ItemFlags& flags) noexcept {
    return ItemFlags{static_cast<PickFlag>(flags.pick), flags.rating};
}

catalog::ImportOutcome to_catalog(ImportJournalOutcome outcome) noexcept {
    return static_cast<catalog::ImportOutcome>(outcome);
}

std::string encode_support_reasons(const SupportAssessment& support) {
    Json reasons = Json::array();
    for (const SupportReason& reason : support.reasons) {
        Json element = Json::object();
        switch (reason.kind) {
            case SupportReasonKind::InterpretationIssue:
                element["kind"] = "issue";
                element["issue"] = reason.issue ? std::string(raw::to_string(*reason.issue)) : std::string();
                break;
            case SupportReasonKind::InterpretationStatus:
                element["kind"] = "status";
                element["status"] = support.interpretation
                                        ? std::string(raw::to_string(*support.interpretation))
                                        : std::string();
                break;
            case SupportReasonKind::ProbeFailed:
                element["kind"] = "probe_failed";
                element["error"] = std::string(to_string(reason.error_code.value_or(ErrorCode::Unknown)));
                break;
        }
        element["message"] = core::sanitize_utf8(reason.message);
        reasons.push_back(std::move(element));
    }
    return reasons.dump(-1, ' ', false, Json::error_handler_t::replace);
}

SupportAssessment decode_support(catalog::SupportStatus status, std::string_view reasons_json) {
    SupportAssessment support;
    support.status = from_catalog(status);
    std::optional<raw::InterpretationStatus> recorded_status;
    bool probe_failed = false;
    std::vector<raw::InterpretationIssue> issues;

    const Json parsed = Json::parse(reasons_json.begin(), reasons_json.end(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        core::log_warn(kLogCategory, "an item's support reasons are not a JSON array; shown without detail");
        if (support.status != SupportStatus::Supported) {
            SupportReason reason;
            reason.kind = SupportReasonKind::InterpretationStatus;
            reason.message = "stored support reasons are unreadable";
            support.reasons.push_back(std::move(reason));
        }
    } else {
        for (const Json& element : parsed) {
            if (!element.is_object()) {
                continue;
            }
            SupportReason reason;
            reason.message = string_field(element, "message");
            const std::string kind = string_field(element, "kind");
            if (kind == "issue") {
                reason.kind = SupportReasonKind::InterpretationIssue;
                reason.issue = parse_enum<raw::InterpretationIssueKind>(string_field(element, "issue"));
                if (reason.issue) {
                    issues.push_back(raw::InterpretationIssue{*reason.issue, reason.message});
                } else {
                    reason.kind = SupportReasonKind::InterpretationStatus;  // unknown issue: generic wording
                }
            } else if (kind == "probe_failed") {
                reason.kind = SupportReasonKind::ProbeFailed;
                reason.error_code =
                    parse_error_code(string_field(element, "error")).value_or(ErrorCode::Unknown);
                probe_failed = true;
            } else {
                reason.kind = SupportReasonKind::InterpretationStatus;
                if (kind == "status") {
                    recorded_status = parse_enum<raw::InterpretationStatus>(string_field(element, "status"));
                }
            }
            support.reasons.push_back(std::move(reason));
        }
    }

    const raw::InterpretationStatus implied = raw::derive_interpretation_status(issues);
    switch (support.status) {
        case SupportStatus::Supported:
            support.interpretation = raw::InterpretationStatus::Complete;
            break;
        case SupportStatus::Limited:
            support.interpretation = std::max(raw::InterpretationStatus::OptionalOperationsSkipped, implied);
            break;
        case SupportStatus::Unsupported:
            if (probe_failed) {
                support.interpretation = std::nullopt;
            } else if (recorded_status) {
                support.interpretation = recorded_status;
            } else {
                support.interpretation =
                    std::max(raw::InterpretationStatus::RequiresUnsupportedOperation, implied);
            }
            break;
    }
    return support;
}

bool has_probe_failure(const SupportAssessment& support) noexcept {
    return std::any_of(support.reasons.begin(), support.reasons.end(), [](const SupportReason& reason) {
        return reason.kind == SupportReasonKind::ProbeFailed;
    });
}

std::optional<std::string> catalog_capture_time(const std::optional<std::string>& text) {
    if (!text) {
        return std::nullopt;
    }
    const std::string_view t = *text;
    // 0123456789012345678
    // YYYY-MM-DDTHH:MM:SS
    if (t.size() != 19 || t[4] != '-' || t[7] != '-' || t[10] != 'T' || t[13] != ':' || t[16] != ':') {
        return std::nullopt;
    }
    for (const std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u}) {
        if (t[i] < '0' || t[i] > '9') {
            return std::nullopt;
        }
    }
    const auto number = [&](std::size_t at) { return (t[at] - '0') * 10 + (t[at + 1] - '0'); };
    const int month = number(5);
    const int day = number(8);
    if (month < 1 || month > 12 || day < 1 || day > 31 || number(11) > 23 || number(14) > 59 ||
        number(17) > 60) {
        return std::nullopt;
    }
    return text;
}

catalog::ItemMetadata to_metadata(const SourceItem& item, catalog::Timestamp probed_at) {
    catalog::ItemMetadata metadata;
    metadata.subimage_index = item.subimage;
    metadata.support_status = to_catalog(item.support.status);
    metadata.support_reasons_json = encode_support_reasons(item.support);
    metadata.probed_at = probed_at;
    if (item.probe) {
        const ProbeSummary& probe = *item.probe;
        metadata.make = core::sanitize_utf8(probe.camera_make);
        metadata.model = core::sanitize_utf8(probe.camera_model);
        metadata.lens = core::sanitize_utf8(probe.lens);
        metadata.capture_time = catalog_capture_time(probe.capture_time_local);
        metadata.width = static_cast<std::uint32_t>(std::max(0, probe.stored_size.width));
        metadata.height = static_cast<std::uint32_t>(std::max(0, probe.stored_size.height));
        const auto orientation = static_cast<std::uint8_t>(probe.orientation);
        metadata.orientation = orientation >= 1 && orientation <= 8 ? orientation : std::uint8_t{1};
        metadata.format = lower_ascii(core::sanitize_utf8(probe.extension_hint));
        metadata.decoder_id = core::sanitize_utf8(probe.decoder);
        metadata.decoder_version = core::sanitize_utf8(probe.decoder_version);
    }
    if (metadata.format.empty()) {
        std::string extension = core::path_to_utf8_lossy(item.path.extension());
        if (!extension.empty() && extension.front() == '.') {
            extension.erase(extension.begin());
        }
        metadata.format = lower_ascii(std::move(extension));
    }
    return metadata;
}

std::optional<ProbeSummary> probe_from_metadata(const catalog::ItemMetadata& metadata,
                                                const SupportAssessment& support) {
    if (has_probe_failure(support)) {
        return std::nullopt;
    }
    ProbeSummary probe;
    probe.extension_hint = metadata.format;
    probe.decoder = metadata.decoder_id;
    probe.decoder_version = metadata.decoder_version;
    probe.camera_make = metadata.make;
    probe.camera_model = metadata.model;
    probe.lens = metadata.lens;
    probe.capture_time_local = metadata.capture_time;
    probe.stored_size =
        image::Size{static_cast<std::int32_t>(std::min<std::uint32_t>(metadata.width, INT32_MAX)),
                    static_cast<std::int32_t>(std::min<std::uint32_t>(metadata.height, INT32_MAX))};
    auto orientation = image::from_exif(metadata.orientation);
    probe.orientation = orientation ? *orientation : image::Orientation::Normal;
    probe.upright_size = image::oriented_size(probe.stored_size, probe.orientation);
    return probe;
}

std::string encode_import_options(const ImportJournalStart& start) {
    Json options = Json::object();
    if (start.copy_to) {
        options["copy_to"] = core::path_to_utf8_lossy(*start.copy_to);
    } else {
        options["copy_to"] = nullptr;
    }
    return options.dump(-1, ' ', false, Json::error_handler_t::replace);
}

}  // namespace lucent::app::detail
