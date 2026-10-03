#pragma once

// Helpers for the tests of the adapters over the application services (adapters/): synthetic DNG
// files (the raw module's test writers), a renderer whose timing the tests control per file, and a
// harness that creates the real services and records their signals.

#include <QList>
#include <QString>
#include <QTest>
#include <QThread>
#include <atomic>
#include <chrono>
#include <cstdint>
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

#include <lucent/application/source_renderer.hpp>
#include <lucent/engine/display_rendering.hpp>
#include <lucent/engine/render.hpp>

#include "adapters/app_export_service.hpp"
#include "adapters/app_image_source.hpp"
#include "adapters/app_library_service.hpp"
#include "adapters/application_services.hpp"
#include "raw_test_support.hpp"
#include "synthetic_dng.hpp"
#include "test_jpegs.hpp"

namespace lucent::ui::testing {

namespace rts = lucent::raw::test_support;

inline constexpr int kAdapterWaitMs = 30000;

// A synthetic DNG spec with an embedded 96 x 64 JPEG preview and the given EXIF orientation.
inline rts::SyntheticDngSpec dng_with_preview(std::uint16_t orientation = 1) {
    rts::SyntheticDngSpec spec;
    spec.orientation = orientation;
    spec.jpeg_previews = {rts::SyntheticJpegPreview{rts::jpeg_bytes(rts::kJpegRgb96x64), 96, 64, 3}};
    return spec;
}

inline std::filesystem::path write_dng(const rts::ScratchDir& dir, const std::filesystem::path& name,
                                       const rts::SyntheticDngSpec& spec = {}) {
    return dir.write(name, rts::build_synthetic_dng(spec));
}

// A SourceRenderer whose renders produce a flat display image of `size` whose grey level is
// derived from the file name (so different items give different pixels), and block while the
// file's gate is closed. A blocked render returns Cancelled once its token is cancelled, unless
// the file was set to ignore cancellation (then it finishes when the gate opens).
class GatedRenderer final : public app::SourceRenderer {
public:
    explicit GatedRenderer(image::Size size = {120, 80}) : size_(size) {}

    void close_gate(const std::wstring& file_name, bool ignore_cancellation = false) {
        const std::lock_guard lock(mutex_);
        gates_[file_name] = Gate{false, ignore_cancellation};
    }
    void open_gate(const std::wstring& file_name) {
        const std::lock_guard lock(mutex_);
        gates_[file_name].open = true;
    }
    // Opens every gate (test teardown: a render ignoring cancellation would block shutdown).
    void open_all() {
        const std::lock_guard lock(mutex_);
        for (auto& [name, gate] : gates_) {
            gate.open = true;
        }
    }

    [[nodiscard]] int started(const std::wstring& file_name) const { return count(started_, file_name); }
    [[nodiscard]] int finished(const std::wstring& file_name) const { return count(finished_, file_name); }
    [[nodiscard]] int cancelled(const std::wstring& file_name) const { return count(cancelled_, file_name); }

    // Waits (processing Qt events) until `file_name` has started `renders` renders.
    [[nodiscard]] bool wait_started(const std::wstring& file_name, int renders) const {
        return QTest::qWaitFor([&] { return started(file_name) >= renders; }, kAdapterWaitMs);
    }

    Expected<engine::DevelopMemoryEstimate> estimate(const app::SourceItem&, const engine::StageOneRequest&,
                                                     const core::CancellationToken&) const override {
        engine::DevelopMemoryEstimate estimate;
        estimate.stage_one_peak = std::uint64_t{1} << 20;
        estimate.stage_one_bytes = std::uint64_t{1} << 20;
        estimate.export_peak = std::uint64_t{1} << 20;
        return estimate;
    }

    // Edit sessions are not exercised by the shell tests.
    Expected<engine::StageOneImage> render_stage_one(const app::SourceItem&, const engine::StageOneRequest&,
                                                     const core::CancellationToken&) const override {
        return fail(ErrorCode::Unsupported, "the gated renderer has no stage one", "test.render");
    }
    Expected<engine::DevelopResult> render_stage_two(const engine::StageOneImage&,
                                                     const engine::DevelopSettings&,
                                                     const core::CancellationToken&) const override {
        return fail(ErrorCode::Unsupported, "the gated renderer has no stage two", "test.render");
    }

    Expected<app::DevelopRender> render(const app::SourceItem& item, const engine::DevelopSettings& settings,
                                        const app::RenderProgress&,
                                        const core::CancellationToken& cancel) const override {
        const std::wstring name = item.path.filename().wstring();
        increment(started_, name);
        while (true) {
            std::optional<Gate> gate;
            {
                const std::lock_guard lock(mutex_);
                if (const auto found = gates_.find(name); found != gates_.end()) {
                    gate = found->second;
                }
            }
            if (!gate || gate->open) {
                break;
            }
            if (cancel.is_cancelled() && !gate->ignore_cancellation) {
                increment(cancelled_, name);
                return fail(ErrorCode::Cancelled, "gated render cancelled", "test.render");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto pixels = image::ImageBuffer<float>::create(size_.width, size_.height, 3);
        if (!pixels) {
            return Unexpected(pixels.error());
        }
        const float level =
            0.05f + 0.9f * static_cast<float>(std::hash<std::wstring>{}(name) % 1000) / 1000.0f;
        for (std::int32_t y = 0; y < size_.height; ++y) {
            float* row = pixels->row(y);
            for (std::int32_t x = 0; x < 3 * size_.width; ++x) {
                row[x] = level;
            }
        }
        app::DevelopRender result;
        result.display.pixels = std::move(*pixels);
        result.display.encoding = engine::display_linear_rec2020();
        result.report.settings = settings;
        result.report.output_size = size_;
        result.report.full_size = size_;
        result.report.stage_two_seconds = 0.01;
        increment(finished_, name);
        return result;
    }

private:
    struct Gate {
        bool open = false;
        bool ignore_cancellation = false;
    };

    int count(const std::map<std::wstring, int>& counts, const std::wstring& name) const {
        const std::lock_guard lock(mutex_);
        const auto found = counts.find(name);
        return found == counts.end() ? 0 : found->second;
    }
    void increment(std::map<std::wstring, int>& counts, const std::wstring& name) const {
        const std::lock_guard lock(mutex_);
        ++counts[name];
    }

    image::Size size_;
    mutable std::mutex mutex_;
    std::map<std::wstring, Gate> gates_;
    mutable std::map<std::wstring, int> started_;
    mutable std::map<std::wstring, int> finished_;
    mutable std::map<std::wstring, int> cancelled_;
};

// Executor options for tests: Lucent.exe's (four workers: two background threads, one kept for
// quick work, one for interactive work), stated explicitly so the tests pin them.
inline adapters::BackendOptions test_backend_options(
    std::shared_ptr<const app::SourceRenderer> renderer = {}) {
    adapters::BackendOptions options;
    options.executor.workers = 4;
    options.executor.reserved_interactive = 1;
    options.executor.reserved_quick = 1;
    options.renderer_override = std::move(renderer);
    return options;
}

// Requests an image from `source` on a separate thread (as Qt Quick's image-loader thread does)
// and returns its reply.
inline std::shared_ptr<ImageReply> request_image(ImageSource& source, ImageKind kind, ItemId item,
                                                 Generation generation, QSize bounding) {
    auto reply = std::make_shared<ImageReply>();
    std::thread loader([&] { source.request_image(ImageRequest{kind, item, generation, bounding}, reply); });
    loader.join();
    return reply;
}

// Waits (processing Qt events, so posted UI work runs) until `reply` has completed.
[[nodiscard]] inline bool wait_completed(const ImageReply& reply) {
    return QTest::qWaitFor([&] { return reply.state() != ImageReply::State::Pending; }, kAdapterWaitMs);
}

}  // namespace lucent::ui::testing
