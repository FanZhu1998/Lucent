// The release gate "Update compatibility" (blueprint section 10: "Prior catalog/recipe fixtures
// migrate safely; unknown newer operations are reported; old missing assets are not silently
// replaced"; ADR 0022), through the application:
//   * every committed catalog fixture (src/catalog/tests/data/migration, one per historical schema
//     version) migrates and loads as a library: every item with its support state, every recipe
//     (versions 1 and 2) read as settings this build renders, every batch's documents, receipts,
//     analysis documents and records;
//   * recipes a newer Lucent wrote (a newer document version, an operation version this build does
//     not implement, an operation it does not know) make their item read-only with the reason:
//     nothing renders or exports them as if the unknown part were absent;
//   * a camera support database that is missing or replaced after an update is never substituted
//     (not by a data pack, not by another copy): items keep the answer of the database they were
//     assessed with and nothing becomes Verified; only the genuine bundled database re-assesses them,
//     recorded with its version.

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <lucent/application/auto_develop.hpp>
#include <lucent/application/batch_service.hpp>
#include <lucent/application/camera_support.hpp>
#include <lucent/application/catalog_library.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/application/recipe_store.hpp>
#include <lucent/assets/asset_store.hpp>
#include <lucent/assets/camera_support.hpp>
#include <lucent/catalog/catalog.hpp>
#include <lucent/core/hash.hpp>

#include "app_test_support.hpp"
#include "batch_documents.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
using Json = nlohmann::json;

std::filesystem::path fixture(int version) {
    return std::filesystem::path(LUCENT_SOURCE_DIR) / L"src" / L"catalog" / L"tests" / L"data" / L"migration" /
           std::format(L"v{}.catalog", version);
}

// A migrated copy of fixture `version`, loaded as the persistent library with its recipes.
struct MigratedLibrary {
    std::shared_ptr<catalog::Catalog> catalog;
    std::unique_ptr<CatalogLibrary> library;
    std::shared_ptr<RecipeStore> store;
    std::unique_ptr<RecipeService> recipes;

    void open(const ts::ScratchDir& dir, int version) {
        const std::filesystem::path path = dir.path() / L"data" / L"lucent.catalog";
        std::filesystem::create_directories(path.parent_path());
        std::filesystem::copy_file(fixture(version), path);
        catalog = ts::open_catalog(path);
        ASSERT_NE(catalog, nullptr);
        ASSERT_EQ(catalog->schema().found_version, version);
        ASSERT_EQ(catalog->schema().version, catalog::kSchemaVersion);
        auto opened = CatalogLibrary::open(catalog);
        ASSERT_TRUE(opened) << opened.error().describe();
        library = std::move(*opened);
        auto created = create_catalog_recipe_store(catalog);
        ASSERT_TRUE(created) << created.error().describe();
        store = *created;
        recipes = std::make_unique<RecipeService>(*library, store);
    }

    [[nodiscard]] SourceItem item(std::uint64_t id) const {
        std::optional<SourceItem> found = library->find(ItemId{id});
        EXPECT_TRUE(found.has_value()) << id;
        return found.value_or(SourceItem{});
    }
};

class FixtureThroughTheApplication : public ::testing::TestWithParam<int> {};

TEST_P(FixtureThroughTheApplication, LoadsEveryItemRecipeAndDocument) {
    const int version = GetParam();
    ts::ScratchDir dir;
    MigratedLibrary fixture;
    fixture.open(dir, version);
    if (HasFatalFailure()) {
        return;
    }

    // Every item, with the support state its stored assessment holds.
    ASSERT_EQ(fixture.library->size(), 10u);
    EXPECT_EQ(fixture.item(4).support.status, SupportStatus::Limited);
    EXPECT_EQ(fixture.item(9).support.status, SupportStatus::Limited);
    EXPECT_EQ(fixture.item(10).support.status, SupportStatus::Unsupported);
    EXPECT_FALSE(fixture.item(10).probe.has_value()) << "its probe failed";
    EXPECT_EQ(fixture.item(1).support.status, version >= 5 ? SupportStatus::Verified : SupportStatus::CompatibleUnverified);
    EXPECT_EQ(fixture.item(5).source_state, SourceState::Missing);
    EXPECT_EQ(fixture.item(6).source_state, SourceState::Changed);
    EXPECT_EQ(fixture.item(4).storage, core::StorageKind::Removable);
    EXPECT_EQ(fixture.item(3).import_mode, ImportMode::Copied);
    EXPECT_EQ(fixture.item(1).flags.rating, 5u);
    EXPECT_FALSE(fixture.item(1).content_duplicates.empty()) << "item 7 holds the same photograph";
    EXPECT_EQ(fixture.item(9).subimage, 1u);

    // Every recipe reads, a version-1 document as the settings it always rendered.
    for (const SourceItem& item : fixture.library->items()) {
        auto state = fixture.recipes->state(item.id);
        ASSERT_TRUE(state) << state.error().describe();
        EXPECT_FALSE(state->read_only.has_value()) << item.id.value << ": " << state->read_only->describe();
        auto current = fixture.recipes->current_settings(item.id);
        ASSERT_TRUE(current) << item.id.value << ": " << current.error().describe();
        auto history = fixture.recipes->history(item.id);
        ASSERT_TRUE(history) << history.error().describe();
        for (const HistoryEntry& entry : history->entries) {
            auto settings = fixture.recipes->revision_settings(item.id, entry.info.id);
            EXPECT_TRUE(settings) << "item " << item.id.value << " revision " << entry.info.revision_number;
        }
    }
    if (version >= 2) {
        auto first = fixture.recipes->history(ItemId{1});
        ASSERT_TRUE(first);
        ASSERT_GE(first->entries.size(), 4u);
        EXPECT_EQ(first->entries[2].info.schema_version, 1u);
        EXPECT_FALSE(first->entries[2].on_current_path) << "the undone branch is kept";
        auto edited = fixture.recipes->revision_settings(ItemId{1}, first->entries[3].info.id);
        ASSERT_TRUE(edited);
        EXPECT_DOUBLE_EQ(edited->exposure_ev, 0.35);
        EXPECT_DOUBLE_EQ(edited->contrast, 0.2);
        const auto* kelvin = std::get_if<engine::KelvinWhiteBalance>(&edited->white_balance);
        ASSERT_NE(kelvin, nullptr);
        EXPECT_DOUBLE_EQ(kelvin->temperature_k, 5600.0);
        EXPECT_EQ(edited->operations.geometry, engine::kOldestGeometryOperationVersion)
            << "a version-1 recipe keeps the whole visible area";
        auto reset = fixture.recipes->current_settings(ItemId{2});
        ASSERT_TRUE(reset);
        EXPECT_DOUBLE_EQ(reset->settings.exposure_ev, 0.0);
        EXPECT_TRUE(reset->settings.locks.contains(engine::DevelopControl::Exposure)) << "the lock survives the reset";
    }
    if (version >= 5) {
        auto analyses = fixture.recipes->automatic_analyses(ItemId{1});
        ASSERT_TRUE(analyses) << analyses.error().describe();
        ASSERT_EQ(analyses->size(), 1u);
        EXPECT_TRUE(Json::parse((*analyses)[0].record_json, nullptr, false).is_object());
        auto first = fixture.recipes->history(ItemId{1});
        ASSERT_TRUE(first);
        EXPECT_EQ(first->entries.back().info.origin, catalog::RevisionOrigin::Auto);
        EXPECT_TRUE(first->entries.back().current);
        // The W9 recipe with geometry version 1, a crop, a radial mask and colour noise reduction.
        const HistoryEntry& w9 = first->entries[first->entries.size() - 3];
        auto settings = fixture.recipes->revision_settings(ItemId{1}, w9.info.id);
        ASSERT_TRUE(settings) << settings.error().describe();
        EXPECT_EQ(settings->operations.geometry, 1u);
        EXPECT_EQ(settings->local_adjustments.size(), 1u);
        EXPECT_GT(settings->chroma_denoise.strength, 0.0);
    }

    // Every batch's documents, receipts and analysis documents read with this build's readers.
    const Status journal = fixture.catalog->read([&](catalog::ReadTransaction& tx) -> Status {
        auto batches = tx.list_batches(100);
        if (!batches) {
            return Unexpected(batches.error());
        }
        EXPECT_EQ(batches->size(), version < 3 ? 0u : version == 3 ? 4u : version == 4 ? 5u : 7u);
        for (const catalog::BatchRecord& batch : *batches) {
            auto destination = detail::read_destination(batch.destination_json);
            EXPECT_TRUE(destination) << batch.name << ": " << destination.error().describe();
            auto exports = detail::read_export_document(batch.export_settings_json);
            EXPECT_TRUE(exports) << batch.name << ": " << exports.error().describe();
            auto policy = detail::read_policy(batch.policy_json);
            EXPECT_TRUE(policy) << batch.name << ": " << policy.error().describe();
            if (policy && version == 3) {
                EXPECT_EQ(policy->retry.max_interruptions, BatchRetryPolicy{}.max_interruptions)
                    << "a member W7 phase 2 did not write takes its default";
            }
            catalog::JobQuery query;
            query.batch = batch.id;
            auto jobs = tx.list_jobs(query);
            if (!jobs) {
                return Unexpected(jobs.error());
            }
            for (const catalog::JobRecord& job : *jobs) {
                auto receipt = tx.find_receipt(job.id);
                if (!receipt) {
                    return Unexpected(receipt.error());
                }
                if (*receipt) {
                    auto details = read_receipt_details((*receipt)->settings_json);
                    EXPECT_TRUE(details) << details.error().describe();
                    if (details) {
                        EXPECT_EQ(details->support.has_value(), version >= 5) << "receipts record the support state from W10";
                    }
                }
                if (job.analysis_json) {
                    auto run = read_auto_run(*job.analysis_json);
                    EXPECT_TRUE(run) << run.error().describe();
                }
            }
        }
        return ok();
    });
    EXPECT_TRUE(journal) << journal.error().describe();
}

INSTANTIATE_TEST_SUITE_P(EveryHistoricalSchemaVersion, FixtureThroughTheApplication,
                         ::testing::Range(1, catalog::kSchemaVersion + 1),
                         [](const ::testing::TestParamInfo<int>& info) { return std::format("v{}", info.param); });

// ---- Recipes of a newer Lucent ------------------------------------------------------------------

// The canonical default document with `edit` applied to its JSON.
template <class Edit>
std::string newer_document(Edit edit) {
    auto document = write_recipe(engine::DevelopSettings{});
    EXPECT_TRUE(document);
    Json json = Json::parse(document->json);
    edit(json);
    return json.dump();
}

TEST(UpdateCompatibility, RecipesWithNewerOperationsAreReportedAndNeverRenderedWithoutThem) {
    ts::ScratchDir dir;
    MigratedLibrary fixture;
    fixture.open(dir, catalog::kSchemaVersion);
    if (HasFatalFailure()) {
        return;
    }
    struct Case {
        std::uint64_t item;
        std::uint32_t schema_version;
        std::string document;
        ErrorCode code;
    };
    const std::vector<Case> cases = {
        // A newer document version with an operation this build has never heard of.
        {2, kRecipeSchemaVersion + 1, newer_document([](Json& j) {
             j["version"] = kRecipeSchemaVersion + 1;
             j["lens_blur"] = Json{{"amount", 0.4}};
             j["operations"]["lens_blur"] = 1;
         }),
         ErrorCode::VersionMismatch},
        // This document version, but an operation version this build does not implement.
        {3, kRecipeSchemaVersion, newer_document([](Json& j) { j["operations"]["exposure"] = 2; }),
         ErrorCode::VersionMismatch},
        // This document version with an operation it does not define: refused, never ignored.
        {7, kRecipeSchemaVersion, newer_document([](Json& j) {
             j["dehaze"] = Json{{"amount", 0.3}};
             j["operations"]["dehaze"] = 1;
         }),
         ErrorCode::InvalidArgument},
    };
    for (const Case& c : cases) {
        // Nothing reads it as settings, with or without the unknown part.
        auto read = read_recipe(c.document);
        ASSERT_FALSE(read) << c.document;
        EXPECT_EQ(read.error().code, c.code);
        const Status appended = fixture.catalog->write([&](catalog::WriteTransaction& tx) -> Status {
            catalog::NewRevision revision;
            revision.origin = catalog::RevisionOrigin::User;
            revision.schema_name = "lucent.develop";
            revision.schema_version = c.schema_version;
            revision.settings_json = c.document;
            revision.engine_version = "9.0.0";
            revision.note = "changed=exposure";
            revision.created_at = catalog::current_time();
            return tx.append_revision(catalog::ItemId{static_cast<std::int64_t>(c.item)}, revision).map([](auto&&) {});
        });
        ASSERT_TRUE(appended) << appended.error().describe();
        auto refreshed = fixture.recipes->refresh(ItemId{c.item});
        ASSERT_TRUE(refreshed) << refreshed.error().describe();

        // Reported: the item is read-only, with the recipe's own reason.
        auto state = fixture.recipes->state(ItemId{c.item});
        ASSERT_TRUE(state) << state.error().describe();
        ASSERT_TRUE(state->read_only.has_value()) << c.item;
        EXPECT_TRUE(state->recipe_unreadable);
        EXPECT_EQ(state->read_only->code, c.code);
        EXPECT_EQ(state->read_only->where, kRecipeUnreadableWhere);
        // Never rendered or exported as something else: previews, exports and batches take their
        // settings from current_settings(), which fails with that reason.
        auto current = fixture.recipes->current_settings(ItemId{c.item});
        ASSERT_FALSE(current);
        EXPECT_EQ(current.error().code, c.code);
        // Never edited on top of what it cannot read.
        engine::DevelopSettings edit;
        edit.exposure_ev = 0.5;
        auto edited = fixture.recipes->apply(ItemId{c.item}, edit);
        ASSERT_FALSE(edited);
        EXPECT_EQ(edited.error().code, c.code);
        // The history still lists it, and the photographer can go back to a recipe this build reads.
        auto history = fixture.recipes->history(ItemId{c.item});
        ASSERT_TRUE(history) << history.error().describe();
        EXPECT_EQ(history->entries.back().info.schema_version, c.schema_version);
        auto undone = fixture.recipes->undo(ItemId{c.item});
        ASSERT_TRUE(undone) << undone.error().describe();
        EXPECT_FALSE(undone->read_only.has_value());
        EXPECT_TRUE(fixture.recipes->current_settings(ItemId{c.item}));
    }
}

// ---- Camera support data after an update --------------------------------------------------------

void copy_tree(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::create_directories(to);
    std::filesystem::copy(from, to, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

std::string database_version_of(const SourceItem& item) {
    return item.support.camera ? item.support.camera->database_version : std::string("none");
}

TEST(UpdateCompatibility, AMissingOrReplacedCameraSupportDatabaseIsNeverSubstituted) {
    const assets::AssetLocations installed = assets::default_asset_locations(nullptr);
    const std::filesystem::path genuine = installed.bundled / assets::kCameraSupportAssetId;
    if (!std::filesystem::exists(genuine / L"manifest.json")) {
        GTEST_FAIL() << "the build tree has no bundled camera support database at " << genuine.string();
    }
    auto request = assets::bundled_asset_request(assets::kCameraSupportAssetId);
    ASSERT_TRUE(request) << request.error().describe();
    ts::ScratchDir dir;

    // A data pack in the per-user folder: an exact copy of the genuine database, so the only thing
    // keeping it out is that it is not signed.
    const std::filesystem::path user = dir.path() / L"user-assets";
    copy_tree(genuine, user / assets::kCameraSupportAssetId);

    struct Case {
        const char* name;
        std::function<void(const std::filesystem::path& bundled)> prepare;  // the installed folder
        std::string_view refusal;                                           // the bundled candidate's
    };
    const std::vector<Case> cases = {
        {"missing after an update", [](const std::filesystem::path&) {}, ""},
        {"a file replaced",
         [&](const std::filesystem::path& bundled) {
             copy_tree(genuine, bundled);
             const std::filesystem::path data = bundled / L"camera-support.json";
             std::string text = read_text(data);
             text.replace(text.find("\"verified\""), 10, "\"limited\" ");
             write_text(data, text);
         },
         "assets.load.hash"},
        {"a file and its manifest replaced consistently",
         [&](const std::filesystem::path& bundled) {
             copy_tree(genuine, bundled);
             const std::filesystem::path data = bundled / L"camera-support.json";
             std::string text = read_text(data) + " ";
             write_text(data, text);
             Json manifest = Json::parse(read_text(bundled / L"manifest.json"));
             manifest["files"][0]["size"] = text.size();
             manifest["files"][0]["sha256"] =
                 core::to_hex(*core::sha256(std::as_bytes(std::span(text.data(), text.size()))));
             write_text(bundled / L"manifest.json", manifest.dump(1) + "\n");
         },
         "assets.load.pin"},
    };
    int index = 0;
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        const std::filesystem::path bundled_root = dir.path() / std::format(L"installed-{}", index++);
        std::filesystem::create_directories(bundled_root);
        c.prepare(bundled_root / assets::kCameraSupportAssetId);
        const assets::CameraSupportLoad load = assets::load_camera_support({bundled_root, user}, *request);
        EXPECT_EQ(load.active, nullptr) << "nothing is activated";
        ASSERT_TRUE(load.problem.has_value());
        bool pack_refused = false;
        for (const assets::AssetCandidate& candidate : load.candidates) {
            if (candidate.origin == assets::AssetOrigin::UserDataPack) {
                pack_refused = candidate.present && candidate.refused && candidate.refused->where == "assets.load.unsigned";
            } else if (!c.refusal.empty()) {
                ASSERT_TRUE(candidate.refused.has_value());
                EXPECT_EQ(candidate.refused->where, c.refusal) << candidate.refused->describe();
            }
        }
        EXPECT_TRUE(pack_refused) << "the unsigned data pack never stands in for the installed database";

        // The library assessed with database 2.0.1 keeps that database's answer (its record then says
        // no database is active, with the kept answer, AST-01); nothing becomes Verified, and no
        // record names another database.
        ts::ScratchDir library_dir;  // outlives the catalog it holds
        MigratedLibrary fixture;
        fixture.open(library_dir, catalog::kSchemaVersion);
        if (HasFatalFailure()) {
            return;
        }
        auto refreshed = refresh_support(*fixture.library, load.active.get());
        ASSERT_TRUE(refreshed) << refreshed.error().describe();
        for (const SourceItem& item : fixture.library->items()) {
            EXPECT_NE(item.support.status, SupportStatus::Verified) << item.path.string();
            if (item.support.camera) {
                EXPECT_TRUE(item.support.camera->database_version.empty() ||
                            item.support.camera->database_version == "2.0.1")
                    << item.support.camera->database_version;
            }
        }
        EXPECT_EQ(fixture.item(1).support.status, SupportStatus::CompatibleUnverified);
        ASSERT_TRUE(fixture.item(1).support.camera.has_value());
        EXPECT_EQ(fixture.item(1).support.camera->listed, SupportStatus::Verified)
            << "the answer of database 2.0.1 is kept, not replaced";
        EXPECT_EQ(fixture.item(4).support.status, SupportStatus::Limited) << "a missing database never improves a state";
        EXPECT_EQ(fixture.item(10).support.status, SupportStatus::Unsupported);
    }

    // Only the genuine installed database re-assesses the items, and says so in their records.
    const assets::CameraSupportLoad load = assets::load_camera_support({installed.bundled, user}, *request);
    ASSERT_NE(load.active, nullptr) << (load.problem ? load.problem->describe() : std::string());
    ts::ScratchDir library_dir;
    MigratedLibrary fixture;
    fixture.open(library_dir, catalog::kSchemaVersion);
    if (HasFatalFailure()) {
        return;
    }
    auto refreshed = refresh_support(*fixture.library, load.active.get());
    ASSERT_TRUE(refreshed) << refreshed.error().describe();
    EXPECT_GT(refreshed->reassessed, 0u);
    const std::string version = load.active->version.to_string();
    EXPECT_NE(version, "2.0.1");
    EXPECT_EQ(database_version_of(fixture.item(1)), version);
    EXPECT_EQ(database_version_of(fixture.item(9)), version);
    EXPECT_EQ(fixture.item(4).support.status, SupportStatus::Limited);
}

}  // namespace
}  // namespace lucent::app
