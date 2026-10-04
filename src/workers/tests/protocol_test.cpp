// Protocol "lucent.worker" v1: encoding, strict decoding, limits, framing, version negotiation and
// build identity (protocol.hpp).

#include <array>
#include <cstddef>
#include <cstring>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <lucent/core/text.hpp>
#include <lucent/workers/protocol.hpp>

#include "workers_test_support.hpp"

namespace lucent::workers {
namespace {

using Json = nlohmann::json;
using test_support::digest_of;
using test_support::sample_job;
using test_support::sample_result;

std::string encode_ok(const Message& message) {
    auto text = encode_message(message);
    EXPECT_TRUE(text) << (text ? "" : text.error().describe());
    return text ? *text : std::string{};
}

// encode(decode(encode(m))) == encode(m): every member survives the round trip.
void expect_round_trip(const Message& message) {
    const std::string first = encode_ok(message);
    auto decoded = decode_message(first, direction_of(message));
    ASSERT_TRUE(decoded) << decoded.error().describe() << "\n" << first;
    EXPECT_EQ(decoded->index(), message.index());
    EXPECT_EQ(encode_ok(*decoded), first);
}

Error decode_error(const std::string& text, Direction direction) {
    auto decoded = decode_message(text, direction);
    EXPECT_FALSE(decoded) << "accepted: " << text;
    return decoded ? Error{} : decoded.error();
}

// The encoded job as JSON, for mutation tests.
Json job_json() { return Json::parse(encode_ok(sample_job())); }

void expect_corrupt(const Json& document, Direction direction = Direction::HostToWorker) {
    const Error error = decode_error(document.dump(), direction);
    EXPECT_EQ(error.code, ErrorCode::Corrupt) << error.describe();
    EXPECT_TRUE(error.where.starts_with("workers.protocol")) << error.where;
}

TEST(WorkerProtocol, EveryMessageTypeRoundTrips) {
    HostHello hello;
    hello.build = current_build_identity();
    hello.heartbeat_ms = 250;
    hello.host_pid = 4242;
    expect_round_trip(hello);

    WorkerHello answer;
    answer.build = current_build_identity();
    answer.pid = 77;
    answer.console_window = true;
    answer.test_hooks = {"hang-job", "allocate:1024"};
    expect_round_trip(answer);

    JobRequest job = sample_job();
    job.source.sha256 = digest_of("content");
    job.output_settings = exporter::ExportSettings::tiff16(exporter::OutputProfile::DisplayP3);
    job.output_settings.transform.intent = exporter::RenderingIntent::Perceptual;
    job.output_settings.transform.black_point_compensation = true;
    job.output_settings.metadata.include_gps = true;
    job.output_settings.metadata.include_software = false;
    job.artist = "Ånne Fotograf";
    job.copyright = "";
    job.allow_unsupported = true;
    expect_round_trip(job);
    expect_round_trip(sample_job());  // null sha256 and rights

    expect_round_trip(CancelJob{"job-1"});
    expect_round_trip(Ping{0xFFFF'FFFF'FFFF'FFFFull});
    expect_round_trip(Shutdown{});
    expect_round_trip(JobProgress{"job-1", JobStage::Encoding, 0.725});
    expect_round_trip(Heartbeat{9, std::nullopt, MemorySnapshot{1, 2, 3, 4}});
    expect_round_trip(Heartbeat{10, std::string("job-1"), MemorySnapshot{}});
    expect_round_trip(sample_result());
    JobResult jpeg = sample_result();
    jpeg.output.format = exporter::ExportFormat::Jpeg;
    jpeg.output.bit_depth = exporter::BitDepth::Eight;
    jpeg.output.chroma_subsampling = exporter::ChromaSubsampling::Yuv420;
    jpeg.source_sha256.reset();
    expect_round_trip(jpeg);
    expect_round_trip(JobError{std::string("job-1"),
                               Error{ErrorCode::SourceChanged, "changed", "worker.job.source", -5},
                               FailureClass::Deterministic});
    expect_round_trip(JobError{std::nullopt, Error{ErrorCode::VersionMismatch, "no", "workers.handshake.build", 0},
                               FailureClass::Deterministic});
    expect_round_trip(Pong{12});
}

TEST(WorkerProtocol, DecodedJobHasTheSentValues) {
    JobRequest job = sample_job("a.B_c:9-z");
    job.source.subimage = 3;
    job.source.sha256 = digest_of("content");
    job.output_settings.jpeg.quality = 80;
    job.output_settings.metadata.include_gps = true;
    job.artist = "Ånne";
    auto decoded = decode_message(encode_ok(job), Direction::HostToWorker);
    ASSERT_TRUE(decoded) << decoded.error().describe();
    const auto& got = std::get<JobRequest>(*decoded);
    EXPECT_EQ(got.job_id, job.job_id);
    EXPECT_EQ(got.source.path, job.source.path);
    EXPECT_EQ(got.source.subimage, 3u);
    EXPECT_EQ(got.source.identity, job.source.identity);
    EXPECT_EQ(got.source.sha256, job.source.sha256);
    EXPECT_EQ(got.recipe.document, job.recipe.document);
    EXPECT_EQ(got.recipe.sha256, job.recipe.sha256);
    EXPECT_EQ(got.output_settings.jpeg.quality, 80);
    EXPECT_TRUE(got.output_settings.metadata.include_gps);
    EXPECT_EQ(got.artist, job.artist);
    EXPECT_FALSE(got.copyright.has_value());
    EXPECT_EQ(got.temp_path, job.temp_path);
    EXPECT_EQ(got.memory_budget_bytes, job.memory_budget_bytes);
}

TEST(WorkerProtocol, IdentityAndDigestsTravelAsLowercaseHex) {
    const Json job = job_json();
    EXPECT_EQ(job["source"]["identity"]["volume_serial"], "0123456789abcdef");
    EXPECT_EQ(job["source"]["identity"]["file_id"], "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    EXPECT_EQ(job["recipe"]["sha256"], core::to_hex(sample_job().recipe.sha256));
    EXPECT_TRUE(job["source"]["sha256"].is_null());

    Json upper = job;
    upper["source"]["identity"]["volume_serial"] = "0123456789ABCDEF";
    expect_corrupt(upper);
    Json short_id = job;
    short_id["source"]["identity"]["file_id"] = "f0f1";
    expect_corrupt(short_id);
}

TEST(WorkerProtocol, StrictDecodingRefusesMalformedDocuments) {
    const Json job = job_json();

    Json unknown = job;
    unknown["priority"] = 1;
    expect_corrupt(unknown);
    Json nested_unknown = job;
    nested_unknown["source"]["note"] = "x";
    expect_corrupt(nested_unknown);
    Json missing = job;
    missing.erase("temp_path");
    expect_corrupt(missing);
    Json wrong_type = job;
    wrong_type["source"]["subimage"] = "0";
    expect_corrupt(wrong_type);
    Json fraction_integer = job;
    fraction_integer["memory_budget_bytes"] = 1.5;
    expect_corrupt(fraction_integer);
    Json negative = job;
    negative["memory_budget_bytes"] = -1;
    expect_corrupt(negative);
    Json unknown_enum = job;
    unknown_enum["export"]["format"] = 9;
    expect_corrupt(unknown_enum);
    Json bad_quality = job;
    bad_quality["export"]["jpeg_quality"] = 0;
    expect_corrupt(bad_quality);
    Json bad_id = job;
    bad_id["job_id"] = "job 1";
    expect_corrupt(bad_id);
    Json relative = job;
    relative["temp_path"] = R"(Exports\out.partial)";
    expect_corrupt(relative);
    Json no_file_name = job;
    no_file_name["temp_path"] = R"(D:\)";
    expect_corrupt(no_file_name);
    Json source_as_output = job;
    source_as_output["temp_path"] = R"(c:\photos\ÜNÏCODE ✓\img_0001.cr3)";  // same file, other case
    expect_corrupt(source_as_output);
    Json tampered = job;
    tampered["recipe"]["document"] = R"({"schema":"lucent.develop","version":2})";
    expect_corrupt(tampered);
    Json unknown_type = job;
    unknown_type["type"] = "render";
    expect_corrupt(unknown_type);

    // Repeated members are refused even though nlohmann would keep the last one.
    std::string repeated = job.dump();
    repeated.insert(1, R"("job_id":"other",)");
    EXPECT_EQ(decode_error(repeated, Direction::HostToWorker).code, ErrorCode::Corrupt);

    EXPECT_EQ(decode_error("[]", Direction::HostToWorker).code, ErrorCode::Corrupt);
    EXPECT_EQ(decode_error("{", Direction::HostToWorker).code, ErrorCode::Corrupt);
    EXPECT_EQ(decode_error("", Direction::HostToWorker).code, ErrorCode::Corrupt);
    EXPECT_EQ(decode_error(R"({"type":"ping","nonce":1} {})", Direction::HostToWorker).code, ErrorCode::Corrupt);
    // Not UTF-8.
    EXPECT_EQ(decode_error("{\"type\":\"cancel\",\"job_id\":\"\xff\"}", Direction::HostToWorker).code,
              ErrorCode::Corrupt);
    // Too deep.
    EXPECT_EQ(decode_error(R"({"type":"ping","nonce":[[[[[[[[[[1]]]]]]]]]]})", Direction::HostToWorker).code,
              ErrorCode::Corrupt);
}

TEST(WorkerProtocol, MessagesOfTheOtherDirectionAreRefused) {
    const std::string job = encode_ok(sample_job());
    EXPECT_EQ(decode_error(job, Direction::WorkerToHost).code, ErrorCode::Corrupt);
    const std::string result = encode_ok(sample_result());
    EXPECT_EQ(decode_error(result, Direction::HostToWorker).code, ErrorCode::Corrupt);
    EXPECT_EQ(direction_of(Message{Pong{}}), Direction::WorkerToHost);
    EXPECT_EQ(direction_of(Message{Shutdown{}}), Direction::HostToWorker);
    EXPECT_EQ(type_name(Message{CancelJob{}}), "cancel");
}

TEST(WorkerProtocol, ResultAndProgressValuesAreRangeChecked) {
    Json progress = Json::parse(encode_ok(JobProgress{"job-1", JobStage::Rendering, 0.5}));
    progress["fraction"] = 1.5;
    expect_corrupt(progress, Direction::WorkerToHost);
    progress["fraction"] = 0.5;
    progress["stage"] = 6;
    expect_corrupt(progress, Direction::WorkerToHost);

    Json error = Json::parse(encode_ok(JobError{std::string("j"), Error{ErrorCode::Corrupt, "m", "w", 0},
                                                FailureClass::Deterministic}));
    error["code"] = 17;  // not an ErrorCode of this build
    expect_corrupt(error, Direction::WorkerToHost);
    error["code"] = 8;
    error["class"] = 4;
    expect_corrupt(error, Direction::WorkerToHost);

    Json result = Json::parse(encode_ok(sample_result()));
    result["output"]["width"] = 0;
    expect_corrupt(result, Direction::WorkerToHost);
    result = Json::parse(encode_ok(sample_result()));
    result["render"]["interpretation"] = 6;
    expect_corrupt(result, Direction::WorkerToHost);
    result = Json::parse(encode_ok(sample_result()));
    result["render"]["warnings"] = Json::array({1});
    expect_corrupt(result, Direction::WorkerToHost);
}

TEST(WorkerProtocol, EncodingValidatesRequests) {
    const auto expect_invalid = [](const Message& message) {
        auto text = encode_message(message);
        ASSERT_FALSE(text);
        EXPECT_EQ(text.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(text.error().where, "workers.protocol.encode");
    };
    JobRequest job = sample_job();
    job.job_id = "";
    expect_invalid(job);
    job = sample_job();
    job.job_id = std::string(65, 'a');
    expect_invalid(job);
    job = sample_job();
    job.source.path = "relative.CR3";
    expect_invalid(job);
    job = sample_job();
    job.temp_path = job.source.path;
    expect_invalid(job);
    job = sample_job();
    job.recipe.sha256[0] ^= 1;
    expect_invalid(job);
    job = sample_job();
    job.output_settings.jpeg.quality = 101;
    expect_invalid(job);
    job = sample_job();
    job.recipe.document.assign(kMaxRecipeBytes + 1, ' ');
    job.recipe.sha256 = digest_of(job.recipe.document);
    expect_invalid(job);
    job = sample_job();
    job.artist = std::string(exporter::kMaxMetadataTextBytes + 1, 'a');
    expect_invalid(job);
    job = sample_job();
    job.source.path = std::wstring(LR"(C:\bad)") + wchar_t(0xD800) + L".CR3";  // unpaired surrogate
    expect_invalid(job);
    expect_invalid(JobProgress{"job-1", JobStage::Rendering, 2.0});
    HostHello hello;
    hello.min_version = 2;
    hello.max_version = 1;
    expect_invalid(hello);
}

TEST(WorkerProtocol, LongErrorTextIsTruncatedAtACharacterBoundary) {
    std::string message;
    while (message.size() < 3 * kMaxErrorMessageBytes) {
        message += "é";  // two bytes
    }
    message += "x";
    const JobError error{std::string("job-1"), Error{ErrorCode::IoError, message, "w", 0}, FailureClass::Transient};
    auto decoded = decode_message(encode_ok(error), Direction::WorkerToHost);
    ASSERT_TRUE(decoded) << decoded.error().describe();
    const std::string& got = std::get<JobError>(*decoded).error.message;
    EXPECT_LE(got.size(), kMaxErrorMessageBytes);
    EXPECT_GE(got.size(), kMaxErrorMessageBytes - 1);
    EXPECT_TRUE(core::is_valid_utf8(got));

    EXPECT_EQ(truncate_utf8("aé", 2), "a");
    EXPECT_EQ(truncate_utf8("aé", 3), "aé");
    EXPECT_EQ(truncate_utf8("✓✓", 4), "✓");
}

TEST(WorkerProtocol, FramesCarryALittleEndianLength) {
    auto frame = encode_frame(Ping{7});
    ASSERT_TRUE(frame);
    const std::string json = encode_ok(Ping{7});
    ASSERT_EQ(frame->size(), kFrameHeaderBytes + json.size());
    std::array<std::byte, kFrameHeaderBytes> header{};
    std::memcpy(header.data(), frame->data(), header.size());
    auto length = decode_frame_length(header);
    ASSERT_TRUE(length);
    EXPECT_EQ(*length, json.size());
    EXPECT_EQ(frame->substr(kFrameHeaderBytes), json);

    const auto header_of = [](std::uint32_t value) {
        std::array<std::byte, kFrameHeaderBytes> h{};
        for (std::size_t i = 0; i < h.size(); ++i) {
            h[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
        }
        return h;
    };
    EXPECT_FALSE(decode_frame_length(header_of(0)));
    EXPECT_TRUE(decode_frame_length(header_of(kMaxMessageBytes)));
    auto too_long = decode_frame_length(header_of(kMaxMessageBytes + 1));
    ASSERT_FALSE(too_long);
    EXPECT_EQ(too_long.error().code, ErrorCode::Corrupt);
    EXPECT_FALSE(decode_frame_length(header_of(0xFFFFFFFFu)));
}

TEST(WorkerProtocol, OversizedMessagesAreRefused) {
    EXPECT_FALSE(decode_message(std::string(kMaxMessageBytes + 1, ' '), Direction::HostToWorker));
    // A result whose warnings would exceed the frame is cut to kMaxWarnings while encoding.
    JobResult result = sample_result();
    result.render.warnings.assign(kMaxWarnings * 4, std::string(kMaxWarningBytes * 2, 'w'));
    auto decoded = decode_message(encode_ok(result), Direction::WorkerToHost);
    ASSERT_TRUE(decoded) << decoded.error().describe();
    const auto& warnings = std::get<JobResult>(*decoded).render.warnings;
    EXPECT_EQ(warnings.size(), kMaxWarnings);
    EXPECT_EQ(warnings.front().size(), kMaxWarningBytes);
}

TEST(WorkerProtocol, VersionNegotiationPicksTheHighestCommonVersion) {
    auto both = negotiate_version(1, 3, 2, 5);
    ASSERT_TRUE(both);
    EXPECT_EQ(*both, 3u);
    auto same = negotiate_version(1, 1, 1, 1);
    ASSERT_TRUE(same);
    EXPECT_EQ(*same, 1u);
    auto disjoint = negotiate_version(1, 1, 2, 3);
    ASSERT_FALSE(disjoint);
    EXPECT_EQ(disjoint.error().code, ErrorCode::VersionMismatch);
    EXPECT_EQ(disjoint.error().where, "workers.handshake.version");
    EXPECT_EQ(negotiate_version(2, 1, 1, 1).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(negotiate_version(0, 1, 1, 1).error().code, ErrorCode::InvalidArgument);
}

TEST(WorkerProtocol, BuildsMustBeIdentical) {
    const BuildIdentity self = current_build_identity();
    EXPECT_FALSE(self.version.empty());
    EXPECT_TRUE(check_same_build(self, self, "worker"));
    BuildIdentity other = self;
    other.git_commit = "0000000";
    auto mismatch = check_same_build(self, other, "worker");
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().code, ErrorCode::VersionMismatch);
    EXPECT_EQ(mismatch.error().where, "workers.handshake.build");
    EXPECT_NE(mismatch.error().message.find("0000000"), std::string::npos);
    EXPECT_NE(mismatch.error().message.find(self.git_commit), std::string::npos);
}

TEST(WorkerProtocol, FailureClassesFollowTheErrorCode) {
    const auto class_of = [](ErrorCode code) { return default_failure_class(Error{code, "", "", 0}); };
    EXPECT_EQ(class_of(ErrorCode::Cancelled), FailureClass::Cancelled);
    EXPECT_EQ(class_of(ErrorCode::IoError), FailureClass::Transient);
    EXPECT_EQ(class_of(ErrorCode::Timeout), FailureClass::Transient);
    EXPECT_EQ(class_of(ErrorCode::OutOfMemory), FailureClass::Transient);
    for (ErrorCode code : {ErrorCode::Corrupt, ErrorCode::Unsupported, ErrorCode::SourceChanged, ErrorCode::NotFound,
                           ErrorCode::DiskFull, ErrorCode::PermissionDenied, ErrorCode::LimitExceeded,
                           ErrorCode::VersionMismatch, ErrorCode::InvalidArgument, ErrorCode::Internal}) {
        EXPECT_EQ(class_of(code), FailureClass::Deterministic) << to_string(code);
    }
    EXPECT_EQ(to_string(FailureClass::Transient), "transient");
    EXPECT_EQ(to_string(JobStage::Verifying), "verifying");
}

TEST(WorkerProtocol, JobIdsAreShortPortableTokens) {
    EXPECT_TRUE(is_valid_job_id("job-12.attempt_3:x"));
    EXPECT_TRUE(is_valid_job_id(std::string(kMaxJobIdBytes, 'a')));
    EXPECT_FALSE(is_valid_job_id(""));
    EXPECT_FALSE(is_valid_job_id(std::string(kMaxJobIdBytes + 1, 'a')));
    EXPECT_FALSE(is_valid_job_id("a b"));
    EXPECT_FALSE(is_valid_job_id("a/b"));
    EXPECT_FALSE(is_valid_job_id("é"));
}

}  // namespace
}  // namespace lucent::workers
