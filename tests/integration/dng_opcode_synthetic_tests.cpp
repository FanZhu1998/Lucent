// End to end through a DNG file (ADR 0014): a synthetic uncompressed DNG with one opcode in each
// list is decoded by the LibRaw adapter (parameters decoded by the TIFF inspector, support
// assessed into the decoded-RAW contract) and rendered through the develop pipeline's stage one;
// the result must equal the same file without opcodes transformed exactly as the specification
// stages and units say. Written in both TIFF byte orders (opcode lists are big-endian in both).

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>

#include <gtest/gtest.h>

#include <lucent/engine/develop.hpp>
#include <lucent/engine/dng_opcode_processing.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "synthetic_dng.hpp"
#include "tiff_test_writer.hpp"

namespace lucent::integration {
namespace {

namespace ts = raw::test_support;

std::filesystem::path write_file(const std::string& name, const std::vector<std::byte>& bytes) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("lucent-opcode-synthetic-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / name;
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

Expected<engine::StageOneImage> stage_one(const std::filesystem::path& path) {
    auto registry = raw::create_default_registry();
    if (!registry) {
        return Unexpected(registry.error());
    }
    engine::StageOneRequest request;
    request.kind = engine::StageOneKind::Interactive;  // no highlight clip
    return engine::render_stage_one_file(*registry, path, 0u, request);
}

TEST(DngOpcodeSynthetic, EveryListRunsAtItsStageThroughTheWholePath) {
    for (const ts::Endian endian : {ts::Endian::Little, ts::Endian::Big}) {
        SCOPED_TRACE(endian == ts::Endian::Little ? "II" : "MM");
        ts::SyntheticDngSpec plain_spec;  // 64 x 48 RGGB, black 256, white 16383
        plain_spec.endian = endian;
        ts::SyntheticDngSpec spec = plain_spec;
        // List 1: + 66 stored units on every stored value (OpcodeList1 deltas are in stored units,
        // as the DNG SDK computes them).
        spec.opcode_list1 = ts::opcode_list(
            {{10, 0x01030000, 0, ts::row_column_parameters({0, 0, 48, 64}, std::vector<float>(48, 66.0f))}});
        // List 2: a uniform gain of 1.25 on the normalised values.
        spec.opcode_list2 = ts::opcode_list(
            {{9, 0x01030000, 0, ts::gain_map_parameters({0, 0, 48, 64}, 1, 1, 1.0, 1.0, 0.0, 0.0, 1, {1.25f})}});
        // List 3: green (plane 1) x 2 on the demosaiced image.
        spec.opcode_list3 = ts::opcode_list(
            {{13, 0x01030000, 0, ts::row_column_parameters({0, 0, 48, 64, 1, 1}, std::vector<float>(64, 2.0f))}});
        const std::string suffix = endian == ts::Endian::Little ? "ii" : "mm";
        auto with = stage_one(write_file("with-" + suffix + ".dng", ts::build_synthetic_dng(spec)));
        auto plain = stage_one(write_file("plain-" + suffix + ".dng", ts::build_synthetic_dng(plain_spec)));
        ASSERT_TRUE(with) << with.error().describe();
        ASSERT_TRUE(plain) << plain.error().describe();

        EXPECT_EQ(with->metadata.interpretation.status, raw::InterpretationStatus::Complete);
        ASSERT_EQ(with->dng_opcodes.opcodes.size(), 3u);
        for (const engine::DngOpcodeRecord& r : with->dng_opcodes.opcodes) {
            EXPECT_EQ(r.outcome, engine::OpcodeOutcome::Applied) << r.name;
        }
        ASSERT_EQ(with->pixels.size(), plain->pixels.size());
        // Stored + 66 is + 66 / (16383 - 256) after normalisation, times the white-balance
        // multiplier of the channel; then x 1.25; green x 2 after demosaicing. Malvar-He-Cutler
        // is linear and passes a constant per-channel offset through unchanged.
        const double offset = 66.0 / (16383.0 - 256.0);
        double worst = 0.0;
        for (std::int32_t y = 4; y < with->pixels.height() - 4; ++y) {
            for (std::int32_t x = 4; x < with->pixels.width() - 4; ++x) {
                for (std::int32_t c = 0; c < 3; ++c) {
                    const double m = with->sample_multipliers[static_cast<std::size_t>(c)];
                    const double expected =
                        (plain->pixels.at(x, y, c) + offset * m) * 1.25 * (c == 1 ? 2.0 : 1.0);
                    worst = std::max(worst, std::abs(with->pixels.at(x, y, c) - expected));
                }
            }
        }
        // Absolute, in normalised units (values 0.02 to 0.5): float rounding inside the demosaic,
        // whose correction terms cancel only up to a few units in the last place.
        EXPECT_LT(worst, 2e-6);
    }
}

TEST(DngOpcodeSynthetic, AMandatoryOpcodeLucentCannotApplyKeepsTheFileUnverified) {
    ts::SyntheticDngSpec spec;
    // WarpFisheye (not implemented) after a supported vignette.
    spec.opcode_list3 = ts::opcode_list({{3, 0x01030000, 0, ts::vignette_parameters({0.1, 0, 0, 0, 0})},
                                         {2, 0x01030000, 0, ts::filler(52)}});
    auto one = stage_one(write_file("fisheye.dng", ts::build_synthetic_dng(spec)));
    ASSERT_TRUE(one) << one.error().describe();
    EXPECT_EQ(one->metadata.interpretation.status, raw::InterpretationStatus::RequiresUnsupportedOperation);
    ASSERT_EQ(one->dng_opcodes.opcodes.size(), 2u);
    EXPECT_EQ(one->dng_opcodes.opcodes[0].outcome, engine::OpcodeOutcome::Applied);
    EXPECT_EQ(one->dng_opcodes.opcodes[1].outcome, engine::OpcodeOutcome::NotApplied);
    bool warned = false;
    for (const std::string& w : one->warnings) {
        warned = warned || w.find("WarpFisheye") != std::string::npos;
    }
    EXPECT_TRUE(warned) << "the stage-one warnings name the opcode Lucent cannot apply";
}

}  // namespace
}  // namespace lucent::integration
