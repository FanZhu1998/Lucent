#include "qualification.hpp"

// clang-format off: psapi.h needs windows.h first.
#include <windows.h>
#include <psapi.h>
// clang-format on

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/text.hpp>
#include <lucent/core/version.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/export/export.hpp>
#include <lucent/export/metadata.hpp>
#include <lucent/export/output_profile.hpp>
#include <lucent/image/color_math.hpp>
#include <lucent/raw/capture_metadata.hpp>
#include <lucent/raw/decoder_registry.hpp>
#include <lucent/raw/dng_tags.hpp>
#include <lucent/raw/file_source.hpp>
#include <lucent/raw/libraw_decoder.hpp>
#include <lucent/raw/tiff_inspector.hpp>

#include "color_difference.hpp"
#include "export_metadata.hpp"
#include "probe_report.hpp"
#include "readback.hpp"

namespace lucent::qualify {

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string describe(const Error& error) {
    return std::string(lucent::to_string(error.code)) + " at " + error.where + ": " + error.message;
}

std::string size_text(std::int32_t w, std::int32_t h) {
    return std::to_string(w) + "x" + std::to_string(h);
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (const std::string& p : parts) {
        if (!out.empty()) {
            out += separator;
        }
        out += p;
    }
    return out;
}

// The result's check with `id`, appended in check_ids() order when first used.
CheckResult& check_of(FixtureResult& result, std::string_view id) {
    for (CheckResult& c : result.checks) {
        if (c.id == id) {
            return c;
        }
    }
    result.checks.push_back(CheckResult{std::string(id)});
    return result.checks.back();
}

void set_check(CheckResult& c, CheckStatus status, Evidence evidence, std::string summary) {
    c.status = status;
    c.evidence = evidence;
    c.summary = std::move(summary);
}

void add_limitation(FixtureResult& result, std::string_view id, std::string detail) {
    for (Limitation& l : result.limitations) {
        if (l.id == id) {
            if (!detail.empty() && l.detail.find(detail) == std::string::npos) {
                l.detail += "; " + detail;
            }
            return;
        }
    }
    result.limitations.push_back(Limitation{std::string(id), std::move(detail)});
}

// ---- Parity (the rules of tests/integration/raw_fixture_tests.cpp and
//      research/tools/compare_probe.py: integers, strings and patterns exactly, floats with
//      |a - b| <= max(1e-6 * max(|a|, |b|), 1e-9)) ------------------------------------------

constexpr double kParityRelTol = 1e-6;
constexpr double kParityAbsTol = 1e-9;

void compare_json(const Json& a, const Json& b, const std::string& path, std::vector<std::string>& problems) {
    if (a.is_null() || b.is_null()) {
        if (!(a.is_null() && b.is_null())) {
            problems.push_back(path + ": " + a.dump() + " vs " + b.dump());
        }
        return;
    }
    if (a.is_array() && b.is_array()) {
        if (a.size() != b.size()) {
            problems.push_back(path + ": length " + std::to_string(a.size()) + " vs " +
                               std::to_string(b.size()));
            return;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            compare_json(a[i], b[i], path + "[" + std::to_string(i) + "]", problems);
        }
        return;
    }
    if (a.is_number() && b.is_number()) {
        if (a.is_number_integer() && b.is_number_integer()) {
            if (a.get<std::int64_t>() != b.get<std::int64_t>()) {
                problems.push_back(path + ": " + a.dump() + " vs " + b.dump());
            }
            return;
        }
        const double x = a.get<double>();
        const double y = b.get<double>();
        if (std::abs(x - y) > std::max(kParityRelTol * std::max(std::abs(x), std::abs(y)), kParityAbsTol)) {
            problems.push_back(path + ": " + a.dump() + " vs " + b.dump());
        }
        return;
    }
    if (a.is_string() && b.is_string()) {
        if (a != b) {
            problems.push_back(path + ": " + a.dump() + " vs " + b.dump());
        }
        return;
    }
    problems.push_back(path + ": type mismatch " + a.dump() + " vs " + b.dump());
}

std::vector<std::string> compare_parity(const Json& lucent, const Json& reference) {
    std::vector<std::string> problems;
    std::set<std::string> fields;
    for (const auto& [key, value] : lucent.items()) {
        fields.insert(key);
    }
    for (const auto& [key, value] : reference.items()) {
        fields.insert(key);
    }
    for (const std::string& field : fields) {
        if (!lucent.contains(field) || !reference.contains(field)) {
            problems.push_back(field + ": missing from " +
                               (lucent.contains(field) ? "the reference" : "Lucent"));
            continue;
        }
        compare_json(lucent[field], reference[field], field, problems);
    }
    return problems;
}

// ---- Colour gates (ADR 0006; tests/integration/render_fixture_tests.cpp) ---------------------

constexpr double kGateRegionDeltaEMean = 2.0;
constexpr double kGateRegionDeltaEMax = 6.0;
constexpr double kGateLuminanceRatioLow = 0.93;
constexpr double kGateLuminanceRatioHigh = 1.07;
constexpr double kRegressionFactor = 1.25;
constexpr double kRegressionMeanFloor = 0.02;
constexpr double kRegressionMaxFloor = 0.05;
constexpr double kRegressionRatioTolerance = 0.003;

using integration::Rgb;

// Region means of the scene-linear image in linear sRGB, each pixel clipped to [0, 1], on the
// grid of research/tools/render_reference.py (row i covers [floor(i H / G), floor((i + 1) H / G))).
std::vector<Rgb> region_means(const engine::ColorImage& scene, std::int32_t grid, const image::Matrix3d& m) {
    std::array<float, 9> f{};
    for (std::size_t i = 0; i < 9; ++i) {
        f[i] = static_cast<float>(m(i / 3, i % 3));
    }
    const std::int32_t w = scene.pixels.width();
    const std::int32_t h = scene.pixels.height();
    const auto band = [grid](std::int32_t size) {
        std::vector<std::int32_t> index(static_cast<std::size_t>(size));
        for (std::int32_t cell = 0; cell < grid; ++cell) {
            const auto first = static_cast<std::int32_t>(static_cast<std::int64_t>(cell) * size / grid);
            const auto last = static_cast<std::int32_t>(static_cast<std::int64_t>(cell + 1) * size / grid);
            for (std::int32_t k = first; k < last; ++k) {
                index[static_cast<std::size_t>(k)] = cell;
            }
        }
        return index;
    };
    const std::vector<std::int32_t> row_cell = band(h);
    const std::vector<std::int32_t> col_cell = band(w);
    std::vector<Rgb> sums(static_cast<std::size_t>(grid) * static_cast<std::size_t>(grid), Rgb{0, 0, 0});
    std::vector<std::uint64_t> counts(sums.size(), 0);
    const auto clip = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
    for (std::int32_t y = 0; y < h; ++y) {
        const float* p = scene.pixels.row(y);
        const std::int32_t rc = row_cell[static_cast<std::size_t>(y)] * grid;
        for (std::int32_t x = 0; x < w; ++x, p += 3) {
            const auto cell = static_cast<std::size_t>(rc + col_cell[static_cast<std::size_t>(x)]);
            sums[cell][0] += clip(f[0] * p[0] + f[1] * p[1] + f[2] * p[2]);
            sums[cell][1] += clip(f[3] * p[0] + f[4] * p[1] + f[5] * p[2]);
            sums[cell][2] += clip(f[6] * p[0] + f[7] * p[1] + f[8] * p[2]);
            ++counts[cell];
        }
    }
    for (std::size_t i = 0; i < sums.size(); ++i) {
        for (double& v : sums[i]) {
            v /= static_cast<double>(std::max<std::uint64_t>(counts[i], 1));
        }
    }
    return sums;
}

struct GateStats {
    double mean_de = 0.0;
    double max_de = 0.0;
    double ratio = 0.0;
};

GateStats compare_regions(const std::vector<Rgb>& ours, const Json& theirs) {
    GateStats s;
    double sum_de = 0.0;
    double y_ours = 0.0;
    double y_theirs = 0.0;
    for (std::size_t i = 0; i < ours.size(); ++i) {
        const Rgb reference{theirs[i][0].get<double>(), theirs[i][1].get<double>(),
                            theirs[i][2].get<double>()};
        const double de = integration::ciede2000(integration::lab_from_linear_srgb(ours[i]),
                                                 integration::lab_from_linear_srgb(reference));
        sum_de += de;
        s.max_de = std::max(s.max_de, de);
        y_ours += integration::luminance_from_linear_srgb(ours[i]);
        y_theirs += integration::luminance_from_linear_srgb(reference);
    }
    s.mean_de = sum_de / static_cast<double>(std::max<std::size_t>(ours.size(), 1));
    s.ratio = y_theirs > 0.0 ? y_ours / y_theirs : 0.0;
    return s;
}

// ---- Pixel statistics ---------------------------------------------------------------------------

struct NumericStats {
    std::uint64_t non_finite = 0;
    std::uint64_t above_one = 0;  // samples > 1 (display images only)
    float max_value = 0.0f;
};

NumericStats numeric_stats(const engine::ColorImage& image, bool display) {
    NumericStats s;
    for (std::int32_t y = 0; y < image.pixels.height(); ++y) {
        const float* p = image.pixels.row(y);
        const std::int32_t n = image.pixels.width() * image.pixels.channels();
        for (std::int32_t i = 0; i < n; ++i) {
            const float v = p[i];
            if (!std::isfinite(v)) {
                ++s.non_finite;
                continue;
            }
            s.max_value = std::max(s.max_value, v);
            if (display && v > 1.0f) {
                ++s.above_one;
            }
        }
    }
    return s;
}

// SHA-256 of the image's samples, row by row without padding.
std::string pixel_hash(const engine::ColorImage& image) {
    core::Sha256 hasher;
    const std::size_t row_bytes = static_cast<std::size_t>(image.pixels.width()) *
                                  static_cast<std::size_t>(image.pixels.channels()) * sizeof(float);
    for (std::int32_t y = 0; y < image.pixels.height(); ++y) {
        hasher.update(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(image.pixels.row(y)), row_bytes));
    }
    auto digest = hasher.finish();
    return digest ? core::to_hex(*digest) : std::string("hash-failed");
}

// Mean display luminance of the outer 4-pixel band of each side against the next 16 pixels.
struct EdgeBand {
    const char* side;
    double outer = 0.0;
    double inner = 0.0;
};

std::vector<EdgeBand> edge_bands(const engine::ColorImage& image) {
    constexpr std::int32_t kOuter = 4;
    constexpr std::int32_t kInner = 16;
    const std::int32_t w = image.pixels.width();
    const std::int32_t h = image.pixels.height();
    std::vector<EdgeBand> bands;
    if (w < 2 * (kOuter + kInner) || h < 2 * (kOuter + kInner)) {
        return bands;
    }
    const auto luminance = [&](std::int32_t x, std::int32_t y) {
        const float* p = image.pixels.row(y) + static_cast<std::ptrdiff_t>(x) * 3;
        return 0.2627 * p[0] + 0.6780 * p[1] + 0.0593 * p[2];
    };
    const auto mean_rows = [&](std::int32_t y0, std::int32_t y1) {
        double sum = 0.0;
        for (std::int32_t y = y0; y < y1; ++y) {
            for (std::int32_t x = 0; x < w; ++x) {
                sum += luminance(x, y);
            }
        }
        return sum / (static_cast<double>(y1 - y0) * w);
    };
    const auto mean_cols = [&](std::int32_t x0, std::int32_t x1) {
        double sum = 0.0;
        for (std::int32_t y = 0; y < h; ++y) {
            for (std::int32_t x = x0; x < x1; ++x) {
                sum += luminance(x, y);
            }
        }
        return sum / (static_cast<double>(x1 - x0) * h);
    };
    bands.push_back({"top", mean_rows(0, kOuter), mean_rows(kOuter, kOuter + kInner)});
    bands.push_back({"bottom", mean_rows(h - kOuter, h), mean_rows(h - kOuter - kInner, h - kOuter)});
    bands.push_back({"left", mean_cols(0, kOuter), mean_cols(kOuter, kOuter + kInner)});
    bands.push_back({"right", mean_cols(w - kOuter, w), mean_cols(w - kOuter - kInner, w - kOuter)});
    return bands;
}

// ---- Raw-data checks ----------------------------------------------------------------------------

struct RawLevelStats {
    std::uint64_t samples = 0;
    std::uint64_t above_white = 0;
    std::uint64_t far_below_black = 0;  // more than 2 % of the range below the site's black level
    double max_sample = 0.0;
    std::uint64_t at_max = 0;
    double max_black = 0.0;
    // Mosaic: mean (black-subtracted) of each site of the 2 x 2 phase, unsaturated samples only.
    std::array<double, 4> phase_mean{};
    std::array<std::uint64_t, 4> phase_count{};
};

RawLevelStats raw_level_stats(const raw::DecodedRaw& decoded) {
    RawLevelStats s;
    const raw::RawMetadata& m = decoded.metadata;
    const raw::SensorLevels& levels = m.levels;
    s.max_black = levels.max_black_level();
    const auto* buffer = std::get_if<image::ImageBuffer<std::uint16_t>>(&raw::pixels_of(decoded.image));
    if (buffer == nullptr) {
        return s;
    }
    const image::PixelRect area = m.geometry.visible_area;
    const std::int32_t channels = buffer->channels();
    const double range = levels.white - s.max_black;
    const bool mosaic = m.representation.kind == raw::RawRepresentation::Mosaic;
    const image::CfaPattern& cfa = m.representation.cfa;
    std::array<double, 4> phase_sum{};
    for (std::int32_t vy = 0; vy < area.height; ++vy) {
        const std::uint16_t* row = buffer->row(area.y + vy);
        for (std::int32_t vx = 0; vx < area.width; ++vx) {
            for (std::int32_t c = 0; c < channels; ++c) {
                const double v = row[static_cast<std::ptrdiff_t>(area.x + vx) * channels + c];
                std::size_t channel = static_cast<std::size_t>(c);
                if (mosaic) {
                    const image::CfaColor color = cfa.color_at(area.y + vy, area.x + vx);
                    const auto& colors = m.representation.channel_colors;
                    const auto it = std::find(colors.begin(), colors.end(), color);
                    channel = it == colors.end() ? 0 : static_cast<std::size_t>(it - colors.begin());
                }
                const double black = levels.black_level_at(std::min<std::size_t>(channel, 3), vx, vy);
                ++s.samples;
                if (v > levels.white) {
                    ++s.above_white;
                }
                if (v < black - 0.02 * range) {
                    ++s.far_below_black;
                }
                if (v > s.max_sample) {
                    s.max_sample = v;
                    s.at_max = 1;
                } else if (v == s.max_sample) {
                    ++s.at_max;
                }
                if (mosaic && v < levels.white) {
                    const std::size_t phase = static_cast<std::size_t>((vy & 1) * 2 + (vx & 1));
                    phase_sum[phase] += v - black;
                    ++s.phase_count[phase];
                }
            }
        }
    }
    for (std::size_t i = 0; i < 4; ++i) {
        s.phase_mean[i] = s.phase_count[i] > 0 ? phase_sum[i] / static_cast<double>(s.phase_count[i]) : 0.0;
    }
    return s;
}

std::string representation_text(const raw::RawMetadata& m) {
    switch (m.representation.kind) {
        case raw::RawRepresentation::Mosaic: {
            const image::CfaPattern cfa = raw::cfa_at_visible_origin(m);
            return "mosaic:" + (cfa.empty() ? std::string("unknown") : cfa.to_string());
        }
        case raw::RawRepresentation::LinearColor:
            return "linear-rgb";
        case raw::RawRepresentation::Monochrome:
            return "monochrome";
    }
    return "unknown";
}

bool dng_has_enhanced_image(const std::filesystem::path& path, std::string& where) {
    auto file = core::ReadOnlyFile::open(path);
    if (!file) {
        return false;
    }
    auto source = raw::FileSource::create(*file);
    if (!source) {
        return false;
    }
    auto summary = raw::inspect_tiff(*source);
    if (!summary || !summary->is_dng()) {
        return false;
    }
    for (const raw::TiffIfdSummary& ifd : summary->ifds) {
        if (ifd.new_subfile_type && *ifd.new_subfile_type == raw::dng_subfile_type::kEnhancedImage) {
            where = ifd.path;
            return true;
        }
    }
    return false;
}

std::string opcode_step_id(raw::DngOpcodeListId list, std::uint32_t index, std::string_view name) {
    return "dng_opcode:" + std::to_string(static_cast<int>(list)) + ":" + std::to_string(index) + ":" +
           std::string(name);
}

// ---- Export round trip --------------------------------------------------------------------------

// JPEG q95 (4:4:4, islow DCT) against the quantised image: the mean absolute code difference of
// the develop renders of the corpus is 0.9 to 2.2 (the base look's contrast keeps more noise than
// the W2 path's Neutral v0, for which the render tests use 1.5), single samples up to about 35. A
// wrong row, a swapped channel or a mis-quantised image differs by tens of codes on average.
constexpr double kJpegMeanCodeDifference = 3.0;
constexpr int kJpegMaxCodeDifference = 80;

struct ExportOutcome {
    bool ok = false;
    std::string problem;
    std::string sha256;
    OrderedJson details = OrderedJson::object();
};

ExportOutcome export_and_read_back(const engine::ColorImage& display, const std::filesystem::path& path,
                                   const exporter::ExportSettings& settings,
                                   const exporter::ExportMetadata& metadata,
                                   const raw::CaptureMetadata& capture) {
    ExportOutcome out;
    const Clock::time_point started = Clock::now();
    auto encoded =
        exporter::encode_for_output(display.pixels.const_view(), display.encoding, settings.profile,
                                    exporter::bit_depth_for(settings.format), settings.transform);
    if (!encoded) {
        out.problem = "encode: " + describe(encoded.error());
        return out;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto receipt = exporter::write_verified(*encoded, path, settings, metadata);
    if (!receipt) {
        out.problem = "write_verified: " + describe(receipt.error());
        return out;
    }
    out.sha256 = core::to_hex(receipt->sha256);
    out.details["file"] = core::path_to_utf8_lossy(path.filename());
    out.details["bytes"] = receipt->byte_size;
    out.details["sha256"] = out.sha256;
    out.details["encoder"] = receipt->encoder;

    ReadbackHeader header;
    std::string error;
    const std::size_t row_samples = encoded->row_samples();
    bool decoded = false;
    if (settings.format == exporter::ExportFormat::Tiff) {
        std::int64_t mismatched_rows = 0;
        decoded = read_tiff16(path, header, error, [&](std::int32_t y, std::span<const std::uint16_t> row) {
            const std::uint16_t* expected =
                encoded->samples16.data() + row_samples * static_cast<std::size_t>(y);
            if (row.size() != row_samples || !std::equal(row.begin(), row.end(), expected)) {
                ++mismatched_rows;
            }
        });
        out.details["mismatched_rows"] = mismatched_rows;
        if (decoded && mismatched_rows != 0) {
            out.problem = std::to_string(mismatched_rows) + " TIFF rows differ from the quantised image";
        }
    } else {
        double sum = 0.0;
        int worst = 0;
        std::uint64_t samples = 0;
        decoded = read_jpeg(path, header, error, [&](std::int32_t y, std::span<const std::uint8_t> row) {
            const std::uint8_t* expected =
                encoded->samples8.data() + row_samples * static_cast<std::size_t>(y);
            for (std::size_t i = 0; i < row.size() && i < row_samples; ++i) {
                const int diff = std::abs(int{row[i]} - int{expected[i]});
                sum += diff;
                worst = std::max(worst, diff);
            }
            samples += row.size();
        });
        const double mean = sum / static_cast<double>(std::max<std::uint64_t>(samples, 1));
        out.details["mean_code_difference"] = mean;
        out.details["max_code_difference"] = worst;
        if (decoded && (samples != encoded->samples8.size() || mean >= kJpegMeanCodeDifference ||
                        worst > kJpegMaxCodeDifference)) {
            out.problem = "JPEG differs from the quantised image beyond quantisation (mean " +
                          std::to_string(mean) + ", max " + std::to_string(worst) + ")";
        }
    }
    if (!decoded) {
        out.problem = "independent decode failed: " + error;
        return out;
    }
    if (header.width != display.pixels.width() || header.height != display.pixels.height() ||
        header.channels != 3) {
        out.problem = "decoded size " + size_text(header.width, header.height) + " differs from the image";
    }
    auto icc = exporter::icc_profile(settings.profile);
    const bool icc_ok =
        icc && header.icc.size() == icc->size() && std::equal(icc->begin(), icc->end(), header.icc.begin());
    out.details["icc_bytes"] = header.icc.size();
    out.details["icc_matches_profile"] = icc_ok;
    if (!icc_ok && out.problem.empty()) {
        out.problem = "embedded ICC profile is not exactly the " +
                      std::string(exporter::to_string(settings.profile)) + " profile";
    }
    auto read = exporter::read_export_metadata(path);
    if (!read) {
        if (out.problem.empty()) {
            out.problem = "metadata read-back: " + describe(read.error());
        }
    } else {
        const bool same = *read == receipt->metadata;
        out.details["metadata_matches_receipt"] = same;
        out.details["camera_make"] = read->camera_make;
        out.details["camera_model"] = read->camera_model;
        const bool camera_kept = capture.model.empty() || !read->camera_model.empty();
        if (out.problem.empty() && !same) {
            out.problem = "metadata read back differs from the receipt";
        } else if (out.problem.empty() && !camera_kept) {
            out.problem = "the camera model was not carried into the export";
        }
    }
    out.details["seconds"] = seconds_since(started);
    out.ok = out.problem.empty();
    return out;
}

}  // namespace

// ---- Names ----------------------------------------------------------------------------------------

std::string_view to_string(CheckStatus status) noexcept {
    switch (status) {
        case CheckStatus::Pass:
            return "pass";
        case CheckStatus::Fail:
            return "fail";
        case CheckStatus::NotApplicable:
            return "not_applicable";
        case CheckStatus::NotRun:
            return "not_run";
    }
    return "unknown";
}

std::string_view to_string(Evidence evidence) noexcept {
    switch (evidence) {
        case Evidence::Strong:
            return "strong";
        case Evidence::Weak:
            return "weak";
        case Evidence::None:
            return "none";
    }
    return "unknown";
}

std::string_view to_string(SupportState state) noexcept {
    switch (state) {
        case SupportState::Verified:
            return "verified";
        case SupportState::CompatibleUnverified:
            return "compatible_unverified";
        case SupportState::Limited:
            return "limited";
        case SupportState::Unsupported:
            return "unsupported";
        case SupportState::NotQualified:
            return "not_qualified";
    }
    return "unknown";
}

std::string_view display_name(SupportState state) noexcept {
    switch (state) {
        case SupportState::Verified:
            return "Verified";
        case SupportState::CompatibleUnverified:
            return "Compatible, unverified";
        case SupportState::Limited:
            return "Limited";
        case SupportState::Unsupported:
            return "Unsupported";
        case SupportState::NotQualified:
            return "Not qualified";
    }
    return "Unknown";
}

// ---- Corpus ---------------------------------------------------------------------------------------

std::filesystem::path CorpusPaths::manifest() const {
    return root / "fixtures" / "manifest.json";
}
std::filesystem::path CorpusPaths::cache_dir() const {
    return root / "fixtures" / "cache";
}
std::filesystem::path CorpusPaths::reference_dir() const {
    return root / "fixtures" / "reference";
}
std::filesystem::path CorpusPaths::expectations() const {
    return root / "fixtures" / "qualification" / "expectations.json";
}
std::filesystem::path CorpusPaths::families() const {
    return root / "fixtures" / "qualification" / "families.json";
}
std::filesystem::path CorpusPaths::render_reference() const {
    return root / "tests" / "integration" / "data" / "render_reference.json";
}
std::filesystem::path CorpusPaths::cached_file(const ManifestFixture& fixture) const {
    return cache_dir() / fixture.id / std::filesystem::path(core::utf8_to_wide_lossy(fixture.file_name));
}

Expected<std::vector<ManifestFixture>> load_manifest(const std::filesystem::path& manifest_path) {
    auto document = read_json_file(manifest_path);
    if (!document) {
        return Unexpected(std::move(document.error()));
    }
    try {
        const Json& d = *document;
        if (d.value("schema", std::string()) != "lucent.fixture-manifest") {
            return fail(ErrorCode::InvalidArgument, "not a lucent.fixture-manifest document",
                        "qualify.manifest");
        }
        if (d.at("schema_version").get<int>() > 1) {
            return fail(ErrorCode::VersionMismatch, "newer fixture manifest schema", "qualify.manifest");
        }
        std::vector<ManifestFixture> out;
        for (const Json& f : d.at("fixtures")) {
            ManifestFixture m;
            m.id = f.at("id").get<std::string>();
            m.tier = f.at("tier").get<std::string>();
            m.make = f.at("make").get<std::string>();
            m.model = f.at("model").get<std::string>();
            m.format = f.at("format").get<std::string>();
            m.mode = f.value("mode", std::string());
            m.file_name = f.at("file_name").get<std::string>();
            m.size_bytes = f.at("size_bytes").get<std::uint64_t>();
            m.sha256 = f.at("sha256").get<std::string>();
            out.push_back(std::move(m));
        }
        return out;
    } catch (const nlohmann::json::exception& error) {
        return fail(ErrorCode::InvalidArgument, std::string("malformed manifest: ") + error.what(),
                    "qualify.manifest");
    }
}

Expected<QualifyCorpus> QualifyCorpus::load(const CorpusPaths& paths, bool require_expectations) {
    QualifyCorpus corpus;
    corpus.paths = paths;
    auto manifest = load_manifest(paths.manifest());
    if (!manifest) {
        return Unexpected(std::move(manifest.error()));
    }
    corpus.manifest = std::move(*manifest);
    if (std::filesystem::exists(paths.expectations()) || require_expectations) {
        auto expectations = load_expectations(paths.expectations());
        if (!expectations) {
            return Unexpected(std::move(expectations.error()));
        }
        corpus.expectations = std::move(*expectations);
    }
    auto reference = read_json_file(paths.render_reference());
    if (!reference) {
        return Unexpected(std::move(reference.error()));
    }
    if (reference->value("schema", std::string()) != "lucent.render-reference" ||
        reference->value("schema_version", 0) != 2) {
        return fail(ErrorCode::VersionMismatch,
                    "render_reference.json is not a lucent.render-reference v2 document", "qualify.corpus");
    }
    corpus.render_reference = std::move(*reference);
    return corpus;
}

const ManifestFixture* QualifyCorpus::find(std::string_view id) const noexcept {
    const auto it =
        std::find_if(manifest.begin(), manifest.end(), [&](const ManifestFixture& f) { return f.id == id; });
    return it == manifest.end() ? nullptr : &*it;
}

const CheckResult* FixtureResult::check(std::string_view check_id) const noexcept {
    const auto it =
        std::find_if(checks.begin(), checks.end(), [&](const CheckResult& c) { return c.id == check_id; });
    return it == checks.end() ? nullptr : &*it;
}

ProcessMemory process_memory() noexcept {
    ProcessMemory m;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        m.peak_working_set_bytes = counters.PeakWorkingSetSize;
        m.peak_commit_bytes = counters.PeakPagefileUsage;
    }
    return m;
}

// ---- The suite --------------------------------------------------------------------------------------

FixtureResult qualify_fixture(const QualifyCorpus& corpus, const ManifestFixture& fixture,
                              const QualifyOptions& options) {
    const Clock::time_point started = Clock::now();
    FixtureResult result;
    result.id = fixture.id;
    result.tier = fixture.tier;
    result.file_name = fixture.file_name;
    const FixtureExpectation* expectation =
        corpus.expectations ? corpus.expectations->find(fixture.id) : nullptr;
    for (const std::string_view id : check_ids()) {
        check_of(result, id);  // fixes the order; every check starts as not_run
    }
    const auto finish = [&]() {
        result.measurements.total_seconds = seconds_since(started);
        const ProcessMemory memory = process_memory();
        result.measurements.peak_working_set_bytes = memory.peak_working_set_bytes;
        result.measurements.peak_commit_bytes = memory.peak_commit_bytes;
        propose_state(result, expectation);
        return result;
    };
    const auto timed = [&](CheckResult& c, Clock::time_point t0) { c.seconds += seconds_since(t0); };

    // ---- fixture_integrity --------------------------------------------------------------------
    const std::filesystem::path file = corpus.paths.cached_file(fixture);
    {
        CheckResult& c = check_of(result, kCheckFixtureIntegrity);
        const Clock::time_point t0 = Clock::now();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            set_check(c, CheckStatus::Fail, Evidence::None,
                      "fixture is not downloaded (tools\\fetch-fixtures.ps1)");
            result.harness_error = "fixture file missing: " + core::path_to_utf8_lossy(file);
            timed(c, t0);
            return finish();
        }
        auto hashed = core::hash_file(file, {});
        if (!hashed) {
            set_check(c, CheckStatus::Fail, Evidence::None, "hash failed: " + describe(hashed.error()));
            result.harness_error = c.summary;
            timed(c, t0);
            return finish();
        }
        result.file_size = hashed->identity.size_bytes;
        result.file_sha256 = core::to_hex(hashed->digest);
        c.details["size_bytes"] = result.file_size;
        c.details["sha256"] = result.file_sha256;
        if (result.file_size != fixture.size_bytes || result.file_sha256 != fixture.sha256) {
            set_check(c, CheckStatus::Fail, Evidence::None, "the cached file differs from the manifest");
            result.harness_error = c.summary;
            timed(c, t0);
            return finish();
        }
        set_check(c, CheckStatus::Pass, Evidence::Strong, "size and SHA-256 match the manifest");
        timed(c, t0);
    }

    auto registry = raw::create_default_registry();
    if (!registry) {
        result.harness_error = "decoder registry: " + describe(registry.error());
        return finish();
    }
    {
        const raw::LibRawBuildInfo build = raw::libraw_build_info();
        result.decoder =
            "LibRaw " + build.version + "; decoded-RAW " + raw::kDecodedRawContractVersion.to_string();
    }

    // ---- Probe, decode and everything that reads the decoded data ---------------------------
    raw::ProbeOptions probe_options;
    probe_options.describe_subimages = true;
    Clock::time_point t_phase = Clock::now();
    auto selection = registry->select(file, probe_options, {});
    result.measurements.phase_seconds["probe"] = seconds_since(t_phase);

    std::optional<raw::RawMetadata> metadata;  // decode metadata, else probe metadata
    std::vector<raw::SubimageInfo> subimages;
    if (selection) {
        metadata = selection->probe.metadata;
        subimages = selection->probe.subimages;
        // The application's view of the mode: its import probes, it does not decode (ADR 0018).
        result.lookup_key = derive_mode_key(selection->probe.metadata).to_string();
        const assets::ModeVariant variant = assets::derive_mode_variant(selection->probe.metadata);
        result.lookup_variant = {{"image_area", variant.image_area}, {"dng_opcodes", variant.dng_opcodes}};
    }

    const Json reference_report = [&]() -> Json {
        auto r = read_json_file(corpus.paths.reference_dir() / (fixture.id + ".parity.json"));
        return r ? *r : Json();
    }();

    std::optional<RawLevelStats> level_stats;
    {
        CheckResult& c = check_of(result, kCheckDecodeParity);
        const Clock::time_point t0 = Clock::now();
        std::optional<raw::DecodedRaw> decoded;
        if (!selection) {
            result.decode_error = describe(selection.error());
        } else {
            raw::DecodeOptions decode_options;
            decode_options.subimage = 0u;  // the reference decodes shot_select 0
            t_phase = Clock::now();
            auto d = selection->decoder->decode(file, decode_options, {});
            result.measurements.phase_seconds["decode"] = seconds_since(t_phase);
            if (d) {
                decoded = std::move(*d);
            } else {
                result.decode_error = describe(d.error());
            }
        }
        result.decoded = decoded.has_value();
        const bool reference_refused = reference_report.is_object() && reference_report.contains("parity") &&
                                       reference_report["parity"].is_null();
        if (!reference_report.is_object() ||
            reference_report.value("file_sha256", std::string()) != result.file_sha256) {
            set_check(c, CheckStatus::NotRun, Evidence::None,
                      "no committed rawpy reference for exactly these bytes (fixtures/reference)");
        } else if (!decoded) {
            const bool unsupported = result.decode_error.rfind("unsupported", 0) == 0;
            if (reference_refused && unsupported) {
                set_check(c, CheckStatus::Pass, Evidence::Strong,
                          "refused as Unsupported, as the reference decoder refuses it (" +
                              reference_report["error"].value("message", std::string()) + ")");
            } else {
                set_check(c, CheckStatus::Fail, Evidence::Strong, "decode failed: " + result.decode_error);
            }
            c.details["lucent_error"] = result.decode_error;
        } else if (reference_refused) {
            set_check(c, CheckStatus::Fail, Evidence::Strong, "Lucent decoded a file the reference refuses");
        } else if (!decoded->metadata.decoder.reference_parity) {
            set_check(c, CheckStatus::Fail, Evidence::Strong, "the decoder reported no parity block");
        } else {
            const raw::ReferenceParity& parity = *decoded->metadata.decoder.reference_parity;
            auto raw_hash = probe::raw_data_sha256(*decoded);
            const Json lucent = Json::parse(
                probe::parity_json(parity, parity.raw_type == "flat" && raw_hash ? *raw_hash : std::nullopt)
                    .dump());
            const std::vector<std::string> problems = compare_parity(lucent, reference_report["parity"]);
            const bool hashed = lucent.contains("raw_data_sha256");
            c.details["fields_compared"] = lucent.size();
            c.details["raw_data_sha256_compared"] = hashed;
            c.details["reference"] = "rawpy " + reference_report["generator"].value("rawpy", std::string()) +
                                     " (LibRaw " + reference_report["generator"]["libraw"].dump() + ")";
            if (!problems.empty()) {
                c.details["differences"] = problems;
                set_check(c, CheckStatus::Fail, Evidence::Strong,
                          std::to_string(problems.size()) + " parity fields differ: " + problems.front());
            } else {
                set_check(c, CheckStatus::Pass, Evidence::Strong,
                          hashed
                              ? "all parity fields and the raw-buffer SHA-256 match the rawpy reference"
                              : "all parity fields match the rawpy reference (full-colour data: pixels are "
                                "compared through the colour check)");
            }
        }
        if (decoded) {
            metadata = decoded->metadata;
            const Status consistent = raw::check_consistency(*decoded);
            if (!consistent) {
                set_check(c, CheckStatus::Fail, Evidence::Strong,
                          "contract violated: " + describe(consistent.error()));
            }
            level_stats = raw_level_stats(*decoded);
        }
        timed(c, t0);
    }  // the decoded pixels are released here

    if (!metadata) {
        // Not even a probe: nothing else can be assessed.
        CheckResult& c = check_of(result, kCheckInterpretation);
        set_check(c, CheckStatus::Fail, Evidence::Strong,
                  "the file could not be identified: " + result.decode_error);
        return finish();
    }
    const raw::RawMetadata& meta = *metadata;
    {
        const raw::ImageArea& a = meta.image_area;
        OrderedJson declared = nullptr;
        if (a.rect) {
            declared = {
                {"x", a.rect->x}, {"y", a.rect->y}, {"width", a.rect->width}, {"height", a.rect->height}};
        }
        OrderedJson user = nullptr;
        if (a.user_crop) {
            user = {{"top", a.user_crop->top},
                    {"left", a.user_crop->left},
                    {"bottom", a.user_crop->bottom},
                    {"right", a.user_crop->right}};
        }
        result.camera_area = {{"source", std::string(raw::to_string(a.source))},
                              {"provenance", std::string(raw::to_string(a.provenance))},
                              {"declared", std::move(declared)},
                              {"user_crop", std::move(user)},
                              {"basis", a.basis},
                              {"applied", nullptr},
                              {"output", nullptr}};
    }
    result.camera_make = meta.camera.make;
    result.camera_model = meta.camera.model;
    result.firmware = meta.camera.software;
    result.lens = meta.lens.model;
    result.mode_key = derive_mode_key(meta);
    result.interpretation_status = std::string(raw::to_string(meta.interpretation.status));
    for (const raw::InterpretationIssue& issue : meta.interpretation.issues) {
        result.interpretation_issues.push_back(std::string(raw::to_string(issue.kind)) + ": " +
                                               issue.message);
    }

    // ---- mode_key --------------------------------------------------------------------------------
    {
        CheckResult& c = check_of(result, kCheckModeKey);
        const std::string derived = result.mode_key->to_string();
        c.details["derived"] = derived;
        c.details["stage"] = result.mode_key->stage;
        if (result.lookup_key && *result.lookup_key != derived) {
            c.details["probe_key"] = *result.lookup_key;
        }
        c.details["bits_provenance"] = result.mode_key->bits_provenance;
        if (expectation == nullptr) {
            set_check(c, CheckStatus::NotRun, Evidence::None, "no expectation to compare with");
        } else if (derived != expectation->mode_key) {
            c.details["expected"] = expectation->mode_key;
            set_check(c, CheckStatus::Fail, Evidence::Strong,
                      "derived " + derived + ", expected " + expectation->mode_key);
        } else {
            set_check(c, CheckStatus::Pass,
                      expectation->compression.verified() ? Evidence::Strong : Evidence::Weak,
                      "derived key equals the expectation");
        }
    }

    // ---- subimages ---------------------------------------------------------------------------------
    {
        CheckResult& c = check_of(result, kCheckSubimages);
        const Clock::time_point t0 = Clock::now();
        OrderedJson list = OrderedJson::array();
        bool roles_ok = !subimages.empty() || meta.subimage_count == 1;
        for (const raw::SubimageInfo& s : subimages) {
            list.push_back({{"index", s.index},
                            {"role", raw::to_string(s.role)},
                            {"interpretation", raw::to_string(s.interpretation)}});
            if (s.role == raw::SubimageRole::DualPixelComponent) {
                add_limitation(
                    result, kLimitDualPixel,
                    "image " + std::to_string(s.index) +
                        " is a Dual Pixel component frame; Lucent develops image 0 and offers no Dual "
                        "Pixel processing");
            }
            if (s.role == raw::SubimageRole::PixelShiftFrame) {
                add_limitation(result, kLimitPixelShift, "pixel-shift frames are not combined");
            }
            const bool photograph =
                s.role == raw::SubimageRole::Primary || s.role == raw::SubimageRole::BurstFrame;
            if (!photograph && s.interpretation == raw::InterpretationStatus::Complete) {
                roles_ok = false;  // a component frame must never look complete
            }
        }
        c.details["count"] = meta.subimage_count;
        c.details["subimages"] = std::move(list);
        if (meta.subimage_count > 1) {
            raw::DecodeOptions implicit;  // no subimage chosen
            auto refused = selection->decoder->decode(file, implicit, {});
            const bool refused_ok = !refused && refused.error().code == ErrorCode::InvalidArgument;
            c.details["decode_without_subimage"] =
                refused ? std::string("decoded") : describe(refused.error());
            if (!refused_ok) {
                set_check(c, CheckStatus::Fail, Evidence::Strong,
                          "a decode without an explicit subimage was not refused with InvalidArgument");
            } else if (!roles_ok) {
                set_check(c, CheckStatus::Fail, Evidence::Strong,
                          "a non-photograph subimage is reported complete");
            } else {
                set_check(
                    c, CheckStatus::Pass, Evidence::Strong,
                    std::to_string(meta.subimage_count) +
                        " images listed with roles; an implicit decode is refused (no silent first image)");
            }
        } else if (!roles_ok || meta.subimage_role != raw::SubimageRole::Primary) {
            set_check(c, CheckStatus::Fail, Evidence::Strong, "single image without the primary role");
        } else {
            set_check(c, CheckStatus::Pass, Evidence::Strong, "single primary image");
        }
        timed(c, t0);
    }

    // ---- interpretation -----------------------------------------------------------------------------
    {
        CheckResult& c = check_of(result, kCheckInterpretation);
        c.details["status"] = result.interpretation_status;
        c.details["issues"] = result.interpretation_issues;
        OrderedJson prior = OrderedJson::array();
        for (const raw::PriorProcessingStep& p : meta.prior_processing) {
            prior.push_back(std::string(raw::to_string(p.kind)));
        }
        c.details["prior_processing"] = std::move(prior);
        const raw::InterpretationStatus s = meta.interpretation.status;
        if (!result.decoded) {
            set_check(c, CheckStatus::Fail, Evidence::Strong,
                      "the decoder cannot decode this mode (" + result.interpretation_status +
                          "): " + result.decode_error);
        } else if (s == raw::InterpretationStatus::Complete) {
            set_check(c, CheckStatus::Pass, Evidence::Strong, "complete: nothing required is missing");
        } else if (s == raw::InterpretationStatus::OptionalOperationsSkipped) {
            set_check(c, CheckStatus::Pass, Evidence::Strong, "only optional operations are unavailable");
            add_limitation(result, kLimitOptionalOperations, join(result.interpretation_issues, "; "));
        } else {
            set_check(c, CheckStatus::Fail, Evidence::Strong,
                      "a mandatory interpretation step is unavailable (" + result.interpretation_status +
                          "): " + join(result.interpretation_issues, "; "));
        }
        if (meta.geometry.pixel_aspect != 1.0) {
            add_limitation(
                result, kLimitNonSquarePixels,
                "pixel aspect " + std::to_string(meta.geometry.pixel_aspect) + " is not resampled");
        }
        if (meta.format.container == raw::ContainerFormat::Dng) {
            std::string where;
            if (dng_has_enhanced_image(file, where)) {
                add_limitation(result, kLimitEnhancedImage,
                               "the DNG holds an Enhanced image (NewSubFileType 16, " + where +
                                   ") that Lucent does not offer; the original raw image is developed");
            }
        }
    }

    // ---- cfa and black_white (decoded data) -----------------------------------------------------------
    {
        CheckResult& c = check_of(result, kCheckCfa);
        const std::string actual = representation_text(meta);
        c.details["representation"] = actual;
        if (!result.decoded) {
            set_check(c, CheckStatus::NotRun, Evidence::None, "not decoded");
        } else {
            std::string problem;
            if (expectation != nullptr && expectation->representation != actual) {
                problem = "representation " + actual + ", expected " + expectation->representation;
            }
            bool physics = false;
            const image::CfaPattern cfa = raw::cfa_at_visible_origin(meta);
            if (level_stats && meta.representation.kind == raw::RawRepresentation::Mosaic && cfa.is_bayer()) {
                // Sites of the 2 x 2 phase in visible coordinates: 0 (0,0), 1 (0,1), 2 (1,0), 3 (1,1).
                const auto& mean = level_stats->phase_mean;
                const bool greens_on_main = image::is_green(cfa.color_at(0, 0));
                const double g1 = greens_on_main ? mean[0] : mean[1];
                const double g2 = greens_on_main ? mean[3] : mean[2];
                const double o1 = greens_on_main ? mean[1] : mean[0];
                const double o2 = greens_on_main ? mean[2] : mean[3];
                const auto rel = [](double a, double b) {
                    return std::abs(a - b) / std::max(1e-9, 0.5 * (a + b));
                };
                const double rel_green = rel(g1, g2);
                const double rel_other = rel(o1, o2);
                c.details["phase_means_black_subtracted"] = {mean[0], mean[1], mean[2], mean[3]};
                c.details["green_pair_relative_difference"] = rel_green;
                c.details["red_blue_pair_relative_difference"] = rel_other;
                physics = true;
                if (problem.empty() && !(rel_green < 0.05 && rel_green < rel_other)) {
                    problem =
                        "the sites labelled green do not behave as the green pair (relative difference " +
                        std::to_string(rel_green) + " vs " + std::to_string(rel_other) + ")";
                }
            }
            if (!problem.empty()) {
                set_check(c, CheckStatus::Fail, Evidence::Strong, problem);
            } else {
                const bool fact = expectation != nullptr && expectation->representation_fact.verified();
                set_check(c, CheckStatus::Pass, fact ? Evidence::Strong : Evidence::Weak,
                          actual + (expectation ? " as expected" : "") +
                              (physics ? "; green-site statistics confirm the Bayer phase" : ""));
            }
        }
    }
    {
        CheckResult& c = check_of(result, kCheckBlackWhite);
        if (!result.decoded || !level_stats) {
            set_check(c, CheckStatus::NotRun, Evidence::None, "not decoded");
        } else if (meta.representation.sample_encoding != raw::SampleEncoding::UInt16) {
            set_check(c, CheckStatus::NotApplicable, Evidence::None, "floating-point data");
        } else {
            const RawLevelStats& s = *level_stats;
            const double white = meta.levels.white;
            const double plateau_limit = std::max<double>(64.0, 1e-6 * static_cast<double>(s.samples));
            c.details["white"] = white;
            c.details["max_black_level"] = s.max_black;
            c.details["samples"] = s.samples;
            c.details["samples_above_white"] = s.above_white;
            c.details["max_sample"] = s.max_sample;
            c.details["samples_at_max"] = s.at_max;
            c.details["samples_far_below_black"] = s.far_below_black;
            const double below_fraction = static_cast<double>(s.far_below_black) /
                                          static_cast<double>(std::max<std::uint64_t>(s.samples, 1));
            const double above_fraction = static_cast<double>(s.above_white) /
                                          static_cast<double>(std::max<std::uint64_t>(s.samples, 1));
            c.details["fraction_above_white"] = above_fraction;
            // A DNG declares its white level; the decoder must use exactly that value.
            std::optional<double> declared;
            if (meta.dng && !meta.dng->raw_ifd.white_level.empty()) {
                const auto& w = meta.dng->raw_ifd.white_level;
                declared = static_cast<double>(*std::min_element(w.begin(), w.end()));
                c.details["dng_white_level"] = *declared;
            }
            std::string problem;
            if (!(white > s.max_black)) {
                problem = "white level not above the black level";
            } else if (declared && *declared != white) {
                problem = "the decoder's white level " + std::to_string(white) +
                          " differs from the DNG WhiteLevel " + std::to_string(*declared);
            } else if (static_cast<double>(s.at_max) >= plateau_limit && s.max_sample < 0.97 * white) {
                problem = "the data clips at " + std::to_string(s.max_sample) + " (" +
                          std::to_string(s.at_max) + " samples) below the white level " +
                          std::to_string(white);
            } else if (below_fraction > 1e-3) {
                problem = "black level too high: " + std::to_string(100.0 * below_fraction) +
                          " % of samples lie more than 2 % of the range below it";
            }
            if (!problem.empty()) {
                set_check(c, CheckStatus::Fail, Evidence::Strong, problem);
            } else {
                // Samples above the white level are not an error: the engine treats them as saturated.
                // They occur where a file declares a white level below its encoding range (Leica M10
                // WhiteLevel 15000), where a lossy codec overshoots (Sony ARW compressed) or where the
                // decoder converted YCbCr or demosaiced data (sRAW, Sony M/S, linear DNG).
                std::ostringstream summary;
                summary.precision(3);
                summary << "levels consistent with the data: white " << white
                        << (declared ? " (as the DNG declares)" : "") << ", data max " << s.max_sample << ", "
                        << 100.0 * above_fraction << " % of samples above white (treated as saturated)";
                set_check(c, CheckStatus::Pass, Evidence::Strong, summary.str());
            }
        }
    }

    if (!result.decoded) {
        return finish();
    }

    // ---- colour (W2 path without opcodes or the camera area, like the reference decoder) ------------
    {
        CheckResult& c = check_of(result, kCheckColour);
        CheckResult& numerical = check_of(result, kCheckNumericalValidity);
        const Clock::time_point t0 = Clock::now();
        engine::RenderSettings settings;
        settings.apply_dng_opcodes = false;
        settings.apply_camera_area = false;  // rawpy renders LibRaw's whole visible area (ADR 0017)
        engine::RenderOptions render_options;
        render_options.keep_scene_linear = true;
        auto render = engine::render_file(*registry, file, 0u, settings, render_options, {});
        result.measurements.phase_seconds["w2_render"] = seconds_since(t0);
        const Json& fixtures = corpus.render_reference.at("fixtures");
        if (!render) {
            set_check(c, CheckStatus::Fail, Evidence::Strong,
                      "W2 render failed: " + describe(render.error()));
            numerical.details["w2"] = "render failed";
        } else {
            result.measurements.w2_estimate_bytes = render->report.memory.peak_bytes;
            const NumericStats scene = numeric_stats(*render->scene_linear, false);
            const NumericStats display = numeric_stats(render->display, true);
            numerical.details["w2_scene_linear_non_finite"] = scene.non_finite;
            numerical.details["w2_display_non_finite"] = display.non_finite;
            numerical.details["w2_display_above_one"] = display.above_one;
            if (!fixtures.contains(fixture.id) || fixtures.at(fixture.id).contains("refused")) {
                set_check(c, CheckStatus::NotRun, Evidence::None, "no committed rawpy render statistics");
            } else {
                const Json& ref = fixtures.at(fixture.id);
                const auto grid = corpus.render_reference.at("grid").get<std::int32_t>();
                auto to_srgb = image::conversion_matrix(render->scene_linear->encoding,
                                                        image::ColorEncoding::linear_srgb());
                const std::vector<Rgb> ours = region_means(*render->scene_linear, grid, *to_srgb);
                const GateStats stats = compare_regions(ours, ref.at("reference_region_means"));
                const Json& measured = ref.at("measured");
                const double mean_limit =
                    std::min(kGateRegionDeltaEMean,
                             kRegressionFactor * measured.at("region_delta_e_mean").get<double>() +
                                 kRegressionMeanFloor);
                const double max_limit =
                    std::min(kGateRegionDeltaEMax,
                             kRegressionFactor * measured.at("region_delta_e_max").get<double>() +
                                 kRegressionMaxFloor);
                const double ratio_ref = measured.at("region_luminance_ratio").get<double>();
                const engine::CalibrationPath path = render->report.color.path;
                const bool like_for_like = path == engine::CalibrationPath::DecoderMatrix ||
                                           path == engine::CalibrationPath::MonochromeNeutral;
                c.details["calibration_path"] = std::string(engine::to_string(path));
                c.details["like_for_like"] = like_for_like;
                c.details["region_delta_e_mean"] = stats.mean_de;
                c.details["region_delta_e_max"] = stats.max_de;
                c.details["luminance_ratio"] = stats.ratio;
                c.details["limits"] = {{"mean", mean_limit},
                                       {"max", max_limit},
                                       {"ratio", ratio_ref},
                                       {"ratio_tolerance", kRegressionRatioTolerance}};
                const std::size_t expected_size =
                    static_cast<std::size_t>(grid) * static_cast<std::size_t>(grid);
                const bool sizes_ok =
                    ours.size() == expected_size &&
                    ref.at("reference_region_means").size() == expected_size &&
                    render->scene_linear->pixels.width() == ref.at("size").at(0).get<std::int32_t>() &&
                    render->scene_linear->pixels.height() == ref.at("size").at(1).get<std::int32_t>();
                std::ostringstream summary;
                summary.precision(3);
                summary << "region CIEDE2000 mean " << stats.mean_de << " (limit " << mean_limit << "), max "
                        << stats.max_de << " (limit " << max_limit << "), luminance ratio " << stats.ratio;
                if (!sizes_ok) {
                    set_check(c, CheckStatus::Fail, Evidence::Strong,
                              "render size differs from the reference's");
                } else if (stats.mean_de > mean_limit || stats.max_de > max_limit ||
                           stats.ratio < kGateLuminanceRatioLow || stats.ratio > kGateLuminanceRatioHigh ||
                           std::abs(stats.ratio - ratio_ref) > kRegressionRatioTolerance) {
                    set_check(c, CheckStatus::Fail, Evidence::Strong, summary.str());
                } else {
                    summary << (like_for_like
                                    ? "; like for like with the reference (same calibration)"
                                    : "; not like for like (Lucent applies the DNG camera profile, the "
                                      "reference LibRaw's D65 matrix): gross errors only");
                    set_check(c, CheckStatus::Pass, like_for_like ? Evidence::Strong : Evidence::Weak,
                              summary.str());
                }
            }
            if (scene.non_finite + display.non_finite + display.above_one > 0) {
                set_check(numerical, CheckStatus::Fail, Evidence::Strong,
                          "W2 render: " + std::to_string(scene.non_finite + display.non_finite) +
                              " non-finite samples, " + std::to_string(display.above_one) +
                              " display samples above 1");
            }
        }
        timed(c, t0);
    }  // W2 images released

    // ---- Develop render 1: dimensions, orientation, corrections, export ------------------------------
    std::string display_hash;
    std::string jpeg_sha256;
    raw::CaptureMetadata capture;
    {
        auto read = raw::read_capture_metadata(file);
        if (read) {
            capture = std::move(*read);
        }
        raw::complete_from_decoder(capture, meta);
    }
    const exporter::ExportMetadata export_metadata =
        worker_app::make_export_metadata(capture, std::nullopt, std::nullopt);
    const std::filesystem::path work = options.work_dir / fixture.id;
    {
        std::error_code ec;
        std::filesystem::remove_all(work, ec);
        std::filesystem::create_directories(work, ec);
    }
    {
        engine::DevelopSettings settings;
        engine::StageOneRequest request;
        request.kind = engine::StageOneKind::Export;
        auto estimate = engine::estimate_develop_memory(meta, request, settings);
        if (estimate) {
            result.measurements.develop_estimate_bytes = estimate->export_peak;
        }
        const Clock::time_point t0 = Clock::now();
        auto developed = engine::render_develop_file(*registry, file, 0u, settings, {}, {});
        result.measurements.phase_seconds["develop_1"] = seconds_since(t0);
        CheckResult& dims = check_of(result, kCheckDimensions);
        CheckResult& orient = check_of(result, kCheckOrientation);
        CheckResult& corrections = check_of(result, kCheckRequiredCorrections);
        CheckResult& numerical = check_of(result, kCheckNumericalValidity);
        CheckResult& exported = check_of(result, kCheckExportRoundTrip);
        if (!developed) {
            const std::string why = "develop render failed: " + describe(developed.error());
            for (CheckResult* c : {&dims, &orient, &corrections, &exported}) {
                set_check(*c, CheckStatus::Fail, Evidence::Strong, why);
            }
            set_check(numerical, CheckStatus::Fail, Evidence::Strong, why);
        } else {
            const engine::DevelopReport& report = developed->report;
            const engine::ColorImage& display = developed->display;
            const image::Size actual = display.pixels.size();
            if (result.camera_area.is_object()) {
                const image::PixelRect& f = report.geometry_frame;
                const bool whole = f == image::PixelRect::from_size(report.upright_size);
                result.camera_area["applied"] =
                    whole ? OrderedJson(nullptr)
                          : OrderedJson{{"x", f.x}, {"y", f.y}, {"width", f.width}, {"height", f.height}};
                result.camera_area["output"] = {actual.width, actual.height};
            }

            // dimensions
            {
                const Clock::time_point tc = Clock::now();
                std::vector<std::string> problems;
                dims.details["rendered"] = {actual.width, actual.height};
                if (expectation != nullptr && expectation->rendered &&
                    !(expectation->rendered->width == actual.width &&
                      expectation->rendered->height == actual.height)) {
                    problems.push_back(
                        "rendered " + size_text(actual.width, actual.height) + ", expected " +
                        size_text(expectation->rendered->width, expectation->rendered->height));
                }
                if (expectation != nullptr && expectation->camera_image_area) {
                    const Size2 area = *expectation->camera_image_area;
                    dims.details["camera_image_area"] = {area.width, area.height};
                    if (actual.width < area.width - kAreaShortfallTolerancePx ||
                        actual.height < area.height - kAreaShortfallTolerancePx) {
                        problems.push_back("the output is smaller than the camera's image area " +
                                           size_text(area.width, area.height));
                    } else if (actual.width > area.width * (1.0 + kMaxAreaExcess) ||
                               actual.height > area.height * (1.0 + kMaxAreaExcess)) {
                        problems.push_back("the output exceeds the camera's image area " +
                                           size_text(area.width, area.height) +
                                           " by more than the permissible 3 %");
                    } else if (actual.width > area.width + kAreaShortfallTolerancePx ||
                               actual.height > area.height + kAreaShortfallTolerancePx) {
                        add_limitation(
                            result, kLimitDefaultCrop,
                            "the camera's image area is " + size_text(area.width, area.height) +
                                "; Lucent renders " + size_text(actual.width, actual.height) +
                                (meta.image_area.source == raw::ImageAreaSource::Unknown
                                     ? ", the decoder's visible area: the file declares no image "
                                       "area the decoder reads (" +
                                           meta.image_area.basis + ")"
                                     : ", the area the file declares (" +
                                           std::string(raw::to_string(meta.image_area.source)) + ")"));
                    }
                }
                if (meta.dng && meta.dng->raw_ifd.default_crop_size) {
                    // Applied when the output (the identity geometry of version 2) has its size, in
                    // the upright orientation.
                    const auto& crop = *meta.dng->raw_ifd.default_crop_size;
                    const image::Size declared =
                        image::oriented_size(image::Size{static_cast<std::int32_t>(std::lround(crop[0])),
                                                         static_cast<std::int32_t>(std::lround(crop[1]))},
                                             meta.geometry.orientation);
                    dims.details["dng_default_crop_size"] = {crop[0], crop[1]};
                    if (declared.width + kAreaShortfallTolerancePx < actual.width ||
                        declared.height + kAreaShortfallTolerancePx < actual.height) {
                        add_limitation(result, kLimitDefaultCrop,
                                       "DNG DefaultCropSize " +
                                           size_text(static_cast<std::int32_t>(crop[0]),
                                                     static_cast<std::int32_t>(crop[1])) +
                                           " is not applied");
                    }
                }
                dims.details["image_area_source"] = std::string(raw::to_string(meta.image_area.source));
                dims.details["geometry_frame"] = {report.geometry_frame.x, report.geometry_frame.y,
                                                  report.geometry_frame.width, report.geometry_frame.height};
                dims.details["upright_size"] = {report.upright_size.width, report.upright_size.height};
                OrderedJson bands = OrderedJson::array();
                // A multi-view frame (two fisheye circles) is dark outside its image circles by
                // construction: its borders say nothing about the decoder's visible area.
                const bool single_view = meta.projection.layout == raw::ViewLayout::SingleView;
                dims.details["edge_bands_evaluated"] = single_view;
                for (const EdgeBand& band : single_view ? edge_bands(display) : std::vector<EdgeBand>{}) {
                    const double ratio = band.inner > 0.0 ? band.outer / band.inner : 0.0;
                    bands.push_back({{"side", band.side},
                                     {"outer", band.outer},
                                     {"inner", band.inner},
                                     {"ratio", ratio}});
                    if (band.inner > 0.02 && (ratio < 0.5 || ratio > 2.0)) {
                        problems.push_back(std::string("the ") + band.side +
                                           " edge band is not image data (outer/inner luminance " +
                                           std::to_string(ratio) + ")");
                    }
                }
                dims.details["edge_bands"] = std::move(bands);
                if (!problems.empty()) {
                    set_check(dims, CheckStatus::Fail, Evidence::Strong, join(problems, "; "));
                } else {
                    const bool verified = expectation != nullptr &&
                                          expectation->camera_image_area_fact.verified() &&
                                          expectation->rendered.has_value();
                    set_check(dims, CheckStatus::Pass, verified ? Evidence::Strong : Evidence::Weak,
                              size_text(actual.width, actual.height) +
                                  (expectation && expectation->camera_image_area
                                       ? " contains the camera's image area " +
                                             size_text(expectation->camera_image_area->width,
                                                       expectation->camera_image_area->height)
                                       : std::string(" (camera image area unknown)")) +
                                  (single_view ? "; edge bands are image data"
                                               : "; multi-view frame, edge bands not evaluated"));
                }
                dims.seconds += seconds_since(tc);
            }

            // orientation
            {
                const int decoded_exif = image::to_exif(meta.geometry.orientation);
                orient.details["decoded_exif"] = decoded_exif;
                orient.details["baked_exif"] = image::to_exif(report.orientation);
                std::string problem;
                if (report.orientation != meta.geometry.orientation) {
                    problem = "the render did not bake the file's orientation";
                } else if (expectation != nullptr && expectation->orientation_exif != decoded_exif) {
                    problem = "orientation " + std::to_string(decoded_exif) + ", expected " +
                              std::to_string(expectation->orientation_exif);
                } else if (image::oriented_size(meta.geometry.visible_area.size(),
                                                meta.geometry.orientation) != report.upright_size) {
                    problem = "the stage-one image does not follow the orientation";
                } else if (report.geometry_frame.size() != actual) {
                    problem = "the output is not the geometry's frame";
                }
                if (!problem.empty()) {
                    set_check(orient, CheckStatus::Fail, Evidence::Strong, problem);
                } else {
                    const bool verified = expectation != nullptr && expectation->orientation.verified();
                    set_check(orient, CheckStatus::Pass, verified ? Evidence::Strong : Evidence::Weak,
                              "EXIF orientation " + std::to_string(decoded_exif) + " baked into the pixels");
                }
            }

            // required_corrections
            {
                std::vector<std::string> problems;
                std::set<std::string> applied;
                OrderedJson records = OrderedJson::array();
                for (const engine::DngOpcodeRecord& r : report.dng_opcodes.opcodes) {
                    records.push_back({{"step", opcode_step_id(r.list, r.index, r.name)},
                                       {"mandatory", r.mandatory},
                                       {"outcome", engine::to_string(r.outcome)}});
                    const bool done = r.outcome == engine::OpcodeOutcome::Applied ||
                                      r.outcome == engine::OpcodeOutcome::AppliedByDecoder ||
                                      r.outcome == engine::OpcodeOutcome::SkippedByWarpRule;
                    if (r.mandatory && !done) {
                        problems.push_back(opcode_step_id(r.list, r.index, r.name) + " " +
                                           std::string(engine::to_string(r.outcome)));
                    }
                    if (r.mandatory && r.outcome == engine::OpcodeOutcome::Applied) {
                        applied.insert(opcode_step_id(r.list, r.index, r.name));
                    }
                }
                corrections.details["opcodes"] = std::move(records);
                for (const raw::RequiredOperation& op : meta.required_operations) {
                    if (op.mandatory && !op.supported_by_lucent && !op.applied_by_decoder &&
                        !op.superseded_by_warp_rectilinear2) {
                        problems.push_back(op.name + " is mandatory and not supported");
                    }
                }
                std::set<std::string> expected_steps;
                if (expectation != nullptr) {
                    for (const MandatoryStep& s : expectation->mandatory_steps) {
                        if (s.id.rfind("dng_opcode:", 0) == 0) {
                            expected_steps.insert(s.id);
                        }
                    }
                    if (expected_steps != applied) {
                        problems.push_back(
                            "applied mandatory opcodes {" +
                            join(std::vector<std::string>(applied.begin(), applied.end()), ", ") +
                            "} differ from the expected {" +
                            join(std::vector<std::string>(expected_steps.begin(), expected_steps.end()),
                                 ", ") +
                            "}");
                    }
                }
                if (!problems.empty()) {
                    set_check(corrections, CheckStatus::Fail, Evidence::Strong, join(problems, "; "));
                } else if (report.dng_opcodes.opcodes.empty() && meta.required_operations.empty()) {
                    set_check(corrections, CheckStatus::NotApplicable, Evidence::None,
                              "the file requires no correction operations");
                } else {
                    set_check(
                        corrections, CheckStatus::Pass, Evidence::Strong,
                        std::to_string(applied.size()) + " mandatory DNG opcodes applied at their stages");
                }
            }

            // limitations from the develop path
            if (report.demosaic.maturity == engine::RouteMaturity::Baseline) {
                add_limitation(
                    result, kLimitXTransBaseline,
                    std::string("demosaic route ") + std::string(engine::to_string(report.demosaic.route)) +
                        " is baseline quality (expect softness and colour artefacts on fine detail)");
            }

            // numerical_validity (develop part)
            {
                const NumericStats s = numeric_stats(display, true);
                numerical.details["develop_display_non_finite"] = s.non_finite;
                numerical.details["develop_display_above_one"] = s.above_one;
                numerical.details["develop_display_max"] = s.max_value;
                if (s.non_finite + s.above_one > 0) {
                    set_check(numerical, CheckStatus::Fail, Evidence::Strong,
                              "develop render: " + std::to_string(s.non_finite) + " non-finite samples, " +
                                  std::to_string(s.above_one) + " display samples above 1");
                } else if (numerical.status != CheckStatus::Fail) {
                    set_check(numerical, CheckStatus::Pass, Evidence::Strong,
                              "W2 and develop images finite; display max(R, G, B) <= 1");
                }
            }
            display_hash = pixel_hash(display);

            // export_round_trip
            {
                const Clock::time_point te = Clock::now();
                const ExportOutcome jpeg =
                    export_and_read_back(display, work / "render.jpg",
                                         exporter::ExportSettings::jpeg_srgb_q95(), export_metadata, capture);
                // Uncompressed: Deflate does not depend on the camera (the exporter's own tests cover
                // it) and costs about 10 s per 45 MP image.
                exporter::ExportSettings tiff_settings =
                    exporter::ExportSettings::tiff16(exporter::OutputProfile::DisplayP3);
                tiff_settings.tiff.compression = exporter::TiffCompression::None;
                const ExportOutcome tiff = export_and_read_back(display, work / "render.tif", tiff_settings,
                                                                export_metadata, capture);
                jpeg_sha256 = jpeg.sha256;
                exported.details["jpeg_srgb_q95"] = jpeg.details;
                exported.details["tiff16_display_p3"] = tiff.details;
                if (!jpeg.ok || !tiff.ok) {
                    set_check(exported, CheckStatus::Fail, Evidence::Strong,
                              !jpeg.ok ? "JPEG: " + jpeg.problem : "TIFF: " + tiff.problem);
                } else {
                    set_check(exported, CheckStatus::Pass, Evidence::Strong,
                              "JPEG (sRGB) and 16-bit TIFF (Display P3) decode independently to the "
                              "quantised image "
                              "with exact ICC profiles and the written metadata");
                }
                exported.seconds += seconds_since(te);
                result.measurements.phase_seconds["export"] = seconds_since(te);
            }

            // preview for the reviewer's visual check
            if (options.previews_dir) {
                const std::int32_t long_edge = std::max(actual.width, actual.height);
                const double scale =
                    std::min(1.0, static_cast<double>(options.preview_long_edge) / long_edge);
                const image::Size size =
                    engine::proxy_size(actual, static_cast<std::int32_t>(std::lround(long_edge * scale)));
                auto small = image::ImageBuffer<float>::create(size.width, size.height, 3);
                if (small && engine::box_downscale(display.pixels.const_view(), small->view())) {
                    std::error_code ec;
                    std::filesystem::create_directories(*options.previews_dir, ec);
                    const std::filesystem::path preview = *options.previews_dir / (fixture.id + ".jpg");
                    std::filesystem::remove(preview, ec);
                    (void)exporter::export_image(small->const_view(), display.encoding, preview,
                                                 exporter::ExportSettings::jpeg_srgb_q95());
                }
            }
        }
    }  // develop render 1 released

    // ---- determinism: a second develop render ----------------------------------------------------------
    {
        CheckResult& c = check_of(result, kCheckDeterminism);
        const Clock::time_point t0 = Clock::now();
        if (display_hash.empty()) {
            set_check(c, CheckStatus::NotRun, Evidence::None, "the first develop render failed");
        } else {
            auto developed =
                engine::render_develop_file(*registry, file, 0u, engine::DevelopSettings{}, {}, {});
            if (!developed) {
                set_check(c, CheckStatus::Fail, Evidence::Strong,
                          "second develop render failed: " + describe(developed.error()));
            } else {
                const std::string second = pixel_hash(developed->display);
                c.details["display_sha256"] = display_hash;
                c.details["second_display_sha256"] = second;
                std::string second_jpeg;
                if (!jpeg_sha256.empty()) {
                    const ExportOutcome again = export_and_read_back(
                        developed->display, work / "determinism.jpg",
                        exporter::ExportSettings::jpeg_srgb_q95(), export_metadata, capture);
                    second_jpeg = again.sha256;
                    c.details["jpeg_sha256"] = jpeg_sha256;
                    c.details["second_jpeg_sha256"] = second_jpeg;
                }
                if (second != display_hash) {
                    set_check(c, CheckStatus::Fail, Evidence::Strong, "the second render differs");
                } else if (!jpeg_sha256.empty() && second_jpeg != jpeg_sha256) {
                    set_check(c, CheckStatus::Fail, Evidence::Strong, "the second JPEG differs");
                } else {
                    set_check(c, CheckStatus::Pass, Evidence::Strong,
                              "two develop renders are bit-identical and their JPEGs byte-identical");
                }
            }
        }
        c.seconds += seconds_since(t0);
        result.measurements.phase_seconds["develop_2"] = seconds_since(t0);
    }
    if (!options.keep_exports) {
        std::error_code ec;
        std::filesystem::remove_all(work, ec);
    }
    return finish();
}

// ---- State rules ------------------------------------------------------------------------------------

namespace {

void decide_state(FixtureResult& result, const FixtureExpectation* expectation);

void compare_with_expectation(FixtureResult& result, const FixtureExpectation* expectation) {
    result.expectation_mismatches.clear();
    if (expectation == nullptr) {
        return;
    }
    if (std::string(to_string(result.state)) != expectation->expected_state) {
        result.expectation_mismatches.push_back("state " + std::string(to_string(result.state)) +
                                                ", expected " + expectation->expected_state);
    }
    for (const CheckResult& c : result.checks) {
        const auto it = expectation->expected_checks.find(c.id);
        const std::string actual(to_string(c.status));
        if (it != expectation->expected_checks.end() && it->second != actual) {
            result.expectation_mismatches.push_back("check " + c.id + " " + actual + ", expected " +
                                                    it->second);
        }
    }
    std::vector<std::string> actual_limits;
    for (const Limitation& l : result.limitations) {
        actual_limits.push_back(l.id);
    }
    std::sort(actual_limits.begin(), actual_limits.end());
    if (actual_limits != expectation->expected_limitations) {
        result.expectation_mismatches.push_back("limitations {" + join(actual_limits, ", ") +
                                                "}, expected {" +
                                                join(expectation->expected_limitations, ", ") + "}");
    }
    if (result.mode_key && result.mode_key->to_string() != expectation->mode_key) {
        result.expectation_mismatches.push_back("mode key " + result.mode_key->to_string() + ", expected " +
                                                expectation->mode_key);
    }
    if (expectation->image_area_source && result.camera_area.is_object() &&
        result.camera_area.value("source", std::string()) != *expectation->image_area_source) {
        result.expectation_mismatches.push_back("image area source " +
                                                result.camera_area.value("source", std::string()) +
                                                ", expected " + *expectation->image_area_source);
    }
    // QUA-07: where the area was placed, not only its size (a rendered fixture only).
    if (expectation->image_area_applied_asserted && result.camera_area.is_object() &&
        result.camera_area.contains("output") && result.camera_area.at("output").is_array()) {
        const OrderedJson& applied = result.camera_area.contains("applied") ? result.camera_area.at("applied")
                                                                          : OrderedJson(nullptr);
        std::optional<std::array<std::int32_t, 4>> actual;
        if (applied.is_object()) {
            actual = std::array<std::int32_t, 4>{applied.value("x", 0), applied.value("y", 0),
                                                 applied.value("width", 0), applied.value("height", 0)};
        }
        if (actual != expectation->image_area_applied) {
            const auto text = [](const std::optional<std::array<std::int32_t, 4>>& r) {
                return r ? std::format("{}x{}+{}+{}", (*r)[2], (*r)[3], (*r)[0], (*r)[1]) : std::string("the whole image");
            };
            result.expectation_mismatches.push_back("camera area applied at " + text(actual) + ", expected " +
                                                    text(expectation->image_area_applied));
        }
    }
}

}  // namespace

void propose_state(FixtureResult& result, const FixtureExpectation* expectation) {
    if (expectation != nullptr) {
        result.product = expectation->product;
        result.family = expectation->family;
        result.mode_label = expectation->mode_label;
    }
    decide_state(result, expectation);
    compare_with_expectation(result, expectation);
}

namespace {

void decide_state(FixtureResult& result, const FixtureExpectation* expectation) {
    result.state_reasons.clear();
    result.evidence_gaps.clear();
    result.expected_state =
        expectation ? std::optional<std::string>(expectation->expected_state) : std::nullopt;
    const CheckResult* integrity = result.check(kCheckFixtureIntegrity);
    if (!result.harness_error.empty() || integrity == nullptr || integrity->status != CheckStatus::Pass) {
        result.state = SupportState::NotQualified;
        result.state_reasons.push_back(result.harness_error.empty() ? "fixture integrity not established"
                                                                    : result.harness_error);
        return;
    }
    // Evidence gaps are listed for every state (they say what Verified would need).
    if (expectation == nullptr) {
        result.evidence_gaps.push_back("no qualification expectation for this fixture");
    } else {
        const auto fact_gap = [&](const Fact& fact, const std::string& what) {
            if (!fact.verified() && fact.status != "not_applicable") {
                result.evidence_gaps.push_back(what + " not independently verified" +
                                               (fact.note.empty() ? std::string() : " (" + fact.note + ")"));
            }
        };
        fact_gap(expectation->identity, "camera identity");
        fact_gap(expectation->compression, "compression mode");
        fact_gap(expectation->camera_image_area_fact, "camera image area");
        fact_gap(expectation->orientation, "orientation");
        fact_gap(expectation->representation_fact, "representation / CFA");
    }
    for (const CheckResult& c : result.checks) {
        if (c.status == CheckStatus::Pass && c.evidence != Evidence::Strong) {
            result.evidence_gaps.push_back(c.id + ": evidence " + std::string(to_string(c.evidence)) + " (" +
                                           c.summary + ")");
        } else if (c.status == CheckStatus::NotRun && result.decoded) {
            result.evidence_gaps.push_back(c.id + ": not run (" + c.summary + ")");
        }
    }

    if (!result.decoded) {
        result.state = SupportState::Unsupported;
        result.state_reasons.push_back("the decoder cannot decode this mode: " + result.decode_error);
        return;
    }
    for (const CheckResult& c : result.checks) {
        if (c.status == CheckStatus::Fail) {
            result.state_reasons.push_back(c.id + " failed: " + c.summary);
        }
    }
    if (!result.state_reasons.empty()) {
        result.state = SupportState::Unsupported;
        return;
    }
    if (!result.limitations.empty()) {
        result.state = SupportState::Limited;
        for (const Limitation& l : result.limitations) {
            result.state_reasons.push_back(l.id + ": " + l.detail);
        }
        return;
    }
    if (result.evidence_gaps.empty()) {
        result.state = SupportState::Verified;
        result.state_reasons.push_back(
            "every check passed with strong evidence on the exact mode key, and every "
            "expected fact is independently verified");
    } else {
        result.state = SupportState::CompatibleUnverified;
        result.state_reasons.push_back(
            "decoding and interpretation succeed, but the evidence is insufficient for "
            "Verified (see evidence gaps)");
    }
}

}  // namespace

// ---- Serialisation ----------------------------------------------------------------------------------

OrderedJson FixtureResult::to_json() const {
    OrderedJson j;
    j["id"] = id;
    j["tier"] = tier;
    j["file"] = {{"name", file_name}, {"size_bytes", file_size}, {"sha256", file_sha256}};
    j["camera"] = {{"make", camera_make}, {"model", camera_model}, {"firmware", firmware}, {"lens", lens}};
    j["product"] = product;
    j["family"] = family;
    j["mode_label"] = mode_label;
    if (mode_key) {
        const ModeKey& k = *mode_key;
        j["mode_key"] = {{"text", k.to_string()},
                         {"version", k.version},
                         {"model_key", k.model_key()},
                         {"make", k.make},
                         {"model", k.model},
                         {"container", k.container},
                         {"compression", k.compression},
                         {"lossiness", k.compression_lossiness},
                         {"area", k.area_text()},
                         {"bits", k.bits},
                         {"bits_provenance", k.bits_provenance},
                         {"representation", k.representation},
                         {"sample", k.sample},
                         {"view", k.view},
                         {"subimage_role", k.subimage_role},
                         {"subimage_index", k.subimage_index},
                         {"subimage_count", k.subimage_count},
                         {"stage", k.stage}};
    } else {
        j["mode_key"] = nullptr;
    }
    j["lookup_key"] = lookup_key ? OrderedJson(*lookup_key) : OrderedJson(nullptr);
    j["lookup_variant"] = lookup_variant;
    j["decoder"] = decoder;
    j["decoded"] = decoded;
    j["decode_error"] = decode_error.empty() ? OrderedJson(nullptr) : OrderedJson(decode_error);
    j["interpretation"] = {{"status", interpretation_status}, {"issues", interpretation_issues}};
    OrderedJson checks_json = OrderedJson::array();
    for (const CheckResult& c : checks) {
        checks_json.push_back({{"id", c.id},
                               {"status", to_string(c.status)},
                               {"evidence", to_string(c.evidence)},
                               {"summary", c.summary},
                               {"seconds", c.seconds},
                               {"details", c.details}});
    }
    j["checks"] = std::move(checks_json);
    OrderedJson limits = OrderedJson::array();
    for (const Limitation& l : limitations) {
        limits.push_back({{"id", l.id}, {"detail", l.detail}});
    }
    j["limitations"] = std::move(limits);
    j["camera_area"] = camera_area;
    j["proposed_state"] = to_string(state);
    j["expected_state"] = expected_state ? OrderedJson(*expected_state) : OrderedJson(nullptr);
    j["state_reasons"] = state_reasons;
    j["evidence_gaps"] = evidence_gaps;
    j["expectation_mismatches"] = expectation_mismatches;
    j["measurements"] = {{"total_seconds", measurements.total_seconds},
                         {"phase_seconds", measurements.phase_seconds},
                         {"peak_working_set_bytes", measurements.peak_working_set_bytes},
                         {"peak_commit_bytes", measurements.peak_commit_bytes},
                         {"w2_render_estimate_bytes", measurements.w2_estimate_bytes},
                         {"develop_export_estimate_bytes", measurements.develop_estimate_bytes}};
    j["harness_error"] = harness_error.empty() ? OrderedJson(nullptr) : OrderedJson(harness_error);
    return j;
}

}  // namespace lucent::qualify
