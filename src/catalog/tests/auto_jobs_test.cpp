// Automatic settings in the job journal (schema v5): automatic batches and their job states
// (Pending -> Analysed -> Frozen), the claim filter of the related-shot pre-pass, the one-time
// rebinding of an automatic job's identity to its effective revision, analyse-only completion
// without a receipt, the analysis records of automatic revisions, and the v4 -> v5 migration.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "lucent/catalog/catalog.hpp"
#include "lucent/catalog/jobs.hpp"

#include "catalog_impl.hpp"
#include "catalog_test_support.hpp"
#include "schema.hpp"

namespace lucent::catalog {
namespace {

using namespace test;
using namespace std::chrono_literals;

constexpr std::int64_t kT0 = 1'800'000'000'000;

// A catalog with `count` hashed sources (one item each, ids 1 ... count) and one User revision on
// item 1 (the recipe an automatic job starts from: its locks).
class AutoJournal : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = open_or_fail(dir_ / L"lucent.catalog");
        ASSERT_NE(catalog_, nullptr);
        const Status written = catalog_->write([&](WriteTransaction& tx) -> Status {
            for (std::uint64_t n = 1; n <= 4; ++n) {
                SourceObservation o = observation(dir_ / L"photos" / std::format(L"IMG_{:04}.CR3", n), n);
                o.sha256 = digest(static_cast<std::uint8_t>(n));
                Expected<SourceUpsertResult> source = tx.upsert_source(o);
                if (!source) {
                    return Unexpected(source.error());
                }
                const ItemMetadata metadata = item();
                if (auto ids = tx.upsert_items(source->id, std::span(&metadata, 1)); !ids) {
                    return Unexpected(ids.error());
                }
            }
            Expected<RevisionInfo> info = tx.append_revision(ItemId{1}, revision(RevisionOrigin::User, 0.0));
            if (!info) {
                return Unexpected(info.error());
            }
            base_ = *info;
            return ok();
        });
        LUCENT_ASSERT_OK(written);
    }

    static NewRevision revision(RevisionOrigin origin, double exposure) {
        NewRevision r;
        r.origin = origin;
        r.schema_name = "lucent.develop";
        r.schema_version = 1;
        r.settings_json = std::format(R"({{"exposure_ev":{},"locks":["contrast"]}})", exposure);
        r.created_at = at(kT0 - 500);
        return r;
    }

    static NewBatch batch_spec(AutoMode mode) {
        NewBatch batch;
        batch.name = "Automatic";
        batch.selection_description = "4 photos";
        batch.destination_json = R"({"folder":"D:\\Exports"})";
        batch.export_settings_json = R"({"format":"jpeg","quality":95})";
        batch.policy_json = "{}";
        batch.renderer_version = "lucent-engine 0.8.0";
        batch.created_at = at(kT0);
        batch.start = true;
        batch.automatic = mode;
        return batch;
    }

    Expected<CreatedBatch> create(AutoMode mode, std::int64_t count = 4) {
        std::vector<NewJob> jobs;
        for (std::int64_t id = 1; id <= count; ++id) {
            NewJob job;
            job.item = ItemId{id};
            if (id == 1) {
                job.revision = base_.id;
                job.settings_sha256 = base_.settings_sha256;
            } else {
                job.settings_sha256 = digest(200);
            }
            job.assets_json = "{}";
            jobs.push_back(job);
        }
        const NewBatch spec = batch_spec(mode);
        return catalog_->transact([&](WriteTransaction& tx) { return tx.create_batch(spec, jobs); });
    }

    Expected<std::optional<ClaimedJob>> claim(std::vector<AutoJobState> states = {}, std::int64_t now = kT0) {
        ClaimRequest request;
        request.owner = "test/slot0";
        request.now = at(now);
        request.lease_duration = 60s;
        request.auto_states = std::move(states);
        return catalog_->transact([&](WriteTransaction& tx) { return tx.claim_next_job(request); });
    }

    JobRecord job(JobId id) const {
        auto found = catalog_->query([&](ReadTransaction& tx) { return tx.find_job(id); });
        EXPECT_TRUE(found && *found);
        return found && *found ? **found : JobRecord{};
    }

    // Appends an Auto revision to `item` (derived from its current one) and returns it.
    RevisionInfo add_auto_revision(ItemId item, double exposure) {
        auto info = catalog_->transact(
            [&](WriteTransaction& tx) { return tx.append_revision(item, revision(RevisionOrigin::Auto, exposure)); });
        EXPECT_TRUE(info) << describe(info);
        return info ? *info : RevisionInfo{};
    }

    static NewAutoAnalysis analysis_for(RevisionId revision, std::optional<JobId> job = std::nullopt) {
        NewAutoAnalysis a;
        a.revision = revision;
        a.job = job;
        a.policy = "natural";
        a.policy_version = 2;
        a.search_version = 2;
        a.metric_contract_version = 2;
        a.consistency_version = 2;
        a.develop_pipeline_version = 1;
        a.develop_settings_version = 1;
        a.attempt = 0;
        a.final_check = AutoCheckOutcome::Passed;
        a.confidence = 0.7;
        a.review = true;
        a.flags_json = R"([{"code":5,"name":"white_balance_uncertain","reason":"estimators disagree"}])";
        a.record_json = R"({"schema":"lucent.auto-record","version":1})";
        a.created_at = at(kT0 + 10);
        return a;
    }

    TempDir dir_;
    std::unique_ptr<Catalog> catalog_;
    RevisionInfo base_;
};

TEST_F(AutoJournal, AutomaticBatchesStartEveryJobPending) {
    auto created = create(AutoMode::Auto);
    LUCENT_ASSERT_OK(created);
    auto batch = catalog_->query([&](ReadTransaction& tx) { return tx.find_batch(created->id); });
    LUCENT_ASSERT_OK(batch);
    ASSERT_TRUE(batch->has_value());
    EXPECT_EQ((*batch)->automatic, AutoMode::Auto);
    for (const JobId id : created->jobs) {
        const JobRecord record = job(id);
        EXPECT_EQ(record.auto_state, AutoJobState::Pending);
        EXPECT_FALSE(record.analysis_json.has_value());
    }
    auto counts = catalog_->query([&](ReadTransaction& tx) { return tx.auto_job_counts(created->id); });
    LUCENT_ASSERT_OK(counts);
    EXPECT_EQ(*counts, (AutoJobCounts{4, 0, 0}));

    // An export batch has no automatic state.
    auto plain = create(AutoMode::None, 1);
    LUCENT_ASSERT_OK(plain);
    EXPECT_EQ(job(plain->jobs[0]).auto_state, AutoJobState::None);
    auto plain_batch = catalog_->query([&](ReadTransaction& tx) { return tx.find_batch(plain->id); });
    EXPECT_EQ((*plain_batch)->automatic, AutoMode::None);
    auto none = catalog_->query([&](ReadTransaction& tx) { return tx.auto_job_counts(BatchId{999}); });
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().code, ErrorCode::NotFound);
}

TEST_F(AutoJournal, AnalysisMovesAJobForwardOnceAndIsDurable) {
    auto created = create(AutoMode::Auto);
    LUCENT_ASSERT_OK(created);
    auto claimed = claim({AutoJobState::Pending});
    LUCENT_ASSERT_OK(claimed);
    ASSERT_TRUE(claimed->has_value());
    const JobLease lease = (*claimed)->lease;

    // Not valid JSON: refused, nothing changes.
    Status bad = catalog_->write([&](WriteTransaction& tx) { return tx.record_job_analysis(lease, "{oops", at(kT0 + 1)); });
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(job(lease.job).auto_state, AutoJobState::Pending);

    const std::string document = R"({"schema":"lucent.auto-analysis","version":1,"shot":{"camera":"x"}})";
    LUCENT_ASSERT_OK(
        catalog_->write([&](WriteTransaction& tx) { return tx.record_job_analysis(lease, document, at(kT0 + 2)); }));
    JobRecord record = job(lease.job);
    EXPECT_EQ(record.auto_state, AutoJobState::Analysed);
    ASSERT_TRUE(record.analysis_json);
    EXPECT_EQ(*record.analysis_json, document);

    // Once only.
    Status again =
        catalog_->write([&](WriteTransaction& tx) { return tx.record_job_analysis(lease, document, at(kT0 + 3)); });
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().where, "catalog.job.auto_state");

    // Nothing is published before the job is frozen.
    PublicationEntry reserved;
    reserved.step = PublicationStep::Reserved;
    reserved.final_path = dir_ / L"out" / L"IMG_0001_Lucent_2.jpg";
    Status published =
        catalog_->write([&](WriteTransaction& tx) { return tx.record_publication_step(lease, reserved, at(kT0 + 4)); });
    ASSERT_FALSE(published);
    EXPECT_EQ(published.error().where, "catalog.job.auto_state");

    // Given back between the states (the related-shot pre-pass): the analysis stays.
    LUCENT_ASSERT_OK(catalog_->transact([&](WriteTransaction& tx) { return tx.release_job(lease, at(kT0 + 5)); }));
    record = job(lease.job);
    EXPECT_EQ(record.stage, JobStage::Queued);
    EXPECT_EQ(record.auto_state, AutoJobState::Analysed);
    EXPECT_EQ(*record.analysis_json, document);
    auto counts = catalog_->query([&](ReadTransaction& tx) { return tx.auto_job_counts(created->id); });
    EXPECT_EQ(*counts, (AutoJobCounts{3, 1, 0, 1}));
}

TEST_F(AutoJournal, ClaimsFollowTheRequestedAutomaticStates) {
    auto created = create(AutoMode::Auto, 3);
    LUCENT_ASSERT_OK(created);
    // Analyse the first job and give it back.
    auto first = claim({AutoJobState::Pending});
    ASSERT_TRUE(first && *first);
    EXPECT_EQ((*first)->job.id, created->jobs[0]);
    LUCENT_ASSERT_OK(catalog_->write(
        [&](WriteTransaction& tx) { return tx.record_job_analysis((*first)->lease, "{}", at(kT0 + 1)); }));
    LUCENT_ASSERT_OK(
        catalog_->transact([&](WriteTransaction& tx) { return tx.release_job((*first)->lease, at(kT0 + 2)); }));

    // Pending jobs only: the analysed first job is passed over.
    auto pending = claim({AutoJobState::Pending}, kT0 + 3);
    ASSERT_TRUE(pending && *pending);
    EXPECT_EQ((*pending)->job.id, created->jobs[1]);
    // Analysed jobs only: the first one.
    auto analysed = claim({AutoJobState::Analysed}, kT0 + 4);
    ASSERT_TRUE(analysed && *analysed);
    EXPECT_EQ((*analysed)->job.id, created->jobs[0]);
    // Frozen jobs only: none.
    auto frozen = claim({AutoJobState::Frozen}, kT0 + 5);
    LUCENT_ASSERT_OK(frozen);
    EXPECT_FALSE(frozen->has_value());
    // Jobs of export batches only: none in this batch.
    auto plain = claim({AutoJobState::None}, kT0 + 6);
    LUCENT_ASSERT_OK(plain);
    EXPECT_FALSE(plain->has_value());
    // Listing by automatic state (the coordinator's view of the pre-pass).
    JobQuery query;
    query.batch = created->id;
    query.stages = {JobStage::Queued};
    query.auto_states = {AutoJobState::Pending};
    auto queued = catalog_->query([&](ReadTransaction& tx) { return tx.list_jobs(query); });
    LUCENT_ASSERT_OK(queued);
    ASSERT_EQ(queued->size(), 1u);
    EXPECT_EQ((*queued)[0].id, created->jobs[2]);
    query.auto_states = {AutoJobState::None};
    queued = catalog_->query([&](ReadTransaction& tx) { return tx.list_jobs(query); });
    LUCENT_ASSERT_OK(queued);
    EXPECT_TRUE(queued->empty());
    // Any state: the third.
    auto any = claim({}, kT0 + 7);
    ASSERT_TRUE(any && *any);
    EXPECT_EQ((*any)->job.id, created->jobs[2]);
}

TEST_F(AutoJournal, FreezingRebindsTheIdentityToTheEffectiveRevisionOnce) {
    auto created = create(AutoMode::Auto, 1);
    LUCENT_ASSERT_OK(created);
    auto claimed = claim();
    ASSERT_TRUE(claimed && *claimed);
    const JobLease lease = (*claimed)->lease;
    const JobRecord before = job(lease.job);
    const RevisionInfo effective = add_auto_revision(ItemId{1}, 0.4);

    AutoFreeze freeze;
    freeze.revision = effective.id;
    freeze.settings_sha256 = effective.settings_sha256;
    // Not analysed yet.
    auto early = catalog_->transact([&](WriteTransaction& tx) { return tx.freeze_auto_job(lease, freeze, at(kT0 + 1)); });
    ASSERT_FALSE(early);
    EXPECT_EQ(early.error().where, "catalog.job.auto_state");
    LUCENT_ASSERT_OK(
        catalog_->write([&](WriteTransaction& tx) { return tx.record_job_analysis(lease, "{}", at(kT0 + 2)); }));

    // A User revision, another item's revision, or other settings are refused.
    AutoFreeze user = freeze;
    user.revision = base_.id;
    user.settings_sha256 = base_.settings_sha256;
    auto refused = catalog_->transact([&](WriteTransaction& tx) { return tx.freeze_auto_job(lease, user, at(kT0 + 3)); });
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidArgument);
    const RevisionInfo other = add_auto_revision(ItemId{2}, 0.4);
    AutoFreeze foreign = freeze;
    foreign.revision = other.id;
    foreign.settings_sha256 = other.settings_sha256;
    EXPECT_FALSE(catalog_->transact([&](WriteTransaction& tx) { return tx.freeze_auto_job(lease, foreign, at(kT0 + 3)); }));
    AutoFreeze mismatched = freeze;
    mismatched.settings_sha256 = digest(77);
    EXPECT_FALSE(
        catalog_->transact([&](WriteTransaction& tx) { return tx.freeze_auto_job(lease, mismatched, at(kT0 + 3)); }));

    freeze.analysis_json = R"({"final":true})";
    auto identity = catalog_->transact([&](WriteTransaction& tx) { return tx.freeze_auto_job(lease, freeze, at(kT0 + 4)); });
    LUCENT_ASSERT_OK(identity);
    const JobRecord after = job(lease.job);
    EXPECT_EQ(after.auto_state, AutoJobState::Frozen);
    ASSERT_TRUE(after.revision);
    EXPECT_EQ(*after.revision, effective.id);
    EXPECT_EQ(after.settings_sha256, effective.settings_sha256);
    EXPECT_EQ(after.identity_sha256, *identity);
    EXPECT_NE(after.identity_sha256, before.identity_sha256);
    EXPECT_EQ(*after.analysis_json, R"({"final":true})");
    // The new identity is the job identity of the effective revision's settings.
    auto batch = catalog_->query([&](ReadTransaction& tx) { return tx.find_batch(created->id); });
    JobIdentityInput input;
    input.source_sha256 = before.source_sha256;
    input.subimage = before.subimage;
    input.settings_sha256 = effective.settings_sha256;
    input.renderer_version = (*batch)->renderer_version;
    input.assets_json = before.assets_json;
    input.export_settings_sha256 = (*batch)->export_settings_sha256;
    EXPECT_EQ(*compute_job_identity(input), after.identity_sha256);

    // Once only, and the identity stays immutable afterwards.
    auto twice = catalog_->transact([&](WriteTransaction& tx) { return tx.freeze_auto_job(lease, freeze, at(kT0 + 5)); });
    ASSERT_FALSE(twice);
    EXPECT_EQ(twice.error().where, "catalog.job.auto_state");
    std::unique_ptr<detail::Connection> raw = raw_connection(dir_ / L"lucent.catalog");
    ASSERT_NE(raw, nullptr);
    EXPECT_FALSE(raw->exec(std::format("UPDATE jobs SET identity_sha256 = zeroblob(32) WHERE id = {}", lease.job.value),
                           "test"));
    EXPECT_FALSE(raw->exec(std::format("UPDATE jobs SET auto_state = 'pending' WHERE id = {}", lease.job.value), "test"));

    // Frozen: publication may begin.
    PublicationEntry reserved;
    reserved.step = PublicationStep::Reserved;
    reserved.final_path = dir_ / L"out" / L"IMG_0001_Lucent_2.jpg";
    LUCENT_EXPECT_OK(
        catalog_->write([&](WriteTransaction& tx) { return tx.record_publication_step(lease, reserved, at(kT0 + 6)); }));
}

TEST_F(AutoJournal, AnalyzeOnlyJobsCompleteWithoutAReceipt) {
    auto created = create(AutoMode::AnalyzeOnly, 1);
    LUCENT_ASSERT_OK(created);
    auto claimed = claim();
    ASSERT_TRUE(claimed && *claimed);
    const JobLease lease = (*claimed)->lease;
    Status early = catalog_->write([&](WriteTransaction& tx) { return tx.complete_without_output(lease, at(kT0 + 1)); });
    ASSERT_FALSE(early);
    EXPECT_EQ(early.error().where, "catalog.job.auto_state");
    LUCENT_ASSERT_OK(
        catalog_->write([&](WriteTransaction& tx) { return tx.record_job_analysis(lease, "{}", at(kT0 + 2)); }));
    const RevisionInfo effective = add_auto_revision(ItemId{1}, 0.3);
    AutoFreeze freeze{effective.id, effective.settings_sha256, std::nullopt};
    LUCENT_ASSERT_OK(
        catalog_->transact([&](WriteTransaction& tx) { return tx.freeze_auto_job(lease, freeze, at(kT0 + 3)); }));
    // An analyse-only job never publishes.
    PublicationEntry reserved;
    reserved.step = PublicationStep::Reserved;
    reserved.final_path = dir_ / L"out" / L"IMG_0001.jpg";
    EXPECT_FALSE(
        catalog_->write([&](WriteTransaction& tx) { return tx.record_publication_step(lease, reserved, at(kT0 + 4)); }));
    LUCENT_ASSERT_OK(
        catalog_->write([&](WriteTransaction& tx) { return tx.complete_without_output(lease, at(kT0 + 5)); }));
    EXPECT_EQ(job(lease.job).stage, JobStage::Complete);
    auto batch = catalog_->query([&](ReadTransaction& tx) { return tx.find_batch(created->id); });
    EXPECT_EQ((*batch)->state, BatchState::Completed);
    auto receipt = catalog_->query([&](ReadTransaction& tx) { return tx.find_receipt(lease.job); });
    LUCENT_ASSERT_OK(receipt);
    EXPECT_FALSE(receipt->has_value());

    // Not for an export batch's job.
    auto plain = create(AutoMode::Auto, 1);
    LUCENT_ASSERT_OK(plain);
    auto other = claim({}, kT0 + 10);
    ASSERT_TRUE(other && *other);
    Status refused =
        catalog_->write([&](WriteTransaction& tx) { return tx.complete_without_output((*other)->lease, at(kT0 + 11)); });
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().where, "catalog.job.auto_state");
}

TEST_F(AutoJournal, AnalysisRecordsArePersistedVersionedAndImmutable) {
    const RevisionInfo automatic = add_auto_revision(ItemId{1}, 0.5);
    auto created = create(AutoMode::Auto, 1);
    LUCENT_ASSERT_OK(created);

    auto id = catalog_->transact(
        [&](WriteTransaction& tx) { return tx.record_auto_analysis(analysis_for(automatic.id, created->jobs[0])); });
    LUCENT_ASSERT_OK(id);
    auto found = catalog_->query([&](ReadTransaction& tx) { return tx.find_auto_analysis(automatic.id); });
    LUCENT_ASSERT_OK(found);
    ASSERT_TRUE(found->has_value());
    const AutoAnalysisRecord& record = **found;
    EXPECT_EQ(record.id, *id);
    EXPECT_EQ(record.item, ItemId{1});
    EXPECT_EQ(record.job, created->jobs[0]);
    EXPECT_EQ(record.policy, "natural");
    EXPECT_EQ(record.policy_version, 2u);
    EXPECT_EQ(record.search_version, 2u);
    EXPECT_EQ(record.metric_contract_version, 2u);
    EXPECT_EQ(record.consistency_version, 2u);
    EXPECT_EQ(record.develop_pipeline_version, 1u);
    EXPECT_EQ(record.develop_settings_version, 1u);
    EXPECT_EQ(record.final_check, AutoCheckOutcome::Passed);
    EXPECT_DOUBLE_EQ(record.confidence, 0.7);
    EXPECT_TRUE(record.review);
    EXPECT_EQ(record.flags_json, analysis_for(automatic.id).flags_json);
    EXPECT_EQ(record.created_at, at(kT0 + 10));

    // One record per revision.
    auto duplicate =
        catalog_->transact([&](WriteTransaction& tx) { return tx.record_auto_analysis(analysis_for(automatic.id)); });
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::AlreadyExists);
    // Only automatic revisions have records.
    auto user = catalog_->transact([&](WriteTransaction& tx) { return tx.record_auto_analysis(analysis_for(base_.id)); });
    ASSERT_FALSE(user);
    EXPECT_EQ(user.error().code, ErrorCode::InvalidArgument);
    // A job of another item.
    const RevisionInfo second = add_auto_revision(ItemId{2}, 0.5);
    auto foreign = catalog_->transact(
        [&](WriteTransaction& tx) { return tx.record_auto_analysis(analysis_for(second.id, created->jobs[0])); });
    ASSERT_FALSE(foreign);
    EXPECT_EQ(foreign.error().code, ErrorCode::InvalidArgument);
    // Invalid values.
    NewAutoAnalysis invalid = analysis_for(second.id);
    invalid.confidence = 1.5;
    EXPECT_EQ(invalid.validate().error().code, ErrorCode::InvalidArgument);
    invalid = analysis_for(second.id);
    invalid.policy_version = 0;
    EXPECT_FALSE(invalid.validate());
    invalid = analysis_for(second.id);
    invalid.flags_json = R"({"not":"an array"})";
    auto object = catalog_->transact([&](WriteTransaction& tx) { return tx.record_auto_analysis(invalid); });
    ASSERT_FALSE(object);
    EXPECT_EQ(object.error().code, ErrorCode::InvalidArgument);

    // Fallback attempts of the item: listed oldest first.
    const RevisionInfo fallback = add_auto_revision(ItemId{1}, 0.25);
    NewAutoAnalysis next = analysis_for(fallback.id);
    next.attempt = 1;
    next.consistency_version.reset();
    next.final_check = AutoCheckOutcome::NotRun;
    LUCENT_ASSERT_OK(catalog_->transact([&](WriteTransaction& tx) { return tx.record_auto_analysis(next); }));
    auto listed = catalog_->query([&](ReadTransaction& tx) { return tx.list_auto_analyses(ItemId{1}); });
    LUCENT_ASSERT_OK(listed);
    ASSERT_EQ(listed->size(), 2u);
    EXPECT_EQ((*listed)[0].revision, automatic.id);
    EXPECT_EQ((*listed)[1].revision, fallback.id);
    EXPECT_EQ((*listed)[1].attempt, 1u);
    EXPECT_FALSE((*listed)[1].consistency_version.has_value());
    EXPECT_EQ((*listed)[1].final_check, AutoCheckOutcome::NotRun);
    auto unknown = catalog_->query([&](ReadTransaction& tx) { return tx.list_auto_analyses(ItemId{99}); });
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::NotFound);

    // Immutable and kept while the item exists; gone with it.
    std::unique_ptr<detail::Connection> raw = raw_connection(dir_ / L"lucent.catalog");
    ASSERT_NE(raw, nullptr);
    EXPECT_FALSE(raw->exec("UPDATE auto_analyses SET confidence = 1", "test"));
    EXPECT_FALSE(raw->exec("DELETE FROM auto_analyses", "test"));
    raw.reset();
    LUCENT_ASSERT_OK(catalog_->write([&](WriteTransaction& tx) {
        const ItemId items[] = {ItemId{1}};
        return tx.remove_items(items);
    }));
    auto gone = catalog_->query([&](ReadTransaction& tx) { return tx.find_auto_analysis(automatic.id); });
    LUCENT_ASSERT_OK(gone);
    EXPECT_FALSE(gone->has_value());
}

// ---- Migration ---------------------------------------------------------------------------------

TEST(CatalogSchemaV5, MigratesAV4CatalogWithBatchesUnchanged) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    {
        Expected<std::unique_ptr<Catalog>> v4 =
            detail::CatalogAccess::open(options_for(path), detail::builtin_migrations().first(4));
        LUCENT_ASSERT_OK(v4);
        const Status written = (*v4)->write([&](WriteTransaction& tx) -> Status {
            SourceObservation o = observation(dir / L"photos" / L"IMG_0001.CR3", 1);
            o.sha256 = digest(1);
            Expected<SourceUpsertResult> source = tx.upsert_source(o);
            if (!source) {
                return Unexpected(source.error());
            }
            const ItemMetadata metadata = item();
            return tx.upsert_items(source->id, std::span(&metadata, 1)).map([](std::vector<ItemId>) {});
        });
        LUCENT_ASSERT_OK(written);
    }
    {
        // A W7 batch, written as a schema-v4 build writes it.
        std::unique_ptr<detail::Connection> raw = raw_connection(path);
        ASSERT_NE(raw, nullptr);
        LUCENT_ASSERT_OK(raw->exec(
            "INSERT INTO batches (name, selection_description, destination_json, export_settings_json, "
            "export_settings_sha256, policy_json, renderer_version, state, job_count, created_at, updated_at) "
            "VALUES ('v4', '', '{}', '{}', zeroblob(32), '{}', 'w7', 'draft', 1, 1, 1);"
            "INSERT INTO jobs (batch_id, ordinal, item_id, subimage, settings_sha256, source_sha256, assets_json, "
            "identity_sha256, stage, not_before, created_at, updated_at) "
            "VALUES (1, 0, 1, 0, zeroblob(32), zeroblob(32), '{}', zeroblob(32), 'queued', 0, 1, 1);",
            "test"));
    }
    std::unique_ptr<Catalog> catalog = open_or_fail(path);
    ASSERT_NE(catalog, nullptr);
    EXPECT_EQ(catalog->schema().found_version, 4);
    EXPECT_EQ(catalog->schema().version, kSchemaVersion);
    EXPECT_EQ(catalog->schema().migrations_applied, (std::vector<std::int32_t>{5}));
    auto batch = catalog->query([&](ReadTransaction& tx) { return tx.find_batch(BatchId{1}); });
    LUCENT_ASSERT_OK(batch);
    ASSERT_TRUE(batch->has_value());
    EXPECT_EQ((*batch)->automatic, AutoMode::None);
    auto job = catalog->query([&](ReadTransaction& tx) { return tx.find_job(JobId{1}); });
    LUCENT_ASSERT_OK(job);
    ASSERT_TRUE(job->has_value());
    EXPECT_EQ((*job)->auto_state, AutoJobState::None);
    EXPECT_FALSE((*job)->analysis_json.has_value());
    catalog.reset();
    std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM sqlite_schema WHERE name = 'auto_analyses'"), 1);
    EXPECT_EQ(query_text(*raw, "SELECT value FROM catalog_meta WHERE key = 'min_reader_version'"), "2");
    // An export job's identity is still immutable after the trigger was replaced.
    raw.reset();
    raw = raw_connection(path);
    EXPECT_FALSE(raw->exec("UPDATE jobs SET identity_sha256 = zeroblob(32)", "test"));
    EXPECT_FALSE(raw->exec("UPDATE jobs SET settings_sha256 = zeroblob(32)", "test"));
}

}  // namespace
}  // namespace lucent::catalog
