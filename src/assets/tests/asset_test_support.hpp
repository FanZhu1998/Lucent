#pragma once

// Test helpers: temporary folders and asset folders written from bytes (the manifest computed with
// the module's own writer, as lucent-qualify does).

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/assets/asset_manifest.hpp>
#include <lucent/assets/camera_support.hpp>
#include <lucent/core/hash.hpp>

namespace lucent::assets::test {

// A fresh folder below %TEMP%, removed with its contents on destruction.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                (L"lucent-assets-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(counter.fetch_add(1)));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

inline void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
    ASSERT_TRUE(static_cast<bool>(out)) << path.string();
}

inline std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline core::SemanticVersion version(const char* text) {
    auto parsed = core::SemanticVersion::parse(text);
    EXPECT_TRUE(parsed.has_value()) << text;
    return parsed ? *parsed : core::SemanticVersion{};
}

struct AssetSpec {
    std::string id = "test-asset";
    std::string version = "1.2.3";
    std::string minimum_app_version = "0.1.0";
    AssetContent content{"lucent.test-content", 1};
    std::map<std::string, std::string> files{{"data.json", "{\"hello\": 1}\n"}};
};

// Writes the files of `spec` and their manifest into `folder`; returns the manifest's text.
inline std::string write_asset(const std::filesystem::path& folder, const AssetSpec& spec) {
    AssetManifest manifest;
    manifest.id = spec.id;
    manifest.version = version(spec.version.c_str());
    manifest.minimum_app_version = version(spec.minimum_app_version.c_str());
    manifest.content = spec.content;
    for (const auto& [path, bytes] : spec.files) {
        auto file = describe_asset_file(path, bytes);
        EXPECT_TRUE(file.has_value());
        if (file) {
            manifest.files.push_back(*file);
        }
        std::filesystem::path target = folder;
        for (const auto& part : std::filesystem::path(path)) {
            target /= part;
        }
        write_bytes(target, bytes);
    }
    auto text = write_asset_manifest(manifest);
    EXPECT_TRUE(text.has_value()) << (text ? std::string() : text.error().describe());
    const std::string written = text ? *text : std::string();
    write_bytes(folder / "manifest.json", written);
    return written;
}

inline core::Digest256 digest_of(const std::string& bytes) {
    auto digest = core::sha256(std::as_bytes(std::span(bytes.data(), bytes.size())));
    EXPECT_TRUE(digest.has_value());
    return digest ? *digest : core::Digest256{};
}

inline AssetRequest request_for(std::string id, const char* app_version = "0.1.0") {
    AssetRequest request;
    request.id = std::move(id);
    request.app_version = version(app_version);
    return request;
}

// A small valid camera support database: one Verified, one Limited and one Unsupported mode.
inline CameraSupportDatabase sample_database() {
    CameraSupportDatabase db;
    db.generated = "2026-10-08T03:13:58Z";
    db.date = "2026-10-08";
    db.tool = "lucent-qualify 0.1.0 (test)";
    db.report = "docs/qualification/qualification-report.json";
    db.basis = running_basis();
    db.basis.check_suite = 2;
    db.basis.state_rules = 1;
    db.limitations = {{"xtrans_baseline_demosaic", "X-Trans demosaicing is baseline quality",
                       "Fine detail can look softer."}};
    CameraSupportEntry verified;
    verified.match =
        "mk1|canon|eos-r5|cr3|canon-crx-lossless-raw|8191x5463|14bit|mosaic:RGGB|uint16|single-view|"
        "primary:0/1";
    verified.state = SupportState::Verified;
    verified.camera = "Canon EOS R5";
    verified.mode = "CR3 RAW";
    verified.family = "canon";
    verified.evidence = "13 of 13 checks strong";
    verified.fixtures = {"canon-eos-r5-cr3-raw"};
    CameraSupportEntry limited;
    limited.match =
        "mk1|fujifilm|x-t4|raf|fujifilm-raf-lossless-compressed|6246x4170|14bit|"
        "mosaic:GGRGGB/GGBGGR/BRGRBG/GGBGGR/GGRGGB/RBGBRG|uint16|single-view|primary:0/1";
    limited.state = SupportState::Limited;
    limited.camera = "Fujifilm X-T4";
    limited.mode = "RAF lossless";
    limited.family = "fujifilm";
    limited.limitations = {
        {"xtrans_baseline_demosaic", "demosaic route xtrans_baseline is baseline quality"}};
    limited.evidence = "13 of 13 checks strong";
    limited.fixtures = {"fujifilm-x-t4-raf-lossless"};
    CameraSupportEntry unsupported;
    unsupported.match =
        "mk1|nikon|z-8|tiff|nikon-nef-high-efficiency-he|8280x5520|14bit|mosaic:RGGB|uint16|"
        "single-view|primary:0/1";
    unsupported.state = SupportState::Unsupported;
    unsupported.camera = "Nikon Z 8";
    unsupported.mode = "NEF HE";
    unsupported.family = "nikon";
    unsupported.evidence = "3 of 14 checks strong; failed: interpretation";
    unsupported.evidence_gaps = {"compression mode not independently verified"};
    unsupported.fixtures = {"nikon-z-8-nef-he"};
    for (CameraSupportEntry* entry : {&verified, &limited, &unsupported}) {
        entry->qualified_key = entry->match;
    }
    verified.image_area = "decoder_inset_crop 8192x5464";
    limited.image_area = "decoder_inset_crop 6240x4160";
    unsupported.image_area = "decoder_inset_crop 8256x5504";
    db.entries = {verified, limited, unsupported};
    db.summary = db.count();
    return db;
}

}  // namespace lucent::assets::test
