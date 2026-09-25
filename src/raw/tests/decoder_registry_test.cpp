// Unit tests for DecoderRegistry with scripted fake decoders.

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/raw/decoder_registry.hpp>
#include <lucent/raw/libraw_decoder.hpp>

namespace lucent::raw {
namespace {

// A decoder whose probe answers with a fixed error code (or success) and counts its calls.
class FakeDecoder final : public RawDecoder {
public:
    FakeDecoder(std::string id, std::optional<ErrorCode> probe_error, std::vector<std::string>* log)
        : id_(std::move(id)), probe_error_(probe_error), log_(log) {}

    std::string_view id() const noexcept override { return id_; }

    Expected<ProbeResult> probe(const std::filesystem::path&, const ProbeOptions& options,
                                core::CancellationToken) const override {
        if (log_ != nullptr) {
            log_->push_back("probe:" + id_);
        }
        if (probe_error_) {
            return fail(*probe_error_, "scripted failure of " + id_, "raw.fake.probe");
        }
        ProbeResult result;
        result.metadata.decoder.adapter_id = id_;
        result.metadata.subimage_index = options.subimage.value_or(0);
        return result;
    }

    Expected<DecodedRaw> decode(const std::filesystem::path&, const DecodeOptions& options,
                                core::CancellationToken) const override {
        if (log_ != nullptr) {
            log_->push_back("decode:" + id_ + ":" + std::to_string(options.subimage.value_or(99)));
        }
        return fail(ErrorCode::Unsupported, "fake decoders do not decode", "raw.fake.decode");
    }

private:
    std::string id_;
    std::optional<ErrorCode> probe_error_;
    std::vector<std::string>* log_;
};

std::unique_ptr<RawDecoder> fake(std::string id, std::optional<ErrorCode> error = std::nullopt,
                                 std::vector<std::string>* log = nullptr) {
    return std::make_unique<FakeDecoder>(std::move(id), error, log);
}

TEST(DecoderRegistry, OrdersByPriorityThenRegistration) {
    DecoderRegistry registry;
    ASSERT_TRUE(registry.add(fake("c"), 20));
    ASSERT_TRUE(registry.add(fake("a"), 10));
    ASSERT_TRUE(registry.add(fake("b"), 20));
    ASSERT_TRUE(registry.add(fake("z"), -5));
    std::vector<std::string> ids;
    for (const RawDecoder* d : registry.decoders()) {
        ids.emplace_back(d->id());
    }
    EXPECT_EQ(ids, (std::vector<std::string>{"z", "a", "c", "b"}));
    EXPECT_EQ(registry.size(), 4u);
    ASSERT_NE(registry.find("b"), nullptr);
    EXPECT_EQ(registry.find("b")->id(), "b");
    EXPECT_EQ(registry.find("missing"), nullptr);
}

TEST(DecoderRegistry, RejectsInvalidRegistrations) {
    DecoderRegistry registry;
    auto null_status = registry.add(nullptr, 0);
    ASSERT_FALSE(null_status);
    EXPECT_EQ(null_status.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(null_status.error().where, "raw.registry.add");

    for (const char* bad : {"", "LibRaw", "has space", "slash/id"}) {
        auto status = registry.add(fake(bad), 0);
        ASSERT_FALSE(status) << bad;
        EXPECT_EQ(status.error().code, ErrorCode::InvalidArgument) << bad;
    }
    ASSERT_TRUE(registry.add(fake("libraw-2.x_y"), 0));
    auto duplicate = registry.add(fake("libraw-2.x_y"), 5);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(registry.size(), 1u);
}

TEST(DecoderRegistry, SelectsFirstAdapterThatRecognisesTheFile) {
    std::vector<std::string> log;
    DecoderRegistry registry;
    ASSERT_TRUE(registry.add(fake("first", ErrorCode::Unsupported, &log), 1));
    ASSERT_TRUE(registry.add(fake("second", std::nullopt, &log), 2));
    ASSERT_TRUE(registry.add(fake("third", std::nullopt, &log), 3));

    ProbeOptions options;
    options.subimage = 1;
    auto selection = registry.select("photo.raw", options, {});
    ASSERT_TRUE(selection) << selection.error().describe();
    EXPECT_EQ(selection->decoder->id(), "second");
    EXPECT_EQ(selection->probe.metadata.decoder.adapter_id, "second");
    EXPECT_EQ(selection->probe.metadata.subimage_index, 1u);
    EXPECT_EQ(log, (std::vector<std::string>{"probe:first", "probe:second"}));
}

TEST(DecoderRegistry, StopsAtFirstErrorThatIsNotUnsupported) {
    for (const ErrorCode code : {ErrorCode::Corrupt, ErrorCode::NotFound, ErrorCode::LimitExceeded,
                                 ErrorCode::SourceChanged, ErrorCode::IoError}) {
        std::vector<std::string> log;
        DecoderRegistry registry;
        ASSERT_TRUE(registry.add(fake("broken", code, &log), 1));
        ASSERT_TRUE(registry.add(fake("fallback", std::nullopt, &log), 2));
        auto selection = registry.select("photo.raw", {}, {});
        ASSERT_FALSE(selection);
        EXPECT_EQ(selection.error().code, code);
        EXPECT_EQ(selection.error().where, "raw.fake.probe");
        EXPECT_EQ(log, (std::vector<std::string>{"probe:broken"}));
    }
}

TEST(DecoderRegistry, UnsupportedWhenNoAdapterRecognisesTheFile) {
    DecoderRegistry registry;
    ASSERT_TRUE(registry.add(fake("one", ErrorCode::Unsupported), 1));
    ASSERT_TRUE(registry.add(fake("two", ErrorCode::Unsupported), 2));
    auto selection = registry.select("photo.raw", {}, {});
    ASSERT_FALSE(selection);
    EXPECT_EQ(selection.error().code, ErrorCode::Unsupported);
    EXPECT_EQ(selection.error().where, "raw.registry.select");
    EXPECT_NE(selection.error().message.find("one: scripted failure of one"), std::string::npos);
    EXPECT_NE(selection.error().message.find("two: scripted failure of two"), std::string::npos);

    DecoderRegistry empty;
    auto none = empty.select("photo.raw", {}, {});
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().code, ErrorCode::Unsupported);
}

TEST(DecoderRegistry, CancelledSelectionProbesNothing) {
    std::vector<std::string> log;
    DecoderRegistry registry;
    ASSERT_TRUE(registry.add(fake("only", std::nullopt, &log), 1));
    core::CancellationSource source;
    source.cancel();
    auto selection = registry.select("photo.raw", {}, source.token());
    ASSERT_FALSE(selection);
    EXPECT_EQ(selection.error().code, ErrorCode::Cancelled);
    EXPECT_TRUE(log.empty());
}

TEST(DecoderRegistry, DecodeUsesSelectedAdapterWithTheRequestedSubimage) {
    std::vector<std::string> log;
    DecoderRegistry registry;
    ASSERT_TRUE(registry.add(fake("skip", ErrorCode::Unsupported, &log), 1));
    ASSERT_TRUE(registry.add(fake("take", std::nullopt, &log), 2));
    DecodeOptions options;
    options.subimage = 3;
    auto decoded = registry.decode("photo.raw", options, {});
    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().where, "raw.fake.decode");
    EXPECT_EQ(log, (std::vector<std::string>{"probe:skip", "probe:take", "decode:take:3"}));
}

TEST(DecoderRegistry, DefaultRegistryHoldsTheLibRawAdapter) {
    auto registry = create_default_registry();
    ASSERT_TRUE(registry) << registry.error().describe();
    ASSERT_EQ(registry->size(), 1u);
    const RawDecoder* libraw = registry->find(kLibRawDecoderId);
    ASSERT_NE(libraw, nullptr);
    EXPECT_EQ(libraw->id(), "libraw");
    // The registry is movable, and the adapter pointer stays valid across the move.
    DecoderRegistry moved = std::move(*registry);
    EXPECT_EQ(moved.find("libraw"), libraw);
}

}  // namespace
}  // namespace lucent::raw
