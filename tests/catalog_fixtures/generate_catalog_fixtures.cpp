// lucent_catalog_fixture_generator: writes the committed catalog fixtures of every historical schema
// version (src/catalog/tests/data/migration; ADR 0022, the release gate "Update compatibility").
//
//   lucent_catalog_fixture_generator <output folder>
//
// For each schema version v (1 ... the build's kSchemaVersion), the fixture is made by driving the
// older migration state:
//   1. the era's content is written through the catalog's API (and the application's own document
//      writers) into a scratch catalog of the current schema, with the documents of that era: W4's
//      library (v1); W6's lucent.develop version-1 recipes (v2); W7's export batches with their
//      journal, receipts and the batch documents as W7 wrote them (v3; the policy without the members
//      W7's closing review added), with interruption counts and report emission (v4); W8 to W12's
//      automatic batches, analysis records, version-2 recipes and camera support records (v5);
//   2. the fixture file is created by exactly the migrations a version-v build ran on an empty file
//      (the first v of detail::builtin_migrations(), each migration's DDL unchanged since it was
//      released: tests/catalog_fixtures/check_historical_ddl.py verifies that against git history);
//   3. every table of the fixture is filled from the scratch catalog, column by column, with the
//      columns schema v has (later columns are dropped, as an older build never wrote them), with
//      foreign keys and the schema's triggers enforced;
//   4. the history is stamped with each version's release time, the file is vacuumed into one
//      self-contained rollback-journal file, and its integrity and foreign keys are checked.
// fixtures.json lists each fixture with its SHA-256, schema and reader versions and row counts.
//
// The rows are therefore what the catalog's code writes for those columns (unchanged in meaning
// since each column was introduced: changing a column's meaning would itself need a migration), and
// the application documents are each era's formats. The catalog's tests
// (src/catalog/tests/migration_fixture_test.cpp) and the application's
// (src/application/tests/update_compatibility_test.cpp) migrate copies of these files.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <float.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <lucent/application/auto_develop.hpp>
#include <lucent/application/batch_service.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/recipe_store.hpp>
#include <lucent/application/source_item.hpp>
#include <lucent/catalog/catalog.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/text.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "batch_documents.hpp"
#include "catalog_impl.hpp"
#include "catalog_mapping.hpp"
#include "schema.hpp"
#include "sqlite.hpp"
#include "synthetic_dng.hpp"

namespace lucent::fixtures {
namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;
using catalog::Timestamp;

// ---- Eras -----------------------------------------------------------------------------------------

struct Era {
    int version = 0;
    std::string_view build;    // the builds that wrote this schema version
    std::string_view commit;   // the commit that introduced the schema version
    std::chrono::sys_days day;
    std::chrono::hours hour;
};

// The schema versions, the workflow that introduced each (docs/development/roadmap.md) and the
// commit of its migration (git log -- src/catalog/src/schema.cpp).
constexpr Era kEras[] = {
    {1, "W4 (catalog and import)", "b2951f7", std::chrono::sys_days{std::chrono::year{2026} / 10 / 3}, 10h},
    {2, "W6 (recipes and editor)", "0491d19", std::chrono::sys_days{std::chrono::year{2026} / 10 / 3}, 21h},
    {3, "W7 (durable batch)", "6711a6a", std::chrono::sys_days{std::chrono::year{2026} / 10 / 4}, 9h},
    {4, "W7 closing review", "93f72ca", std::chrono::sys_days{std::chrono::year{2026} / 10 / 4}, 15h},
    {5, "W8 to W12 (automatic settings to release hardening)", "7df00b8",
     std::chrono::sys_days{std::chrono::year{2026} / 10 / 8}, 12h},
};
static_assert(std::size(kEras) == static_cast<std::size_t>(catalog::kSchemaVersion),
              "one era per schema version: add the new version's era (and its fixture)");

Timestamp era_time(const Era& era, std::chrono::minutes offset = 0min) {
    return Timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(era.day.time_since_epoch() + era.hour + offset));
}

std::int64_t to_ms(Timestamp t) {
    return t.time_since_epoch().count();
}

// ---- Failure handling -----------------------------------------------------------------------------

[[noreturn]] void die(const std::string& what) {
    std::cerr << "lucent_catalog_fixture_generator: " << what << "\n";
    std::exit(1);
}

template <class T>
T must(Expected<T> value, std::string_view what) {
    if (!value) {
        die(std::format("{}: {}", what, value.error().describe()));
    }
    return std::move(*value);
}

void must(const Status& status, std::string_view what) {
    if (!status) {
        die(std::format("{}: {}", what, status.error().describe()));
    }
}

// ---- Values -------------------------------------------------------------------------------------

core::Digest256 digest_of(std::string_view text) {
    return must(core::sha256(std::as_bytes(std::span(text.data(), text.size()))), "sha256");
}

constexpr std::uint64_t kFixedVolume = 0x00000000D1A5E701ull;  // Z:, a fixed disk
constexpr std::uint64_t kCardVolume = 0x000000005D0CA4D1ull;   // E:, a camera card

core::FileIdentity identity(std::uint64_t n, std::uint64_t volume, const Era& era) {
    core::FileIdentity id;
    id.volume_serial = volume;
    for (int i = 0; i < 8; ++i) {
        id.file_id[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((n * 0x9E3779B97F4A7C15ull) >> (8 * i));
    }
    id.file_id[12] = 0x4C;  // "L"
    id.size_bytes = 25'000'000 + n * 1013;
    // FILETIME of a capture a few days before the era (100 ns since 1601).
    const std::int64_t unix_ms = to_ms(era_time(era)) - static_cast<std::int64_t>(n) * 86'400'000;
    id.last_write_time = (unix_ms + 11'644'473'600'000) * 10'000;
    return id;
}

// ---- Recipe documents ---------------------------------------------------------------------------

// The canonical "lucent.develop" version-1 text W6 to W8 wrote (ADR 0010; recipe_test.cpp's
// kVersion1DefaultDocument with these values), checked to read back with this build.
std::string recipe_v1(double exposure, std::string_view white_balance = R"({"mode":"as_shot"})", double contrast = 0,
                      std::string_view locks = "[]", std::string_view tone_curve = "[[0,0],[1,1]]",
                      double saturation = 0, double vibrance = 0) {
    const auto number = [](double value) { return must(app::canonical_number(value), "canonical number"); };
    std::string text = std::format(
        R"({{"base_look":{{"id":"neutral","version":1}},"contrast":{},"exposure_ev":{},"highlights":0,"locks":{},)"
        R"("operations":{{"color":1,"contrast":1,"exposure":1,"local_tone":1,"tone_curve":1,"white_balance":1}},)"
        R"("saturation":{},"schema":"lucent.develop","shadows":0,"tone_curve":{},"version":1,"vibrance":{},)"
        R"("white_balance":{}}})",
        number(contrast), number(exposure), locks, number(saturation), tone_curve, number(vibrance), white_balance);
    must(app::read_recipe(text), "a version-1 recipe of the fixture reads back");
    return text;
}

std::string recipe_v2(const engine::DevelopSettings& settings) {
    return must(app::write_recipe(settings), "write_recipe").json;
}

catalog::NewRevision revision(catalog::RevisionOrigin origin, std::string settings_json, std::uint32_t schema_version,
                              std::string note, Timestamp at) {
    catalog::NewRevision r;
    r.origin = origin;
    r.schema_name = "lucent.develop";
    r.schema_version = schema_version;
    r.settings_json = std::move(settings_json);
    r.engine_version = "0.1.0";
    r.note = std::move(note);
    r.created_at = at;
    return r;
}

// ---- Support records ------------------------------------------------------------------------------

// The keys of the bundled database's entries for the Canon EOS R5 (lossless CR3, full frame:
// Verified) and the DJI Mini 2 DNG (Compatible, unverified), with their qualified image areas.
constexpr std::string_view kR5Key =
    "mk1|canon|eos-r5|cr3|canon-crx-lossless-raw|8191x5463|14bit|mosaic:RGGB|uint16|single-view|primary:0/1";
constexpr std::string_view kDjiKey =
    "mk1|dji|fc7303|dng|uncompressed|4000x3000|16bit|mosaic:RGGB|uint16|single-view|primary:0/1";

// A record of the database W10 shipped (2.0.1); this build bundles 2.1.0.
app::CameraSupportEvaluation record_201(std::string_view key, std::string_view image_area) {
    app::CameraSupportEvaluation camera;
    camera.mode_key = std::string(key);
    camera.image_area = std::string(image_area);
    camera.database_id = "camera-support";
    camera.database_version = "2.0.1";
    camera.database_sha256 = core::to_hex(digest_of("camera-support 2.0.1 manifest"));
    camera.qualified_on = "2026-10-09";
    return camera;
}

// Compatible, unverified (with W10's record: not listed, or the R5 entry's Verified).
app::SupportAssessment supported(bool camera_record, std::string_view key = {}, bool verified = false) {
    app::SupportAssessment a;
    a.status = app::SupportStatus::CompatibleUnverified;
    a.interpretation = raw::InterpretationStatus::Complete;
    if (camera_record) {
        app::CameraSupportEvaluation camera =
            record_201(key.empty() ? std::string_view("mk1|nikon|z-6|nef|unlisted-mode") : key,
                       verified ? "decoder_inset_crop 8192x5464" : "");
        if (verified) {
            a.status = app::SupportStatus::Verified;
            camera.listed = app::SupportStatus::Verified;
            camera.qualified_key = camera.mode_key;
            camera.camera = "Canon EOS R5";
            camera.mode = "CR3 RAW (lossless CRX), full frame, 14-bit";
            camera.evidence = "colour chart, geometry and export fixtures";
        } else if (!key.empty()) {
            camera.listed = app::SupportStatus::CompatibleUnverified;
            camera.qualified_key = camera.mode_key;
        }
        a.camera = std::move(camera);
    }
    return a;
}

app::SupportAssessment limited(bool camera_record) {
    app::SupportAssessment a;
    a.status = app::SupportStatus::Limited;
    a.interpretation = raw::InterpretationStatus::OptionalOperationsSkipped;
    app::SupportReason reason;
    reason.kind = app::SupportReasonKind::InterpretationIssue;
    reason.issue = raw::InterpretationIssueKind::OptionalOperationUnsupported;
    reason.message = "the DNG asks for an optional correction (OpcodeList3 FixVignetteRadial) that is not applied";
    a.reasons.push_back(reason);
    if (camera_record) {
        app::CameraSupportEvaluation camera = record_201(kDjiKey, "dng_default_crop 4000x3000");
        camera.dng_opcodes = {"list3:FixVignetteRadial", "list3:WarpRectilinear", "list3:GainMap"};
        camera.interpretation = raw::InterpretationStatus::OptionalOperationsSkipped;
        camera.listed = app::SupportStatus::CompatibleUnverified;
        camera.qualified_key = camera.mode_key;
        camera.camera = "DJI Mini 2";
        camera.mode = "DNG, uncompressed, 16-bit, 4:3";
        a.camera = std::move(camera);
    }
    return a;
}

app::SupportAssessment unsupported() {
    return app::assess_probe_failure(
        Error{ErrorCode::Unsupported, "the decoder recognises the file but cannot decode it", "raw.libraw.open", -2});
}

// ---- Batch documents ----------------------------------------------------------------------------

std::string destination_json(const std::filesystem::path& folder, std::wstring tag, bool report_files = true) {
    app::detail::DestinationDocument d;
    d.folder = folder;
    core::FileIdentity folder_object;
    folder_object.volume_serial = kFixedVolume;
    folder_object.file_id[0] = static_cast<std::uint8_t>(tag.size() + folder.native().size());
    folder_object.file_id[15] = 0x44;
    d.folder_identity = folder_object;
    d.name_pattern = "{original-stem}_Lucent_{revision}";
    d.collisions = app::CollisionPolicy::UniqueName;
    d.temp_tag = std::move(tag);
    d.write_report_files = report_files;
    d.storage = core::StorageKind::Fixed;
    return must(app::detail::write_destination(d), "write_destination");
}

std::string export_json() {
    app::detail::ExportDocument d;  // JPEG, sRGB, quality 95, capture metadata, no GPS
    d.artist = "Ann Example";
    d.copyright = "(c) 2026 Ann Example";
    return must(app::detail::write_export_document(d), "write_export_document");
}

// The policy as `era` wrote it: W7 phase 2 (schema v3) had neither retry.max_interruptions nor
// single_export, which its closing review added (schema v4); "automatic" came with W8.
std::string policy_json(const Era& era, bool single_export, std::uint64_t excluded = 0) {
    app::detail::PolicyDocument d;
    d.single_export = single_export;
    if (excluded > 0) {
        app::PlanExclusion exclusion;
        exclusion.item = app::ItemId{10};
        exclusion.source = L"Z:\\Lucent fixture photos\\Unsupported\\IMG_0009.X3F";
        exclusion.reason = Error{ErrorCode::Unsupported, "the photograph is unsupported", "app.batch.unsupported", 0};
        d.excluded.push_back(exclusion);
        d.excluded_count = excluded;
    }
    Json policy = Json::parse(must(app::detail::write_policy(d), "write_policy"));
    if (era.version < 4) {
        policy["retry"].erase("max_interruptions");
        policy.erase("single_export");
    }
    return policy.dump();
}

std::string renderer_version(const Era& era) {
    return era.version < 5 ? "lucent 0.1.0; develop pipeline 2, look neutral 1; worker protocol 1"
                           : "lucent 0.1.0; develop pipeline 3, geometry 2, look neutral 1; worker protocol 2";
}

std::string receipt_json(const Era& era, const catalog::JobRecord& job, std::uint32_t revision_number,
                         std::string_view export_settings) {
    app::ReceiptDetails r;
    r.revision = job.revision;
    r.revision_number = revision_number;
    r.recipe_sha256 = job.settings_sha256;
    r.export_settings_sha256 = digest_of(export_settings);
    r.width = 8192;
    r.height = 5464;
    r.icc_sha256 = digest_of("sRGB IEC61966-2.1 (Lucent)");
    r.chroma_subsampling = exporter::ChromaSubsampling::Yuv444;
    r.encoder = "libjpeg-turbo 3.2.0";
    r.metadata_notes = {"GPS position omitted (not chosen)"};
    r.demosaic = "bayer";
    r.camera_color = "dng_color_matrix";
    r.pipeline_version = era.version < 5 ? 2 : 3;
    r.look = "neutral";
    r.look_version = 1;
    r.estimated_peak_bytes = 1'100'000'000;
    r.worker_peak_commit_bytes = 760'000'000;
    r.prepare_seconds = 0.4;
    r.render_seconds = 1.0;
    r.encode_seconds = 2.2;
    r.write_seconds = 0.8;
    r.verify_seconds = 0.05;
    r.total_seconds = 4.4;
    r.attempt = job.attempts;
    if (era.version >= 5) {
        r.support = app::SupportStatus::CompatibleUnverified;  // W10 added the support state
        r.camera_support_version = "2.0.1";
    }
    return must(app::write_receipt_details(r), "write_receipt_details");
}

// ---- Content ------------------------------------------------------------------------------------

struct Library {
    std::vector<catalog::ItemId> items;  // 1 ... 10 in the order of the file comment
    std::map<std::int64_t, catalog::RevisionId> current;  // item -> current revision (eras v2+)
};

const std::filesystem::path kPhotos = L"Z:\\Lucent fixture photos";

void write_library(catalog::Catalog& c, const Era& era, Library& library) {
    const bool camera_records = era.version >= 5;
    const std::string decoder_version =
        era.version >= 5 ? "0.22.2-Release; decoded-RAW 1.4" : "0.22.2-Release";
    must(c.write([&](catalog::WriteTransaction& tx) -> Status {
        // Imports: a completed one, a copy from a card, one interrupted by a crash (marked at the next
        // start) and one still running when the last session ended.
        const catalog::ImportId first = must(tx.begin_import(catalog::ImportStart{
                                                 era_time(era), {{kPhotos, true}}, R"({"copy_to":null})"}),
                                             "begin_import");
        const catalog::ImportId copy = must(
            tx.begin_import(catalog::ImportStart{era_time(era, 5min), {{L"E:\\DCIM", true}},
                                                 R"({"copy_to":"Z:\\Lucent library\\Iceland 2026"})"}),
            "begin_import");
        struct Source {
            std::filesystem::path path;
            std::uint64_t volume;
            bool hashed;
            std::uint64_t content;  // equal content numbers are content duplicates
            core::StorageKind storage;
            std::optional<std::filesystem::path> origin;
        };
        const std::vector<Source> sources = {
            {kPhotos / L"Iceland 2026" / L"IMG_0001.CR3", kFixedVolume, true, 1, core::StorageKind::Fixed, {}},
            {kPhotos / L"Iceland 2026" / L"IMG_0002.CR3", kFixedVolume, true, 2, core::StorageKind::Fixed, {}},
            {L"Z:\\Lucent library\\Iceland 2026\\IMG_0003.CR3", kFixedVolume, true, 3, core::StorageKind::Fixed,
             L"E:\\DCIM\\100CANON\\IMG_0003.CR3"},
            {L"E:\\DCIM\\100CANON\\IMG_0004.CR3", kCardVolume, false, 4, core::StorageKind::Removable, {}},
            {kPhotos / L"\u00C5lesund" / L"DSC_0005.ARW", kFixedVolume, true, 5, core::StorageKind::Fixed, {}},
            {kPhotos / L"Iceland 2026" / L"DSC_0006.NEF", kFixedVolume, true, 6, core::StorageKind::Fixed, {}},
            {kPhotos / L"Iceland 2026" / L"Kopien" / L"IMG_0001 \u2013 Kopie.CR3", kFixedVolume, true, 1,
             core::StorageKind::Fixed, {}},
            {kPhotos / L"Drone" / L"DJI_0008.DNG", kFixedVolume, true, 8, core::StorageKind::Fixed, {}},
            {kPhotos / L"Unsupported" / L"IMG_0009.X3F", kFixedVolume, false, 9, core::StorageKind::Fixed, {}},
        };
        std::vector<catalog::SourceId> ids;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const Source& s = sources[i];
            catalog::SourceObservation o;
            o.path = s.path;
            o.identity = identity(i + 1, s.volume, era);
            if (s.hashed) {
                o.sha256 = digest_of(std::format("lucent fixture photo {}", s.content));
            }
            o.storage_kind = s.storage;
            if (s.origin) {
                o.import_mode = catalog::ImportMode::Copied;
                o.origin_path = s.origin;
                o.origin_identity = identity(100 + i, kCardVolume, era);
            }
            o.seen_at = era_time(era, std::chrono::minutes(1 + static_cast<int>(i)));
            ids.push_back(must(tx.upsert_source(o), "upsert_source").id);
        }
        // One photo on each line of the contact sheet's variety.
        struct ItemSpec {
            std::size_t source;
            std::uint32_t subimage;
            const char* make;
            const char* model;
            const char* lens;
            std::optional<std::string> captured;
            std::uint32_t width, height;
            std::uint8_t orientation;
            const char* format;
            app::SupportAssessment support;
            catalog::ItemFlags flags;
        };
        const std::vector<ItemSpec> items = {
            {0, 0, "Canon", "EOS R5", "RF24-105mm F4 L IS USM", "2026-09-28T07:12:44", 8192, 5464, 1, "cr3",
             supported(camera_records, kR5Key, true), {catalog::PickFlag::Picked, 5}},
            {1, 0, "Canon", "EOS R5", "RF24-105mm F4 L IS USM", "2026-09-28T07:13:02", 8192, 5464, 6, "cr3",
             supported(camera_records), {catalog::PickFlag::Rejected, 0}},
            {2, 0, "Canon", "EOS R5", "RF15-35mm F2.8 L IS USM", "2026-09-29T18:40:10", 8192, 5464, 8, "cr3",
             supported(camera_records), {catalog::PickFlag::None, 3}},
            {3, 0, "Canon", "EOS R5", "", "2026-09-30T09:01:00", 8192, 5464, 1, "cr3", limited(camera_records),
             {catalog::PickFlag::None, 0}},
            {4, 0, "Sony", "ILCE-7RM5", "FE 24-70mm F2.8 GM II", "2026-09-21T12:00:00", 9504, 6336, 1, "arw",
             supported(camera_records), {catalog::PickFlag::Picked, 2}},
            {5, 0, "Nikon", "Z 6", "NIKKOR Z 24-70mm f/4 S", std::nullopt, 6048, 4024, 1, "nef",
             supported(camera_records), {catalog::PickFlag::None, 0}},
            {6, 0, "Canon", "EOS R5", "RF24-105mm F4 L IS USM", "2026-09-28T07:12:44", 8192, 5464, 1, "cr3",
             supported(camera_records), {catalog::PickFlag::None, 1}},
            {7, 0, "DJI", "FC7303", "", "2026-09-27T16:20:00", 4000, 3000, 1, "dng",
             supported(camera_records, kDjiKey), {catalog::PickFlag::Picked, 4}},
            {7, 1, "DJI", "FC7303", "", "2026-09-27T16:20:00", 4000, 3000, 1, "dng", limited(camera_records),
             {catalog::PickFlag::None, 0}},
            {8, 0, "", "", "", std::nullopt, 0, 0, 1, "x3f", unsupported(), {catalog::PickFlag::None, 0}},
        };
        for (std::size_t i = 0; i < items.size(); ++i) {
            const ItemSpec& spec = items[i];
            catalog::ItemMetadata m;
            m.subimage_index = spec.subimage;
            m.make = spec.make;
            m.model = spec.model;
            m.lens = spec.lens;
            m.capture_time = spec.captured;
            m.width = spec.width;
            m.height = spec.height;
            m.orientation = spec.orientation;
            m.format = spec.format;
            m.support_status = app::detail::to_catalog(spec.support.status);
            m.support_reasons_json = app::detail::encode_support_reasons(spec.support);
            m.decoder_id = "libraw";
            m.decoder_version = decoder_version;
            m.probed_at = era_time(era, std::chrono::minutes(2 + static_cast<int>(i)));
            if (era.version >= 2 && spec.width > 0) {  // capture settings arrived with schema v2 (W6)
                m.iso = spec.source == 4 ? 800.0 : 100.0;
                m.exposure_time_s = 1.0 / 250;
                m.f_number = 8.0;
                m.focal_length_mm = spec.source == 7 ? 6.7 : 35.0;
                m.image_count = spec.source == 7 ? 2u : 1u;
            }
            const std::vector<catalog::ItemId> added =
                must(tx.upsert_items(ids[spec.source], std::span(&m, 1)), "upsert_items");
            library.items.push_back(added.front());
            if (spec.flags.pick != catalog::PickFlag::None || spec.flags.rating != 0) {
                must(tx.set_item_flags(added.front(), spec.flags), "set_item_flags");
            }
        }
        must(tx.finish_import(first, catalog::ImportFinish{era_time(era, 4min), catalog::ImportOutcome::Completed,
                                                           catalog::ImportCounts{9, 8, 0, 0, 1, 0, 0}}),
             "finish_import");
        must(tx.finish_import(copy, catalog::ImportFinish{era_time(era, 6min), catalog::ImportOutcome::Completed,
                                                          catalog::ImportCounts{2, 1, 0, 1, 0, 0, 0}}),
             "finish_import");
        // A card import a crash interrupted (marked at the next start), then one left running.
        must(tx.begin_import(catalog::ImportStart{era_time(era, 20min), {{L"E:\\DCIM\\101CANON", false}},
                                                  R"({"copy_to":null})"}),
             "begin_import");
        must(tx.mark_interrupted_imports(era_time(era, 30min)), "mark_interrupted_imports");
        must(tx.begin_import(catalog::ImportStart{era_time(era, 40min), {{kPhotos / L"\u00C5lesund", true}},
                                                  R"({"copy_to":null})"}),
             "begin_import");
        // A missing file and a changed one, found by the presence check.
        must(tx.set_source_state(ids[4], catalog::SourceState::Missing, era_time(era, 31min)), "set_source_state");
        must(tx.set_source_state(ids[5], catalog::SourceState::Changed, era_time(era, 31min)), "set_source_state");
        return ok();
    }),
         "library");
}

void write_recipes(catalog::Catalog& c, const Era& era, Library& library) {
    must(c.write([&](catalog::WriteTransaction& tx) -> Status {
        const auto t = [&](int minutes) { return era_time(era, std::chrono::minutes(60 + minutes)); };
        const catalog::ItemId i1 = library.items[0];
        const catalog::ItemId i2 = library.items[1];
        const catalog::ItemId i3 = library.items[2];
        const catalog::ItemId i5 = library.items[4];
        using catalog::RevisionOrigin;
        // Item 1: three edits, an undo, and a new branch from the second.
        must(tx.append_revision(i1, revision(RevisionOrigin::User, recipe_v1(0.35), 1, "changed=exposure", t(1))),
             "append_revision");
        must(tx.append_revision(i1, revision(RevisionOrigin::User,
                                             recipe_v1(0.35, R"({"mode":"kelvin","temperature_k":5600,"tint":4})"), 1,
                                             "changed=white_balance", t(2))),
             "append_revision");
        must(tx.append_revision(i1, revision(RevisionOrigin::User,
                                             recipe_v1(0.35, R"({"mode":"kelvin","temperature_k":5600,"tint":4})", 0,
                                                       "[]", "[[0,0],[0.25,0.2],[0.75,0.82],[1,1]]"),
                                             1, "changed=tone_curve", t(3))),
             "append_revision");
        must(tx.undo_revision(i1), "undo_revision");
        must(tx.append_revision(i1, revision(RevisionOrigin::User,
                                             recipe_v1(0.35, R"({"mode":"kelvin","temperature_k":5600,"tint":4})", 0.2),
                                             1, "changed=contrast", t(4))),
             "append_revision");
        // Item 2: an exposure edit with its lock, then a recorded reset that keeps the lock.
        must(tx.append_revision(i2, revision(RevisionOrigin::User, recipe_v1(-0.5, R"({"mode":"as_shot"})", 0,
                                                                             R"(["exposure"])"),
                                             1, "changed=exposure;locks=exposure", t(5))),
             "append_revision");
        must(tx.append_revision(i2, revision(RevisionOrigin::Reset,
                                             recipe_v1(0, R"({"mode":"as_shot"})", 0, R"(["exposure"])"), 1,
                                             "changed=exposure", t(6))),
             "append_revision");
        // Item 3: settings pasted from item 1 (colour only).
        must(tx.append_revision(i3, revision(RevisionOrigin::Paste,
                                             recipe_v1(0, R"({"mode":"as_shot"})", 0, "[]", "[[0,0],[1,1]]", 0.15, 0.1),
                                             1, "changed=saturation,vibrance", t(7))),
             "append_revision");
        // Item 5 (its file is missing now): one edit.
        must(tx.append_revision(i5, revision(RevisionOrigin::User, recipe_v1(1), 1, "changed=exposure", t(8))),
             "append_revision");

        if (era.version >= 5) {
            // W9: version-2 recipes with geometry version 1 (a straightened crop), a radial mask and
            // chroma denoising; W10: geometry version 2.
            engine::DevelopSettings s = must(app::read_recipe(recipe_v1(0.35, R"({"mode":"kelvin","temperature_k":5600,"tint":4})", 0.2)), "read");
            s.operations.geometry = 1;
            s.geometry.straighten_degrees = 1.5;
            s.geometry.crop = engine::CropRect{0.05, 0.04, 0.95, 0.96};
            engine::LocalAdjustment radial;
            radial.id = 1;
            radial.shape = engine::RadialGradientMask{{0.5, 0.45}, 0.2, 0.15, 0.0, 0.5};
            radial.controls.exposure_ev = 0.4;
            s.local_adjustments = {radial};
            s.chroma_denoise = engine::ChromaDenoiseSetting{0.3, 4.0};
            must(tx.append_revision(i1, revision(RevisionOrigin::User, recipe_v2(s), 2,
                                                 "parts=chroma_denoise,masks,geometry", t(20))),
                 "append_revision");
            s.operations.geometry = engine::kGeometryOperationVersion;
            s.saturation = 0.1;
            must(tx.append_revision(i1, revision(RevisionOrigin::User, recipe_v2(s), 2, "changed=saturation", t(21))),
                 "append_revision");
        }
        return ok();
    }),
         "recipes");
    must(c.read([&](catalog::ReadTransaction& tx) -> Status {
        for (const catalog::ItemId item : library.items) {
            auto current = must(tx.current_revision(item), "current_revision");
            if (current) {
                library.current[item.value] = current->info.id;
            }
        }
        return ok();
    }),
         "current revisions");
}

// A claimed job's lease, published output and receipt, as the coordinator does (ADR 0012).
void complete_job(catalog::Catalog& c, const Era& era, catalog::BatchId batch, const std::string& owner, Timestamp at,
                  const std::filesystem::path& folder, std::string_view export_settings, std::uint32_t revision_number) {
    const catalog::ClaimedJob claimed = must(c.transact([&](catalog::WriteTransaction& tx) {
        catalog::ClaimRequest request;
        request.owner = owner;
        request.now = at;
        request.batch = batch;
        return tx.claim_next_job(request).and_then([](std::optional<catalog::ClaimedJob> job) -> Expected<catalog::ClaimedJob> {
            if (!job) {
                return fail(ErrorCode::NotFound, "no job to claim", "fixture");
            }
            return *job;
        });
    }),
                                             "claim_next_job");
    const catalog::JobLease& lease = claimed.lease;
    const std::wstring name = std::format(L"IMG_{:04}_Lucent_{}.jpg", claimed.job.item ? claimed.job.item->value : 0,
                                          revision_number);
    const std::filesystem::path final_path = folder / name;
    const std::filesystem::path temp_path =
        folder / std::format(L"~lucent-{}-{}-a{}.tmp", L"0f1e2d3c", claimed.job.id.value, lease.attempt);
    const std::string output = std::format("fixture output of job {}", claimed.job.id.value);
    const core::Digest256 output_sha = digest_of(output);
    const std::uint64_t size = 6'000'000 + static_cast<std::uint64_t>(claimed.job.id.value) * 7919;
    must(c.write([&](catalog::WriteTransaction& tx) -> Status {
        auto at_ms = [&](int s) { return at + std::chrono::seconds(s); };
        must(tx.record_publication_step(lease, {catalog::PublicationStep::Reserved, std::nullopt, final_path}, at_ms(1)),
             "reserved");
        must(tx.record_publication_step(lease, {catalog::PublicationStep::TempWritten, temp_path, final_path}, at_ms(4)),
             "temp_written");
        must(tx.advance_job(lease, catalog::JobStage::Rendering, at_ms(4)), "advance");
        must(tx.record_publication_step(lease, {catalog::PublicationStep::Verified, temp_path, final_path, output_sha, size},
                                        at_ms(5)),
             "verified");
        must(tx.advance_job(lease, catalog::JobStage::Verifying, at_ms(5)), "advance");
        must(tx.record_publication_step(lease,
                                        {catalog::PublicationStep::Published, temp_path, final_path, output_sha, size},
                                        at_ms(6)),
             "published");
        auto job = must(tx.find_job(lease.job), "find_job");
        catalog::NewReceipt receipt;
        receipt.final_path = final_path;
        receipt.size_bytes = size;
        receipt.output_sha256 = output_sha;
        receipt.settings_json = receipt_json(era, *job, revision_number, export_settings);
        must(tx.record_receipt(lease, receipt, at_ms(6)), "record_receipt");
        must(tx.advance_job(lease, catalog::JobStage::Publishing, at_ms(7)), "advance");
        return ok();
    }),
         "complete job");
}

catalog::NewJob job_for(const Library& library, std::size_t index, catalog::Catalog& c) {
    catalog::NewJob job;
    job.item = library.items[index];
    const auto current = library.current.find(job.item.value);
    if (current != library.current.end()) {
        job.revision = current->second;
        job.settings_sha256 = must(c.query([&](catalog::ReadTransaction& tx) { return tx.find_revision(current->second); }),
                                   "find_revision")
                                  ->info.settings_sha256;
    } else {
        job.settings_sha256 = digest_of(recipe_v1(0));  // as shot (W7 froze the version-1 default)
    }
    return job;
}

void write_batches(catalog::Catalog& c, const Era& era, const Library& library) {
    const auto t = [&](int minutes) { return era_time(era, std::chrono::minutes(120 + minutes)); };
    const std::filesystem::path out = L"Z:\\Lucent exports";
    const std::string exports = export_json();
    const auto create = [&](std::string name, const std::filesystem::path& folder, std::wstring tag,
                            std::vector<catalog::NewJob> jobs, Timestamp at, bool single = false,
                            std::uint64_t excluded = 0) {
        catalog::NewBatch b;
        b.name = std::move(name);
        b.selection_description = std::format("{} photos", jobs.size());
        b.destination_json = destination_json(folder, std::move(tag), !single);
        b.export_settings_json = exports;
        b.policy_json = policy_json(era, single, excluded);
        b.renderer_version = renderer_version(era);
        b.created_at = at;
        b.start = true;
        return must(c.transact([&](catalog::WriteTransaction& tx) { return tx.create_batch(b, jobs); }), "create_batch");
    };
    // 1. A finished export: both photos published with receipts (reported).
    const catalog::CreatedBatch done =
        create("Iceland picks", out / L"Iceland picks", L"0f1e2d3c", {job_for(library, 0, c), job_for(library, 2, c)}, t(0),
               false, 1);
    complete_job(c, era, done.id, "session-1/slot0/1", t(1), out / L"Iceland picks", exports, 4);
    complete_job(c, era, done.id, "session-1/slot1/2", t(2), out / L"Iceland picks", exports, 1);
    // 2. A cancelled export: one photo published before the cancellation, one cancelled.
    const catalog::CreatedBatch cancelled =
        create("Drone", out / L"Drone", L"1a2b3c4d", {job_for(library, 7, c), job_for(library, 1, c)}, t(10));
    complete_job(c, era, cancelled.id, "session-1/slot0/3", t(11), out / L"Drone", exports, 0);
    must(c.transact([&](catalog::WriteTransaction& tx) { return tx.cancel_batch(cancelled.id, t(12)); }), "cancel_batch");
    // 3. A paused export: a photo whose source went missing failed, one waits, one was already
    // exported.
    const catalog::CreatedBatch paused =
        create("\u00C5lesund", out / L"\u00C5lesund", L"5e6f7a8b",
               {job_for(library, 4, c), job_for(library, 6, c), job_for(library, 1, c)}, t(20));
    must(c.write([&](catalog::WriteTransaction& tx) -> Status {
        catalog::ClaimRequest request;
        request.owner = "session-1/slot0/4";
        request.now = t(21);
        request.batch = paused.id;
        auto claimed = must(tx.claim_next_job(request), "claim");
        if (!claimed) {
            return fail(ErrorCode::NotFound, "no job", "fixture");
        }
        catalog::JobFailure failure;
        failure.failure_class = catalog::FailureClass::Deterministic;
        failure.error = Error{ErrorCode::NotFound, "the original is missing: reconnect or relink it", "app.batch.source.missing",
                              2};
        must(tx.record_failure(claimed->lease, failure, t(21) + 20s), "record_failure");
        must(tx.skip_job(paused.jobs[2], Error{ErrorCode::AlreadyExists, "already exported", "app.batch.existing", 0}, t(22),
                         out / L"\u00C5lesund" / L"IMG_0002_Lucent_2.jpg"),
             "skip_job");
        return tx.pause_batch(paused.id, t(23)).map([](catalog::BatchState) {});
    }),
         "paused batch");
    if (era.version >= 4) {
        // W7 closing review: a single export (a batch of one, reported without report files) and
        // interruption counts.
        const catalog::CreatedBatch single =
            create("IMG_0003.CR3", out / L"Singles", L"9c8d7e6f", {job_for(library, 2, c)}, t(30), true);
        complete_job(c, era, single.id, "session-1/slot0/5", t(31), out / L"Singles", exports, 1);
        // The waiting photo of the paused batch was interrupted once (the computer slept).
        must(c.write([&](catalog::WriteTransaction& tx) -> Status {
            must(tx.resume_batch(paused.id, t(40)), "resume");
            catalog::ClaimRequest request;
            request.owner = "session-1/slot1/6";
            request.now = t(40);
            request.batch = paused.id;
            auto claimed = must(tx.claim_next_job(request), "claim");
            if (!claimed) {
                return fail(ErrorCode::NotFound, "no job", "fixture");
            }
            catalog::JobRecovery recovery;
            recovery.action = catalog::RecoveryAction::Requeue;
            recovery.now = t(40) + 30s;
            recovery.error = Error{ErrorCode::Timeout, "the lease expired while the computer slept",
                                   "app.batch.interrupted.lease", 0};
            recovery.owner_gone = true;
            recovery.interruption = true;
            must(tx.recover_job(claimed->lease, recovery), "recover_job");
            return tx.pause_batch(paused.id, t(44)).map([](catalog::BatchState) {});
        }),
             "interruption");
    }
    // 4. A running export whose session ended mid-render (Lucent was closed by a crash): its first
    // photo is still leased, with the temporary output the journal names.
    const catalog::CreatedBatch running =
        create("Unfinished", out / L"Unfinished", L"2468ace0", {job_for(library, 5, c), job_for(library, 6, c)}, t(50));
    must(c.write([&](catalog::WriteTransaction& tx) -> Status {
        catalog::ClaimRequest request;
        request.owner = "session-1/slot0/7";
        request.now = t(51);
        request.batch = running.id;
        auto claimed = must(tx.claim_next_job(request), "claim");
        if (!claimed) {
            return fail(ErrorCode::NotFound, "no job", "fixture");
        }
        const std::filesystem::path final_path = out / L"Unfinished" / L"DSC_0006_Lucent_0.jpg";
        must(tx.record_publication_step(claimed->lease, {catalog::PublicationStep::Reserved, std::nullopt, final_path},
                                        t(51) + 1s),
             "reserved");
        return tx.record_publication_step(claimed->lease,
                                          {catalog::PublicationStep::TempWritten,
                                           out / L"Unfinished" / std::format(L"~lucent-2468ace0-{}-a1.tmp", claimed->job.id.value),
                                           final_path},
                                          t(51) + 6s);
    }),
         "running batch");
    if (era.version >= 4) {
        // Finished batches get their report (the last one was finished but not reported yet when the
        // session ended: the next start writes its report).
        must(c.transact([&](catalog::WriteTransaction& tx) { return tx.mark_batch_reported(done.id, t(3)); }), "report");
    }
}

// W8 to W12: the editor's Auto on item 1, an automatic batch and an analyse-only batch, with the
// analysis documents and records of a real run (run_automatic on a synthetic DNG).
void write_automatic(catalog::Catalog& c, const Era& era, Library& library, const std::filesystem::path& scratch) {
    _set_FMA3_enable(0);  // as Lucent.exe and lucent-worker.exe
    raw::test_support::SyntheticDngSpec spec;
    spec.width = 256;
    spec.height = 192;
    spec.white_level = 65535;
    spec.black_level = 256;
    spec.raw_noise_profile = {2e-5, 2e-7, 2e-5, 2e-7, 2e-5, 2e-7};
    const std::vector<std::byte> dng = raw::test_support::build_synthetic_dng(spec);
    const std::filesystem::path photo = scratch / L"auto-source.dng";
    {
        std::ofstream file(photo, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(dng.data()), static_cast<std::streamsize>(dng.size()));
    }
    const raw::DecoderRegistry registry = must(raw::create_default_registry(), "registry");
    const auto t = [&](int minutes) { return era_time(era, std::chrono::minutes(300 + minutes)); };

    // A real run, with its measured durations replaced by fixed ones so the fixture is reproducible.
    const auto run_from = [&](const engine::DevelopSettings& start) {
        app::AutoRun run = must(app::run_automatic(registry, photo, 0, start, app::AutoRunRequest{}), "run_automatic");
        run.analysis_seconds = 1.9;
        run.consistency_seconds = 0.0;
        run.check_seconds = 5.0;
        run.total_seconds = 6.9;
        for (app::AutoAttempt& attempt : run.attempts) {
            attempt.check.seconds = 5.0;
        }
        return run;
    };
    const auto settings_of = [&](catalog::ItemId item) {
        const auto current = library.current.find(item.value);
        if (current == library.current.end()) {
            return engine::DevelopSettings{};
        }
        const auto revision =
            must(c.query([&](catalog::ReadTransaction& tx) { return tx.find_revision(current->second); }), "find_revision");
        return must(app::read_recipe(revision->settings_json), "read_recipe");
    };
    const auto append_auto = [&](catalog::WriteTransaction& tx, catalog::ItemId item, const app::AutoRun& run,
                                 std::optional<catalog::JobId> job, Timestamp at) -> Expected<catalog::RevisionInfo> {
        const auto revisions = must(app::automatic_revisions(run, job, at, "0.1.0"), "automatic_revisions");
        std::optional<catalog::RevisionId> parent;
        if (const auto current = library.current.find(item.value); current != library.current.end()) {
            parent = current->second;
        }
        auto appended = app::append_automatic_revisions(tx, item, parent, revisions, true);
        if (!appended) {
            return Unexpected(appended.error());
        }
        library.current[item.value] = appended->revisions.back().id;
        return appended->revisions.back();
    };

    // The editor's Auto on item 1 (no job): the latest revision is the automatic one.
    const catalog::ItemId i1 = library.items[0];
    const app::AutoRun interactive = run_from(settings_of(i1));
    must(c.transact([&](catalog::WriteTransaction& tx) { return append_auto(tx, i1, interactive, std::nullopt, t(0)); }),
         "interactive auto");

    const std::filesystem::path out = L"Z:\\Lucent exports\\Automatic";
    const std::string exports = export_json();
    for (const catalog::AutoMode mode : {catalog::AutoMode::Auto, catalog::AutoMode::AnalyzeOnly}) {
        const bool analyze_only = mode == catalog::AutoMode::AnalyzeOnly;
        const catalog::ItemId item = analyze_only ? library.items[5] : library.items[7];
        catalog::NewBatch b;
        b.name = analyze_only ? "Analyse the Nikon photo" : "Auto: drone";
        b.selection_description = "1 photo";
        b.destination_json = destination_json(out, analyze_only ? L"13579bdf" : L"02468ace");
        b.export_settings_json = exports;
        b.policy_json = policy_json(era, false);
        b.renderer_version = renderer_version(era);
        b.created_at = t(analyze_only ? 20 : 10);
        b.start = true;
        b.automatic = mode;
        catalog::NewJob job;
        job.item = item;
        job.settings_sha256 = must(app::write_recipe(engine::DevelopSettings{}), "write_recipe").sha256;
        const catalog::CreatedBatch batch =
            must(c.transact([&](catalog::WriteTransaction& tx) { return tx.create_batch(b, std::span(&job, 1)); }),
                 "create automatic batch");
        const Timestamp start = b.created_at + 1min;
        const app::AutoRun run = run_from(engine::DevelopSettings{});
        const std::string analysis = must(app::write_auto_run(run), "write_auto_run");
        catalog::ClaimRequest request;
        request.owner = analyze_only ? "session-2/slot0/1" : "session-2/slot1/2";
        request.now = start;
        request.batch = batch.id;
        const catalog::ClaimedJob claimed = must(c.transact([&](catalog::WriteTransaction& tx) {
            return tx.claim_next_job(request).and_then(
                [](std::optional<catalog::ClaimedJob> claim) -> Expected<catalog::ClaimedJob> {
                    if (!claim) {
                        return fail(ErrorCode::NotFound, "no job to claim", "fixture");
                    }
                    return *claim;
                });
        }),
                                                 "claim automatic job");
        must(c.write([&](catalog::WriteTransaction& tx) -> Status {
            must(tx.record_job_analysis(claimed.lease, analysis, start + 2s), "record_job_analysis");
            const catalog::RevisionInfo effective = must(append_auto(tx, item, run, claimed.job.id, start + 3s), "append");
            catalog::AutoFreeze freeze;
            freeze.revision = effective.id;
            freeze.settings_sha256 = effective.settings_sha256;
            must(tx.freeze_auto_job(claimed.lease, freeze, start + 3s), "freeze_auto_job");
            if (analyze_only) {
                return tx.complete_without_output(claimed.lease, start + 4s);
            }
            return ok();
        }),
             "automatic job");
        if (!analyze_only) {
            const auto revision_number = static_cast<std::uint32_t>(must(c.query([&](catalog::ReadTransaction& tx) {
                                                                             return tx.revision_history(item);
                                                                         }),
                                                                         "history")
                                                                        .revisions.size());
            // The frozen job continues its attempt: publication and receipt.
            const std::filesystem::path final_path = out / L"DJI_0008_Lucent_auto.jpg";
            const std::filesystem::path temp_path = out / std::format(L"~lucent-02468ace-{}-a1.tmp", claimed.job.id.value);
            const core::Digest256 output_sha = digest_of("fixture automatic output");
            must(c.write([&](catalog::WriteTransaction& tx) -> Status {
                const catalog::JobLease& lease = claimed.lease;
                must(tx.record_publication_step(lease, {catalog::PublicationStep::Reserved, std::nullopt, final_path}, start + 5s),
                     "reserved");
                must(tx.record_publication_step(lease, {catalog::PublicationStep::TempWritten, temp_path, final_path},
                                                start + 8s),
                     "temp");
                must(tx.advance_job(lease, catalog::JobStage::Rendering, start + 8s), "advance");
                must(tx.record_publication_step(
                         lease, {catalog::PublicationStep::Verified, temp_path, final_path, output_sha, 4'200'000}, start + 9s),
                     "verified");
                must(tx.advance_job(lease, catalog::JobStage::Verifying, start + 9s), "advance");
                must(tx.record_publication_step(
                         lease, {catalog::PublicationStep::Published, temp_path, final_path, output_sha, 4'200'000},
                         start + 10s),
                     "published");
                auto frozen = must(tx.find_job(lease.job), "find_job");
                catalog::NewReceipt receipt;
                receipt.final_path = final_path;
                receipt.size_bytes = 4'200'000;
                receipt.output_sha256 = output_sha;
                receipt.settings_json = receipt_json(era, *frozen, revision_number, exports);
                must(tx.record_receipt(lease, receipt, start + 10s), "receipt");
                must(tx.advance_job(lease, catalog::JobStage::Publishing, start + 11s), "advance");
                return tx.mark_batch_reported(batch.id, start + 12s);
            }),
                 "automatic export");
        }
    }
}

// ---- Fixture files ------------------------------------------------------------------------------

std::unique_ptr<catalog::detail::Connection> connect(const std::filesystem::path& path) {
    return must(catalog::detail::Connection::open(path, catalog::detail::Access::ReadWrite, 5000ms, "fixture"),
                "open connection");
}

std::vector<std::string> column_names(catalog::detail::Connection& db, std::string_view schema, std::string_view table) {
    auto statement = must(db.prepare("SELECT name FROM pragma_table_info(?1, ?2) ORDER BY cid", "fixture"),
                          "prepare table_info");
    statement.bind_text(1, table).bind_text(2, schema);
    std::vector<std::string> names;
    while (must(statement.step(), "table_info")) {
        names.emplace_back(statement.column_text(0));
    }
    return names;
}

std::int64_t count_rows(catalog::detail::Connection& db, std::string_view table) {
    return must(db.query_int64(std::format("SELECT count(*) FROM {}", table), "fixture"), "count");
}

// The tables a fixture fills, in an order that satisfies their foreign keys.
constexpr std::string_view kTables[] = {"imports",  "sources",  "items",
                                        "recipe_revisions", "batches", "jobs",
                                        "job_events", "publication_journal", "receipts",
                                        "auto_analyses"};

Json make_fixture(const Era& era, const std::filesystem::path& folder, const std::filesystem::path& scratch_folder) {
    std::error_code ignored;
    const std::filesystem::path scratch = scratch_folder / std::format(L"scratch-v{}.catalog", era.version);
    std::filesystem::remove(scratch, ignored);
    {
        catalog::OpenOptions options;
        options.path = scratch;
        options.app_version = "0.1.0";
        options.backup_directory = scratch_folder / L"backups";
        std::shared_ptr<catalog::Catalog> c = must(catalog::Catalog::open(options), "scratch catalog");
        Library library;
        write_library(*c, era, library);
        if (era.version >= 2) {
            write_recipes(*c, era, library);
        }
        if (era.version >= 5) {
            write_automatic(*c, era, library, scratch_folder);
        }
        if (era.version >= 3) {
            write_batches(*c, era, library);
        }
    }

    const std::filesystem::path fixture = folder / std::format(L"v{}.catalog", era.version);
    for (const wchar_t* suffix : {L"", L"-wal", L"-shm", L".lock"}) {
        std::filesystem::remove(fixture.native() + suffix, ignored);
    }
    {
        // Exactly the migrations a version-v build ran on an empty file.
        catalog::OpenOptions options;
        options.path = fixture;
        options.app_version = "0.1.0";
        options.backup_directory = scratch_folder / L"backups";
        const auto migrations =
            catalog::detail::builtin_migrations().first(static_cast<std::size_t>(era.version));
        must(catalog::detail::CatalogAccess::open(options, migrations), "fixture schema");
    }
    auto db = connect(fixture);
    must(db->exec("PRAGMA foreign_keys = ON", "fixture"), "foreign keys");
    const std::string scratch_text = core::path_to_utf8_lossy(scratch);
    std::string attach = "ATTACH DATABASE '";
    for (const char ch : scratch_text) {
        attach += ch == '\'' ? std::string("''") : std::string(1, ch);
    }
    attach += "' AS scratch";
    must(db->exec(attach, "fixture"), "attach");
    must(db->exec("BEGIN IMMEDIATE", "fixture"), "begin");
    Json counts = Json::object();
    for (const std::string_view table : kTables) {
        const std::vector<std::string> columns = column_names(*db, "main", table);
        if (columns.empty()) {
            continue;  // not in this schema version
        }
        const std::vector<std::string> source = column_names(*db, "scratch", table);
        std::string list;
        for (const std::string& column : columns) {
            // items.current_revision_id is set after the revisions exist (they reference the items).
            if (std::find(source.begin(), source.end(), column) == source.end() ||
                (table == "items" && column == "current_revision_id")) {
                continue;
            }
            list += (list.empty() ? "" : ", ") + column;
        }
        must(db->exec(std::format("INSERT INTO main.{0} ({1}) SELECT {1} FROM scratch.{0} ORDER BY rowid", table, list),
                      "fixture"),
             std::format("copy {}", table));
        if (table == "recipe_revisions") {
            must(db->exec("UPDATE main.items SET current_revision_id = (SELECT s.current_revision_id FROM scratch.items s "
                          "WHERE s.id = main.items.id)",
                          "fixture"),
                 "current revisions");
        }
        counts[std::string(table)] = count_rows(*db, std::format("main.{}", table));
    }
    // The schema history as the builds of each version wrote it: created by the W4 build, migrated by
    // the build of each later version at its release.
    for (int v = 1; v <= era.version; ++v) {
        must(db->exec(std::format("UPDATE schema_history SET applied_at = {}, app_version = '0.1.0' WHERE version = {}",
                                  to_ms(era_time(kEras[v - 1])), v),
                      "fixture"),
             "history");
    }
    must(db->exec(std::format("UPDATE catalog_meta SET value = '{}' WHERE key = 'created_at'", to_ms(era_time(kEras[0]))),
                  "fixture"),
         "meta");
    must(db->exec("COMMIT", "fixture"), "commit");
    must(db->exec("DETACH DATABASE scratch", "fixture"), "detach");
    const std::int64_t violations = must(db->query_int64("SELECT count(*) FROM pragma_foreign_key_check", "fixture"), "fk");
    const std::string integrity = must(db->query_text("PRAGMA integrity_check", "fixture"), "integrity");
    if (violations != 0 || integrity != "ok") {
        die(std::format("v{}: {} foreign key violations, integrity '{}'", era.version, violations, integrity));
    }
    must(db->exec("PRAGMA journal_mode = DELETE; VACUUM;", "fixture"), "vacuum");
    const std::string min_reader =
        must(db->query_text("SELECT value FROM catalog_meta WHERE key = 'min_reader_version'", "fixture"), "meta");
    db.reset();
    for (const wchar_t* suffix : {L"-wal", L"-shm", L".lock"}) {
        std::filesystem::remove(fixture.native() + suffix, ignored);
    }
    std::filesystem::remove(scratch, ignored);
    for (const wchar_t* suffix : {L"-wal", L"-shm", L".lock"}) {
        std::filesystem::remove(scratch.native() + suffix, ignored);
    }

    const core::FileHashResult hashed = must(core::hash_file(fixture, core::CancellationToken{}), "hash fixture");
    Json entry;
    entry["file"] = core::path_to_utf8_lossy(fixture.filename());
    entry["schema_version"] = era.version;
    entry["min_reader_version"] = std::stoi(min_reader);
    entry["written_by"] = std::string(era.build);
    entry["introduced_by_commit"] = std::string(era.commit);
    entry["size"] = hashed.identity.size_bytes;
    entry["sha256"] = core::to_hex(hashed.digest);
    entry["rows"] = counts;
    std::cout << std::format("v{}: {} bytes, {}\n", era.version, hashed.identity.size_bytes, counts.dump());
    return entry;
}

}  // namespace
}  // namespace lucent::fixtures

int wmain(int argc, wchar_t** argv) {
    using namespace lucent::fixtures;
    if (argc != 2) {
        std::cerr << "usage: lucent_catalog_fixture_generator <output folder>\n";
        return 2;
    }
    const std::filesystem::path folder = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(folder);
    const std::filesystem::path scratch = std::filesystem::temp_directory_path() /
                                          std::format(L"lucent-catalog-fixtures-{}", ::GetCurrentProcessId());
    std::filesystem::create_directories(scratch);
    Json fixtures = Json::array();
    for (const Era& era : kEras) {
        fixtures.push_back(make_fixture(era, folder, scratch));
    }
    Json manifest;
    manifest["schema"] = "lucent.catalog-fixtures";
    manifest["version"] = 1;
    manifest["generator"] = "tests/catalog_fixtures/generate_catalog_fixtures.cpp";
    manifest["fixtures"] = fixtures;
    std::ofstream(folder / L"fixtures.json", std::ios::binary | std::ios::trunc) << manifest.dump(2) << "\n";
    std::error_code ignored;
    std::filesystem::remove_all(scratch, ignored);
    return 0;
}
