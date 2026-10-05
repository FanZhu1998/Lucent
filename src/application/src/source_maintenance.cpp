#include <algorithm>
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

#include <lucent/application/import.hpp>
#include <lucent/application/source_maintenance.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/storage.hpp>
#include <lucent/core/text.hpp>
#include <lucent/export/thumbnail.hpp>
#include <lucent/raw/embedded_preview.hpp>

#include "win32_paths.hpp"

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.maintenance";
constexpr std::string_view kWhereRelink = "app.maintenance.relink";

// One source file and its items, as the library knew them when the run started.
struct FileGroup {
    std::filesystem::path path;
    core::FileIdentity identity;
    std::optional<core::Digest256> sha256;
    SourceState state = SourceState::Present;
    std::vector<ItemId> items;  // library order
    std::vector<std::uint32_t> subimages;
    std::vector<bool> probed;  // the item has a probe summary (a thumbnail can be cached for it)
    bool outdated = false;     // an item of the file has an outdated probe (probe_outdated())
};

Status require_maintenance(const Library& library, std::string_view where) {
    if (!library.capabilities().source_maintenance) {
        return fail(ErrorCode::Unsupported, "this library does not keep source facts", std::string(where));
    }
    return ok();
}

// The files of `selection` (all files when empty), in library order (a selection: in the order of
// its first item per file). A selection is resolved item by item (with the other images of each
// file), so a small batch does not copy the whole library.
std::vector<FileGroup> group_files(const Library& library, std::span<const ItemId> selection) {
    std::vector<FileGroup> groups;
    std::unordered_map<std::wstring, std::size_t> index;
    std::unordered_set<std::uint64_t> listed;
    const auto add = [&](const SourceItem& item) {
        if (!listed.insert(item.id.value).second) {
            return;
        }
        auto [it, inserted] = index.emplace(detail::fold_case(item.path.native()), groups.size());
        if (inserted) {
            FileGroup group;
            group.path = item.path;
            group.identity = item.identity;
            group.sha256 = item.sha256;
            group.state = item.source_state;
            groups.push_back(std::move(group));
        }
        FileGroup& group = groups[it->second];
        group.items.push_back(item.id);
        group.subimages.push_back(item.subimage);
        group.probed.push_back(item.probe.has_value());
        group.outdated = group.outdated || probe_outdated(item);
    };
    if (selection.empty()) {
        for (const SourceItem& item : library.items()) {
            add(item);
        }
        return groups;
    }
    for (const ItemId id : selection) {
        const std::optional<SourceItem> item = library.find(id);
        if (!item || listed.contains(item->id.value)) {
            continue;
        }
        add(*item);
        // The other images of the file share its facts.
        for (std::uint32_t subimage = 0; subimage < item->subimage_count; ++subimage) {
            if (subimage == item->subimage) {
                continue;
            }
            if (const std::optional<ItemId> sibling = library.find_source(item->path, {}, subimage)) {
                if (const std::optional<SourceItem> other = library.find(*sibling);
                    other && detail::same_path_text(other->path, item->path)) {
                    add(*other);
                }
            }
        }
    }
    return groups;
}

void report_progress(const MaintenanceProgressCallback& progress, const MaintenanceProgress& state) {
    if (!progress) {
        return;
    }
    try {
        progress(state);
    } catch (const std::exception& e) {
        core::log_error(kWhere, "a maintenance progress callback threw: {}", e.what());
    } catch (...) {
        core::log_error(kWhere, "a maintenance progress callback threw an unknown exception");
    }
}

void append(std::vector<ItemId>& to, const std::vector<ItemId>& from) {
    to.insert(to.end(), from.begin(), from.end());
}

// Records a state for the file of `group`; failures are logged (the next check repeats it).
void set_state(Library& library, const FileGroup& group, SourceState state) {
    if (group.items.empty() || group.state == state) {
        return;
    }
    if (Status set = library.set_source_state(group.items.front(), state); !set) {
        core::log_warn(kWhere, "could not record that '{}' is {}: {}", core::path_to_utf8_lossy(group.path),
                       to_string(state), set.error().describe());
    }
}

// What a stat of the file says after a read reported SourceChanged: Missing, Changed, or (the
// same identity again: a transient change) std::nullopt.
std::optional<SourceState> classify_after_change(const FileGroup& group) {
    auto now = core::stat_file(group.path);
    if (!now) {
        return detail::indicates_absence(now.error()) ? std::optional(SourceState::Missing) : std::nullopt;
    }
    return *now == group.identity ? std::nullopt : std::optional(SourceState::Changed);
}

// Stores the camera's embedded JPEG of a freshly hashed file in the disk cache, once per item.
std::uint64_t cache_camera_thumbnails(const FileGroup& group, const core::Digest256& sha256,
                                      const core::FileIdentity& hashed, const HashOptions& options,
                                      const core::CancellationToken& cancel) {
    if (!options.thumbnails ||
        std::none_of(group.probed.begin(), group.probed.end(), [](bool p) { return p; })) {
        return 0;
    }
    raw::EmbeddedPreviewOptions extract = options.thumbnail_options;
    extract.min_long_edge = kCachedCameraPreviewEdge;
    auto embedded = raw::extract_embedded_preview(group.path, extract, cancel);
    if (!embedded || !embedded->preview || embedded->source_identity != hashed) {
        return 0;  // no usable preview (or the file changed): thumbnails come from the file later
    }
    const std::optional<Thumbnail> entry = camera_thumbnail_entry(*embedded->preview, cancel);
    if (!entry) {
        return 0;
    }
    const Thumbnail& thumbnail = *entry;
    std::uint64_t stored = 0;
    for (std::size_t i = 0; i < group.items.size(); ++i) {
        if (!group.probed[i]) {
            continue;
        }
        if (Status inserted =
                options.thumbnails->insert(camera_thumbnail_key(sha256, group.subimages[i]), thumbnail);
            inserted) {
            ++stored;
        }
    }
    return stored;
}

}  // namespace

std::optional<Thumbnail> camera_thumbnail_entry(const raw::EmbeddedPreview& preview,
                                                const core::CancellationToken& cancel) {
    if (preview.format != raw::EmbeddedPreviewFormat::Jpeg || preview.bytes.empty()) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(preview.bytes.data()),
                                              preview.bytes.size());
    exporter::JpegThumbnailOptions options;
    options.max_long_edge = kCachedCameraPreviewEdge;
    options.quality = kCachedCameraPreviewQuality;
    options.max_unreduced_bytes = kCachedCameraPreviewMaxUnreducedBytes;
    auto reduced = exporter::make_jpeg_thumbnail(bytes, options, cancel);
    if (!reduced) {
        if (reduced.error().code != ErrorCode::Cancelled) {
            core::log_debug(kWhere, "a camera preview was not cached: {}", reduced.error().describe());
        }
        return std::nullopt;
    }
    Thumbnail thumbnail;
    thumbnail.width = reduced->width;
    thumbnail.height = reduced->height;
    const auto* first = reinterpret_cast<const std::byte*>(reduced->jpeg.data());
    thumbnail.jpeg.assign(first, first + reduced->jpeg.size());
    return thumbnail;
}

ThumbnailKey camera_thumbnail_key(const core::Digest256& sha256, std::uint32_t subimage) noexcept {
    ThumbnailKey key;
    key.source_sha256 = sha256;
    key.subimage = subimage;
    key.generator = ThumbnailGenerator::EmbeddedJpeg;
    key.generator_version = kCameraThumbnailGeneratorVersion;
    key.size_class = ThumbnailSizeClass::Large;
    return key;
}

// ---------------------------------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------------------------------

Expected<HashReport> hash_sources(Library& library, std::span<const ItemId> items, const HashOptions& options,
                                  const MaintenanceProgressCallback& progress,
                                  core::CancellationToken cancel) {
    if (Status supported = require_maintenance(library, kWhere); !supported) {
        return Unexpected(std::move(supported.error()));
    }
    if (options.chunk_bytes == 0 || options.chunk_bytes > core::kMaxHashChunkBytes) {
        return fail(ErrorCode::InvalidArgument, "invalid hash chunk size", std::string(kWhere));
    }
    try {
        HashReport report;
        std::vector<FileGroup> groups = group_files(library, items);
        std::erase_if(groups, [&](const FileGroup& group) {
            return group.state != SourceState::Present || (group.sha256 && !options.verify_hashed);
        });
        MaintenanceProgress state;
        state.phase = MaintenancePhase::Hashing;
        state.files_total = groups.size();
        for (const FileGroup& group : groups) {
            state.bytes_total += group.identity.size_bytes;
            state.items_total += group.items.size();
        }
        for (const FileGroup& group : groups) {
            if (cancel.is_cancelled()) {
                report.cancelled = true;
                break;
            }
            ++report.files;
            state.current = group.path;
            report_progress(progress, state);

            auto hashed = core::hash_file(group.path, cancel, options.chunk_bytes);
            state.bytes_done += group.identity.size_bytes;
            state.items_done += group.items.size();
            ++state.files_done;
            if (!hashed) {
                const ErrorCode code = hashed.error().code;
                if (code == ErrorCode::Cancelled) {
                    report.cancelled = true;
                    break;
                }
                if (code == ErrorCode::OutOfMemory) {
                    return Unexpected(std::move(hashed.error()));
                }
                std::optional<SourceState> found;
                if (detail::indicates_absence(hashed.error())) {
                    found = SourceState::Missing;
                } else if (code == ErrorCode::SourceChanged) {
                    found = classify_after_change(group);
                }
                if (found == SourceState::Missing) {
                    set_state(library, group, SourceState::Missing);
                    ++report.missing;
                } else if (found == SourceState::Changed) {
                    set_state(library, group, SourceState::Changed);
                    ++report.changed;
                    append(report.changed_items, group.items);
                } else {
                    ++report.failed;
                    core::log_warn(kWhere, "could not hash '{}': {}", core::path_to_utf8_lossy(group.path),
                                   hashed.error().describe());
                }
                continue;
            }
            report.bytes += hashed->identity.size_bytes;
            const bool content_changed = group.sha256 && *group.sha256 != hashed->digest;
            if (hashed->identity != group.identity || content_changed) {
                set_state(library, group, SourceState::Changed);
                ++report.changed;
                append(report.changed_items, group.items);
                continue;
            }
            if (group.sha256) {
                ++report.verified;
                continue;
            }
            Status recorded = library.set_content_hash(group.items.front(), hashed->digest, hashed->identity);
            if (!recorded) {
                if (recorded.error().code == ErrorCode::OutOfMemory) {
                    return Unexpected(std::move(recorded.error()));
                }
                // SourceChanged: the library re-read the file meanwhile (an import); the next run
                // hashes the new version. NotFound: the items were removed.
                if (recorded.error().code != ErrorCode::SourceChanged &&
                    recorded.error().code != ErrorCode::NotFound) {
                    ++report.failed;
                    core::log_warn(kWhere, "could not record the hash of '{}': {}",
                                   core::path_to_utf8_lossy(group.path), recorded.error().describe());
                }
                continue;
            }
            ++report.hashed;
            report.thumbnails_cached +=
                cache_camera_thumbnails(group, hashed->digest, hashed->identity, options, cancel);
        }
        state.current.clear();
        report_progress(progress, state);
        return report;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "hashing ran out of memory", std::string(kWhere));
    }
}

// ---------------------------------------------------------------------------------------------
// Presence checks
// ---------------------------------------------------------------------------------------------

Expected<RefreshReport> refresh_sources(Library& library, const raw::DecoderRegistry& registry,
                                        std::span<const ItemId> items, const RefreshOptions& options,
                                        const MaintenanceProgressCallback& progress,
                                        core::CancellationToken cancel) {
    if (Status supported = require_maintenance(library, kWhere); !supported) {
        return Unexpected(std::move(supported.error()));
    }
    try {
        RefreshReport report;
        const std::vector<FileGroup> groups = group_files(library, items);
        MaintenanceProgress state;
        state.phase = MaintenancePhase::Checking;
        state.files_total = groups.size();
        for (const FileGroup& group : groups) {
            state.items_total += group.items.size();
        }
        std::vector<const FileGroup*> changed;
        std::vector<const FileGroup*> outdated;  // present, with an outdated probe
        for (const FileGroup& group : groups) {
            if (cancel.is_cancelled()) {
                report.cancelled = true;
                break;
            }
            ++report.files;
            state.current = group.path;
            auto now = core::stat_file(group.path);
            ++state.files_done;
            state.items_done += group.items.size();
            report_progress(progress, state);
            if (!now) {
                // Nothing at the path, or its volume is absent (a card reader without the card).
                if (detail::indicates_absence(now.error())) {
                    set_state(library, group, SourceState::Missing);
                    ++report.missing;
                    append(report.missing_items, group.items);
                } else {
                    ++report.failed;
                    core::log_warn(kWhere, "could not check '{}': {}", core::path_to_utf8_lossy(group.path),
                                   now.error().describe());
                }
                continue;
            }
            if (*now == group.identity) {
                ++report.present;
                if (group.state != SourceState::Present) {
                    set_state(library, group, SourceState::Present);
                    ++report.restored;
                }
                if (!group.sha256) {
                    append(report.unhashed_items, group.items);
                }
                if (group.outdated && options.reprobe_outdated) {
                    ++report.outdated;
                    outdated.push_back(&group);
                }
                continue;
            }
            ++report.changed;
            set_state(library, group, SourceState::Changed);
            changed.push_back(&group);
        }
        std::vector<const FileGroup*> reread_files;
        if (options.reprobe_changed) {
            reread_files = changed;
        }
        reread_files.insert(reread_files.end(), outdated.begin(), outdated.end());
        if (!reread_files.empty() && !report.cancelled) {
            state.phase = MaintenancePhase::Reprobing;
            state.files_done = 0;
            state.files_total = reread_files.size();
            state.current.clear();
            report_progress(progress, state);
            ImportRequest request;
            for (const FileGroup* group : reread_files) {
                request.files.push_back(group->path);
            }
            request.probe_limits = options.probe_limits;
            request.record = false;
            request.reprobe_known = !outdated.empty();
            auto reread = import_sources(library, registry, request, {}, cancel);
            if (!reread) {
                return Unexpected(std::move(reread.error()));
            }
            append(report.reprobed_items, reread->refreshed);
            // A file re-read for an outdated probe keeps its hash; its unhashed items are listed
            // above already.
            std::unordered_set<std::uint64_t> listed;
            for (const ItemId id : report.unhashed_items) {
                listed.insert(id.value);
            }
            for (const std::vector<ItemId>* ids : {&reread->refreshed, &reread->added}) {  // added: new images
                for (const ItemId id : *ids) {
                    const std::optional<SourceItem> item = library.find(id);
                    if (item && !item->sha256 && listed.insert(id.value).second) {
                        report.unhashed_items.push_back(id);
                    }
                }
            }
            report.cancelled = report.cancelled || reread->cancelled;
            state.files_done = reread_files.size();
            report_progress(progress, state);
        }
        return report;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "checking the library's files ran out of memory",
                    std::string(kWhere));
    }
}

// ---------------------------------------------------------------------------------------------
// Relinking
// ---------------------------------------------------------------------------------------------

namespace {

struct RelinkCandidate {
    std::filesystem::path path;  // canonical
    core::FileIdentity identity;
};

std::string relink_folder_key(const core::FileIdentity& identity, const std::filesystem::path& canonical) {
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

}  // namespace

Expected<RelinkReport> relink_sources(Library& library, const RelinkRequest& request,
                                      const MaintenanceProgressCallback& progress,
                                      core::CancellationToken cancel) {
    if (Status supported = require_maintenance(library, kWhereRelink); !supported) {
        return Unexpected(std::move(supported.error()));
    }
    if (request.chunk_bytes == 0 || request.chunk_bytes > core::kMaxHashChunkBytes) {
        return fail(ErrorCode::InvalidArgument, "invalid hash chunk size", std::string(kWhereRelink));
    }
    auto folder = detail::canonical_path(request.folder, kWhereRelink);
    if (!folder) {
        return Unexpected(std::move(folder.error()));
    }
    if (!detail::is_directory(*folder)) {
        return fail(ErrorCode::InvalidArgument, "the folder to search is not a folder",
                    std::string(kWhereRelink));
    }
    try {
        RelinkReport report;
        std::vector<FileGroup> targets = group_files(library, request.items);
        std::erase_if(targets, [](const FileGroup& group) { return group.state != SourceState::Missing; });
        report.files = targets.size();
        if (targets.empty()) {
            return report;
        }
        // Folded file name -> targets with that name.
        std::unordered_map<std::wstring, std::vector<std::size_t>> by_name;
        for (std::size_t t = 0; t < targets.size(); ++t) {
            by_name[detail::fold_case(targets[t].path.filename().native())].push_back(t);
        }
        std::vector<std::vector<RelinkCandidate>> candidates(targets.size());

        // Scan the folder for files with a wanted name and size.
        MaintenanceProgress state;
        state.phase = MaintenancePhase::Scanning;
        std::unordered_set<std::string> visited;
        if (auto identity = core::stat_file(*folder)) {
            visited.insert(relink_folder_key(*identity, *folder));
        }
        std::vector<std::pair<std::filesystem::path, std::uint32_t>> stack{{*folder, 0}};
        while (!stack.empty() && !report.cancelled) {
            if (cancel.is_cancelled()) {
                report.cancelled = true;
                break;
            }
            auto [directory, depth] = std::move(stack.back());
            stack.pop_back();
            ++report.directories_scanned;
            state.files_done = report.directories_scanned;
            state.current = directory;
            report_progress(progress, state);
            auto entries = detail::list_directory(directory, kWhereRelink);
            if (!entries) {
                continue;  // an unreadable folder simply holds no candidates
            }
            std::vector<std::pair<std::filesystem::path, std::uint32_t>> subfolders;
            for (const detail::DirectoryEntry& entry : *entries) {
                const std::filesystem::path child = directory / entry.name;
                if (entry.device) {
                    continue;
                }
                if (entry.directory) {
                    if (!request.include_subfolders || depth + 1 > request.max_depth) {
                        continue;
                    }
                    auto canonical = detail::canonical_path(child, kWhereRelink);
                    if (!canonical) {
                        continue;
                    }
                    auto identity = core::stat_file(*canonical);
                    if (!identity || !visited.insert(relink_folder_key(*identity, *canonical)).second) {
                        continue;
                    }
                    subfolders.emplace_back(std::move(*canonical), depth + 1);
                    continue;
                }
                ++report.files_seen;
                if (core::is_temporary_file_name(entry.name)) {
                    continue;
                }
                const auto wanted = by_name.find(detail::fold_case(entry.name));
                if (wanted == by_name.end()) {
                    continue;
                }
                auto canonical = detail::canonical_path(child, kWhereRelink);
                if (!canonical) {
                    continue;
                }
                auto identity = core::stat_file(*canonical);
                if (!identity) {
                    continue;
                }
                for (const std::size_t t : wanted->second) {
                    if (identity->size_bytes == targets[t].identity.size_bytes) {
                        candidates[t].push_back(RelinkCandidate{*canonical, *identity});
                    }
                }
            }
            for (auto it = subfolders.rbegin(); it != subfolders.rend(); ++it) {
                stack.push_back(std::move(*it));
            }
        }

        // Confirm by content.
        state.phase = MaintenancePhase::Matching;
        state.files_done = 0;
        state.files_total = 0;
        for (std::size_t t = 0; t < targets.size(); ++t) {
            if (targets[t].sha256) {
                state.files_total += candidates[t].size();
            }
        }
        // Moves `target` to the candidate; false when the library refuses (logged).
        const auto relocate = [&](const FileGroup& target, const RelinkCandidate& candidate,
                                  const core::FileIdentity& identity,
                                  const std::optional<core::Digest256>& sha256) -> Expected<bool> {
            SourceRelocation relocation;
            relocation.path = candidate.path;
            relocation.identity = identity;
            relocation.sha256 = sha256;
            relocation.storage = core::storage_kind(candidate.path);
            if (Status moved = library.relocate_source(target.items.front(), relocation); !moved) {
                if (moved.error().code == ErrorCode::OutOfMemory) {
                    return Unexpected(std::move(moved.error()));
                }
                core::log_warn(kWhereRelink, "could not relink '{}' to '{}': {}",
                               core::path_to_utf8_lossy(target.path),
                               core::path_to_utf8_lossy(candidate.path), moved.error().describe());
                return false;
            }
            return true;
        };
        // Already another library file? (A file moved within its volume keeps its identity, so the
        // lookup may find the missing file itself: that one is a candidate.)
        const auto taken = [&](const FileGroup& target, const RelinkCandidate& candidate) {
            const std::optional<ItemId> owner = library.find_source(candidate.path, candidate.identity, 0);
            return owner && std::find(target.items.begin(), target.items.end(), *owner) == target.items.end();
        };
        for (std::size_t t = 0; t < targets.size(); ++t) {
            if (report.cancelled) {
                break;  // the remaining files were not looked at
            }
            const FileGroup& target = targets[t];
            bool relinked = false;
            // The library's own file version (same volume serial, file id, size and modification
            // time: moved within its volume, or a card under another drive letter) needs no hash.
            for (const RelinkCandidate& candidate : candidates[t]) {
                if (candidate.identity != target.identity || taken(target, candidate)) {
                    continue;
                }
                auto moved = relocate(target, candidate, candidate.identity, std::nullopt);
                if (!moved) {
                    return Unexpected(std::move(moved.error()));
                }
                if (*moved) {
                    relinked = true;
                    break;
                }
            }
            if (!relinked && !target.sha256) {
                ++report.unverifiable;
            } else if (!relinked) {
                for (const RelinkCandidate& candidate : candidates[t]) {
                    if (report.cancelled || cancel.is_cancelled()) {
                        report.cancelled = true;
                        break;
                    }
                    ++state.files_done;
                    state.current = candidate.path;
                    report_progress(progress, state);
                    if (taken(target, candidate)) {
                        continue;
                    }
                    auto hashed = core::hash_file(candidate.path, cancel, request.chunk_bytes);
                    if (!hashed) {
                        if (hashed.error().code == ErrorCode::Cancelled) {
                            report.cancelled = true;
                            break;
                        }
                        continue;
                    }
                    ++report.candidates_hashed;
                    state.bytes_done += hashed->identity.size_bytes;
                    if (hashed->digest != *target.sha256) {
                        continue;
                    }
                    auto moved = relocate(target, candidate, hashed->identity, hashed->digest);
                    if (!moved) {
                        return Unexpected(std::move(moved.error()));
                    }
                    if (*moved) {
                        relinked = true;
                        break;
                    }
                }
            }
            if (relinked) {
                ++report.relinked;
                append(report.relinked_items, target.items);
            } else {
                ++report.unmatched;
                append(report.unmatched_items, target.items);
            }
        }
        state.current.clear();
        report_progress(progress, state);
        return report;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "relinking ran out of memory", std::string(kWhereRelink));
    }
}

}  // namespace lucent::app
