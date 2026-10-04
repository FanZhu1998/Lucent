#include <lucent/application/batch_destination.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <utility>

#include <lucent/application/export_service.hpp>
#include <lucent/core/text.hpp>

#include "win32_paths.hpp"

namespace lucent::app {

namespace {

constexpr std::string_view kWhereName = "app.batch.name";
constexpr std::string_view kWhereDestination = "app.batch.destination";

bool is_high_surrogate(wchar_t c) noexcept { return c >= 0xD800 && c <= 0xDBFF; }
bool is_low_surrogate(wchar_t c) noexcept { return c >= 0xDC00 && c <= 0xDFFF; }

bool is_forbidden_char(wchar_t c) noexcept {
    if (c < 0x20) {
        return true;
    }
    switch (c) {
        case L'<':
        case L'>':
        case L':':
        case L'"':
        case L'/':
        case L'\\':
        case L'|':
        case L'?':
        case L'*':
            return true;
        default:
            return false;
    }
}

// Removes trailing dots and spaces (Windows strips them when it creates a file).
void trim_trailing_dots_and_spaces(std::wstring& text) {
    while (!text.empty() && (text.back() == L'.' || text.back() == L' ')) {
        text.pop_back();
    }
}

// Shortens `text` to at most `units` UTF-16 units without leaving a lone high surrogate.
void truncate_units(std::wstring& text, std::size_t units) {
    if (text.size() <= units) {
        return;
    }
    std::size_t keep = units;
    if (keep > 0 && is_high_surrogate(text[keep - 1])) {
        --keep;
    }
    text.resize(keep);
}

// Replaces unpaired surrogates with U+FFFD.
std::wstring repair_surrogates(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (is_high_surrogate(c)) {
            if (i + 1 < text.size() && is_low_surrogate(text[i + 1])) {
                out.push_back(c);
                out.push_back(text[i + 1]);
                ++i;
            } else {
                out.push_back(L'�');
            }
        } else if (is_low_surrogate(c)) {
            out.push_back(L'�');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

Expected<std::wstring> to_wide(std::string_view text, std::string_view where) {
    auto wide = core::utf8_to_wide(text);
    if (!wide) {
        return fail(ErrorCode::InvalidArgument, "the name pattern is not valid UTF-8", std::string(where));
    }
    return std::move(*wide);
}

}  // namespace

std::string_view to_string(CollisionPolicy policy) noexcept {
    switch (policy) {
        case CollisionPolicy::UniqueName:
            return "unique_name";
        case CollisionPolicy::Overwrite:
            return "overwrite";
    }
    return "invalid";
}

Expected<CollisionPolicy> collision_policy_from_string(std::string_view text) {
    if (text == "unique_name") {
        return CollisionPolicy::UniqueName;
    }
    if (text == "overwrite") {
        return CollisionPolicy::Overwrite;
    }
    return fail(ErrorCode::InvalidArgument, std::format("unknown collision policy '{}'", text),
                std::string(kWhereDestination));
}

bool is_reserved_device_name(std::wstring_view name) noexcept {
    std::wstring_view base = name.substr(0, name.find(L'.'));
    while (!base.empty() && base.back() == L' ') {
        base.remove_suffix(1);
    }
    if (base.size() < 3 || base.size() > 7) {
        return false;
    }
    std::wstring upper;
    upper.reserve(base.size());
    for (const wchar_t c : base) {
        upper.push_back(c >= L'a' && c <= L'z' ? static_cast<wchar_t>(c - L'a' + L'A') : c);
    }
    static constexpr std::array<std::wstring_view, 6> kPlain = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$",
                                                                L"CONOUT$"};
    if (std::find(kPlain.begin(), kPlain.end(), upper) != kPlain.end()) {
        return true;
    }
    if (upper.size() == 4 && (upper.starts_with(L"COM") || upper.starts_with(L"LPT"))) {
        const wchar_t digit = upper[3];
        // Windows also reserves the superscript digits 1-3 after COM / LPT.
        return (digit >= L'0' && digit <= L'9') || digit == L'¹' || digit == L'²' || digit == L'³';
    }
    return false;
}

std::wstring sanitize_file_name(std::wstring_view name, std::size_t max_units) {
    std::wstring out = repair_surrogates(name);
    for (wchar_t& c : out) {
        if (is_forbidden_char(c)) {
            c = L'_';
        }
    }
    trim_trailing_dots_and_spaces(out);
    if (is_reserved_device_name(out)) {
        out.insert(out.begin(), L'_');
    }
    truncate_units(out, std::max<std::size_t>(1, max_units));
    trim_trailing_dots_and_spaces(out);
    if (out.empty()) {
        out = L"_";
    }
    return out;
}

Status validate_file_name(std::wstring_view name) {
    if (name.empty()) {
        return fail(ErrorCode::InvalidArgument, "a file name cannot be empty", std::string(kWhereName));
    }
    if (name.size() > kMaxFileNameUnits) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("a file name has at most {} characters ({} given)", kMaxFileNameUnits, name.size()),
                    std::string(kWhereName));
    }
    if (name == L"." || name == L"..") {
        return fail(ErrorCode::InvalidArgument, "'.' and '..' are not file names", std::string(kWhereName));
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const wchar_t c = name[i];
        if (is_forbidden_char(c)) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("character U+{:04X} is not allowed in a Windows file name",
                                    static_cast<unsigned>(c)),
                        std::string(kWhereName));
        }
        if (is_high_surrogate(c)) {
            if (i + 1 >= name.size() || !is_low_surrogate(name[i + 1])) {
                return fail(ErrorCode::InvalidArgument, "the file name is not valid Unicode",
                            std::string(kWhereName));
            }
            ++i;
        } else if (is_low_surrogate(c)) {
            return fail(ErrorCode::InvalidArgument, "the file name is not valid Unicode", std::string(kWhereName));
        }
    }
    if (name.back() == L'.' || name.back() == L' ') {
        return fail(ErrorCode::InvalidArgument, "a Windows file name cannot end with a dot or a space",
                    std::string(kWhereName));
    }
    if (is_reserved_device_name(name)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("'{}' is a reserved Windows device name", core::wide_to_utf8_lossy(name)),
                    std::string(kWhereName));
    }
    return ok();
}

Expected<NamePattern> parse_name_pattern(std::string_view pattern) {
    if (pattern.empty()) {
        return fail(ErrorCode::InvalidArgument, "the name pattern is empty", std::string(kWhereName));
    }
    if (pattern.size() > kMaxNamePatternBytes) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the name pattern has more than {} bytes", kMaxNamePatternBytes),
                    std::string(kWhereName));
    }
    auto wide = to_wide(pattern, kWhereName);
    if (!wide) {
        return Unexpected(std::move(wide.error()));
    }
    NamePattern parsed;
    parsed.text = std::string(pattern);
    bool has_stem = false;
    bool has_sequence = false;
    std::wstring literal;
    const auto flush_literal = [&] {
        if (!literal.empty()) {
            parsed.parts.push_back(NamePatternPart{NameToken::Literal, std::move(literal)});
            literal.clear();
        }
    };
    const std::wstring& text = *wide;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'}') {
            return fail(ErrorCode::InvalidArgument, "the name pattern has a '}' without '{'", std::string(kWhereName));
        }
        if (c == L'{') {
            const std::size_t close = text.find(L'}', i + 1);
            if (close == std::wstring::npos) {
                return fail(ErrorCode::InvalidArgument, "the name pattern has a '{' without '}'",
                            std::string(kWhereName));
            }
            const std::wstring_view token = std::wstring_view(text).substr(i + 1, close - i - 1);
            NameToken kind = NameToken::Literal;
            if (token == L"original-stem") {
                kind = NameToken::OriginalStem;
                has_stem = true;
            } else if (token == L"revision") {
                kind = NameToken::Revision;
            } else if (token == L"image") {
                kind = NameToken::Image;
                parsed.has_image = true;
            } else if (token == L"sequence") {
                kind = NameToken::Sequence;
                has_sequence = true;
            } else {
                return fail(ErrorCode::InvalidArgument,
                            std::format("unknown token '{{{}}}' in the name pattern (known: {{original-stem}}, "
                                        "{{revision}}, {{image}}, {{sequence}})",
                                        core::wide_to_utf8_lossy(token)),
                            std::string(kWhereName));
            }
            flush_literal();
            parsed.parts.push_back(NamePatternPart{kind, {}});
            i = close;
            continue;
        }
        if (is_forbidden_char(c)) {
            return fail(ErrorCode::InvalidArgument,
                        c == L'/' || c == L'\\'
                            ? std::string("the name pattern cannot contain a folder separator; use mirrored "
                                          "subfolders for folders")
                            : std::format("character U+{:04X} is not allowed in a Windows file name",
                                          static_cast<unsigned>(c)),
                        std::string(kWhereName));
        }
        literal.push_back(c);
    }
    flush_literal();
    if (!has_stem && !has_sequence) {
        return fail(ErrorCode::InvalidArgument,
                    "the name pattern must contain {original-stem} or {sequence} so that outputs get distinct names",
                    std::string(kWhereName));
    }
    return parsed;
}

Expected<std::filesystem::path> expand_output_name(const NamePattern& pattern, const OutputNameFields& fields,
                                                   exporter::ExportFormat format, std::uint32_t index) {
    const std::wstring_view extension = export_extension(format);
    if (extension.empty()) {
        return fail(ErrorCode::InvalidArgument, "unknown export format", std::string(kWhereName));
    }
    if (index == 0) {
        return fail(ErrorCode::InvalidArgument, "name indices start at 1", std::string(kWhereName));
    }
    if (index > kMaxOutputNameIndex) {
        return fail(ErrorCode::LimitExceeded, std::format("name index {} is above {}", index, kMaxOutputNameIndex),
                    std::string(kWhereName));
    }
    if (fields.subimage_count == 0 || fields.subimage >= fields.subimage_count) {
        return fail(ErrorCode::InvalidArgument, std::format("image {} of {}", fields.subimage, fields.subimage_count),
                    std::string(kWhereName));
    }
    if (!fields.source.has_filename()) {
        return fail(ErrorCode::InvalidArgument, "the source path has no file name", std::string(kWhereName));
    }
    std::wstring stem = fields.source.stem().native();
    if (stem.empty()) {
        stem = L"image";
    }
    std::wstring body;
    for (const NamePatternPart& part : pattern.parts) {
        switch (part.token) {
            case NameToken::Literal:
                body += part.literal;
                break;
            case NameToken::OriginalStem:
                body += stem;
                break;
            case NameToken::Revision:
                body += std::to_wstring(fields.revision_number);
                break;
            case NameToken::Image:
                body += std::to_wstring(fields.subimage + 1);
                break;
            case NameToken::Sequence: {
                std::wstring number = std::to_wstring(static_cast<std::uint64_t>(fields.ordinal) + 1);
                if (number.size() < 4) {
                    number.insert(0, 4 - number.size(), L'0');
                }
                body += number;
                break;
            }
        }
    }
    std::wstring suffix;
    if (fields.subimage_count > 1 && !pattern.has_image) {
        suffix += L"_img" + std::to_wstring(fields.subimage + 1);
    }
    if (index >= 2) {
        suffix += L"_" + std::to_wstring(index);
    }
    suffix += extension;
    // The body is sanitised on its own (a source stem is valid on the source's volume, but literal
    // text and other volumes' rules are checked again) and shortened to leave room for the suffix.
    // Windows reserves a device name before the first dot ("CON.jpg"), so the body decides.
    const std::size_t room = kMaxFileNameUnits - suffix.size();
    std::wstring name = sanitize_file_name(body, room);
    name += suffix;
    if (Status valid = validate_file_name(name); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    return std::filesystem::path(std::move(name));
}

std::filesystem::path output_directory_for(const std::filesystem::path& folder, bool mirror_subfolders,
                                           const std::filesystem::path& mirror_root,
                                           const std::filesystem::path& source) {
    if (!mirror_subfolders || mirror_root.empty()) {
        return folder;
    }
    const std::filesystem::path parent = source.parent_path();
    if (!detail::is_same_or_within(parent, mirror_root)) {
        return folder;
    }
    // The components of `parent` below the root, in their on-disk spelling.
    std::vector<std::wstring> root_parts;
    for (const auto& part : mirror_root.relative_path()) {
        if (!part.empty()) {
            root_parts.push_back(part.native());
        }
    }
    std::vector<std::wstring> parent_parts;
    for (const auto& part : parent.relative_path()) {
        if (!part.empty()) {
            parent_parts.push_back(part.native());
        }
    }
    std::filesystem::path directory = folder;
    for (std::size_t i = root_parts.size(); i < parent_parts.size(); ++i) {
        directory /= sanitize_file_name(parent_parts[i]);
    }
    return directory;
}

std::filesystem::path common_parent_folder(const std::vector<std::filesystem::path>& sources) {
    if (sources.empty()) {
        return {};
    }
    std::filesystem::path common = sources.front().parent_path();
    for (std::size_t i = 1; i < sources.size() && !common.empty(); ++i) {
        const std::filesystem::path parent = sources[i].parent_path();
        while (!common.empty() && !detail::is_same_or_within(parent, common)) {
            const std::filesystem::path up = common.parent_path();
            if (up == common) {
                common.clear();
                break;
            }
            common = up;
        }
    }
    return common;
}

std::wstring folded_path_key(const std::filesystem::path& path) {
    return detail::fold_case(path.lexically_normal().native());
}

}  // namespace lucent::app
