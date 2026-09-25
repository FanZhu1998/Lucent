#include "lucent/core/text.hpp"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <string>

#include "lucent/core/win32_error.hpp"

#include "win32.hpp"

namespace lucent::core {

namespace {

// Result of examining the UTF-8 sequence that starts at one byte.
struct Utf8Step {
    std::size_t length;  // bytes consumed: the whole sequence, or the maximal ill-formed subpart
    bool valid;
};

// Decodes one sequence per RFC 3629 / Unicode Table 3-7 (well-formed UTF-8 byte sequences).
// For ill-formed input, `length` is the maximal subpart (the longest prefix of a well-formed
// sequence, at least one byte), which is what a single U+FFFD replaces.
Utf8Step utf8_step(std::string_view text, std::size_t index) noexcept {
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80) {
        return {1, true};
    }
    std::size_t trailing = 0;
    unsigned char first_min = 0x80;  // allowed range of the first continuation byte
    unsigned char first_max = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        trailing = 1;
    } else if (lead == 0xE0) {
        trailing = 2;
        first_min = 0xA0;  // excludes overlong 3-byte forms
    } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
        trailing = 2;
    } else if (lead == 0xED) {
        trailing = 2;
        first_max = 0x9F;  // excludes UTF-16 surrogates U+D800..U+DFFF
    } else if (lead == 0xF0) {
        trailing = 3;
        first_min = 0x90;  // excludes overlong 4-byte forms
    } else if (lead >= 0xF1 && lead <= 0xF3) {
        trailing = 3;
    } else if (lead == 0xF4) {
        trailing = 3;
        first_max = 0x8F;  // excludes code points above U+10FFFF
    } else {
        return {1, false};  // continuation byte, overlong 2-byte lead (C0, C1) or F5..FF
    }

    for (std::size_t k = 1; k <= trailing; ++k) {
        if (index + k >= text.size()) {
            return {k, false};  // truncated
        }
        const auto byte = static_cast<unsigned char>(text[index + k]);
        const unsigned char min = k == 1 ? first_min : static_cast<unsigned char>(0x80);
        const unsigned char max = k == 1 ? first_max : static_cast<unsigned char>(0xBF);
        if (byte < min || byte > max) {
            return {k, false};
        }
    }
    return {trailing + 1, true};
}

constexpr bool is_high_surrogate(wchar_t unit) noexcept {
    return unit >= 0xD800 && unit <= 0xDBFF;
}

Expected<std::wstring> to_wide(std::string_view utf8, std::string_view where) {
    if (utf8.empty()) {
        return std::wstring();
    }
    if (utf8.size() > static_cast<std::size_t>(INT_MAX)) {
        return fail(ErrorCode::LimitExceeded, "text is too long to convert (more than INT_MAX bytes)",
                    std::string(where));
    }
    // Validate here rather than relying only on MB_ERR_INVALID_CHARS, so the documented
    // RFC 3629 contract holds on every Windows version.
    if (!is_valid_utf8(utf8)) {
        return fail(ErrorCode::InvalidArgument, "text is not valid UTF-8", std::string(where),
                    ERROR_NO_UNICODE_TRANSLATION);
    }
    const int input_length = static_cast<int>(utf8.size());
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), input_length, nullptr, 0);
    if (needed <= 0) {
        const DWORD code = ::GetLastError();
        return Unexpected(win32_error(code, std::string(where), "UTF-8 to UTF-16 conversion failed"));
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), input_length, wide.data(), needed);
    if (written != needed) {
        const DWORD code = ::GetLastError();
        return Unexpected(win32_error(code, std::string(where), "UTF-8 to UTF-16 conversion failed"));
    }
    return wide;
}

Expected<std::string> to_utf8(std::wstring_view wide, std::string_view where) {
    if (wide.empty()) {
        return std::string();
    }
    if (wide.size() > static_cast<std::size_t>(INT_MAX)) {
        return fail(ErrorCode::LimitExceeded, "text is too long to convert (more than INT_MAX code units)",
                    std::string(where));
    }
    const int input_length = static_cast<int>(wide.size());
    const int needed = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), input_length,
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_NO_UNICODE_TRANSLATION) {
            return fail(ErrorCode::InvalidArgument, "text is not valid UTF-16 (unpaired surrogate)",
                        std::string(where), code);
        }
        return Unexpected(win32_error(code, std::string(where), "UTF-16 to UTF-8 conversion failed"));
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), input_length,
                                              utf8.data(), needed, nullptr, nullptr);
    if (written != needed) {
        const DWORD code = ::GetLastError();
        return Unexpected(win32_error(code, std::string(where), "UTF-16 to UTF-8 conversion failed"));
    }
    return utf8;
}

}  // namespace

Expected<std::wstring> utf8_to_wide(std::string_view utf8) {
    return to_wide(utf8, "core.text.utf8_to_wide");
}

Expected<std::string> wide_to_utf8(std::wstring_view wide) {
    return to_utf8(wide, "core.text.wide_to_utf8");
}

std::string wide_to_utf8_lossy(std::wstring_view wide) {
    std::string utf8;
    // Without WC_ERR_INVALID_CHARS, unpaired surrogates become U+FFFD. Convert in slices so
    // arbitrarily long input stays within the API's int lengths; slices never split a
    // surrogate pair.
    constexpr std::size_t kSlice = std::size_t{1} << 24;
    std::size_t start = 0;
    while (start < wide.size()) {
        std::size_t count = std::min(kSlice, wide.size() - start);
        if (start + count < wide.size() && is_high_surrogate(wide[start + count - 1])) {
            --count;  // keep the pair together (count >= 1 remains because kSlice > 1)
        }
        const wchar_t* data = wide.data() + start;
        const int length = static_cast<int>(count);
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, data, length, nullptr, 0, nullptr, nullptr);
        if (needed > 0) {
            const std::size_t offset = utf8.size();
            utf8.resize(offset + static_cast<std::size_t>(needed));
            ::WideCharToMultiByte(CP_UTF8, 0, data, length, utf8.data() + offset, needed, nullptr, nullptr);
        }
        start += count;
    }
    return utf8;
}

std::wstring utf8_to_wide_lossy(std::string_view utf8) {
    // Sanitising first keeps the replacement policy identical to sanitize_utf8() and makes the
    // conversion itself infallible.
    const std::string clean = sanitize_utf8(utf8);
    std::wstring wide;
    constexpr std::size_t kSlice = std::size_t{1} << 24;
    std::size_t start = 0;
    while (start < clean.size()) {
        std::size_t count = std::min(kSlice, clean.size() - start);
        // Move the slice end back to a sequence boundary (never inside a multi-byte sequence).
        while (start + count < clean.size() &&
               (static_cast<unsigned char>(clean[start + count]) & 0xC0) == 0x80) {
            --count;
        }
        const char* data = clean.data() + start;
        const int length = static_cast<int>(count);
        const int needed = ::MultiByteToWideChar(CP_UTF8, 0, data, length, nullptr, 0);
        if (needed > 0) {
            const std::size_t offset = wide.size();
            wide.resize(offset + static_cast<std::size_t>(needed));
            ::MultiByteToWideChar(CP_UTF8, 0, data, length, wide.data() + offset, needed);
        }
        start += count;
    }
    return wide;
}

Expected<std::string> path_to_utf8(const std::filesystem::path& path) {
    return to_utf8(path.native(), "core.text.path_to_utf8");
}

std::string path_to_utf8_lossy(const std::filesystem::path& path) {
    return wide_to_utf8_lossy(path.native());
}

Expected<std::filesystem::path> path_from_utf8(std::string_view utf8) {
    auto wide = to_wide(utf8, "core.text.path_from_utf8");
    if (!wide) {
        return Unexpected(std::move(wide.error()));
    }
    return std::filesystem::path(std::move(*wide));
}

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        // ASCII fast path.
        if (static_cast<unsigned char>(text[index]) < 0x80) {
            ++index;
            continue;
        }
        const Utf8Step step = utf8_step(text, index);
        if (!step.valid) {
            return false;
        }
        index += step.length;
    }
    return true;
}

std::string sanitize_utf8(std::string_view text) {
    if (is_valid_utf8(text)) {
        return std::string(text);
    }
    constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD
    std::string result;
    result.reserve(text.size() + 8);
    std::size_t index = 0;
    while (index < text.size()) {
        const Utf8Step step = utf8_step(text, index);
        if (step.valid) {
            result.append(text.substr(index, step.length));
        } else {
            result.append(kReplacement);
        }
        index += step.length;
    }
    return result;
}

}  // namespace lucent::core
