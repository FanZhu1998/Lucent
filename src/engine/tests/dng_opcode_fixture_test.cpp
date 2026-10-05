// DNG opcode processing on the camera fixtures (ADR 0014). The drone DNGs of the corpus carry
// mandatory opcodes:
//   dji-fc4382-dng          OpcodeList3: WarpRectilinear (3 equal coefficient sets)
//   dji-fc7303-dng          OpcodeList3: FixVignetteRadial, WarpRectilinear, GainMap (3 planes)
//   dji-fc220-dng           OpcodeList1: FixBadPixelsList (2169 points); OpcodeList3: FixVignetteRadial
//   hasselblad-l2d-20c-dng  OpcodeList3: GainMap (3 planes), WarpRectilinear (per-plane sets)
// For each cached one, stage one (export kind, as shot, full resolution) is rendered with the
// opcodes and with the opcodes removed from the metadata. Checks:
//   * every opcode is applied, at its stage, and the interpretation is complete;
//   * vignetting: away from the warps, the ratio of the two renders is the FixVignetteRadial gain
//     of the specification's formula at that pixel, evaluated here independently;
//   * geometry: patches of the warped render are found in the unwarped render, by a sub-pixel block
//     search, at the displacement the specification's WarpRectilinear formula gives (evaluated
//     here independently), within 1 pixel (the block match averages over the patch), and they
//     match an independent bilinear rendering of that formula without a shift (within 0.1 pixel);
//   * the geometry record maps output points like that formula;
//   * determinism: a second render is bit-identical.
// With LUCENT_OPCODE_DUMP_DIR set, both renders are also written there as float32 NPY files of
// camera RGB (white-balanced, stored orientation), with a JSON sidecar, for the rawpy comparison of
// tests/tools/dng_opcode_reference.py and for visual inspection.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/engine/develop.hpp>
#include <lucent/engine/dng_opcode_processing.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "develop_test_support.hpp"
#include "sensor_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
using raw::DngOpcodeListId;

std::optional<std::filesystem::path> dump_dir() {
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "LUCENT_OPCODE_DUMP_DIR") != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::filesystem::path dir(value);
    std::free(value);
    if (dir.empty()) {
        return std::nullopt;
    }
    std::filesystem::create_directories(dir);
    return dir;
}

// float32 (H, W, C) NPY, version 1.0.
void write_npy(const std::filesystem::path& path, const image::ImageBuffer<float>& img) {
    std::string header = "{'descr': '<f4', 'fortran_order': False, 'shape': (" + std::to_string(img.height()) + ", " +
                         std::to_string(img.width()) + ", " + std::to_string(img.channels()) + "), }";
    const std::size_t unpadded = 10 + header.size() + 1;
    header.append((64 - unpadded % 64) % 64, ' ');
    header.push_back('\n');
    std::ofstream out(path, std::ios::binary);
    out.write("\x93NUMPY\x01\x00", 8);
    const auto length = static_cast<std::uint16_t>(header.size());
    out.put(static_cast<char>(length & 0xFF));
    out.put(static_cast<char>(length >> 8));
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    for (std::int32_t y = 0; y < img.height(); ++y) {
        out.write(reinterpret_cast<const char*>(img.row(y)),
                  static_cast<std::streamsize>(sizeof(float) * img.width() * img.channels()));
    }
}

Expected<raw::DecodedRaw> decode(const std::filesystem::path& file) {
    auto registry = raw::create_default_registry();
    if (!registry) {
        return Unexpected(registry.error());
    }
    return registry->decode(file, raw::DecodeOptions{}, {});
}

// The decoded image with every opcode removed (as if the file had none).
raw::DecodedRaw without_opcodes(raw::DecodedRaw decoded) {
    raw::RawMetadata& m = decoded.metadata;
    if (m.dng) {
        m.dng->raw_ifd.opcodes.clear();
        m.dng->raw_ifd.opcode_lists = {};
    }
    std::erase_if(m.required_operations,
                  [](const raw::RequiredOperation& op) { return op.kind == raw::RequiredOperationKind::DngOpcode; });
    m.interpretation.issues.clear();
    m.interpretation.status = raw::derive_interpretation_status(m.interpretation.issues);
    return decoded;
}

StageOneRequest export_request() {
    StageOneRequest request;
    request.kind = StageOneKind::Export;
    return request;
}

// The specification's WarpRectilinear mapping (DNG 1.7.1.0 chapter 7), written out here: the
// input position shown at output pixel (x, y) of a W x H image, for coefficient set k.
std::array<double, 2> spec_warp(double x, double y, double w, double h, const raw::DngWarpPlane& k, double chx,
                                double chy) {
    const double cx = chx * (w - 1.0);
    const double cy = chy * (h - 1.0);
    const double mx = std::max(cx, w - 1.0 - cx);
    const double my = std::max(cy, h - 1.0 - cy);
    const double m = std::hypot(mx, my);
    const double dx = (x - cx) / m;
    const double dy = (y - cy) / m;
    const double r2 = dx * dx + dy * dy;
    const double f = k.radial[0] + k.radial[2] * r2 + k.radial[4] * r2 * r2 + k.radial[6] * r2 * r2 * r2;
    const double xt = k.tangential[0] * 2 * dx * dy + k.tangential[1] * (r2 + 2 * dx * dx);
    const double yt = k.tangential[1] * 2 * dx * dy + k.tangential[0] * (r2 + 2 * dy * dy);
    return {cx + m * (f * dx + xt), cy + m * (f * dy + yt)};
}

// 1 - normalised cross-correlation of the green channel between the n x n patch of `a` at
// (ax, ay) and the patch of `b` at (bx, by): 0 for patches equal up to gain and offset (the
// vignette and gain-map opcodes change intensities slowly). Every pixel must be inside both.
double patch_distance(const image::ImageBuffer<float>& a, std::int32_t ax, std::int32_t ay,
                      const image::ImageBuffer<float>& b, std::int32_t bx, std::int32_t by, std::int32_t n) {
    double sa = 0.0, sb = 0.0, saa = 0.0, sbb = 0.0, sab = 0.0;
    for (std::int32_t j = 0; j < n; ++j) {
        const float* ra = a.row(ay + j);
        const float* rb = b.row(by + j);
        for (std::int32_t i = 0; i < n; ++i) {
            const double va = ra[(ax + i) * 3 + 1];
            const double vb = rb[(bx + i) * 3 + 1];
            sa += va;
            sb += vb;
            saa += va * va;
            sbb += vb * vb;
            sab += va * vb;
        }
    }
    const double count = static_cast<double>(n) * n;
    const double cov = sab - sa * sb / count;
    const double va = saa - sa * sa / count;
    const double vb = sbb - sb * sb / count;
    if (!(va > 0.0) || !(vb > 0.0)) {
        return 1.0;
    }
    return 1.0 - cov / std::sqrt(va * vb);
}

double patch_variance(const image::ImageBuffer<float>& a, std::int32_t x0, std::int32_t y0, std::int32_t n) {
    double s = 0.0;
    double s2 = 0.0;
    for (std::int32_t j = 0; j < n; ++j) {
        for (std::int32_t i = 0; i < n; ++i) {
            const double v = a.at(x0 + i, y0 + j, 1);
            s += v;
            s2 += v * v;
        }
    }
    const double count = static_cast<double>(n) * n;
    const double mean = s / count;
    return s2 / count - mean * mean;
}

// Finds where the n x n patch of `warped` at (x0, y0) lies in `plain`, searching integer shifts
// within +-radius of `guess` and refining with a parabola through the distance minimum in each
// axis; std::nullopt when the minimum lies on the border of the search window.
std::optional<std::array<double, 2>> locate(const image::ImageBuffer<float>& warped, std::int32_t x0, std::int32_t y0,
                                            const image::ImageBuffer<float>& plain, std::array<double, 2> guess,
                                            std::int32_t n, std::int32_t radius) {
    const std::int32_t gx = static_cast<std::int32_t>(std::lround(guess[0]));
    const std::int32_t gy = static_cast<std::int32_t>(std::lround(guess[1]));
    double best = std::numeric_limits<double>::infinity();
    std::int32_t bx = 0;
    std::int32_t by = 0;
    const auto inside = [&](std::int32_t x, std::int32_t y) {
        return x >= 0 && y >= 0 && x + n <= plain.width() && y + n <= plain.height();
    };
    for (std::int32_t dy = -radius; dy <= radius; ++dy) {
        for (std::int32_t dx = -radius; dx <= radius; ++dx) {
            const std::int32_t x = x0 + gx + dx;
            const std::int32_t y = y0 + gy + dy;
            if (!inside(x - 1, y - 1) || !inside(x + 1, y + 1)) {
                continue;
            }
            const double distance = patch_distance(warped, x0, y0, plain, x, y, n);
            if (distance < best) {
                best = distance;
                bx = gx + dx;
                by = gy + dy;
            }
        }
    }
    if (!std::isfinite(best) || std::abs(bx - gx) == radius || std::abs(by - gy) == radius) {
        return std::nullopt;  // no interior minimum
    }
    const auto parabola = [](double m, double c, double p) {
        const double denominator = m - 2.0 * c + p;
        return denominator > 0.0 ? 0.5 * (m - p) / denominator : 0.0;
    };
    const double ox = parabola(patch_distance(warped, x0, y0, plain, x0 + bx - 1, y0 + by, n), best,
                               patch_distance(warped, x0, y0, plain, x0 + bx + 1, y0 + by, n));
    const double oy = parabola(patch_distance(warped, x0, y0, plain, x0 + bx, y0 + by - 1, n), best,
                               patch_distance(warped, x0, y0, plain, x0 + bx, y0 + by + 1, n));
    return std::array<double, 2>{bx + ox, by + oy};
}

// The green channel of `plain` resampled bilinearly at the specification's mapping (spec_warp)
// for the output pixels [x0, x0 + size) x [y0, y0 + size): an independent rendering of the warp.
image::ImageBuffer<float> spec_warped_region(const image::ImageBuffer<float>& plain, std::int32_t x0, std::int32_t y0,
                                             std::int32_t size, const raw::DngWarpPlane& k, double chx, double chy) {
    auto region = image::ImageBuffer<float>::create(size, size, 3);
    const double w = plain.width();
    const double h = plain.height();
    const auto at = [&](std::int32_t x, std::int32_t y) {
        return static_cast<double>(plain.at(std::clamp(x, 0, plain.width() - 1), std::clamp(y, 0, plain.height() - 1), 1));
    };
    for (std::int32_t j = 0; j < size; ++j) {
        for (std::int32_t i = 0; i < size; ++i) {
            const auto s = spec_warp(x0 + i, y0 + j, w, h, k, chx, chy);
            const auto xi = static_cast<std::int32_t>(std::floor(s[0]));
            const auto yi = static_cast<std::int32_t>(std::floor(s[1]));
            const double fx = s[0] - xi;
            const double fy = s[1] - yi;
            const double top = at(xi, yi) + fx * (at(xi + 1, yi) - at(xi, yi));
            const double bottom = at(xi, yi + 1) + fx * (at(xi + 1, yi + 1) - at(xi, yi + 1));
            region->at(i, j, 1) = static_cast<float>(top + fy * (bottom - top));
        }
    }
    return std::move(*region);
}

struct Fixture {
    const char* id;
    std::vector<std::string> opcodes;  // "<list>:<name>" in execution order
};

const std::vector<Fixture>& fixtures() {
    static const std::vector<Fixture> list = {
        {"dji-fc4382-dng", {"3:WarpRectilinear"}},
        {"dji-fc7303-dng", {"3:FixVignetteRadial", "3:WarpRectilinear", "3:GainMap"}},
        {"dji-fc220-dng", {"1:FixBadPixelsList", "3:FixVignetteRadial"}},
        {"hasselblad-l2d-20c-dng", {"3:GainMap", "3:WarpRectilinear"}},
    };
    return list;
}

TEST(DngOpcodeFixtures, DroneDngsRenderWithTheirOpcodes) {
    const auto dump = dump_dir();
    std::size_t checked = 0;
    for (const Fixture& fixture : fixtures()) {
        const auto file = ts::fixture_file(fixture.id);
        if (!file) {
            continue;
        }
        SCOPED_TRACE(fixture.id);
        auto decoded = decode(*file);
        ASSERT_TRUE(decoded) << decoded.error().describe();
        ASSERT_EQ(decoded->metadata.interpretation.status, raw::InterpretationStatus::Complete);
        const raw::RawMetadata metadata = decoded->metadata;
        auto second = decode(*file);
        ASSERT_TRUE(second) << second.error().describe();
        raw::DecodedRaw plain_input = without_opcodes(std::move(*second));

        auto with = render_stage_one(std::move(*decoded), export_request());
        ASSERT_TRUE(with) << with.error().describe();
        auto plain = render_stage_one(std::move(plain_input), export_request());
        ASSERT_TRUE(plain) << plain.error().describe();
        ASSERT_EQ(with->pixels.size(), plain->pixels.size());
        ASSERT_EQ(with->orientation, image::Orientation::Normal);

        // Every opcode applied, in order and at its stage.
        std::vector<std::string> applied;
        for (const DngOpcodeRecord& r : with->dng_opcodes.opcodes) {
            EXPECT_EQ(r.outcome, OpcodeOutcome::Applied) << r.name << ": " << r.detail;
            applied.push_back(std::to_string(static_cast<int>(r.list)) + ":" + r.name);
            std::cout << "[ opcode   ] " << fixture.id << " OpcodeList" << static_cast<int>(r.list) << "[" << r.index
                      << "] " << r.name << " " << to_string(r.outcome) << " in " << r.seconds << " s: " << r.detail
                      << " (samples " << r.affected_samples << ", outside " << r.outside_samples << ", limited "
                      << r.limited_samples << ")\n";
        }
        EXPECT_EQ(applied, fixture.opcodes);
        std::vector<RenderStage> stages;
        for (const StageRecord& s : with->stages) {
            stages.push_back(s.stage);
        }
        const auto position = [&](RenderStage stage) {
            return std::find(stages.begin(), stages.end(), stage) - stages.begin();
        };
        EXPECT_LT(position(RenderStage::Demosaic), position(RenderStage::DngOpcodeList3));
        EXPECT_LT(position(RenderStage::DngOpcodeList3), position(RenderStage::Orientation));
        if (metadata.dng->raw_ifd.opcode_lists[0]) {
            EXPECT_LT(position(RenderStage::DngOpcodeList1), position(RenderStage::Normalize));
        }
        const CarriedLensCorrections carried = carried_lens_corrections(metadata);
        std::cout << "[ carried  ] " << fixture.id << ": distortion " << carried.distortion << ", lateral CA "
                  << carried.lateral_chromatic_aberration << ", vignetting " << carried.vignetting << "\n";

        // Vignetting alone (dji-fc220: no warp): the ratio is the specification's gain.
        const raw::DngOpcode* vignette = nullptr;
        const raw::DngOpcode* warp = nullptr;
        for (const raw::DngOpcode& op : metadata.dng->raw_ifd.opcodes) {
            if (op.id == 3) {
                vignette = &op;
            }
            if (op.id == 1) {
                warp = &op;
            }
        }
        const double w = with->pixels.width();
        const double h = with->pixels.height();
        if (vignette != nullptr && warp == nullptr) {
            const auto& v = std::get<raw::DngFixVignetteRadial>(vignette->parameters);
            const double cx = v.center_x * (w - 1);
            const double cy = v.center_y * (h - 1);
            const double m2 = std::pow(std::max(cx, w - 1 - cx), 2) + std::pow(std::max(cy, h - 1 - cy), 2);
            // Pixels within 3 of a listed bad pixel differ by the List 1 repair; skip them.
            std::vector<std::uint8_t> repaired(static_cast<std::size_t>(w * h), 0);
            for (const raw::DngOpcode& op : metadata.dng->raw_ifd.opcodes) {
                if (const auto* list = std::get_if<raw::DngFixBadPixelsList>(&op.parameters)) {
                    for (const raw::DngBadPoint& p : list->points) {
                        for (std::int64_t yy = std::int64_t{p.row} - 3; yy <= std::int64_t{p.row} + 3; ++yy) {
                            for (std::int64_t xx = std::int64_t{p.column} - 3; xx <= std::int64_t{p.column} + 3; ++xx) {
                                if (xx >= 0 && yy >= 0 && xx < w && yy < h) {
                                    repaired[static_cast<std::size_t>(yy * static_cast<std::int64_t>(w) + xx)] = 1;
                                }
                            }
                        }
                    }
                }
            }
            std::size_t samples = 0;
            double worst = 0.0;
            for (std::int32_t y = 7; y < with->pixels.height(); y += 97) {
                for (std::int32_t x = 5; x < with->pixels.width(); x += 89) {
                    if (repaired[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]) {
                        continue;
                    }
                    const double r2 = ((x - cx) * (x - cx) + (y - cy) * (y - cy)) / m2;
                    const double gain =
                        1 + r2 * (v.k[0] + r2 * (v.k[1] + r2 * (v.k[2] + r2 * (v.k[3] + r2 * v.k[4]))));
                    for (std::int32_t c = 0; c < 3; ++c) {
                        const double before = plain->pixels.at(x, y, c);
                        if (before < 1e-3) {
                            continue;
                        }
                        worst = std::max(worst, std::abs(with->pixels.at(x, y, c) / before - gain) / gain);
                        ++samples;
                    }
                }
            }
            std::cout << "[ vignette ] " << fixture.id << ": " << samples << " samples, worst relative gain error "
                      << worst << "\n";
            EXPECT_GT(samples, 1000u);
            // Bad-pixel repair (List 1) changes a few neighbourhoods; everything else is exact.
            EXPECT_LT(worst, 2e-6);
        }

        // Geometry: patches of the warped render sit where the specification's formula puts them.
        if (warp != nullptr) {
            const auto& params = std::get<raw::DngWarpRectilinear>(warp->parameters);
            const raw::DngWarpPlane& green = params.planes[std::min<std::size_t>(1, params.planes.size() - 1)];
            // The geometry record agrees with the formula.
            const GeometryChange* change = nullptr;
            for (const GeometryChange& g : with->dng_opcodes.geometry) {
                if (g.kind == GeometryChangeKind::Warp) {
                    change = &g;
                }
            }
            ASSERT_NE(change, nullptr);
            for (const auto& [px, py] : std::vector<std::array<double, 2>>{{0, 0}, {w - 1, h - 1}, {w / 3, h / 5}}) {
                const auto s = spec_warp(px, py, w, h, green, params.center_x, params.center_y);
                const image::PointD mapped = change->source_of({px + 0.5, py + 0.5}, 1);
                EXPECT_NEAR(mapped.x - 0.5, s[0], 1e-9);
                EXPECT_NEAR(mapped.y - 0.5, s[1], 1e-9);
            }
            // Block search (normalised cross-correlation) at textured 32 x 32 patches on a 7 x 5
            // grid. A wrong mapping (inverted, mis-centred, mis-scaled) puts the match outside the
            // +-4 pixel window around the formula's displacement.
            constexpr std::int32_t n = 32;
            std::size_t located = 0;
            std::size_t compared = 0;
            double worst = 0.0;
            double worst_residual = 0.0;
            double largest = 0.0;
            for (std::int32_t gy = 0; gy < 5; ++gy) {
                for (std::int32_t gx = 0; gx < 7; ++gx) {
                    const std::int32_t x0 = static_cast<std::int32_t>((0.06 + 0.88 * gx / 6.0) * w) - n / 2;
                    const std::int32_t y0 = static_cast<std::int32_t>((0.06 + 0.88 * gy / 4.0) * h) - n / 2;
                    if (patch_variance(with->pixels, x0, y0, n) < 1e-5) {
                        continue;  // too flat to locate
                    }
                    const double cxp = x0 + (n - 1) / 2.0;
                    const double cyp = y0 + (n - 1) / 2.0;
                    const auto s = spec_warp(cxp, cyp, w, h, green, params.center_x, params.center_y);
                    const std::array<double, 2> expected{s[0] - cxp, s[1] - cyp};
                    const auto found = locate(with->pixels, x0, y0, plain->pixels, expected, n, 4);
                    if (!found) {
                        continue;
                    }
                    const double error = std::hypot((*found)[0] - expected[0], (*found)[1] - expected[1]);
                    worst = std::max(worst, error);
                    largest = std::max(largest, std::hypot(expected[0], expected[1]));
                    ++located;
                    // Against the independent bilinear rendering of the same mapping, the patch
                    // must not be shifted at all (a half-pixel convention or centre error would).
                    constexpr std::int32_t r = 2;
                    const image::ImageBuffer<float> reference = spec_warped_region(
                        plain->pixels, x0 - r - 1, y0 - r - 1, n + 2 * r + 2, green, params.center_x, params.center_y);
                    const std::array<double, 2> origin{static_cast<double>(r + 1 - x0), static_cast<double>(r + 1 - y0)};
                    const auto residual = locate(with->pixels, x0, y0, reference, origin, n, r);
                    if (residual) {
                        worst_residual =
                            std::max(worst_residual, std::hypot((*residual)[0] - origin[0], (*residual)[1] - origin[1]));
                        ++compared;
                    }
                }
            }
            std::cout << "[ geometry ] " << fixture.id << ": " << located
                      << " patches located in the unwarped render; largest displacement " << largest
                      << " px; worst deviation of the block match from the specification's mapping " << worst
                      << " px; worst residual shift against an independent bilinear rendering of the mapping "
                      << worst_residual << " px (" << compared << " patches)\n";
            EXPECT_GE(located, 10u);
            // The block match measures the mean displacement over a 32 x 32 patch, which differs
            // from the centre's where the warp magnifies (up to 0.06 per pixel at the Hasselblad's
            // corners); a wrong direction, centre or scale would not match within the window.
            EXPECT_LT(worst, 1.0);
            EXPECT_GE(compared, 10u);
            // Catmull-Rom against bilinear interpolation, and the parabola fit to the correlation
            // peak, leave a few hundredths of a pixel; a mapping error of a pixel or half a pixel
            // (wrong frame, plane or rounding) would not.
            EXPECT_LT(worst_residual, 0.1);
        }

        if (dump) {
            const std::filesystem::path base = *dump / fixture.id;
            write_npy(base.string() + "-with.npy", with->pixels);
            write_npy(base.string() + "-plain.npy", plain->pixels);
            std::ofstream side(base.string() + ".json");
            side << "{\"file\": \"" << file->generic_string() << "\", \"multipliers\": [" << with->sample_multipliers[0]
                 << ", " << with->sample_multipliers[1] << ", " << with->sample_multipliers[2]
                 << "], \"clip_level\": " << with->white_balance.clip_level << "}\n";
        }
        ++checked;
    }
    if (checked == 0) {
        GTEST_SKIP() << "no drone DNG fixture is downloaded; run tools\\fetch-fixtures.ps1 -Tier all";
    }
}

TEST(DngOpcodeFixtures, OpcodeProcessingIsDeterministic) {
    LUCENT_ENGINE_REQUIRE_FIXTURE(file, "dji-fc7303-dng");
    auto a = decode(file);
    auto b = decode(file);
    ASSERT_TRUE(a && b);
    auto one = render_stage_one(std::move(*a), export_request());
    DevelopOptions serial;
    serial.parallel = false;
    auto two = render_stage_one(std::move(*b), export_request(), serial);
    ASSERT_TRUE(one && two);
    EXPECT_TRUE(ts::bit_identical(one->pixels, two->pixels));
}

}  // namespace
}  // namespace lucent::engine
