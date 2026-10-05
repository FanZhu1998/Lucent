#include "services/edit_service.hpp"

#include <QCoreApplication>
#include <algorithm>

namespace lucent::ui {
namespace {

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::EditService", text);
}

}  // namespace

bool EditSettings::renders_as_shot() const noexcept {
    const EditSettings defaults;
    return white_balance == WhiteBalanceMode::AsShot && exposure_ev == 0.0 && contrast == 0.0 &&
           highlights == 0.0 && shadows == 0.0 && saturation == 0.0 && vibrance == 0.0 &&
           tone_curve == defaults.tone_curve && chroma_denoise.strength == 0.0 && geometry.is_identity() &&
           std::none_of(masks.cbegin(), masks.cend(), [](const EditMask& mask) { return mask.has_effect(); });
}

QString control_label(EditControl control) {
    switch (control) {
        case EditControl::WhiteBalance:
            return tr_text("White balance");
        case EditControl::Exposure:
            return tr_text("Exposure");
        case EditControl::Contrast:
            return tr_text("Contrast");
        case EditControl::Highlights:
            return tr_text("Highlights");
        case EditControl::Shadows:
            return tr_text("Shadows");
        case EditControl::ToneCurve:
            return tr_text("Tone curve");
        case EditControl::Saturation:
            return tr_text("Saturation");
        case EditControl::Vibrance:
            return tr_text("Vibrance");
        case EditControl::BaseLook:
            return tr_text("Base look");
    }
    return {};
}

QString category_label(SettingsCategory category) {
    switch (category) {
        case SettingsCategory::WhiteBalance:
            return tr_text("White balance");
        case SettingsCategory::Exposure:
            return tr_text("Exposure");
        case SettingsCategory::Tone:
            return tr_text("Tone");
        case SettingsCategory::Color:
            return tr_text("Color");
        case SettingsCategory::Local:
            return tr_text("Highlights, shadows and masks");
        case SettingsCategory::Detail:
            return tr_text("Noise reduction");
        case SettingsCategory::Geometry:
            return tr_text("Crop and rotation");
    }
    return {};
}

QString part_label(EditPart part) {
    switch (part) {
        case EditPart::ChromaDenoise:
            return tr_text("Noise reduction");
        case EditPart::Masks:
            return tr_text("Masks");
        case EditPart::Geometry:
            return tr_text("Crop and rotation");
    }
    return {};
}

QString mask_kind_label(MaskKind kind) {
    switch (kind) {
        case MaskKind::Brush:
            return tr_text("Brush");
        case MaskKind::Linear:
            return tr_text("Linear gradient");
        case MaskKind::Radial:
            return tr_text("Radial gradient");
    }
    return {};
}

}  // namespace lucent::ui
