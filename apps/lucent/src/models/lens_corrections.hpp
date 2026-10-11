#pragma once

// LensCorrections and LensProfileListModel: the Edit view's "Lens corrections" panel (blueprint
// section 2, "lens corrections"; section 3: "An optional lens-profile miss can leave correction
// off"; section 5, step 4; ADR 0023). EditSession owns them (EditSession.lens, .lens.profiles).
//
// What it shows. The photo's lens settings (EditSettings::lens: whether its recipe has lens
// corrections at all, the automatic or named profile and the three switches) and what the newest
// frame's render did with them (EditFrame::lens): the camera and lens the file records, the lens
// database's match and its quality (exact, fuzzy, none) with the reasons, the profile used, each
// correction's outcome, and what the file already corrects itself (DNG opcodes). A correction the
// file carries is never applied a second time: its switch is shown off and cannot be changed, and
// `carriedNote` says so. Without a lens database (missing or refused) the panel says why and offers
// nothing to change; a recipe made before Lucent had lens corrections (operation version 0) shows
// `legacy` and offers turnOn(). W12 (APL-06): while the database still loads off the UI thread
// (`loading`), nothing can be changed either and the panel says it is loading; the photo's frames wait
// for it, and the panel follows when it has loaded (EditService::lens_database_changed).
//
// Changes. setAutomatic(true) returns to the automatic profile (the database's exact match only);
// setAutomatic(false) opens the manual choice without changing anything until a profile is chosen
// (chooseProfile(): the search results, or with an empty search the match's candidates). Each switch,
// a chosen profile, the return to automatic and turnOn() are ONE revision each (EditService::apply).
//
// The profile choice. `searchText` filters the database's profiles by maker and model
// (EditService::search_lens_profiles(), at most kMaxSearchResults); `profiles` lists them (or, with
// an empty search, the fuzzy match's candidates), those that fit the identified camera first
// (`compatible`: a mount of LensInfo::compatible_mounts), marking the profile in use (`current`).
//
// Thread-safety: UI thread only.

#include <QAbstractListModel>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>
#include <memory>
#include <optional>

#include "services/edit_service.hpp"

namespace lucent::ui {

class EditSession;

class LensProfileListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("LensProfileListModel is provided by EditSession.lens.profiles.")
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

public:
    enum Role {
        LabelRole = Qt::UserRole + 1,  // "Nikon Nikkor Z 24-70mm f/4 S"
        DetailRole,                    // "Nikon Z · crop factor 1 · distortion, chromatic aberration"
        MakerRole,
        ModelRole,
        CropFactorRole,
        CompatibleRole,  // one of its mounts fits the identified camera (LensInfo::compatible_mounts)
        CurrentRole,     // the profile the settings name
    };

    explicit LensProfileListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int count() const noexcept { return static_cast<int>(entries_.size()); }

    // Shows `entries`, those with a mount of `mounts` (the identified camera's compatible mounts;
    // empty: unknown) first, in their order otherwise; `current` is the profile the settings name.
    void set_entries(QList<LensProfileEntry> entries, const QStringList& mounts,
                     const std::optional<EditLensProfile>& current);
    [[nodiscard]] const QList<LensProfileEntry>& entries() const noexcept { return entries_; }

    // "Maker Model" (the model alone when it already starts with the maker).
    [[nodiscard]] static QString label_of(const EditLensProfile& profile);

signals:
    void countChanged();

private:
    [[nodiscard]] bool compatible(const LensProfileEntry& entry) const;

    QList<LensProfileEntry> entries_;
    QStringList mounts_;
    std::optional<EditLensProfile> current_;
};

class LensCorrections : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("LensCorrections is provided by EditSession.lens.")
    // W12 (APL-06): the lens database is still loading (`unavailableReason` says so; not a problem).
    Q_PROPERTY(bool loading READ loading NOTIFY changed FINAL)
    // A lens database is active (otherwise `unavailableReason` says why) and its version.
    Q_PROPERTY(bool available READ available NOTIFY changed FINAL)
    Q_PROPERTY(QString unavailableReason READ unavailable_reason NOTIFY changed FINAL)
    Q_PROPERTY(QString databaseVersion READ database_version NOTIFY changed FINAL)
    // The settings can be changed (a photo is open, its recipe is editable and a database is active).
    Q_PROPERTY(bool editable READ editable NOTIFY changed FINAL)
    // The recipe was made before Lucent had lens corrections: none is applied (turnOn() changes that).
    Q_PROPERTY(bool legacy READ legacy NOTIFY changed FINAL)
    Q_PROPERTY(bool automatic READ automatic NOTIFY changed FINAL)
    // A frame has told what its render did (the fields below are empty until then).
    Q_PROPERTY(bool known READ known NOTIFY changed FINAL)
    // "exact", "fuzzy", "none"; empty while unknown.
    Q_PROPERTY(QString matchKind READ match_kind NOTIFY changed FINAL)
    Q_PROPERTY(QString matchLabel READ match_label NOTIFY changed FINAL)      // "Exact match", ...
    Q_PROPERTY(QString matchSummary READ match_summary NOTIFY changed FINAL)  // one sentence
    Q_PROPERTY(QString cameraText READ camera_text NOTIFY changed FINAL)      // "Nikon Z 6"
    Q_PROPERTY(
        QString lensText READ lens_text NOTIFY changed FINAL)  // "NIKKOR Z 24-70mm f/4 S · 52 mm · f/6.7"
    // The profile in use ("Nikon Nikkor Z 24-70mm f/4 S"; empty: none).
    Q_PROPERTY(QString profileText READ profile_text NOTIFY changed FINAL)
    // The match's reasons (the engine's words) for the details.
    Q_PROPERTY(QStringList reasons READ reasons NOTIFY changed FINAL)
    // [{key, label, checked, editable, carried, status, detail}] for "distortion",
    // "chromaticAberration" and "vignetting".
    Q_PROPERTY(QVariantList corrections READ corrections NOTIFY changed FINAL)
    // What the file corrects itself, and that those switches stay off; empty when nothing.
    Q_PROPERTY(QString carriedNote READ carried_note NOTIFY changed FINAL)
    // The manual profile choice is open.
    Q_PROPERTY(bool picking READ picking NOTIFY pickingChanged FINAL)
    Q_PROPERTY(QString searchText READ search_text WRITE setSearchText NOTIFY pickingChanged FINAL)
    Q_PROPERTY(lucent::ui::LensProfileListModel* profiles READ profiles CONSTANT FINAL)

public:
    static constexpr int kMaxSearchResults = 60;

    explicit LensCorrections(EditSession& session);
    ~LensCorrections() override;

    Q_INVOKABLE void setAutomatic(bool automatic);
    // "distortion", "chromaticAberration" or "vignetting".
    Q_INVOKABLE void setCorrection(const QString& key, bool on);
    // A row of `profiles`.
    Q_INVOKABLE void chooseProfile(int row);
    // Lens corrections for a recipe made before Lucent had them (the defaults: automatic, all on).
    Q_INVOKABLE void turnOn();
    Q_INVOKABLE void openPicker();
    Q_INVOKABLE void closePicker();
    Q_INVOKABLE void setSearchText(const QString& text);

    [[nodiscard]] bool loading() const noexcept { return database_.loading; }
    [[nodiscard]] bool available() const;
    [[nodiscard]] QString unavailable_reason() const;
    [[nodiscard]] QString database_version() const;
    [[nodiscard]] bool editable() const;
    [[nodiscard]] bool legacy() const;
    [[nodiscard]] bool automatic() const;
    [[nodiscard]] bool known() const noexcept { return info_ != nullptr; }
    [[nodiscard]] QString match_kind() const;
    [[nodiscard]] QString match_label() const;
    [[nodiscard]] QString match_summary() const;
    [[nodiscard]] QString camera_text() const;
    [[nodiscard]] QString lens_text() const;
    [[nodiscard]] QString profile_text() const;
    [[nodiscard]] QStringList reasons() const;
    [[nodiscard]] QVariantList corrections() const;
    [[nodiscard]] QString carried_note() const;
    [[nodiscard]] bool picking() const noexcept { return picking_; }
    [[nodiscard]] QString search_text() const { return search_text_; }
    [[nodiscard]] LensProfileListModel* profiles() const noexcept { return profiles_.get(); }

    // For tests and the automation: the frame's lens record in use (null until a frame arrived).
    [[nodiscard]] const LensInfoPtr& info() const noexcept { return info_; }
    // The user-facing outcome of a correction ("Applied", "Corrected by the file", ...).
    [[nodiscard]] static QString outcome_text(LensOutcome outcome, int correction);

signals:
    void changed();
    void pickingChanged();

private:
    void on_session_changed();
    // W12 (APL-06): reads the database's description again (it has loaded).
    void on_database_changed();
    void refresh_profiles();
    void apply(const EditLensCorrection& next);
    [[nodiscard]] bool carried(int correction) const;

    EditSession& session_;
    LensDatabaseInfo database_;
    LensInfoPtr info_;
    std::unique_ptr<LensProfileListModel> profiles_;
    bool picking_ = false;
    QString search_text_;
};

}  // namespace lucent::ui
