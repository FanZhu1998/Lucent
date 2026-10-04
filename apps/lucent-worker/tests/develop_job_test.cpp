// DevelopJobRunner in-process on synthetic DNGs: the output it writes, what it verifies, and that
// a failed or cancelled job leaves nothing behind (develop_job.hpp).

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include <lucent/application/recipe.hpp>
#include <lucent/export/metadata.hpp>
#include <lucent/workers/job_output.hpp>

#include "develop_job.hpp"
#include "worker_test_support.hpp"

namespace lucent::worker_app {
namespace {

using namespace test_support;
using workers::JobStage;

class DevelopJob : public ::testing::Test {
protected:
    DevelopJob() : runner_(default_registry()) {
        source_ = write_dng(dir_, L"IMG_0001.dng");
        temp_ = dir_.path() / L"out" / L"~lucent-job-1.partial";
        rts::make_directories(temp_.parent_path());
    }

    void expect_nothing_left() const {
        EXPECT_FALSE(path_exists(temp_));
        EXPECT_FALSE(path_exists(workers::job_scratch_directory(temp_)));
    }

    rts::ScratchDir dir_;
    DevelopJobRunner runner_;
    std::filesystem::path source_;
    std::filesystem::path temp_;
};

TEST_F(DevelopJob, WritesAVerifiedJpegAtTheTempPath) {
    LocalContext context;
    auto job = make_job(source_, temp_);
    auto result = runner_.run(job, context);
    ASSERT_TRUE(result) << result.error().describe();

    EXPECT_EQ(result->output.temp_path, temp_);
    ASSERT_TRUE(path_exists(temp_));
    EXPECT_FALSE(path_exists(workers::job_scratch_directory(temp_)));
    EXPECT_EQ(result->output.sha256, file_digest(temp_));
    EXPECT_EQ(result->output.size_bytes, std::filesystem::file_size(temp_));
    EXPECT_EQ(result->output.format, exporter::ExportFormat::Jpeg);
    EXPECT_EQ(result->output.profile, exporter::OutputProfile::Srgb);
    EXPECT_EQ(result->output.bit_depth, exporter::BitDepth::Eight);
    EXPECT_EQ(result->output.chroma_subsampling, exporter::ChromaSubsampling::Yuv444);
    EXPECT_GT(result->output.width, 0u);
    EXPECT_EQ(result->output.width, result->render.width);
    EXPECT_EQ(result->output.height, result->render.height);
    EXPECT_FALSE(result->output.encoder.empty());
    EXPECT_TRUE(result->verification.decoded && result->verification.compared && result->verification.rehashed &&
                result->verification.source_unchanged);
    EXPECT_EQ(result->source_identity, job.source.identity);
    EXPECT_FALSE(result->source_sha256.has_value());
    EXPECT_EQ(result->recipe_sha256, job.recipe.sha256);
    EXPECT_EQ(result->render.look, "neutral");
    EXPECT_GT(result->render.estimated_peak_bytes, 0u);
    EXPECT_GT(result->timings.total_seconds, 0.0);
    EXPECT_GT(result->memory.peak_working_set, 0u);

    // Progress went through every stage in order and ended at 1.
    ASSERT_FALSE(context.reports.empty());
    EXPECT_EQ(context.reports.front().first, JobStage::Preparing);
    EXPECT_EQ(context.reports.back(), std::pair(JobStage::Verifying, 1.0));
    for (std::size_t i = 1; i < context.reports.size(); ++i) {
        EXPECT_GE(context.reports[i].first, context.reports[i - 1].first);
        EXPECT_GE(context.reports[i].second, context.reports[i - 1].second);
    }
}

TEST_F(DevelopJob, WritesA16BitTiffInDisplayP3) {
    LocalContext context;
    auto job = make_job(source_, temp_);
    job.output_settings = exporter::ExportSettings::tiff16(exporter::OutputProfile::DisplayP3);
    auto result = runner_.run(job, context);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->output.format, exporter::ExportFormat::Tiff);
    EXPECT_EQ(result->output.profile, exporter::OutputProfile::DisplayP3);
    EXPECT_EQ(result->output.bit_depth, exporter::BitDepth::Sixteen);
    EXPECT_FALSE(result->output.chroma_subsampling.has_value());
    EXPECT_EQ(result->output.sha256, file_digest(temp_));
}

TEST_F(DevelopJob, RendersTheRecipeAndChecksTheSourceHash) {
    engine::DevelopSettings brighter;
    brighter.exposure_ev = 1.0;
    LocalContext context;
    auto job = make_job(source_, temp_, "job-1", brighter);
    auto hashed = core::hash_file(source_, {});
    ASSERT_TRUE(hashed);
    job.source.sha256 = hashed->digest;
    auto result = runner_.run(job, context);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->source_sha256, hashed->digest);
    auto document = app::write_recipe(brighter);
    ASSERT_TRUE(document);
    EXPECT_EQ(result->recipe_sha256, document->sha256);

    // The same job with the default recipe produces another file.
    LocalContext again;
    const auto other_temp = temp_.parent_path() / L"~lucent-job-2.partial";
    auto plain = runner_.run(make_job(source_, other_temp, "job-2"), again);
    ASSERT_TRUE(plain) << plain.error().describe();
    EXPECT_NE(plain->output.sha256, result->output.sha256);
}

TEST_F(DevelopJob, CarriesCaptureMetadataAndTheUsersRights) {
    LocalContext context;
    auto job = make_job(source_, temp_);
    job.artist = "Test Artist";
    job.copyright = "(c) 2026 Test";
    auto result = runner_.run(job, context);
    ASSERT_TRUE(result) << result.error().describe();
    auto metadata = exporter::read_export_metadata(temp_);
    ASSERT_TRUE(metadata) << metadata.error().describe();
    EXPECT_EQ(metadata->camera_make, "Lucent");
    EXPECT_EQ(metadata->camera_model, "Synthetic DNG");
    EXPECT_EQ(metadata->artist, "Test Artist");
    EXPECT_EQ(metadata->copyright, "(c) 2026 Test");
    EXPECT_FALSE(metadata->gps.has_value());
}

TEST_F(DevelopJob, RefusesARecipeThatIsNotCanonical) {
    LocalContext context;
    auto job = make_job(source_, temp_);
    job.recipe.document = " " + job.recipe.document;  // same settings, other text
    job.recipe.sha256 = digest_of(job.recipe.document);
    auto result = runner_.run(job, context);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(result.error().where, "worker.job.recipe");
    expect_nothing_left();
}

TEST_F(DevelopJob, RefusesAChangedSource) {
    LocalContext context;
    auto job = make_job(source_, temp_);
    job.source.identity.size_bytes += 1;
    auto changed = runner_.run(job, context);
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().code, ErrorCode::SourceChanged);
    expect_nothing_left();

    job = make_job(source_, temp_);
    job.source.sha256 = core::Digest256{};  // not its content
    auto other_content = runner_.run(job, context);
    ASSERT_FALSE(other_content);
    EXPECT_EQ(other_content.error().code, ErrorCode::SourceChanged);
    expect_nothing_left();

    job = make_job(source_, temp_);
    std::filesystem::remove(source_);
    auto gone = runner_.run(job, context);
    ASSERT_FALSE(gone);
    EXPECT_EQ(gone.error().code, ErrorCode::NotFound);
    expect_nothing_left();
}

TEST_F(DevelopJob, NeverReplacesAnExistingFile) {
    {
        std::ofstream existing(temp_, std::ios::binary);
        existing << "keep";
    }
    LocalContext context;
    auto result = runner_.run(make_job(source_, temp_), context);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(std::filesystem::file_size(temp_), 4u);

    std::filesystem::remove(temp_);
    rts::make_directories(workers::job_scratch_directory(temp_));  // an earlier attempt's leftovers
    auto leftover = runner_.run(make_job(source_, temp_), context);
    ASSERT_FALSE(leftover);
    EXPECT_EQ(leftover.error().code, ErrorCode::AlreadyExists);
    EXPECT_TRUE(path_exists(workers::job_scratch_directory(temp_)));  // not the job's to remove
}

TEST_F(DevelopJob, NeedsAnExistingOutputFolder) {
    LocalContext context;
    auto result = runner_.run(make_job(source_, dir_.path() / L"missing" / L"x.partial"), context);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::NotFound);
}

TEST_F(DevelopJob, CancellationBeforeTheStartLeavesNothing) {
    LocalContext context;
    context.source.cancel();
    auto result = runner_.run(make_job(source_, temp_), context);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
    expect_nothing_left();
}

TEST_F(DevelopJob, CancellationDuringTheRenderLeavesNothing) {
    workers::TestHooks hooks;
    hooks.render_delay_ms = 2000;
    LocalContext context(hooks);
    std::atomic<bool> rendering{false};
    context.on_progress = [&](JobStage stage, double) {
        if (stage == JobStage::Rendering && !rendering.exchange(true)) {
            context.source.cancel();
        }
    };
    const auto start = std::chrono::steady_clock::now();
    auto result = runner_.run(make_job(source_, temp_), context);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
    EXPECT_TRUE(rendering.load());
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms);  // the delay observed the cancellation
    expect_nothing_left();
}

}  // namespace
}  // namespace lucent::worker_app
