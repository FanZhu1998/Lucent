// Restart reconciliation (batch_service.hpp, "Recovery"; ADR 0012): a coordinator process
// (lucent_batch_test_coordinator) is killed between the steps of the publication protocol; a new
// BatchService over the same catalog must finish the batch with no duplicate success, no output
// published twice, no unrecorded or partial output left in the folder, and the originals intact.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/application/batch_service.hpp>

#include "batch_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
using namespace std::chrono_literals;

std::wstring quote(const std::wstring& argument) { return L"\"" + argument + L"\""; }

// Runs the coordinator helper and returns its exit code (nothing when it did not end in time).
std::optional<DWORD> run_coordinator(const std::vector<std::wstring>& arguments) {
    std::wstring command = quote(std::filesystem::path(LUCENT_BATCH_COORDINATOR_EXE).make_preferred().wstring());
    for (const std::wstring& argument : arguments) {
        command += L" " + quote(argument);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                          &startup, &process)) {
        ADD_FAILURE() << "cannot start the coordinator: " << ::GetLastError();
        return std::nullopt;
    }
    ::CloseHandle(process.hThread);
    std::optional<DWORD> code;
    if (::WaitForSingleObject(process.hProcess, 180'000) == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        ::GetExitCodeProcess(process.hProcess, &exit_code);
        code = exit_code;
    } else {
        ::TerminateProcess(process.hProcess, 1);
    }
    ::CloseHandle(process.hProcess);
    return code;
}

// Waits until the batch is Completed (polling: it may finish before an observer subscribes).
bool wait_completed(BatchService& batches, catalog::BatchId id, std::chrono::seconds timeout = 120s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto status = batches.status(id);
        if (status && status->record.state == catalog::BatchState::Completed) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

struct Crash {
    ts::ScratchDir dir;
    std::filesystem::path root;
    std::filesystem::path out;
    std::vector<std::filesystem::path> files;
    std::vector<core::Digest256> originals;

    explicit Crash(std::size_t photos = 2) : root(dir.path() / L"lucent"), out(dir.path() / L"out") {
        files = ts::write_dngs(dir, L"in", photos);
        for (const auto& file : files) {
            originals.push_back(ts::file_sha256(file));
        }
        ts::BatchHarness prepare(root);
        prepare.import(files);
    }

    // Runs the coordinator until it kills itself at `point` of the first job.
    void kill_at(const std::string& point, const std::wstring& collisions = L"unique_name") {
        const std::optional<DWORD> code =
            run_coordinator({(root / L"data\\lucent.catalog").wstring(), out.wstring(),
                             std::wstring(point.begin(), point.end()), L"{original-stem}_Lucent_{revision}", collisions});
        ASSERT_TRUE(code.has_value()) << "the coordinator did not end";
        ASSERT_EQ(*code, 77u) << "the coordinator did not reach " << point;
    }
};

// Checks everything a recovered batch must satisfy; returns the batch's first job.
catalog::JobRecord check_recovered(Crash& crash, ts::BatchHarness& h) {
    auto batches = h.batches().list();
    EXPECT_TRUE(batches);
    EXPECT_EQ(batches->size(), 1u);
    const catalog::BatchId id = batches->front().record.id;
    EXPECT_TRUE(wait_completed(h.batches(), id));
    const catalog::BatchRecord record = h.record(id);
    EXPECT_EQ(record.counts.complete, 2u);
    // One receipt per job, each matching its file; no other output in the folder.
    const auto receipts = h.receipts(id);
    EXPECT_EQ(receipts.size(), 2u);
    for (const catalog::ReceiptRecord& receipt : receipts) {
        EXPECT_EQ(ts::file_sha256(receipt.final_path), receipt.output_sha256);
    }
    std::vector<std::wstring> outputs;
    for (const std::wstring& name : ts::folder_entries(crash.out)) {
        if (name.ends_with(L".jpg")) {
            outputs.push_back(name);
        }
    }
    EXPECT_EQ(outputs, (std::vector<std::wstring>{L"photo0_Lucent_0.jpg", L"photo1_Lucent_0.jpg"}));
    EXPECT_TRUE(ts::leftovers(crash.out).empty());
    // Every attempt's publication is closed, and at most one per job committed a receipt.
    std::optional<catalog::JobRecord> first;
    for (const catalog::JobRecord& job : h.jobs(id)) {
        if (job.ordinal == 0) {
            first = job;
        }
        auto journal = h.catalog().query([&](catalog::ReadTransaction& tx) { return tx.publication_journal(job.id); });
        EXPECT_TRUE(journal);
        std::size_t committed = 0;
        std::set<std::uint32_t> attempts;
        std::set<std::uint32_t> closed;
        for (const catalog::PublicationRecord& entry : *journal) {
            attempts.insert(entry.attempt);
            if (entry.step == catalog::PublicationStep::ReceiptCommitted ||
                entry.step == catalog::PublicationStep::Abandoned) {
                closed.insert(entry.attempt);
            }
            committed += entry.step == catalog::PublicationStep::ReceiptCommitted ? 1 : 0;
        }
        EXPECT_EQ(committed, 1u);
        EXPECT_EQ(attempts, closed) << "job " << job.id.value << " has an open publication";
    }
    auto open = h.catalog().query([&](catalog::ReadTransaction& tx) {
        return tx.list_publications_in_progress(std::nullopt, 100);
    });
    EXPECT_TRUE(open && open->empty());
    for (std::size_t i = 0; i < crash.files.size(); ++i) {
        EXPECT_EQ(ts::file_sha256(crash.files[i]), crash.originals[i]) << "original preservation";
    }
    return first.value_or(catalog::JobRecord{});
}

class BatchRecovery : public ::testing::TestWithParam<const char*> {};

TEST_P(BatchRecovery, FinishesWithoutDuplicatesAfterACrash) {
    const std::string point = GetParam();
    Crash crash;
    if (point == "temp_written") {
        // A temporary file an in-process export left behind long ago, in the same folder.
        const auto stale = crash.dir.write_text(L"out\\~lucent-0123456789abcdef.tmp", "stale");
        HANDLE handle = ::CreateFileW(stale.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(handle, INVALID_HANDLE_VALUE);
        FILETIME old{0x10000000, 0x01D00000};  // 2014
        ::SetFileTime(handle, nullptr, nullptr, &old);
        ::CloseHandle(handle);
    }
    crash.kill_at(point);
    if (HasFatalFailure()) {
        return;
    }
    ts::BatchHarness h(crash.root);
    const RecoveryReport recovery = h.batches().last_recovery();
    const catalog::JobRecord first = check_recovered(crash, h);
    EXPECT_EQ(recovery.reclaimed, 1u);
    if (point == "published" || point == "receipt") {
        EXPECT_EQ(recovery.completed, 1u) << "a published output verified against its journal completes the job";
        EXPECT_EQ(first.attempts, 1u);
    } else {
        EXPECT_EQ(recovery.requeued, 1u);
        EXPECT_EQ(first.attempts, 2u);
        EXPECT_EQ(recovery.publications_closed, 1u);
    }
    if (point == "after_rename") {
        EXPECT_EQ(recovery.orphans_removed, 1u) << "an unrecorded output is never left published";
    }
    if (point == "temp_written" || point == "verified" || point == "before_rename") {
        EXPECT_GE(recovery.leftovers_removed, 1u);
    }
}

INSTANTIATE_TEST_SUITE_P(PublicationSteps, BatchRecovery,
                         ::testing::Values("reserved", "temp_written", "verified", "before_rename", "after_rename",
                                           "published", "receipt"),
                         [](const ::testing::TestParamInfo<const char*>& info) { return std::string(info.param); });

class BatchRecoveryOverwrite : public ::testing::TestWithParam<const char*> {};

TEST_P(BatchRecoveryOverwrite, RestoresOrReplacesTheFileItWasReplacing) {
    const std::string point = GetParam();
    Crash crash;
    std::filesystem::create_directories(crash.out);
    const std::string earlier = "the user's earlier export";
    const auto existing = crash.dir.write_text(L"out\\photo0_Lucent_0.jpg", earlier);
    crash.kill_at(point, L"overwrite");
    if (HasFatalFailure()) {
        return;
    }
    if (point == "after_rename") {
        // The crash left the replaced file aside and the new output unrecorded at its name.
        EXPECT_FALSE(ts::leftovers(crash.out).empty());
    }
    ts::BatchHarness h(crash.root);
    const RecoveryReport recovery = h.batches().last_recovery();
    check_recovered(crash, h);
    if (point == "after_rename") {
        EXPECT_EQ(recovery.orphans_removed, 1u);
        EXPECT_EQ(recovery.restored_aside, 1u) << "the file being replaced is put back first";
    }
    EXPECT_NE(ts::file_sha256(existing), *core::sha256(std::as_bytes(std::span(earlier.data(), earlier.size()))))
        << "in the end the batch replaced it";
}

INSTANTIATE_TEST_SUITE_P(PublicationSteps, BatchRecoveryOverwrite,
                         ::testing::Values("before_rename", "after_rename", "published"),
                         [](const ::testing::TestParamInfo<const char*>& info) { return std::string(info.param); });

// ---- The destination out of reach at restart (a USB drive not plugged in yet, a share not
// reconnected, an output held by a sync client): recovery decides nothing about outputs it cannot
// see, and the batch still ends with exactly one output and one receipt per photo.

BatchServiceOptions reconnecting_options() {
    BatchServiceOptions options = ts::fast_batch_options();
    options.reclaim_interval = 300ms;
    return options;
}

class BatchRecoveryAway : public ::testing::TestWithParam<const char*> {};

TEST_P(BatchRecoveryAway, WaitsForTheOutputFolderInsteadOfGuessing) {
    const std::string point = GetParam();
    Crash crash;
    crash.kill_at(point);
    if (HasFatalFailure()) {
        return;
    }
    // The drive is not back when Lucent starts again.
    const std::filesystem::path away = crash.dir.path() / L"away";
    std::filesystem::rename(crash.out, away);
    {
        ts::BatchHarness h(crash.root, reconnecting_options());
        const RecoveryReport recovery = h.batches().last_recovery();
        EXPECT_EQ(recovery.reclaimed, 0u) << "nothing is decided about an output folder that cannot be seen";
        EXPECT_GE(recovery.deferred, 1u);
        EXPECT_EQ(recovery.failed, 0u);
        auto batches = h.batches().list();
        ASSERT_TRUE(batches);
        ASSERT_EQ(batches->size(), 1u);
        const catalog::BatchId id = batches->front().record.id;
        // The rest of the batch waits for the folder too.
        ASSERT_TRUE(ts::eventually([&] {
            auto status = h.batches().status(id);
            return status && status->attention != BatchAttention::None &&
                   status->record.state == catalog::BatchState::Paused;
        }));
        EXPECT_EQ(h.record(id).counts.leased(), 1u) << "the interrupted job keeps its lease until the folder is back";
        EXPECT_EQ(h.record(id).counts.failed, 0u);
        std::this_thread::sleep_for(1s);  // a few reclaim ticks without the folder change nothing
        EXPECT_EQ(h.record(id).counts.leased(), 1u);
        EXPECT_FALSE(std::filesystem::exists(crash.out)) << "the output folder is never created again";
        // The drive is back: resume.
        std::filesystem::rename(away, crash.out);
        ASSERT_TRUE(h.batches().resume(id));
        const catalog::JobRecord first = check_recovered(crash, h);
        if (point == "after_rename") {
            EXPECT_EQ(first.attempts, 2u);
            EXPECT_EQ(h.batches().last_recovery().orphans_removed, 1u);
        } else {
            EXPECT_EQ(first.attempts, 1u) << "the verified output was completed, not rendered again";
        }
        EXPECT_EQ(first.transient_failures, 0u);
    }
}

INSTANTIATE_TEST_SUITE_P(PublicationSteps, BatchRecoveryAway, ::testing::Values("after_rename", "published", "receipt"),
                         [](const ::testing::TestParamInfo<const char*>& info) { return std::string(info.param); });

TEST(BatchRecoveryLocked, AnOutputHeldByAnotherProgramIsCheckedWhenItIsFree) {
    Crash crash;
    crash.kill_at("receipt");
    if (HasFatalFailure()) {
        return;
    }
    // A sync client or a virus scanner holds the published output exclusively at start-up.
    const std::filesystem::path output = crash.out / L"photo0_Lucent_0.jpg";
    HANDLE lock = ::CreateFileW(output.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);
    ts::BatchHarness h(crash.root, reconnecting_options());
    const RecoveryReport recovery = h.batches().last_recovery();
    EXPECT_EQ(recovery.failed, 0u) << "an output that cannot be read now is not lost";
    EXPECT_EQ(recovery.deferred, 1u);
    auto batches = h.batches().list();
    ASSERT_TRUE(batches && batches->size() == 1u);
    const catalog::BatchId id = batches->front().record.id;
    // The other photo is exported meanwhile; the held one waits.
    ASSERT_TRUE(ts::eventually([&] { return h.record(id).counts.complete == 1u; }));
    EXPECT_EQ(h.record(id).counts.leased(), 1u);
    ::CloseHandle(lock);
    const catalog::JobRecord first = check_recovered(crash, h);
    EXPECT_EQ(first.attempts, 1u);
    EXPECT_EQ(first.stage, catalog::JobStage::Complete);
}

TEST(BatchRecoveryReport, ABatchThatFinishedJustBeforeAStopGetsItsReport) {
    Crash crash(1);
    crash.kill_at("complete");
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_EQ(ts::folder_entries(crash.out), (std::vector<std::wstring>{L"photo0_Lucent_0.jpg"}));
    catalog::BatchId id;
    {
        ts::BatchHarness h(crash.root);
        EXPECT_EQ(h.batches().last_recovery().reports_written, 1u);
        auto batches = h.batches().list();
        ASSERT_TRUE(batches && batches->size() == 1u);
        id = batches->front().record.id;
        EXPECT_EQ(batches->front().record.state, catalog::BatchState::Completed);
        EXPECT_TRUE(batches->front().record.reported_at.has_value());
    }
    const std::vector<std::wstring> names = ts::folder_entries(crash.out);
    EXPECT_EQ(names, (std::vector<std::wstring>{std::format(L"Lucent-batch-{}-report.html", id.value),
                                                std::format(L"Lucent-batch-{}-report.json", id.value),
                                                L"photo0_Lucent_0.jpg"}));
    ts::BatchHarness again(crash.root);
    EXPECT_EQ(again.batches().last_recovery().reports_written, 0u) << "written once";
}

}  // namespace
}  // namespace lucent::app
