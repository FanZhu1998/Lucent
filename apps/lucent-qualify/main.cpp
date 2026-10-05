// lucent-qualify: runs Lucent's camera qualification suite over the fixture corpus and writes a
// versioned JSON report and the Markdown support table (ADR 0016).
//
// Usage:
//   lucent-qualify [--root DIR] [--tier smoke|qualification|all] [--id ID]... [--out-json FILE]
//                  [--out-md FILE] [--previews DIR] [--work-dir DIR] [--keep-exports] [--in-process]
//   lucent-qualify --from-report FILE --out-md FILE
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
//   --from-report F  do not qualify anything: read the report F and write --out-md from it
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
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

#include <lucent/core/text.hpp>

#include "qualification.hpp"
#include "report.hpp"

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
        "       lucent-qualify --from-report FILE --out-md FILE\n",
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

lucent::qualify::QualifyOptions options_of(const Arguments& args) {
    lucent::qualify::QualifyOptions options;
    options.work_dir = args.work_dir;
    options.keep_exports = args.keep_exports;
    options.previews_dir = args.previews;
    return options;
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
        if (!args.out_md) {
            std::fprintf(stderr, "lucent-qualify: --from-report needs --out-md\n");
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

    if (args.child) {
        const lucent::qualify::ManifestFixture* fixture = corpus->find(args.ids.front());
        if (fixture == nullptr) {
            std::fprintf(stderr, "lucent-qualify: unknown fixture %s\n", args.ids.front().c_str());
            return kExitUsage;
        }
        const lucent::qualify::FixtureResult result =
            lucent::qualify::qualify_fixture(*corpus, *fixture, options_of(args));
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
            result = lucent::qualify::qualify_fixture(*corpus, *fixture, options_of(args)).to_json();
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
