#include <format>
#include <string>
#include <utility>

#include <lucent/application/source_item.hpp>

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

std::string_view to_string(SupportStatus status) noexcept {
    switch (status) {
        case SupportStatus::Supported:
            return "supported";
        case SupportStatus::Limited:
            return "limited";
        case SupportStatus::Unsupported:
            return "unsupported";
    }
    return "unknown";
}

SupportStatus support_status_for(raw::InterpretationStatus status) noexcept {
    switch (status) {
        case raw::InterpretationStatus::Complete:
            return SupportStatus::Supported;
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
    if (assessment.status != SupportStatus::Supported && assessment.reasons.empty()) {
        assessment.reasons.push_back(status_reason(interpretation.status));
    }
    return assessment;
}

SupportAssessment assess_interpretation_status(raw::InterpretationStatus status) {
    SupportAssessment assessment;
    assessment.interpretation = status;
    assessment.status = support_status_for(status);
    if (assessment.status != SupportStatus::Supported) {
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

}  // namespace lucent::app
