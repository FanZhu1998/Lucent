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
    // A listener of on_ready() added before the outcome was known. Its subscription and the source share
    // it; the listener itself is taken out (and released) by whichever runs it or ends it first.
    struct Listener {
        explicit Listener(std::function<void()> f) noexcept : function(std::move(f)) {}

        std::mutex mutex;
        std::condition_variable idle;           // the listener has returned
        std::function<void()> function;         // (mutex) until it is called or its subscription ends
        bool ended = false;                     // (mutex) its subscription ended: never called
        std::optional<std::thread::id> caller;  // (mutex) the thread calling it, while it runs
    };

    mutable std::mutex mutex;
    mutable std::condition_variable loaded_cv;
    std::optional<assets::LensDatabaseLookup> outcome;          // (mutex) set once
    std::atomic<const assets::LensDatabaseLookup*> published{};  // &*outcome once set
    std::chrono::steady_clock::time_point finished{};             // set before `published`
    std::vector<std::shared_ptr<Listener>> listeners;             // (mutex) until the outcome is set

    void finish(assets::LensDatabaseLookup&& lookup) {
        std::vector<std::shared_ptr<Listener>> waiting;
        {
            const std::lock_guard lock(mutex);
            outcome.emplace(std::move(lookup));
            finished = std::chrono::steady_clock::now();
            published.store(&*outcome, std::memory_order_release);
            waiting.swap(listeners);
        }
        loaded_cv.notify_all();
        const std::thread::id self = std::this_thread::get_id();
        for (const std::shared_ptr<Listener>& listener : waiting) {
            std::function<void()> function;
            {
                const std::lock_guard lock(listener->mutex);
                if (listener->ended) {
                    continue;
                }
                function.swap(listener->function);
                listener->caller = self;
            }
            call(function);
            function = nullptr;  // its captures are released before its subscription can end
            {
                const std::lock_guard lock(listener->mutex);
                listener->caller.reset();
            }
            listener->idle.notify_all();
        }
    }

    // Ends a listener's subscription: it is never called afterwards. Waits while another thread calls it.
    static void end(const std::weak_ptr<Shared>& weak, const std::shared_ptr<Listener>& listener) noexcept {
        std::function<void()> function;  // a listener that never ran is released here, on this thread
        {
            std::unique_lock lock(listener->mutex);
            listener->ended = true;
            const std::thread::id self = std::this_thread::get_id();
            listener->idle.wait(lock, [&] { return !listener->caller || *listener->caller == self; });
            function.swap(listener->function);
        }
        if (const std::shared_ptr<Shared> shared = weak.lock()) {
            const std::lock_guard lock(shared->mutex);
            std::erase(shared->listeners, listener);  // still waiting for the outcome
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
            assets::LensDatabaseLookup lookup = run(loader);
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

assets::LensDatabaseLookup LensDatabaseSource::run(const Loader& loader) noexcept {
    try {
        try {
            if (!loader) {
                return failed_load(ErrorCode::Internal, "no lens database loader was given");
            }
            return loader();
        } catch (const std::bad_alloc&) {
            return failed_load(ErrorCode::OutOfMemory, "out of memory while loading the lens database");
        } catch (const std::exception& e) {
            return failed_load(ErrorCode::Internal,
                               std::string("loading the lens database failed: ") + e.what());
        } catch (...) {
            return failed_load(ErrorCode::Internal, "loading the lens database failed");
        }
    } catch (...) {
        // Not even the problem's text could be allocated.
        assets::LensDatabaseLookup lookup;
        lookup.problem.emplace();
        lookup.problem->code = ErrorCode::OutOfMemory;
        return lookup;
    }
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

Subscription LensDatabaseSource::on_ready(std::function<void()> listener) {
    if (!listener) {
        return Subscription{};
    }
    auto pending = std::make_shared<Shared::Listener>(std::move(listener));
    // Made before the listener is added, so that an added listener always has its subscription.
    Subscription subscription(
        [weak = std::weak_ptr<Shared>(shared_), pending] { Shared::end(weak, pending); });
    {
        const std::lock_guard lock(shared_->mutex);
        if (!shared_->outcome) {
            shared_->listeners.push_back(pending);
            return subscription;
        }
    }
    // The outcome is known: at once, on this thread (`subscription` then ends with nothing to remove).
    Shared::call(pending->function);
    return Subscription{};
}

}  // namespace lucent::app
