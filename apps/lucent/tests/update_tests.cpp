// "Check for updates" in the shell (W12, ADR 0022): the view model's flow over the simulated service
// (not configured, check, download, install only between jobs, failures), and the real adapter over a
// Backend whose channel is a loopback server signed with a test key (check, verified download,
// pre-update catalog backup, Setup started, errors in the photographer's words). Nothing leaves the
// machine.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QSignalSpy>
#include <QTest>

#include <lucent/application/http_client.hpp>
#include <lucent/application/update_channel.hpp>
#include <lucent/application/update_manifest.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/signature.hpp>

#include "adapter_test_support.hpp"
#include "adapters/app_update_service.hpp"
#include "app_controller.hpp"
#include "fakes/fake_services.hpp"
#include "loopback_http_server.hpp"
#include "models/update_model.hpp"
#include "test_support.hpp"

namespace lucent::ui {
namespace {

constexpr int kWaitMs = 20000;

// ---- The view model over the simulated service --------------------------------------------------

class UpdateModelTest : public ::testing::Test {
protected:
    void SetUp() override {
        service = std::make_unique<fakes::FakeUpdateService>(fakes::FakeTimings::immediate());
        model = std::make_unique<UpdateModel>(*service);
    }

    bool wait_for(UpdateModel::State state) {
        return QTest::qWaitFor([&] { return model->state() == state; }, kWaitMs);
    }

    std::unique_ptr<fakes::FakeUpdateService> service;
    std::unique_ptr<UpdateModel> model;
};

TEST(UpdateModelNotConfigured, ABuildWithoutAChannelOffersNothing) {
    UnavailableUpdateService unavailable(QStringLiteral("0.1.0"));
    UpdateModel model(unavailable);
    EXPECT_FALSE(model.configured());
    EXPECT_EQ(model.state(), UpdateModel::NotConfigured);
    EXPECT_TRUE(model.channel_text().contains(QStringLiteral("not configured in this build"))) << model.channel_text().toStdString();
    EXPECT_FALSE(model.can_check());
    EXPECT_FALSE(model.can_download());
    EXPECT_FALSE(model.can_install());
    model.check();
    EXPECT_EQ(model.state(), UpdateModel::NotConfigured);
}

TEST(UpdateModelNotConfigured, TheControllerFallsBackWhenTheServicesBringNone) {
    Services services = fakes::create_fake_services(fakes::FakeTimings::immediate());
    services.updates.reset();
    AppController controller(std::move(services));
    ASSERT_NE(controller.updates(), nullptr);
    EXPECT_FALSE(controller.updates()->configured());
    EXPECT_EQ(controller.updates()->state(), UpdateModel::NotConfigured);
}

TEST_F(UpdateModelTest, ChecksDownloadsAndInstallsOnlyBetweenJobs) {
    EXPECT_TRUE(model->configured());
    EXPECT_EQ(model->state(), UpdateModel::Idle);
    EXPECT_TRUE(model->channel_text().contains(QStringLiteral("when you ask, and only then")));
    EXPECT_EQ(service->checks(), 0) << "nothing is checked until the photographer asks";

    model->check();
    EXPECT_EQ(model->state(), UpdateModel::Checking);
    EXPECT_TRUE(model->can_cancel());
    ASSERT_TRUE(wait_for(UpdateModel::Available));
    EXPECT_EQ(model->version(), QStringLiteral("9.9.0"));
    EXPECT_TRUE(model->status_text().contains(QStringLiteral("9.9.0")));
    EXPECT_TRUE(model->release_notes().contains(QStringLiteral("<b>Plain text</b>")));
    EXPECT_TRUE(model->can_download());
    EXPECT_FALSE(model->can_install());

    QSignalSpy quit(model.get(), &UpdateModel::quitRequested);
    model->download();
    ASSERT_TRUE(wait_for(UpdateModel::Ready));
    EXPECT_TRUE(model->status_text().contains(QStringLiteral("downloaded and verified")));
    EXPECT_TRUE(model->can_install());

    // A running export: installing waits, and says why.
    service->script().blocker = QStringLiteral("Finish or pause the running export first.");
    model->refresh();
    EXPECT_FALSE(model->can_install());
    EXPECT_EQ(model->install_blocker(), service->script().blocker);
    EXPECT_TRUE(model->status_text().contains(service->script().blocker));
    model->install();
    QTest::qWait(50);
    EXPECT_EQ(model->state(), UpdateModel::Ready);
    EXPECT_EQ(service->installs(), 0);
    EXPECT_EQ(quit.count(), 0);

    // The export ended: the update installs, and the application is asked to quit.
    service->script().blocker.clear();
    ASSERT_TRUE(QTest::qWaitFor([&] { return model->can_install(); }, kWaitMs)) << "re-checked while Ready";
    model->install();
    ASSERT_TRUE(QTest::qWaitFor([&] { return quit.count() == 1; }, kWaitMs));
    EXPECT_EQ(service->installs(), 1);
}

TEST_F(UpdateModelTest, FailuresAndOldOrUnsuitableReleasesAreReported) {
    service->script().check.ok = false;
    service->script().check.error = QStringLiteral("The update information is not signed by Lucent's publisher, so it was ignored.");
    model->check();
    ASSERT_TRUE(wait_for(UpdateModel::Failed));
    EXPECT_EQ(model->status_text(), service->script().check.error);
    EXPECT_FALSE(model->can_download());
    EXPECT_TRUE(model->can_check()) << "a check can be repeated";

    service->script().check.ok = true;
    service->script().check.error.clear();
    service->script().check.availability = UpdateAvailability::UpToDate;
    model->check();
    ASSERT_TRUE(wait_for(UpdateModel::UpToDate));
    EXPECT_TRUE(model->status_text().contains(QStringLiteral("is up to date")));
    EXPECT_FALSE(model->can_download());

    service->script().check.availability = UpdateAvailability::NeedsNewerWindows;
    service->script().check.minimum_windows = QStringLiteral("10.0.30000");
    model->check();
    ASSERT_TRUE(wait_for(UpdateModel::NeedsNewerWindows));
    EXPECT_TRUE(model->status_text().contains(QStringLiteral("10.0.30000")));
    EXPECT_FALSE(model->can_download());

    service->script().check.availability = UpdateAvailability::Available;
    service->script().download.ok = false;
    service->script().download.error = QStringLiteral("The downloaded installer is not the one the publisher signed, so it was deleted.");
    model->check();
    ASSERT_TRUE(wait_for(UpdateModel::Available));
    model->download();
    ASSERT_TRUE(wait_for(UpdateModel::Failed));
    EXPECT_EQ(model->status_text(), service->script().download.error);
    EXPECT_TRUE(model->can_download()) << "the release can be downloaded again";
}

// ---- The adapter over a Backend with a loopback channel -----------------------------------------

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

class UpdateAdapterTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(server.listening());
        auto generated = core::EcdsaP256KeyPair::generate();
        ASSERT_TRUE(generated);
        key.emplace(std::move(*generated));
        installer = std::string(200 * 1024, 'S');
    }

    void TearDown() override { services = Services{}; }

    void publish(std::string_view manifest_text) {
        app::ReleaseSignature signature;
        signature.key_id = core::key_id(key->public_key());
        signature.signature = *key->sign(bytes_of(manifest_text));
        server.set("/stable/release-manifest.json", {.body = std::string(manifest_text)});
        server.set("/stable/release-manifest.json.sig", {.body = app::write_release_signature(signature)});
        server.set("/stable/Lucent-Setup-0.2.0-x64.exe", {.body = installer});
    }

    std::string manifest() {
        app::ReleaseManifest m;
        m.channel = "stable";
        m.version = *core::SemanticVersion::parse("0.2.0");
        m.published = "2026-10-09T12:00:00Z";
        m.minimum_os = app::WindowsVersion{10, 0, 17763};
        m.installer.url = server.url("/stable/Lucent-Setup-0.2.0-x64.exe");
        m.installer.size = installer.size();
        m.installer.sha256 = *core::sha256(bytes_of(installer));
        m.release_notes = "Faster previews.";
        return *app::write_release_manifest(m);
    }

    void open() {
        adapters::BackendOptions options = testing::test_backend_options();
        options.catalog_path = dir.path() / L"lucent" / L"data" / L"lucent.catalog";
        options.batches = false;
        app::UpdateServiceOptions updates;
        auto channel = app::make_update_channel("stable", server.url("/stable/release-manifest.json"),
                                                core::to_hex(key->public_key()), "", app::UrlPolicy::LoopbackOnly);
        ASSERT_TRUE(channel) << channel.error().describe();
        updates.channel = *channel;
        updates.current_version = *core::SemanticVersion::parse("0.1.0");
        updates.download_directory = dir.path() / L"lucent" / L"cache" / L"updates";
        updates.log_directory = dir.path() / L"lucent" / L"cache" / L"logs";
        app::HttpClientOptions http;
        http.policy = app::UrlPolicy::LoopbackOnly;
        http.system_proxy = false;
        auto client = app::create_winhttp_client(http);
        ASSERT_TRUE(client);
        updates.http = std::shared_ptr<app::HttpClient>(std::move(*client));
        updates.launcher = [this](const std::filesystem::path& path, const std::wstring& command_line)
            -> Expected<std::uint32_t> {
            launched.emplace_back(path, command_line);
            return 77u;
        };
        options.updates = std::move(updates);
        auto created = adapters::create_application_services(options);
        ASSERT_TRUE(created) << created.error().describe();
        services = std::move(*created);
        adapter = dynamic_cast<adapters::AppUpdateService*>(services.updates.get());
        ASSERT_NE(adapter, nullptr);
    }

    app::test::LoopbackHttpServer server;
    raw::test_support::ScratchDir dir;
    std::optional<core::EcdsaP256KeyPair> key;
    std::string installer;
    Services services;
    adapters::AppUpdateService* adapter = nullptr;
    std::vector<std::pair<std::filesystem::path, std::wstring>> launched;
};

TEST_F(UpdateAdapterTest, ChecksDownloadsBacksUpAndStartsSetup) {
    publish(manifest());
    open();
    if (HasFatalFailure()) {
        return;
    }
    const UpdateChannelInfo channel = adapter->channel();
    EXPECT_TRUE(channel.configured);
    EXPECT_EQ(channel.channel, QStringLiteral("stable"));
    EXPECT_EQ(channel.server, QStringLiteral("127.0.0.1"));
    EXPECT_EQ(channel.current_version, QStringLiteral("0.1.0"));
    EXPECT_TRUE(server.requests().empty()) << "nothing is fetched before the photographer asks";

    std::vector<UpdateCheckInfo> checked;
    QObject::connect(adapter, &UpdateService::checked, adapter, [&](const UpdateCheckInfo& i) { checked.push_back(i); });
    adapter->check();
    ASSERT_TRUE(QTest::qWaitFor([&] { return checked.size() == 1; }, kWaitMs));
    const UpdateCheckInfo info = checked.front();
    ASSERT_TRUE(info.ok) << info.error.toStdString();
    EXPECT_EQ(info.availability, UpdateAvailability::Available);
    EXPECT_EQ(info.version, QStringLiteral("0.2.0"));
    EXPECT_EQ(info.published, QStringLiteral("2026-10-09"));
    EXPECT_EQ(info.release_notes, QStringLiteral("Faster previews."));

    std::vector<UpdateDownloadInfo> downloaded;
    QObject::connect(adapter, &UpdateService::downloaded, adapter,
                     [&](const UpdateDownloadInfo& i) { downloaded.push_back(i); });
    adapter->download();
    ASSERT_TRUE(QTest::qWaitFor([&] { return downloaded.size() == 1; }, kWaitMs));
    const UpdateDownloadInfo download = downloaded.front();
    ASSERT_TRUE(download.ok) << download.error.toStdString();
    EXPECT_TRUE(std::filesystem::exists(download.installer.toStdWString()));
    EXPECT_TRUE(adapter->install_blocker().isEmpty());

    std::vector<UpdateInstallInfo> installed;
    QObject::connect(adapter, &UpdateService::install_finished, adapter,
                     [&](const UpdateInstallInfo& i) { installed.push_back(i); });
    adapter->install();
    ASSERT_TRUE(QTest::qWaitFor([&] { return installed.size() == 1; }, kWaitMs));
    const UpdateInstallInfo install = installed.front();
    ASSERT_TRUE(install.ok) << install.error.toStdString();
    ASSERT_EQ(launched.size(), 1u);
    EXPECT_EQ(launched[0].first.native(), download.installer.toStdWString());
    // The library was backed up first, as a pre-update backup beside the catalog's other backups.
    bool backed_up = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path() / L"lucent" / L"data" / L"backups")) {
        backed_up = backed_up || entry.path().filename().native().ends_with(L".pre-update.bak");
    }
    EXPECT_TRUE(backed_up);
}

TEST_F(UpdateAdapterTest, ErrorsAreWordedForThePhotographer) {
    std::string tampered = manifest();
    publish(tampered);
    tampered.replace(tampered.find("Faster"), 6, "Slower");
    server.set("/stable/release-manifest.json", {.body = tampered});
    open();
    if (HasFatalFailure()) {
        return;
    }
    std::vector<UpdateCheckInfo> checked;
    QObject::connect(adapter, &UpdateService::checked, adapter, [&](const UpdateCheckInfo& i) { checked.push_back(i); });
    adapter->check();
    ASSERT_TRUE(QTest::qWaitFor([&] { return checked.size() == 1; }, kWaitMs));
    const UpdateCheckInfo info = checked.front();
    EXPECT_FALSE(info.ok);
    EXPECT_EQ(info.error, QStringLiteral("The update information is not signed by Lucent's publisher, so it was ignored."));
    // A server that is gone.
    EXPECT_EQ(adapters::user_facing_update_error(Error{ErrorCode::IoError, "x", "app.update.network", 12029}),
              QStringLiteral("Lucent could not reach the update server. Check the internet connection and try again."));
    EXPECT_TRUE(adapters::user_facing_update_error(Error{ErrorCode::AlreadyExists, "x", "app.update.busy", 0})
                    .contains(QStringLiteral("between jobs")));
}

}  // namespace
}  // namespace lucent::ui
