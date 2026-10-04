#include "output_publication.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <new>
#include <utility>

#include <windows.h>

#include <lucent/core/text.hpp>
#include <lucent/core/win32_error.hpp>
#include <lucent/workers/job_output.hpp>

#include "win32_paths.hpp"

namespace lucent::app::detail {

namespace {

constexpr std::string_view kWhereVerify = "app.batch.verify";
constexpr std::string_view kWhereSweep = "app.batch.sweep";
constexpr std::size_t kChunkBytes = std::size_t{1} << 20;
// Largest embedded ICC profile accepted when hashing it (Lucent's profiles are a few KiB).
constexpr std::uint64_t kMaxIccBytes = std::uint64_t{16} << 20;

HANDLE as_handle(void* handle) noexcept { return static_cast<HANDLE>(handle); }

std::string quoted(const std::filesystem::path& path) { return "'" + core::path_to_utf8_lossy(path) + "'"; }

std::int64_t filetime_ticks(const FILETIME& time) noexcept {
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime);
}

Expected<core::FileIdentity> query_identity(HANDLE handle, const std::filesystem::path& path, std::string_view where) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(handle, &info)) {
        return Unexpected(core::win32_error(::GetLastError(), std::string(where), "cannot query " + quoted(path)));
    }
    core::FileIdentity identity;
    identity.size_bytes = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    identity.last_write_time = filetime_ticks(info.ftLastWriteTime);
    FILE_ID_INFO id_info{};
    if (::GetFileInformationByHandleEx(handle, FileIdInfo, &id_info, sizeof(id_info))) {
        identity.volume_serial = id_info.VolumeSerialNumber;
        std::memcpy(identity.file_id.data(), id_info.FileId.Identifier, identity.file_id.size());
    } else {
        identity.volume_serial = info.dwVolumeSerialNumber;
        const std::uint64_t index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        for (std::size_t i = 0; i < 8; ++i) {
            identity.file_id[i] = static_cast<std::uint8_t>(index >> (8 * i));
        }
    }
    return identity;
}

// Layout-compatible with FILE_RENAME_INFO (see core's storage.cpp for the same structure).
struct RenameInfoHeader {
    DWORD flags;
    HANDLE root_directory;
    DWORD file_name_length;
    WCHAR file_name[1];
};
static_assert(offsetof(RenameInfoHeader, root_directory) == offsetof(FILE_RENAME_INFO, RootDirectory));
static_assert(offsetof(RenameInfoHeader, file_name_length) == offsetof(FILE_RENAME_INFO, FileNameLength));
static_assert(offsetof(RenameInfoHeader, file_name) == offsetof(FILE_RENAME_INFO, FileName));

DWORD rename_handle_no_clobber(HANDLE handle, const std::wstring& target) {
    const std::size_t name_bytes = target.size() * sizeof(wchar_t);
    if (name_bytes > std::numeric_limits<DWORD>::max() / 2) {
        return ERROR_FILENAME_EXCED_RANGE;
    }
    const std::size_t total = offsetof(RenameInfoHeader, file_name) + name_bytes + sizeof(wchar_t);
    std::vector<std::byte> buffer;
    try {
        buffer.resize(std::max(total, sizeof(RenameInfoHeader)));
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    auto* info = reinterpret_cast<RenameInfoHeader*>(buffer.data());
    info->flags = 0;  // ReplaceIfExists = FALSE: never replace anything
    info->root_directory = nullptr;
    info->file_name_length = static_cast<DWORD>(name_bytes);
    std::memcpy(info->file_name, target.c_str(), name_bytes + sizeof(wchar_t));
    if (!::SetFileInformationByHandle(handle, FileRenameInfo, buffer.data(), static_cast<DWORD>(buffer.size()))) {
        return ::GetLastError();
    }
    return ERROR_SUCCESS;
}

DWORD delete_handle(HANDLE handle) noexcept {
    // POSIX semantics (the name disappears at once) where supported, classic otherwise.
    struct DispositionEx {
        DWORD flags;
    };
    constexpr DWORD kDelete = 0x1;          // FILE_DISPOSITION_FLAG_DELETE
    constexpr DWORD kPosixSemantics = 0x2;  // FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
    constexpr auto kFileDispositionInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
    DispositionEx ex{kDelete | kPosixSemantics};
    if (::SetFileInformationByHandle(handle, kFileDispositionInfoEx, &ex, sizeof(ex))) {
        return ERROR_SUCCESS;
    }
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (!::SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition))) {
        return ::GetLastError();
    }
    return ERROR_SUCCESS;
}

std::uint16_t be16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>((std::to_integer<unsigned>(p[0]) << 8) | std::to_integer<unsigned>(p[1]));
}

class TiffReader {
public:
    TiffReader(bool little_endian) noexcept : little_(little_endian) {}
    [[nodiscard]] std::uint16_t u16(const std::byte* p) const noexcept {
        const unsigned a = std::to_integer<unsigned>(p[0]);
        const unsigned b = std::to_integer<unsigned>(p[1]);
        return static_cast<std::uint16_t>(little_ ? (a | (b << 8)) : ((a << 8) | b));
    }
    [[nodiscard]] std::uint32_t u32(const std::byte* p) const noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const auto byte = static_cast<std::uint32_t>(std::to_integer<unsigned>(p[little_ ? 3 - i : i]));
            v = (v << 8) | byte;
        }
        return v;
    }

private:
    bool little_;
};

Error corrupt(const std::filesystem::path& path, std::string_view what) {
    return Error{ErrorCode::Corrupt, std::format("{} is not a valid Lucent output: {}", quoted(path), what),
                 std::string(kWhereVerify), 0};
}

bool starts_with_folded(std::wstring_view text, std::wstring_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    return fold_case(text.substr(0, prefix.size())) == fold_case(prefix);
}

bool is_hex(wchar_t c) noexcept {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

// "~lucent-<16 hex digits>.tmp": the exporter's own temporary (exporter/export.hpp, step 3).
bool is_exporter_temporary(std::wstring_view name) {
    constexpr std::wstring_view kPrefix = L"~lucent-";
    constexpr std::wstring_view kSuffix = L".tmp";
    if (name.size() != kPrefix.size() + 16 + kSuffix.size() || !starts_with_folded(name, kPrefix) ||
        fold_case(name.substr(name.size() - kSuffix.size())) != fold_case(kSuffix)) {
        return false;
    }
    return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(kPrefix.size()),
                       name.begin() + static_cast<std::ptrdiff_t>(kPrefix.size() + 16), is_hex);
}

}  // namespace

// ---- HeldOutput ------------------------------------------------------------------------------

HeldOutput::~HeldOutput() { close(); }

HeldOutput::HeldOutput(HeldOutput&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), path_(std::move(other.path_)) {}

HeldOutput& HeldOutput::operator=(HeldOutput&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
        path_ = std::move(other.path_);
    }
    return *this;
}

void HeldOutput::close() noexcept {
    if (handle_ != nullptr) {
        ::CloseHandle(as_handle(handle_));
        handle_ = nullptr;
    }
}

Expected<HeldOutput> HeldOutput::open(const std::filesystem::path& path, std::string_view where) {
    auto api = api_path(path, where);
    if (!api) {
        return Unexpected(std::move(api.error()));
    }
    HANDLE handle = INVALID_HANDLE_VALUE;
    // A scanner (antivirus, indexer) may hold a freshly written file for a moment without sharing
    // writes: wait up to half a second for it before reporting the sharing violation.
    for (int attempt = 0;; ++attempt) {
        handle = ::CreateFileW(api->c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD code = ::GetLastError();
        if (code != ERROR_SHARING_VIOLATION || attempt >= 10) {
            return Unexpected(core::win32_error(code, std::string(where), "cannot open " + quoted(path)));
        }
        ::Sleep(50);
    }
    HeldOutput held(handle, path);
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) &&
        (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return fail(ErrorCode::InvalidArgument, quoted(path) + " is not a regular file", std::string(where));
    }
    return held;
}

Expected<core::FileIdentity> HeldOutput::identity() const {
    if (handle_ == nullptr) {
        return fail(ErrorCode::Internal, "no file is held", std::string(kWhereVerify));
    }
    return query_identity(as_handle(handle_), path_, kWhereVerify);
}

Expected<std::size_t> HeldOutput::read_at(std::uint64_t offset, std::span<std::byte> buffer) const {
    if (handle_ == nullptr) {
        return fail(ErrorCode::Internal, "no file is held", std::string(kWhereVerify));
    }
    std::size_t total = 0;
    while (total < buffer.size()) {
        OVERLAPPED overlapped{};
        const std::uint64_t at = offset + total;
        overlapped.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
        overlapped.OffsetHigh = static_cast<DWORD>(at >> 32);
        const DWORD piece = static_cast<DWORD>(std::min<std::size_t>(buffer.size() - total, std::size_t{1} << 30));
        DWORD read = 0;
        if (!::ReadFile(as_handle(handle_), buffer.data() + total, piece, &read, &overlapped)) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_HANDLE_EOF) {
                break;
            }
            return Unexpected(core::win32_error(code, std::string(kWhereVerify), "cannot read " + quoted(path_)));
        }
        if (read == 0) {
            break;
        }
        total += read;
    }
    return total;
}

Status HeldOutput::read_exact_at(std::uint64_t offset, std::span<std::byte> buffer) const {
    auto read = read_at(offset, buffer);
    if (!read) {
        return Unexpected(std::move(read.error()));
    }
    if (*read != buffer.size()) {
        return Unexpected(corrupt(path_, "unexpected end of file"));
    }
    return ok();
}

Expected<OutputDigest> HeldOutput::digest(const core::CancellationToken& cancel,
                                          const std::function<void()>& between_reads) const {
    auto before = identity();
    if (!before) {
        return Unexpected(std::move(before.error()));
    }
    std::vector<std::byte> chunk;
    try {
        chunk.resize(kChunkBytes);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the hashing buffer", std::string(kWhereVerify));
    }
    core::Sha256 hasher;
    std::uint64_t offset = 0;
    while (offset < before->size_bytes) {
        if (between_reads) {
            between_reads();
        }
        if (Status s = cancel.check(kWhereVerify); !s) {
            return Unexpected(std::move(s.error()));
        }
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkBytes, before->size_bytes - offset));
        auto read = read_at(offset, std::span(chunk.data(), want));
        if (!read) {
            return Unexpected(std::move(read.error()));
        }
        if (*read != want) {
            return Unexpected(corrupt(path_, "the file is shorter than its size"));
        }
        hasher.update(std::span<const std::byte>(chunk.data(), want));
        offset += want;
    }
    auto after = identity();
    if (!after) {
        return Unexpected(std::move(after.error()));
    }
    if (*after != *before) {
        // Cannot happen while the handle denies writers; a defect or a file-system anomaly.
        return fail(ErrorCode::SourceChanged, quoted(path_) + " changed while it was held", std::string(kWhereVerify));
    }
    auto sha = hasher.finish();
    if (!sha) {
        return Unexpected(std::move(sha.error()));
    }
    return OutputDigest{before->size_bytes, *sha};
}

Expected<OutputHeader> HeldOutput::inspect_header() const {
    auto id = identity();
    if (!id) {
        return Unexpected(std::move(id.error()));
    }
    std::array<std::byte, 4> magic{};
    if (Status s = read_exact_at(0, magic); !s) {
        return Unexpected(std::move(s.error()));
    }
    const unsigned m0 = std::to_integer<unsigned>(magic[0]);
    const unsigned m1 = std::to_integer<unsigned>(magic[1]);
    if (m0 == 0xFF && m1 == 0xD8) {
        return inspect_jpeg(id->size_bytes);
    }
    if ((m0 == 'I' && m1 == 'I') || (m0 == 'M' && m1 == 'M')) {
        return inspect_tiff(id->size_bytes);
    }
    return Unexpected(corrupt(path_, "neither a JPEG nor a TIFF file"));
}

Expected<OutputHeader> HeldOutput::inspect_jpeg(std::uint64_t size) const {
    OutputHeader header;
    header.format = exporter::ExportFormat::Jpeg;
    // ICC_PROFILE chunks: sequence number -> bytes.
    std::vector<std::vector<std::byte>> chunks;
    std::uint32_t chunk_count = 0;
    bool frame = false;
    std::uint64_t offset = 2;
    while (offset + 4 <= size) {
        std::array<std::byte, 4> marker{};
        if (Status s = read_exact_at(offset, marker); !s) {
            return Unexpected(std::move(s.error()));
        }
        if (std::to_integer<unsigned>(marker[0]) != 0xFF) {
            return Unexpected(corrupt(path_, std::format("no JPEG marker at byte {}", offset)));
        }
        const unsigned code = std::to_integer<unsigned>(marker[1]);
        if (code == 0xDA || code == 0xD9) {  // start of scan / end of image: headers are complete
            break;
        }
        const std::uint16_t length = be16(marker.data() + 2);
        if (length < 2 || offset + 2 + length > size) {
            return Unexpected(corrupt(path_, std::format("JPEG segment at byte {} has an invalid length", offset)));
        }
        const std::uint64_t body = offset + 4;
        const std::uint32_t body_bytes = length - 2u;
        const bool sof = code >= 0xC0 && code <= 0xCF && code != 0xC4 && code != 0xC8 && code != 0xCC;
        if (sof) {
            std::array<std::byte, 6> sof_bytes{};
            if (body_bytes < sof_bytes.size()) {
                return Unexpected(corrupt(path_, "short JPEG frame header"));
            }
            if (Status s = read_exact_at(body, sof_bytes); !s) {
                return Unexpected(std::move(s.error()));
            }
            header.bits_per_sample = std::to_integer<unsigned>(sof_bytes[0]);
            header.height = be16(sof_bytes.data() + 1);
            header.width = be16(sof_bytes.data() + 3);
            header.components = std::to_integer<unsigned>(sof_bytes[5]);
            frame = true;
        } else if (code == 0xE2 && body_bytes > 14) {
            std::array<std::byte, 14> id{};
            if (Status s = read_exact_at(body, id); !s) {
                return Unexpected(std::move(s.error()));
            }
            static constexpr char kIcc[12] = {'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', '\0'};
            if (std::memcmp(id.data(), kIcc, sizeof(kIcc)) == 0) {
                const unsigned sequence = std::to_integer<unsigned>(id[12]);
                const unsigned count = std::to_integer<unsigned>(id[13]);
                if (sequence == 0 || count == 0 || sequence > count || (chunk_count != 0 && chunk_count != count)) {
                    return Unexpected(corrupt(path_, "inconsistent ICC_PROFILE chunks"));
                }
                chunk_count = count;
                chunks.resize(count);
                if (!chunks[sequence - 1].empty()) {
                    return Unexpected(corrupt(path_, "a repeated ICC_PROFILE chunk"));
                }
                chunks[sequence - 1].resize(body_bytes - 14);
                if (Status s = read_exact_at(body + 14, chunks[sequence - 1]); !s) {
                    return Unexpected(std::move(s.error()));
                }
            }
        }
        offset += 2u + length;
    }
    if (!frame) {
        return Unexpected(corrupt(path_, "no JPEG frame header"));
    }
    if (chunk_count != 0) {
        core::Sha256 hasher;
        for (const std::vector<std::byte>& chunk : chunks) {
            if (chunk.empty()) {
                return Unexpected(corrupt(path_, "a missing ICC_PROFILE chunk"));
            }
            hasher.update(chunk);
        }
        auto sha = hasher.finish();
        if (!sha) {
            return Unexpected(std::move(sha.error()));
        }
        header.icc_sha256 = *sha;
    }
    return header;
}

Expected<OutputHeader> HeldOutput::inspect_tiff(std::uint64_t size) const {
    OutputHeader header;
    header.format = exporter::ExportFormat::Tiff;
    std::array<std::byte, 8> head{};
    if (Status s = read_exact_at(0, head); !s) {
        return Unexpected(std::move(s.error()));
    }
    const bool little = std::to_integer<unsigned>(head[0]) == 'I';
    const TiffReader r(little);
    if (r.u16(head.data() + 2) != 42) {
        return Unexpected(corrupt(path_, "not a classic TIFF file"));
    }
    const std::uint32_t ifd = r.u32(head.data() + 4);
    if (ifd < 8 || std::uint64_t{ifd} + 2 > size) {
        return Unexpected(corrupt(path_, "the first IFD offset is invalid"));
    }
    std::array<std::byte, 2> count_bytes{};
    if (Status s = read_exact_at(ifd, count_bytes); !s) {
        return Unexpected(std::move(s.error()));
    }
    const std::uint16_t entries = r.u16(count_bytes.data());
    std::vector<std::byte> table(static_cast<std::size_t>(entries) * 12);
    if (Status s = read_exact_at(std::uint64_t{ifd} + 2, table); !s) {
        return Unexpected(std::move(s.error()));
    }
    std::optional<std::pair<std::uint32_t, std::uint32_t>> icc;  // offset, count
    std::uint32_t bits_count = 0;
    std::uint32_t bits_value_or_offset = 0;
    for (std::uint16_t i = 0; i < entries; ++i) {
        const std::byte* e = table.data() + static_cast<std::size_t>(i) * 12;
        const std::uint16_t tag = r.u16(e);
        const std::uint16_t type = r.u16(e + 2);
        const std::uint32_t count = r.u32(e + 4);
        const std::uint32_t value = type == 3 && count == 1 ? r.u16(e + 8) : r.u32(e + 8);
        switch (tag) {
            case 256:
                header.width = value;
                break;
            case 257:
                header.height = value;
                break;
            case 258:
                bits_count = count;
                bits_value_or_offset = type == 3 && count <= 2 ? r.u16(e + 8) : r.u32(e + 8);
                break;
            case 277:
                header.components = value;
                break;
            case 34675:
                icc = std::make_pair(r.u32(e + 8), count);
                break;
            default:
                break;
        }
    }
    if (bits_count == 1 || bits_count == 2) {
        header.bits_per_sample = bits_value_or_offset;
    } else if (bits_count > 2 && bits_count <= 16) {
        std::vector<std::byte> bits(static_cast<std::size_t>(bits_count) * 2);
        if (Status s = read_exact_at(bits_value_or_offset, bits); !s) {
            return Unexpected(std::move(s.error()));
        }
        header.bits_per_sample = r.u16(bits.data());
        for (std::uint32_t i = 1; i < bits_count; ++i) {
            if (r.u16(bits.data() + 2 * i) != header.bits_per_sample) {
                header.bits_per_sample = 0;  // mixed sample sizes: never Lucent's
            }
        }
    }
    if (header.width == 0 || header.height == 0) {
        return Unexpected(corrupt(path_, "the TIFF has no image dimensions"));
    }
    if (icc) {
        if (icc->second == 0 || icc->second > kMaxIccBytes || std::uint64_t{icc->first} + icc->second > size) {
            return Unexpected(corrupt(path_, "the TIFF ICC profile tag is invalid"));
        }
        std::vector<std::byte> profile(icc->second);
        if (Status s = read_exact_at(icc->first, profile); !s) {
            return Unexpected(std::move(s.error()));
        }
        auto sha = core::sha256(profile);
        if (!sha) {
            return Unexpected(std::move(sha.error()));
        }
        header.icc_sha256 = *sha;
    }
    return header;
}

Status HeldOutput::rename_no_clobber(const std::filesystem::path& target) {
    if (handle_ == nullptr) {
        return fail(ErrorCode::Internal, "no file is held", "app.batch.publish");
    }
    auto api = api_path(target, "app.batch.publish");
    if (!api) {
        return Unexpected(std::move(api.error()));
    }
    const DWORD code = rename_handle_no_clobber(as_handle(handle_), *api);
    if (code != ERROR_SUCCESS) {
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
            return fail(ErrorCode::AlreadyExists, quoted(target) + " already exists", "app.batch.publish",
                        static_cast<std::int64_t>(code));
        }
        return Unexpected(core::win32_error(code, "app.batch.publish",
                                            std::format("cannot rename {} to {}", quoted(path_), quoted(target))));
    }
    path_ = target;
    return ok();
}

Status HeldOutput::flush() {
    if (handle_ == nullptr) {
        return fail(ErrorCode::Internal, "no file is held", "app.batch.publish");
    }
    if (!::FlushFileBuffers(as_handle(handle_))) {
        return Unexpected(core::win32_error(::GetLastError(), "app.batch.publish", "cannot flush " + quoted(path_)));
    }
    return ok();
}

Status HeldOutput::remove() {
    if (handle_ == nullptr) {
        return fail(ErrorCode::Internal, "no file is held", "app.batch.cleanup");
    }
    const DWORD code = delete_handle(as_handle(handle_));
    close();
    if (code != ERROR_SUCCESS) {
        return Unexpected(core::win32_error(code, "app.batch.cleanup", "cannot delete " + quoted(path_)));
    }
    return ok();
}

Expected<HeldOutput> HeldOutput::open_exclusive(const std::filesystem::path& path, std::string_view where) {
    auto api = api_path(path, where);
    if (!api) {
        return Unexpected(std::move(api.error()));
    }
    // FILE_FLAG_OPEN_REPARSE_POINT: a link is handled as itself, never followed to its target.
    HANDLE handle = ::CreateFileW(api->c_str(), DELETE | FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Unexpected(core::win32_error(::GetLastError(), std::string(where), "cannot open " + quoted(path)));
    }
    HeldOutput held(handle, path);
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) &&
        (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return fail(ErrorCode::InvalidArgument, quoted(path) + " is not a regular file", std::string(where));
    }
    return held;
}

// ---- Free functions --------------------------------------------------------------------------

Status remove_unused_file(const std::filesystem::path& path, std::string_view where) {
    auto held = HeldOutput::open_exclusive(path, where);
    if (!held) {
        if (held.error().code == ErrorCode::NotFound) {
            return ok();
        }
        return Unexpected(std::move(held.error()));
    }
    return held->remove();
}

Status probe_writable(const std::filesystem::path& directory, std::wstring_view tag) {
    constexpr std::string_view where = "app.batch.destination";
    const std::filesystem::path probe = directory / (L"~lucent-" + std::wstring(tag) + L"-probe.tmp");
    auto api = api_path(probe, where);
    if (!api) {
        return Unexpected(std::move(api.error()));
    }
    HANDLE handle = ::CreateFileW(api->c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Unexpected(core::win32_error(::GetLastError(), std::string(where),
                                            "cannot create a file in " + quoted(directory)));
    }
    const char byte = 0;
    DWORD written = 0;
    const BOOL wrote = ::WriteFile(handle, &byte, 1, &written, nullptr);
    const DWORD code = wrote ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(handle);  // FILE_FLAG_DELETE_ON_CLOSE removes it
    if (!wrote) {
        return Unexpected(core::win32_error(code, std::string(where), "cannot write in " + quoted(directory)));
    }
    return ok();
}

std::wstring temp_file_name(std::wstring_view tag, std::int64_t job, std::uint32_t attempt) {
    return std::format(L"~lucent-{}-{}-a{}.tmp", tag, job, attempt);
}

std::wstring aside_file_name(std::wstring_view tag, std::int64_t job, std::uint32_t attempt) {
    return std::format(L"~lucent-{}-{}-a{}-replaced.tmp", tag, job, attempt);
}

SweepResult sweep_publication_leftovers(const std::filesystem::path& directory, std::span<const std::wstring> tags,
                                        std::span<const std::filesystem::path> keep,
                                        std::chrono::seconds exporter_min_age) {
    SweepResult result;
    auto entries = list_directory(directory, kWhereSweep);
    if (!entries) {
        return result;
    }
    std::vector<std::wstring> kept;
    kept.reserve(keep.size());
    for (const std::filesystem::path& path : keep) {
        kept.push_back(fold_case(path.lexically_normal().native()));
    }
    const auto is_kept = [&](const std::filesystem::path& path) {
        const std::wstring key = fold_case(path.lexically_normal().native());
        return std::find(kept.begin(), kept.end(), key) != kept.end();
    };
    FILETIME now_ft{};
    ::GetSystemTimeAsFileTime(&now_ft);
    const std::int64_t now = filetime_ticks(now_ft);
    const std::int64_t min_age_ticks = static_cast<std::int64_t>(exporter_min_age.count()) * 10'000'000;

    for (const DirectoryEntry& entry : *entries) {
        if (entry.reparse_point || entry.device) {
            continue;
        }
        const std::wstring_view name = entry.name;
        bool ours = false;
        for (const std::wstring& tag : tags) {
            if (starts_with_folded(name, L"~lucent-" + tag + L"-")) {
                ours = true;
                break;
            }
        }
        const std::filesystem::path path = directory / entry.name;
        if (ours) {
            constexpr std::wstring_view kParts = L".parts";
            if (entry.directory) {
                if (name.size() > kParts.size() && fold_case(name.substr(name.size() - kParts.size())) ==
                                                       fold_case(kParts)) {
                    const std::filesystem::path temp = directory / std::wstring(name.substr(0, name.size() - kParts.size()));
                    if (is_kept(temp)) {
                        continue;
                    }
                    if (workers::remove_job_scratch_directory(temp)) {
                        ++result.removed;
                        result.removed_paths.push_back(path);
                    } else {
                        ++result.in_use;
                    }
                }
                continue;
            }
            if (is_kept(path)) {
                continue;
            }
            if (remove_unused_file(path, kWhereSweep)) {
                ++result.removed;
                result.removed_paths.push_back(path);
            } else {
                ++result.in_use;
            }
            continue;
        }
        if (!entry.directory && is_exporter_temporary(name)) {
            auto id = core::stat_file(path);
            if (!id || now - id->last_write_time < min_age_ticks) {
                continue;
            }
            if (remove_unused_file(path, kWhereSweep)) {
                ++result.removed;
                result.removed_paths.push_back(path);
            } else {
                ++result.in_use;
            }
        }
    }
    return result;
}

}  // namespace lucent::app::detail
