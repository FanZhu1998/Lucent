// Related-shot consistency (blueprint section 6; ADR 0011 phase 2):
//   * grouping of synthetic shot features: time gaps and the span limit, illumination splits
//     (scene luminance, as-shot white, as-shot colour, complete linkage, unknown exposure),
//     dissimilar shots, caller overrides, grouping off, items without a capture time;
//   * targets: medians, the camera white balance within one calibration domain only, colour across
//     domains (camera gains are never averaged across cameras), anchors, manual members;
//   * the consistency term's dead zones and scales;
//   * on synthetic scenes: shot features of real analyses, regularisation pulling a related pair
//     together, locks and hard checks that consistency never overrides, anchors, the cross-camera
//     rule end to end, and determinism (a reopened session regularises exactly like a kept one).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/analysis/related_shots.hpp>
#include <lucent/engine/develop_operations.hpp>

#include "analysis_test_support.hpp"

namespace lucent::analysis {
namespace {

using test_support::chart_scene;
using test_support::make_scene;
using test_support::SceneOptions;

// ---- Synthetic shot features -------------------------------------------------------------------

ShotFeatures shot(std::int64_t seconds, double scene_ev = 10.0) {
    ShotFeatures s;
    s.capture_seconds = seconds;
    s.capture_time = std::format("t+{}", seconds);
    s.camera = "Synthetic A";
    s.calibration_domain = "A";
    s.size = image::Size{6000, 4000};
    s.exposure_ev100 = 10.0;
    s.median_signal = kMidGreySignal;
    s.scene_ev = scene_ev;
    s.white_u = 0.2100;
    s.white_v = 0.3200;
    s.white_kelvin = 5200.0;
    s.rendered_colour = true;
    s.rendered_u = 0.1980;
    s.rendered_v = 0.4680;
    s.descriptors = true;
    for (std::size_t k = 0; k < kShotCells; ++k) {
        s.structure[k] = static_cast<float>(std::sin(0.7 * static_cast<double>(k)));
        s.colour_u[k] = static_cast<float>(0.004 * std::cos(0.3 * static_cast<double>(k)));
        s.colour_v[k] = 0.0f;
    }
    for (std::size_t k = 0; k < kShotToneBins; ++k) {
        s.tones[k] = 1.0f / static_cast<float>(kShotToneBins);
    }
    return s;
}

Grouping group(const std::vector<ShotFeatures>& shots, const std::vector<std::optional<std::string>>& user = {},
               GroupingMode mode = GroupingMode::CaptureTime) {
    std::vector<GroupInput> inputs(shots.size());
    for (std::size_t i = 0; i < shots.size(); ++i) {
        inputs[i].shot = &shots[i];
        if (i < user.size()) {
            inputs[i].user_group = user[i];
        }
    }
    Grouping g = group_related_shots(inputs, mode);
    for (std::size_t i = 0; i < shots.size(); ++i) {
        std::printf("[ GROUP    ] %zu -> %s: %s\n", i,
                    g.group_of[i] ? g.groups[*g.group_of[i]].id.c_str() : "-", g.decisions[i].reason.c_str());
    }
    return g;
}

std::vector<std::size_t> members_of(const Grouping& g, std::size_t item) {
    return g.groups.at(g.group_of.at(item).value()).members;
}

TEST(Grouping, SplitsOnTimeGapsAndTheSpanLimit) {
    // Shuffled input order: grouping follows capture time.
    const std::vector<ShotFeatures> shots{shot(215), shot(0), shot(30), shot(200), shot(10)};
    const Grouping g = group(shots);
    EXPECT_EQ(members_of(g, 1), (std::vector<std::size_t>{1, 4, 2}));
    EXPECT_EQ(members_of(g, 3), (std::vector<std::size_t>{3, 0}));
    EXPECT_EQ(g.groups[*g.group_of[1]].id, "time-1");
    EXPECT_EQ(g.groups[*g.group_of[3]].basis, GroupBasis::CaptureTime);
    EXPECT_TRUE(g.decisions[4].joined);
    EXPECT_FALSE(g.decisions[3].joined);
    EXPECT_NE(g.decisions[3].reason.find("170 s after"), std::string::npos) << g.decisions[3].reason;

    // A shot every 50 s: each gap is fine, but a sequence may not span more than 15 minutes.
    std::vector<ShotFeatures> steady;
    for (std::int64_t k = 0; k < 20; ++k) {
        steady.push_back(shot(50 * k));
    }
    const Grouping s = group(steady);
    ASSERT_EQ(s.groups.size(), 2u);
    EXPECT_EQ(s.groups[0].members.size(), 19u);  // 0 .. 900 s
    EXPECT_EQ(s.groups[1].members.size(), 1u);
    EXPECT_NE(s.decisions[19].reason.find("span"), std::string::npos);
}

TEST(Grouping, SplitsOnIlluminationChanges) {
    // Scene luminance: a 2 EV jump starts a new group.
    {
        const Grouping g = group({shot(0, 10.0), shot(5, 10.2), shot(10, 12.2), shot(15, 12.3)});
        EXPECT_EQ(members_of(g, 0), (std::vector<std::size_t>{0, 1}));
        EXPECT_EQ(members_of(g, 2), (std::vector<std::size_t>{2, 3}));
        ASSERT_TRUE(g.decisions[2].comparison.has_value());
        EXPECT_TRUE(g.decisions[2].comparison->illumination_change);
        EXPECT_NEAR(*g.decisions[2].comparison->scene_ev_change, 2.0, 1e-9);
    }
    // Light cannot creep through a chain: each step is 0.6 EV but the third shot is 1.2 EV from
    // the first (complete linkage).
    {
        const Grouping g = group({shot(0, 10.0), shot(5, 10.6), shot(10, 11.2)});
        EXPECT_EQ(members_of(g, 0), (std::vector<std::size_t>{0, 1}));
        EXPECT_EQ(members_of(g, 2), (std::vector<std::size_t>{2}));
        EXPECT_NE(g.decisions[2].reason.find("light differs from item 0"), std::string::npos);
    }
    // The as-shot white moves (the camera's white balance followed a change of light).
    {
        std::vector<ShotFeatures> shots{shot(0), shot(5)};
        shots[1].white_u = *shots[0].white_u + 0.012;
        const Grouping g = group(shots);
        EXPECT_EQ(g.groups.size(), 2u);
        EXPECT_NE(g.decisions[1].reason.find("as-shot white"), std::string::npos);
    }
    // A fixed white balance under changed light: the as-shot rendering's colour moves.
    {
        std::vector<ShotFeatures> shots{shot(0), shot(5)};
        shots[1].rendered_v += 0.015;
        const Grouping g = group(shots);
        EXPECT_EQ(g.groups.size(), 2u);
        EXPECT_NE(g.decisions[1].reason.find("as-shot colour"), std::string::npos);
    }
    // Without exposure metadata a change of light cannot be ruled out: no automatic group.
    {
        std::vector<ShotFeatures> shots{shot(0), shot(5)};
        shots[1].scene_ev.reset();
        const Grouping g = group(shots);
        EXPECT_EQ(g.groups.size(), 2u);
        EXPECT_FALSE(g.decisions[1].comparison->illumination_verified);
    }
}

TEST(Grouping, SplitsDissimilarShots) {
    std::vector<ShotFeatures> shots{shot(0), shot(5), shot(10)};
    for (std::size_t k = 0; k < kShotCells; ++k) {
        shots[1].structure[k] = -shots[1].structure[k];  // a different layout
    }
    shots[2].size = image::Size{4000, 6000};  // portrait
    const Grouping g = group(shots);
    EXPECT_EQ(g.groups.size(), 3u);
    EXPECT_FALSE(g.decisions[1].comparison->similar);
    EXPECT_NE(g.decisions[1].reason.find("layout"), std::string::npos);
    EXPECT_FALSE(g.decisions[2].comparison->comparable);
    EXPECT_NE(g.decisions[2].reason.find("orientation"), std::string::npos);
    // Similar enough: a slightly different framing.
    std::vector<ShotFeatures> close{shot(0), shot(5)};
    for (std::size_t k = 0; k < kShotCells; ++k) {
        close[1].structure[k] += 0.2f * static_cast<float>(std::cos(static_cast<double>(k)));
    }
    EXPECT_EQ(group(close).groups.size(), 1u);
}

TEST(Grouping, CallerOverridesWin) {
    // Items 0 and 4 belong together by the caller's choice despite a long gap and other light;
    // item 2 is kept alone; 1 and 3 group automatically around it.
    std::vector<ShotFeatures> shots{shot(0, 10.0), shot(5), shot(8), shot(12), shot(3000, 14.0)};
    const Grouping g = group(shots, {std::string("a"), std::nullopt, std::string("solo"), std::nullopt,
                                     std::string("a")});
    ASSERT_EQ(g.groups.size(), 3u);
    EXPECT_EQ(g.groups[0].id, "user:a");
    EXPECT_EQ(g.groups[0].basis, GroupBasis::User);
    EXPECT_EQ(g.groups[0].members, (std::vector<std::size_t>{0, 4}));
    EXPECT_EQ(g.groups[1].id, "user:solo");
    EXPECT_EQ(g.groups[1].members, (std::vector<std::size_t>{2}));
    EXPECT_FALSE(g.decisions[2].grouped);
    EXPECT_EQ(g.groups[2].members, (std::vector<std::size_t>{1, 3}));
    EXPECT_EQ(g.groups[2].basis, GroupBasis::CaptureTime);
}

TEST(Grouping, OffAndUntimedItemsStayAlone) {
    const Grouping off = group({shot(0), shot(5), shot(10)}, {}, GroupingMode::None);
    EXPECT_EQ(off.groups.size(), 3u);
    for (const RelatedGroup& r : off.groups) {
        EXPECT_EQ(r.basis, GroupBasis::Single);
    }
    // Caller groups are explicit: they still apply without automatic grouping.
    const Grouping off_user =
        group({shot(0), shot(5), shot(10)}, {std::string("a"), std::nullopt, std::string("a")}, GroupingMode::None);
    ASSERT_EQ(off_user.groups.size(), 2u);
    EXPECT_EQ(off_user.groups[0].basis, GroupBasis::User);
    EXPECT_EQ(off_user.groups[0].members, (std::vector<std::size_t>{0, 2}));
    EXPECT_EQ(off_user.groups[1].basis, GroupBasis::Single);
    std::vector<ShotFeatures> shots{shot(0), shot(5)};
    shots[1].capture_seconds.reset();
    const Grouping g = group(shots);
    EXPECT_EQ(g.groups.size(), 2u);
    EXPECT_EQ(g.decisions[1].reason, "no capture time");
    // Not analysed: no group at all.
    std::vector<GroupInput> inputs(2);
    inputs[0].shot = &shots[0];
    const Grouping missing = group_related_shots(inputs);
    EXPECT_FALSE(missing.group_of[1].has_value());
}

TEST(Grouping, ParsesCaptureTimes) {
    EXPECT_EQ(parse_capture_time("1970-01-01T00:00:10"), 10);
    EXPECT_EQ(*parse_capture_time("2023-10-19T16:13:48") - *parse_capture_time("2023-10-19T16:12:37"), 71);
    EXPECT_EQ(*parse_capture_time("2024-03-01T00:00:00") - *parse_capture_time("2024-02-28T23:59:59"),
              86401);  // a leap day
    EXPECT_EQ(parse_capture_time("2023-10-19 16:13:48.25+02:00"), parse_capture_time("2023-10-19T16:13:48"));
    EXPECT_FALSE(parse_capture_time("2023-13-19T16:13:48"));
    EXPECT_FALSE(parse_capture_time("garbage"));
}

// ---- Targets and the term ----------------------------------------------------------------------

ConsistencyMember member(double lightness, std::string domain, double log_red, double log_blue,
                         double u = 0.20, double v = 0.47) {
    ConsistencyMember m;
    m.available = true;
    m.calibration_domain = std::move(domain);
    m.appearance.valid = true;
    m.appearance.lightness = lightness;
    m.appearance.colour = true;
    m.appearance.u = u;
    m.appearance.v = v;
    m.appearance.camera_gains = true;
    m.appearance.log_red = log_red;
    m.appearance.log_blue = log_blue;
    return m;
}

Grouping one_group(std::size_t n) {
    Grouping g;
    RelatedGroup r;
    r.id = "g";
    r.basis = GroupBasis::User;
    for (std::size_t i = 0; i < n; ++i) {
        r.members.push_back(i);
        g.group_of.push_back(0);
        g.decisions.emplace_back();
    }
    g.groups.push_back(r);
    return g;
}

TEST(ConsistencyTargets, MedianLookAndCameraWhiteBalanceWithinOneDomain) {
    const std::vector<ConsistencyMember> members{member(0.40, "A", 0.70, -0.40), member(0.50, "A", 0.74, -0.42),
                                                 member(0.45, "A", 0.72, -0.38)};
    const auto targets = consistency_targets(one_group(3), members);
    for (std::size_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(targets[i].has_value()) << i;
        const ConsistencyTarget& t = *targets[i];
        EXPECT_EQ(t.group_id, "g");
        EXPECT_DOUBLE_EQ(t.lightness, 0.45);
        EXPECT_FALSE(t.colour);  // one domain: camera white balance is comparable
        EXPECT_TRUE(t.camera_white_balance);
        EXPECT_EQ(t.domain, "A");
        EXPECT_DOUBLE_EQ(t.log_red, 0.72);
        EXPECT_DOUBLE_EQ(t.log_blue, -0.40);
        EXPECT_EQ(t.contributors, 3u);
        EXPECT_DOUBLE_EQ(t.white_balance_confidence, 0.6);
    }
}

TEST(ConsistencyTargets, AcrossCamerasColourIsComparedAndGainsNeverAveraged) {
    // Camera B's gains are far from A's (another sensor): they must never enter A's target.
    const std::vector<ConsistencyMember> members{member(0.40, "A", 0.70, -0.40, 0.200, 0.470),
                                                 member(0.50, "A", 0.74, -0.44, 0.202, 0.472),
                                                 member(0.48, "B", 0.10, 0.20, 0.210, 0.480)};
    const auto targets = consistency_targets(one_group(3), members);
    ASSERT_TRUE(targets[0] && targets[1] && targets[2]);
    for (const auto& t : targets) {
        EXPECT_TRUE(t->colour);  // two domains: the colour-managed comparison applies
        EXPECT_DOUBLE_EQ(t->u, 0.202);
        EXPECT_DOUBLE_EQ(t->v, 0.472);
        EXPECT_DOUBLE_EQ(t->lightness, 0.48);
    }
    EXPECT_TRUE(targets[0]->camera_white_balance);
    EXPECT_DOUBLE_EQ(targets[0]->log_red, 0.72);   // the median of A's two only
    EXPECT_DOUBLE_EQ(targets[0]->log_blue, -0.42);
    EXPECT_EQ(targets[0]->domain_contributors, 2u);
    EXPECT_FALSE(targets[2]->camera_white_balance);  // alone in its domain: colour only
}

TEST(ConsistencyTargets, AnchorsDefineTheLookAndManualMembersStayOut) {
    std::vector<ConsistencyMember> members{member(0.60, "A", 0.80, -0.30), member(0.40, "A", 0.70, -0.40),
                                           member(0.42, "A", 0.70, -0.40), member(0.20, "A", 0.60, -0.50)};
    members[0].anchor = true;
    members[3].manual = true;  // not marked: neither contributes nor moves
    const auto targets = consistency_targets(one_group(4), members);
    EXPECT_FALSE(targets[0].has_value());  // an anchor keeps its settings
    EXPECT_FALSE(targets[3].has_value());
    ASSERT_TRUE(targets[1] && targets[2]);
    EXPECT_TRUE(targets[1]->anchored);
    EXPECT_DOUBLE_EQ(targets[1]->lightness, 0.60);
    EXPECT_DOUBLE_EQ(targets[1]->log_red, 0.80);
    EXPECT_DOUBLE_EQ(targets[1]->white_balance_confidence, 1.0);
    // Without the anchor the manual member still does not contribute.
    members[0].anchor = false;
    const auto unanchored = consistency_targets(one_group(4), members);
    ASSERT_TRUE(unanchored[1].has_value());
    EXPECT_DOUBLE_EQ(unanchored[1]->lightness, 0.42);  // median of 0.60, 0.40, 0.42
    // A group with one analysed member has no target.
    std::vector<ConsistencyMember> lone{member(0.5, "A", 0.7, -0.4), ConsistencyMember{}};
    EXPECT_FALSE(consistency_targets(one_group(2), lone)[0].has_value());
}

TEST(ConsistencyTerm, DeadZonesAndScales) {
    ConsistencyTarget t;
    t.lightness = 0.50;
    t.colour = true;
    t.u = 0.20;
    t.v = 0.47;
    t.camera_white_balance = true;
    t.log_red = 0.70;
    t.log_blue = -0.40;
    Appearance a;
    a.valid = true;
    a.lightness = 0.505;  // inside the dead zone
    a.colour = true;
    a.u = 0.201;
    a.v = 0.47;
    a.camera_gains = true;
    a.log_red = 0.703;
    a.log_blue = -0.40;
    EXPECT_EQ(consistency_term(a, t).total, 0.0);
    a.lightness = 0.55;  // (0.05 - 0.01) / 0.04 = 1
    a.u = 0.206;         // (0.006 - 0.002) / 0.004 = 1
    a.log_red = 0.72;    // (0.02 - 0.005) / 0.015 = 1
    const ConsistencyTerms terms = consistency_term(a, t);
    EXPECT_NEAR(terms.lightness, 1.0, 1e-9);
    EXPECT_NEAR(terms.colour, 1.0, 1e-9);
    EXPECT_NEAR(terms.white_balance, 1.0, 1e-9);
    EXPECT_NEAR(terms.total, 3.0, 1e-9);
    t.weight = 0.5;
    EXPECT_NEAR(consistency_term(a, t).total, 1.5, 1e-9);
    a.valid = false;
    EXPECT_EQ(consistency_term(a, t).total, 0.0);
}

// ---- On synthetic scenes -----------------------------------------------------------------------

SceneOptions related_options(std::string time, double exposure = 1.0) {
    SceneOptions o;
    o.capture_time = std::move(time);
    o.f_number = 8.0f;
    o.exposure_time_s = static_cast<float>(0.01 * exposure);  // same scene, longer exposure
    return o;
}

std::optional<AutoSession> open(const test_support::Scene& scene, const SceneOptions& o,
                                const engine::DevelopSettings& current = {}) {
    auto session = AutoSession::open(make_scene(scene, o), current);
    EXPECT_TRUE(session) << session.error().describe();
    if (!session) {
        return std::nullopt;
    }
    return std::move(*session);
}

void trace(const char* what, const AutoResult& r) {
    const CandidateRecord& c = r.candidates[r.chosen];
    std::printf("[ CONSIST  ] %s: chosen %zu (%s) E%+.3f r%.4f b%.4f S%.3f H%.3f C%+.3f s%.3f; L %.3f u'v' (%.4f, "
                "%.4f)",
                what, r.chosen, std::string(to_string(c.origin)).c_str(), c.edit.exposure_ev, c.edit.red_gain,
                c.edit.blue_gain, c.edit.shadows_ev, c.edit.highlights_ev, c.edit.contrast, c.edit.saturation,
                r.appearance.lightness, r.appearance.u, r.appearance.v);
    if (r.consistency) {
        const ConsistencyRecord& k = *r.consistency;
        std::printf("; term %.3f -> %.3f, axis '%s', %u renders", k.independent_terms.total, k.terms.total,
                    k.white_balance_axis.c_str(), k.added_candidates);
    }
    std::printf("\n");
}

// Every candidate with its terms (run with --gtest_also_run_disabled_tests).
void dump(const AutoResult& r) {
    if (!::testing::GTEST_FLAG(also_run_disabled_tests)) {
        return;
    }
    const auto score = [](const CandidateScore& s) {
        if (!s.evaluated) {
            return std::string("-");
        }
        const ObjectiveTerms& t = s.terms;
        return std::format("{} {:.3f} [b {:.2f} h {:.2f} s {:.2f} n {:.2f} c {:.2f} col {:.2f} wb {:.2f} nat {:.2f} "
                           "e {:.2f} k {:.2f}] L {:.3f}{}",
                           s.feasible ? "ok" : "NO", t.total, t.brightness, t.highlights, t.shadows, t.noise,
                           t.contrast, t.color, t.white_balance, t.native, t.edit, t.consistency,
                           s.appearance.lightness, s.failures.empty() ? "" : " " + s.failures.front());
    };
    for (const CandidateRecord& c : r.candidates) {
        std::printf("  %2u %-22s %-40s E%+.3f r%.3f b%.3f S%.3f H%.3f | proxy %s | analysis %s\n", c.index,
                    std::string(to_string(c.origin)).c_str(), c.note.substr(0, 40).c_str(), c.edit.exposure_ev,
                    c.edit.red_gain, c.edit.blue_gain, c.edit.shadows_ev, c.edit.highlights_ev,
                    score(c.proxy).c_str(), score(c.analysis).c_str());
    }
}

ConsistencyMember as_member(const AutoResult& r) {
    ConsistencyMember m;
    m.available = true;
    m.appearance = r.appearance;
    m.calibration_domain = r.shot.calibration_domain;
    return m;
}

TEST(ShotFeatures, DescribeRealAnalyses) {
    auto a = open(chart_scene(1.0), related_options("2026-10-03T10:00:00"));
    auto b = open(chart_scene(1.6), related_options("2026-10-03T10:00:08", 1.6));
    SceneOptions other_light = related_options("2026-10-03T10:00:12");
    other_light.as_shot = {1.6, 1.0, 1.9};  // the camera balanced for another light
    auto c = open(chart_scene(1.0, 1.25, 0.79), other_light);
    auto d = open(test_support::chart_scene(1.0), related_options("2026-10-03T10:00:15"));
    ASSERT_TRUE(a && b && c && d);
    const ShotFeatures& fa = a->independent().shot;
    EXPECT_EQ(fa.capture_seconds, parse_capture_time("2026-10-03T10:00:00"));
    EXPECT_TRUE(fa.descriptors);
    EXPECT_TRUE(fa.scene_ev.has_value());
    EXPECT_TRUE(fa.white_u.has_value());
    EXPECT_TRUE(fa.rendered_colour);
    EXPECT_EQ(fa.camera, "Synthetic Scene");
    EXPECT_NE(fa.calibration_domain.find("decoder_matrix"), std::string::npos);

    // The same chart 0.68 EV brighter through a longer exposure: similar, same light.
    const ShotComparison ab = compare_shots(fa, b->independent().shot);
    std::printf("[ SHOT     ] a-b: %s; layout %.3f, colour %.4f, tones %.3f, scene %.3f EV\n", ab.reason.c_str(),
                ab.structure_ev, ab.colour_layout, ab.tone_distance, ab.scene_ev_change.value_or(-1.0));
    EXPECT_TRUE(ab.similar) << ab.reason;
    EXPECT_FALSE(ab.illumination_change) << ab.reason;
    EXPECT_LT(*ab.scene_ev_change, 0.2);
    EXPECT_DOUBLE_EQ(*ab.gap_seconds, 8.0);

    // The same chart under warmer light with the camera following it: a change of light.
    const ShotComparison ac = compare_shots(fa, c->independent().shot);
    std::printf("[ SHOT     ] a-c: %s\n", ac.reason.c_str());
    EXPECT_TRUE(ac.illumination_change);

    // A different scene: not similar.
    auto e = open(
        [](std::int32_t x, std::int32_t y) -> std::array<double, 3> {
            const double v = (x < 480 ? 0.02 : 0.5) * (1.0 + 0.1 * test_support::texture(x, y));
            return test_support::neutral(v);
        },
        related_options("2026-10-03T10:00:20"));
    ASSERT_TRUE(e);
    const ShotComparison ae = compare_shots(fa, e->independent().shot);
    std::printf("[ SHOT     ] a-e: %s\n", ae.reason.c_str());
    EXPECT_FALSE(ae.similar);

    // Identical inputs describe identically.
    const ShotFeatures& fd = d->independent().shot;
    EXPECT_EQ(fd.structure, fa.structure);
    EXPECT_EQ(fd.tones, fa.tones);
    EXPECT_EQ(fd.rendered_u, fa.rendered_u);
}

TEST(Consistency, PullsARelatedPairTogether) {
    // Two shots of one chart, the second 0.77 EV brighter: both medians lie inside the Natural v1
    // band, so the independent results keep the difference.
    auto a = open(chart_scene(1.0), related_options("2026-10-03T10:00:00"));
    auto b = open(chart_scene(1.7), related_options("2026-10-03T10:00:10", 1.7));
    ASSERT_TRUE(a && b);
    trace("a independent", a->independent());
    trace("b independent", b->independent());
    const double before = std::abs(a->independent().appearance.lightness - b->independent().appearance.lightness);
    ASSERT_GT(before, 0.05);

    const std::vector<ShotFeatures> shots{a->independent().shot, b->independent().shot};
    std::vector<GroupInput> inputs(2);
    inputs[0].shot = &shots[0];
    inputs[1].shot = &shots[1];
    const Grouping g = group_related_shots(inputs);
    ASSERT_EQ(g.groups.size(), 1u) << g.decisions[1].reason;
    const std::vector<ConsistencyMember> members{as_member(a->independent()), as_member(b->independent())};
    const auto targets = consistency_targets(g, members);
    ASSERT_TRUE(targets[0] && targets[1]);
    EXPECT_TRUE(targets[0]->camera_white_balance);
    EXPECT_FALSE(targets[0]->colour);

    auto ra = a->regularise(*targets[0]);
    auto rb = b->regularise(*targets[1]);
    ASSERT_TRUE(ra) << ra.error().describe();
    ASSERT_TRUE(rb) << rb.error().describe();
    trace("a regularised", *ra);
    trace("b regularised", *rb);
    const double after = std::abs(ra->appearance.lightness - rb->appearance.lightness);
    EXPECT_LT(after, before - 0.03);
    for (const AutoResult* r : {&*ra, &*rb}) {
        ASSERT_TRUE(r->consistency.has_value());
        const ConsistencyRecord& k = *r->consistency;
        EXPECT_EQ(k.target.group_id, "time-1");
        EXPECT_LE(k.terms.total, k.independent_terms.total + 1e-12);
        EXPECT_TRUE(k.changed);
        EXPECT_GT(k.added_candidates, 0u);
        EXPECT_LE(k.added_candidates, kDefaultConsistencyBudget);
        EXPECT_TRUE(r->candidates[r->chosen].analysis.feasible);
        EXPECT_EQ(r->versions.consistency_version, kConsistencyVersion);
        EXPECT_GT(r->timing.consistency_seconds, 0.0);
        // The consistency stage's candidates carry the term; the independent result does not.
        EXPECT_EQ(r->candidates[k.independent_chosen].analysis.terms.consistency, k.independent_terms.total);
    }
    // The darker shot is brightened, the brighter one darkened.
    EXPECT_GT(ra->settings.exposure_ev, a->independent().settings.exposure_ev);
    EXPECT_LT(rb->settings.exposure_ev, b->independent().settings.exposure_ev);
    EXPECT_GT(ra->consistency->adjustment.exposure_ev, 0.0);
    // The independent result stays available and unchanged.
    EXPECT_FALSE(a->independent().consistency.has_value());
}

TEST(Consistency, NeverOverridesLocks) {
    engine::DevelopSettings current;
    current.exposure_ev = 0.0;
    current.contrast = 0.05;
    current.locks = {engine::DevelopControl::Exposure, engine::DevelopControl::Contrast};
    auto s = open(chart_scene(1.0), related_options("2026-10-03T10:00:00"), current);
    ASSERT_TRUE(s);
    const AutoResult& independent = s->independent();
    ConsistencyTarget t;
    t.group_id = "locks";
    t.lightness = independent.appearance.lightness + 0.20;  // far brighter than this shot
    t.camera_white_balance = true;                           // and a warmer white balance
    t.domain = independent.shot.calibration_domain;
    t.log_red = independent.appearance.log_red + 0.04;
    t.log_blue = independent.appearance.log_blue - 0.02;
    auto r = s->regularise(t);
    ASSERT_TRUE(r) << r.error().describe();
    trace("locked", *r);
    dump(*r);
    for (const CandidateRecord& c : r->candidates) {
        EXPECT_EQ(c.settings.exposure_ev, 0.0) << c.note;
        EXPECT_EQ(c.settings.contrast, 0.05) << c.note;
        EXPECT_EQ(c.settings.locks, current.locks) << c.note;
    }
    EXPECT_EQ(r->settings.exposure_ev, 0.0);
    EXPECT_EQ(r->settings.contrast, 0.05);
    // The unlocked white balance follows the target; the locked exposure cannot reach the lightness.
    EXPECT_EQ(r->consistency->white_balance_axis, "camera");
    EXPECT_GT(r->appearance.log_red, independent.appearance.log_red + 0.02);
    EXPECT_LT(r->consistency->terms.white_balance, r->consistency->independent_terms.white_balance);
    EXPECT_GT(r->consistency->terms.lightness, 1.0);
    EXPECT_TRUE(r->has_flag(ReviewFlagCode::ConsistencyLimited));

    // With the white balance locked as well, it never moves either.
    engine::DevelopSettings more = current;
    more.white_balance = engine::KelvinWhiteBalance{5500.0, 0.0};
    more.locks.insert(engine::DevelopControl::WhiteBalance);
    auto locked = open(chart_scene(1.0), related_options("2026-10-03T10:00:00"), more);
    ASSERT_TRUE(locked);
    auto rl = locked->regularise(t);
    ASSERT_TRUE(rl) << rl.error().describe();
    for (const CandidateRecord& c : rl->candidates) {
        EXPECT_EQ(c.settings.white_balance, more.white_balance) << c.note;
    }
    EXPECT_EQ(rl->consistency->white_balance_axis, "");
}

TEST(Consistency, NeverOverridesHardChecks) {
    // A frame whose left part is barely above black, pulled towards an impossible dark target with
    // a weight that outweighs every preference term: darkening blocks those shadows to code 0, a
    // hard check, so the most consistent candidates are infeasible; the result stays feasible and
    // is flagged.
    const test_support::Scene scene = [](std::int32_t x, std::int32_t y) -> std::array<double, 3> {
        if (x < 400) {
            return test_support::neutral(0.0004);
        }
        return test_support::chart_scene(1.0)(x, y);
    };
    auto s = open(scene, related_options("2026-10-03T10:00:00"));
    ASSERT_TRUE(s);
    ConsistencyTarget t;
    t.group_id = "impossible";
    t.lightness = 0.02;
    t.weight = 10.0;
    auto r = s->regularise(t);
    ASSERT_TRUE(r) << r.error().describe();
    trace("dark", *r);
    dump(*r);
    const CandidateRecord& chosen = r->candidates[r->chosen];
    EXPECT_TRUE(chosen.analysis.feasible);
    const MetricResult* blocked = r->diagnostic(MetricId::ShadowBlocking);
    ASSERT_NE(blocked, nullptr);
    EXPECT_LE(blocked->value("new_fraction").value_or(1.0), kMaxNewShadowBlockFraction);
    EXPECT_TRUE(r->has_flag(ReviewFlagCode::ConsistencyLimited));
    // Some candidate was more consistent but failed a hard check, and lost.
    bool overridden = false;
    for (const CandidateRecord& c : r->candidates) {
        if (c.proxy.evaluated && !c.proxy.feasible && c.proxy.appearance.valid &&
            c.proxy.terms.consistency < chosen.proxy.terms.consistency) {
            overridden = true;
        }
    }
    EXPECT_TRUE(overridden);
}

TEST(Consistency, AnchorsKeepTheirSettingsAndSetTheLook) {
    // The anchor: a manual edit (+0.6 EV, everything locked) the caller marked.
    engine::DevelopSettings manual;
    manual.exposure_ev = 0.6;
    manual.locks = {engine::DevelopControl::Exposure,   engine::DevelopControl::Shadows,
                    engine::DevelopControl::Highlights, engine::DevelopControl::Contrast,
                    engine::DevelopControl::WhiteBalance, engine::DevelopControl::Saturation};
    ASSERT_TRUE(is_manual(manual, true));
    auto anchor = open(chart_scene(1.0), related_options("2026-10-03T10:00:00"), manual);
    auto other = open(chart_scene(1.0), related_options("2026-10-03T10:00:05"));
    ASSERT_TRUE(anchor && other);
    auto look = anchor->appearance_of(manual);
    ASSERT_TRUE(look) << look.error().describe();
    std::vector<ConsistencyMember> members{as_member(anchor->independent()), as_member(other->independent())};
    members[0].anchor = true;
    members[0].manual = true;
    members[0].appearance = *look;
    const auto targets = consistency_targets(one_group(2), members);
    EXPECT_FALSE(targets[0].has_value());
    ASSERT_TRUE(targets[1].has_value());
    EXPECT_TRUE(targets[1]->anchored);
    EXPECT_DOUBLE_EQ(targets[1]->lightness, look->lightness);
    auto r = other->regularise(*targets[1]);
    ASSERT_TRUE(r) << r.error().describe();
    trace("towards the anchor", *r);
    EXPECT_LT(std::abs(r->appearance.lightness - look->lightness),
              std::abs(other->independent().appearance.lightness - look->lightness));
}

TEST(Consistency, AcrossCamerasUsesColourManagedTargets) {
    // Camera B: another calibration and as-shot balance (its neutrals balance under its own
    // as-shot multipliers), same scene and light.
    SceneOptions oa1 = related_options("2026-10-03T10:00:00");
    SceneOptions oa2 = related_options("2026-10-03T10:00:04", 1.3);
    SceneOptions ob = related_options("2026-10-03T10:00:02");
    ob.model = "Scene B";
    ob.cam_xyz = engine::test_support::kCmD65;
    ob.as_shot = {1.8, 1.0, 1.7};
    const test_support::Scene chart = chart_scene(1.0);
    // Camera B's automatic white balance left a slight warm cast (4 % red, 3 % less blue).
    const test_support::Scene chart_b = [chart](std::int32_t x, std::int32_t y) -> std::array<double, 3> {
        const auto c = chart(x, y);
        return {c[0] * 2.0 / 1.8 * 1.04, c[1], c[2] * 1.5 / 1.7 * 0.97};
    };
    auto a1 = open(chart, oa1);
    auto a2 = open(chart_scene(1.3), oa2);
    auto b = open(chart_b, ob);
    ASSERT_TRUE(a1 && a2 && b);
    EXPECT_NE(a1->independent().shot.calibration_domain, b->independent().shot.calibration_domain);
    EXPECT_EQ(a1->independent().shot.calibration_domain, a2->independent().shot.calibration_domain);
    const std::vector<ConsistencyMember> members{as_member(a1->independent()), as_member(a2->independent()),
                                                 as_member(b->independent())};
    const auto targets = consistency_targets(one_group(3), members);
    ASSERT_TRUE(targets[0] && targets[1] && targets[2]);
    EXPECT_TRUE(targets[2]->colour);
    EXPECT_FALSE(targets[2]->camera_white_balance);
    EXPECT_TRUE(targets[0]->camera_white_balance);
    EXPECT_DOUBLE_EQ(targets[0]->log_red, 0.5 * (members[0].appearance.log_red + members[1].appearance.log_red));
    trace("camera B independent", b->independent());
    auto rb = b->regularise(*targets[2]);
    ASSERT_TRUE(rb) << rb.error().describe();
    trace("camera B", *rb);
    dump(*rb);
    const ConsistencyRecord& k = *rb->consistency;
    EXPECT_EQ(k.white_balance_axis, "colour");  // never camera gains across domains
    const auto colour_distance = [&](const Appearance& x) {
        return std::hypot(x.u - targets[2]->u, x.v - targets[2]->v);
    };
    std::printf("[ CONSIST  ] camera B colour distance %.4f -> %.4f\n", colour_distance(b->independent().appearance),
                colour_distance(rb->appearance));
    EXPECT_GT(colour_distance(b->independent().appearance), kConsistencyColourDeadZone);
    EXPECT_LT(colour_distance(rb->appearance), colour_distance(b->independent().appearance));
    EXPECT_LT(k.terms.colour, k.independent_terms.colour);
    std::size_t probes = 0;
    for (const CandidateRecord& c : rb->candidates) {
        probes += c.note.find("probe") != std::string::npos ? 1u : 0u;
    }
    EXPECT_EQ(probes, 2u);
}

TEST(Consistency, IsDeterministicAndIndependentOfTheSession) {
    auto a = open(chart_scene(1.0, 1.04, 0.97), related_options("2026-10-03T10:00:00"));
    auto b = open(chart_scene(1.0, 1.04, 0.97), related_options("2026-10-03T10:00:00"));
    ASSERT_TRUE(a && b);
    ConsistencyTarget t;
    t.group_id = "d";
    t.lightness = a->independent().appearance.lightness + 0.08;
    t.camera_white_balance = true;
    t.domain = a->independent().shot.calibration_domain;
    t.log_red = a->independent().appearance.log_red + 0.03;
    t.log_blue = a->independent().appearance.log_blue - 0.02;
    auto r1 = a->regularise(t);
    auto r2 = a->regularise(t);  // again on the same session
    auto r3 = b->regularise(t);  // on another session of the same input ("reopened")
    ASSERT_TRUE(r1 && r2 && r3);
    trace("deterministic", *r1);
    for (const AutoResult* r : {&*r2, &*r3}) {
        EXPECT_EQ(r->settings, r1->settings);
        EXPECT_EQ(r->chosen, r1->chosen);
        ASSERT_EQ(r->candidates.size(), r1->candidates.size());
        for (std::size_t i = 0; i < r1->candidates.size(); ++i) {
            EXPECT_EQ(r->candidates[i].settings, r1->candidates[i].settings) << i;
            EXPECT_EQ(r->candidates[i].proxy.terms, r1->candidates[i].proxy.terms) << i;
            EXPECT_EQ(r->candidates[i].analysis.terms, r1->candidates[i].analysis.terms) << i;
        }
        EXPECT_EQ(r->consistency->terms, r1->consistency->terms);
        EXPECT_EQ(r->consistency->white_balance_axis, r1->consistency->white_balance_axis);
    }
    EXPECT_EQ(r1->consistency->white_balance_axis, "camera");
    // Invalid targets are refused.
    ConsistencyTarget bad = t;
    bad.lightness = std::nan("");
    auto refused = a->regularise(bad);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidArgument);
}

TEST(Consistency, WithoutPressureTheIndependentResultStands) {
    auto s = open(chart_scene(0.7), related_options("2026-10-03T10:00:00"));
    ASSERT_TRUE(s);
    const AutoResult& independent = s->independent();
    ConsistencyTarget t;
    t.group_id = "already";
    t.lightness = independent.appearance.lightness + 0.005;  // inside the dead zone
    t.camera_white_balance = true;
    t.domain = independent.shot.calibration_domain;
    t.log_red = independent.appearance.log_red;
    t.log_blue = independent.appearance.log_blue;
    auto r = s->regularise(t);
    ASSERT_TRUE(r) << r.error().describe();
    EXPECT_EQ(r->settings, independent.settings);
    EXPECT_EQ(r->chosen, independent.chosen);
    EXPECT_EQ(r->candidates.size(), independent.candidates.size());
    ASSERT_TRUE(r->consistency.has_value());
    EXPECT_FALSE(r->consistency->changed);
    EXPECT_EQ(r->consistency->added_candidates, 0u);
    EXPECT_EQ(r->consistency->terms.total, 0.0);
    EXPECT_EQ(r->timing.consistency_candidates, 0u);
}

TEST(Consistency, PreviewsAreReducedRendersOfTheAnalysisImage) {
    auto s = open(chart_scene(1.0), related_options("2026-10-03T10:00:00"));
    ASSERT_TRUE(s);
    auto preview = s->preview(s->independent().settings, 480);
    ASSERT_TRUE(preview) << preview.error().describe();
    EXPECT_EQ(preview->pixels.width(), 480);
    EXPECT_EQ(preview->pixels.height(), 320);
    EXPECT_EQ(preview->encoding, engine::display_linear_rec2020());
    auto full = s->preview(s->independent().settings, 4000);  // capped at the analysis size
    ASSERT_TRUE(full);
    EXPECT_EQ(full->pixels.size(), s->independent().analysis_size);
    EXPECT_FALSE(s->preview(s->independent().settings, 4));
}

}  // namespace
}  // namespace lucent::analysis
