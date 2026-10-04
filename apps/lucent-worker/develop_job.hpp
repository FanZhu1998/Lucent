#pragma once

// The job lucent-worker.exe runs: render a library photograph with a frozen recipe through the
// develop pipeline and write one verified output file (blueprint section 7, "Export integrity").
// It is the export render of app::ExportService without publication: the host publishes the file
// afterwards. Kept in a library so tests can run the same code in-process.
//
// DevelopJobRunner::run(), in order (progress stages in brackets):
//  [Preparing]  the recipe is read strictly (app::read_recipe) and must be the canonical document
//               of its settings with the request's SHA-256 (so the identity recorded is exactly
//               what is rendered: InvalidArgument, "worker.job.recipe"); temp_path's directory must
//               exist (NotFound) and neither temp_path nor its scratch directory may exist
//               (AlreadyExists); the source must have the expected identity (SourceChanged;
//               NotFound when it is gone) and, when the request carries a SHA-256, hash to it with a
//               stable read; the source's capture metadata is read (raw::read_capture_metadata,
//               completed from a probe); the engine estimates the render's memory.
//  [Rendering]  app::SourceRenderer::render (render_develop_file's composition); an image whose
//               decoded interpretation is Unsupported fails with Unsupported ("worker.job.support")
//               unless allow_unsupported.
//  [Encoding]   exporter::encode_for_output to the output profile and bit depth.
//  [Writing]    a free-space check (the uncompressed size: DiskFull), then exporter::write_verified
//               into the scratch directory (temporary file, flush, re-read verification, metadata
//               check, SHA-256), a no-clobber move to temp_path, and removal of the scratch
//               directory.
//  [Verifying]  temp_path is hashed again and must match the exporter's receipt (Corrupt,
//               "worker.job.verify"); the source must still have the expected identity
//               (SourceChanged).
// On any failure or cancellation the job removes what it created (job_output.hpp) before returning.
// Cancellation is observed between the steps and inside the renderer, encoder and writer.
//
// Thread-safety: run() may be called concurrently (each call has its own state); the worker calls
// it on one thread.

#include <memory>

#include <lucent/raw/decoder_registry.hpp>
#include <lucent/raw/raw_decoder.hpp>
#include <lucent/workers/worker_server.hpp>

namespace lucent::worker_app {

struct DevelopJobOptions {
    raw::DecodeLimits decode_limits;
    bool parallel = true;
};

class DevelopJobRunner final : public workers::JobRunner {
public:
    // InvalidArgument from run() when `registry` is null.
    explicit DevelopJobRunner(std::shared_ptr<const raw::DecoderRegistry> registry, DevelopJobOptions options = {});

    [[nodiscard]] Expected<workers::JobResult> run(const workers::JobRequest& request,
                                                   workers::JobContext& context) override;

private:
    std::shared_ptr<const raw::DecoderRegistry> registry_;
    DevelopJobOptions options_;
};

}  // namespace lucent::worker_app
