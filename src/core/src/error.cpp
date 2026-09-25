#include "lucent/core/error.hpp"

#include <format>

namespace lucent {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Unknown: return "unknown";
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::AlreadyExists: return "already_exists";
    case ErrorCode::PermissionDenied: return "permission_denied";
    case ErrorCode::IoError: return "io_error";
    case ErrorCode::DiskFull: return "disk_full";
    case ErrorCode::Corrupt: return "corrupt";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::LimitExceeded: return "limit_exceeded";
    case ErrorCode::OutOfMemory: return "out_of_memory";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::Timeout: return "timeout";
    case ErrorCode::VersionMismatch: return "version_mismatch";
    case ErrorCode::Internal: return "internal";
    case ErrorCode::SourceChanged: return "source_changed";
    }
    return "unknown";
}

std::string Error::describe() const {
    std::string text = where.empty() ? message : std::format("{}: {}", where, message);
    if (native_code != 0) {
        text += std::format(" ({}, native {})", to_string(code), native_code);
    } else {
        text += std::format(" ({})", to_string(code));
    }
    return text;
}

}  // namespace lucent
