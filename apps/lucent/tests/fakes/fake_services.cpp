#include "fakes/fake_services.hpp"

#include <array>
#include <utility>

#include <QCoreApplication>
#include <QFileInfo>

namespace lucent::ui::fakes {
namespace {

struct CameraGuess {
    const char* suffix;
    const char* make;
    const char* model;
    const char* lens;
};

// Simulated camera per file suffix. DNG and unlisted suffixes stay "Unknown camera": a DNG can
// come from any maker.
constexpr std::array kCameraGuesses{
    CameraGuess{"cr3", "Canon", "EOS R5", "RF24-105mm F4 L IS USM"},
    CameraGuess{"cr2", "Canon", "EOS 5D Mark IV", "EF24-70mm f/2.8L II USM"},
    CameraGuess{"arw", "Sony", "ILCE-7M4", "FE 24-70mm F2.8 GM II"},
    CameraGuess{"nef", "Nikon", "Z 6", "NIKKOR Z 24-70mm f/4 S"},
    CameraGuess{"raf", "Fujifilm", "X-T4", "XF16-80mmF4 R OIS WR"},
    CameraGuess{"orf", "OM Digital Solutions", "OM-1", "M.Zuiko Digital ED 12-40mm F2.8 PRO II"},
    CameraGuess{"rw2", "Panasonic", "DC-S5M2", "LUMIX S 20-60mm F3.5-5.6"},
    CameraGuess{"pef", "Pentax", "K-3 Mark III", "HD PENTAX-DA 16-85mm F3.5-5.6"},
    CameraGuess{"3fr", "Hasselblad", "X1D II 50C", "XCD 45mm F3.5"},
};

constexpr std::array<double, 6> kIso{100, 200, 400, 800, 1600, 3200};
constexpr std::array<double, 8> kExposure{1.0 / 2000, 1.0 / 1000, 1.0 / 500, 1.0 / 250,
                                          1.0 / 125,  1.0 / 60,   1.0 / 30,  0.5};
constexpr std::array<double, 6> kAperture{2.8, 4.0, 5.6, 8.0, 11.0, 1.8};
constexpr std::array<double, 6> kFocal{24, 35, 50, 70, 85, 105};

QString tr_fake(const char* text) {
    return QCoreApplication::translate("lucent::ui::fakes", text);
}

template <typename Array>
auto pick(const Array& values, std::uint32_t bits) {
    return values[bits % values.size()];
}

}  // namespace

void FakeCatalog::add(ItemId item, Entry entry) {
    const std::lock_guard lock(mutex_);
    entries_[item] = std::move(entry);
}

std::optional<FakeCatalog::Entry> FakeCatalog::find(ItemId item) const {
    const std::lock_guard lock(mutex_);
    const auto found = entries_.find(item);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::uint32_t stable_hash(const QString& text) noexcept {
    std::uint32_t hash = 2166136261u;
    for (const char byte : text.toUtf8()) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 16777619u;
    }
    return hash;
}

LibraryItem make_fake_item(ItemId id, const QString& path, qint64 size_bytes, const QDateTime& modified) {
    const QFileInfo info(path);
    const std::uint32_t hash = stable_hash(info.fileName());
    const QString suffix = info.suffix().toLower();

    LibraryItem item;
    item.id = id;
    item.file_path = info.absoluteFilePath();
    item.file_size_bytes = size_bytes;
    for (const CameraGuess& guess : kCameraGuesses) {
        if (suffix == QLatin1StringView(guess.suffix)) {
            item.camera_make = QString::fromUtf8(guess.make);
            item.camera_model = QString::fromUtf8(guess.model);
            item.lens = QString::fromUtf8(guess.lens);
            break;
        }
    }
    item.capture_time = modified;
    const bool portrait = (hash >> 12) % 5 == 0;
    item.pixel_width = portrait ? 4000 : 6000;
    item.pixel_height = portrait ? 6000 : 4000;
    item.iso = pick(kIso, hash);
    item.exposure_time_s = pick(kExposure, hash >> 3);
    item.f_number = pick(kAperture, hash >> 6);
    item.focal_length_mm = pick(kFocal, hash >> 9);

    switch ((hash >> 15) % 10) {
    case 0:
        item.support = SupportStatus::Unsupported;
        item.support_detail = tr_fake("Simulated: this capture mode cannot be decoded.");
        break;
    case 1:
        item.support = SupportStatus::Limited;
        item.support_detail = tr_fake("Simulated: no lens profile; lens corrections stay off.");
        break;
    case 2:
    case 3:
    case 4:
        item.support = SupportStatus::Verified;
        break;
    default:
        item.support = SupportStatus::CompatibleUnverified;
        item.support_detail = tr_fake("Simulated: decodes, but this model has no quality fixtures yet.");
        break;
    }
    if ((hash >> 19) % 6 == 0) {
        item.flags |= flag_bit(ItemFlag::ReviewRecommended);
    }
    if ((hash >> 22) % 9 == 0) {
        item.flags |= flag_bit(ItemFlag::DuplicateContent);
    }
    item.thumbnail_generation = 1;
    item.thumbnail_is_camera_preview = true;
    item.preview_generation = 1;
    return item;
}

Services create_fake_services(FakeTimings timings, bool batches) {
    auto catalog = std::make_shared<FakeCatalog>();
    Services services;
    services.library = std::make_unique<FakeLibraryService>(catalog, timings);
    services.images = std::make_shared<FakeImageSource>(catalog, timings);
    services.exports = std::make_unique<FakeExportService>(catalog, timings);
    services.edits = std::make_unique<FakeEditService>(catalog, timings);
    services.batches = std::make_unique<FakeBatchService>(catalog, timings, batches);
    services.updates = std::make_unique<FakeUpdateService>(timings);
    services.simulation_notice = tr_fake("Simulated services");
    return services;
}

}  // namespace lucent::ui::fakes
