#include <lucent/workers/worker_launch.hpp>

#include <charconv>
#include <format>
#include <limits>

#include <lucent/core/text.hpp>

namespace lucent::workers {

namespace {

constexpr std::string_view kWhereLaunch = "workers.launch";
constexpr std::string_view kWhereHook = "workers.launch.hook";
constexpr std::uint64_t kMaxAllocateBytes = std::uint64_t{1} << 40;
constexpr std::uint32_t kMaxDelayMs = 10 * 60 * 1000;
constexpr std::uint32_t kMaxFailedFinalChecks = 16;

Error invalid_hook(std::string_view spec) {
    return Error{ErrorCode::InvalidArgument, std::format("invalid test hook '{}'", core::sanitize_utf8(spec)),
                 std::string(kWhereHook), 0};
}

template <class T>
bool parse_number(std::string_view text, T& value) {
    if (text.empty() || text.size() > 20) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

bool is_hook_text(std::string_view text) {
    if (text.empty() || text.size() > 64) {
        return false;
    }
    for (const char c : text) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

// "--name=value" -> value when `argument` starts with "--name=".
std::optional<std::wstring_view> option_value(std::wstring_view argument, std::wstring_view name) {
    if (argument.size() > name.size() + 1 && argument.starts_with(name) && argument[name.size()] == L'=') {
        return argument.substr(name.size() + 1);
    }
    return std::nullopt;
}

bool parse_handle(std::wstring_view text, std::uintptr_t& value) {
    std::string narrow;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') {
            return false;
        }
        narrow.push_back(static_cast<char>(c));
    }
    return parse_number(narrow, value) && value != 0;
}

}  // namespace

bool TestHooks::any() const noexcept {
    return hang_job || freeze || crash_job || allocate_bytes != 0 || render_delay_ms != 0 || fake_commit.has_value() ||
           protocol_range.has_value() || fail_final_checks != 0;
}

std::vector<std::string> TestHooks::specs() const {
    std::vector<std::string> out;
    if (hang_job) {
        out.emplace_back("hang-job");
    }
    if (freeze) {
        out.emplace_back("freeze");
    }
    if (crash_job) {
        out.emplace_back("crash-job");
    }
    if (allocate_bytes != 0) {
        out.push_back(std::format("allocate:{}", allocate_bytes));
    }
    if (render_delay_ms != 0) {
        out.push_back(std::format("render-delay:{}", render_delay_ms));
    }
    if (fake_commit) {
        out.push_back(std::format("fake-commit:{}", *fake_commit));
    }
    if (protocol_range) {
        out.push_back(std::format("protocol:{}-{}", protocol_range->first, protocol_range->second));
    }
    if (fail_final_checks != 0) {
        out.push_back(std::format("fail-final-check:{}", fail_final_checks));
    }
    return out;
}

Status apply_test_hook(std::string_view spec, TestHooks& hooks) {
    const std::size_t colon = spec.find(':');
    const std::string_view name = spec.substr(0, colon);
    const std::string_view value = colon == std::string_view::npos ? std::string_view{} : spec.substr(colon + 1);
    const bool has_value = colon != std::string_view::npos;
    if (name == "hang-job" && !has_value) {
        hooks.hang_job = true;
    } else if (name == "freeze" && !has_value) {
        hooks.freeze = true;
    } else if (name == "crash-job" && !has_value) {
        hooks.crash_job = true;
    } else if (name == "allocate" && has_value) {
        std::uint64_t bytes = 0;
        if (!parse_number(value, bytes) || bytes == 0 || bytes > kMaxAllocateBytes) {
            return Unexpected(invalid_hook(spec));
        }
        hooks.allocate_bytes = bytes;
    } else if (name == "render-delay" && has_value) {
        std::uint32_t ms = 0;
        if (!parse_number(value, ms) || ms == 0 || ms > kMaxDelayMs) {
            return Unexpected(invalid_hook(spec));
        }
        hooks.render_delay_ms = ms;
    } else if (name == "fake-commit" && has_value) {
        if (!is_hook_text(value)) {
            return Unexpected(invalid_hook(spec));
        }
        hooks.fake_commit = std::string(value);
    } else if (name == "protocol" && has_value) {
        const std::size_t dash = value.find('-');
        std::uint32_t low = 0;
        std::uint32_t high = 0;
        if (dash == std::string_view::npos || !parse_number(value.substr(0, dash), low) ||
            !parse_number(value.substr(dash + 1), high) || low == 0 || low > high) {
            return Unexpected(invalid_hook(spec));
        }
        hooks.protocol_range = std::pair(low, high);
    } else if (name == "fail-final-check" && has_value) {
        std::uint32_t count = 0;
        if (!parse_number(value, count) || count == 0 || count > kMaxFailedFinalChecks) {
            return Unexpected(invalid_hook(spec));
        }
        hooks.fail_final_checks = count;
    } else {
        return Unexpected(invalid_hook(spec));
    }
    return ok();
}

Expected<std::wstring> worker_arguments(const WorkerLaunch& launch) {
    if (launch.request_handle == 0 || launch.response_handle == 0) {
        return fail(ErrorCode::InvalidArgument, "the worker's pipe handles are missing", std::string(kWhereLaunch));
    }
    std::wstring arguments(kWorkerModeArgument);
    arguments += std::format(L" --request-handle={} --response-handle={}", launch.request_handle,
                             launch.response_handle);
    if (launch.parent_handle != 0) {
        arguments += std::format(L" --parent-handle={}", launch.parent_handle);
    }
    for (const std::string& spec : launch.hooks.specs()) {
        // Specs are ASCII without spaces or quotes (is_hook_text and decimal numbers).
        arguments += L" --test-hook=";
        arguments.append(spec.begin(), spec.end());
    }
    return arguments;
}

Expected<WorkerLaunch> parse_worker_arguments(std::span<const std::wstring> arguments) {
    const auto usage = [](std::string message) {
        return Unexpected(Error{ErrorCode::InvalidArgument, std::move(message), std::string(kWhereLaunch), 0});
    };
    if (arguments.empty() || arguments.front() != kWorkerModeArgument) {
        return usage("lucent-worker.exe is started by Lucent only");
    }
    WorkerLaunch launch;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::wstring_view argument = arguments[i];
        if (auto value = option_value(argument, L"--request-handle")) {
            if (launch.request_handle != 0 || !parse_handle(*value, launch.request_handle)) {
                return usage("invalid or repeated --request-handle");
            }
        } else if (auto response = option_value(argument, L"--response-handle")) {
            if (launch.response_handle != 0 || !parse_handle(*response, launch.response_handle)) {
                return usage("invalid or repeated --response-handle");
            }
        } else if (auto parent = option_value(argument, L"--parent-handle")) {
            if (launch.parent_handle != 0 || !parse_handle(*parent, launch.parent_handle)) {
                return usage("invalid or repeated --parent-handle");
            }
        } else if (auto hook = option_value(argument, L"--test-hook")) {
            auto spec = core::wide_to_utf8(*hook);
            if (!spec) {
                return usage("invalid --test-hook");
            }
            if (auto applied = apply_test_hook(*spec, launch.hooks); !applied) {
                return Unexpected(std::move(applied.error()));
            }
        } else {
            return usage(std::format("unknown argument '{}'", core::wide_to_utf8_lossy(argument)));
        }
    }
    if (launch.request_handle == 0 || launch.response_handle == 0) {
        return usage("the pipe handles are missing");
    }
    return launch;
}

}  // namespace lucent::workers
