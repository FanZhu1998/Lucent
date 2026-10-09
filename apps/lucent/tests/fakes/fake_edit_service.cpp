#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <QColor>
#include <QCoreApplication>
#include <QPainter>
#include <QTimer>

#include "adapters/edit_conversions.hpp"
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

QList<EditPart> changed_parts(const EditSettings& a, const EditSettings& b) {
    QList<EditPart> parts;
    if (a.chroma_denoise != b.chroma_denoise) {
        parts << EditPart::ChromaDenoise;
    }
    if (a.masks != b.masks) {
        parts << EditPart::Masks;
    }
    if (a.geometry != b.geometry) {
        parts << EditPart::Geometry;
    }
    return parts;
}

// `image` (the whole upright scene) through `geometry`, approximately: straightening rotates it on
// its canvas, then the crop, the mirror and the quarter turns.
QImage apply_geometry(const QImage& image, const EditGeometry& geometry) {
    QImage out = image;
    if (geometry.straighten_degrees != 0.0) {
        QImage canvas(out.size(), QImage::Format_RGB32);
        canvas.fill(Qt::black);
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.translate(out.width() / 2.0, out.height() / 2.0);
        painter.rotate(-geometry.straighten_degrees);  // counter-clockwise as displayed
        painter.drawImage(QPointF(-out.width() / 2.0, -out.height() / 2.0), out);
        painter.end();
        out = canvas;
    }
    const EditCrop& crop = geometry.crop;
    const QRect rect(qRound(crop.left * out.width()), qRound(crop.top * out.height()),
                     std::max(1, qRound(crop.width() * out.width())), std::max(1, qRound(crop.height() * out.height())));
    out = out.copy(rect);
    if (geometry.flip_horizontal) {
        out = out.flipped(Qt::Horizontal);
    }
    if (geometry.quarter_turns % 4 != 0) {
        out = out.transformed(QTransform().rotate(90.0 * (geometry.quarter_turns % 4)));
    }
    return out.convertToFormat(QImage::Format_RGB32);
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
        entry.parts = revision.parts;
        entry.current = revision.id == item.current;
        entry.on_current_path = path.contains(revision.id);
        entry.auto_kind = revision.auto_kind;
        entry.auto_failed = revision.auto_failed;
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

QSize FakeEditService::full_size(ItemId id) const {
    if (std::optional<FakeCatalog::Entry> found = catalog_->find(id); found && found->width > 0 && found->height > 0) {
        return QSize(found->width, found->height);
    }
    return QSize(6000, 4000);
}

void FakeEditService::describe(EditFrame& frame, ItemId id, const EditSettings& settings,
                               const EditGeometry& geometry, quint32 mask_overlay) const {
    const QSize full = full_size(id);
    const GeometryMap map = adapters::geometry_map(geometry, full);
    frame.full_upright_size = full;
    frame.geometry = geometry;
    if (!map.valid) {
        return;
    }
    frame.full_output_size = map.output_size;
    const double w = map.output_size.width();
    const double h = map.output_size.height();
    for (const EditMask& mask : settings.masks) {
        FrameMaskOutline outline;
        outline.id = mask.id;
        outline.kind = mask.kind;
        outline.enabled = mask.enabled;
        outline.invert = mask.invert;
        for (const QPolygonF& line : adapters::mask_outline_of(mask, full)) {
            QPolygonF shown;
            for (const QPointF& point : line) {
                const QPointF output = map.normalized_to_output.map(point);
                shown << QPointF(output.x() / w, output.y() / h);
            }
            outline.polylines << shown;
        }
        frame.masks << outline;
        if (mask.id == mask_overlay) {
            frame.mask_overlay = adapters::mask_overlay_image(adapters::to_local_adjustment(mask),
                                                              adapters::to_geometry_setting(geometry),
                                                              adapters::to_image_size(full), 256);
            frame.mask_overlay_id = frame.mask_overlay.isNull() ? 0 : mask.id;
        }
    }
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
            emit auto_summary_changed(id, summary_of(id));
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
        EditGeometry shown = state.settings.geometry;
        if (found->second.crop_editing) {
            shown.crop = EditCrop{};
            out->uncropped = true;
        }
        out->image = apply_geometry(paint(id, state.settings), shown);
        describe(*out, id, state.settings, shown, found->second.mask_overlay);
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
            before->image = apply_geometry(paint(id, EditSettings{}), shown);
            before->geometry = shown;
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
    revision.parts = changed_parts(from, settings);
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

void FakeEditService::set_crop_editing(ItemId id, bool editing) {
    if (const auto found = items_.find(id); found != items_.end() && found->second.crop_editing != editing) {
        found->second.crop_editing = editing;
        publish(id, 0, false, true, true);
    }
}

void FakeEditService::set_mask_overlay(ItemId id, quint32 mask_id) {
    if (const auto found = items_.find(id); found != items_.end() && found->second.mask_overlay != mask_id) {
        found->second.mask_overlay = mask_id;
        if (mask_id != 0) {
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
        if (has(SettingsCategory::Masks)) {
            settings.masks = from.masks;
            report.applied << SettingsCategory::Masks;
        }
        if (has(SettingsCategory::Detail)) {
            settings.chroma_denoise = from.chroma_denoise;
            report.applied << SettingsCategory::Detail;
        }
        if (has(SettingsCategory::Geometry)) {
            if (!from.geometry.crop.is_full() && full_size(id) != full_size(clipboard_->source)) {
                report.skipped << tr_fake("Crop and rotation: simulated photo of another shape.");
            } else {
                settings.geometry = from.geometry;
                report.applied << SettingsCategory::Geometry;
            }
        }
        if (settings != committed(items_[id])) {
            ++report.changed;
            append(id, settings, QStringLiteral("paste"), 0);
        }
    }
    later([this, report] { emit pasted(report); });
}

// ---- Simulated Auto ------------------------------------------------------------------------------

AutoSummary FakeEditService::summary_of(ItemId id) const {
    const auto found = items_.find(id);
    if (found == items_.end()) {
        return {};
    }
    const Revision* current = find(found->second, found->second.current);
    return current != nullptr && current->summary ? *current->summary : AutoSummary{};
}

bool FakeEditService::is_auto_running(ItemId item) const {
    return item == 0 ? !auto_runs_.empty() : auto_runs_.contains(item);
}

void FakeEditService::hold_auto(bool held) {
    auto_held_ = held;
}

void FakeEditService::run_auto(ItemId id) {
    if (id == 0 || auto_runs_.contains(id)) {
        return;
    }
    AutoRun run;
    run.fail = std::exchange(fail_next_auto_, false);
    auto_runs_[id] = run;
    QTimer::singleShot(0, this, [this, id] { auto_step(id); });
}

void FakeEditService::cancel_auto(ItemId id) {
    if (const auto found = auto_runs_.find(id); found != auto_runs_.end()) {
        found->second.cancel = true;
    }
}

void FakeEditService::auto_step(ItemId id) {
    const auto found = auto_runs_.find(id);
    if (found == auto_runs_.end()) {
        return;
    }
    AutoRun& run = found->second;
    const int delay = std::max(0, timings_.preview_delay_ms);
    if (run.cancel) {
        auto_runs_.erase(found);
        AutoOutcome outcome;
        outcome.item = id;
        outcome.cancelled = true;
        outcome.message = tr_fake("Auto was cancelled; nothing was changed.");
        emit auto_finished(outcome);
        return;
    }
    constexpr std::array kStages{AutoStage::Queued, AutoStage::Analysing, AutoStage::Checking, AutoStage::Saving};
    if (run.step < static_cast<int>(kStages.size())) {
        emit auto_progress(AutoProgress{id, kStages[static_cast<std::size_t>(run.step)], run.step * 0.3});
        run.step += auto_held_ && run.step == 1 ? 0 : 1;  // a held run stays in "Analysing"
        QTimer::singleShot(auto_held_ ? std::max(delay, 10) : delay, this, [this, id] { auto_step(id); });
        return;
    }
    const AutoRun done = run;
    auto_runs_.erase(found);
    finish_auto(id, done);
}

void FakeEditService::finish_auto(ItemId id, const AutoRun& run) {
    Item& item = items_[id];
    const EditSettings from = committed(item);
    const quint64 parent = item.current;
    EditSettings chosen = from;
    AutoSummary summary;
    summary.confidence = run.fail ? 0.6 : 0.82;
    if (from.locked(EditControl::Exposure)) {
        summary.locked << EditControl::Exposure;
        summary.locks_prevented_targets = true;
        summary.lock_notes << tr_fake("Simulated: exposure is locked, so the photo stays about 0.5 EV darker than "
                                      "Lucent's usual target.");
        summary.review_reasons << tr_fake("Locked controls kept Auto from its usual targets.");
    } else {
        chosen.exposure_ev = std::clamp(from.exposure_ev + 0.5, -kExposureLimit, kExposureLimit);
    }
    if (!from.locked(EditControl::Contrast)) {
        chosen.contrast = std::clamp(from.contrast + 0.1, -kAdjustmentLimit, kAdjustmentLimit);
    }
    summary.notes << tr_fake("Simulated: the colour of the light was uncertain, so the white balance was kept "
                             "close to the camera's.");
    const auto add_revision = [&](const EditSettings& settings, const QString& kind, bool failed,
                                  const AutoSummary& described) {
        Revision revision;
        revision.id = next_revision_++;
        revision.number = static_cast<quint32>(item.revisions.size() + 1);
        revision.parent = parent;  // siblings: undo returns to the recipe before Auto
        revision.settings = settings;
        revision.origin = QStringLiteral("auto");
        revision.created = QDateTime::currentDateTimeUtc();
        revision.changed = changed_controls(from, settings);
        revision.auto_kind = kind;
        revision.auto_failed = failed;
        AutoSummary stored = described;
        stored.revision = revision.id;
        stored.revision_number = revision.number;
        stored.changed = revision.changed;
        revision.summary = stored;
        item.revisions.push_back(revision);
        return revision;
    };
    int written = 0;
    if (run.fail) {
        AutoSummary failed = summary;
        failed.review = true;
        static_cast<void>(add_revision(chosen, QStringLiteral("chosen"), true, failed));
        ++written;
        chosen.contrast = from.contrast;  // the conservative fallback keeps the contrast
        summary.attempt = 1;
        summary.fallback_detail = tr_fake("Simulated: attempt 0 failed the full-resolution check.");
        summary.review_reasons << tr_fake("The first automatic edit failed a quality check, so a more conservative "
                                          "one was used.");
    }
    summary.review = !summary.review_reasons.isEmpty();
    const Revision effective =
        add_revision(chosen, run.fail ? QStringLiteral("fallback") : QStringLiteral("chosen"), false, summary);
    ++written;
    const bool made_current = !item.transient.has_value() && item.current == parent;
    if (made_current) {
        item.current = effective.id;
    }
    publish(id, 0, true, made_current, true);
    AutoOutcome outcome;
    outcome.item = id;
    outcome.saved = true;
    outcome.made_current = made_current;
    outcome.revision_number = effective.number;
    outcome.revisions = written;
    outcome.review = summary.review;
    outcome.message = made_current ? tr_fake("Auto saved its settings as revision %1.").arg(effective.number)
                                   : tr_fake("Auto finished, but the photo was changed meanwhile: its settings are "
                                             "kept in the history as revision %1.")
                                         .arg(effective.number);
    later([this, outcome] { emit auto_finished(outcome); });
}

GeometryMap FakeEditService::map_geometry(const EditGeometry& geometry, QSize upright_size, QRect frame) const {
    return adapters::geometry_map(geometry, upright_size, frame);
}

bool FakeEditService::crop_inside_image(const EditGeometry& geometry, QSize frame_size) const {
    return adapters::geometry_crop_inside(geometry, frame_size);
}

EditCrop FakeEditService::largest_inside_crop(double straighten_degrees, QSize frame_size, double aspect) const {
    return adapters::geometry_largest_inside_crop(straighten_degrees, frame_size, aspect);
}

EditGeometry FakeEditService::turned(const EditGeometry& geometry, GeometryTurn turn) const {
    return adapters::geometry_turned(geometry, turn);
}

QList<QPolygonF> FakeEditService::mask_outline(const EditMask& mask, QSize upright_size) const {
    return adapters::mask_outline_of(mask, upright_size);
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
