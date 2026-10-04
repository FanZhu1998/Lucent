// lucent-auto (auto_tool.hpp): argument handling, discovery with fixture tiers, the output-folder
// protection, the blind order and base64 (always run), and the executable's output layout on the
// smallest smoke fixture and on a related pair (skip when the fixtures are not downloaded). The
// source folders are checked to be untouched.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include "auto_report.hpp"
#include "auto_tool.hpp"

namespace lucent::auto_tool {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() /
                         ("lucent-auto-test-" + name + "-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void touch(const fs::path& p, const std::string& text = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

Arguments parsed(const std::vector<std::wstring>& args) {
    auto r = parse_arguments(args);
    EXPECT_TRUE(std::holds_alternative<Arguments>(r)) << (std::holds_alternative<std::string>(r)
                                                              ? std::get<std::string>(r)
                                                              : std::string());
    return std::holds_alternative<Arguments>(r) ? std::get<Arguments>(r) : Arguments{};
}

std::string usage_error(const std::vector<std::wstring>& args) {
    auto r = parse_arguments(args);
    EXPECT_TRUE(std::holds_alternative<std::string>(r));
    return std::holds_alternative<std::string>(r) ? std::get<std::string>(r) : std::string();
}

TEST(LucentAutoArguments, DefaultsAndOptions) {
    const Arguments d = parsed({L"a.dng"});
    EXPECT_EQ(d.inputs, (std::vector<fs::path>{L"a.dng"}));
    EXPECT_EQ(d.group_by, analysis::GroupingMode::CaptureTime);
    EXPECT_EQ(d.out_dir, fs::path(L"lucent-auto-out"));
    EXPECT_FALSE(d.tier.has_value());
    EXPECT_EQ(d.preview_edge, kDefaultPreviewEdge);
    EXPECT_EQ(d.seed, kDefaultSeed);
    EXPECT_DOUBLE_EQ(d.max_gap_seconds, 60.0);

    const Arguments a = parsed({L"--group-by", L"none", L"--out", L"o", L"--tier", L"smoke", L"x", L"--max-gap",
                                L"30", L"--preview", L"640", L"--seed", L"7", L"y"});
    EXPECT_EQ(a.inputs, (std::vector<fs::path>{L"x", L"y"}));
    EXPECT_EQ(a.group_by, analysis::GroupingMode::None);
    EXPECT_EQ(a.out_dir, fs::path(L"o"));
    EXPECT_EQ(a.tier, Tier::Smoke);
    EXPECT_DOUBLE_EQ(a.max_gap_seconds, 30.0);
    EXPECT_EQ(a.preview_edge, 640);
    EXPECT_EQ(a.seed, 7u);
    EXPECT_EQ(parsed({L"--tier", L"all", L"x"}).tier, Tier::All);
    EXPECT_TRUE(parsed({L"--help"}).help);
}

TEST(LucentAutoArguments, UsageErrors) {
    EXPECT_EQ(usage_error({}), "no input files or folders");
    EXPECT_NE(usage_error({L"--group-by", L"day", L"x"}).find("--group-by"), std::string::npos);
    EXPECT_NE(usage_error({L"--tier", L"gold", L"x"}).find("--tier"), std::string::npos);
    EXPECT_NE(usage_error({L"--preview", L"50", L"x"}).find("--preview"), std::string::npos);
    EXPECT_NE(usage_error({L"--preview", L"640.5", L"x"}).find("--preview"), std::string::npos);
    EXPECT_NE(usage_error({L"--max-gap", L"-1", L"x"}).find("--max-gap"), std::string::npos);
    EXPECT_NE(usage_error({L"--seed", L"-3", L"x"}).find("--seed"), std::string::npos);
    EXPECT_NE(usage_error({L"--out", L"a", L"--out", L"b", L"x"}).find("twice"), std::string::npos);
    EXPECT_NE(usage_error({L"x", L"--out"}).find("incomplete"), std::string::npos);
    EXPECT_NE(usage_error({L"--bogus", L"x"}).find("--bogus"), std::string::npos);
}

// A fixture tree: root/fixtures/manifest.json, cache/a/A.NEF (smoke), cache/b/B.dng
// (qualification), cache/b/notes.txt, and a loose folder without a manifest.
fs::path make_tree(const std::string& name) {
    const fs::path root = scratch(name);
    touch(root / "fixtures" / "manifest.json",
          R"({"schema": "lucent.fixture-manifest", "schema_version": 1, "fixtures": [
              {"id": "a", "tier": "smoke", "file_name": "A.NEF"},
              {"id": "b", "tier": "qualification", "file_name": "B.dng"}]})");
    touch(root / "fixtures" / "cache" / "a" / "A.NEF");
    touch(root / "fixtures" / "cache" / "b" / "B.dng");
    touch(root / "fixtures" / "cache" / "b" / "notes.txt");
    touch(root / "loose" / "Holiday 01.cr3");
    touch(root / "loose" / "readme.md");
    return root;
}

TEST(LucentAutoDiscovery, FindsRawFilesAndAppliesTiers) {
    const fs::path root = make_tree("discovery");
    Arguments args;
    args.inputs = {root / "fixtures" / "cache", root / "loose"};
    auto all = discover_inputs(args);
    ASSERT_TRUE(all) << all.error().describe();
    ASSERT_EQ(all->files.size(), 3u);
    EXPECT_EQ(all->files[0].name, "a");
    EXPECT_EQ(all->files[0].tier, "smoke");
    EXPECT_EQ(all->files[1].name, "b");
    EXPECT_EQ(all->files[1].tier, "qualification");
    EXPECT_EQ(all->files[2].name, "Holiday_01");  // sanitised stem, no fixture
    EXPECT_TRUE(all->files[2].fixture.empty());

    args.tier = Tier::Smoke;
    auto smoke = discover_inputs(args);
    ASSERT_TRUE(smoke) << smoke.error().describe();
    ASSERT_EQ(smoke->files.size(), 1u);
    EXPECT_EQ(smoke->files[0].fixture, "a");
    EXPECT_EQ(smoke->skipped.size(), 2u);  // b (qualification) and the loose file (no manifest)

    args.tier = Tier::All;
    auto every = discover_inputs(args);
    ASSERT_TRUE(every);
    EXPECT_EQ(every->files.size(), 2u);

    // The same file named twice counts once; an explicit file is taken whatever its extension.
    Arguments twice;
    twice.inputs = {root / "loose" / "Holiday 01.cr3", root / "loose", root / "loose" / "readme.md"};
    auto d = discover_inputs(twice);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->files.size(), 2u);

    Arguments missing;
    missing.inputs = {root / "nope"};
    auto none = discover_inputs(missing);
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().code, ErrorCode::NotFound);

    Arguments no_manifest;
    no_manifest.inputs = {root / "loose"};
    no_manifest.tier = Tier::Smoke;
    auto refused = discover_inputs(no_manifest);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidArgument);
}

TEST(LucentAutoDiscovery, NeverWritesIntoSourceFolders) {
    const fs::path root = make_tree("protect");
    Arguments args;
    args.inputs = {root / "fixtures" / "cache"};
    auto d = discover_inputs(args);
    ASSERT_TRUE(d);
    const auto check = [&](const fs::path& out) {
        Arguments a = args;
        a.out_dir = out;
        return check_output_directory(a, *d);
    };
    EXPECT_FALSE(check(root / "fixtures" / "cache"));                 // the input folder itself
    EXPECT_FALSE(check(root / "fixtures" / "cache" / "out"));         // inside it
    EXPECT_FALSE(check(root / "fixtures" / "cache" / "a" / "deep"));  // inside a file's folder
    EXPECT_FALSE(check(root / "FIXTURES" / "Cache" / "x"));           // case-insensitive
    EXPECT_TRUE(check(root / "fixtures" / "out"));                    // beside it: fine
    EXPECT_TRUE(check(root / "fresh"));
    touch(root / "used" / "old.txt");
    auto used = check(root / "used");                                 // not empty: never overwritten
    ASSERT_FALSE(used);
    EXPECT_NE(used.error().message.find("not empty"), std::string::npos);
    fs::create_directories(root / "empty");
    EXPECT_TRUE(check(root / "empty"));
    // An explicit file: its folder is a source folder.
    Arguments file;
    file.inputs = {root / "loose" / "Holiday 01.cr3"};
    auto fd = discover_inputs(file);
    ASSERT_TRUE(fd);
    file.out_dir = root / "loose" / "results";
    EXPECT_FALSE(check_output_directory(file, *fd));
}

TEST(LucentAutoBlind, OrderIsFixedPerSeedAndBalanced) {
    std::size_t first = 0;
    std::vector<bool> a;
    std::vector<bool> b;
    for (std::size_t i = 0; i < 1000; ++i) {
        a.push_back(auto_first(kDefaultSeed, i));
        b.push_back(auto_first(kDefaultSeed + 1, i));
        first += a.back() ? 1u : 0u;
        EXPECT_EQ(a.back(), auto_first(kDefaultSeed, i));
    }
    EXPECT_GT(first, 430u);
    EXPECT_LT(first, 570u);
    EXPECT_NE(a, b);
}

TEST(LucentAutoReport, Base64AndEscaping) {
    const auto b64 = [](const std::string& s) {
        return base64(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
    };
    EXPECT_EQ(b64(""), "");
    EXPECT_EQ(b64("f"), "Zg==");
    EXPECT_EQ(b64("fo"), "Zm8=");
    EXPECT_EQ(b64("foo"), "Zm9v");
    EXPECT_EQ(b64("foob"), "Zm9vYg==");
    EXPECT_EQ(b64("foobar"), "Zm9vYmFy");
    EXPECT_EQ(html_escape("<a href=\"x\">&'</a>"), "&lt;a href=&quot;x&quot;&gt;&amp;&#39;&lt;/a&gt;");
}

// ---- The executable ----------------------------------------------------------------------------

fs::path tool_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n < buffer.size()) {
            buffer.resize(n);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return fs::path(buffer).parent_path() / L"lucent-auto.exe";
}

std::wstring quote(const std::wstring& arg) {
    std::wstring out = L"\"";
    for (const wchar_t c : arg) {
        if (c == L'"') {
            out += L'\\';
        }
        out += c;
    }
    if (!out.empty() && out.back() == L'\\') {
        out += L'\\';  // a trailing backslash would escape the closing quote
    }
    return out + L"\"";
}

DWORD run_tool(const std::vector<std::wstring>& args) {
    std::wstring command = quote(tool_path().wstring());
    for (const std::wstring& a : args) {
        command += L" " + quote(a);
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE null_out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                                  nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = null_out;
    si.hStdError = null_out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &si, &pi);
    CloseHandle(null_out);
    EXPECT_TRUE(started) << "cannot start " << tool_path().string();
    if (!started) {
        return 0xFFFFFFFF;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

// Every file under `dir` with its size and modification time.
std::map<std::wstring, std::pair<std::uintmax_t, fs::file_time_type>> listing(const fs::path& dir) {
    std::map<std::wstring, std::pair<std::uintmax_t, fs::file_time_type>> out;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        out[e.path().wstring()] = {e.is_regular_file() ? e.file_size() : 0, e.last_write_time()};
    }
    return out;
}

Json read_json(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return Json::parse(in);
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

TEST(LucentAutoTool, UsageErrorsExitWithOne) {
    ASSERT_TRUE(fs::exists(tool_path())) << tool_path().string();
    EXPECT_EQ(run_tool({}), 1u);
    EXPECT_EQ(run_tool({L"--group-by", L"week", L"x"}), 1u);
    EXPECT_EQ(run_tool({L"--help"}), 0u);
    const fs::path dir = scratch("tool-usage");
    EXPECT_EQ(run_tool({(dir / L"missing").wstring(), L"--out", (dir / L"o").wstring()}), 1u);
    EXPECT_FALSE(fs::exists(dir / L"o"));
}

TEST(LucentAutoTool, WritesTheEvaluationSetForASmokeFixture) {
    const fs::path fixtures = fs::path(LUCENT_FIXTURE_DIR);
    const fs::path source = fixtures / "cache" / "dji-fc4382-dng";
    if (!fs::exists(source)) {
        GTEST_SKIP() << "fixture 'dji-fc4382-dng' is not downloaded; run tools\\fetch-fixtures.ps1";
    }
    const auto before = listing(source);
    const fs::path out = scratch("tool-smoke") / "out";

    // Into a source folder: refused before anything is written.
    EXPECT_EQ(run_tool({L"--tier", L"smoke", L"--out", (source / L"auto").wstring(), source.wstring()}), 1u);
    EXPECT_FALSE(fs::exists(source / L"auto"));

    ASSERT_EQ(run_tool({L"--tier", L"smoke", L"--preview", L"480", L"--out", out.wstring(), source.wstring()}), 0u);
    const std::string stem = "000-dji-fc4382-dng";
    for (const fs::path& p : {fs::path("run.json"), fs::path("summary.tsv"), fs::path("sheet.html"),
                              fs::path("blind") / "sheet.html", fs::path("blind") / "key.json",
                              fs::path("items") / (stem + ".json"), fs::path("previews") / (stem + "-as-shot.jpg"),
                              fs::path("previews") / (stem + "-auto.jpg"), fs::path("pairs") / (stem + ".jpg")}) {
        EXPECT_TRUE(fs::is_regular_file(out / p)) << p.string();
    }
    const Json run = read_json(out / "run.json");
    EXPECT_EQ(run["schema"], kRunSchema);
    EXPECT_EQ(run["schema_version"], kSchemaVersion);
    EXPECT_EQ(run["arguments"]["tier"], "smoke");
    EXPECT_EQ(run["totals"]["items"], 1);
    EXPECT_EQ(run["totals"]["failed"], 0);
    ASSERT_EQ(run["items"].size(), 1u);
    EXPECT_EQ(run["items"][0]["status"], "ok");
    EXPECT_EQ(run["items"][0]["item"], "items/" + stem + ".json");
    EXPECT_EQ(run["groups"].size(), 1u);
    EXPECT_EQ(run["versions"]["consistency_version"], analysis::kConsistencyVersion);

    const Json item = read_json(out / "items" / (stem + ".json"));
    EXPECT_EQ(item["schema"], kItemSchema);
    EXPECT_EQ(item["status"], "ok");
    EXPECT_EQ(item["source"]["fixture"]["id"], "dji-fc4382-dng");
    EXPECT_EQ(item["source"]["fixture"]["tier"], "smoke");
    const Json& result = item["result"];
    EXPECT_TRUE(result["settings"]["recipe_sha256"].is_string());
    EXPECT_EQ(result["settings"]["recipe"]["schema"], "lucent.develop");
    EXPECT_TRUE(result["settings"]["exposure_ev"].is_number());
    EXPECT_TRUE(result["confidence"].is_number());
    EXPECT_TRUE(result["flags"].is_array());
    EXPECT_FALSE(result["diagnostics"].empty());
    EXPECT_GT(result["timing"]["total_seconds"].get<double>(), 0.0);
    EXPECT_GT(result["candidates"].size(), 8u);
    EXPECT_EQ(item["previews"]["pair"], "pairs/" + stem + ".jpg");
    EXPECT_FALSE(item["regularised"].get<bool>());  // alone: no consistency stage

    // The sheets: self-contained, and the blind one tells nothing but A and B.
    const std::string sheet = read_text(out / "sheet.html");
    EXPECT_NE(sheet.find("data:image/jpeg;base64,"), std::string::npos);
    EXPECT_NE(sheet.find("Natural v1 automatic"), std::string::npos);
    EXPECT_EQ(sheet.find("src=\"previews/"), std::string::npos);
    const std::string blind = read_text(out / "blind" / "sheet.html");
    EXPECT_NE(blind.find("data:image/jpeg;base64,"), std::string::npos);
    for (const char* leak : {"As shot", "as_shot", "automatic</figcaption>", "Exposure", "auto.jpg"}) {
        EXPECT_EQ(blind.find(leak), std::string::npos) << leak;
    }
    const Json key = read_json(out / "blind" / "key.json");
    EXPECT_EQ(key["schema"], kBlindKeySchema);
    ASSERT_EQ(key["items"].size(), 1u);
    const bool first = auto_first(kDefaultSeed, 0);
    EXPECT_EQ(key["items"][0]["A"], first ? "auto" : "as_shot");
    EXPECT_EQ(key["items"][0]["B"], first ? "as_shot" : "auto");

    // Nothing was written into or changed in the source folder; a second run never overwrites.
    EXPECT_EQ(listing(source), before);
    EXPECT_EQ(run_tool({L"--tier", L"smoke", L"--out", out.wstring(), source.wstring()}), 1u);
}

TEST(LucentAutoTool, GroupsARelatedPair) {
    const fs::path cache = fs::path(LUCENT_FIXTURE_DIR) / "cache";
    const fs::path a = cache / "nikon-z-6-nef-compressed";
    const fs::path b = cache / "nikon-z-6-nef-lossless";
    if (!fs::exists(a) || !fs::exists(b)) {
        GTEST_SKIP() << "the Nikon Z 6 fixtures are not downloaded; run tools\\fetch-fixtures.ps1 -Tier all";
    }
    const fs::path out = scratch("tool-pair") / "out";
    ASSERT_EQ(run_tool({L"--preview", L"320", L"--out", out.wstring(), a.wstring(), b.wstring()}), 0u);
    const Json run = read_json(out / "run.json");
    ASSERT_EQ(run["groups"].size(), 1u);
    EXPECT_EQ(run["groups"][0]["id"], "time-1");
    EXPECT_EQ(run["groups"][0]["basis"], "capture_time");
    EXPECT_EQ(run["groups"][0]["members"].size(), 2u);
    EXPECT_TRUE(run["decisions"][1]["joined"].get<bool>());
    for (int i = 0; i < 2; ++i) {
        const std::string name = i == 0 ? "000-nikon-z-6-nef-compressed" : "001-nikon-z-6-nef-lossless";
        const Json item = read_json(out / "items" / (name + ".json"));
        EXPECT_EQ(item["group"]["id"], "time-1");
        EXPECT_TRUE(item["regularised"].get<bool>());
        EXPECT_TRUE(item["result"]["consistency"].is_object());
        EXPECT_EQ(item["result"]["consistency"]["target"]["group"], "time-1");
        EXPECT_TRUE(item["independent"].is_object());
    }
    // --group-by none: two single images, no consistency stage.
    const fs::path alone = scratch("tool-pair-none") / "out";
    ASSERT_EQ(run_tool({L"--group-by", L"none", L"--preview", L"320", L"--out", alone.wstring(), a.wstring(),
                        b.wstring()}),
              0u);
    const Json none = read_json(alone / "run.json");
    EXPECT_EQ(none["groups"].size(), 2u);
    EXPECT_EQ(none["totals"]["regularised"], 0);
}

}  // namespace
}  // namespace lucent::auto_tool
