#include "win32_util.hpp"

#include <string_view>

namespace lucent::workers::detail {

std::wstring verbatim(const std::filesystem::path& path) {
    const std::wstring native = path.lexically_normal().native();
    if (!path.is_absolute() || native.starts_with(LR"(\\?\)") || native.starts_with(LR"(\\.\)")) {
        return native;
    }
    if (native.starts_with(LR"(\\)")) {
        return std::wstring(LR"(\\?\UNC\)") + native.substr(2);
    }
    return std::wstring(LR"(\\?\)") + native;
}

}  // namespace lucent::workers::detail
