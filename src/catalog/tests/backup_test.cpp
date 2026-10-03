// User backups, retention and restore.

#include "backup.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include "catalog_test_support.hpp"
#include "schema.hpp"

namespace lucent::catalog {
namespace {

using namespace test;

std::uint64_t count_in(const std::filesystem::path& catalog_path) {
    OpenOptions options = options_for(catalog_path);
    options.mode = OpenMode::ReadOnly;
    Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options);
    if (!catalog) {
        ADD_FAILURE() << catalog.error().describe();
        return 0;
    }
    Expected<std::uint64_t> count = (*catalog)->query([](ReadTransaction& tx) { return tx.count_sources(); });
    return count ? *count : 0;
}

TEST(CatalogBackupNames, FormatAndParseRoundTrip) {
    const detail::BackupName name{at(1'791'045'296'789), 3,
                                  BackupKind::PreMigration};  // 2026-10-03T16:34:56.789Z
    const std::wstring text = detail::format_backup_name(L"lucent.catalog", name);
    EXPECT_EQ(text, L"lucent.catalog.20261003T163456789Z.v3.pre-migration.bak");
    const std::optional<detail::BackupName> parsed = detail::parse_backup_name(L"lucent.catalog", text);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->created_at, name.created_at);
    EXPECT_EQ(parsed->schema_version, 3);
    EXPECT_EQ(parsed->kind, BackupKind::PreMigration);
    // Case-insensitive prefix, like Windows names.
    EXPECT_TRUE(detail::parse_backup_name(L"Lucent.Catalog", text).has_value());
    for (const wchar_t* other :
         {L"other.catalog.20261003T163456789Z.v3.manual.bak", L"lucent.catalog.20261003T163456789Z.v3.manual",
          L"lucent.catalog.20261003T163456789Z.v3.weekly.bak",
          L"lucent.catalog.20261303T163456789Z.v3.manual.bak",
          L"lucent.catalog.20261003T163456789Z.3.manual.bak",
          L"lucent.catalog.20261003T163456789Z.v3.x.manual.bak", L"lucent.catalog.bak"}) {
        EXPECT_FALSE(detail::parse_backup_name(L"lucent.catalog", other).has_value())
            << core::path_to_utf8_lossy(other);
    }
}

class CatalogBackup : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = open_or_fail(path_);
        ASSERT_NE(catalog_, nullptr);
        LUCENT_ASSERT_OK(populate(*catalog_, dir_ / L"photos", 1, 30));
    }

    TempDir dir_;
    std::filesystem::path path_ = dir_ / L"lucent.catalog";
    std::unique_ptr<Catalog> catalog_;
};

TEST_F(CatalogBackup, CreatesAVerifiedSelfContainedBackup) {
    Expected<BackupInfo> backup = catalog_->create_backup();
    LUCENT_ASSERT_OK(backup);
    EXPECT_EQ(backup->kind, BackupKind::Manual);
    EXPECT_EQ(backup->schema_version, 1);
    EXPECT_EQ(backup->path.parent_path(), catalog_->backup_directory());
    EXPECT_EQ(catalog_->backup_directory(), dir_ / L"backups");
    EXPECT_EQ(backup->size_bytes, std::filesystem::file_size(backup->path));
    // One file: rollback-journal mode, no WAL companions or temporaries left behind.
    std::vector<std::wstring> names;
    for (const auto& entry : std::filesystem::directory_iterator(catalog_->backup_directory())) {
        names.push_back(entry.path().filename().native());
    }
    ASSERT_EQ(names.size(), 1u);
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(backup->path, detail::Access::ReadOnly);
        ASSERT_NE(raw, nullptr);
        EXPECT_EQ(query_text(*raw, "PRAGMA journal_mode"), "delete");
        EXPECT_EQ(query_int(*raw, "PRAGMA application_id"), detail::kApplicationId);
    }
    EXPECT_EQ(count_in(backup->path), 30u);

    Expected<std::vector<BackupInfo>> listed = catalog_->list_backups();
    LUCENT_ASSERT_OK(listed);
    ASSERT_EQ(listed->size(), 1u);
    EXPECT_EQ(listed->front().path, backup->path);
    EXPECT_EQ(listed->front().kind, BackupKind::Manual);
    EXPECT_EQ(listed->front().size_bytes, backup->size_bytes);
}

TEST_F(CatalogBackup, BackupToAnExplicitPathNeverOverwrites) {
    const std::filesystem::path target = dir_ / L"elsewhere" / L"Копия каталога.bak";
    std::filesystem::create_directories(target.parent_path());
    Expected<BackupInfo> backup = catalog_->backup_to(target);
    LUCENT_ASSERT_OK(backup);
    EXPECT_EQ(backup->path, target);
    EXPECT_EQ(count_in(target), 30u);
    Expected<BackupInfo> again = catalog_->backup_to(target);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(count_in(target), 30u);
    Expected<BackupInfo> no_folder = catalog_->backup_to(dir_ / L"missing" / L"x.bak");
    ASSERT_FALSE(no_folder);
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(target.parent_path())) {
        (void)entry;
        ++files;
    }
    EXPECT_EQ(files, 1u);  // no temporaries left
}

TEST_F(CatalogBackup, RetentionKeepsTheNewestOfEachKind) {
    catalog_.reset();
    OpenOptions options = options_for(path_);
    options.backups_to_keep = 2;
    options.backup_directory = dir_ / L"my backups";
    Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options);
    LUCENT_ASSERT_OK(catalog);
    // A backup of another kind that retention of manual backups must not touch.
    const std::filesystem::path premigration =
        options.backup_directory /
        detail::format_backup_name(L"lucent.catalog", {at(1000), 1, BackupKind::PreMigration});
    std::filesystem::create_directories(options.backup_directory);
    std::filesystem::copy_file(path_, premigration);
    // And an unrelated file.
    write_bytes(options.backup_directory / L"notes.txt", {'h', 'i'});

    std::vector<std::filesystem::path> created;
    for (int i = 0; i < 4; ++i) {
        Expected<BackupInfo> backup = (*catalog)->create_backup();
        LUCENT_ASSERT_OK(backup);
        created.push_back(backup->path);
    }
    EXPECT_FALSE(std::filesystem::exists(created[0]));
    EXPECT_FALSE(std::filesystem::exists(created[1]));
    EXPECT_TRUE(std::filesystem::exists(created[2]));
    EXPECT_TRUE(std::filesystem::exists(created[3]));
    EXPECT_TRUE(std::filesystem::exists(premigration));
    EXPECT_TRUE(std::filesystem::exists(options.backup_directory / L"notes.txt"));
    Expected<std::vector<BackupInfo>> listed = (*catalog)->list_backups();
    LUCENT_ASSERT_OK(listed);
    ASSERT_EQ(listed->size(), 3u);
    EXPECT_EQ((*listed)[0].path, created[3]);  // newest first
    EXPECT_EQ((*listed)[1].path, created[2]);
    EXPECT_EQ((*listed)[2].path, premigration);
}

// An online backup taken while a writer commits batches captures exactly one committed state.
TEST_F(CatalogBackup, BackupDuringWritesIsAConsistentSnapshot) {
    constexpr std::uint64_t kBatch = 10;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (std::uint64_t b = 0; !stop.load() && b < 200; ++b) {
            const Status s = populate(*catalog_, dir_ / L"more", 1000 + b * kBatch, kBatch);
            if (!s) {
                ADD_FAILURE() << s.error().describe();
                return;
            }
        }
    });
    std::vector<std::filesystem::path> backups;
    for (int i = 0; i < 3; ++i) {
        Expected<BackupInfo> backup = catalog_->backup_to(dir_ / std::format(L"snapshot-{}.bak", i));
        EXPECT_TRUE(backup) << describe(backup);
        if (backup) {
            backups.push_back(backup->path);
        }
    }
    stop = true;
    writer.join();
    for (const std::filesystem::path& backup : backups) {
        const std::uint64_t count = count_in(backup);
        EXPECT_GE(count, 30u);
        EXPECT_EQ((count - 30) % kBatch, 0u) << core::path_to_utf8_lossy(backup);
    }
}

TEST_F(CatalogBackup, RestoreReplacesTheCatalogAndKeepsTheOldOne) {
    Expected<BackupInfo> backup = catalog_->create_backup();
    LUCENT_ASSERT_OK(backup);
    LUCENT_ASSERT_OK(populate(*catalog_, dir_ / L"photos", 100, 12));

    // Refused while the catalog is open.
    Expected<RestoreReport> busy = Catalog::restore(path_, backup->path);
    ASSERT_FALSE(busy);
    EXPECT_EQ(busy.error().code, ErrorCode::AlreadyExists);

    catalog_.reset();
    Expected<RestoreReport> report = Catalog::restore(path_, backup->path);
    LUCENT_ASSERT_OK(report);
    EXPECT_EQ(report->restored_schema_version, 1);
    EXPECT_TRUE(report->moved_aside.empty());
    ASSERT_TRUE(report->pre_restore_backup.has_value());
    EXPECT_EQ(report->pre_restore_backup->kind, BackupKind::PreRestore);
    EXPECT_EQ(count_in(report->pre_restore_backup->path), 42u);  // the replaced state is kept
    EXPECT_TRUE(std::filesystem::exists(backup->path));          // the backup itself is untouched

    std::unique_ptr<Catalog> restored = open_or_fail(path_);
    ASSERT_NE(restored, nullptr);
    Expected<std::uint64_t> count = restored->query([](ReadTransaction& tx) { return tx.count_sources(); });
    LUCENT_ASSERT_OK(count);
    EXPECT_EQ(*count, 30u);
    EXPECT_FALSE(restored->is_read_only());
    // The restored catalog is a normal live catalog again (WAL mode).
    LUCENT_ASSERT_OK(populate(*restored, dir_ / L"photos", 500, 1));
}

TEST_F(CatalogBackup, RestoreIntoAFreshLocation) {
    Expected<BackupInfo> backup = catalog_->create_backup();
    LUCENT_ASSERT_OK(backup);
    const std::filesystem::path fresh = dir_ / L"new machine" / L"lucent.catalog";
    Expected<RestoreReport> report = Catalog::restore(fresh, backup->path);
    LUCENT_ASSERT_OK(report);
    EXPECT_FALSE(report->pre_restore_backup.has_value());
    EXPECT_EQ(count_in(fresh), 30u);
}

TEST_F(CatalogBackup, RestoreRefusesDamagedForeignAndNewerBackups) {
    Expected<BackupInfo> backup = catalog_->create_backup();
    LUCENT_ASSERT_OK(backup);
    catalog_.reset();
    const std::vector<char> good = read_bytes(backup->path);

    const auto expect_refused = [&](const std::filesystem::path& candidate, ErrorCode code) {
        const std::vector<char> before = read_bytes(path_);
        Expected<RestoreReport> report = Catalog::restore(path_, candidate);
        ASSERT_FALSE(report);
        EXPECT_EQ(report.error().code, code) << report.error().describe();
        EXPECT_EQ(read_bytes(path_), before);  // the catalog is untouched
    };

    // Damaged: one page overwritten.
    std::vector<char> damaged = good;
    const std::size_t page = damaged.size() / 4096 / 2;
    std::fill(damaged.begin() + static_cast<std::ptrdiff_t>(page * 4096),
              damaged.begin() + static_cast<std::ptrdiff_t>((page + 1) * 4096), static_cast<char>(0x5A));
    write_bytes(dir_ / L"damaged.bak", damaged);
    expect_refused(dir_ / L"damaged.bak", ErrorCode::Corrupt);

    // Foreign SQLite database.
    {
        std::unique_ptr<detail::Connection> raw =
            raw_connection(dir_ / L"foreign.bak", detail::Access::ReadWriteCreate);
        ASSERT_NE(raw, nullptr);
        LUCENT_ASSERT_OK(raw->exec("CREATE TABLE t (x)", "test"));
    }
    expect_refused(dir_ / L"foreign.bak", ErrorCode::Unsupported);

    // Newer schema.
    write_bytes(dir_ / L"newer.bak", good);
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(dir_ / L"newer.bak");
        ASSERT_NE(raw, nullptr);
        LUCENT_ASSERT_OK(raw->exec("PRAGMA user_version = 9", "test"));
    }
    expect_refused(dir_ / L"newer.bak", ErrorCode::VersionMismatch);

    // Missing, and the catalog itself.
    expect_refused(dir_ / L"absent.bak", ErrorCode::NotFound);
    expect_refused(path_, ErrorCode::InvalidArgument);
}

TEST_F(CatalogBackup, RestoreOverADamagedCatalogMovesItAside) {
    Expected<BackupInfo> backup = catalog_->create_backup();
    LUCENT_ASSERT_OK(backup);
    catalog_.reset();
    std::vector<char> bytes = read_bytes(path_);
    std::fill(bytes.begin(), bytes.begin() + 16, '\0');  // no longer a database
    write_bytes(path_, bytes);
    ASSERT_FALSE(Catalog::open(options_for(path_)));

    Expected<RestoreReport> report = Catalog::restore(path_, backup->path);
    LUCENT_ASSERT_OK(report);
    EXPECT_FALSE(report->pre_restore_backup.has_value());
    ASSERT_EQ(report->moved_aside.size(), 1u);
    EXPECT_TRUE(std::filesystem::exists(report->moved_aside[0]));
    EXPECT_EQ(read_bytes(report->moved_aside[0]), bytes);  // the damaged file is kept for diagnosis
    EXPECT_EQ(count_in(path_), 30u);
}

}  // namespace
}  // namespace lucent::catalog
