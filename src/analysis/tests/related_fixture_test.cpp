// The two-pass related-shot flow on real files (develop_related_files): the Nikon Z 6 smoke and
// qualification fixtures are two shots of one scene taken 8 s apart. The flow is run with the
// sessions kept between the passes and with every session reopened from its file: the results must
// be identical (determinism), and the grouping decision is reported. Skips when a fixture is absent.

#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/analysis/related_shots.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "analysis_test_support.hpp"

namespace lucent::analysis {
namespace {

TEST(RelatedFixtures, KeptAndReopenedSessionsGiveTheSameResults) {
    LUCENT_ENGINE_REQUIRE_FIXTURE(first, "nikon-z-6-nef-compressed");
    LUCENT_ENGINE_REQUIRE_FIXTURE(second, "nikon-z-6-nef-lossless");
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry) << registry.error().describe();
    std::vector<RelatedItem> items(2);
    items[0].path = first;
    items[1].path = second;

    RelatedOptions kept;
    auto a = develop_related_files(*registry, items, kept);
    ASSERT_TRUE(a) << a.error().describe();
    RelatedOptions reopen;
    reopen.session_budget_bytes = 0;
    auto b = develop_related_files(*registry, items, reopen);
    ASSERT_TRUE(b) << b.error().describe();

    for (std::size_t i = 0; i < items.size(); ++i) {
        const RelatedItemResult& r = a->items[i];
        ASSERT_FALSE(r.error) << r.error->describe();
        std::printf("[ RELATED  ] %zu: %s (%s); target %s; %.2f s%s\n", i,
                    a->grouping.decisions[i].reason.c_str(),
                    a->grouping.groups[*a->grouping.group_of[i]].id.c_str(), r.target ? "yes" : "no", r.seconds,
                    r.regularised && r.regularised->consistency
                        ? std::format("; term {:.3f} -> {:.3f}", r.regularised->consistency->independent_terms.total,
                                      r.regularised->consistency->terms.total)
                              .c_str()
                        : "");
        EXPECT_EQ(r.settings, b->items[i].settings) << i;
        EXPECT_EQ(r.target.has_value(), b->items[i].target.has_value()) << i;
        EXPECT_FALSE(r.reopened);
        EXPECT_EQ(b->items[i].reopened, b->items[i].target.has_value()) << i;
        if (r.regularised) {
            ASSERT_TRUE(b->items[i].regularised.has_value());
            EXPECT_EQ(r.regularised->chosen, b->items[i].regularised->chosen);
            EXPECT_EQ(r.regularised->consistency->terms, b->items[i].regularised->consistency->terms);
            EXPECT_TRUE(r.regularised->candidates[r.regularised->chosen].analysis.feasible);
        }
    }
    EXPECT_EQ(a->reopened, 0u);
    EXPECT_GT(a->peak_session_bytes, 0u);
    EXPECT_EQ(b->peak_session_bytes, 0u);
    EXPECT_EQ(a->grouping.group_of, b->grouping.group_of);
    // Two shots of one scene, 8 s apart, same exposure: one group.
    EXPECT_EQ(a->grouping.groups.size(), 1u);
}

}  // namespace
}  // namespace lucent::analysis
