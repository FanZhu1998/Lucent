#pragma once

// Export of one library item (blueprint section 2, step 3; section 7, "Export integrity and
// user-selected destinations").
//
// W7 (ADR 0012, decision "Single exports are batches of one"): an ExportService given a
// BatchService exports through it whenever it can, so a single export is a durable batch of one
// photo: persisted before dispatch, rendered by an isolated lucent-worker process, published with
// the journaled protocol and recorded with a receipt (batch_service.hpp). The request maps to a
// batch with the W3 naming policy below (kSingleExportNamePattern, unique names), the request's
// output settings and ExistingOutputs::ExportAgain (asking again makes a new file), without report
// files. The executor task waits for the batch and builds the ExportRecord from its receipt, so
// callers see the same contract. Cancelling the export cancels the batch. The in-process path below
// remains for a library without a catalog (the session library: tests, and Lucent.exe when its
// catalog cannot be opened) and for explicit settings that no recipe revision records (the journal
// could not reproduce them after a restart).
//
// Flow of export_item(): validate the request -> estimate memory and check the destination's free
// space -> admit the render (MemoryAdmission) -> render (the engine's develop pipeline, the export
// render of SourceRenderer::render: render_develop_file's composition, ADR 0010) -> convert to the
// output profile and quantise (exporter::encode_for_output) -> release the render memory -> choose
// a free name -> exporter::write_verified (temporary sibling, flush, re-read verification, SHA-256,
// no-clobber publication) -> ExportRecord.
//
// Recipes (blueprint section 2: "an already queued export retains its frozen revision"; section 7:
// a job's identity includes "recipe revision/hash + renderer and asset versions"). A request either
// names the settings to render or none: ExportService::submit() then freezes the item's current
// recipe (RecipeService::current_settings) at submission, so a later edit never changes a queued
// export, and an item whose recipe this build cannot read is refused (never exported with other
// settings). The record's RecipeIdentity holds the revision, the canonical settings document and its
// SHA-256, the operation and base-look versions and the pipeline version.
//
// Naming: "{original-stem}[_img{k}]_Lucent[_{n}].{jpg|tif}" in the output directory.
//   * {original-stem} is the source file name without its extension ("IMG_0001" for
//     "IMG_0001.CR3"), truncated (never splitting a surrogate pair) so that the whole name fits
//     the 255-character component limit; a stem that is a reserved Windows device name (CON,
//     PRN, AUX, NUL, COM0-9, LPT0-9, before its first dot) is prefixed with "_".
//   * "_img{k}" (k = subimage + 1) appears only for items from multi-image files.
//   * The first choice has no "_{n}". When a name is taken, n = 2, 3, ... up to
//     kMaxExportNameIndex. "Taken" is decided case-insensitively with the file system's casing
//     rules against a listing of the directory, so "img_0001_lucent.JPG" blocks
//     "IMG_0001_Lucent.jpg" even in a case-sensitive directory.
//   * Publication never overwrites (exporter::write_verified): if another file appears under the
//     chosen name after the listing, publication fails with AlreadyExists and the next free name
//     is chosen (at most kMaxPublicationAttempts attempts). Originals are never targets.
//
// Unsupported items (SupportStatus::Unsupported: a mandatory interpretation step is missing) are
// refused unless ExportRequest::allow_unsupported is set, in which case the record says so; the
// blueprint forbids exporting them silently.
//
// Callbacks (ExportService): progress and completion run on an executor worker thread (also for
// exports through a batch); the completion is called exactly once per accepted request. They must
// not destroy the service.
//
// Thread-safety: export_item() is reentrant; ExportService members are safe to call concurrently.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <lucent/application/library.hpp>
#include <lucent/application/preview.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/source_renderer.hpp>
#include <lucent/application/task_executor.hpp>
#include <lucent/catalog/types.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/export/export.hpp>

namespace lucent::app {

class RecipeService;
class BatchService;

inline constexpr std::uint32_t kMaxExportNameIndex = 9999;
inline constexpr std::uint32_t kMaxPublicationAttempts = 32;

// The recipe an export rendered (see the file comment).
struct RecipeIdentity {
    std::optional<catalog::RevisionId> revision;  // none: explicit settings without a revision, or as shot
    std::uint32_t revision_number = 0;            // of `revision` (0: none)
    std::string schema{kRecipeSchema};
    std::uint32_t schema_version = kRecipeSchemaVersion;
    std::string settings_json;          // the canonical settings document (recipe.hpp)
    core::Digest256 settings_sha256{};  // SHA-256 of settings_json
    engine::DevelopOperationVersions operations;
    std::string base_look;
    std::uint32_t base_look_version = 0;
    std::uint32_t pipeline_version = engine::kDevelopPipelineVersion;
};

// The identity of `settings` (validated; errors of write_recipe()).
[[nodiscard]] Expected<RecipeIdentity> recipe_identity(
    const engine::DevelopSettings& settings, std::optional<catalog::RevisionId> revision = std::nullopt,
    std::uint32_t revision_number = 0);

struct ExportRequest {
    ItemId item;
    // The settings to render; std::nullopt: the item's current recipe, frozen by
    // ExportService::submit() (export_item() needs them resolved: InvalidArgument otherwise).
    std::optional<engine::DevelopSettings> settings;
    // With explicit settings, the revision they are (recorded in the RecipeIdentity).
    std::optional<catalog::RevisionId> recipe_revision;
    std::uint32_t recipe_revision_number = 0;
    std::uint64_t settings_revision = 0;     // caller's tag, recorded
    std::filesystem::path output_directory;  // must exist
    // Format (JPEG or 16-bit TIFF), output profile, rendering intent, JPEG quality, TIFF
    // compression. Default: JPEG / sRGB / quality 95.
    exporter::ExportSettings output = exporter::ExportSettings::jpeg_srgb_q95();
    bool allow_unsupported = false;  // export an Unsupported item anyway (recorded)
};

enum class ExportStage : std::uint8_t {
    Preparing = 1,  // validation, estimate, free-space check, waiting for memory admission
    Rendering = 2,
    Encoding = 3,
    Writing = 4,  // temporary file, verification, hashing, publication
    Finished = 5,
};

[[nodiscard]] std::string_view to_string(ExportStage stage) noexcept;

struct ExportProgress {
    ItemId item;
    ExportStage stage = ExportStage::Preparing;
    double fraction = 0.0;  // overall, 0..1, non-decreasing
};

struct ExportRecord {
    ItemId item;
    std::filesystem::path source_path;
    std::uint32_t subimage = 0;
    core::FileIdentity source_identity;
    std::optional<core::Digest256> source_sha256;      // the item's content hash when it was exported
    std::uint64_t settings_revision = 0;               // the request's tag
    engine::DevelopSettings settings;                  // rendered
    RecipeIdentity recipe;                             // their identity (see the file comment)
    SupportStatus support = SupportStatus::CompatibleUnverified;  // of the item when it was exported
    exporter::ExportReceipt receipt;  // published path, SHA-256, dimensions, profile, encoder
    std::uint32_t name_index = 1;     // 1: no "_{n}" suffix
    std::uint32_t publication_attempts = 1;
    RenderSummary render;
    double render_seconds = 0.0;  // admission wait excluded
    double encode_seconds = 0.0;
    double write_seconds = 0.0;
    double total_seconds = 0.0;
    std::chrono::system_clock::time_point finished_at;
};

using ExportProgressCallback = std::function<void(const ExportProgress&)>;
using ExportCompletion = std::function<void(ItemId, const Expected<ExportRecord>&)>;

// ".jpg" or ".tif"; empty for an unknown format.
[[nodiscard]] std::wstring_view export_extension(exporter::ExportFormat format) noexcept;

// The file name for `source` (see the file comment). InvalidArgument ("app.export.name") for an
// unknown format, index 0, subimage >= subimage_count or a source without a file name;
// LimitExceeded above kMaxExportNameIndex.
[[nodiscard]] Expected<std::filesystem::path> export_file_name(const std::filesystem::path& source,
                                                               std::uint32_t subimage,
                                                               std::uint32_t subimage_count,
                                                               exporter::ExportFormat format,
                                                               std::uint32_t index);

struct ChosenName {
    std::filesystem::path path;  // directory / name
    std::uint32_t index = 1;
};

// The first name with index >= first_index that no entry of `directory` matches
// case-insensitively. Errors: those of export_file_name; the listing's (NotFound,
// PermissionDenied, IoError); LimitExceeded when every index up to kMaxExportNameIndex is taken.
[[nodiscard]] Expected<ChosenName> choose_export_name(const std::filesystem::path& directory,
                                                      const std::filesystem::path& source,
                                                      std::uint32_t subimage, std::uint32_t subimage_count,
                                                      exporter::ExportFormat format,
                                                      std::uint32_t first_index = 1);

// Exports `item` synchronously on the calling thread with request.settings (which must be set).
// `admission` may be null (no admission).
// Errors ("app.export.*" for the service's own checks): InvalidArgument (no or invalid settings,
// invalid output settings, empty directory); NotFound (the output directory does not exist);
// Unsupported (an Unsupported item without allow_unsupported); DiskFull (the destination volume
// has less free space than the uncompressed output); SourceChanged; LimitExceeded; Cancelled;
// and the errors of the renderer, the exporter and publication.
[[nodiscard]] Expected<ExportRecord> export_item(const SourceItem& item, const ExportRequest& request,
                                                 const SourceRenderer& renderer, MemoryAdmission* admission,
                                                 Lane lane, const ExportProgressCallback& progress,
                                                 const core::CancellationToken& cancel);

// Runs export_item() on the executor's Background lane. Each submitted export occupies one
// Background thread from the moment it starts, including while it waits for the memory admission
// (at most one full-resolution render at a time), so a client with a queue of exports should
// submit them one at a time, the next when the previous one has completed (as Lucent.exe's
// adapter does): otherwise waiting exports fill the Background lane and imports wait behind them.
class ExportService {
public:
    // The library, the executor, `recipes` and `batches` (when not null) must outlive the service.
    // With a null `renderer` every submit() fails with InvalidArgument. Without `recipes`, requests
    // without settings export "as shot". With `batches`, exports go through durable batches of one
    // (see the file comment).
    ExportService(Library& library, TaskExecutor& executor, std::shared_ptr<const SourceRenderer> renderer,
                  RecipeService* recipes = nullptr, BatchService* batches = nullptr);
    // Cancels outstanding exports and waits until their completions have returned.
    ~ExportService();
    ExportService(const ExportService&) = delete;
    ExportService& operator=(const ExportService&) = delete;

    // Queues an export, freezing its recipe (see the file comment); the handle cancels it. Errors,
    // with no callback ever called ("app.export"): InvalidArgument for a null completion or invalid
    // settings; NotFound for an unknown item; the recipe's error when the item's current recipe
    // cannot be read; Cancelled when the service or executor is shutting down.
    [[nodiscard]] Expected<TaskHandle> submit(const ExportRequest& request, ExportProgressCallback progress,
                                              ExportCompletion completion);

    struct State;  // implementation detail

private:
    std::shared_ptr<State> state_;
};

}  // namespace lucent::app
