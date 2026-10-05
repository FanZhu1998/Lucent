#include "models/edit_session.hpp"

#include <QCoreApplication>
#include <QLocale>
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

namespace lucent::ui {
namespace {

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::EditSession", text);
}

enum class Scale : std::uint8_t {
    Linear,
    Reciprocal,   // equal slider distances are equal distances in 1/value (mired for temperature)
    Logarithmic,  // equal slider distances are equal ratios (brush size, noise radius)
};

struct ControlSpec {
    const char* key;
    const char* label;
    double minimum;
    double maximum;
    double step;
    double coarse;
    int decimals;
    const char* unit;
    EditControl lock;
    bool signed_value;
    Scale scale;
    bool lockable = true;  // W9's controls have no lock (Auto never changes them; ADR 0015)
    double default_value = 0.0;
};

enum class KeyGroup : std::uint8_t {
    Global,  // the photo's controls (white balance .. vibrance, straighten, noise reduction)
    Mask,    // the selected mask's
    Tool,    // the brush: tool settings, not part of the recipe
};

KeyGroup group_of(const QString& key) {
    if (key.startsWith(u"mask")) {
        return KeyGroup::Mask;
    }
    if (key.startsWith(u"brush")) {
        return KeyGroup::Tool;
    }
    return KeyGroup::Global;
}

// Display units: exposure in EV, temperature in kelvin, tint in DNG units, the others -100..+100
// (the settings' -1..+1; for the camera channels, +-100 is +-2 EV red against blue and +-1 EV
// red and blue against green).
constexpr std::array kControls{
    ControlSpec{"temperature", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Temperature"), kMinTemperature,
                kMaxTemperature, 50.0, 500.0, 0, "K", EditControl::WhiteBalance, false, Scale::Reciprocal},
    ControlSpec{"tint", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Tint"), -kTintLimit, kTintLimit, 1.0, 10.0, 0,
                "", EditControl::WhiteBalance, true, Scale::Linear},
    ControlSpec{"redBlue", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Blue–amber"), -100.0, 100.0, 1.0, 10.0, 0,
                "", EditControl::WhiteBalance, true, Scale::Linear},
    ControlSpec{"greenMagenta", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Green–magenta"), -100.0, 100.0, 1.0,
                10.0, 0, "", EditControl::WhiteBalance, true, Scale::Linear},
    ControlSpec{"exposure", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Exposure"), -kExposureLimit,
                kExposureLimit, 0.05, 0.5, 2, "EV", EditControl::Exposure, true, Scale::Linear},
    ControlSpec{"contrast", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Contrast"), -100.0, 100.0, 1.0, 10.0, 0, "",
                EditControl::Contrast, true, Scale::Linear},
    ControlSpec{"highlights", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Highlights"), -100.0, 100.0, 1.0, 10.0,
                0, "", EditControl::Highlights, true, Scale::Linear},
    ControlSpec{"shadows", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Shadows"), -100.0, 100.0, 1.0, 10.0, 0, "",
                EditControl::Shadows, true, Scale::Linear},
    ControlSpec{"saturation", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Saturation"), -100.0, 100.0, 1.0, 10.0,
                0, "", EditControl::Saturation, true, Scale::Linear},
    ControlSpec{"vibrance", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Vibrance"), -100.0, 100.0, 1.0, 10.0, 0, "",
                EditControl::Vibrance, true, Scale::Linear},
    // W9 (ADR 0015).
    ControlSpec{"straighten", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Straighten"), -kMaxStraighten,
                kMaxStraighten, 0.1, 1.0, 1, "°", EditControl::BaseLook, true, Scale::Linear, false, 0.0},
    ControlSpec{"noiseReduction", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Amount"), 0.0, 100.0, 1.0, 10.0, 0,
                "", EditControl::BaseLook, false, Scale::Linear, false, 0.0},
    ControlSpec{"noiseRadius", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Radius"), kMinDenoiseRadius,
                kMaxDenoiseRadius, 1.0, 4.0, 0, "px", EditControl::BaseLook, false, Scale::Logarithmic, false,
                kDefaultDenoiseRadius},
    ControlSpec{"maskExposure", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Exposure"), -kMaxLocalExposure,
                kMaxLocalExposure, 0.05, 0.5, 2, "EV", EditControl::BaseLook, true, Scale::Linear, false, 0.0},
    ControlSpec{"maskContrast", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Contrast"), -100.0, 100.0, 1.0, 10.0, 0,
                "", EditControl::BaseLook, true, Scale::Linear, false, 0.0},
    ControlSpec{"maskHighlights", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Highlights"), -100.0, 100.0, 1.0,
                10.0, 0, "", EditControl::BaseLook, true, Scale::Linear, false, 0.0},
    ControlSpec{"maskShadows", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Shadows"), -100.0, 100.0, 1.0, 10.0, 0,
                "", EditControl::BaseLook, true, Scale::Linear, false, 0.0},
    ControlSpec{"maskSaturation", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Saturation"), -100.0, 100.0, 1.0,
                10.0, 0, "", EditControl::BaseLook, true, Scale::Linear, false, 0.0},
    ControlSpec{"maskTemperature", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Temperature"), -100.0, 100.0, 1.0,
                10.0, 0, "", EditControl::BaseLook, true, Scale::Linear, false, 0.0},
    ControlSpec{"maskTint", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Tint"), -100.0, 100.0, 1.0, 10.0, 0, "",
                EditControl::BaseLook, true, Scale::Linear, false, 0.0},
    ControlSpec{"maskOpacity", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Opacity"), 0.0, 100.0, 1.0, 10.0, 0, "%",
                EditControl::BaseLook, false, Scale::Linear, false, 100.0},
    ControlSpec{"maskFeather", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Feather"), 0.0, 100.0, 1.0, 10.0, 0, "",
                EditControl::BaseLook, false, Scale::Linear, false, 50.0},
    ControlSpec{"brushSize", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Size"), 0.1, 25.0, 0.1, 1.0, 1, "%",
                EditControl::BaseLook, false, Scale::Logarithmic, false, 2.0},
    ControlSpec{"brushFeather", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Feather"), 0.0, 100.0, 1.0, 10.0, 0, "",
                EditControl::BaseLook, false, Scale::Linear, false, 50.0},
    ControlSpec{"brushFlow", QT_TRANSLATE_NOOP("lucent::ui::EditSession", "Flow"), 1.0, 100.0, 1.0, 10.0, 0, "",
                EditControl::BaseLook, false, Scale::Linear, false, 100.0},
};

const EditMask* mask_in(const EditSettings& settings, quint32 id) {
    for (const EditMask& mask : settings.masks) {
        if (mask.id == id) {
            return &mask;
        }
    }
    return nullptr;
}

const ControlSpec* spec_of(const QString& key) {
    for (const ControlSpec& spec : kControls) {
        if (key == QLatin1StringView(spec.key)) {
            return &spec;
        }
    }
    return nullptr;
}

struct LockSpec {
    const char* key;
    EditControl control;
};

constexpr std::array kLocks{
    LockSpec{"whiteBalance", EditControl::WhiteBalance}, LockSpec{"exposure", EditControl::Exposure},
    LockSpec{"contrast", EditControl::Contrast},         LockSpec{"highlights", EditControl::Highlights},
    LockSpec{"shadows", EditControl::Shadows},           LockSpec{"toneCurve", EditControl::ToneCurve},
    LockSpec{"saturation", EditControl::Saturation},     LockSpec{"vibrance", EditControl::Vibrance},
};

std::optional<EditControl> lock_of(const QString& key) {
    for (const LockSpec& lock : kLocks) {
        if (key == QLatin1StringView(lock.key)) {
            return lock.control;
        }
    }
    return std::nullopt;
}

QString lock_key(EditControl control) {
    for (const LockSpec& lock : kLocks) {
        if (lock.control == control) {
            return QString::fromLatin1(lock.key);
        }
    }
    return {};
}

struct CategorySpec {
    const char* key;
    SettingsCategory category;
};

constexpr std::array kCategories{
    CategorySpec{"whiteBalance", SettingsCategory::WhiteBalance}, CategorySpec{"exposure", SettingsCategory::Exposure},
    CategorySpec{"tone", SettingsCategory::Tone},                 CategorySpec{"color", SettingsCategory::Color},
    CategorySpec{"local", SettingsCategory::Local},               CategorySpec{"detail", SettingsCategory::Detail},
    CategorySpec{"geometry", SettingsCategory::Geometry},
};

double round_to(double value, int decimals) {
    const double factor = std::pow(10.0, decimals);
    const double rounded = std::round(value * factor) / factor;
    return rounded == 0.0 ? 0.0 : rounded;  // never -0
}

bool is_white_balance_key(const QString& key) {
    return key == u"temperature" || key == u"tint" || key == u"redBlue" || key == u"greenMagenta";
}

constexpr int kMaxRememberedTokens = 1024;

}  // namespace

EditSession::EditSession(EditService& service, std::shared_ptr<FrameStore> store, QObject* parent)
    : QObject(parent), service_(service), store_(std::move(store)), history_(std::make_unique<EditHistoryModel>()) {
    automatic_ = std::make_unique<AutoSettingsModel>(
        service_, [this](EditControl control) { return describe_committed(control); });
    connect(automatic_.get(), &AutoSettingsModel::runningChanged, this, &EditSession::canRunAutoChanged);
    connect(automatic_.get(), &AutoSettingsModel::finished, this, &EditSession::notice);
    clock_.start();
    nudge_timer_.setSingleShot(true);
    nudge_timer_.setInterval(kNudgeCommitMs);
    connect(&nudge_timer_, &QTimer::timeout, this, &EditSession::endNudge);
    connect(&service_, &EditService::state_changed, this, &EditSession::on_state);
    connect(&service_, &EditService::history_changed, this, &EditSession::on_history);
    connect(&service_, &EditService::white_balance_changed, this, &EditSession::on_white_balance);
    connect(&service_, &EditService::frame_ready, this, &EditSession::on_frame);
    connect(&service_, &EditService::frame_failed, this, &EditSession::on_frame_failed);
    connect(&service_, &EditService::command_failed, this, &EditSession::on_command_failed);
    connect(&service_, &EditService::copied, this, &EditSession::on_copied);
    connect(&service_, &EditService::pasted, this, &EditSession::on_pasted);
    connect(&service_, &EditService::busy_changed, this, &EditSession::busyChanged);
    curve_samples_ = service_.sample_tone_curve(settings_.tone_curve, kCurveSamples);
    crop_ = std::make_unique<CropTool>(*this);
    masks_ = std::make_unique<MaskEditor>(*this);
}

EditSession::~EditSession() = default;

bool EditSession::is_control(const QString& key) {
    return spec_of(key) != nullptr;
}

// ---- Item ------------------------------------------------------------------------------------------

void EditSession::set_item(ItemId item) {
    if (item == item_) {
        return;
    }
    if (item_ != 0) {
        endNudge();
        endGesture();
        service_.close(item_);
        store_->remove(item_);
    }
    item_ = item;
    const bool had_tool = !tool_.isEmpty();
    tool_.clear();
    selected_mask_ = 0;
    state_.reset();
    answered_token_ = last_token_;
    paste_pending_ = false;
    white_balance_ = WhiteBalanceInfo{};
    history_->clear();
    selected_point_ = -1;
    reset_frames();
    set_local(EditSettings{});
    automatic_->set_item(item_);
    if (item_ != 0) {
        service_.open(item_);
        send_comparison();
        if (full_resolution_) {
            service_.set_full_resolution(item_, true);
        }
    }
    if (had_tool) {
        emit toolChanged();
    }
    emit maskSelectionChanged();
    emit activeChanged();
    emit stateChanged();
    emit whiteBalanceChanged();
    emit selectedCurvePointChanged();
    emit canRunAutoChanged();
}

void EditSession::reset_frames() {
    frame_source_.clear();
    before_source_.clear();
    clipping_source_.clear();
    mask_overlay_source_.clear();
    frame_full_upright_ = QSize();
    frame_geometry_ = EditGeometry{};
    frame_map_ = GeometryMap{};
    frame_uncropped_ = false;
    frame_masks_.clear();
    frame_info_.reset();
    frame_full_resolution_ = false;
    frame_error_.clear();
    pending_shown_.clear();
    emit frameChanged();
}

// ---- Controls ----------------------------------------------------------------------------------------

std::array<double, 3> EditSession::as_shot_neutral() const {
    std::array<double, 3> neutral = white_balance_.as_shot_neutral;
    if (!(neutral[1] > 0.0)) {
        return {1.0, 1.0, 1.0};
    }
    return {neutral[0] / neutral[1], 1.0, neutral[2] / neutral[1]};
}

double EditSession::display_value(const EditSettings& settings, const QString& key) const {
    const ControlSpec* spec = spec_of(key);
    if (spec == nullptr) {
        return 0.0;
    }
    if (key == u"temperature" || key == u"tint") {
        if (settings.white_balance == WhiteBalanceMode::Kelvin) {
            return key == u"temperature" ? settings.temperature_k : settings.tint;
        }
        if (white_balance_.kelvin_available) {
            const double as_shot =
                key == u"temperature" ? white_balance_.as_shot_temperature_k : white_balance_.as_shot_tint;
            return std::clamp(round_to(as_shot, spec->decimals), spec->minimum, spec->maximum);
        }
        return key == u"temperature" ? 5000.0 : 0.0;
    }
    if (key == u"redBlue" || key == u"greenMagenta") {
        if (settings.white_balance != WhiteBalanceMode::CameraNeutral || !(settings.neutral[1] > 0.0)) {
            return 0.0;
        }
        const std::array<double, 3> as_shot = as_shot_neutral();
        const double red = std::log2(settings.neutral[0] / settings.neutral[1] / as_shot[0]);
        const double blue = std::log2(settings.neutral[2] / settings.neutral[1] / as_shot[2]);
        const double value = key == u"redBlue" ? (red - blue) * 50.0 : (red + blue) * 100.0;
        return std::clamp(round_to(value, spec->decimals), spec->minimum, spec->maximum);
    }
    if (key == u"exposure") {
        return settings.exposure_ev;
    }
    if (key == u"straighten") {
        return settings.geometry.straighten_degrees;
    }
    if (key == u"noiseReduction") {
        return round_to(settings.chroma_denoise.strength * 100.0, 6);
    }
    if (key == u"noiseRadius") {
        return settings.chroma_denoise.radius;
    }
    switch (group_of(key)) {
        case KeyGroup::Tool:
            return key == u"brushSize" ? brush_size_ : key == u"brushFeather" ? brush_feather_ : brush_flow_;
        case KeyGroup::Mask: {
            const EditMask* mask = mask_in(settings, selected_mask_);
            if (mask == nullptr) {
                return spec->default_value;
            }
            const EditLocalControls& c = mask->controls;
            if (key == u"maskExposure") {
                return c.exposure_ev;
            }
            const double unit = key == u"maskContrast"      ? c.contrast
                                : key == u"maskHighlights"  ? c.highlights
                                : key == u"maskShadows"     ? c.shadows
                                : key == u"maskSaturation"  ? c.saturation
                                : key == u"maskTemperature" ? c.temperature
                                : key == u"maskTint"        ? c.tint
                                : key == u"maskOpacity"     ? mask->opacity
                                                            : mask->feather;
            return round_to(unit * 100.0, 6);
        }
        case KeyGroup::Global:
            break;
    }
    const double unit = key == u"contrast"     ? settings.contrast
                        : key == u"highlights" ? settings.highlights
                        : key == u"shadows"    ? settings.shadows
                        : key == u"saturation" ? settings.saturation
                                               : settings.vibrance;
    return round_to(unit * 100.0, 6);
}

EditSettings EditSession::with_value(const EditSettings& settings, const QString& key, double value) const {
    const ControlSpec* spec = spec_of(key);
    EditSettings next = settings;
    if (spec == nullptr || !std::isfinite(value)) {
        return next;
    }
    const double v = std::clamp(round_to(value, spec->decimals), spec->minimum, spec->maximum);
    if (key == u"temperature" || key == u"tint") {
        if (next.white_balance != WhiteBalanceMode::Kelvin) {
            next.white_balance = WhiteBalanceMode::Kelvin;
            next.temperature_k = display_value(settings, QStringLiteral("temperature"));
            next.tint = display_value(settings, QStringLiteral("tint"));
        }
        (key == u"temperature" ? next.temperature_k : next.tint) = v;
        return next;
    }
    if (key == u"redBlue" || key == u"greenMagenta") {
        double warmth = display_value(settings, QStringLiteral("redBlue")) / 50.0;
        double tint = display_value(settings, QStringLiteral("greenMagenta")) / 100.0;
        (key == u"redBlue" ? warmth : tint) = key == u"redBlue" ? v / 50.0 : v / 100.0;
        const std::array<double, 3> as_shot = as_shot_neutral();
        next.white_balance = WhiteBalanceMode::CameraNeutral;
        next.neutral = {as_shot[0] * std::exp2((warmth + tint) / 2.0), 1.0, as_shot[2] * std::exp2((tint - warmth) / 2.0)};
        return next;
    }
    if (key == u"exposure") {
        next.exposure_ev = v;
        return next;
    }
    if (key == u"straighten") {
        next.geometry = crop_->straightened(settings.geometry, v);
        return next;
    }
    if (key == u"noiseReduction") {
        next.chroma_denoise.strength = v / 100.0;
        return next;
    }
    if (key == u"noiseRadius") {
        next.chroma_denoise.radius = v;
        return next;
    }
    switch (group_of(key)) {
        case KeyGroup::Tool:
            return next;  // not part of the settings (set_tool_value)
        case KeyGroup::Mask: {
            EditMask* mask = nullptr;
            for (EditMask& candidate : next.masks) {
                if (candidate.id == selected_mask_) {
                    mask = &candidate;
                }
            }
            if (mask == nullptr) {
                return next;
            }
            EditLocalControls& c = mask->controls;
            const double fraction = v / 100.0;
            if (key == u"maskExposure") {
                c.exposure_ev = v;
            } else if (key == u"maskContrast") {
                c.contrast = fraction;
            } else if (key == u"maskHighlights") {
                c.highlights = fraction;
            } else if (key == u"maskShadows") {
                c.shadows = fraction;
            } else if (key == u"maskSaturation") {
                c.saturation = fraction;
            } else if (key == u"maskTemperature") {
                c.temperature = fraction;
            } else if (key == u"maskTint") {
                c.tint = fraction;
            } else if (key == u"maskOpacity") {
                mask->opacity = fraction;
            } else if (key == u"maskFeather" && mask->kind != MaskKind::Brush) {
                mask->feather = fraction;
            }
            return next;
        }
        case KeyGroup::Global:
            break;
    }
    const double unit = v / 100.0;
    if (key == u"contrast") {
        next.contrast = unit;
    } else if (key == u"highlights") {
        next.highlights = unit;
    } else if (key == u"shadows") {
        next.shadows = unit;
    } else if (key == u"saturation") {
        next.saturation = unit;
    } else {
        next.vibrance = unit;
    }
    return next;
}

QVariantMap EditSession::control(const QString& key) const {
    const ControlSpec* spec = spec_of(key);
    if (spec == nullptr) {
        return {};
    }
    const double default_value = default_of(key);
    return QVariantMap{
        {QStringLiteral("label"), tr_text(spec->label)},
        {QStringLiteral("minimum"), spec->minimum},
        {QStringLiteral("maximum"), spec->maximum},
        {QStringLiteral("step"), spec->step},
        {QStringLiteral("coarseStep"), spec->coarse},
        {QStringLiteral("decimals"), spec->decimals},
        {QStringLiteral("unit"), QString::fromLatin1(spec->unit)},
        {QStringLiteral("defaultValue"), default_value},
        {QStringLiteral("lockKey"), spec->lockable ? lock_key(spec->lock) : QString()},
        // The slider runs in position() (not linear in the display value).
        {QStringLiteral("reciprocal"), spec->scale != Scale::Linear},
    };
}

double EditSession::default_of(const QString& key) const {
    const ControlSpec* spec = spec_of(key);
    if (spec == nullptr) {
        return 0.0;
    }
    if (key == u"temperature" || key == u"tint") {
        return display_value(EditSettings{}, key);
    }
    if (key == u"maskFeather") {
        const EditMask* mask = mask_in(settings_, selected_mask_);
        return mask != nullptr && mask->kind == MaskKind::Linear ? 100.0 : 50.0;
    }
    return spec->default_value;
}

void EditSession::set_tool_value(const QString& key, double value) {
    const ControlSpec* spec = spec_of(key);
    if (spec == nullptr || !std::isfinite(value)) {
        return;
    }
    const double v = std::clamp(round_to(value, spec->decimals), spec->minimum, spec->maximum);
    double& target = key == u"brushSize" ? brush_size_ : key == u"brushFeather" ? brush_feather_ : brush_flow_;
    if (target == v) {
        return;
    }
    target = v;
    ++settings_revision_;
    emit settingsChanged();
}

double EditSession::value(const QString& key) const {
    return display_value(settings_, key);
}

QString EditSession::valueText(const QString& key) const {
    if (spec_of(key) == nullptr) {
        return {};
    }
    if (!settings_known()) {
        return QStringLiteral("–");
    }
    return value_text_of(settings_, key);
}

QString EditSession::value_text_of(const EditSettings& settings, const QString& key) const {
    const ControlSpec* spec = spec_of(key);
    if (spec == nullptr) {
        return {};
    }
    const double v = round_to(display_value(settings, key), spec->decimals);
    QLocale locale;
    locale.setNumberOptions(QLocale::OmitGroupSeparator);
    QString text = locale.toString(v, 'f', spec->decimals);
    if (spec->signed_value && v > 0.0) {
        text.prepend(u'+');
    }
    return text;
}

double EditSession::position(const QString& key) const {
    const ControlSpec* spec = spec_of(key);
    if (spec == nullptr) {
        return 0.0;
    }
    const double v = display_value(settings_, key);
    if (spec->scale == Scale::Reciprocal) {
        const double low = 1.0 / spec->minimum;
        const double high = 1.0 / spec->maximum;
        return std::clamp((low - 1.0 / v) / (low - high), 0.0, 1.0);
    }
    if (spec->scale == Scale::Logarithmic) {
        return std::clamp(std::log(std::max(v, spec->minimum) / spec->minimum) / std::log(spec->maximum / spec->minimum),
                          0.0, 1.0);
    }
    return std::clamp((v - spec->minimum) / (spec->maximum - spec->minimum), 0.0, 1.0);
}

void EditSession::setPosition(const QString& key, double position) {
    const ControlSpec* spec = spec_of(key);
    if (spec == nullptr || !std::isfinite(position)) {
        return;
    }
    const double p = std::clamp(position, 0.0, 1.0);
    double v = 0.0;
    if (spec->scale == Scale::Reciprocal) {
        const double low = 1.0 / spec->minimum;
        const double high = 1.0 / spec->maximum;
        v = 1.0 / (low - p * (low - high));
    } else if (spec->scale == Scale::Logarithmic) {
        v = spec->minimum * std::pow(spec->maximum / spec->minimum, p);
    } else {
        v = spec->minimum + p * (spec->maximum - spec->minimum);
    }
    setValue(key, v);
}

void EditSession::setValue(const QString& key, double value) {
    if (spec_of(key) == nullptr) {
        return;
    }
    if (group_of(key) == KeyGroup::Tool) {
        set_tool_value(key, value);
        return;
    }
    change(with_value(settings_, key, value));
}

bool EditSession::setValueText(const QString& key, const QString& text) {
    const ControlSpec* spec = spec_of(key);
    if (spec == nullptr) {
        return false;
    }
    QString cleaned = text.trimmed();
    const QString unit = QString::fromLatin1(spec->unit);
    if (!unit.isEmpty() && cleaned.endsWith(unit, Qt::CaseInsensitive)) {
        cleaned.chop(unit.size());
        cleaned = cleaned.trimmed();
    }
    if (cleaned.startsWith(u'+')) {
        cleaned.remove(0, 1);
    }
    cleaned.replace(QChar(0x2212), u'-');  // a typographic minus
    bool ok = false;
    double v = QLocale().toDouble(cleaned, &ok);
    if (!ok) {
        v = QLocale::c().toDouble(cleaned, &ok);
    }
    if (!ok || !std::isfinite(v)) {
        emit settingsChanged();  // the field shows the value again
        return false;
    }
    endNudge();
    commit_gesture();
    setValue(key, v);
    // The field shows the value as stored (rounded, clamped) even when nothing changed.
    ++settings_revision_;
    emit settingsChanged();
    return true;
}

void EditSession::resetValue(const QString& key) {
    if (is_white_balance_key(key)) {
        resetWhiteBalance();
        return;
    }
    if (spec_of(key) == nullptr) {
        return;
    }
    endNudge();
    commit_gesture();
    setValue(key, default_of(key));
}

void EditSession::resetWhiteBalance() {
    endNudge();
    commit_gesture();
    EditSettings next = settings_;
    next.white_balance = WhiteBalanceMode::AsShot;
    change(next);
}

void EditSession::beginGesture() {
    if (!can_edit() || pointer_down_) {
        return;
    }
    endNudge();  // a key gesture ends where a pointer one begins
    pointer_down_ = true;
    start_gesture();
}

void EditSession::endGesture() {
    if (!pointer_down_) {
        return;  // no pointer gesture (a key's gesture ends with endNudge())
    }
    pointer_down_ = false;
    commit_gesture();
}

void EditSession::start_gesture() {
    if (gesture_) {
        return;
    }
    gesture_ = true;
    gesture_changed_ = false;
    emit gestureChanged();
}

void EditSession::commit_gesture() {
    if (!gesture_) {
        return;
    }
    if (gesture_changed_ && item_ != 0) {
        remember_token(service_.commit(item_));
    }
    gesture_changed_ = false;
    if (!pointer_down_) {
        gesture_ = false;
        emit gestureChanged();
    }
}

void EditSession::nudge(const QString& key, int steps, bool coarse) {
    const ControlSpec* spec = spec_of(key);
    if (spec == nullptr || steps == 0 || !can_edit()) {
        return;
    }
    if (!nudging_) {
        if (gesture_) {
            return;  // a drag is in progress
        }
        start_gesture();
        nudging_ = true;
    }
    const double step = coarse ? spec->coarse : spec->step;
    setValue(key, display_value(settings_, key) + steps * step);
    nudge_timer_.start();
}

void EditSession::endNudge() {
    if (!nudging_) {
        return;
    }
    nudging_ = false;
    nudge_timer_.stop();
    commit_gesture();
}

bool EditSession::isLocked(const QString& key) const {
    const std::optional<EditControl> control = lock_of(key);
    return control && settings_.locked(*control);
}

void EditSession::setLocked(const QString& key, bool locked) {
    const std::optional<EditControl> control = lock_of(key);
    if (!control || !can_edit() || item_ == 0 || settings_.locked(*control) == locked) {
        return;
    }
    endNudge();
    commit_gesture();
    EditSettings next = settings_;
    next.set_locked(*control, locked);
    set_local(next);
    remember_token(service_.set_lock(item_, *control, locked));
}

void EditSession::change(const EditSettings& next) {
    if (!can_edit() || item_ == 0 || next == settings_) {
        return;
    }
    if (awaiting_history()) {
        // The values this change builds on are about to be replaced by an undo, redo, selected
        // revision, reset or paste: sending it would silently re-apply what that command removed.
        ++settings_revision_;  // the controls show the values held again
        emit settingsChanged();
        return;
    }
    set_local(next);
    if (gesture_) {
        gesture_changed_ = true;
        remember_token(service_.set_transient(item_, next));
    } else {
        remember_token(service_.apply(item_, next));
    }
}

void EditSession::set_local(const EditSettings& settings) {
    const bool curve_changed = settings.tone_curve != settings_.tone_curve || curve_samples_.isEmpty();
    settings_ = settings;
    validate_selection();
    ++settings_revision_;
    if (curve_changed) {
        curve_samples_ = service_.sample_tone_curve(settings_.tone_curve, kCurveSamples);
    }
    if (selected_point_ >= settings_.tone_curve.size()) {
        selected_point_ = -1;
        emit selectedCurvePointChanged();
    }
    emit settingsChanged();
}

void EditSession::remember_history_token(quint64 token) {
    remember_token(token);
    history_token_ = std::max(history_token_, token);
}

void EditSession::remember_token(quint64 token) {
    if (token == 0) {
        return;
    }
    last_token_ = std::max(last_token_, token);
    token_times_.insert(token, clock_.elapsed());
    if (token_times_.size() > kMaxRememberedTokens) {
        for (auto it = token_times_.begin(); it != token_times_.end();) {
            it = it.key() + kMaxRememberedTokens / 2 < token ? token_times_.erase(it) : std::next(it);
        }
    }
}

// ---- White balance ------------------------------------------------------------------------------

QString EditSession::white_balance_mode() const {
    if (!settings_known()) {
        return QStringLiteral("unknown");
    }
    switch (settings_.white_balance) {
    case WhiteBalanceMode::Kelvin:
        return QStringLiteral("kelvin");
    case WhiteBalanceMode::CameraNeutral:
        return QStringLiteral("cameraNeutral");
    case WhiteBalanceMode::AsShot:
        break;
    }
    return QStringLiteral("asShot");
}

QString EditSession::white_balance_controls() const {
    if (settings_.white_balance == WhiteBalanceMode::Kelvin) {
        return QStringLiteral("kelvin");
    }
    if (settings_.white_balance == WhiteBalanceMode::CameraNeutral) {
        return QStringLiteral("neutral");
    }
    if (!white_balance_.known || white_balance_.monochrome) {
        return QStringLiteral("none");
    }
    return white_balance_.kelvin_available ? QStringLiteral("kelvin") : QStringLiteral("neutral");
}

QString EditSession::white_balance_note() const {
    if (!ready()) {
        return {};
    }
    return white_balance_.explanation;
}

// ---- Tone curve -----------------------------------------------------------------------------------

QVariantList EditSession::curve_points() const {
    QVariantList points;
    if (!settings_known()) {
        return points;
    }
    for (const QPointF& point : settings_.tone_curve) {
        points << point;
    }
    return points;
}

void EditSession::set_selected_curve_point(int index) {
    const int valid = index >= 0 && index < settings_.tone_curve.size() ? index : -1;
    if (valid == selected_point_) {
        return;
    }
    selected_point_ = valid;
    emit selectedCurvePointChanged();
}

QPointF EditSession::constrain_curve_point(const QList<QPointF>& points, int index, QPointF wanted) {
    if (index < 0 || index >= points.size()) {
        return wanted;
    }
    double x_low = 0.0;
    double x_high = 1.0;
    double y_low = 0.0;
    double y_high = 1.0;
    if (index > 0) {
        x_low = points[index - 1].x() + kMinCurveGap;
        y_low = points[index - 1].y();
    }
    if (index + 1 < points.size()) {
        x_high = points[index + 1].x() - kMinCurveGap;
        y_high = points[index + 1].y();
    }
    const double wx = std::isfinite(wanted.x()) ? wanted.x() : points[index].x();
    const double wy = std::isfinite(wanted.y()) ? wanted.y() : points[index].y();
    // Four decimals keep recipes tidy; clamping again after rounding keeps the bounds exact.
    double x = points[index].x();
    if (x_low <= x_high) {
        x = std::clamp(round_to(std::clamp(wx, x_low, x_high), 4), x_low, x_high);
    }
    const double y = std::clamp(round_to(std::clamp(wy, y_low, y_high), 4), y_low, y_high);
    return QPointF(x, y);
}

double EditSession::curveValueAt(double x) const {
    if (curve_samples_.size() < 2 || !std::isfinite(x)) {
        return std::clamp(x, 0.0, 1.0);
    }
    const double at = std::clamp(x, 0.0, 1.0) * double(curve_samples_.size() - 1);
    const auto below = static_cast<qsizetype>(std::floor(at));
    const qsizetype above = std::min(below + 1, curve_samples_.size() - 1);
    const double t = at - double(below);
    return curve_samples_[below] * (1.0 - t) + curve_samples_[above] * t;
}

int EditSession::addCurvePoint(double x) {
    if (!can_edit() || !std::isfinite(x) || settings_.tone_curve.size() >= kMaxCurvePoints) {
        return -1;
    }
    const double cx = round_to(std::clamp(x, 0.0, 1.0), 4);
    QList<QPointF> points = settings_.tone_curve;
    qsizetype at = 0;
    while (at < points.size() && points[at].x() < cx) {
        ++at;
    }
    if ((at > 0 && cx - points[at - 1].x() < kMinCurveGap) ||
        (at < points.size() && points[at].x() - cx < kMinCurveGap)) {
        return -1;
    }
    double y = curveValueAt(cx);
    if (at > 0) {
        y = std::max(y, points[at - 1].y());
    }
    if (at < points.size()) {
        y = std::min(y, points[at].y());
    }
    points.insert(at, QPointF(cx, round_to(std::clamp(y, 0.0, 1.0), 4)));
    EditSettings next = settings_;
    next.tone_curve = points;
    change(next);
    set_selected_curve_point(static_cast<int>(at));
    return static_cast<int>(at);
}

QPointF EditSession::moveCurvePoint(int index, double x, double y) {
    if (index < 0 || index >= settings_.tone_curve.size()) {
        return {};
    }
    const QPointF moved = constrain_curve_point(settings_.tone_curve, index, QPointF(x, y));
    if (can_edit()) {
        EditSettings next = settings_;
        next.tone_curve[index] = moved;
        change(next);
    }
    return settings_.tone_curve[index];
}

void EditSession::nudgeCurvePoint(int index, double dx, double dy) {
    if (index < 0 || index >= settings_.tone_curve.size() || !can_edit()) {
        return;
    }
    if (!nudging_) {
        if (gesture_) {
            return;
        }
        beginGesture();
        nudging_ = true;
    }
    const QPointF point = settings_.tone_curve[index];
    moveCurvePoint(index, point.x() + dx, point.y() + dy);
    nudge_timer_.start();
}

bool EditSession::removeCurvePoint(int index) {
    if (!can_edit() || index <= 0 || index + 1 >= settings_.tone_curve.size()) {
        return false;  // the end points stay (they can be moved)
    }
    endNudge();
    commit_gesture();
    EditSettings next = settings_;
    next.tone_curve.removeAt(index);
    selected_point_ = -1;
    emit selectedCurvePointChanged();
    change(next);
    return true;
}

void EditSession::resetCurve() {
    endNudge();
    commit_gesture();
    EditSettings next = settings_;
    next.tone_curve = EditSettings{}.tone_curve;
    selected_point_ = -1;
    emit selectedCurvePointChanged();
    change(next);
}

// ---- History, comparison, copy and paste ------------------------------------------------------------

void EditSession::undo() {
    endNudge();
    commit_gesture();
    if (item_ != 0 && can_undo()) {
        remember_history_token(service_.undo(item_));
    }
}

void EditSession::redo() {
    endNudge();
    commit_gesture();
    if (item_ != 0 && can_redo()) {
        remember_history_token(service_.redo(item_));
    }
}

void EditSession::selectRevision(int row) {
    const std::optional<quint64> revision = history_->revision_at(row);
    if (!revision || item_ == 0 || !history_available()) {
        return;
    }
    if (state_ && state_->revision == *revision) {
        return;
    }
    endNudge();
    commit_gesture();
    remember_history_token(service_.select_revision(item_, *revision));
}

void EditSession::resetToAsShot() {
    endNudge();
    commit_gesture();
    if (item_ != 0 && history_available()) {
        remember_history_token(service_.reset_to_as_shot(item_));
    }
}

QVariantList EditSession::categories() const {
    QVariantList list;
    for (const CategorySpec& category : kCategories) {
        list << QVariantMap{
            {QStringLiteral("key"), QString::fromLatin1(category.key)},
            {QStringLiteral("label"), category_label(category.category)},
            // Selected by default: every category but Geometry (a crop rarely belongs on another photo).
            {QStringLiteral("checked"), (kDefaultCategoryBits & category_bit(category.category)) != 0},
        };
    }
    return list;
}

void EditSession::copySettings(const QStringList& category_keys) {
    quint32 bits = 0;
    for (const QString& key : category_keys) {
        for (const CategorySpec& category : kCategories) {
            if (key == QLatin1StringView(category.key)) {
                bits |= category_bit(category.category);
            }
        }
    }
    if (item_ == 0 || bits == 0) {
        return;
    }
    endNudge();
    commit_gesture();
    service_.copy_settings(item_, bits);
}

void EditSession::pasteSettings() {
    if (item_ == 0 || !has_clipboard_) {
        return;
    }
    endNudge();
    commit_gesture();
    paste_pending_ = true;  // until the paste is reported (its state comes first)
    service_.paste_settings({item_});
}

void EditSession::setFullResolution(bool full) {
    if (full == full_resolution_) {
        return;
    }
    full_resolution_ = full;
    if (item_ != 0) {
        service_.set_full_resolution(item_, full);
    }
}

void EditSession::set_compare(bool compare) {
    if (compare == compare_) {
        return;
    }
    compare_ = compare;
    send_comparison();
    emit compareChanged();
}

void EditSession::set_split(bool split) {
    if (split == split_) {
        return;
    }
    split_ = split;
    send_comparison();
    emit compareChanged();
}

void EditSession::send_comparison() {
    if (item_ != 0) {
        service_.set_comparison(item_, compare_ || split_);
    }
}

void EditSession::set_clipping(bool clipping) {
    if (clipping == clipping_) {
        return;
    }
    clipping_ = clipping;
    service_.set_clipping(clipping);
    if (!clipping && item_ != 0) {
        store_->drop(FrameStore::Kind::Clipping, item_);  // a 100 % overlay is large
        clipping_source_.clear();
    }
    emit clippingChanged();
    emit frameChanged();
}

void EditSession::noteFrameShown(const QString& source) {
    const auto found = pending_shown_.constFind(source);
    if (found == pending_shown_.cend()) {
        return;
    }
    last_latency_ms_ = double(clock_.elapsed() - *found);
    shown_latencies_.push_back(last_latency_ms_);
    pending_shown_.clear();
    emit latencyChanged();
}

void EditSession::reset_latencies() {
    shown_latencies_.clear();
    published_latencies_.clear();
    service_latencies_.clear();
    quick_frames_ = 0;
    final_frames_ = 0;
}

void EditSession::clearMessage() {
    set_message({});
}

// ---- Auto ----------------------------------------------------------------------------------------------

bool EditSession::can_run_auto() const {
    return item_ != 0 && can_edit() && settings_known() && !automatic_->running();
}

void EditSession::runAuto() {
    if (!can_run_auto()) {
        return;
    }
    // Auto starts from the committed settings: a gesture in progress is committed first (the service
    // runs Auto after the commit).
    endNudge();
    commit_gesture();
    automatic_->start();
    service_.run_auto(item_);
}

void EditSession::cancelAuto() {
    automatic_->cancel();
}

QString EditSession::describe_committed(EditControl control) const {
    const EditSettings& settings = state_ ? state_->committed : settings_;
    const auto with_unit = [&](const char* key) {
        const QString name = QString::fromLatin1(key);
        const ControlSpec* spec = spec_of(name);
        QString text = tr_text(spec->label) + u' ' + value_text_of(settings, name);
        if (spec->unit[0] != '\0') {
            text += u' ' + QString::fromLatin1(spec->unit);
        }
        return text;
    };
    switch (control) {
    case EditControl::WhiteBalance:
        switch (settings.white_balance) {
        case WhiteBalanceMode::Kelvin:
            return tr_text("White balance %1 K, tint %2")
                .arg(value_text_of(settings, QStringLiteral("temperature")),
                     value_text_of(settings, QStringLiteral("tint")));
        case WhiteBalanceMode::CameraNeutral:
            return tr_text("White balance: blue–amber %1, green–magenta %2")
                .arg(value_text_of(settings, QStringLiteral("redBlue")),
                     value_text_of(settings, QStringLiteral("greenMagenta")));
        case WhiteBalanceMode::AsShot:
            break;
        }
        return tr_text("White balance as shot");
    case EditControl::Exposure:
        return with_unit("exposure");
    case EditControl::Contrast:
        return with_unit("contrast");
    case EditControl::Highlights:
        return with_unit("highlights");
    case EditControl::Shadows:
        return with_unit("shadows");
    case EditControl::Saturation:
        return with_unit("saturation");
    case EditControl::Vibrance:
        return with_unit("vibrance");
    case EditControl::ToneCurve:
    case EditControl::BaseLook:
        break;
    }
    return control_label(control);
}

// ---- Tools (W9) -----------------------------------------------------------------------------------------

void EditSession::finish_pending_gesture() {
    endNudge();
    commit_gesture();
}

void EditSession::show_message(const QString& text) {
    set_message(text);
    emit notice(text);
}

void EditSession::setTool(const QString& tool) {
    const QString next = tool == u"crop" || tool == u"masks" ? tool : QString();
    if (next == tool_ || (!next.isEmpty() && item_ == 0)) {
        return;
    }
    finish_pending_gesture();
    if (tool_ == u"crop" && item_ != 0) {
        service_.set_crop_editing(item_, false);
    }
    tool_ = next;
    if (tool_ == u"crop") {
        crop_opened_with_ = settings_.geometry;
        service_.set_crop_editing(item_, true);
    }
    emit toolChanged();
}

void EditSession::finishTool() {
    setTool(QString());
}

void EditSession::cancelTool() {
    if (tool_ == u"crop" && can_edit() && settings_.geometry != crop_opened_with_) {
        finish_pending_gesture();
        EditSettings next = settings_;
        next.geometry = crop_opened_with_;
        change(next);
    }
    setTool(QString());
}

void EditSession::validate_selection() {
    quint32 selected = selected_mask_;
    if (mask_in(settings_, selected) == nullptr) {
        selected = settings_.masks.isEmpty() ? 0u : settings_.masks.back().id;
    }
    if (selected != selected_mask_) {
        selected_mask_ = selected;
        send_mask_overlay();
        emit maskSelectionChanged();
    }
}

void EditSession::set_selected_mask(quint32 id) {
    if (id == selected_mask_ || (id != 0 && mask_in(settings_, id) == nullptr)) {
        return;
    }
    selected_mask_ = id;
    send_mask_overlay();
    ++settings_revision_;  // the local controls show the newly selected mask
    emit maskSelectionChanged();
    emit settingsChanged();
}

void EditSession::set_mask_overlay(bool overlay) {
    if (overlay == mask_overlay_) {
        return;
    }
    mask_overlay_ = overlay;
    send_mask_overlay();
    if (!overlay) {
        if (item_ != 0) {
            store_->drop(FrameStore::Kind::Mask, item_);
        }
        mask_overlay_source_.clear();
    }
    emit maskOverlayChanged();
    emit frameChanged();
}

void EditSession::send_mask_overlay() {
    if (item_ != 0) {
        service_.set_mask_overlay(item_, mask_overlay_ ? selected_mask_ : 0u);
    }
}

void EditSession::set_message(const QString& message) {
    if (message == message_) {
        return;
    }
    message_ = message;
    emit messageChanged();
}

bool EditSession::busy() const {
    return service_.is_busy();
}

QString EditSession::frame_phase() const {
    if (frame_source_.isEmpty()) {
        return {};
    }
    return frame_phase_ == FramePhase::Final ? QStringLiteral("final") : QStringLiteral("quick");
}

// ---- Service signals ---------------------------------------------------------------------------------

void EditSession::on_state(const EditState& state) {
    if (state.item != item_ || item_ == 0) {
        return;
    }
    answered_token_ = std::max(answered_token_, state.token);
    const bool committed_changed = !state_ || state_->committed != state.committed;
    state_ = state;
    // A gesture that has not changed anything yet (a drag that began while an undo was on its way)
    // takes the new values too, so its changes build on them.
    if ((!gesture_ || !gesture_changed_) && answered_token_ >= last_token_ && state.settings != settings_) {
        set_local(state.settings);
    }
    emit stateChanged();
    emit canRunAutoChanged();
    if (committed_changed) {
        automatic_->values_changed();
    }
}

void EditSession::on_history(ItemId item, const QList<EditHistoryEntry>& entries) {
    if (item != item_ || item_ == 0) {
        return;
    }
    const bool as_shot_current =
        std::none_of(entries.cbegin(), entries.cend(), [](const EditHistoryEntry& entry) { return entry.current; });
    history_->set_entries(entries, as_shot_current);
}

void EditSession::on_white_balance(ItemId item, const WhiteBalanceInfo& info) {
    if (item != item_ || item_ == 0) {
        return;
    }
    white_balance_ = info;
    ++settings_revision_;  // the as-shot values shown changed
    emit whiteBalanceChanged();
    emit settingsChanged();
}

void EditSession::on_frame(const EditFramePtr& frame) {
    if (!frame || frame->item != item_ || item_ == 0 || frame->sequence <= last_sequence_) {
        return;
    }
    last_sequence_ = frame->sequence;
    if (frame->before) {
        store_->put(FrameStore::Kind::Before, item_, frame->sequence, frame->image);
        before_source_ = FrameStore::url(FrameStore::Kind::Before, item_, frame->sequence);
        emit frameChanged();
        return;
    }
    store_->put(FrameStore::Kind::Frame, item_, frame->sequence, frame->image);
    frame_source_ = FrameStore::url(FrameStore::Kind::Frame, item_, frame->sequence);
    // W9: the frame's geometry and masks for the tools, and the selected mask's weights.
    frame_full_upright_ = frame->full_upright_size;
    frame_geometry_ = frame->geometry;
    frame_uncropped_ = frame->uncropped;
    frame_masks_ = frame->masks;
    frame_map_ = frame_full_upright_.isEmpty() ? GeometryMap{}
                                               : service_.map_geometry(frame_geometry_, frame_full_upright_);
    if (mask_overlay_ && !frame->mask_overlay.isNull() && frame->mask_overlay_id == selected_mask_) {
        store_->put(FrameStore::Kind::Mask, item_, frame->sequence, frame->mask_overlay);
        mask_overlay_source_ = FrameStore::url(FrameStore::Kind::Mask, item_, frame->sequence);
    } else {
        mask_overlay_source_.clear();
    }
    if (!frame->clipping.isNull()) {
        store_->put(FrameStore::Kind::Clipping, item_, frame->sequence, frame->clipping);
        clipping_source_ = FrameStore::url(FrameStore::Kind::Clipping, item_, frame->sequence);
    } else {
        clipping_source_.clear();
    }
    frame_info_ = frame->info;
    frame_phase_ = frame->phase;
    frame_full_resolution_ = frame->full_resolution;
    frame_error_.clear();
    (frame->phase == FramePhase::Final ? final_frames_ : quick_frames_) += 1;
    if (frame->token != 0) {
        if (const auto found = token_times_.constFind(frame->token); found != token_times_.cend()) {
            published_latencies_.push_back(double(clock_.elapsed() - *found));
            service_latencies_.push_back(frame->latency_seconds * 1000.0);
            pending_shown_.clear();
            pending_shown_.insert(frame_source_, *found);
        }
    }
    emit frameChanged();
}

void EditSession::on_frame_failed(ItemId item, const QString& message) {
    if (item != item_ || item_ == 0) {
        return;
    }
    frame_error_ = message;
    emit frameChanged();
}

void EditSession::on_command_failed(ItemId item, const QString& message) {
    if (item != item_ || item_ == 0) {
        return;
    }
    set_message(message);
    emit notice(message);
}

void EditSession::on_copied(ItemId item, quint32 categories) {
    if (!has_clipboard_) {
        has_clipboard_ = true;
        emit clipboardChanged();
    }
    if (item != item_) {
        return;
    }
    QStringList labels;
    for (const SettingsCategory category : kAllSettingsCategories) {
        if ((categories & category_bit(category)) != 0) {
            labels << category_label(category);
        }
    }
    const QString text = tr_text("Copied %1.").arg(labels.join(QStringLiteral(", ")));
    set_message(text);
    emit notice(text);
}

void EditSession::on_pasted(const PasteReport& report) {
    paste_pending_ = false;  // the pasted state (if any) was posted before the report
    QStringList applied;
    for (const SettingsCategory category : report.applied) {
        applied << category_label(category);
    }
    QString text;
    if (applied.isEmpty() && !report.errors.isEmpty()) {
        text = tr_text("Could not paste: %1").arg(report.errors.join(QStringLiteral(" ")));
    } else {
        text = applied.isEmpty() ? tr_text("Nothing was pasted.")
                                 : tr_text("Pasted %1.").arg(applied.join(QStringLiteral(", ")));
        if (!applied.isEmpty() && report.changed == 0) {
            text += u' ' + tr_text("The photo already had these settings.");
        }
        if (!report.skipped.isEmpty()) {
            text += u' ' + tr_text("Not pasted — %1").arg(report.skipped.join(QStringLiteral(" ")));
        }
        if (!report.errors.isEmpty()) {
            text += u' ' + report.errors.join(QStringLiteral(" "));
        }
    }
    set_message(text);
    emit notice(text);
}

}  // namespace lucent::ui
