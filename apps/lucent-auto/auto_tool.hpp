#pragma once

// lucent-auto (developer tool, not shipped; W8, ADR 0011): runs Natural v1 automatic settings
// (policy "natural" version 2) with related-shot consistency over RAW files and folders and writes
// an evaluation set.
//
//   lucent-auto [--group-by time|none] [--out <dir>] [--tier smoke|all] [--max-gap <seconds>]
//               [--preview <px>] [--seed <n>] <files/folders...>
//
//   --group-by   time (default): related-shot grouping by capture time, visual similarity and
//                illumination, then the consistency stage (analysis/related_shots.hpp); none: every
//                image alone.
//   --out        output directory (default .\lucent-auto-out). Created when missing; it must be
//                empty, and it may not be, or lie inside, a source folder or the folder of an
//                input file: nothing is ever written into the source folders or overwritten.
//   --tier       only fixtures of that tier of a lucent.fixture-manifest (fixtures/manifest.json,
//                found beside or above the input folders; a fixture is cache/<id>/<file_name>):
//                smoke, or all fixtures. Without --tier every RAW file found is used.
//   --max-gap    the grouping's largest gap between consecutive related shots (default 60 s).
//   --preview    long edge of the preview renders (default 960 px, at most the 1536-px analysis
//                image they are rendered from).
//   --seed       seed of the blind sheet's left/right order (default 20261004).
//
// Folders are searched recursively for RAW file extensions (.dng .cr2 .cr3 .nef .arw .raf .orf
// .rw2 .pef .3fr .iiq .srw .x3f .erf .mos .mef .kdc .dcr .gpr .rwl .nrw .sr2 .srf .crw .fff .raw);
// files named explicitly are used whatever their extension. A file holding several images is
// analysed for its image 0. Every image starts from the as-shot settings (no locks).
//
// Output layout (all files new; JSON is UTF-8):
//   run.json                  lucent.auto-run v1: generator, arguments, versions, inputs and the
//                             files skipped, the grouping with its decisions, a summary per item,
//                             totals
//   summary.tsv               one row per item: chosen exposure, white balance, shadows,
//                             highlights, contrast, saturation, confidence, flags, group, time
//   items/NNN-<name>.json     lucent.auto-item v1: settings (summary and the canonical recipe),
//                             confidence and its reasons, flags, diagnostics, proposals, the
//                             consistency record, every candidate, timing and memory
//   previews/NNN-<name>-as-shot.jpg, NNN-<name>-auto.jpg
//                             sRGB JPEG renders of the as-shot and the automatic settings, from the
//                             analysis image (the interactive path the optimiser judged), reduced
//   pairs/NNN-<name>.jpg      as shot (left) and auto (right) side by side
//   sheet.html                self-contained evaluation sheet (images embedded): per group and
//                             image, as shot and auto side by side with settings and flags
//   blind/sheet.html          the blind variant: per image the two renders as A and B in an order
//                             drawn from --seed, nothing else that tells them apart, with a
//                             preference form that exports lucent.blind-choices JSON; images
//                             without a visible change (settings as shot, or 8-bit previews within
//                             one code value) are left out and only counted
//   key/blind-key.json        lucent.blind-key v1: which of A and B is the automatic render, and the
//                             unchanged images; kept outside blind/ so the folder handed to
//                             reviewers holds no key
// <name> is the fixture id when known, else the file stem (characters outside [A-Za-z0-9._-]
// replaced by '_'); NNN is the item's input index.
//
// Exit status: 0 when every output was written (items that failed are reported in run.json,
// summary.tsv and the sheets); 1 usage error; 2 failure (output, cancellation).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <lucent/analysis/related_shots.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>

namespace lucent::auto_tool {

inline constexpr std::int32_t kDefaultPreviewEdge = 960;
inline constexpr std::uint64_t kDefaultSeed = 20261004;

enum class Tier : std::uint8_t { Smoke, All };

struct Arguments {
    std::vector<std::filesystem::path> inputs;
    std::filesystem::path out_dir = "lucent-auto-out";
    analysis::GroupingMode group_by = analysis::GroupingMode::CaptureTime;
    std::optional<Tier> tier;
    double max_gap_seconds = analysis::GroupingOptions{}.max_gap_seconds;
    std::int32_t preview_edge = kDefaultPreviewEdge;
    std::uint64_t seed = kDefaultSeed;
    bool help = false;
};

// The arguments (argv[0] excluded), or a usage error message.
[[nodiscard]] std::variant<Arguments, std::string> parse_arguments(const std::vector<std::wstring>& args);

[[nodiscard]] std::string usage();

// A file to analyse, with what the fixture manifest says about it.
struct InputFile {
    std::filesystem::path path;
    std::string name;     // the fixture id, else the sanitised file stem
    std::string fixture;  // fixture id, or empty
    std::string tier;     // "smoke", "qualification", or empty
};

struct SkippedFile {
    std::filesystem::path path;
    std::string reason;
};

struct Discovery {
    std::vector<InputFile> files;  // sorted by path within each input, inputs in argument order
    std::vector<SkippedFile> skipped;
};

// Expands the inputs (see the file comment) and applies --tier. Errors: NotFound for an input that
// does not exist; InvalidArgument when --tier is given and no fixture manifest is found.
[[nodiscard]] Expected<Discovery> discover_inputs(const Arguments& args);

// InvalidArgument when the output directory is, or lies inside, an input folder or the folder of
// an input file (compared as absolute, case-insensitive paths after resolving what exists), or
// exists and is not an empty directory.
[[nodiscard]] Status check_output_directory(const Arguments& args, const Discovery& discovery);

// The blind order of item `index`: true when the automatic render is shown as A (left).
[[nodiscard]] bool auto_first(std::uint64_t seed, std::size_t index) noexcept;

// Runs everything and writes the outputs; returns the exit status. Progress goes to stderr, the
// summary table to stdout.
[[nodiscard]] int run(const Arguments& args, const core::CancellationToken& cancel);

}  // namespace lucent::auto_tool
