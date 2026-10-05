// The editor's interactive "Auto" (auto_service.hpp, ADR 0013).

#include <lucent/application/auto_service.hpp>

#include <algorithm>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <new>
#include <utility>

#include <lucent/core/file.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/text.hpp>

#include "observer_list.hpp"

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.auto";
constexpr std::string_view kWhereBusy = "app.auto.busy";
constexpr std::string_view kWhereCheck = "app.auto.check";

}  // namespace

std::string_view to_string(AutoTaskStage stage) noexcept {
    switch (stage) {
        case AutoTaskStage::Queued:
            return "queued";
        case AutoTaskStage::Analysing:
            return "analysing";
        case AutoTaskStage::Checking:
            return "checking";
        case AutoTaskStage::Saving:
            return "saving";
        case AutoTaskStage::Finished:
            return "finished";
        case AutoTaskStage::WaitingForMemory:
            return "waiting_for_memory";
    }
    return "invalid";
}

struct AutoService::Impl {
    Impl(Library& l, RecipeService& r, TaskExecutor& e, AutoServiceOptions o)
        : library(l), recipes(r), executor(e), options(std::move(o)), observers("app.auto") {}

    struct Task {
        AutoTaskStatus status;
        std::optional<AutoTaskResult> result;
        TaskHandle handle;
        double last_reported = -1.0;
    };

    Library& library;
    RecipeService& recipes;
    TaskExecutor& executor;
    AutoServiceOptions options;
    detail::ObserverList<AutoEvent> observers;

    mutable std::mutex mutex;
    mutable std::condition_variable finished_cv;
    // (mutex) the runs in progress and the last kKeptAutoResults finished ones (for status() and wait())
    std::map<std::uint64_t, Task> tasks;
    std::uint64_t next_ticket = 1;        // (mutex)
    bool closing = false;                 // (mutex)

    void progress(std::uint64_t ticket, AutoTaskStage stage, double fraction) {
        AutoEvent event;
        {
            std::lock_guard lock(mutex);
            auto found = tasks.find(ticket);
            if (found == tasks.end()) {
                return;
            }
            Task& task = found->second;
            const bool new_stage = task.status.stage != stage;
            task.status.stage = stage;
            task.status.fraction = std::max(task.status.fraction, std::clamp(fraction, 0.0, 1.0));
            if (!new_stage && task.status.fraction < task.last_reported + 0.01 && task.status.fraction < 1.0) {
                return;
            }
            task.last_reported = task.status.fraction;
            event.status = task.status;
        }
        event.kind = AutoEvent::Kind::Progress;
        observers.notify(event);
    }

    void finish(std::uint64_t ticket, AutoTaskResult result) {
        AutoEvent event;
        {
            std::lock_guard lock(mutex);
            auto found = tasks.find(ticket);
            if (found == tasks.end()) {
                return;
            }
            Task& task = found->second;
            task.status.stage = AutoTaskStage::Finished;
            task.status.fraction = 1.0;
            task.result = result;
            event.status = task.status;
            // Forget the oldest finished runs beyond the ones kept for status() and wait().
            std::size_t finished = 0;
            for (auto it = tasks.rbegin(); it != tasks.rend(); ++it) {
                if (it->second.status.stage == AutoTaskStage::Finished) {
                    ++finished;
                }
            }
            for (auto it = tasks.begin(); it != tasks.end() && finished > kKeptAutoResults;) {
                if (it->second.status.stage == AutoTaskStage::Finished && it->first != ticket) {
                    it = tasks.erase(it);
                    --finished;
                } else {
                    ++it;
                }
            }
        }
        event.kind = AutoEvent::Kind::Finished;
        event.result = std::move(result);
        finished_cv.notify_all();
        if (event.result->error) {
            core::log_info(kWhere, "automatic settings of item {} ended: {}", event.status.item.value,
                           event.result->error->describe());
        }
        observers.notify(event);
    }

    // The task body (on an executor thread).
    AutoTaskResult execute(std::uint64_t ticket, ItemId item_id, TaskContext& context) {
        AutoTaskResult result;
        result.ticket = ticket;
        result.item = item_id;
        const core::CancellationToken& cancel = context.cancel();
        const auto failed = [&](Error error) {
            result.error = std::move(error);
            return result;
        };
        if (Status c = cancel.check(kWhere); !c) {
            return failed(c.error());
        }
        std::optional<SourceItem> item = library.find(item_id);
        if (!item) {
            return failed(Error{ErrorCode::NotFound, std::format("no library item {}", item_id.value),
                                std::string(kWhere), 0});
        }
        auto current = recipes.current_settings(item_id);
        if (!current) {
            return failed(current.error());
        }
        result.base = current->revision;

        // The file must still be the one the library knows; its probe sizes the admission.
        raw::ProbeOptions probe;
        probe.subimage = item->subimage;
        probe.limits = options.run.analysis.develop.decode_limits;
        probe.describe_subimages = false;
        auto selection = options.registry->select(item->path, probe, cancel);
        if (!selection) {
            return failed(selection.error());
        }
        if (!(selection->probe.metadata.source.identity == item->identity)) {
            return failed(Error{ErrorCode::SourceChanged,
                                std::format("'{}' changed since it was added to the library",
                                            core::path_to_utf8_lossy(item->path)),
                                std::string(kWhere), 0});
        }
        auto estimate = estimate_automatic_memory(selection->probe.metadata, current->settings, options.run);
        if (!estimate) {
            return failed(estimate.error());
        }
        {
            std::lock_guard lock(mutex);
            if (auto found = tasks.find(ticket); found != tasks.end()) {
                found->second.status.memory_bytes = *estimate;
            }
        }
        // Waiting for memory: say so, and ask once for memory that is rebuilt on demand (the
        // editor's cached full-resolution images) to be released.
        const MemoryAdmission& admission = context.admission();
        if (const AdmissionStats held = admission.stats();
            *estimate <= admission.options().budget_bytes &&
            (held.active_bytes + *estimate > admission.options().budget_bytes ||
             held.active >= admission.options().max_concurrent)) {
            progress(ticket, AutoTaskStage::WaitingForMemory, 0.0);
            if (held.active_bytes + *estimate > admission.options().budget_bytes && options.make_room) {
                options.make_room(item_id);
            }
        }
        auto lease = context.admit(*estimate);
        if (!lease) {
            return failed(lease.error());
        }
        // The run keeps its bytes but not the render place (task_executor.hpp, retained memory): the
        // editor's stage-one builds, refines and 100 % views render beside it within the budget.
        lease->retain_only(*estimate);

        AutoRunRequest request;
        request.final_check = true;
        request.fail_final_checks = options.fail_final_checks;
        auto run = run_automatic(*options.registry, item->path, item->subimage, current->settings, request, options.run,
                                 cancel, [&](AutoRunStage stage, double fraction) {
                                     progress(ticket,
                                              stage == AutoRunStage::Analysing ? AutoTaskStage::Analysing
                                                                               : AutoTaskStage::Checking,
                                              0.95 * fraction);
                                 });
        lease->release();
        if (!run) {
            return failed(run.error());
        }
        result.run = std::move(*run);
        if (Status c = cancel.check(kWhere); !c) {
            return failed(c.error());
        }
        // The file must not have changed while it was decoded and analysed (blueprint: stable reads
        // before and after decoding): otherwise the result describes another file.
        if (auto after = core::stat_file(item->path); !after || !(*after == item->identity)) {
            return failed(Error{ErrorCode::SourceChanged,
                                std::format("'{}' changed while its automatic settings were chosen; nothing was saved",
                                            core::path_to_utf8_lossy(item->path)),
                                std::string(kWhere), 0});
        }
        if (result.run->check_failed()) {
            return failed(Error{ErrorCode::Internal,
                                "no automatic edit passed the full-resolution check; nothing was changed",
                                std::string(kWhereCheck), 0});
        }

        progress(ticket, AutoTaskStage::Saving, 0.97);
        auto revisions = automatic_revisions(*result.run, std::nullopt, recipes.now(), recipes.engine_version());
        if (!revisions) {
            return failed(revisions.error());
        }
        auto applied = recipes.apply_automatic(item_id, result.base, *revisions);
        if (!applied) {
            return failed(applied.error());
        }
        result.revisions = std::move(applied->revisions);
        if (!result.revisions.empty()) {
            result.effective = result.revisions.back().id;
        }
        result.made_current = applied->made_current;
        result.state = std::move(applied->state);
        return result;
    }
};

AutoService::AutoService(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

AutoService::~AutoService() {
    std::vector<TaskHandle> handles;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->closing = true;
        for (auto& [ticket, task] : impl_->tasks) {
            if (task.handle.valid() && !task.handle.finished()) {
                task.handle.cancel();
                handles.push_back(task.handle);
            }
        }
    }
    for (const TaskHandle& handle : handles) {
        handle.wait();
    }
}

Expected<std::unique_ptr<AutoService>> AutoService::create(Library& library, RecipeService& recipes,
                                                           TaskExecutor& executor, AutoServiceOptions options) {
    if (!options.registry) {
        return fail(ErrorCode::InvalidArgument, "the automatic settings need a decoder registry", "app.auto.options");
    }
    try {
        auto impl = std::make_shared<Impl>(library, recipes, executor, std::move(options));
        return std::unique_ptr<AutoService>(new AutoService(std::move(impl)));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot create the automatic-settings service", std::string(kWhere));
    }
}

Expected<std::uint64_t> AutoService::run(ItemId item) {
    Impl& s = *impl_;
    if (!s.library.find(item)) {
        return fail(ErrorCode::NotFound, std::format("no library item {}", item.value), std::string(kWhere));
    }
    auto state = s.recipes.state(item);
    if (!state) {
        return Unexpected(std::move(state.error()));
    }
    if (state->read_only) {
        return Unexpected(*state->read_only);
    }
    std::uint64_t ticket = 0;
    {
        std::lock_guard lock(s.mutex);
        if (s.closing) {
            return fail(ErrorCode::Cancelled, "the automatic-settings service is closing", std::string(kWhere));
        }
        for (const auto& [id, task] : s.tasks) {
            if (task.status.item == item && task.status.stage != AutoTaskStage::Finished) {
                return fail(ErrorCode::AlreadyExists,
                            std::format("automatic settings are already running for item {}", item.value),
                            std::string(kWhereBusy));
            }
        }
        ticket = s.next_ticket++;
        Impl::Task task;
        task.status.ticket = ticket;
        task.status.item = item;
        s.tasks.emplace(ticket, std::move(task));
    }
    std::weak_ptr<Impl> weak = impl_;
    auto submitted = s.executor.submit(Lane::Background, [weak, ticket, item](TaskContext& context) {
        auto self = weak.lock();
        if (!self) {
            return;
        }
        AutoTaskResult result;
        try {
            result = self->execute(ticket, item, context);
        } catch (const std::bad_alloc&) {
            result.ticket = ticket;
            result.item = item;
            result.error = Error{ErrorCode::OutOfMemory, "out of memory during the automatic settings",
                                 std::string(kWhere), 0};
        }
        self->finish(ticket, std::move(result));
    });
    if (!submitted) {
        std::lock_guard lock(s.mutex);
        s.tasks.erase(ticket);
        return Unexpected(std::move(submitted.error()));
    }
    {
        std::lock_guard lock(s.mutex);
        if (auto found = s.tasks.find(ticket); found != s.tasks.end()) {
            found->second.handle = *submitted;
        }
    }
    return ticket;
}

void AutoService::cancel(std::uint64_t ticket) noexcept {
    std::lock_guard lock(impl_->mutex);
    if (auto found = impl_->tasks.find(ticket); found != impl_->tasks.end() && found->second.handle.valid()) {
        found->second.handle.cancel();
    }
}

std::optional<AutoTaskStatus> AutoService::status(std::uint64_t ticket) const {
    std::lock_guard lock(impl_->mutex);
    if (auto found = impl_->tasks.find(ticket); found != impl_->tasks.end()) {
        return found->second.status;
    }
    return std::nullopt;
}

std::optional<AutoTaskResult> AutoService::wait(std::uint64_t ticket, std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->mutex);
    const bool done = impl_->finished_cv.wait_for(lock, timeout, [&] {
        auto found = impl_->tasks.find(ticket);
        return found == impl_->tasks.end() || found->second.result.has_value();
    });
    if (!done) {
        return std::nullopt;
    }
    auto found = impl_->tasks.find(ticket);
    if (found == impl_->tasks.end()) {
        return std::nullopt;
    }
    return found->second.result;
}

Subscription AutoService::subscribe(AutoObserver observer) { return impl_->observers.add(std::move(observer)); }

}  // namespace lucent::app
