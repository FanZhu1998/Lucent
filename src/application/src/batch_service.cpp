#include <lucent/application/batch_service.hpp>

#include <algorithm>
#include <cstdio>
#include <format>
#include <map>
#include <new>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <windows.h>

#include <lucent/application/import.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/application/source_maintenance.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/storage.hpp>
#include <lucent/core/text.hpp>
#include <lucent/core/win32_error.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/workers/job_output.hpp>
#include <lucent/workers/protocol.hpp>

#include "batch_service_impl.hpp"
#include "output_publication.hpp"
#include "win32_paths.hpp"

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.batch";
constexpr std::string_view kWhereOpen = "app.batch.open";
constexpr std::string_view kWhereOptions = "app.batch.options";
constexpr std::string_view kWhereEmpty = "app.batch.empty";
constexpr std::string_view kWhereSelection = "app.batch.selection";
// Room kept free on the destination beyond the next output.
constexpr std::uint64_t kDestinationReserveBytes = std::uint64_t{64} << 20;
constexpr std::uint64_t kFileOverheadBytes = std::uint64_t{1} << 20;
constexpr std::uint32_t kPageSize = 1000;

std::string quoted(const std::filesystem::path& path) { return "'" + core::path_to_utf8_lossy(path) + "'"; }

catalog::ItemId to_catalog(ItemId id) { return catalog::ItemId{static_cast<std::int64_t>(id.value)}; }
ItemId from_catalog(catalog::ItemId id) { return ItemId{static_cast<std::uint64_t>(id.value)}; }

std::string describe_versions() {
    std::string versions;
    for (const std::uint32_t version : engine::kDevelopEngineVersions) {
        if (!versions.empty()) {
            versions += '.';
        }
        versions += std::to_string(version);
    }
    return versions;
}

bool is_finished(const catalog::BatchRecord& record) { return detail::is_finished_batch(record); }

// Everything plan() and submit() work out about one photograph.
struct PreparedJob {
    SourceItem item;
    FrozenRecipe frozen;
    core::Digest256 settings_sha256{};
    std::filesystem::path final_directory;
    std::filesystem::path destination;
    bool renamed = false;
    bool replaces = false;
    std::optional<std::filesystem::path> existing_output;  // an identical output already in place
};

struct Prepared {
    BatchPlan plan;
    std::vector<PreparedJob> jobs;
    detail::DestinationDocument destination;
    detail::ExportDocument output;
    detail::PolicyDocument policy;
    std::string export_json;
    core::Digest256 export_sha256{};
    std::string name;
    std::string selection;
};

void add_exclusion(Prepared& prepared, ItemId item, std::filesystem::path source, Error reason) {
    ++prepared.plan.excluded_count;
    if (prepared.plan.excluded.size() < kMaxListedExclusions) {
        prepared.plan.excluded.push_back(PlanExclusion{item, std::move(source), std::move(reason)});
    }
}

}  // namespace

// ---- Free helpers ----------------------------------------------------------------------------

namespace detail {

std::string random_hex(std::size_t bytes) {
    std::random_device device;  // MSVC: the operating system's cryptographic generator
    std::string out;
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        out += std::format("{:02x}", device() & 0xFFu);
    }
    return out;
}

PhysicalMemory physical_memory() noexcept {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!::GlobalMemoryStatusEx(&status)) {
        return {};
    }
    return PhysicalMemory{status.ullTotalPhys, status.ullAvailPhys};
}

std::uint64_t output_pixels(const SourceItem& item) noexcept {
    if (!item.probe || item.probe->upright_size.empty()) {
        return 0;
    }
    return static_cast<std::uint64_t>(item.probe->upright_size.area());
}

}  // namespace detail

std::string_view to_string(BatchAttention attention) noexcept {
    switch (attention) {
        case BatchAttention::None:
            return "none";
        case BatchAttention::DestinationFull:
            return "destination_full";
        case BatchAttention::DestinationUnavailable:
            return "destination_unavailable";
        case BatchAttention::DestinationPermission:
            return "destination_permission";
        case BatchAttention::DestinationReplaced:
            return "destination_replaced";
    }
    return "invalid";
}

std::string_view to_string(ReportOutcome outcome) noexcept {
    switch (outcome) {
        case ReportOutcome::Exported:
            return "exported";
        case ReportOutcome::Skipped:
            return "skipped";
        case ReportOutcome::Failed:
            return "failed";
        case ReportOutcome::Cancelled:
            return "cancelled";
        case ReportOutcome::Pending:
            return "pending";
    }
    return "invalid";
}

Status BatchServiceOptions::validate() const {
    const auto invalid = [](std::string message) {
        return fail(ErrorCode::InvalidArgument, std::move(message), std::string(kWhereOptions));
    };
    if (max_workers < 1 || max_workers > 4) {
        return invalid("max_workers must be 1 to 4");
    }
    if (lease_duration < catalog::kMinLeaseDuration || lease_duration > catalog::kMaxLeaseDuration) {
        return invalid("lease_duration is outside the catalog's range");
    }
    if (lease_renewal.count() <= 0 || lease_renewal >= lease_duration) {
        return invalid("lease_renewal must be positive and shorter than lease_duration");
    }
    if (job_timeout.count() < 0) {
        return invalid("job_timeout cannot be negative");
    }
    if (heartbeat_interval < std::chrono::milliseconds(workers::kMinHeartbeatMs) ||
        heartbeat_interval > std::chrono::milliseconds(workers::kMaxHeartbeatMs) ||
        heartbeat_timeout <= heartbeat_interval) {
        return invalid("heartbeat_interval must be 10 ms to 60 s and below heartbeat_timeout");
    }
    if (cancel_grace.count() <= 0) {
        return invalid("cancel_grace must be positive");
    }
    if (reclaim_interval.count() <= 0) {
        return invalid("reclaim_interval must be positive");
    }
    if (memory.total_bytes != 0 && memory.interactive_reserve_bytes >= memory.total_bytes) {
        return invalid("the interactive reserve must be below the total memory budget");
    }
    if (!(memory.default_bytes_per_pixel > 0.0) || !(memory.measured_margin >= 1.0)) {
        return invalid("default_bytes_per_pixel must be positive and measured_margin at least 1");
    }
    if (retry.initial_backoff.count() <= 0 || retry.max_backoff < retry.initial_backoff) {
        return invalid("the retry back-off must be positive and max_backoff at least initial_backoff");
    }
    if (stale_temporary_age.count() < 0) {
        return invalid("stale_temporary_age cannot be negative");
    }
    return ok();
}

// ---- Impl: contexts, events --------------------------------------------------------------------

Expected<catalog::BatchRecord> BatchService::Impl::batch_record(catalog::BatchId id) const {
    auto found = catalog->query([&](catalog::ReadTransaction& tx) { return tx.find_batch(id); });
    if (!found) {
        return Unexpected(std::move(found.error()));
    }
    if (!*found) {
        return fail(ErrorCode::NotFound, std::format("no batch {}", id.value), std::string(kWhere));
    }
    return std::move(**found);
}

Expected<std::shared_ptr<detail::BatchContext>> BatchService::Impl::load_context(const catalog::BatchRecord& record,
                                                                                bool cache) const {
    {
        std::lock_guard lock(mutex);
        if (const auto found = contexts.find(record.id.value); found != contexts.end()) {
            return found->second;
        }
    }
    auto context = std::make_shared<detail::BatchContext>();
    context->id = record.id;
    context->name = record.name;
    auto destination = detail::read_destination(record.destination_json);
    if (!destination) {
        return Unexpected(std::move(destination.error()));
    }
    context->destination = std::move(*destination);
    auto pattern = parse_name_pattern(context->destination.name_pattern);
    if (!pattern) {
        return Unexpected(std::move(pattern.error()));
    }
    context->pattern = std::move(*pattern);
    auto output = detail::read_export_document(record.export_settings_json);
    if (!output) {
        return Unexpected(std::move(output.error()));
    }
    context->output = std::move(*output);
    auto policy = detail::read_policy(record.policy_json);
    if (!policy) {
        return Unexpected(std::move(policy.error()));
    }
    context->policy = std::move(*policy);
    context->renderer_version = record.renderer_version;
    context->export_settings_sha256 = record.export_settings_sha256;
    context->folder_identity = context->destination.folder_identity;
    if (!cache) {
        return context;
    }
    std::lock_guard lock(mutex);
    return contexts.try_emplace(record.id.value, std::move(context)).first->second;
}

Expected<std::shared_ptr<detail::BatchContext>> BatchService::Impl::context_for(catalog::BatchId id) const {
    {
        std::lock_guard lock(mutex);
        if (const auto found = contexts.find(id.value); found != contexts.end()) {
            return found->second;
        }
    }
    auto record = batch_record(id);
    if (!record) {
        return Unexpected(std::move(record.error()));
    }
    return load_context(*record, !is_finished(*record));
}

void BatchService::Impl::emit(BatchEvent event) const { observers.notify(event); }

void BatchService::Impl::emit_state(catalog::BatchId id, BatchEventKind kind, std::optional<Error> error) {
    auto record = batch_record(id);
    if (!record) {
        return;
    }
    BatchEvent event;
    event.kind = kind;
    event.batch = id;
    event.state = record->state;
    event.counts = record->counts;
    event.error = std::move(error);
    if (auto context = context_for(id)) {
        std::lock_guard lock(mutex);
        event.attention = (*context)->attention;
    }
    emit(std::move(event));
}

void BatchService::Impl::request_wake() noexcept {
    {
        std::lock_guard lock(mutex);
        wake_requested = true;
    }
    coordinator_cv.notify_all();
}

void BatchService::Impl::request_reclaim() noexcept {
    {
        std::lock_guard lock(mutex);
        reclaim_requested = true;
        wake_requested = true;
    }
    coordinator_cv.notify_all();
}

void BatchService::Impl::note_active(catalog::BatchId id) {
    std::lock_guard lock(mutex);
    active_batches.insert(id.value);
    ++active_generation;
}

Status BatchService::Impl::scan_batches(const std::function<void(const catalog::BatchRecord&)>& visit) {
    std::vector<std::int64_t> unfinished;
    for (catalog::BatchId before{};;) {
        auto batches = catalog->query([&](catalog::ReadTransaction& tx) { return tx.list_batches(kPageSize, before); });
        if (!batches) {
            return Unexpected(std::move(batches.error()));
        }
        for (const catalog::BatchRecord& record : *batches) {
            before = record.id;
            if (!is_finished(record)) {
                unfinished.push_back(record.id.value);
            }
            if (visit) {
                visit(record);
            }
        }
        if (batches->size() < kPageSize) {
            break;
        }
    }
    std::lock_guard lock(mutex);
    active_batches.insert(unfinished.begin(), unfinished.end());
    ++active_generation;
    active_scanned = true;
    return ok();
}

void BatchService::Impl::check_finished(catalog::BatchId id) {
    auto record = batch_record(id);
    if (!record) {
        return;
    }
    auto context = context_for(id);
    if (!context) {
        return;
    }
    const bool finished = is_finished(*record);
    {
        std::lock_guard lock(mutex);
        if (!finished) {
            if (active_batches.insert(id.value).second) {
                ++active_generation;
            }
            if (record->state == catalog::BatchState::Running || record->state == catalog::BatchState::Paused) {
                finished_batches.erase(id.value);
            }
            return;
        }
        // active_batches keeps it until dispatch() reads it as finished (the only place that drops
        // batches, guarded against a batch reopened meanwhile).
        if (!finished_batches.insert(id.value).second) {
            return;
        }
    }
    // The report files are written before Finished is announced, so an observer of Finished finds
    // them (ReportWritten follows with their path; a failure to write them is logged).
    std::optional<ReportFiles> files;
    bool handled = !(*context)->destination.write_report_files;
    if ((*context)->destination.write_report_files) {
        auto written = write_report_files(id);
        if (written) {
            files = std::move(*written);
            handled = true;
        } else {
            // Not recorded as reported: the next start writes it (report_unreported).
            core::log_warn(kWhere, "the report of batch {} was not written: {}", id.value, written.error().describe());
        }
    }
    if (handled && !record->reported_at) {
        if (Status marked = catalog->write([&](catalog::WriteTransaction& tx) { return tx.mark_batch_reported(id, now()); });
            !marked) {
            core::log_warn(kWhere, "cannot record the report of batch {}: {}", id.value, marked.error().describe());
        }
    }
    BatchEvent event;
    event.kind = BatchEventKind::Finished;
    event.batch = id;
    event.state = record->state;
    event.counts = record->counts;
    emit(event);
    if (files) {
        BatchEvent written = event;
        written.kind = BatchEventKind::ReportWritten;
        written.output = files->html;
        emit(std::move(written));
    }
    // Nothing runs any more: drop the batch's runtime state (it is parsed again when needed). An
    // attempt that is still returning keeps its own reference.
    std::lock_guard lock(mutex);
    contexts.erase(id.value);
}

std::optional<std::uint64_t> BatchService::Impl::free_space(const std::filesystem::path& directory) const {
    if (options.test.free_space) {
        return options.test.free_space(directory);
    }
    auto bytes = detail::free_disk_bytes(directory, detail::kWhereDestination);
    if (!bytes) {
        return std::nullopt;
    }
    return *bytes;
}

Expected<detail::FrozenDocument> BatchService::Impl::frozen_document(const catalog::JobRecord& job,
                                                                     ItemId item) const {
    engine::DevelopSettings settings;
    detail::FrozenDocument frozen;
    if (job.revision) {
        auto revision = catalog->query([&](catalog::ReadTransaction& tx) { return tx.find_revision(*job.revision); });
        if (!revision) {
            return Unexpected(std::move(revision.error()));
        }
        if (!*revision || (*revision)->info.item.value != static_cast<std::int64_t>(item.value)) {
            return fail(ErrorCode::NotFound, std::format("recipe revision {} of the job is gone", job.revision->value),
                        std::string(detail::kWhereRecipe));
        }
        auto read = read_recipe((*revision)->settings_json);
        if (!read) {
            Error error = std::move(read.error());
            error.where = std::string(detail::kWhereRecipe);
            return Unexpected(std::move(error));
        }
        settings = std::move(*read);
        frozen.revision_number = (*revision)->info.revision_number;
    }
    auto document = write_recipe(settings);
    if (!document) {
        return Unexpected(std::move(document.error()));
    }
    if (document->sha256 != job.settings_sha256) {
        return fail(ErrorCode::VersionMismatch,
                    "the recipe frozen for this job cannot be reproduced by this version of Lucent (its canonical "
                    "document has another SHA-256)",
                    std::string(detail::kWhereRecipe));
    }
    frozen.sha256 = document->sha256;
    frozen.json = std::move(document->json);
    return frozen;
}

// ---- Recovery ----------------------------------------------------------------------------------

namespace {

// The journal entries of `attempt`, in order.
std::vector<catalog::PublicationRecord> attempt_entries(const std::vector<catalog::PublicationRecord>& journal,
                                                        std::uint32_t attempt) {
    std::vector<catalog::PublicationRecord> out;
    for (const catalog::PublicationRecord& entry : journal) {
        if (entry.attempt == attempt) {
            out.push_back(entry);
        }
    }
    return out;
}

bool path_exists(const std::filesystem::path& path) {
    auto api = detail::api_path(path, "app.batch.recover");
    return api && ::GetFileAttributesW(api->c_str()) != INVALID_FILE_ATTRIBUTES;
}

// An output already in the folder for a plan: present with the receipt's size (plan() does not
// hash; submit() confirms with check_file).
bool output_present(const std::filesystem::path& path, std::uint64_t size) {
    auto id = core::stat_file(path);
    return id && id->size_bytes == size;
}

}  // namespace

detail::FileCheck detail::check_file(const std::filesystem::path& path, std::uint64_t size,
                                     const core::Digest256& sha256, const core::CancellationToken& cancel) {
    auto id = core::stat_file(path);
    if (!id) {
        return detail::indicates_absence(id.error()) ? FileCheck::Mismatch : FileCheck::Unknown;
    }
    if (id->size_bytes != size) {
        return FileCheck::Mismatch;
    }
    // A stable read that shares with every other reader; it fails with a sharing violation only
    // while another program holds the file exclusively (a sync client, an antivirus scan).
    auto hashed = core::hash_file(path, cancel);
    if (!hashed) {
        return detail::indicates_absence(hashed.error()) ? FileCheck::Mismatch : FileCheck::Unknown;
    }
    return hashed->digest == sha256 && hashed->identity.size_bytes == size ? FileCheck::Match : FileCheck::Mismatch;
}

std::optional<core::FileIdentity> BatchService::Impl::folder_identity_of(const detail::BatchContext& context) const {
    std::lock_guard lock(mutex);
    return context.folder_identity;
}

detail::Reach BatchService::Impl::reach_destination(const detail::BatchContext& context) const {
    auto id = core::stat_file(context.destination.folder);
    if (!id) {
        return detail::Reach::Unreachable;
    }
    const std::optional<core::FileIdentity> recorded = folder_identity_of(context);
    if (recorded && !core::is_same_file(*id, *recorded)) {
        return detail::Reach::Replaced;
    }
    return detail::Reach::Reachable;
}

void BatchService::Impl::note_deferred_job(catalog::JobId job, std::string_view why) {
    bool first = false;
    {
        std::lock_guard lock(mutex);
        first = deferred_jobs.insert(job.value).second;
        if (first) {
            ++recovery.deferred;
        }
    }
    if (first) {
        core::log_warn(kWhere, "job {} is reconciled later: {}", job.value, why);
    }
}

Error BatchService::Impl::interruption_error(std::string_view owner) const {
    const bool this_session = owner.starts_with(session + "/");
    if (this_session) {
        return Error{ErrorCode::Timeout,
                     "the attempt's lease expired (the computer slept, or a step took longer than the lease); the "
                     "photo was queued again",
                     std::string(detail::kWhereLeaseExpired), 0};
    }
    return Error{ErrorCode::Timeout, "Lucent stopped while this photo was being exported; it was queued again",
                 std::string(detail::kWhereInterrupted), 0};
}

Expected<catalog::JobStage> BatchService::Impl::recover_interrupted(const catalog::JobLease& lease,
                                                                    const catalog::JobRecord& job, Error error,
                                                                    const detail::BatchContext* context) {
    catalog::JobRecovery request;
    request.now = now();
    request.owner_gone = true;
    request.interruption = true;
    request.failure_class = catalog::FailureClass::Transient;
    request.retry_at = request.now;
    request.action = catalog::RecoveryAction::Requeue;
    const std::uint32_t bound =
        context != nullptr ? context->policy.retry.max_interruptions : BatchRetryPolicy{}.max_interruptions;
    if (job.interruptions >= bound) {
        // Bounded on its own: a job whose every attempt is interrupted must not loop forever.
        request.action = catalog::RecoveryAction::Fail;
        error = Error{ErrorCode::Timeout,
                      std::format("the export of this photo was interrupted {} times ({})", job.interruptions + 1,
                                  error.message),
                      std::string(detail::kWhereInterruptedTooOften), 0};
    }
    request.error = std::move(error);
    return catalog->transact([&](catalog::WriteTransaction& tx) { return tx.recover_job(lease, request); });
}

bool BatchService::Impl::close_publication(const catalog::PublicationInProgress& open,
                                           std::vector<std::filesystem::path>* keep) {
    const catalog::PublicationRecord& latest = open.latest;
    auto context = context_for(open.batch);
    const std::optional<std::filesystem::path> aside =
        context && latest.final_path
            ? std::optional<std::filesystem::path>(
                  latest.final_path->parent_path() /
                  detail::aside_file_name((*context)->destination.temp_tag, latest.job.value, latest.attempt))
            : std::nullopt;
    const auto defer = [&](std::string_view why) {
        bool first = false;
        {
            std::lock_guard lock(mutex);
            first = deferred_publications.insert({latest.job.value, latest.attempt}).second;
            if (first) {
                ++recovery.deferred;
            }
        }
        if (first) {
            core::log_warn(kWhere, "the publication of job {} attempt {} is closed later: {}", latest.job.value,
                           latest.attempt, why);
        }
        if (keep != nullptr) {
            if (latest.temp_path) {
                keep->push_back(*latest.temp_path);
            }
            if (aside) {
                keep->push_back(*aside);
            }
        }
        return false;
    };
    // Nothing is decided about files that cannot be seen: an unplugged drive, a share that is not
    // reconnected yet, or another folder at the output folder's path.
    if (context && reach_destination(**context) != detail::Reach::Reachable) {
        return defer("its output folder cannot be reached");
    }
    // While the temporary file is still there, the rename did not happen.
    const bool temp_present = latest.temp_path && path_exists(*latest.temp_path);
    // An output renamed into place whose Published step never committed (crash between the rename
    // and the journal write): the attempt's own verified bytes, unrecorded. Remove it so that every
    // published file has a receipt; the job is rendered again.
    bool orphan = false;
    if (latest.step == catalog::PublicationStep::Verified && latest.final_path && latest.output_sha256 &&
        latest.output_size && !temp_present) {
        switch (detail::check_file(*latest.final_path, *latest.output_size, *latest.output_sha256)) {
            case detail::FileCheck::Match:
                orphan = true;
                break;
            case detail::FileCheck::Mismatch:
                break;
            case detail::FileCheck::Unknown:
                return defer("its output cannot be read now");
        }
    }
    {
        std::lock_guard lock(mutex);
        deferred_publications.erase({latest.job.value, latest.attempt});
    }
    // The temporary output and its scratch folder.
    if (latest.temp_path) {
        if (Status removed = workers::remove_job_output(*latest.temp_path); removed) {
            std::lock_guard lock(mutex);
            ++recovery.leftovers_removed;
        } else {
            core::log_warn(kWhere, "cannot remove {}: {}", core::path_to_utf8_lossy(*latest.temp_path),
                           removed.error().describe());
        }
    }
    if (orphan) {
        bool recorded = false;
        auto job = catalog->query([&](catalog::ReadTransaction& tx) { return tx.find_job(latest.job); });
        if (job && *job) {
            auto receipts = catalog->query(
                [&](catalog::ReadTransaction& tx) { return tx.find_receipts_by_identity((*job)->identity_sha256); });
            if (receipts) {
                for (const catalog::ReceiptRecord& receipt : *receipts) {
                    if (detail::same_path_text(receipt.final_path, *latest.final_path)) {
                        recorded = true;
                    }
                }
            }
        }
        if (!recorded) {
            if (Status removed = detail::remove_unused_file(*latest.final_path, "app.batch.recover"); removed) {
                std::lock_guard lock(mutex);
                ++recovery.orphans_removed;
            } else {
                core::log_warn(kWhere, "cannot remove the unrecorded output {}: {}",
                               core::path_to_utf8_lossy(*latest.final_path), removed.error().describe());
            }
        }
    }
    // An overwrite interrupted after the existing file was moved aside: put it back when its name is
    // free again (the replacement never committed).
    if (aside && path_exists(*aside)) {
        if (!path_exists(*latest.final_path)) {
            auto held = detail::HeldOutput::open_exclusive(*aside, "app.batch.recover");
            if (held && held->rename_no_clobber(*latest.final_path)) {
                std::lock_guard lock(mutex);
                ++recovery.restored_aside;
            }
        } else {
            (void)detail::remove_unused_file(*aside, "app.batch.recover");
        }
    }
    if (Status closed = catalog->write([&](catalog::WriteTransaction& tx) {
            return tx.abandon_publication(latest.job, latest.attempt, now());
        });
        closed) {
        std::lock_guard lock(mutex);
        ++recovery.publications_closed;
    } else {
        core::log_warn(kWhere, "cannot close the publication of job {} attempt {}: {}", latest.job.value,
                       latest.attempt, closed.error().describe());
    }
    return true;
}

bool BatchService::Impl::reconcile_job(const catalog::JobRecord& job, std::vector<std::filesystem::path>* keep) {
    if (!job.lease) {
        return true;
    }
    const catalog::JobLease lease = *job.lease;
    auto context = context_for(job.batch);
    auto journal = catalog->query([&](catalog::ReadTransaction& tx) { return tx.publication_journal(job.id); });
    auto receipt = catalog->query([&](catalog::ReadTransaction& tx) { return tx.find_receipt(job.id); });
    if (!journal || !receipt) {
        core::log_error(kWhere, "cannot read the journal of job {}", job.id.value);
        return false;
    }
    const std::vector<catalog::PublicationRecord> entries = attempt_entries(*journal, lease.attempt);
    const std::optional<catalog::PublicationRecord> latest =
        entries.empty() ? std::nullopt : std::optional<catalog::PublicationRecord>(entries.back());
    const auto defer = [&](std::string_view why) {
        note_deferred_job(job.id, why);
        if (keep != nullptr && latest) {
            if (latest->temp_path) {
                keep->push_back(*latest->temp_path);
            }
            if (context && latest->final_path) {
                keep->push_back(latest->final_path->parent_path() /
                                detail::aside_file_name((*context)->destination.temp_tag, job.id.value, lease.attempt));
            }
        }
        return false;
    };
    // An output folder out of reach (a drive not plugged in yet, a share not reconnected, another
    // folder at its path) says nothing about the outputs in it: decide once it is back.
    if (context && reach_destination(**context) != detail::Reach::Reachable) {
        return defer("its output folder cannot be reached");
    }

    const auto remove_aside = [&](const std::filesystem::path& final_path) {
        if (context) {
            (void)detail::remove_unused_file(
                final_path.parent_path() /
                    detail::aside_file_name((*context)->destination.temp_tag, job.id.value, lease.attempt),
                "app.batch.recover");
        }
    };

    std::optional<catalog::JobRecovery> decided;  // Complete or Fail; otherwise an interruption
    if (*receipt) {
        // The receipt committed; only the stage change was lost. Verify before counting it.
        const catalog::ReceiptRecord& stored = **receipt;
        switch (detail::check_file(stored.final_path, stored.size_bytes, stored.output_sha256)) {
            case detail::FileCheck::Match:
                decided.emplace();
                decided->action = catalog::RecoveryAction::Complete;
                remove_aside(stored.final_path);
                break;
            case detail::FileCheck::Mismatch:
                // Its folder is reachable and the file is gone or holds other bytes: lost after it was
                // published. Receipts are one per job, so this job cannot publish again.
                decided.emplace();
                decided->action = catalog::RecoveryAction::Fail;
                decided->failure_class = catalog::FailureClass::Deterministic;
                decided->error = Error{ErrorCode::NotFound,
                                       std::format("the published output {} is missing or was changed",
                                                   quoted(stored.final_path)),
                                       std::string(detail::kWhereOutputLost), 0};
                break;
            case detail::FileCheck::Unknown:
                return defer("its published output cannot be read now");
        }
    } else if (latest && latest->step == catalog::PublicationStep::Published && latest->final_path &&
               latest->output_sha256 && latest->output_size) {
        // Published, receipt not committed: the output is verified against the journal.
        switch (detail::check_file(*latest->final_path, *latest->output_size, *latest->output_sha256)) {
            case detail::FileCheck::Match: {
                catalog::NewReceipt made;
                made.final_path = *latest->final_path;
                made.size_bytes = *latest->output_size;
                made.output_sha256 = *latest->output_sha256;
                ReceiptDetails details;
                details.revision = job.revision;
                details.recipe_sha256 = job.settings_sha256;
                details.attempt = lease.attempt;
                if (context) {
                    details.settings = (*context)->output.settings;
                    details.export_settings_sha256 = (*context)->export_settings_sha256;
                    details.format = details.settings.format;
                    details.profile = details.settings.profile;
                    details.bit_depth = exporter::bit_depth_for(details.settings.format);
                }
                details.review.push_back(
                    "Lucent stopped while publishing this photo; the published file was verified afterwards, but "
                    "its render details were not recorded.");
                auto json = write_receipt_details(details);
                made.settings_json = json ? std::move(*json) : std::string("{}");
                decided.emplace();
                decided->action = catalog::RecoveryAction::Complete;
                decided->receipt = std::move(made);
                remove_aside(*latest->final_path);
                break;
            }
            case detail::FileCheck::Mismatch:
                break;  // gone or changed since: rendered again
            case detail::FileCheck::Unknown:
                return defer("its published output cannot be read now");
        }
    } else if (latest && latest->step == catalog::PublicationStep::Verified && latest->final_path &&
               latest->output_sha256 && latest->output_size && latest->temp_path && !path_exists(*latest->temp_path) &&
               detail::check_file(*latest->final_path, *latest->output_size, *latest->output_sha256) ==
                   detail::FileCheck::Unknown) {
        // Whether the rename happened (and an unrecorded output must go) cannot be told yet.
        return defer("its output cannot be read now");
    }

    Expected<catalog::JobStage> stage = catalog::JobStage::Queued;
    if (decided) {
        decided->now = now();
        decided->owner_gone = true;
        stage = catalog->transact([&](catalog::WriteTransaction& tx) { return tx.recover_job(lease, *decided); });
    } else {
        stage = recover_interrupted(lease, job, interruption_error(lease.owner), context ? context->get() : nullptr);
    }
    if (!stage) {
        core::log_error(kWhere, "cannot recover job {}: {}", job.id.value, stage.error().describe());
        return false;
    }
    {
        std::lock_guard lock(mutex);
        deferred_jobs.erase(job.id.value);
        ++recovery.reclaimed;
        if (*stage == catalog::JobStage::Complete) {
            ++recovery.completed;
        } else if (*stage == catalog::JobStage::Failed) {
            ++recovery.failed;
        } else {
            ++recovery.requeued;
        }
    }
    const bool interrupted = !decided || decided->action != catalog::RecoveryAction::Complete;
    if (interrupted && latest && latest->step != catalog::PublicationStep::ReceiptCommitted &&
        latest->step != catalog::PublicationStep::Abandoned) {
        catalog::PublicationInProgress open;
        open.latest = *latest;
        open.batch = job.batch;
        (void)close_publication(open, keep);
    }
    return true;
}

void BatchService::Impl::report_unreported() {
    auto batches = catalog->query([&](catalog::ReadTransaction& tx) { return tx.list_unreported_batches(kPageSize); });
    if (!batches) {
        core::log_warn(kWhere, "cannot list the batches without a report: {}", batches.error().describe());
        return;
    }
    for (const catalog::BatchRecord& record : *batches) {
        if (!is_finished(record)) {
            continue;
        }
        auto context = load_context(record, false);
        if (!context) {
            continue;
        }
        if ((*context)->destination.write_report_files && reach_destination(**context) != detail::Reach::Reachable) {
            continue;  // tried again at the next start
        }
        {
            std::lock_guard lock(mutex);
            finished_batches.erase(record.id.value);  // announced again, with its report
        }
        check_finished(record.id);
        auto reported = batch_record(record.id);
        if (reported && reported->reported_at && (*context)->destination.write_report_files) {
            std::lock_guard lock(mutex);
            ++recovery.reports_written;
        }
    }
}

Status BatchService::Impl::recover() {
    const catalog::Timestamp started = now();
    std::set<std::int64_t> touched_batches;
    std::vector<std::filesystem::path> keep;  // files deferred reconciliations still need
    // 1. Leases of a previous session (no worker survives its coordinator: Job Objects end them).
    std::int64_t after_job = 0;
    for (;;) {
        catalog::ReclaimQuery query;
        query.now = started;
        query.live_owners = std::vector<std::string>{};
        query.limit = kPageSize;
        auto jobs = catalog->query([&](catalog::ReadTransaction& tx) { return tx.list_reclaimable_jobs(query); });
        if (!jobs) {
            return Unexpected(std::move(jobs.error()));
        }
        std::size_t fresh = 0;
        for (const catalog::JobRecord& job : *jobs) {
            if (job.id.value <= after_job) {
                continue;  // deferred in an earlier page: still leased
            }
            ++fresh;
            after_job = job.id.value;
            touched_batches.insert(job.batch.value);
            (void)reconcile_job(job, &keep);
        }
        if (jobs->size() < kPageSize || fresh == 0) {
            break;
        }
    }
    // 2. Publications left open by attempts that no longer hold a lease.
    std::int64_t after = 0;
    for (;;) {
        auto open = catalog->query([&](catalog::ReadTransaction& tx) {
            return tx.list_publications_in_progress(std::nullopt, kPageSize, after);
        });
        if (!open) {
            return Unexpected(std::move(open.error()));
        }
        for (const catalog::PublicationInProgress& entry : *open) {
            after = std::max(after, entry.latest.id);
            if (!entry.attempt_is_live()) {
                touched_batches.insert(entry.batch.value);
                (void)close_publication(entry, &keep);
            }
        }
        if (open->size() < kPageSize) {
            break;
        }
    }
    // 3. Leftovers in the output folders of unfinished batches and of the batches reconciled above
    //    (every batch is looked at: each single export is a batch, so there can be many), except
    //    the files of deferred reconciliations. The same scan fills the set of unfinished batches
    //    that dispatch() works from.
    std::map<std::wstring, std::pair<std::filesystem::path, std::vector<std::wstring>>> folders;
    const Status scanned = scan_batches([&](const catalog::BatchRecord& record) {
        if (is_finished(record) && !touched_batches.contains(record.id.value)) {
            return;
        }
        auto context = load_context(record, !is_finished(record));
        if (!context) {
            core::log_warn(kWhere, "batch {} cannot be read: {}", record.id.value, context.error().describe());
            return;
        }
        const std::filesystem::path& folder = (*context)->destination.folder;
        auto& entry = folders[folded_path_key(folder)];
        entry.first = folder;
        entry.second.push_back((*context)->destination.temp_tag);
    });
    if (!scanned) {
        return scanned;
    }
    for (const auto& [key, entry] : folders) {
        const detail::SweepResult swept =
            detail::sweep_publication_leftovers(entry.first, entry.second, keep, options.stale_temporary_age);
        std::lock_guard lock(mutex);
        recovery.leftovers_removed += swept.removed;
    }
    // 4. Batches that finished during reconciliation get their report, and so do batches that
    //    finished just before Lucent stopped (their report was not written yet).
    for (const std::int64_t id : touched_batches) {
        check_finished(catalog::BatchId{id});
    }
    report_unreported();
    std::lock_guard lock(mutex);
    if (recovery.reclaimed + recovery.publications_closed + recovery.leftovers_removed + recovery.deferred +
            recovery.reports_written >
        0) {
        core::log_info(kWhere,
                       "recovered the batch journal: {} leases reclaimed ({} completed, {} requeued, {} failed), "
                       "{} publications closed, {} unrecorded outputs and {} leftovers removed, {} deferred until "
                       "their output folder can be read, {} reports written",
                       recovery.reclaimed, recovery.completed, recovery.requeued, recovery.failed,
                       recovery.publications_closed, recovery.orphans_removed, recovery.leftovers_removed,
                       recovery.deferred, recovery.reports_written);
    }
    return ok();
}

// ---- Open and close ----------------------------------------------------------------------------

BatchService::BatchService(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

BatchService::~BatchService() {
    if (impl_) {
        impl_->stop_threads();
    }
}

Expected<std::unique_ptr<BatchService>> BatchService::open(Library& library, std::shared_ptr<catalog::Catalog> catalog,
                                                           RecipeService& recipes, BatchServiceOptions options) {
    if (Status valid = options.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    if (!catalog) {
        return fail(ErrorCode::InvalidArgument, "the batch service needs a catalog", std::string(kWhereOpen));
    }
    if (!library.capabilities().persistent) {
        return fail(ErrorCode::InvalidArgument,
                    "batches need the persistent library: the job journal references catalog items",
                    std::string(kWhereOpen));
    }
    if (catalog->is_read_only()) {
        if (const auto& reason = catalog->schema().read_only_reason) {
            return Unexpected(*reason);
        }
        return fail(ErrorCode::PermissionDenied, "the catalog is read-only", std::string(kWhereOpen));
    }
    try {
        auto impl = std::make_unique<Impl>(library, std::move(catalog), recipes, std::move(options));
        impl->session = detail::random_hex(6);
        impl->renderer_version = std::format("lucent {}; develop {}; worker protocol {}",
                                             workers::describe(workers::current_build_identity()),
                                             describe_versions(), workers::kMaxProtocolVersion);
        if (impl->renderer_version.size() > catalog::kMaxJobLabelBytes) {
            impl->renderer_version.resize(catalog::kMaxJobLabelBytes);
        }
        const detail::PhysicalMemory memory = detail::physical_memory();
        constexpr std::uint64_t kGiB = std::uint64_t{1} << 30;
        std::uint64_t total = impl->options.memory.total_bytes;
        if (total == 0) {
            total = std::clamp<std::uint64_t>(memory.total / 2, 2 * kGiB, 8 * kGiB);
        }
        impl->total_budget = total;
        impl->worker_budget = std::max<std::uint64_t>(
            kGiB, total > impl->options.memory.interactive_reserve_bytes ? total - impl->options.memory.interactive_reserve_bytes : 0);
        impl->counters.total_budget_bytes = impl->total_budget;
        impl->counters.worker_budget_bytes = impl->worker_budget;
        impl->counters.interactive_reserve_bytes = impl->options.memory.interactive_reserve_bytes;
        impl->counters.max_workers = impl->options.max_workers;
        // Recovery runs first on the coordinator thread (see "Recovery"): checking outputs on a slow
        // or disconnected destination must never hold up the caller (Lucent's start-up).
        impl->start_threads();
        return std::unique_ptr<BatchService>(new BatchService(std::move(impl)));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot start the batch service", std::string(kWhereOpen));
    } catch (const std::system_error& e) {
        return fail(ErrorCode::Internal, std::format("cannot start the batch threads: {}", e.what()),
                    std::string(kWhereOpen));
    }
}

// ---- Planning and submission -------------------------------------------------------------------

namespace {

enum class PrepareMode : std::uint8_t { Plan, Submit };

struct Selected {
    ItemId item;
    std::optional<FrozenRecipe> recipe;
};

}  // namespace

// Implemented here rather than in Impl to keep the header small: the shared work of plan() and
// submit().
static Expected<Prepared> prepare_batch(BatchService::Impl& s, const BatchRequest& request, PrepareMode mode,
                                        const BatchSubmitProgressCallback& progress,
                                        const core::CancellationToken& cancel) {
    const auto report = [&](BatchSubmitProgress::Phase phase, std::uint64_t done, std::uint64_t total) {
        if (progress) {
            progress(BatchSubmitProgress{phase, done, total});
        }
    };
    Prepared prepared;

    // ---- Settings and destination ------------------------------------------------------------
    if (Status valid = request.output.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    auto pattern = parse_name_pattern(request.destination.name_pattern);
    if (!pattern) {
        return Unexpected(std::move(pattern.error()));
    }
    const std::filesystem::path& given = request.destination.folder;
    if (given.empty() || !given.is_absolute()) {
        return fail(ErrorCode::InvalidArgument, "the output folder must be an absolute path",
                    std::string(detail::kWhereDestination));
    }
    for (const auto* text : {&request.artist, &request.copyright}) {
        if (*text && (!core::is_valid_utf8(**text) || (*text)->size() > exporter::kMaxMetadataTextBytes)) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("artist and copyright must be valid UTF-8 of at most {} bytes",
                                    exporter::kMaxMetadataTextBytes),
                        std::string(kWhere));
        }
    }
    std::filesystem::path folder = given;
    bool folder_exists = detail::is_directory(given);
    if (!folder_exists && mode == PrepareMode::Submit) {
        if (Status created = core::create_directory_tree(given); !created) {
            Error error = std::move(created.error());
            error.where = std::string(detail::kWhereDestination);
            return Unexpected(std::move(error));
        }
        folder_exists = true;
    }
    if (folder_exists) {
        auto canonical = detail::canonical_path(given, detail::kWhereDestination);
        if (!canonical) {
            return Unexpected(std::move(canonical.error()));
        }
        folder = std::move(*canonical);
    }
    prepared.plan.folder = folder;
    prepared.plan.folder_exists = folder_exists;
    prepared.plan.storage = core::storage_kind(folder_exists ? folder : given.root_path());
    if (prepared.plan.storage == core::StorageKind::Network) {
        prepared.plan.notices.push_back(PlanNotice::NetworkDestination);
        prepared.plan.warnings.push_back(
            "the destination is a network location: outputs are verified, but a network share may not keep "
            "them durably if the connection is lost");
    } else if (prepared.plan.storage == core::StorageKind::Removable) {
        prepared.plan.notices.push_back(PlanNotice::RemovableDestination);
        prepared.plan.warnings.push_back(
            "the destination is removable: keep it connected until the batch finishes");
    }
    if (request.destination.collisions == CollisionPolicy::Overwrite) {
        prepared.plan.notices.push_back(PlanNotice::ReplacesExisting);
        prepared.plan.warnings.push_back(
            "existing files with an output's name will be replaced (never an original, never another output of "
            "this batch)");
    }
    if (request.output.metadata.include_gps) {
        prepared.plan.notices.push_back(PlanNotice::WritesGps);
        prepared.plan.warnings.push_back("GPS positions will be written into the exported files");
    }
    detail::DestinationDocument& destination = prepared.destination;
    destination.folder = folder;
    destination.name_pattern = pattern->text;
    destination.mirror_subfolders = request.destination.mirror_subfolders;
    destination.collisions = request.destination.collisions;
    destination.write_report_files = request.write_report_files;
    destination.storage = prepared.plan.storage;
    destination.temp_tag = core::utf8_to_wide_lossy(detail::random_hex(4));
    if (mode == PrepareMode::Submit) {
        if (Status writable = detail::probe_writable(folder, destination.temp_tag); !writable) {
            Error error = std::move(writable.error());
            error.where = std::string(detail::kWhereDestination);
            return Unexpected(std::move(error));
        }
        auto identity = core::stat_file(folder);
        if (identity) {
            core::FileIdentity object;
            object.volume_serial = identity->volume_serial;
            object.file_id = identity->file_id;
            destination.folder_identity = object;
        }
    }

    prepared.output.settings = request.output;
    prepared.output.artist = request.artist;
    prepared.output.copyright = request.copyright;
    prepared.output.allow_unsupported = request.allow_unsupported;
    auto export_json = detail::write_export_document(prepared.output);
    if (!export_json) {
        return Unexpected(std::move(export_json.error()));
    }
    prepared.export_json = std::move(*export_json);
    auto export_sha = core::sha256(std::as_bytes(std::span(prepared.export_json.data(), prepared.export_json.size())));
    if (!export_sha) {
        return Unexpected(std::move(export_sha.error()));
    }
    prepared.export_sha256 = *export_sha;
    prepared.policy.existing_outputs = request.existing_outputs;
    prepared.policy.retry = s.options.retry;
    prepared.policy.single_export = request.single_export;

    // ---- Selection ---------------------------------------------------------------------------
    std::vector<Selected> selected;
    std::unordered_set<std::uint64_t> seen;
    for (const BatchItem& item : request.items) {
        if (seen.insert(item.item.value).second) {
            selected.push_back(Selected{item.item, item.recipe});
        }
    }
    std::filesystem::path selection_root;
    if (request.folder) {
        std::filesystem::path input = request.folder->path;
        if (mode == PrepareMode::Submit) {
            if (!s.options.registry) {
                return fail(ErrorCode::InvalidArgument, "folder batches need a decoder registry",
                            std::string(kWhereSelection));
            }
            ImportRequest import;
            import.folders.push_back(ImportFolder{input, request.folder->include_subfolders});
            import.lucent_directories = s.options.lucent_directories;
            import.output_directories.push_back(folder);
            auto imported = import_sources(
                s.library, *s.options.registry, import,
                [&](const ImportProgress& p) {
                    report(BatchSubmitProgress::Phase::Importing, p.probed, p.candidates);
                },
                cancel);
            if (!imported) {
                return Unexpected(std::move(imported.error()));
            }
            if (imported->cancelled) {
                return fail(ErrorCode::Cancelled, "the import of the folder was cancelled", std::string(kWhereSelection));
            }
        }
        auto canonical = detail::canonical_path(input, kWhereSelection);
        if (!canonical) {
            return Unexpected(std::move(canonical.error()));
        }
        selection_root = *canonical;
        std::vector<SourceItem> inside;
        const std::wstring root_key = folded_path_key(selection_root);
        for (SourceItem& item : s.library.items()) {
            const bool within = request.folder->include_subfolders
                                    ? detail::is_same_or_within(item.path.parent_path(), selection_root)
                                    : folded_path_key(item.path.parent_path()) == root_key;
            if (within) {
                inside.push_back(std::move(item));
            }
        }
        std::sort(inside.begin(), inside.end(), [](const SourceItem& a, const SourceItem& b) {
            const std::wstring ka = folded_path_key(a.path);
            const std::wstring kb = folded_path_key(b.path);
            return ka != kb ? ka < kb : a.subimage < b.subimage;
        });
        for (const SourceItem& item : inside) {
            if (seen.insert(item.id.value).second) {
                selected.push_back(Selected{item.id, std::nullopt});
            }
        }
        prepared.selection = "the folder " + core::path_to_utf8_lossy(selection_root);
    }
    if (selected.empty()) {
        return fail(ErrorCode::InvalidArgument, "no photographs were selected", std::string(kWhereEmpty));
    }
    if (prepared.selection.empty()) {
        prepared.selection = std::format("{} selected photo{}", selected.size(), selected.size() == 1 ? "" : "s");
    }

    // ---- Items, recipes, hashes --------------------------------------------------------------
    const auto as_shot = write_recipe(engine::DevelopSettings{});
    if (!as_shot) {
        return Unexpected(as_shot.error());
    }
    std::vector<std::pair<SourceItem, FrozenRecipe>> candidates;
    std::vector<ItemId> unhashed;
    for (std::size_t i = 0; i < selected.size(); ++i) {
        if (Status c = cancel.check(kWhere); !c) {
            return Unexpected(std::move(c.error()));
        }
        report(BatchSubmitProgress::Phase::Planning, i, selected.size());
        const Selected& choice = selected[i];
        std::optional<SourceItem> item = s.library.find(choice.item);
        if (!item) {
            add_exclusion(prepared, choice.item, {},
                          Error{ErrorCode::NotFound, std::format("no library item {}", choice.item.value),
                                std::string(kWhereSelection), 0});
            continue;
        }
        if (item->support.status == SupportStatus::Unsupported && !request.allow_unsupported) {
            std::string reasons;
            for (const SupportReason& reason : item->support.reasons) {
                reasons += reasons.empty() ? reason.message : "; " + reason.message;
            }
            add_exclusion(prepared, item->id, item->path,
                          Error{ErrorCode::Unsupported,
                                std::format("{} is not supported: {}", quoted(item->path), reasons),
                                "app.batch.support", 0});
            continue;
        }
        if (item->source_state == SourceState::Missing) {
            add_exclusion(prepared, item->id, item->path,
                          Error{ErrorCode::NotFound, std::format("{} is missing", quoted(item->path)),
                                std::string(detail::kWhereSourceMissing), 0});
            continue;
        }
        if (item->source_state == SourceState::Changed) {
            add_exclusion(prepared, item->id, item->path,
                          Error{ErrorCode::SourceChanged,
                                std::format("{} changed on disk after it was added", quoted(item->path)),
                                std::string(detail::kWhereSourceChanged), 0});
            continue;
        }
        FrozenRecipe frozen;
        if (choice.recipe) {
            frozen = *choice.recipe;
            if (Status valid = frozen.settings.validate(); !valid) {
                return Unexpected(std::move(valid.error()));
            }
            if (!frozen.revision) {
                auto document = write_recipe(frozen.settings);
                if (!document || document->sha256 != as_shot->sha256) {
                    return fail(ErrorCode::InvalidArgument,
                                "frozen settings without a recipe revision must be the as-shot settings",
                                std::string(detail::kWhereRecipe));
                }
            }
        } else {
            auto current = s.recipes.current_settings(item->id);
            if (!current) {
                add_exclusion(prepared, item->id, item->path, current.error());
                continue;
            }
            frozen.settings = std::move(current->settings);
            frozen.revision = current->revision;
            frozen.revision_number = current->revision_number;
        }
        if (!item->sha256) {
            unhashed.push_back(item->id);
        }
        candidates.emplace_back(std::move(*item), std::move(frozen));
    }
    if (!unhashed.empty()) {
        prepared.plan.to_hash = unhashed.size();
        if (mode == PrepareMode::Submit) {
            report(BatchSubmitProgress::Phase::Hashing, 0, unhashed.size());
            auto hashed = hash_sources(
                s.library, unhashed, {},
                [&](const MaintenanceProgress& p) {
                    report(BatchSubmitProgress::Phase::Hashing, p.bytes_done, p.bytes_total);
                },
                cancel);
            if (!hashed) {
                return Unexpected(std::move(hashed.error()));
            }
            if (hashed->cancelled) {
                return fail(ErrorCode::Cancelled, "hashing the sources was cancelled", std::string(kWhere));
            }
            for (auto& [item, frozen] : candidates) {
                if (!item.sha256) {
                    if (std::optional<SourceItem> fresh = s.library.find(item.id)) {
                        item = std::move(*fresh);
                    }
                }
            }
        }
    }

    // ---- Names, existing outputs, sizes ------------------------------------------------------
    destination.mirror_root = request.destination.mirror_root.value_or(std::filesystem::path{});
    if (destination.mirror_subfolders && destination.mirror_root.empty()) {
        if (!selection_root.empty()) {
            destination.mirror_root = selection_root;
        } else {
            std::vector<std::filesystem::path> sources;
            for (const auto& [item, frozen] : candidates) {
                sources.push_back(item.path);
            }
            destination.mirror_root = common_parent_folder(sources);
        }
    } else if (destination.mirror_subfolders) {
        if (auto canonical = detail::canonical_path(destination.mirror_root, kWhereSelection)) {
            destination.mirror_root = std::move(*canonical);
        }
    }
    std::map<std::wstring, std::unordered_set<std::wstring>> existing;  // folded dir -> folded names
    std::map<std::wstring, std::unordered_set<std::wstring>> planned;
    std::vector<PlannedOutput> written_examples;  // outputs that will be written, first
    std::vector<PlannedOutput> skipped_examples;  // then already exported ones
    const exporter::BitDepth depth = exporter::bit_depth_for(request.output.format);
    const std::uint64_t bytes_per_pixel = depth == exporter::BitDepth::Sixteen ? 6 : 3;
    for (std::size_t ordinal = 0; ordinal < candidates.size(); ++ordinal) {
        auto& [item, frozen] = candidates[ordinal];
        if (mode == PrepareMode::Submit && !item.sha256) {
            add_exclusion(prepared, item.id, item.path,
                          Error{ErrorCode::SourceChanged,
                                std::format("{} could not be read to compute its fingerprint (changed or unreadable)",
                                            quoted(item.path)),
                                std::string(detail::kWhereSourceChanged), 0});
            continue;
        }
        PreparedJob job;
        job.final_directory = output_directory_for(folder, destination.mirror_subfolders, destination.mirror_root, item.path);
        const std::wstring dir_key = folded_path_key(job.final_directory);
        auto listed = existing.find(dir_key);
        if (listed == existing.end()) {
            std::unordered_set<std::wstring> names;
            if (auto entries = detail::list_directory(job.final_directory, detail::kWhereDestination)) {
                for (const detail::DirectoryEntry& entry : *entries) {
                    names.insert(detail::fold_case(entry.name));
                }
            }
            listed = existing.emplace(dir_key, std::move(names)).first;
        }
        std::unordered_set<std::wstring>& batch_names = planned[dir_key];
        OutputNameFields fields{item.path, frozen.revision ? frozen.revision_number : 0, item.subimage,
                                item.subimage_count, static_cast<std::uint32_t>(prepared.jobs.size())};
        std::uint32_t index = 1;
        std::filesystem::path name;
        for (;; ++index) {
            auto expanded = expand_output_name(*pattern, fields, request.output.format, index);
            if (!expanded) {
                return Unexpected(std::move(expanded.error()));
            }
            const std::wstring key = detail::fold_case(expanded->native());
            if (batch_names.contains(key)) {
                continue;
            }
            if (listed->second.contains(key)) {
                if (request.destination.collisions == CollisionPolicy::UniqueName) {
                    continue;
                }
                // Overwrite: never an original photograph of the library.
                if (s.library.find_source(job.final_directory / *expanded, core::FileIdentity{}, 0)) {
                    continue;
                }
                job.replaces = true;
            }
            name = std::move(*expanded);
            batch_names.insert(key);
            break;
        }
        job.destination = job.final_directory / name;
        if (job.destination.native().size() > kMaxOutputPathChars) {
            add_exclusion(prepared, item.id, item.path,
                          Error{ErrorCode::LimitExceeded,
                                std::format("the output path would be longer than {} characters", kMaxOutputPathChars),
                                std::string(detail::kWhereDestination), 0});
            batch_names.erase(detail::fold_case(name.native()));
            continue;
        }
        job.renamed = index > 1;
        // An identical output already in this folder (ExistingOutputs::Skip).
        if (item.sha256 && request.existing_outputs == ExistingOutputs::Skip) {
            auto settings_doc = write_recipe(frozen.settings);
            if (settings_doc) {
                catalog::JobIdentityInput input;
                input.source_sha256 = *item.sha256;
                input.subimage = item.subimage;
                input.settings_sha256 = settings_doc->sha256;
                input.renderer_version = s.renderer_version;
                input.assets_json = "{}";
                input.export_settings_sha256 = prepared.export_sha256;
                if (auto identity = catalog::compute_job_identity(input)) {
                    auto receipts = s.catalog->query(
                        [&](catalog::ReadTransaction& tx) { return tx.find_receipts_by_identity(*identity); });
                    if (receipts) {
                        for (const catalog::ReceiptRecord& receipt : *receipts) {
                            if (folded_path_key(receipt.final_path.parent_path()) != dir_key) {
                                continue;
                            }
                            // A plan only looks at the file's size (re-hashing every earlier output
                            // whenever the dialog changes would read gigabytes); submit() confirms it
                            // byte for byte, cancellably.
                            const bool present =
                                mode == PrepareMode::Plan
                                    ? output_present(receipt.final_path, receipt.size_bytes)
                                    : detail::check_file(receipt.final_path, receipt.size_bytes, receipt.output_sha256,
                                                         cancel) == detail::FileCheck::Match;
                            if (present) {
                                job.existing_output = receipt.final_path;
                                break;
                            }
                        }
                        if (Status c = cancel.check(kWhere); !c) {
                            return Unexpected(std::move(c.error()));
                        }
                    }
                }
            }
        }
        if (job.existing_output) {
            ++prepared.plan.already_exported;
            batch_names.erase(detail::fold_case(name.native()));
            job.destination = *job.existing_output;
            job.renamed = false;
            job.replaces = false;
        } else {
            ++prepared.plan.outputs;
            prepared.plan.renamed += job.renamed ? 1 : 0;
            prepared.plan.replaced += job.replaces ? 1 : 0;
            const std::uint64_t pixels = detail::output_pixels(item);
            const std::uint64_t bytes = (pixels == 0 ? std::uint64_t{50'000'000} : pixels) * bytes_per_pixel +
                                        kFileOverheadBytes;
            prepared.plan.estimated_output_bytes += bytes;
            prepared.plan.largest_output_bytes = std::max(prepared.plan.largest_output_bytes, bytes);
        }
        if (!job.existing_output && written_examples.size() < kPlanExamples) {
            written_examples.push_back(PlannedOutput{item.id, item.path, job.destination, job.renamed, job.replaces, false});
        } else if (job.existing_output && skipped_examples.size() < kPlanExamples) {
            skipped_examples.push_back(PlannedOutput{item.id, item.path, job.destination, false, false, true});
        }
        if (auto doc = write_recipe(frozen.settings)) {
            job.settings_sha256 = doc->sha256;
        } else {
            return Unexpected(std::move(doc.error()));
        }
        job.item = std::move(item);
        job.frozen = std::move(frozen);
        prepared.jobs.push_back(std::move(job));
    }
    prepared.plan.examples = std::move(written_examples);
    for (PlannedOutput& example : skipped_examples) {
        if (prepared.plan.examples.size() >= kPlanExamples) {
            break;
        }
        prepared.plan.examples.push_back(std::move(example));
    }
    if (folder_exists) {
        prepared.plan.free_bytes = s.free_space(folder);
        if (prepared.plan.free_bytes && prepared.plan.outputs > 0 &&
            *prepared.plan.free_bytes < prepared.plan.largest_output_bytes + kDestinationReserveBytes) {
            prepared.plan.notices.push_back(PlanNotice::TooFullForOne);
        } else if (prepared.plan.free_bytes && *prepared.plan.free_bytes < prepared.plan.estimated_output_bytes) {
            prepared.plan.notices.push_back(PlanNotice::MayRunOutOfSpace);
        }
        if (prepared.plan.free_bytes && *prepared.plan.free_bytes < prepared.plan.estimated_output_bytes) {
            prepared.plan.warnings.push_back(std::format(
                "the destination has {} MB free; the outputs may need up to {} MB (uncompressed bound)",
                *prepared.plan.free_bytes >> 20, prepared.plan.estimated_output_bytes >> 20));
        }
    }
    prepared.policy.excluded = prepared.plan.excluded;
    prepared.policy.excluded_count = prepared.plan.excluded_count;
    prepared.name = request.name;
    if (prepared.name.empty()) {
        prepared.name = prepared.jobs.size() == 1
                            ? "Export of " + core::path_to_utf8_lossy(prepared.jobs.front().item.path.filename())
                            : std::format("Export of {} photos", prepared.jobs.size());
    }
    return prepared;
}

Expected<BatchPlan> BatchService::plan(const BatchRequest& request, const core::CancellationToken& cancel) const {
    auto prepared = prepare_batch(*impl_, request, PrepareMode::Plan, {}, cancel);
    if (!prepared) {
        return Unexpected(std::move(prepared.error()));
    }
    return std::move(prepared->plan);
}

Expected<SubmittedBatch> BatchService::submit(const BatchRequest& request, const BatchSubmitProgressCallback& progress,
                                              const core::CancellationToken& cancel) {
    Impl& s = *impl_;
    auto prepared = prepare_batch(s, request, PrepareMode::Submit, progress, cancel);
    if (!prepared) {
        return Unexpected(std::move(prepared.error()));
    }
    if (prepared->jobs.empty()) {
        std::string reasons;
        for (const PlanExclusion& exclusion : prepared->plan.excluded) {
            if (reasons.size() > 400) {
                reasons += "; ...";
                break;
            }
            reasons += reasons.empty() ? exclusion.reason.message : "; " + exclusion.reason.message;
        }
        return fail(ErrorCode::InvalidArgument, "nothing in the selection can be exported: " + reasons,
                    std::string(kWhereEmpty));
    }
    if (prepared->plan.outputs > 0 && prepared->plan.free_bytes &&
        *prepared->plan.free_bytes < prepared->plan.largest_output_bytes + kDestinationReserveBytes) {
        return fail(ErrorCode::DiskFull,
                    std::format("the destination has {} MB free; one output may need up to {} MB",
                                *prepared->plan.free_bytes >> 20, prepared->plan.largest_output_bytes >> 20),
                    std::string(detail::kWhereDestination));
    }
    if (progress) {
        progress(BatchSubmitProgress{BatchSubmitProgress::Phase::Writing, 0, prepared->jobs.size()});
    }
    catalog::NewBatch batch;
    batch.name = core::sanitize_utf8(prepared->name);
    if (batch.name.size() > catalog::kMaxBatchTextBytes) {
        batch.name.resize(catalog::kMaxBatchTextBytes);
        batch.name = core::sanitize_utf8(batch.name);
    }
    batch.selection_description = core::sanitize_utf8(prepared->selection);
    if (batch.selection_description.size() > catalog::kMaxBatchTextBytes) {
        batch.selection_description.resize(catalog::kMaxBatchTextBytes);
        batch.selection_description = core::sanitize_utf8(batch.selection_description);
    }
    auto destination_json = detail::write_destination(prepared->destination);
    if (!destination_json) {
        return Unexpected(std::move(destination_json.error()));
    }
    auto policy_json = detail::write_policy(prepared->policy);
    if (!policy_json) {
        return Unexpected(std::move(policy_json.error()));
    }
    batch.destination_json = std::move(*destination_json);
    batch.export_settings_json = prepared->export_json;
    batch.policy_json = std::move(*policy_json);
    batch.renderer_version = s.renderer_version;
    batch.created_at = s.now();
    batch.start = request.start;
    std::vector<catalog::NewJob> jobs;
    jobs.reserve(prepared->jobs.size());
    for (const PreparedJob& job : prepared->jobs) {
        catalog::NewJob made;
        made.item = to_catalog(job.item.id);
        made.revision = job.frozen.revision;
        made.settings_sha256 = job.settings_sha256;
        jobs.push_back(std::move(made));
    }
    std::uint64_t skipped = 0;
    auto created = s.catalog->transact([&](catalog::WriteTransaction& tx) -> Expected<catalog::CreatedBatch> {
        auto made = tx.create_batch(batch, jobs);
        if (!made) {
            return made;
        }
        for (std::size_t i = 0; i < prepared->jobs.size(); ++i) {
            const PreparedJob& job = prepared->jobs[i];
            if (!job.existing_output) {
                continue;
            }
            const Error reason{ErrorCode::AlreadyExists,
                               std::format("an identical output already exists at {}", quoted(*job.existing_output)),
                               std::string(detail::kWhereExisting), 0};
            if (Status skip = tx.skip_job(made->jobs[i], reason, batch.created_at, job.existing_output); !skip) {
                return Unexpected(std::move(skip.error()));
            }
            ++skipped;
        }
        return made;
    });
    if (!created) {
        return Unexpected(std::move(created.error()));
    }
    SubmittedBatch result;
    result.id = created->id;
    result.plan = std::move(prepared->plan);
    result.jobs = created->jobs.size();
    result.skipped_existing = skipped;
    core::log_info(kWhere, "batch {} created: {} jobs ({} already exported), {} excluded, into {}", result.id.value,
                   result.jobs, skipped, result.plan.excluded_count, core::path_to_utf8_lossy(result.plan.folder));
    s.note_active(result.id);
    s.emit_state(result.id, BatchEventKind::Created);
    s.check_finished(result.id);
    s.request_wake();
    return result;
}

// ---- Commands ----------------------------------------------------------------------------------

Status BatchService::start(catalog::BatchId batch) {
    Impl& s = *impl_;
    auto state = s.catalog->transact([&](catalog::WriteTransaction& tx) { return tx.start_batch(batch, s.now()); });
    if (!state) {
        return Unexpected(std::move(state.error()));
    }
    s.note_active(batch);
    s.emit_state(batch, BatchEventKind::StateChanged);
    s.check_finished(batch);
    s.request_wake();
    return ok();
}

Status BatchService::pause(catalog::BatchId batch) {
    Impl& s = *impl_;
    auto state = s.catalog->transact([&](catalog::WriteTransaction& tx) { return tx.pause_batch(batch, s.now()); });
    if (!state) {
        return Unexpected(std::move(state.error()));
    }
    s.emit_state(batch, BatchEventKind::StateChanged);
    return ok();
}

Status BatchService::resume(catalog::BatchId batch) {
    Impl& s = *impl_;
    if (auto context = s.context_for(batch)) {
        std::lock_guard lock(s.mutex);
        (*context)->attention = BatchAttention::None;
        (*context)->attention_error.reset();
        (*context)->destination_checked = {};
    }
    auto state = s.catalog->transact([&](catalog::WriteTransaction& tx) { return tx.resume_batch(batch, s.now()); });
    if (!state) {
        return Unexpected(std::move(state.error()));
    }
    s.note_active(batch);
    s.emit_state(batch, BatchEventKind::StateChanged);
    s.request_reclaim();  // jobs left leased while the folder was out of reach
    return ok();
}

Status BatchService::use_replaced_folder(catalog::BatchId batch) {
    Impl& s = *impl_;
    auto record = s.batch_record(batch);
    if (!record) {
        return Unexpected(std::move(record.error()));
    }
    if (is_finished(*record)) {
        return fail(ErrorCode::InvalidArgument, std::format("batch {} has finished", batch.value),
                    "catalog.batch.invalid_transition");
    }
    auto context = s.context_for(batch);
    if (!context) {
        return Unexpected(std::move(context.error()));
    }
    const std::filesystem::path& folder = (*context)->destination.folder;
    auto identity = core::stat_file(folder);
    if (!identity || !detail::is_directory(folder)) {
        return fail(ErrorCode::NotFound, std::format("there is no folder at {}", quoted(folder)),
                    std::string(detail::kWhereDestination));
    }
    detail::DestinationDocument document = (*context)->destination;
    core::FileIdentity object;
    object.volume_serial = identity->volume_serial;
    object.file_id = identity->file_id;
    document.folder_identity = object;
    auto json = detail::write_destination(document);
    if (!json) {
        return Unexpected(std::move(json.error()));
    }
    if (Status stored = s.catalog->write(
            [&](catalog::WriteTransaction& tx) { return tx.set_batch_destination(batch, *json, s.now()); });
        !stored) {
        return stored;
    }
    {
        std::lock_guard lock(s.mutex);
        (*context)->folder_identity = object;
    }
    core::log_info(kWhere, "batch {} continues in the folder now at {}", batch.value,
                   core::path_to_utf8_lossy(folder));
    if (record->state == catalog::BatchState::Paused) {
        return resume(batch);
    }
    {
        std::lock_guard lock(s.mutex);
        (*context)->attention = BatchAttention::None;
        (*context)->attention_error.reset();
        (*context)->destination_checked = {};
    }
    s.emit_state(batch, BatchEventKind::StateChanged);
    s.request_reclaim();
    return ok();
}

Status BatchService::cancel(catalog::BatchId batch, const std::optional<Error>& reason) {
    Impl& s = *impl_;
    auto state = s.catalog->transact([&](catalog::WriteTransaction& tx) {
        return reason ? tx.cancel_batch_with_reason(batch, s.now(), *reason) : tx.cancel_batch(batch, s.now());
    });
    if (!state) {
        return Unexpected(std::move(state.error()));
    }
    for (detail::Slot* slot : s.busy_slots_of(batch)) {
        std::lock_guard lock(s.mutex);
        if (slot->busy && slot->batch == batch && slot->stop == detail::StopReason::None) {
            slot->stop = detail::StopReason::BatchCancelled;
            slot->cancel.cancel();
        }
    }
    s.emit_state(batch, BatchEventKind::StateChanged);
    s.check_finished(batch);
    return ok();
}

Expected<std::uint64_t> BatchService::retry_failed(catalog::BatchId batch) {
    Impl& s = *impl_;
    auto count = s.catalog->transact([&](catalog::WriteTransaction& tx) { return tx.retry_failed_jobs(batch, s.now()); });
    if (!count) {
        return Unexpected(std::move(count.error()));
    }
    s.note_active(batch);
    s.emit_state(batch, BatchEventKind::StateChanged);
    s.check_finished(batch);
    s.request_wake();
    return *count;
}

// ---- Status --------------------------------------------------------------------------------------

namespace {

constexpr std::string_view kReviewJsonPath = "$.review";  // ReceiptDetails::review in a receipt

BatchStatus make_status(BatchService::Impl& s, catalog::BatchRecord record) {
    BatchStatus status;
    auto context = s.context_for(record.id);
    std::optional<catalog::BatchJobFacts> facts;
    if (context) {
        status.folder = (*context)->destination.folder;
        status.single_export = (*context)->policy.single_export;
        status.excluded = (*context)->policy.excluded_count;
    }
    std::uint32_t active = 0;
    {
        std::lock_guard lock(s.mutex);
        if (context) {
            status.attention = (*context)->attention;
            status.attention_error = (*context)->attention_error;
            status.attention_free_bytes = (*context)->attention_free_bytes;
            status.attention_needed_bytes = (*context)->attention_needed_bytes;
            if ((*context)->facts_counts == record.counts) {
                facts = (*context)->facts;
            }
        }
        for (const auto& slot : s.slots) {
            if (slot->busy && slot->batch == record.id) {
                status.running.push_back(slot->running);
                ++active;
            }
        }
        // An estimate only while the batch runs: a paused or stalled one has no time left to tell.
        if (context && (*context)->completed_this_session >= 3 && record.state == catalog::BatchState::Running &&
            (*context)->attention == BatchAttention::None && record.counts.pending() > 0) {
            const double per_job = (*context)->seconds_this_session / static_cast<double>((*context)->completed_this_session);
            const double workers = static_cast<double>(std::max<std::uint32_t>(1, active));
            status.eta_seconds = per_job * static_cast<double>(record.counts.pending()) / workers;
        }
    }
    // Review and retry counts read from the catalog (cached while the counts do not move).
    if (!facts) {
        auto read = s.catalog->query(
            [&](catalog::ReadTransaction& tx) { return tx.batch_job_facts(record.id, kReviewJsonPath); });
        if (read) {
            facts = *read;
            if (context) {
                std::lock_guard lock(s.mutex);
                (*context)->facts_counts = record.counts;
                (*context)->facts = *read;
            }
        }
    }
    if (facts) {
        status.review_recommended = facts->flagged_receipts;
        status.retryable = facts->failed_retryable;
    }
    // A batch of one photograph: its output or why there is none.
    if (record.job_count == 1 && record.counts.pending() == 0) {
        catalog::JobQuery query;
        query.batch = record.id;
        query.limit = 1;
        auto jobs = s.catalog->query([&](catalog::ReadTransaction& tx) { return tx.list_jobs(query); });
        if (jobs && !jobs->empty()) {
            const catalog::JobRecord& job = jobs->front();
            if (job.stage == catalog::JobStage::Complete) {
                auto receipt = s.catalog->query([&](catalog::ReadTransaction& tx) { return tx.find_receipt(job.id); });
                if (receipt && *receipt) {
                    status.output = (*receipt)->final_path;
                }
            } else {
                if (job.stage == catalog::JobStage::Skipped && job.final_path) {
                    status.output = job.final_path;  // the existing output it was skipped for
                }
                if (job.last_error) {
                    status.outcome_error = Error{job.last_error->code, job.last_error->message, job.last_error->where,
                                                 job.last_error->native_code};
                    status.outcome_reason = detail::describe_failure(*status.outcome_error).reason;
                }
            }
        }
    }
    status.record = std::move(record);
    return status;
}

}  // namespace

Expected<BatchStatus> BatchService::status(catalog::BatchId batch) const {
    auto record = impl_->batch_record(batch);
    if (!record) {
        return Unexpected(std::move(record.error()));
    }
    return make_status(*impl_, std::move(*record));
}

Expected<std::vector<BatchStatus>> BatchService::list(std::uint32_t limit, catalog::BatchId before) const {
    auto records = impl_->catalog->query([&](catalog::ReadTransaction& tx) { return tx.list_batches(limit, before); });
    if (!records) {
        return Unexpected(std::move(records.error()));
    }
    std::vector<BatchStatus> out;
    out.reserve(records->size());
    for (catalog::BatchRecord& record : *records) {
        out.push_back(make_status(*impl_, std::move(record)));
    }
    return out;
}

Expected<std::vector<BatchStatus>> BatchService::list_unfinished() const {
    std::vector<BatchStatus> out;
    auto records =
        impl_->catalog->query([&](catalog::ReadTransaction& tx) { return tx.list_unfinished_batches(kPageSize); });
    if (!records) {
        return Unexpected(std::move(records.error()));
    }
    out.reserve(records->size());
    for (catalog::BatchRecord& record : *records) {
        out.push_back(make_status(*impl_, std::move(record)));
    }
    return out;
}

Expected<BatchReport> BatchService::report(catalog::BatchId batch) const { return impl_->build_report(batch); }

Expected<ReportFiles> BatchService::write_report(catalog::BatchId batch) { return impl_->write_report_files(batch); }

Expected<std::filesystem::path> BatchService::output_folder(catalog::BatchId batch) const {
    auto context = impl_->context_for(batch);
    if (!context) {
        return Unexpected(std::move(context.error()));
    }
    return (*context)->destination.folder;
}

Subscription BatchService::subscribe(BatchObserver observer) { return impl_->observers.add(std::move(observer)); }

BatchSchedulerStats BatchService::stats() const {
    std::lock_guard lock(impl_->mutex);
    BatchSchedulerStats stats = impl_->counters;
    stats.calibrated = impl_->calibrated;
    stats.bytes_per_pixel = impl_->calibrated && impl_->measured_bytes_per_pixel > 0.0
                                ? impl_->measured_bytes_per_pixel * impl_->options.memory.measured_margin
                                : impl_->options.memory.default_bytes_per_pixel;
    stats.active_workers = 0;
    for (const auto& slot : impl_->slots) {
        if (slot->busy) {
            ++stats.active_workers;
        }
        if (slot->pid != 0) {
            stats.worker_pids.push_back(slot->pid);
        }
    }
    return stats;
}

RecoveryReport BatchService::last_recovery() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->recovery;
}

bool BatchService::wait_for_recovery(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->mutex);
    return impl_->recovered_cv.wait_for(lock, timeout, [&] { return impl_->recovered; });
}

void BatchService::wake() noexcept { impl_->request_wake(); }

}  // namespace lucent::app
