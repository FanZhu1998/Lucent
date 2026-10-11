// Lens-profile corrections: the Lensfun database, matching and planning (lens_correction.hpp).
//
// This is the only translation unit of Lucent that includes Lensfun (a private dependency of
// lucent_engine, ADR 0023). Every Lensfun call runs under one process-wide lock (lensfun_mutex()):
// Lensfun 0.3.4 keeps mutable state no instance owns (lens.cpp: lfLens's constructor and destructor
// count the users of the lens-name regular expressions, _lf_lens_regex_refs, which
// _lf_parse_lens_name compiles lazily behind a plain flag and the last destructor frees), and
// loading (lfDatabase::Load -> lfLens::Check -> GuessParameters), searching (FindLenses builds a
// temporary lfLens) and destroying a database (~lfDatabase deletes its lenses) all touch it;
// lfDatabase::FindCamerasExt and FindLenses also write each candidate's Score into the database's
// own objects, and lfLens::GuessParameters switches LC_NUMERIC.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <lensfun/lensfun.h>

#include <lucent/engine/lens_correction.hpp>

namespace lucent::engine {

namespace {

constexpr const char* kWhereDb = "engine.lens.database";
constexpr const char* kWherePlan = "engine.lens.plan";

constexpr std::size_t kCacheEntries = 64;

// The lock of every Lensfun call in the process (see the file comment). Recursive: a database whose
// load fails is destroyed while load() holds it.
std::recursive_mutex& lensfun_mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

// ---- Lensfun helpers -----------------------------------------------------------------------------

// Arrays Lensfun returns (FindCamerasExt, FindLenses) are freed with lf_free().
template <class T>
struct LfList {
    const T** items = nullptr;
    explicit LfList(const T** list) noexcept : items(list) {}
    LfList(const LfList&) = delete;
    LfList& operator=(const LfList&) = delete;
    ~LfList() {
        if (items != nullptr) {
            lf_free(static_cast<void*>(items));
        }
    }
    [[nodiscard]] std::vector<const T*> vector() const {
        std::vector<const T*> out;
        for (std::size_t i = 0; items != nullptr && items[i] != nullptr; ++i) {
            out.push_back(items[i]);
        }
        return out;
    }
};

// The spellings of a Lensfun multi-language string: the default one first, then the translations
// ("default\0lang\0text\0...\0\0", as _lf_mlstrcmp walks it).
std::vector<std::string_view> spellings(const char* ml) {
    std::vector<std::string_view> out;
    if (ml == nullptr) {
        return out;
    }
    const char* p = ml;
    while (*p != '\0') {
        out.emplace_back(p);
        p += std::strlen(p) + 1;
        if (*p == '\0') {
            break;
        }
        p += std::strlen(p) + 1;  // the language code
    }
    return out;
}

std::string default_spelling(const char* ml) {
    const auto all = spellings(ml);
    return all.empty() ? std::string{} : std::string(all.front());
}

bool is_space(unsigned char c) noexcept {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

// Lensfun's name comparison (_lf_strcmp): leading and trailing white space ignored, runs of white
// space as one space, ASCII letters compared without case.
bool same_spelling(std::string_view a, std::string_view b) noexcept {
    const auto normalise = [](std::string_view s) {
        std::string out;
        bool space = false;
        for (const char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            if (is_space(c)) {
                space = !out.empty();
                continue;
            }
            if (space) {
                out.push_back(' ');
                space = false;
            }
            out.push_back(static_cast<char>(c < 0x80 ? std::tolower(c) : c));
        }
        return out;
    };
    return normalise(a) == normalise(b);
}

bool any_spelling_is(const char* ml, std::string_view name) {
    if (name.empty()) {
        return false;
    }
    for (const std::string_view s : spellings(ml)) {
        if (same_spelling(s, name)) {
            return true;
        }
    }
    return false;
}

std::string lens_type_name(lfLensType type) {
    switch (type) {
        case LF_RECTILINEAR:
            return "rectilinear";
        case LF_FISHEYE:
            return "fisheye";
        case LF_PANORAMIC:
            return "panoramic";
        case LF_EQUIRECTANGULAR:
            return "equirectangular";
        case LF_FISHEYE_ORTHOGRAPHIC:
            return "fisheye_orthographic";
        case LF_FISHEYE_STEREOGRAPHIC:
            return "fisheye_stereographic";
        case LF_FISHEYE_EQUISOLID:
            return "fisheye_equisolid";
        case LF_FISHEYE_THOBY:
            return "fisheye_thoby";
        default:
            return "unknown";
    }
}

std::string mounts_text(const lfLens* lens) {
    std::string out;
    for (std::size_t i = 0; lens->Mounts != nullptr && lens->Mounts[i] != nullptr; ++i) {
        out += (out.empty() ? "" : ", ") + std::string(lens->Mounts[i]);
    }
    return out;
}

LensProfileId profile_id(const lfLens* lens) {
    return LensProfileId{default_spelling(lens->Maker), default_spelling(lens->Model),
                         static_cast<double>(lens->CropFactor)};
}

std::string describe(const LensProfileId& id) {
    return std::format("{} / {} (crop factor {:.4g})", id.maker, id.model, id.crop_factor);
}

LensCameraProfile camera_profile(const lfCamera* camera) {
    return LensCameraProfile{default_spelling(camera->Maker), default_spelling(camera->Model),
                             camera->Mount != nullptr ? std::string(camera->Mount) : std::string{},
                             static_cast<double>(camera->CropFactor)};
}

bool within(double value, double reference, double tolerance) noexcept {
    return std::abs(value - reference) <= tolerance * std::abs(reference);
}

// ---- The name rule (section 1) --------------------------------------------------------------------

std::vector<std::string> remove_words(std::vector<std::string> words,
                                      const std::vector<std::string>& remove) {
    words.erase(std::remove_if(words.begin(), words.end(),
                               [&](const std::string& w) {
                                   return std::find(remove.begin(), remove.end(), w) != remove.end();
                               }),
                words.end());
    return words;
}

// The file's lens name and one of the database lens's names hold the same words once the lens
// maker's words are removed from both.
bool same_lens_name(std::string_view file_name, const lfLens* lens) {
    std::vector<std::string> maker_words;
    for (const std::string_view m : spellings(lens->Maker)) {
        for (std::string& w : lens_name_words(m)) {
            maker_words.push_back(std::move(w));
        }
    }
    const std::vector<std::string> file_words = remove_words(lens_name_words(file_name), maker_words);
    if (file_words.empty()) {
        return false;
    }
    for (const std::string_view model : spellings(lens->Model)) {
        if (remove_words(lens_name_words(model), maker_words) == file_words) {
            return true;
        }
    }
    return false;
}

bool same_lens_entry_name(const lfLens* a, const lfLens* b) {
    return same_spelling(default_spelling(a->Maker), default_spelling(b->Maker)) &&
           same_spelling(default_spelling(a->Model), default_spelling(b->Model));
}

// ---- Plan cache keys ------------------------------------------------------------------------------

struct PlanKey {
    LensQuery query;
    std::optional<LensCorrectionSetting> setting;
    CarriedLensCorrections carried;
    image::Size upright;
    image::Orientation orientation = image::Orientation::Normal;
    bool monochrome = false;

    friend bool operator==(const PlanKey&, const PlanKey&) = default;
};

template <class Key, class Value>
class Lru {
public:
    [[nodiscard]] std::optional<Value> find(const Key& key) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->first == key) {
                entries_.splice(entries_.begin(), entries_, it);
                return entries_.front().second;
            }
        }
        return std::nullopt;
    }
    void insert(Key key, Value value) noexcept {
        try {
            entries_.emplace_front(std::move(key), std::move(value));
            while (entries_.size() > kCacheEntries) {
                entries_.pop_back();
            }
        } catch (const std::bad_alloc&) {
            // Not kept: the cache is an optimisation.
        }
    }

private:
    std::list<std::pair<Key, Value>> entries_;
};

std::vector<double> sorted_unique(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

std::string list_text(const std::vector<double>& values) {
    std::string out;
    for (const double v : values) {
        out += (out.empty() ? "" : ", ") + std::format("{:.4g}", v);
    }
    return out;
}

}  // namespace

// ---- Public helpers --------------------------------------------------------------------------------

std::vector<std::string> lens_name_words(std::string_view name) {
    std::vector<std::string> out;
    const auto digit = [](unsigned char c) { return c >= '0' && c <= '9'; };
    // ispunct of the C locale: printable ASCII that is neither alphanumeric nor a space.
    const auto punct = [](unsigned char c) { return c < 0x80 && std::ispunct(c) != 0; };
    std::size_t i = 0;
    while (i < name.size()) {
        while (i < name.size() && is_space(static_cast<unsigned char>(name[i]))) {
            ++i;
        }
        if (i >= name.size()) {
            break;
        }
        const std::size_t start = i;
        const auto first = static_cast<unsigned char>(name[i++]);
        if (digit(first)) {
            while (i < name.size() && (digit(static_cast<unsigned char>(name[i])) || name[i] == '.')) {
                ++i;
            }
        } else if (punct(first)) {
            while (i < name.size() && punct(static_cast<unsigned char>(name[i]))) {
                ++i;
            }
        } else {
            while (i < name.size()) {
                const auto c = static_cast<unsigned char>(name[i]);
                if (is_space(c) || digit(c) || punct(c)) {
                    break;
                }
                ++i;
            }
        }
        // A lone punctuation mark or a lone "f" is not a word ("+" and "*" are).
        if (i - start == 1 && (punct(first) || std::tolower(first) == 'f') && first != '*' && first != '+') {
            continue;
        }
        std::string word(name.substr(start, i - start));
        for (char& ch : word) {
            const auto c = static_cast<unsigned char>(ch);
            if (c < 0x80) {
                ch = static_cast<char>(std::tolower(c));
            }
        }
        out.push_back(std::move(word));
    }
    std::sort(out.begin(), out.end());
    return out;
}

LensQuery lens_query(const raw::RawMetadata& metadata) {
    LensQuery q;
    q.camera_make = metadata.camera.make;
    q.camera_model = metadata.camera.model;
    q.camera_normalized_make = metadata.camera.normalized_make;
    q.camera_normalized_model = metadata.camera.normalized_model;
    q.lens_make = metadata.lens.make;
    q.lens_model = metadata.lens.model;
    const auto positive = [](std::optional<float> v) -> std::optional<double> {
        if (v && std::isfinite(*v) && *v > 0.0f) {
            return static_cast<double>(*v);
        }
        return std::nullopt;
    };
    q.lens_min_focal_mm = positive(metadata.lens.min_focal_length_mm);
    q.lens_max_focal_mm = positive(metadata.lens.max_focal_length_mm);
    q.focal_length_mm = positive(metadata.capture.focal_length_mm);
    q.f_number = positive(metadata.capture.f_number);
    q.focal_length_35mm_mm = positive(metadata.lens.focal_length_35mm_equivalent_mm);
    if (metadata.sensor_crop) {
        q.sensor_crop = true;
        const std::optional<double>& factor = metadata.sensor_crop->factor;
        if (factor && std::isfinite(*factor) && *factor > 1.0) {
            q.sensor_crop_factor = *factor;
        }
        q.sensor_crop_basis = metadata.sensor_crop->basis;
    }
    return q;
}

std::optional<double> crop_factor_from_focal_lengths(const LensQuery& query) noexcept {
    if (query.focal_length_mm && query.focal_length_35mm_mm && *query.focal_length_mm > 0.0 &&
        *query.focal_length_35mm_mm > 0.0) {
        return *query.focal_length_35mm_mm / *query.focal_length_mm;
    }
    return std::nullopt;
}

std::string_view to_string(LensMatchKind kind) noexcept {
    switch (kind) {
        case LensMatchKind::None:
            return "none";
        case LensMatchKind::Fuzzy:
            return "fuzzy";
        case LensMatchKind::Exact:
            return "exact";
    }
    return "invalid";
}

std::string_view to_string(LensDistortionModel model) noexcept {
    switch (model) {
        case LensDistortionModel::Poly3:
            return "poly3";
        case LensDistortionModel::Poly5:
            return "poly5";
        case LensDistortionModel::PTLens:
            return "ptlens";
    }
    return "invalid";
}

std::string_view to_string(LensTcaModel model) noexcept {
    switch (model) {
        case LensTcaModel::Linear:
            return "linear";
        case LensTcaModel::Poly3:
            return "poly3";
    }
    return "invalid";
}

std::string_view to_string(LensVignettingModel model) noexcept {
    switch (model) {
        case LensVignettingModel::PabloDAngelo:
            return "pa";
    }
    return "invalid";
}

std::string_view to_string(LensCorrectionKind kind) noexcept {
    switch (kind) {
        case LensCorrectionKind::Distortion:
            return "distortion";
        case LensCorrectionKind::LateralChromaticAberration:
            return "lateral_chromatic_aberration";
        case LensCorrectionKind::Vignetting:
            return "vignetting";
    }
    return "invalid";
}

std::string_view to_string(LensCorrectionOutcome outcome) noexcept {
    switch (outcome) {
        case LensCorrectionOutcome::Applied:
            return "applied";
        case LensCorrectionOutcome::Disabled:
            return "disabled";
        case LensCorrectionOutcome::CarriedByFile:
            return "carried_by_file";
        case LensCorrectionOutcome::Withheld:
            return "withheld";
        case LensCorrectionOutcome::NoProfile:
            return "no_profile";
        case LensCorrectionOutcome::NotApplicable:
            return "not_applicable";
        case LensCorrectionOutcome::NoCalibration:
            return "no_calibration";
    }
    return "invalid";
}

// ---- LensDatabase ---------------------------------------------------------------------------------

struct LensDatabase::Impl {
    lfDatabase db;
    LensDatabaseInfo info;
    std::vector<LensProfileSummary> profiles;  // made by load(), immutable afterwards
    // Under lensfun_mutex().
    mutable Lru<LensQuery, LensMatch> matches;
    mutable Lru<PlanKey, LensCorrectionPlan> plans;

    // Under lensfun_mutex().
    [[nodiscard]] LensMatch match_locked(const LensQuery& query) const;
    [[nodiscard]] const lfLens* find_locked(const LensProfileId& id) const;
    [[nodiscard]] Expected<LensCorrectionPlan> plan_locked(
        const raw::RawMetadata& metadata, const std::optional<LensCorrectionSetting>& setting,
        image::Size upright, image::Orientation orientation, bool monochrome) const;
};

LensDatabase::LensDatabase() : impl_(std::make_unique<Impl>()) {}

LensDatabase::~LensDatabase() {
    // ~lfDatabase deletes its lenses, which releases Lensfun's process-wide regular expressions.
    const std::lock_guard lock(lensfun_mutex());
    impl_.reset();
}

const LensDatabaseInfo& LensDatabase::info() const noexcept {
    return impl_->info;
}

const std::vector<LensProfileSummary>& LensDatabase::profiles() const noexcept {
    return impl_->profiles;
}

Expected<std::shared_ptr<const LensDatabase>> LensDatabase::load(std::span<const LensDatabaseFile> files,
                                                                 LensDatabaseIdentity identity) {
    if (files.empty()) {
        return fail(ErrorCode::InvalidArgument, "a lens database needs at least one file", kWhereDb);
    }
    const std::lock_guard lock(lensfun_mutex());
    try {
        std::shared_ptr<LensDatabase> database(new LensDatabase());
        Impl& impl = *database->impl_;
        for (const LensDatabaseFile& file : files) {
            const lfError e = impl.db.Load(file.name.c_str(), file.data.data(), file.data.size());
            if (e != LF_NO_ERROR) {
                return fail(
                    ErrorCode::Corrupt,
                    std::format("Lensfun cannot read the lens database file '{}' (error {}): it is not data "
                                "format {} XML that Lensfun {}.{}.{} understands",
                                file.name, static_cast<int>(e), kLensfunDataFormatVersion, LF_VERSION_MAJOR,
                                LF_VERSION_MINOR, LF_VERSION_MICRO),
                    kWhereDb);
            }
        }
        LensDatabaseInfo& summary = impl.info;
        summary.identity = std::move(identity);
        summary.library = std::format("Lensfun {}.{}.{}", LF_VERSION_MAJOR, LF_VERSION_MINOR, LF_VERSION_MICRO);
        summary.files = files.size();
        for (const lfMount* const* m = impl.db.GetMounts(); m != nullptr && *m != nullptr; ++m) {
            ++summary.mounts;
        }
        for (const lfCamera* const* c = impl.db.GetCameras(); c != nullptr && *c != nullptr; ++c) {
            ++summary.cameras;
        }
        for (const lfLens* const* l = impl.db.GetLenses(); l != nullptr && *l != nullptr; ++l) {
            ++summary.lenses;
            const lfLens* lens = *l;
            LensProfileSummary profile;
            profile.id = profile_id(lens);
            profile.mounts = mounts_text(lens);
            profile.lens_type = lens_type_name(lens->Type);
            profile.min_focal_mm = std::isfinite(lens->MinFocal) ? static_cast<double>(lens->MinFocal) : 0.0;
            profile.max_focal_mm = std::isfinite(lens->MaxFocal) ? static_cast<double>(lens->MaxFocal) : 0.0;
            profile.min_f_number = std::isfinite(lens->MinAperture) ? static_cast<double>(lens->MinAperture) : 0.0;
            profile.distortion = lens->CalibDistortion != nullptr && lens->CalibDistortion[0] != nullptr;
            profile.lateral_chromatic_aberration = lens->CalibTCA != nullptr && lens->CalibTCA[0] != nullptr;
            profile.vignetting = lens->CalibVignetting != nullptr && lens->CalibVignetting[0] != nullptr;
            impl.profiles.push_back(std::move(profile));
        }
        if (summary.lenses == 0) {
            return fail(ErrorCode::Corrupt, "the lens database holds no lens", kWhereDb);
        }
        std::sort(impl.profiles.begin(), impl.profiles.end(),
                  [](const LensProfileSummary& a, const LensProfileSummary& b) {
                      return std::tie(a.id.maker, a.id.model, a.id.crop_factor) <
                             std::tie(b.id.maker, b.id.model, b.id.crop_factor);
                  });
        return std::shared_ptr<const LensDatabase>(std::move(database));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot load the lens database", kWhereDb);
    }
}

LensMatch LensDatabase::match(const LensQuery& query) const {
    const std::lock_guard lock(lensfun_mutex());
    if (auto cached = impl_->matches.find(query)) {
        return std::move(*cached);
    }
    LensMatch result = impl_->match_locked(query);
    impl_->matches.insert(query, result);
    return result;
}

bool LensDatabase::contains(const LensProfileId& id) const {
    const std::lock_guard lock(lensfun_mutex());
    return impl_->find_locked(id) != nullptr;
}

Expected<LensCorrectionPlan> LensDatabase::plan(const raw::RawMetadata& metadata,
                                                const std::optional<LensCorrectionSetting>& setting,
                                                image::Size upright_size, image::Orientation orientation,
                                                bool monochrome) const {
    PlanKey key{lens_query(metadata), setting,     carried_lens_corrections(metadata),
                upright_size,         orientation, monochrome};
    const std::lock_guard lock(lensfun_mutex());
    if (auto cached = impl_->plans.find(key)) {
        return std::move(*cached);
    }
    auto planned = impl_->plan_locked(metadata, setting, upright_size, orientation, monochrome);
    if (planned) {
        impl_->plans.insert(std::move(key), *planned);
    }
    return planned;
}

const lfLens* LensDatabase::Impl::find_locked(const LensProfileId& id) const {
    for (const lfLens* const* l = db.GetLenses(); l != nullptr && *l != nullptr; ++l) {
        const lfLens* lens = *l;
        if (same_spelling(default_spelling(lens->Maker), id.maker) &&
            same_spelling(default_spelling(lens->Model), id.model) &&
            std::abs(static_cast<double>(lens->CropFactor) - id.crop_factor) < 0.01) {
            return lens;
        }
    }
    return nullptr;
}

LensMatch LensDatabase::Impl::match_locked(const LensQuery& q) const {
    LensMatch m;
    // ---- The camera ------------------------------------------------------------------------------
    std::vector<std::pair<std::string, std::string>> names;
    const auto add_name = [&](const std::string& make, const std::string& model) {
        if (make.empty() || model.empty()) {
            return;
        }
        for (const auto& [a, b] : names) {
            if (same_spelling(a, make) && same_spelling(b, model)) {
                return;
            }
        }
        names.emplace_back(make, model);
    };
    add_name(q.camera_make, q.camera_model);
    add_name(q.camera_normalized_make, q.camera_normalized_model);
    std::vector<const lfCamera*> exact_cameras;
    std::vector<const lfCamera*> other_cameras;
    for (const auto& [make, model] : names) {
        const LfList<lfCamera> found(db.FindCamerasExt(make.c_str(), model.c_str(), 0));
        for (const lfCamera* camera : found.vector()) {
            const bool exact = any_spelling_is(camera->Maker, make) && any_spelling_is(camera->Model, model);
            auto& list = exact ? exact_cameras : other_cameras;
            if (std::find(list.begin(), list.end(), camera) == list.end()) {
                list.push_back(camera);
            }
        }
    }
    const lfCamera* camera = nullptr;
    if (names.empty()) {
        m.reasons.push_back("the file names no camera");
    } else if (exact_cameras.empty()) {
        if (other_cameras.empty()) {
            m.reasons.push_back(std::format("the camera '{} {}' is not in the database", names.front().first,
                                            names.front().second));
        } else {
            const LensCameraProfile closest = camera_profile(other_cameras.front());
            m.reasons.push_back(
                std::format("the camera '{} {}' is not in the database by that name (closest: '{} {}')",
                            names.front().first, names.front().second, closest.maker, closest.model));
        }
    } else {
        const lfCamera* first = exact_cameras.front();
        const bool agree = std::all_of(exact_cameras.begin(), exact_cameras.end(), [&](const lfCamera* c) {
            return same_spelling(c->Mount != nullptr ? c->Mount : "",
                                 first->Mount != nullptr ? first->Mount : "") &&
                   std::abs(c->CropFactor - first->CropFactor) < 0.01f;
        });
        if (agree) {
            camera = first;
            m.camera = camera_profile(camera);
            m.reasons.push_back(std::format("camera: '{} {}' (mount {}, crop factor {:.4g})", m.camera->maker,
                                            m.camera->model, m.camera->mount, m.camera->crop_factor));
        } else {
            m.reasons.push_back(
                std::format("{} database cameras are named '{} {}' with different mounts or crop "
                            "factors",
                            exact_cameras.size(), names.front().first, names.front().second));
        }
    }
    // A crop mode of the sensor: the camera records a sensor crop (Canon's APS-H and APS-C crop
    // modes, whose files record no 35 mm equivalent focal length), or the file's focal lengths say
    // another crop factor than the camera's. A file with neither is taken to cover the camera's whole
    // sensor, and the reasons say so.
    bool crop_mode = false;
    if (camera != nullptr) {
        const auto exif = crop_factor_from_focal_lengths(q);
        if (q.sensor_crop) {
            crop_mode = true;
            m.reasons.push_back(std::format(
                "the camera records a crop of its sensor ({}): automatic profiles are matched for the "
                "camera's whole sensor only",
                q.sensor_crop_basis.empty() ? std::string("no detail") : q.sensor_crop_basis));
        } else if (exif && !within(*exif, static_cast<double>(camera->CropFactor), kLensCropModeTolerance)) {
            crop_mode = true;
            m.reasons.push_back(std::format(
                "the file's 35 mm equivalent focal length ({:.4g} mm for {:.4g} mm) gives crop factor "
                "{:.3g}, the camera's is {:.4g}: the image is a crop of the sensor, and automatic "
                "profiles are matched for the camera's whole sensor only",
                *q.focal_length_35mm_mm, *q.focal_length_mm, *exif, camera->CropFactor));
        } else if (!exif) {
            m.reasons.push_back(
                "the file records no 35 mm equivalent focal length and no sensor crop Lucent can read: the "
                "image is taken to cover the camera's whole sensor");
        }
    }

    // ---- The lens --------------------------------------------------------------------------------
    std::vector<const lfLens*> strict;
    bool maker_disagrees = false;
    if (q.lens_model.empty()) {
        if (camera == nullptr) {
            m.reasons.push_back("the file names no lens");
            m.kind = LensMatchKind::None;
            return m;
        }
        // A fixed-lens camera: its mount is not one of the database's mounts, and one lens has it.
        const bool fixed_mount = camera->Mount != nullptr && db.FindMount(camera->Mount) == nullptr;
        const LfList<lfLens> found(db.FindLenses(camera, nullptr, nullptr, 0));
        std::vector<const lfLens*> distinct;
        for (const lfLens* lens : found.vector()) {
            if (std::none_of(distinct.begin(), distinct.end(),
                             [&](const lfLens* d) { return same_lens_entry_name(d, lens); })) {
                distinct.push_back(lens);
            }
        }
        if (fixed_mount && distinct.size() == 1 && !crop_mode) {
            m.kind = LensMatchKind::Exact;
            m.lens = profile_id(distinct.front());
            m.candidates.push_back(
                LensCandidate{*m.lens, mounts_text(distinct.front()), distinct.front()->Score, true});
            m.reasons.push_back(std::format("the camera has a fixed lens (mount '{}'): {}", camera->Mount,
                                            describe(*m.lens)));
            return m;
        }
        m.reasons.push_back(fixed_mount
                                ? std::format("the file names no lens, and the camera's fixed mount '{}' "
                                              "carries {} lenses in the database",
                                              camera->Mount, distinct.size())
                                : "the file names no lens (an interchangeable-lens camera)");
        m.kind = LensMatchKind::None;
        return m;
    }
    {
        const char* maker = q.lens_make.empty() ? nullptr : q.lens_make.c_str();
        const LfList<lfLens> with_maker(db.FindLenses(camera, maker, q.lens_model.c_str(), 0));
        strict = with_maker.vector();
        if (strict.empty() && maker != nullptr) {
            const LfList<lfLens> without(db.FindLenses(camera, nullptr, q.lens_model.c_str(), 0));
            strict = without.vector();
            maker_disagrees = !strict.empty();
        }
    }
    // Scores are written into the database's objects by every search: copy them now.
    std::vector<LensCandidate> candidates;
    std::vector<const lfLens*> same;
    for (const lfLens* lens : strict) {
        bool name = same_lens_name(q.lens_model, lens);
        if (name && q.lens_min_focal_mm && q.lens_max_focal_mm && lens->MinFocal > 0.0f &&
            lens->MaxFocal > 0.0f &&
            (!within(*q.lens_min_focal_mm, lens->MinFocal, kLensFocalTolerance) ||
             !within(*q.lens_max_focal_mm, lens->MaxFocal, kLensFocalTolerance))) {
            m.reasons.push_back(
                std::format("'{}' has the file's lens name but the focal range {:.4g}-{:.4g} mm, the "
                            "file's lens {:.4g}-{:.4g} mm",
                            default_spelling(lens->Model), lens->MinFocal, lens->MaxFocal,
                            *q.lens_min_focal_mm, *q.lens_max_focal_mm));
            name = false;
        }
        candidates.push_back(LensCandidate{profile_id(lens), mounts_text(lens), lens->Score, name});
        if (name) {
            same.push_back(lens);
        }
    }
    if (strict.empty()) {
        const char* maker = q.lens_make.empty() ? nullptr : q.lens_make.c_str();
        const LfList<lfLens> loose(db.FindLenses(camera, maker, q.lens_model.c_str(), LF_SEARCH_LOOSE));
        std::vector<const lfLens*> found = loose.vector();
        if (found.empty() && maker != nullptr) {
            const LfList<lfLens> without(
                db.FindLenses(camera, nullptr, q.lens_model.c_str(), LF_SEARCH_LOOSE));
            found = without.vector();
        }
        for (const lfLens* lens : found) {
            candidates.push_back(LensCandidate{profile_id(lens), mounts_text(lens), lens->Score, false});
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const LensCandidate& a, const LensCandidate& b) { return a.score > b.score; });
    if (candidates.size() > kMaxLensCandidates) {
        candidates.resize(kMaxLensCandidates);
    }
    m.candidates = candidates;

    // Distinct lenses (by name) among those with the file's lens name. Lensfun returns only the
    // calibrations made on a sensor no smaller than the camera's (crop factor at most 1/0.96 of it).
    std::vector<const lfLens*> distinct;
    for (const lfLens* lens : same) {
        if (std::none_of(distinct.begin(), distinct.end(),
                         [&](const lfLens* d) { return same_lens_entry_name(d, lens); })) {
            distinct.push_back(lens);
        }
    }
    if (candidates.empty()) {
        m.reasons.push_back(
            std::format("no database lens matches the file's lens '{}'{}", q.lens_model,
                        m.camera ? std::format(" on mount {}", m.camera->mount) : std::string{}));
        m.kind = LensMatchKind::None;
        return m;
    }
    if (strict.empty()) {
        m.reasons.push_back(
            std::format("no database lens holds every word of the file's lens name '{}'; the closest "
                        "is {} (Lensfun score {})",
                        q.lens_model, describe(candidates.front().lens), candidates.front().score));
        m.kind = LensMatchKind::Fuzzy;
        return m;
    }
    if (distinct.empty()) {
        m.reasons.push_back(
            std::format("{} database lens(es) contain the words of '{}' but none has the same name "
                        "(best: {})",
                        strict.size(), q.lens_model, describe(candidates.front().lens)));
        m.kind = LensMatchKind::Fuzzy;
        return m;
    }
    if (distinct.size() > 1) {
        std::string list;
        for (const lfLens* lens : distinct) {
            list += (list.empty() ? "" : "; ") + describe(profile_id(lens));
        }
        m.reasons.push_back(
            std::format("several database lenses have the file's lens name '{}': {}", q.lens_model, list));
        m.kind = LensMatchKind::Fuzzy;
        return m;
    }
    // The calibration of that lens closest to the camera's sensor: the largest crop factor among
    // the entries Lensfun accepted.
    const lfLens* chosen = nullptr;
    for (const lfLens* lens : same) {
        if (same_lens_entry_name(lens, distinct.front()) &&
            (chosen == nullptr || lens->CropFactor > chosen->CropFactor)) {
            chosen = lens;
        }
    }
    const LensProfileId id = profile_id(chosen);
    if (camera == nullptr) {
        m.reasons.push_back(
            std::format("the lens '{}' is {}, but without an identified camera its mount and the "
                        "image's crop factor are not known",
                        q.lens_model, describe(id)));
        m.kind = LensMatchKind::Fuzzy;
        return m;
    }
    if (maker_disagrees) {
        m.reasons.push_back(std::format("the lens name '{}' is {}, but the file names the lens maker '{}'",
                                        q.lens_model, describe(id), q.lens_make));
        m.kind = LensMatchKind::Fuzzy;
        return m;
    }
    if (crop_mode) {
        m.kind = LensMatchKind::Fuzzy;
        return m;
    }
    m.kind = LensMatchKind::Exact;
    m.lens = id;
    m.reasons.push_back(std::format("lens: {} (the file's '{}')", describe(id), q.lens_model));
    return m;
}

namespace {

// The calibration focal lengths of one kind.
template <class Calibration>
std::vector<double> calibrated_focals(Calibration* const* list) {
    std::vector<double> out;
    for (std::size_t i = 0; list != nullptr && list[i] != nullptr; ++i) {
        out.push_back(static_cast<double>(list[i]->Focal));
    }
    return sorted_unique(std::move(out));
}

bool is_prime(const lfLens* lens) noexcept {
    return lens->MinFocal > 0.0f && std::abs(lens->MaxFocal - lens->MinFocal) < 1e-3f;
}

// Whether the photograph's focal length `focal` lies within the calibrated focal lengths `focals`:
// within kLensFocalTolerance of a zoom's calibrated range; within kLensPrimeFocalTolerance of a
// prime's calibrated focal length(s), since a teleconverter multiplies the focal length by 1.4 or
// more and the bare lens's calibration does not describe the combination (the database calibrates
// such combinations as lenses of their own); any focal length for a prime when the file records
// none (`from_file` false: `focal` is then the prime's own).
bool focal_covered(const lfLens* lens, const std::vector<double>& focals, double focal, bool from_file) noexcept {
    if (focals.empty()) {
        return false;
    }
    const bool prime = is_prime(lens);
    if (prime && !from_file) {
        return true;
    }
    const double tolerance = prime ? kLensPrimeFocalTolerance : kLensFocalTolerance;
    return focal >= focals.front() * (1.0 - tolerance) && focal <= focals.back() * (1.0 + tolerance);
}

// The focal length a calibration of `focals` is interpolated for: the photograph's, or for a prime
// its calibrated focal length nearest to it (a prime has one focal length; Lensfun clamps distortion
// and lateral CA to the calibrated ones anyway, but weighs vignetting by the focal length's distance
// in millimetres for a prime, so a file that records 51.6 mm for a 50 mm prime would find none).
double interpolation_focal(const lfLens* lens, const std::vector<double>& focals, double focal) noexcept {
    if (!is_prime(lens) || focals.empty()) {
        return focal;
    }
    return std::clamp(focal, focals.front(), focals.back());
}

// The largest normalised radius of the image's corners in `f`.
double corner_radius(const LensfunFrame& f) noexcept {
    double corner = 0.0;
    for (const image::PointD p :
         {image::PointD{0.0, 0.0}, image::PointD{static_cast<double>(f.size.width), 0.0},
          image::PointD{0.0, static_cast<double>(f.size.height)},
          image::PointD{static_cast<double>(f.size.width), static_cast<double>(f.size.height)}}) {
        const image::PointD n = f.normalized(p);
        corner = std::max(corner, std::hypot(n.x, n.y));
    }
    return corner;
}

bool finite_terms(std::span<const float> terms) noexcept {
    return std::all_of(terms.begin(), terms.end(), [](float v) { return std::isfinite(v); });
}

// The composed green map of `map` is strictly increasing in the radius out to the image's corners.
bool monotone(const LensGeometryMap& map) {
    const double corner = corner_radius(map.frame);
    constexpr int kSteps = 1024;
    double previous = 0.0;
    for (int i = 1; i <= kSteps; ++i) {
        const double r = corner * 1.05 * i / kSteps;
        const image::PointD s = map.normalized_source(image::PointD{r, 0.0}, 1);
        if (!std::isfinite(s.x) || !(s.x > previous)) {
            return false;
        }
        previous = s.x;
    }
    return true;
}

// The vignetting divisor stays positive (and the gain bounded) over the image.
bool vignetting_valid(const LensVignettingCorrection& v) {
    const double corner = corner_radius(v.frame);
    constexpr int kSteps = 1024;
    for (int i = 0; i <= kSteps; ++i) {
        const double g = v.gain_at_normalized(image::PointD{corner * 1.05 * i / kSteps, 0.0});
        if (!std::isfinite(g) || !(g > 0.0) || g > 64.0) {
            return false;
        }
    }
    return true;
}

// The scale keeps the corrected frame filled: every output pixel reads inside the image. Lensfun's
// GetAutoScale checks eight boundary points of a frame it takes to be symmetric about the lens centre,
// for the distortion alone (lateral CA gets a fixed 1 per mille). This checks the pixel centres of
// the whole boundary (at most kSamples + 1 per edge) for every channel against the image's outermost
// pixel centres, Lensfun's own criterion, and raises the scale where Lensfun's leaves an output pixel
// reading beyond the image (a lens centre offset, strong lateral CA): the smallest such scale, with
// Lensfun's 1 per mille margin. Along each ray from the lens centre a larger scale moves every source
// inward (the composed map is monotone, monotone() above), so the search is a bisection.
double filling_scale(const LensGeometryMap& map) {
    const LensfunFrame& f = map.frame;
    const double w = static_cast<double>(f.size.width);
    const double h = static_cast<double>(f.size.height);
    constexpr int kSamples = 256;
    std::vector<image::PointD> boundary;
    boundary.reserve(4 * (kSamples + 1));
    for (int i = 0; i <= kSamples; ++i) {
        const double x = 0.5 + (w - 1.0) * i / kSamples;
        const double y = 0.5 + (h - 1.0) * i / kSamples;
        boundary.push_back(f.normalized(image::PointD{x, 0.5}));
        boundary.push_back(f.normalized(image::PointD{x, h - 0.5}));
        boundary.push_back(f.normalized(image::PointD{0.5, y}));
        boundary.push_back(f.normalized(image::PointD{w - 0.5, y}));
    }
    LensGeometryMap m = map;
    const auto inside = [&](double scale) {
        m.scale = scale;
        constexpr double kSlack = 1e-9;  // pixels: rounding only
        for (const image::PointD n : boundary) {
            for (std::uint32_t channel = 0; channel < 3; ++channel) {
                if (!m.tca && channel != 1) {
                    continue;  // without lateral CA every channel reads where green does
                }
                const image::PointD s = f.stored(m.normalized_source(n, channel));
                if (!(s.x >= 0.5 - kSlack && s.x <= w - 0.5 + kSlack && s.y >= 0.5 - kSlack &&
                      s.y <= h - 0.5 + kSlack)) {
                    return false;
                }
            }
        }
        return true;
    };
    if (inside(map.scale)) {
        return map.scale;
    }
    double low = map.scale;
    double high = map.scale;
    for (int i = 0; i < 64 && !inside(high); ++i) {
        high *= 1.01;
    }
    if (!inside(high)) {
        return map.scale;  // not reachable for a monotone model; keep Lensfun's scale
    }
    for (int i = 0; i < 48; ++i) {
        const double mid = 0.5 * (low + high);
        if (inside(mid)) {
            high = mid;
        } else {
            low = mid;
        }
    }
    return high * 1.001;
}

}  // namespace

Expected<LensCorrectionPlan> LensDatabase::Impl::plan_locked(
    const raw::RawMetadata& metadata, const std::optional<LensCorrectionSetting>& setting,
    image::Size upright, image::Orientation orientation, bool monochrome) const {
    LensCorrectionPlan plan;
    plan.database = info.identity;
    plan.query = lens_query(metadata);
    plan.carried = carried_lens_corrections(metadata);
    auto& decisions = plan.decisions;
    if (!setting) {
        for (LensCorrectionDecision& d : decisions) {
            d.outcome = LensCorrectionOutcome::Disabled;
            d.detail = "lens-correction operation version 0: the settings predate lens-profile corrections";
        }
        return plan;
    }
    plan.operation_version = kLensCorrectionOperationVersion;
    plan.setting = *setting;
    plan.automatic = !setting->profile.has_value();
    plan.match = match_locked(plan.query);

    // 1. The setting, then what the file carries.
    const std::array<bool, 3> enabled = {setting->distortion, setting->lateral_chromatic_aberration,
                                         setting->vignetting};
    const std::array<bool, 3> carried = {plan.carried.distortion, plan.carried.lateral_chromatic_aberration,
                                         plan.carried.vignetting};
    std::string carried_sources;
    for (const std::string& s : plan.carried.sources) {
        carried_sources += (carried_sources.empty() ? "" : ", ") + s;
    }
    std::array<bool, 3> open{};
    for (std::size_t k = 0; k < 3; ++k) {
        if (!enabled[k]) {
            decisions[k].outcome = LensCorrectionOutcome::Disabled;
            decisions[k].detail = "disabled by the setting";
        } else if (carried[k]) {
            decisions[k].outcome = LensCorrectionOutcome::CarriedByFile;
            decisions[k].detail =
                std::format("the file already corrects it ({}): never applied twice", carried_sources);
        } else {
            open[k] = true;
        }
    }
    const auto close_open = [&](LensCorrectionOutcome outcome, const std::string& detail) {
        for (std::size_t k = 0; k < 3; ++k) {
            if (open[k]) {
                decisions[k].outcome = outcome;
                decisions[k].detail = detail;
                open[k] = false;
            }
        }
    };
    if (std::none_of(open.begin(), open.end(), [](bool b) { return b; })) {
        return plan;
    }

    // 2. The profile.
    const lfLens* lens = nullptr;
    if (plan.automatic) {
        const bool any_carried =
            plan.carried.distortion || plan.carried.lateral_chromatic_aberration || plan.carried.vignetting;
        if (plan.match.kind != LensMatchKind::Exact) {
            close_open(LensCorrectionOutcome::NoProfile,
                       std::format("automatic: the lens match is {} ({})", to_string(plan.match.kind),
                                   plan.match.reasons.empty() ? std::string("no reason")
                                                              : plan.match.reasons.back()));
            return plan;
        }
        if (any_carried) {
            close_open(LensCorrectionOutcome::Withheld,
                       std::format(
                           "automatic: the file carries lens corrections of its own ({}); a database profile "
                           "is applied only when chosen",
                           carried_sources));
            return plan;
        }
        lens = find_locked(*plan.match.lens);
        if (lens == nullptr) {
            return fail(ErrorCode::Internal, "the matched lens profile is not in the database", kWherePlan);
        }
    } else {
        lens = find_locked(*setting->profile);
        if (lens == nullptr) {
            const std::string detail =
                std::format("the chosen profile {} is not in the lens database {} {}", describe(*setting->profile),
                            info.identity.asset_id, info.identity.asset_version);
            close_open(LensCorrectionOutcome::NoProfile, detail);
            plan.missing_dependency = detail;
            return plan;
        }
    }
    plan.profile = profile_id(lens);
    plan.camera = plan.match.camera;
    plan.lens_type = lens_type_name(lens->Type);

    // 3. The image's crop factor: the database camera's; for a crop of the sensor (a named profile
    // only: an automatic match is not exact then) the file's 35 mm equivalent focal length, else the
    // camera's times the factor of the sensor crop the file records, else unknown.
    const std::optional<double> exif_crop = crop_factor_from_focal_lengths(plan.query);
    if (plan.camera) {
        plan.crop_factor = plan.camera->crop_factor;
        plan.crop_factor_basis =
            std::format("the database camera '{} {}'", plan.camera->maker, plan.camera->model);
        const bool exif_differs = exif_crop && !within(*exif_crop, plan.crop_factor, kLensCropModeTolerance);
        if (exif_differs || (plan.query.sensor_crop && exif_crop)) {
            plan.crop_factor = *exif_crop;
            plan.crop_factor_basis = std::format(
                "the file's 35 mm equivalent focal length ({:.4g} mm for {:.4g} mm; the camera's whole "
                "sensor is {:.4g}: a crop mode)",
                *plan.query.focal_length_35mm_mm, *plan.query.focal_length_mm, plan.camera->crop_factor);
        } else if (plan.query.sensor_crop && plan.query.sensor_crop_factor) {
            plan.crop_factor = plan.camera->crop_factor * *plan.query.sensor_crop_factor;
            plan.crop_factor_basis = std::format(
                "the database camera '{} {}' ({:.4g}) times the sensor crop the file records ({:.4g}: {})",
                plan.camera->maker, plan.camera->model, plan.camera->crop_factor, *plan.query.sensor_crop_factor,
                plan.query.sensor_crop_basis);
        } else if (plan.query.sensor_crop) {
            close_open(LensCorrectionOutcome::NoProfile,
                       std::format("the image's crop factor is unknown: the camera records a crop of its sensor "
                                   "({}) but not its size, and the file records no 35 mm equivalent focal length",
                                   plan.query.sensor_crop_basis));
            return plan;
        }
    } else if (exif_crop) {
        plan.crop_factor = *exif_crop;
        plan.crop_factor_basis =
            std::format("the file's 35 mm equivalent focal length ({:.4g} mm for {:.4g} mm)",
                        *plan.query.focal_length_35mm_mm, *plan.query.focal_length_mm);
    } else {
        close_open(
            LensCorrectionOutcome::NoProfile,
            "the image's crop factor is unknown: the camera is not in the database and the file records no "
            "35 mm equivalent focal length");
        return plan;
    }
    if (plan.crop_factor < static_cast<double>(lens->CropFactor) * 0.96) {
        close_open(LensCorrectionOutcome::NoCalibration,
                   std::format(
                       "the profile was calibrated on a smaller sensor (crop factor {:.4g}) than the image's "
                       "({:.4g}): the image's corners lie beyond the calibration",
                       lens->CropFactor, plan.crop_factor));
        return plan;
    }

    // 4. The model's frame: the stored-orientation image normalised for the calibration's sensor.
    const image::Size stored = image::oriented_size(upright, image::inverse(orientation));
    auto frame = LensfunFrame::create(stored, plan.crop_factor, static_cast<double>(lens->CropFactor),
                                      static_cast<double>(lens->AspectRatio),
                                      static_cast<double>(lens->CenterX), static_cast<double>(lens->CenterY));
    if (!frame) {
        close_open(LensCorrectionOutcome::NotApplicable,
                   "the image is too small for a lens model: " + frame.error().message);
        return plan;
    }

    // 5. The photograph's focal length and f-number.
    std::optional<double> focal = plan.query.focal_length_mm;
    const bool focal_from_file = focal.has_value();
    std::string focal_basis = "the file's focal length";
    if (!focal && is_prime(lens)) {
        focal = static_cast<double>(lens->MinFocal);
        focal_basis = "the prime's focal length (the file records none)";
    }
    if (!focal) {
        close_open(LensCorrectionOutcome::NoCalibration, "the focal length is unknown");
        return plan;
    }
    // Why `focals` do not cover the photograph's focal length; `range` names the zoom's calibrated
    // focal lengths, `calibrations` the prime's calibrations.
    const auto outside_focals = [&](const std::vector<double>& focals, std::string_view range,
                                    std::string_view calibrations) {
        if (is_prime(lens)) {
            return std::format(
                "{} {:.4g} mm differs from the prime's calibrated {} mm by more than {:.0f} % (a teleconverter?): "
                "the bare lens's {} do not describe it",
                focal_basis, *focal, list_text(focals), kLensPrimeFocalTolerance * 100.0, calibrations);
        }
        return std::format("{} {:.4g} mm lies outside the {} ({} mm)", focal_basis, *focal, range, list_text(focals));
    };

    // 6. Calibrations.
    std::optional<LensDistortionCalibration> distortion;
    std::optional<LensTcaCalibration> tca;
    std::optional<LensVignettingCalibration> vignetting;
    std::string vignetting_distance_basis;
    double geometry_focal = *focal;  // the focal length Lensfun's modifier is initialised with
    if (open[0]) {
        const std::vector<double> focals = calibrated_focals(lens->CalibDistortion);
        lfLensCalibDistortion res{};
        if (focals.empty()) {
            decisions[0] = {LensCorrectionKind::Distortion, LensCorrectionOutcome::NoCalibration,
                            "the profile has no distortion calibration"};
        } else if (!focal_covered(lens, focals, *focal, focal_from_file)) {
            decisions[0] = {LensCorrectionKind::Distortion, LensCorrectionOutcome::NoCalibration,
                            outside_focals(focals, "calibrated focal lengths", "distortion calibrations")};
        } else if (const double f = interpolation_focal(lens, focals, *focal);
                   !lens->InterpolateDistortion(static_cast<float>(f), res) || !finite_terms(res.Terms) ||
                   (res.Model != LF_DIST_MODEL_POLY3 && res.Model != LF_DIST_MODEL_POLY5 &&
                    res.Model != LF_DIST_MODEL_PTLENS)) {
            decisions[0] = {LensCorrectionKind::Distortion, LensCorrectionOutcome::NoCalibration,
                            "the profile's distortion calibration uses a model Lucent does not implement"};
        } else {
            LensDistortionCalibration c;
            c.model = res.Model == LF_DIST_MODEL_POLY3   ? LensDistortionModel::Poly3
                      : res.Model == LF_DIST_MODEL_POLY5 ? LensDistortionModel::Poly5
                                                         : LensDistortionModel::PTLens;
            for (std::size_t i = 0; i < 3; ++i) {
                c.terms[i] = static_cast<double>(res.Terms[i]);
            }
            c.focal_mm = f;
            c.calibrated_focal_mm = focals;
            distortion = c;
            geometry_focal = f;
        }
    }
    if (open[1]) {
        const std::vector<double> focals = calibrated_focals(lens->CalibTCA);
        lfLensCalibTCA res{};
        if (monochrome) {
            decisions[1] = {LensCorrectionKind::LateralChromaticAberration,
                            LensCorrectionOutcome::NotApplicable,
                            "monochrome data has no colour channels to align"};
        } else if (focals.empty()) {
            decisions[1] = {LensCorrectionKind::LateralChromaticAberration,
                            LensCorrectionOutcome::NoCalibration,
                            "the profile has no lateral chromatic aberration calibration"};
        } else if (!focal_covered(lens, focals, *focal, focal_from_file)) {
            decisions[1] = {LensCorrectionKind::LateralChromaticAberration,
                            LensCorrectionOutcome::NoCalibration,
                            outside_focals(focals, "calibrated focal lengths", "lateral chromatic aberration calibrations")};
        } else if (const double f = interpolation_focal(lens, focals, *focal);
                   !lens->InterpolateTCA(static_cast<float>(f), res) || !finite_terms(res.Terms) ||
                   (res.Model != LF_TCA_MODEL_LINEAR && res.Model != LF_TCA_MODEL_POLY3)) {
            decisions[1] = {
                LensCorrectionKind::LateralChromaticAberration, LensCorrectionOutcome::NoCalibration,
                "the profile's lateral chromatic aberration calibration uses a model Lucent does not "
                "implement"};
        } else {
            LensTcaCalibration c;
            c.model = res.Model == LF_TCA_MODEL_LINEAR ? LensTcaModel::Linear : LensTcaModel::Poly3;
            for (std::size_t i = 0; i < 6; ++i) {
                c.terms[i] = static_cast<double>(res.Terms[i]);
            }
            if (c.model == LensTcaModel::Linear) {
                c.terms[2] = c.terms[3] = c.terms[4] = c.terms[5] = 0.0;
            }
            c.focal_mm = f;
            c.calibrated_focal_mm = focals;
            tca = c;
            if (!distortion) {
                geometry_focal = f;
            }
        }
    }
    if (open[2]) {
        std::vector<double> focals;
        std::vector<double> apertures;
        std::vector<double> distances;
        for (std::size_t i = 0; lens->CalibVignetting != nullptr && lens->CalibVignetting[i] != nullptr;
             ++i) {
            focals.push_back(static_cast<double>(lens->CalibVignetting[i]->Focal));
            apertures.push_back(static_cast<double>(lens->CalibVignetting[i]->Aperture));
            distances.push_back(static_cast<double>(lens->CalibVignetting[i]->Distance));
        }
        focals = sorted_unique(std::move(focals));
        apertures = sorted_unique(std::move(apertures));
        distances = sorted_unique(std::move(distances));
        const double f = interpolation_focal(lens, focals, *focal);
        // Lensfun's interpolation for one focus distance, with the gain it gives the image's corners.
        struct AtDistance {
            double distance = 0.0;
            lfLensCalibVignetting res{};
            double corner_gain = 0.0;
        };
        const double corner = corner_radius(*frame);
        const auto interpolate = [&](double distance) -> std::optional<AtDistance> {
            AtDistance a;
            a.distance = distance;
            if (!lens->InterpolateVignetting(static_cast<float>(f), static_cast<float>(*plan.query.f_number),
                                             static_cast<float>(distance), a.res) ||
                !finite_terms(a.res.Terms) || a.res.Model != LF_VIGNETTING_MODEL_PA) {
                return std::nullopt;
            }
            LensVignettingCorrection v;
            v.frame = *frame;
            for (std::size_t i = 0; i < 3; ++i) {
                v.calibration.terms[i] = static_cast<double>(a.res.Terms[i]);
            }
            a.corner_gain = v.gain_at_normalized(image::PointD{corner, 0.0});
            if (!std::isfinite(a.corner_gain)) {
                return std::nullopt;
            }
            return a;
        };
        std::string refused;  // why no calibration covers the photograph (empty: one does)
        std::optional<AtDistance> chosen;
        if (focals.empty()) {
            refused = "the profile has no vignetting calibration";
        } else if (!plan.query.f_number) {
            refused = "the f-number is unknown";
        } else if (!focal_covered(lens, focals, *focal, focal_from_file)) {
            refused = outside_focals(focals, "vignetting calibrations' focal lengths", "vignetting calibrations");
        } else if (*plan.query.f_number < apertures.front() * (1.0 - kLensApertureTolerance) ||
                   *plan.query.f_number > apertures.back() * (1.0 + kLensApertureTolerance)) {
            refused = std::format("f/{:.3g} lies outside the calibrated apertures (f/{})", *plan.query.f_number,
                                  list_text(apertures));
        } else {
            if (plan.query.focus_distance_m) {
                chosen = interpolate(*plan.query.focus_distance_m);
                vignetting_distance_basis =
                    std::format(" for the file's focus distance {:.3g} m", *plan.query.focus_distance_m);
            } else if (distances.size() <= 1) {
                // One calibrated distance: Lensfun's interpolation for "infinity", as without a distance.
                chosen = interpolate(kLensDefaultFocusDistanceM);
                vignetting_distance_basis =
                    std::format(" for {:.4g} m (the focus distance is unknown; the profile's vignetting is "
                                "calibrated at one distance)",
                                kLensDefaultFocusDistanceM);
            } else {
                // The focus distance is unknown and the vignetting may depend on it: the calibrated
                // distance whose gain at the image's corners is the smallest, so the corners are never
                // brightened more than the lens darkens them at any calibrated distance; the farthest
                // (Lensfun's choice without a distance) unless a nearer one corrects the corners less
                // by more than kLensVignettingDistanceTolerance (calibrations that agree across distances
                // keep it). Distances Lensfun has no calibration close enough to (at this focal length and
                // f-number) are left out.
                std::optional<AtDistance> farthest;
                std::optional<AtDistance> least;
                std::optional<AtDistance> largest;
                for (auto d = distances.rbegin(); d != distances.rend(); ++d) {
                    const std::optional<AtDistance> at = interpolate(*d);
                    if (!at) {
                        continue;
                    }
                    if (!farthest) {
                        farthest = at;
                    }
                    if (!least || at->corner_gain < least->corner_gain) {
                        least = at;
                    }
                    if (!largest || at->corner_gain > largest->corner_gain) {
                        largest = at;
                    }
                }
                if (farthest && least) {
                    chosen = farthest->corner_gain > least->corner_gain * (1.0 + kLensVignettingDistanceTolerance)
                                 ? least
                                 : farthest;
                }
                if (chosen && largest) {
                    if (largest->corner_gain > chosen->corner_gain * (1.0 + kLensVignettingDistanceTolerance)) {
                        vignetting_distance_basis = std::format(
                            " for {:.4g} m: the focus distance is unknown, and of the calibrated distances ({} m) "
                            "this one corrects the image's corners least (gain {:.3g}; up to {:.3g} for {:.4g} m)",
                            chosen->distance, list_text(distances), chosen->corner_gain, largest->corner_gain,
                            largest->distance);
                    } else {
                        vignetting_distance_basis = std::format(
                            " for {:.4g} m: the focus distance is unknown, and the calibrations at {} m agree at the "
                            "image's corners (gain {:.3g})",
                            chosen->distance, list_text(distances), chosen->corner_gain);
                    }
                }
            }
            if (!chosen) {
                refused = "Lensfun has no vignetting calibration close enough to this photograph";
            }
        }
        if (!refused.empty()) {
            decisions[2] = {LensCorrectionKind::Vignetting, LensCorrectionOutcome::NoCalibration, refused};
        } else {
            LensVignettingCalibration c;
            for (std::size_t i = 0; i < 3; ++i) {
                c.terms[i] = static_cast<double>(chosen->res.Terms[i]);
            }
            c.focal_mm = f;
            c.f_number = *plan.query.f_number;
            c.distance_m = chosen->distance;
            c.calibrated_focal_mm = focals;
            c.calibrated_f_numbers = apertures;
            c.calibrated_distances_m = distances;
            vignetting = c;
        }
    }

    // 7. The geometry.
    if (distortion || tca) {
        // Lensfun's automatic scale for these callbacks (distortion and/or TCA, no projection change).
        const auto f = static_cast<float>(geometry_focal);
        const auto distance = static_cast<float>(vignetting ? vignetting->distance_m : kLensDefaultFocusDistanceM);
        lfModifier modifier(lens, static_cast<float>(plan.crop_factor), stored.width, stored.height);
        const int wanted = (distortion ? LF_MODIFY_DISTORTION : 0) | (tca ? LF_MODIFY_TCA : 0);
        const int got = modifier.Initialize(lens, LF_PF_F32, f, static_cast<float>(plan.query.f_number.value_or(0.0)),
                                            distance, 1.0f, lens->Type, wanted, false);
        const double scale = static_cast<double>(modifier.GetAutoScale(false));
        if ((got & wanted) != wanted || !std::isfinite(scale) || !(scale > 0.0)) {
            return fail(
                ErrorCode::Internal,
                std::format("Lensfun refused the calibrations it interpolated (flags {} of {}, scale {})",
                            got, wanted, scale),
                kWherePlan);
        }
        LensGeometryMap map;
        map.frame = *frame;
        map.orientation = orientation;
        map.upright_size = upright;
        map.scale = scale;
        map.distortion = distortion;
        map.tca = tca;
        if (distortion && !monotone(map)) {
            decisions[0] = {LensCorrectionKind::Distortion, LensCorrectionOutcome::NoCalibration,
                            "the profile's distortion model folds back within the image"};
            distortion.reset();
            map.distortion.reset();
            if (tca) {
                lfModifier tca_only(lens, static_cast<float>(plan.crop_factor), stored.width, stored.height);
                (void)tca_only.Initialize(lens, LF_PF_F32, static_cast<float>(tca->focal_mm), 0.0f, distance, 1.0f,
                                          lens->Type, LF_MODIFY_TCA, false);
                map.scale = static_cast<double>(tca_only.GetAutoScale(false));
            }
        }
        if (map.distortion || map.tca) {
            map.lensfun_scale = map.scale;
            map.scale = filling_scale(map);
            plan.geometry = std::move(map);
        }
    }
    if (vignetting) {
        LensVignettingCorrection v;
        v.frame = *frame;
        v.orientation = orientation;
        v.upright_size = upright;
        v.calibration = *vignetting;
        if (vignetting_valid(v)) {
            plan.vignetting = std::move(v);
        } else {
            decisions[2] = {LensCorrectionKind::Vignetting, LensCorrectionOutcome::NoCalibration,
                            "the profile's vignetting model is not positive and bounded over the image"};
            vignetting.reset();
        }
    }

    // 8. The decisions of what is applied.
    const auto source = [&](double calibrated_focal) {
        return std::format(
            "{} for the {}; {} {}{}", describe(*plan.profile), plan.automatic ? "exact match" : "chosen profile",
            focal_basis,
            calibrated_focal == *focal
                ? std::format("{:.4g} mm", *focal)
                : std::format("{:.4g} mm (the prime's calibrated {:.4g} mm)", *focal, calibrated_focal),
            plan.query.f_number ? std::format(", f/{:.3g}", *plan.query.f_number) : std::string{});
    };
    if (open[0] && distortion) {
        decisions[0] = {
            LensCorrectionKind::Distortion, LensCorrectionOutcome::Applied,
            std::format("{}: {} [{:.6g}, {:.6g}, {:.6g}], scale {:.6g} (Lensfun's {:.6g})", source(distortion->focal_mm),
                        to_string(distortion->model), distortion->terms[0], distortion->terms[1],
                        distortion->terms[2], plan.geometry->scale, plan.geometry->lensfun_scale)};
    }
    if (open[1] && tca) {
        decisions[1] = {LensCorrectionKind::LateralChromaticAberration, LensCorrectionOutcome::Applied,
                        std::format("{}: {} [{:.7g}, {:.7g}, {:.7g}, {:.7g}, {:.7g}, {:.7g}]", source(tca->focal_mm),
                                    to_string(tca->model), tca->terms[0], tca->terms[1], tca->terms[2],
                                    tca->terms[3], tca->terms[4], tca->terms[5])};
    }
    if (open[2] && vignetting) {
        decisions[2] = {LensCorrectionKind::Vignetting, LensCorrectionOutcome::Applied,
                        std::format("{}: pa [{:.6g}, {:.6g}, {:.6g}]{}", source(vignetting->focal_mm),
                                    vignetting->terms[0], vignetting->terms[1], vignetting->terms[2],
                                    vignetting_distance_basis)};
    }
    return plan;
}

Expected<LensCorrectionPlan> plan_lens_correction(const LensDatabase* database,
                                                  const raw::RawMetadata& metadata,
                                                  const std::optional<LensCorrectionSetting>& setting,
                                                  image::Size upright_size, image::Orientation orientation,
                                                  bool monochrome) {
    if (database != nullptr) {
        return database->plan(metadata, setting, upright_size, orientation, monochrome);
    }
    LensCorrectionPlan plan;
    plan.query = lens_query(metadata);
    plan.carried = carried_lens_corrections(metadata);
    if (setting) {
        plan.operation_version = kLensCorrectionOperationVersion;
        plan.setting = *setting;
        plan.automatic = !setting->profile.has_value();
    }
    const std::array<bool, 3> enabled = {plan.setting.distortion, plan.setting.lateral_chromatic_aberration,
                                         plan.setting.vignetting};
    const std::array<bool, 3> carried = {plan.carried.distortion, plan.carried.lateral_chromatic_aberration,
                                         plan.carried.vignetting};
    // An automatic setting applies no database profile to a file that carries corrections of its own
    // (Withheld, or NoProfile without an exact match): a database would not change the result.
    const bool database_would_apply =
        !(plan.automatic && std::any_of(carried.begin(), carried.end(), [](bool b) { return b; }));
    for (std::size_t k = 0; k < 3; ++k) {
        LensCorrectionDecision& d = plan.decisions[k];
        if (!setting) {
            d.outcome = LensCorrectionOutcome::Disabled;
            d.detail = "lens-correction operation version 0: the settings predate lens-profile corrections";
        } else if (!enabled[k]) {
            d.outcome = LensCorrectionOutcome::Disabled;
            d.detail = "disabled by the setting";
        } else if (carried[k]) {
            d.outcome = LensCorrectionOutcome::CarriedByFile;
            d.detail = "the file already corrects it: never applied twice";
        } else {
            d.outcome = LensCorrectionOutcome::NoProfile;
            d.detail = "no lens database is available";
            if (database_would_apply) {
                plan.missing_dependency =
                    "no lens database is available: the lens corrections the settings ask for are not applied";
            }
        }
    }
    return plan;
}

const LensCorrectionDecision& LensCorrectionPlan::decision(LensCorrectionKind kind) const noexcept {
    switch (kind) {
        case LensCorrectionKind::Distortion:
            return decisions[0];
        case LensCorrectionKind::LateralChromaticAberration:
            return decisions[1];
        case LensCorrectionKind::Vignetting:
            return decisions[2];
    }
    return decisions[0];
}

bool LensCorrectionPlan::applied(LensCorrectionKind kind) const noexcept {
    return decision(kind).outcome == LensCorrectionOutcome::Applied;
}

}  // namespace lucent::engine
