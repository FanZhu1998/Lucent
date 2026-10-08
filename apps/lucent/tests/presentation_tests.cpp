// User-facing labels (en_US locale, see test_main.cpp).

#include <gtest/gtest.h>

#include <limits>

#include <QLocale>

#include "models/presentation.hpp"
#include "test_support.hpp"

namespace lucent::ui::presentation {
namespace {

const QLocale kLocale(QLocale::English, QLocale::UnitedStates);

TEST(Presentation, ShutterSpeedsReadLikeACamera) {
    EXPECT_EQ(shutter_label(1.0 / 250, kLocale), QStringLiteral("1/250 s"));
    EXPECT_EQ(shutter_label(0.004, kLocale), QStringLiteral("1/250 s"));
    EXPECT_EQ(shutter_label(1.0 / 8000, kLocale), QStringLiteral("1/8000 s"));
    EXPECT_EQ(shutter_label(0.5, kLocale), QStringLiteral("1/2 s"));
    EXPECT_EQ(shutter_label(0.3, kLocale), QStringLiteral("0.3 s"));
    EXPECT_EQ(shutter_label(0.4, kLocale), QStringLiteral("0.4 s"));
    EXPECT_EQ(shutter_label(1.0, kLocale), QStringLiteral("1 s"));
    EXPECT_EQ(shutter_label(1.3, kLocale), QStringLiteral("1.3 s"));
    EXPECT_EQ(shutter_label(30.0, kLocale), QStringLiteral("30 s"));
    EXPECT_TRUE(shutter_label(0.0, kLocale).isEmpty());
    EXPECT_TRUE(shutter_label(-1.0, kLocale).isEmpty());
    EXPECT_TRUE(shutter_label(std::numeric_limits<double>::quiet_NaN(), kLocale).isEmpty());
    EXPECT_TRUE(shutter_label(std::numeric_limits<double>::infinity(), kLocale).isEmpty());
}

TEST(Presentation, ApertureAndLocaleDecimalSeparator) {
    EXPECT_EQ(aperture_label(4.0, kLocale), QStringLiteral("f/4"));
    EXPECT_EQ(aperture_label(5.6, kLocale), QStringLiteral("f/5.6"));
    EXPECT_EQ(aperture_label(1.8, kLocale), QStringLiteral("f/1.8"));
    EXPECT_EQ(aperture_label(5.6, QLocale(QLocale::German, QLocale::Germany)), QStringLiteral("f/5,6"));
    EXPECT_TRUE(aperture_label(0.0, kLocale).isEmpty());
}

TEST(Presentation, CameraLabelAvoidsRepeatingTheMake) {
    LibraryItem item;
    EXPECT_EQ(camera_label(item), QStringLiteral("Unknown camera"));
    item.camera_make = QStringLiteral("Canon");
    EXPECT_EQ(camera_label(item), QStringLiteral("Canon"));
    item.camera_model = QStringLiteral("EOS R5");
    EXPECT_EQ(camera_label(item), QStringLiteral("Canon EOS R5"));
    item.camera_make = QStringLiteral("NIKON CORPORATION");
    item.camera_model = QStringLiteral("NIKON Z 6");
    EXPECT_EQ(camera_label(item), QStringLiteral("NIKON Z 6"));
    item.camera_make = QStringLiteral("Nikon");
    EXPECT_EQ(camera_label(item), QStringLiteral("NIKON Z 6"));
    item.camera_make.clear();
    EXPECT_EQ(camera_label(item), QStringLiteral("NIKON Z 6"));
    item.camera_make = QStringLiteral("Nikon");
    item.camera_model = QStringLiteral("Z 8");
    EXPECT_EQ(camera_label(item), QStringLiteral("Nikon Z 8"));
}

TEST(Presentation, ExposureDimensionsSizeAndTime) {
    LibraryItem item = testing::make_item(1, QStringLiteral("A.CR3"));
    EXPECT_EQ(exposure_label(item, kLocale), QStringLiteral("1/250 s · f/5.6 · ISO 400 · 35 mm"));
    EXPECT_EQ(dimensions_label(item, kLocale), QStringLiteral("6000 × 4000 · 24.0 MP"));
    EXPECT_EQ(file_size_label(item.file_size_bytes, kLocale), QStringLiteral("1.4 MB"));
    EXPECT_FALSE(capture_time_label(item, kLocale).isEmpty());

    item.iso = 0;
    item.f_number = 0;
    EXPECT_EQ(exposure_label(item, kLocale), QStringLiteral("1/250 s · 35 mm"));
    item = LibraryItem{};
    EXPECT_TRUE(exposure_label(item, kLocale).isEmpty());
    EXPECT_TRUE(dimensions_label(item, kLocale).isEmpty());
    EXPECT_TRUE(file_size_label(item.file_size_bytes, kLocale).isEmpty());
    EXPECT_EQ(capture_time_label(item, kLocale), QStringLiteral("Capture time unknown"));
}

TEST(Presentation, SupportStatesUseTheBlueprintWording) {
    EXPECT_EQ(support_label(SupportStatus::Verified), QStringLiteral("Verified"));
    EXPECT_EQ(support_label(SupportStatus::CompatibleUnverified), QStringLiteral("Compatible, unverified"));
    EXPECT_EQ(support_short_label(SupportStatus::CompatibleUnverified), QStringLiteral("Unverified"));
    EXPECT_EQ(support_label(SupportStatus::Limited), QStringLiteral("Limited"));
    EXPECT_EQ(support_label(SupportStatus::Unsupported), QStringLiteral("Unsupported"));
    EXPECT_EQ(support_label(SupportStatus::Unknown), QStringLiteral("Not checked"));
    EXPECT_EQ(support_key(SupportStatus::CompatibleUnverified), QStringLiteral("unverified"));
    EXPECT_EQ(support_key(static_cast<SupportStatus>(200)), QStringLiteral("unknown"));
}

TEST(Presentation, EverySupportStateSaysWhatItMeans) {
    QStringList meanings;
    for (const SupportStatus status :
         {SupportStatus::Verified, SupportStatus::CompatibleUnverified, SupportStatus::Limited,
          SupportStatus::Unsupported, SupportStatus::Unknown}) {
        const QString meaning = support_meaning(status);
        EXPECT_FALSE(meaning.isEmpty());
        EXPECT_FALSE(meanings.contains(meaning));
        meanings << meaning;
    }
    EXPECT_TRUE(support_meaning(SupportStatus::Limited).contains(QStringLiteral("review note")));
    EXPECT_TRUE(support_meaning(SupportStatus::Unsupported).contains(QStringLiteral("skip")));

    // Help > About lists the four states best first, each with its key, label and meaning.
    const QVariantList states = support_states();
    ASSERT_EQ(states.size(), 4);
    EXPECT_EQ(states[0].toMap().value(QStringLiteral("key")).toString(), QStringLiteral("verified"));
    EXPECT_EQ(states[3].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Unsupported"));
    EXPECT_EQ(states[2].toMap().value(QStringLiteral("meaning")).toString(),
              support_meaning(SupportStatus::Limited));
}

TEST(Presentation, AboutSummarisesTheCameraSupportDatabase) {
    CameraSupportInfo info;
    info.available = true;
    info.version = QStringLiteral("1.0.0");
    info.qualified_on = QStringLiteral("2026-10-08");
    info.modes = 34;
    info.verified = 14;
    info.compatible_unverified = 11;
    info.limited = 6;
    info.unsupported = 3;
    QVariantMap map = camera_support_map(info);
    EXPECT_TRUE(map.value(QStringLiteral("available")).toBool());
    EXPECT_EQ(map.value(QStringLiteral("version")).toString(), QStringLiteral("1.0.0"));
    const QString summary = map.value(QStringLiteral("summary")).toString();
    EXPECT_TRUE(summary.contains(QStringLiteral("Version 1.0.0, qualified 2026-10-08")))
        << summary.toStdString();
    EXPECT_TRUE(summary.contains(QStringLiteral("14 verified")));
    info.verified_entries_hold = false;
    EXPECT_TRUE(camera_support_map(info)
                    .value(QStringLiteral("summary"))
                    .toString()
                    .contains(QStringLiteral("another version")));

    CameraSupportInfo none;
    none.problem = QStringLiteral("The installed camera support database failed its integrity check.");
    map = camera_support_map(none);
    EXPECT_FALSE(map.value(QStringLiteral("available")).toBool());
    EXPECT_EQ(map.value(QStringLiteral("summary")).toString(), none.problem);
}

TEST(Presentation, FlagsAreListedInBitOrder) {
    EXPECT_TRUE(flag_labels(0).isEmpty());
    const ItemFlags all = flag_bit(ItemFlag::ReviewRecommended) | flag_bit(ItemFlag::MissingSource) |
                          flag_bit(ItemFlag::RemovableSource) | flag_bit(ItemFlag::DuplicateContent);
    EXPECT_EQ(flag_labels(all), (QStringList{QStringLiteral("Review recommended"), QStringLiteral("Missing file"),
                                             QStringLiteral("Removable media"), QStringLiteral("Duplicate")}));
}

TEST(Presentation, JobStatesAndExportOptions) {
    EXPECT_EQ(job_state_key(ExportJobState::Running), QStringLiteral("running"));
    EXPECT_EQ(job_state_label(ExportJobState::Completed), QStringLiteral("Exported"));
    EXPECT_EQ(export_options_label(ExportOptions{}), QStringLiteral("JPEG · sRGB · quality 95"));
    EXPECT_EQ(export_options_label(ExportOptions{ExportFileFormat::Jpeg, ExportColorSpace::DisplayP3, 80}),
              QStringLiteral("JPEG · Display P3 · quality 80"));
    EXPECT_EQ(export_options_label(ExportOptions{ExportFileFormat::Tiff16, ExportColorSpace::DisplayP3}),
              QStringLiteral("TIFF 16-bit · Display P3"));
    EXPECT_TRUE(is_active(ExportJobState::Queued));
    EXPECT_FALSE(is_active(ExportJobState::Cancelled));
}

TEST(Presentation, FileNamesOfMultiImageFilesNameTheImage) {
    LibraryItem item;
    item.file_path = QStringLiteral(R"(C:\Photos\DSC_0001.ARW)");
    EXPECT_EQ(file_name(item), QStringLiteral("DSC_0001.ARW"));
    item.subimage = 1;
    item.subimage_count = 3;
    EXPECT_EQ(file_name(item), QStringLiteral("DSC_0001.ARW (image 2 of 3)"));
}

TEST(Presentation, RenderLabelNamesTheRenderingAndItsTime) {
    const QLocale locale(QLocale::English, QLocale::UnitedStates);
    RenderInfo info;
    EXPECT_EQ(render_label(info, locale), QStringLiteral("Lucent render"));
    info.rendering = QStringLiteral("Neutral v0");
    info.provisional = true;
    info.render_seconds = 0.954;
    EXPECT_EQ(render_label(info, locale), QStringLiteral("Neutral v0 (provisional) · rendered in 0.95 s"));
}

TEST(Presentation, SourceBadgesAreOrderedByUrgency) {
    using namespace presentation;
    const ItemFlags all = flag_bit(ItemFlag::RemovableSource) | flag_bit(ItemFlag::DuplicateContent) |
                          flag_bit(ItemFlag::ChangedSource) | flag_bit(ItemFlag::MissingSource) |
                          flag_bit(ItemFlag::ReviewRecommended);
    EXPECT_EQ(flag_keys(all), (QStringList{QStringLiteral("missing"), QStringLiteral("changed"),
                                           QStringLiteral("duplicate"), QStringLiteral("removable")}));
    EXPECT_TRUE(flag_keys(flag_bit(ItemFlag::ReviewRecommended)).isEmpty());
    for (const QString& key : flag_keys(all)) {
        EXPECT_FALSE(flag_badge_label(key).isEmpty()) << key.toStdString();
        EXPECT_FALSE(flag_badge_detail(key).isEmpty()) << key.toStdString();
    }
    EXPECT_TRUE(flag_labels(flag_bit(ItemFlag::ChangedSource)).contains(QStringLiteral("Changed on disk")));
    EXPECT_EQ(pick_key(PickState::Picked), QStringLiteral("picked"));
    EXPECT_TRUE(pick_label(PickState::None).isEmpty());

    LibraryItem item;
    EXPECT_TRUE(source_note(item).isEmpty());
    item.flags = flag_bit(ItemFlag::MissingSource);
    EXPECT_TRUE(source_note(item).contains(QStringLiteral("Relink")));
}

}  // namespace
}  // namespace lucent::ui::presentation
