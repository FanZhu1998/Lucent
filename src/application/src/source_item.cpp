#include <format>
#include <string>
#include <utility>

#include <lucent/application/source_item.hpp>
#include <lucent/assets/mode_key.hpp>
#include <lucent/raw/libraw_decoder.hpp>

namespace lucent::app {

namespace {

SupportReason status_reason(raw::InterpretationStatus status) {
    SupportReason reason;
    reason.kind = SupportReasonKind::InterpretationStatus;
    reason.message =
        std::format("the decoder reports the interpretation status '{}'", raw::to_string(status));
    return reason;
}

}  // namespace

std::string_view to_string(SourceState state) noexcept {
    switch (state) {
        case SourceState::Present:
            return "present";
        case SourceState::Missing:
            return "missing";
        case SourceState::Changed:
            return "changed";
    }
    return "unknown";
}

std::string_view to_string(ImportMode mode) noexcept {
    switch (mode) {
        case ImportMode::Referenced:
            return "referenced";
        case ImportMode::Copied:
            return "copied";
    }
    return "unknown";
}

SupportStatus support_status_for(raw::InterpretationStatus status) noexcept {
    switch (status) {
        case raw::InterpretationStatus::Complete:
            return SupportStatus::CompatibleUnverified;
        case raw::InterpretationStatus::OptionalOperationsSkipped:
            return SupportStatus::Limited;
        case raw::InterpretationStatus::RequiresUnsupportedOperation:
        case raw::InterpretationStatus::Ambiguous:
        case raw::InterpretationStatus::NotDecodable:
            return SupportStatus::Unsupported;
    }
    return SupportStatus::Unsupported;
}

SupportAssessment assess_interpretation(const raw::Interpretation& interpretation) {
    SupportAssessment assessment;
    assessment.interpretation = interpretation.status;
    assessment.status = support_status_for(interpretation.status);
    // The contract derives the status from its issues; a status worse than its issues imply
    // (never expected) still yields the stricter answer.
    const raw::InterpretationStatus derived = raw::derive_interpretation_status(interpretation.issues);
    if (support_status_for(derived) > assessment.status) {
        assessment.status = support_status_for(derived);
    }
    for (const raw::InterpretationIssue& issue : interpretation.issues) {
        SupportReason reason;
        reason.kind = SupportReasonKind::InterpretationIssue;
        reason.issue = issue.kind;
        reason.message = issue.message;
        assessment.reasons.push_back(std::move(reason));
    }
    if (assessment.status != SupportStatus::CompatibleUnverified && assessment.reasons.empty()) {
        assessment.reasons.push_back(status_reason(interpretation.status));
    }
    return assessment;
}

SupportAssessment assess_interpretation_status(raw::InterpretationStatus status) {
    SupportAssessment assessment;
    assessment.interpretation = status;
    assessment.status = support_status_for(status);
    if (assessment.status != SupportStatus::CompatibleUnverified) {
        assessment.reasons.push_back(status_reason(status));
    }
    return assessment;
}

SupportAssessment assess_probe_failure(const Error& error) {
    SupportAssessment assessment;
    assessment.status = SupportStatus::Unsupported;
    SupportReason reason;
    reason.kind = SupportReasonKind::ProbeFailed;
    reason.error_code = error.code;
    reason.message = error.describe();
    assessment.reasons.push_back(std::move(reason));
    return assessment;
}

ProbeSummary summarize_probe(const raw::RawMetadata& metadata) {
    ProbeSummary summary;
    summary.container = metadata.format.container;
    summary.extension_hint = metadata.format.extension_hint;
    summary.decoder = metadata.decoder.adapter_id;
    summary.decoder_version = metadata.decoder.library_version;
    summary.contract = raw::kDecodedRawContractVersion;
    const raw::CameraIdentity& camera = metadata.camera;
    summary.camera_make = camera.normalized_make.empty() ? camera.make : camera.normalized_make;
    summary.camera_model = camera.normalized_model.empty() ? camera.model : camera.normalized_model;
    if (!metadata.lens.model.empty()) {
        summary.lens = metadata.lens.make.empty() || metadata.lens.model.starts_with(metadata.lens.make)
                           ? metadata.lens.model
                           : metadata.lens.make + " " + metadata.lens.model;
    }
    summary.capture_time_local = metadata.capture.capture_time_local;
    summary.iso = metadata.capture.iso;
    summary.exposure_time_s = metadata.capture.exposure_time_s;
    summary.f_number = metadata.capture.f_number;
    summary.focal_length_mm = metadata.capture.focal_length_mm;
    summary.stored_size = metadata.geometry.visible_area.size();
    summary.orientation = metadata.geometry.orientation;
    summary.upright_size = image::oriented_size(summary.stored_size, summary.orientation);
    summary.representation = metadata.representation.kind;
    summary.role = metadata.subimage_role;
    summary.compression = metadata.format.compression.name;
    return summary;
}

Status ItemFlags::validate() const {
    if (pick != PickFlag::None && pick != PickFlag::Picked && pick != PickFlag::Rejected) {
        return fail(ErrorCode::InvalidArgument, std::format("unknown pick flag {}", static_cast<int>(pick)),
                    "app.library.flags");
    }
    if (rating > 5) {
        return fail(ErrorCode::InvalidArgument, std::format("rating {} is above 5", rating),
                    "app.library.flags");
    }
    return ok();
}

bool is_live_reason(SupportReasonKind kind) noexcept {
    return kind == SupportReasonKind::InterpretationIssue || kind == SupportReasonKind::InterpretationStatus ||
           kind == SupportReasonKind::ProbeFailed || kind == SupportReasonKind::EngineLimitation;
}

namespace {

// "LibRaw" adapter's library version of this build ("0.22.2-Release"), computed once.
const std::string& running_libraw_version() noexcept {
    static const std::string version = [] {
        try {
            return raw::libraw_build_info().version;
        } catch (...) {
            return std::string();
        }
    }();
    return version;
}

}  // namespace

bool probe_outdated(const SourceItem& item) noexcept {
    if (!item.probe) {
        return false;
    }
    if (!item.probe->contract || *item.probe->contract < kOldestCurrentProbeContract) {
        return true;
    }
    // AST-08: a probe of another LibRaw version may have another mode key (a new visible area).
    if (item.probe->decoder == raw::kLibRawDecoderId && !running_libraw_version().empty() &&
        item.probe->decoder_version != running_libraw_version()) {
        return true;
    }
    // Assessed before the camera support database (ADR 0018), or by older support rules: only a new
    // probe gives the mode key the database is consulted with.
    if (!item.support.camera || item.support.camera->rules < kSupportRulesVersion) {
        return true;
    }
    // A key of another mode-key version never equals a database key (AST-08).
    const std::string prefix = "mk" + std::to_string(assets::kModeKeyVersion) + "|";
    return !item.support.camera->mode_key.starts_with(prefix);
}

}  // namespace lucent::app
