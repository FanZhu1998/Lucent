// lucent-probe: prints a lucent.probe-report (schema version 1) for each RAW file.
//
// Usage:
//   lucent-probe [--decode] [--pretty] [--subimage N] [--no-hash] [--out-dir DIR] FILE...
//
// Output:
//   * Default: JSON Lines on stdout, one compact report per input file, in argument order.
//   * --pretty: indented JSON on stdout; a single file prints one object, several files print
//     one JSON array holding the reports in argument order.
//   * --out-dir DIR: writes each report, indented, to DIR/<file name>.json (one object per file,
//     the form research/tools/compare_probe.py reads) and prints one status line per file to
//     stderr instead of writing reports to stdout. Two inputs with the same file name (compared
//     case-insensitively, as Windows compares names) are a usage error, so that no report
//     silently overwrites another.
//   Reports are UTF-8 with "\n" line ends. Report text is sanitised to UTF-8; should anything
//   invalid remain, it is replaced with U+FFFD instead of failing the output.
//
// Options:
//   --decode      unpack the pixel data: parity values are taken after unpacking and
//                 raw_data_sha256 is added for single-channel data (see probe_report.hpp)
//   --subimage N  probe/decode image N of a multi-image file (required with --decode when the
//                 file holds more than one image)
//   --no-hash     omit file_sha256 (skips reading the whole file once more)
//
// Exit status: 0 when every file was reported with "status": "ok"; 2 when any file failed (its
// report still carries "status": "error" and an "error" object with code, where, message and
// native_code) or the reports could not be written; 1 for usage errors.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <io.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/text.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "probe_report.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFileFailed = 2;

struct Arguments {
    lucent::probe::ReportOptions report;
    bool help = false;
    bool pretty = false;
    std::optional<std::filesystem::path> out_dir;
    std::vector<std::filesystem::path> files;
};

void print_usage(std::FILE* stream) {
    std::fputs(
        "usage: lucent-probe [--decode] [--pretty] [--subimage N] [--no-hash] [--out-dir DIR] FILE...\n"
        "Prints a lucent.probe-report (schema 1) per file: JSON Lines by default, indented with\n"
        "--pretty (an array for several files), or one DIR/<file>.json per file with --out-dir.\n"
        "Exit status: 0 all ok, 2 any file failed, 1 usage error.\n",
        stream);
}

// Report file names of --out-dir must not collide (Windows names are case-insensitive).
bool out_dir_names_are_unique(const std::vector<std::filesystem::path>& files) {
    std::set<std::wstring> names;
    for (const std::filesystem::path& file : files) {
        std::wstring name = file.filename().wstring();
        for (wchar_t& c : name) {
            c = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c)));
        }
        if (!names.insert(std::move(name)).second) {
            std::fprintf(stderr,
                         "lucent-probe: two inputs are named '%s'; their --out-dir reports would collide\n",
                         lucent::core::path_to_utf8_lossy(file.filename()).c_str());
            return false;
        }
    }
    return true;
}

std::optional<Arguments> parse(int argc, wchar_t** argv) {
    Arguments args;
    bool only_files = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (only_files || arg.empty() || arg.front() != L'-') {
            args.files.emplace_back(arg);
            continue;
        }
        if (arg == L"--") {
            only_files = true;
        } else if (arg == L"--decode") {
            args.report.decode = true;
        } else if (arg == L"--pretty") {
            args.pretty = true;
        } else if (arg == L"--no-hash") {
            args.report.hash_file = false;
        } else if (arg == L"--subimage" && i + 1 < argc) {
            wchar_t* end = nullptr;
            const std::wstring_view text(argv[++i]);
            const unsigned long value = std::wcstoul(argv[i], &end, 10);
            if (text.empty() || end != argv[i] + text.size() || text.front() == L'-' ||
                value > 0xFFFFFFFFul) {
                std::fputs("lucent-probe: --subimage needs a non-negative integer\n", stderr);
                return std::nullopt;
            }
            args.report.subimage = static_cast<std::uint32_t>(value);
        } else if (arg == L"--out-dir" && i + 1 < argc) {
            args.out_dir = std::filesystem::path(argv[++i]);
        } else if (arg == L"--help" || arg == L"-h") {
            args.help = true;
            return args;
        } else {
            std::fprintf(stderr, "lucent-probe: unknown or incomplete option '%s'\n",
                         lucent::core::wide_to_utf8_lossy(arg).c_str());
            return std::nullopt;
        }
    }
    if (args.files.empty()) {
        std::fputs("lucent-probe: no input files\n", stderr);
        return std::nullopt;
    }
    if (args.out_dir && !out_dir_names_are_unique(args.files)) {
        return std::nullopt;
    }
    return args;
}

// Serialises a report (indent < 0: compact).
std::string dump(const lucent::probe::Json& document, int indent) {
    return document.dump(indent, ' ', false, lucent::probe::Json::error_handler_t::replace);
}

bool write_stdout(const std::string& text) {
    const bool written = std::fwrite(text.data(), 1, text.size(), stdout) == text.size() &&
                         std::fputc('\n', stdout) != EOF && std::fflush(stdout) == 0;
    if (!written) {
        std::fputs("lucent-probe: cannot write to standard output\n", stderr);
    }
    return written;
}

int run(const Arguments& args) {
    auto registry = lucent::raw::create_default_registry();
    if (!registry) {
        std::fprintf(stderr, "lucent-probe: %s\n", registry.error().describe().c_str());
        return kExitFileFailed;
    }
    if (args.out_dir) {
        std::error_code ec;
        std::filesystem::create_directories(*args.out_dir, ec);
        if (ec) {
            std::fprintf(stderr, "lucent-probe: cannot create %s: %s\n",
                         lucent::core::path_to_utf8_lossy(*args.out_dir).c_str(), ec.message().c_str());
            return kExitUsage;
        }
    }

    bool all_ok = true;
    lucent::probe::Json array = lucent::probe::Json::array();
    for (const std::filesystem::path& file : args.files) {
        lucent::probe::Report report =
            lucent::probe::build_report(*registry, file, args.report, lucent::core::CancellationToken{});
        all_ok = all_ok && report.ok;
        if (args.out_dir) {
            const std::filesystem::path target = *args.out_dir / (file.filename().wstring() + L".json");
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            out << dump(report.document, 2) << '\n';
            out.close();
            if (!out) {
                std::fprintf(stderr, "lucent-probe: cannot write %s\n",
                             lucent::core::path_to_utf8_lossy(target).c_str());
                all_ok = false;
                continue;
            }
            const std::string status = report.ok ? std::string("ok") : dump(report.document["error"], -1);
            std::fprintf(stderr, "%s: %s\n", lucent::core::path_to_utf8_lossy(file).c_str(), status.c_str());
            continue;
        }
        if (args.pretty && args.files.size() > 1) {
            array.push_back(std::move(report.document));
            continue;
        }
        if (!write_stdout(dump(report.document, args.pretty ? 2 : -1))) {
            return kExitFileFailed;
        }
    }
    if (!args.out_dir && args.pretty && args.files.size() > 1 && !write_stdout(dump(array, 2))) {
        return kExitFileFailed;
    }
    return all_ok ? kExitOk : kExitFileFailed;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Reports are UTF-8 bytes with "\n" line ends; keep the CRT from translating them.
    _setmode(_fileno(stdout), _O_BINARY);
    // build_report() never throws; this guards argument handling and output (allocation).
    try {
        const std::optional<Arguments> args = parse(argc, argv);
        if (!args) {
            print_usage(stderr);
            return kExitUsage;
        }
        if (args->help) {
            print_usage(stdout);
            return kExitOk;
        }
        return run(*args);
    } catch (const std::bad_alloc&) {
        std::fputs("lucent-probe: out of memory\n", stderr);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "lucent-probe: unexpected exception: %s\n", e.what());
    } catch (...) {
        std::fputs("lucent-probe: unexpected exception\n", stderr);
    }
    return kExitFileFailed;
}
