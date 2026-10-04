#pragma once

// BatchService: the shell's view of durable batch exports (blueprint section 2, "Automatic batch
// development" steps 1 to 5; section 7, "Export integrity and user-selected destinations";
// ADR 0012). The application service behind it (app::BatchService) persists every batch in the
// catalog's job journal, renders in isolated worker processes and publishes with a no-clobber,
// verified protocol; single exports are batches of one. W8: automatic batches (BatchAutomatic)
// analyse every photo in its worker first and export (or, analysing only, keep) the automatic
// revision; their snapshots carry the analysis progress and their reports what Auto did.
//
// Threading contract: as LibraryService (library_service.hpp). The object lives on the UI thread,
// member functions return promptly (planning, submitting, status reads, commands and reports run
// elsewhere), and signals are emitted on the UI thread. Progress is coalesced: however many jobs
// a batch has and however often they report, batch_changed() arrives at most a few times a
// second per batch.
//
// Strings are user-facing unless stated otherwise; paths use native separators.

#include <cstdint>
#include <optional>

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

#include "services/service_types.hpp"

namespace lucent::ui {

// The catalog's batch id; stable across restarts. Zero is never valid.
using BatchId = std::uint64_t;

// The default naming pattern (app::kDefaultNamePattern).
inline constexpr char kDefaultBatchNamePattern[] = "{original-stem}_Lucent_{revision}";

enum class BatchCollisions : std::uint8_t {
    UniqueName = 1,  // a free name with "_2", "_3", ... (default; nothing is ever replaced)
    Overwrite = 2,   // replace an existing file of the same name (never an original)
};

enum class BatchExisting : std::uint8_t {
    Skip = 1,         // an identical output already in the folder is not exported again
    ExportAgain = 2,  // exported again under a new name
};

// Automatic settings of a batch (blueprint section 2, "Automatic batch development": Lucent
// "analyzes each image, chooses its own settings, validates the result, and exports
// automatically"; "An optional Analyze only mode produces editable previews before export";
// ADR 0013). Numeric values are app::AutomaticSettings'.
enum class BatchAutomatic : std::uint8_t {
    Off = 0,          // each photo is exported with its own current edit
    Auto = 1,         // each photo is analysed, saved as an automatic revision, checked and exported
    AnalyzeOnly = 2,  // each photo is analysed and saved as an automatic revision; nothing is exported
};

// What the Export batch dialog asks for (blueprint section 2 step 3).
struct BatchSetup {
    QList<ItemId> items;      // the photographs (a selection or the listed photos)
    QString input_folder;     // a folder batch (imported first); empty otherwise
    bool include_subfolders = true;
    QString output_folder;    // absolute; created when it does not exist
    ExportOptions output;     // JPEG / sRGB / quality 95, or 16-bit TIFF with a profile
    QString name_pattern = QString::fromLatin1(kDefaultBatchNamePattern);
    bool mirror_subfolders = false;
    BatchCollisions collisions = BatchCollisions::UniqueName;
    BatchExisting existing = BatchExisting::Skip;
    bool include_capture = true;  // camera, lens, exposure, capture time
    bool include_rights = true;   // Artist / Copyright
    QString artist;               // with include_rights: written when not empty
    QString copyright;
    bool include_gps = false;     // never on unless chosen
    QString name;                 // shown in lists; empty: "Export of <n> photos"
    BatchAutomatic automatic = BatchAutomatic::Off;
    // Related photographs are kept visually consistent (with Auto or AnalyzeOnly only).
    bool keep_consistent = false;
};

struct BatchPlanExample {
    QString source_name;
    QString destination;  // the full output path (an already exported one: the existing file)
    bool renamed = false;
    bool replaces = false;
    bool already_exported = false;  // skipped: an identical output is already in the folder
};

struct BatchPlanExclusion {
    QString source_name;
    QString reason;
};

// The plan of a setup: example destination paths and counts before anything is written
// (BatchService::request_plan).
struct BatchPlanInfo {
    bool ok = false;
    QString error;           // why there is no plan (an invalid pattern, nothing selected, ...)
    // A folder batch whose photos are not in the library yet: they are added when the batch starts,
    // so the examples and counts appear only after that.
    bool folder_not_listed = false;
    QString folder;          // the output folder
    bool folder_exists = false;
    QList<BatchPlanExample> examples;
    qint64 outputs = 0;           // photographs that will be exported
    qint64 to_hash = 0;           // originals fingerprinted before the batch starts
    qint64 already_exported = 0;  // identical outputs already in the folder
    qint64 renamed = 0;           // outputs that get a "_n" suffix
    qint64 replaced = 0;          // existing files that will be replaced
    qint64 excluded = 0;          // photographs left out (with reasons below)
    QList<BatchPlanExclusion> exclusions;  // at most a few hundred
    qint64 estimated_bytes = 0;   // upper bound of all outputs
    qint64 largest_bytes = 0;
    qint64 free_bytes = -1;       // of the destination drive; -1 when unknown
    QStringList warnings;
    // Automatic settings: what the analysis adds (its cost, what is written), one sentence each.
    QStringList automatic_notes;
};

enum class BatchState : std::uint8_t {
    Draft = 1,
    Running = 2,
    Paused = 3,
    Cancelled = 4,
    Completed = 5,
};

// Why the service paused a batch by itself (app::BatchAttention).
enum class BatchAttention : std::uint8_t {
    None = 0,
    DestinationFull = 1,
    DestinationUnavailable = 2,
    DestinationPermission = 3,
    // Another folder is at the output folder's path (created again, or another drive got its
    // letter): BatchService::use_replaced_folder continues there.
    DestinationReplaced = 4,
};

struct BatchRunningJob {
    qint64 job = 0;  // the catalog's job id
    ItemId item = 0;
    QString source_name;
    QString stage;  // "Rendering", ...
    double fraction = 0.0;
};

// One batch as a value snapshot (BatchService sends a complete new one whenever it changes).
struct BatchSnapshot {
    BatchId id = 0;
    QString name;
    QString selection;  // what was selected ("5 selected photos", "the folder D:\Shoot")
    QString folder;     // the output folder
    BatchState state = BatchState::Draft;
    qint64 total = 0;
    qint64 completed = 0;  // exported
    qint64 queued = 0;
    qint64 running = 0;    // being rendered, verified or published
    qint64 failed = 0;
    qint64 skipped = 0;
    qint64 cancelled = 0;
    qint64 review = -1;    // exported outputs recommended for review; -1 until counted
    qint64 excluded = -1;  // photos left out when the batch was created; -1 until counted
    qint64 retryable = 0;  // failed photos that Retry failed exports again
    // An interactive single export (Export in Edit), shown as one photo; a batch from the dialog
    // is a batch whatever its size.
    bool single_export = false;
    // A batch of one photo that ended without exporting it: why (failed, skipped, or cancelled
    // because of the destination); empty otherwise.
    QString outcome_reason;
    BatchAttention attention = BatchAttention::None;
    QString attention_message;  // what happened and what to do; empty without an attention
    QList<BatchRunningJob> running_jobs;
    double eta_seconds = -1.0;  // only when the service has measured enough; -1 otherwise
    QDateTime created;
    QDateTime finished;     // invalid while not finished
    QString last_output;    // the most recently published file of this session
    QString report_path;    // the HTML report once written; empty otherwise
    // Automatic settings (W8). An AnalyzeOnly batch's `completed` photos were analysed, not exported.
    BatchAutomatic automatic = BatchAutomatic::Off;
    bool keep_consistent = false;
    // While an automatic batch has work left: its unfinished photos not analysed yet, and the photos
    // whose analysis is recorded (any stage); -1 when not known (finished, or not automatic).
    qint64 analysis_pending = -1;
    qint64 analysis_recorded = -1;

    [[nodiscard]] qint64 remaining() const noexcept { return queued + running; }
    [[nodiscard]] bool single() const noexcept { return single_export; }
    [[nodiscard]] bool finished_state() const noexcept {
        return state == BatchState::Completed || (state == BatchState::Cancelled && running == 0);
    }
};

enum class BatchOutcome : std::uint8_t {
    Exported = 1,
    Skipped = 2,
    Failed = 3,
    Cancelled = 4,
    Pending = 5,
    Analysed = 6,  // AnalyzeOnly: automatic revisions written, nothing exported
};

struct BatchReportEntry {
    ItemId item = 0;
    QString source_name;
    QString source_path;
    BatchOutcome outcome = BatchOutcome::Pending;
    QString output;  // the exported (or existing) file
    QString reason;  // what happened (failed, skipped, cancelled)
    QString action;  // what to do; may be empty
    QStringList review;  // why a review is recommended; empty when it is not
    int attempts = 0;
    // Automatic batches: what automatic settings did to the photo (its effective automatic
    // revision's analysis record). `automatic` false for other batches.
    bool automatic = false;
    quint32 revision_number = 0;  // the effective automatic revision; 0 until it is saved
    double confidence = -1.0;     // 0..1; -1 unknown
    int attempt = 0;              // 0: Auto's choice; n: the n-th conservative fallback
    QStringList flags;            // user-facing findings, one per flag
    bool locks_prevented = false;  // locked controls kept Auto from its usual targets
    QStringList locks;            // user-facing: what the locks did
    QString group;                // the related-shot group it was matched to; empty: none
};

// The report of one batch: exported, skipped, failed and review-recommended photographs with
// reasons (blueprint section 2 step 5).
struct BatchReportInfo {
    BatchId id = 0;
    bool ok = false;
    QString error;  // why the report could not be read
    QString name;
    QString folder;
    BatchState state = BatchState::Draft;
    qint64 exported = 0;
    qint64 skipped = 0;
    qint64 failed = 0;
    qint64 cancelled = 0;
    qint64 pending = 0;
    qint64 review = 0;
    qint64 analysed = 0;  // AnalyzeOnly: photos with automatic revisions
    BatchAutomatic automatic = BatchAutomatic::Off;
    QString format;  // "JPEG · sRGB · 8-bit" from an exported output; empty when none
    QList<BatchReportEntry> entries;  // batch order, then photographs left out at the start
    QStringList notes;
    QDateTime created;
    QDateTime finished;
};

class BatchService : public QObject {
    Q_OBJECT

public:
    explicit BatchService(QObject* parent = nullptr) : QObject(parent) {}
    ~BatchService() override = default;

    BatchService(const BatchService&) = delete;
    BatchService& operator=(const BatchService&) = delete;

    // False when this session cannot keep durable batches (the library is not saved, or the batch
    // service could not start); unavailable_reason() says why. Every request then fails at once.
    [[nodiscard]] virtual bool is_available() const = 0;
    [[nodiscard]] virtual QString unavailable_reason() const = 0;

    // Plans `setup` (nothing is written). Returns a token; plan_ready(token, ...) follows unless a
    // newer request supersedes this one first (its work is cancelled and nothing is emitted).
    virtual quint64 request_plan(const BatchSetup& setup) = 0;

    // Creates and starts a batch: the folder is imported first and unhashed originals are
    // fingerprinted (submit_progress), then the batch is written and dispatched. Returns a token;
    // submitted(token, ...) follows exactly once. Submissions run one at a time in order.
    virtual quint64 submit(const BatchSetup& setup) = 0;
    // Stops a submission that has not written its batch yet (submitted() reports Cancelled).
    virtual void cancel_submit(quint64 token) = 0;

    // Batch commands; failures arrive as command_failed().
    virtual void pause(BatchId batch) = 0;
    virtual void resume(BatchId batch) = 0;  // also after an attention, once the problem is fixed
    // After BatchAttention::DestinationReplaced: continue in the folder now at the output path.
    virtual void use_replaced_folder(BatchId batch) = 0;
    virtual void cancel(BatchId batch) = 0;
    virtual void retry_failed(BatchId batch) = 0;

    // Lists the newest batches and every batch with work left, whatever its age
    // (batches_listed).
    virtual void refresh() = 0;
    // Lists the next page of older batches (batches_listed); older_batches(false) once there are
    // no more.
    virtual void load_older() = 0;
    // Reads the report of `batch` (report_ready).
    virtual void request_report(BatchId batch) = 0;
    // Makes sure the readable HTML report of a finished batch exists in its output folder (writing
    // it when needed) and reports its path (report_file_ready; an empty path with the reason when
    // it cannot be written).
    virtual void request_report_file(BatchId batch) = 0;

    // True while a submission runs or a batch has jobs to run (not while it is paused).
    [[nodiscard]] virtual bool is_busy() const = 0;

signals:
    void plan_ready(quint64 token, const lucent::ui::BatchPlanInfo& plan);
    // `text` describes the step ("Adding the folder's photos… 12 of 40"); fraction 0..1 or -1.
    void submit_progress(quint64 token, const QString& text, double fraction);
    // `batch` is 0 when the batch was not created; `error` then says why.
    void submitted(quint64 token, lucent::ui::BatchId batch, const QString& error);
    // Batches the service lists (newest first): they are added to (or update) what is known.
    void batches_listed(const QList<lucent::ui::BatchSnapshot>& batches);
    // Whether older batches than those listed exist (load_older).
    void older_batches(bool available);
    // A complete new snapshot of one batch (also for a batch not listed before: a new one).
    void batch_changed(const lucent::ui::BatchSnapshot& batch);
    void report_ready(lucent::ui::BatchId batch, const lucent::ui::BatchReportInfo& report);
    void report_file_ready(lucent::ui::BatchId batch, const QString& path, const QString& error);
    void command_failed(lucent::ui::BatchId batch, const QString& message);
    void busy_changed(bool busy);
};

}  // namespace lucent::ui

Q_DECLARE_METATYPE(lucent::ui::BatchPlanInfo)
Q_DECLARE_METATYPE(lucent::ui::BatchSnapshot)
Q_DECLARE_METATYPE(lucent::ui::BatchReportInfo)
