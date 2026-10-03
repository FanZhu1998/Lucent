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
    Reciprocal,  // equal slider distances are equal distances in 1/value (mired for temperature)
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
};

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
};

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
    CategorySpec{"local", SettingsCategory::Local},
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
    state_.reset();
    answered_token_ = last_token_;
    white_balance_ = WhiteBalanceInfo{};
    history_->clear();
    selected_point_ = -1;
    reset_frames();
    set_local(EditSettings{});
    if (item_ != 0) {
        service_.open(item_);
        send_comparison();
        if (full_resolution_) {
            service_.set_full_resolution(item_, true);
        }
    }
    emit activeChanged();
    emit stateChanged();
    emit whiteBalanceChanged();
    emit selectedCurvePointChanged();
}

void EditSession::reset_frames() {
    frame_source_.clear();
    before_source_.clear();
    clipping_source_.clear();
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
    double default_value = 0.0;
    if (key == u"temperature" || key == u"tint") {
        default_value = display_value(EditSettings{}, key);
    }
    return QVariantMap{
        {QStringLiteral("label"), tr_text(spec->label)},
        {QStringLiteral("minimum"), spec->minimum},
        {QStringLiteral("maximum"), spec->maximum},
        {QStringLiteral("step"), spec->step},
        {QStringLiteral("coarseStep"), spec->coarse},
        {QStringLiteral("decimals"), spec->decimals},
        {QStringLiteral("unit"), QString::fromLatin1(spec->unit)},
        {QStringLiteral("defaultValue"), default_value},
        {QStringLiteral("lockKey"), lock_key(spec->lock)},
    };
}

double EditSession::value(const QString& key) const {
    return display_value(settings_, key);
}

QString EditSession::valueText(const QString& key) const {
    const ControlSpec* spec = spec_of(key);
    if (spec == nullptr) {
        return {};
    }
    const double v = round_to(display_value(settings_, key), spec->decimals);
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
    } else {
        v = spec->minimum + p * (spec->maximum - spec->minimum);
    }
    setValue(key, v);
}

void EditSession::setValue(const QString& key, double value) {
    if (spec_of(key) == nullptr) {
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
    endGesture();
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
    endGesture();
    setValue(key, 0.0);
}

void EditSession::resetWhiteBalance() {
    endNudge();
    endGesture();
    EditSettings next = settings_;
    next.white_balance = WhiteBalanceMode::AsShot;
    change(next);
}

void EditSession::beginGesture() {
    if (!can_edit() || gesture_) {
        return;
    }
    gesture_ = true;
    gesture_changed_ = false;
    emit gestureChanged();
}

void EditSession::endGesture() {
    if (!gesture_) {
        return;
    }
    gesture_ = false;
    if (gesture_changed_ && item_ != 0) {
        remember_token(service_.commit(item_));
    }
    gesture_changed_ = false;
    emit gestureChanged();
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
        beginGesture();
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
    endGesture();
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
    endGesture();
    EditSettings next = settings_;
    next.set_locked(*control, locked);
    set_local(next);
    remember_token(service_.set_lock(item_, *control, locked));
}

void EditSession::change(const EditSettings& next) {
    if (!can_edit() || item_ == 0 || next == settings_) {
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
    endGesture();
    EditSettings next = settings_;
    next.tone_curve.removeAt(index);
    selected_point_ = -1;
    emit selectedCurvePointChanged();
    change(next);
    return true;
}

void EditSession::resetCurve() {
    endNudge();
    endGesture();
    EditSettings next = settings_;
    next.tone_curve = EditSettings{}.tone_curve;
    selected_point_ = -1;
    emit selectedCurvePointChanged();
    change(next);
}

// ---- History, comparison, copy and paste ------------------------------------------------------------

void EditSession::undo() {
    endNudge();
    endGesture();
    if (item_ != 0 && can_undo()) {
        remember_token(service_.undo(item_));
    }
}

void EditSession::redo() {
    endNudge();
    endGesture();
    if (item_ != 0 && can_redo()) {
        remember_token(service_.redo(item_));
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
    endGesture();
    remember_token(service_.select_revision(item_, *revision));
}

void EditSession::resetToAsShot() {
    endNudge();
    endGesture();
    if (item_ != 0 && history_available()) {
        remember_token(service_.reset_to_as_shot(item_));
    }
}

QVariantList EditSession::categories() const {
    QVariantList list;
    for (const CategorySpec& category : kCategories) {
        list << QVariantMap{{QStringLiteral("key"), QString::fromLatin1(category.key)},
                            {QStringLiteral("label"), category_label(category.category)}};
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
    endGesture();
    service_.copy_settings(item_, bits);
}

void EditSession::pasteSettings() {
    if (item_ == 0 || !has_clipboard_) {
        return;
    }
    endNudge();
    endGesture();
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
    state_ = state;
    if (!gesture_ && answered_token_ >= last_token_ && state.settings != settings_) {
        set_local(state.settings);
    }
    emit stateChanged();
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
