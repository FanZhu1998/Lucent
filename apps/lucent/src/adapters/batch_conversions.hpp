#pragma once

// Conversions between the durable batch service (app::BatchService, batch_service.hpp) and the
// shell's batch types (services/batch_service.hpp), and the user-facing wording of its states,
// attentions, exclusions and errors (lucent::Error::message is developer-facing and never shown;
// conversions.hpp).
//
// Pure functions of their arguments with no file-system access, safe on any thread.

#include <QDateTime>
#include <QString>
#include <optional>

#include <lucent/application/batch_service.hpp>
#include <lucent/catalog/jobs.hpp>
#include <lucent/core/error.hpp>
#include <lucent/workers/protocol.hpp>

#include "services/batch_service.hpp"

namespace lucent::ui::adapters {

// The request for `setup`: the items, or the input folder (imported first); the destination; the
// preset with the metadata choices (capture, Artist / Copyright, GPS only when chosen); the
// existing-output policy. An empty artist or copyright keeps what the camera recorded.
[[nodiscard]] app::BatchRequest to_batch_request(const BatchSetup& setup);

[[nodiscard]] BatchState to_ui_state(catalog::BatchState state) noexcept;
[[nodiscard]] BatchAttention to_ui_attention(app::BatchAttention attention) noexcept;

// What happened and what to do, naming the output folder (native separators); empty for None.
[[nodiscard]] QString describe_attention(BatchAttention attention, const QString& folder);

// "Preparing", "Rendering", "Encoding", "Writing", "Verifying".
[[nodiscard]] QString describe_worker_stage(workers::JobStage stage);

// Why a photo is left out of a batch (PlanExclusion::reason).
[[nodiscard]] QString describe_exclusion(const Error& reason);

// Why a pattern is not valid (empty when it is): names the problem in the user's terms.
[[nodiscard]] QString describe_pattern_problem(const QString& pattern);

// Why planning or creating a batch failed.
[[nodiscard]] QString describe_batch_error(const Error& error);

// The plan as the dialog shows it. `folder_batch`: the setup selected a folder.
[[nodiscard]] BatchPlanInfo to_plan_info(const app::BatchPlan& plan);
// A failed plan: the reason, or for a folder batch whose photos are not listed yet,
// folder_not_listed with no error.
[[nodiscard]] BatchPlanInfo to_plan_failure(const Error& error, bool folder_batch, const QString& pattern);

// A snapshot of the status (counts, attention, running jobs, ETA).
[[nodiscard]] BatchSnapshot to_snapshot(const app::BatchStatus& status);
// Applies the state and counts an event or record carries.
void apply_counts(BatchSnapshot& snapshot, catalog::BatchState state, const catalog::BatchCounts& counts);

[[nodiscard]] BatchReportInfo to_report_info(const app::BatchReport& report);

// The step of a submission ("Adding the folder's photos… 12 of 40") and its fraction (-1 unknown).
[[nodiscard]] QString describe_submit_progress(const app::BatchSubmitProgress& progress);
[[nodiscard]] double submit_fraction(const app::BatchSubmitProgress& progress) noexcept;

[[nodiscard]] QDateTime to_qdatetime(catalog::Timestamp time);

// "Lucent-batch-<id>-report.html" in `folder` (ADR 0012, "Completion report").
[[nodiscard]] std::filesystem::path report_html_path(const std::filesystem::path& folder, catalog::BatchId batch);

}  // namespace lucent::ui::adapters
