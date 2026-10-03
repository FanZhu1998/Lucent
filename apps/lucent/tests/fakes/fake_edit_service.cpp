#include <algorithm>
#include <cmath>
#include <utility>

#include <QColor>
#include <QCoreApplication>
#include <QTimer>

#include "fakes/fake_services.hpp"

namespace lucent::ui::fakes {
namespace {

QString tr_fake(const char* text) {
    return QCoreApplication::translate("lucent::ui::fakes", text);
}

constexpr QSize kFrameBox{768, 512};

// The controls whose values differ between `a` and `b`.
QList<EditControl> changed_controls(const EditSettings& a, const EditSettings& b) {
    QList<EditControl> changed;
    if (a.white_balance != b.white_balance || a.temperature_k != b.temperature_k || a.tint != b.tint ||
        a.neutral != b.neutral) {
        changed << EditControl::WhiteBalance;
    }
    if (a.exposure_ev != b.exposure_ev) {
        changed << EditControl::Exposure;
    }
    if (a.contrast != b.contrast) {
        changed << EditControl::Contrast;
    }
    if (a.highlights != b.highlights) {
        changed << EditControl::Highlights;
    }
    if (a.shadows != b.shadows) {
        changed << EditControl::Shadows;
    }
    if (a.tone_curve != b.tone_curve) {
        changed << EditControl::ToneCurve;
    }
    if (a.saturation != b.saturation) {
        changed << EditControl::Saturation;
    }
    if (a.vibrance != b.vibrance) {
        changed << EditControl::Vibrance;
    }
    return changed;
}

RenderInfoPtr analyse(const QImage& image) {
    auto info = std::make_shared<RenderInfo>();
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int r = qRed(row[x]);
            const int g = qGreen(row[x]);
            const int b = qBlue(row[x]);
            ++info->red[static_cast<std::size_t>(r)];
            ++info->green[static_cast<std::size_t>(g)];
            ++info->blue[static_cast<std::size_t>(b)];
            ++info->luma[static_cast<std::size_t>(std::clamp(qRound(0.2126 * r + 0.7152 * g + 0.0722 * b), 0, 255))];
        }
    }
    info->pixels = static_cast<std::uint64_t>(image.width()) * static_cast<std::uint64_t>(image.height());
    info->preview_width = image.width();
    info->preview_height = image.height();
    info->rendering = QStringLiteral("Simulated");
    return info;
}

QImage overlay_of(const QImage& image) {
    QImage overlay(image.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        auto* out = reinterpret_cast<QRgb*>(overlay.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb p = row[x];
            const bool high = qRed(p) == 255 || qGreen(p) == 255 || qBlue(p) == 255;
            const bool low = qRed(p) == 0 || qGreen(p) == 0 || qBlue(p) == 0;
            out[x] = high ? qPremultiply(qRgba(255, 40, 40, 230)) : low ? qPremultiply(qRgba(40, 120, 255, 230)) : 0u;
        }
    }
    return overlay;
}

}  // namespace

FakeEditService::FakeEditService(std::shared_ptr<const FakeCatalog> catalog, FakeTimings timings, QObject* parent)
    : EditService(parent), catalog_(std::move(catalog)), timings_(timings) {}

int FakeEditService::revision_count(ItemId item) const {
    const auto found = items_.find(item);
    return found == items_.end() ? 0 : static_cast<int>(found->second.revisions.size());
}

const FakeEditService::Revision* FakeEditService::find(const Item& item, quint64 id) const {
    for (const Revision& revision : item.revisions) {
        if (revision.id == id) {
            return &revision;
        }
    }
    return nullptr;
}

EditSettings FakeEditService::committed(const Item& item) const {
    const Revision* revision = find(item, item.current);
    return revision != nullptr ? revision->settings : EditSettings{};
}

bool FakeEditService::kelvin_available(ItemId id) const {
    const std::optional<FakeCatalog::Entry> entry = catalog_->find(id);
    return !entry || entry->support != SupportStatus::Limited;
}

EditState FakeEditService::state_of(ItemId id, quint64 token) const {
    EditState state;
    state.item = id;
    state.token = token;
    const auto found = items_.find(id);
    if (found == items_.end()) {
        return state;
    }
    const Item& item = found->second;
    state.committed = committed(item);
    state.settings = item.transient.value_or(state.committed);
    state.transient = item.transient.has_value();
    state.transient_generation = item.transient_generation;
    if (const Revision* current = find(item, item.current)) {
        state.revision = current->id;
        state.revision_number = current->number;
    }
    state.can_undo = item.current != 0;
    state.can_redo = std::any_of(item.revisions.cbegin(), item.revisions.cend(),
                                 [&item](const Revision& revision) { return revision.parent == item.current; });
    state.edited = item.current != 0 && !state.committed.renders_as_shot();
    return state;
}

QList<EditHistoryEntry> FakeEditService::history_of(ItemId id) const {
    QList<EditHistoryEntry> entries;
    const auto found = items_.find(id);
    if (found == items_.end()) {
        return entries;
    }
    const Item& item = found->second;
    QList<quint64> path;
    for (const Revision* at = find(item, item.current); at != nullptr; at = find(item, at->parent)) {
        path << at->id;
    }
    for (const Revision& revision : item.revisions) {
        EditHistoryEntry entry;
        entry.revision = revision.id;
        entry.number = revision.number;
        if (const Revision* parent = find(item, revision.parent)) {
            entry.parent_number = parent->number;
        }
        entry.origin = revision.origin;
        entry.created = revision.created;
        entry.changed = revision.changed;
        entry.current = revision.id == item.current;
        entry.on_current_path = path.contains(revision.id);
        entries << entry;
    }
    return entries;
}

void FakeEditService::later(std::function<void()> work) {
    ++pending_;
    if (pending_ == 1) {
        emit busy_changed(true);
    }
    QTimer::singleShot(std::max(0, timings_.thumbnail_delay_ms), this, [this, work = std::move(work)] {
        work();
        --pending_;
        if (pending_ == 0) {
            emit busy_changed(false);
        }
    });
}

QImage FakeEditService::paint(ItemId id, const EditSettings& settings) const {
    FakeCatalog::Entry entry;
    if (std::optional<FakeCatalog::Entry> found = catalog_->find(id)) {
        entry = *found;
    } else {
        entry.width = 6000;
        entry.height = 4000;
    }
    QImage image = FakeImageSource::paint(entry, ImageKind::Preview, 1, kFrameBox);
    image = image.convertToFormat(QImage::Format_RGB32);
    const double gain = std::exp2(settings.exposure_ev);
    const double contrast = 1.0 + settings.contrast;
    const double warm = settings.white_balance == WhiteBalanceMode::Kelvin ? (settings.temperature_k - 5200.0) / 20000.0
                                                                          : 0.0;
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const auto adjust = [&](int value, double tilt) {
                double v = value / 255.0 * gain * (1.0 + tilt);
                v = 0.5 + (v - 0.5) * contrast;
                return std::clamp(qRound(v * 255.0), 0, 255);
            };
            row[x] = qRgb(adjust(qRed(row[x]), warm), adjust(qGreen(row[x]), 0.0), adjust(qBlue(row[x]), -warm));
        }
    }
    return image;
}

void FakeEditService::publish(ItemId id, quint64 token, bool history, bool frame, bool settled) {
    const EditState state = state_of(id, token);
    later([this, id, state, history, frame, settled, token] {
        const auto found = items_.find(id);
        if (found == items_.end()) {
            return;
        }
        emit state_changed(state);
        if (history) {
            emit history_changed(id, history_of(id));
        }
        if (!frame || !found->second.open) {
            return;
        }
        auto out = std::make_shared<EditFrame>();
        out->item = id;
        out->sequence = ++next_sequence_;
        out->generation = ++next_generation_;
        out->revision = state.transient ? 0 : state.revision;
        out->transient_generation = state.transient ? state.transient_generation : 0;
        out->token = token;
        out->phase = settled ? FramePhase::Final : FramePhase::Quick;
        out->image = paint(id, state.settings);
        if (clipping_) {
            out->clipping = overlay_of(out->image);
        }
        out->info = analyse(out->image);
        emit frame_ready(out);
        if (found->second.before) {
            auto before = std::make_shared<EditFrame>();
            before->item = id;
            before->sequence = ++next_sequence_;
            before->before = true;
            before->phase = FramePhase::Quick;
            before->image = paint(id, EditSettings{});
            before->info = analyse(before->image);
            emit frame_ready(before);
        }
    });
}

quint64 FakeEditService::append(ItemId id, const EditSettings& settings, const QString& origin, quint64 token) {
    Item& item = items_[id];
    const EditSettings from = committed(item);
    item.transient.reset();
    if (settings == from && origin != u"reset") {
        publish(id, token, false, true, true);
        return token;
    }
    Revision revision;
    revision.id = next_revision_++;
    revision.number = static_cast<quint32>(item.revisions.size() + 1);
    revision.parent = item.current;
    revision.settings = settings;
    revision.origin = origin;
    revision.created = QDateTime::currentDateTimeUtc();
    revision.changed = changed_controls(from, settings);
    item.revisions.push_back(revision);
    item.current = revision.id;
    publish(id, token, true, true, true);
    return token;
}

void FakeEditService::open(ItemId id) {
    Item& item = items_[id];
    item.open = true;
    const bool kelvin = kelvin_available(id);
    later([this, id, kelvin] {
        WhiteBalanceInfo info;
        info.known = true;
        info.kelvin_available = kelvin;
        info.as_shot_temperature_k = 5200.0;
        info.as_shot_tint = 4.0;
        info.as_shot_neutral = {2.0, 1.0, 1.5};
        if (!kelvin) {
            info.explanation = tr_fake("Simulated: this camera's calibration does not define a colour temperature.");
        }
        emit white_balance_changed(id, info);
    });
    publish(id, 0, true, true, true);
}

void FakeEditService::close(ItemId id) {
    if (const auto found = items_.find(id); found != items_.end()) {
        found->second.open = false;
        found->second.transient.reset();
    }
}

void FakeEditService::set_full_resolution(ItemId id, bool full) {
    if (full && items_.contains(id)) {
        publish(id, 0, false, true, true);
    }
}

void FakeEditService::set_comparison(ItemId id, bool wanted) {
    if (const auto found = items_.find(id); found != items_.end()) {
        found->second.before = wanted;
        if (wanted) {
            publish(id, 0, false, true, true);
        }
    }
}

void FakeEditService::set_clipping(bool wanted) {
    clipping_ = wanted;
    for (const auto& [id, item] : items_) {
        if (item.open && wanted) {
            publish(id, 0, false, true, true);
        }
    }
}

quint64 FakeEditService::set_transient(ItemId id, const EditSettings& settings) {
    const quint64 token = ++next_token_;
    Item& item = items_[id];
    item.transient = settings;
    ++item.transient_generation;
    publish(id, token, false, true, false);
    return token;
}

quint64 FakeEditService::commit(ItemId id) {
    const quint64 token = ++next_token_;
    Item& item = items_[id];
    if (!item.transient) {
        publish(id, token, false, true, true);
        return token;
    }
    return append(id, *item.transient, QStringLiteral("user"), token);
}

quint64 FakeEditService::apply(ItemId id, const EditSettings& settings) {
    return append(id, settings, QStringLiteral("user"), ++next_token_);
}

quint64 FakeEditService::set_lock(ItemId id, EditControl control, bool locked) {
    const quint64 token = ++next_token_;
    EditSettings settings = committed(items_[id]);
    settings.set_locked(control, locked);
    return append(id, settings, QStringLiteral("user"), token);
}

quint64 FakeEditService::undo(ItemId id) {
    const quint64 token = ++next_token_;
    Item& item = items_[id];
    item.transient.reset();
    if (const Revision* current = find(item, item.current)) {
        item.current = current->parent;
    }
    publish(id, token, true, true, true);
    return token;
}

quint64 FakeEditService::redo(ItemId id) {
    const quint64 token = ++next_token_;
    Item& item = items_[id];
    item.transient.reset();
    for (auto it = item.revisions.rbegin(); it != item.revisions.rend(); ++it) {
        if (it->parent == item.current) {
            item.current = it->id;
            break;
        }
    }
    publish(id, token, true, true, true);
    return token;
}

quint64 FakeEditService::select_revision(ItemId id, quint64 revision) {
    const quint64 token = ++next_token_;
    Item& item = items_[id];
    item.transient.reset();
    if (revision == 0 || find(item, revision) != nullptr) {
        item.current = revision;
    }
    publish(id, token, true, true, true);
    return token;
}

quint64 FakeEditService::reset_to_as_shot(ItemId id) {
    EditSettings settings;
    settings.locks = committed(items_[id]).locks;
    return append(id, settings, QStringLiteral("reset"), ++next_token_);
}

void FakeEditService::copy_settings(ItemId id, quint32 categories) {
    clipboard_ = Clipboard{id, committed(items_[id]), categories};
    later([this, id, categories] { emit copied(id, categories); });
}

void FakeEditService::paste_settings(const QList<ItemId>& targets) {
    PasteReport report;
    report.targets = static_cast<int>(targets.size());
    if (!clipboard_) {
        report.errors << tr_fake("Nothing has been copied yet.");
        later([this, report] { emit pasted(report); });
        return;
    }
    for (const ItemId id : targets) {
        EditSettings settings = committed(items_[id]);
        const EditSettings& from = clipboard_->settings;
        const auto has = [this](SettingsCategory category) {
            return (clipboard_->categories & category_bit(category)) != 0;
        };
        if (has(SettingsCategory::WhiteBalance)) {
            if (from.white_balance == WhiteBalanceMode::Kelvin && !kelvin_available(id)) {
                report.skipped << tr_fake("White balance: simulated camera without a colour temperature.");
            } else {
                settings.white_balance = from.white_balance;
                settings.temperature_k = from.temperature_k;
                settings.tint = from.tint;
                settings.neutral = from.neutral;
                report.applied << SettingsCategory::WhiteBalance;
            }
        }
        if (has(SettingsCategory::Exposure)) {
            settings.exposure_ev = from.exposure_ev;
            report.applied << SettingsCategory::Exposure;
        }
        if (has(SettingsCategory::Tone)) {
            settings.contrast = from.contrast;
            settings.tone_curve = from.tone_curve;
            report.applied << SettingsCategory::Tone;
        }
        if (has(SettingsCategory::Color)) {
            settings.saturation = from.saturation;
            settings.vibrance = from.vibrance;
            report.applied << SettingsCategory::Color;
        }
        if (has(SettingsCategory::Local)) {
            settings.highlights = from.highlights;
            settings.shadows = from.shadows;
            report.applied << SettingsCategory::Local;
        }
        if (settings != committed(items_[id])) {
            ++report.changed;
            append(id, settings, QStringLiteral("paste"), 0);
        }
    }
    later([this, report] { emit pasted(report); });
}

QList<double> FakeEditService::sample_tone_curve(const QList<QPointF>& points, int count) const {
    QList<double> samples;
    if (points.size() < 2 || count < 2) {
        return samples;
    }
    for (int i = 0; i < count; ++i) {
        const double x = double(i) / double(count - 1);
        double y = x <= points.front().x() ? points.front().y() : points.back().y();
        for (qsizetype p = 0; p + 1 < points.size(); ++p) {
            if (x >= points[p].x() && x <= points[p + 1].x()) {
                const double span = points[p + 1].x() - points[p].x();
                const double t = span > 0.0 ? (x - points[p].x()) / span : 0.0;
                y = points[p].y() + t * (points[p + 1].y() - points[p].y());
                break;
            }
        }
        samples << y;
    }
    return samples;
}

}  // namespace lucent::ui::fakes
