#pragma once

// Export of one library item (blueprint section 2, step 3; section 7, "Export integrity and
// user-selected destinations"). Batches, durable job records and receipts on disk are W7's.
//
// Flow of export_item(): validate the request -> estimate memory and check the destination's free
// space -> admit the render (MemoryAdmission) -> render (engine) -> convert to the output profile
// and quantise (exporter::encode_for_output) -> release the render memory -> choose a free name
// -> exporter::write_verified (temporary sibling, flush, re-read verification, SHA-256,
// no-clobber publication) -> ExportRecord.
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
// Callbacks (ExportService): progress and completion run on an executor worker thread; the
// completion is called exactly once per accepted request. They must not destroy the service.
//
// Thread-safety: export_item() is reentrant; ExportService members are safe to call concurrently.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

#include <lucent/application/library.hpp>
#include <lucent/application/preview.hpp>
#include <lucent/application/source_renderer.hpp>
#include <lucent/application/task_executor.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/file.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/export/export.hpp>

namespace lucent::app {

inline constexpr std::uint32_t kMaxExportNameIndex = 9999;
inline constexpr std::uint32_t kMaxPublicationAttempts = 32;

struct ExportRequest {
    ItemId item;
    engine::RenderSettings settings;
    std::uint64_t settings_revision = 0;
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
    std::uint64_t settings_revision = 0;
    engine::RenderSettings settings;
    SupportStatus support = SupportStatus::Supported;  // of the item when it was exported
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

// Exports `item` synchronously on the calling thread. `admission` may be null (no admission).
// Errors ("app.export.*" for the service's own checks): InvalidArgument (invalid settings or
// output settings, empty directory); NotFound (the output directory does not exist);
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
    // The library and the executor must outlive the service. With a null `renderer` every submit()
    // fails with InvalidArgument.
    ExportService(Library& library, TaskExecutor& executor, std::shared_ptr<const SourceRenderer> renderer);
    // Cancels outstanding exports and waits until their completions have returned.
    ~ExportService();
    ExportService(const ExportService&) = delete;
    ExportService& operator=(const ExportService&) = delete;

    // Queues an export; the handle cancels it. Errors, with no callback ever called
    // ("app.export"): InvalidArgument for a null completion or invalid settings; NotFound for an
    // unknown item; Cancelled when the service or executor is shutting down.
    [[nodiscard]] Expected<TaskHandle> submit(const ExportRequest& request, ExportProgressCallback progress,
                                              ExportCompletion completion);

    struct State;  // implementation detail

private:
    std::shared_ptr<State> state_;
};

}  // namespace lucent::app
