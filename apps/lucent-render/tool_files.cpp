#include "tool_files.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <string_view>
#include <system_error>
#include <utility>

#include <windows.h>

#include <lucent/core/file.hpp>
#include <lucent/core/text.hpp>
#include <lucent/core/win32_error.hpp>

namespace lucent::render_tool {

namespace {

constexpr const char* kWherePath = "lucent-render.path";
constexpr const char* kWhereOutput = "lucent-render.output";
constexpr const char* kWherePublish = "lucent-render.publish";

// Room for the 8.3-name rule that CreateDirectoryW applies (MAX_PATH - 12), as in lucent_export.
constexpr std::size_t kShortPathLimit = MAX_PATH - 12;

std::string display(const std::filesystem::path& path) {
    return core::path_to_utf8_lossy(path);
}

Expected<std::wstring> full_path(const std::filesystem::path& path) {
    const std::wstring& native = path.native();
    if (native.empty()) {
        return fail(ErrorCode::InvalidArgument, "path is empty", kWherePath);
    }
    if (native.find(L'\0') != std::wstring::npos) {
        return fail(ErrorCode::InvalidArgument, "path contains a NUL character", kWherePath);
    }
    std::wstring full(native.size() + 16, L'\0');
    for (;;) {
        const DWORD length = ::GetFullPathNameW(native.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
        if (length == 0) {
            return Unexpected(core::last_error(kWherePath, "cannot resolve " + display(path)));
        }
        if (length < full.size()) {
            full.resize(length);
            return full;
        }
        full.resize(length);  // too small: the current directory changed; retry with the new size
    }
}

// Deletes a file quietly (temporary files on failure paths).
void remove_quietly(const std::filesystem::path& path) noexcept {
    ::DeleteFileW(path.c_str());
}

}  // namespace

Expected<std::filesystem::path> long_path(const std::filesystem::path& path) {
    const std::wstring& native = path.native();
    if (native.empty()) {
        return fail(ErrorCode::InvalidArgument, "path is empty", kWherePath);
    }
    if (native.find(L'\0') != std::wstring::npos) {
        return fail(ErrorCode::InvalidArgument, "path contains a NUL character", kWherePath);
    }
    if (native.starts_with(L"\\\\?\\") || native.starts_with(L"\\\\.\\") || native.size() < kShortPathLimit) {
        return path;
    }
    auto full = full_path(path);
    if (!full) {
        return Unexpected(std::move(full.error()));
    }
    if (full->starts_with(L"\\\\?\\") || full->starts_with(L"\\\\.\\")) {
        return std::filesystem::path(*full);
    }
    if (full->starts_with(L"\\\\")) {
        return std::filesystem::path(L"\\\\?\\UNC\\" + full->substr(2));
    }
    return std::filesystem::path(L"\\\\?\\" + *full);
}

Status create_directories(const std::filesystem::path& directory) {
    auto target = long_path(directory);
    if (!target) {
        return Unexpected(std::move(target.error()));
    }
    std::error_code ec;
    std::filesystem::create_directories(*target, ec);
    if (ec) {
        return fail(ErrorCode::IoError, "cannot create the output directory: " + ec.message(), kWhereOutput,
                    ec.value());
    }
    if (!std::filesystem::is_directory(*target, ec)) {
        return fail(ErrorCode::IoError, "the output path is not a directory: " + display(directory), kWhereOutput);
    }
    return ok();
}

bool exists(const std::filesystem::path& path) noexcept {
    try {
        auto target = long_path(path);
        return target && ::GetFileAttributesW(target->c_str()) != INVALID_FILE_ATTRIBUTES;
    } catch (...) {
        return false;
    }
}

Expected<bool> same_target(const std::filesystem::path& a, const std::filesystem::path& b) {
    auto full_a = full_path(a);
    if (!full_a) {
        return Unexpected(std::move(full_a.error()));
    }
    auto full_b = full_path(b);
    if (!full_b) {
        return Unexpected(std::move(full_b.error()));
    }
    if (::CompareStringOrdinal(full_a->c_str(), static_cast<int>(full_a->size()), full_b->c_str(),
                               static_cast<int>(full_b->size()), TRUE) == CSTR_EQUAL) {
        return true;
    }
    auto id_a = core::stat_file(a);
    auto id_b = core::stat_file(b);
    return id_a && id_b && core::is_same_file(*id_a, *id_b);
}

Status publish_new_file(const std::filesystem::path& destination,
                        const std::function<Status(const std::filesystem::path&)>& write) {
    if (!destination.has_filename()) {
        return fail(ErrorCode::InvalidArgument, "the output path names no file: " + display(destination), kWherePublish);
    }
    auto target = long_path(destination);
    if (!target) {
        return Unexpected(std::move(target.error()));
    }
    if (render_tool::exists(*target)) {
        return fail(ErrorCode::AlreadyExists,
                    "the file exists and is never overwritten: " + display(destination), kWherePublish,
                    ERROR_FILE_EXISTS);
    }
    const std::filesystem::path directory = target->parent_path().empty() ? std::filesystem::path(L".")
                                                                          : target->parent_path();
    std::random_device random;
    std::filesystem::path temp;
    for (int attempt = 0; attempt < 16 && temp.empty(); ++attempt) {
        const std::uint64_t token = (std::uint64_t{random()} << 32) ^ std::uint64_t{random()};
        wchar_t name[40];
        std::swprintf(name, std::size(name), L"~lucent-%016llx.tmp", static_cast<unsigned long long>(token));
        auto candidate = long_path(directory / name);
        if (!candidate) {
            return Unexpected(std::move(candidate.error()));
        }
        const HANDLE handle = ::CreateFileW(candidate->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle);
            temp = *candidate;
            break;
        }
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            return Unexpected(core::win32_error(error, kWherePublish,
                                                "cannot create a temporary file next to " + display(destination)));
        }
    }
    if (temp.empty()) {
        return fail(ErrorCode::IoError, "cannot find a free temporary file name next to " + display(destination),
                    kWherePublish);
    }
    if (auto written = write(temp); !written) {
        remove_quietly(temp);
        return written;
    }
    // No MOVEFILE_REPLACE_EXISTING: a destination that appeared meanwhile makes the rename fail.
    if (!::MoveFileExW(temp.c_str(), target->c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = ::GetLastError();
        remove_quietly(temp);
        if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
            return fail(ErrorCode::AlreadyExists,
                        "the file exists and is never overwritten: " + display(destination), kWherePublish,
                        static_cast<std::int64_t>(error));
        }
        return Unexpected(core::win32_error(error, kWherePublish, "cannot publish " + display(destination)));
    }
    return ok();
}

Status publish_new_text_file(const std::filesystem::path& destination, const std::string& text) {
    return publish_new_file(destination, [&text](const std::filesystem::path& temp) -> Status {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            return fail(ErrorCode::IoError, "cannot write " + core::path_to_utf8_lossy(temp), kWherePublish);
        }
        return ok();
    });
}

}  // namespace lucent::render_tool
