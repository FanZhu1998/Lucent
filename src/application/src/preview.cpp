#include <format>
#include <functional>

#include <lucent/application/preview.hpp>
#include <lucent/application/preview_cache.hpp>

namespace lucent::app {

namespace {

// Allowance for the records kept beside the pixels (render report, summary, histogram).
constexpr std::uint64_t kRecordAllowanceBytes = 16 << 10;

std::size_t combine(std::size_t seed, std::size_t value) noexcept {
    return seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2));
}

}  // namespace

Status PreviewQuality::validate() const {
    switch (kind) {
        case PreviewQualityKind::Full:
            return ok();
        case PreviewQualityKind::Fit:
            if (long_edge < kMinLongEdge || long_edge > kMaxLongEdge) {
                return fail(
                    ErrorCode::InvalidArgument,
                    std::format("fit long edge {} is outside {}..{}", long_edge, kMinLongEdge, kMaxLongEdge),
                    "app.preview.quality");
            }
            return ok();
    }
    return fail(ErrorCode::InvalidArgument, std::format("unknown preview quality {}", static_cast<int>(kind)),
                "app.preview.quality");
}

std::string to_string(const PreviewQuality& quality) {
    if (quality.kind == PreviewQualityKind::Full) {
        return "full";
    }
    return std::format("fit-{}", quality.long_edge);
}

RenderSummary summarize_render(const engine::RenderReport& report) {
    RenderSummary summary;
    summary.pipeline_version = report.pipeline_version;
    summary.rendering = report.settings.rendering;
    summary.interpretation = report.interpretation.status;
    summary.issues = report.interpretation.issues;
    summary.warnings = report.warnings;
    summary.rendered_size = report.output_size;
    summary.orientation = report.orientation;
    summary.limited_display_pixels = report.rendering.limited_pixels;
    summary.estimated_peak_bytes = report.memory.peak_bytes;
    summary.render_seconds = report.total_seconds;
    for (const engine::StageRecord& stage : report.stages) {
        summary.provisional = summary.provisional || stage.provisional;
        if (stage.stage == engine::RenderStage::Demosaic) {
            summary.demosaic = stage.algorithm;
        } else if (stage.stage == engine::RenderStage::CameraColor) {
            summary.camera_color = stage.algorithm;
        } else if (stage.stage == engine::RenderStage::WhiteBalance) {
            summary.clipped_raw_samples += stage.clipped_samples;
        }
    }
    return summary;
}

std::size_t PreviewKeyHash::operator()(const PreviewKey& key) const noexcept {
    std::size_t seed = std::hash<std::uint64_t>{}(key.item.value);
    seed = combine(seed, std::hash<std::uint64_t>{}(key.settings_revision));
    seed = combine(seed, static_cast<std::size_t>(key.quality.kind));
    if (key.quality.kind == PreviewQualityKind::Fit) {
        seed = combine(seed, std::hash<std::int32_t>{}(key.quality.long_edge));
    }
    return seed;
}

std::uint64_t PreviewImage::byte_size() const noexcept {
    return static_cast<std::uint64_t>(image.byte_size()) + kRecordAllowanceBytes;
}

// ---------------------------------------------------------------------------------------------
// PreviewCache
// ---------------------------------------------------------------------------------------------

PreviewCache::PreviewCache(std::uint64_t capacity_bytes) {
    stats_.capacity_bytes = capacity_bytes;
}

std::shared_ptr<const PreviewImage> PreviewCache::find(const PreviewKey& key) {
    std::lock_guard lock(mutex_);
    const auto it = index_.find(key);
    if (it == index_.end()) {
        ++stats_.misses;
        return nullptr;
    }
    lru_.splice(lru_.begin(), lru_, it->second);
    ++stats_.hits;
    return it->second->preview;
}

bool PreviewCache::insert(const PreviewKey& key, std::shared_ptr<const PreviewImage> preview) {
    if (!preview) {
        return false;
    }
    const std::uint64_t bytes = preview->byte_size();
    std::lock_guard lock(mutex_);
    if (bytes > stats_.capacity_bytes) {
        ++stats_.rejected;
        return false;
    }
    try {
        if (const auto it = index_.find(key); it != index_.end()) {
            stats_.bytes -= it->second->bytes;
            lru_.erase(it->second);
            index_.erase(it);
        }
        lru_.push_front(Entry{key, std::move(preview), bytes});
        try {
            index_.emplace(key, lru_.begin());
        } catch (...) {
            lru_.pop_front();
            throw;
        }
    } catch (const std::bad_alloc&) {
        return false;  // the cache is disposable: failing to store is not an error
    }
    stats_.bytes += bytes;
    ++stats_.insertions;
    evict_to(stats_.capacity_bytes);
    return true;
}

void PreviewCache::erase_item(ItemId item) {
    std::lock_guard lock(mutex_);
    for (auto it = lru_.begin(); it != lru_.end();) {
        if (it->key.item == item) {
            stats_.bytes -= it->bytes;
            index_.erase(it->key);
            it = lru_.erase(it);
        } else {
            ++it;
        }
    }
}

void PreviewCache::clear() {
    std::lock_guard lock(mutex_);
    lru_.clear();
    index_.clear();
    stats_.bytes = 0;
}

void PreviewCache::set_capacity(std::uint64_t capacity_bytes) {
    std::lock_guard lock(mutex_);
    stats_.capacity_bytes = capacity_bytes;
    evict_to(capacity_bytes);
}

PreviewCacheStats PreviewCache::stats() const {
    std::lock_guard lock(mutex_);
    PreviewCacheStats stats = stats_;
    stats.entries = lru_.size();
    return stats;
}

void PreviewCache::evict_to(std::uint64_t capacity) {
    while (stats_.bytes > capacity && !lru_.empty()) {
        const Entry& last = lru_.back();
        stats_.bytes -= last.bytes;
        index_.erase(last.key);
        lru_.pop_back();
        ++stats_.evictions;
    }
}

}  // namespace lucent::app
