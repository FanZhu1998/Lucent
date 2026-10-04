// The worker command line and its test hooks (worker_launch.hpp).

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/workers/worker_launch.hpp>

namespace lucent::workers {
namespace {

std::vector<std::wstring> split(const std::wstring& text) {
    std::vector<std::wstring> parts;
    std::wstring current;
    for (wchar_t c : text) {
        if (c == L' ') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

TEST(WorkerLaunch, ArgumentsRoundTrip) {
    WorkerLaunch launch;
    launch.request_handle = 0x1A4;
    launch.response_handle = 0x1B8;
    launch.parent_handle = 0x2C0;
    for (const char* spec : {"hang-job", "freeze", "crash-job", "allocate:1048576", "render-delay:250",
                             "fake-commit:deadbee", "protocol:2-3"}) {
        ASSERT_TRUE(apply_test_hook(spec, launch.hooks)) << spec;
    }
    auto arguments = worker_arguments(launch);
    ASSERT_TRUE(arguments);
    auto parsed = parse_worker_arguments(split(*arguments));
    ASSERT_TRUE(parsed) << parsed.error().describe();
    EXPECT_EQ(parsed->request_handle, launch.request_handle);
    EXPECT_EQ(parsed->response_handle, launch.response_handle);
    EXPECT_EQ(parsed->parent_handle, launch.parent_handle);
    EXPECT_EQ(parsed->hooks.specs(), launch.hooks.specs());
    EXPECT_TRUE(parsed->hooks.hang_job && parsed->hooks.freeze && parsed->hooks.crash_job);
    EXPECT_EQ(parsed->hooks.allocate_bytes, 1048576u);
    EXPECT_EQ(parsed->hooks.render_delay_ms, 250u);
    EXPECT_EQ(parsed->hooks.fake_commit, "deadbee");
    EXPECT_EQ(parsed->hooks.protocol_range, std::pair(2u, 3u));
}

TEST(WorkerLaunch, ProductionLaunchHasNoHooksAndAnOptionalParent) {
    WorkerLaunch launch;
    launch.request_handle = 4;
    launch.response_handle = 8;
    auto arguments = worker_arguments(launch);
    ASSERT_TRUE(arguments);
    EXPECT_EQ(*arguments, L"--lucent-worker --request-handle=4 --response-handle=8");
    auto parsed = parse_worker_arguments(split(*arguments));
    ASSERT_TRUE(parsed);
    EXPECT_FALSE(parsed->hooks.any());
    EXPECT_TRUE(parsed->hooks.specs().empty());
    EXPECT_EQ(parsed->parent_handle, 0u);

    launch.response_handle = 0;
    EXPECT_FALSE(worker_arguments(launch));
}

TEST(WorkerLaunch, AnythingElseIsAUsageError) {
    const std::vector<std::vector<std::wstring>> invalid = {
        {},
        {L"--request-handle=4", L"--response-handle=8"},  // no mode argument
        {L"--lucent-worker"},
        {L"--lucent-worker", L"--request-handle=4"},
        {L"--lucent-worker", L"--request-handle=0", L"--response-handle=8"},
        {L"--lucent-worker", L"--request-handle=-4", L"--response-handle=8"},
        {L"--lucent-worker", L"--request-handle=4x", L"--response-handle=8"},
        {L"--lucent-worker", L"--request-handle=4", L"--request-handle=5", L"--response-handle=8"},
        {L"--lucent-worker", L"--request-handle=4", L"--response-handle=8", L"--verbose"},
        {L"--lucent-worker", L"--request-handle=4", L"--response-handle=8", L"--test-hook=explode"},
        {L"--lucent-worker", L"--request-handle=4", L"--response-handle=8", L"--parent-handle="},
        {L"--lucent-worker", L"--request-handle=99999999999999999999999", L"--response-handle=8"},
    };
    for (const auto& arguments : invalid) {
        auto parsed = parse_worker_arguments(arguments);
        EXPECT_FALSE(parsed) << arguments.size();
        if (!parsed) {
            EXPECT_EQ(parsed.error().code, ErrorCode::InvalidArgument);
        }
    }
}

TEST(WorkerLaunch, TestHookSpecsAreValidated) {
    for (const char* spec : {"", "hang", "hang-job:1", "allocate", "allocate:", "allocate:0", "allocate:x",
                             "allocate:1099511627777", "render-delay:0", "render-delay:600001", "fake-commit:",
                             "fake-commit:a b", "fake-commit:\"x\"", "protocol:0-1", "protocol:2-1", "protocol:1",
                             "protocol:1-", "freeze:yes"}) {
        TestHooks hooks;
        auto applied = apply_test_hook(spec, hooks);
        EXPECT_FALSE(applied) << spec;
        if (!applied) {
            EXPECT_EQ(applied.error().where, "workers.launch.hook");
        }
        EXPECT_FALSE(hooks.any()) << spec;
    }
}

}  // namespace
}  // namespace lucent::workers
