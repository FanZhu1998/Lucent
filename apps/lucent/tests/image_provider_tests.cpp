// Image addresses, ImageReply, GenerationRegistry and the asynchronous image providers: stale
// generations are never delivered, finished() is always emitted exactly once and asynchronously,
// cancellation reaches the source.

#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <thread>

#include <QQuickImageResponse>
#include <QQuickTextureFactory>
#include <QSignalSpy>
#include <QTest>

#include "imaging/generation_registry.hpp"
#include "imaging/image_id.hpp"
#include "imaging/image_provider.hpp"
#include "test_support.hpp"

namespace lucent::ui {
namespace {

using testing::ManualImageSource;
using testing::solid_image;
using testing::SynchronousImageSource;

// Waits for finished(); the response is created by the calling thread, whose event loop delivers
// the provider's queued completion.
constexpr int kWaitMs = 5000;

TEST(ImageId, RoundTrips) {
    EXPECT_EQ(image_id(12, 3), QStringLiteral("12/3"));
    const auto key = parse_image_id(u"12/3");
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(*key, (ImageKey{12, 3}));

    constexpr ItemId kMax = std::numeric_limits<ItemId>::max();
    const auto max_key = parse_image_id(image_id(kMax, kMax));
    ASSERT_TRUE(max_key.has_value());
    EXPECT_EQ(*max_key, (ImageKey{kMax, kMax}));
}

TEST(ImageId, RejectsMalformedIds) {
    const char* const malformed[] = {"",     "12",   "12/",  "/3",   "a/3",   "12/b",  "0/3",
                                     "12/0", "012/3", "12/03", "+1/2", "1/2/3", " 1/2", "1/2 ",
                                     "-1/2", "18446744073709551616/1", "1/99999999999999999999"};
    for (const char* text : malformed) {
        EXPECT_FALSE(parse_image_id(QString::fromLatin1(text)).has_value()) << text;
    }
}

TEST(ImageId, UrlsNameTheProviderAndAreEmptyWithoutAnImage) {
    EXPECT_EQ(image_url(ImageKind::Thumbnail, 5, 2), QStringLiteral("image://thumbnail/5/2"));
    EXPECT_EQ(image_url(ImageKind::Preview, 5, 2), QStringLiteral("image://preview/5/2"));
    EXPECT_TRUE(image_url(ImageKind::Thumbnail, 5, kNoImage).isEmpty());
    EXPECT_TRUE(image_url(ImageKind::Thumbnail, 0, 1).isEmpty());
    EXPECT_TRUE(image_id(0, 1).isEmpty());
}

TEST(ImageReply, FirstCompletionWinsAndNotifiesOnce) {
    int notifications = 0;
    ImageReply reply([&notifications] { ++notifications; });
    EXPECT_EQ(reply.state(), ImageReply::State::Pending);
    EXPECT_TRUE(reply.finish(solid_image(QSize(4, 2))));
    EXPECT_FALSE(reply.fail(QStringLiteral("late")));
    EXPECT_FALSE(reply.cancel());
    EXPECT_EQ(reply.state(), ImageReply::State::Finished);
    EXPECT_EQ(reply.image().size(), QSize(4, 2));
    EXPECT_TRUE(reply.error().isEmpty());
    EXPECT_EQ(notifications, 1);
    // cancel() after completion still tells a source that the image is no longer wanted.
    EXPECT_TRUE(reply.is_cancelled());
}

TEST(ImageReply, NullImageIsAFailure) {
    ImageReply reply;
    EXPECT_TRUE(reply.finish(QImage()));
    EXPECT_EQ(reply.state(), ImageReply::State::Failed);
    EXPECT_FALSE(reply.error().isEmpty());
    EXPECT_TRUE(reply.image().isNull());
}

TEST(ImageReply, CancelCompletesAPendingReply) {
    int notifications = 0;
    ImageReply reply([&notifications] { ++notifications; });
    EXPECT_TRUE(reply.cancel());
    EXPECT_TRUE(reply.is_cancelled());
    EXPECT_EQ(reply.state(), ImageReply::State::Cancelled);
    EXPECT_FALSE(reply.finish(solid_image(QSize(2, 2))));
    EXPECT_EQ(notifications, 1);
}

TEST(ImageReply, CancelHandlerRunsOnceAfterTheReplyCompleted) {
    ImageReply reply;
    int calls = 0;
    ImageReply::State seen = ImageReply::State::Pending;
    reply.on_cancel([&] {
        ++calls;
        seen = reply.state();
    });
    EXPECT_TRUE(reply.cancel());
    EXPECT_FALSE(reply.cancel());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(seen, ImageReply::State::Cancelled);

    // Installed after cancellation: runs at once.
    int late = 0;
    reply.on_cancel([&] { ++late; });
    EXPECT_EQ(late, 1);
}

TEST(ImageReply, CancelHandlerIsNotRunWhenTheReplyFinishes) {
    ImageReply reply;
    int calls = 0;
    reply.on_cancel([&] { ++calls; });
    EXPECT_TRUE(reply.finish(testing::solid_image(QSize(4, 4))));
    EXPECT_EQ(calls, 0);
    // The consumer may still cancel a finished reply: the handler then learns it is unwanted.
    EXPECT_FALSE(reply.cancel());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(reply.state(), ImageReply::State::Finished);
}

TEST(ImageReply, DetachedListenerIsNeverCalled) {
    int notifications = 0;
    ImageReply reply([&notifications] { ++notifications; });
    reply.detach();
    EXPECT_TRUE(reply.fail(QStringLiteral("gone")));
    EXPECT_EQ(notifications, 0);
    EXPECT_EQ(reply.error(), QStringLiteral("gone"));
}

TEST(ImageReply, ConcurrentCompletionNotifiesExactlyOnce) {
    std::atomic<int> notifications{0};
    ImageReply reply([&notifications] { notifications.fetch_add(1); });
    std::vector<std::thread> threads;
    std::atomic<int> winners{0};
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&reply, &winners, i] {
            const bool won = i % 2 == 0 ? reply.finish(solid_image(QSize(1, 1))) : reply.fail(QStringLiteral("x"));
            if (won) {
                winners.fetch_add(1);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(winners.load(), 1);
    EXPECT_EQ(notifications.load(), 1);
}

TEST(GenerationRegistry, TracksCurrentGenerationsPerKind) {
    GenerationRegistry registry;
    EXPECT_FALSE(registry.current(1, ImageKind::Thumbnail).has_value());
    registry.set(1, 2, 5);
    EXPECT_EQ(registry.current(1, ImageKind::Thumbnail), Generation{2});
    EXPECT_EQ(registry.current(1, ImageKind::Preview), Generation{5});
    EXPECT_TRUE(registry.is_current(1, ImageKind::Thumbnail, 2));
    EXPECT_FALSE(registry.is_current(1, ImageKind::Thumbnail, 1));
    EXPECT_FALSE(registry.is_current(1, ImageKind::Preview, kNoImage));
    registry.set(1, 3, 5);
    EXPECT_FALSE(registry.is_current(1, ImageKind::Thumbnail, 2));
    registry.remove(1);
    EXPECT_FALSE(registry.current(1, ImageKind::Preview).has_value());
    registry.set(2, kNoImage, kNoImage);
    EXPECT_FALSE(registry.is_current(2, ImageKind::Thumbnail, kNoImage));
    registry.clear();
    EXPECT_FALSE(registry.current(2, ImageKind::Thumbnail).has_value());
}

TEST(ProviderFitWithin, ScalesDownPreservingAspectRatioAndNeverUp) {
    const QImage wide = solid_image(QSize(200, 100));
    EXPECT_EQ(fit_within(wide, QSize(50, 50)).size(), QSize(50, 25));
    EXPECT_EQ(fit_within(wide, QSize(40, 0)).size(), QSize(40, 20));
    EXPECT_EQ(fit_within(wide, QSize(-1, 10)).size(), QSize(20, 10));
    EXPECT_EQ(fit_within(wide, QSize(400, 400)).size(), QSize(200, 100));
    EXPECT_EQ(fit_within(wide, QSize()).size(), QSize(200, 100));
    EXPECT_EQ(fit_within(solid_image(QSize(1000, 1)), QSize(10, 10)).size(), QSize(10, 1));
    EXPECT_TRUE(fit_within(QImage(), QSize(10, 10)).isNull());
}

TEST(ProviderClampBoundingSize, LimitsEachRequestedSide) {
    EXPECT_EQ(clamp_bounding_size(QSize(100000, 20)), QSize(kMaxRequestedSide, 20));
    EXPECT_EQ(clamp_bounding_size(QSize(-1, 100000)), QSize(-1, kMaxRequestedSide));
    EXPECT_EQ(clamp_bounding_size(QSize(640, 480)), QSize(640, 480));
}

class AsyncImageProviderTest : public ::testing::Test {
protected:
    // The provider normally belongs to a QQmlEngine; here the test owns it.
    std::shared_ptr<ManualImageSource> source = std::make_shared<ManualImageSource>();
    std::shared_ptr<GenerationRegistry> registry = std::make_shared<GenerationRegistry>();
    AsyncImageProvider provider{ImageKind::Thumbnail, source, registry};

    struct Result {
        bool finished = false;
        QString error;
        QImage image;
    };

    // Waits for `response` to finish, reads its outcome and deletes it (Qt Quick's role).
    static Result finish(QQuickImageResponse* response, QSignalSpy& spy) {
        Result result;
        result.finished = spy.count() > 0 || spy.wait(kWaitMs);
        EXPECT_EQ(spy.count(), 1);
        result.error = response->errorString();
        if (QQuickTextureFactory* factory = response->textureFactory(); factory != nullptr) {
            result.image = factory->image();
            delete factory;
        }
        delete response;
        return result;
    }
};

TEST_F(AsyncImageProviderTest, DeliversTheCurrentGenerationScaledIntoTheRequestedBox) {
    registry->set(7, 2, 1);
    QQuickImageResponse* response = provider.requestImageResponse(QStringLiteral("7/2"), QSize(64, 64));
    QSignalSpy spy(response, &QQuickImageResponse::finished);

    ASSERT_EQ(source->count(), 1u);
    const auto pending = source->pending().front();
    EXPECT_EQ(pending.request.kind, ImageKind::Thumbnail);
    EXPECT_EQ(pending.request.item, ItemId{7});
    EXPECT_EQ(pending.request.generation, Generation{2});
    EXPECT_EQ(pending.request.bounding_size, QSize(64, 64));

    std::thread worker([reply = pending.reply] { reply->finish(solid_image(QSize(128, 64))); });
    worker.join();

    const Result result = finish(response, spy);
    EXPECT_TRUE(result.finished);
    EXPECT_TRUE(result.error.isEmpty()) << result.error.toStdString();
    EXPECT_EQ(result.image.size(), QSize(64, 32));
}

TEST_F(AsyncImageProviderTest, RejectsAStaleGenerationWithoutAskingTheSource) {
    registry->set(7, 3, 1);
    QQuickImageResponse* response = provider.requestImageResponse(QStringLiteral("7/2"), QSize(64, 64));
    QSignalSpy spy(response, &QQuickImageResponse::finished);
    const Result result = finish(response, spy);
    EXPECT_TRUE(result.finished);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_TRUE(result.image.isNull());
    EXPECT_EQ(source->count(), 0u);
}

TEST_F(AsyncImageProviderTest, DiscardsAResultSupersededWhileInFlight) {
    registry->set(7, 2, 1);
    QQuickImageResponse* response = provider.requestImageResponse(QStringLiteral("7/2"), QSize());
    QSignalSpy spy(response, &QQuickImageResponse::finished);
    ASSERT_EQ(source->count(), 1u);

    registry->set(7, 3, 1);  // e.g. Lucent's render replaced the camera preview meanwhile
    source->pending().front().reply->finish(solid_image(QSize(8, 8)));

    const Result result = finish(response, spy);
    EXPECT_TRUE(result.finished);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_TRUE(result.image.isNull());
}

TEST_F(AsyncImageProviderTest, RejectsUnknownItemsAndMalformedIds) {
    for (const QString& id : {QStringLiteral("99/1"), QStringLiteral("not-an-id"), QStringLiteral("7/0")}) {
        QQuickImageResponse* response = provider.requestImageResponse(id, QSize(16, 16));
        QSignalSpy spy(response, &QQuickImageResponse::finished);
        const Result result = finish(response, spy);
        EXPECT_TRUE(result.finished) << id.toStdString();
        EXPECT_FALSE(result.error.isEmpty()) << id.toStdString();
    }
    EXPECT_EQ(source->count(), 0u);
}

TEST_F(AsyncImageProviderTest, PassesTheSourceFailureReason) {
    registry->set(7, 1, 1);
    QQuickImageResponse* response = provider.requestImageResponse(QStringLiteral("7/1"), QSize(16, 16));
    QSignalSpy spy(response, &QQuickImageResponse::finished);
    source->pending().front().reply->fail(QStringLiteral("Unsupported capture mode"));
    const Result result = finish(response, spy);
    EXPECT_EQ(result.error, QStringLiteral("Unsupported capture mode"));
}

TEST_F(AsyncImageProviderTest, CancelStillFinishesAndReachesTheSource) {
    registry->set(7, 1, 1);
    QQuickImageResponse* response = provider.requestImageResponse(QStringLiteral("7/1"), QSize(16, 16));
    QSignalSpy spy(response, &QQuickImageResponse::finished);
    const auto reply = source->pending().front().reply;
    response->cancel();
    EXPECT_TRUE(reply->is_cancelled());
    EXPECT_FALSE(reply->finish(solid_image(QSize(4, 4))));  // too late: the outcome is decided
    const Result result = finish(response, spy);
    EXPECT_TRUE(result.finished);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_TRUE(result.image.isNull());
}

TEST_F(AsyncImageProviderTest, ClampsHugeRequestedSizes) {
    registry->set(7, 1, 1);
    QQuickImageResponse* response = provider.requestImageResponse(QStringLiteral("7/1"), QSize(100000, -1));
    QSignalSpy spy(response, &QQuickImageResponse::finished);
    EXPECT_EQ(source->pending().front().request.bounding_size, QSize(kMaxRequestedSide, -1));
    source->pending().front().reply->finish(solid_image(QSize(4, 4)));
    EXPECT_TRUE(finish(response, spy).error.isEmpty());
}

TEST_F(AsyncImageProviderTest, ASourceCompletingAfterTheResponseIsDeletedIsHarmless) {
    registry->set(7, 1, 1);
    QQuickImageResponse* response = provider.requestImageResponse(QStringLiteral("7/1"), QSize(16, 16));
    const auto reply = source->pending().front().reply;
    delete response;
    std::thread worker([reply] { reply->finish(solid_image(QSize(4, 4))); });
    worker.join();
    QTest::qWait(20);  // any queued call for the deleted response must have been dropped
    EXPECT_EQ(reply->state(), ImageReply::State::Finished);
}

TEST(AsyncImageProvider, NeverFinishesInsideRequestImageResponse) {
    auto registry = std::make_shared<GenerationRegistry>();
    registry->set(3, 1, 1);
    AsyncImageProvider provider(ImageKind::Preview, std::make_shared<SynchronousImageSource>(QSize(20, 10)),
                                registry);
    QQuickImageResponse* response = provider.requestImageResponse(QStringLiteral("3/1"), QSize(10, 10));
    QSignalSpy spy(response, &QQuickImageResponse::finished);
    EXPECT_EQ(spy.count(), 0);  // the source completed synchronously; finished() is still queued
    ASSERT_TRUE(spy.wait(kWaitMs));
    EXPECT_TRUE(response->errorString().isEmpty());
    QQuickTextureFactory* factory = response->textureFactory();
    ASSERT_NE(factory, nullptr);
    EXPECT_EQ(factory->image().size(), QSize(10, 5));
    delete factory;
    delete response;
}

}  // namespace
}  // namespace lucent::ui
