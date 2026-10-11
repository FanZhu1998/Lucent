#pragma once

// LensDatabaseSource: the session's lens database, loaded on a thread of its own (W12, review finding
// APL-06 of W13; ADR 0023, amendment of the W12 release phase).
//
// Verifying the bundled lens database's files and parsing them with Lensfun takes about 140 ms on the
// reference laptop (assets::load_lens_database). Lucent.exe's window must not wait for it, so its
// services are created with a source that is still loading, and each consumer waits only where the
// database decides what it does:
//
//   - wait() blocks until the load has finished. The renderer waits before every estimate and render
//     (EngineRendererOptions::lens_database_source), the recipe service before it names the database in
//     a revision (RecipeServiceOptions::engine_version_source), the batch service before it names the
//     database in a new batch (BatchServiceOptions::lens_database_source). These all run on service
//     threads. So no render runs without the corrections its settings ask for, and no revision, batch,
//     job identity or thumbnail key names another database than the one the renders use: while the
//     database loads, corrections are pending, never wrong.
//   - try_get() never blocks: null while loading. The user interface shows that the database is loading
//     (Help > About, the Lens corrections panel) until on_ready() tells it the outcome.
//
// A database that cannot be loaded is an outcome like any other (assets::LensDatabaseLookup::problem):
// wait() returns it, and the session runs without lens corrections, as before.
//
// Thread-safety: every member is thread-safe. The destructor waits for the loading thread, so the loader
// must finish on its own (assets::load_lens_database reads files of the installation folder only); the
// loading thread never owns the source, so the destructor never runs on it.

#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include <lucent/assets/lens_database.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/lens_correction.hpp>

namespace lucent::app {

class LensDatabaseSource {
public:
    using Loader = std::function<assets::LensDatabaseLookup()>;

    // Runs `loader` on a new thread and returns at once. A loader that throws (or is empty) gives an
    // outcome without a database whose problem says so (Internal, or OutOfMemory for std::bad_alloc).
    // Errors: OutOfMemory, and Internal when no thread can be started.
    [[nodiscard]] static Expected<std::shared_ptr<LensDatabaseSource>> start(Loader loader);
    // A source whose outcome is known already (tools, tests, sessions that load synchronously).
    [[nodiscard]] static std::shared_ptr<LensDatabaseSource> ready(assets::LensDatabaseLookup lookup);

    ~LensDatabaseSource();
    LensDatabaseSource(const LensDatabaseSource&) = delete;
    LensDatabaseSource& operator=(const LensDatabaseSource&) = delete;

    // The outcome, blocking until the load has finished. The reference stays valid while the source lives.
    [[nodiscard]] const assets::LensDatabaseLookup& wait() const;
    // The database (null: none), blocking until the load has finished.
    [[nodiscard]] std::shared_ptr<const engine::LensDatabase> database() const { return wait().database; }
    // The outcome, or null while the database is loading. Never blocks.
    [[nodiscard]] const assets::LensDatabaseLookup* try_get() const noexcept;
    [[nodiscard]] bool loaded() const noexcept { return try_get() != nullptr; }
    // When the load finished (std::chrono::steady_clock), once loaded(); the clock's epoch before.
    [[nodiscard]] std::chrono::steady_clock::time_point finished_at() const noexcept;

    // Calls `listener` once, when the load has finished: on the loading thread, or at once on the calling
    // thread when it has finished already. Listeners run in the order added, outside the source's lock;
    // a listener may call wait(), try_get() and database(), but not on_ready(). Exceptions a listener
    // throws are logged and dropped.
    void on_ready(std::function<void()> listener);

private:
    struct Shared;
    explicit LensDatabaseSource(std::shared_ptr<Shared> shared) noexcept;

    std::shared_ptr<Shared> shared_;
    std::thread thread_;
};

}  // namespace lucent::app
