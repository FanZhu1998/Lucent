// The durable batch journal (schema v3; jobs.hpp, catalog.hpp): batches, jobs, leases, stage
// transitions with their events, the publication journal and receipts. Every value reaches
// SQLite as a bound parameter; SQL text is assembled only from the fixed fragments below.
//
// Every write runs inside Catalog::write() (one writer connection, BEGIN IMMEDIATE), and each call
// that changes more than one row does so inside its own savepoint, so a call is atomic and a
// failed call leaves no partial change behind.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lucent/catalog/catalog.hpp"
#include "lucent/catalog/jobs.hpp"
#include "lucent/core/hash.hpp"
#include "lucent/core/text.hpp"

#include "codec.hpp"
#include "repository_support.hpp"
#include "sqlite.hpp"

namespace lucent::catalog {

// ---------------------------------------------------------------------------------------------
// Spellings
// ---------------------------------------------------------------------------------------------

std::string_view to_string(BatchState value) noexcept {
    switch (value) {
        case BatchState::Draft:
            return "draft";
        case BatchState::Running:
            return "running";
        case BatchState::Paused:
            return "paused";
        case BatchState::Cancelled:
            return "cancelled";
        case BatchState::Completed:
            return "completed";
    }
    return "invalid";
}

std::string_view to_string(JobStage value) noexcept {
    switch (value) {
        case JobStage::Queued:
            return "queued";
        case JobStage::Rendering:
            return "rendering";
        case JobStage::Verifying:
            return "verifying";
        case JobStage::Publishing:
            return "publishing";
        case JobStage::Complete:
            return "complete";
        case JobStage::Failed:
            return "failed";
        case JobStage::Skipped:
            return "skipped";
        case JobStage::Cancelled:
            return "cancelled";
    }
    return "invalid";
}

std::string_view to_string(FailureClass value) noexcept {
    switch (value) {
        case FailureClass::Transient:
            return "transient";
        case FailureClass::Deterministic:
            return "deterministic";
    }
    return "invalid";
}

std::string_view to_string(PublicationStep value) noexcept {
    switch (value) {
        case PublicationStep::Reserved:
            return "reserved";
        case PublicationStep::TempWritten:
            return "temp_written";
        case PublicationStep::Verified:
            return "verified";
        case PublicationStep::Published:
            return "published";
        case PublicationStep::ReceiptCommitted:
            return "receipt_committed";
        case PublicationStep::Abandoned:
            return "abandoned";
    }
    return "invalid";
}

namespace detail {

namespace {
template <class Enum, std::uint8_t First, std::uint8_t Last>
std::optional<Enum> parse_enum(std::string_view text) noexcept {
    for (std::uint8_t value = First; value <= Last; ++value) {
        if (to_string(static_cast<Enum>(value)) == text) {
            return static_cast<Enum>(value);
        }
    }
    return std::nullopt;
}
}  // namespace

std::optional<BatchState> parse_batch_state(std::string_view text) noexcept {
    return parse_enum<BatchState, 1, 5>(text);
}
std::optional<JobStage> parse_job_stage(std::string_view text) noexcept {
    return parse_enum<JobStage, 1, 8>(text);
}
std::optional<FailureClass> parse_failure_class(std::string_view text) noexcept {
    return parse_enum<FailureClass, 1, 2>(text);
}
std::optional<PublicationStep> parse_publication_step(std::string_view text) noexcept {
    return parse_enum<PublicationStep, 1, 6>(text);
}

ErrorCode error_code_from_db(std::int64_t value) noexcept {
    // Every code this build knows; a code appended by a newer Lucent reads as Unknown.
    if (value >= static_cast<std::int64_t>(ErrorCode::Unknown) &&
        value <= static_cast<std::int64_t>(ErrorCode::SourceChanged)) {
        return static_cast<ErrorCode>(value);
    }
    return ErrorCode::Unknown;
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------------------------

namespace {

using detail::Connection;
using detail::Parameter;
using detail::RowDecoder;
using detail::Statement;
using detail::TransactionGuard;

constexpr std::string_view kIdentityDomain = "lucent.job-identity";
constexpr std::uint32_t kIdentityVersion = 1;

void append_le(std::string& out, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

void append_text(std::string& out, std::string_view text) {
    append_le(out, text.size(), 8);
    out.append(text);
}

void append_digest(std::string& out, const core::Digest256& digest) {
    out.append(reinterpret_cast<const char*>(digest.data()), digest.size());
}

Status check_size(std::string_view text, std::size_t limit, std::string_view field, std::string_view where) {
    if (text.size() > limit) {
        return fail(ErrorCode::LimitExceeded, std::format("{} exceeds {} bytes", field, limit), std::string(where));
    }
    return ok();
}

}  // namespace

Expected<core::Digest256> compute_job_identity(const JobIdentityInput& input) {
    std::string message;
    message.reserve(kIdentityDomain.size() + 1 + 4 + 32 * 3 + 4 + 16 + input.renderer_version.size() +
                    input.assets_json.size());
    append_text(message, kIdentityDomain);
    append_le(message, kIdentityVersion, 4);
    append_digest(message, input.source_sha256);
    append_le(message, input.subimage, 4);
    append_digest(message, input.settings_sha256);
    append_text(message, input.renderer_version);
    append_text(message, input.assets_json);
    append_digest(message, input.export_settings_sha256);
    return detail::sha256_of(message);
}

Status NewBatch::validate() const {
    constexpr std::string_view where = "catalog.batch";
    for (const auto& [text, field] : {std::pair<std::string_view, std::string_view>{name, "name"},
                                      {selection_description, "selection_description"},
                                      {destination_json, "destination_json"},
                                      {export_settings_json, "export_settings_json"},
                                      {policy_json, "policy_json"},
                                      {renderer_version, "renderer_version"}}) {
        if (Status s = detail::check_text(text, field, where); !s) {
            return s;
        }
    }
    for (const auto& [text, field] : {std::pair<std::string_view, std::string_view>{name, "name"},
                                      {selection_description, "selection_description"}}) {
        if (Status s = check_size(text, kMaxBatchTextBytes, field, where); !s) {
            return s;
        }
    }
    for (const auto& [text, field] :
         {std::pair<std::string_view, std::string_view>{destination_json, "destination_json"},
          {export_settings_json, "export_settings_json"},
          {policy_json, "policy_json"}}) {
        if (Status s = check_size(text, kMaxBatchJsonBytes, field, where); !s) {
            return s;
        }
    }
    if (renderer_version.empty()) {
        return detail::invalid(where, "renderer_version must not be empty");
    }
    return check_size(renderer_version, kMaxJobLabelBytes, "renderer_version", where);
}

// ---------------------------------------------------------------------------------------------
// Row layouts and decoding
// ---------------------------------------------------------------------------------------------

namespace {

using detail::check_active;
using detail::check_limit;
using detail::first;
using detail::invalid;
using detail::sql;

constexpr std::string_view kBatchColumns =
    "b.id, b.name, b.selection_description, b.destination_json, b.export_settings_json, "
    "b.export_settings_sha256, b.policy_json, b.renderer_version, b.state, b.job_count, b.created_at, "
    "b.updated_at, b.started_at, b.finished_at";

constexpr std::string_view kJobColumns =
    "j.id, j.batch_id, j.ordinal, j.item_id, j.subimage, j.revision_id, j.settings_sha256, j.source_sha256, "
    "j.assets_json, j.identity_sha256, j.stage, j.attempts, j.transient_failures, j.deterministic_failures, "
    "j.last_failure_class, j.last_error_code, j.last_error_where, j.last_error_message, j.last_error_native, "
    "j.last_error_at, j.lease_owner, j.lease_expires_at, j.heartbeat_at, j.not_before, j.temp_path, "
    "j.final_path, j.created_at, j.updated_at";

constexpr std::string_view kEventColumns =
    "e.id, e.job_id, e.batch_id, e.at, e.from_stage, e.to_stage, e.attempt, e.lease_owner, e.failure_class, "
    "e.error_code, e.error_where, e.error_message, e.error_native";

constexpr std::string_view kJournalColumns =
    "p.id, p.job_id, p.attempt, p.step, p.at, p.temp_path, p.final_path, p.output_sha256, p.output_size";

constexpr std::string_view kReceiptColumns =
    "r.id, r.job_id, r.identity_sha256, r.final_path, r.size_bytes, r.output_sha256, r.settings_json, "
    "r.created_at";

// The stages of a job that holds a lease, as an SQL list.
constexpr std::string_view kLeasedStagesSql = "('rendering', 'verifying', 'publishing')";

// Decoding helpers on top of RowDecoder. Each returns false and sets `error` on failure, which
// keeps the long record readers below linear.
class Reader {
public:
    Reader(const Statement& row, const RowDecoder& decode) noexcept : row_(row), decode_(decode) {}

    [[nodiscard]] bool failed() const noexcept { return error_.has_value(); }
    [[nodiscard]] Error take_error() { return std::move(*error_); }

    std::string text(int column) { return std::string(row_.column_text(column)); }
    std::int64_t int64(int column) { return row_.column_int64(column); }
    Timestamp time(int column) { return detail::timestamp_from_db(row_.column_int64(column)); }
    std::optional<Timestamp> optional_time(int column) {
        return row_.is_null(column) ? std::nullopt : std::optional<Timestamp>(time(column));
    }
    std::optional<std::string> optional_text(int column) {
        return row_.is_null(column) ? std::nullopt : std::optional<std::string>(text(column));
    }

    std::uint32_t u32(int column, std::string_view name) {
        return keep(decode_.u32(name, row_.column_int64(column)), 0u);
    }
    std::uint64_t u64(int column, std::string_view name) {
        const std::int64_t value = row_.column_int64(column);
        if (value < 0) {
            record(decode_.bad(name, std::to_string(value)));
            return 0;
        }
        return static_cast<std::uint64_t>(value);
    }
    core::Digest256 digest(int column, std::string_view name) {
        return keep(decode_.blob<32>(name, row_.column_blob(column)), core::Digest256{});
    }
    std::optional<core::Digest256> optional_digest(int column, std::string_view name) {
        if (row_.is_null(column)) {
            return std::nullopt;
        }
        return digest(column, name);
    }
    std::filesystem::path path(int column, std::string_view name) {
        return keep(decode_.path(name, row_.column_text(column)), std::filesystem::path());
    }
    std::optional<std::filesystem::path> optional_path(int column, std::string_view name) {
        if (row_.is_null(column)) {
            return std::nullopt;
        }
        return path(column, name);
    }
    template <class T>
    T parsed(std::optional<T> value, int column, std::string_view name, T fallback) {
        if (!value) {
            record(decode_.bad(name, row_.column_text(column)));
            return fallback;
        }
        return *value;
    }
    JobStage stage(int column, std::string_view name) {
        return parsed(detail::parse_job_stage(row_.column_text(column)), column, name, JobStage::Queued);
    }

    // An error record stored in columns class, code, where, message, native (consecutive) and
    // `at` (anywhere); std::nullopt when the code is NULL.
    std::optional<JobError> error(int class_column, int at_column, std::string_view name) {
        if (row_.is_null(class_column + 1)) {
            return std::nullopt;
        }
        JobError error;
        if (!row_.is_null(class_column)) {
            error.failure_class = parsed(detail::parse_failure_class(row_.column_text(class_column)),
                                         class_column, name, FailureClass::Transient);
        }
        error.code = detail::error_code_from_db(row_.column_int64(class_column + 1));
        error.where = text(class_column + 2);
        error.message = text(class_column + 3);
        error.native_code = row_.column_int64(class_column + 4);
        error.at = time(at_column);
        return error;
    }

private:
    template <class T>
    T keep(Expected<T> value, T fallback) {
        if (!value) {
            record(std::move(value.error()));
            return fallback;
        }
        return std::move(*value);
    }
    void record(Error error) {
        if (!error_) {
            error_ = std::move(error);
        }
    }

    const Statement& row_;
    const RowDecoder& decode_;
    std::optional<Error> error_;
};

Expected<BatchRecord> read_batch_row(const Statement& row, const RowDecoder& decode) {
    Reader r(row, decode);
    BatchRecord batch;
    batch.id = BatchId{r.int64(0)};
    batch.name = r.text(1);
    batch.selection_description = r.text(2);
    batch.destination_json = r.text(3);
    batch.export_settings_json = r.text(4);
    batch.export_settings_sha256 = r.digest(5, "batches.export_settings_sha256");
    batch.policy_json = r.text(6);
    batch.renderer_version = r.text(7);
    batch.state = r.parsed(detail::parse_batch_state(row.column_text(8)), 8, "batches.state", BatchState::Draft);
    batch.job_count = r.u64(9, "batches.job_count");
    batch.created_at = r.time(10);
    batch.updated_at = r.time(11);
    batch.started_at = r.optional_time(12);
    batch.finished_at = r.optional_time(13);
    if (r.failed()) {
        return Unexpected(r.take_error());
    }
    return batch;
}

Expected<JobRecord> read_job_row(const Statement& row, const RowDecoder& decode) {
    Reader r(row, decode);
    JobRecord job;
    job.id = JobId{r.int64(0)};
    job.batch = BatchId{r.int64(1)};
    job.ordinal = r.u32(2, "jobs.ordinal");
    if (!row.is_null(3)) {
        job.item = ItemId{r.int64(3)};
    }
    job.subimage = r.u32(4, "jobs.subimage");
    if (!row.is_null(5)) {
        job.revision = RevisionId{r.int64(5)};
    }
    job.settings_sha256 = r.digest(6, "jobs.settings_sha256");
    job.source_sha256 = r.digest(7, "jobs.source_sha256");
    job.assets_json = r.text(8);
    job.identity_sha256 = r.digest(9, "jobs.identity_sha256");
    job.stage = r.stage(10, "jobs.stage");
    job.attempts = r.u32(11, "jobs.attempts");
    job.transient_failures = r.u32(12, "jobs.transient_failures");
    job.deterministic_failures = r.u32(13, "jobs.deterministic_failures");
    job.last_error = r.error(14, 19, "jobs.last_failure_class");
    if (!row.is_null(20)) {
        JobLease lease;
        lease.job = job.id;
        lease.owner = r.text(20);
        lease.attempt = job.attempts;
        lease.expires_at = r.time(21);
        job.lease = std::move(lease);
    }
    job.heartbeat_at = r.optional_time(22);
    job.not_before = r.time(23);
    job.temp_path = r.optional_path(24, "jobs.temp_path");
    job.final_path = r.optional_path(25, "jobs.final_path");
    job.created_at = r.time(26);
    job.updated_at = r.time(27);
    if (r.failed()) {
        return Unexpected(r.take_error());
    }
    return job;
}

Expected<JobEvent> read_event_row(const Statement& row, const RowDecoder& decode) {
    Reader r(row, decode);
    JobEvent event;
    event.id = r.int64(0);
    event.job = JobId{r.int64(1)};
    event.batch = BatchId{r.int64(2)};
    event.at = r.time(3);
    if (!row.is_null(4)) {
        event.from = r.stage(4, "job_events.from_stage");
    }
    event.to = r.stage(5, "job_events.to_stage");
    event.attempt = r.u32(6, "job_events.attempt");
    event.lease_owner = r.optional_text(7);
    event.error = r.error(8, 3, "job_events.failure_class");
    if (r.failed()) {
        return Unexpected(r.take_error());
    }
    return event;
}

Expected<PublicationRecord> read_journal_row(const Statement& row, const RowDecoder& decode) {
    Reader r(row, decode);
    PublicationRecord entry;
    entry.id = r.int64(0);
    entry.job = JobId{r.int64(1)};
    entry.attempt = r.u32(2, "publication_journal.attempt");
    entry.step = r.parsed(detail::parse_publication_step(row.column_text(3)), 3, "publication_journal.step",
                          PublicationStep::Reserved);
    entry.at = r.time(4);
    entry.temp_path = r.optional_path(5, "publication_journal.temp_path");
    entry.final_path = r.optional_path(6, "publication_journal.final_path");
    entry.output_sha256 = r.optional_digest(7, "publication_journal.output_sha256");
    if (!row.is_null(8)) {
        entry.output_size = r.u64(8, "publication_journal.output_size");
    }
    if (r.failed()) {
        return Unexpected(r.take_error());
    }
    return entry;
}

Expected<ReceiptRecord> read_receipt_row(const Statement& row, const RowDecoder& decode) {
    Reader r(row, decode);
    ReceiptRecord receipt;
    receipt.id = ReceiptId{r.int64(0)};
    receipt.job = JobId{r.int64(1)};
    receipt.identity_sha256 = r.digest(2, "receipts.identity_sha256");
    receipt.final_path = r.path(3, "receipts.final_path");
    receipt.size_bytes = r.u64(4, "receipts.size_bytes");
    receipt.output_sha256 = r.digest(5, "receipts.output_sha256");
    receipt.settings_json = r.text(6);
    receipt.created_at = r.time(7);
    if (r.failed()) {
        return Unexpected(r.take_error());
    }
    return receipt;
}

// Per-stage counts of a batch's jobs.
Expected<BatchCounts> batch_counts(Connection& connection, BatchId batch, const RowDecoder& decode,
                                   std::string_view where) {
    Expected<Statement> statement =
        connection.prepare("SELECT stage, count(*) FROM jobs WHERE batch_id = ?1 GROUP BY stage", where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, batch.value);
    BatchCounts counts;
    for (;;) {
        Expected<bool> row = statement->step();
        if (!row) {
            return Unexpected(std::move(row.error()));
        }
        if (!*row) {
            return counts;
        }
        const std::optional<JobStage> stage = detail::parse_job_stage(statement->column_text(0));
        if (!stage) {
            return Unexpected(decode.bad("jobs.stage", statement->column_text(0)));
        }
        const auto n = static_cast<std::uint64_t>(statement->column_int64(1));
        switch (*stage) {
            case JobStage::Queued:
                counts.queued = n;
                break;
            case JobStage::Rendering:
                counts.rendering = n;
                break;
            case JobStage::Verifying:
                counts.verifying = n;
                break;
            case JobStage::Publishing:
                counts.publishing = n;
                break;
            case JobStage::Complete:
                counts.complete = n;
                break;
            case JobStage::Failed:
                counts.failed = n;
                break;
            case JobStage::Skipped:
                counts.skipped = n;
                break;
            case JobStage::Cancelled:
                counts.cancelled = n;
                break;
        }
    }
}

Expected<std::optional<JobRecord>> load_job(Connection& connection, JobId id, bool newer_schema,
                                            std::string_view where) {
    Expected<Statement> statement =
        connection.prepare(sql({"SELECT ", kJobColumns, " FROM jobs j WHERE j.id = ?1"}), where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, id.value);
    return first<JobRecord>(*statement, RowDecoder(newer_schema, where), read_job_row);
}

Expected<std::optional<ReceiptRecord>> load_receipt(Connection& connection, JobId job, bool newer_schema,
                                                    std::string_view where) {
    Expected<Statement> statement =
        connection.prepare(sql({"SELECT ", kReceiptColumns, " FROM receipts r WHERE r.job_id = ?1"}), where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, job.value);
    return first<ReceiptRecord>(*statement, RowDecoder(newer_schema, where), read_receipt_row);
}

// The latest journal entry of a job's attempt.
Expected<std::optional<PublicationRecord>> latest_entry(Connection& connection, JobId job, std::uint32_t attempt,
                                                        std::string_view where) {
    Expected<Statement> statement = connection.prepare(
        sql({"SELECT ", kJournalColumns,
             " FROM publication_journal p WHERE p.job_id = ?1 AND p.attempt = ?2 ORDER BY p.id DESC LIMIT 1"}),
        where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, job.value).bind_int64(2, attempt);
    return first<PublicationRecord>(*statement, RowDecoder(false, where), read_journal_row);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// ReadTransaction
// ---------------------------------------------------------------------------------------------

Expected<std::optional<BatchRecord>> ReadTransaction::find_batch(BatchId id) const {
    constexpr std::string_view where = "catalog.find_batch";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> statement =
        connection_->prepare(sql({"SELECT ", kBatchColumns, " FROM batches b WHERE b.id = ?1"}), where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, id.value);
    const RowDecoder decode(newer_schema_, where);
    Expected<std::optional<BatchRecord>> batch = first<BatchRecord>(*statement, decode, read_batch_row);
    if (!batch || !batch->has_value()) {
        return batch;
    }
    statement->reset();
    Expected<BatchCounts> counts = batch_counts(*connection_, id, decode, where);
    if (!counts) {
        return Unexpected(std::move(counts.error()));
    }
    (*batch)->counts = *counts;
    return batch;
}

Expected<std::vector<BatchRecord>> ReadTransaction::list_batches(std::uint32_t limit, BatchId before) const {
    constexpr std::string_view where = "catalog.list_batches";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = check_limit(limit, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> statement = connection_->prepare(
        sql({"SELECT ", kBatchColumns,
             " FROM batches b WHERE (?1 = 0 OR b.id < ?1) ORDER BY b.id DESC LIMIT ?2"}),
        where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, before.value).bind_int64(2, limit);
    const RowDecoder decode(newer_schema_, where);
    Expected<std::vector<BatchRecord>> batches = detail::collect<BatchRecord>(*statement, decode, read_batch_row);
    if (!batches) {
        return batches;
    }
    statement->reset();
    for (BatchRecord& batch : *batches) {
        Expected<BatchCounts> counts = batch_counts(*connection_, batch.id, decode, where);
        if (!counts) {
            return Unexpected(std::move(counts.error()));
        }
        batch.counts = *counts;
    }
    return batches;
}

Expected<std::optional<JobRecord>> ReadTransaction::find_job(JobId id) const {
    constexpr std::string_view where = "catalog.find_job";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    return load_job(*connection_, id, newer_schema_, where);
}

Expected<std::vector<JobRecord>> ReadTransaction::list_jobs(const JobQuery& query) const {
    constexpr std::string_view where = "catalog.list_jobs";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = check_limit(query.limit, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    std::string text = sql({"SELECT ", kJobColumns, " FROM jobs j WHERE j.id > ?"});
    std::vector<Parameter> parameters{query.after.value};
    if (query.batch) {
        text += " AND j.batch_id = ?";
        parameters.emplace_back(query.batch->value);
    }
    if (!query.stages.empty()) {
        text += " AND j.stage IN (";
        for (std::size_t i = 0; i < query.stages.size(); ++i) {
            const std::string_view stage = to_string(query.stages[i]);
            if (stage == "invalid") {
                return Unexpected(invalid(where, "unknown job stage in the filter").error());
            }
            text += i == 0 ? "?" : ", ?";
            parameters.emplace_back(std::string(stage));
        }
        text += ")";
    }
    text += " ORDER BY j.id LIMIT ?";
    parameters.emplace_back(static_cast<std::int64_t>(query.limit));
    Expected<Statement> statement = connection_->prepare(text, where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    detail::bind_all(*statement, parameters);
    return detail::collect<JobRecord>(*statement, RowDecoder(newer_schema_, where), read_job_row);
}

Expected<std::vector<JobEvent>> ReadTransaction::list_job_events(const EventQuery& query) const {
    constexpr std::string_view where = "catalog.list_job_events";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = check_limit(query.limit, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    std::string text = sql({"SELECT ", kEventColumns, " FROM job_events e WHERE e.id > ?"});
    std::vector<Parameter> parameters{query.after};
    if (query.batch) {
        text += " AND e.batch_id = ?";
        parameters.emplace_back(query.batch->value);
    }
    if (query.job) {
        text += " AND e.job_id = ?";
        parameters.emplace_back(query.job->value);
    }
    text += " ORDER BY e.id LIMIT ?";
    parameters.emplace_back(static_cast<std::int64_t>(query.limit));
    Expected<Statement> statement = connection_->prepare(text, where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    detail::bind_all(*statement, parameters);
    return detail::collect<JobEvent>(*statement, RowDecoder(newer_schema_, where), read_event_row);
}

Expected<std::vector<PublicationRecord>> ReadTransaction::publication_journal(JobId job) const {
    constexpr std::string_view where = "catalog.publication_journal";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> statement = connection_->prepare(
        sql({"SELECT ", kJournalColumns, " FROM publication_journal p WHERE p.job_id = ?1 ORDER BY p.id"}), where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, job.value);
    return detail::collect<PublicationRecord>(*statement, RowDecoder(newer_schema_, where), read_journal_row);
}

Expected<std::optional<ReceiptRecord>> ReadTransaction::find_receipt(JobId job) const {
    constexpr std::string_view where = "catalog.find_receipt";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    return load_receipt(*connection_, job, newer_schema_, where);
}

Expected<std::vector<ReceiptRecord>> ReadTransaction::find_receipts_by_identity(
    const core::Digest256& identity) const {
    constexpr std::string_view where = "catalog.find_receipts_by_identity";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> statement = connection_->prepare(
        sql({"SELECT ", kReceiptColumns, " FROM receipts r WHERE r.identity_sha256 = ?1 ORDER BY r.id"}), where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_blob(1, detail::bytes_of(identity));
    return detail::collect<ReceiptRecord>(*statement, RowDecoder(newer_schema_, where), read_receipt_row);
}

Expected<std::vector<JobRecord>> ReadTransaction::list_reclaimable_jobs(const ReclaimQuery& query) const {
    constexpr std::string_view where = "catalog.list_reclaimable_jobs";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = check_limit(query.limit, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    std::string text =
        sql({"SELECT ", kJobColumns, " FROM jobs j WHERE j.lease_owner IS NOT NULL AND (j.lease_expires_at <= ?"});
    std::vector<Parameter> parameters{detail::to_db(query.now)};
    if (query.live_owners) {
        if (query.live_owners->empty()) {
            text += " OR 1";
        } else {
            text += " OR j.lease_owner NOT IN (";
            for (std::size_t i = 0; i < query.live_owners->size(); ++i) {
                text += i == 0 ? "?" : ", ?";
                parameters.emplace_back((*query.live_owners)[i]);
            }
            text += ")";
        }
    }
    text += ")";
    if (query.batch) {
        text += " AND j.batch_id = ?";
        parameters.emplace_back(query.batch->value);
    }
    text += " ORDER BY j.id LIMIT ?";
    parameters.emplace_back(static_cast<std::int64_t>(query.limit));
    Expected<Statement> statement = connection_->prepare(text, where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    detail::bind_all(*statement, parameters);
    return detail::collect<JobRecord>(*statement, RowDecoder(newer_schema_, where), read_job_row);
}

Expected<std::vector<PublicationInProgress>> ReadTransaction::list_publications_in_progress(
    std::optional<BatchId> batch, std::uint32_t limit, std::int64_t after) const {
    constexpr std::string_view where = "catalog.list_publications_in_progress";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = check_limit(limit, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> statement = connection_->prepare(
        sql({"SELECT ", kJournalColumns,
             ", j.batch_id, j.stage, j.attempts "
             "FROM (SELECT job_id, attempt, max(id) AS last FROM publication_journal GROUP BY job_id, attempt) l "
             "JOIN publication_journal p ON p.id = l.last JOIN jobs j ON j.id = p.job_id "
             "WHERE p.step NOT IN ('receipt_committed', 'abandoned') AND p.id > ?1 "
             "AND (?2 IS NULL OR j.batch_id = ?2) ORDER BY p.id LIMIT ?3"}),
        where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, after).bind_int64(3, limit);
    if (batch) {
        statement->bind_int64(2, batch->value);
    } else {
        statement->bind_null(2);
    }
    return detail::collect<PublicationInProgress>(
        *statement, RowDecoder(newer_schema_, where),
        [](const Statement& row, const RowDecoder& decode) -> Expected<PublicationInProgress> {
            Expected<PublicationRecord> latest = read_journal_row(row, decode);
            if (!latest) {
                return Unexpected(std::move(latest.error()));
            }
            Reader r(row, decode);
            PublicationInProgress open;
            open.latest = std::move(*latest);
            open.batch = BatchId{r.int64(9)};
            open.job_stage = r.stage(10, "jobs.stage");
            open.job_attempts = r.u32(11, "jobs.attempts");
            if (r.failed()) {
                return Unexpected(r.take_error());
            }
            return open;
        });
}

// ---------------------------------------------------------------------------------------------
// WriteTransaction: shared steps
// ---------------------------------------------------------------------------------------------

namespace {

constexpr std::string_view kLeaseLost = "catalog.job.lease_lost";
constexpr std::string_view kStageConflict = "catalog.job.stage_conflict";
constexpr std::string_view kNoReceipt = "catalog.job.no_receipt";
constexpr std::string_view kJournalOrder = "catalog.job.journal_order";
constexpr std::string_view kBatchCancelled = "catalog.job.batch_cancelled";
constexpr std::string_view kBatchTransition = "catalog.batch.invalid_transition";
constexpr std::string_view kReceiptConflict = "catalog.receipt.conflict";

// `text` cut to at most `limit` bytes at a code-point boundary (valid UTF-8 in, valid UTF-8 out).
std::string truncate_utf8(std::string_view text, std::size_t limit) {
    std::string clean = core::sanitize_utf8(text);
    if (clean.size() <= limit) {
        return clean;
    }
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(clean[end]) & 0xC0) == 0x80) {
        --end;
    }
    clean.resize(end);
    return clean;
}

JobError job_error(const Error& error, std::optional<FailureClass> failure_class, Timestamp at) {
    JobError out;
    out.failure_class = failure_class;
    out.code = error.code;
    out.where = truncate_utf8(error.where, kMaxErrorTextBytes);
    out.message = truncate_utf8(error.message, kMaxErrorTextBytes);
    out.native_code = error.native_code;
    out.at = at;
    return out;
}

// The parts of a job and its batch that the write calls check.
struct JobState {
    JobId id;
    BatchId batch;
    JobStage stage = JobStage::Queued;
    std::uint32_t attempts = 0;
    std::optional<std::string> lease_owner;
    std::optional<Timestamp> lease_expires_at;
    BatchState batch_state = BatchState::Draft;
    core::Digest256 identity{};
};

Expected<JobState> load_state(Connection& connection, JobId id, std::string_view where) {
    Expected<Statement> statement = connection.prepare(
        "SELECT j.batch_id, j.stage, j.attempts, j.lease_owner, j.lease_expires_at, b.state, j.identity_sha256 "
        "FROM jobs j JOIN batches b ON b.id = j.batch_id WHERE j.id = ?1",
        where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, id.value);
    Expected<bool> row = statement->step();
    if (!row) {
        return Unexpected(std::move(row.error()));
    }
    if (!*row) {
        return fail(ErrorCode::NotFound, std::format("no job with id {}", id.value), std::string(where));
    }
    const RowDecoder decode(false, where);
    Reader r(*statement, decode);
    JobState state;
    state.id = id;
    state.batch = BatchId{r.int64(0)};
    state.stage = r.stage(1, "jobs.stage");
    state.attempts = r.u32(2, "jobs.attempts");
    state.lease_owner = r.optional_text(3);
    state.lease_expires_at = r.optional_time(4);
    state.batch_state =
        r.parsed(detail::parse_batch_state(statement->column_text(5)), 5, "batches.state", BatchState::Draft);
    state.identity = r.digest(6, "jobs.identity_sha256");
    if (r.failed()) {
        return Unexpected(r.take_error());
    }
    return state;
}

Status lease_lost(const JobLease& lease, std::string_view reason) {
    return fail(ErrorCode::InvalidArgument,
                std::format("job {}: the lease of '{}' (attempt {}) {}", lease.job.value,
                            core::sanitize_utf8(lease.owner), lease.attempt, reason),
                std::string(kLeaseLost));
}

Status stage_conflict(JobId job, JobStage stage, std::string_view expected) {
    return fail(ErrorCode::InvalidArgument,
                std::format("job {} is {}, not {}", job.value, to_string(stage), expected),
                std::string(kStageConflict));
}

// The job's state when `lease` is its current lease (same owner and attempt, leased stage) and,
// with `require_unexpired`, not expired at `now`.
Expected<JobState> check_lease(Connection& connection, const JobLease& lease, Timestamp now, bool require_unexpired,
                               std::string_view where) {
    Expected<JobState> state = load_state(connection, lease.job, where);
    if (!state) {
        return state;
    }
    if (!is_leased(state->stage)) {
        return Unexpected(lease_lost(lease, std::format("ended: the job is {}", to_string(state->stage))).error());
    }
    if (state->lease_owner != lease.owner || state->attempts != lease.attempt) {
        return Unexpected(lease_lost(lease, "was superseded by another claim or recovered").error());
    }
    if (require_unexpired && (!state->lease_expires_at || *state->lease_expires_at <= now)) {
        return Unexpected(lease_lost(lease, "expired").error());
    }
    return state;
}

Status check_owner(std::string_view owner, std::string_view where) {
    if (owner.empty() || owner.size() > kMaxJobLabelBytes || !core::is_valid_utf8(owner)) {
        return invalid(where, std::format("the lease owner must be 1 ... {} bytes of UTF-8", kMaxJobLabelBytes));
    }
    return ok();
}

Status check_duration(std::chrono::milliseconds duration, std::string_view where) {
    if (duration < kMinLeaseDuration || duration > kMaxLeaseDuration) {
        return invalid(where, std::format("the lease duration must be {} ... {}", kMinLeaseDuration,
                                          kMaxLeaseDuration));
    }
    return ok();
}

void bind_error(Statement& statement, int first_column, const JobError* error) {
    if (error == nullptr) {
        for (int i = 0; i < 5; ++i) {
            statement.bind_null(first_column + i);
        }
        return;
    }
    if (error->failure_class) {
        statement.bind_text(first_column, to_string(*error->failure_class));
    } else {
        statement.bind_null(first_column);
    }
    statement.bind_int64(first_column + 1, static_cast<std::int64_t>(error->code))
        .bind_text(first_column + 2, error->where)
        .bind_text(first_column + 3, error->message)
        .bind_int64(first_column + 4, error->native_code);
}

Status add_event(Connection& connection, JobId job, BatchId batch, Timestamp at, std::optional<JobStage> from,
                 JobStage to, std::uint32_t attempt, const std::optional<std::string>& owner, const JobError* error,
                 std::string_view where) {
    Expected<Statement> insert = connection.prepare(
        "INSERT INTO job_events (job_id, batch_id, at, from_stage, to_stage, attempt, lease_owner, failure_class, "
        "error_code, error_where, error_message, error_native) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)",
        where);
    if (!insert) {
        return Unexpected(std::move(insert.error()));
    }
    insert->bind_int64(1, job.value)
        .bind_int64(2, batch.value)
        .bind_int64(3, detail::to_db(at))
        .bind_text(5, to_string(to))
        .bind_int64(6, attempt)
        .bind_optional_text(7, owner);
    if (from) {
        insert->bind_text(4, to_string(*from));
    } else {
        insert->bind_null(4);
    }
    bind_error(*insert, 8, error);
    return insert->run();
}

// Completes a Running or Paused batch whose jobs are all terminal, and records finished_at of a
// cancelled batch whose last leased job ended.
Status settle_batch(Connection& connection, BatchId batch, Timestamp now, std::string_view where) {
    Expected<Statement> pending = connection.prepare(
        sql({"SELECT count(*) FROM jobs WHERE batch_id = ?1 AND (stage = 'queued' OR stage IN ", kLeasedStagesSql,
             ")"}),
        where);
    if (!pending) {
        return Unexpected(std::move(pending.error()));
    }
    pending->bind_int64(1, batch.value);
    Expected<bool> row = pending->step();
    if (!row) {
        return Unexpected(std::move(row.error()));
    }
    const std::int64_t remaining = pending->column_int64(0);
    pending->reset();
    if (remaining > 0) {
        return ok();
    }
    Expected<Statement> update = connection.prepare(
        "UPDATE batches SET "
        "state = CASE WHEN state IN ('running', 'paused') THEN 'completed' ELSE state END, "
        "finished_at = ?2, updated_at = ?2 "
        "WHERE id = ?1 AND finished_at IS NULL AND state IN ('running', 'paused', 'cancelled')",
        where);
    if (!update) {
        return Unexpected(std::move(update.error()));
    }
    update->bind_int64(1, batch.value).bind_int64(2, detail::to_db(now));
    return update->run();
}

// How a transition changes the job besides its stage.
struct Transition {
    JobStage to = JobStage::Queued;
    std::optional<Timestamp> not_before;  // Queued: claimable from then
    const JobError* error = nullptr;      // recorded as last error and on the event
    bool count_failure = false;           // increment the counter of error->failure_class
};

// Moves a job from `state.stage` (checked again in the UPDATE) to `transition.to`, keeping the
// lease between leased stages and clearing it otherwise, appends the event and settles the batch
// when the job became terminal. Runs inside the caller's savepoint.
Status apply_transition(Connection& connection, const JobState& state, const Transition& transition, Timestamp now,
                        std::string_view where) {
    const bool keep_lease = is_leased(transition.to);
    const bool transient = transition.count_failure && transition.error != nullptr &&
                           transition.error->failure_class == FailureClass::Transient;
    const bool deterministic = transition.count_failure && transition.error != nullptr &&
                               transition.error->failure_class == FailureClass::Deterministic;
    Expected<Statement> update = connection.prepare(
        "UPDATE jobs SET stage = ?2, "
        "lease_owner = CASE WHEN ?3 THEN lease_owner ELSE NULL END, "
        "lease_expires_at = CASE WHEN ?3 THEN lease_expires_at ELSE NULL END, "
        "not_before = coalesce(?4, not_before), "
        "transient_failures = transient_failures + ?5, deterministic_failures = deterministic_failures + ?6, "
        "last_failure_class = CASE WHEN ?7 THEN ?8 ELSE last_failure_class END, "
        "last_error_code = CASE WHEN ?7 THEN ?9 ELSE last_error_code END, "
        "last_error_where = CASE WHEN ?7 THEN ?10 ELSE last_error_where END, "
        "last_error_message = CASE WHEN ?7 THEN ?11 ELSE last_error_message END, "
        "last_error_native = CASE WHEN ?7 THEN ?12 ELSE last_error_native END, "
        "last_error_at = CASE WHEN ?7 THEN ?13 ELSE last_error_at END, "
        "updated_at = ?13 "
        "WHERE id = ?1 AND stage = ?14 AND attempts = ?15",
        where);
    if (!update) {
        return Unexpected(std::move(update.error()));
    }
    update->bind_int64(1, state.id.value)
        .bind_text(2, to_string(transition.to))
        .bind_int64(3, keep_lease ? 1 : 0)
        .bind_int64(5, transient ? 1 : 0)
        .bind_int64(6, deterministic ? 1 : 0)
        .bind_int64(7, transition.error != nullptr ? 1 : 0)
        .bind_int64(13, detail::to_db(now))
        .bind_text(14, to_string(state.stage))
        .bind_int64(15, state.attempts);
    if (transition.not_before) {
        update->bind_int64(4, detail::to_db(*transition.not_before));
    } else {
        update->bind_null(4);
    }
    bind_error(*update, 8, transition.error);
    if (Status s = update->run(); !s) {
        return s;
    }
    if (connection.changes() != 1) {
        return fail(ErrorCode::Internal, std::format("job {} changed during its transition", state.id.value),
                    std::string(where));
    }
    // The event names the owner the transition was made for (the lease that ended or continues).
    if (Status s = add_event(connection, state.id, state.batch, now, state.stage, transition.to, state.attempts,
                             state.lease_owner, transition.error, where);
        !s) {
        return s;
    }
    if (is_terminal(transition.to)) {
        return settle_batch(connection, state.batch, now, where);
    }
    return ok();
}

// Runs `body` inside a savepoint that is committed when it succeeds.
template <class Body>
auto in_savepoint(Connection& connection, std::string_view where, Body&& body) -> decltype(body()) {
    Expected<TransactionGuard> savepoint = TransactionGuard::begin(connection, TransactionGuard::Kind::Savepoint, where);
    if (!savepoint) {
        return Unexpected(std::move(savepoint.error()));
    }
    auto result = body();
    if (!result) {
        return result;
    }
    if (Status s = savepoint->commit(); !s) {
        return Unexpected(std::move(s.error()));
    }
    return result;
}

Expected<std::optional<std::string>> optional_path_text(const std::optional<std::filesystem::path>& path,
                                                        std::string_view where) {
    if (!path) {
        return std::optional<std::string>();
    }
    Expected<std::string> text = detail::path_text(*path, where);
    if (!text) {
        return Unexpected(std::move(text.error()));
    }
    return std::optional<std::string>(std::move(*text));
}

Status append_journal(Connection& connection, JobId job, std::uint32_t attempt, PublicationStep step, Timestamp at,
                      const std::optional<std::string>& temp_path, const std::optional<std::string>& final_path,
                      const std::optional<core::Digest256>& sha256, std::optional<std::uint64_t> size,
                      std::string_view where) {
    Expected<Statement> insert = connection.prepare(
        "INSERT INTO publication_journal (job_id, attempt, step, at, temp_path, final_path, output_sha256, "
        "output_size) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
        where);
    if (!insert) {
        return Unexpected(std::move(insert.error()));
    }
    insert->bind_int64(1, job.value)
        .bind_int64(2, attempt)
        .bind_text(3, to_string(step))
        .bind_int64(4, detail::to_db(at))
        .bind_optional_text(5, temp_path)
        .bind_optional_text(6, final_path);
    if (sha256) {
        insert->bind_blob(7, detail::bytes_of(*sha256));
    } else {
        insert->bind_null(7);
    }
    if (size) {
        Expected<std::int64_t> value = detail::count_to_db(*size, where);
        if (!value) {
            return Unexpected(std::move(value.error()));
        }
        insert->bind_int64(8, *value);
    } else {
        insert->bind_null(8);
    }
    return insert->run();
}

Status validate_receipt(const NewReceipt& receipt, std::string_view where) {
    if (Status s = detail::check_text(receipt.settings_json, "settings_json", where); !s) {
        return s;
    }
    if (Status s = check_size(receipt.settings_json, kMaxBatchJsonBytes, "settings_json", where); !s) {
        return s;
    }
    if (receipt.final_path.empty()) {
        return invalid(where, "the receipt needs the final path");
    }
    return ok();
}

// The receipt must describe what the attempt's Published journal entry published.
Status check_receipt_matches(const PublicationRecord& published, const NewReceipt& receipt, std::string_view where) {
    if (published.step != PublicationStep::Published) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("job {} attempt {}: the last publication step is {}, not published",
                                published.job.value, published.attempt, to_string(published.step)),
                    std::string(kJournalOrder));
    }
    if (published.final_path != receipt.final_path || published.output_sha256 != receipt.output_sha256 ||
        published.output_size != receipt.size_bytes) {
        return invalid(where, std::format("the receipt of job {} does not match its published output (path, size "
                                          "or SHA-256)",
                                          published.job.value));
    }
    return ok();
}

// Writes a receipt and its ReceiptCommitted journal entry; or, when the job has a receipt,
// returns it if it is the same and AlreadyExists if it differs.
Expected<ReceiptRecord> store_receipt(Connection& connection, const JobState& state, std::uint32_t attempt,
                                      const NewReceipt& receipt, Timestamp now, std::string_view where) {
    Expected<std::string> path = detail::path_text(receipt.final_path, where);
    if (!path) {
        return Unexpected(std::move(path.error()));
    }
    Expected<std::int64_t> size = detail::count_to_db(receipt.size_bytes, where);
    if (!size) {
        return Unexpected(std::move(size.error()));
    }
    Expected<Statement> insert = connection.prepare(
        "INSERT INTO receipts (job_id, identity_sha256, final_path, size_bytes, output_sha256, settings_json, "
        "created_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) RETURNING id",
        where);
    if (!insert) {
        return Unexpected(std::move(insert.error()));
    }
    insert->bind_int64(1, state.id.value)
        .bind_blob(2, detail::bytes_of(state.identity))
        .bind_text(3, *path)
        .bind_int64(4, *size)
        .bind_blob(5, detail::bytes_of(receipt.output_sha256))
        .bind_text(6, receipt.settings_json)
        .bind_int64(7, detail::to_db(now));
    Expected<bool> row = insert->step();
    if (!row) {
        Error error = std::move(row.error());
        if (error.code == ErrorCode::InvalidArgument) {
            error.message = "receipt rejected (settings_json must be valid JSON): " + error.message;
        }
        return Unexpected(std::move(error));
    }
    if (!*row) {
        return fail(ErrorCode::Internal, "INSERT ... RETURNING returned no row", std::string(where));
    }
    ReceiptRecord record;
    record.id = ReceiptId{insert->column_int64(0)};
    insert->reset();
    record.job = state.id;
    record.identity_sha256 = state.identity;
    record.final_path = receipt.final_path;
    record.size_bytes = receipt.size_bytes;
    record.output_sha256 = receipt.output_sha256;
    record.settings_json = receipt.settings_json;
    record.created_at = now;
    if (Status s = append_journal(connection, state.id, attempt, PublicationStep::ReceiptCommitted, now, std::nullopt,
                                  *path, receipt.output_sha256, receipt.size_bytes, where);
        !s) {
        return Unexpected(std::move(s.error()));
    }
    return record;
}

// std::nullopt when the job has no receipt; the stored receipt when it equals `receipt`;
// AlreadyExists when it differs.
Expected<std::optional<ReceiptRecord>> existing_receipt(Connection& connection, JobId job,
                                                        const NewReceipt* receipt, std::string_view where) {
    Expected<std::optional<ReceiptRecord>> stored = load_receipt(connection, job, false, where);
    if (!stored || !stored->has_value() || receipt == nullptr) {
        return stored;
    }
    const ReceiptRecord& have = **stored;
    if (have.final_path != receipt->final_path || have.size_bytes != receipt->size_bytes ||
        have.output_sha256 != receipt->output_sha256 || have.settings_json != receipt->settings_json) {
        return fail(ErrorCode::AlreadyExists,
                    std::format("job {} already has a different receipt (receipt {})", job.value, have.id.value),
                    std::string(kReceiptConflict));
    }
    return stored;
}

Status check_failure_class(FailureClass value, std::string_view where) {
    if (to_string(value) == "invalid") {
        return invalid(where, "unknown failure class");
    }
    return ok();
}

// Batch commands: moves the batch from one of `from` to `to` (a no-op when it is already `to`).
Expected<BatchState> change_batch(Connection& connection, BatchId id, std::initializer_list<BatchState> from,
                                  BatchState to, Timestamp now, std::string_view where) {
    Expected<Statement> select = connection.prepare("SELECT state FROM batches WHERE id = ?1", where);
    if (!select) {
        return Unexpected(std::move(select.error()));
    }
    select->bind_int64(1, id.value);
    Expected<bool> row = select->step();
    if (!row) {
        return Unexpected(std::move(row.error()));
    }
    if (!*row) {
        return fail(ErrorCode::NotFound, std::format("no batch with id {}", id.value), std::string(where));
    }
    const std::optional<BatchState> state = detail::parse_batch_state(select->column_text(0));
    if (!state) {
        return Unexpected(RowDecoder(false, where).bad("batches.state", select->column_text(0)));
    }
    select->reset();
    if (*state == to) {
        return to;
    }
    if (std::find(from.begin(), from.end(), *state) == from.end()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("batch {} is {}: it cannot become {}", id.value, to_string(*state), to_string(to)),
                    std::string(kBatchTransition));
    }
    Expected<Statement> update = connection.prepare(
        "UPDATE batches SET state = ?2, updated_at = ?3, "
        "started_at = CASE WHEN ?2 = 'running' THEN coalesce(started_at, ?3) ELSE started_at END "
        "WHERE id = ?1",
        where);
    if (!update) {
        return Unexpected(std::move(update.error()));
    }
    update->bind_int64(1, id.value).bind_text(2, to_string(to)).bind_int64(3, detail::to_db(now));
    if (Status s = update->run(); !s) {
        return Unexpected(std::move(s.error()));
    }
    return to;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// WriteTransaction: batches
// ---------------------------------------------------------------------------------------------

Expected<CreatedBatch> WriteTransaction::create_batch(const NewBatch& batch, std::span<const NewJob> jobs) {
    constexpr std::string_view where = "catalog.create_batch";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = batch.validate(); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (jobs.empty()) {
        return fail(ErrorCode::InvalidArgument, "a batch needs at least one job", std::string(where));
    }
    if (jobs.size() > kMaxBatchJobs) {
        return fail(ErrorCode::LimitExceeded, std::format("a batch has at most {} jobs", kMaxBatchJobs),
                    std::string(where));
    }
    for (const NewJob& job : jobs) {
        if (Status s = detail::check_text(job.assets_json, "assets_json", where); !s) {
            return Unexpected(std::move(s.error()));
        }
        if (Status s = check_size(job.assets_json, kMaxBatchJsonBytes, "assets_json", where); !s) {
            return Unexpected(std::move(s.error()));
        }
    }
    Expected<core::Digest256> export_digest = detail::sha256_of(batch.export_settings_json);
    if (!export_digest) {
        return Unexpected(std::move(export_digest.error()));
    }
    Connection& connection = *connection_;
    return in_savepoint(connection, where, [&]() -> Expected<CreatedBatch> {
        const std::int64_t now = detail::to_db(batch.created_at);
        Expected<Statement> insert = connection.prepare(
            "INSERT INTO batches (name, selection_description, destination_json, export_settings_json, "
            "export_settings_sha256, policy_json, renderer_version, state, job_count, created_at, updated_at, "
            "started_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?10, ?11) RETURNING id",
            where);
        if (!insert) {
            return Unexpected(std::move(insert.error()));
        }
        insert->bind_text(1, batch.name)
            .bind_text(2, batch.selection_description)
            .bind_text(3, batch.destination_json)
            .bind_text(4, batch.export_settings_json)
            .bind_blob(5, detail::bytes_of(*export_digest))
            .bind_text(6, batch.policy_json)
            .bind_text(7, batch.renderer_version)
            .bind_text(8, to_string(batch.start ? BatchState::Running : BatchState::Draft))
            .bind_int64(9, static_cast<std::int64_t>(jobs.size()))
            .bind_int64(10, now);
        if (batch.start) {
            insert->bind_int64(11, now);
        } else {
            insert->bind_null(11);
        }
        Expected<bool> row = insert->step();
        if (!row) {
            Error error = std::move(row.error());
            if (error.code == ErrorCode::InvalidArgument) {
                error.message = "batch rejected (its JSON documents must be valid JSON): " + error.message;
            }
            return Unexpected(std::move(error));
        }
        if (!*row) {
            return fail(ErrorCode::Internal, "INSERT ... RETURNING returned no row", std::string(where));
        }
        CreatedBatch created;
        created.id = BatchId{insert->column_int64(0)};
        insert->reset();
        created.jobs.reserve(jobs.size());

        for (std::size_t ordinal = 0; ordinal < jobs.size(); ++ordinal) {
            const NewJob& job = jobs[ordinal];
            // The item's subimage and its source's content hash.
            Expected<Statement> item = connection.prepare(
                "SELECT i.subimage_index, s.sha256 FROM items i JOIN sources s ON s.id = i.source_id WHERE i.id = ?1",
                where);
            if (!item) {
                return Unexpected(std::move(item.error()));
            }
            item->bind_int64(1, job.item.value);
            Expected<bool> found = item->step();
            if (!found) {
                return Unexpected(std::move(found.error()));
            }
            if (!*found) {
                return fail(ErrorCode::NotFound, std::format("no item with id {}", job.item.value), std::string(where));
            }
            if (item->is_null(1)) {
                return fail(ErrorCode::InvalidArgument,
                            std::format("the source of item {} has not been hashed yet", job.item.value),
                            "catalog.create_batch.unhashed");
            }
            JobIdentityInput identity;
            identity.subimage = static_cast<std::uint32_t>(item->column_int64(0));
            Expected<core::Digest256> source_digest = RowDecoder(false, where).blob<32>("sources.sha256", item->column_blob(1));
            if (!source_digest) {
                return Unexpected(std::move(source_digest.error()));
            }
            item->reset();
            identity.source_sha256 = *source_digest;
            identity.settings_sha256 = job.settings_sha256;
            identity.renderer_version = batch.renderer_version;
            identity.assets_json = job.assets_json;
            identity.export_settings_sha256 = *export_digest;

            if (job.revision) {
                Expected<Statement> revision =
                    connection.prepare("SELECT item_id, settings_sha256 FROM recipe_revisions WHERE id = ?1", where);
                if (!revision) {
                    return Unexpected(std::move(revision.error()));
                }
                revision->bind_int64(1, job.revision->value);
                Expected<bool> has = revision->step();
                if (!has) {
                    return Unexpected(std::move(has.error()));
                }
                if (!*has) {
                    return fail(ErrorCode::NotFound, std::format("no recipe revision with id {}", job.revision->value),
                                std::string(where));
                }
                const std::span<const std::uint8_t> stored = revision->column_blob(1);
                const bool same_item = revision->column_int64(0) == job.item.value;
                const bool same_settings = stored.size() == job.settings_sha256.size() &&
                                           std::equal(stored.begin(), stored.end(), job.settings_sha256.begin());
                revision->reset();
                if (!same_item) {
                    return Unexpected(invalid(where, std::format("recipe revision {} does not belong to item {}",
                                                                 job.revision->value, job.item.value))
                                          .error());
                }
                if (!same_settings) {
                    return Unexpected(invalid(where, std::format("the settings SHA-256 of the job for item {} is "
                                                                 "not that of recipe revision {}",
                                                                 job.item.value, job.revision->value))
                                          .error());
                }
            }
            Expected<core::Digest256> identity_digest = compute_job_identity(identity);
            if (!identity_digest) {
                return Unexpected(std::move(identity_digest.error()));
            }

            Expected<Statement> add = connection.prepare(
                "INSERT INTO jobs (batch_id, ordinal, item_id, subimage, revision_id, settings_sha256, source_sha256, "
                "assets_json, identity_sha256, stage, not_before, created_at, updated_at) "
                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, 'queued', ?10, ?10, ?10) RETURNING id",
                where);
            if (!add) {
                return Unexpected(std::move(add.error()));
            }
            add->bind_int64(1, created.id.value)
                .bind_int64(2, static_cast<std::int64_t>(ordinal))
                .bind_int64(3, job.item.value)
                .bind_int64(4, identity.subimage)
                .bind_blob(6, detail::bytes_of(job.settings_sha256))
                .bind_blob(7, detail::bytes_of(identity.source_sha256))
                .bind_text(8, job.assets_json)
                .bind_blob(9, detail::bytes_of(*identity_digest))
                .bind_int64(10, now);
            if (job.revision) {
                add->bind_int64(5, job.revision->value);
            } else {
                add->bind_null(5);
            }
            Expected<bool> added = add->step();
            if (!added) {
                Error error = std::move(added.error());
                if (error.code == ErrorCode::InvalidArgument) {
                    error.message = "job rejected (assets_json must be valid JSON): " + error.message;
                }
                return Unexpected(std::move(error));
            }
            if (!*added) {
                return fail(ErrorCode::Internal, "INSERT ... RETURNING returned no row", std::string(where));
            }
            const JobId id{add->column_int64(0)};
            add->reset();
            if (Status s = add_event(connection, id, created.id, batch.created_at, std::nullopt, JobStage::Queued, 0,
                                     std::nullopt, nullptr, where);
                !s) {
                return Unexpected(std::move(s.error()));
            }
            created.jobs.push_back(id);
        }
        return created;
    });
}

Expected<BatchState> WriteTransaction::start_batch(BatchId id, Timestamp now) {
    constexpr std::string_view where = "catalog.start_batch";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    return change_batch(*connection_, id, {BatchState::Draft}, BatchState::Running, now, where);
}

Expected<BatchState> WriteTransaction::pause_batch(BatchId id, Timestamp now) {
    constexpr std::string_view where = "catalog.pause_batch";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    return change_batch(*connection_, id, {BatchState::Running}, BatchState::Paused, now, where);
}

Expected<BatchState> WriteTransaction::resume_batch(BatchId id, Timestamp now) {
    constexpr std::string_view where = "catalog.resume_batch";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    return change_batch(*connection_, id, {BatchState::Paused}, BatchState::Running, now, where);
}

Expected<BatchState> WriteTransaction::cancel_batch(BatchId id, Timestamp now) {
    constexpr std::string_view where = "catalog.cancel_batch";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Connection& connection = *connection_;
    return in_savepoint(connection, where, [&]() -> Expected<BatchState> {
        Expected<BatchState> state = change_batch(
            connection, id, {BatchState::Draft, BatchState::Running, BatchState::Paused}, BatchState::Cancelled, now,
            where);
        if (!state) {
            return state;
        }
        // Queued jobs: one event each, then the stage.
        Expected<Statement> events = connection.prepare(
            "INSERT INTO job_events (job_id, batch_id, at, from_stage, to_stage, attempt) "
            "SELECT id, batch_id, ?2, 'queued', 'cancelled', attempts FROM jobs "
            "WHERE batch_id = ?1 AND stage = 'queued' ORDER BY id",
            where);
        if (!events) {
            return Unexpected(std::move(events.error()));
        }
        events->bind_int64(1, id.value).bind_int64(2, detail::to_db(now));
        if (Status s = events->run(); !s) {
            return Unexpected(std::move(s.error()));
        }
        Expected<Statement> cancel = connection.prepare(
            "UPDATE jobs SET stage = 'cancelled', updated_at = ?2 WHERE batch_id = ?1 AND stage = 'queued'", where);
        if (!cancel) {
            return Unexpected(std::move(cancel.error()));
        }
        cancel->bind_int64(1, id.value).bind_int64(2, detail::to_db(now));
        if (Status s = cancel->run(); !s) {
            return Unexpected(std::move(s.error()));
        }
        if (Status s = settle_batch(connection, id, now, where); !s) {
            return Unexpected(std::move(s.error()));
        }
        return state;
    });
}

Expected<std::uint64_t> WriteTransaction::retry_failed_jobs(BatchId id, Timestamp now) {
    constexpr std::string_view where = "catalog.retry_failed_jobs";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Connection& connection = *connection_;
    return in_savepoint(connection, where, [&]() -> Expected<std::uint64_t> {
        Expected<Statement> select = connection.prepare("SELECT state FROM batches WHERE id = ?1", where);
        if (!select) {
            return Unexpected(std::move(select.error()));
        }
        select->bind_int64(1, id.value);
        Expected<bool> row = select->step();
        if (!row) {
            return Unexpected(std::move(row.error()));
        }
        if (!*row) {
            return fail(ErrorCode::NotFound, std::format("no batch with id {}", id.value), std::string(where));
        }
        const std::optional<BatchState> state = detail::parse_batch_state(select->column_text(0));
        if (!state) {
            return Unexpected(RowDecoder(false, where).bad("batches.state", select->column_text(0)));
        }
        select->reset();
        if (*state == BatchState::Draft || *state == BatchState::Cancelled) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("batch {} is {}: its failed jobs cannot be retried", id.value, to_string(*state)),
                        std::string(kBatchTransition));
        }
        Expected<Statement> events = connection.prepare(
            "INSERT INTO job_events (job_id, batch_id, at, from_stage, to_stage, attempt) "
            "SELECT id, batch_id, ?2, 'failed', 'queued', attempts FROM jobs "
            "WHERE batch_id = ?1 AND stage = 'failed' ORDER BY id",
            where);
        if (!events) {
            return Unexpected(std::move(events.error()));
        }
        events->bind_int64(1, id.value).bind_int64(2, detail::to_db(now));
        if (Status s = events->run(); !s) {
            return Unexpected(std::move(s.error()));
        }
        Expected<Statement> requeue = connection.prepare(
            "UPDATE jobs SET stage = 'queued', not_before = ?2, updated_at = ?2 WHERE batch_id = ?1 AND stage = 'failed'",
            where);
        if (!requeue) {
            return Unexpected(std::move(requeue.error()));
        }
        requeue->bind_int64(1, id.value).bind_int64(2, detail::to_db(now));
        if (Status s = requeue->run(); !s) {
            return Unexpected(std::move(s.error()));
        }
        const auto requeued = static_cast<std::uint64_t>(connection.changes());
        if (requeued > 0 && *state == BatchState::Completed) {
            Expected<Statement> reopen = connection.prepare(
                "UPDATE batches SET state = 'running', finished_at = NULL, updated_at = ?2 WHERE id = ?1", where);
            if (!reopen) {
                return Unexpected(std::move(reopen.error()));
            }
            reopen->bind_int64(1, id.value).bind_int64(2, detail::to_db(now));
            if (Status s = reopen->run(); !s) {
                return Unexpected(std::move(s.error()));
            }
        }
        return requeued;
    });
}

// ---------------------------------------------------------------------------------------------
// WriteTransaction: leases and stages
// ---------------------------------------------------------------------------------------------

Expected<std::optional<ClaimedJob>> WriteTransaction::claim_next_job(const ClaimRequest& request) {
    constexpr std::string_view where = "catalog.claim_next_job";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = check_owner(request.owner, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = check_duration(request.lease_duration, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Connection& connection = *connection_;
    return in_savepoint(connection, where, [&]() -> Expected<std::optional<ClaimedJob>> {
        Expected<Statement> next = connection.prepare(
            "SELECT j.id FROM jobs j JOIN batches b ON b.id = j.batch_id "
            "WHERE j.stage = 'queued' AND b.state = 'running' AND j.not_before <= ?1 "
            "AND (?2 IS NULL OR j.batch_id = ?2) ORDER BY j.batch_id, j.ordinal LIMIT 1",
            where);
        if (!next) {
            return Unexpected(std::move(next.error()));
        }
        next->bind_int64(1, detail::to_db(request.now));
        if (request.batch) {
            next->bind_int64(2, request.batch->value);
        } else {
            next->bind_null(2);
        }
        Expected<bool> row = next->step();
        if (!row) {
            return Unexpected(std::move(row.error()));
        }
        if (!*row) {
            return std::optional<ClaimedJob>();
        }
        const JobId id{next->column_int64(0)};
        next->reset();

        Expected<JobState> state = load_state(connection, id, where);
        if (!state) {
            return Unexpected(std::move(state.error()));
        }
        const Timestamp expires = request.now + request.lease_duration;
        // "AND stage = 'queued'": the claim is conditional on the row still being unclaimed, so
        // even a caller that bypassed the writer lock could not lease it twice.
        Expected<Statement> claim = connection.prepare(
            "UPDATE jobs SET stage = 'rendering', attempts = attempts + 1, lease_owner = ?2, lease_expires_at = ?3, "
            "heartbeat_at = ?4, updated_at = ?4 WHERE id = ?1 AND stage = 'queued'",
            where);
        if (!claim) {
            return Unexpected(std::move(claim.error()));
        }
        claim->bind_int64(1, id.value)
            .bind_text(2, request.owner)
            .bind_int64(3, detail::to_db(expires))
            .bind_int64(4, detail::to_db(request.now));
        if (Status s = claim->run(); !s) {
            return Unexpected(std::move(s.error()));
        }
        if (connection.changes() != 1) {
            return fail(ErrorCode::Internal, std::format("job {} was claimed concurrently", id.value),
                        std::string(where));
        }
        const std::uint32_t attempt = state->attempts + 1;
        if (Status s = add_event(connection, id, state->batch, request.now, JobStage::Queued, JobStage::Rendering,
                                 attempt, request.owner, nullptr, where);
            !s) {
            return Unexpected(std::move(s.error()));
        }
        Expected<std::optional<JobRecord>> job = load_job(connection, id, false, where);
        if (!job) {
            return Unexpected(std::move(job.error()));
        }
        if (!job->has_value()) {
            return fail(ErrorCode::Internal, "a claimed job vanished", std::string(where));
        }
        ClaimedJob claimed;
        claimed.lease = JobLease{id, request.owner, attempt, expires};
        claimed.job = std::move(**job);
        return std::optional<ClaimedJob>(std::move(claimed));
    });
}

Expected<LeaseRenewal> WriteTransaction::heartbeat_job(const JobLease& lease, Timestamp now,
                                                       std::chrono::milliseconds lease_duration) {
    constexpr std::string_view where = "catalog.heartbeat_job";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = check_duration(lease_duration, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<JobState> state = check_lease(*connection_, lease, now, true, where);
    if (!state) {
        return Unexpected(std::move(state.error()));
    }
    const Timestamp expires = now + lease_duration;
    Expected<Statement> update = connection_->prepare(
        "UPDATE jobs SET lease_expires_at = ?2, heartbeat_at = ?3, updated_at = ?3 WHERE id = ?1", where);
    if (!update) {
        return Unexpected(std::move(update.error()));
    }
    update->bind_int64(1, lease.job.value).bind_int64(2, detail::to_db(expires)).bind_int64(3, detail::to_db(now));
    if (Status s = update->run(); !s) {
        return Unexpected(std::move(s.error()));
    }
    LeaseRenewal renewal;
    renewal.expires_at = expires;
    renewal.batch_state = state->batch_state;
    renewal.cancel_requested = state->batch_state == BatchState::Cancelled;
    return renewal;
}

Expected<JobStage> WriteTransaction::advance_job(const JobLease& lease, JobStage from, Timestamp now) {
    constexpr std::string_view where = "catalog.advance_job";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Connection& connection = *connection_;
    return in_savepoint(connection, where, [&]() -> Expected<JobStage> {
        Expected<JobState> state = check_lease(connection, lease, now, true, where);
        if (!state) {
            return Unexpected(std::move(state.error()));
        }
        if (state->stage != from) {
            return Unexpected(stage_conflict(lease.job, state->stage, to_string(from)).error());
        }
        JobStage to = JobStage::Queued;
        switch (from) {
            case JobStage::Rendering:
                to = JobStage::Verifying;
                break;
            case JobStage::Verifying:
                to = JobStage::Publishing;
                break;
            case JobStage::Publishing:
                to = JobStage::Complete;
                break;
            default:
                return Unexpected(stage_conflict(lease.job, state->stage, "rendering, verifying or publishing").error());
        }
        if (state->batch_state == BatchState::Cancelled && to != JobStage::Complete) {
            return fail(ErrorCode::Cancelled,
                        std::format("batch {} was cancelled: job {} must stop (cancel_job)", state->batch.value,
                                    lease.job.value),
                        std::string(kBatchCancelled));
        }
        if (to == JobStage::Complete) {
            Expected<std::optional<ReceiptRecord>> receipt = load_receipt(connection, lease.job, false, where);
            if (!receipt) {
                return Unexpected(std::move(receipt.error()));
            }
            if (!receipt->has_value()) {
                return fail(ErrorCode::InvalidArgument,
                            std::format("job {} has no receipt: it cannot be complete", lease.job.value),
                            std::string(kNoReceipt));
            }
        }
        Transition transition;
        transition.to = to;
        if (Status s = apply_transition(connection, *state, transition, now, where); !s) {
            return Unexpected(std::move(s.error()));
        }
        return to;
    });
}

Status WriteTransaction::record_publication_step(const JobLease& lease, const PublicationEntry& entry, Timestamp now) {
    constexpr std::string_view where = "catalog.record_publication_step";
    if (Status s = check_active(connection_, where); !s) {
        return s;
    }
    const auto step_value = static_cast<std::uint8_t>(entry.step);
    if (step_value < static_cast<std::uint8_t>(PublicationStep::Reserved) ||
        step_value > static_cast<std::uint8_t>(PublicationStep::Published)) {
        return invalid(where, "only the steps reserved ... published can be recorded directly");
    }
    if (!entry.final_path) {
        return invalid(where, "every publication step needs the final path");
    }
    if (entry.step >= PublicationStep::TempWritten && !entry.temp_path) {
        return invalid(where, std::format("step {} needs the temporary path", to_string(entry.step)));
    }
    if (entry.step >= PublicationStep::Verified && (!entry.output_sha256 || !entry.output_size)) {
        return invalid(where, std::format("step {} needs the output SHA-256 and size", to_string(entry.step)));
    }
    Expected<std::optional<std::string>> temp_text = optional_path_text(entry.temp_path, where);
    if (!temp_text) {
        return Unexpected(std::move(temp_text.error()));
    }
    Expected<std::optional<std::string>> final_text = optional_path_text(entry.final_path, where);
    if (!final_text) {
        return Unexpected(std::move(final_text.error()));
    }
    Connection& connection = *connection_;
    return in_savepoint(connection, where, [&]() -> Status {
        Expected<JobState> state = check_lease(connection, lease, now, true, where);
        if (!state) {
            return Unexpected(std::move(state.error()));
        }
        if (entry.step == PublicationStep::Published && state->stage != JobStage::Publishing) {
            return stage_conflict(lease.job, state->stage, "publishing");
        }
        Expected<std::optional<PublicationRecord>> latest = latest_entry(connection, lease.job, lease.attempt, where);
        if (!latest) {
            return Unexpected(std::move(latest.error()));
        }
        const std::uint8_t expected_previous = static_cast<std::uint8_t>(step_value - 1);  // 0: none
        const std::uint8_t previous =
            latest->has_value() ? static_cast<std::uint8_t>((*latest)->step) : std::uint8_t{0};
        if (previous != expected_previous) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("job {} attempt {}: step {} cannot follow {}", lease.job.value, lease.attempt,
                                    to_string(entry.step),
                                    latest->has_value() ? to_string((*latest)->step) : std::string_view("nothing")),
                        std::string(kJournalOrder));
        }
        if (Status s = append_journal(connection, lease.job, lease.attempt, entry.step, now, *temp_text, *final_text,
                                      entry.output_sha256, entry.output_size, where);
            !s) {
            return s;
        }
        Expected<Statement> update = connection.prepare(
            "UPDATE jobs SET temp_path = coalesce(?2, temp_path), final_path = ?3, updated_at = ?4 WHERE id = ?1",
            where);
        if (!update) {
            return Unexpected(std::move(update.error()));
        }
        update->bind_int64(1, lease.job.value)
            .bind_optional_text(2, *temp_text)
            .bind_optional_text(3, *final_text)
            .bind_int64(4, detail::to_db(now));
        return update->run();
    });
}

Expected<ReceiptRecord> WriteTransaction::record_receipt(const JobLease& lease, const NewReceipt& receipt,
                                                         Timestamp now) {
    constexpr std::string_view where = "catalog.record_receipt";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = validate_receipt(receipt, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Connection& connection = *connection_;
    return in_savepoint(connection, where, [&]() -> Expected<ReceiptRecord> {
        // Idempotent replay first: the same receipt is accepted even after the lease ended (a
        // worker whose first call committed but whose reply was lost).
        Expected<std::optional<ReceiptRecord>> stored = existing_receipt(connection, lease.job, &receipt, where);
        if (!stored) {
            return Unexpected(std::move(stored.error()));
        }
        if (stored->has_value()) {
            return std::move(**stored);
        }
        Expected<JobState> state = check_lease(connection, lease, now, true, where);
        if (!state) {
            return Unexpected(std::move(state.error()));
        }
        if (state->stage != JobStage::Publishing) {
            return Unexpected(stage_conflict(lease.job, state->stage, "publishing").error());
        }
        Expected<std::optional<PublicationRecord>> latest = latest_entry(connection, lease.job, lease.attempt, where);
        if (!latest) {
            return Unexpected(std::move(latest.error()));
        }
        if (!latest->has_value()) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("job {} attempt {} has published nothing", lease.job.value, lease.attempt),
                        std::string(kJournalOrder));
        }
        if (Status s = check_receipt_matches(**latest, receipt, where); !s) {
            return Unexpected(std::move(s.error()));
        }
        return store_receipt(connection, *state, lease.attempt, receipt, now, where);
    });
}

Expected<JobStage> WriteTransaction::record_failure(const JobLease& lease, const JobFailure& failure, Timestamp now) {
    constexpr std::string_view where = "catalog.record_failure";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = check_failure_class(failure.failure_class, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Connection& connection = *connection_;
    return in_savepoint(connection, where, [&]() -> Expected<JobStage> {
        Expected<JobState> state = check_lease(connection, lease, now, true, where);
        if (!state) {
            return Unexpected(std::move(state.error()));
        }
        const JobError error = job_error(failure.error, failure.failure_class, now);
        Transition transition;
        transition.error = &error;
        transition.count_failure = true;
        if (!failure.retry_at) {
            transition.to = JobStage::Failed;
        } else if (state->batch_state == BatchState::Cancelled) {
            transition.to = JobStage::Cancelled;
        } else {
            transition.to = JobStage::Queued;
            transition.not_before = *failure.retry_at;
        }
        if (Status s = apply_transition(connection, *state, transition, now, where); !s) {
            return Unexpected(std::move(s.error()));
        }
        return transition.to;
    });
}

Expected<JobStage> WriteTransaction::release_job(const JobLease& lease, Timestamp now) {
    constexpr std::string_view where = "catalog.release_job";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Connection& connection = *connection_;
    return in_savepoint(connection, where, [&]() -> Expected<JobStage> {
        Expected<JobState> state = check_lease(connection, lease, now, true, where);
        if (!state) {
            return Unexpected(std::move(state.error()));
        }
        Transition transition;
        if (state->batch_state == BatchState::Cancelled) {
            transition.to = JobStage::Cancelled;
        } else {
            transition.to = JobStage::Queued;
            transition.not_before = now;
        }
        if (Status s = apply_transition(connection, *state, transition, now, where); !s) {
            return Unexpected(std::move(s.error()));
        }
        return transition.to;
    });
}

Status WriteTransaction::cancel_job(const JobLease& lease, Timestamp now) {
    constexpr std::string_view where = "catalog.cancel_job";
    if (Status s = check_active(connection_, where); !s) {
        return s;
    }
    Connection& connection = *connection_;
    return in_savepoint(connection, where, [&]() -> Status {
        Expected<JobState> state = check_lease(connection, lease, now, true, where);
        if (!state) {
            return Unexpected(std::move(state.error()));
        }
        Expected<std::optional<PublicationRecord>> latest = latest_entry(connection, lease.job, lease.attempt, where);
        if (!latest) {
            return Unexpected(std::move(latest.error()));
        }
        if (latest->has_value() && (*latest)->step >= PublicationStep::Published) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("job {} has published its output: finish it instead of cancelling it",
                                    lease.job.value),
                        std::string(kStageConflict));
        }
        Transition transition;
        transition.to = JobStage::Cancelled;
        return apply_transition(connection, *state, transition, now, where);
    });
}

Status WriteTransaction::skip_job(JobId job, const Error& reason, Timestamp now) {
    constexpr std::string_view where = "catalog.skip_job";
    if (Status s = check_active(connection_, where); !s) {
        return s;
    }
    Connection& connection = *connection_;
    return in_savepoint(connection, where, [&]() -> Status {
        Expected<JobState> state = load_state(connection, job, where);
        if (!state) {
            return Unexpected(std::move(state.error()));
        }
        if (state->stage != JobStage::Queued) {
            return stage_conflict(job, state->stage, "queued");
        }
        const JobError error = job_error(reason, std::nullopt, now);
        Transition transition;
        transition.to = JobStage::Skipped;
        transition.error = &error;
        return apply_transition(connection, *state, transition, now, where);
    });
}

Expected<JobStage> WriteTransaction::recover_job(const JobLease& stale, const JobRecovery& recovery) {
    constexpr std::string_view where = "catalog.recover_job";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = check_failure_class(recovery.failure_class, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (recovery.receipt) {
        if (Status s = validate_receipt(*recovery.receipt, where); !s) {
            return Unexpected(std::move(s.error()));
        }
    }
    Connection& connection = *connection_;
    const Timestamp now = recovery.now;
    return in_savepoint(connection, where, [&]() -> Expected<JobStage> {
        Expected<JobState> state = check_lease(connection, stale, now, false, where);
        if (!state) {
            return Unexpected(std::move(state.error()));
        }
        if (!recovery.owner_gone && state->lease_expires_at && *state->lease_expires_at > now) {
            return Unexpected(lease_lost(stale, "is still valid: its owner may still be working").error());
        }
        const JobError error = job_error(recovery.error, recovery.failure_class, now);
        Transition transition;
        switch (recovery.action) {
            case RecoveryAction::Requeue:
                transition.error = &error;
                transition.count_failure = true;
                if (state->batch_state == BatchState::Cancelled) {
                    transition.to = JobStage::Cancelled;
                } else {
                    transition.to = JobStage::Queued;
                    transition.not_before = recovery.retry_at.value_or(now);
                }
                break;
            case RecoveryAction::Fail:
                transition.error = &error;
                transition.count_failure = true;
                transition.to = JobStage::Failed;
                break;
            case RecoveryAction::Complete: {
                Expected<std::optional<ReceiptRecord>> stored =
                    existing_receipt(connection, stale.job, recovery.receipt ? &*recovery.receipt : nullptr, where);
                if (!stored) {
                    return Unexpected(std::move(stored.error()));
                }
                if (!stored->has_value()) {
                    if (!recovery.receipt) {
                        return fail(ErrorCode::InvalidArgument,
                                    std::format("job {} has no receipt: an abandoned lease is never completed "
                                                "without one",
                                                stale.job.value),
                                    std::string(kNoReceipt));
                    }
                    Expected<std::optional<PublicationRecord>> latest =
                        latest_entry(connection, stale.job, stale.attempt, where);
                    if (!latest) {
                        return Unexpected(std::move(latest.error()));
                    }
                    if (!latest->has_value()) {
                        return fail(ErrorCode::InvalidArgument,
                                    std::format("job {} attempt {} has published nothing", stale.job.value,
                                                stale.attempt),
                                    std::string(kJournalOrder));
                    }
                    if (Status s = check_receipt_matches(**latest, *recovery.receipt, where); !s) {
                        return Unexpected(std::move(s.error()));
                    }
                    Expected<ReceiptRecord> written =
                        store_receipt(connection, *state, stale.attempt, *recovery.receipt, now, where);
                    if (!written) {
                        return Unexpected(std::move(written.error()));
                    }
                }
                transition.to = JobStage::Complete;
                break;
            }
            default:
                return Unexpected(invalid(where, "unknown recovery action").error());
        }
        if (Status s = apply_transition(connection, *state, transition, now, where); !s) {
            return Unexpected(std::move(s.error()));
        }
        return transition.to;
    });
}

Status WriteTransaction::abandon_publication(JobId job, std::uint32_t attempt, Timestamp now) {
    constexpr std::string_view where = "catalog.abandon_publication";
    if (Status s = check_active(connection_, where); !s) {
        return s;
    }
    Connection& connection = *connection_;
    return in_savepoint(connection, where, [&]() -> Status {
        Expected<JobState> state = load_state(connection, job, where);
        if (!state) {
            return Unexpected(std::move(state.error()));
        }
        if (is_leased(state->stage) && state->attempts == attempt) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("job {} attempt {} still holds its lease: recover the job first", job.value,
                                    attempt),
                        std::string(kStageConflict));
        }
        Expected<std::optional<PublicationRecord>> latest = latest_entry(connection, job, attempt, where);
        if (!latest) {
            return Unexpected(std::move(latest.error()));
        }
        if (!latest->has_value() || (*latest)->step == PublicationStep::ReceiptCommitted ||
            (*latest)->step == PublicationStep::Abandoned) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("job {} attempt {} has no publication in progress", job.value, attempt),
                        std::string(kJournalOrder));
        }
        Expected<std::optional<std::string>> temp_text = optional_path_text((*latest)->temp_path, where);
        if (!temp_text) {
            return Unexpected(std::move(temp_text.error()));
        }
        Expected<std::optional<std::string>> final_text = optional_path_text((*latest)->final_path, where);
        if (!final_text) {
            return Unexpected(std::move(final_text.error()));
        }
        if (Status s = append_journal(connection, job, attempt, PublicationStep::Abandoned, now, *temp_text,
                                      *final_text, (*latest)->output_sha256, (*latest)->output_size, where);
            !s) {
            return s;
        }
        Expected<Statement> update = connection.prepare(
            "UPDATE jobs SET temp_path = NULL, updated_at = ?3 WHERE id = ?1 AND attempts = ?2", where);
        if (!update) {
            return Unexpected(std::move(update.error()));
        }
        update->bind_int64(1, job.value).bind_int64(2, attempt).bind_int64(3, detail::to_db(now));
        return update->run();
    });
}

}  // namespace lucent::catalog
