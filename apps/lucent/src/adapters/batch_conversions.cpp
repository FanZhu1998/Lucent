#include "adapters/batch_conversions.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <string_view>

#include <lucent/application/batch_destination.hpp>
#include <lucent/core/text.hpp>

#include "adapters/conversions.hpp"

namespace lucent::ui::adapters {
namespace {

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::adapters", text);
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

QString from_utf8(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString native(const std::filesystem::path& path) {
    return QDir::toNativeSeparators(to_qstring(path));
}

QString file_name_of(const std::filesystem::path& path) {
    return to_qstring(path.filename());
}

qint64 to_qint(std::uint64_t value) {
    return static_cast<qint64>(std::min<std::uint64_t>(value, static_cast<std::uint64_t>(INT64_MAX)));
}

BatchOutcome to_ui_outcome(app::ReportOutcome outcome) {
    switch (outcome) {
        case app::ReportOutcome::Exported:
            return BatchOutcome::Exported;
        case app::ReportOutcome::Skipped:
            return BatchOutcome::Skipped;
        case app::ReportOutcome::Failed:
            return BatchOutcome::Failed;
        case app::ReportOutcome::Cancelled:
            return BatchOutcome::Cancelled;
        case app::ReportOutcome::Analysed:
            return BatchOutcome::Analysed;
        case app::ReportOutcome::Pending:
            break;
    }
    return BatchOutcome::Pending;
}

QString data_size(std::uint64_t bytes) {
    return QLocale().formattedDataSize(static_cast<qint64>(std::min<std::uint64_t>(bytes, INT64_MAX)), 1,
                                       QLocale::DataSizeTraditionalFormat);
}

QString format_of(const app::ReceiptDetails& details) {
    const QString format =
        details.format == exporter::ExportFormat::Tiff ? tr_text("TIFF") : tr_text("JPEG");
    const QString profile =
        details.profile == exporter::OutputProfile::DisplayP3 ? tr_text("Display P3") : tr_text("sRGB");
    const QString depth =
        details.bit_depth == exporter::BitDepth::Sixteen ? tr_text("16-bit") : tr_text("8-bit");
    return QStringLiteral("%1 · %2 · %3").arg(format, profile, depth);
}

}  // namespace

app::BatchRequest to_batch_request(const BatchSetup& setup) {
    app::BatchRequest request;
    request.name = setup.name.trimmed().toStdString();
    for (const ItemId item : setup.items) {
        request.items.push_back(app::BatchItem{app::ItemId{item}, std::nullopt});
    }
    if (!setup.input_folder.isEmpty()) {
        request.folder = app::BatchFolder{to_path(QDir::cleanPath(setup.input_folder)), setup.include_subfolders};
    }
    request.destination.folder = to_path(QDir::cleanPath(setup.output_folder));
    request.destination.name_pattern = setup.name_pattern.toStdString();
    request.destination.mirror_subfolders = setup.mirror_subfolders;
    request.destination.collisions = setup.collisions == BatchCollisions::Overwrite
                                         ? app::CollisionPolicy::Overwrite
                                         : app::CollisionPolicy::UniqueName;
    request.output = to_export_settings(setup.output);
    request.output.metadata.include_capture = setup.include_capture;
    request.output.metadata.include_copyright = setup.include_rights;
    request.output.metadata.include_gps = setup.include_gps;
    request.output.metadata.include_software = true;
    if (setup.include_rights) {
        // An empty field keeps what the camera recorded (std::nullopt).
        if (const QString artist = setup.artist.trimmed(); !artist.isEmpty()) {
            request.artist = artist.toStdString();
        }
        if (const QString copyright = setup.copyright.trimmed(); !copyright.isEmpty()) {
            request.copyright = copyright.toStdString();
        }
    }
    request.existing_outputs =
        setup.existing == BatchExisting::ExportAgain ? app::ExistingOutputs::ExportAgain : app::ExistingOutputs::Skip;
    request.allow_unsupported = false;  // blueprint section 3: never exported silently
    request.start = true;
    request.write_report_files = true;
    switch (setup.automatic) {
        case BatchAutomatic::Auto:
            request.automatic = app::AutomaticSettings::Auto;
            break;
        case BatchAutomatic::AnalyzeOnly:
            request.automatic = app::AutomaticSettings::AnalyzeOnly;
            break;
        case BatchAutomatic::Off:
            request.automatic = app::AutomaticSettings::Off;
            break;
    }
    // Keeping related photos consistent is part of the automatic settings only.
    request.keep_consistent = request.automatic != app::AutomaticSettings::Off && setup.keep_consistent;
    return request;
}

BatchAutomatic to_ui_automatic(catalog::AutoMode mode) noexcept {
    switch (mode) {
        case catalog::AutoMode::Auto:
            return BatchAutomatic::Auto;
        case catalog::AutoMode::AnalyzeOnly:
            return BatchAutomatic::AnalyzeOnly;
        case catalog::AutoMode::None:
            break;
    }
    return BatchAutomatic::Off;
}

BatchState to_ui_state(catalog::BatchState state) noexcept {
    switch (state) {
        case catalog::BatchState::Draft:
            return BatchState::Draft;
        case catalog::BatchState::Running:
            return BatchState::Running;
        case catalog::BatchState::Paused:
            return BatchState::Paused;
        case catalog::BatchState::Cancelled:
            return BatchState::Cancelled;
        case catalog::BatchState::Completed:
            return BatchState::Completed;
    }
    return BatchState::Draft;
}

BatchAttention to_ui_attention(app::BatchAttention attention) noexcept {
    switch (attention) {
        case app::BatchAttention::DestinationFull:
            return BatchAttention::DestinationFull;
        case app::BatchAttention::DestinationUnavailable:
            return BatchAttention::DestinationUnavailable;
        case app::BatchAttention::DestinationPermission:
            return BatchAttention::DestinationPermission;
        case app::BatchAttention::DestinationReplaced:
            return BatchAttention::DestinationReplaced;
        case app::BatchAttention::None:
            break;
    }
    return BatchAttention::None;
}

QString describe_attention(BatchAttention attention, const QString& folder, std::optional<std::uint64_t> free_bytes,
                           std::uint64_t needed_bytes) {
    const QString where = folder.isEmpty() ? tr_text("the output folder") : QDir::toNativeSeparators(folder);
    switch (attention) {
        case BatchAttention::DestinationFull:
            if (free_bytes && needed_bytes > 0) {
                return tr_text("The drive of %1 is full (%2 free; the next photo may need up to %3), so the batch is "
                               "paused. Free some space on it, then choose Resume.")
                    .arg(where, data_size(*free_bytes), data_size(needed_bytes));
            }
            return tr_text("The drive of %1 is full, so the batch is paused. Free some space on it, then choose "
                           "Resume.")
                .arg(where);
        case BatchAttention::DestinationReplaced:
            return tr_text("%1 is not the folder this batch started in: it was created again, or another drive now "
                           "uses its drive letter. Choose Use this folder to continue there, or reconnect the "
                           "original drive and choose Resume.")
                .arg(where);
        case BatchAttention::DestinationUnavailable:
            return tr_text("%1 is not available: its drive may have been disconnected, so the batch is paused. "
                           "Reconnect it, then choose Resume.")
                .arg(where);
        case BatchAttention::DestinationPermission:
            return tr_text("Lucent is no longer allowed to create files in %1, so the batch is paused. Check the "
                           "folder's permissions, then choose Resume.")
                .arg(where);
        case BatchAttention::None:
            break;
    }
    return {};
}

QString describe_worker_stage(workers::JobStage stage) {
    switch (stage) {
        case workers::JobStage::Preparing:
            return tr_text("Preparing");
        case workers::JobStage::Rendering:
            return tr_text("Rendering");
        case workers::JobStage::Encoding:
            return tr_text("Encoding");
        case workers::JobStage::Writing:
            return tr_text("Writing");
        case workers::JobStage::Verifying:
            return tr_text("Verifying");
        case workers::JobStage::Analysing:
            return tr_text("Analysing");
        case workers::JobStage::Checking:
            return tr_text("Checking at full size");
    }
    return tr_text("Preparing");
}

QString describe_exclusion(const Error& reason) {
    const std::string_view where = reason.where;
    if (starts_with(where, "app.recipe")) {
        return describe_error(reason);  // the photo's edit cannot be read
    }
    switch (reason.code) {
        case ErrorCode::Unsupported:
            return tr_text("Unsupported: Lucent cannot render this camera file faithfully yet.");
        case ErrorCode::NotFound:
            if (where == "app.batch.source.missing") {
                return tr_text("The original file is missing. Reconnect its drive or relink the photo.");
            }
            return tr_text("The photo is no longer in the library.");
        case ErrorCode::SourceChanged:
            return tr_text("The original changed after it was added. Add it to the library again first.");
        case ErrorCode::LimitExceeded:
            return tr_text("The output path would be too long. Choose a shorter folder or name pattern.");
        default:
            break;
    }
    return describe_error(reason);
}

QString describe_pattern_problem(const QString& pattern) {
    const QString trimmed = pattern.trimmed();
    if (trimmed.isEmpty()) {
        return tr_text("Enter a name pattern, for example {original-stem}_Lucent_{revision}.");
    }
    if (app::parse_name_pattern(pattern.toStdString())) {
        return {};
    }
    if (pattern.toUtf8().size() > static_cast<qsizetype>(app::kMaxNamePatternBytes)) {
        return tr_text("The name pattern is too long.");
    }
    static const QRegularExpression forbidden(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
    if (forbidden.match(pattern).hasMatch()) {
        return tr_text("A file name cannot contain \\ / : * ? \" < > | or control characters.");
    }
    static const QRegularExpression token(QStringLiteral("\\{([^{}]*)\\}"));
    for (auto it = token.globalMatch(pattern); it.hasNext();) {
        const QString name = it.next().captured(1);
        if (name != u"original-stem" && name != u"revision" && name != u"image" && name != u"sequence") {
            return tr_text("{%1} is not a token. Use {original-stem}, {revision}, {image} or {sequence}.").arg(name);
        }
    }
    QString without = pattern;
    without.remove(token);
    if (without.contains(u'{') || without.contains(u'}')) {
        return tr_text("Braces are only used around the tokens {original-stem}, {revision}, {image} and {sequence}.");
    }
    if (!pattern.contains(QStringLiteral("{original-stem}")) && !pattern.contains(QStringLiteral("{sequence}"))) {
        return tr_text("The pattern needs {original-stem} or {sequence}, so that every photo gets its own name.");
    }
    return tr_text("This name pattern cannot be used.");
}

QString describe_batch_error(const Error& error) {
    const std::string_view where = error.where;
    if (error.code == ErrorCode::Cancelled) {
        return tr_text("Cancelled.");
    }
    if (starts_with(where, "app.batch.name")) {
        return error.code == ErrorCode::LimitExceeded
                   ? tr_text("No free file name is left for an output. Choose another folder or name pattern.")
                   : tr_text("This name pattern cannot be used.");
    }
    if (where == "app.batch.empty") {
        return tr_text("Nothing in the selection can be exported.");
    }
    if (starts_with(where, "app.batch.destination")) {
        switch (error.code) {
            case ErrorCode::NotFound:
                return tr_text("The output folder is not available or could not be created.");
            case ErrorCode::PermissionDenied:
                return tr_text("Lucent is not allowed to create files in the output folder.");
            case ErrorCode::DiskFull:
                return tr_text("The output folder's drive does not have enough free space for even one photo.");
            case ErrorCode::InvalidArgument:
                return tr_text("Choose an output folder.");
            default:
                break;
        }
    }
    if (error.code == ErrorCode::DiskFull) {
        return tr_text("The output folder's drive does not have enough free space for even one photo.");
    }
    if (where == "app.batch" && error.code == ErrorCode::InvalidArgument) {
        return tr_text("The artist or copyright text is too long.");
    }
    return describe_error(error);
}

BatchPlanInfo to_plan_info(const app::BatchPlan& plan) {
    BatchPlanInfo info;
    info.ok = true;
    info.folder = native(plan.folder);
    info.folder_exists = plan.folder_exists;
    for (const app::PlannedOutput& example : plan.examples) {
        info.examples.push_back(BatchPlanExample{file_name_of(example.source), native(example.destination),
                                                 example.renamed, example.replaces_existing,
                                                 example.already_exported});
    }
    info.outputs = to_qint(plan.outputs);
    info.to_hash = to_qint(plan.to_hash);
    info.already_exported = to_qint(plan.already_exported);
    info.renamed = to_qint(plan.renamed);
    info.replaced = to_qint(plan.replaced);
    info.excluded = to_qint(plan.excluded_count);
    for (const app::PlanExclusion& exclusion : plan.excluded) {
        info.exclusions.push_back(BatchPlanExclusion{
            exclusion.source.empty() ? tr_text("Photo %1").arg(exclusion.item.value) : file_name_of(exclusion.source),
            describe_exclusion(exclusion.reason)});
    }
    info.estimated_bytes = to_qint(plan.estimated_output_bytes);
    info.largest_bytes = to_qint(plan.largest_output_bytes);
    info.free_bytes = plan.free_bytes ? to_qint(*plan.free_bytes) : -1;
    // The service's warnings are developer-facing; its notices (stable codes) in the user's terms.
    // Overwriting and GPS have their own confirmations in the dialog.
    for (const app::PlanNotice notice : plan.notices) {
        switch (notice) {
            case app::PlanNotice::NetworkDestination:
                info.warnings << tr_text("The output folder is on a network drive. Every file is verified, but keep "
                                         "the connection up until the batch has finished.");
                break;
            case app::PlanNotice::RemovableDestination:
                info.warnings << tr_text("The output folder is on a removable drive. Keep it connected until the "
                                         "batch has finished.");
                break;
            case app::PlanNotice::TooFullForOne:
                info.warnings << tr_text("The output folder's drive is too full for even one photo. Free some space "
                                         "first.");
                break;
            case app::PlanNotice::MayRunOutOfSpace:
                if (plan.outputs > 0) {
                    info.warnings << tr_text("The drive may run out of space before the batch ends (the estimate is "
                                             "an upper bound). If it does, the batch pauses until you free space.");
                }
                break;
            case app::PlanNotice::ReplacesExisting:
            case app::PlanNotice::WritesGps:
                break;
            case app::PlanNotice::AutomaticSettings:
                info.automatic_notes << tr_text("Each photo is analysed and its result checked at full size before "
                                                "anything is written: a few seconds per photo, about as long again as "
                                                "its export, more for large photos.");
                break;
            case app::PlanNotice::AnalyzeOnly:
                info.automatic_notes << tr_text("Nothing is exported: each photo gets an automatic revision to review "
                                                "and adjust in Edit. The batch report is saved in the output folder.");
                break;
            case app::PlanNotice::KeepConsistent:
                info.automatic_notes << tr_text("Related photos are analysed together first, then each again towards "
                                                "its group's look: about twice the analysis time.");
                break;
        }
    }
    const bool automatic = std::find(plan.notices.begin(), plan.notices.end(), app::PlanNotice::AutomaticSettings) !=
                           plan.notices.end();
    const bool analyze_only =
        std::find(plan.notices.begin(), plan.notices.end(), app::PlanNotice::AnalyzeOnly) != plan.notices.end();
    if (automatic && !analyze_only && !plan.examples.empty()) {
        // The examples are named after the photos' current edits; the outputs after their new
        // automatic revisions.
        info.automatic_notes << tr_text("The example names show each photo's current edit number; its file is named "
                                        "after the automatic revision it gets.");
    }
    return info;
}

BatchPlanInfo to_plan_failure(const Error& error, bool folder_batch, const QString& pattern) {
    BatchPlanInfo info;
    if (folder_batch && error.where == "app.batch.empty") {
        info.folder_not_listed = true;  // the folder is imported when the batch starts
        return info;
    }
    if (starts_with(error.where, "app.batch.name") && error.code == ErrorCode::InvalidArgument) {
        info.error = describe_pattern_problem(pattern);
        if (!info.error.isEmpty()) {
            return info;
        }
    }
    info.error = describe_batch_error(error);
    return info;
}

void apply_counts(BatchSnapshot& snapshot, catalog::BatchState state, const catalog::BatchCounts& counts) {
    snapshot.state = to_ui_state(state);
    snapshot.total = to_qint(counts.total());
    snapshot.completed = to_qint(counts.complete);
    snapshot.queued = to_qint(counts.queued);
    snapshot.running = to_qint(counts.leased());
    snapshot.failed = to_qint(counts.failed);
    snapshot.skipped = to_qint(counts.skipped);
    snapshot.cancelled = to_qint(counts.cancelled);
}

BatchSnapshot to_snapshot(const app::BatchStatus& status) {
    BatchSnapshot snapshot;
    const catalog::BatchRecord& record = status.record;
    snapshot.id = static_cast<BatchId>(record.id.value);
    snapshot.name = from_utf8(record.name);
    snapshot.selection = from_utf8(record.selection_description);
    snapshot.folder = native(status.folder);
    apply_counts(snapshot, record.state, record.counts);
    snapshot.attention = to_ui_attention(status.attention);
    snapshot.attention_message = describe_attention(snapshot.attention, snapshot.folder, status.attention_free_bytes,
                                                    status.attention_needed_bytes);
    snapshot.single_export = status.single_export;
    snapshot.review = to_qint(status.review_recommended);
    snapshot.excluded = to_qint(status.excluded);
    snapshot.retryable = to_qint(status.retryable);
    if (status.output) {
        snapshot.last_output = native(*status.output);
    }
    snapshot.outcome_reason = from_utf8(status.outcome_reason);
    for (const app::RunningJob& job : status.running) {
        snapshot.running_jobs.push_back(BatchRunningJob{static_cast<qint64>(job.job.value),
                                                        job.item ? job.item->value : 0, file_name_of(job.source),
                                                        describe_worker_stage(job.stage),
                                                        std::clamp(job.fraction, 0.0, 1.0)});
    }
    if (status.eta_seconds && std::isfinite(*status.eta_seconds) && *status.eta_seconds >= 0.0) {
        snapshot.eta_seconds = *status.eta_seconds;
    }
    snapshot.created = to_qdatetime(record.created_at);
    if (record.finished_at) {
        snapshot.finished = to_qdatetime(*record.finished_at);
    }
    snapshot.automatic = to_ui_automatic(status.automatic);
    snapshot.keep_consistent = status.keep_consistent;
    if (status.automatic_counts) {
        snapshot.analysis_pending = to_qint(status.automatic_counts->pending);
        snapshot.analysis_recorded = to_qint(status.automatic_counts->documents);
    }
    return snapshot;
}

BatchReportInfo to_report_info(const app::BatchReport& report) {
    BatchReportInfo info;
    info.id = static_cast<BatchId>(report.batch.id.value);
    info.ok = true;
    info.name = from_utf8(report.batch.name);
    info.folder = native(report.folder);
    info.state = to_ui_state(report.batch.state);
    info.exported = to_qint(report.exported);
    info.skipped = to_qint(report.skipped);
    info.failed = to_qint(report.failed);
    info.cancelled = to_qint(report.cancelled);
    info.pending = to_qint(report.pending);
    info.review = to_qint(report.review_recommended);
    info.analysed = to_qint(report.analysed);
    info.automatic = to_ui_automatic(report.batch.automatic);
    info.created = to_qdatetime(report.batch.created_at);
    if (report.batch.finished_at) {
        info.finished = to_qdatetime(*report.batch.finished_at);
    }
    for (const std::string& note : report.notes) {
        info.notes << from_utf8(note);
    }
    info.entries.reserve(static_cast<qsizetype>(report.entries.size()));
    for (const app::ReportEntry& entry : report.entries) {
        BatchReportEntry out;
        out.item = entry.item ? entry.item->value : 0;
        out.source_name = file_name_of(entry.source);
        out.source_path = native(entry.source);
        out.outcome = to_ui_outcome(entry.outcome);
        if (entry.output) {
            out.output = native(*entry.output);
        }
        out.reason = from_utf8(entry.reason);
        out.action = from_utf8(entry.action);
        for (const std::string& note : entry.review) {
            out.review << from_utf8(note);
        }
        out.attempts = static_cast<int>(entry.attempts);
        if (entry.automatic) {
            const app::AutomaticReport& automatic = *entry.automatic;
            out.automatic = true;
            out.revision_number = automatic.revision_number;
            if (automatic.revision) {
                out.confidence = std::clamp(automatic.confidence, 0.0, 1.0);
            }
            out.attempt = static_cast<int>(automatic.attempt);
            for (const std::string& flag : automatic.flags) {
                if (const QString text = from_utf8(flag); !text.isEmpty() && !out.flags.contains(text)) {
                    out.flags << text;
                }
            }
            out.locks_prevented = automatic.locks_prevented_targets;
            for (const std::string& lock : automatic.locks) {
                out.locks << from_utf8(lock);
            }
            out.group = automatic.group ? from_utf8(*automatic.group) : QString();
        }
        if (info.format.isEmpty() && entry.details) {
            info.format = format_of(*entry.details);
        }
        info.entries.push_back(std::move(out));
    }
    return info;
}

QString describe_submit_progress(const app::BatchSubmitProgress& progress) {
    using Phase = app::BatchSubmitProgress::Phase;
    switch (progress.phase) {
        case Phase::Importing:
            return progress.total > 0 ? tr_text("Adding the folder's photos… %1 of %2")
                                            .arg(QString::number(progress.done), QString::number(progress.total))
                                      : tr_text("Looking for photos in the folder…");
        case Phase::Hashing:
            return tr_text("Fingerprinting the originals…");
        case Phase::Planning:
            return progress.total > 0 ? tr_text("Checking the photos… %1 of %2")
                                            .arg(QString::number(progress.done), QString::number(progress.total))
                                      : tr_text("Checking the photos…");
        case Phase::Writing:
            return tr_text("Creating the batch…");
    }
    return {};
}

double submit_fraction(const app::BatchSubmitProgress& progress) noexcept {
    if (progress.total == 0) {
        return -1.0;
    }
    return std::clamp(static_cast<double>(progress.done) / static_cast<double>(progress.total), 0.0, 1.0);
}

QDateTime to_qdatetime(catalog::Timestamp time) {
    return QDateTime::fromMSecsSinceEpoch(time.time_since_epoch().count(), QTimeZone::UTC).toLocalTime();
}

std::filesystem::path report_html_path(const std::filesystem::path& folder, catalog::BatchId batch) {
    return folder / std::format(L"Lucent-batch-{}-report.html", batch.value);
}

}  // namespace lucent::ui::adapters
