#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <format>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <lucent/application/import.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/storage.hpp>
#include <lucent/core/text.hpp>
#include <lucent/raw/decoded_raw.hpp>

#include "win32_paths.hpp"

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.import";
constexpr std::string_view kWhereCopy = "app.import.copy";

// Lower-case extensions without the dot. RAW: LibRaw's supported containers as named by camera
// makers (a hint only; the probe decides).
constexpr std::array<std::wstring_view, 44> kRawExtensions = {
    L"3fr", L"ari", L"arw", L"bay", L"cap", L"cr2", L"cr3", L"crw", L"cs1", L"dc2", L"dcr",
    L"dcs", L"dng", L"drf", L"eip", L"erf", L"fff", L"gpr", L"iiq", L"k25", L"kc2", L"kdc",
    L"mdc", L"mef", L"mos", L"mrw", L"nef", L"nrw", L"orf", L"ori", L"pef", L"ptx", L"pxn",
    L"qtk", L"raf", L"raw", L"rw2", L"rwl", L"rwz", L"sr2", L"srf", L"srw", L"sti", L"x3f",
};
constexpr std::array<std::wstring_view, 48> kNonRawExtensions = {
    L"jpg",  L"jpeg", L"jpe",  L"jfif", L"png", L"gif", L"bmp", L"tif",  L"tiff", L"heic", L"heif", L"hif",
    L"avif", L"webp", L"jxl",  L"psd",  L"psb", L"xmp", L"xml", L"json", L"txt",  L"md",   L"pdf",  L"ini",
    L"db",   L"thm",  L"lrv",  L"lrf",  L"mov", L"mp4", L"m4v", L"avi",  L"mts",  L"m2ts", L"3gp",  L"mkv",
    L"wav",  L"mp3",  L"insv", L"insp", L"lnk", L"exe", L"dll", L"zip",  L"pp3",  L"dop",  L"cos",  L"tmp",
};

std::wstring lower_ascii(std::wstring_view text) {
    std::wstring out(text);
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return out;
}

// The exporter's publication temporaries ("~lucent-*.tmp") and the temporary siblings of core's
// atomic writes and verified copies (".lucent-<hex>.tmp").
bool is_lucent_temporary(std::wstring_view name) {
    const std::wstring lower = lower_ascii(name);
    if (lower.starts_with(L"~lucent-") && lower.ends_with(L".tmp")) {
        return true;
    }
    return core::is_temporary_file_name(std::filesystem::path(name));
}

// Key identifying a folder's file object: volume serial + file id, or the folded canonical path
// when the file system reports no file id.
std::string folder_key(const core::FileIdentity& identity, const std::filesystem::path& canonical) {
    const bool has_id =
        std::any_of(identity.file_id.begin(), identity.file_id.end(), [](std::uint8_t b) { return b != 0; });
    if (!has_id) {
        return "path:" + core::wide_to_utf8_lossy(detail::fold_case(canonical.native()));
    }
    std::string key(sizeof(identity.volume_serial) + identity.file_id.size(), '\0');
    std::memcpy(key.data(), &identity.volume_serial, sizeof(identity.volume_serial));
    std::memcpy(key.data() + sizeof(identity.volume_serial), identity.file_id.data(),
                identity.file_id.size());
    return "id:" + key;
}

SkipReason reason_for(const Error& error) {
    return error.code == ErrorCode::NotFound ? SkipReason::NotFound : SkipReason::Inaccessible;
}

// True when the library can store `path`: valid UTF-16. NTFS also accepts names with unpaired
// surrogates, which no UTF-8 text (catalog, logs, journal) can represent.
bool has_valid_name(const std::filesystem::path& path) {
    return core::path_to_utf8(path).has_value();
}

Error invalid_name_error() {
    return Error{
        ErrorCode::InvalidArgument,
        "the name is not valid Unicode (it contains an unpaired surrogate), so Lucent cannot store it",
        std::string(kWhere), 0};
}

bool is_photograph_role(raw::SubimageRole role) {
    return role != raw::SubimageRole::DualPixelComponent;
}

Status validate_copy_options(const core::CopyOptions& options) {
    constexpr std::size_t kUnit = std::size_t{64} << 10;
    if (options.chunk_bytes < kUnit || options.chunk_bytes % kUnit != 0 ||
        options.chunk_bytes > core::kMaxHashChunkBytes ||
        (options.name_policy != core::CopyNamePolicy::FailIfExists &&
         options.name_policy != core::CopyNamePolicy::MakeUnique)) {
        return fail(ErrorCode::InvalidArgument, "invalid copy options", std::string(kWhereCopy));
    }
    return ok();
}

class Importer {
public:
    Importer(Library& library, const raw::DecoderRegistry& registry, const ImportRequest& request,
             const ImportProgressCallback& progress, core::CancellationToken cancel)
        : library_(library),
          registry_(registry),
          request_(request),
          progress_(progress),
          cancel_(std::move(cancel)) {}

    Expected<ImportReport> run() {
        const auto start = std::chrono::steady_clock::now();
        if (request_.batch_size == 0) {
            return fail(ErrorCode::InvalidArgument, "batch_size must be at least 1", std::string(kWhere));
        }
        if (auto valid = request_.probe_limits.validate(); !valid) {
            return Unexpected(std::move(valid.error()));
        }
        if (request_.copy_to) {
            if (auto valid = validate_copy_options(request_.copy_options); !valid) {
                return Unexpected(std::move(valid.error()));
            }
            auto destination = detail::canonical_path(*request_.copy_to, kWhereCopy);
            if (!destination) {
                return Unexpected(std::move(destination.error()));
            }
            if (!detail::is_directory(*destination)) {
                return fail(ErrorCode::InvalidArgument, "the library folder for copies is not a folder",
                            std::string(kWhereCopy));
            }
            copy_root_ = std::move(*destination);
        }
        capabilities_ = library_.capabilities();
        if (request_.record) {
            ImportJournalStart journal;
            for (const std::filesystem::path& file : request_.files) {
                journal.roots.push_back(ImportJournalRoot{file, false});
            }
            for (const ImportFolder& folder : request_.folders) {
                journal.roots.push_back(ImportJournalRoot{folder.path, folder.include_subfolders});
            }
            journal.copy_to = copy_root_.empty() ? std::nullopt : std::optional(copy_root_);
            // A root the journal cannot store is reported (InvalidName) by the scan instead.
            std::erase_if(journal.roots,
                          [](const ImportJournalRoot& root) { return !has_valid_name(root.path); });
            auto record = library_.begin_import(journal);
            if (!record) {
                return Unexpected(std::move(record.error()));
            }
            report_.record = *record;
        }
        Status body = run_body();
        discard_unregistered_copies();
        if (!body) {
            finish_record(ImportJournalOutcome::Failed);
            return Unexpected(std::move(body.error()));
        }
        report_.directories_scanned = state_.directories_scanned;
        report_.files_seen = state_.files_seen;
        report_.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        finish_record(report_.cancelled ? ImportJournalOutcome::Cancelled : ImportJournalOutcome::Completed);
        state_.phase = ImportPhase::Done;
        state_.current.clear();
        notify();
        return std::move(report_);
    }

private:
    struct Candidate {
        std::filesystem::path path;  // canonical
        // Selected explicitly with a non-RAW extension: a probe failure means "not a RAW photo".
        bool not_raw_hint = false;
        // The canonical import folder the file was found in (empty for selected files).
        std::filesystem::path root;
    };

    // What is known about the file being probed when it is a verified copy.
    struct CopyContext {
        std::filesystem::path origin;
        core::CopyReceipt receipt;
        std::size_t index = 0;  // in copies_
    };

    // A verified copy this import made; it is removed at the end unless it became a library file
    // (discard_unregistered_copies).
    struct MadeCopy {
        std::filesystem::path path;  // canonical
        core::FileIdentity identity;  // as published
        std::filesystem::path origin;
        std::optional<std::uint32_t> subimage;  // an image of it that was queued
        bool registered = false;
    };

    struct PendingItem {
        SourceItem item;
        std::optional<std::size_t> copy;  // index in copies_ when the item's file is such a copy
    };

    Status run_body() {
        resolve_exclusions();
        if (auto scanned = scan(); !scanned) {
            return scanned;
        }
        if (!report_.cancelled) {
            if (auto probed = probe_all(); !probed) {
                return probed;
            }
        }
        return flush();
    }

    // ---- helpers ----------------------------------------------------------------------------

    void notify() {
        if (!progress_) {
            return;
        }
        try {
            progress_(state_);
        } catch (const std::exception& e) {
            core::log_error(kWhere, "an import progress callback threw: {}", e.what());
        } catch (...) {
            core::log_error(kWhere, "an import progress callback threw an unknown exception");
        }
    }

    bool cancelled() {
        if (cancel_.is_cancelled()) {
            report_.cancelled = true;
        }
        return report_.cancelled;
    }

    void skip(std::filesystem::path path, SkipReason reason, std::optional<Error> error = std::nullopt,
              std::optional<ItemId> existing = std::nullopt) {
        report_.skipped.push_back(SkippedPath{std::move(path), reason, existing, std::move(error)});
    }

    void finish_record(ImportJournalOutcome outcome) {
        if (report_.record == 0) {
            return;
        }
        ImportJournalFinish finish;
        finish.outcome = outcome;
        finish.files_seen = state_.files_seen;
        finish.added = report_.added.size();
        finish.refreshed = report_.refreshed.size();
        finish.unchanged = restored_files_;  // found again with the library's identity
        finish.skipped = report_.ignored_non_raw;
        for (const SkippedPath& skipped : report_.skipped) {
            switch (skipped.reason) {
                case SkipReason::Duplicate:
                    ++(skipped.existing ? finish.unchanged : finish.duplicates);
                    break;
                case SkipReason::Inaccessible:
                case SkipReason::NotFound:
                case SkipReason::NotAFileOrFolder:
                case SkipReason::DepthLimit:
                case SkipReason::CopyFailed:
                case SkipReason::InvalidName:
                    ++finish.failed;
                    break;
                case SkipReason::InOutputDirectory:
                case SkipReason::InLucentDirectory:
                case SkipReason::LucentTemporaryFile:
                case SkipReason::DirectoryCycle:
                case SkipReason::NotRawPhoto:
                    ++finish.skipped;
                    break;
            }
        }
        if (Status finished = library_.finish_import(report_.record, finish); !finished) {
            core::log_warn(kWhere, "the import record could not be completed: {}",
                           finished.error().describe());
        }
    }

    // The kind of volume `path` is on, cached per folder (the query opens the volume).
    core::StorageKind storage_of(const std::filesystem::path& path) {
        const std::wstring folder = detail::fold_case(path.parent_path().native());
        if (const auto found = storage_.find(folder); found != storage_.end()) {
            return found->second;
        }
        const core::StorageKind kind = core::storage_kind(path);
        storage_.emplace(folder, kind);
        return kind;
    }

    void resolve_exclusions() {
        const auto resolve = [&](const std::vector<std::filesystem::path>& paths,
                                 std::vector<std::filesystem::path>& out) {
            for (const std::filesystem::path& path : paths) {
                if (path.empty()) {
                    continue;
                }
                auto canonical = detail::canonical_path(path, kWhere);
                // A folder that does not exist yet can still be matched by its spelling.
                std::error_code ec;
                std::filesystem::path resolved = canonical ? *canonical : std::filesystem::absolute(path, ec);
                if (!resolved.empty()) {
                    out.push_back(std::move(resolved));
                }
            }
        };
        resolve(request_.output_directories, output_directories_);
        resolve(request_.lucent_directories, lucent_directories_);
    }

    // Why `canonical` must not be imported, if it must not. Lucent's directories always apply;
    // output directories only while scanning an import folder (active_outputs_; see the header).
    std::optional<SkipReason> excluded(const std::filesystem::path& canonical, bool scanning) const {
        for (const std::filesystem::path& directory : lucent_directories_) {
            if (detail::is_same_or_within(canonical, directory)) {
                return SkipReason::InLucentDirectory;
            }
        }
        if (scanning) {
            for (const std::filesystem::path& directory : active_outputs_) {
                if (detail::is_same_or_within(canonical, directory)) {
                    return SkipReason::InOutputDirectory;
                }
            }
        }
        return std::nullopt;
    }

    void add_candidate(const std::filesystem::path& canonical, bool not_raw_hint,
                       const std::filesystem::path& root) {
        if (!seen_.insert(detail::fold_case(canonical.native())).second) {
            skip(canonical, SkipReason::Duplicate);
            return;
        }
        candidates_.push_back(Candidate{canonical, not_raw_hint, root});
        state_.candidates = candidates_.size();
    }

    // ---- scanning ---------------------------------------------------------------------------

    Status scan() {
        for (const std::filesystem::path& file : request_.files) {
            if (cancelled()) {
                return ok();
            }
            ++state_.files_seen;
            if (!has_valid_name(file)) {
                skip(file, SkipReason::InvalidName, invalid_name_error());
                continue;
            }
            auto canonical = detail::canonical_path(file, kWhere);
            if (!canonical) {
                skip(file, reason_for(canonical.error()), std::move(canonical.error()));
                continue;
            }
            if (detail::is_directory(*canonical)) {
                skip(*canonical, SkipReason::NotAFileOrFolder,
                     Error{ErrorCode::InvalidArgument, "a folder was selected as a photo",
                           std::string(kWhere), 0});
                continue;
            }
            consider_file(*canonical, canonical->filename().native(), /*explicit_file=*/true, {});
        }
        for (const ImportFolder& folder : request_.folders) {
            if (cancelled()) {
                return ok();
            }
            if (!has_valid_name(folder.path)) {
                skip(folder.path, SkipReason::InvalidName, invalid_name_error());
                continue;
            }
            auto canonical = detail::canonical_path(folder.path, kWhere);
            if (!canonical) {
                skip(folder.path, reason_for(canonical.error()), std::move(canonical.error()));
                continue;
            }
            if (!detail::is_directory(*canonical)) {
                skip(*canonical, SkipReason::NotAFileOrFolder,
                     Error{ErrorCode::InvalidArgument, "a file was selected as a folder", std::string(kWhere),
                           0});
                continue;
            }
            if (auto reason = excluded(*canonical, /*scanning=*/false)) {
                skip(*canonical, *reason);
                continue;
            }
            auto identity = core::stat_file(*canonical);
            if (!identity) {
                skip(*canonical, reason_for(identity.error()), std::move(identity.error()));
                continue;
            }
            if (!visited_.insert(folder_key(*identity, *canonical)).second) {
                skip(*canonical, SkipReason::DirectoryCycle);
                continue;
            }
            scan_tree(*canonical, folder.include_subfolders);
        }
        return ok();
    }

    // Depth-first walk of one import folder, in sorted order.
    void scan_tree(const std::filesystem::path& root, bool recursive) {
        // Output directories that contain the import folder do not apply to it: the user asked
        // for this tree (for example after exporting next to the originals).
        active_outputs_.clear();
        for (const std::filesystem::path& directory : output_directories_) {
            if (!detail::is_same_or_within(root, directory)) {
                active_outputs_.push_back(directory);
            }
        }
        std::vector<std::pair<std::filesystem::path, std::uint32_t>> stack{{root, 0}};
        while (!stack.empty()) {
            if (cancelled()) {
                return;
            }
            auto [directory, depth] = std::move(stack.back());
            stack.pop_back();
            state_.phase = ImportPhase::Scanning;
            state_.current = directory;
            ++state_.directories_scanned;
            notify();

            auto entries = detail::list_directory(directory, kWhere);
            if (!entries) {
                skip(directory, reason_for(entries.error()), std::move(entries.error()));
                continue;
            }
            std::vector<std::pair<std::filesystem::path, std::uint32_t>> subfolders;
            for (const detail::DirectoryEntry& entry : *entries) {
                if (cancelled()) {
                    return;
                }
                const std::filesystem::path child = directory / entry.name;
                if (entry.device) {
                    skip(child, SkipReason::NotAFileOrFolder);
                    continue;
                }
                if (!entry.directory) {
                    ++state_.files_seen;
                    consider_file(child, entry.name, /*explicit_file=*/false, root);
                    continue;
                }
                if (!recursive) {
                    continue;
                }
                if (depth + 1 > request_.max_depth) {
                    skip(child, SkipReason::DepthLimit);
                    continue;
                }
                if (!has_valid_name(child)) {
                    skip(child, SkipReason::InvalidName, invalid_name_error());
                    continue;
                }
                auto canonical = detail::canonical_path(child, kWhere);  // follows junctions and links
                if (!canonical) {
                    skip(child, reason_for(canonical.error()), std::move(canonical.error()));
                    continue;
                }
                if (auto reason = excluded(*canonical, /*scanning=*/true)) {
                    skip(*canonical, *reason);
                    continue;
                }
                auto identity = core::stat_file(*canonical);
                if (!identity) {
                    skip(child, reason_for(identity.error()), std::move(identity.error()));
                    continue;
                }
                if (!visited_.insert(folder_key(*identity, *canonical)).second) {
                    skip(child, SkipReason::DirectoryCycle);
                    continue;
                }
                subfolders.emplace_back(*canonical, depth + 1);
            }
            // Push in reverse so the walk visits subfolders in sorted order.
            for (auto it = subfolders.rbegin(); it != subfolders.rend(); ++it) {
                stack.push_back(std::move(*it));
            }
        }
    }

    // `path` is canonical for explicit files and directory-relative (canonical folder + name)
    // for folder entries.
    void consider_file(const std::filesystem::path& path, std::wstring_view name, bool explicit_file,
                       const std::filesystem::path& root) {
        if (is_lucent_temporary(name)) {
            skip(path, SkipReason::LucentTemporaryFile);
            return;
        }
        if (!has_valid_name(path)) {
            skip(path, SkipReason::InvalidName, invalid_name_error());
            return;
        }
        const ExtensionHint hint = classify_extension(path);
        if (!explicit_file && hint == ExtensionHint::NotRaw) {
            ++report_.ignored_non_raw;
            return;
        }
        // Folder files with an unknown extension, and explicitly selected files with a non-RAW
        // one, are probed only when they carry a RAW container signature.
        const bool sniff_first =
            explicit_file ? hint == ExtensionHint::NotRaw : hint == ExtensionHint::Unknown;
        if (sniff_first) {
            auto signature = sniff(path);
            if (!signature) {
                skip(path, reason_for(signature.error()), std::move(signature.error()));
                return;
            }
            if (*signature == raw::ContainerFormat::Unknown) {
                if (explicit_file) {
                    skip(path, SkipReason::NotRawPhoto);
                } else {
                    ++report_.ignored_non_raw;
                }
                return;
            }
        }
        std::filesystem::path canonical = path;
        if (!explicit_file) {
            auto resolved = detail::canonical_path(path, kWhere);  // a file symbolic link
            if (!resolved) {
                skip(path, reason_for(resolved.error()), std::move(resolved.error()));
                return;
            }
            if (!has_valid_name(*resolved)) {
                skip(path, SkipReason::InvalidName, invalid_name_error());
                return;
            }
            canonical = std::move(*resolved);
        }
        if (auto reason = excluded(canonical, /*scanning=*/!explicit_file)) {
            skip(canonical, *reason);
            return;
        }
        add_candidate(canonical, explicit_file && hint == ExtensionHint::NotRaw, root);
    }

    static Expected<raw::ContainerFormat> sniff(const std::filesystem::path& path) {
        auto file = core::ReadOnlyFile::open(path, core::AccessHint::Sequential);
        if (!file) {
            return Unexpected(std::move(file.error()));
        }
        std::array<std::byte, 16> header{};
        auto read = file->read_at(0, header);
        if (!read) {
            return Unexpected(std::move(read.error()));
        }
        return raw::detect_container_format(std::span<const std::byte>(header.data(), *read));
    }

    // ---- probing ----------------------------------------------------------------------------

    Status probe_all() {
        for (const Candidate& candidate : candidates_) {
            if (cancelled()) {
                return ok();
            }
            state_.phase = ImportPhase::Probing;
            state_.current = candidate.path;
            const bool copy = !copy_root_.empty() && !detail::is_same_or_within(candidate.path, copy_root_);
            if (Status handled = copy ? copy_and_probe(candidate) : import_in_place(candidate); !handled) {
                return handled;
            }
            ++state_.probed;
            notify();
            if (pending_.size() >= request_.batch_size) {
                if (auto flushed = flush(); !flushed) {
                    return flushed;
                }
            }
        }
        return ok();
    }

    // ---- files the library already knows -------------------------------------------------------

    // How the file at a candidate's path relates to the library (see the header's rules).
    enum class Known : std::uint8_t {
        New,        // not a library file
        Same,       // a library file at this path, with the library's identity
        Changed,    // a library file at this path, with another identity: probed again, refreshed
        Moved,      // the library's version of a file whose own path no longer holds it
        OtherPath,  // another path to a library file that is still at its own path (a hard link)
    };

    struct KnownFile {
        Known kind = Known::New;
        std::optional<SourceItem> item;  // the library's item (image 0) of that file
    };

    KnownFile known_file(const std::filesystem::path& path, const core::FileIdentity& identity) const {
        const std::optional<ItemId> id = library_.find_source(path, identity, 0);
        if (!id) {
            return {};
        }
        std::optional<SourceItem> item = library_.find(*id);
        if (!item) {
            return {};  // removed meanwhile
        }
        if (detail::same_path_text(item->path, path)) {
            return KnownFile{item->identity == identity ? Known::Same : Known::Changed, std::move(item)};
        }
        // Found by its file version at another path: a hard link, or the library's file moved here
        // (renamed or moved within its volume, or a card back under another drive letter). A stat
        // that fails for another reason than absence cannot tell: the file stays a duplicate.
        auto at_own_path = core::stat_file(item->path);
        const bool still_there =
            at_own_path ? *at_own_path == item->identity : !detail::indicates_absence(at_own_path.error());
        return KnownFile{still_there ? Known::OtherPath : Known::Moved, std::move(item)};
    }

    // Every item of the file of `item` (its images), at the library's current path of the file.
    std::vector<ItemId> items_of_file(const SourceItem& item) const {
        const std::optional<SourceItem> now = library_.find(item.id);
        const SourceItem& current = now ? *now : item;
        std::vector<ItemId> ids;
        for (std::uint32_t subimage = 0; subimage < current.subimage_count; ++subimage) {
            if (auto id = library_.find_source(current.path, {}, subimage)) {
                ids.push_back(*id);
            }
        }
        if (ids.empty()) {
            ids.push_back(item.id);
        }
        return ids;
    }

    void record_restored(const SourceItem& item) {
        const std::vector<ItemId> ids = items_of_file(item);
        report_.restored.insert(report_.restored.end(), ids.begin(), ids.end());
        ++restored_files_;
    }

    // A library file found at its own path with its identity (Known::Same): a duplicate, or, when
    // the library had marked it Missing or Changed, Present again (a card that came back).
    // Errors: OutOfMemory.
    Status restore(const SourceItem& item, const std::filesystem::path& path) {
        if (item.source_state == SourceState::Present) {
            skip(path, SkipReason::Duplicate, std::nullopt, item.id);
            return ok();
        }
        if (Status restored = library_.set_source_state(item.id, SourceState::Present); !restored) {
            if (restored.error().code == ErrorCode::OutOfMemory) {
                return restored;
            }
            skip(path, SkipReason::Duplicate, std::move(restored.error()), item.id);
            return ok();
        }
        record_restored(item);
        return ok();
    }

    // The library's own file version found at `path` (Known::Moved): the item now points there
    // (Library::relocate_source, verified by identity; ids, flags and hash kept). A library that
    // cannot relocate reports the file as a duplicate. Errors: OutOfMemory.
    Status follow(const SourceItem& item, const std::filesystem::path& path,
                  const core::FileIdentity& identity) {
        SourceRelocation relocation;
        relocation.path = path;
        relocation.identity = identity;
        relocation.storage = storage_of(path);
        if (Status moved = library_.relocate_source(item.id, relocation); !moved) {
            if (moved.error().code == ErrorCode::OutOfMemory) {
                return moved;
            }
            std::optional<Error> why;
            if (moved.error().code != ErrorCode::Unsupported) {
                why = std::move(moved.error());
            }
            skip(path, SkipReason::Duplicate, std::move(why), item.id);
            return ok();
        }
        record_restored(item);
        return ok();
    }

    // In place: a file the library knows is probed again only when it changed, or when a
    // maintenance re-read asks for it (reprobe_known; see the header).
    Status import_in_place(const Candidate& candidate) {
        if (auto identity = core::stat_file(candidate.path)) {
            const KnownFile known = known_file(candidate.path, *identity);
            switch (known.kind) {
                case Known::Same:
                    if (request_.reprobe_known && !request_.copy_to) {
                        break;  // probed again and refreshed in place
                    }
                    return restore(*known.item, candidate.path);
                case Known::OtherPath:
                    skip(candidate.path, SkipReason::Duplicate, std::nullopt, known.item->id);
                    return ok();
                case Known::Moved:
                    return follow(*known.item, candidate.path, *identity);
                case Known::New:
                case Known::Changed:
                    break;
            }
        }
        return probe(candidate, nullptr);
    }

    SourceItem failed_item(const std::filesystem::path& path, std::uint32_t subimage, std::uint32_t count,
                           const core::FileIdentity& identity, const Error& error) const {
        SourceItem item;
        item.path = path;
        item.subimage = subimage;
        item.subimage_count = count;
        item.identity = identity;
        item.support = assess_probe_failure(error);
        return item;
    }

    SourceItem probed_item(const std::filesystem::path& path, const raw::RawMetadata& metadata,
                           std::uint32_t subimage, std::uint32_t count) const {
        SourceItem item;
        item.path = path;
        item.subimage = subimage;
        item.subimage_count = count;
        item.identity = metadata.source.identity;
        item.probe = summarize_probe(metadata);
        item.support = assess_interpretation(metadata.interpretation);
        return item;
    }

    // Probes one file and queues its items. Only cancellation and allocation failures are errors;
    // a file the decoder rejects becomes an Unsupported item (or, for an explicitly selected file
    // with a non-RAW extension, a NotRawPhoto skip). `copy` is set when the file is a verified
    // copy made by this import (skips then name the original).
    Status probe(const Candidate& candidate, const CopyContext* copy) {
        const std::filesystem::path& path = candidate.path;
        core::FileIdentity identity;
        if (auto stat = core::stat_file(path); stat) {
            identity = *stat;
        }
        raw::ProbeOptions options;
        options.limits = request_.probe_limits;
        options.describe_subimages = true;
        auto selection = registry_.select(path, options, cancel_);
        if (!selection) {
            if (selection.error().code == ErrorCode::Cancelled) {
                report_.cancelled = true;
                return ok();
            }
            if (candidate.not_raw_hint) {
                skip(copy != nullptr ? copy->origin : path, SkipReason::NotRawPhoto,
                     std::move(selection.error()));
                return ok();
            }
            queue(failed_item(path, 0, 1, identity, selection.error()), copy);
            return ok();
        }
        const raw::ProbeResult& result = selection->probe;
        const raw::RawMetadata& metadata = result.metadata;
        const std::uint32_t count = std::max<std::uint32_t>(1, metadata.subimage_count);
        if (count == 1 || result.subimages.empty()) {
            queue(probed_item(path, metadata, metadata.subimage_index, count), copy);
            return ok();
        }
        report_.unlisted_subimages += count - static_cast<std::uint32_t>(result.subimages.size());
        for (const raw::SubimageInfo& info : result.subimages) {
            if (info.error) {
                queue(failed_item(path, info.index, count, metadata.source.identity, *info.error), copy);
                continue;
            }
            if (!is_photograph_role(info.role)) {
                continue;
            }
            if (info.index == metadata.subimage_index) {
                queue(probed_item(path, metadata, info.index, count), copy);
                continue;
            }
            raw::ProbeOptions one;
            one.subimage = info.index;
            one.limits = request_.probe_limits;
            one.describe_subimages = false;
            auto again = selection->decoder->probe(path, one, cancel_);
            if (!again) {
                if (again.error().code == ErrorCode::Cancelled) {
                    report_.cancelled = true;
                    return ok();
                }
                queue(failed_item(path, info.index, count, metadata.source.identity, again.error()), copy);
                continue;
            }
            queue(probed_item(path, again->metadata, info.index, count), copy);
        }
        return ok();
    }

    // ---- copying into the library folder ------------------------------------------------------

    // The folder a candidate is copied into: the destination, then the import folder's name and
    // the subfolders below it (selected files: the destination itself).
    std::filesystem::path copy_folder_for(const Candidate& candidate) const {
        if (candidate.root.empty()) {
            return copy_root_;
        }
        std::filesystem::path base = copy_root_;
        if (const std::filesystem::path name = candidate.root.filename(); !name.empty()) {
            base /= name;
        }
        const std::filesystem::path relative =
            candidate.path.parent_path().lexically_relative(candidate.root);
        if (relative.empty() || relative == L"." || relative.is_absolute() ||
            (!relative.empty() && *relative.begin() == L"..")) {
            return base;  // the folder itself, or a file reached through a link outside it
        }
        return base / relative;
    }

    // What an earlier copy of the original at a candidate's path is (Library::find_copied_from).
    enum class EarlierCopy : std::uint8_t {
        None,     // no copy of this photo: copy it
        Present,  // already copied: a duplicate
        Missing,  // copied before, but the copy is gone: copy again, the item moves to the new copy
    };

    // Whether the copy `copied` holds the photo the original at `path` holds now: proven by the
    // original's identity recorded with the copy, else by comparing the original's content hash
    // with the copy's (a path and a size alone do not identify a photo: cameras restart their
    // numbering, and many RAW formats have a fixed size). std::nullopt when cancelled.
    std::optional<std::pair<EarlierCopy, std::optional<SourceItem>>> earlier_copy(
        ItemId copied, const std::filesystem::path& path, const core::FileIdentity& identity) {
        std::optional<SourceItem> item = library_.find(copied);
        if (!item) {
            return std::pair{EarlierCopy::None, std::optional<SourceItem>()};
        }
        bool same = item->origin_identity && *item->origin_identity == identity;
        if (!same && item->sha256) {
            auto hashed = core::hash_file(path, cancel_, request_.copy_options.chunk_bytes);
            if (!hashed) {
                if (hashed.error().code == ErrorCode::Cancelled) {
                    return std::nullopt;
                }
                // Unreadable now: the copy below reports why.
                return std::pair{EarlierCopy::None, std::optional<SourceItem>()};
            }
            same = hashed->identity == identity && hashed->digest == *item->sha256;
        }
        if (!same) {
            return std::pair{EarlierCopy::None, std::optional<SourceItem>()};
        }
        const EarlierCopy kind =
            item->source_state == SourceState::Missing ? EarlierCopy::Missing : EarlierCopy::Present;
        return std::pair{kind, std::move(item)};
    }

    // Copies the candidate (see the header's copy rules) and probes the verified copy, or moves an
    // existing library item to it.
    Status copy_and_probe(const Candidate& candidate) {
        const std::filesystem::path& path = candidate.path;
        auto identity = core::stat_file(path);
        if (!identity) {
            skip(path, reason_for(identity.error()), std::move(identity.error()));
            return ok();
        }
        // The library item that moves to the copy, if any.
        std::optional<SourceItem> owner;
        const KnownFile known = known_file(path, *identity);
        // Only referenced originals move to a copy, and only in a library that can relocate.
        const bool movable = known.item && known.item->import_mode == ImportMode::Referenced &&
                             capabilities_.source_maintenance;
        switch (known.kind) {
            case Known::OtherPath:
                skip(path, SkipReason::Duplicate, std::nullopt, known.item->id);
                return ok();
            case Known::Same:
                if (!movable) {
                    return restore(*known.item, path);  // a library copy itself, or no relocation
                }
                owner = known.item;
                break;
            case Known::Moved:
                if (!movable) {
                    return follow(*known.item, path, *identity);
                }
                owner = known.item;  // the library's own version: the copy proves it (move_to_copy)
                break;
            case Known::Changed: {
                if (!movable) {
                    return probe(candidate, nullptr);  // refreshed in place
                }
                // A referenced original replaced by another version (a reused card): its items are
                // refreshed in place first, then moved to the verified copy of the new version, so
                // the item is not left referencing the card after a copy was asked for.
                const std::size_t queued = pending_.size();
                if (Status probed = probe(candidate, nullptr); !probed) {
                    return probed;
                }
                if (report_.cancelled || pending_.size() == queued) {
                    return ok();  // cancelled, or not a RAW photo (reported)
                }
                if (Status flushed = flush(); !flushed) {
                    return flushed;
                }
                owner = library_.find(known.item->id);
                break;
            }
            case Known::New:
                if (const std::optional<ItemId> copied = library_.find_copied_from(path, *identity)) {
                    auto earlier = earlier_copy(*copied, path, *identity);
                    if (!earlier) {
                        report_.cancelled = true;
                        return ok();
                    }
                    if (earlier->first == EarlierCopy::Present) {
                        skip(path, SkipReason::Duplicate, std::nullopt, copied);
                        return ok();
                    }
                    if (earlier->first == EarlierCopy::Missing) {
                        owner = std::move(earlier->second);
                    }
                }
                break;
        }

        if (candidate.not_raw_hint) {
            // A selected file with a non-RAW extension is copied only when it is a RAW photo.
            raw::ProbeOptions options;
            options.limits = request_.probe_limits;
            auto selection = registry_.select(path, options, cancel_);
            if (!selection) {
                if (selection.error().code == ErrorCode::Cancelled) {
                    report_.cancelled = true;
                    return ok();
                }
                skip(path, SkipReason::NotRawPhoto, std::move(selection.error()));
                return ok();
            }
        }

        state_.phase = ImportPhase::Copying;
        notify();
        const std::filesystem::path folder = copy_folder_for(candidate);
        if (Status created = core::create_directory_tree(folder); !created) {
            skip(path, SkipReason::CopyFailed, std::move(created.error()));
            return ok();
        }
        const std::uint64_t bytes_before = state_.bytes_copied;
        auto receipt = core::copy_file_verified(
            path, folder, {}, request_.copy_options, cancel_, [&](const core::CopyProgress& progress) {
                if (progress.phase == core::CopyPhase::Copying) {
                    state_.bytes_copied = bytes_before + progress.bytes_done;
                    notify();
                }
            });
        if (!receipt) {
            state_.bytes_copied = bytes_before;
            if (receipt.error().code == ErrorCode::Cancelled) {
                report_.cancelled = true;
                return ok();
            }
            skip(path, SkipReason::CopyFailed, std::move(receipt.error()));
            return ok();
        }
        state_.bytes_copied = bytes_before + receipt->size_bytes;
        report_.bytes_copied += receipt->size_bytes;
        ++state_.copied;
        auto canonical = detail::canonical_path(receipt->final_path, kWhereCopy);
        if (canonical) {
            receipt->final_path = std::move(*canonical);
        }
        // Until it is registered, the copy is this import's to remove (discard_unregistered_copies).
        copies_.push_back(MadeCopy{receipt->final_path, receipt->copy_identity, path, std::nullopt, false});
        const std::size_t copy_index = copies_.size() - 1;
        state_.phase = ImportPhase::Probing;
        notify();

        if (owner && move_to_copy(*owner, path, *receipt)) {
            copies_[copy_index].registered = true;
            return ok();
        }
        CopyContext context{path, std::move(*receipt), copy_index};
        Candidate copy_candidate{context.receipt.final_path, candidate.not_raw_hint, {}};
        return probe(copy_candidate, &context);
    }

    // Moves the library item `owner` (with the other images of its file) to the verified copy of
    // `origin`. The copy must hold the item's photo: proven by the item's content hash, or, for an
    // item never hashed, by its identity being the original's identity during the copy (the copy's
    // hash is then recorded for it first). False when that cannot be proven or the library
    // refuses: the copy is then registered as an item of its own.
    bool move_to_copy(const SourceItem& owner, const std::filesystem::path& origin,
                      const core::CopyReceipt& receipt) {
        if (owner.sha256 ? *owner.sha256 != receipt.sha256 : receipt.source_identity != owner.identity) {
            return false;
        }
        if (!owner.sha256) {
            if (Status hashed = library_.set_content_hash(owner.id, receipt.sha256, receipt.source_identity);
                !hashed) {
                core::log_warn(kWhereCopy, "could not record the hash of '{}': {}",
                               core::path_to_utf8_lossy(origin), hashed.error().describe());
                return false;
            }
        }
        SourceRelocation relocation;
        relocation.path = receipt.final_path;
        relocation.identity = receipt.copy_identity;
        relocation.sha256 = receipt.sha256;
        relocation.storage = storage_of(receipt.final_path);
        relocation.import_mode = ImportMode::Copied;
        relocation.origin_path = origin;
        relocation.origin_identity = receipt.source_identity;
        if (Status moved = library_.relocate_source(owner.id, relocation); !moved) {
            core::log_warn(kWhereCopy, "could not move '{}' to its copy: {}",
                           core::path_to_utf8_lossy(origin), moved.error().describe());
            return false;
        }
        const std::vector<ItemId> ids = items_of_file(owner);
        report_.copied.insert(report_.copied.end(), ids.begin(), ids.end());
        return true;
    }

    // Deletes the copies this import made that did not become library files (a probe that found
    // no RAW photo, a cancelled probe, a batch the library refused), so nothing unregistered is
    // left in the library folder. A copy the library lists after all (a commit whose in-memory
    // update failed) is kept. Failures are logged; such a copy stays where it is.
    void discard_unregistered_copies() noexcept {
        try {
            for (MadeCopy& copy : copies_) {
                if (copy.registered) {
                    continue;
                }
                copy.registered = true;  // dealt with
                if (copy.subimage && library_.find_source(copy.path, {}, *copy.subimage)) {
                    continue;
                }
                if (Status removed = core::remove_verified_copy(copy.path, copy.identity); !removed) {
                    core::log_warn(kWhereCopy, "an unused copy could not be removed: {}",
                                   removed.error().describe());
                }
            }
        } catch (const std::exception& e) {
            core::log_error(kWhereCopy, "removing unused copies failed: {}", e.what());
        }
    }

    void queue(SourceItem item, const CopyContext* copy) {
        item.storage = storage_of(item.path);
        std::optional<std::size_t> copy_index;
        if (copy != nullptr) {
            item.import_mode = ImportMode::Copied;
            item.origin_path = copy->origin;
            item.origin_identity = copy->receipt.source_identity;
            // The verified hash belongs to the copy as published; a copy modified since is hashed
            // again later.
            if (item.identity == copy->receipt.copy_identity) {
                item.sha256 = copy->receipt.sha256;
            }
            copy_index = copy->index;
            MadeCopy& made = copies_[copy->index];
            if (!made.subimage) {
                made.subimage = item.subimage;
            }
        }
        pending_.push_back(PendingItem{std::move(item), copy_index});
    }

    // True when `item` is a new description of an existing item (same canonical path and image)
    // whose file changed since it was imported, or whose file a maintenance re-read probed again
    // (reprobe_known), which is then refreshed (or reported as a duplicate when the library
    // refuses the refresh). Errors: OutOfMemory.
    Expected<bool> refresh_if_changed(SourceItem& item) {
        const std::optional<ItemId> existing =
            library_.find_source(item.path, core::FileIdentity{}, item.subimage);
        if (!existing) {
            return false;
        }
        const std::optional<SourceItem> current = library_.find(*existing);
        const bool reprobe = request_.reprobe_known && !request_.copy_to;
        if (!current || (current->identity == item.identity && !reprobe)) {
            return false;  // add() reports the duplicate
        }
        item.id = *existing;
        auto refreshed = library_.refresh(item);
        if (refreshed) {
            report_.refreshed.push_back(*existing);
        } else if (refreshed.error().code == ErrorCode::OutOfMemory) {
            return Unexpected(std::move(refreshed.error()));
        } else {
            skip(item.path, SkipReason::Duplicate, std::move(refreshed.error()), *existing);
        }
        return true;
    }

    // Commits the queued items: items of changed files are refreshed in place, the rest added.
    Status flush() {
        if (pending_.empty()) {
            return ok();
        }
        std::vector<SourceItem> additions;
        additions.reserve(pending_.size());
        struct Added {
            SupportStatus status;
            std::filesystem::path path;
            core::StorageKind storage;
            std::optional<std::size_t> copy;  // index in copies_ of a copy made by this import
        };
        std::vector<Added> facts;
        facts.reserve(pending_.size());
        for (PendingItem& pending : pending_) {
            if (auto refreshed = refresh_if_changed(pending.item); !refreshed) {
                pending_.clear();
                return Unexpected(std::move(refreshed.error()));
            } else if (!*refreshed) {
                facts.push_back(Added{pending.item.support.status, pending.item.path, pending.item.storage,
                                      pending.copy});
                additions.push_back(std::move(pending.item));
            }
        }
        pending_.clear();
        if (additions.empty()) {
            return ok();
        }
        auto outcomes = library_.add(std::move(additions));
        if (!outcomes) {
            return Unexpected(std::move(outcomes.error()));
        }
        // First of all (nothing here allocates): the copies that are library files now.
        for (std::size_t i = 0; i < outcomes->size(); ++i) {
            if ((*outcomes)[i].added && facts[i].copy) {
                copies_[*facts[i].copy].registered = true;
            }
        }
        for (std::size_t i = 0; i < outcomes->size(); ++i) {
            const AddOutcome& outcome = (*outcomes)[i];
            if (!outcome.added) {
                const std::filesystem::path& reported =
                    facts[i].copy ? copies_origin(*facts[i].copy) : facts[i].path;
                skip(reported, SkipReason::Duplicate, std::nullopt, outcome.id);
                continue;
            }
            report_.added.push_back(outcome.id);
            ++state_.added;
            if (facts[i].copy) {
                report_.copied.push_back(outcome.id);
            }
            if (facts[i].storage == core::StorageKind::Removable) {
                ++report_.removable_items;
                const std::filesystem::path root = facts[i].path.root_path();
                const bool known = std::any_of(
                    report_.removable_locations.begin(), report_.removable_locations.end(),
                    [&](const std::filesystem::path& p) { return detail::same_path_text(p, root); });
                if (!known) {
                    report_.removable_locations.push_back(root);
                }
            }
            switch (facts[i].status) {
                case SupportStatus::Supported:
                    ++report_.supported;
                    break;
                case SupportStatus::Limited:
                    ++report_.limited;
                    break;
                case SupportStatus::Unsupported:
                    ++report_.unsupported;
                    break;
            }
        }
        return ok();
    }

    const std::filesystem::path& copies_origin(std::size_t index) const { return copies_[index].origin; }

    Library& library_;
    const raw::DecoderRegistry& registry_;
    const ImportRequest& request_;
    const ImportProgressCallback& progress_;
    core::CancellationToken cancel_;
    LibraryCapabilities capabilities_;

    std::filesystem::path copy_root_;                        // canonical copy_to; empty: in place
    std::vector<std::filesystem::path> output_directories_;  // canonical
    std::vector<std::filesystem::path> lucent_directories_;  // canonical
    std::vector<std::filesystem::path> active_outputs_;      // those that apply to the folder being scanned
    std::unordered_set<std::wstring> seen_;                  // folded canonical paths of candidates
    std::unordered_set<std::string> visited_;                // folder keys
    std::unordered_map<std::wstring, core::StorageKind> storage_;  // folded folder -> kind
    std::vector<Candidate> candidates_;
    std::vector<PendingItem> pending_;
    std::vector<MadeCopy> copies_;  // verified copies made by this import
    std::uint64_t restored_files_ = 0;
    ImportProgress state_;
    ImportReport report_;
};

}  // namespace

std::string_view to_string(SkipReason reason) noexcept {
    switch (reason) {
        case SkipReason::Duplicate:
            return "duplicate";
        case SkipReason::InOutputDirectory:
            return "in_output_directory";
        case SkipReason::InLucentDirectory:
            return "in_lucent_directory";
        case SkipReason::LucentTemporaryFile:
            return "lucent_temporary_file";
        case SkipReason::DirectoryCycle:
            return "directory_cycle";
        case SkipReason::DepthLimit:
            return "depth_limit";
        case SkipReason::Inaccessible:
            return "inaccessible";
        case SkipReason::NotFound:
            return "not_found";
        case SkipReason::NotAFileOrFolder:
            return "not_a_file_or_folder";
        case SkipReason::NotRawPhoto:
            return "not_raw_photo";
        case SkipReason::CopyFailed:
            return "copy_failed";
        case SkipReason::InvalidName:
            return "invalid_name";
    }
    return "unknown";
}

ExtensionHint classify_extension(const std::filesystem::path& path) {
    std::wstring extension = path.extension().native();
    if (extension.size() < 2) {
        return ExtensionHint::Unknown;
    }
    extension = lower_ascii(std::wstring_view(extension).substr(1));
    if (std::find(kRawExtensions.begin(), kRawExtensions.end(), extension) != kRawExtensions.end()) {
        return ExtensionHint::Raw;
    }
    if (std::find(kNonRawExtensions.begin(), kNonRawExtensions.end(), extension) != kNonRawExtensions.end()) {
        return ExtensionHint::NotRaw;
    }
    return ExtensionHint::Unknown;
}

std::span<const std::wstring_view> raw_extensions() noexcept {
    return kRawExtensions;
}

Expected<ImportReport> import_sources(Library& library, const raw::DecoderRegistry& registry,
                                      const ImportRequest& request, const ImportProgressCallback& progress,
                                      core::CancellationToken cancel) {
    try {
        Importer importer(library, registry, request, progress, std::move(cancel));
        return importer.run();
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "the import ran out of memory", std::string(kWhere));
    }
}

}  // namespace lucent::app
