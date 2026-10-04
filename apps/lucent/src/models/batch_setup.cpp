#include "models/batch_setup.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QVariantMap>

#include "models/batch_presentation.hpp"

namespace lucent::ui {
namespace {

constexpr int kDefaultPlanDelayMs = 300;
constexpr qsizetype kMaxPatternBytes = 200;  // app::kMaxNamePatternBytes

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::BatchSetupModel", text);
}

QString native_path(const QUrl& url) {
    return url.isLocalFile() ? QDir::toNativeSeparators(QDir::cleanPath(url.toLocalFile())) : QString();
}

// The rules of app::parse_name_pattern (batch_destination.hpp), checked as the user types; the
// service checks again when it plans.
QString check_pattern(const QString& pattern) {
    if (pattern.trimmed().isEmpty()) {
        return tr_text("Enter a name pattern, for example {original-stem}_Lucent_{revision}.");
    }
    if (pattern.toUtf8().size() > kMaxPatternBytes) {
        return tr_text("The name pattern is too long.");
    }
    static const QRegularExpression forbidden(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
    if (forbidden.match(pattern).hasMatch()) {
        return tr_text("A file name cannot contain \\ / : * ? \" < > | or control characters.");
    }
    static const QRegularExpression token(QStringLiteral("\\{([^{}]*)\\}"));
    for (auto it = token.globalMatch(pattern); it.hasNext();) {
        const QString name = it.next().captured(1);
        if (name != u"original-stem" && name != u"revision" && name != u"image" && name != u"sequence") {
            return tr_text("{%1} is not a token. Use {original-stem}, {revision}, {image} or {sequence}.").arg(name);
        }
    }
    QString without = pattern;
    without.remove(token);
    if (without.contains(u'{') || without.contains(u'}')) {
        return tr_text("Braces are only used around the tokens {original-stem}, {revision}, {image} and {sequence}.");
    }
    if (!pattern.contains(QStringLiteral("{original-stem}")) && !pattern.contains(QStringLiteral("{sequence}"))) {
        return tr_text("The pattern needs {original-stem} or {sequence}, so that every photo gets its own name.");
    }
    return {};
}

}  // namespace

BatchSetupModel::BatchSetupModel(BatchService& service, QObject* parent)
    : QObject(parent), service_(service), name_pattern_(QString::fromLatin1(kDefaultBatchNamePattern)) {
    plan_timer_.setSingleShot(true);
    plan_timer_.setInterval(kDefaultPlanDelayMs);
    connect(&plan_timer_, &QTimer::timeout, this, &BatchSetupModel::request_plan);
    connect(&service_, &BatchService::plan_ready, this, &BatchSetupModel::on_plan_ready);
}

void BatchSetupModel::set_plan_delay(std::chrono::milliseconds delay) {
    plan_timer_.setInterval(static_cast<int>(delay.count()));
}

QString BatchSetupModel::automatic_settings_note() const {
    return tr_text("Available when Natural v1 is integrated");
}

QString BatchSetupModel::outputFolderText() const {
    return native_path(output_folder_);
}

QString BatchSetupModel::inputFolderText() const {
    return native_path(input_folder_);
}

void BatchSetupModel::changed(bool remembered) {
    pattern_problem_ = check_pattern(name_pattern_);
    emit setupChanged();
    if (remembered) {
        emit rememberedChanged();
    }
    if (active_) {
        plan_timer_.start();  // restarts the quiet period
        emit planChanged();   // planning() is true from now on
    }
}

void BatchSetupModel::setScope(Scope scope) {
    if (scope != Selection && scope != AllPhotos && scope != Folder) {
        return;
    }
    if (scope != scope_) {
        scope_ = scope;
        changed();
    }
}

void BatchSetupModel::setInputFolder(const QUrl& folder) {
    if (folder != input_folder_) {
        input_folder_ = folder;
        changed();
    }
}

void BatchSetupModel::setIncludeSubfolders(bool include) {
    if (include != include_subfolders_) {
        include_subfolders_ = include;
        changed();
    }
}

void BatchSetupModel::setOutputFolder(const QUrl& folder) {
    if (folder == output_folder_) {
        return;
    }
    output_folder_ = folder;
    // A confirmation to replace files names the folder: it does not carry over to another one.
    overwrite_ = false;
    changed(true);
}

void BatchSetupModel::setPreset(Preset preset) {
    if ((preset == Jpeg || preset == Tiff16) && preset != preset_) {
        preset_ = preset;
        changed(true);
    }
}

void BatchSetupModel::setDisplayP3(bool p3) {
    if (p3 != display_p3_) {
        display_p3_ = p3;
        changed(true);
    }
}

void BatchSetupModel::setNamePattern(const QString& pattern) {
    if (pattern != name_pattern_) {
        name_pattern_ = pattern;
        changed(true);
    }
}

void BatchSetupModel::resetNamePattern() {
    setNamePattern(QString::fromLatin1(kDefaultBatchNamePattern));
}

void BatchSetupModel::setMirrorSubfolders(bool mirror) {
    if (mirror != mirror_subfolders_) {
        mirror_subfolders_ = mirror;
        changed();
    }
}

void BatchSetupModel::confirmOverwrite() {
    if (!overwrite_) {
        overwrite_ = true;
        changed();
    }
}

void BatchSetupModel::useUniqueNames() {
    if (overwrite_) {
        overwrite_ = false;
        changed();
    }
}

void BatchSetupModel::setSkipExisting(bool skip) {
    if (skip != skip_existing_) {
        skip_existing_ = skip;
        changed();
    }
}

void BatchSetupModel::setIncludeCapture(bool include) {
    if (include != include_capture_) {
        include_capture_ = include;
        changed();
    }
}

void BatchSetupModel::setIncludeRights(bool include) {
    if (include != include_rights_) {
        include_rights_ = include;
        changed();
    }
}

void BatchSetupModel::setArtist(const QString& artist) {
    if (artist != artist_) {
        artist_ = artist;
        changed(true);
    }
}

void BatchSetupModel::setCopyright(const QString& copyright) {
    if (copyright != copyright_) {
        copyright_ = copyright;
        changed(true);
    }
}

void BatchSetupModel::setIncludeGps(bool include) {
    if (include != include_gps_) {
        include_gps_ = include;
        changed();
    }
}

void BatchSetupModel::setActive(bool active) {
    if (active == active_) {
        return;
    }
    active_ = active;
    emit activeChanged();
    if (active_) {
        plan_timer_.start();
    } else {
        plan_timer_.stop();
        plan_token_ = 0;  // a late answer is ignored
    }
    emit planChanged();
}

void BatchSetupModel::prepare(const QList<ItemId>& selection, const QList<ItemId>& all) {
    selection_ = selection;
    all_ = all;
    emit sourcesChanged();
    // Several photos selected: those; otherwise every listed photo (one selected photo is usually
    // just the current one). The dialog offers the other choices.
    scope_ = selection_.size() > 1 || (all_.isEmpty() && !selection_.isEmpty()) ? Selection : AllPhotos;
    overwrite_ = false;
    include_gps_ = false;
    changed();
}

void BatchSetupModel::prepare_folder(const QUrl& folder, bool include_subfolders) {
    input_folder_ = folder;
    include_subfolders_ = include_subfolders;
    scope_ = Folder;
    overwrite_ = false;
    include_gps_ = false;
    changed();
}

void BatchSetupModel::restore(const QUrl& output_folder, const QString& pattern, Preset preset, bool display_p3,
                              const QString& artist, const QString& copyright) {
    output_folder_ = output_folder;
    if (!pattern.isEmpty()) {
        name_pattern_ = pattern;
    }
    preset_ = preset == Tiff16 ? Tiff16 : Jpeg;
    display_p3_ = display_p3;
    artist_ = artist;
    copyright_ = copyright;
    pattern_problem_ = check_pattern(name_pattern_);
    emit setupChanged();
}

BatchSetup BatchSetupModel::setup() const {
    BatchSetup setup;
    switch (scope_) {
    case Selection:
        setup.items = selection_;
        break;
    case AllPhotos:
        setup.items = all_;
        break;
    case Folder:
        setup.input_folder = input_folder_.isLocalFile() ? QDir::cleanPath(input_folder_.toLocalFile()) : QString();
        setup.include_subfolders = include_subfolders_;
        if (!setup.input_folder.isEmpty()) {
            const QString name = QFileInfo(setup.input_folder).fileName();
            setup.name = tr_text("Export of %1")
                             .arg(name.isEmpty() ? QDir::toNativeSeparators(setup.input_folder) : name);
        }
        break;
    }
    setup.output_folder = output_folder_.isLocalFile() ? QDir::cleanPath(output_folder_.toLocalFile()) : QString();
    if (preset_ == Tiff16) {
        setup.output.format = ExportFileFormat::Tiff16;
        setup.output.color_space = display_p3_ ? ExportColorSpace::DisplayP3 : ExportColorSpace::Srgb;
    } else {
        // The sharing preset: JPEG / sRGB / quality 95 (blueprint section 7).
        setup.output.format = ExportFileFormat::Jpeg;
        setup.output.color_space = ExportColorSpace::Srgb;
        setup.output.jpeg_quality = kDefaultJpegQuality;
    }
    setup.name_pattern = name_pattern_;
    setup.mirror_subfolders = mirror_subfolders_;
    setup.collisions = overwrite_ ? BatchCollisions::Overwrite : BatchCollisions::UniqueName;
    setup.existing = skip_existing_ ? BatchExisting::Skip : BatchExisting::ExportAgain;
    setup.include_capture = include_capture_;
    setup.include_rights = include_rights_;
    setup.artist = artist_;
    setup.copyright = copyright_;
    setup.include_gps = include_gps_;
    return setup;
}

QString BatchSetupModel::batch_label() const {
    const BatchSetup described = setup();
    if (!described.name.isEmpty()) {
        return described.name;
    }
    return tr_text("Export of %1").arg(presentation::photo_count_label(described.items.size()));
}

QString BatchSetupModel::basic_problem() const {
    switch (scope_) {
    case Selection:
        if (selection_.isEmpty()) {
            return tr_text("Select photos in the library first.");
        }
        break;
    case AllPhotos:
        if (all_.isEmpty()) {
            return tr_text("The library has no photos yet.");
        }
        break;
    case Folder:
        if (!input_folder_.isLocalFile()) {
            return tr_text("Choose the folder of photos to export.");
        }
        break;
    }
    if (!output_folder_.isLocalFile()) {
        return tr_text("Choose an output folder.");
    }
    return pattern_problem_;
}

QString BatchSetupModel::start_hint() const {
    if (const QString problem = basic_problem(); !problem.isEmpty()) {
        return problem;
    }
    // While a plan is computed (a few hundred milliseconds after each change) the last check stands:
    // the service validates everything again when the batch is created.
    if (planning() || !active_) {
        return {};
    }
    if (!plan_.ok) {
        return plan_.folder_not_listed ? QString() : plan_.error;
    }
    if (plan_.outputs + plan_.already_exported == 0) {
        return tr_text("None of these photos can be exported (see the reasons below).");
    }
    return {};
}

quint64 BatchSetupModel::start() {
    if (!basic_problem().isEmpty()) {
        return 0;
    }
    if (active_ && !can_start()) {
        return 0;
    }
    return service_.submit(setup());
}

void BatchSetupModel::request_plan() {
    if (!active_) {
        return;
    }
    if (!basic_problem().isEmpty()) {
        // Nothing to ask the service: the dialog shows the local problem instead of a plan.
        plan_token_ = 0;
        plan_ = BatchPlanInfo{};
        emit planChanged();
        return;
    }
    plan_token_ = service_.request_plan(setup());
    emit planChanged();
}

void BatchSetupModel::on_plan_ready(quint64 token, const BatchPlanInfo& plan) {
    if (token == 0 || token != plan_token_) {
        return;  // superseded by a newer request (or the dialog closed)
    }
    plan_token_ = 0;
    if (plan_timer_.isActive()) {
        return;  // the setup changed meanwhile; the next plan follows
    }
    plan_ = plan;
    emit planChanged();
}

QVariantList BatchSetupModel::examples() const {
    QVariantList list;
    for (const BatchPlanExample& example : plan_.examples) {
        QString note;
        if (example.already_exported) {
            note = tr_text("already exported: skipped");
        } else if (example.replaces) {
            note = tr_text("replaces the existing file");
        } else if (example.renamed) {
            note = tr_text("a number is added: the name is taken");
        }
        list.push_back(QVariantMap{{QStringLiteral("source"), example.source_name},
                                   {QStringLiteral("destination"), example.destination},
                                   {QStringLiteral("note"), note},
                                   {QStringLiteral("skipped"), example.already_exported}});
    }
    return list;
}

QVariantList BatchSetupModel::exclusions() const {
    QVariantList list;
    for (const BatchPlanExclusion& exclusion : plan_.exclusions) {
        list.push_back(QVariantMap{{QStringLiteral("source"), exclusion.source_name},
                                   {QStringLiteral("reason"), exclusion.reason}});
    }
    return list;
}

QString BatchSetupModel::counts_text() const {
    if (plan_.folder_not_listed) {
        return tr_text("The folder's photos are added to the library when the batch starts, then exported.");
    }
    if (!plan_.ok) {
        return {};
    }
    QStringList parts;
    parts << (plan_.outputs == 1 ? tr_text("1 photo will be exported")
                                 : tr_text("%1 photos will be exported").arg(plan_.outputs));
    if (plan_.renamed > 0) {
        parts << (plan_.renamed == 1 ? tr_text("1 gets a number added to its name")
                                     : tr_text("%1 get a number added to their names").arg(plan_.renamed));
    }
    if (plan_.replaced > 0) {
        parts << (plan_.replaced == 1 ? tr_text("1 existing file will be replaced")
                                      : tr_text("%1 existing files will be replaced").arg(plan_.replaced));
    }
    if (plan_.already_exported > 0) {
        parts << (skip_existing_ ? tr_text("%1 already exported (skipped)").arg(plan_.already_exported)
                                 : tr_text("%1 already exported").arg(plan_.already_exported));
    }
    if (plan_.excluded > 0) {
        parts << (plan_.excluded == 1 ? tr_text("1 left out") : tr_text("%1 left out").arg(plan_.excluded));
    }
    if (plan_.to_hash > 0) {
        parts << (plan_.to_hash == 1 ? tr_text("1 original is fingerprinted first")
                                     : tr_text("%1 originals are fingerprinted first").arg(plan_.to_hash));
    }
    return parts.join(QStringLiteral(" · "));
}

QString BatchSetupModel::space_text() const {
    if (!plan_.ok) {
        return {};
    }
    const QLocale locale;
    QStringList parts;
    if (plan_.outputs > 0) {
        parts << tr_text("Up to %1")
                     .arg(locale.formattedDataSize(plan_.estimated_bytes, 1,
                                                   QLocale::DataSizeTraditionalFormat));
    }
    if (plan_.free_bytes >= 0) {
        parts << tr_text("%1 free on the drive")
                     .arg(locale.formattedDataSize(plan_.free_bytes, 1, QLocale::DataSizeTraditionalFormat));
    }
    if (!plan_.folder_exists) {
        parts << tr_text("the folder will be created");
    }
    return parts.join(QStringLiteral(" · "));
}

}  // namespace lucent::ui
