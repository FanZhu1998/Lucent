// Batches frozen by an earlier Lucent still export after an update (W12, ADR 0022; blueprint section
// 8: "Apply application updates between jobs"; ADR 0017: batches frozen before the update still
// run). W6 to W8 froze each job's recipe as a "lucent.develop" version-1 document (an edit's revision,
// or the as-shot settings); this build writes version 2. A queued job of such a batch must export
// the recipe it froze, which this build reads and renders exactly as before, not fail because its
// canonical text changed.

#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <lucent/application/batch_service.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>

#include "batch_documents.hpp"
#include "batch_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
using namespace std::chrono_literals;

// The canonical text W6 to W8 wrote: version 1 (ADR 0010).
std::string version_1_recipe(std::string_view exposure) {
    return std::format(
        R"({{"base_look":{{"id":"neutral","version":1}},"contrast":0,"exposure_ev":{},"highlights":0,"locks":[],)"
        R"("operations":{{"color":1,"contrast":1,"exposure":1,"local_tone":1,"tone_curve":1,"white_balance":1}},)"
        R"("saturation":0,"schema":"lucent.develop","shadows":0,"tone_curve":[[0,0],[1,1]],"version":1,)"
        R"("vibrance":0,"white_balance":{{"mode":"as_shot"}}}})",
        exposure);
}

core::Digest256 sha256_of(std::string_view text) {
    return *core::sha256(std::as_bytes(std::span(text.data(), text.size())));
}

TEST(BatchUpdate, JobsFrozenWithVersionOneRecipesExportAfterTheUpdate) {
    ts::ScratchDir dir;
    const std::vector<std::filesystem::path> photos = ts::write_dngs(dir, dir.path() / L"photos", 2);
    const std::filesystem::path out = dir.path() / L"out";
    std::filesystem::create_directories(out);
    catalog::BatchId batch;
    ts::BatchHarness h(dir.path() / L"lucent");
    const std::vector<ItemId> items = h.import(photos);
    ASSERT_EQ(items.size(), 2u);
    // The batch a W7 build froze: the first photo with its version-1 edit, the second as shot
    // (version 1's default document), with W7's documents.
    const std::string edited_document = version_1_recipe("0.5");
    app::detail::DestinationDocument destination;
    destination.folder = out;
    auto folder = core::stat_file(out);
    ASSERT_TRUE(folder);
    destination.folder_identity = *folder;
    destination.name_pattern = "{original-stem}_Lucent_{revision}";
    destination.temp_tag = L"0a1b2c3d";
    destination.storage = core::StorageKind::Fixed;
    app::detail::PolicyDocument policy;
    nlohmann::json w7_policy = nlohmann::json::parse(*app::detail::write_policy(policy));
    w7_policy["retry"].erase("max_interruptions");
    w7_policy.erase("single_export");
    const Status created = h.catalog().write([&](catalog::WriteTransaction& tx) -> Status {
        catalog::NewRevision revision;
        revision.schema_name = "lucent.develop";
        revision.schema_version = 1;
        revision.settings_json = edited_document;
        revision.engine_version = "0.1.0";
        revision.note = "changed=exposure";
        revision.created_at = catalog::current_time();
        auto appended = tx.append_revision(catalog::ItemId{static_cast<std::int64_t>(items[0].value)}, revision);
        if (!appended) {
            return Unexpected(appended.error());
        }
        std::vector<catalog::NewJob> jobs(2);
        jobs[0].item = catalog::ItemId{static_cast<std::int64_t>(items[0].value)};
        jobs[0].revision = appended->id;
        jobs[0].settings_sha256 = sha256_of(edited_document);
        jobs[1].item = catalog::ItemId{static_cast<std::int64_t>(items[1].value)};
        jobs[1].settings_sha256 = sha256_of(version_1_recipe("0"));
        catalog::NewBatch b;
        b.name = "Frozen by W7";
        b.selection_description = "2 photos";
        b.destination_json = *app::detail::write_destination(destination);
        b.export_settings_json = *app::detail::write_export_document(app::detail::ExportDocument{});
        b.policy_json = w7_policy.dump();
        b.renderer_version = "lucent 0.1.0; develop pipeline 2, look neutral 1; worker protocol 1";
        b.created_at = catalog::current_time();
        b.start = true;
        auto made = tx.create_batch(b, jobs);
        if (!made) {
            return Unexpected(made.error());
        }
        batch = made->id;
        return ok();
    });
    ASSERT_TRUE(created) << created.error().describe();

    // The updated Lucent starts: recovery finds the running batch and continues it.
    h.reopen();
    ASSERT_TRUE(h.log.wait_for_kind(batch, BatchEventKind::Finished, 180s));
    const std::vector<catalog::JobRecord> jobs = h.jobs(batch);
    ASSERT_EQ(jobs.size(), 2u);
    for (const catalog::JobRecord& job : jobs) {
        EXPECT_EQ(job.stage, catalog::JobStage::Complete)
            << "job " << job.id.value << ": " << (job.last_error ? job.last_error->message : std::string());
    }
    const std::vector<catalog::ReceiptRecord> receipts = h.receipts(batch);
    ASSERT_EQ(receipts.size(), 2u);
    // Each output was rendered from the recipe its job froze, in this build's canonical form.
    auto edited = read_recipe(edited_document);
    ASSERT_TRUE(edited);
    const auto edited_canonical = write_recipe(*edited);
    ASSERT_TRUE(edited_canonical);
    auto details = read_receipt_details(receipts[0].settings_json);
    ASSERT_TRUE(details) << details.error().describe();
    EXPECT_EQ(details->recipe_sha256, edited_canonical->sha256);
    EXPECT_TRUE(std::filesystem::exists(receipts[0].final_path));
    EXPECT_TRUE(std::filesystem::exists(receipts[1].final_path));
}

}  // namespace
}  // namespace lucent::app
