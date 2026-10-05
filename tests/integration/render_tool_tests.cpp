// Tests of lucent-render and its support code (labels "fixtures", "integration"):
//   * the float TIFF dump writer/reader round trip (synthetic, always runs);
//   * the CIEDE2000 implementation used by the render gates, against Sharma et al.'s published
//     test pairs (always runs);
//   * the lucent-render executable: usage errors, missing input, side files that would name the
//     input or the export (refused) or already exist (never replaced), strict reading of --develop
//     recipes, the develop pipeline by default with its recipe identity in the result and report,
//     the W2 path (--legacy-neutral-v0) with report and dump, output directories beyond MAX_PATH,
//     and no-clobber on a second run (the render cases skip when dji-fc4382-dng, the smallest
//     smoke fixture, is not downloaded).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <lucent/application/recipe.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/text.hpp>
#include <lucent/engine/develop_settings.hpp>

#include "color_difference.hpp"
#include "fixture_manifest.hpp"
#include "float_tiff.hpp"

namespace lucent::integration {
namespace {

using Json = nlohmann::json;

std::filesystem::path scratch_dir(const std::string& name) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("lucent-tool-test-" + name + "-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

// ---- Float TIFF --------------------------------------------------------------------------------

TEST(FloatTiff, RoundTripsEverySampleBitExactly) {
    const std::filesystem::path dir = scratch_dir("tiff");
    for (const image::Size size :
         {image::Size{1, 1}, image::Size{7, 16}, image::Size{13, 17}, image::Size{64, 33}}) {
        auto buffer = image::ImageBuffer<float>::create(size.width, size.height, 3);
        ASSERT_TRUE(buffer);
        for (std::int32_t y = 0; y < size.height; ++y) {
            for (std::int32_t x = 0; x < size.width; ++x) {
                buffer->at(x, y, 0) = static_cast<float>(x) * 0.25f - 1.0f;
                buffer->at(x, y, 1) = static_cast<float>(y) * 1e-3f;
                buffer->at(x, y, 2) = x == 0 && y == 0 ? std::numeric_limits<float>::infinity() : 3.5e30f;
            }
        }
        const std::filesystem::path path =
            dir / (std::to_string(size.width) + "x" + std::to_string(size.height) + ".tiff");
        ASSERT_TRUE(render_tool::write_float_tiff(path, buffer->const_view(), "test encoding"));
        auto back = render_tool::read_float_tiff(path);
        ASSERT_TRUE(back) << back.error().describe();
        ASSERT_EQ(back->size(), size);
        for (std::int32_t y = 0; y < size.height; ++y) {
            for (std::int32_t x = 0; x < size.width; ++x) {
                for (std::int32_t c = 0; c < 3; ++c) {
                    ASSERT_EQ(back->at(x, y, c), buffer->at(x, y, c));
                }
            }
        }
        // Header (8) + IFD + values + data: the pixel data is the file's tail.
        EXPECT_GT(std::filesystem::file_size(path), static_cast<std::uintmax_t>(size.area()) * 12u);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(FloatTiff, RejectsInvalidInputAndForeignFiles) {
    const std::filesystem::path dir = scratch_dir("tiff-bad");
    auto mono = image::ImageBuffer<float>::create(4, 4, 1);
    ASSERT_TRUE(mono);
    auto written = render_tool::write_float_tiff(dir / "mono.tiff", mono->const_view(), "");
    ASSERT_FALSE(written);
    EXPECT_EQ(written.error().code, ErrorCode::InvalidArgument);

    {
        std::ofstream(dir / "text.tiff", std::ios::binary) << "not a tiff at all";
    }
    auto foreign = render_tool::read_float_tiff(dir / "text.tiff");
    ASSERT_FALSE(foreign);
    EXPECT_EQ(foreign.error().code, ErrorCode::Unsupported);

    auto rgb = image::ImageBuffer<float>::create(8, 20, 3);
    ASSERT_TRUE(rgb);
    ASSERT_TRUE(render_tool::write_float_tiff(dir / "ok.tiff", rgb->const_view(), "x"));
    std::filesystem::resize_file(dir / "ok.tiff", std::filesystem::file_size(dir / "ok.tiff") - 100);
    auto truncated = render_tool::read_float_tiff(dir / "ok.tiff");
    ASSERT_FALSE(truncated);
    EXPECT_EQ(truncated.error().code, ErrorCode::Corrupt);

    auto missing = render_tool::read_float_tiff(dir / "missing.tiff");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- CIEDE2000 -------------------------------------------------------------------------------

TEST(Ciede2000, MatchesSharmaTestData) {
    struct Pair {
        Lab a;
        Lab b;
        double expected;
    };
    // Sharma, Wu, Dalal (2005), table 1: pairs 1, 2, 7, 17, 18, 19, 20, 25, 34.
    const std::array<Pair, 9> pairs = {{
        {{50.0000, 2.6772, -79.7751}, {50.0000, 0.0000, -82.7485}, 2.0425},
        {{50.0000, 3.1571, -77.2803}, {50.0000, 0.0000, -82.7485}, 2.8615},
        {{50.0000, 0.0000, 0.0000}, {50.0000, -1.0000, 2.0000}, 2.3669},
        {{50.0000, 2.5000, 0.0000}, {73.0000, 25.0000, -18.0000}, 27.1492},
        {{50.0000, 2.5000, 0.0000}, {61.0000, -5.0000, 29.0000}, 22.8977},
        {{50.0000, 2.5000, 0.0000}, {56.0000, -27.0000, -3.0000}, 31.9030},
        {{50.0000, 2.5000, 0.0000}, {58.0000, 24.0000, 15.0000}, 19.4535},
        {{60.2574, -34.0099, 36.2677}, {60.4626, -34.1751, 39.4387}, 1.2644},
        {{2.0776, 0.0795, -1.1350}, {0.9033, -0.0636, -0.5514}, 0.9082},
    }};
    for (const Pair& p : pairs) {
        EXPECT_NEAR(ciede2000(p.a, p.b), p.expected, 1e-4);
        EXPECT_NEAR(ciede2000(p.b, p.a), p.expected, 1e-4);  // symmetric
    }
    EXPECT_EQ(ciede2000(Lab{40, 10, -5}, Lab{40, 10, -5}), 0.0);
}

TEST(Ciede2000, LabOfTheWhiteIsNeutral) {
    const Lab white = lab_from_linear_srgb(Rgb{1.0, 1.0, 1.0});
    EXPECT_NEAR(white[0], 100.0, 1e-9);
    EXPECT_NEAR(white[1], 0.0, 1e-9);
    EXPECT_NEAR(white[2], 0.0, 1e-9);
    EXPECT_NEAR(luminance_from_linear_srgb(Rgb{1.0, 1.0, 1.0}), 1.0, 1e-12);
    const Lab grey = lab_from_linear_srgb(Rgb{0.18, 0.18, 0.18});
    EXPECT_NEAR(grey[0], 49.4961, 1e-3);  // CIE L* of Y = 0.18
}

// ---- lucent-render executable --------------------------------------------------------------------

struct RunResult {
    DWORD exit_code = 0;
    std::string out;
};

std::filesystem::path tool_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n < buffer.size()) {
            buffer.resize(n);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path() / L"lucent-render.exe";
}

std::wstring quote(const std::wstring& arg) {
    std::wstring out = L"\"";
    for (const wchar_t c : arg) {
        if (c == L'"') {
            out += L'\\';
        }
        out += c;
    }
    return out + L"\"";
}

// Runs lucent-render with `args`, capturing standard output (standard error is discarded).
RunResult run_tool(const std::vector<std::wstring>& args) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    EXPECT_TRUE(CreatePipe(&read, &write, &sa, 0));
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                  OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = null_out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring command = quote(tool_path().wstring());
    for (const std::wstring& a : args) {
        command += L" " + quote(a);
    }
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &si, &pi);
    CloseHandle(write);
    CloseHandle(null_out);
    RunResult result;
    EXPECT_TRUE(started) << "cannot start " << tool_path().string();
    if (!started) {
        CloseHandle(read);
        result.exit_code = 0xFFFFFFFF;
        return result;
    }
    char chunk[4096];
    DWORD got = 0;
    while (ReadFile(read, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
        result.out.append(chunk, got);
    }
    CloseHandle(read);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &result.exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}

Json last_line(const std::string& out) {
    const auto end = out.find_last_not_of("\r\n");
    const auto start = out.rfind('\n', end);
    return Json::parse(out.substr(start == std::string::npos ? 0 : start + 1,
                                  end - (start == std::string::npos ? 0 : start + 1) + 1));
}

TEST(LucentRender, ReportsUsageErrorsAsJson) {
    ASSERT_TRUE(std::filesystem::exists(tool_path())) << tool_path().string();
    const RunResult none = run_tool({});
    EXPECT_EQ(none.exit_code, 1u);
    const Json j = last_line(none.out);
    EXPECT_EQ(j["schema"], "lucent.render-result");
    EXPECT_EQ(j["status"], "error");
    EXPECT_EQ(j["error"]["code"], "invalid_argument");
    EXPECT_EQ(j["error"]["where"], "lucent-render.usage");

    EXPECT_EQ(run_tool({L"x.cr3", L"--out", L"o", L"--format", L"png"}).exit_code, 1u);
    EXPECT_EQ(run_tool({L"x.cr3", L"--out", L"o", L"--format", L"tiff16", L"--quality", L"90"}).exit_code,
              1u);
    EXPECT_EQ(run_tool({L"x.cr3", L"--out", L"o", L"--quality", L"0"}).exit_code, 1u);
    EXPECT_EQ(run_tool({L"x.cr3"}).exit_code, 1u);
    EXPECT_EQ(run_tool({L"--help"}).exit_code, 0u);
    // --no-dng-opcodes belongs to the W2 path (the develop pipeline always applies the opcodes).
    const RunResult opcodes = run_tool({L"x.cr3", L"--out", L"o", L"--no-dng-opcodes"});
    EXPECT_EQ(opcodes.exit_code, 1u);
    EXPECT_NE(last_line(opcodes.out)["error"]["message"].get<std::string>().find("--legacy-neutral-v0"),
              std::string::npos);
}

TEST(LucentRender, MissingInputFailsWithNotFound) {
    const std::filesystem::path dir = scratch_dir("missing");
    const RunResult r =
        run_tool({(dir / L"does-not-exist.dng").wstring(), L"--out", (dir / L"out").wstring()});
    EXPECT_EQ(r.exit_code, 2u);
    const Json j = last_line(r.out);
    EXPECT_EQ(j["status"], "error");
    EXPECT_EQ(j["error"]["code"], "not_found");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

std::string file_digest(const std::filesystem::path& path) {
    auto hashed = core::hash_file(path, {});
    EXPECT_TRUE(hashed) << hashed.error().describe();
    return hashed ? core::to_hex(hashed->digest) : std::string();
}

TEST(LucentRender, SideFilesNeverNameTheInputOrTheExport) {
    // The checks run before any decoding, so the input need not be a RAW file.
    const std::filesystem::path dir = scratch_dir("side");
    const std::filesystem::path input = dir / L"src.dng";
    {
        std::ofstream(input, std::ios::binary) << "not really a raw file";
    }
    const std::string before = file_digest(input);
    const std::filesystem::path out = dir / L"out";
    const std::wstring export_name = (out / L"src_Lucent.jpg").wstring();
    ASSERT_TRUE(CreateHardLinkW((dir / L"link.dng").c_str(), input.c_str(), nullptr));
    const std::vector<std::vector<std::wstring>> collisions = {
        {L"--dump-linear", input.wstring()},
        {L"--report", input.wstring()},
        {L"--dump-linear", (dir / L"SRC.DNG").wstring()},  // case-insensitive
        {L"--report", (dir / L"." / L"src.dng").wstring()},
        {L"--report", (dir / L"link.dng").wstring()},  // hard link to the input
        {L"--report", export_name},
        {L"--dump-linear", export_name},
        {L"--report", (dir / L"same.bin").wstring(), L"--dump-linear", (dir / L"same.bin").wstring()},
    };
    for (const std::vector<std::wstring>& extra : collisions) {
        // --dump-linear belongs to the W2 path.
        std::vector<std::wstring> args = {input.wstring(), L"--out", out.wstring(), L"--legacy-neutral-v0"};
        args.insert(args.end(), extra.begin(), extra.end());
        const RunResult r = run_tool(args);
        EXPECT_EQ(r.exit_code, 1u) << core::wide_to_utf8_lossy(extra.back());
        const Json j = last_line(r.out);
        EXPECT_EQ(j["error"]["code"], "invalid_argument");
        EXPECT_EQ(j["error"]["where"], "lucent-render.usage");
    }
    EXPECT_EQ(file_digest(input), before);
    EXPECT_FALSE(std::filesystem::exists(out));
    EXPECT_FALSE(std::filesystem::exists(dir / L"same.bin"));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(LucentRender, ExistingSideFilesAreNeverReplaced) {
    const std::filesystem::path dir = scratch_dir("side-exists");
    const std::filesystem::path out = dir / L"out";
    const std::filesystem::path existing = dir / L"existing.json";
    {
        std::ofstream(existing, std::ios::binary) << "precious";
    }
    const std::string before = file_digest(existing);

    // An existing report: refused before anything else, not even an error report replaces it.
    const RunResult report =
        run_tool({(dir / L"missing.dng").wstring(), L"--out", out.wstring(), L"--report", existing.wstring()});
    EXPECT_EQ(report.exit_code, 2u);
    EXPECT_EQ(last_line(report.out)["error"]["code"], "already_exists");
    EXPECT_EQ(file_digest(existing), before);

    // An existing dump: refused; the (new) report records the failure.
    const std::filesystem::path report_path = dir / L"report.json";
    const RunResult dump =
        run_tool({(dir / L"missing.dng").wstring(), L"--out", out.wstring(), L"--legacy-neutral-v0",
                  L"--dump-linear", existing.wstring(), L"--report", report_path.wstring()});
    EXPECT_EQ(dump.exit_code, 2u);
    EXPECT_EQ(last_line(dump.out)["error"]["code"], "already_exists");
    EXPECT_EQ(file_digest(existing), before);
    const Json written = read_json(report_path);
    EXPECT_EQ(written["status"], "error");
    EXPECT_EQ(written["error"]["code"], "already_exists");

    // A failed render still writes its report (and only its report).
    const std::filesystem::path failure_report = dir / L"failure.json";
    const RunResult missing =
        run_tool({(dir / L"missing.dng").wstring(), L"--out", out.wstring(), L"--report", failure_report.wstring()});
    EXPECT_EQ(missing.exit_code, 2u);
    EXPECT_EQ(read_json(failure_report)["error"]["code"], "not_found");
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        EXPECT_EQ(entry.path().filename().wstring().rfind(L"~lucent-", 0), std::wstring::npos)
            << "temporary file left behind: " << entry.path().string();
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(LucentRender, WritesIntoOutputDirectoriesBeyondMaxPath) {
    const std::vector<Fixture> fixtures = load_manifest();
    const auto it = std::find_if(fixtures.begin(), fixtures.end(),
                                 [](const Fixture& f) { return f.id == "dji-fc4382-dng"; });
    ASSERT_NE(it, fixtures.end());
    const std::filesystem::path input = cached_file(*it);
    if (!std::filesystem::exists(input)) {
        GTEST_SKIP() << "fixture 'dji-fc4382-dng' is not downloaded; run tools\\fetch-fixtures.ps1";
    }
    const std::filesystem::path dir = scratch_dir("long");
    std::filesystem::path out = dir;
    while (out.native().size() < 320) {
        out /= std::wstring(40, L'd');
    }
    const std::filesystem::path report_path = out / L"report.json";
    const RunResult r = run_tool({input.wstring(), L"--out", out.wstring(), L"--report", report_path.wstring()});
    ASSERT_EQ(r.exit_code, 0u) << r.out;
    const std::wstring verbatim = L"\\\\?\\" + out.wstring();
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(verbatim) / (input.stem().wstring() + L"_Lucent.jpg")));
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(verbatim) / L"report.json"));
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(L"\\\\?\\" + dir.wstring()), ec);
}

// The smallest smoke fixture, or std::nullopt (with the reason in `skip`) when not downloaded.
std::optional<std::filesystem::path> small_fixture() {
    const std::vector<Fixture> fixtures = load_manifest();
    const auto it = std::find_if(fixtures.begin(), fixtures.end(),
                                 [](const Fixture& f) { return f.id == "dji-fc4382-dng"; });
    EXPECT_NE(it, fixtures.end());
    if (it == fixtures.end()) {
        return std::nullopt;
    }
    const std::filesystem::path input = cached_file(*it);
    if (!std::filesystem::exists(input)) {
        return std::nullopt;
    }
    return input;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

TEST(LucentRender, DevelopFilesAreReadStrictlyBeforeAnythingIsRendered) {
    const std::filesystem::path dir = scratch_dir("develop-strict");
    const std::filesystem::path out = dir / L"out";
    const std::filesystem::path input = dir / L"missing.dng";  // never reached
    auto canonical = app::write_recipe(engine::DevelopSettings{});
    ASSERT_TRUE(canonical);
    Json unknown = Json::parse(canonical->json);
    unknown["sharpening"] = 0.5;
    Json newer = Json::parse(canonical->json);
    newer["version"] = 2;
    Json newer_operation = Json::parse(canonical->json);
    newer_operation["operations"]["exposure"] = 2;
    const std::vector<std::pair<Json, std::string>> cases = {
        {unknown, "invalid_argument"},
        {newer, "version_mismatch"},
        {newer_operation, "version_mismatch"},
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const std::filesystem::path settings = dir / (L"settings" + std::to_wstring(i) + L".json");
        write_text(settings, cases[i].first.dump());
        const RunResult r =
            run_tool({input.wstring(), L"--out", out.wstring(), L"--develop", settings.wstring()});
        EXPECT_EQ(r.exit_code, 1u) << r.out;
        const Json j = last_line(r.out);
        EXPECT_EQ(j["error"]["code"], cases[i].second) << cases[i].first.dump();
        EXPECT_EQ(j["error"]["where"], "lucent-render.develop");
    }
    const RunResult absent =
        run_tool({input.wstring(), L"--out", out.wstring(), L"--develop", (dir / L"none.json").wstring()});
    EXPECT_EQ(absent.exit_code, 1u);
    EXPECT_EQ(last_line(absent.out)["error"]["code"], "not_found");
    // An exposure outside the recipe's range is refused too.
    const RunResult exposure = run_tool({input.wstring(), L"--out", out.wstring(), L"--exposure", L"6"});
    EXPECT_EQ(exposure.exit_code, 1u);
    // --dump-linear is the W2 path's; --develop is not.
    EXPECT_EQ(
        run_tool({input.wstring(), L"--out", out.wstring(), L"--dump-linear", (dir / L"d.tiff").wstring()})
            .exit_code,
        1u);
    EXPECT_EQ(run_tool({input.wstring(), L"--out", out.wstring(), L"--legacy-neutral-v0", L"--develop",
                        (dir / L"settings0.json").wstring()})
                  .exit_code,
              1u);
    EXPECT_FALSE(std::filesystem::exists(out));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(LucentRender, TheDevelopPipelineIsTheDefaultAndReportsTheRecipe) {
    const std::optional<std::filesystem::path> input = small_fixture();
    if (!input) {
        GTEST_SKIP() << "fixture 'dji-fc4382-dng' is not downloaded; run tools\\fetch-fixtures.ps1";
    }
    const std::filesystem::path dir = scratch_dir("develop");
    const std::filesystem::path report_path = dir / L"report.json";
    const RunResult r =
        run_tool({input->wstring(), L"--out", (dir / L"a").wstring(), L"--report", report_path.wstring()});
    ASSERT_EQ(r.exit_code, 0u) << r.out;
    const Json result = last_line(r.out);
    EXPECT_EQ(result["schema_version"], 2);
    EXPECT_EQ(result["pipeline"], "develop");
    const auto as_shot = app::write_recipe(engine::DevelopSettings{});
    ASSERT_TRUE(as_shot);
    EXPECT_EQ(result["settings_sha256"], core::to_hex(as_shot->sha256));

    const Json report = read_json(report_path);
    EXPECT_EQ(report["schema_version"], 2);
    EXPECT_EQ(report["pipeline"], "develop");
    EXPECT_EQ(report["recipe"]["schema"], "lucent.develop");
    EXPECT_EQ(report["recipe"]["version"], 1);
    EXPECT_EQ(report["recipe"]["sha256"], result["settings_sha256"]);
    EXPECT_EQ(report["recipe"]["document"], Json::parse(as_shot->json));
    const Json& render = report["render"];
    EXPECT_EQ(render["settings"], report["recipe"]["document"]);
    EXPECT_EQ(render["stage_one"]["kind"], "export");
    EXPECT_EQ(render["base_look"]["id"], "neutral");
    EXPECT_EQ(render["base_look"]["version"], 1);
    const std::vector<std::string> operations = {"white_balance", "camera_color", "working_space",
                                                 "exposure",      "local_tone",   "contrast",
                                                 "tone_curve",    "color",        "base_look"};
    ASSERT_EQ(render["operations"].size(), operations.size());
    for (std::size_t i = 0; i < operations.size(); ++i) {
        EXPECT_EQ(render["operations"][i]["operation"], operations[i]);
    }
    EXPECT_FALSE(render["operations"][4]["applied"].get<bool>());  // no highlights/shadows as shot
    EXPECT_EQ(render["color"]["path"], "dng_color_matrix");
    EXPECT_FALSE(report["metadata"].is_null());

    // A recipe file: its identity is recorded and the pixels differ.
    engine::DevelopSettings edited;
    edited.exposure_ev = 0.5;
    edited.shadows = 0.4;
    edited.white_balance = engine::KelvinWhiteBalance{4300.0, 3.0};
    const auto document = app::write_recipe(edited);
    ASSERT_TRUE(document);
    const std::filesystem::path settings = dir / L"edit.json";
    write_text(settings, Json::parse(document->json).dump(2));  // any formatting reads the same
    const RunResult developed =
        run_tool({input->wstring(), L"--out", (dir / L"b").wstring(), L"--develop", settings.wstring(),
                  L"--report", (dir / L"edit-report.json").wstring()});
    ASSERT_EQ(developed.exit_code, 0u) << developed.out;
    const Json edit_result = last_line(developed.out);
    EXPECT_EQ(edit_result["settings_sha256"], core::to_hex(document->sha256));
    EXPECT_NE(edit_result["sha256"], result["sha256"]);
    const Json edit_report = read_json(dir / L"edit-report.json");
    EXPECT_EQ(edit_report["recipe"]["file"], core::path_to_utf8_lossy(settings));
    EXPECT_TRUE(edit_report["render"]["operations"][4]["applied"].get<bool>());
    EXPECT_FALSE(edit_report["render"]["white_balance"]["target_temperature"].is_null());

    // Renders are deterministic: the same recipe gives the same file.
    const RunResult again =
        run_tool({input->wstring(), L"--out", (dir / L"c").wstring(), L"--develop", settings.wstring()});
    ASSERT_EQ(again.exit_code, 0u) << again.out;
    EXPECT_EQ(last_line(again.out)["sha256"], edit_result["sha256"]);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// The W2 render path stays available for the reference comparisons (render_reference.py).
TEST(LucentRender, RendersExportsReportsAndNeverOverwrites) {
    const std::optional<std::filesystem::path> fixture = small_fixture();
    if (!fixture) {
        GTEST_SKIP() << "fixture 'dji-fc4382-dng' is not downloaded; run tools\\fetch-fixtures.ps1";
    }
    const std::filesystem::path input = *fixture;
    const std::filesystem::path dir = scratch_dir("render");
    const std::filesystem::path out = dir / L"out";
    const std::filesystem::path report_path = dir / L"report.json";
    const std::filesystem::path dump_path = dir / L"linear.tiff";
    const std::vector<std::wstring> args = {input.wstring(),       L"--out",       out.wstring(),
                                            L"--legacy-neutral-v0", L"--format",    L"tiff16",
                                            L"--profile",          L"display-p3",  L"--report",
                                            report_path.wstring(), L"--dump-linear", dump_path.wstring()};
    const RunResult r = run_tool(args);
    ASSERT_EQ(r.exit_code, 0u) << r.out;
    const Json result = last_line(r.out);
    EXPECT_EQ(result["status"], "ok");
    EXPECT_EQ(result["pipeline"], "neutral-v0");
    EXPECT_TRUE(result["settings_sha256"].is_null());
    EXPECT_EQ(result["format"], "tiff");
    EXPECT_EQ(result["profile"], "display-p3");
    const std::filesystem::path output = out / (input.stem().wstring() + L"_Lucent.tif");
    ASSERT_TRUE(std::filesystem::exists(output));
    auto hashed = core::hash_file(output, {});
    ASSERT_TRUE(hashed);
    EXPECT_EQ(result["sha256"], core::to_hex(hashed->digest));

    const Json report = read_json(report_path);
    EXPECT_EQ(report["schema"], "lucent.render-report");
    EXPECT_EQ(report["schema_version"], 2);
    EXPECT_EQ(report["pipeline"], "neutral-v0");
    EXPECT_TRUE(report["recipe"].is_null());
    EXPECT_EQ(report["status"], "ok");
    EXPECT_EQ(report["export"]["sha256"], result["sha256"]);
    // dji-fc4382-dng carries an OpcodeList3 WarpRectilinear, applied after demosaicing (ADR 0014).
    const std::vector<std::string> expected = {
        "decode",       "normalize",     "white_balance",     "demosaic",   "dng_opcode_list3",
        "camera_color", "working_space", "display_rendering", "orientation"};
    ASSERT_EQ(report["render"]["stages"].size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(report["render"]["stages"][i]["stage"], expected[i]);
    }
    EXPECT_EQ(report["render"]["stages"][4]["parameters"]["applied"], "WarpRectilinear");
    EXPECT_EQ(report["render"]["color"]["path"], "dng_color_matrix");
    EXPECT_TRUE(report["render"]["stages"][7]["provisional"].get<bool>());
    EXPECT_GT(report["process"]["peak_working_set_bytes"].get<std::uint64_t>(), 0u);

    auto dump = render_tool::read_float_tiff(dump_path);
    ASSERT_TRUE(dump) << dump.error().describe();
    EXPECT_EQ(dump->width(), report["render"]["output_size"]["width"].get<std::int32_t>());
    EXPECT_EQ(dump->height(), report["render"]["output_size"]["height"].get<std::int32_t>());
    EXPECT_EQ(dump->width(), result["width"].get<std::int32_t>());

    // A second run must not overwrite the published file.
    const RunResult again = run_tool(args);
    EXPECT_EQ(again.exit_code, 2u);
    EXPECT_EQ(last_line(again.out)["error"]["code"], "already_exists");
    auto rehash = core::hash_file(output, {});
    ASSERT_TRUE(rehash);
    EXPECT_EQ(rehash->digest, hashed->digest);
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(out)) {
        (void)entry;
        ++files;
    }
    EXPECT_EQ(files, 1u);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// --no-dng-opcodes: the W2 path without the file's opcodes, for comparisons with decoders that
// apply none (render_reference.py). Every opcode is recorded as disabled, the render is not
// complete, and the pixels differ from the default render.
TEST(LucentRender, NoDngOpcodesRecordsTheOpcodesAsDisabled) {
    const std::optional<std::filesystem::path> fixture = small_fixture();
    if (!fixture) {
        GTEST_SKIP() << "fixture 'dji-fc4382-dng' is not downloaded; run tools\\fetch-fixtures.ps1";
    }
    const std::filesystem::path dir = scratch_dir("no-opcodes");
    const auto render = [&](const wchar_t* name, bool opcodes) {
        std::vector<std::wstring> args = {fixture->wstring(), L"--out", (dir / name).wstring(), L"--legacy-neutral-v0",
                                          L"--report", (dir / (std::wstring(name) + L".json")).wstring()};
        if (!opcodes) {
            args.push_back(L"--no-dng-opcodes");
        }
        return run_tool(args);
    };
    const RunResult with = render(L"with", true);
    ASSERT_EQ(with.exit_code, 0u) << with.out;
    const RunResult without = render(L"without", false);
    ASSERT_EQ(without.exit_code, 0u) << without.out;
    EXPECT_NE(last_line(with.out)["sha256"], last_line(without.out)["sha256"]);

    const Json on = read_json(dir / L"with.json");
    EXPECT_TRUE(on["render"]["settings"]["apply_dng_opcodes"].get<bool>());
    EXPECT_EQ(on["render"]["interpretation"]["status"], "complete");
    const Json off = read_json(dir / L"without.json");
    EXPECT_FALSE(off["render"]["settings"]["apply_dng_opcodes"].get<bool>());
    EXPECT_NE(off["render"]["interpretation"]["status"], "complete");
    bool found = false;
    for (const Json& stage : off["render"]["stages"]) {
        if (stage["stage"] == "dng_opcode_list3") {
            found = true;
            EXPECT_EQ(stage["parameters"]["applied_count"], 0);
            EXPECT_EQ(stage["parameters"]["opcode_0"].get<std::string>().rfind("disabled: WarpRectilinear", 0), 0u)
                << stage["parameters"]["opcode_0"];
        }
    }
    EXPECT_TRUE(found);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace
}  // namespace lucent::integration
