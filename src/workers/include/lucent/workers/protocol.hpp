#pragma once

// The worker protocol "lucent.worker", version 2: the messages exchanged between a host
// (WorkerHost, worker_host.hpp, used by the application's batch service) and an isolated
// lucent-worker.exe process (blueprint section 4: "The UI and worker negotiate a protocol version.
// Package matching binaries together; fail clearly if incompatible components are mixed";
// section 7, "Durable, memory-bounded batch processing").
//
// Transport. Two anonymous pipes per worker, one per direction, created by the host and inherited
// by the worker alone (worker_host.hpp). Each message is one frame: a 4-byte little-endian
// unsigned payload length N with 1 <= N <= kMaxMessageBytes, then N bytes of UTF-8 JSON holding
// one object. Pixels never cross the pipe: the worker reads the source file and writes the output
// file itself; only paths, identities, settings, hashes and measurements are exchanged.
//
// Envelope. Every message is {"type": "<wire name>", ...members}. All members are always present
// (an absent value is JSON null where the member is optional). Enumerations are written as their
// persisted numeric values (lucent::ErrorCode, exporter enums, raw::InterpretationStatus and the
// enums below), 64-bit identity fields and digests as lowercase hex strings, sizes and counts as
// unsigned integers, paths as UTF-8 text of absolute Windows paths.
//
// Strict reading (decode_message). A frame or document is Corrupt ("workers.protocol.*") when it
// is not one JSON object, repeats a member name, lacks a member, has an unknown member or message
// type, a member of the wrong JSON type, a value out of range, an unknown enumerator, a string
// that is not valid UTF-8 or longer than its limit, or a message for the other direction. Unknown
// members are never ignored: both sides are the same build (the handshake guarantees it), so an
// unknown member is a defect, never an extension.
//
// Handshake. Frozen: every future protocol version starts with exactly these two messages with
// these members, so a mismatched pair can always report the mismatch.
//   host   -> worker  host_hello   {"protocol": "lucent.worker", "min_version": 1,
//                                   "max_version": 1, "build": {...}, "heartbeat_ms": 500,
//                                   "host_pid": 1234}
//   worker -> host    worker_hello {"protocol": "lucent.worker", "version": 1, "build": {...},
//                                   "pid": 5678, "console_window": false, "test_hooks": [...]}
//                  or error        {"job_id": null, "code": 14, "where":
//                                   "workers.handshake.version" | "workers.handshake.build", ...}
//                                  after which the worker exits with WorkerExitCode::Refused.
// The worker selects the highest version both ranges contain (negotiate_version) and requires the
// host's BuildIdentity to equal its own; the host checks the worker's answer the same way. A
// mismatch is VersionMismatch with a message naming both builds.
//
// After the handshake (version 2):
//   host -> worker:  job, analyze, cancel, ping, shutdown
//   worker -> host:  progress, heartbeat, result, analysis, error, pong
// A worker runs one job at a time (an export "job" or an "analyze" job: both are jobs below); the
// host sends a job only while none is running. The worker sends a heartbeat every heartbeat_ms from
// a thread independent of the job, from the handshake until it exits, so a missing heartbeat means
// the process is unresponsive (not merely busy). Exactly one answer (result for a job, analysis for
// an analyze job) or error answers each job; cancel names the job and is a no-op when it is no
// longer running (the answer may already be on its way). A job's error carries a FailureClass.
//
// Version history: 1 (W7) job, cancel, ping, shutdown / progress, heartbeat, result, error, pong.
// 2 (W8 application wiring) adds analyze / analysis (automatic settings in the worker: the
// analysis, related-shot regularisation and the full-resolution final check of one photo) and the
// progress stages Analysing and Checking. Every version-1 message is unchanged.
//
// Analysis documents. An analyze job carries the application's request document and its analysis
// answer carries the application's result document, each as one JSON text in a string member
// ("lucent.auto-request" and "lucent.auto-run", application/auto_develop.hpp). The protocol checks
// that they are non-empty UTF-8 within their limits; their content is read strictly by the
// application (both ends are the same build). Analyze jobs write no file.
//
// Output files (job_output.hpp). The worker creates exactly the job's `temp_path` (no-clobber: it
// fails with AlreadyExists rather than replace anything) and, while writing, the scratch directory
// job_scratch_directory(temp_path) beside it, which it removes before answering. It never writes
// anywhere else, never publishes, and never touches the source. After a worker is lost mid-job the
// host removes both (remove_job_output).
//
// Thread-safety: everything here is a pure function of its arguments or a plain value type.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/export/export.hpp>
#include <lucent/export/metadata.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::workers {

// ---- Versions and limits ----------------------------------------------------------------------

inline constexpr std::string_view kProtocolName = "lucent.worker";
// Protocol versions this build speaks. Bump kMaxProtocolVersion for an incompatible change of any
// message after the handshake; the handshake itself never changes.
inline constexpr std::uint32_t kMinProtocolVersion = 2;
inline constexpr std::uint32_t kMaxProtocolVersion = 2;

inline constexpr std::size_t kFrameHeaderBytes = 4;
// Largest JSON payload of one frame. The largest legitimate message is a job request with two
// long paths and a recipe (a few KiB); the limit leaves room for 32,767-character paths.
inline constexpr std::uint32_t kMaxMessageBytes = 1u << 20;  // 1 MiB

// Member limits (bytes of UTF-8 unless stated otherwise).
inline constexpr std::size_t kMaxJobIdBytes = 64;           // [A-Za-z0-9._:-]+
inline constexpr std::size_t kMaxPathBytes = 32767 * 3;     // a 32,767-character path
inline constexpr std::size_t kMaxRecipeBytes = 256 * 1024;  // canonical recipe document
// The application's documents of an analyze job (see "Analysis documents" above).
inline constexpr std::size_t kMaxAnalysisRequestBytes = 128 * 1024;
inline constexpr std::size_t kMaxAnalysisDocumentBytes = 768 * 1024;
inline constexpr std::size_t kMaxErrorMessageBytes = 4096;  // longer messages are truncated
inline constexpr std::size_t kMaxWhereBytes = 128;
inline constexpr std::size_t kMaxShortTextBytes = 256;      // build fields, encoder, algorithm ids
inline constexpr std::size_t kMaxWarnings = 64;
inline constexpr std::size_t kMaxWarningBytes = 1024;       // longer warnings are truncated
inline constexpr std::size_t kMaxTestHooks = 16;
inline constexpr std::uint32_t kMinHeartbeatMs = 10;
inline constexpr std::uint32_t kMaxHeartbeatMs = 60'000;

// Exit codes of lucent-worker.exe. A crashed worker exits with its exception code instead
// (for example 0xC0000005), which never collides with these.
enum class WorkerExitCode : std::uint32_t {
    Clean = 0,              // shutdown requested by the host
    ParentGone = 2,         // the host closed the pipe or ended: the worker never stays orphaned
    Refused = 3,            // the handshake failed (version or build mismatch)
    ProtocolViolation = 4,  // the host sent something the protocol forbids
    Usage = 5,              // started without a valid worker command line (worker_launch.hpp)
    Internal = 6,           // the worker could not initialise
};

// ---- Builds -----------------------------------------------------------------------------------

// The identity of a build (core::BuildInfo). Host and worker must be the same build: packaged
// together, never mixed (blueprint section 4).
struct BuildIdentity {
    std::string version;     // product SemVer, e.g. "0.1.0"
    std::string git_commit;  // abbreviated commit at configure time, or "unknown"
    std::string compiler;    // e.g. "MSVC 19.44.35229.0"
    std::string build_type;  // e.g. "RelWithDebInfo"

    friend bool operator==(const BuildIdentity&, const BuildIdentity&) = default;
};

// This build (core::build_info()).
[[nodiscard]] BuildIdentity current_build_identity();

// "0.1.0 (commit 1a2b3c4, MSVC 19.44.35229.0, RelWithDebInfo)" for messages and logs.
[[nodiscard]] std::string describe(const BuildIdentity& build);

// The highest version within both [host_min, host_max] and [worker_min, worker_max];
// VersionMismatch ("workers.handshake.version") when the ranges do not overlap, InvalidArgument
// when a range is empty (min > max) or starts at 0.
[[nodiscard]] Expected<std::uint32_t> negotiate_version(std::uint32_t host_min, std::uint32_t host_max,
                                                        std::uint32_t worker_min, std::uint32_t worker_max);

// ok() when `peer` equals `self`; otherwise VersionMismatch ("workers.handshake.build") whose
// message names both builds (`peer_role` is "worker" or "host").
[[nodiscard]] Status check_same_build(const BuildIdentity& self, const BuildIdentity& peer,
                                      std::string_view peer_role);

// ---- Failure classes --------------------------------------------------------------------------

// How a scheduler should treat a failed job. Persisted in job records (append only).
enum class FailureClass : std::uint8_t {
    // An automatic retry may succeed without any change: the worker crashed or stopped responding,
    // a transient I/O error, memory pressure from outside the worker. Retries must be bounded.
    Transient = 1,
    // A retry with the same input, settings and limits fails again: corrupt or unsupported input,
    // a changed or missing source, a destination problem, a refused recipe, the worker's memory
    // limit. The ErrorCode says which change or user action is needed.
    Deterministic = 2,
    // The job was cancelled on request.
    Cancelled = 3,
};

[[nodiscard]] std::string_view to_string(FailureClass value) noexcept;  // "transient", ...

// The class of a failure reported by a worker, by its code: Cancelled -> Cancelled; IoError,
// Timeout, OutOfMemory, Unknown -> Transient; every other code -> Deterministic.
[[nodiscard]] FailureClass default_failure_class(const Error& error) noexcept;

// ---- Messages ---------------------------------------------------------------------------------

// Progress stages of a job (persisted in progress messages; append only).
enum class JobStage : std::uint8_t {
    Preparing = 1,  // validation, recipe, source identity (and hash), memory estimate
    Rendering = 2,  // decode and develop
    Encoding = 3,   // output profile conversion and quantisation
    Writing = 4,    // the exporter's write, re-read verification and hashing
    Verifying = 5,  // the output re-hashed at temp_path, the source identity re-checked
    Analysing = 6,  // (analyze, version 2) decode, analysis, related-shot regularisation
    Checking = 7,   // (analyze, version 2) the full-resolution final check and its fallbacks
};

[[nodiscard]] std::string_view to_string(JobStage stage) noexcept;  // "preparing", ...

// host_hello: the host's half of the handshake (see the file comment).
struct HostHello {
    std::uint32_t min_version = kMinProtocolVersion;
    std::uint32_t max_version = kMaxProtocolVersion;
    BuildIdentity build;
    std::uint32_t heartbeat_ms = 500;  // kMinHeartbeatMs..kMaxHeartbeatMs
    std::uint32_t host_pid = 0;
};

// worker_hello: the worker's answer.
struct WorkerHello {
    std::uint32_t version = kMaxProtocolVersion;  // the negotiated version
    BuildIdentity build;
    std::uint32_t pid = 0;
    bool console_window = false;          // GetConsoleWindow() != nullptr in the worker
    std::vector<std::string> test_hooks;  // active test hooks (worker_launch.hpp); empty in production
};

// The source a job reads. The worker refuses to render unless the file at `path` has `identity`
// before reading and still has it afterwards (SourceChanged), and, when `sha256` is set, unless its
// content hashes to it (an extra stable read of the whole file, core::hash_file, before decoding).
struct JobSource {
    std::filesystem::path path;  // absolute
    std::uint32_t subimage = 0;
    core::FileIdentity identity;
    std::optional<core::Digest256> sha256;
};

// The frozen recipe: the canonical "lucent.develop" document (application/recipe.hpp) and its
// SHA-256. The decoder requires sha256 == SHA-256(document); the worker additionally requires the
// document to be canonical (reading and rewriting it reproduces it byte for byte).
struct JobRecipe {
    std::string document;
    core::Digest256 sha256{};
};

struct JobRequest {
    std::string job_id;  // 1..kMaxJobIdBytes of [A-Za-z0-9._:-]
    JobSource source;
    JobRecipe recipe;
    // Must pass validate(). Includes the metadata policy (which EXIF groups are written; GPS only
    // on an explicit choice).
    exporter::ExportSettings output_settings;
    // The user's rights text written as Artist and Copyright (subject to the metadata policy). Unset:
    // the source's own values; an empty string: none. At most exporter::kMaxMetadataTextBytes each.
    std::optional<std::string> artist;
    std::optional<std::string> copyright;
    // The file the worker creates (absolute, with a file name; its directory must exist). The host
    // publishes it afterwards; the worker never does.
    std::filesystem::path temp_path;
    // The engine's refusal threshold for the render (EngineRendererOptions::memory_budget_bytes);
    // 0 selects the engine default.
    std::uint64_t memory_budget_bytes = 0;
    // Render an item whose decoded interpretation is Unsupported (recorded by the caller); otherwise
    // such a job fails with Unsupported (blueprint: never exported silently).
    bool allow_unsupported = false;
};

struct CancelJob {
    std::string job_id;
};

struct Ping {
    std::uint64_t nonce = 0;
};

struct Shutdown {};

struct JobProgress {
    std::string job_id;
    JobStage stage = JobStage::Preparing;
    double fraction = 0.0;  // overall, 0..1, non-decreasing within a job
};

// Process memory of the worker (GetProcessMemoryInfo), in bytes.
struct MemorySnapshot {
    std::uint64_t working_set = 0;
    std::uint64_t peak_working_set = 0;
    std::uint64_t commit = 0;       // private bytes (PrivateUsage)
    std::uint64_t peak_commit = 0;  // PeakPagefileUsage
};

struct Heartbeat {
    std::uint64_t sequence = 0;         // 1, 2, 3, ... per worker
    std::optional<std::string> job_id;  // the running job, if any
    MemorySnapshot memory;
};

// The file a job produced at its temp_path.
struct JobOutput {
    std::filesystem::path temp_path;
    core::Digest256 sha256{};  // of the file at temp_path, re-hashed after it was moved there
    std::uint64_t size_bytes = 0;
    exporter::ExportFormat format = exporter::ExportFormat::Jpeg;
    exporter::OutputProfile profile = exporter::OutputProfile::Srgb;
    exporter::BitDepth bit_depth = exporter::BitDepth::Eight;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    core::Digest256 icc_sha256{};
    std::optional<exporter::ChromaSubsampling> chroma_subsampling;  // JPEG only
    std::string encoder;                                            // e.g. "libjpeg-turbo 3.1.0; lcms 2.17"
    // What the metadata step changed or dropped (exporter::PreparedMetadata::notes, then the
    // source's EXIF reading warnings); at most kMaxWarnings, each truncated to kMaxWarningBytes.
    std::vector<std::string> metadata_notes;
};

// What the worker verified before answering.
struct OutputVerification {
    bool decoded = false;   // the exporter re-read and fully decoded the written file
    bool compared = false;  // ... and compared it with the intended bytes (JPEG) or samples (TIFF)
    bool rehashed = false;  // the file at temp_path was hashed again (stable read) and matched
    bool source_unchanged = false;  // the source still had the expected identity after the job
};

struct RenderSummaryInfo {
    raw::InterpretationStatus interpretation = raw::InterpretationStatus::Complete;
    std::vector<std::string> warnings;  // at most kMaxWarnings
    std::uint32_t width = 0;            // full-resolution upright size
    std::uint32_t height = 0;
    std::string demosaic;
    std::string camera_color;
    std::uint32_t pipeline_version = 0;
    std::string look;
    std::uint32_t look_version = 0;
    std::uint64_t clipped_raw_samples = 0;
    std::uint64_t limited_display_pixels = 0;
    std::uint64_t estimated_peak_bytes = 0;  // the engine's estimate for this render
};

struct JobTimings {
    double prepare_seconds = 0.0;  // validation, recipe, identity, optional hash, estimate
    double render_seconds = 0.0;
    double encode_seconds = 0.0;
    double write_seconds = 0.0;   // write, verification, hashing, move to temp_path
    double verify_seconds = 0.0;  // re-hash at temp_path and the source re-check
    double total_seconds = 0.0;
};

struct JobResult {
    std::string job_id;
    JobOutput output;
    OutputVerification verification;
    core::FileIdentity source_identity;           // as verified before and after
    std::optional<core::Digest256> source_sha256;  // when the request asked for verification
    core::Digest256 recipe_sha256{};
    RenderSummaryInfo render;
    JobTimings timings;
    MemorySnapshot memory;  // at the end of the job (peaks cover the worker's lifetime)
};

// analyze (version 2): the automatic settings of one photo. The worker refuses to analyse unless
// the source has its identity before and after (and, with `source.sha256`, hashes to it), the
// recipe document is the canonical document of its SHA-256 (the settings the analysis starts from:
// locks and locked values), and the request document is one the application can read.
struct AnalysisRequest {
    std::string job_id;  // 1..kMaxJobIdBytes of [A-Za-z0-9._:-]
    JobSource source;
    JobRecipe recipe;
    // The application's "lucent.auto-request" document (non-empty UTF-8, at most
    // kMaxAnalysisRequestBytes): consistency target, anchor, whether to run the final check.
    std::string request;
    // The engine's refusal threshold (as JobRequest::memory_budget_bytes); 0: the engine default.
    std::uint64_t memory_budget_bytes = 0;
    // Analyse an item whose decoded interpretation is Unsupported (as JobRequest).
    bool allow_unsupported = false;
};

// analysis (version 2): the answer to an analyze job.
struct AnalysisResult {
    std::string job_id;
    core::FileIdentity source_identity;           // as verified before and after
    std::optional<core::Digest256> source_sha256;  // when the request asked for verification
    core::Digest256 recipe_sha256{};              // of the starting recipe (the request's)
    // The application's "lucent.auto-run" document (non-empty UTF-8, at most
    // kMaxAnalysisDocumentBytes).
    std::string document;
    double seconds = 0.0;   // the whole job
    MemorySnapshot memory;  // at the end of the job (peaks cover the worker's lifetime)
};

// A failed job (job_id set), or a fatal worker error (job_id empty: handshake refusal, protocol
// violation), after which the worker exits.
struct JobError {
    std::optional<std::string> job_id;
    Error error;  // message truncated to kMaxErrorMessageBytes when encoded
    FailureClass failure_class = FailureClass::Deterministic;
};

struct Pong {
    std::uint64_t nonce = 0;
};

using Message = std::variant<HostHello, WorkerHello, JobRequest, CancelJob, Ping, Shutdown, JobProgress,
                             Heartbeat, JobResult, JobError, Pong, AnalysisRequest, AnalysisResult>;

// Who sends a message: host_hello, job, analyze, cancel, ping and shutdown go host -> worker, the
// rest worker -> host.
enum class Direction : std::uint8_t {
    HostToWorker = 1,
    WorkerToHost = 2,
};

[[nodiscard]] Direction direction_of(const Message& message) noexcept;

// The wire name of the message's type ("host_hello", "job", "analyze", "analysis", "error", ...).
[[nodiscard]] std::string_view type_name(const Message& message) noexcept;

// ---- Encoding and decoding --------------------------------------------------------------------

// The JSON text of `message`. Values are validated as decode_message() would (InvalidArgument,
// "workers.protocol.encode": a malformed job id, a relative path, a path that is not valid Unicode,
// a recipe whose sha256 does not match, invalid export settings, an empty or oversized analysis
// document, a message above kMaxMessageBytes),
// except that over-long error messages and warnings are truncated (at a UTF-8 boundary) and only
// the first kMaxWarnings warnings are kept. OutOfMemory.
[[nodiscard]] Expected<std::string> encode_message(const Message& message);

// Parses one message sent in `direction` (see "Strict reading" above). Corrupt
// ("workers.protocol.decode") for anything the protocol does not allow; OutOfMemory.
[[nodiscard]] Expected<Message> decode_message(std::string_view json, Direction direction);

// encode_message() with the frame header prepended.
[[nodiscard]] Expected<std::string> encode_frame(const Message& message);

// The payload length announced by a frame header. Corrupt ("workers.protocol.frame") for 0 or a
// length above kMaxMessageBytes (LimitExceeded is not used: a peer of the same build never sends
// one, so it means a damaged stream).
[[nodiscard]] Expected<std::uint32_t> decode_frame_length(std::span<const std::byte, kFrameHeaderBytes> header);

// True when `id` is 1..kMaxJobIdBytes characters of [A-Za-z0-9._:-].
[[nodiscard]] bool is_valid_job_id(std::string_view id) noexcept;

// `text` cut to at most `max_bytes` bytes without splitting a UTF-8 sequence ("..." is not added).
[[nodiscard]] std::string truncate_utf8(std::string_view text, std::size_t max_bytes);

}  // namespace lucent::workers
