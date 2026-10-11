// Command-line parsing of Lucent.exe (developer_automation.hpp).

#include <gtest/gtest.h>

#include "developer_automation.hpp"

namespace lucent::ui {
namespace {

CommandLine parse(QStringList arguments) {
    arguments.prepend(QStringLiteral("Lucent.exe"));
    return parse_command_line(arguments);
}

TEST(CommandLine, RunsWithDefaultsWithoutArguments) {
    const CommandLine command_line = parse({});
    EXPECT_EQ(command_line.action, CommandLine::Action::Run);
    EXPECT_TRUE(command_line.options.imports.isEmpty());
    EXPECT_FALSE(command_line.options.open_first);
    EXPECT_FALSE(command_line.options.view.has_value());
    EXPECT_TRUE(command_line.options.screenshot_path.isEmpty());
    EXPECT_EQ(command_line.options.settle_timeout_ms, 60000);
}

TEST(CommandLine, ParsesEveryDeveloperOption) {
    const CommandLine command_line =
        parse({"--import", "a", "--import", "b", "--include-subfolders", "--open-first", "--view", "Edit",
               "--export-all", "out", "--screenshot", "shot.PNG", "--quit-after-screenshot", "--settle-timeout",
               "5000", "c.cr3"});
    ASSERT_EQ(command_line.action, CommandLine::Action::Run) << command_line.message.toStdString();
    const DeveloperOptions& options = command_line.options;
    EXPECT_EQ(options.imports, (QStringList{"a", "b", "c.cr3"}));
    EXPECT_TRUE(options.include_subfolders);
    EXPECT_TRUE(options.open_first);
    EXPECT_EQ(options.view, AppController::Edit);
    EXPECT_EQ(options.export_folder, QStringLiteral("out"));
    EXPECT_EQ(options.screenshot_path, QStringLiteral("shot.PNG"));
    EXPECT_TRUE(options.quit_after_screenshot);
    EXPECT_EQ(options.settle_timeout_ms, 5000);
}

TEST(CommandLine, ParsesExportZoomAndMeasureOptions) {
    const CommandLine defaults = parse({});
    EXPECT_EQ(defaults.options.export_options, ExportOptions{});
    EXPECT_FALSE(defaults.options.zoom_full.has_value());
    EXPECT_FALSE(defaults.options.measure);

    const CommandLine command_line = parse({"--export-format", "TIFF16", "--export-color-space", "display-p3",
                                            "--export-quality", "80", "--zoom", "100", "--measure", "--open-first",
                                            "--show-export-dialog"});
    ASSERT_EQ(command_line.action, CommandLine::Action::Run) << command_line.message.toStdString();
    const DeveloperOptions& options = command_line.options;
    EXPECT_EQ(options.export_options.format, ExportFileFormat::Tiff16);
    EXPECT_EQ(options.export_options.color_space, ExportColorSpace::DisplayP3);
    EXPECT_EQ(options.export_options.jpeg_quality, 80);
    EXPECT_EQ(options.zoom_full, true);
    EXPECT_TRUE(options.measure);
    EXPECT_TRUE(options.show_export_dialog);
    EXPECT_EQ(parse({"--zoom", "fit"}).options.zoom_full, false);
    EXPECT_EQ(parse({"--export-format", "jpeg"}).options.export_options.format, ExportFileFormat::Jpeg);
}

TEST(CommandLine, ParsesTheEditingAutomation) {
    const CommandLine command_line =
        parse({"--open-first", "--set", "exposure=+1", "--set", "temperature=5200", "--set",
               "curve=0:0,0.3:0.25,1:1", "--set", "lock=toneCurve", "--drag", "shadows", "--drag-steps", "120",
               "--compare", "split", "--clipping"});
    ASSERT_EQ(command_line.action, CommandLine::Action::Run) << command_line.message.toStdString();
    const DeveloperOptions& options = command_line.options;
    ASSERT_EQ(options.edits.size(), 4u);
    EXPECT_EQ(options.edits[0], std::make_pair(QStringLiteral("exposure"), QStringLiteral("+1")));
    EXPECT_EQ(options.edits[2].first, QStringLiteral("curve"));
    EXPECT_EQ(options.edits[3], std::make_pair(QStringLiteral("lock"), QStringLiteral("toneCurve")));
    EXPECT_EQ(options.drag, QStringLiteral("shadows"));
    EXPECT_EQ(options.drag_steps, 120);
    EXPECT_EQ(options.compare, QStringLiteral("split"));
    EXPECT_TRUE(options.clipping);

    const CommandLine defaults = parse({});
    EXPECT_TRUE(defaults.options.edits.empty());
    EXPECT_TRUE(defaults.options.drag.isEmpty());
    EXPECT_EQ(defaults.options.drag_steps, 60);
    EXPECT_TRUE(defaults.options.compare.isEmpty());
    EXPECT_FALSE(defaults.options.clipping);
}

TEST(CommandLine, AcceptsEveryViewName) {
    EXPECT_EQ(parse({"--view", "library"}).options.view, AppController::Library);
    EXPECT_EQ(parse({"--view", "exports"}).options.view, AppController::Exports);
}

TEST(CommandLine, RejectsInvalidValues) {
    const QList<QStringList> invalid{
        {"--view", "grid"},
        {"--screenshot", "shot.jpg"},
        {"--quit-after-screenshot"},
        {"--settle-timeout", "0"},
        {"--settle-timeout", "soon"},
        {"--export-all", " "},
        {"--no-such-option"},
        {"--view"},
        {"--export-format", "png"},
        {"--export-color-space", "adobe"},
        {"--export-quality", "0"},
        {"--export-quality", "101"},
        {"--zoom", "200"},
        {"--show-export-dialog"},
        {"--set", "exposure=+1"},                      // needs --open-first
        {"--open-first", "--set", "exposure"},         // no value
        {"--open-first", "--set", "sharpness=10"},     // not a control
        {"--open-first", "--set", "exposure=bright"},  // not a number
        {"--open-first", "--set", "curve=0:0"},        // one point
        {"--open-first", "--set", "curve=0:0,2:1"},    // out of range
        {"--open-first", "--set", "lock=grain"},
        {"--open-first", "--drag", "grain"},
        {"--open-first", "--drag", "exposure", "--drag-steps", "5"},
        {"--open-first", "--compare", "sideways"},
        {"--clipping"},
    };
    for (const QStringList& arguments : invalid) {
        const CommandLine command_line = parse(arguments);
        EXPECT_EQ(command_line.action, CommandLine::Action::Error) << arguments.join(' ').toStdString();
        EXPECT_FALSE(command_line.message.isEmpty());
    }
}

TEST(CommandLine, HelpAndVersion) {
    const CommandLine help = parse({"--help"});
    EXPECT_EQ(help.action, CommandLine::Action::ShowHelp);
    EXPECT_TRUE(help.message.contains(QStringLiteral("--screenshot")));
    EXPECT_TRUE(help.message.contains(QStringLiteral("developers")));
    const CommandLine version = parse({"--version"});
    EXPECT_EQ(version.action, CommandLine::Action::ShowVersion);
    EXPECT_TRUE(version.message.startsWith(QStringLiteral("Lucent ")));
}

TEST(CommandLine, ParsesTheBatchAutomation) {
    const CommandLine command_line =
        parse({"--export-batch", "in", "--to", "out", "--preset", "TIFF", "--screenshot-running", "run.png",
               "--quit-when-done"});
    ASSERT_EQ(command_line.action, CommandLine::Action::Run) << command_line.message.toStdString();
    const DeveloperOptions& options = command_line.options;
    EXPECT_EQ(options.batch_folder, QStringLiteral("in"));
    EXPECT_EQ(options.batch_to, QStringLiteral("out"));
    EXPECT_TRUE(options.batch_tiff);
    EXPECT_EQ(options.running_screenshot_path, QStringLiteral("run.png"));
    EXPECT_TRUE(options.quit_when_done);
    EXPECT_FALSE(options.show_batch_dialog);
    EXPECT_FALSE(parse({"--export-batch", "in", "--to", "out", "--preset", "jpeg"}).options.batch_tiff);
    EXPECT_TRUE(parse({"--export-batch", "in", "--to", "out", "--show-batch-dialog"}).options.show_batch_dialog);

    EXPECT_EQ(parse({"--export-batch", "in"}).action, CommandLine::Action::Error);  // no --to
    EXPECT_EQ(parse({"--to", "out"}).action, CommandLine::Action::Error);
    EXPECT_EQ(parse({"--export-batch", "in", "--to", "out", "--preset", "png"}).action, CommandLine::Action::Error);
    EXPECT_EQ(parse({"--export-batch", "in", "--to", "out", "--screenshot-running", "a.jpg"}).action,
              CommandLine::Action::Error);
    EXPECT_EQ(parse({"--export-batch", "in", "--to", "out", "--show-batch-dialog", "--quit-when-done"}).action,
              CommandLine::Action::Error);
}

// W8: Auto in the editor (--auto), automatic batches (--automatic, --keep-consistent).
TEST(CommandLine, ParsesTheAutomaticSettingsAutomation) {
    EXPECT_FALSE(parse({}).options.run_auto);
    EXPECT_EQ(parse({}).options.batch_automatic, BatchAutomatic::Off);
    EXPECT_TRUE(parse({"--open-first", "--auto"}).options.run_auto);
    EXPECT_EQ(parse({"--auto"}).action, CommandLine::Action::Error);  // needs --open-first

    const CommandLine command_line =
        parse({"--export-batch", "in", "--to", "out", "--automatic", "Analyze-Only", "--keep-consistent"});
    ASSERT_EQ(command_line.action, CommandLine::Action::Run) << command_line.message.toStdString();
    EXPECT_EQ(command_line.options.batch_automatic, BatchAutomatic::AnalyzeOnly);
    EXPECT_TRUE(command_line.options.keep_consistent);
    EXPECT_EQ(parse({"--export-batch", "in", "--to", "out", "--automatic", "auto"}).options.batch_automatic,
              BatchAutomatic::Auto);
    EXPECT_EQ(parse({"--export-batch", "in", "--to", "out", "--automatic", "off"}).options.batch_automatic,
              BatchAutomatic::Off);

    EXPECT_EQ(parse({"--export-batch", "in", "--to", "out", "--automatic", "always"}).action,
              CommandLine::Action::Error);
    EXPECT_EQ(parse({"--export-batch", "in", "--to", "out", "--keep-consistent"}).action,
              CommandLine::Action::Error);  // needs automatic settings
    EXPECT_EQ(parse({"--automatic", "auto"}).action, CommandLine::Action::Error);  // needs --export-batch
}

// W12: the measurement hooks of the performance baseline (docs/performance).
TEST(CommandLine, ParsesTheMeasurementHooks) {
    const DeveloperOptions defaults = parse({}).options;
    EXPECT_FALSE(defaults.resume_batch);
    EXPECT_FALSE(defaults.cancel_batch_after.has_value());
    EXPECT_FALSE(defaults.pause_batch_after.has_value());
    EXPECT_EQ(defaults.resume_after_ms, 5000);
    EXPECT_FALSE(defaults.cancel_auto_after_ms.has_value());

    const CommandLine batch = parse({"--export-batch", "in", "--to", "out", "--cancel-batch-after", "3",
                                     "--pause-batch-after", "1", "--resume-after", "250", "--quit-when-done",
                                     "--measure"});
    ASSERT_EQ(batch.action, CommandLine::Action::Run) << batch.message.toStdString();
    EXPECT_EQ(batch.options.cancel_batch_after, 3);
    EXPECT_EQ(batch.options.pause_batch_after, 1);
    EXPECT_EQ(batch.options.resume_after_ms, 250);
    EXPECT_FALSE(batch.options.resume_batch);

    // --resume-batch follows the library's unfinished batch: no folder, no output settings.
    const CommandLine resume = parse({"--resume-batch", "--quit-when-done", "--cancel-batch-after", "0",
                                      "--screenshot-running", "run.png", "--data-root", "root"});
    ASSERT_EQ(resume.action, CommandLine::Action::Run) << resume.message.toStdString();
    EXPECT_TRUE(resume.options.resume_batch);
    EXPECT_TRUE(resume.options.quit_when_done);
    EXPECT_EQ(resume.options.cancel_batch_after, 0);
    EXPECT_TRUE(resume.options.batch_folder.isEmpty());

    const CommandLine automatic = parse({"--open-first", "--auto", "--cancel-auto-after", "1500"});
    ASSERT_EQ(automatic.action, CommandLine::Action::Run) << automatic.message.toStdString();
    EXPECT_EQ(automatic.options.cancel_auto_after_ms, 1500);

    for (const QStringList& invalid : {
             QStringList{"--resume-batch", "--export-batch", "in", "--to", "out"},  // one or the other
             QStringList{"--cancel-batch-after", "2"},                              // no batch to cancel
             QStringList{"--pause-batch-after", "2"},
             QStringList{"--quit-when-done"},
             QStringList{"--resume-batch", "--to", "out"},  // output settings belong to --export-batch
             QStringList{"--resume-batch", "--automatic", "auto"},
             QStringList{"--export-batch", "in", "--to", "out", "--cancel-batch-after", "-1"},
             QStringList{"--export-batch", "in", "--to", "out", "--cancel-batch-after", "many"},
             QStringList{"--export-batch", "in", "--to", "out", "--resume-after", "100"},  // needs a pause
             QStringList{"--export-batch", "in", "--to", "out", "--pause-batch-after", "1", "--resume-after",
                         "700000"},
             QStringList{"--export-batch", "in", "--to", "out", "--show-batch-dialog", "--cancel-batch-after", "1"},
             QStringList{"--open-first", "--cancel-auto-after", "100"},  // needs --auto
             QStringList{"--open-first", "--auto", "--cancel-auto-after", "-5"},
         }) {
        EXPECT_EQ(parse(invalid).action, CommandLine::Action::Error) << invalid.join(' ').toStdString();
    }
}

TEST(CommandLine, ParsesTheGeometryAndMaskAutomation) {
    const CommandLine command_line =
        parse({"--open-first", "--turn", "CW", "--turn", "flip", "--straighten", "-2.5", "--crop", "0.1,0.2,0.7,0.6",
               "--add-mask", "radial:0.5,0.4,0.2,0.1,30", "--add-mask", "linear", "--add-mask", "brush",
               "--mask-set", "maskSaturation=+25", "--mask-exposure", "0.7", "--brush", "0.1,0.2;0.3,0.4",
               "--brush-size", "4", "--mask-overlay", "--tool", "crop"});
    ASSERT_EQ(command_line.action, CommandLine::Action::Run) << command_line.message.toStdString();
    const DeveloperOptions& options = command_line.options;
    EXPECT_EQ(options.turns, (QStringList{"cw", "flip"}));
    EXPECT_EQ(options.straighten, -2.5);
    ASSERT_TRUE(options.crop.has_value());
    EXPECT_DOUBLE_EQ((*options.crop)[2], 0.7);
    EXPECT_EQ(options.add_masks.size(), 3);
    ASSERT_EQ(options.mask_values.size(), 2u);
    EXPECT_EQ(options.mask_values.back().first, QStringLiteral("maskExposure"));
    ASSERT_EQ(options.brushes.size(), 1u);
    EXPECT_EQ(options.brushes.front(), (QList<QPointF>{QPointF(0.1, 0.2), QPointF(0.3, 0.4)}));
    EXPECT_DOUBLE_EQ(options.brush_size, 4.0);
    EXPECT_TRUE(options.mask_overlay);
    EXPECT_EQ(options.tool, QStringLiteral("crop"));

    for (const QStringList& invalid : {
             QStringList{"--open-first", "--turn", "half"},
             QStringList{"--open-first", "--straighten", "50"},
             QStringList{"--open-first", "--crop", "0.5,0.5,0.6,0.2"},
             QStringList{"--open-first", "--add-mask", "radial:0.5,0.5"},
             QStringList{"--open-first", "--add-mask", "square"},
             QStringList{"--open-first", "--mask-set", "exposure=1"},
             QStringList{"--open-first", "--brush", "0.1;0.2"},
             QStringList{"--open-first", "--brush-size", "40"},
             QStringList{"--open-first", "--tool", "lasso"},
             QStringList{"--crop", "0.1,0.1,0.5,0.5"},
         }) {
        EXPECT_EQ(parse(invalid).action, CommandLine::Action::Error) << invalid.join(' ').toStdString();
    }
}

// W13 (ADR 0023): the lens-correction automation.
TEST(CommandLine, ParsesTheLensAutomation) {
    const CommandLine command_line = parse({"--open-first",
                                            "--lens",
                                            "off",
                                            "--lens",
                                            "distortion=ON",
                                            "--lens",
                                            "ca=off",
                                            "--lens",
                                            "vignetting=on",
                                            "--lens",
                                            "profile=Nikkor Z 24-70",
                                            "--lens",
                                            "auto",
                                            "--lens",
                                            "on",
                                            "--lens",
                                            "all",
                                            "--lens",
                                            "search=sigma 50",
                                            "--show-panel",
                                            "Lens"});
    ASSERT_EQ(command_line.action, CommandLine::Action::Run) << command_line.message.toStdString();
    const DeveloperOptions& options = command_line.options;
    EXPECT_EQ(options.lens_edits,
              (QStringList{"off", "distortion=ON", "ca=off", "vignetting=on", "profile=Nikkor Z 24-70",
                           "auto", "on", "all", "search=sigma 50"}));
    EXPECT_EQ(options.show_panel, QStringLiteral("lens"));
    for (const QStringList& invalid : {
             QStringList{"--open-first", "--lens", "maybe"},
             QStringList{"--open-first", "--lens", "distortion=half"},
             QStringList{"--open-first", "--lens", "profile="},
             QStringList{"--open-first", "--lens", "search="},
             QStringList{"--open-first", "--show-panel", "history"},
             QStringList{"--lens", "off"},
             QStringList{"--show-panel", "lens"},
         }) {
        EXPECT_EQ(parse(invalid).action, CommandLine::Action::Error) << invalid.join(' ').toStdString();
    }
}

}  // namespace
}  // namespace lucent::ui
