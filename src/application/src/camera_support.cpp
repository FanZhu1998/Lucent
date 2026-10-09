#include <algorithm>
#include <format>
#include <iterator>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <lucent/application/camera_support.hpp>
#include <lucent/assets/mode_key.hpp>
#include <lucent/core/hash.hpp>

namespace lucent::app {

namespace {

// The user-facing words of a limitation: the database's text, this build's own text, or the code.
std::string limitation_message(const assets::CameraSupportDatabase* database, const std::string& code) {
    if (database != nullptr) {
        if (const assets::LimitationText* text = database->limitation(code)) {
            return text->summary + ": " + text->meaning;
        }
    }
    if (const std::optional<assets::LimitationText> text = assets::standard_limitation_text(code)) {
        return text->summary + ": " + text->meaning;
    }
    return code;
}

SupportReason limitation_reason(SupportReasonKind kind, const std::string& code,
                                const assets::CameraSupportDatabase* database) {
    SupportReason reason;
    reason.kind = kind;
    reason.limitation = code;
    reason.message = limitation_message(database, code);
    return reason;
}

// Rules step 2: the engine's own limitations for this file, known from its probe (AST-03).
std::vector<std::string> engine_limitation_codes(const raw::RawMetadata& metadata, const SupportFileFacts& file) {
    std::vector<std::string> codes;
    if (metadata.representation.kind == raw::RawRepresentation::Mosaic && metadata.representation.cfa.is_xtrans()) {
        codes.emplace_back(assets::kLimitationXTransBaseline);
    }
    if (file.dual_pixel_component || metadata.subimage_role == raw::SubimageRole::DualPixelComponent) {
        codes.emplace_back(assets::kLimitationDualPixel);
    }
    return codes;
}

// The database's part of an assessment (rules step 3).
struct DatabaseAnswer {
    SupportStatus state = SupportStatus::CompatibleUnverified;  // not listed
    std::vector<SupportReason> reasons;                        // CameraLimitation, CameraUnsupported
};

DatabaseAnswer answer_of(const assets::CameraSupportMatch& match, const CameraSupportEvaluation& evaluation,
                         const assets::CameraSupportDatabase& database) {
    DatabaseAnswer answer;
    answer.state = match.state;
    if (answer.state == SupportStatus::Verified && (!evaluation.basis_matches || !evaluation.variant_matches)) {
        answer.state = SupportStatus::CompatibleUnverified;
    }
    if (match.state == SupportStatus::Limited || match.state == SupportStatus::Unsupported) {
        for (const std::string& code : match.limitations) {
            answer.reasons.push_back(limitation_reason(SupportReasonKind::CameraLimitation, code, &database));
        }
    }
    if (match.state == SupportStatus::Unsupported) {
        SupportReason reason;
        reason.kind = SupportReasonKind::CameraUnsupported;
        reason.message = std::format(
            "Lucent's qualification found that {}{}{} does not develop correctly ({}).",
            evaluation.camera.empty() ? std::string("this camera") : evaluation.camera,
            evaluation.mode.empty() ? "" : ", ", evaluation.mode, evaluation.evidence);
        answer.reasons.push_back(std::move(reason));
    }
    return answer;
}

// Rules step 4: the worst of the live part (`live`: the interpretation and the engine limitations)
// and the database's answer.
SupportAssessment apply(SupportAssessment live, CameraSupportEvaluation evaluation, DatabaseAnswer answer) {
    SupportAssessment out = std::move(live);
    std::set<std::string> engine_codes;
    for (const SupportReason& reason : out.reasons) {
        if (reason.kind == SupportReasonKind::EngineLimitation) {
            engine_codes.insert(reason.limitation);
        }
    }
    for (SupportReason& reason : answer.reasons) {
        if (reason.kind == SupportReasonKind::CameraLimitation && engine_codes.contains(reason.limitation)) {
            continue;  // the engine limitation already names it (once in reports and review notes)
        }
        out.reasons.push_back(std::move(reason));
    }
    // A complete interpretation without engine limitations puts no cap on the state; the database
    // decides how good it is.
    const SupportStatus live_cap =
        out.status == SupportStatus::CompatibleUnverified ? SupportStatus::Verified : out.status;
    out.status = assets::worse(live_cap, answer.state);
    out.camera = std::move(evaluation);
    return out;
}

void clear_database(CameraSupportEvaluation& evaluation) {
    evaluation.rules = kSupportRulesVersion;
    evaluation.database_id.clear();
    evaluation.database_version.clear();
    evaluation.database_sha256.clear();
    evaluation.qualified_on.clear();
    evaluation.basis_matches = true;
    evaluation.variant_matches = true;
    evaluation.listed.reset();
    evaluation.qualified_key.clear();
    evaluation.camera.clear();
    evaluation.mode.clear();
    evaluation.evidence.clear();
}

// Rules step 3 with `database` (null: not listed).
SupportAssessment combine(SupportAssessment live, CameraSupportEvaluation evaluation,
                          const assets::CameraSupport* database) {
    clear_database(evaluation);
    DatabaseAnswer answer;
    if (database != nullptr) {
        evaluation.database_id = database->asset_id;
        evaluation.database_version = database->version_text();
        evaluation.database_sha256 = core::to_hex(database->manifest_sha256);
        evaluation.qualified_on = database->database.date;
        evaluation.basis_matches = database->verified_entries_hold();
        if (const auto match = database->database.lookup(evaluation.mode_key)) {
            evaluation.listed = match->state;
            if (match->entry != nullptr) {
                const assets::CameraSupportEntry& entry = *match->entry;
                evaluation.qualified_key = entry.qualified_key;
                evaluation.camera = entry.camera;
                evaluation.mode = entry.mode;
                evaluation.evidence = entry.evidence;
                // QUA-08: the qualified file's image area and DNG opcodes ("*": any area, curated
                // entries, which are never Verified).
                evaluation.variant_matches = entry.image_area == "*" || (entry.image_area == evaluation.image_area &&
                                                                         entry.dng_opcodes == evaluation.dng_opcodes);
            }
            answer = answer_of(*match, evaluation, database->database);
        }
    }
    return apply(std::move(live), std::move(evaluation), std::move(answer));
}

// AST-01: no database is active. The answer of the database `stored` was assessed with is kept, so a
// missing or refused database never improves an item; Verified, which nothing confirms now, becomes
// Compatible, unverified.
SupportAssessment keep(SupportAssessment live, CameraSupportEvaluation evaluation,
                       const CameraSupportEvaluation& stored, const std::vector<SupportReason>& stored_reasons) {
    clear_database(evaluation);
    evaluation.basis_matches = stored.basis_matches;
    evaluation.variant_matches = stored.variant_matches;
    evaluation.listed = stored.listed;
    evaluation.qualified_key = stored.qualified_key;
    evaluation.camera = stored.camera;
    evaluation.mode = stored.mode;
    evaluation.evidence = stored.evidence;
    DatabaseAnswer answer;
    if (stored.listed) {
        answer.state =
            *stored.listed == SupportStatus::Verified ? SupportStatus::CompatibleUnverified : *stored.listed;
        for (const SupportReason& reason : stored_reasons) {
            if (reason.kind == SupportReasonKind::CameraLimitation ||
                reason.kind == SupportReasonKind::CameraUnsupported) {
                answer.reasons.push_back(reason);
            }
        }
    }
    return apply(std::move(live), std::move(evaluation), std::move(answer));
}

}  // namespace

SupportAssessment assess_support(const raw::RawMetadata& metadata, const assets::CameraSupport* database,
                                 const SupportFileFacts& file) {
    SupportAssessment live = assess_interpretation(metadata.interpretation);
    const assets::CameraSupportDatabase* words = database != nullptr ? &database->database : nullptr;
    for (const std::string& code : engine_limitation_codes(metadata, file)) {
        live.reasons.push_back(limitation_reason(SupportReasonKind::EngineLimitation, code, words));
        live.status = assets::worse(live.status, SupportStatus::Limited);
    }
    CameraSupportEvaluation evaluation;
    evaluation.mode_key = assets::derive_mode_key(metadata).to_string();
    assets::ModeVariant variant = assets::derive_mode_variant(metadata);
    evaluation.image_area = std::move(variant.image_area);
    evaluation.dng_opcodes = std::move(variant.dng_opcodes);
    evaluation.interpretation = metadata.interpretation.status;
    return combine(std::move(live), std::move(evaluation), database);
}

std::optional<SupportAssessment> reassess_support(const SupportAssessment& stored,
                                                  const assets::CameraSupport* database) {
    if (!stored.camera) {
        return std::nullopt;
    }
    // The live part as the probe gave it: its status, its interpretation reasons and its engine
    // limitations.
    SupportAssessment live;
    live.interpretation = stored.camera->interpretation;
    SupportStatus interpretation = support_status_for(stored.camera->interpretation);
    std::vector<raw::InterpretationIssue> issues;
    bool interpretation_reasons = false;
    bool engine_limited = false;
    for (const SupportReason& reason : stored.reasons) {
        if (!is_live_reason(reason.kind)) {
            continue;
        }
        if (reason.kind == SupportReasonKind::EngineLimitation) {
            engine_limited = true;
        } else {
            interpretation_reasons = true;
        }
        if (reason.kind == SupportReasonKind::InterpretationIssue && reason.issue) {
            issues.push_back(raw::InterpretationIssue{*reason.issue, reason.message});
        }
        live.reasons.push_back(reason);
    }
    interpretation = assets::worse(interpretation, support_status_for(raw::derive_interpretation_status(issues)));
    if (interpretation != SupportStatus::CompatibleUnverified && !interpretation_reasons) {
        const std::vector<SupportReason> status = assess_interpretation_status(stored.camera->interpretation).reasons;
        live.reasons.insert(live.reasons.begin(), status.begin(), status.end());
    }
    live.status = engine_limited ? assets::worse(interpretation, SupportStatus::Limited) : interpretation;
    CameraSupportEvaluation evaluation;
    evaluation.mode_key = stored.camera->mode_key;
    evaluation.image_area = stored.camera->image_area;
    evaluation.dng_opcodes = stored.camera->dng_opcodes;
    evaluation.interpretation = stored.camera->interpretation;
    if (database == nullptr) {
        return keep(std::move(live), std::move(evaluation), *stored.camera, stored.reasons);
    }
    return combine(std::move(live), std::move(evaluation), database);
}

bool assessed_with(const SupportAssessment& support, const assets::CameraSupport* database) noexcept {
    if (!support.camera || support.camera->rules != kSupportRulesVersion) {
        return false;
    }
    const CameraSupportEvaluation& camera = *support.camera;
    if (database == nullptr) {
        // Nothing can be Verified without a database (a record claiming so is re-assessed).
        return camera.database_id.empty() && support.status != SupportStatus::Verified;
    }
    try {
        return camera.database_id == database->asset_id && camera.database_version == database->version_text() &&
               camera.database_sha256 == core::to_hex(database->manifest_sha256) &&
               camera.basis_matches == database->verified_entries_hold();
    } catch (...) {
        return false;  // allocation failure while comparing: re-assess
    }
}

std::optional<SupportAssessment> refreshed_support(const SourceItem& item, const assets::CameraSupport* database) {
    if (!item.support.camera) {
        return std::nullopt;  // no record: source maintenance probes it again
    }
    if (probe_outdated(item)) {
        // Probed again by source maintenance; meanwhile its stale record must not keep it Verified.
        if (item.support.status != SupportStatus::Verified) {
            return std::nullopt;
        }
        SupportAssessment capped = item.support;
        capped.status = SupportStatus::CompatibleUnverified;
        capped.camera->basis_matches = false;
        return capped;
    }
    if (assessed_with(item.support, database)) {
        return std::nullopt;
    }
    return reassess_support(item.support, database);
}

SupportAssessment current_support(const SourceItem& item, const assets::CameraSupport* database) {
    std::optional<SupportAssessment> refreshed = refreshed_support(item, database);
    return refreshed ? std::move(*refreshed) : item.support;
}

Expected<SupportRefresh> refresh_support(Library& library, const assets::CameraSupport* database) {
    SupportRefresh refresh;
    std::vector<SupportUpdate> updates;
    try {
        for (const SourceItem& item : library.items()) {
            ++refresh.items;
            if (item.probe && probe_outdated(item)) {
                ++refresh.needs_probe;
            }
            std::optional<SupportAssessment> fresh = refreshed_support(item, database);
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
