// PreviewCache: LRU order, byte bounds, replacement, per-item invalidation and statistics.

#include <memory>

#include <gtest/gtest.h>

#include <lucent/application/preview_cache.hpp>

namespace lucent::app {
namespace {

// A preview charged `pixel_bytes` + the fixed record allowance.
std::shared_ptr<const PreviewImage> preview(std::size_t pixel_bytes) {
    auto p = std::make_shared<PreviewImage>();
    p->image.rgb.resize(pixel_bytes);
    return p;
}

PreviewKey key(std::uint64_t item, std::uint64_t revision = 1,
               PreviewQuality quality = PreviewQuality::fit(256)) {
    return PreviewKey{ItemId{item}, revision, quality};
}

TEST(PreviewKey, QualityAndRevisionDistinguishEntries) {
    EXPECT_EQ(key(1), key(1));
    EXPECT_NE(key(1), key(2));
    EXPECT_NE(key(1, 1), key(1, 2));
    EXPECT_NE(key(1, 1, PreviewQuality::fit(256)), key(1, 1, PreviewQuality::fit(512)));
    EXPECT_NE(key(1, 1, PreviewQuality::fit(256)), key(1, 1, PreviewQuality::full()));
    // long_edge is irrelevant for Full.
    EXPECT_EQ(key(1, 1, PreviewQuality{PreviewQualityKind::Full, 5}), key(1, 1, PreviewQuality::full()));
    EXPECT_EQ(PreviewKeyHash{}(key(1, 1, PreviewQuality{PreviewQualityKind::Full, 5})),
              PreviewKeyHash{}(key(1, 1, PreviewQuality::full())));
    EXPECT_EQ(to_string(PreviewQuality::fit(2048)), "fit-2048");
    EXPECT_EQ(to_string(PreviewQuality::full()), "full");
    EXPECT_TRUE(PreviewQuality::fit(16).validate());
    EXPECT_FALSE(PreviewQuality::fit(15).validate());
    EXPECT_FALSE(PreviewQuality::fit(16385).validate());
    EXPECT_FALSE((PreviewQuality{static_cast<PreviewQualityKind>(9), 0}).validate());
}

TEST(PreviewCache, EvictsLeastRecentlyUsedByBytes) {
    const std::uint64_t entry = preview(100'000)->byte_size();
    PreviewCache cache(entry * 3);
    ASSERT_TRUE(cache.insert(key(1), preview(100'000)));
    ASSERT_TRUE(cache.insert(key(2), preview(100'000)));
    ASSERT_TRUE(cache.insert(key(3), preview(100'000)));
    EXPECT_TRUE(cache.find(key(1)));  // 1 becomes most recently used; 2 is now the oldest
    ASSERT_TRUE(cache.insert(key(4), preview(100'000)));
    EXPECT_FALSE(cache.find(key(2)));
    EXPECT_TRUE(cache.find(key(1)));
    EXPECT_TRUE(cache.find(key(3)));
    EXPECT_TRUE(cache.find(key(4)));

    const PreviewCacheStats stats = cache.stats();
    EXPECT_EQ(stats.entries, 3u);
    EXPECT_EQ(stats.bytes, entry * 3);
    EXPECT_LE(stats.bytes, stats.capacity_bytes);
    EXPECT_EQ(stats.evictions, 1u);
    EXPECT_EQ(stats.insertions, 4u);
    EXPECT_EQ(stats.hits, 4u);
    EXPECT_EQ(stats.misses, 1u);
}

TEST(PreviewCache, LargeEntriesEvictSeveralAndOversizedOnesAreRejected) {
    const std::uint64_t small = preview(10'000)->byte_size();
    PreviewCache cache(small * 4);
    for (std::uint64_t i = 1; i <= 4; ++i) {
        ASSERT_TRUE(cache.insert(key(i), preview(10'000)));
    }
    // An entry charged between two and three small ones evicts exactly the three oldest.
    const auto large = preview(static_cast<std::size_t>(small * 2));
    ASSERT_GT(large->byte_size(), 2 * small);
    ASSERT_LE(large->byte_size(), 3 * small);
    ASSERT_TRUE(cache.insert(key(9), large));
    EXPECT_EQ(cache.stats().entries, 2u);
    EXPECT_TRUE(cache.find(key(4)));
    EXPECT_FALSE(cache.find(key(1)));

    EXPECT_FALSE(cache.insert(key(10), preview(static_cast<std::size_t>(small * 5))));
    EXPECT_EQ(cache.stats().rejected, 1u);
    EXPECT_FALSE(cache.insert(key(11), nullptr));
}

TEST(PreviewCache, ReplacementInvalidationAndCapacityChanges) {
    PreviewCache cache(10'000'000);
    ASSERT_TRUE(cache.insert(key(1, 1), preview(1000)));
    ASSERT_TRUE(cache.insert(key(1, 2), preview(1000)));
    ASSERT_TRUE(cache.insert(key(2, 1), preview(1000)));
    const auto replacement = preview(5000);
    ASSERT_TRUE(cache.insert(key(2, 1), replacement));
    EXPECT_EQ(cache.find(key(2, 1)), replacement);
    EXPECT_EQ(cache.stats().entries, 3u);
    EXPECT_EQ(cache.stats().bytes, 2 * preview(1000)->byte_size() + replacement->byte_size());

    cache.erase_item(ItemId{1});
    EXPECT_EQ(cache.stats().entries, 1u);
    EXPECT_FALSE(cache.find(key(1, 1)));

    cache.set_capacity(1);
    EXPECT_EQ(cache.stats().entries, 0u);
    EXPECT_EQ(cache.stats().bytes, 0u);
    cache.set_capacity(10'000'000);
    ASSERT_TRUE(cache.insert(key(3), preview(1)));
    cache.clear();
    EXPECT_EQ(cache.stats().entries, 0u);
}

}  // namespace
}  // namespace lucent::app
