#pragma once

// Helpers shared by the application tests: synthetic DNG files (the raw module's test writers),
// junctions, a scripted SourceRenderer whose timing the tests control, and result collectors.

#include <algorithm>
#include <atomic>
#include <chrono>
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

#include <lucent/application/library.hpp>
#include <lucent/application/source_renderer.hpp>
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

// Behaviour of ScriptedRenderer for one settings revision.
struct Script {
    std::shared_ptr<Gate> gate;       // block until opened (nullptr: do not block)
    bool honour_cancellation = true;  // false: finish even when cancelled while blocked
    std::optional<Error> error;       // fail with this error instead of rendering
};

// A SourceRenderer whose renders produce a synthetic display image (display-referred linear
// Rec.2020, a horizontal ramp) of `size`, and whose timing tests control per settings revision.
// The settings revision is read from RenderSettings::exposure_ev (tests encode it there).
class ScriptedRenderer final : public SourceRenderer {
public:
    explicit ScriptedRenderer(image::Size size = {400, 300}) : size_(size) {}

    void script(std::uint64_t revision, Script script) {
        std::lock_guard lock(mutex_);
        scripts_[revision] = std::move(script);
    }

    [[nodiscard]] int renders() const { return renders_.load(); }
    [[nodiscard]] int started() const { return started_.load(); }

    // Blocks until `count` renders have started.
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

    Expected<std::uint64_t> estimate_memory(const SourceItem&, const engine::RenderSettings&,
                                            const core::CancellationToken&) const override {
        return std::uint64_t{1} << 20;
    }

    Expected<engine::RenderResult> render(const SourceItem&, const engine::RenderSettings& settings,
                                          const StageObserver& on_stage,
                                          const core::CancellationToken& cancel) const override {
        ++started_;
        Script script;
        {
            std::lock_guard lock(mutex_);
            if (const auto it = scripts_.find(static_cast<std::uint64_t>(settings.exposure_ev));
                it != scripts_.end()) {
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
        auto pixels = image::ImageBuffer<float>::create(size_.width, size_.height, 3);
        EXPECT_TRUE(pixels);
        for (std::int32_t y = 0; y < size_.height; ++y) {
            float* row = pixels->row(y);
            for (std::int32_t x = 0; x < size_.width; ++x) {
                const float v = static_cast<float>(x) / static_cast<float>(size_.width - 1);
                row[3 * x] = v;
                row[3 * x + 1] = v;
                row[3 * x + 2] = v;
            }
        }
        engine::RenderResult result;
        result.display.pixels = std::move(*pixels);
        result.display.encoding = engine::display_linear_rec2020();
        result.report.settings = settings;
        result.report.output_size = size_;
        engine::StageRecord stage;
        stage.stage = engine::RenderStage::Demosaic;
        stage.algorithm = "scripted";
        result.report.stages.push_back(stage);
        if (on_stage) {
            on_stage(stage);
        }
        return result;
    }

private:
    image::Size size_;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, Script> scripts_;
    mutable std::atomic<int> renders_{0};
    mutable std::atomic<int> started_{0};
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
                            SupportStatus status = SupportStatus::Supported) {
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

}  // namespace lucent::app::test_support
