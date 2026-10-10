#pragma once

// AutoService: the editor's interactive "Auto" for one photograph (blueprint section 2, "Detailed
// individual editing": "Auto creates a new editable revision. The photographer can lock controls
// before rerunning it. The optimizer may change only unlocked controls and must report when the
// locks prevent its usual technical targets"; section 6 step 7; ADR 0013).
//
// run(item) queues a task on the executor's Background lane (never the UI thread) that
//   1. reads the item's committed settings (current_settings: its revision, locks and locked
//      values; a pending gesture is not included) and probes its file, which must still be the
//      file the library knows (SourceChanged otherwise);
//   2. waits for memory admission of estimate_automatic_memory() (stage WaitingForMemory while
//      the claim does not fit beside the claims already held; AutoServiceOptions::make_room is
//      then asked once to release what can be rebuilt). The run holds its bytes as RETAINED
//      memory (task_executor.hpp): it takes the render place only to be admitted and gives it
//      back at once, so the editor's stage-one builds, refines and 100 % views keep rendering
//      beside it within the budget, which still counts the run's bytes;
//   3. runs run_automatic() (auto_develop.hpp) in process: the analysis of those settings, the
//      full-resolution final check and its bounded conservative fallbacks; then checks that the
//      file is still the library's (SourceChanged, nothing written, when it changed during the run);
//   4. writes one automatic revision per attempt up to the effective one (origin Auto, derived from
//      the starting revision, each with its analysis record: RecipeService::apply_automatic), so
//      the earlier attempt stays in the history with the reason it was replaced; the effective one
//      becomes current unless the photographer changed the item meanwhile (then the revisions are
//      kept as another branch and AutoTaskResult::made_current is false). Undo returns to the recipe
//      before the run.
// Nothing is written after a cancellation or an error. A final check in which no attempt passes
// (only a numerically invalid baseline) writes nothing and ends with "app.auto.check".
//
// Progress (AutoEvent::Progress) is reported as the run advances (about every percent, and at every
// change of stage), and completion (AutoEvent::Finished) once, with the result; observers run on an
// executor thread and must return promptly (library.hpp contract). status() and wait() poll and
// block; the results of the last kKeptAutoResults finished runs are kept for them (older ones are
// forgotten: status() and wait() then return std::nullopt).
//
// One run per item at a time: run() for an item with a run in progress fails with AlreadyExists
// ("app.auto.busy"). The destructor cancels the runs in progress and waits for them.
//
// Thread-safety: all members may be called concurrently from any thread (wait() never from a task
// of the same executor).

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <lucent/application/auto_develop.hpp>
#include <lucent/application/library.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/application/task_executor.hpp>
#include <lucent/catalog/types.hpp>
#include <lucent/core/error.hpp>
#include <lucent/raw/decoder_registry.hpp>

namespace lucent::app {

// Finished runs whose results status() and wait() still return.
inline constexpr std::size_t kKeptAutoResults = 16;

struct AutoServiceOptions {
    std::shared_ptr<const raw::DecoderRegistry> registry;  // required
    AutoRunOptions run;
    // Called once (on an executor thread) when a run's memory claim does not fit beside the claims
    // already held, with the item: release memory that is rebuilt on demand (the editor's cached
    // full-resolution stage ones). Must return promptly and must not wait for executor tasks.
    std::function<void(ItemId)> make_room;
    // Test only: the first n attempts of every final check fail (AutoRunRequest::fail_final_checks).
    std::uint32_t fail_final_checks = 0;
};

enum class AutoTaskStage : std::uint8_t {
    Queued = 1,     // waiting for a thread
    Analysing = 2,  // decode and analysis
    Checking = 3,   // the full-resolution final check
    Saving = 4,     // writing the revisions and records
    Finished = 5,
    WaitingForMemory = 6,  // admitted only when other work releases memory (step 2)
};

// "queued", "analysing", "checking", "saving", "finished", "waiting_for_memory".
[[nodiscard]] std::string_view to_string(AutoTaskStage stage) noexcept;

struct AutoTaskStatus {
    std::uint64_t ticket = 0;
    ItemId item;
    AutoTaskStage stage = AutoTaskStage::Queued;
    double fraction = 0.0;  // of the whole run, 0..1, non-decreasing
    std::uint64_t memory_bytes = 0;  // the run's memory claim, once estimated
};

struct AutoTaskResult {
    std::uint64_t ticket = 0;
    ItemId item;
    // Why nothing (or not everything) was written: Cancelled, the run's error, "app.auto.check" when
    // no attempt passed the final check, the store's error. std::nullopt: the revisions are written.
    std::optional<Error> error;
    std::optional<AutoRun> run;                     // when the analysis finished
    std::optional<catalog::RevisionId> base;        // the revision the run started from (none: as shot)
    std::vector<catalog::RevisionInfo> revisions;   // one per attempt up to the effective one
    std::optional<catalog::RevisionId> effective;   // the last of them
    bool made_current = false;
    std::optional<RecipeState> state;  // the item's state afterwards (when revisions were written)
};

struct AutoEvent {
    enum class Kind : std::uint8_t { Progress = 1, Finished = 2 };
    Kind kind = Kind::Progress;
    AutoTaskStatus status;
    std::optional<AutoTaskResult> result;  // Finished
};

using AutoObserver = std::function<void(const AutoEvent&)>;

class AutoService {
public:
    // InvalidArgument ("app.auto.options") without a registry. `library`, `recipes` and
    // `executor` must outlive the service.
    [[nodiscard]] static Expected<std::unique_ptr<AutoService>> create(Library& library, RecipeService& recipes,
                                                                       TaskExecutor& executor,
                                                                       AutoServiceOptions options);
    ~AutoService();
    AutoService(const AutoService&) = delete;
    AutoService& operator=(const AutoService&) = delete;

    // Starts a run (see the file comment) and returns its ticket. Errors: NotFound for an unknown
    // item; the recipe's error when the item cannot be edited (read-only, unreadable recipe);
    // AlreadyExists ("app.auto.busy"); the executor's.
    [[nodiscard]] Expected<std::uint64_t> run(ItemId item);
    // Cancels a run (no effect once it finished).
    void cancel(std::uint64_t ticket) noexcept;
    // A run's status; std::nullopt for an unknown ticket.
    [[nodiscard]] std::optional<AutoTaskStatus> status(std::uint64_t ticket) const;
    // Blocks until the run finished (or `timeout` passed: std::nullopt) and returns its result.
    [[nodiscard]] std::optional<AutoTaskResult> wait(std::uint64_t ticket, std::chrono::milliseconds timeout) const;
    // Runs queued or in progress (not finished). An update waits while any runs (ADR 0022).
    [[nodiscard]] std::size_t runs_in_progress() const;

    [[nodiscard]] Subscription subscribe(AutoObserver observer);

    struct Impl;  // implementation detail

private:
    explicit AutoService(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};

}  // namespace lucent::app
