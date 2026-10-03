#include "lucent/core/app_paths.hpp"

// clang-format off: <shlobj.h> requires the types declared by <windows.h>.
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
// clang-format on

#include <format>
#include <string>

#include "lucent/core/storage.hpp"

namespace lucent::core {

AppDirectories app_directories_at(const std::filesystem::path& root) {
    AppDirectories directories;
    directories.root = root;
    directories.data = root / L"data";
    directories.cache = root / L"cache";
    directories.logs = directories.cache / L"logs";
    return directories;
}

Expected<std::filesystem::path> local_app_data_folder() {
    constexpr const char* kWhere = "core.app_paths.local_app_data";
    PWSTR raw = nullptr;
    const HRESULT result = ::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    // The buffer must be freed even on failure (it is then null or empty).
    struct Free {
        PWSTR text;
        ~Free() { ::CoTaskMemFree(text); }
    } owner{raw};
    if (FAILED(result) || raw == nullptr || *raw == L'\0') {
        const auto code = static_cast<std::uint32_t>(result);
        const ErrorCode error_code =
            result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || result == E_INVALIDARG ? ErrorCode::NotFound
                                                                                         : ErrorCode::IoError;
        return fail(error_code,
                    std::format("SHGetKnownFolderPath(LocalAppData) failed with HRESULT 0x{:08X}", code),
                    kWhere, static_cast<std::int64_t>(code));
    }
    return std::filesystem::path(std::wstring(raw));
}

Expected<AppDirectories> default_app_directories() {
    auto base = local_app_data_folder();
    if (!base) {
        return Unexpected(std::move(base.error()));
    }
    return app_directories_at(*base / L"Lucent");
}

Status create_app_directories(const AppDirectories& directories) {
    for (const std::filesystem::path* directory :
         {&directories.root, &directories.data, &directories.cache, &directories.logs}) {
        if (auto status = create_directory_tree(*directory); !status) {
            return status;
        }
    }
    return ok();
}

}  // namespace lucent::core
