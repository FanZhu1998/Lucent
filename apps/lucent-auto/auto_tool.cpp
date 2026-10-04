#include "auto_tool.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <format>
#include <map>
#include <system_error>

#include <lucent/core/file.hpp>
#include <lucent/core/text.hpp>
#include <lucent/export/export.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "auto_report.hpp"
#include "tool_files.hpp"

namespace lucent::auto_tool {

namespace {

namespace an = analysis;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFailed = 2;
constexpr const char* kWhere = "lucent-auto";

constexpr std::array<std::wstring_view, 26> kRawExtensions{
    L".dng", L".cr2", L".cr3", L".crw", L".nef", L".nrw", L".arw", L".srf", L".sr2",
    L".raf", L".orf", L".rw2", L".pef", L".3fr", L".fff", L".iiq", L".rwl", L".srw",
    L".x3f", L".erf", L".mos", L".mef", L".kdc", L".dcr", L".raw", L".gpr"};

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return s;
}

bool is_raw_extension(const fs::path& p) {
    const std::wstring ext = lower(p.extension().wstring());
    return std::find(kRawExtensions.begin(), kRawExtensions.end(), ext) != kRawExtensions.end();
}

std::string sanitise(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                          c == '-' || c == '_';
        out += keep ? c : '_';
    }
    return out.empty() ? std::string("image") : out;
}

std::optional<double> parse_number(const std::wstring& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const double v = std::wcstod(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != L'\0' || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

// Absolute, normalised, lower-case path text for containment tests: the longest existing prefix is
// resolved (junctions and 8.3 names), the rest appended lexically.
std::wstring comparable(const fs::path& p) {
    std::error_code ec;
    fs::path absolute = fs::absolute(p, ec);
    if (ec) {
        absolute = p;
    }
    fs::path resolved = fs::weakly_canonical(absolute, ec);
    if (ec) {
        resolved = absolute.lexically_normal();
    }
    std::wstring text = lower(resolved.make_preferred().wstring());
    while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) {
        text.pop_back();
    }
    return text;
}

bool same_or_inside(const std::wstring& path, const std::wstring& folder) {
    if (path == folder) {
        return true;
    }
    if (path.size() <= folder.size() || path.compare(0, folder.size(), folder) != 0) {
        return false;
    }
    return folder.back() == L'\\' || path[folder.size()] == L'\\';
}

// A fixture manifest (lucent.fixture-manifest) found at or above a folder.
struct Manifest {
    struct Entry {
        std::string tier;
        std::string file_name;
    };
    std::map<std::string, Entry> fixtures;
};

std::optional<Manifest> read_manifest(const fs::path& path) {
    auto bytes = core::read_file_bytes(path, std::uint64_t{8} << 20);
    if (!bytes) {
        return std::nullopt;
    }
    const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    const Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("schema", std::string()) != "lucent.fixture-manifest" ||
        !j.contains("fixtures") || !j["fixtures"].is_array()) {
        return std::nullopt;
    }
    Manifest m;
    for (const Json& f : j["fixtures"]) {
        if (f.is_object() && f.contains("id") && f["id"].is_string()) {
            m.fixtures[f["id"].get<std::string>()] =
                Manifest::Entry{f.value("tier", std::string()), f.value("file_name", std::string())};
        }
    }
    return m;
}

class Manifests {
public:
    // The manifest at or above `folder` (up to four levels), if any.
    const Manifest* find(const fs::path& folder) {
        fs::path dir = folder;
        for (int level = 0; level < 5 && !dir.empty(); ++level) {
            const fs::path candidate = dir / L"manifest.json";
            const std::wstring key = comparable(candidate);
            auto it = cache_.find(key);
            if (it == cache_.end()) {
                std::error_code ec;
                std::optional<Manifest> m;
                if (fs::is_regular_file(candidate, ec)) {
                    m = read_manifest(candidate);
                }
                it = cache_.emplace(key, std::move(m)).first;
            }
            if (it->second) {
                return &*it->second;
            }
            const fs::path parent = dir.parent_path();
            if (parent == dir) {
                break;
            }
            dir = parent;
        }
        return nullptr;
    }
    [[nodiscard]] bool any() const {
        return std::any_of(cache_.begin(), cache_.end(), [](const auto& e) { return e.second.has_value(); });
    }

private:
    std::map<std::wstring, std::optional<Manifest>> cache_;
};

std::vector<std::uint8_t> read_jpeg(const fs::path& out_dir, const std::string& relative) {
    auto rel = core::path_from_utf8(relative);
    if (!rel) {
        return {};
    }
    auto bytes = core::read_file_bytes(out_dir / *rel, std::uint64_t{64} << 20);
    if (!bytes) {
        return {};
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes->data());
    return std::vector<std::uint8_t>(data, data + bytes->size());
}

Expected<exporter::OutputImage> encode(const engine::ColorImage& image) {
    return exporter::encode_for_output(image.pixels.const_view(), image.encoding, exporter::OutputProfile::Srgb,
                                       exporter::BitDepth::Eight);
}

exporter::ExportSettings preview_settings() {
    exporter::ExportSettings s = exporter::ExportSettings::jpeg_srgb_q95();
    s.jpeg.quality = 85;
    return s;
}

std::string relative(const std::string& folder, const std::string& file) {
    return folder + "/" + file;
}

// The previews of one item between the passes.
struct Pending {
    std::optional<exporter::OutputImage> as_shot;
    std::optional<exporter::OutputImage> auto_image;
    engine::DevelopSettings auto_settings;
};

}  // namespace

std::string usage() {
    return "usage: lucent-auto [--group-by time|none] [--out <dir>] [--tier smoke|all] [--max-gap <seconds>]\n"
           "                   [--preview <px>] [--seed <n>] <files/folders...>\n"
           "Runs Natural v1 automatic settings (with related-shot consistency unless --group-by none) over RAW\n"
           "files and folders and writes per-item JSON, previews, an evaluation sheet and a blind sheet to <dir>\n"
           "(default .\\lucent-auto-out; it must be empty and may not lie inside a source folder).\n"
           "Exit status: 0 ok, 1 usage error, 2 failure.\n";
}

std::variant<Arguments, std::string> parse_arguments(const std::vector<std::wstring>& args) {
    Arguments a;
    bool out_given = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring& arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == L"--help" || arg == L"-h") {
            a.help = true;
            return a;
        }
        if (!arg.empty() && arg.front() != L'-') {
            a.inputs.emplace_back(arg);
        } else if (arg == L"--group-by" && has_value) {
            const std::wstring& v = args[++i];
            if (v == L"time") {
                a.group_by = an::GroupingMode::CaptureTime;
            } else if (v == L"none") {
                a.group_by = an::GroupingMode::None;
            } else {
                return std::string("--group-by must be time or none");
            }
        } else if (arg == L"--out" && has_value) {
            if (out_given) {
                return std::string("--out is given twice");
            }
            a.out_dir = fs::path(args[++i]);
            out_given = true;
            if (a.out_dir.empty()) {
                return std::string("--out needs a directory");
            }
        } else if (arg == L"--tier" && has_value) {
            const std::wstring& v = args[++i];
            if (v == L"smoke") {
                a.tier = Tier::Smoke;
            } else if (v == L"all") {
                a.tier = Tier::All;
            } else {
                return std::string("--tier must be smoke or all");
            }
        } else if (arg == L"--max-gap" && has_value) {
            const auto v = parse_number(args[++i]);
            if (!v || *v < 0.0 || *v > 86400.0) {
                return std::string("--max-gap needs a number of seconds from 0 to 86400");
            }
            a.max_gap_seconds = *v;
        } else if (arg == L"--preview" && has_value) {
            const auto v = parse_number(args[++i]);
            if (!v || *v != std::floor(*v) || *v < 128.0 || *v > 1536.0) {
                return std::string("--preview needs a whole number of pixels from 128 to 1536");
            }
            a.preview_edge = static_cast<std::int32_t>(*v);
        } else if (arg == L"--seed" && has_value) {
            const std::wstring& text = args[++i];
            wchar_t* end = nullptr;
            errno = 0;
            const unsigned long long v = std::wcstoull(text.c_str(), &end, 10);
            if (text.empty() || errno != 0 || *end != L'\0' || text.front() == L'-') {
                return std::string("--seed needs a non-negative integer");
            }
            a.seed = v;
        } else {
            return "unknown or incomplete option '" + core::wide_to_utf8_lossy(arg) + "'";
        }
    }
    if (a.inputs.empty()) {
        return std::string("no input files or folders");
    }
    return a;
}

Expected<Discovery> discover_inputs(const Arguments& args) {
    Discovery d;
    Manifests manifests;
    const std::wstring out = comparable(args.out_dir);
    std::vector<std::wstring> seen;
    const auto add = [&](const fs::path& file) {
        const std::wstring key = comparable(file);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            return;
        }
        seen.push_back(key);
        InputFile f;
        f.path = file;
        f.name = sanitise(core::path_to_utf8_lossy(file.stem()));
        const fs::path folder = file.parent_path();
        if (const Manifest* m = manifests.find(folder)) {
            const std::string id = core::path_to_utf8_lossy(folder.filename());
            const auto it = m->fixtures.find(id);
            if (it != m->fixtures.end() &&
                (it->second.file_name.empty() || it->second.file_name == core::path_to_utf8_lossy(file.filename()))) {
                f.fixture = id;
                f.tier = it->second.tier;
                f.name = sanitise(id);
            }
        }
        if (args.tier) {
            if (f.fixture.empty()) {
                d.skipped.push_back({file, "not a fixture of a lucent.fixture-manifest (--tier)"});
                return;
            }
            if (*args.tier == Tier::Smoke && f.tier != "smoke") {
                d.skipped.push_back({file, std::format("fixture tier '{}' (--tier smoke)", f.tier)});
                return;
            }
        }
        d.files.push_back(std::move(f));
    };
    for (const fs::path& input : args.inputs) {
        std::error_code ec;
        const fs::file_status status = fs::status(input, ec);
        if (ec || !fs::exists(status)) {
            return fail(ErrorCode::NotFound, "input not found: " + core::path_to_utf8_lossy(input), kWhere);
        }
        if (fs::is_regular_file(status)) {
            add(input);
            continue;
        }
        if (!fs::is_directory(status)) {
            return fail(ErrorCode::InvalidArgument, "input is neither a file nor a folder: " +
                                                        core::path_to_utf8_lossy(input), kWhere);
        }
        std::vector<fs::path> files;
        fs::recursive_directory_iterator it(input, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::directory_entry& entry = *it;
            std::error_code e2;
            if (entry.is_directory(e2) && same_or_inside(comparable(entry.path()), out)) {
                it.disable_recursion_pending();  // never read the tool's own output
                continue;
            }
            if (entry.is_regular_file(e2) && is_raw_extension(entry.path())) {
                files.push_back(entry.path());
            }
        }
        if (ec) {
            return fail(ErrorCode::IoError,
                        std::format("cannot list {}: {}", core::path_to_utf8_lossy(input), ec.message()), kWhere);
        }
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            return lower(a.generic_wstring()) < lower(b.generic_wstring());
        });
        for (const fs::path& f : files) {
            add(f);
        }
    }
    if (args.tier && !manifests.any()) {
        return fail(ErrorCode::InvalidArgument,
                    "--tier needs a lucent.fixture-manifest (manifest.json) at or above the input folders", kWhere);
    }
    return d;
}

Status check_output_directory(const Arguments& args, const Discovery& discovery) {
    const std::wstring out = comparable(args.out_dir);
    std::vector<std::wstring> sources;
    for (const fs::path& input : args.inputs) {
        std::error_code ec;
        sources.push_back(comparable(fs::is_directory(input, ec) ? input : input.parent_path()));
    }
    for (const InputFile& f : discovery.files) {
        sources.push_back(comparable(f.path.parent_path()));
    }
    for (const std::wstring& source : sources) {
        if (same_or_inside(out, source)) {
            return fail(ErrorCode::InvalidArgument,
                        "the output directory " + core::path_to_utf8_lossy(args.out_dir) +
                            " is or lies inside a source folder (" + core::wide_to_utf8_lossy(source) +
                            "): lucent-auto never writes into source folders",
                        kWhere);
        }
    }
    std::error_code ec;
    if (fs::exists(args.out_dir, ec)) {
        if (!fs::is_directory(args.out_dir, ec)) {
            return fail(ErrorCode::InvalidArgument,
                        "the output path exists and is not a directory: " + core::path_to_utf8_lossy(args.out_dir),
                        kWhere);
        }
        if (!fs::is_empty(args.out_dir, ec) || ec) {
            return fail(ErrorCode::InvalidArgument,
                        "the output directory is not empty (nothing is ever overwritten): " +
                            core::path_to_utf8_lossy(args.out_dir),
                        kWhere);
        }
    }
    return ok();
}

int run(const Arguments& args, const core::CancellationToken& cancel) {
    const auto start = Clock::now();
    const auto usage_error = [](const std::string& message) {
        std::fprintf(stderr, "lucent-auto: %s\n", core::sanitize_utf8(message).c_str());
        return kExitUsage;
    };
    const auto failed = [](const Error& error) {
        std::fprintf(stderr, "lucent-auto: %s\n", core::sanitize_utf8(error.describe()).c_str());
        return kExitFailed;
    };
    auto discovery = discover_inputs(args);
    if (!discovery) {
        return usage_error(discovery.error().message);
    }
    if (auto checked = check_output_directory(args, *discovery); !checked) {
        return usage_error(checked.error().message);
    }
    if (discovery->files.empty()) {
        return usage_error("no RAW files found in the inputs");
    }
    const fs::path& out = args.out_dir;
    for (const char* sub : {"", "items", "previews", "pairs", "blind"}) {
        if (auto created = render_tool::create_directories(*sub ? out / sub : out); !created) {
            return failed(created.error());
        }
    }
    auto registry = raw::create_default_registry();
    if (!registry) {
        return failed(registry.error());
    }

    // Items: image 0 of multi-image files.
    const std::size_t n = discovery->files.size();
    std::vector<ItemReport> reports(n);
    std::vector<an::RelatedItem> items(n);
    for (std::size_t i = 0; i < n; ++i) {
        reports[i].index = i;
        reports[i].input = discovery->files[i];
        items[i].path = discovery->files[i].path;
        raw::ProbeOptions probe;
        probe.describe_subimages = false;
        if (auto selection = registry->select(items[i].path, probe, cancel)) {
            reports[i].subimage_count = selection->probe.metadata.subimage_count;
            if (reports[i].subimage_count > 1) {
                items[i].subimage = 0;
                reports[i].subimage = 0;
            }
        }
    }

    std::vector<Pending> pending(n);
    const std::int32_t edge = args.preview_edge;
    std::size_t done = 0;
    an::RelatedCallbacks callbacks;
    callbacks.independent = [&](std::size_t i, const an::RelatedItemResult& r, an::AutoSession& session) {
        ++done;
        const an::AutoResult& ind = *r.independent;
        const an::EditVector& e = ind.candidates.at(ind.chosen).edit;
        std::fprintf(stderr, "[%zu/%zu] %s: %+.2f EV, wb %.3f/%.3f, shadows %+.2f, highlights %+.2f, confidence %.2f, "
                             "%.2f s\n",
                     done, n, reports[i].input.name.c_str(), e.exposure_ev, e.red_gain, e.blue_gain, e.shadows_ev,
                     e.highlights_ev, ind.confidence, ind.timing.total_seconds);
        auto as_shot = session.preview(ind.candidates.at(0).settings, edge, cancel);
        auto chosen = session.preview(r.settings, edge, cancel);
        if (!as_shot || !chosen) {
            reports[i].preview_error = !as_shot ? as_shot.error() : chosen.error();
            return;
        }
        auto a = encode(*as_shot);
        auto b = encode(*chosen);
        if (!a || !b) {
            reports[i].preview_error = !a ? a.error() : b.error();
            return;
        }
        pending[i].as_shot = std::move(*a);
        pending[i].auto_image = std::move(*b);
        pending[i].auto_settings = r.settings;
    };
    callbacks.final = [&](std::size_t i, const an::RelatedItemResult& r, an::AutoSession* session) {
        Pending& p = pending[i];
        if (r.error || !p.as_shot || !p.auto_image) {
            return;
        }
        if (session != nullptr && !(r.settings == p.auto_settings)) {
            auto rendered = session->preview(r.settings, edge, cancel);
            auto encoded = rendered ? encode(*rendered) : Expected<exporter::OutputImage>(Unexpected(rendered.error()));
            if (!encoded) {
                reports[i].preview_error = encoded.error();
                p = Pending{};
                return;
            }
            p.auto_image = std::move(*encoded);
            if (r.regularised && r.regularised->consistency) {
                const an::ConsistencyRecord& k = *r.regularised->consistency;
                std::fprintf(stderr, "        %s regularised in %s: %+.2f EV, consistency %.2f -> %.2f\n",
                             reports[i].input.name.c_str(), k.target.group_id.c_str(), k.adjustment.exposure_ev,
                             k.independent_terms.total, k.terms.total);
            }
        }
        const std::string stem = item_stem(i, reports[i].input.name);
        const exporter::ExportSettings settings = preview_settings();
        const auto write = [&](const exporter::OutputImage& image, const std::string& rel) -> Status {
            auto path = core::path_from_utf8(rel);
            if (!path) {
                return Unexpected(path.error());
            }
            auto receipt = exporter::write_verified(image, out / *path, settings, cancel);
            if (!receipt) {
                return Unexpected(receipt.error());
            }
            return ok();
        };
        const std::string as_shot_rel = relative("previews", stem + "-as-shot.jpg");
        const std::string auto_rel = relative("previews", stem + "-auto.jpg");
        const std::string pair_rel = relative("pairs", stem + ".jpg");
        Status status = write(*p.as_shot, as_shot_rel);
        if (status) {
            status = write(*p.auto_image, auto_rel);
        }
        if (status) {
            status = write(side_by_side(*p.as_shot, *p.auto_image), pair_rel);
        }
        if (status) {
            reports[i].as_shot_jpg = as_shot_rel;
            reports[i].auto_jpg = auto_rel;
            reports[i].pair_jpg = pair_rel;
        } else {
            reports[i].preview_error = status.error();
        }
        p = Pending{};
    };

    an::RelatedOptions options;
    options.grouping = args.group_by;
    options.grouping_options.max_gap_seconds = args.max_gap_seconds;
    auto result = an::develop_related_files(*registry, items, options, cancel, callbacks);
    if (!result) {
        return failed(result.error());
    }

    // Reports.
    for (std::size_t i = 0; i < n; ++i) {
        ItemReport& r = reports[i];
        r.result = &result->items[i];
        r.decision = result->grouping.decisions[i].reason;
        if (const auto g = result->grouping.group_of[i]) {
            const an::RelatedGroup& group = result->grouping.groups[*g];
            r.group_id = group.id;
            r.group_basis = group.basis;
            r.group_size = group.members.size();
        }
        r.item_json = relative("items", item_stem(i, r.input.name) + ".json");
    }
    const auto publish = [&](const fs::path& relative_path, const std::string& text) -> Status {
        return render_tool::publish_new_text_file(out / relative_path, text);
    };
    for (const ItemReport& r : reports) {
        auto path = core::path_from_utf8(r.item_json);
        if (!path) {
            return failed(path.error());
        }
        if (auto written = publish(*path, item_json(r).dump(2, ' ', false, Json::error_handler_t::replace) + "\n");
            !written) {
            return failed(written.error());
        }
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    const std::array<std::pair<fs::path, std::string>, 5> documents{{
        {L"summary.tsv", summary_tsv(reports)},
        {L"sheet.html", sheet_html(args, *result, reports, out, read_jpeg)},
        {fs::path(L"blind") / L"sheet.html", blind_html(args, reports, out, read_jpeg)},
        {fs::path(L"blind") / L"key.json",
         blind_key(args, reports).dump(2, ' ', false, Json::error_handler_t::replace) + "\n"},
        {L"run.json",
         run_json(args, *discovery, *result, reports, seconds).dump(2, ' ', false, Json::error_handler_t::replace) +
             "\n"},
    }};
    for (const auto& [path, text] : documents) {
        if (auto written = publish(path, text); !written) {
            return failed(written.error());
        }
    }
    const std::string table = summary_table(reports);
    std::fwrite(table.data(), 1, table.size(), stdout);
    std::fprintf(stdout, "lucent-auto: %zu images, %zu groups, %.1f s; wrote %s\n", n, result->grouping.groups.size(),
                 seconds, core::path_to_utf8_lossy(out).c_str());
    std::fflush(stdout);
    return kExitOk;
}

}  // namespace lucent::auto_tool
