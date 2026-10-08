#include <algorithm>
#include <format>
#include <iterator>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <lucent/application/camera_support.hpp>
#include <lucent/assets/mode_key.hpp>

namespace lucent::app {

namespace {

bool interpretation_reason(SupportReasonKind kind) noexcept {
    return kind == SupportReasonKind::InterpretationIssue ||
           kind == SupportReasonKind::InterpretationStatus || kind == SupportReasonKind::ProbeFailed;
}

// The user-facing words of a limitation: the database's text, or the code when it has none.
std::string limitation_message(const assets::CameraSupportDatabase& database, const std::string& code) {
    if (const assets::LimitationText* text = database.limitation(code)) {
        return text->summary + ": " + text->meaning;
    }
    return code;
}

// Rules step 2 and 3 (see the header): `live` holds the interpretation's status, reasons and
// status; `evaluation` the probe's mode key and interpretation status.
SupportAssessment combine(SupportAssessment live, CameraSupportEvaluation evaluation,
                          const assets::CameraSupport* database) {
    evaluation.rules = kSupportRulesVersion;
    evaluation.database_id.clear();
    evaluation.database_version.clear();
    evaluation.qualified_on.clear();
    evaluation.basis_matches = true;
    evaluation.listed.reset();
    evaluation.qualified_key.clear();
    evaluation.camera.clear();
    evaluation.mode.clear();
    evaluation.evidence.clear();

    SupportStatus listed_state = SupportStatus::CompatibleUnverified;  // not listed
    std::vector<SupportReason> database_reasons;
    if (database != nullptr) {
        evaluation.database_id = database->asset_id;
        evaluation.database_version = database->version_text();
        evaluation.qualified_on = database->database.date;
        evaluation.basis_matches = database->verified_entries_hold();
        if (const auto match = database->database.lookup(evaluation.mode_key)) {
            evaluation.listed = match->state;
            if (match->entry != nullptr) {
                evaluation.qualified_key = match->entry->qualified_key;
                evaluation.camera = match->entry->camera;
                evaluation.mode = match->entry->mode;
                evaluation.evidence = match->entry->evidence;
            }
            listed_state = match->state;
            if (listed_state == SupportStatus::Verified && !evaluation.basis_matches) {
                listed_state = SupportStatus::CompatibleUnverified;
            }
            if (match->state == SupportStatus::Limited || match->state == SupportStatus::Unsupported) {
                for (const std::string& code : match->limitations) {
                    SupportReason reason;
                    reason.kind = SupportReasonKind::CameraLimitation;
                    reason.limitation = code;
                    reason.message = limitation_message(database->database, code);
                    database_reasons.push_back(std::move(reason));
                }
            }
            if (match->state == SupportStatus::Unsupported) {
                SupportReason reason;
                reason.kind = SupportReasonKind::CameraUnsupported;
                reason.message =
                    std::format("Lucent's qualification found that {}{}{} does not develop correctly ({}).",
                                evaluation.camera.empty() ? std::string("this camera") : evaluation.camera,
                                evaluation.mode.empty() ? "" : ", ", evaluation.mode, evaluation.evidence);
                database_reasons.push_back(std::move(reason));
            }
        }
    }

    SupportAssessment out = std::move(live);
    // A complete interpretation puts no cap on the state; the database decides how good it is.
    const SupportStatus live_cap =
        out.status == SupportStatus::CompatibleUnverified ? SupportStatus::Verified : out.status;
    out.status = assets::worse(live_cap, listed_state);
    out.reasons.insert(out.reasons.end(), std::make_move_iterator(database_reasons.begin()),
                       std::make_move_iterator(database_reasons.end()));
    out.camera = std::move(evaluation);
    return out;
}

}  // namespace

SupportAssessment assess_support(const raw::RawMetadata& metadata, const assets::CameraSupport* database) {
    SupportAssessment live = assess_interpretation(metadata.interpretation);
    CameraSupportEvaluation evaluation;
    evaluation.mode_key = assets::derive_mode_key(metadata).to_string();
    evaluation.interpretation = metadata.interpretation.status;
    return combine(std::move(live), std::move(evaluation), database);
}

std::optional<SupportAssessment> reassess_support(const SupportAssessment& stored,
                                                  const assets::CameraSupport* database) {
    if (!stored.camera) {
        return std::nullopt;
    }
    // The live part as the probe gave it: its status, and its interpretation reasons.
    SupportAssessment live;
    live.interpretation = stored.camera->interpretation;
    live.status = support_status_for(stored.camera->interpretation);
    std::vector<raw::InterpretationIssue> issues;
    for (const SupportReason& reason : stored.reasons) {
        if (!interpretation_reason(reason.kind)) {
            continue;
        }
        if (reason.kind == SupportReasonKind::InterpretationIssue && reason.issue) {
            issues.push_back(raw::InterpretationIssue{*reason.issue, reason.message});
        }
        live.reasons.push_back(reason);
    }
    live.status = assets::worse(live.status, support_status_for(raw::derive_interpretation_status(issues)));
    if (live.status != SupportStatus::CompatibleUnverified && live.reasons.empty()) {
        live.reasons = assess_interpretation_status(stored.camera->interpretation).reasons;
    }
    CameraSupportEvaluation evaluation;
    evaluation.mode_key = stored.camera->mode_key;
    evaluation.interpretation = stored.camera->interpretation;
    return combine(std::move(live), std::move(evaluation), database);
}

bool assessed_with(const SupportAssessment& support, const assets::CameraSupport* database) noexcept {
    if (!support.camera || support.camera->rules != kSupportRulesVersion) {
        return false;
    }
    const CameraSupportEvaluation& camera = *support.camera;
    if (database == nullptr) {
        return camera.database_id.empty();
    }
    return camera.database_id == database->asset_id && camera.database_version == database->version_text() &&
           camera.basis_matches == database->verified_entries_hold();
}

Expected<SupportRefresh> refresh_support(Library& library, const assets::CameraSupport* database) {
    SupportRefresh refresh;
    std::vector<SupportUpdate> updates;
    try {
        for (const SourceItem& item : library.items()) {
            ++refresh.items;
            if (!item.support.camera) {
                if (item.probe) {
                    ++refresh.needs_probe;
                }
                continue;
            }
            if (assessed_with(item.support, database)) {
                continue;
            }
            std::optional<SupportAssessment> fresh = reassess_support(item.support, database);
            if (!fresh) {
                continue;
            }
            ++refresh.reassessed;
            if (fresh->status != item.support.status) {
                ++refresh.changed;
            }
            updates.push_back(SupportUpdate{item.id, std::move(*fresh)});
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "re-assessing the library's support ran out of memory",
                    "app.library.support");
    }
    if (updates.empty()) {
        return refresh;
    }
    if (Status stored = library.set_support(updates); !stored) {
        return Unexpected(std::move(stored.error()));
    }
    return refresh;
}

}  // namespace lucent::app
