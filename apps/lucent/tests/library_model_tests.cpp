// LibraryModel, CurrentItem and ExportJobModel: roles, identity, and the ordering guarantee that
// the generation registry is updated before a changed image URL is announced.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <set>

#include <QSignalSpy>

#include "imaging/generation_registry.hpp"
#include "models/current_item.hpp"
#include "models/export_job_model.hpp"
#include "models/library_model.hpp"
#include "test_support.hpp"

namespace lucent::ui {
namespace {

using testing::make_item;
using testing::RecordingExportService;
using testing::ScriptedLibraryService;

QVariant role(const QAbstractItemModel& model, int row, int role_id) {
    return model.data(model.index(row, 0), role_id);
}

class LibraryModelTest : public ::testing::Test {
protected:
    ScriptedLibraryService service;
    std::shared_ptr<GenerationRegistry> registry = std::make_shared<GenerationRegistry>();
    LibraryModel model{service, registry};
};

TEST_F(LibraryModelTest, AddsItemsInOrderAndExposesPresentationRoles) {
    QSignalSpy count_spy(&model, &LibraryModel::countChanged);
    LibraryItem second = make_item(2, QStringLiteral("B.ARW"));
    second.support = SupportStatus::Limited;
    second.support_detail = QStringLiteral("No lens profile.");
    second.flags = flag_bit(ItemFlag::ReviewRecommended) | flag_bit(ItemFlag::DuplicateContent);
    service.add({make_item(1, QStringLiteral("A.CR3")), second});

    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.count(), 2);
    EXPECT_EQ(count_spy.count(), 1);
    EXPECT_EQ(role(model, 0, LibraryModel::FileNameRole).toString(), QStringLiteral("A.CR3"));
    EXPECT_EQ(role(model, 0, LibraryModel::FolderRole).toString(), QStringLiteral("C:\\Photos"));
    EXPECT_EQ(role(model, 0, LibraryModel::CameraLabelRole).toString(), QStringLiteral("Canon EOS R5"));
    EXPECT_EQ(role(model, 0, LibraryModel::ExposureLabelRole).toString(),
              QStringLiteral("1/250 s · f/5.6 · ISO 400 · 35 mm"));
    EXPECT_EQ(role(model, 0, LibraryModel::DimensionsLabelRole).toString(),
              QStringLiteral("6000 × 4000 · 24.0 MP"));
    EXPECT_EQ(role(model, 0, LibraryModel::ThumbnailSourceRole).toString(),
              QStringLiteral("image://thumbnail/1/1"));
    EXPECT_EQ(role(model, 0, LibraryModel::PreviewSourceRole).toString(), QStringLiteral("image://preview/1/1"));
    EXPECT_TRUE(role(model, 0, LibraryModel::ThumbnailIsCameraPreviewRole).toBool());
    EXPECT_EQ(role(model, 0, LibraryModel::SupportKeyRole).toString(), QStringLiteral("verified"));

    EXPECT_EQ(role(model, 1, LibraryModel::SupportKeyRole).toString(), QStringLiteral("limited"));
    EXPECT_EQ(role(model, 1, LibraryModel::SupportDetailRole).toString(), QStringLiteral("No lens profile."));
    EXPECT_EQ(role(model, 1, LibraryModel::FlagLabelsRole).toStringList(),
              (QStringList{QStringLiteral("Review recommended"), QStringLiteral("Duplicate")}));
    const QString accessible = role(model, 1, LibraryModel::AccessibleLabelRole).toString();
    EXPECT_TRUE(accessible.startsWith(QStringLiteral("B.ARW, Canon EOS R5, "))) << accessible.toStdString();
    EXPECT_TRUE(accessible.contains(QStringLiteral("camera preview")));

    EXPECT_EQ(registry->current(1, ImageKind::Thumbnail), Generation{1});
    EXPECT_EQ(registry->current(2, ImageKind::Preview), Generation{1});
    EXPECT_FALSE(role(model, 2, LibraryModel::FileNameRole).isValid());
    EXPECT_EQ(model.item_at(5), nullptr);
    EXPECT_EQ(model.row_of(2), 1);
    EXPECT_FALSE(model.row_of(3).has_value());
}

TEST_F(LibraryModelTest, RoleNamesAreUniqueAndCoverEveryRole) {
    const QHash<int, QByteArray> names = model.roleNames();
    std::set<QByteArray> unique;
    for (int id = LibraryModel::FileNameRole; id <= LibraryModel::EditedRole; ++id) {
        ASSERT_TRUE(names.contains(id)) << id;
        unique.insert(names.value(id));
    }
    EXPECT_EQ(unique.size(), static_cast<std::size_t>(names.size()));
}

TEST_F(LibraryModelTest, RenderRolesFollowTheItemsLatestRender) {
    service.add({make_item(1, QStringLiteral("A.CR3"))});
    EXPECT_FALSE(role(model, 0, LibraryModel::HasRenderRole).toBool());
    EXPECT_TRUE(role(model, 0, LibraryModel::RenderLabelRole).toString().isEmpty());
    EXPECT_EQ(role(model, 0, LibraryModel::RenderInfoRole).value<RenderInfoPtr>(), nullptr);

    LibraryItem rendered = make_item(1, QStringLiteral("A.CR3"));
    auto info = std::make_shared<RenderInfo>();
    info->rendering = QStringLiteral("Neutral v0");
    info->notes = {QStringLiteral("An optional step was skipped.")};
    rendered.render = info;
    rendered.processing = ProcessingStatus::Failed;
    rendered.processing_detail = QStringLiteral("Out of memory.");
    service.change(rendered);
    EXPECT_TRUE(role(model, 0, LibraryModel::HasRenderRole).toBool());
    EXPECT_EQ(role(model, 0, LibraryModel::RenderInfoRole).value<RenderInfoPtr>(), info);
    EXPECT_EQ(role(model, 0, LibraryModel::RenderLabelRole).toString(), QStringLiteral("Neutral v0"));
    EXPECT_EQ(role(model, 0, LibraryModel::RenderNotesRole).toStringList(), info->notes);
    EXPECT_EQ(role(model, 0, LibraryModel::ProcessingDetailRole).toString(), QStringLiteral("Out of memory."));
}

TEST_F(LibraryModelTest, ProcessingRolesShowTheStateAndMarkIncompleteRenders) {
    service.add({make_item(1, QStringLiteral("A.CR3"))});
    EXPECT_EQ(role(model, 0, LibraryModel::ProcessingKeyRole).toString(), QStringLiteral("notprocessed"));
    EXPECT_FALSE(role(model, 0, LibraryModel::RenderIncompleteRole).toBool());

    LibraryItem item = make_item(1, QStringLiteral("A.CR3"));
    item.processing = ProcessingStatus::Processing;
    service.change(item);
    EXPECT_EQ(role(model, 0, LibraryModel::ProcessingKeyRole).toString(), QStringLiteral("processing"));
    EXPECT_EQ(role(model, 0, LibraryModel::ProcessingLabelRole).toString(), QStringLiteral("Processing"));

    auto info = std::make_shared<RenderInfo>();
    item.render = info;
    item.processing = ProcessingStatus::Processed;
    service.change(item);
    EXPECT_EQ(role(model, 0, LibraryModel::ProcessingKeyRole).toString(), QStringLiteral("processed"));
    EXPECT_FALSE(role(model, 0, LibraryModel::RenderIncompleteRole).toBool());
    EXPECT_TRUE(role(model, 0, LibraryModel::ProcessingDetailRole).toString().isEmpty());

    // A render without a mandatory step is never presented as a plain "Processed".
    auto incomplete = std::make_shared<RenderInfo>();
    incomplete->incomplete = true;
    item.render = incomplete;
    service.change(item);
    EXPECT_EQ(role(model, 0, LibraryModel::ProcessingKeyRole).toString(), QStringLiteral("incomplete"));
    EXPECT_EQ(role(model, 0, LibraryModel::ProcessingLabelRole).toString(), QStringLiteral("Incomplete render"));
    EXPECT_TRUE(role(model, 0, LibraryModel::RenderIncompleteRole).toBool());
    EXPECT_TRUE(role(model, 0, LibraryModel::ProcessingDetailRole).toString().contains(QStringLiteral("not accurate")));

    item.processing = ProcessingStatus::Failed;
    item.processing_detail = QStringLiteral("Out of memory.");
    service.change(item);
    EXPECT_EQ(role(model, 0, LibraryModel::ProcessingKeyRole).toString(), QStringLiteral("failed"));
    EXPECT_FALSE(role(model, 0, LibraryModel::RenderIncompleteRole).toBool());
    EXPECT_EQ(role(model, 0, LibraryModel::ProcessingDetailRole).toString(), QStringLiteral("Out of memory."));
    EXPECT_TRUE(model.roleNames().contains(LibraryModel::ProcessingKeyRole));
    EXPECT_TRUE(model.roleNames().contains(LibraryModel::RenderIncompleteRole));
}

TEST_F(LibraryModelTest, ARegistryUpdatePrecedesTheChangeNotification) {
    service.add({make_item(1, QStringLiteral("A.CR3"))});
    Generation seen_in_registry = kNoImage;
    QString seen_url;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model, [&] {
        seen_in_registry = registry->current(1, ImageKind::Thumbnail).value_or(kNoImage);
        seen_url = role(model, 0, LibraryModel::ThumbnailSourceRole).toString();
    });
    LibraryItem rendered = make_item(1, QStringLiteral("A.CR3"), 2, 1);
    rendered.thumbnail_is_camera_preview = false;
    service.change(rendered);

    EXPECT_EQ(seen_in_registry, Generation{2});
    EXPECT_EQ(seen_url, QStringLiteral("image://thumbnail/1/2"));
    EXPECT_FALSE(role(model, 0, LibraryModel::ThumbnailIsCameraPreviewRole).toBool());
}

TEST_F(LibraryModelTest, DuplicateIdsUpdateInsteadOfAddingRows) {
    service.add({make_item(1, QStringLiteral("A.CR3")), make_item(1, QStringLiteral("A.CR3"))});
    EXPECT_EQ(model.rowCount(), 1);
    QSignalSpy data_spy(&model, &QAbstractItemModel::dataChanged);
    service.add({make_item(1, QStringLiteral("A.CR3"), 3, 1)});
    EXPECT_EQ(model.rowCount(), 1);
    EXPECT_EQ(data_spy.count(), 1);
    EXPECT_EQ(registry->current(1, ImageKind::Thumbnail), Generation{3});
}

TEST_F(LibraryModelTest, IgnoresUnknownChangesAndInvalidIds) {
    QSignalSpy data_spy(&model, &QAbstractItemModel::dataChanged);
    service.change(make_item(42, QStringLiteral("X.CR3")));
    service.add({make_item(0, QStringLiteral("zero.CR3"))});
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(data_spy.count(), 0);
    EXPECT_FALSE(registry->current(42, ImageKind::Thumbnail).has_value());
}

TEST_F(LibraryModelTest, AnItemWithoutImagesHasNoSources) {
    service.add({make_item(1, QStringLiteral("A.CR3"), kNoImage, kNoImage)});
    EXPECT_TRUE(role(model, 0, LibraryModel::ThumbnailSourceRole).toString().isEmpty());
    EXPECT_TRUE(role(model, 0, LibraryModel::PreviewSourceRole).toString().isEmpty());
    EXPECT_FALSE(role(model, 0, LibraryModel::ThumbnailIsCameraPreviewRole).toBool());
}

TEST_F(LibraryModelTest, CurrentItemFollowsItsItem) {
    service.add({make_item(1, QStringLiteral("A.CR3")), make_item(2, QStringLiteral("B.ARW"))});
    CurrentItem current(model);
    EXPECT_FALSE(current.valid());
    EXPECT_TRUE(current.file_name().isEmpty());

    QSignalSpy changed(&current, &CurrentItem::changed);
    current.set_row(1);
    EXPECT_EQ(changed.count(), 1);
    EXPECT_TRUE(current.valid());
    EXPECT_EQ(current.item_id(), ItemId{2});
    EXPECT_EQ(current.file_name(), QStringLiteral("B.ARW"));
    EXPECT_EQ(current.pixel_width(), 6000);
    EXPECT_EQ(current.preview_source(), QStringLiteral("image://preview/2/1"));

    current.set_row(1);  // unchanged: no notification
    EXPECT_EQ(changed.count(), 1);

    service.change(make_item(1, QStringLiteral("A.CR3"), 2, 2));  // another row
    EXPECT_EQ(changed.count(), 1);
    service.change(make_item(2, QStringLiteral("B.ARW"), 1, 2));  // this row
    EXPECT_EQ(changed.count(), 2);
    EXPECT_EQ(current.preview_source(), QStringLiteral("image://preview/2/2"));

    current.set_row(7);
    EXPECT_FALSE(current.valid());
    EXPECT_EQ(current.row(), -1);
    EXPECT_EQ(changed.count(), 3);
}

class ExportJobModelTest : public ::testing::Test {
protected:
    RecordingExportService service;
    ExportJobModel model{service};

    static ExportJob job(JobId id, ExportJobState state, double progress = 0.0) {
        ExportJob j;
        j.id = id;
        j.item = id;
        j.source_name = QStringLiteral("IMG_%1.CR3").arg(id);
        j.output_folder = QStringLiteral("C:/Out");
        j.destination_path = QStringLiteral("C:/Out/IMG_%1.jpg").arg(id);
        j.state = state;
        j.progress = progress;
        return j;
    }
};

TEST_F(ExportJobModelTest, ListsJobsInQueueOrderWithCounts) {
    QSignalSpy counts(&model, &ExportJobModel::countsChanged);
    service.add(job(1, ExportJobState::Queued));
    service.add(job(2, ExportJobState::Queued));
    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(role(model, 1, ExportJobModel::SourceNameRole).toString(), QStringLiteral("IMG_2.CR3"));
    EXPECT_EQ(role(model, 0, ExportJobModel::SourceNameRole).toString(), QStringLiteral("IMG_1.CR3"));
    EXPECT_EQ(role(model, 1, ExportJobModel::OutputFolderRole).toString(), QStringLiteral("C:\\Out"));
    EXPECT_EQ(role(model, 1, ExportJobModel::DestinationPathRole).toString(), QStringLiteral("C:\\Out\\IMG_2.jpg"));
    EXPECT_EQ(role(model, 1, ExportJobModel::FormatLabelRole).toString(), QStringLiteral("JPEG · sRGB · quality 95"));
    EXPECT_TRUE(role(model, 1, ExportJobModel::CanCancelRole).toBool());
    EXPECT_TRUE(role(model, 1, ExportJobModel::CanOpenFolderRole).toBool());
    ExportJob cancelling = job(2, ExportJobState::Queued);
    cancelling.cancel_requested = true;
    service.change(cancelling);
    EXPECT_FALSE(role(model, 1, ExportJobModel::CanCancelRole).toBool());  // already asked
    service.change(job(2, ExportJobState::Queued));
    EXPECT_EQ(model.active_count(), 2);
    EXPECT_EQ(counts.count(), 2);

    service.change(job(1, ExportJobState::Running, 0.42));
    EXPECT_EQ(role(model, 0, ExportJobModel::StateKeyRole).toString(), QStringLiteral("running"));
    EXPECT_EQ(role(model, 0, ExportJobModel::StateLabelRole).toString(), QStringLiteral("Exporting"));
    EXPECT_EQ(role(model, 0, ExportJobModel::ProgressLabelRole).toString(), QStringLiteral("42 %"));
    EXPECT_DOUBLE_EQ(role(model, 0, ExportJobModel::ProgressRole).toDouble(), 0.42);

    service.change(job(1, ExportJobState::Completed, 0.5));
    EXPECT_DOUBLE_EQ(role(model, 0, ExportJobModel::ProgressRole).toDouble(), 1.0);
    EXPECT_TRUE(role(model, 0, ExportJobModel::ProgressLabelRole).toString().isEmpty());
    EXPECT_FALSE(role(model, 0, ExportJobModel::CanCancelRole).toBool());
    service.change(job(2, ExportJobState::Failed));
    EXPECT_EQ(model.active_count(), 0);
    EXPECT_EQ(model.completed_count(), 1);
    EXPECT_EQ(model.failed_count(), 1);
    EXPECT_EQ(model.row_of(1), 0);
    EXPECT_EQ(model.job_at(1)->id, JobId{2});
}

TEST_F(ExportJobModelTest, ClampsProgressAndIgnoresUnknownJobs) {
    service.add(job(1, ExportJobState::Running, std::numeric_limits<double>::quiet_NaN()));
    EXPECT_DOUBLE_EQ(role(model, 0, ExportJobModel::ProgressRole).toDouble(), 0.0);
    service.change(job(1, ExportJobState::Running, 1.7));
    EXPECT_DOUBLE_EQ(role(model, 0, ExportJobModel::ProgressRole).toDouble(), 1.0);

    QSignalSpy data_spy(&model, &QAbstractItemModel::dataChanged);
    service.change(job(9, ExportJobState::Completed));
    service.add(job(0, ExportJobState::Queued));
    EXPECT_EQ(data_spy.count(), 0);
    EXPECT_EQ(model.rowCount(), 1);
}

// ---- W4 --------------------------------------------------------------------------------------

TEST(LibraryModelStart, StartsWithTheItemsTheLibraryAlreadyHolds) {
    ScriptedLibraryService service;
    LibraryItem missing = make_item(7, QStringLiteral("GONE.CR3"));
    missing.flags = flag_bit(ItemFlag::MissingSource) | flag_bit(ItemFlag::RemovableSource);
    service.initial = {make_item(5, QStringLiteral("A.CR3")), missing};
    auto registry = std::make_shared<GenerationRegistry>();
    LibraryModel model(service, registry);
    ASSERT_EQ(model.count(), 2);
    EXPECT_EQ(model.item_at(1)->id, 7u);
    EXPECT_EQ(registry->current(7, ImageKind::Thumbnail), Generation{1});
    EXPECT_EQ(model.missing_count(), 1);
    EXPECT_EQ(model.missing_items(), QList<ItemId>{7});
    EXPECT_TRUE(role(model, 1, LibraryModel::MissingRole).toBool());
    EXPECT_EQ(role(model, 1, LibraryModel::FlagKeysRole).toStringList(),
              (QStringList{QStringLiteral("missing"), QStringLiteral("removable")}));
    const QVariantList badges = role(model, 1, LibraryModel::FlagBadgesRole).toList();
    ASSERT_EQ(badges.size(), 2);
    EXPECT_EQ(badges[0].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Missing"));
    EXPECT_FALSE(badges[0].toMap().value(QStringLiteral("detail")).toString().isEmpty());
}

TEST_F(LibraryModelTest, MissingCountFollowsTheItems) {
    QSignalSpy missing(&model, &LibraryModel::missingCountChanged);
    service.add({make_item(1, QStringLiteral("A.CR3")), make_item(2, QStringLiteral("B.CR3"))});
    EXPECT_EQ(model.missing_count(), 0);
    LibraryItem gone = make_item(1, QStringLiteral("A.CR3"));
    gone.flags = flag_bit(ItemFlag::MissingSource);
    service.change(gone);
    EXPECT_EQ(model.missing_count(), 1);
    service.change(make_item(1, QStringLiteral("A.CR3")));  // relinked
    EXPECT_EQ(model.missing_count(), 0);
    EXPECT_EQ(missing.count(), 2);
}

TEST_F(LibraryModelTest, PickAndSourceRolesDescribeTheFile) {
    LibraryItem item = make_item(1, QStringLiteral("A.CR3"));
    item.pick = PickState::Rejected;
    item.copied = true;
    item.origin_path = QStringLiteral("E:\\DCIM\\A.CR3");
    item.duplicate_paths = {QStringLiteral("D:\\Old\\A.CR3")};
    item.flags = flag_bit(ItemFlag::DuplicateContent);
    service.add({item});
    EXPECT_EQ(role(model, 0, LibraryModel::PickKeyRole).toString(), QStringLiteral("rejected"));
    EXPECT_EQ(role(model, 0, LibraryModel::PickLabelRole).toString(), QStringLiteral("Rejected"));
    EXPECT_EQ(role(model, 0, LibraryModel::OriginPathRole).toString(), item.origin_path);
    EXPECT_EQ(role(model, 0, LibraryModel::DuplicatePathsRole).toStringList(), item.duplicate_paths);
    const QString note = role(model, 0, LibraryModel::SourceNoteRole).toString();
    EXPECT_TRUE(note.contains(QStringLiteral("A verified copy of E:\\DCIM\\A.CR3"))) << note.toStdString();
    EXPECT_TRUE(note.contains(QStringLiteral("Same content as D:\\Old\\A.CR3"))) << note.toStdString();

    CurrentItem current(model);
    current.set_row(0);
    EXPECT_EQ(current.pick_key(), QStringLiteral("rejected"));
    EXPECT_EQ(current.flag_keys(), QStringList{QStringLiteral("duplicate")});
    EXPECT_EQ(current.flag_badges().size(), 1);
    EXPECT_FALSE(current.missing());
    EXPECT_EQ(current.source_note(), note);
}

}  // namespace
}  // namespace lucent::ui
