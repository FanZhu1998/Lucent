#include "adapters/edit_conversions.hpp"

#include <QCoreApplication>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <new>
#include <variant>

#include <lucent/engine/white_balance_model.hpp>

#include "adapters/conversions.hpp"

namespace lucent::ui::adapters {
namespace {

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::adapters", text);
}

QString camera_name(const app::CameraCapabilities& camera) {
    if (camera.make.empty() && camera.model.empty()) {
        return tr_text("an unknown camera");
    }
    if (camera.model.empty()) {
        return QString::fromStdString(camera.make);
    }
    const QString make = QString::fromStdString(camera.make);
    const QString model = QString::fromStdString(camera.model);
    // "NIKON CORPORATION" + "NIKON Z 6": the model alone (presentation::camera_label's rule).
    const QString first = make.section(u' ', 0, 0);
    if (!first.isEmpty() && model.startsWith(first, Qt::CaseInsensitive)) {
        return model;
    }
    return make.isEmpty() ? model : make + u' ' + model;
}

QString origin_key(catalog::RevisionOrigin origin) {
    const std::string_view text = catalog::to_string(origin);
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

}  // namespace

// ---- Settings ------------------------------------------------------------------------------------

EditSettings to_edit_settings(const engine::DevelopSettings& settings) {
    EditSettings out;
    std::visit(
        [&out](const auto& wb) {
            using T = std::decay_t<decltype(wb)>;
            if constexpr (std::is_same_v<T, engine::KelvinWhiteBalance>) {
                out.white_balance = WhiteBalanceMode::Kelvin;
                out.temperature_k = wb.temperature_k;
                out.tint = wb.tint;
            } else if constexpr (std::is_same_v<T, engine::CameraNeutralWhiteBalance>) {
                out.white_balance = WhiteBalanceMode::CameraNeutral;
                out.neutral = {wb.r, wb.g, wb.b};
            } else {
                out.white_balance = WhiteBalanceMode::AsShot;
            }
        },
        settings.white_balance);
    out.exposure_ev = settings.exposure_ev;
    out.contrast = settings.contrast;
    out.highlights = settings.highlights;
    out.shadows = settings.shadows;
    out.saturation = settings.saturation;
    out.vibrance = settings.vibrance;
    out.tone_curve.clear();
    for (const engine::ToneCurvePoint& point : settings.tone_curve.points) {
        out.tone_curve << QPointF(point.x, point.y);
    }
    for (const engine::DevelopControl control : settings.locks) {
        out.locks |= control_bit(to_edit_control(control));
    }
    return out;
}

engine::DevelopSettings to_develop_settings(const EditSettings& settings, const engine::DevelopSettings& base) {
    engine::DevelopSettings out = base;
    switch (settings.white_balance) {
        case WhiteBalanceMode::Kelvin:
            out.white_balance = engine::KelvinWhiteBalance{settings.temperature_k, settings.tint};
            break;
        case WhiteBalanceMode::CameraNeutral:
            out.white_balance =
                engine::CameraNeutralWhiteBalance{settings.neutral[0], settings.neutral[1], settings.neutral[2]};
            break;
        case WhiteBalanceMode::AsShot:
        default:
            out.white_balance = engine::AsShotWhiteBalance{};
            break;
    }
    out.exposure_ev = settings.exposure_ev;
    out.contrast = settings.contrast;
    out.highlights = settings.highlights;
    out.shadows = settings.shadows;
    out.saturation = settings.saturation;
    out.vibrance = settings.vibrance;
    out.tone_curve.points.clear();
    for (const QPointF& point : settings.tone_curve) {
        out.tone_curve.points.push_back(engine::ToneCurvePoint{point.x(), point.y()});
    }
    out.locks.clear();
    for (const engine::DevelopControl control : app::kAllDevelopControls) {
        if ((settings.locks & control_bit(to_edit_control(control))) != 0) {
            out.locks.insert(control);
        }
    }
    return out;
}

engine::DevelopControl to_develop_control(EditControl control) noexcept {
    return static_cast<engine::DevelopControl>(static_cast<std::uint8_t>(control));
}

EditControl to_edit_control(engine::DevelopControl control) noexcept {
    return static_cast<EditControl>(static_cast<std::uint8_t>(control));
}

std::set<app::RecipeCategory> to_recipe_categories(quint32 bits) {
    std::set<app::RecipeCategory> categories;
    for (const app::RecipeCategory category : app::kAllRecipeCategories) {
        if ((bits & category_bit(to_settings_category(category))) != 0) {
            categories.insert(category);
        }
    }
    return categories;
}

SettingsCategory to_settings_category(app::RecipeCategory category) noexcept {
    return static_cast<SettingsCategory>(static_cast<std::uint8_t>(category));
}

// ---- State and history ---------------------------------------------------------------------------

EditState to_edit_state(const app::RecipeState& state, bool store_read_only) {
    EditState out;
    out.item = state.item.value;
    out.revision = state.revision ? static_cast<quint64>(state.revision->value) : 0;
    out.revision_number = state.revision_number;
    out.transient = state.transient;
    out.transient_generation = state.transient_generation;
    out.can_undo = state.can_undo;
    out.can_redo = state.can_redo;
    if (!state.recipe_unreadable) {
        out.settings = to_edit_settings(state.settings);
        out.committed = to_edit_settings(state.committed);
    }
    if (state.read_only) {
        out.read_only = true;
        out.read_only_reason = describe_recipe_problem(*state.read_only, state.recipe_unreadable);
    }
    out.history_available = !store_read_only;
    out.edited = state.revision.has_value() && (state.recipe_unreadable || !state.committed.is_as_shot());
    return out;
}

QList<EditHistoryEntry> to_edit_history(const app::RecipeHistory& history) {
    QList<EditHistoryEntry> entries;
    entries.reserve(static_cast<qsizetype>(history.entries.size()));
    for (const app::HistoryEntry& entry : history.entries) {
        EditHistoryEntry out;
        out.revision = static_cast<quint64>(entry.info.id.value);
        out.number = entry.info.revision_number;
        if (entry.info.parent) {
            for (const app::HistoryEntry& other : history.entries) {
                if (other.info.id == *entry.info.parent) {
                    out.parent_number = other.info.revision_number;
                    break;
                }
            }
        }
        out.origin = origin_key(entry.info.origin);
        out.created = QDateTime::fromMSecsSinceEpoch(
            static_cast<qint64>(entry.info.created_at.time_since_epoch().count()), QTimeZone::UTC);
        for (const engine::DevelopControl control : entry.changed) {
            out.changed << to_edit_control(control);
        }
        for (const engine::DevelopControl control : entry.locks) {
            out.locks << to_edit_control(control);
        }
        out.reverted_from = entry.reverted_from.value_or(0);
        out.current = entry.current;
        out.on_current_path = entry.on_current_path;
        entries << out;
    }
    return entries;
}

QString describe_recipe_problem(const Error& error, bool recipe_unreadable) {
    if (recipe_unreadable) {
        if (error.code == ErrorCode::VersionMismatch) {
            return tr_text(
                "This photo's edit was made by a newer version of Lucent and is shown read-only. Update "
                "Lucent to change it, or go back to an earlier revision.");
        }
        return tr_text(
            "This photo's edit record is damaged and cannot be read, so it is shown read-only. Go back to "
            "an earlier revision or reset it to as shot.");
    }
    if (error.code == ErrorCode::VersionMismatch) {
        return tr_text(
            "This library was saved by a newer version of Lucent and is open read-only, so edits cannot be "
            "saved.");
    }
    return tr_text("This photo cannot be edited: %1").arg(describe_error(error));
}

// ---- Camera ------------------------------------------------------------------------------------------

WhiteBalanceInfo unknown_white_balance_info() {
    WhiteBalanceInfo info;
    info.explanation = tr_text("The camera's white balance could not be read, so it cannot be adjusted.");
    return info;
}

WhiteBalanceInfo white_balance_info(const raw::RawMetadata& metadata) {
    auto model = engine::WhiteBalanceModel::create(metadata);
    if (!model) {
        return unknown_white_balance_info();
    }
    WhiteBalanceInfo info;
    info.known = true;
    info.monochrome = model->monochrome();
    const engine::RgbMultipliers& as_shot = model->as_shot();
    info.as_shot_neutral = {as_shot[0], as_shot[1], as_shot[2]};
    if (info.monochrome) {
        info.explanation = tr_text("A monochrome photo has no colour balance to adjust.");
        return info;
    }
    if (model->kelvin_available()) {
        if (auto temperature = model->temperature_for(as_shot); temperature) {
            info.kelvin_available = true;
            info.as_shot_temperature_k = temperature->temperature_k;
            info.as_shot_tint = temperature->tint;
            return info;
        }
    }
    info.explanation = tr_text(
        "This camera's calibration does not define a colour temperature, so Lucent does not invent one. "
        "Adjust the camera's own red–blue and green–magenta balance instead; the values are relative to "
        "the as-shot balance.");
    return info;
}

// ---- Paste -------------------------------------------------------------------------------------------

QString describe_paste_skip(const app::PasteSkip& skip, const app::RecipeClipboard& clipboard,
                            const std::optional<app::CameraCapabilities>& target) {
    const QString category = category_label(to_settings_category(skip.category));
    if (skip.category == app::RecipeCategory::WhiteBalance) {
        switch (engine::mode_of(clipboard.settings.white_balance)) {
            case engine::WhiteBalanceMode::CameraNeutral:
                return tr_text("%1: the copied balance is specific to the %2's sensor and this photo is from %3.")
                    .arg(category, camera_name(clipboard.camera),
                         target ? camera_name(*target) : tr_text("another camera"));
            case engine::WhiteBalanceMode::Kelvin:
                return tr_text("%1: this photo's camera calibration does not define a colour temperature.")
                    .arg(category);
            case engine::WhiteBalanceMode::AsShot:
                break;
        }
    }
    return tr_text("%1: not pasted.").arg(category);
}

// ---- Frames ------------------------------------------------------------------------------------------

QImage clipping_overlay(const app::ClippingMask& mask) {
    if (mask.width <= 0 || mask.height <= 0 ||
        mask.flags.size() != static_cast<std::size_t>(mask.width) * static_cast<std::size_t>(mask.height)) {
        return {};
    }
    QImage overlay(mask.width, mask.height, QImage::Format_ARGB32_Premultiplied);
    if (overlay.isNull()) {
        return {};
    }
    // Premultiplied, alpha 0.9: clearly visible, the photo still faintly underneath.
    const QRgb highlight = qPremultiply(qRgba(255, 40, 40, 230));
    const QRgb shadow = qPremultiply(qRgba(40, 120, 255, 230));
    for (std::int32_t y = 0; y < mask.height; ++y) {
        const std::uint8_t* flags = mask.flags.data() + static_cast<std::size_t>(y) * mask.width;
        auto* row = reinterpret_cast<QRgb*>(overlay.scanLine(y));
        for (std::int32_t x = 0; x < mask.width; ++x) {
            const std::uint8_t f = flags[x];
            row[x] = (f & app::ClippingMask::kHighlight) != 0 ? highlight
                     : (f & app::ClippingMask::kShadow) != 0  ? shadow
                                                              : 0u;
        }
    }
    return overlay;
}

std::shared_ptr<EditFrame> to_edit_frame(const app::DevelopFrame& frame, bool clipping) {
    try {
        auto out = std::make_shared<EditFrame>();
        out->item = frame.item.value;
        out->generation = frame.generation;
        out->revision = frame.revision ? static_cast<quint64>(frame.revision->value) : 0;
        out->transient_generation = frame.transient_generation;
        out->phase = frame.phase == app::DevelopPhase::Final ? FramePhase::Final : FramePhase::Quick;
        out->full_resolution = frame.full_resolution;
        out->image = to_qimage(frame.image);
        if (out->image.isNull()) {
            return nullptr;
        }
        if (clipping) {
            out->clipping = clipping_overlay(frame.clipping);
        }
        auto info = std::const_pointer_cast<RenderInfo>(
            to_render_info(frame.image, frame.histogram, frame.summary, frame.latency_seconds));
        info->clipping_counted = true;
        info->clipped_highlights = frame.clipping.highlight_pixels;
        info->clipped_shadows = frame.clipping.shadow_pixels;
        out->info = std::move(info);
        out->latency_seconds = frame.latency_seconds;
        out->stage_two_seconds = frame.stage_two_seconds;
        out->display_seconds = frame.display_seconds;
        return out;
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

}  // namespace lucent::ui::adapters
