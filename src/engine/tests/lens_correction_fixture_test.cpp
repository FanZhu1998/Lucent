// Lens-profile corrections on the camera fixtures (lens_correction.hpp, ADR 0023): which corpus
// files the bundled Lensfun database matches and what an automatic develop corrects in each, that a
// DNG's own opcode corrections are never applied a second time, the exact matches rendered, and,
// disabled (--gtest_also_run_disabled_tests), the cost on the 45 MP Canon EOS R5 file and the
// largest exact matches, and renders dumped for inspection (LUCENT_LENS_DUMP_DIR). Cases whose
// fixture is not downloaded skip (tools\fetch-fixtures.ps1 -Tier all).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/engine/develop.hpp>
#include <lucent/engine/lens_correction.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "develop_test_support.hpp"
#include "lens_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

// What an automatic develop (the default settings) of each corpus file does with the bundled
// database 1.0.0 (Lensfun data of 2026-10-08) and LibRaw 0.22.2: the match kind and which
// corrections are applied ("d" distortion, "t" lateral CA, "v" vignetting). The table's status and
// derivation are stated in the .inc file (predicted until a run confirms it). Every fixture of
// fixtures/manifest.json is listed; a fixture that is not downloaded skips.
struct CorpusCase {
    const char* id;
    LensMatchKind match;
    const char* applied;  // subset of "dtv"
};

const std::vector<CorpusCase>& corpus_expectations() {
    static const std::vector<CorpusCase> table = {
#include "data/lens_corpus_expectations.inc"
    };
    return table;
}

struct Probed {
    raw::RawMetadata metadata;
    image::Size upright;
};

std::optional<Probed> probe(const raw::DecoderRegistry& registry, const std::filesystem::path& file) {
    auto selection = registry.select(file, raw::ProbeOptions{}, {});
    if (!selection) {
        return std::nullopt;
    }
    Probed p;
    p.metadata = selection->probe.metadata;
    const image::PixelRect& v = p.metadata.geometry.visible_area;
    p.upright = image::oriented_size(image::Size{v.width, v.height}, p.metadata.geometry.orientation);
    return p;
}

std::string applied_letters(const LensCorrectionPlan& p) {
    std::string out;
    if (p.applied(LensCorrectionKind::Distortion))
        out += 'd';
    if (p.applied(LensCorrectionKind::LateralChromaticAberration))
        out += 't';
    if (p.applied(LensCorrectionKind::Vignetting))
        out += 'v';
    return out;
}

// The corrections the file's own DNG opcodes carry, as letters ("d", "t", "v").
std::string carried_letters(const CarriedLensCorrections& c) {
    std::string out;
    if (c.distortion)
        out += 'd';
    if (c.lateral_chromatic_aberration)
        out += 't';
    if (c.vignetting)
        out += 'v';
    return out;
}

TEST(LensFixtures, TheCorpusMatchesAsDocumented) {
    auto db = ts::bundled_lens_database();
    ASSERT_TRUE(db) << "assets/lens-database is missing";
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    std::size_t probed = 0;
    std::ostringstream table;
    table << "\n| Fixture | Camera (file) | Lens (file) | f | f-number | 35 mm eq. | Match | Profile | "
             "Applied | Carried by the file | Why |\n|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const CorpusCase& e : corpus_expectations()) {
        const auto file = ts::fixture_file(e.id);
        if (!file) {
            continue;
        }
        auto p = probe(*registry, *file);
        ASSERT_TRUE(p) << e.id;
        ++probed;
        const bool mono = p->metadata.representation.kind == raw::RawRepresentation::Monochrome;
        auto plan = db->plan(p->metadata, LensCorrectionSetting{}, p->upright,
                             p->metadata.geometry.orientation, mono);
        ASSERT_TRUE(plan) << e.id << ": " << plan.error().describe();
        const LensQuery& q = plan->query;
        std::string why;
        if (plan->any_applied()) {
            why = plan->match.reasons.empty() ? std::string{} : plan->match.reasons.back();
        } else {
            for (const LensCorrectionDecision& d : plan->decisions) {
                if (d.outcome != LensCorrectionOutcome::Disabled) {
                    why = std::string(to_string(d.outcome)) + ": " + d.detail;
                    break;
                }
            }
        }
        const auto num = [](const std::optional<double>& v) {
            std::ostringstream s;
            if (v)
                s << std::setprecision(4) << *v;
            return s.str();
        };
        table << "| " << e.id << " | " << q.camera_make << " " << q.camera_model << " | "
              << (q.lens_make.empty() ? "" : q.lens_make + " ") << q.lens_model << " | "
              << num(q.focal_length_mm) << " | " << num(q.f_number) << " | " << num(q.focal_length_35mm_mm)
              << " | " << to_string(plan->match.kind) << " | "
              << (plan->profile ? plan->profile->maker + " / " + plan->profile->model : std::string{})
              << " | " << applied_letters(*plan) << " | " << carried_letters(plan->carried) << " | " << why
              << " |\n";
        EXPECT_EQ(plan->match.kind, e.match) << e.id << ": " << to_string(plan->match.kind);
        EXPECT_EQ(applied_letters(*plan), std::string(e.applied)) << e.id;
        // The match never guesses: an automatic correction needs an exact match.
        if (plan->any_applied()) {
            EXPECT_EQ(plan->match.kind, LensMatchKind::Exact) << e.id;
        }
    }
    std::cout << table.str();
    if (probed == 0) {
        GTEST_SKIP() << "no camera fixture is downloaded; run tools\\fetch-fixtures.ps1";
    }
}

// DNGs that carry lens corrections in their opcodes (ADR 0014) and whose camera has a profile in the
// database: the DJI FC220 (OpcodeList3 FixVignetteRadial) and the Hasselblad L2D-20c (OpcodeList3
// WarpRectilinear with three different coefficient sets, and a GainMap over the whole image). The
// corpus's other DNGs with lens opcodes (DJI FC4382 and FC7303, OnePlus One, bq Aquaris U Plus) have
// no camera in the database. Automatic mode applies nothing to them, and their own profile, chosen,
// skips every carried correction: never applied twice.
TEST(LensFixtures, CorrectionsADngCarriesAreNeverAppliedTwice) {
    auto db = ts::bundled_lens_database();
    ASSERT_TRUE(db);
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    struct Case {
        const char* id;
        LensProfileId profile;  // the camera's own lens in the database
        bool distortion_carried;
        bool tca_carried;
        bool vignetting_carried;
    };
    const std::vector<Case> cases = {
        {"dji-fc220-dng", {"DJI", "Mavic Pro FC220 & compatibles", 5.64}, false, false, true},
        {"hasselblad-l2d-20c-dng", {"Hasselblad", "L2D-20c & compatibles", 1.953}, true, true, true},
    };
    std::size_t ran = 0;
    for (const Case& c : cases) {
        SCOPED_TRACE(c.id);
        const auto file = ts::fixture_file(c.id);
        if (!file) {
            continue;
        }
        ++ran;
        auto p = probe(*registry, *file);
        ASSERT_TRUE(p);
        const CarriedLensCorrections carried = carried_lens_corrections(p->metadata);
        EXPECT_EQ(carried.distortion, c.distortion_carried);
        EXPECT_EQ(carried.lateral_chromatic_aberration, c.tca_carried);
        EXPECT_EQ(carried.vignetting, c.vignetting_carried);
        ASSERT_TRUE(db->contains(c.profile));
        // Automatic: nothing at all.
        auto automatic = db->plan(p->metadata, LensCorrectionSetting{}, p->upright,
                                  p->metadata.geometry.orientation, false);
        ASSERT_TRUE(automatic);
        EXPECT_FALSE(automatic->any_applied());
        // The camera's own profile, chosen: every carried correction is skipped and recorded.
        LensCorrectionSetting named;
        named.profile = c.profile;
        auto chosen = db->plan(p->metadata, named, p->upright, p->metadata.geometry.orientation, false);
        ASSERT_TRUE(chosen);
        EXPECT_EQ(
            chosen->decision(LensCorrectionKind::Distortion).outcome == LensCorrectionOutcome::CarriedByFile,
            c.distortion_carried);
        EXPECT_EQ(chosen->decision(LensCorrectionKind::LateralChromaticAberration).outcome ==
                      LensCorrectionOutcome::CarriedByFile,
                  c.tca_carried);
        EXPECT_EQ(
            chosen->decision(LensCorrectionKind::Vignetting).outcome == LensCorrectionOutcome::CarriedByFile,
            c.vignetting_carried);
        EXPECT_FALSE(chosen->vignetting.has_value());
        // Rendered: enabling the carried correction changes no pixel.
        StageOneRequest request;
        request.proxy_long_edge = 1024;
        auto one = render_stage_one_file(*registry, *file, 0u, request);
        ASSERT_TRUE(one) << one.error().describe();
        DevelopOptions options;
        options.lens_database = db;
        DevelopSettings with;
        with.lens_correction = named;
        DevelopSettings without = with;
        without.lens_correction.vignetting = false;
        if (c.distortion_carried) {
            without.lens_correction.distortion = false;
        }
        if (c.tca_carried) {
            without.lens_correction.lateral_chromatic_aberration = false;
        }
        auto a = render_stage_two(*one, with, options);
        auto b = render_stage_two(*one, without, options);
        ASSERT_TRUE(a && b);
        EXPECT_TRUE(ts::bit_identical(a->display.pixels, b->display.pixels));
        std::cout
            << "[" << c.id << "] chosen profile: distortion "
            << to_string(a->report.lens_correction.plan.decision(LensCorrectionKind::Distortion).outcome)
            << ", lateral CA "
            << to_string(
                   a->report.lens_correction.plan.decision(LensCorrectionKind::LateralChromaticAberration)
                       .outcome)
            << ", vignetting "
            << to_string(a->report.lens_correction.plan.decision(LensCorrectionKind::Vignetting).outcome)
            << "\n";
    }
    if (ran == 0) {
        GTEST_SKIP() << "the drone DNG fixtures are not downloaded";
    }
}

// Stage one of a corpus file, or std::nullopt (with a note) when this build's decoder cannot decode
// it: LibRaw 0.22.2 recognises the Nikon Z 8 High Efficiency NEFs but cannot decode their data
// (their lens is still matched from the probe, TheCorpusMatchesAsDocumented).
std::optional<StageOneImage> decodable_stage_one(const raw::DecoderRegistry& registry,
                                                 const std::filesystem::path& file, const StageOneRequest& request,
                                                 const std::string& id) {
    auto one = render_stage_one_file(registry, file, 0u, request);
    if (!one && one.error().code == ErrorCode::Unsupported) {
        std::cout << "[" << id << "] not decodable by this build: " << one.error().describe() << "\n";
        return std::nullopt;
    }
    EXPECT_TRUE(one) << id << ": " << one.error().describe();
    if (!one) {
        return std::nullopt;
    }
    return std::move(*one);
}

// The exact matches of the corpus render with their corrections: the frame stays filled (no source
// position beyond the image) and the corrections change the render.
TEST(LensFixtures, ExactMatchesRenderTheirCorrections) {
    auto db = ts::bundled_lens_database();
    ASSERT_TRUE(db);
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    std::size_t ran = 0;
    for (const CorpusCase& e : corpus_expectations()) {
        if (std::string(e.applied).empty()) {
            continue;
        }
        const auto file = ts::fixture_file(e.id);
        if (!file) {
            continue;
        }
        SCOPED_TRACE(e.id);
        StageOneRequest request;
        request.proxy_long_edge = 1024;
        auto one = decodable_stage_one(*registry, *file, request, e.id);
        if (!one) {
            continue;
        }
        ++ran;
        DevelopOptions options;
        options.lens_database = db;
        auto corrected = render_stage_two(*one, DevelopSettings{}, options);
        DevelopSettings off;
        off.lens_correction.distortion = off.lens_correction.lateral_chromatic_aberration =
            off.lens_correction.vignetting = false;
        auto plain = render_stage_two(*one, off, options);
        ASSERT_TRUE(corrected && plain);
        const LensCorrectionRecord& r = corrected->report.lens_correction;
        EXPECT_EQ(applied_letters(r.plan), std::string(e.applied));
        EXPECT_EQ(r.outside_samples, 0u) << "the corrected frame reads beyond the image";
        EXPECT_GT(ts::max_abs_difference(corrected->display.pixels, plain->display.pixels), 1e-3);
        std::cout << "[" << e.id << "] " << (r.plan.profile ? r.plan.profile->model : "") << ": scale "
                  << (r.plan.geometry ? r.plan.geometry->scale : 1.0) << ", corner vignetting gain "
                  << (r.plan.vignetting ? r.plan.vignetting->gain_at(image::PointD{0.5, 0.5}) : 1.0) << "\n";
    }
    if (ran == 0) {
        GTEST_SKIP() << "no fixture with an exact match is downloaded";
    }
}

// ---- Renders for inspection ------------------------------------------------------------------------

std::optional<std::string> environment(const char* name) {
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string text(value);
    std::free(value);
    if (text.empty()) {
        return std::nullopt;
    }
    return text;
}

// float32 (H, W, C) NPY, version 1.0, of the rectangle `r` of `img`.
void write_npy(const std::filesystem::path& path, const image::ImageBuffer<float>& img, const image::PixelRect& r) {
    std::string header = "{'descr': '<f4', 'fortran_order': False, 'shape': (" + std::to_string(r.height) + ", " +
                         std::to_string(r.width) + ", " + std::to_string(img.channels()) + "), }";
    const std::size_t unpadded = 10 + header.size() + 1;
    header.append((64 - unpadded % 64) % 64, ' ');
    header.push_back('\n');
    std::ofstream out(path, std::ios::binary);
    out.write("\x93NUMPY\x01\x00", 8);
    const auto length = static_cast<std::uint16_t>(header.size());
    out.put(static_cast<char>(length & 0xFF));
    out.put(static_cast<char>(length >> 8));
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    for (std::int32_t y = r.y; y < r.y + r.height; ++y) {
        out.write(reinterpret_cast<const char*>(img.row(y) + static_cast<std::ptrdiff_t>(r.x) * img.channels()),
                  static_cast<std::streamsize>(sizeof(float) * static_cast<std::size_t>(r.width) *
                                               static_cast<std::size_t>(img.channels())));
    }
}

// With LUCENT_LENS_DUMP_DIR set, writes for each exact match of the corpus (only the fixtures whose
// id contains one of the comma-separated LUCENT_LENS_DUMP_IDS, when that is set) the display-linear
// Rec.2020 renders of the automatic corrections and of none: the 1024-pixel proxy whole, and
// full-resolution 768-pixel crops of the four corners and the centre (float32 NPY; also of the
// automatic corrections without lateral CA, which isolates its effect), with the plan in a text
// file. For visual inspection (W13); run with --gtest_also_run_disabled_tests.
TEST(LensFixtures, DISABLED_DumpRendersForInspection) {
    const auto dir_text = environment("LUCENT_LENS_DUMP_DIR");
    if (!dir_text) {
        GTEST_SKIP() << "LUCENT_LENS_DUMP_DIR is not set";
    }
    const std::filesystem::path dir(*dir_text);
    std::filesystem::create_directories(dir);
    std::vector<std::string> wanted;
    if (const auto ids = environment("LUCENT_LENS_DUMP_IDS")) {
        std::stringstream list(*ids);
        for (std::string id; std::getline(list, id, ',');) {
            if (!id.empty()) {
                wanted.push_back(id);
            }
        }
    }
    auto db = ts::bundled_lens_database();
    ASSERT_TRUE(db);
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    DevelopOptions options;
    options.lens_database = db;
    DevelopSettings off;
    off.lens_correction.distortion = off.lens_correction.lateral_chromatic_aberration =
        off.lens_correction.vignetting = false;
    for (const CorpusCase& e : corpus_expectations()) {
        const std::string id = e.id;
        if (std::string(e.applied).empty() ||
            (!wanted.empty() && std::none_of(wanted.begin(), wanted.end(), [&](const std::string& w) {
                 return id.find(w) != std::string::npos;
             }))) {
            continue;
        }
        const auto file = ts::fixture_file(id);
        if (!file) {
            continue;
        }
        SCOPED_TRACE(id);
        {
            StageOneRequest request;
            request.proxy_long_edge = 1024;
            auto one = decodable_stage_one(*registry, *file, request, id);
            if (!one) {
                continue;
            }
            auto on = render_stage_two(*one, DevelopSettings{}, options);
            auto plain = render_stage_two(*one, off, options);
            ASSERT_TRUE(on && plain);
            const image::PixelRect whole = image::PixelRect::from_size(on->display.pixels.size());
            write_npy(dir / (id + "-proxy-on.npy"), on->display.pixels, whole);
            write_npy(dir / (id + "-proxy-off.npy"), plain->display.pixels, whole);
            std::ofstream text(dir / (id + "-plan.txt"));
            const LensCorrectionPlan& p = on->report.lens_correction.plan;
            text << "match " << to_string(p.match.kind) << "\n";
            for (const std::string& reason : p.match.reasons) {
                text << "  " << reason << "\n";
            }
            for (const LensCorrectionDecision& d : p.decisions) {
                text << to_string(d.kind) << ": " << to_string(d.outcome) << ": " << d.detail << "\n";
            }
            text << "output " << on->display.pixels.width() << " x " << on->display.pixels.height() << "\n";
        }
        StageOneRequest full;
        auto one = render_stage_one_file(*registry, *file, 0u, full);
        ASSERT_TRUE(one) << one.error().describe();
        image::Size size;
        // "on": the automatic corrections; "nt": the same without lateral CA (the TCA's own effect is
        // the difference); "off": none.
        DevelopSettings no_tca;
        no_tca.lens_correction.lateral_chromatic_aberration = false;
        const std::vector<std::pair<std::string, DevelopSettings>> variants = {
            {"on", DevelopSettings{}}, {"nt", no_tca}, {"off", off}};
        for (const auto& [variant, variant_settings] : variants) {
            auto r = render_stage_two(*one, variant_settings, options);
            ASSERT_TRUE(r) << r.error().describe();
            // The output (the camera's image area as shot): the same size with and without the lens.
            size = r->display.pixels.size();
            constexpr std::int32_t kCrop = 768;
            const std::int32_t cw = std::min(kCrop, size.width);
            const std::int32_t ch = std::min(kCrop, size.height);
            const std::vector<std::pair<std::string, image::PixelRect>> crops = {
                {"tl", image::PixelRect{0, 0, cw, ch}},
                {"tr", image::PixelRect{size.width - cw, 0, cw, ch}},
                {"bl", image::PixelRect{0, size.height - ch, cw, ch}},
                {"br", image::PixelRect{size.width - cw, size.height - ch, cw, ch}},
                {"c", image::PixelRect{(size.width - cw) / 2, (size.height - ch) / 2, cw, ch}},
            };
            for (const auto& [name, rect] : crops) {
                write_npy(dir / (id + "-full-" + name + "-" + variant + ".npy"), r->display.pixels, rect);
            }
        }
        std::cout << "[dump] " << id << ": " << size.width << " x " << size.height << " written to " << dir.string()
                  << "\n";
    }
}

// The cost of the correction on one file: the export render (full resolution) and a 2048-pixel
// proxy's stage two, with `corrected` and with every correction off; prints the times.
void benchmark_correction(const char* label, const std::filesystem::path& file, const DevelopSettings& corrected,
                          const std::string& expected_applied) {
    auto db = ts::bundled_lens_database();
    ASSERT_TRUE(db);
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    DevelopOptions options;
    options.lens_database = db;
    DevelopSettings plain;
    plain.lens_correction.distortion = plain.lens_correction.lateral_chromatic_aberration =
        plain.lens_correction.vignetting = false;
    for (int run = 0; run < 2; ++run) {
        for (const bool lens : {false, true}) {
            const auto t = Clock::now();
            auto r = render_develop_file(*registry, file, 0u, lens ? corrected : plain, options);
            ASSERT_TRUE(r) << r.error().describe();
            const double total = seconds_since(t);
            double first_pass = 0.0;
            for (const OperationRecord& o : r->report.operations) {
                if (o.operation == DevelopOperation::WhiteBalance) {
                    first_pass = o.seconds;
                }
            }
            if (lens) {
                EXPECT_EQ(applied_letters(r->report.lens_correction.plan), expected_applied);
            }
            std::cout << "[" << label << " export " << r->report.upright_size.width << "x"
                      << r->report.upright_size.height << "] lens " << (lens ? "on " : "off") << ": total "
                      << total << " s, stage two " << r->report.stage_two_seconds
                      << " s, first pass (with the correction) " << first_pass << " s, plan "
                      << r->report.lens_correction.seconds << " s, applied "
                      << applied_letters(r->report.lens_correction.plan) << "\n";
        }
    }
    StageOneRequest request;
    request.proxy_long_edge = 2048;
    auto one = render_stage_one_file(*registry, file, 0u, request);
    ASSERT_TRUE(one) << one.error().describe();
    for (const bool lens : {false, true}) {
        std::vector<double> times;
        std::vector<double> first_pass;
        for (int i = 0; i < 7; ++i) {
            const auto t = Clock::now();
            auto r = render_stage_two(*one, lens ? corrected : plain, options);
            ASSERT_TRUE(r);
            times.push_back(seconds_since(t));
            for (const OperationRecord& o : r->report.operations) {
                if (o.operation == DevelopOperation::WhiteBalance) {
                    first_pass.push_back(o.seconds);
                }
            }
        }
        std::sort(times.begin(), times.end());
        std::sort(first_pass.begin(), first_pass.end());
        std::cout << "[" << label << " proxy 2048] lens " << (lens ? "on " : "off") << ": stage two median "
                  << times[3] * 1000.0 << " ms (min " << times.front() * 1000.0 << "), first pass median "
                  << (first_pass.size() == times.size() ? first_pass[3] * 1000.0 : 0.0) << " ms\n";
    }
}

// Cost on the 45 MP Canon EOS R5 file with its own lens chosen (the Sigma 50 mm f/1.4 DG HSM Art is
// a fuzzy match, so the photographer names it), on the 51 MP Fujifilm GFX 50S II file (an exact
// match with all three corrections; the 45.7 MP Nikon Z 8 files match exactly too but this build
// cannot decode them) and on the 61 MP Sony ILCE-7RM5 file (an exact match, the corpus's largest):
// the export render and a proxy's stage two, with and without the correction. Run with
// --gtest_also_run_disabled_tests.
TEST(LensFixtures, DISABLED_BenchmarkTheCorrectionOnTheR5) {
    LUCENT_ENGINE_REQUIRE_FIXTURE(file, "canon-eos-r5-cr3-raw");
    auto db = ts::bundled_lens_database();
    ASSERT_TRUE(db);
    DevelopSettings corrected;
    corrected.lens_correction.profile = LensProfileId{"Sigma", "Sigma 50mm f/1.4 DG HSM [A]", 1.0};
    ASSERT_TRUE(db->contains(*corrected.lens_correction.profile));
    benchmark_correction("R5, named profile", file, corrected, "dtv");
    for (const char* id : {"fujifilm-gfx-50s-ii-raf-compressed", "sony-ilce-7rm5-arw-compressed"}) {
        const auto exact = ts::fixture_file(id);
        if (!exact) {
            continue;
        }
        const auto expected = std::find_if(corpus_expectations().begin(), corpus_expectations().end(),
                                           [&](const CorpusCase& e) { return std::string(e.id) == id; });
        ASSERT_NE(expected, corpus_expectations().end());
        benchmark_correction(id, *exact, DevelopSettings{}, expected->applied);
    }
    // The database itself: reading and parsing the 56 files.
    const ts::LoadedLensDatabase loaded = ts::load_bundled_lens_database();
    ASSERT_TRUE(loaded.database);
    std::cout << "[lens database] " << loaded.database->info().files << " files, "
              << loaded.database->info().cameras << " cameras, " << loaded.database->info().lenses
              << " lenses, " << loaded.database->info().mounts << " mounts, loaded in " << loaded.seconds
              << " s\n";
    // A plan on the freshly loaded database (the match, Lensfun's interpolation and autoscale, the
    // fold-back and filling-scale checks), then the same plan from its cache.
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    for (const char* id :
         {"canon-eos-r5-cr3-raw", "fujifilm-gfx-50s-ii-raf-compressed", "sony-ilce-7rm5-arw-compressed"}) {
        const auto exact = ts::fixture_file(id);
        if (!exact) {
            continue;
        }
        auto p = probe(*registry, *exact);
        ASSERT_TRUE(p);
        LensCorrectionSetting setting;
        if (std::string(id) == "canon-eos-r5-cr3-raw") {
            setting.profile = LensProfileId{"Sigma", "Sigma 50mm f/1.4 DG HSM [A]", 1.0};
        }
        auto t = Clock::now();
        auto cold = loaded.database->plan(p->metadata, setting, p->upright, p->metadata.geometry.orientation, false);
        const double cold_seconds = seconds_since(t);
        t = Clock::now();
        auto warm = loaded.database->plan(p->metadata, setting, p->upright, p->metadata.geometry.orientation, false);
        const double warm_seconds = seconds_since(t);
        ASSERT_TRUE(cold && warm);
        EXPECT_EQ(*cold, *warm);
        std::cout << "[plan] " << id << ": " << cold_seconds * 1000.0 << " ms, cached " << warm_seconds * 1e6
                  << " us, applied " << applied_letters(*cold) << "\n";
    }
}

}  // namespace
}  // namespace lucent::engine
