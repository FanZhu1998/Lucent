#pragma once

// Helpers shared by the application tests: synthetic DNG files (the raw module's test writers),
// junctions, a scripted SourceRenderer whose timing the tests control, and result collectors.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>
#include <winioctl.h>

#include <lucent/application/catalog_library.hpp>
#include <lucent/application/library.hpp>
#include <lucent/application/source_renderer.hpp>
#include <lucent/catalog/catalog.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/display_rendering.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "raw_test_support.hpp"
#include "synthetic_dng.hpp"
#include "test_jpegs.hpp"

namespace lucent::app::test_support {

namespace rts = lucent::raw::test_support;
using rts::ScratchDir;

inline constexpr auto kWaitTimeout = std::chrono::seconds(30);

// Writes a synthetic DNG (64 x 48 CFA by default) and returns its path.
inline std::filesystem::path write_dng(const ScratchDir& dir, const std::filesystem::path& name,
                                       const rts::SyntheticDngSpec& spec = {}) {
    return dir.write(name, rts::build_synthetic_dng(spec));
}

inline std::shared_ptr<const raw::DecoderRegistry> default_registry() {
    auto registry = raw::create_default_registry();
    EXPECT_TRUE(registry);
    return std::make_shared<const raw::DecoderRegistry>(std::move(*registry));
}

// Creates `link` as an NTFS junction (mount point) to the existing directory `target`. Junctions
// need no privileges, unlike directory symbolic links.
inline bool create_junction(const std::filesystem::path& link, const std::filesystem::path& target) {
    if (!::CreateDirectoryW(rts::verbatim(link).c_str(), nullptr)) {
        return false;
    }
    const std::wstring target_path = std::filesystem::absolute(target).wstring();
    const std::wstring substitute = L"\\??\\" + target_path;
    const std::wstring print = target_path;
    const std::size_t path_bytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
    // REPARSE_DATA_BUFFER (MountPointReparseBuffer) laid out by hand: it is declared in the DDK only.
    std::vector<std::byte> buffer(8 + 8 + path_bytes);
    const auto put16 = [&](std::size_t offset, std::size_t value) {
        const auto v = static_cast<std::uint16_t>(value);
        std::memcpy(buffer.data() + offset, &v, 2);
    };
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    std::memcpy(buffer.data(), &tag, 4);
    put16(4, 8 + path_bytes);                              // ReparseDataLength
    put16(8, 0);                                           // SubstituteNameOffset
    put16(10, substitute.size() * sizeof(wchar_t));        // SubstituteNameLength
    put16(12, (substitute.size() + 1) * sizeof(wchar_t));  // PrintNameOffset
    put16(14, print.size() * sizeof(wchar_t));             // PrintNameLength
    std::memcpy(buffer.data() + 16, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(buffer.data() + 16 + (substitute.size() + 1) * sizeof(wchar_t), print.c_str(),
                (print.size() + 1) * sizeof(wchar_t));
    HANDLE handle = ::CreateFileW(rts::verbatim(link).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD returned = 0;
    const BOOL ok = ::DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
                                      static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    ::CloseHandle(handle);
    return ok != FALSE;
}

// Removes a junction without touching its target (ScratchDir's recursive delete must not follow it).
inline void remove_junction(const std::filesystem::path& link) {
    ::RemoveDirectoryW(rts::verbatim(link).c_str());
}

// A gate that blocks until opened (or the token is cancelled).
class Gate {
public:
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }
    // True when opened, false when cancelled first.
    bool wait(const core::CancellationToken& cancel) {
        std::unique_lock lock(mutex_);
        while (!open_) {
            if (cancel.is_cancelled()) {
                return false;
            }
            changed_.wait_for(lock, std::chrono::milliseconds(2));
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
};

// Tests identify settings by a "revision" r encoded in DevelopSettings::exposure_ev as r / 100 EV
// (valid for r <= 500), so scripted renders can be controlled per settings.
inline engine::DevelopSettings settings_for(std::uint64_t revision) {
    engine::DevelopSettings settings;
    settings.exposure_ev = static_cast<double>(revision) / 100.0;
    return settings;
}

inline std::uint64_t revision_of(const engine::DevelopSettings& settings) {
    return static_cast<std::uint64_t>(std::llround(settings.exposure_ev * 100.0));
}

// Behaviour of ScriptedRenderer for one settings revision.
struct Script {
    std::shared_ptr<Gate> gate;       // block until opened (nullptr: do not block)
    bool honour_cancellation = true;  // false: finish even when cancelled while blocked
    std::optional<Error> error;       // fail with this error instead of rendering
    // Stage two only: the white-balance correction its report states (refine tests).
    std::optional<engine::RgbMultipliers> correction;
};

// A SourceRenderer whose renders produce a synthetic display image (display-referred linear
// Rec.2020, a horizontal ramp) of `size` (stage two: of the stage one's size), and whose timing
// tests control per settings revision (settings_for). Stage one produces a flat camera-RGB image
// of `size` or of the requested proxy size, gated by stage_one_gate().
class ScriptedRenderer final : public SourceRenderer {
public:
    explicit ScriptedRenderer(image::Size size = {400, 300}) : size_(size) {}

    void script(std::uint64_t revision, Script script) {
        std::lock_guard lock(mutex_);
        scripts_[revision] = std::move(script);
    }
    void stage_one_gate(std::shared_ptr<Gate> gate) {
        std::lock_guard lock(mutex_);
        stage_one_gate_ = std::move(gate);
    }
    // The stage_two_bytes estimate_stage_two() gives (unset: SourceRenderer's default estimate).
    void stage_two_estimate(std::uint64_t bytes) {
        std::lock_guard lock(mutex_);
        stage_two_estimate_ = bytes;
    }

    [[nodiscard]] int renders() const { return renders_.load(); }
    [[nodiscard]] int started() const { return started_.load(); }
    [[nodiscard]] int stage_one_builds() const { return stage_one_.load(); }
    [[nodiscard]] int stage_two_started() const { return stage_two_started_.load(); }
    // Revisions stage two rendered, in completion order.
    [[nodiscard]] std::vector<std::uint64_t> stage_two_revisions() const {
        std::lock_guard lock(mutex_);
        return stage_two_revisions_;
    }

    // Blocks until `count` renders (render() or stage two) have started.
    bool wait_started(int count) const {
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (started_.load() < count) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    Expected<engine::DevelopMemoryEstimate> estimate(const SourceItem&, const engine::StageOneRequest&,
                                                     const core::CancellationToken&) const override {
        engine::DevelopMemoryEstimate estimate;
        estimate.stage_one_peak = std::uint64_t{1} << 20;
        estimate.stage_one_bytes = std::uint64_t{1} << 20;
        estimate.stage_two_bytes = std::uint64_t{1} << 20;
        estimate.export_peak = std::uint64_t{1} << 20;
        return estimate;
    }

    Expected<engine::DevelopMemoryEstimate> estimate_stage_two(
        const engine::StageOneImage& stage_one, const engine::StageOneRequest& request,
        const engine::DevelopSettings& settings) const override {
        std::optional<std::uint64_t> bytes;
        {
            std::lock_guard lock(mutex_);
            bytes = stage_two_estimate_;
        }
        if (!bytes) {
            return SourceRenderer::estimate_stage_two(stage_one, request, settings);
        }
        engine::DevelopMemoryEstimate estimate;
        estimate.stage_two_bytes = *bytes;
        return estimate;
    }

    Expected<engine::StageOneImage> render_stage_one(const SourceItem&,
                                                     const engine::StageOneRequest& request,
                                                     const core::CancellationToken& cancel) const override {
        std::shared_ptr<Gate> gate;
        {
            std::lock_guard lock(mutex_);
            gate = stage_one_gate_;
        }
        if (gate && !gate->wait(cancel)) {
            return fail(ErrorCode::Cancelled, "scripted stage one cancelled", "test.render");
        }
        ++stage_one_;
        const image::Size size = engine::proxy_size(size_, request.proxy_long_edge);
        auto pixels = image::ImageBuffer<float>::create(size.width, size.height, 3);
        EXPECT_TRUE(pixels);
        engine::StageOneImage one;
        one.pixels = std::move(*pixels);
        one.encoding = image::ColorEncoding::camera_native_linear();
        one.kind = request.kind;
        one.full_size = size_;
        one.scale = static_cast<double>(std::max(size.width, size.height)) /
                    static_cast<double>(std::max(size_.width, size_.height));
        return one;
    }

    Expected<engine::DevelopResult> render_stage_two(const engine::StageOneImage& stage_one,
                                                     const engine::DevelopSettings& settings,
                                                     const core::CancellationToken& cancel) const override {
        ++stage_two_started_;
        const std::uint64_t revision = revision_of(settings);
        auto image = scripted(revision, stage_one.pixels.size(), cancel);
        if (!image) {
            return Unexpected(std::move(image.error()));
        }
        engine::DevelopResult result;
        result.display = std::move(*image);
        result.report.settings = settings;
        result.report.output_size = stage_one.pixels.size();
        result.report.full_size = stage_one.full_size;
        result.report.scale = stage_one.scale;
        result.report.demosaic.algorithm = "scripted";
        {
            std::lock_guard lock(mutex_);
            if (const auto it = scripts_.find(revision); it != scripts_.end() && it->second.correction) {
                result.report.correction = *it->second.correction;
            }
            stage_two_revisions_.push_back(revision);
        }
        return result;
    }

    Expected<DevelopRender> render(const SourceItem&, const engine::DevelopSettings& settings,
                                   const RenderProgress& progress,
                                   const core::CancellationToken& cancel) const override {
        auto image = scripted(revision_of(settings), size_, cancel);
        if (!image) {
            return Unexpected(std::move(image.error()));
        }
        DevelopRender result;
        result.display = std::move(*image);
        result.report.settings = settings;
        result.report.output_size = size_;
        result.report.full_size = size_;
        result.report.demosaic.algorithm = "scripted";
        if (progress) {
            progress(1.0);
        }
        return result;
    }

private:
    // Runs the script of `revision` and produces the ramp image of `size`.
    Expected<engine::ColorImage> scripted(std::uint64_t revision, image::Size size,
                                          const core::CancellationToken& cancel) const {
        ++started_;
        Script script;
        {
            std::lock_guard lock(mutex_);
            if (const auto it = scripts_.find(revision); it != scripts_.end()) {
                script = it->second;
            }
        }
        if (script.gate) {
            const bool opened = script.gate->wait(cancel);
            if (!opened && script.honour_cancellation) {
                return fail(ErrorCode::Cancelled, "scripted render cancelled", "test.render");
            }
            if (!opened) {
                // Ignore the cancellation: keep waiting for the gate, then finish.
                script.gate->wait(core::CancellationToken{});
            }
        }
        if (script.error) {
            return Unexpected(*script.error);
        }
        ++renders_;
        auto pixels = image::ImageBuffer<float>::create(size.width, size.height, 3);
        EXPECT_TRUE(pixels);
        for (std::int32_t y = 0; y < size.height; ++y) {
            float* row = pixels->row(y);
            for (std::int32_t x = 0; x < size.width; ++x) {
                const float v = static_cast<float>(x) / static_cast<float>(std::max(1, size.width - 1));
                row[3 * x] = v;
                row[3 * x + 1] = v;
                row[3 * x + 2] = v;
            }
        }
        engine::ColorImage image;
        image.pixels = std::move(*pixels);
        image.encoding = engine::display_linear_rec2020();
        return image;
    }

    image::Size size_;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, Script> scripts_;
    std::shared_ptr<Gate> stage_one_gate_;
    std::optional<std::uint64_t> stage_two_estimate_;
    mutable std::vector<std::uint64_t> stage_two_revisions_;
    mutable std::atomic<int> renders_{0};
    mutable std::atomic<int> started_{0};
    mutable std::atomic<int> stage_one_{0};
    mutable std::atomic<int> stage_two_started_{0};
};

// Collects callback values and waits for them.
template <class T>
class Collector {
public:
    void add(T value) {
        {
            std::lock_guard lock(mutex_);
            values_.push_back(std::move(value));
        }
        changed_.notify_all();
    }
    bool wait_for(std::size_t count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, kWaitTimeout, [&] { return values_.size() >= count; });
    }
    std::vector<T> values() const {
        std::lock_guard lock(mutex_);
        return values_;
    }
    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return values_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<T> values_;
};

// A library item for `path` that has not been probed (tests of services that use scripted
// renderers do not need a real file).
inline SourceItem fake_item(const std::filesystem::path& path,
                            SupportStatus status = SupportStatus::CompatibleUnverified) {
    SourceItem item;
    item.path = path;
    item.support.status = status;
    item.support.interpretation = raw::InterpretationStatus::Complete;
    ProbeSummary probe;
    probe.stored_size = image::Size{400, 300};
    probe.upright_size = probe.stored_size;
    item.probe = probe;
    return item;
}

// A synthetic DNG spec with an embedded 96 x 64 JPEG preview and the given EXIF orientation.
inline rts::SyntheticDngSpec dng_with_preview(std::uint16_t orientation = 1) {
    rts::SyntheticDngSpec spec;
    spec.orientation = orientation;
    spec.jpeg_previews = {rts::SyntheticJpegPreview{rts::jpeg_bytes(rts::kJpegRgb96x64), 96, 64, 3}};
    return spec;
}

// Opens (creating) the catalog at `path` for a test; nullptr (with a test failure) on error.
inline std::shared_ptr<catalog::Catalog> open_catalog(const std::filesystem::path& path,
                                                      catalog::OpenMode mode = catalog::OpenMode::ReadWrite) {
    catalog::OpenOptions options;
    options.path = path;
    options.mode = mode;
    auto opened = catalog::Catalog::open(options);
    EXPECT_TRUE(opened) << (opened ? std::string() : opened.error().describe());
    if (!opened) {
        return nullptr;
    }
    return std::shared_ptr<catalog::Catalog>(std::move(*opened));
}

// A catalog-backed library over a catalog file in `dir` (a fresh catalog the first time).
struct CatalogFixture {
    std::shared_ptr<catalog::Catalog> catalog;
    std::unique_ptr<CatalogLibrary> library;

    [[nodiscard]] static CatalogFixture open(const std::filesystem::path& catalog_path) {
        CatalogFixture fixture;
        fixture.catalog = open_catalog(catalog_path);
        if (fixture.catalog) {
            auto library = CatalogLibrary::open(fixture.catalog);
            EXPECT_TRUE(library) << (library ? std::string() : library.error().describe());
            if (library) {
                fixture.library = std::move(*library);
            }
        }
        return fixture;
    }

    // Closes the library and the catalog (the catalog file is released).
    void close() {
        library.reset();
        catalog.reset();
    }
};

// SHA-256 of the file at `path` (a stable read); all zero with a test failure on error.
inline core::Digest256 file_sha256(const std::filesystem::path& path) {
    auto hashed = core::hash_file(path, {});
    EXPECT_TRUE(hashed) << (hashed ? std::string() : hashed.error().describe());
    return hashed ? hashed->digest : core::Digest256{};
}

// The library's item for the file named `name` (first image), or std::nullopt.
inline std::optional<SourceItem> item_named(const Library& library, const std::wstring& name) {
    for (const SourceItem& item : library.items()) {
        if (item.path.filename().wstring() == name) {
            return item;
        }
    }
    return std::nullopt;
}

}  // namespace lucent::app::test_support
