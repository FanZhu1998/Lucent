// Support-status mapping from the decoded-RAW contract, probe summaries and item flags.

#include <gtest/gtest.h>

#include <lucent/application/source_item.hpp>
#include <lucent/raw/libraw_decoder.hpp>

#include "app_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;

raw::InterpretationIssue issue(raw::InterpretationIssueKind kind, std::string message) {
    return raw::InterpretationIssue{kind, std::move(message)};
}

TEST(SupportAssessment, MapsEveryInterpretationStatus) {
    using S = raw::InterpretationStatus;
    EXPECT_EQ(support_status_for(S::Complete), SupportStatus::CompatibleUnverified);
    EXPECT_EQ(support_status_for(S::OptionalOperationsSkipped), SupportStatus::Limited);
    EXPECT_EQ(support_status_for(S::RequiresUnsupportedOperation), SupportStatus::Unsupported);
    EXPECT_EQ(support_status_for(S::Ambiguous), SupportStatus::Unsupported);
    EXPECT_EQ(support_status_for(S::NotDecodable), SupportStatus::Unsupported);
    EXPECT_EQ(support_status_for(static_cast<S>(99)), SupportStatus::Unsupported);  // newer contract
}

TEST(SupportAssessment, CompleteIsSupportedWithoutReasons) {
    const SupportAssessment a = assess_interpretation(raw::Interpretation{});
    EXPECT_EQ(a.status, SupportStatus::CompatibleUnverified);
    EXPECT_EQ(a.interpretation, raw::InterpretationStatus::Complete);
    EXPECT_TRUE(a.reasons.empty());
}

TEST(SupportAssessment, IssuesBecomeReasons) {
    raw::Interpretation limited;
    limited.status = raw::InterpretationStatus::OptionalOperationsSkipped;
    limited.issues = {issue(raw::InterpretationIssueKind::OptionalOperationUnsupported, "opcode 4 skipped")};
    const SupportAssessment l = assess_interpretation(limited);
    EXPECT_EQ(l.status, SupportStatus::Limited);
    ASSERT_EQ(l.reasons.size(), 1u);
    EXPECT_EQ(l.reasons[0].kind, SupportReasonKind::InterpretationIssue);
    EXPECT_EQ(l.reasons[0].issue, raw::InterpretationIssueKind::OptionalOperationUnsupported);
    EXPECT_EQ(l.reasons[0].message, "opcode 4 skipped");

    raw::Interpretation unsupported;
    unsupported.status = raw::InterpretationStatus::RequiresUnsupportedOperation;
    unsupported.issues = {issue(raw::InterpretationIssueKind::MandatoryOperationUnsupported, "GainMap"),
                          issue(raw::InterpretationIssueKind::OptionalOperationUnsupported, "MapPolynomial")};
    const SupportAssessment u = assess_interpretation(unsupported);
    EXPECT_EQ(u.status, SupportStatus::Unsupported);
    EXPECT_EQ(u.reasons.size(), 2u);
}

TEST(SupportAssessment, NeverReportsBetterThanTheIssuesImply) {
    raw::Interpretation inconsistent;  // status says Complete, the issue says otherwise
    inconsistent.issues = {issue(raw::InterpretationIssueKind::MultiViewSource, "dual fisheye")};
    EXPECT_EQ(assess_interpretation(inconsistent).status, SupportStatus::Unsupported);

    raw::Interpretation no_issue;  // a bad status without issue details still gets a reason
    no_issue.status = raw::InterpretationStatus::Ambiguous;
    const SupportAssessment a = assess_interpretation(no_issue);
    EXPECT_EQ(a.status, SupportStatus::Unsupported);
    ASSERT_EQ(a.reasons.size(), 1u);
    EXPECT_EQ(a.reasons[0].kind, SupportReasonKind::InterpretationStatus);
}

TEST(SupportAssessment, StatusOnlyAndProbeFailures) {
    EXPECT_TRUE(assess_interpretation_status(raw::InterpretationStatus::Complete).reasons.empty());
    const SupportAssessment ambiguous = assess_interpretation_status(raw::InterpretationStatus::Ambiguous);
    EXPECT_EQ(ambiguous.status, SupportStatus::Unsupported);
    EXPECT_EQ(ambiguous.reasons.size(), 1u);

    const SupportAssessment failed =
        assess_probe_failure(Error{ErrorCode::Corrupt, "truncated", "raw.libraw.truncated", 0});
    EXPECT_EQ(failed.status, SupportStatus::Unsupported);
    EXPECT_FALSE(failed.interpretation);
    ASSERT_EQ(failed.reasons.size(), 1u);
    EXPECT_EQ(failed.reasons[0].kind, SupportReasonKind::ProbeFailed);
    EXPECT_EQ(failed.reasons[0].error_code, ErrorCode::Corrupt);
    EXPECT_NE(failed.reasons[0].message.find("truncated"), std::string::npos);
}

TEST(SupportAssessment, Names) {
    EXPECT_EQ(to_string(SupportStatus::CompatibleUnverified), "compatible_unverified");
    EXPECT_EQ(to_string(SupportStatus::Verified), "verified");
    EXPECT_EQ(to_string(SupportStatus::Limited), "limited");
    EXPECT_EQ(to_string(SupportStatus::Unsupported), "unsupported");
}

TEST(ProbeSummary, SummarisesARealProbe) {
    ts::ScratchDir dir;
    raw::test_support::SyntheticDngSpec spec;
    spec.orientation = 6;
    spec.make = "Lucent";
    spec.model = "Summary Camera";
    const auto path = ts::write_dng(dir, "summary.dng", spec);
    auto decoder = raw::create_libraw_decoder();
    ASSERT_TRUE(decoder);
    auto probe = (*decoder)->probe(path, raw::ProbeOptions{}, {});
    ASSERT_TRUE(probe) << probe.error().describe();

    const ProbeSummary s = summarize_probe(probe->metadata);
    EXPECT_EQ(s.container, raw::ContainerFormat::Dng);
    EXPECT_EQ(s.extension_hint, "dng");
    EXPECT_EQ(s.decoder, "libraw");
    EXPECT_NE(s.camera_model.find("Summary Camera"), std::string::npos) << s.camera_model;
    EXPECT_EQ(s.stored_size, (image::Size{64, 48}));
    EXPECT_EQ(s.orientation, image::Orientation::Rotate90Cw);
    EXPECT_EQ(s.upright_size, (image::Size{48, 64}));
    EXPECT_EQ(s.representation, raw::RawRepresentation::Mosaic);
    EXPECT_EQ(s.role, raw::SubimageRole::Primary);
    EXPECT_FALSE(s.compression.empty());
}

TEST(ItemFlags, Validation) {
    EXPECT_TRUE(ItemFlags{}.validate());
    EXPECT_TRUE((ItemFlags{PickFlag::Picked, 5}).validate());
    EXPECT_FALSE((ItemFlags{PickFlag::Rejected, 6}).validate());
    EXPECT_FALSE((ItemFlags{static_cast<PickFlag>(7), 0}).validate());
}

}  // namespace
}  // namespace lucent::app
