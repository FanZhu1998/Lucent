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
#include <unordered_set>
#include <utility>
#include <vector>

#include <lucent/application/import.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/text.hpp>
#include <lucent/raw/decoded_raw.hpp>

#include "win32_paths.hpp"

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.import";

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

bool is_lucent_temporary(std::wstring_view name) {
    const std::wstring lower = lower_ascii(name);
    return lower.starts_with(L"~lucent-") && lower.ends_with(L".tmp");
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

bool is_photograph_role(raw::SubimageRole role) {
    return role != raw::SubimageRole::DualPixelComponent;
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
        resolve_exclusions();
        if (auto scanned = scan(); !scanned) {
            return Unexpected(std::move(scanned.error()));
        }
        if (!report_.cancelled) {
            if (auto probed = probe_all(); !probed) {
                return Unexpected(std::move(probed.error()));
            }
        }
        if (auto flushed = flush(); !flushed) {
            return Unexpected(std::move(flushed.error()));
        }
        report_.directories_scanned = state_.directories_scanned;
        report_.files_seen = state_.files_seen;
        report_.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
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
    };

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

    void add_candidate(const std::filesystem::path& canonical, bool not_raw_hint) {
        if (!seen_.insert(detail::fold_case(canonical.native())).second) {
            skip(canonical, SkipReason::Duplicate);
            return;
        }
        candidates_.push_back(Candidate{canonical, not_raw_hint});
        state_.candidates = candidates_.size();
    }

    // ---- scanning ---------------------------------------------------------------------------

    Status scan() {
        for (const std::filesystem::path& file : request_.files) {
            if (cancelled()) {
                return ok();
            }
            ++state_.files_seen;
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
            consider_file(*canonical, canonical->filename().native(), /*explicit_file=*/true);
        }
        for (const ImportFolder& folder : request_.folders) {
            if (cancelled()) {
                return ok();
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
                    consider_file(child, entry.name, /*explicit_file=*/false);
                    continue;
                }
                if (!recursive) {
                    continue;
                }
                if (depth + 1 > request_.max_depth) {
                    skip(child, SkipReason::DepthLimit);
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
    void consider_file(const std::filesystem::path& path, std::wstring_view name, bool explicit_file) {
        if (is_lucent_temporary(name)) {
            skip(path, SkipReason::LucentTemporaryFile);
            return;
        }
        const ExtensionHint hint = classify_extension(path);
        if (!explicit_file && hint == ExtensionHint::NotRaw) {
            ++report_.ignored_non_raw;
            return;
        }
        // Folder files with an unknown extension, and explicitly selected files with a non-RAW
        // one, are probed only when they carry a RAW container signature.
        const bool sniff_first = explicit_file ? hint == ExtensionHint::NotRaw : hint == ExtensionHint::Unknown;
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
            canonical = std::move(*resolved);
        }
        if (auto reason = excluded(canonical, /*scanning=*/!explicit_file)) {
            skip(canonical, *reason);
            return;
        }
        add_candidate(canonical, explicit_file && hint == ExtensionHint::NotRaw);
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
            if (auto probed = probe(candidate); !probed) {
                return probed;
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
    // with a non-RAW extension, a NotRawPhoto skip).
    Status probe(const Candidate& candidate) {
        const std::filesystem::path& path = candidate.path;
        core::FileIdentity identity;
        if (auto stat = core::stat_file(path); stat) {
            identity = *stat;
            if (auto existing = library_.find_source(path, identity, 0)) {
                // Unchanged (or another path to the same file): a duplicate. The same path with
                // another identity is a changed file: probed again, refreshed by flush().
                const std::optional<SourceItem> item = library_.find(*existing);
                if (!item || item->identity == identity || !detail::same_path_text(item->path, path)) {
                    skip(path, SkipReason::Duplicate, std::nullopt, *existing);
                    return ok();
                }
            }
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
                skip(path, SkipReason::NotRawPhoto, std::move(selection.error()));
                return ok();
            }
            queue(failed_item(path, 0, 1, identity, selection.error()));
            return ok();
        }
        const raw::ProbeResult& result = selection->probe;
        const raw::RawMetadata& metadata = result.metadata;
        const std::uint32_t count = std::max<std::uint32_t>(1, metadata.subimage_count);
        if (count == 1 || result.subimages.empty()) {
            queue(probed_item(path, metadata, metadata.subimage_index, count));
            return ok();
        }
        report_.unlisted_subimages += count - static_cast<std::uint32_t>(result.subimages.size());
        for (const raw::SubimageInfo& info : result.subimages) {
            if (info.error) {
                queue(failed_item(path, info.index, count, metadata.source.identity, *info.error));
                continue;
            }
            if (!is_photograph_role(info.role)) {
                continue;
            }
            if (info.index == metadata.subimage_index) {
                queue(probed_item(path, metadata, info.index, count));
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
                queue(failed_item(path, info.index, count, metadata.source.identity, again.error()));
                continue;
            }
            queue(probed_item(path, again->metadata, info.index, count));
        }
        return ok();
    }

    void queue(SourceItem item) { pending_.push_back(std::move(item)); }

    // True when `item` is a new description of an existing item (same canonical path and image)
    // whose file changed since it was imported, which is then refreshed (or reported as a
    // duplicate when the library refuses the refresh). Errors: OutOfMemory.
    Expected<bool> refresh_if_changed(SourceItem& item) {
        const std::optional<ItemId> existing = library_.find_source(item.path, core::FileIdentity{}, item.subimage);
        if (!existing) {
            return false;
        }
        const std::optional<SourceItem> current = library_.find(*existing);
        if (!current || current->identity == item.identity) {
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
        for (SourceItem& item : pending_) {
            if (auto refreshed = refresh_if_changed(item); !refreshed) {
                return Unexpected(std::move(refreshed.error()));
            } else if (!*refreshed) {
                additions.push_back(std::move(item));
            }
        }
        pending_.clear();
        if (additions.empty()) {
            return ok();
        }
        std::vector<SupportStatus> statuses;
        std::vector<std::filesystem::path> paths;
        statuses.reserve(additions.size());
        paths.reserve(additions.size());
        for (const SourceItem& item : additions) {
            statuses.push_back(item.support.status);
            paths.push_back(item.path);
        }
        auto outcomes = library_.add(std::move(additions));
        if (!outcomes) {
            return Unexpected(std::move(outcomes.error()));
        }
        for (std::size_t i = 0; i < outcomes->size(); ++i) {
            const AddOutcome& outcome = (*outcomes)[i];
            if (!outcome.added) {
                skip(paths[i], SkipReason::Duplicate, std::nullopt, outcome.id);
                continue;
            }
            report_.added.push_back(outcome.id);
            ++state_.added;
            switch (statuses[i]) {
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

    Library& library_;
    const raw::DecoderRegistry& registry_;
    const ImportRequest& request_;
    const ImportProgressCallback& progress_;
    core::CancellationToken cancel_;

    std::vector<std::filesystem::path> output_directories_;  // canonical
    std::vector<std::filesystem::path> lucent_directories_;  // canonical
    std::vector<std::filesystem::path> active_outputs_;      // those that apply to the folder being scanned
    std::unordered_set<std::wstring> seen_;    // folded canonical paths of candidates
    std::unordered_set<std::string> visited_;  // folder keys
    std::vector<Candidate> candidates_;
    std::vector<SourceItem> pending_;
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
