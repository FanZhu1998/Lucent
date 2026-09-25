#include "lucent/core/cancellation.hpp"

#include <atomic>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

namespace lucent::core {
namespace {

TEST(Cancellation, DefaultTokenNeverCancels) {
    const CancellationToken token;
    EXPECT_FALSE(token.can_be_cancelled());
    EXPECT_FALSE(token.is_cancelled());
    EXPECT_TRUE(token.check("core.test"));
}

TEST(Cancellation, SourceCancelsItsTokens) {
    CancellationSource source;
    const CancellationToken token = source.token();
    const CancellationToken copy = token;  // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_TRUE(token.can_be_cancelled());
    EXPECT_FALSE(source.is_cancelled());
    EXPECT_FALSE(token.is_cancelled());
    EXPECT_TRUE(token.check("core.test"));

    source.cancel();
    EXPECT_TRUE(source.is_cancelled());
    EXPECT_TRUE(token.is_cancelled());
    EXPECT_TRUE(copy.is_cancelled());

    const Status status = token.check("core.test.step");
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::Cancelled);
    EXPECT_EQ(status.error().where, "core.test.step");
}

TEST(Cancellation, CancelIsIdempotentAndSticky) {
    CancellationSource source;
    source.cancel();
    source.cancel();
    EXPECT_TRUE(source.is_cancelled());
    EXPECT_TRUE(source.token().is_cancelled());  // tokens created after cancel() see it too
}

TEST(Cancellation, CopiesAndMovesShareTheFlag) {
    CancellationSource source;
    const CancellationToken token = source.token();
    CancellationSource copy = source;
    CancellationSource moved = std::move(source);  // a move copies: `source` keeps its flag
    copy.cancel();
    EXPECT_TRUE(token.is_cancelled());
    EXPECT_TRUE(moved.is_cancelled());
    EXPECT_TRUE(source.is_cancelled());  // NOLINT(bugprone-use-after-move): documented behaviour
}

TEST(Cancellation, IndependentSourcesDoNotInterfere) {
    CancellationSource a;
    CancellationSource b;
    a.cancel();
    EXPECT_TRUE(a.token().is_cancelled());
    EXPECT_FALSE(b.token().is_cancelled());
}

TEST(Cancellation, TokenOutlivesItsSource) {
    CancellationToken token;
    {
        CancellationSource source;
        token = source.token();
        source.cancel();
    }
    EXPECT_TRUE(token.is_cancelled());
}

TEST(Cancellation, WritesBeforeCancelAreVisibleToObservers) {
    // Release/acquire: a worker that observes cancellation also observes data written before
    // cancel(). The outcome is deterministic; only the timing varies.
    CancellationSource source;
    const CancellationToken token = source.token();
    int reason = 0;  // plain, non-atomic data published by cancel()
    int observed = -1;
    std::thread worker([&] {
        while (!token.is_cancelled()) {
            std::this_thread::yield();
        }
        observed = reason;
    });
    reason = 42;
    source.cancel();
    worker.join();
    EXPECT_EQ(observed, 42);
}

}  // namespace
}  // namespace lucent::core
