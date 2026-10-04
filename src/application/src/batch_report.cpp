// Completion reports of BatchService (batch_service.hpp, "Completion"; ADR 0012, "Report format").

#include <algorithm>
#include <chrono>
#include <format>
#include <new>
#include <span>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <lucent/application/auto_develop.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/storage.hpp>
#include <lucent/core/text.hpp>

#include "batch_service_impl.hpp"
#include "win32_paths.hpp"

namespace lucent::app {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kWhereReport = "app.batch.report";
constexpr std::string_view kReportSchema = "lucent.batch-report";
constexpr std::uint32_t kReportVersion = 1;

ItemId from_catalog(catalog::ItemId id) { return ItemId{static_cast<std::uint64_t>(id.value)}; }

std::string iso_time(catalog::Timestamp time) {
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(time));
}

Json optional_time(const std::optional<catalog::Timestamp>& time) { return time ? Json(iso_time(*time)) : Json(); }

Json parsed_or_text(const std::string& text) {
    Json parsed = Json::parse(text, nullptr, false);
    return parsed.is_discarded() ? Json(text) : parsed;
}

std::string html_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

bool starts_with(std::string_view text, std::string_view prefix) { return text.substr(0, prefix.size()) == prefix; }

// What automatic settings did to a job's photo, from its effective revision's analysis record.
AutomaticReport automatic_report(const catalog::Catalog& catalog, const catalog::JobRecord& job) {
    AutomaticReport report;
    report.state = job.auto_state;
    if (job.auto_state != catalog::AutoJobState::Frozen || !job.revision) {
        return report;
    }
    report.revision = job.revision;
    auto revision = catalog.query([&](catalog::ReadTransaction& tx) { return tx.find_revision(*job.revision); });
    if (revision && *revision) {
        report.revision_number = (*revision)->info.revision_number;
    }
    auto record = catalog.query([&](catalog::ReadTransaction& tx) { return tx.find_auto_analysis(*job.revision); });
    if (!record || !*record) {
        return report;
    }
    report.confidence = (*record)->confidence;
    report.attempt = (*record)->attempt;
    report.review = (*record)->review;
    if (job.item) {
        auto all = catalog.query([&](catalog::ReadTransaction& tx) { return tx.list_auto_analyses(*job.item); });
        if (all) {
            for (const catalog::AutoAnalysisRecord& other : *all) {
                report.attempts += other.job == job.id ? 1 : 0;
            }
        }
    }
    const Json flags = Json::parse((*record)->flags_json, nullptr, false);
    if (flags.is_array()) {
        for (const Json& entry : flags) {
            if (!entry.is_object() || !entry.contains("code") || !entry["code"].is_number_unsigned()) {
                continue;
            }
            AutoFlag flag;
            flag.code = entry["code"].get<std::uint16_t>();
            flag.reason = entry.contains("reason") && entry["reason"].is_string() ? entry["reason"].get<std::string>()
                                                                                  : std::string();
            report.flags.push_back(describe_auto_flag(flag));
        }
    }
    const Json doc = Json::parse((*record)->record_json, nullptr, false);
    if (doc.is_object()) {
        if (doc.contains("consistency") && doc["consistency"].is_object() &&
            doc["consistency"].contains("target") && doc["consistency"]["target"].contains("group_id") &&
            doc["consistency"]["target"]["group_id"].is_string()) {
            report.group = doc["consistency"]["target"]["group_id"].get<std::string>();
        }
        if (doc.contains("locks") && doc["locks"].is_object()) {
            const Json& locks = doc["locks"];
            report.locks_prevented_targets =
                locks.contains("prevented_targets") && locks["prevented_targets"].is_boolean() &&
                locks["prevented_targets"].get<bool>();
            if (locks.contains("reasons") && locks["reasons"].is_array()) {
                for (const Json& reason : locks["reasons"]) {
                    if (reason.is_string()) {
                        report.locks.push_back(reason.get<std::string>());
                    }
                }
            }
        }
    }
    return report;
}

// The review notes of an automatic result: its flags that recommend a review.
std::vector<std::string> automatic_review(const catalog::Catalog& catalog, const catalog::JobRecord& job) {
    std::vector<std::string> notes;
    if (!job.revision) {
        return notes;
    }
    auto record = catalog.query([&](catalog::ReadTransaction& tx) { return tx.find_auto_analysis(*job.revision); });
    if (!record || !*record) {
        return notes;
    }
    const Json flags = Json::parse((*record)->flags_json, nullptr, false);
    if (!flags.is_array()) {
        return notes;
    }
    for (const Json& entry : flags) {
        if (!entry.is_object() || !entry.contains("code") || !entry["code"].is_number_unsigned() ||
            !entry.contains("review") || !entry["review"].is_boolean() || !entry["review"].get<bool>()) {
            continue;
        }
        AutoFlag flag;
        flag.code = entry["code"].get<std::uint16_t>();
        flag.reason = entry.contains("reason") && entry["reason"].is_string() ? entry["reason"].get<std::string>()
                                                                              : std::string();
        notes.push_back(describe_auto_flag(flag));
    }
    if (notes.empty() && (*record)->review) {
        notes.push_back("The first automatic edit failed a quality check, so a more conservative one was used.");
    }
    return notes;
}

Json automatic_json(const AutomaticReport& a) {
    Json out = Json::object();
    out["state"] = std::string(catalog::to_string(a.state));
    out["revision"] = a.revision ? Json(a.revision->value) : Json();
    out["revision_number"] = a.revision_number;
    out["confidence"] = a.confidence;
    out["attempt"] = a.attempt;
    out["attempts"] = a.attempts;
    out["review"] = a.review;
    Json flags = Json::array();
    for (const std::string& flag : a.flags) {
        flags.push_back(core::sanitize_utf8(flag));
    }
    out["flags"] = std::move(flags);
    out["group"] = a.group ? Json(core::sanitize_utf8(*a.group)) : Json();
    out["locks_prevented_targets"] = a.locks_prevented_targets;
    Json locks = Json::array();
    for (const std::string& lock : a.locks) {
        locks.push_back(core::sanitize_utf8(lock));
    }
    out["locks"] = std::move(locks);
    return out;
}

}  // namespace

// ---- Wording ---------------------------------------------------------------------------------

detail::Wording detail::describe_failure(const Error& error) {
    const std::string_view where = error.where;
    switch (error.code) {
        case ErrorCode::AlreadyExists:
            if (where == kWhereExisting) {
                return {"Already exported: an identical output is in the folder.", ""};
            }
            return {"The output name was taken while the photo was being exported.", "Choose Retry failed."};
        case ErrorCode::SourceChanged:
            return {"The original file changed after the batch was created.",
                    "Add the photo to the library again, then export it in a new batch."};
        case ErrorCode::NotFound:
            if (where == kWhereSourceMissing) {
                return {"The original file could not be found.",
                        "Reconnect its drive or relink the photo, then choose Retry failed."};
            }
            if (where == kWhereRemoved || starts_with(where, "app.batch.selection")) {
                return {"The photo is no longer in the library.", ""};
            }
            if (where == kWhereOutputLost) {
                return {"The exported file is missing or was changed after it was published.",
                        "Export the photo again (in a new export: Retry failed cannot export it twice in one batch)."};
            }
            if (where == kWhereDestinationReplaced) {
                return {"Another folder took the output folder's place (it was created again, or another drive got "
                        "its letter).",
                        "Choose Use this folder to continue there, or reconnect the original drive and resume."};
            }
            if (where == kWhereDestination) {
                return {"The output folder is not available.", "Reconnect the drive, then resume the batch."};
            }
            if (where == kWhereRecipe) {
                return {"The photo's recipe for this batch is no longer in the library.", ""};
            }
            return {"A file Lucent needed was not found.", "Choose Retry failed."};
        case ErrorCode::Unsupported:
            if (where == kWhereAutoSupport) {
                return {"Lucent cannot interpret this camera file faithfully yet, so it was not analysed or exported.",
                        "See the photo's compatibility details in the library."};
            }
            return {"Lucent cannot render this camera file faithfully yet, so it was not exported.",
                    "See the photo's compatibility details in the library."};
        case ErrorCode::Corrupt:
            if (where == kWhereVerify) {
                return {"The exported file failed verification and was discarded.",
                        "Choose Retry failed; check the destination drive if it happens again."};
            }
            return {"The file could not be decoded; it may be damaged.", "Check the original file."};
        case ErrorCode::LimitExceeded:
        case ErrorCode::OutOfMemory:
            if (starts_with(where, "app.batch.name") || where == kWhereDestination) {
                return {"The output name or path would be too long or no free name is left.",
                        "Choose a shorter folder or name pattern."};
            }
            return {"The photo needs more memory than Lucent's export budget allows.",
                    "Close other programs, then choose Retry failed."};
        case ErrorCode::DiskFull:
            return {"The destination ran out of space.",
                    "Free space on the drive, then resume the batch or choose Retry failed."};
        case ErrorCode::PermissionDenied:
            if (where == kWhereSourceAccess) {
                return {"Lucent was not allowed to read the original file.",
                        "Check the file's permissions, then choose Retry failed."};
            }
            return {"Lucent was not allowed to write to the output folder.",
                    "Choose another folder or change its permissions, then retry."};
        case ErrorCode::VersionMismatch:
            return {"The photo's edit cannot be read by this version of Lucent.",
                    "Open the photo in the version of Lucent that made the edit."};
        case ErrorCode::Timeout:
            if (where == kWhereInterrupted) {
                return {"Lucent stopped during the export; the photo was queued again.", ""};
            }
            if (where == kWhereLeaseExpired) {
                return {"The export was interrupted (for example while the computer slept); the photo was queued "
                        "again.",
                        ""};
            }
            if (where == kWhereInterruptedTooOften) {
                return {"Lucent was interrupted repeatedly while exporting this photo.",
                        "Choose Retry failed; if it happens again, export the photo on its own."};
            }
            return {"The export took too long or its process stopped responding.", "Choose Retry failed."};
        case ErrorCode::Internal:
            if (where == kWhereAutoCheck) {
                return {"No automatic edit of this photo passed the final quality check, so it was not exported.",
                        "Edit the photo by hand and export it on its own."};
            }
            return {"The export process stopped unexpectedly.",
                    "Choose Retry failed; report the problem if it happens again."};
        case ErrorCode::IoError:
            return {"A read or write error occurred.", "Check the drives, then choose Retry failed."};
        case ErrorCode::Cancelled:
            return {"Cancelled before it was exported.", ""};
        case ErrorCode::InvalidArgument:
            return {"The batch's settings could not be used for this photo.", "Check the export settings."};
        case ErrorCode::Unknown:
            break;
    }
    return {"The export failed.", "Choose Retry failed."};
}

// ---- Building --------------------------------------------------------------------------------

Expected<BatchReport> BatchService::Impl::build_report(catalog::BatchId id) const {
    auto record = batch_record(id);
    if (!record) {
        return Unexpected(std::move(record.error()));
    }
    auto context = context_for(id);
    if (!context) {
        return Unexpected(std::move(context.error()));
    }
    const detail::BatchContext& ctx = **context;
    BatchReport report;
    report.batch = *record;
    report.folder = ctx.destination.folder;
    report.generated_at = now();
    if (ctx.destination.storage == core::StorageKind::Network) {
        report.notes.push_back(
            "The output folder is on a network location: the files were verified when they were written, but a "
            "network share may not keep them durably.");
    } else if (ctx.destination.storage == core::StorageKind::Removable) {
        report.notes.push_back("The output folder is on a removable drive.");
    }
    if (ctx.destination.collisions == CollisionPolicy::Overwrite) {
        report.notes.push_back("Existing files with an output's name were replaced (never an original photo).");
    }
    if (ctx.output.settings.metadata.include_gps) {
        report.notes.push_back("GPS positions were written into the exported files.");
    }
    if (ctx.output.allow_unsupported) {
        report.notes.push_back("Photos Lucent does not fully support were exported on request.");
    }
    if (ctx.automatic == catalog::AutoMode::Auto) {
        report.notes.push_back(
            "Automatic settings: each photo was analysed and exported with its automatic revision (Natural).");
    } else if (ctx.automatic == catalog::AutoMode::AnalyzeOnly) {
        report.notes.push_back(
            "Analyse only: each photo received automatic revisions to review in the editor; nothing was exported.");
    }
    if (ctx.policy.automatic.keep_consistent) {
        report.notes.push_back("Related photos were kept visually consistent with each other.");
    }

    catalog::JobQuery query;
    query.batch = id;
    query.limit = 1000;
    for (;;) {
        auto jobs = catalog->query([&](catalog::ReadTransaction& tx) { return tx.list_jobs(query); });
        if (!jobs) {
            return Unexpected(std::move(jobs.error()));
        }
        for (const catalog::JobRecord& job : *jobs) {
            ReportEntry entry;
            entry.job = job.id;
            entry.item = job.item ? std::optional<ItemId>(from_catalog(*job.item)) : std::nullopt;
            entry.ordinal = job.ordinal;
            entry.attempts = job.attempts;
            if (entry.item) {
                if (std::optional<SourceItem> item = library.find(*entry.item)) {
                    entry.source = item->path;
                }
            }
            if (job.last_error) {
                entry.error = Error{job.last_error->code, job.last_error->message, job.last_error->where,
                                    job.last_error->native_code};
            }
            if (ctx.automatic != catalog::AutoMode::None) {
                entry.automatic = automatic_report(*catalog, job);
            }
            switch (job.stage) {
                case catalog::JobStage::Complete: {
                    if (ctx.automatic == catalog::AutoMode::AnalyzeOnly) {
                        entry.outcome = ReportOutcome::Analysed;
                        entry.error.reset();
                        entry.reason = entry.automatic && entry.automatic->revision_number > 0
                                           ? std::format("Analysed: automatic settings saved as revision {}.",
                                                         entry.automatic->revision_number)
                                           : std::string("Analysed.");
                        entry.review = automatic_review(*catalog, job);
                        break;
                    }
                    entry.outcome = ReportOutcome::Exported;
                    entry.error.reset();
                    entry.reason = "Exported.";
                    auto receipt = catalog->query([&](catalog::ReadTransaction& tx) { return tx.find_receipt(job.id); });
                    if (receipt && *receipt) {
                        entry.output = (*receipt)->final_path;
                        entry.size_bytes = (*receipt)->size_bytes;
                        entry.sha256 = (*receipt)->output_sha256;
                        if (auto details = read_receipt_details((*receipt)->settings_json)) {
                            entry.review = details->review;
                            entry.details = std::move(*details);
                        }
                    }
                    break;
                }
                case catalog::JobStage::Skipped:
                    entry.outcome = ReportOutcome::Skipped;
                    if (entry.error && entry.error->where == detail::kWhereExisting) {
                        // The existing output submission verified in this batch's folder (the job
                        // names it), with the receipt that describes it.
                        auto receipts = catalog->query([&](catalog::ReadTransaction& tx) {
                            return tx.find_receipts_by_identity(job.identity_sha256);
                        });
                        const catalog::ReceiptRecord* match = nullptr;
                        if (receipts) {
                            for (const catalog::ReceiptRecord& receipt : *receipts) {
                                if (job.final_path ? detail::same_path_text(receipt.final_path, *job.final_path)
                                                   : detail::is_same_or_within(receipt.final_path, ctx.destination.folder)) {
                                    match = &receipt;
                                    if (job.final_path) {
                                        break;
                                    }
                                }
                            }
                        }
                        if (match != nullptr) {
                            entry.output = match->final_path;
                            entry.size_bytes = match->size_bytes;
                            entry.sha256 = match->output_sha256;
                        } else if (job.final_path) {
                            entry.output = job.final_path;
                        }
                    }
                    break;
                case catalog::JobStage::Failed:
                    // An unsupported file found while decoding is skipped with its reason (blueprint
                    // section 2), not counted as a failure of the batch.
                    entry.outcome = entry.error && entry.error->code == ErrorCode::Unsupported ? ReportOutcome::Skipped
                                                                                                 : ReportOutcome::Failed;
                    break;
                case catalog::JobStage::Cancelled:
                    entry.outcome = ReportOutcome::Cancelled;
                    break;
                default:
                    entry.outcome = ReportOutcome::Pending;
                    break;
            }
            if (entry.outcome != ReportOutcome::Exported && entry.outcome != ReportOutcome::Analysed) {
                const detail::Wording wording =
                    entry.error ? detail::describe_failure(*entry.error)
                                : detail::Wording{entry.outcome == ReportOutcome::Pending ? "Not exported yet." : "", ""};
                entry.reason = wording.reason;
                entry.action = wording.action;
            }
            report.entries.push_back(std::move(entry));
            query.after = job.id;
        }
        if (jobs->size() < query.limit) {
            break;
        }
    }
    for (const PlanExclusion& exclusion : ctx.policy.excluded) {
        ReportEntry entry;
        entry.item = exclusion.item;
        entry.source = exclusion.source;
        entry.outcome = ReportOutcome::Skipped;
        entry.error = exclusion.reason;
        const detail::Wording wording = detail::describe_failure(exclusion.reason);
        entry.reason = wording.reason;
        entry.action = wording.action;
        report.entries.push_back(std::move(entry));
    }
    if (ctx.policy.excluded_count > ctx.policy.excluded.size()) {
        report.notes.push_back(std::format("{} more photos were excluded when the batch was created (not listed).",
                                           ctx.policy.excluded_count - ctx.policy.excluded.size()));
        report.skipped += ctx.policy.excluded_count - ctx.policy.excluded.size();
    }
    for (const ReportEntry& entry : report.entries) {
        switch (entry.outcome) {
            case ReportOutcome::Exported:
                ++report.exported;
                report.review_recommended += entry.review.empty() ? 0 : 1;
                break;
            case ReportOutcome::Skipped:
                ++report.skipped;
                break;
            case ReportOutcome::Failed:
                ++report.failed;
                break;
            case ReportOutcome::Cancelled:
                ++report.cancelled;
                break;
            case ReportOutcome::Pending:
                ++report.pending;
                break;
            case ReportOutcome::Analysed:
                ++report.analysed;
                report.review_recommended += entry.review.empty() ? 0 : 1;
                break;
        }
    }
    return report;
}

// ---- Rendering -------------------------------------------------------------------------------

Expected<std::string> report_json(const BatchReport& report) {
    try {
        Json out = Json::object();
        out["schema"] = kReportSchema;
        out["version"] = kReportVersion;
        Json batch = Json::object();
        batch["id"] = report.batch.id.value;
        batch["name"] = core::sanitize_utf8(report.batch.name);
        batch["selection"] = core::sanitize_utf8(report.batch.selection_description);
        batch["state"] = std::string(catalog::to_string(report.batch.state));
        batch["created_at"] = iso_time(report.batch.created_at);
        batch["started_at"] = optional_time(report.batch.started_at);
        batch["finished_at"] = optional_time(report.batch.finished_at);
        batch["renderer_version"] = core::sanitize_utf8(report.batch.renderer_version);
        batch["export_settings"] = parsed_or_text(report.batch.export_settings_json);
        batch["destination"] = parsed_or_text(report.batch.destination_json);
        out["batch"] = std::move(batch);
        out["folder"] = core::path_to_utf8_lossy(report.folder);
        Json summary = Json::object();
        summary["exported"] = report.exported;
        summary["skipped"] = report.skipped;
        summary["failed"] = report.failed;
        summary["cancelled"] = report.cancelled;
        summary["pending"] = report.pending;
        summary["analysed"] = report.analysed;
        summary["review_recommended"] = report.review_recommended;
        out["summary"] = std::move(summary);
        Json notes = Json::array();
        for (const std::string& note : report.notes) {
            notes.push_back(core::sanitize_utf8(note));
        }
        out["notes"] = std::move(notes);
        Json entries = Json::array();
        for (const ReportEntry& entry : report.entries) {
            Json e = Json::object();
            e["job"] = entry.job ? Json(entry.job->value) : Json();
            e["item"] = entry.item ? Json(entry.item->value) : Json();
            e["ordinal"] = entry.ordinal;
            e["source"] = core::path_to_utf8_lossy(entry.source);
            e["outcome"] = std::string(to_string(entry.outcome));
            e["output"] = entry.output ? Json(core::path_to_utf8_lossy(*entry.output)) : Json();
            e["size_bytes"] = entry.size_bytes;
            e["sha256"] = entry.sha256 ? Json(core::to_hex(*entry.sha256)) : Json();
            e["reason"] = entry.reason;
            e["action"] = entry.action;
            Json review = Json::array();
            for (const std::string& note : entry.review) {
                review.push_back(core::sanitize_utf8(note));
            }
            e["review"] = std::move(review);
            e["attempts"] = entry.attempts;
            e["automatic"] = entry.automatic ? automatic_json(*entry.automatic) : Json();
            if (entry.error) {
                Json error = Json::object();
                error["code"] = std::string(to_string(entry.error->code));
                error["where"] = core::sanitize_utf8(entry.error->where);
                error["message"] = core::sanitize_utf8(entry.error->message);
                e["error"] = std::move(error);
            } else {
                e["error"] = Json();
            }
            entries.push_back(std::move(e));
        }
        out["entries"] = std::move(entries);
        out["generated_at"] = iso_time(report.generated_at);
        return out.dump(2);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot build the report", std::string(kWhereReport));
    } catch (const Json::exception& e) {
        return fail(ErrorCode::Internal, std::format("cannot build the report: {}", e.what()), std::string(kWhereReport));
    }
}

std::string report_html(const BatchReport& report) {
    std::string html;
    html.reserve(4096 + report.entries.size() * 400);
    html += "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n<title>";
    html += html_escape(report.batch.name);
    html += " - Lucent export report</title>\n<style>\n"
            "body{font-family:'Segoe UI',system-ui,sans-serif;margin:24px;color:#1b1b1b;background:#fff}\n"
            "h1{font-size:20px;margin:0 0 4px}p.meta{color:#555;margin:0 0 16px}\n"
            "ul.summary{display:flex;gap:16px;list-style:none;padding:0;flex-wrap:wrap}\n"
            "ul.summary li{border:1px solid #ddd;border-radius:6px;padding:8px 12px}\n"
            "table{border-collapse:collapse;width:100%;font-size:13px}\n"
            "th,td{border-bottom:1px solid #e5e5e5;text-align:left;padding:6px 8px;vertical-align:top}\n"
            "th{background:#f5f5f5}td.exported{color:#1a7f37}td.failed{color:#b42318}\n"
            "td.skipped,td.cancelled,td.pending{color:#6b5b00}code{font-size:12px;word-break:break-all}\n"
            "@media (prefers-color-scheme: dark){body{background:#1e1e1e;color:#e8e8e8}th{background:#2a2a2a}"
            "th,td{border-color:#333}ul.summary li{border-color:#444}p.meta{color:#aaa}}\n"
            "</style></head><body>\n";
    html += "<h1>" + html_escape(report.batch.name) + "</h1>\n";
    html += "<p class=\"meta\">Output folder: <code>" + html_escape(core::path_to_utf8_lossy(report.folder)) +
            "</code><br>State: " + html_escape(catalog::to_string(report.batch.state)) +
            " &middot; report written " + html_escape(iso_time(report.generated_at)) + " (UTC)</p>\n";
    html += std::format(
        "<ul class=\"summary\"><li><strong>{}</strong> exported</li><li><strong>{}</strong> review recommended</li>"
        "<li><strong>{}</strong> skipped</li><li><strong>{}</strong> failed</li><li><strong>{}</strong> cancelled</li>"
        "<li><strong>{}</strong> not finished</li>",
        report.exported, report.review_recommended, report.skipped, report.failed, report.cancelled, report.pending);
    if (report.analysed > 0) {
        html += std::format("<li><strong>{}</strong> analysed</li>", report.analysed);
    }
    html += "</ul>\n";
    if (!report.notes.empty()) {
        html += "<ul>\n";
        for (const std::string& note : report.notes) {
            html += "<li>" + html_escape(note) + "</li>\n";
        }
        html += "</ul>\n";
    }
    html += "<table><thead><tr><th>#</th><th>Photo</th><th>Result</th><th>Output</th><th>Details</th></tr></thead>"
            "<tbody>\n";
    for (const ReportEntry& entry : report.entries) {
        const std::string outcome(to_string(entry.outcome));
        html += "<tr><td>" + (entry.job ? std::to_string(entry.ordinal + 1) : std::string("&ndash;")) + "</td>";
        html += "<td><code>" + html_escape(core::path_to_utf8_lossy(entry.source.filename())) + "</code></td>";
        html += "<td class=\"" + outcome + "\">" + html_escape(outcome) + "</td>";
        html += "<td>" + (entry.output ? "<code>" + html_escape(core::path_to_utf8_lossy(entry.output->filename())) + "</code>"
                                       : std::string()) +
                "</td><td>";
        if (!entry.reason.empty() && entry.outcome != ReportOutcome::Exported) {
            html += html_escape(entry.reason);
        }
        if (!entry.action.empty()) {
            html += "<br><em>" + html_escape(entry.action) + "</em>";
        }
        if (entry.automatic && entry.automatic->revision) {
            html += std::format("<br>Automatic settings: revision {}, confidence {:.0f} %{}{}",
                                entry.automatic->revision_number, 100.0 * entry.automatic->confidence,
                                entry.automatic->attempt > 0 ? ", conservative fallback" : "",
                                entry.automatic->group ? ", kept consistent with related photos" : "");
            for (const std::string& lock : entry.automatic->locks) {
                html += "<br><em>" + html_escape(lock) + "</em>";
            }
        }
        if (!entry.review.empty()) {
            html += "<strong>Review recommended:</strong><ul>";
            for (const std::string& note : entry.review) {
                html += "<li>" + html_escape(note) + "</li>";
            }
            html += "</ul>";
        }
        html += "</td></tr>\n";
    }
    html += "</tbody></table>\n</body></html>\n";
    return html;
}

Expected<ReportFiles> BatchService::Impl::write_report_files(catalog::BatchId id) {
    auto report = build_report(id);
    if (!report) {
        return Unexpected(std::move(report.error()));
    }
    auto json = report_json(*report);
    if (!json) {
        return Unexpected(std::move(json.error()));
    }
    const std::string html = report_html(*report);
    ReportFiles files;
    const std::wstring stem = std::format(L"Lucent-batch-{}-report", id.value);
    files.json = report->folder / (stem + L".json");
    files.html = report->folder / (stem + L".html");
    core::AtomicWriteOptions write;
    write.existing = core::ExistingFile::Replace;
    if (Status written = core::write_file_atomic(files.json, std::as_bytes(std::span(json->data(), json->size())), write);
        !written) {
        return Unexpected(std::move(written.error()));
    }
    if (Status written = core::write_file_atomic(files.html, std::as_bytes(std::span(html.data(), html.size())), write);
        !written) {
        return Unexpected(std::move(written.error()));
    }
    return files;
}

}  // namespace lucent::app
