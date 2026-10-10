#pragma once

// Helpers of the lens-correction tests (lens_correction.hpp, ADR 0023): a synthetic Lensfun
// database (data format 1) with known calibrations of every model Lucent implements, the bundled
// database read from the source tree, and lens identities for synthetic metadata.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/engine/lens_correction.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::engine::test_support {

// Cameras: "Lucent Test Camera" (mount "Lucent Test Mount", crop factor 1), "Lucent Test Camera
// APS-C" (1.5), "Lucent Test Four Thirds" (mount "Lucent Four Thirds", 2), and the fixed-lens
// "Lucent Test Compact" (an undeclared mount, 5.6). Lenses:
// - Lucent 24-70mm f/2.8 Test: ptlens at 24, 35 and 70 mm, poly3 TCA at 24 and 70 mm, vignetting at
//   24 and 70 mm for f/2.8 and f/8.
// - Lucent 24-70mm f/2.8 Test II: a second lens whose name holds the first's words (fuzzy cases).
// - Lucent 50mm f/1.8: a prime with poly5, linear TCA, vignetting for f/1.8 and f/16.
// - Other 50mm f/1.8: another maker's lens of the same name (ambiguity).
// - Lucent 18mm f/4 Wide: calibrated on the APS-C sensor (crop 1.5), a lens centre offset, poly3.
// - Lucent 12mm f/2: 4:3 aspect ratio, crop factor 2.
// - Test Compact fixed lens: the compact's, poly3 at 4.5 mm, vignetting for f/2.
inline constexpr const char* kSyntheticLensDatabase = R"XML(<lensdatabase version="1">
    <mount>
        <name>Lucent Test Mount</name>
    </mount>
    <mount>
        <name>Lucent Four Thirds</name>
    </mount>
    <camera>
        <maker>Lucent</maker>
        <model>Test Camera</model>
        <mount>Lucent Test Mount</mount>
        <cropfactor>1</cropfactor>
    </camera>
    <camera>
        <maker>Lucent</maker>
        <model>Test Camera APS-C</model>
        <mount>Lucent Test Mount</mount>
        <cropfactor>1.5</cropfactor>
    </camera>
    <camera>
        <maker>Lucent</maker>
        <model>Test Four Thirds</model>
        <mount>Lucent Four Thirds</mount>
        <cropfactor>2</cropfactor>
    </camera>
    <camera>
        <maker>Lucent</maker>
        <model>Test Compact</model>
        <mount>lucentTestCompact</mount>
        <cropfactor>5.6</cropfactor>
    </camera>
    <lens>
        <maker>Lucent</maker>
        <model>Lucent 24-70mm f/2.8 Test</model>
        <mount>Lucent Test Mount</mount>
        <cropfactor>1</cropfactor>
        <calibration>
            <distortion model="ptlens" focal="24" a="0.012" b="-0.041" c="0.018"/>
            <distortion model="ptlens" focal="35" a="0.004" b="-0.012" c="0.006"/>
            <distortion model="ptlens" focal="70" a="-0.002" b="0.011" c="-0.004"/>
            <tca model="poly3" focal="24" vr="1.00042" vb="0.99961" cr="0.00011" cb="-0.00009" br="0.00021" bb="-0.00018"/>
            <tca model="poly3" focal="70" vr="1.00018" vb="0.99977" br="0.00005" bb="-0.00004"/>
            <vignetting model="pa" focal="24" aperture="2.8" distance="1000" k1="-0.62" k2="0.18" k3="-0.07"/>
            <vignetting model="pa" focal="24" aperture="8" distance="1000" k1="-0.21" k2="0.03" k3="-0.01"/>
            <vignetting model="pa" focal="70" aperture="2.8" distance="1000" k1="-0.38" k2="0.09" k3="-0.03"/>
            <vignetting model="pa" focal="70" aperture="8" distance="1000" k1="-0.11" k2="0.02" k3="0"/>
        </calibration>
    </lens>
    <lens>
        <maker>Lucent</maker>
        <model>Lucent 24-70mm f/2.8 Test II</model>
        <mount>Lucent Test Mount</mount>
        <cropfactor>1</cropfactor>
        <calibration>
            <distortion model="ptlens" focal="24" a="0.01" b="-0.03" c="0.01"/>
            <distortion model="ptlens" focal="70" a="0" b="0.01" c="0"/>
        </calibration>
    </lens>
    <lens>
        <maker>Lucent</maker>
        <model>Lucent 50mm f/1.8</model>
        <mount>Lucent Test Mount</mount>
        <cropfactor>1</cropfactor>
        <calibration>
            <distortion model="poly5" focal="50" k1="-0.031" k2="0.006"/>
            <tca model="linear" focal="50" kr="1.0003" kb="0.9996"/>
            <vignetting model="pa" focal="50" aperture="1.8" distance="1000" k1="-0.45" k2="0.05" k3="-0.02"/>
            <vignetting model="pa" focal="50" aperture="16" distance="1000" k1="-0.08" k2="0.01" k3="0"/>
        </calibration>
    </lens>
    <lens>
        <maker>Other</maker>
        <model>Other 50mm f/1.8</model>
        <mount>Lucent Test Mount</mount>
        <cropfactor>1</cropfactor>
        <calibration>
            <distortion model="poly3" focal="50" k1="-0.01"/>
        </calibration>
    </lens>
    <lens>
        <maker>Lucent</maker>
        <model>Lucent 18mm f/4 Wide</model>
        <mount>Lucent Test Mount</mount>
        <cropfactor>1.5</cropfactor>
        <center x="0.012" y="-0.021"/>
        <calibration>
            <distortion model="poly3" focal="18" k1="-0.047"/>
            <tca model="linear" focal="18" kr="1.0006" kb="0.9993"/>
            <vignetting model="pa" focal="18" aperture="4" distance="1000" k1="-0.7" k2="0.2" k3="-0.05"/>
        </calibration>
    </lens>
    <lens>
        <maker>Lucent</maker>
        <model>Lucent 12mm f/2</model>
        <mount>Lucent Four Thirds</mount>
        <cropfactor>2</cropfactor>
        <aspect-ratio>4:3</aspect-ratio>
        <calibration>
            <distortion model="ptlens" focal="12" a="0.021" b="-0.056" c="0.019"/>
            <vignetting model="pa" focal="12" aperture="2" distance="1000" k1="-0.55" k2="0.12" k3="-0.04"/>
        </calibration>
    </lens>
    <lens>
        <maker>Lucent</maker>
        <model>Test Compact fixed lens</model>
        <mount>lucentTestCompact</mount>
        <cropfactor>5.6</cropfactor>
        <calibration>
            <distortion model="poly3" focal="4.5" k1="-0.025"/>
            <vignetting model="pa" focal="4.5" aperture="2" distance="1000" k1="-0.8" k2="0.3" k3="-0.1"/>
        </calibration>
    </lens>
</lensdatabase>
)XML";

inline std::shared_ptr<const LensDatabase> synthetic_lens_database() {
    static const std::shared_ptr<const LensDatabase> database = [] {
        const std::vector<LensDatabaseFile> files{{"synthetic.xml", kSyntheticLensDatabase}};
        auto loaded =
            LensDatabase::load(files, LensDatabaseIdentity{"lens-database-test", "0.0.1", "synthetic"});
        EXPECT_TRUE(loaded) << (loaded ? std::string{} : loaded.error().describe());
        return loaded ? *loaded : std::shared_ptr<const LensDatabase>{};
    }();
    return database;
}

inline std::filesystem::path bundled_lens_database_dir() {
    return std::filesystem::path(LUCENT_SOURCE_DIR) / "assets" / "lens-database";
}

struct LoadedLensDatabase {
    std::shared_ptr<const LensDatabase> database;
    double seconds = 0.0;  // reading and parsing
};

// The bundled database as the asset holds it (every *.xml of assets/lens-database, in name order,
// as the asset's sorted manifest loads them; the manifest's hashes are checked by lucent_assets'
// tests). Null when the folder is missing.
inline LoadedLensDatabase load_bundled_lens_database() {
    const auto start = std::chrono::steady_clock::now();
    const std::filesystem::path dir = bundled_lens_database_dir();
    if (!std::filesystem::is_directory(dir)) {
        return {};
    }
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".xml") {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end(),
              [](const auto& a, const auto& b) { return a.filename().string() < b.filename().string(); });
    std::vector<std::string> contents;  // the bytes the file views point into, during load()
    for (const auto& p : paths) {
        std::ifstream in(p, std::ios::binary);
        contents.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::vector<LensDatabaseFile> files;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        files.push_back(LensDatabaseFile{paths[i].filename().string(), contents[i]});
    }
    auto loaded =
        LensDatabase::load(files, LensDatabaseIdentity{"lens-database", "source-tree", "unverified"});
    EXPECT_TRUE(loaded) << (loaded ? std::string{} : loaded.error().describe());
    return LoadedLensDatabase{
        loaded ? *loaded : nullptr,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()};
}

inline std::shared_ptr<const LensDatabase> bundled_lens_database() {
    static const std::shared_ptr<const LensDatabase> database = load_bundled_lens_database().database;
    return database;
}

// Gives `m` a camera and lens identity and the photograph's focal length and f-number.
inline void set_lens_identity(raw::RawMetadata& m, std::string camera_make, std::string camera_model,
                              std::string lens_model, float focal_mm, std::optional<float> f_number,
                              std::optional<float> focal_35mm = std::nullopt, std::string lens_make = {}) {
    m.camera.make = camera_make;
    m.camera.normalized_make = std::move(camera_make);
    m.camera.model = camera_model;
    m.camera.normalized_model = std::move(camera_model);
    m.lens.make = std::move(lens_make);
    m.lens.model = std::move(lens_model);
    m.capture.focal_length_mm = focal_mm;
    m.capture.f_number = f_number;
    m.lens.focal_length_35mm_equivalent_mm = focal_35mm;
}

}  // namespace lucent::engine::test_support
