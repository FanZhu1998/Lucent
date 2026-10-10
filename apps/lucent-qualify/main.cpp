// lucent-qualify: runs Lucent's camera qualification suite over the fixture corpus and writes a
// versioned JSON report and the Markdown support table (ADR 0016).
//
// Usage:
//   lucent-qualify [--root DIR] [--tier smoke|qualification|all] [--id ID]... [--out-json FILE]
//                  [--out-md FILE] [--previews DIR] [--work-dir DIR] [--keep-exports] [--in-process]
//   lucent-qualify --from-report FILE --out-md FILE
//   lucent-qualify --from-report FILE --export-support DIR --asset-version X.Y.Z
//                  [--min-app-version X.Y.Z] [--check]
//
//   --root DIR       repository root (default: the source tree this tool was built from)
//   --tier T         fixtures of this manifest tier, or all (default: smoke); ignored with --id
//   --id ID          qualify this fixture (repeatable)
//   --out-json FILE  write the lucent.qualification-report v1 document
//   --out-md FILE    write the Markdown support table and coverage-gap list
//   --previews DIR   write a reduced sRGB JPEG of each develop render, for visual review
//   --work-dir DIR   directory for the exports (default: %TEMP%\lucent-qualify-<pid>)
//   --keep-exports   keep the exported JPEG/TIFF files
//   --in-process     run every fixture in this process (default: one child process per fixture,
//                    so that each fixture's peak memory is measured on its own and a crash in one
//                    fixture cannot end the run)
//   --from-report F  do not qualify anything: read the report F and write --out-md from it, or
//                    export the camera support database asset from it (ADR 0018):
//   --export-support DIR   write DIR/camera-support.json and DIR/manifest.json (support_export.hpp)
//   --asset-version V      the asset's version (SemVer; required with --export-support). Refused when
//                          DIR already holds this version with other content (AST-02): any change of
//                          the content is a new version
//   --min-app-version V    the oldest Lucent that may use it (default: this build's version)
//   --check                compare with the files in DIR instead of writing them (exit 3 when they
//                          differ)
//   --allow-dirty-build    export a report whose build was made from uncommitted changes (its commit
//                          ends in "-dirty") or from an unknown commit; refused otherwise (QUA-06), so
//                          that a published database names the commit that produced its evidence
//
// Fixtures run one after another (each needs about 1 to 1.5 GB at its peak).
//
// Exit status: 0 every selected fixture was qualified and matched its expectation; 3 a fixture's
// proposed state, check statuses, limitations or mode key differ from its expectation; 2 a fixture
// could not be qualified (not downloaded, altered, or its process failed); 1 usage or setup error.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

#include <lucent/assets/asset_store.hpp>
#include <lucent/assets/lens_database.hpp>
#include <lucent/core/text.hpp>
#include <lucent/core/version.hpp>

#include "qualification.hpp"
#include "report.hpp"
#include "support_export.hpp"

namespace {

using lucent::qualify::OrderedJson;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitNotQualified = 2;
constexpr int kExitMismatch = 3;

struct Arguments {
    std::filesystem::path root;
    std::string tier = "smoke";
    std::vector<std::string> ids;
    std::optional<std::filesystem::path> out_json;
    std::optional<std::filesystem::path> out_md;
    std::optional<std::filesystem::path> previews;
    std::filesystem::path work_dir;
    bool keep_exports = false;
    bool in_process = false;
    std::optional<std::filesystem::path> from_report;
    std::optional<std::filesystem::path> export_support;
    std::string asset_version;
    std::string min_app_version;
    bool check = false;
    bool allow_dirty_build = false;
    // Internal: run one fixture and write its result.
    bool child = false;
    std::optional<std::filesystem::path> result_file;
};

std::string utf8(const std::wstring& text) {
    return lucent::core::wide_to_utf8_lossy(text);
}
std::string utf8(const std::filesystem::path& path) {
    return lucent::core::path_to_utf8_lossy(path);
}

void usage() {
    std::fputs(
        "usage: lucent-qualify [--root DIR] [--tier smoke|qualification|all] [--id ID]... [--out-json FILE]\n"
        "                      [--out-md FILE] [--previews DIR] [--work-dir DIR] [--keep-exports] "
        "[--in-process]\n"
        "       lucent-qualify --from-report FILE --out-md FILE\n"
        "       lucent-qualify --from-report FILE --export-support DIR --asset-version X.Y.Z\n"
        "                      [--min-app-version X.Y.Z] [--check] [--allow-dirty-build]\n",
        stderr);
}

std::optional<Arguments> parse(int argc, wchar_t** argv) {
    Arguments args;
#ifdef LUCENT_SOURCE_DIR
    args.root = std::filesystem::path(lucent::core::utf8_to_wide_lossy(LUCENT_SOURCE_DIR));
#else
    args.root = std::filesystem::current_path();
#endif
    args.work_dir = std::filesystem::temp_directory_path() /
                    (L"lucent-qualify-" + std::to_wstring(GetCurrentProcessId()));
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const auto value = [&]() -> std::optional<std::wstring> {
            if (i + 1 >= argc) {
                return std::nullopt;
            }
            return std::wstring(argv[++i]);
        };
        std::optional<std::wstring> v;
        if (arg == L"--keep-exports") {
            args.keep_exports = true;
        } else if (arg == L"--in-process") {
            args.in_process = true;
        } else if (arg == L"--check") {
            args.check = true;
        } else if (arg == L"--allow-dirty-build") {
            args.allow_dirty_build = true;
        } else if (arg == L"--export-support" && (v = value())) {
            args.export_support = std::filesystem::path(*v);
        } else if (arg == L"--asset-version" && (v = value())) {
            args.asset_version = utf8(*v);
        } else if (arg == L"--min-app-version" && (v = value())) {
            args.min_app_version = utf8(*v);
        } else if (arg == L"--child") {
            args.child = true;
        } else if (arg == L"--root" && (v = value())) {
            args.root = *v;
        } else if (arg == L"--tier" && (v = value())) {
            args.tier = utf8(*v);
        } else if (arg == L"--id" && (v = value())) {
            args.ids.push_back(utf8(*v));
        } else if (arg == L"--out-json" && (v = value())) {
            args.out_json = std::filesystem::path(*v);
        } else if (arg == L"--out-md" && (v = value())) {
            args.out_md = std::filesystem::path(*v);
        } else if (arg == L"--previews" && (v = value())) {
            args.previews = std::filesystem::path(*v);
        } else if (arg == L"--work-dir" && (v = value())) {
            args.work_dir = *v;
        } else if (arg == L"--from-report" && (v = value())) {
            args.from_report = std::filesystem::path(*v);
        } else if (arg == L"--result" && (v = value())) {
            args.result_file = std::filesystem::path(*v);
        } else {
            std::fprintf(stderr, "lucent-qualify: unknown or incomplete option '%s'\n", utf8(arg).c_str());
            return std::nullopt;
        }
    }
    if (args.tier != "smoke" && args.tier != "qualification" && args.tier != "all") {
        std::fprintf(stderr, "lucent-qualify: --tier must be smoke, qualification or all\n");
        return std::nullopt;
    }
    if (args.child && (args.ids.size() != 1 || !args.result_file)) {
        std::fprintf(stderr, "lucent-qualify: --child needs one --id and --result\n");
        return std::nullopt;
    }
    return args;
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

// Command-line quoting for CommandLineToArgvW / the MSVC runtime.
std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }
    // Backslashes are literal unless they precede a quote: N backslashes before a quote become
    // 2N + 1, and N trailing backslashes (before the closing quote) become 2N.
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            out.push_back(c);
            continue;
        }
        if (c == L'"') {
            out.append(backslashes + 1, L'\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes, L'\\');
    out.push_back(L'"');
    return out;
}

struct ChildOutcome {
    bool ok = false;
    std::string error;
    OrderedJson result;
};

// Runs one fixture in a child process of this executable and reads its result file.
ChildOutcome run_child(const Arguments& args, const std::string& id) {
    ChildOutcome outcome;
    wchar_t exe[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (length == 0 || length >= std::size(exe)) {
        outcome.error = "cannot locate lucent-qualify.exe";
        return outcome;
    }
    const std::filesystem::path result_file =
        args.work_dir / (lucent::core::utf8_to_wide_lossy(id) + L".result.json");
    std::error_code ec;
    std::filesystem::remove(result_file, ec);
    std::wstring command = quote(exe) + L" --child --root " + quote(args.root.wstring()) + L" --id " +
                           quote(lucent::core::utf8_to_wide_lossy(id)) + L" --result " +
                           quote(result_file.wstring()) + L" --work-dir " + quote(args.work_dir.wstring());
    if (args.previews) {
        command += L" --previews " + quote(args.previews->wstring());
    }
    if (args.keep_exports) {
        command += L" --keep-exports";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process)) {
        outcome.error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        return outcome;
    }
    constexpr DWORD kTimeoutMs = 30 * 60 * 1000;
    const DWORD waited = WaitForSingleObject(process.hProcess, kTimeoutMs);
    DWORD exit_code = 0;
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        outcome.error = "the fixture process did not finish within 30 minutes";
    }
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!outcome.error.empty()) {
        return outcome;
    }
    if (exit_code != 0) {
        char code[16];
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(exit_code));
        outcome.error = std::string("the fixture process exited with ") + code;
        return outcome;
    }
    std::ifstream in(result_file, std::ios::binary);
    try {
        outcome.result = OrderedJson::parse(in);
        outcome.ok = true;
    } catch (const nlohmann::json::exception& e) {
        outcome.error = std::string("unreadable result file: ") + e.what();
    }
    std::filesystem::remove(result_file, ec);
    return outcome;
}

// A result for a fixture whose process failed: not qualified, with the reason.
OrderedJson failed_result(const lucent::qualify::ManifestFixture& fixture, const std::string& error) {
    lucent::qualify::FixtureResult r;
    r.id = fixture.id;
    r.tier = fixture.tier;
    r.file_name = fixture.file_name;
    r.harness_error = error;
    lucent::qualify::propose_state(r, nullptr);
    return r.to_json();
}

// The report's path as the database records it: relative to --root with '/' separators when it is
// inside the repository, otherwise its file name.
std::string report_name(const Arguments& args) {
    std::error_code ec;
    const std::filesystem::path report = std::filesystem::weakly_canonical(*args.from_report, ec);
    const std::filesystem::path root = std::filesystem::weakly_canonical(args.root, ec);
    const std::filesystem::path relative = report.lexically_relative(root);
    if (!relative.empty() && !relative.is_absolute() && *relative.begin() != L"..") {
        return utf8(relative.generic_wstring());
    }
    return utf8(args.from_report->filename());
}

// The file's bytes; empty when it cannot be read.
std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool same_text(const std::filesystem::path& path, const std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    const std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return existing == text;
}

// --export-support: writes (or with --check compares) the camera support database asset.
int export_support(const Arguments& args, const OrderedJson& report) {
    auto asset_version = lucent::core::SemanticVersion::parse(args.asset_version);
    if (!asset_version) {
        std::fprintf(stderr, "lucent-qualify: --export-support needs --asset-version X.Y.Z (%s)\n",
                     asset_version.error().message.c_str());
        return kExitUsage;
    }
    const std::string minimum_text =
        args.min_app_version.empty() ? std::string(lucent::core::build_info().version) : args.min_app_version;
    auto minimum = lucent::core::SemanticVersion::parse(minimum_text);
    if (!minimum) {
        std::fprintf(stderr, "lucent-qualify: --min-app-version: %s\n", minimum.error().message.c_str());
        return kExitUsage;
    }
    lucent::qualify::SupportExportOptions options;
    options.asset_version = std::move(*asset_version);
    options.minimum_app_version = std::move(*minimum);
    options.report_path = report_name(args);
    auto exported = lucent::qualify::export_camera_support(report, options);
    if (!exported) {
        std::fprintf(stderr, "lucent-qualify: %s\n", exported.error().describe().c_str());
        return kExitUsage;
    }
    const std::filesystem::path database_path =
        *args.export_support / std::filesystem::path(lucent::assets::kCameraSupportFileName);
    const std::filesystem::path manifest_path =
        *args.export_support / std::filesystem::path(lucent::assets::kAssetManifestFileName);
    const lucent::assets::SupportSummary& summary = exported->database.summary;
    if (args.check) {
        const bool same = same_text(database_path, exported->database_text) &&
                          same_text(manifest_path, exported->manifest_text);
        std::fprintf(stderr, "lucent-qualify: %s %s the export of %s\n", utf8(*args.export_support).c_str(),
                     same ? "equals" : "DIFFERS FROM", utf8(*args.from_report).c_str());
        return same ? kExitOk : kExitMismatch;
    }
    if (lucent::Status provenance = lucent::qualify::check_report_provenance(report, args.allow_dirty_build);
        !provenance) {
        std::fprintf(stderr, "lucent-qualify: %s\n", provenance.error().describe().c_str());
        return kExitUsage;
    }
    if (lucent::Status version = lucent::qualify::check_version_change(read_text(manifest_path),
                                                                        read_text(database_path), *exported);
        !version) {
        std::fprintf(stderr, "lucent-qualify: %s\n", version.error().describe().c_str());
        return kExitUsage;
    }
    if (!write_text(database_path, exported->database_text) ||
        !write_text(manifest_path, exported->manifest_text)) {
        std::fprintf(stderr, "lucent-qualify: cannot write %s\n", utf8(*args.export_support).c_str());
        return kExitUsage;
    }
    std::fprintf(
        stderr,
        "lucent-qualify: camera support database %s: %u modes (%u verified, %u compatible unverified, "
        "%u limited, %u unsupported) in %s\n",
        options.asset_version.to_string().c_str(), summary.modes, summary.verified,
        summary.compatible_unverified, summary.limited, summary.unsupported,
        utf8(*args.export_support).c_str());
    return kExitOk;
}

lucent::qualify::QualifyOptions options_of(const Arguments& args,
                                           std::shared_ptr<const lucent::engine::LensDatabase> lens_database) {
    lucent::qualify::QualifyOptions options;
    options.work_dir = args.work_dir;
    options.keep_exports = args.keep_exports;
    options.previews_dir = args.previews;
    options.lens_database = std::move(lens_database);
    return options;
}

// The bundled lens database next to this executable, verified against the build's pin (ADR 0023):
// the develop renders correct lenses as Lucent's exports do, and the report's basis names this
// database, so a qualification without it would certify renders Lucent does not make.
std::shared_ptr<const lucent::engine::LensDatabase> load_lens_database_or_report() {
    lucent::assets::LensDatabaseLookup lens =
        lucent::assets::load_lens_database(lucent::assets::default_asset_locations(nullptr));
    if (!lens.database) {
        std::fprintf(stderr,
                     "lucent-qualify: the bundled lens database cannot be loaded (%s); the qualification renders "
                     "lenses as Lucent's exports do and needs it\n",
                     lens.problem ? lens.problem->describe().c_str() : "unknown reason");
    }
    return lens.database;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::optional<Arguments> parsed = parse(argc, argv);
    if (!parsed) {
        usage();
        return kExitUsage;
    }
    const Arguments& args = *parsed;
    if (args.from_report) {
        if (!args.out_md && !args.export_support) {
            std::fprintf(stderr, "lucent-qualify: --from-report needs --out-md or --export-support\n");
            return kExitUsage;
        }
        auto document = lucent::qualify::read_json_file(*args.from_report);
        if (!document || document->value("schema", std::string()) != lucent::qualify::kReportSchema ||
            document->value("schema_version", 0) != lucent::qualify::kReportSchemaVersion) {
            std::fprintf(stderr, "lucent-qualify: %s is not a readable %s v%d document\n",
                         utf8(*args.from_report).c_str(), std::string(lucent::qualify::kReportSchema).c_str(),
                         lucent::qualify::kReportSchemaVersion);
            return kExitUsage;
        }
        const OrderedJson report = OrderedJson::parse(document->dump());
        if (args.export_support) {
            return export_support(args, report);
        }
        return write_text(*args.out_md, lucent::qualify::support_table_markdown(report)) ? kExitOk
                                                                                         : kExitUsage;
    }
    lucent::qualify::CorpusPaths paths{args.root};
    auto corpus = lucent::qualify::QualifyCorpus::load(paths, /*require_expectations=*/false);
    if (!corpus) {
        std::fprintf(stderr, "lucent-qualify: %s\n", corpus.error().describe().c_str());
        return kExitUsage;
    }
    std::error_code ec;
    std::filesystem::create_directories(args.work_dir, ec);
    const std::shared_ptr<const lucent::engine::LensDatabase> lens_database = load_lens_database_or_report();
    if (!lens_database) {
        return kExitUsage;
    }

    if (args.child) {
        const lucent::qualify::ManifestFixture* fixture = corpus->find(args.ids.front());
        if (fixture == nullptr) {
            std::fprintf(stderr, "lucent-qualify: unknown fixture %s\n", args.ids.front().c_str());
            return kExitUsage;
        }
        const lucent::qualify::FixtureResult result =
            lucent::qualify::qualify_fixture(*corpus, *fixture, options_of(args, lens_database));
        return write_text(*args.result_file, result.to_json().dump(1)) ? kExitOk : kExitUsage;
    }

    std::vector<const lucent::qualify::ManifestFixture*> selected;
    if (!args.ids.empty()) {
        for (const std::string& id : args.ids) {
            const lucent::qualify::ManifestFixture* fixture = corpus->find(id);
            if (fixture == nullptr) {
                std::fprintf(stderr, "lucent-qualify: unknown fixture %s\n", id.c_str());
                return kExitUsage;
            }
            selected.push_back(fixture);
        }
    } else {
        for (const lucent::qualify::ManifestFixture& fixture : corpus->manifest) {
            if (args.tier == "all" || fixture.tier == args.tier) {
                selected.push_back(&fixture);
            }
        }
    }

    auto families = lucent::qualify::load_families(paths.families());
    if (!families) {
        std::fprintf(stderr, "lucent-qualify: %s\n", families.error().describe().c_str());
        return kExitUsage;
    }
    auto manifest_document = lucent::qualify::read_json_file(paths.manifest());
    if (!manifest_document) {
        std::fprintf(stderr, "lucent-qualify: %s\n", manifest_document.error().describe().c_str());
        return kExitUsage;
    }

    std::vector<OrderedJson> results;
    lucent::qualify::ReportInputs inputs{
        paths, args.ids.empty() ? args.tier : std::string("ids"), {}, !args.in_process};
    bool not_qualified = false;
    bool mismatch = false;
    std::size_t index = 0;
    for (const lucent::qualify::ManifestFixture* fixture : selected) {
        ++index;
        std::fprintf(stderr, "[%zu/%zu] %s ...\n", index, selected.size(), fixture->id.c_str());
        OrderedJson result;
        if (args.in_process) {
            result = lucent::qualify::qualify_fixture(*corpus, *fixture, options_of(args, lens_database)).to_json();
        } else {
            ChildOutcome child = run_child(args, fixture->id);
            result = child.ok ? std::move(child.result) : failed_result(*fixture, child.error);
        }
        inputs.ids.push_back(fixture->id);
        const std::string state = result.value("proposed_state", std::string());
        const std::string expected =
            result.at("expected_state").is_string() ? result.at("expected_state").get<std::string>() : "-";
        const bool differs = !result.at("expectation_mismatches").empty();
        not_qualified = not_qualified || state == "not_qualified";
        mismatch = mismatch || differs;
        std::fprintf(stderr, "[%zu/%zu] %s: %s (expected %s)%s, %.1f s\n", index, selected.size(),
                     fixture->id.c_str(), state.c_str(), expected.c_str(), differs ? " MISMATCH" : "",
                     result.at("measurements").value("total_seconds", 0.0));
        for (const auto& m : result.at("expectation_mismatches")) {
            std::fprintf(stderr, "    %s\n", m.get<std::string>().c_str());
        }
        results.push_back(std::move(result));
    }

    const OrderedJson report = lucent::qualify::build_report(results, inputs, *families, *manifest_document);
    if (args.out_json && !write_text(*args.out_json, report.dump(1) + "\n")) {
        std::fprintf(stderr, "lucent-qualify: cannot write %s\n", utf8(*args.out_json).c_str());
        return kExitUsage;
    }
    if (args.out_md && !write_text(*args.out_md, lucent::qualify::support_table_markdown(report))) {
        std::fprintf(stderr, "lucent-qualify: cannot write %s\n", utf8(*args.out_md).c_str());
        return kExitUsage;
    }
    std::fprintf(stderr, "summary: %s\n", report.at("summary").dump().c_str());
    if (!args.keep_exports) {
        std::filesystem::remove_all(args.work_dir, ec);
    }
    if (not_qualified) {
        return kExitNotQualified;
    }
    return mismatch ? kExitMismatch : kExitOk;
}
