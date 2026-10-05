#pragma once

// User-facing text for batches (the Exports view, the report pane and the Export batch dialog).
// Pure functions of their arguments (locale included), safe on any thread. Strings go through
// QCoreApplication::translate (context "lucent::ui::presentation").

#include <QLocale>
#include <QString>

#include "services/batch_service.hpp"

namespace lucent::ui::presentation {

// Stable keys for styling in QML: "draft", "running", "waiting" (running, nothing started yet),
// "paused", "attention", "completed", "failed" (finished with failures and nothing exported),
// "partial" (finished with some failures), "cancelled".
[[nodiscard]] QString batch_state_key(const BatchSnapshot& batch);
// "Running", "Paused", "Needs attention", "Completed", "Completed with failures", "Failed",
// "Cancelled"; a single export says what happened to its photo ("Exported", "Skipped", "Failed",
// "Not exported" when the destination stopped it, "Exporting", "Waiting").
[[nodiscard]] QString batch_state_label(const BatchSnapshot& batch);

// Finished share of the photos, the running ones counted by their progress: 0..1.
[[nodiscard]] double batch_progress(const BatchSnapshot& batch);

// What is happening now: "Rendering IMG_0001.CR3 (42 %) · Writing DSC_0751.NEF (90 %)",
// "Waiting to start", "Finishing 1 photo before pausing", "Paused"; empty when finished.
[[nodiscard]] QString batch_stage_label(const BatchSnapshot& batch, const QLocale& locale);

// "About 40 s left", "About 3 min left", "About 1 h 5 min left"; empty for a negative value.
[[nodiscard]] QString eta_label(double seconds, const QLocale& locale);

// "3 of 5 photos" or "1 photo".
[[nodiscard]] QString photo_count_label(qint64 count);

// "Running · 3 of 5 exported · 1 failed · 2 left out · about 2 min left" for screen readers (the
// time only while the batch runs).
[[nodiscard]] QString batch_accessible_label(const BatchSnapshot& batch, const QLocale& locale);

// "exported", "skipped", "failed", "cancelled", "pending", "analysed" and their labels.
[[nodiscard]] QString outcome_key(BatchOutcome outcome);
[[nodiscard]] QString outcome_label(BatchOutcome outcome);

// ---- Automatic batches (W8) ---------------------------------------------------------------------

// "off", "auto", "analyzeOnly".
[[nodiscard]] QString automatic_key(BatchAutomatic automatic);
// "Auto", "Analyse only" (", related shots kept consistent" when they are); empty when off.
[[nodiscard]] QString automatic_label(const BatchSnapshot& batch);
// "3 exported", or "3 analysed" for an analysing-only batch.
[[nodiscard]] QString completed_label(const BatchSnapshot& batch, const QLocale& locale);
// The related-shot pre-pass of a running batch: "Analysing related shots together first: 3 of 5
// analysed"; empty otherwise (each photo's own stage, "Analysing" or "Checking at full size", is in
// batch_stage_label).
[[nodiscard]] QString analysis_label(const BatchSnapshot& batch, const QLocale& locale);

// "Created 4 Oct 2026 14:03 · finished 14:05".
[[nodiscard]] QString batch_times_label(const QDateTime& created, const QDateTime& finished, const QLocale& locale);

}  // namespace lucent::ui::presentation
