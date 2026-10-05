#pragma once

// The analyze job lucent-worker.exe runs (protocol version 2, workers/protocol.hpp): the automatic
// settings of one library photograph, isolated from Lucent.exe (blueprint section 4; ADR 0013).
//
// run_analysis_job(), in order (progress stages in brackets):
//  [Analysing]  the starting recipe must be the canonical document of its SHA-256 (InvalidArgument,
//               "worker.job.recipe"); the request document must be a readable "lucent.auto-request"
//               (InvalidArgument, "worker.analysis.request"); the source must have the expected
//               identity (SourceChanged; NotFound when it is gone) and, with a SHA-256, hash to it;
//               then app::run_automatic(): decode, analysis, related-shot regularisation with the
//               request's target, the anchor's appearance;
//  [Checking]   the full-resolution final check and its fallbacks (when the request asks for it);
//               the worker's fail-final-check hook makes its first attempts fail (and its
//               render-delay hook delays the analysis once before it starts);
// then the source must still have its identity (SourceChanged), and the answer carries the
// "lucent.auto-run" document. Nothing is written to disk. Cancellation is observed inside the
// analysis and the check.
//
// Thread-safety: reentrant (each call has its own state).

#include <memory>

#include <lucent/raw/decoder_registry.hpp>
#include <lucent/raw/raw_decoder.hpp>
#include <lucent/workers/worker_server.hpp>

namespace lucent::worker_app {

struct AnalysisJobOptions {
    raw::DecodeLimits decode_limits;
    bool parallel = true;
};

[[nodiscard]] Expected<workers::AnalysisResult> run_analysis_job(const raw::DecoderRegistry& registry,
                                                                 const workers::AnalysisRequest& request,
                                                                 workers::JobContext& context,
                                                                 const AnalysisJobOptions& options = {});

}  // namespace lucent::worker_app
