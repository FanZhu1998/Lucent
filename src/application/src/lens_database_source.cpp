#include <lucent/application/lens_database_source.hpp>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <lucent/core/log.hpp>

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.lens_database";

assets::LensDatabaseLookup failed_load(ErrorCode code, std::string message) {
    assets::LensDatabaseLookup lookup;
    lookup.problem = Error{code, std::move(message), std::string(kWhere), 0};
    return lookup;
}

}  // namespace

// The state the loading thread shares with the source. The thread owns only this, never the source.
struct LensDatabaseSource::Shared {
    mutable std::mutex mutex;
    mutable std::condition_variable loaded_cv;
    std::optional<assets::LensDatabaseLookup> outcome;          // (mutex) set once
    std::atomic<const assets::LensDatabaseLookup*> published{};  // &*outcome once set
    std::chrono::steady_clock::time_point finished{};             // set before `published`
    std::vector<std::function<void()>> listeners;                // (mutex) until the outcome is set

    void finish(assets::LensDatabaseLookup lookup) {
        std::vector<std::function<void()>> waiting;
        {
            const std::lock_guard lock(mutex);
            outcome.emplace(std::move(lookup));
            finished = std::chrono::steady_clock::now();
            published.store(&*outcome, std::memory_order_release);
            waiting.swap(listeners);
        }
        loaded_cv.notify_all();
        for (std::function<void()>& listener : waiting) {
            call(listener);
        }
    }

    static void call(const std::function<void()>& listener) noexcept {
        try {
            if (listener) {
                listener();
            }
        } catch (const std::exception& e) {
            core::log_error(kWhere, "a lens database listener threw: {}", e.what());
        } catch (...) {
            core::log_error(kWhere, "a lens database listener threw an unknown exception");
        }
    }
};

LensDatabaseSource::LensDatabaseSource(std::shared_ptr<Shared> shared) noexcept : shared_(std::move(shared)) {}

Expected<std::shared_ptr<LensDatabaseSource>> LensDatabaseSource::start(Loader loader) {
    try {
        auto shared = std::make_shared<Shared>();
        std::shared_ptr<LensDatabaseSource> source(new LensDatabaseSource(shared));
        source->thread_ = std::thread([shared, loader = std::move(loader)]() mutable {
            assets::LensDatabaseLookup lookup;
            try {
                lookup = loader ? loader() : failed_load(ErrorCode::Internal, "no lens database loader was given");
            } catch (const std::bad_alloc&) {
                lookup = failed_load(ErrorCode::OutOfMemory, "out of memory while loading the lens database");
            } catch (const std::exception& e) {
                lookup = failed_load(ErrorCode::Internal, std::string("loading the lens database failed: ") + e.what());
            } catch (...) {
                lookup = failed_load(ErrorCode::Internal, "loading the lens database failed");
            }
            // The loader is destroyed here, on its own thread, before anyone is told.
            loader = nullptr;
            shared->finish(std::move(lookup));
        });
        return source;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot start loading the lens database", std::string(kWhere));
    } catch (const std::system_error& e) {
        return fail(ErrorCode::Internal, std::string("cannot start the lens database's loading thread: ") + e.what(),
                    std::string(kWhere));
    }
}

std::shared_ptr<LensDatabaseSource> LensDatabaseSource::ready(assets::LensDatabaseLookup lookup) {
    auto shared = std::make_shared<Shared>();
    shared->finish(std::move(lookup));
    return std::shared_ptr<LensDatabaseSource>(new LensDatabaseSource(std::move(shared)));
}

LensDatabaseSource::~LensDatabaseSource() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

const assets::LensDatabaseLookup& LensDatabaseSource::wait() const {
    if (const assets::LensDatabaseLookup* outcome = try_get()) {
        return *outcome;
    }
    std::unique_lock lock(shared_->mutex);
    shared_->loaded_cv.wait(lock, [this] { return shared_->outcome.has_value(); });
    return *shared_->outcome;
}

const assets::LensDatabaseLookup* LensDatabaseSource::try_get() const noexcept {
    return shared_->published.load(std::memory_order_acquire);
}

std::chrono::steady_clock::time_point LensDatabaseSource::finished_at() const noexcept {
    // `finished` is written before `published` is released, so it is visible once try_get() is not null.
    return try_get() != nullptr ? shared_->finished : std::chrono::steady_clock::time_point{};
}

void LensDatabaseSource::on_ready(std::function<void()> listener) {
    {
        const std::lock_guard lock(shared_->mutex);
        if (!shared_->outcome) {
            shared_->listeners.push_back(std::move(listener));
            return;
        }
    }
    Shared::call(listener);
}

}  // namespace lucent::app
