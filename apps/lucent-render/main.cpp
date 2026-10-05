// lucent-render: renders one RAW file through the engine's develop pipeline (ADR 0010: the export
// render, engine::render_develop_file's composition, with DevelopSettings v2 (ADR 0015: chroma
// denoising, masks, geometry) and the "neutral" v1 base look) and exports it with lucent::exporter
// (verified, no-clobber publication).
//
// Usage:
//   lucent-render <raw> --out <dir> [--format jpeg|tiff16] [--profile srgb|display-p3]
//                 [--quality N] [--subimage N] [--develop <settings.json>] [--exposure EV]
//                 [--report <file.json>]
//   lucent-render <raw> --out <dir> --legacy-neutral-v0 [--exposure EV] [--dump-linear <file.tiff>]
//                 [--no-dng-opcodes] [other options as above, without --develop]
//
//   --develop F        the settings to render: a "lucent.develop" recipe document (recipe.hpp),
//                      version 1 (upgraded on read) or 2, read strictly (an unknown field or
//                      operation, or a newer version, is an error, never ignored). Default: "as
//                      shot".
//   --legacy-neutral-v0  render with the W2 path instead (engine::render_file, provisional
//                      Neutral v0 display rendering; ADR 0006), for the W2 reference comparisons
//                      (research/tools/render_reference.py).
//
//   --out DIR          export directory (created when missing). The file is named
//                      <raw stem>_Lucent.jpg / .tif and is never overwritten: an existing file
//                      is an error (already_exists).
//   --format           jpeg (8-bit, default) or tiff16 (16-bit, Deflate).
//   --profile          output ICC profile: srgb (default) or display-p3.
//   --quality N        JPEG quality 1..100 (default 95); jpeg only.
//   --subimage N       image of a multi-image file (required for those, e.g. Dual Pixel RAW).
//   --exposure EV      user exposure on top of the file's baseline (default 0; develop: replaces
//                      the recipe's exposure_ev, within +-5 EV).
//   --dump-linear F    --legacy-neutral-v0 only: also write the scene-linear image before display
//                      rendering, converted to linear sRGB primaries (D65), as a 32-bit float TIFF
//                      (float_tiff.hpp), for reference comparisons (research/tools/render_reference.py).
//   --no-dng-opcodes   --legacy-neutral-v0 only: do not apply the file's DNG opcodes
//                      (RenderSettings::apply_dng_opcodes = false, ADR 0014), for comparisons with
//                      decoders that apply none (rawpy / LibRaw, render_reference.py). Every opcode
//                      is then recorded as disabled and the render is never reported complete. The
//                      develop pipeline always applies them.
//   --report F         write the lucent.render-report JSON (render_json.hpp), also for failures.
//
// Nothing is ever overwritten: an existing export, report or dump file is an error
// (already_exists), and a --report or --dump-linear path that names the input, the export
// destination or the other side file is a usage error. Side files are published like the export
// (temporary sibling, rename without replacing). Long paths work without the system-wide
// long-path setting.
//
// Output: one lucent.render-result JSON line on standard output, also for failures.
// Exit status: 0 success; 1 usage error (including a --develop file that cannot be read as a
// recipe: invalid_argument, or version_mismatch for a newer one); 2 render, export or output
// failure. Ctrl+C / Ctrl+Break
// cancel cooperatively; closing the console, log-off and shutdown cancel too and wait up to 4.5 s
// for the render and export to stop and delete their temporary files (the export never
// publishes a partial file). A process killed outright can leave a "~lucent-*.tmp" file in the
// output directory.

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <io.h>
// clang-format off: psapi.h needs the declarations of windows.h.
#include <windows.h>
#include <psapi.h>
// clang-format on

#include <lucent/application/recipe.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/text.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/export/export.hpp>
#include <lucent/image/color_math.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "float_tiff.hpp"
#include "probe_report.hpp"
#include "render_json.hpp"
#include "tool_files.hpp"

namespace {

using lucent::render_tool::Json;
using Clock = std::chrono::steady_clock;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFailed = 2;

// Console control events arrive on a system thread. The source is created once and never
// destroyed (so a late event can never reach a dead object) and published through an atomic;
// g_finished is a manual-reset event set once run() has returned and every temporary file is
// gone.
std::atomic<lucent::core::CancellationSource*> g_cancel{nullptr};
std::atomic<HANDLE> g_finished{nullptr};
// The system ends the process about 5 s after a close event; leave it a margin.
constexpr DWORD kCloseGraceMilliseconds = 4500;

BOOL WINAPI on_console_event(DWORD event) {
    lucent::core::CancellationSource* source = g_cancel.load();
    if (source == nullptr) {
        return FALSE;
    }
    switch (event) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            source->cancel();
            return TRUE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            // The process is terminated when this handler returns: cancel, then wait for the
            // cooperative stop so the exporter can delete its temporary file.
            source->cancel();
            if (const HANDLE finished = g_finished.load(); finished != nullptr) {
                ::WaitForSingleObject(finished, kCloseGraceMilliseconds);
            }
            return TRUE;
        default:
            return FALSE;
    }
}

// Signals g_finished on scope exit (also when run() throws).
struct FinishedSignal {
    FinishedSignal() = default;
    FinishedSignal(const FinishedSignal&) = delete;
    FinishedSignal& operator=(const FinishedSignal&) = delete;
    ~FinishedSignal() {
        if (const HANDLE finished = g_finished.load(); finished != nullptr) {
            ::SetEvent(finished);
        }
    }
};

enum class Format { Jpeg, Tiff16 };

struct Arguments {
    std::filesystem::path input;
    std::filesystem::path out_dir;
    Format format = Format::Jpeg;
    lucent::exporter::OutputProfile profile = lucent::exporter::OutputProfile::Srgb;
    std::optional<std::int32_t> quality;
    std::optional<std::uint32_t> subimage;
    std::optional<double> exposure_ev;
    std::optional<std::filesystem::path> develop;
    bool legacy = false;          // --legacy-neutral-v0
    bool no_dng_opcodes = false;  // --no-dng-opcodes
    std::optional<std::filesystem::path> dump_linear;
    std::optional<std::filesystem::path> report;
    bool help = false;
};

void print_usage(std::FILE* stream) {
    std::fputs(
        "usage: lucent-render <raw> --out <dir> [--format jpeg|tiff16] [--profile srgb|display-p3]\n"
        "                     [--quality N] [--subimage N] [--develop <settings.json>] [--exposure EV]\n"
        "                     [--report <file.json>]\n"
        "       lucent-render <raw> --out <dir> --legacy-neutral-v0 [--exposure EV]\n"
        "                     [--dump-linear <file.tiff>] [--no-dng-opcodes] [other options]\n"
        "Renders a RAW file (develop pipeline, neutral v1 look; --legacy-neutral-v0: the W2 render path)\n"
        "and exports <stem>_Lucent.jpg|.tif to <dir> without overwriting anything. Prints one\n"
        "lucent.render-result JSON line.\n"
        "Exit status: 0 ok, 1 usage error, 2 failure.\n",
        stream);
}

// A --develop recipe: its settings (always the current version) and the version the file had.
struct SettingsFile {
    lucent::engine::DevelopSettings settings;
    std::uint32_t file_version = 0;
};

// Reads the --develop recipe strictly (recipe.hpp; version 1 is upgraded). Its errors are usage
// errors.
lucent::Expected<SettingsFile> read_settings_file(const std::filesystem::path& path) {
    auto bytes = lucent::core::read_file_bytes(path, std::uint64_t{16} << 20);
    if (!bytes) {
        return lucent::Unexpected(std::move(bytes.error()));
    }
    const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    auto settings = lucent::app::read_recipe(text);
    if (!settings) {
        return lucent::Unexpected(std::move(settings.error()));
    }
    SettingsFile out;
    out.settings = std::move(*settings);
    // A readable recipe has an unsigned "version" member (read_recipe checked it).
    const Json document = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (document.is_object() && document.contains("version") && document.at("version").is_number_unsigned()) {
        out.file_version = document.at("version").get<std::uint32_t>();
    }
    return out;
}

bool write_line(const Json& j) {
    const std::string text = j.dump(-1, ' ', false, Json::error_handler_t::replace);
    return std::fwrite(text.data(), 1, text.size(), stdout) == text.size() &&
           std::fputc('\n', stdout) != EOF && std::fflush(stdout) == 0;
}

Json result_error(const lucent::Error& error) {
    return Json{{"schema", lucent::render_tool::kResultSchema},
                {"schema_version", lucent::render_tool::kSchemaVersion},
                {"status", "error"},
                {"error", lucent::render_tool::error_json(error)}};
}

std::optional<long long> parse_integer(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return std::nullopt;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const long long value = std::wcstoll(text, &end, 10);
    if (errno != 0 || end == text || *end != L'\0') {
        return std::nullopt;
    }
    return value;
}

// Returns the arguments, or an error message for a usage error.
std::variant<Arguments, std::string> parse(int argc, wchar_t** argv) {
    Arguments args;
    bool quality_given = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        const bool has_value = i + 1 < argc;
        if (arg == L"--help" || arg == L"-h") {
            args.help = true;
            return args;
        }
        if (!arg.empty() && arg.front() != L'-') {
            if (!args.input.empty()) {
                return std::string("only one input file is accepted");
            }
            args.input = std::filesystem::path(arg);
        } else if (arg == L"--out" && has_value) {
            args.out_dir = std::filesystem::path(argv[++i]);
        } else if (arg == L"--format" && has_value) {
            const std::wstring_view v(argv[++i]);
            if (v == L"jpeg") {
                args.format = Format::Jpeg;
            } else if (v == L"tiff16") {
                args.format = Format::Tiff16;
            } else {
                return std::string("--format must be jpeg or tiff16");
            }
        } else if (arg == L"--profile" && has_value) {
            const std::wstring_view v(argv[++i]);
            if (v == L"srgb") {
                args.profile = lucent::exporter::OutputProfile::Srgb;
            } else if (v == L"display-p3") {
                args.profile = lucent::exporter::OutputProfile::DisplayP3;
            } else {
                return std::string("--profile must be srgb or display-p3");
            }
        } else if (arg == L"--quality" && has_value) {
            const auto v = parse_integer(argv[++i]);
            if (!v || *v < 1 || *v > 100) {
                return std::string("--quality needs an integer from 1 to 100");
            }
            args.quality = static_cast<std::int32_t>(*v);
            quality_given = true;
        } else if (arg == L"--subimage" && has_value) {
            const auto v = parse_integer(argv[++i]);
            if (!v || *v < 0 || *v > 0xFFFFFFFFll) {
                return std::string("--subimage needs a non-negative integer");
            }
            args.subimage = static_cast<std::uint32_t>(*v);
        } else if (arg == L"--exposure" && has_value) {
            wchar_t* end = nullptr;
            const wchar_t* text = argv[++i];
            const double v = std::wcstod(text, &end);
            if (end == text || *end != L'\0' || !std::isfinite(v)) {
                return std::string("--exposure needs a number (EV)");
            }
            args.exposure_ev = v;
        } else if (arg == L"--develop" && has_value) {
            args.develop = std::filesystem::path(argv[++i]);
        } else if (arg == L"--legacy-neutral-v0") {
            args.legacy = true;
        } else if (arg == L"--no-dng-opcodes") {
            args.no_dng_opcodes = true;
        } else if (arg == L"--dump-linear" && has_value) {
            args.dump_linear = std::filesystem::path(argv[++i]);
        } else if (arg == L"--report" && has_value) {
            args.report = std::filesystem::path(argv[++i]);
        } else {
            return "unknown or incomplete option '" + lucent::core::wide_to_utf8_lossy(arg) + "'";
        }
    }
    if (args.input.empty()) {
        return std::string("no input file");
    }
    if (args.out_dir.empty()) {
        return std::string("--out is required");
    }
    if (quality_given && args.format != Format::Jpeg) {
        return std::string("--quality applies to --format jpeg only");
    }
    if (args.legacy && args.develop) {
        return std::string("--develop applies to the develop pipeline, not to --legacy-neutral-v0");
    }
    if (args.no_dng_opcodes && !args.legacy) {
        return std::string(
            "--no-dng-opcodes switches the W2 path's opcode processing off for reference comparisons; add "
            "--legacy-neutral-v0 (the develop pipeline always applies the DNG opcodes)");
    }
    if (args.dump_linear && !args.legacy) {
        return std::string(
            "--dump-linear writes the W2 path's scene-linear image; add --legacy-neutral-v0 (the develop "
            "pipeline does not expose a scene-linear stage)");
    }
    return args;
}

Json process_memory_json() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters))) {
        return nullptr;
    }
    return Json{{"peak_working_set_bytes", static_cast<std::uint64_t>(counters.PeakWorkingSetSize)},
                {"peak_commit_bytes", static_cast<std::uint64_t>(counters.PeakPagefileUsage)}};
}

// Converts the scene-linear Rec.2020 image to linear sRGB primaries in place and publishes it
// as a new file (tool_files.hpp).
lucent::Status dump_linear(lucent::engine::ColorImage& scene, const std::filesystem::path& path,
                           Json& record) {
    const auto start = Clock::now();
    const lucent::image::ColorEncoding target = lucent::image::ColorEncoding::linear_srgb();
    auto matrix = lucent::image::conversion_matrix(scene.encoding, target);
    if (!matrix) {
        return lucent::Unexpected(std::move(matrix.error()));
    }
    if (auto applied = lucent::image::apply_matrix(scene.pixels.view(), *matrix); !applied) {
        return applied;
    }
    scene.encoding = target;
    const std::string description = "lucent scene-linear " + target.to_string();
    if (auto written = lucent::render_tool::publish_new_file(
            path,
            [&](const std::filesystem::path& temp) {
                return lucent::render_tool::write_float_tiff(temp, scene.pixels.const_view(), description);
            });
        !written) {
        return written;
    }
    record = Json{{"path", lucent::core::path_to_utf8_lossy(path)},
                  {"encoding", lucent::render_tool::encoding_json(target)},
                  {"width", scene.pixels.width()},
                  {"height", scene.pixels.height()},
                  {"seconds", std::chrono::duration<double>(Clock::now() - start).count()}};
    return lucent::ok();
}

lucent::Status write_report(const std::filesystem::path& path, const Json& report) {
    return lucent::render_tool::publish_new_text_file(
        path, report.dump(2, ' ', false, Json::error_handler_t::replace) + "\n");
}

// A usage error found after parsing (it needs the file system): reported like parse errors.
int usage_error(const std::string& message) {
    std::fprintf(stderr, "lucent-render: %s\n", message.c_str());
    write_line(result_error(lucent::Error{lucent::ErrorCode::InvalidArgument, message, "lucent-render.usage", 0}));
    return kExitUsage;
}

// A --develop file that is not a usable recipe: a usage error that keeps the reader's code
// (invalid_argument, version_mismatch, not_found, ...).
int recipe_error(const lucent::Error& error) {
    const std::string message = "--develop: " + lucent::core::sanitize_utf8(error.message);
    std::fprintf(stderr, "lucent-render: %s\n", message.c_str());
    write_line(result_error(lucent::Error{error.code, message, "lucent-render.develop", error.native_code}));
    return kExitUsage;
}

// The option name and path of a side file (--report or --dump-linear), if given.
struct SideFile {
    const char* option;
    const std::optional<std::filesystem::path>* path;
    const char* other_option;
    const std::optional<std::filesystem::path>* other;
};

int run(const Arguments& args, const lucent::core::CancellationToken& cancel) {
    const auto start = Clock::now();
    const char* pipeline =
        args.legacy ? lucent::render_tool::kLegacyPipeline : lucent::render_tool::kDevelopPipeline;

    // The develop settings and their identity (before anything is written).
    lucent::engine::DevelopSettings settings;
    lucent::app::RecipeDocument recipe;
    Json recipe_json = nullptr;
    std::uint32_t file_version = 0;
    if (!args.legacy) {
        if (args.develop) {
            auto read = read_settings_file(*args.develop);
            if (!read) {
                return recipe_error(read.error());
            }
            settings = std::move(read->settings);
            file_version = read->file_version;
        }
        if (args.exposure_ev) {
            settings.exposure_ev = *args.exposure_ev;
        }
        auto document = lucent::app::write_recipe(settings);
        if (!document) {
            return recipe_error(document.error());
        }
        recipe = std::move(*document);
        recipe_json = Json{
            {"schema", std::string(lucent::app::kRecipeSchema)},
            {"version", lucent::app::kRecipeSchemaVersion},
            {"sha256", lucent::core::to_hex(recipe.sha256)},
            {"document", Json::parse(recipe.json)},
            {"file", args.develop ? Json(lucent::core::path_to_utf8_lossy(*args.develop)) : Json(nullptr)},
            {"file_version", args.develop ? Json(file_version) : Json(nullptr)}};
    }

    Json report{{"schema", lucent::render_tool::kReportSchema},
                {"schema_version", lucent::render_tool::kSchemaVersion},
                {"generator", lucent::render_tool::generator_json()},
                {"source", Json{{"path", lucent::core::path_to_utf8_lossy(args.input)},
                                {"file_name", lucent::core::path_to_utf8_lossy(args.input.filename())},
                                {"subimage", args.subimage ? Json(*args.subimage) : Json(nullptr)}}},
                {"pipeline", pipeline},
                {"recipe", recipe_json},
                {"status", "error"},
                {"error", nullptr},
                {"metadata", nullptr},
                {"render", nullptr},
                {"dump_linear", nullptr},
                {"export", nullptr},
                {"process", nullptr},
                {"seconds", nullptr}};
    const auto finish = [&](const std::optional<lucent::Error>& error, Json result) {
        report["seconds"] = std::chrono::duration<double>(Clock::now() - start).count();
        report["process"] = process_memory_json();
        if (error) {
            report["status"] = "error";
            report["error"] = lucent::render_tool::error_json(*error);
            result = result_error(*error);
        } else {
            report["status"] = "ok";
        }
        bool ok = !error;
        if (args.report) {
            if (auto written = write_report(*args.report, report); !written) {
                std::fprintf(stderr, "lucent-render: cannot write the report: %s\n",
                             lucent::core::sanitize_utf8(written.error().describe()).c_str());
                ok = false;
                if (!error) {
                    result = result_error(written.error());
                }
            }
        }
        if (!write_line(result)) {
            return kExitFailed;
        }
        return ok ? kExitOk : kExitFailed;
    };

    std::filesystem::path name = args.input.stem();
    name += args.format == Format::Jpeg ? L"_Lucent.jpg" : L"_Lucent.tif";
    const std::filesystem::path destination = args.out_dir / name;

    // Side files must never name the input, the export or each other (usage errors, before any
    // file is written), and never replace an existing file.
    const std::array<SideFile, 2> sides = {{
        {"--report", &args.report, "--dump-linear", &args.dump_linear},
        {"--dump-linear", &args.dump_linear, "--report", &args.report},
    }};
    for (const SideFile& side : sides) {
        if (!side.path->has_value()) {
            continue;
        }
        const std::string other_name = std::string("the ") + side.other_option + " file";
        const std::array<std::pair<std::string, const std::filesystem::path*>, 3> others = {{
            {"the input file", &args.input},
            {"the export destination", &destination},
            {other_name, side.other->has_value() ? &**side.other : nullptr},
        }};
        for (const auto& [what, other] : others) {
            if (other == nullptr) {
                continue;
            }
            auto same = lucent::render_tool::same_target(**side.path, *other);
            if (!same) {
                return finish(same.error(), {});
            }
            if (*same) {
                return usage_error(std::string(side.option) + " must not name " + what + " (" +
                                   lucent::core::path_to_utf8_lossy(**side.path) + ")");
            }
        }
    }
    if (args.report && lucent::render_tool::exists(*args.report)) {
        // Not even an error report may replace it.
        const std::string message = "the --report file exists and is never overwritten: " +
                                    lucent::core::path_to_utf8_lossy(*args.report);
        std::fprintf(stderr, "lucent-render: %s\n", message.c_str());
        write_line(result_error(
            lucent::Error{lucent::ErrorCode::AlreadyExists, message, "lucent-render.output", 0}));
        return kExitFailed;
    }
    if (args.dump_linear && lucent::render_tool::exists(*args.dump_linear)) {
        return finish(lucent::Error{lucent::ErrorCode::AlreadyExists,
                                    "the --dump-linear file exists and is never overwritten: " +
                                        lucent::core::path_to_utf8_lossy(*args.dump_linear),
                                    "lucent-render.output", 0},
                      {});
    }

    // Destination (checked before the expensive render; the exporter re-checks atomically).
    if (auto created = lucent::render_tool::create_directories(args.out_dir); !created) {
        return finish(created.error(), {});
    }
    if (lucent::render_tool::exists(destination)) {
        return finish(lucent::Error{lucent::ErrorCode::AlreadyExists,
                                    "the output file exists and is never overwritten: " +
                                        lucent::core::path_to_utf8_lossy(destination),
                                    "lucent-render.output", 0},
                      {});
    }

    auto registry = lucent::raw::create_default_registry();
    if (!registry) {
        return finish(registry.error(), {});
    }
    lucent::engine::ColorImage display;
    std::vector<std::string> warnings;
    if (args.legacy) {
        // The W2 render path (ADR 0006).
        lucent::engine::RenderSettings legacy;
        legacy.exposure_ev = args.exposure_ev.value_or(0.0);
        legacy.apply_dng_opcodes = !args.no_dng_opcodes;
        lucent::engine::RenderOptions options;
        options.keep_scene_linear = args.dump_linear.has_value();
        auto rendered =
            lucent::engine::render_file(*registry, args.input, args.subimage, legacy, options, cancel);
        if (!rendered) {
            return finish(rendered.error(), {});
        }
        report["metadata"] = lucent::probe::metadata_json(rendered->metadata);
        report["render"] = lucent::render_tool::render_report_json(rendered->report);
        warnings = rendered->report.warnings;
        if (args.dump_linear && rendered->scene_linear) {
            Json record;
            if (auto dumped = dump_linear(*rendered->scene_linear, *args.dump_linear, record); !dumped) {
                return finish(dumped.error(), {});
            }
            report["dump_linear"] = std::move(record);
            rendered->scene_linear.reset();  // release before exporting
        }
        display = std::move(rendered->display);
    } else {
        // The develop pipeline's export render: exactly engine::render_develop_file (develop.cpp),
        // run in its two steps to report the decoded metadata.
        lucent::engine::StageOneRequest request;
        request.kind = lucent::engine::StageOneKind::Export;
        request.white_balance = settings.white_balance;
        auto one =
            lucent::engine::render_stage_one_file(*registry, args.input, args.subimage, request, {}, cancel);
        if (!one) {
            return finish(one.error(), {});
        }
        report["metadata"] = lucent::probe::metadata_json(one->metadata);
        auto developed = lucent::engine::render_stage_two(std::move(*one), settings, {}, cancel);
        if (!developed) {
            return finish(developed.error(), {});
        }
        report["render"] = lucent::render_tool::develop_report_json(developed->report);
        warnings = developed->report.warnings;
        display = std::move(developed->display);
    }
    for (const std::string& warning : warnings) {
        std::fprintf(stderr, "lucent-render: warning: %s\n", lucent::core::sanitize_utf8(warning).c_str());
    }

    const auto export_start = Clock::now();
    lucent::exporter::ExportSettings export_settings =
        args.format == Format::Jpeg ? lucent::exporter::ExportSettings::jpeg_srgb_q95()
                                    : lucent::exporter::ExportSettings::tiff16(args.profile);
    export_settings.profile = args.profile;
    if (args.quality) {
        export_settings.jpeg.quality = *args.quality;
    }
    if (auto valid = export_settings.validate(); !valid) {
        return finish(valid.error(), {});
    }
    // Quantise, then release the float display image before writing: the export's peak is the
    // quantised image plus its codec buffers, not also the float image.
    auto encoded = lucent::exporter::encode_for_output(
        display.pixels.const_view(), display.encoding, export_settings.profile,
        lucent::exporter::bit_depth_for(export_settings.format), export_settings.transform, cancel);
    if (!encoded) {
        return finish(encoded.error(), {});
    }
    display.pixels = lucent::image::ImageBuffer<float>{};
    auto receipt = lucent::exporter::write_verified(*encoded, destination, export_settings, cancel);
    if (!receipt) {
        return finish(receipt.error(), {});
    }
    *encoded = lucent::exporter::OutputImage{};
    Json export_json = lucent::render_tool::receipt_json(*receipt);
    export_json["seconds"] = std::chrono::duration<double>(Clock::now() - export_start).count();
    report["export"] = std::move(export_json);

    Json result{{"schema", lucent::render_tool::kResultSchema},
                {"schema_version", lucent::render_tool::kSchemaVersion},
                {"status", "ok"},
                {"pipeline", pipeline},
                {"output", lucent::core::path_to_utf8_lossy(receipt->path)},
                {"format", std::string(lucent::exporter::to_string(receipt->format))},
                {"profile", std::string(lucent::exporter::to_string(receipt->profile))},
                {"width", receipt->width},
                {"height", receipt->height},
                {"sha256", lucent::core::to_hex(receipt->sha256)},
                {"settings_sha256", args.legacy ? Json(nullptr) : Json(lucent::core::to_hex(recipe.sha256))},
                {"seconds", std::chrono::duration<double>(Clock::now() - start).count()},
                {"warnings", warnings.size()},
                {"error", nullptr}};
    return finish(std::nullopt, std::move(result));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    try {
        auto parsed = parse(argc, argv);
        if (auto* message = std::get_if<std::string>(&parsed)) {
            std::fprintf(stderr, "lucent-render: %s\n", message->c_str());
            print_usage(stderr);
            write_line(result_error(
                lucent::Error{lucent::ErrorCode::InvalidArgument, *message, "lucent-render.usage", 0}));
            return kExitUsage;
        }
        const Arguments& args = std::get<Arguments>(parsed);
        if (args.help) {
            print_usage(stdout);
            return kExitOk;
        }
        // Never destroyed: the console handler may still read it after run() returns.
        auto* cancel = new lucent::core::CancellationSource();
        g_finished.store(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        g_cancel.store(cancel);
        ::SetConsoleCtrlHandler(on_console_event, TRUE);
        int status = kExitFailed;
        {
            FinishedSignal signal;
            status = run(args, cancel->token());
        }
        ::SetConsoleCtrlHandler(on_console_event, FALSE);
        return status;
    } catch (const std::bad_alloc&) {
        write_line(
            result_error(lucent::Error{lucent::ErrorCode::OutOfMemory, "out of memory", "lucent-render", 0}));
    } catch (const std::exception& e) {
        write_line(result_error(lucent::Error{lucent::ErrorCode::Internal,
                                              std::string("unexpected exception: ") + e.what(),
                                              "lucent-render", 0}));
    } catch (...) {
        write_line(result_error(
            lucent::Error{lucent::ErrorCode::Internal, "unexpected exception", "lucent-render", 0}));
    }
    return kExitFailed;
}
