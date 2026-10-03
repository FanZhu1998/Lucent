// Conversions between library items and catalog records (src/catalog_mapping.hpp): the support
// reasons JSON round trip, tolerance of unreadable or newer JSON, capture-time validation and the
// probe summary that survives a restart.

#include "catalog_mapping.hpp"

#include <string>

#include <gtest/gtest.h>

#include <lucent/application/source_item.hpp>

namespace lucent::app {
namespace {

SupportAssessment unsupported_with_issues() {
    raw::Interpretation interpretation;
    interpretation.status = raw::InterpretationStatus::RequiresUnsupportedOperation;
    interpretation.issues = {
        raw::InterpretationIssue{raw::InterpretationIssueKind::MandatoryOperationUnsupported,
                                 "GainMap \"x\""},
        raw::InterpretationIssue{raw::InterpretationIssueKind::OptionalOperationUnsupported, "line\nbreak"},
    };
    return assess_interpretation(interpretation);
}

TEST(CatalogMapping, SupportReasonsRoundTripThroughJson) {
    const SupportAssessment original = unsupported_with_issues();
    const std::string json = detail::encode_support_reasons(original);
    const SupportAssessment restored = detail::decode_support(catalog::SupportStatus::Unsupported, json);
    EXPECT_EQ(restored.status, SupportStatus::Unsupported);
    ASSERT_EQ(restored.reasons.size(), 2u);
    EXPECT_EQ(restored.reasons[0].kind, SupportReasonKind::InterpretationIssue);
    EXPECT_EQ(restored.reasons[0].issue, raw::InterpretationIssueKind::MandatoryOperationUnsupported);
    EXPECT_EQ(restored.reasons[0].message, "GainMap \"x\"");
    EXPECT_EQ(restored.reasons[1].issue, raw::InterpretationIssueKind::OptionalOperationUnsupported);
    EXPECT_EQ(restored.reasons[1].message, "line\nbreak");
    EXPECT_EQ(restored.interpretation, raw::InterpretationStatus::RequiresUnsupportedOperation);
}

TEST(CatalogMapping, StatusAndProbeFailureReasonsRoundTrip) {
    const SupportAssessment status = assess_interpretation_status(raw::InterpretationStatus::Ambiguous);
    const SupportAssessment restored_status =
        detail::decode_support(catalog::SupportStatus::Unsupported, detail::encode_support_reasons(status));
    ASSERT_EQ(restored_status.reasons.size(), 1u);
    EXPECT_EQ(restored_status.reasons[0].kind, SupportReasonKind::InterpretationStatus);
    EXPECT_EQ(restored_status.interpretation, raw::InterpretationStatus::Ambiguous);

    const SupportAssessment failed =
        assess_probe_failure(Error{ErrorCode::Corrupt, "truncated", "raw.libraw.open", 0});
    const SupportAssessment restored_failed =
        detail::decode_support(catalog::SupportStatus::Unsupported, detail::encode_support_reasons(failed));
    ASSERT_EQ(restored_failed.reasons.size(), 1u);
    EXPECT_EQ(restored_failed.reasons[0].kind, SupportReasonKind::ProbeFailed);
    EXPECT_EQ(restored_failed.reasons[0].error_code, ErrorCode::Corrupt);
    EXPECT_FALSE(restored_failed.interpretation.has_value());
    EXPECT_TRUE(detail::has_probe_failure(restored_failed));
}

TEST(CatalogMapping, SupportedItemsStoreAnEmptyArray) {
    SupportAssessment supported;
    supported.status = SupportStatus::Supported;
    supported.interpretation = raw::InterpretationStatus::Complete;
    EXPECT_EQ(detail::encode_support_reasons(supported), "[]");
    const SupportAssessment restored = detail::decode_support(catalog::SupportStatus::Supported, "[]");
    EXPECT_TRUE(restored.reasons.empty());
    EXPECT_EQ(restored.interpretation, raw::InterpretationStatus::Complete);
}

TEST(CatalogMapping, UnreadableOrNewerReasonsDegradeToGenericOnes) {
    const SupportAssessment garbage = detail::decode_support(catalog::SupportStatus::Limited, "{not json");
    EXPECT_EQ(garbage.status, SupportStatus::Limited);
    ASSERT_EQ(garbage.reasons.size(), 1u);
    EXPECT_EQ(garbage.reasons[0].kind, SupportReasonKind::InterpretationStatus);
    EXPECT_EQ(garbage.interpretation, raw::InterpretationStatus::OptionalOperationsSkipped);

    // A newer Lucent may write issue kinds and element kinds this build does not know.
    const SupportAssessment newer = detail::decode_support(
        catalog::SupportStatus::Unsupported,
        R"([{"kind":"issue","issue":"quantum_sensor","message":"m"},{"kind":"hologram"},7])");
    ASSERT_EQ(newer.reasons.size(), 2u);
    EXPECT_EQ(newer.reasons[0].kind, SupportReasonKind::InterpretationStatus);
    EXPECT_FALSE(newer.reasons[0].issue.has_value());
    EXPECT_EQ(newer.interpretation, raw::InterpretationStatus::RequiresUnsupportedOperation);
}

TEST(CatalogMapping, OnlyWellFormedCaptureTimesAreStored) {
    EXPECT_EQ(detail::catalog_capture_time(std::string("2026-10-03T12:34:56")), "2026-10-03T12:34:56");
    EXPECT_FALSE(detail::catalog_capture_time(std::string("2026:10:03 12:34:56")));
    EXPECT_FALSE(detail::catalog_capture_time(std::string("2026-13-03T12:34:56")));
    EXPECT_FALSE(detail::catalog_capture_time(std::string("2026-10-03T24:00:00")));
    EXPECT_FALSE(detail::catalog_capture_time(std::nullopt));
}

TEST(CatalogMapping, ProbeSummarySurvivesWhatTheCatalogStores) {
    SourceItem item;
    item.path = L"C:\\Photos\\IMG_0001.CR3";
    item.support.status = SupportStatus::Supported;
    item.support.interpretation = raw::InterpretationStatus::Complete;
    ProbeSummary probe;
    probe.extension_hint = "cr3";
    probe.decoder = "libraw";
    probe.decoder_version = "0.22.2-Release";
    probe.camera_make = "Canon";
    probe.camera_model = "EOS R5";
    probe.lens = "RF24-105mm F4 L IS USM";
    probe.capture_time_local = "2026-09-01T07:15:00";
    probe.stored_size = image::Size{8192, 5464};
    probe.orientation = image::Orientation::Rotate90Cw;
    probe.iso = 400.0f;
    item.probe = probe;

    const catalog::ItemMetadata metadata = detail::to_metadata(item, catalog::Timestamp{});
    EXPECT_EQ(metadata.format, "cr3");
    EXPECT_EQ(metadata.orientation, 6);
    EXPECT_EQ(metadata.width, 8192u);
    const std::optional<ProbeSummary> restored = detail::probe_from_metadata(metadata, item.support);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->camera_make, "Canon");
    EXPECT_EQ(restored->camera_model, "EOS R5");
    EXPECT_EQ(restored->lens, probe.lens);
    EXPECT_EQ(restored->capture_time_local, probe.capture_time_local);
    EXPECT_EQ(restored->orientation, image::Orientation::Rotate90Cw);
    EXPECT_EQ(restored->upright_size, (image::Size{5464, 8192}));
    EXPECT_EQ(restored->decoder_version, "0.22.2-Release");
    EXPECT_FALSE(restored->iso.has_value());  // not stored by catalog schema v1

    // An item whose probe failed stores the file extension as its format and restores no probe.
    SourceItem failed;
    failed.path = L"C:\\Photos\\broken.NEF";
    failed.support = assess_probe_failure(Error{ErrorCode::Corrupt, "bad", "raw.probe", 0});
    const catalog::ItemMetadata failed_metadata = detail::to_metadata(failed, catalog::Timestamp{});
    EXPECT_EQ(failed_metadata.format, "nef");
    EXPECT_EQ(failed_metadata.orientation, 1);
    EXPECT_FALSE(detail::probe_from_metadata(failed_metadata, failed.support));
}

}  // namespace
}  // namespace lucent::app
