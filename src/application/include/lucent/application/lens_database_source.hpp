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
// wait() returns it, and the session runs without lens corrections, as before. The source logs nothing
// about the outcome; its loader does (Lucent.exe's Backend logs every outcome, a loader's exception
// included).
//
// Thread-safety: every member is thread-safe. The destructor waits for the loading thread, so the loader
// must finish on its own. assets::load_lens_database reads the installation's assets folder, and it also
// probes the per-user data-pack folder (<user data>\assets\lens-database: a directory check, and its
// manifest is read when the folder exists), which is local by default and any folder with --data-root.
// A read that stalls in either place delays every wait() and the destructor (in Lucent.exe, the UI
// thread at exit). The loading thread never owns the source, so the destructor never runs on it.

#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include <lucent/application/library.hpp>
#include <lucent/assets/lens_database.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/lens_correction.hpp>

namespace lucent::app {

class LensDatabaseSource {
public:
    using Loader = std::function<assets::LensDatabaseLookup()>;

    // Runs `loader` on a new thread and returns at once; the outcome is run(loader)'s.
    // Errors: OutOfMemory, and Internal when no thread can be started.
    [[nodiscard]] static Expected<std::shared_ptr<LensDatabaseSource>> start(Loader loader);
    // A source whose outcome is known already (tools, tests, sessions that load synchronously).
    [[nodiscard]] static std::shared_ptr<LensDatabaseSource> ready(assets::LensDatabaseLookup lookup);
    // Runs `loader` on the calling thread, as start() runs it on its own: the loader's outcome, or, when
    // it throws (or is empty), an outcome without a database whose problem says so (Internal, or
    // OutOfMemory for std::bad_alloc; without a message when not even that can be allocated). Logs
    // nothing: a loader that logs its outcome loads through run() itself, so that an exception is logged
    // like any other outcome (the Backend's loader does).
    [[nodiscard]] static assets::LensDatabaseLookup run(const Loader& loader) noexcept;

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

    // Calls `listener` once the outcome is known, unless its subscription ends first. A listener added
    // while the database loads runs on the loading thread once the outcome is published; those run one
    // after another, in the order added. A listener added once the outcome is set runs at once, on the
    // calling thread, before on_ready() returns (the subscription is then empty): it can run before, or
    // at the same time as, listeners added earlier that the loading thread has not called yet.
    // Ending the subscription (reset() or its destructor) removes a listener that has not run and waits
    // for one running on another thread (a listener may end its own subscription); afterwards it is
    // never called, and the source has released it. Owners keep the subscription as a member, so that
    // the listener ends with them, also when their constructor throws after adding it.
    // Listeners run outside the source's lock; they may call wait(), try_get() and database(), but not
    // on_ready(). Exceptions a listener throws are logged and dropped. An empty `listener` is never called.
    [[nodiscard]] Subscription on_ready(std::function<void()> listener);

private:
    struct Shared;
    explicit LensDatabaseSource(std::shared_ptr<Shared> shared) noexcept;

    std::shared_ptr<Shared> shared_;
    std::thread thread_;
};

}  // namespace lucent::app
