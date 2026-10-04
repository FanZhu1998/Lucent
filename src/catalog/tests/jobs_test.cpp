// The durable batch journal (schema v3): batch creation, leases and their exclusivity under
// concurrency, expired-lease recovery, stage-transition validation, the publication journal,
// receipts (idempotent; no Complete without one), failures and retry inputs, pause / resume /
// cancel, reconciliation helpers and the append-only tables.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "lucent/catalog/catalog.hpp"
#include "lucent/catalog/jobs.hpp"

#include "catalog_test_support.hpp"

namespace lucent::catalog {
namespace {

using namespace test;
using namespace std::chrono_literals;

constexpr std::int64_t kT0 = 1'800'000'000'000;  // a fixed coordinator clock (ms since 1970)

core::Digest256 settings_digest(std::uint8_t seed) {
    return digest(static_cast<std::uint8_t>(seed + 100));
}

// A catalog with `count` hashed sources (one item each, ids 1 ... count) and a recipe revision
// on item 1.
class BatchJournal : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = open_or_fail(dir_ / L"lucent.catalog");
        ASSERT_NE(catalog_, nullptr);
        add_items(1, 12);
        const Status written = catalog_->write([&](WriteTransaction& tx) -> Status {
            NewRevision revision;
            revision.schema_name = "lucent.develop";
            revision.schema_version = 1;
            revision.settings_json = R"({"exposure_ev":0.7})";
            revision.created_at = at(kT0 - 1000);
            Expected<RevisionInfo> info = tx.append_revision(ItemId{1}, revision);
            if (!info) {
                return Unexpected(info.error());
            }
            revision_ = *info;
            return ok();
        });
        LUCENT_ASSERT_OK(written);
    }

    void add_items(std::uint64_t first, std::uint64_t count, bool hashed = true) {
        const Status written = catalog_->write([&](WriteTransaction& tx) -> Status {
            for (std::uint64_t n = first; n < first + count; ++n) {
                SourceObservation o = observation(dir_ / L"photos" / std::format(L"IMG_{:04}.CR3", n), n);
                if (hashed) {
                    o.sha256 = digest(static_cast<std::uint8_t>(n));
                }
                Expected<SourceUpsertResult> source = tx.upsert_source(o);
                if (!source) {
                    return Unexpected(source.error());
                }
                const ItemMetadata metadata = item();
                if (Expected<std::vector<ItemId>> ids = tx.upsert_items(source->id, std::span(&metadata, 1)); !ids) {
                    return Unexpected(ids.error());
                }
            }
            return ok();
        });
        LUCENT_ASSERT_OK(written);
    }

    static NewBatch batch_spec(bool start = true) {
        NewBatch batch;
        batch.name = "Iceland — picks";
        batch.selection_description = "12 picked photos";
        batch.destination_json = R"({"folder":"D:\\Exports","naming":"{original-stem}_Lucent_{revision}"})";
        batch.export_settings_json = R"({"format":"jpeg","quality":95,"profile":"sRGB"})";
        batch.policy_json = R"({"max_transient_attempts":3})";
        batch.renderer_version = "lucent-engine 0.7.0";
        batch.created_at = at(kT0);
        batch.start = start;
        return batch;
    }

    // Jobs for items first ... first + count - 1 (item 1 with its revision), as-shot otherwise.
    std::vector<NewJob> jobs_for(std::int64_t first, std::int64_t count) const {
        std::vector<NewJob> jobs;
        for (std::int64_t id = first; id < first + count; ++id) {
            NewJob job;
            job.item = ItemId{id};
            if (id == 1) {
                job.revision = revision_.id;
                job.settings_sha256 = revision_.settings_sha256;
            } else {
                job.settings_sha256 = settings_digest(0);
            }
            job.assets_json = R"({"camera_profile":"canon-eos-r5@1"})";
            jobs.push_back(job);
        }
        return jobs;
    }

    Expected<CreatedBatch> create(std::int64_t first, std::int64_t count, bool start = true) {
        const std::vector<NewJob> jobs = jobs_for(first, count);
        const NewBatch spec = batch_spec(start);
        return catalog_->transact([&](WriteTransaction& tx) { return tx.create_batch(spec, jobs); });
    }

    Expected<std::optional<ClaimedJob>> claim(std::string owner, std::int64_t now_ms,
                                              std::chrono::milliseconds lease = 30s) {
        ClaimRequest request;
        request.owner = std::move(owner);
        request.now = at(now_ms);
        request.lease_duration = lease;
        return catalog_->transact([&](WriteTransaction& tx) { return tx.claim_next_job(request); });
    }

    // Claims and asserts that a job was claimed.
    ClaimedJob must_claim(std::string owner, std::int64_t now_ms, std::chrono::milliseconds lease = 30s) {
        Expected<std::optional<ClaimedJob>> claimed = claim(std::move(owner), now_ms, lease);
        EXPECT_TRUE(claimed) << describe(claimed);
        EXPECT_TRUE(claimed && claimed->has_value());
        return claimed && claimed->has_value() ? **claimed : ClaimedJob{};
    }

    Expected<JobStage> advance(const JobLease& lease, JobStage from, std::int64_t now_ms) {
        return catalog_->transact([&](WriteTransaction& tx) { return tx.advance_job(lease, from, at(now_ms)); });
    }

    Status step(const JobLease& lease, PublicationStep s, std::int64_t now_ms, const std::string& name = "out") {
        PublicationEntry entry;
        entry.step = s;
        entry.final_path = dir_ / L"exports" / (name + ".jpg");
        if (s >= PublicationStep::TempWritten) {
            entry.temp_path = dir_ / L"exports" / (name + ".jpg.lucent-tmp");
        }
        if (s >= PublicationStep::Verified) {
            entry.output_sha256 = digest(200);
            entry.output_size = 4'321'000;
        }
        return catalog_->write(
            [&](WriteTransaction& tx) { return tx.record_publication_step(lease, entry, at(now_ms)); });
    }

    NewReceipt receipt_for(const std::string& name = "out") const {
        NewReceipt receipt;
        receipt.final_path = dir_ / L"exports" / (name + ".jpg");
        receipt.size_bytes = 4'321'000;
        receipt.output_sha256 = digest(200);
        receipt.settings_json = R"({"recipe":"frozen","export":"jpeg-95"})";
        return receipt;
    }

    Expected<ReceiptRecord> record_receipt(const JobLease& lease, std::int64_t now_ms,
                                           const NewReceipt& receipt) {
        return catalog_->transact([&](WriteTransaction& tx) { return tx.record_receipt(lease, receipt, at(now_ms)); });
    }

    // Drives a claimed job through rendering, verification and publication up to Published.
    void publish(const JobLease& lease, std::int64_t now_ms, const std::string& name = "out") {
        LUCENT_ASSERT_OK(step(lease, PublicationStep::Reserved, now_ms, name));
        LUCENT_ASSERT_OK(step(lease, PublicationStep::TempWritten, now_ms + 1, name));
        LUCENT_ASSERT_OK(advance(lease, JobStage::Rendering, now_ms + 2));
        LUCENT_ASSERT_OK(step(lease, PublicationStep::Verified, now_ms + 3, name));
        LUCENT_ASSERT_OK(advance(lease, JobStage::Verifying, now_ms + 4));
        LUCENT_ASSERT_OK(step(lease, PublicationStep::Published, now_ms + 5, name));
    }

    JobRecord job(JobId id) {
        Expected<std::optional<JobRecord>> found = catalog_->query([&](ReadTransaction& tx) { return tx.find_job(id); });
        EXPECT_TRUE(found && found->has_value()) << describe(found);
        return found && found->has_value() ? **found : JobRecord{};
    }

    BatchRecord batch(BatchId id) {
        Expected<std::optional<BatchRecord>> found =
            catalog_->query([&](ReadTransaction& tx) { return tx.find_batch(id); });
        EXPECT_TRUE(found && found->has_value()) << describe(found);
        return found && found->has_value() ? **found : BatchRecord{};
    }

    std::vector<JobEvent> events(JobId id) {
        Expected<std::vector<JobEvent>> list = catalog_->query([&](ReadTransaction& tx) {
            EventQuery query;
            query.job = id;
            return tx.list_job_events(query);
        });
        EXPECT_TRUE(list) << describe(list);
        return list ? *list : std::vector<JobEvent>{};
    }

    std::vector<PublicationInProgress> open_publications() {
        Expected<std::vector<PublicationInProgress>> list = catalog_->query(
            [&](ReadTransaction& tx) { return tx.list_publications_in_progress(std::nullopt, 100); });
        EXPECT_TRUE(list) << describe(list);
        return list ? *list : std::vector<PublicationInProgress>{};
    }

    template <class T>
    static void expect_error(const Expected<T>& result, ErrorCode code, std::string_view where) {
        ASSERT_FALSE(result) << "expected " << where;
        EXPECT_EQ(result.error().code, code) << result.error().describe();
        EXPECT_EQ(result.error().where, where) << result.error().describe();
    }

    TempDir dir_;
    std::unique_ptr<Catalog> catalog_;
    RevisionInfo revision_;
};

// ---------------------------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------------------------

TEST(JobIdentity, IsDeterministicAndCoversEveryInput) {
    JobIdentityInput base;
    base.source_sha256 = digest(1);
    base.subimage = 0;
    base.settings_sha256 = digest(2);
    base.renderer_version = "engine 1";
    base.assets_json = R"({"profile":"a"})";
    base.export_settings_sha256 = digest(3);
    Expected<core::Digest256> a = compute_job_identity(base);
    Expected<core::Digest256> b = compute_job_identity(base);
    LUCENT_ASSERT_OK(a);
    LUCENT_ASSERT_OK(b);
    EXPECT_EQ(*a, *b);

    std::vector<JobIdentityInput> variants(7, base);
    variants[0].source_sha256 = digest(9);
    variants[1].subimage = 1;
    variants[2].settings_sha256 = digest(9);
    variants[3].renderer_version = "engine 2";
    variants[4].assets_json = R"({"profile":"b"})";
    variants[5].export_settings_sha256 = digest(9);
    // Length prefixes keep field boundaries: moving bytes between two text fields changes it.
    variants[6].renderer_version = "engine 1{";
    variants[6].assets_json = R"("profile":"a"})";
    std::set<core::Digest256> seen{*a};
    for (const JobIdentityInput& variant : variants) {
        Expected<core::Digest256> identity = compute_job_identity(variant);
        LUCENT_ASSERT_OK(identity);
        EXPECT_TRUE(seen.insert(*identity).second) << core::to_hex(*identity);
    }
}

// ---------------------------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------------------------

TEST_F(BatchJournal, CreatesABatchAndItsJobsInOneCall) {
    Expected<CreatedBatch> created = create(1, 3, /*start=*/false);
    LUCENT_ASSERT_OK(created);
    ASSERT_EQ(created->jobs.size(), 3u);

    const BatchRecord record = batch(created->id);
    EXPECT_EQ(record.state, BatchState::Draft);
    EXPECT_EQ(record.name, "Iceland — picks");
    EXPECT_EQ(record.job_count, 3u);
    EXPECT_EQ(record.counts.queued, 3u);
    EXPECT_EQ(record.counts.total(), 3u);
    EXPECT_FALSE(record.started_at.has_value());
    Expected<core::Digest256> export_digest = core::sha256(
        std::as_bytes(std::span(record.export_settings_json.data(), record.export_settings_json.size())));
    LUCENT_ASSERT_OK(export_digest);
    EXPECT_EQ(record.export_settings_sha256, *export_digest);

    for (std::size_t i = 0; i < 3; ++i) {
        const JobRecord j = job(created->jobs[i]);
        EXPECT_EQ(j.batch, created->id);
        EXPECT_EQ(j.ordinal, i);
        EXPECT_EQ(j.item, ItemId{static_cast<std::int64_t>(i + 1)});
        EXPECT_EQ(j.stage, JobStage::Queued);
        EXPECT_EQ(j.attempts, 0u);
        EXPECT_FALSE(j.lease.has_value());
        EXPECT_FALSE(j.last_error.has_value());
        EXPECT_EQ(j.source_sha256, digest(static_cast<std::uint8_t>(i + 1)));
        JobIdentityInput input;
        input.source_sha256 = j.source_sha256;
        input.subimage = j.subimage;
        input.settings_sha256 = j.settings_sha256;
        input.renderer_version = record.renderer_version;
        input.assets_json = j.assets_json;
        input.export_settings_sha256 = record.export_settings_sha256;
        Expected<core::Digest256> identity = compute_job_identity(input);
        LUCENT_ASSERT_OK(identity);
        EXPECT_EQ(j.identity_sha256, *identity);
        const std::vector<JobEvent> history = events(j.id);
        ASSERT_EQ(history.size(), 1u);
        EXPECT_FALSE(history[0].from.has_value());
        EXPECT_EQ(history[0].to, JobStage::Queued);
    }
    EXPECT_EQ(job(created->jobs[0]).revision, revision_.id);
    EXPECT_FALSE(job(created->jobs[1]).revision.has_value());

    // A draft dispatches nothing until it is started.
    Expected<std::optional<ClaimedJob>> nothing = claim("worker-1", kT0 + 10);
    LUCENT_ASSERT_OK(nothing);
    EXPECT_FALSE(nothing->has_value());
    Expected<BatchState> started =
        catalog_->transact([&](WriteTransaction& tx) { return tx.start_batch(created->id, at(kT0 + 20)); });
    LUCENT_ASSERT_OK(started);
    EXPECT_EQ(*started, BatchState::Running);
    EXPECT_EQ(batch(created->id).started_at, at(kT0 + 20));
    const ClaimedJob first = must_claim("worker-1", kT0 + 30);
    EXPECT_EQ(first.job.id, created->jobs[0]);  // ordinal order
}

TEST_F(BatchJournal, CreationIsAllOrNothingAndValidated) {
    add_items(100, 1, /*hashed=*/false);  // item 13: its source is not hashed
    const auto count_rows = [&](std::string_view table) {
        std::unique_ptr<detail::Connection> raw = raw_connection(dir_ / L"lucent.catalog", detail::Access::ReadOnly);
        return raw ? query_int(*raw, std::format("SELECT count(*) FROM {}", table)) : -1;
    };
    const auto try_create = [&](std::vector<NewJob> jobs, NewBatch spec = batch_spec()) {
        return catalog_->transact([&](WriteTransaction& tx) { return tx.create_batch(spec, jobs); });
    };

    std::vector<NewJob> missing = jobs_for(1, 3);
    missing[2].item = ItemId{999};
    Expected<CreatedBatch> result = try_create(missing);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::NotFound);

    std::vector<NewJob> unhashed = jobs_for(1, 2);
    unhashed.push_back(jobs_for(13, 1)[0]);
    result = try_create(unhashed);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().where, "catalog.create_batch.unhashed");

    std::vector<NewJob> wrong_settings = jobs_for(1, 1);
    wrong_settings[0].settings_sha256 = settings_digest(5);  // not the revision's document
    result = try_create(wrong_settings);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);

    std::vector<NewJob> foreign_revision = jobs_for(2, 1);
    foreign_revision[0].revision = revision_.id;  // a revision of item 1
    foreign_revision[0].settings_sha256 = revision_.settings_sha256;
    result = try_create(foreign_revision);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);

    NewBatch bad_json = batch_spec();
    bad_json.destination_json = "{not json";
    result = try_create(jobs_for(1, 2), bad_json);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);

    std::vector<NewJob> bad_assets = jobs_for(1, 2);
    bad_assets[1].assets_json = "[1,";
    result = try_create(bad_assets);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);

    NewBatch no_renderer = batch_spec();
    no_renderer.renderer_version.clear();
    result = try_create(jobs_for(1, 1), no_renderer);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);

    NewBatch huge = batch_spec();
    huge.policy_json = std::format(R"({{"pad":"{}"}})", std::string(kMaxBatchJsonBytes, 'x'));
    result = try_create(jobs_for(1, 1), huge);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::LimitExceeded);

    result = try_create({});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);

    EXPECT_EQ(count_rows("batches"), 0);
    EXPECT_EQ(count_rows("jobs"), 0);
    EXPECT_EQ(count_rows("job_events"), 0);
}

// ---------------------------------------------------------------------------------------------
// The whole lifecycle
// ---------------------------------------------------------------------------------------------

TEST_F(BatchJournal, AJobGoesThroughEveryStageAndCompletesWithAReceipt) {
    Expected<CreatedBatch> created = create(1, 1);
    LUCENT_ASSERT_OK(created);
    const ClaimedJob claimed = must_claim("worker-1", kT0 + 100);
    EXPECT_EQ(claimed.lease.attempt, 1u);
    EXPECT_EQ(claimed.lease.owner, "worker-1");
    EXPECT_EQ(claimed.lease.expires_at, at(kT0 + 100 + 30'000));
    EXPECT_EQ(claimed.job.stage, JobStage::Rendering);
    ASSERT_TRUE(claimed.job.lease.has_value());
    EXPECT_EQ(*claimed.job.lease, claimed.lease);

    Expected<LeaseRenewal> renewal = catalog_->transact(
        [&](WriteTransaction& tx) { return tx.heartbeat_job(claimed.lease, at(kT0 + 10'000), 30s); });
    LUCENT_ASSERT_OK(renewal);
    EXPECT_EQ(renewal->expires_at, at(kT0 + 40'000));
    EXPECT_FALSE(renewal->cancel_requested);
    EXPECT_EQ(job(claimed.job.id).heartbeat_at, at(kT0 + 10'000));

    publish(claimed.lease, kT0 + 11'000);
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_EQ(job(claimed.job.id).stage, JobStage::Publishing);
    EXPECT_EQ(job(claimed.job.id).final_path, dir_ / L"exports" / L"out.jpg");
    Expected<ReceiptRecord> receipt = record_receipt(claimed.lease, kT0 + 12'000, receipt_for());
    LUCENT_ASSERT_OK(receipt);
    EXPECT_EQ(receipt->identity_sha256, claimed.job.identity_sha256);
    Expected<JobStage> complete = advance(claimed.lease, JobStage::Publishing, kT0 + 12'001);
    LUCENT_ASSERT_OK(complete);
    EXPECT_EQ(*complete, JobStage::Complete);

    const JobRecord done = job(claimed.job.id);
    EXPECT_EQ(done.stage, JobStage::Complete);
    EXPECT_FALSE(done.lease.has_value());
    const BatchRecord record = batch(created->id);
    EXPECT_EQ(record.state, BatchState::Completed);
    EXPECT_EQ(record.finished_at, at(kT0 + 12'001));
    EXPECT_EQ(record.counts.complete, 1u);

    std::vector<JobStage> stages;
    for (const JobEvent& event : events(claimed.job.id)) {
        stages.push_back(event.to);
    }
    EXPECT_EQ(stages, (std::vector<JobStage>{JobStage::Queued, JobStage::Rendering, JobStage::Verifying,
                                             JobStage::Publishing, JobStage::Complete}));

    Expected<std::vector<PublicationRecord>> journal =
        catalog_->query([&](ReadTransaction& tx) { return tx.publication_journal(claimed.job.id); });
    LUCENT_ASSERT_OK(journal);
    std::vector<PublicationStep> steps;
    for (const PublicationRecord& entry : *journal) {
        steps.push_back(entry.step);
        EXPECT_EQ(entry.attempt, 1u);
    }
    EXPECT_EQ(steps, (std::vector<PublicationStep>{PublicationStep::Reserved, PublicationStep::TempWritten,
                                                   PublicationStep::Verified, PublicationStep::Published,
                                                   PublicationStep::ReceiptCommitted}));
    EXPECT_TRUE(open_publications().empty());

    Expected<std::vector<ReceiptRecord>> same_identity = catalog_->query(
        [&](ReadTransaction& tx) { return tx.find_receipts_by_identity(claimed.job.identity_sha256); });
    LUCENT_ASSERT_OK(same_identity);
    ASSERT_EQ(same_identity->size(), 1u);
    EXPECT_EQ(same_identity->front().output_sha256, digest(200));
}

TEST_F(BatchJournal, NoJobBecomesCompleteWithoutAReceipt) {
    LUCENT_ASSERT_OK(create(1, 1));
    const ClaimedJob claimed = must_claim("worker-1", kT0);
    publish(claimed.lease, kT0 + 1);
    if (HasFatalFailure()) {
        return;
    }
    expect_error(advance(claimed.lease, JobStage::Publishing, kT0 + 10), ErrorCode::InvalidArgument,
                 "catalog.job.no_receipt");
    EXPECT_EQ(job(claimed.job.id).stage, JobStage::Publishing);

    // Nor through recovery of an abandoned lease.
    JobRecovery recovery;
    recovery.action = RecoveryAction::Complete;
    recovery.now = at(kT0 + 60'000);
    expect_error(catalog_->transact([&](WriteTransaction& tx) { return tx.recover_job(claimed.lease, recovery); }),
                 ErrorCode::InvalidArgument, "catalog.job.no_receipt");
    EXPECT_EQ(job(claimed.job.id).stage, JobStage::Publishing);

    // Direct SQL cannot give a job a lease in a terminal stage either.
    std::unique_ptr<detail::Connection> raw = raw_connection(dir_ / L"lucent.catalog");
    ASSERT_NE(raw, nullptr);
    EXPECT_FALSE(raw->exec(std::format("UPDATE jobs SET stage = 'complete' WHERE id = {}", claimed.job.id.value),
                           "test"));
}

TEST_F(BatchJournal, StageTransitionsAreValidated) {
    LUCENT_ASSERT_OK(create(1, 1));
    const ClaimedJob claimed = must_claim("worker-1", kT0);
    const JobLease& lease = claimed.lease;

    // Optimistic check: the caller's idea of the stage must be the job's.
    expect_error(advance(lease, JobStage::Verifying, kT0 + 1), ErrorCode::InvalidArgument,
                 "catalog.job.stage_conflict");
    expect_error(advance(lease, JobStage::Queued, kT0 + 1), ErrorCode::InvalidArgument,
                 "catalog.job.stage_conflict");

    // Journal order.
    expect_error(step(lease, PublicationStep::TempWritten, kT0 + 2), ErrorCode::InvalidArgument,
                 "catalog.job.journal_order");
    LUCENT_ASSERT_OK(step(lease, PublicationStep::Reserved, kT0 + 3));
    expect_error(step(lease, PublicationStep::Reserved, kT0 + 4), ErrorCode::InvalidArgument,
                 "catalog.job.journal_order");
    expect_error(step(lease, PublicationStep::Verified, kT0 + 4), ErrorCode::InvalidArgument,
                 "catalog.job.journal_order");
    LUCENT_ASSERT_OK(step(lease, PublicationStep::TempWritten, kT0 + 5));
    LUCENT_ASSERT_OK(step(lease, PublicationStep::Verified, kT0 + 6));
    // Published only while publishing.
    expect_error(step(lease, PublicationStep::Published, kT0 + 7), ErrorCode::InvalidArgument,
                 "catalog.job.stage_conflict");
    // The last two steps are written by the catalog itself.
    PublicationEntry committed;
    committed.step = PublicationStep::ReceiptCommitted;
    committed.final_path = dir_ / L"x.jpg";
    EXPECT_FALSE(catalog_->write(
        [&](WriteTransaction& tx) { return tx.record_publication_step(lease, committed, at(kT0 + 8)); }));
    // Missing fields.
    PublicationEntry incomplete;
    incomplete.step = PublicationStep::Reserved;
    EXPECT_FALSE(catalog_->write(
        [&](WriteTransaction& tx) { return tx.record_publication_step(lease, incomplete, at(kT0 + 8)); }));

    LUCENT_ASSERT_OK(advance(lease, JobStage::Rendering, kT0 + 9));
    LUCENT_ASSERT_OK(advance(lease, JobStage::Verifying, kT0 + 10));
    // A receipt before the output was published.
    expect_error(record_receipt(lease, kT0 + 11, receipt_for()), ErrorCode::InvalidArgument,
                 "catalog.job.journal_order");
    LUCENT_ASSERT_OK(step(lease, PublicationStep::Published, kT0 + 12));
    // A receipt that does not describe the published file.
    NewReceipt wrong = receipt_for();
    wrong.size_bytes += 1;
    Expected<ReceiptRecord> mismatch = record_receipt(lease, kT0 + 13, wrong);
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().code, ErrorCode::InvalidArgument);
    // Cancelling after publication is refused: the job finishes instead.
    EXPECT_FALSE(catalog_->write([&](WriteTransaction& tx) { return tx.cancel_job(lease, at(kT0 + 14)); }));
    LUCENT_ASSERT_OK(record_receipt(lease, kT0 + 15, receipt_for()));
    LUCENT_ASSERT_OK(advance(lease, JobStage::Publishing, kT0 + 16));
    // The lease ended with Complete.
    expect_error(advance(lease, JobStage::Complete, kT0 + 17), ErrorCode::InvalidArgument, "catalog.job.lease_lost");
    // skip_job only for queued jobs.
    const Status skipped = catalog_->write([&](WriteTransaction& tx) {
        return tx.skip_job(claimed.job.id, Error{ErrorCode::AlreadyExists, "exists", "test"}, at(kT0 + 18));
    });
    ASSERT_FALSE(skipped);
    EXPECT_EQ(skipped.error().where, "catalog.job.stage_conflict");
}

TEST_F(BatchJournal, ReceiptsAreIdempotent) {
    LUCENT_ASSERT_OK(create(1, 1));
    const ClaimedJob claimed = must_claim("worker-1", kT0);
    publish(claimed.lease, kT0 + 1);
    if (HasFatalFailure()) {
        return;
    }
    Expected<ReceiptRecord> first = record_receipt(claimed.lease, kT0 + 100, receipt_for());
    LUCENT_ASSERT_OK(first);
    Expected<ReceiptRecord> again = record_receipt(claimed.lease, kT0 + 200, receipt_for());
    LUCENT_ASSERT_OK(again);
    EXPECT_EQ(again->id, first->id);
    EXPECT_EQ(again->created_at, at(kT0 + 100));  // the stored receipt, unchanged

    NewReceipt different = receipt_for();
    different.settings_json = R"({"recipe":"other"})";
    expect_error(record_receipt(claimed.lease, kT0 + 300, different), ErrorCode::AlreadyExists,
                 "catalog.receipt.conflict");

    LUCENT_ASSERT_OK(advance(claimed.lease, JobStage::Publishing, kT0 + 400));
    // A replay after the lease ended (the worker's reply was lost) still returns the receipt.
    Expected<ReceiptRecord> replay = record_receipt(claimed.lease, kT0 + 500, receipt_for());
    LUCENT_ASSERT_OK(replay);
    EXPECT_EQ(replay->id, first->id);

    std::unique_ptr<detail::Connection> raw = raw_connection(dir_ / L"lucent.catalog");
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM receipts"), 1);
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM publication_journal WHERE step = 'receipt_committed'"), 1);
    EXPECT_FALSE(raw->exec("UPDATE receipts SET size_bytes = 1", "test"));
    EXPECT_FALSE(raw->exec("DELETE FROM receipts", "test"));
    EXPECT_FALSE(raw->exec(std::format("INSERT INTO receipts (job_id, identity_sha256, final_path, size_bytes, "
                                       "output_sha256, settings_json, created_at) SELECT job_id, identity_sha256, "
                                       "final_path, size_bytes, output_sha256, settings_json, 0 FROM receipts"),
                           "test"));  // one receipt per job
}

// ---------------------------------------------------------------------------------------------
// Leases
// ---------------------------------------------------------------------------------------------

TEST_F(BatchJournal, ConcurrentClaimsNeverLeaseAJobTwice) {
    add_items(13, 188);  // 200 items in all
    LUCENT_ASSERT_OK(create(1, 200));
    constexpr int kWorkers = 8;
    std::mutex mutex;
    std::map<std::int64_t, std::vector<std::string>> owners;  // job -> every owner that claimed it
    std::vector<std::string> errors;
    std::vector<std::thread> threads;
    for (int w = 0; w < kWorkers; ++w) {
        threads.emplace_back([&, w] {
            const std::string owner = std::format("worker-{}", w);
            for (;;) {
                ClaimRequest request;
                request.owner = owner;
                request.now = at(kT0 + 1);
                request.lease_duration = 60s;
                Expected<std::optional<ClaimedJob>> claimed =
                    catalog_->transact([&](WriteTransaction& tx) { return tx.claim_next_job(request); });
                std::lock_guard lock(mutex);
                if (!claimed) {
                    errors.push_back(claimed.error().describe());
                    return;
                }
                if (!claimed->has_value()) {
                    return;
                }
                owners[(*claimed)->job.id.value].push_back(owner);
                if ((*claimed)->lease.attempt != 1 || (*claimed)->lease.owner != owner) {
                    errors.push_back("unexpected lease");
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_TRUE(errors.empty()) << errors.front();
    EXPECT_EQ(owners.size(), 200u);
    for (const auto& [job_id, claimed_by] : owners) {
        EXPECT_EQ(claimed_by.size(), 1u) << "job " << job_id;
    }
    // The catalog agrees: every job leased once, by the owner that received it.
    Expected<std::vector<JobRecord>> jobs = catalog_->query([](ReadTransaction& tx) {
        JobQuery query;
        query.limit = 1000;
        return tx.list_jobs(query);
    });
    LUCENT_ASSERT_OK(jobs);
    ASSERT_EQ(jobs->size(), 200u);
    for (const JobRecord& j : *jobs) {
        EXPECT_EQ(j.stage, JobStage::Rendering);
        EXPECT_EQ(j.attempts, 1u);
        ASSERT_TRUE(j.lease.has_value());
        EXPECT_EQ(owners[j.id.value], std::vector<std::string>{j.lease->owner});
    }
    std::unique_ptr<detail::Connection> raw = raw_connection(dir_ / L"lucent.catalog", detail::Access::ReadOnly);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM job_events WHERE to_stage = 'rendering'"), 200);
}

TEST_F(BatchJournal, AnExpiredLeaseIsRecoveredAndTheStaleOwnerLocksOut) {
    LUCENT_ASSERT_OK(create(1, 1));
    const ClaimedJob stale = must_claim("worker-a", kT0, 10s);
    LUCENT_ASSERT_OK(step(stale.lease, PublicationStep::Reserved, kT0 + 1));
    LUCENT_ASSERT_OK(step(stale.lease, PublicationStep::TempWritten, kT0 + 2));

    const auto reclaimable = [&](std::int64_t now_ms) {
        Expected<std::vector<JobRecord>> list = catalog_->query([&](ReadTransaction& tx) {
            ReclaimQuery query;
            query.now = at(now_ms);
            return tx.list_reclaimable_jobs(query);
        });
        EXPECT_TRUE(list) << describe(list);
        return list ? list->size() : std::size_t{999};
    };
    EXPECT_EQ(reclaimable(kT0 + 9'999), 0u);
    EXPECT_EQ(reclaimable(kT0 + 10'000), 1u);  // expires_at <= now

    // A leased job is never claimed again, expired or not.
    Expected<std::optional<ClaimedJob>> none = claim("worker-b", kT0 + 20'000);
    LUCENT_ASSERT_OK(none);
    EXPECT_FALSE(none->has_value());
    // The stale owner cannot renew an expired lease nor change the job.
    expect_error(catalog_->transact([&](WriteTransaction& tx) {
        return tx.heartbeat_job(stale.lease, at(kT0 + 20'000), 10s);
    }),
                 ErrorCode::InvalidArgument, "catalog.job.lease_lost");
    expect_error(advance(stale.lease, JobStage::Rendering, kT0 + 20'000), ErrorCode::InvalidArgument,
                 "catalog.job.lease_lost");

    // Recovery refuses an unexpired lease unless the owner is known to be gone.
    JobRecovery early;
    early.now = at(kT0 + 5'000);
    expect_error(catalog_->transact([&](WriteTransaction& tx) { return tx.recover_job(stale.lease, early); }),
                 ErrorCode::InvalidArgument, "catalog.job.lease_lost");

    JobRecovery requeue;
    requeue.now = at(kT0 + 20'000);
    requeue.error = Error{ErrorCode::Timeout, "worker stopped responding", "workers.heartbeat"};
    Expected<JobStage> recovered =
        catalog_->transact([&](WriteTransaction& tx) { return tx.recover_job(stale.lease, requeue); });
    LUCENT_ASSERT_OK(recovered);
    EXPECT_EQ(*recovered, JobStage::Queued);
    JobRecord after = job(stale.job.id);
    EXPECT_EQ(after.stage, JobStage::Queued);
    EXPECT_FALSE(after.lease.has_value());
    EXPECT_EQ(after.transient_failures, 1u);
    ASSERT_TRUE(after.last_error.has_value());
    EXPECT_EQ(after.last_error->code, ErrorCode::Timeout);
    EXPECT_EQ(after.last_error->failure_class, FailureClass::Transient);
    EXPECT_EQ(reclaimable(kT0 + 20'000), 0u);
    // Recovering twice fails: the lease is gone.
    expect_error(catalog_->transact([&](WriteTransaction& tx) { return tx.recover_job(stale.lease, requeue); }),
                 ErrorCode::InvalidArgument, "catalog.job.lease_lost");

    // The interrupted publication of attempt 1 is still open until reconciled.
    std::vector<PublicationInProgress> open = open_publications();
    ASSERT_EQ(open.size(), 1u);
    EXPECT_EQ(open[0].latest.step, PublicationStep::TempWritten);
    EXPECT_EQ(open[0].latest.attempt, 1u);
    EXPECT_FALSE(open[0].attempt_is_live());

    // A new owner claims attempt 2; the stale lease stays locked out even if it were "fresh".
    const ClaimedJob fresh = must_claim("worker-b", kT0 + 21'000);
    EXPECT_EQ(fresh.lease.attempt, 2u);
    JobLease forged = stale.lease;
    forged.expires_at = at(kT0 + 99'000);
    expect_error(advance(forged, JobStage::Rendering, kT0 + 21'001), ErrorCode::InvalidArgument,
                 "catalog.job.lease_lost");
    JobLease wrong_owner = fresh.lease;
    wrong_owner.owner = "worker-a";
    expect_error(advance(wrong_owner, JobStage::Rendering, kT0 + 21'001), ErrorCode::InvalidArgument,
                 "catalog.job.lease_lost");
    // The stale owner's failure report is refused too.
    JobFailure failure;
    failure.error = Error{ErrorCode::IoError, "late", "test"};
    expect_error(catalog_->transact([&](WriteTransaction& tx) {
        return tx.record_failure(stale.lease, failure, at(kT0 + 21'002));
    }),
                 ErrorCode::InvalidArgument, "catalog.job.lease_lost");

    // Attempt 1's publication is reconciled while attempt 2 runs.
    LUCENT_ASSERT_OK(
        catalog_->write([&](WriteTransaction& tx) { return tx.abandon_publication(stale.job.id, 1, at(kT0 + 21'003)); }));
    EXPECT_TRUE(open_publications().empty());
    expect_error(
        catalog_->transact([&](WriteTransaction& tx) -> Expected<int> {
            if (Status s = tx.abandon_publication(stale.job.id, 1, at(kT0 + 21'004)); !s) {
                return Unexpected(s.error());
            }
            return 0;
        }),
        ErrorCode::InvalidArgument, "catalog.job.journal_order");
    publish(fresh.lease, kT0 + 22'000);
    if (HasFatalFailure()) {
        return;
    }
    LUCENT_ASSERT_OK(record_receipt(fresh.lease, kT0 + 23'000, receipt_for()));
    LUCENT_ASSERT_OK(advance(fresh.lease, JobStage::Publishing, kT0 + 23'001));
    EXPECT_EQ(job(stale.job.id).attempts, 2u);
}

TEST_F(BatchJournal, StartUpReclaimsLeasesOfAPreviousSessionBeforeTheyExpire) {
    LUCENT_ASSERT_OK(create(1, 2));
    const ClaimedJob a = must_claim("session-1/worker-0", kT0, 60s);
    const ClaimedJob b = must_claim("session-2/worker-0", kT0, 60s);
    Expected<std::vector<JobRecord>> list = catalog_->query([&](ReadTransaction& tx) {
        ReclaimQuery query;
        query.now = at(kT0 + 1'000);
        query.live_owners = std::vector<std::string>{"session-2/worker-0"};
        return tx.list_reclaimable_jobs(query);
    });
    LUCENT_ASSERT_OK(list);
    ASSERT_EQ(list->size(), 1u);
    EXPECT_EQ(list->front().id, a.job.id);
    ASSERT_TRUE(list->front().lease.has_value());

    Expected<std::vector<JobRecord>> everything = catalog_->query([&](ReadTransaction& tx) {
        ReclaimQuery query;
        query.now = at(kT0 + 1'000);
        query.live_owners = std::vector<std::string>{};  // a fresh start: no worker survives
        return tx.list_reclaimable_jobs(query);
    });
    LUCENT_ASSERT_OK(everything);
    EXPECT_EQ(everything->size(), 2u);

    JobRecovery recovery;
    recovery.now = at(kT0 + 1'000);
    recovery.owner_gone = true;
    Expected<JobStage> recovered = catalog_->transact(
        [&](WriteTransaction& tx) { return tx.recover_job(*list->front().lease, recovery); });
    LUCENT_ASSERT_OK(recovered);
    EXPECT_EQ(*recovered, JobStage::Queued);
    EXPECT_EQ(job(b.job.id).stage, JobStage::Rendering);  // the live owner keeps its job
}

TEST_F(BatchJournal, RecoveryCompletesAPublishedJobOnlyWithAMatchingReceipt) {
    LUCENT_ASSERT_OK(create(1, 1));
    const ClaimedJob claimed = must_claim("worker-1", kT0, 10s);
    publish(claimed.lease, kT0 + 1);  // crashed after publishing, before the receipt
    if (HasFatalFailure()) {
        return;
    }
    std::vector<PublicationInProgress> open = open_publications();
    ASSERT_EQ(open.size(), 1u);
    EXPECT_EQ(open[0].latest.step, PublicationStep::Published);
    EXPECT_TRUE(open[0].attempt_is_live());
    EXPECT_EQ(open[0].latest.output_sha256, digest(200));

    JobRecovery recovery;
    recovery.action = RecoveryAction::Complete;
    recovery.now = at(kT0 + 60'000);
    recovery.receipt = receipt_for();
    recovery.receipt->output_sha256 = digest(201);  // the file on disk is not what was published
    Expected<JobStage> refused =
        catalog_->transact([&](WriteTransaction& tx) { return tx.recover_job(claimed.lease, recovery); });
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidArgument);

    recovery.receipt = receipt_for();
    Expected<JobStage> completed =
        catalog_->transact([&](WriteTransaction& tx) { return tx.recover_job(claimed.lease, recovery); });
    LUCENT_ASSERT_OK(completed);
    EXPECT_EQ(*completed, JobStage::Complete);
    Expected<std::optional<ReceiptRecord>> receipt =
        catalog_->query([&](ReadTransaction& tx) { return tx.find_receipt(claimed.job.id); });
    LUCENT_ASSERT_OK(receipt);
    ASSERT_TRUE(receipt->has_value());
    EXPECT_EQ((*receipt)->final_path, dir_ / L"exports" / L"out.jpg");
    EXPECT_TRUE(open_publications().empty());
    EXPECT_EQ(job(claimed.job.id).transient_failures, 0u);  // completing is not a failure
}

// ---------------------------------------------------------------------------------------------
// Failures, retries, release, skip
// ---------------------------------------------------------------------------------------------

TEST_F(BatchJournal, FailuresRecordTheRetryPolicyInputs) {
    Expected<CreatedBatch> created = create(2, 1);
    LUCENT_ASSERT_OK(created);
    const ClaimedJob first = must_claim("worker-1", kT0);

    JobFailure transient;
    transient.failure_class = FailureClass::Transient;
    transient.error = Error{ErrorCode::DiskFull, "the destination is full", "export.write", 112};
    transient.retry_at = at(kT0 + 5'000);
    Expected<JobStage> stage =
        catalog_->transact([&](WriteTransaction& tx) { return tx.record_failure(first.lease, transient, at(kT0 + 1)); });
    LUCENT_ASSERT_OK(stage);
    EXPECT_EQ(*stage, JobStage::Queued);
    JobRecord j = job(first.job.id);
    EXPECT_EQ(j.not_before, at(kT0 + 5'000));
    EXPECT_EQ(j.transient_failures, 1u);
    EXPECT_EQ(j.deterministic_failures, 0u);
    ASSERT_TRUE(j.last_error.has_value());
    EXPECT_EQ(j.last_error->code, ErrorCode::DiskFull);
    EXPECT_EQ(j.last_error->where, "export.write");
    EXPECT_EQ(j.last_error->native_code, 112);
    EXPECT_EQ(j.last_error->at, at(kT0 + 1));

    // Back-off: not claimable before retry_at.
    Expected<std::optional<ClaimedJob>> early = claim("worker-1", kT0 + 4'999);
    LUCENT_ASSERT_OK(early);
    EXPECT_FALSE(early->has_value());
    const ClaimedJob second = must_claim("worker-1", kT0 + 5'000);
    EXPECT_EQ(second.lease.attempt, 2u);

    JobFailure deterministic;
    deterministic.failure_class = FailureClass::Deterministic;
    deterministic.error = Error{ErrorCode::Corrupt, std::string(10'000, 'e') + "\xC3\xA9", "raw.libraw.unpack"};
    stage = catalog_->transact(
        [&](WriteTransaction& tx) { return tx.record_failure(second.lease, deterministic, at(kT0 + 6'000)); });
    LUCENT_ASSERT_OK(stage);
    EXPECT_EQ(*stage, JobStage::Failed);
    j = job(first.job.id);
    EXPECT_EQ(j.attempts, 2u);
    EXPECT_EQ(j.transient_failures, 1u);
    EXPECT_EQ(j.deterministic_failures, 1u);
    ASSERT_TRUE(j.last_error.has_value());
    EXPECT_EQ(j.last_error->failure_class, FailureClass::Deterministic);
    EXPECT_EQ(j.last_error->message.size(), kMaxErrorTextBytes);  // truncated, still UTF-8
    EXPECT_TRUE(core::is_valid_utf8(j.last_error->message));

    BatchRecord record = batch(created->id);
    EXPECT_EQ(record.state, BatchState::Completed);  // every job is terminal
    EXPECT_EQ(record.counts.failed, 1u);

    // The report's events carry the failures.
    const std::vector<JobEvent> history = events(first.job.id);
    ASSERT_EQ(history.size(), 5u);  // queued, rendering, queued (retry), rendering, failed
    ASSERT_TRUE(history[2].error.has_value());
    EXPECT_EQ(history[2].error->code, ErrorCode::DiskFull);
    EXPECT_EQ(history[2].lease_owner, "worker-1");
    EXPECT_EQ(history[4].to, JobStage::Failed);

    // Retry on request reopens the batch.
    Expected<std::uint64_t> retried =
        catalog_->transact([&](WriteTransaction& tx) { return tx.retry_failed_jobs(created->id, at(kT0 + 7'000)); });
    LUCENT_ASSERT_OK(retried);
    EXPECT_EQ(*retried, 1u);
    record = batch(created->id);
    EXPECT_EQ(record.state, BatchState::Running);
    EXPECT_FALSE(record.finished_at.has_value());
    EXPECT_EQ(must_claim("worker-1", kT0 + 7'000).lease.attempt, 3u);
}

TEST_F(BatchJournal, ReleaseAndSkip) {
    Expected<CreatedBatch> created = create(2, 2);
    LUCENT_ASSERT_OK(created);
    const ClaimedJob claimed = must_claim("worker-1", kT0);
    Expected<JobStage> released =
        catalog_->transact([&](WriteTransaction& tx) { return tx.release_job(claimed.lease, at(kT0 + 10)); });
    LUCENT_ASSERT_OK(released);
    EXPECT_EQ(*released, JobStage::Queued);
    const JobRecord j = job(claimed.job.id);
    EXPECT_EQ(j.transient_failures + j.deterministic_failures, 0u);
    EXPECT_FALSE(j.last_error.has_value());

    LUCENT_ASSERT_OK(catalog_->write([&](WriteTransaction& tx) {
        return tx.skip_job(created->jobs[1], Error{ErrorCode::AlreadyExists, "output exists", "export.plan"},
                           at(kT0 + 20));
    }));
    const JobRecord skipped = job(created->jobs[1]);
    EXPECT_EQ(skipped.stage, JobStage::Skipped);
    ASSERT_TRUE(skipped.last_error.has_value());
    EXPECT_FALSE(skipped.last_error->failure_class.has_value());
    EXPECT_EQ(skipped.last_error->code, ErrorCode::AlreadyExists);

    const ClaimedJob again = must_claim("worker-2", kT0 + 30);
    EXPECT_EQ(again.job.id, claimed.job.id);
    EXPECT_EQ(again.lease.attempt, 2u);
}

// ---------------------------------------------------------------------------------------------
// Batch commands
// ---------------------------------------------------------------------------------------------

TEST_F(BatchJournal, PauseResumeAndCancel) {
    Expected<CreatedBatch> created = create(1, 4);
    LUCENT_ASSERT_OK(created);
    const BatchId id = created->id;
    const auto command = [&](auto member, std::int64_t now_ms) {
        return catalog_->transact([&](WriteTransaction& tx) { return (tx.*member)(id, at(now_ms)); });
    };
    const ClaimedJob running = must_claim("worker-1", kT0);

    LUCENT_ASSERT_OK(command(&WriteTransaction::pause_batch, kT0 + 1));
    Expected<BatchState> again = command(&WriteTransaction::pause_batch, kT0 + 2);  // idempotent
    LUCENT_ASSERT_OK(again);
    EXPECT_EQ(*again, BatchState::Paused);
    Expected<std::optional<ClaimedJob>> none = claim("worker-2", kT0 + 3);
    LUCENT_ASSERT_OK(none);
    EXPECT_FALSE(none->has_value());
    // The leased job finishes its current safe stage.
    LUCENT_ASSERT_OK(advance(running.lease, JobStage::Rendering, kT0 + 4));
    expect_error(command(&WriteTransaction::start_batch, kT0 + 5), ErrorCode::InvalidArgument,
                 "catalog.batch.invalid_transition");

    LUCENT_ASSERT_OK(command(&WriteTransaction::resume_batch, kT0 + 6));
    const ClaimedJob second = must_claim("worker-2", kT0 + 7);
    EXPECT_EQ(second.job.ordinal, 1u);

    Expected<BatchState> cancelled = command(&WriteTransaction::cancel_batch, kT0 + 8);
    LUCENT_ASSERT_OK(cancelled);
    EXPECT_EQ(*cancelled, BatchState::Cancelled);
    BatchRecord record = batch(id);
    EXPECT_EQ(record.counts.cancelled, 2u);
    EXPECT_EQ(record.counts.leased(), 2u);
    EXPECT_FALSE(record.finished_at.has_value());  // two jobs are still leased
    none = claim("worker-3", kT0 + 9);
    LUCENT_ASSERT_OK(none);
    EXPECT_FALSE(none->has_value());

    // Leased jobs learn of it from their heartbeat and cannot move on before publication.
    Expected<LeaseRenewal> renewal = catalog_->transact(
        [&](WriteTransaction& tx) { return tx.heartbeat_job(running.lease, at(kT0 + 10), 30s); });
    LUCENT_ASSERT_OK(renewal);
    EXPECT_TRUE(renewal->cancel_requested);
    expect_error(advance(running.lease, JobStage::Verifying, kT0 + 11), ErrorCode::Cancelled,
                 "catalog.job.batch_cancelled");
    LUCENT_ASSERT_OK(catalog_->write([&](WriteTransaction& tx) { return tx.cancel_job(running.lease, at(kT0 + 12)); }));
    // A requeue of the other becomes a cancellation.
    Expected<JobStage> released =
        catalog_->transact([&](WriteTransaction& tx) { return tx.release_job(second.lease, at(kT0 + 13)); });
    LUCENT_ASSERT_OK(released);
    EXPECT_EQ(*released, JobStage::Cancelled);

    record = batch(id);
    EXPECT_EQ(record.state, BatchState::Cancelled);
    EXPECT_EQ(record.counts.cancelled, 4u);
    EXPECT_EQ(record.finished_at, at(kT0 + 13));
    expect_error(command(&WriteTransaction::resume_batch, kT0 + 14), ErrorCode::InvalidArgument,
                 "catalog.batch.invalid_transition");
    expect_error(catalog_->transact([&](WriteTransaction& tx) { return tx.retry_failed_jobs(id, at(kT0 + 15)); }),
                 ErrorCode::InvalidArgument, "catalog.batch.invalid_transition");
    expect_error(catalog_->transact([](WriteTransaction& tx) { return tx.cancel_batch(BatchId{999}, at(0)); }),
                 ErrorCode::NotFound, "catalog.cancel_batch");
}

TEST_F(BatchJournal, ACompletedBatchCannotBeCancelled) {
    Expected<CreatedBatch> created = create(2, 1);
    LUCENT_ASSERT_OK(created);
    LUCENT_ASSERT_OK(catalog_->write([&](WriteTransaction& tx) {
        return tx.skip_job(created->jobs[0], Error{ErrorCode::NotFound, "source missing", "app"}, at(kT0));
    }));
    EXPECT_EQ(batch(created->id).state, BatchState::Completed);
    expect_error(
        catalog_->transact([&](WriteTransaction& tx) { return tx.cancel_batch(created->id, at(kT0 + 1)); }),
        ErrorCode::InvalidArgument, "catalog.batch.invalid_transition");
}

// ---------------------------------------------------------------------------------------------
// Reports and integrity
// ---------------------------------------------------------------------------------------------

TEST_F(BatchJournal, ListsBatchesJobsAndEventsForReports) {
    Expected<CreatedBatch> one = create(1, 3);
    Expected<CreatedBatch> two = create(4, 2);
    LUCENT_ASSERT_OK(one);
    LUCENT_ASSERT_OK(two);
    Expected<std::vector<BatchRecord>> batches =
        catalog_->query([](ReadTransaction& tx) { return tx.list_batches(10); });
    LUCENT_ASSERT_OK(batches);
    ASSERT_EQ(batches->size(), 2u);
    EXPECT_EQ(batches->at(0).id, two->id);  // newest first
    EXPECT_EQ(batches->at(0).counts.queued, 2u);
    Expected<std::vector<BatchRecord>> older =
        catalog_->query([&](ReadTransaction& tx) { return tx.list_batches(10, two->id); });
    LUCENT_ASSERT_OK(older);
    ASSERT_EQ(older->size(), 1u);
    EXPECT_EQ(older->front().id, one->id);

    const ClaimedJob claimed = must_claim("worker-1", kT0);  // the oldest batch first
    EXPECT_EQ(claimed.job.batch, one->id);
    Expected<std::vector<JobRecord>> queued = catalog_->query([&](ReadTransaction& tx) {
        JobQuery query;
        query.batch = one->id;
        query.stages = {JobStage::Queued};
        return tx.list_jobs(query);
    });
    LUCENT_ASSERT_OK(queued);
    EXPECT_EQ(queued->size(), 2u);
    Expected<std::vector<JobRecord>> paged = catalog_->query([&](ReadTransaction& tx) {
        JobQuery query;
        query.after = one->jobs[2];
        query.limit = 1;
        return tx.list_jobs(query);
    });
    LUCENT_ASSERT_OK(paged);
    ASSERT_EQ(paged->size(), 1u);
    EXPECT_EQ(paged->front().id, two->jobs[0]);
    Expected<std::vector<JobEvent>> batch_events = catalog_->query([&](ReadTransaction& tx) {
        EventQuery query;
        query.batch = one->id;
        return tx.list_job_events(query);
    });
    LUCENT_ASSERT_OK(batch_events);
    EXPECT_EQ(batch_events->size(), 4u);  // three creations and one claim
    EXPECT_FALSE(catalog_->query([](ReadTransaction& tx) { return tx.list_batches(0); }));
}

TEST_F(BatchJournal, JobsSurviveTheRemovalOfTheirItem) {
    Expected<CreatedBatch> created = create(1, 2);
    LUCENT_ASSERT_OK(created);
    const JobRecord before = job(created->jobs[0]);
    // Removing item 1's source removes the item and its revisions; the job keeps its record.
    LUCENT_ASSERT_OK(catalog_->write([](WriteTransaction& tx) { return tx.remove_source(SourceId{1}); }));
    const JobRecord after = job(created->jobs[0]);
    EXPECT_FALSE(after.item.has_value());
    EXPECT_FALSE(after.revision.has_value());
    EXPECT_EQ(after.identity_sha256, before.identity_sha256);
    EXPECT_EQ(after.settings_sha256, before.settings_sha256);
    EXPECT_EQ(events(created->jobs[0]).size(), 1u);
    std::unique_ptr<detail::Connection> raw = raw_connection(dir_ / L"lucent.catalog", detail::Access::ReadOnly);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM pragma_foreign_key_check"), 0);
}

TEST_F(BatchJournal, EventsJournalAndIdentitiesAreAppendOnly) {
    LUCENT_ASSERT_OK(create(1, 1));
    const ClaimedJob claimed = must_claim("worker-1", kT0);
    LUCENT_ASSERT_OK(step(claimed.lease, PublicationStep::Reserved, kT0 + 1));
    std::unique_ptr<detail::Connection> raw = raw_connection(dir_ / L"lucent.catalog");
    ASSERT_NE(raw, nullptr);
    EXPECT_FALSE(raw->exec("UPDATE job_events SET to_stage = 'complete'", "test"));
    EXPECT_FALSE(raw->exec("DELETE FROM job_events", "test"));
    EXPECT_FALSE(raw->exec("UPDATE publication_journal SET step = 'published'", "test"));
    EXPECT_FALSE(raw->exec("DELETE FROM publication_journal", "test"));
    EXPECT_FALSE(raw->exec("UPDATE jobs SET identity_sha256 = zeroblob(32)", "test"));
    EXPECT_FALSE(raw->exec("UPDATE jobs SET settings_sha256 = zeroblob(32)", "test"));
    EXPECT_FALSE(raw->exec("UPDATE jobs SET lease_owner = NULL, lease_expires_at = NULL", "test"));
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM job_events"), 2);
}

TEST_F(BatchJournal, RejectsInvalidRequests) {
    LUCENT_ASSERT_OK(create(1, 1));
    ClaimRequest request;
    request.now = at(kT0);
    EXPECT_FALSE(catalog_->transact([&](WriteTransaction& tx) { return tx.claim_next_job(request); }));  // no owner
    request.owner = std::string(kMaxJobLabelBytes + 1, 'w');
    EXPECT_FALSE(catalog_->transact([&](WriteTransaction& tx) { return tx.claim_next_job(request); }));
    request.owner = "worker";
    request.lease_duration = 10ms;
    EXPECT_FALSE(catalog_->transact([&](WriteTransaction& tx) { return tx.claim_next_job(request); }));
    request.lease_duration = std::chrono::hours(2);
    EXPECT_FALSE(catalog_->transact([&](WriteTransaction& tx) { return tx.claim_next_job(request); }));

    JobLease unknown{JobId{999}, "worker", 1, at(kT0 + 1'000)};
    expect_error(advance(unknown, JobStage::Rendering, kT0), ErrorCode::NotFound, "catalog.advance_job");
    EXPECT_EQ(to_string(JobStage::Publishing), "publishing");
    EXPECT_EQ(to_string(PublicationStep::TempWritten), "temp_written");
    EXPECT_EQ(to_string(static_cast<BatchState>(99)), "invalid");
}

TEST_F(BatchJournal, AnUnknownStageWrittenByANewerLucentIsReported) {
    Expected<CreatedBatch> created = create(1, 1);
    LUCENT_ASSERT_OK(created);
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(dir_ / L"lucent.catalog");
        ASSERT_NE(raw, nullptr);
        LUCENT_ASSERT_OK(raw->exec("UPDATE jobs SET stage = 'uploading'", "test"));
    }
    Expected<std::optional<JobRecord>> found =
        catalog_->query([&](ReadTransaction& tx) { return tx.find_job(created->jobs[0]); });
    ASSERT_FALSE(found);
    EXPECT_EQ(found.error().code, ErrorCode::Corrupt);
    Expected<std::optional<BatchRecord>> listed =
        catalog_->query([&](ReadTransaction& tx) { return tx.find_batch(created->id); });
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().code, ErrorCode::Corrupt);
}

}  // namespace
}  // namespace lucent::catalog
