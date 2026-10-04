#pragma once

// BatchSetupModel: the state of the Export batch dialog (blueprint section 2, "Automatic batch
// development" steps 1 to 4; section 7, "Export integrity and user-selected destinations").
//
// The photographs (the Library's selection, all listed photos, or a folder), the output folder,
// the preset (JPEG / sRGB / quality 95, or 16-bit TIFF with an sRGB or Display P3 profile), the
// naming pattern with its tokens, mirrored subfolders, the collision policy (unique names by default;
// replacing existing files only after confirmOverwrite(), which the dialog calls after an explicit
// confirmation that names the folder, and never for originals: the service gives those a new name),
// what happens to photos already exported with the same settings, and the metadata (capture info,
// Artist / Copyright with the user's text, GPS only when chosen: off whenever the dialog opens).
// W8: automatic settings (Off: each photo's own current edit; Auto: Lucent analyses each photo,
// saves its choice as an automatic revision, checks it at full size and exports it; Analyse only:
// automatic revisions to review in Edit, nothing exported) and whether related shots are kept
// consistent (with Auto or Analyse only). The plan's notes say what the analysis adds.
//
// While the dialog is open (`active`), every change asks the service for a new plan after a short
// quiet period (`plan_delay`, 300 ms): typing a pattern makes one request, not one per key. A plan
// that arrives for an older request is ignored. The plan gives the example destination paths, the
// counts (exported, renamed, replaced, already exported, left out with reasons), free space and
// warnings shown before starting.
//
// QML-facing members use QML naming (lowerCamelCase); C++-only members use snake_case.
//
// Thread-safety: UI thread only.

#include <chrono>

#include <QList>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include "services/batch_service.hpp"

namespace lucent::ui {

class BatchSetupModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("BatchSetupModel is provided by AppController.batchSetup.")
    Q_PROPERTY(Scope scope READ scope WRITE setScope NOTIFY setupChanged FINAL)
    Q_PROPERTY(int selectionCount READ selection_count NOTIFY sourcesChanged FINAL)
    Q_PROPERTY(int allCount READ all_count NOTIFY sourcesChanged FINAL)
    Q_PROPERTY(QUrl inputFolder READ input_folder WRITE setInputFolder NOTIFY setupChanged FINAL)
    Q_PROPERTY(bool includeSubfolders READ include_subfolders WRITE setIncludeSubfolders NOTIFY setupChanged FINAL)
    Q_PROPERTY(QUrl outputFolder READ output_folder WRITE setOutputFolder NOTIFY setupChanged FINAL)
    Q_PROPERTY(Preset preset READ preset WRITE setPreset NOTIFY setupChanged FINAL)
    Q_PROPERTY(bool displayP3 READ display_p3 WRITE setDisplayP3 NOTIFY setupChanged FINAL)
    Q_PROPERTY(QString namePattern READ name_pattern WRITE setNamePattern NOTIFY setupChanged FINAL)
    // Why the pattern cannot be used, checked locally as the user types; empty when it can.
    Q_PROPERTY(QString patternProblem READ pattern_problem NOTIFY setupChanged FINAL)
    Q_PROPERTY(bool mirrorSubfolders READ mirror_subfolders WRITE setMirrorSubfolders NOTIFY setupChanged FINAL)
    Q_PROPERTY(bool overwrite READ overwrite NOTIFY setupChanged FINAL)
    Q_PROPERTY(bool skipExisting READ skip_existing WRITE setSkipExisting NOTIFY setupChanged FINAL)
    Q_PROPERTY(bool includeCapture READ include_capture WRITE setIncludeCapture NOTIFY setupChanged FINAL)
    Q_PROPERTY(bool includeRights READ include_rights WRITE setIncludeRights NOTIFY setupChanged FINAL)
    Q_PROPERTY(QString artist READ artist WRITE setArtist NOTIFY setupChanged FINAL)
    Q_PROPERTY(QString copyright READ copyright WRITE setCopyright NOTIFY setupChanged FINAL)
    Q_PROPERTY(bool includeGps READ include_gps WRITE setIncludeGps NOTIFY setupChanged FINAL)
    Q_PROPERTY(Automatic automatic READ automatic WRITE setAutomatic NOTIFY setupChanged FINAL)
    // Kept consistent only with Auto or Analyse only (the choice is remembered while Off).
    Q_PROPERTY(bool keepConsistent READ keep_consistent WRITE setKeepConsistent NOTIFY setupChanged FINAL)
    // What the chosen automatic settings do, in one or two sentences.
    Q_PROPERTY(QString automaticNote READ automatic_note NOTIFY setupChanged FINAL)
    // Analysing only: nothing is exported (Start reads "Start analysis").
    Q_PROPERTY(bool analyzeOnly READ analyze_only NOTIFY setupChanged FINAL)
    // True while the dialog is open: only then are plans requested.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)

    Q_PROPERTY(bool planning READ planning NOTIFY planChanged FINAL)
    Q_PROPERTY(bool planValid READ plan_valid NOTIFY planChanged FINAL)
    Q_PROPERTY(QString planError READ plan_error NOTIFY planChanged FINAL)
    Q_PROPERTY(bool folderNotListed READ folder_not_listed NOTIFY planChanged FINAL)
    // [{source, destination, note}] for the first few photos.
    Q_PROPERTY(QVariantList examples READ examples NOTIFY planChanged FINAL)
    Q_PROPERTY(int outputs READ outputs NOTIFY planChanged FINAL)
    Q_PROPERTY(int alreadyExported READ already_exported NOTIFY planChanged FINAL)
    Q_PROPERTY(int renamed READ renamed NOTIFY planChanged FINAL)
    Q_PROPERTY(int replaced READ replaced NOTIFY planChanged FINAL)
    Q_PROPERTY(int excludedCount READ excluded_count NOTIFY planChanged FINAL)
    // [{source, reason}] of the photos left out.
    Q_PROPERTY(QVariantList exclusions READ exclusions NOTIFY planChanged FINAL)
    // "5 photos will be exported · 1 gets a new name · 2 already exported (skipped)".
    Q_PROPERTY(QString countsText READ counts_text NOTIFY planChanged FINAL)
    // "Up to 480 MB · 120 GB free on the drive".
    Q_PROPERTY(QString spaceText READ space_text NOTIFY planChanged FINAL)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY planChanged FINAL)
    // What the automatic settings add to this plan (their cost, what is written).
    Q_PROPERTY(QStringList analysisNotes READ analysis_notes NOTIFY planChanged FINAL)
    // Start is possible without a local problem (photos, output folder, pattern) unless the latest
    // plan failed or found nothing to export; while a plan is being computed it stays possible (the
    // service validates everything again when the batch is created).
    Q_PROPERTY(bool canStart READ can_start NOTIFY planChanged FINAL)
    // Why Start is disabled; empty when it is enabled.
    Q_PROPERTY(QString startHint READ start_hint NOTIFY planChanged FINAL)

public:
    enum Scope { Selection = 1, AllPhotos = 2, Folder = 3 };
    Q_ENUM(Scope)
    enum Preset { Jpeg = 1, Tiff16 = 2 };
    Q_ENUM(Preset)
    // Values of lucent::ui::BatchAutomatic.
    enum Automatic { AutomaticOff = 0, AutomaticAuto = 1, AutomaticAnalyzeOnly = 2 };
    Q_ENUM(Automatic)

    // `service` must outlive the model.
    explicit BatchSetupModel(BatchService& service, QObject* parent = nullptr);

    // --- QML API ---------------------------------------------------------------------------------
    Q_INVOKABLE void confirmOverwrite();  // after the dialog's explicit confirmation
    Q_INVOKABLE void useUniqueNames();
    Q_INVOKABLE void resetNamePattern();
    // The output folder as text (native separators); empty when none is chosen.
    Q_INVOKABLE QString outputFolderText() const;
    Q_INVOKABLE QString inputFolderText() const;

    [[nodiscard]] Scope scope() const noexcept { return scope_; }
    void setScope(Scope scope);
    [[nodiscard]] int selection_count() const noexcept { return static_cast<int>(selection_.size()); }
    [[nodiscard]] int all_count() const noexcept { return static_cast<int>(all_.size()); }
    [[nodiscard]] QUrl input_folder() const { return input_folder_; }
    void setInputFolder(const QUrl& folder);
    [[nodiscard]] bool include_subfolders() const noexcept { return include_subfolders_; }
    void setIncludeSubfolders(bool include);
    [[nodiscard]] QUrl output_folder() const { return output_folder_; }
    void setOutputFolder(const QUrl& folder);
    [[nodiscard]] Preset preset() const noexcept { return preset_; }
    void setPreset(Preset preset);
    [[nodiscard]] bool display_p3() const noexcept { return display_p3_; }
    void setDisplayP3(bool p3);
    [[nodiscard]] QString name_pattern() const { return name_pattern_; }
    void setNamePattern(const QString& pattern);
    [[nodiscard]] QString pattern_problem() const { return pattern_problem_; }
    [[nodiscard]] bool mirror_subfolders() const noexcept { return mirror_subfolders_; }
    void setMirrorSubfolders(bool mirror);
    [[nodiscard]] bool overwrite() const noexcept { return overwrite_; }
    [[nodiscard]] bool skip_existing() const noexcept { return skip_existing_; }
    void setSkipExisting(bool skip);
    [[nodiscard]] bool include_capture() const noexcept { return include_capture_; }
    void setIncludeCapture(bool include);
    [[nodiscard]] bool include_rights() const noexcept { return include_rights_; }
    void setIncludeRights(bool include);
    [[nodiscard]] QString artist() const { return artist_; }
    void setArtist(const QString& artist);
    [[nodiscard]] QString copyright() const { return copyright_; }
    void setCopyright(const QString& copyright);
    [[nodiscard]] bool include_gps() const noexcept { return include_gps_; }
    void setIncludeGps(bool include);
    [[nodiscard]] Automatic automatic() const noexcept { return automatic_; }
    void setAutomatic(Automatic automatic);
    [[nodiscard]] bool keep_consistent() const noexcept { return keep_consistent_; }
    void setKeepConsistent(bool keep);
    [[nodiscard]] QString automatic_note() const;
    [[nodiscard]] bool analyze_only() const noexcept { return automatic_ == AutomaticAnalyzeOnly; }
    // The plan's; before a folder's photos are listed (no plan yet), what the analysis costs.
    [[nodiscard]] QStringList analysis_notes() const;
    [[nodiscard]] bool active() const noexcept { return active_; }
    void setActive(bool active);

    [[nodiscard]] bool planning() const noexcept { return plan_token_ != 0 || plan_timer_.isActive(); }
    [[nodiscard]] bool plan_valid() const noexcept { return plan_.ok; }
    [[nodiscard]] QString plan_error() const { return plan_.error; }
    [[nodiscard]] bool folder_not_listed() const noexcept { return plan_.folder_not_listed; }
    [[nodiscard]] QVariantList examples() const;
    [[nodiscard]] int outputs() const noexcept { return static_cast<int>(plan_.outputs); }
    [[nodiscard]] int already_exported() const noexcept { return static_cast<int>(plan_.already_exported); }
    [[nodiscard]] int renamed() const noexcept { return static_cast<int>(plan_.renamed); }
    [[nodiscard]] int replaced() const noexcept { return static_cast<int>(plan_.replaced); }
    [[nodiscard]] int excluded_count() const noexcept { return static_cast<int>(plan_.excluded); }
    [[nodiscard]] QVariantList exclusions() const;
    [[nodiscard]] QString counts_text() const;
    [[nodiscard]] QString space_text() const;
    [[nodiscard]] QStringList warnings() const { return plan_.warnings; }
    [[nodiscard]] bool can_start() const { return start_hint().isEmpty(); }
    [[nodiscard]] QString start_hint() const;

    // --- C++ API ---------------------------------------------------------------------------------

    // The Library's selection and every listed photo, in display order. The scope becomes Selection
    // when several photos are selected, otherwise All photos. The collision policy returns to unique
    // names and GPS to off: neither is kept between batches.
    void prepare(const QList<ItemId>& selection, const QList<ItemId>& all);
    // A folder batch of `folder`.
    void prepare_folder(const QUrl& folder, bool include_subfolders);
    // What the dialog describes.
    [[nodiscard]] BatchSetup setup() const;
    // The name the Exports view shows while the batch is prepared.
    [[nodiscard]] QString batch_label() const;
    // Submits setup() (BatchService::submit) when can_start(); returns its token, or 0.
    quint64 start();
    void set_plan_delay(std::chrono::milliseconds delay);
    // The token of the plan request in flight (0 when none; tests).
    [[nodiscard]] quint64 plan_token() const noexcept { return plan_token_; }
    // Remembered between sessions by Lucent.exe (QSettings): artist, copyright, output folder,
    // pattern and preset. Restoring does not request a plan.
    void restore(const QUrl& output_folder, const QString& pattern, Preset preset, bool display_p3,
                 const QString& artist, const QString& copyright);

signals:
    void setupChanged();
    void sourcesChanged();
    void planChanged();
    void activeChanged();
    // Artist, copyright, output folder, pattern or preset changed (to be remembered).
    void rememberedChanged();

private:
    void changed(bool remembered = false);
    void request_plan();
    void on_plan_ready(quint64 token, const BatchPlanInfo& plan);
    [[nodiscard]] QString basic_problem() const;
    // Analysing only: the photos the plan will analyse (the selected ones not left out); -1 when
    // not known (a folder batch).
    [[nodiscard]] qint64 photos_to_analyse() const;

    BatchService& service_;
    QList<ItemId> selection_;
    QList<ItemId> all_;
    Scope scope_ = AllPhotos;
    QUrl input_folder_;
    bool include_subfolders_ = true;
    QUrl output_folder_;
    Preset preset_ = Jpeg;
    bool display_p3_ = false;
    QString name_pattern_;
    QString pattern_problem_;
    bool mirror_subfolders_ = false;
    bool overwrite_ = false;
    bool skip_existing_ = true;
    bool include_capture_ = true;
    bool include_rights_ = true;
    QString artist_;
    QString copyright_;
    bool include_gps_ = false;
    Automatic automatic_ = AutomaticOff;
    bool keep_consistent_ = false;
    bool active_ = false;

    QTimer plan_timer_;
    quint64 plan_token_ = 0;  // the request in flight
    BatchPlanInfo plan_;
};

}  // namespace lucent::ui
