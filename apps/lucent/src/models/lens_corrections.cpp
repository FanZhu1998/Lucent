#include "models/lens_corrections.hpp"

#include <QCoreApplication>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "models/edit_session.hpp"

namespace lucent::ui {
namespace {

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::LensCorrections", text);
}

constexpr std::array<const char*, 3> kKeys = {"distortion", "chromaticAberration", "vignetting"};

int correction_index(const QString& key) {
    for (int i = 0; i < static_cast<int>(kKeys.size()); ++i) {
        if (key == QLatin1StringView(kKeys[static_cast<std::size_t>(i)])) {
            return i;
        }
    }
    return -1;
}

QString correction_label(int correction) {
    switch (correction) {
        case 0:
            return tr_text("Distortion");
        case 1:
            return tr_text("Chromatic aberration");
        default:
            return tr_text("Vignetting");
    }
}

// Lower-case names for sentences ("distortion and vignetting").
QString correction_noun(int correction) {
    switch (correction) {
        case 0:
            return tr_text("distortion");
        case 1:
            return tr_text("chromatic aberration");
        default:
            return tr_text("vignetting");
    }
}

QString join_words(const QStringList& words) {
    if (words.size() <= 1) {
        return words.value(0);
    }
    return QStringList(words.mid(0, words.size() - 1)).join(QStringLiteral(", ")) + u' ' + tr_text("and") +
           u' ' + words.last();
}

bool& setting_of(EditLensCorrection& lens, int correction) {
    return correction == 0 ? lens.distortion : correction == 1 ? lens.chromatic_aberration : lens.vignetting;
}

bool same_profile(const EditLensProfile& a, const EditLensProfile& b) {
    return a.maker == b.maker && a.model == b.model && std::abs(a.crop_factor - b.crop_factor) < 0.01;
}

QString number_text(double value) {
    return QString::number(value, 'g', 4);
}

}  // namespace

// ---- LensProfileListModel -------------------------------------------------------------------------------

LensProfileListModel::LensProfileListModel(QObject* parent) : QAbstractListModel(parent) {}

int LensProfileListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QString LensProfileListModel::label_of(const EditLensProfile& profile) {
    if (profile.maker.isEmpty() || profile.model.startsWith(profile.maker, Qt::CaseInsensitive)) {
        return profile.model;
    }
    return profile.maker + u' ' + profile.model;
}

QVariant LensProfileListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) {
        return {};
    }
    const LensProfileEntry& entry = entries_[index.row()];
    switch (role) {
        case Qt::DisplayRole:
        case LabelRole:
            return label_of(entry.profile);
        case DetailRole: {
            // What tells calibrations of the same lens apart first (the line is elided at the panel's
            // width): the sensor they were made on, then the focal range, the calibrations and the
            // mounts.
            QStringList parts;
            parts << tr_text("crop factor %1").arg(number_text(entry.profile.crop_factor));
            if (entry.min_focal_mm > 0.0) {
                parts << (entry.max_focal_mm > entry.min_focal_mm
                              ? tr_text("%1-%2 mm")
                                    .arg(number_text(entry.min_focal_mm), number_text(entry.max_focal_mm))
                              : tr_text("%1 mm").arg(number_text(entry.min_focal_mm)));
            }
            QStringList calibrations;
            if (entry.distortion) {
                calibrations << tr_text("distortion");
            }
            if (entry.chromatic_aberration) {
                calibrations << tr_text("chromatic aberration");
            }
            if (entry.vignetting) {
                calibrations << tr_text("vignetting");
            }
            parts << (calibrations.isEmpty() ? tr_text("no calibration") : calibrations.join(QStringLiteral(", ")));
            if (!entry.mounts.isEmpty()) {
                parts << entry.mounts;
            }
            return parts.join(QStringLiteral(" · "));
        }
        case MakerRole:
            return entry.profile.maker;
        case ModelRole:
            return entry.profile.model;
        case CropFactorRole:
            return entry.profile.crop_factor;
        case CompatibleRole:
            return compatible(entry);
        case CurrentRole:
            return current_.has_value() && same_profile(*current_, entry.profile);
        default:
            return {};
    }
}

QHash<int, QByteArray> LensProfileListModel::roleNames() const {
    return {{LabelRole, "label"},    {DetailRole, "detail"},         {MakerRole, "maker"},
            {ModelRole, "model"},    {CropFactorRole, "cropFactor"}, {CompatibleRole, "compatible"},
            {CurrentRole, "current"}};
}

bool LensProfileListModel::compatible(const LensProfileEntry& entry) const {
    const QStringList mounts = entry.mounts.split(QStringLiteral(", "), Qt::SkipEmptyParts);
    return std::any_of(mounts.cbegin(), mounts.cend(),
                       [this](const QString& m) { return mounts_.contains(m); });
}

void LensProfileListModel::set_entries(QList<LensProfileEntry> entries, const QStringList& mounts,
                                       const std::optional<EditLensProfile>& current) {
    const QStringList previous_mounts = std::exchange(mounts_, mounts);
    std::stable_partition(entries.begin(), entries.end(),
                          [this](const LensProfileEntry& entry) { return compatible(entry); });
    // The same list (a frame arrived while the choice is open): no reset, so the view keeps its place.
    const auto same_entry = [](const LensProfileEntry& a, const LensProfileEntry& b) {
        return same_profile(a.profile, b.profile) && a.mounts == b.mounts;
    };
    if (mounts_ == previous_mounts && current == current_ &&
        std::equal(entries.cbegin(), entries.cend(), entries_.cbegin(), entries_.cend(), same_entry)) {
        return;
    }
    const bool count_changed = entries.size() != entries_.size();
    beginResetModel();
    entries_ = std::move(entries);
    current_ = current;
    endResetModel();
    if (count_changed) {
        emit countChanged();
    }
}

// ---- LensCorrections ----------------------------------------------------------------------------------

LensCorrections::LensCorrections(EditSession& session)
    : session_(session), profiles_(std::make_unique<LensProfileListModel>()) {
    database_ = session_.service().lens_database();
    connect(&session_, &EditSession::activeChanged, this, [this] {
        database_ = session_.service().lens_database();  // fixed for a session; read per photo
        picking_ = false;
        search_text_.clear();
        info_.reset();
        emit pickingChanged();
        on_session_changed();
    });
    connect(&session_, &EditSession::frameChanged, this, &LensCorrections::on_session_changed);
    connect(&session_, &EditSession::settingsChanged, this, &LensCorrections::on_session_changed);
    connect(&session_, &EditSession::stateChanged, this, &LensCorrections::on_session_changed);
    connect(&session_.service(), &EditService::lens_database_changed, this, &LensCorrections::on_database_changed);
}

LensCorrections::~LensCorrections() = default;

void LensCorrections::on_database_changed() {
    if (!database_.loading) {
        return;
    }
    database_ = session_.service().lens_database();
    if (picking_) {
        refresh_profiles();
    }
    emit changed();
}

void LensCorrections::on_session_changed() {
    if (database_.loading) {
        // A frame can arrive before the posted notice that the database has loaded (frames wait for it).
        database_ = session_.service().lens_database();
    }
    if (session_.frame_lens()) {
        info_ = session_.frame_lens();  // the newest frame's; kept while a new one renders
    } else if (!session_.has_frame()) {
        info_.reset();
    }
    if (picking_) {
        refresh_profiles();
    }
    emit changed();
}

bool LensCorrections::available() const {
    return database_.available;
}

QString LensCorrections::unavailable_reason() const {
    if (database_.available) {
        return {};
    }
    if (database_.loading) {
        return tr_text("The lens database is still loading; lens corrections follow when it has.");
    }
    return database_.problem.isEmpty()
               ? tr_text("Lens corrections are unavailable: no lens database is in use.")
               : database_.problem;
}

QString LensCorrections::database_version() const {
    return database_.version;
}

bool LensCorrections::editable() const {
    return available() && session_.active() && session_.can_edit() && session_.settings_known();
}

bool LensCorrections::legacy() const {
    return session_.settings_known() && !session_.settings().lens.enabled;
}

bool LensCorrections::automatic() const {
    return !session_.settings().lens.profile.has_value();
}

QString LensCorrections::match_kind() const {
    if (!info_) {
        return {};
    }
    switch (info_->match) {
        case LensMatchQuality::Exact:
            return QStringLiteral("exact");
        case LensMatchQuality::Fuzzy:
            return QStringLiteral("fuzzy");
        case LensMatchQuality::None:
            break;
    }
    return QStringLiteral("none");
}

QString LensCorrections::match_label() const {
    if (!info_) {
        return {};
    }
    switch (info_->match) {
        case LensMatchQuality::Exact:
            return tr_text("Exact match");
        case LensMatchQuality::Fuzzy:
            return tr_text("Similar profiles only");
        case LensMatchQuality::None:
            break;
    }
    return tr_text("No profile");
}

QString LensCorrections::match_summary() const {
    if (!available()) {
        return unavailable_reason();
    }
    if (legacy()) {
        return tr_text("This photo was edited before Lucent had lens corrections, so none are applied.");
    }
    if (!info_) {
        return tr_text("Looking up the lens profile…");
    }
    const EditLensCorrection& lens = session_.settings().lens;
    if (!lens.distortion && !lens.chromatic_aberration && !lens.vignetting) {
        return tr_text("Every lens correction is switched off for this photo.");
    }
    // What the newest render did (its plan's decisions).
    const auto any = [this](LensOutcome outcome) {
        return std::any_of(info_->corrections.cbegin(), info_->corrections.cend(),
                           [outcome](const LensCorrectionState& c) { return c.outcome == outcome; });
    };
    if (!automatic()) {
        if (!any(LensOutcome::Applied) && any(LensOutcome::NoProfile)) {
            // A recipe naming a profile this database does not hold (made with another database):
            // reported, never silently replaced (ADR 0023, W13 closure).
            return tr_text(
                "The profile chosen for this photo is not in this lens database, so nothing is applied. "
                "Choose a profile again, or switch to the automatic profile.");
        }
        if (!any(LensOutcome::Applied) && any(LensOutcome::NoCalibration)) {
            return tr_text(
                "The profile you chose is not calibrated for this photo (its focal length, aperture or "
                "sensor), so nothing is applied.");
        }
        return tr_text("Using the profile you chose for this photo.");
    }
    switch (info_->match) {
        case LensMatchQuality::Exact:
            if (any(LensOutcome::Applied)) {
                return tr_text(
                    "The lens database has this camera and lens: its profile is applied automatically.");
            }
            if (any(LensOutcome::Withheld)) {
                return tr_text(
                    "The lens database has this lens's profile, but the file corrects its lens itself, so "
                    "the profile is not applied automatically.");
            }
            if (any(LensOutcome::NoCalibration)) {
                return tr_text(
                    "The lens database has this camera and lens, but its profile is not calibrated for this "
                    "photo (its focal length or aperture), so nothing is applied.");
            }
            if (any(LensOutcome::CarriedByFile)) {
                return tr_text(
                    "The lens database has this camera and lens, but the file already makes these "
                    "corrections itself, so nothing is applied again.");
            }
            return tr_text("The lens database has this camera and lens.");
        case LensMatchQuality::Fuzzy:
            return tr_text(
                "The lens database has similar profiles, but none is certainly this lens, so none is "
                "applied automatically. Choose one if it is the same lens.");
        case LensMatchQuality::None:
            break;
    }
    if (info_->lens.isEmpty()) {
        return tr_text("The file does not record its lens, so no profile is applied. You can choose one.");
    }
    return tr_text(
        "The lens database has no profile for this camera and lens, so none is applied. You can "
        "choose one.");
}

QString LensCorrections::camera_text() const {
    return info_ ? info_->camera : QString();
}

QString LensCorrections::lens_text() const {
    if (!info_) {
        return {};
    }
    QStringList parts;
    parts << (info_->lens.isEmpty() ? tr_text("Lens not recorded") : info_->lens);
    if (info_->focal_length_mm > 0.0) {
        parts << tr_text("%1 mm").arg(number_text(info_->focal_length_mm));
    }
    if (info_->f_number > 0.0) {
        parts << tr_text("f/%1").arg(number_text(info_->f_number));
    }
    return parts.join(QStringLiteral(" · "));
}

QString LensCorrections::profile_text() const {
    // The photographer's choice; otherwise the one the render used, or the exact match it would use
    // (when its corrections are switched off).
    if (const std::optional<EditLensProfile>& named = session_.settings().lens.profile; named && !legacy()) {
        return LensProfileListModel::label_of(*named);
    }
    if (!info_ || !info_->enabled) {
        return {};
    }
    if (info_->profile) {
        return LensProfileListModel::label_of(*info_->profile);
    }
    if (info_->automatic && info_->match == LensMatchQuality::Exact && info_->matched) {
        return LensProfileListModel::label_of(*info_->matched);
    }
    return {};
}

QStringList LensCorrections::reasons() const {
    return info_ ? info_->reasons : QStringList();
}

bool LensCorrections::carried(int correction) const {
    if (!info_) {
        return false;
    }
    switch (correction) {
        case 0:
            return info_->carried_distortion;
        case 1:
            return info_->carried_chromatic_aberration;
        default:
            return info_->carried_vignetting;
    }
}

QString LensCorrections::outcome_text(LensOutcome outcome, int correction) {
    switch (outcome) {
        case LensOutcome::Applied:
            return tr_text("Applied");
        case LensOutcome::Disabled:
            return tr_text("Off");
        case LensOutcome::CarriedByFile:
            return tr_text("Corrected by the file");
        case LensOutcome::Withheld:
            return tr_text("Not applied automatically");
        case LensOutcome::NoProfile:
            return tr_text("No profile");
        case LensOutcome::NotApplicable:
            return correction == 1 ? tr_text("Not needed for a monochrome photo") : tr_text("Not applicable");
        case LensOutcome::NoCalibration:
            return tr_text("Not calibrated for this photo");
    }
    return {};
}

QVariantList LensCorrections::corrections() const {
    QVariantList out;
    const EditLensCorrection& lens = session_.settings().lens;
    const bool can_change = editable() && !legacy();
    for (int i = 0; i < static_cast<int>(kKeys.size()); ++i) {
        const bool from_file = carried(i);
        bool setting = false;
        switch (i) {
            case 0:
                setting = lens.distortion;
                break;
            case 1:
                setting = lens.chromatic_aberration;
                break;
            default:
                setting = lens.vignetting;
                break;
        }
        QString status;
        QString detail;
        if (info_) {
            const LensCorrectionState& state = info_->corrections[static_cast<std::size_t>(i)];
            status = outcome_text(state.outcome, i);
            detail = state.detail;
        }
        out << QVariantMap{{QStringLiteral("key"), QString::fromLatin1(kKeys[static_cast<std::size_t>(i)])},
                           {QStringLiteral("label"), correction_label(i)},
                           // A correction the file carries is never applied again: shown off.
                           {QStringLiteral("checked"), setting && !from_file && !legacy()},
                           {QStringLiteral("editable"), can_change && !from_file},
                           {QStringLiteral("carried"), from_file},
                           {QStringLiteral("status"), status},
                           {QStringLiteral("detail"), detail}};
    }
    return out;
}

QString LensCorrections::carried_note() const {
    QStringList carried_words;
    for (int i = 0; i < static_cast<int>(kKeys.size()); ++i) {
        if (carried(i)) {
            carried_words << correction_noun(i);
        }
    }
    if (carried_words.isEmpty()) {
        return {};
    }
    return (carried_words.size() == 1
                ? tr_text("This file already corrects %1 itself (DNG opcodes). Lucent never "
                          "applies the same correction twice, so that switch stays off.")
                : tr_text("This file already corrects %1 itself (DNG opcodes). Lucent never "
                          "applies the same correction twice, so those switches stay off."))
        .arg(join_words(carried_words));
}

void LensCorrections::apply(const EditLensCorrection& next) {
    EditSettings settings = session_.settings();
    if (settings.lens == next) {
        return;
    }
    settings.lens = next;
    session_.finish_pending_gesture();
    session_.edit_settings(settings);
}

void LensCorrections::setAutomatic(bool automatic) {
    if (!editable() || legacy()) {
        return;
    }
    if (!automatic) {
        openPicker();  // nothing changes until a profile is chosen
        return;
    }
    closePicker();
    EditLensCorrection next = session_.settings().lens;
    next.profile.reset();
    apply(next);
}

void LensCorrections::setCorrection(const QString& key, bool on) {
    const int index = correction_index(key);
    if (index < 0 || !editable() || legacy() || carried(index)) {
        return;
    }
    EditLensCorrection next = session_.settings().lens;
    setting_of(next, index) = on;
    apply(next);
}

void LensCorrections::chooseProfile(int row) {
    if (!editable() || legacy() || row < 0 || row >= profiles_->count()) {
        return;
    }
    EditLensCorrection next = session_.settings().lens;
    next.profile = profiles_->entries()[row].profile;
    closePicker();
    apply(next);
}

void LensCorrections::turnOn() {
    if (!editable() || !legacy()) {
        return;
    }
    apply(EditLensCorrection{});
}

void LensCorrections::openPicker() {
    if (!editable() || legacy()) {
        return;
    }
    picking_ = true;
    refresh_profiles();
    emit pickingChanged();
}

void LensCorrections::closePicker() {
    if (!picking_) {
        return;
    }
    picking_ = false;
    emit pickingChanged();
}

void LensCorrections::setSearchText(const QString& text) {
    if (text == search_text_) {
        return;
    }
    search_text_ = text;
    refresh_profiles();
    emit pickingChanged();
}

void LensCorrections::refresh_profiles() {
    QList<LensProfileEntry> entries;
    if (!search_text_.trimmed().isEmpty()) {
        entries = session_.service().search_lens_profiles(search_text_, kMaxSearchResults);
    } else if (info_) {
        // The match's candidates (with their details from the database), or the exact match.
        QList<EditLensProfile> suggested = info_->candidates;
        if (suggested.isEmpty() && info_->matched) {
            suggested << *info_->matched;
        }
        for (const EditLensProfile& profile : suggested) {
            LensProfileEntry entry;
            entry.profile = profile;
            for (const LensProfileEntry& found : session_.service().search_lens_profiles(
                     profile.maker + u' ' + profile.model, kMaxSearchResults)) {
                if (same_profile(found.profile, profile)) {
                    entry = found;
                    break;
                }
            }
            entries << entry;
        }
    }
    profiles_->set_entries(std::move(entries), info_ ? info_->compatible_mounts : QStringList(),
                           session_.settings().lens.profile);
}

}  // namespace lucent::ui
