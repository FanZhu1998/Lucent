#pragma once

// UTF-8 / UTF-16 conversion and path <-> UTF-8 helpers.
//
// Lucent's API, log, JSON and catalog strings are UTF-8 (std::string). Windows APIs and
// std::filesystem::path on Windows are UTF-16 (wchar_t). These helpers are the only sanctioned
// way to cross that boundary (docs/development/conventions.md, "Files, paths and text").
//
// Strict functions (utf8_to_wide, wide_to_utf8, path_to_utf8, path_from_utf8) never substitute
// characters: malformed input is an ErrorCode::InvalidArgument error, so a value that is stored
// (for example a source path in the catalog) always converts back to exactly the same value.
// The *_lossy / sanitize functions replace malformed sequences with U+FFFD and are meant only
// for display and logging.
//
// Thread-safety: all functions are pure and safe to call concurrently.

#include <filesystem>
#include <string>
#include <string_view>

#include "lucent/core/error.hpp"

namespace lucent::core {

// Converts UTF-8 to UTF-16. Fails with InvalidArgument on any malformed sequence (overlong
// forms, encoded surrogates, code points above U+10FFFF, truncated sequences) and with
// LimitExceeded when the input is longer than INT_MAX bytes. Embedded NULs are preserved.
[[nodiscard]] Expected<std::wstring> utf8_to_wide(std::string_view utf8);

// Converts UTF-16 to UTF-8. Fails with InvalidArgument on unpaired surrogates and with
// LimitExceeded when the input is longer than INT_MAX code units. Embedded NULs are preserved.
[[nodiscard]] Expected<std::string> wide_to_utf8(std::wstring_view wide);

// Converts UTF-16 to UTF-8, replacing each unpaired surrogate with U+FFFD. Never fails except
// by std::bad_alloc. Use only for display and diagnostics.
[[nodiscard]] std::string wide_to_utf8_lossy(std::wstring_view wide);

// Converts UTF-8 to UTF-16, replacing malformed sequences with U+FFFD. Never fails except by
// std::bad_alloc. Use only for display and diagnostics (for example OutputDebugStringW).
[[nodiscard]] std::wstring utf8_to_wide_lossy(std::string_view utf8);

// The path's native UTF-16 form as UTF-8. NTFS permits names that are not valid UTF-16
// (unpaired surrogates); such a path fails with InvalidArgument rather than being altered,
// because a lossy value could never be used to reopen the file.
[[nodiscard]] Expected<std::string> path_to_utf8(const std::filesystem::path& path);

// Display form of a path for messages and logs: like path_to_utf8, but unpaired surrogates
// become U+FFFD. Never fails except by std::bad_alloc.
[[nodiscard]] std::string path_to_utf8_lossy(const std::filesystem::path& path);

// Builds a path from UTF-8 text. Fails with InvalidArgument on malformed UTF-8. The text is
// not otherwise validated as a path; opening functions report invalid names themselves.
[[nodiscard]] Expected<std::filesystem::path> path_from_utf8(std::string_view utf8);

// True when `text` is well-formed UTF-8 (RFC 3629: no overlong forms, no surrogates, nothing
// above U+10FFFF). The empty string is valid.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

// Returns `text` with every maximal ill-formed subsequence replaced by U+FFFD (the Unicode
// "substitution of maximal subparts" practice). Well-formed input is returned unchanged. Use it
// for strings from untrusted metadata (camera make/model, EXIF text) before they reach JSON,
// logs or the catalog.
[[nodiscard]] std::string sanitize_utf8(std::string_view text);

}  // namespace lucent::core
